/* SPDX-License-Identifier: GPL-3.0-only */
#include "vb_chop.h"
#include "vb_samp.h"
#include "vb_dsp.h"

static const char *const ONOFF[2] = {"OFF", "ON"};
static const char *const ORDERN[4] = {"SEQ", "SHUF", "STUT", "REV"};
static const char *const MAPN[2] = {"KEYS", "STEPS"};
static const vb_param_t PARAMS[VB_CH_NPARAMS] = {
    {"SENS", 0, 127, 64, 0, 0},           {"SLICE", 0, VB_CH_MAXSLICES - 1, 0, 0, 0},
    {"PITCH", 0, 48, 24, VB_P_MOD | VB_P_BIPOLAR, 0}, {"REV", 0, 1, 0, VB_P_LIST, ONOFF},
    {"ORDER", 0, 3, 0, VB_P_LIST, ORDERN}, {"LEN", 1, 127, 127, VB_P_MOD, 0},
    {"ATK", 0, 127, 0, VB_P_MOD, 0},      {"DEC", 0, 127, 100, VB_P_MOD, 0},
    {"MAP", 0, 1, 0, VB_P_LIST, MAPN},
};

/* 2^(k/12), Q16 */
static const uint32_t SEMI_Q16[12] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715};

#define BLK 256u               /* detection block, samples (11.6 ms at 22.05 kHz) */
#define MAXBLK 256u            /* 65535 / 256 */
#define MINGAP 3u              /* blocks between onsets */
#define XFADE 64               /* retrigger fade-out, output samples */
#define EDGE_OUT 64            /* fade at the end of a pass */
#define EDGE_IN 16             /* fade at the start of a pass */
#define STUT_MIN 441u          /* shortest stutter segment, source samples (20 ms) */

/* ---------------- detection, cached ---------------- */

static struct {
    const int16_t *buf;
    uint32_t len, fp;
    int sens, valid, n;
    uint16_t start[VB_CH_MAXSLICES];
    uint8_t perm[VB_CH_MAXSLICES];
} C;
static int last_sens = 64;
static uint32_t step;          /* MAP STEPS: shared by all CHOP voices, reset by voice_init */

static uint32_t fingerprint(const int16_t *b, uint32_t len)
{
    uint32_t h = 2166136261u ^ len, i;
    for (i = 0; i < 64u; i++)
        h = (h ^ (uint16_t)b[(len * i) >> 6]) * 16777619u;
    return h;
}

static int32_t absi(int32_t x) { return x < 0 ? -x : x; }

/* the rise of block b's level over the lower of the two blocks before it: a hit that straddles a block boundary
 * still shows its whole rise in one block */
static int32_t flux(const uint16_t *lvl, uint32_t b)
{
    int32_t ref = b >= 2u && lvl[b - 2u] < lvl[b - 1u] ? lvl[b - 2u] : lvl[b - 1u];
    return (int32_t)lvl[b] - ref;
}

static void detect(const int16_t *buf, uint32_t len, int sens)
{
    static uint16_t lvl[MAXBLK];
    static uint16_t cb[MAXBLK / 2 + 1];
    static int32_t cf[MAXBLK / 2 + 1];
    uint32_t nb = len / BLK, b, i, nc = 0;
    int32_t maxf = 0, thr;
    uint32_t r;
    int n = 1, c;
    if (nb > MAXBLK)
        nb = MAXBLK;
    C.start[0] = 0;
    for (b = 0; b < nb; b++) {                /* mean |x| per block */
        uint32_t sum = 0;
        const int16_t *x = buf + b * BLK;
        for (i = 0; i < BLK; i++)
            sum += (uint32_t)absi(x[i]);
        lvl[b] = (uint16_t)(sum / BLK);
    }
    for (b = 1; b < nb; b++)
        if (flux(lvl, b) > maxf)
            maxf = flux(lvl, b);
    if (maxf > 0) {
        sens = vb_clamp(sens, 0, 127);
        thr = maxf * (127 - sens) * (127 - sens) / 16129;
        if (thr < maxf / 32)
            thr = maxf / 32;
        if (thr < 1)
            thr = 1;
        for (b = MINGAP; b < nb; b++) {        /* peak-picked positive flux */
            int32_t f = flux(lvl, b), fp = flux(lvl, b - 1u), fn = b + 1u < nb ? flux(lvl, b + 1u) : 0;
            if (f < thr || f < fp || f <= fn || (int32_t)lvl[b] * 4 <= ((int32_t)lvl[b] - f) * 5)
                continue;
            if (nc && b - cb[nc - 1] < MINGAP) {
                if (f > cf[nc - 1]) { cb[nc - 1] = (uint16_t)b; cf[nc - 1] = f; }
                continue;
            }
            cb[nc] = (uint16_t)b;
            cf[nc++] = f;
        }
        while (nc > VB_CH_MAXSLICES - 1) {     /* too many: drop the weakest */
            uint32_t w = 0;
            for (i = 1; i < nc; i++)
                if (cf[i] < cf[w])
                    w = i;
            for (i = w; i + 1u < nc; i++) { cb[i] = cb[i + 1u]; cf[i] = cf[i + 1u]; }
            nc--;
        }
        for (c = 0; c < (int)nc; c++) {        /* refine: the first sample over a quarter of the hit's peak */
            uint32_t bb = cb[c], s0 = (bb - 1u) * BLK, e0 = (bb + 1u) * BLK, st = bb * BLK;
            int32_t pk = 0, t2;
            for (i = s0; i < e0; i++)
                if (absi(buf[i]) > pk)
                    pk = absi(buf[i]);
            t2 = pk / 4;
            if (t2 < 3 * (int32_t)lvl[bb - 2u])
                t2 = 3 * (int32_t)lvl[bb - 2u];
            for (i = s0; i < e0; i++)
                if (absi(buf[i]) > t2) {
                    st = i >= s0 + 16u ? i - 16u : s0;
                    break;
                }
            if (st <= (uint32_t)C.start[n - 1] + 64u)
                continue;
            C.start[n++] = (uint16_t)st;
        }
    }
    C.n = n;
    r = 0x5EED1234u ^ (uint32_t)n * 0x9E3779B9u;   /* SHUF: a fixed permutation for each slice count */
    for (c = 0; c < n; c++)
        C.perm[c] = (uint8_t)c;
    for (c = n - 1; c > 0; c--) {
        uint32_t j = vb_rng(&r) % (uint32_t)(c + 1);
        uint8_t t = C.perm[c];
        C.perm[c] = C.perm[j];
        C.perm[j] = t;
    }
}

/* the slices of the current print at `sens`; returns the count (0: no usable print) */
static int ensure(int sens, const int16_t **pbuf, uint32_t *plen)
{
    uint32_t len, fp;
    const int16_t *buf = vb_samp_buffer(&len);
    *pbuf = buf;
    *plen = len;
    if (!buf || len < 4u)
        return 0;
    fp = fingerprint(buf, len);
    if (!C.valid || C.buf != buf || C.len != len || C.fp != fp || C.sens != sens) {
        detect(buf, len, sens);
        C.buf = buf;
        C.len = len;
        C.fp = fp;
        C.sens = sens;
        C.valid = 1;
    }
    return C.n;
}

int vb_chop_slices(uint16_t *starts, int max)
{
    const int16_t *buf;
    uint32_t len;
    int n = ensure(last_sens, &buf, &len), i;
    for (i = 0; i < n && i < max; i++)
        starts[i] = C.start[i];
    return n;
}

/* ---------------- the voice ---------------- */

typedef struct {
    uint32_t pos, inc;       /* Q16 source position and step (the step is fixed per pass) */
    uint32_t a, len;         /* the played portion: source samples [a, a + len) */
    uint32_t left, passlen;  /* output samples left in the pass, output samples of one pass */
    uint32_t pa, plen;       /* a retrigger waiting for the fade-out */
    int32_t env, vel, pvel;  /* Q15 */
    int16_t xf;              /* retrigger fade-out countdown (0: none) */
    uint8_t gate, on, dir, stut, decay, pend, pstut;
} cv_t;

typedef char cv_fits[sizeof(cv_t) <= VB_VOICE_BYTES ? 1 : -1];

static uint32_t pitch_inc(const uint8_t *p)
{
    int st = vb_clamp(p[VB_CH_PITCH], 0, 48) - 24, oct = st >= 0 ? st / 12 : -((-st + 11) / 12);
    uint32_t inc = SEMI_Q16[st - oct * 12] >> 1;          /* the print is at half rate: 0.5 at 0 */
    return oct >= 0 ? inc << oct : inc >> -oct;
}

static void pass(cv_t *s, const uint8_t *p)
{
    uint32_t L = s->len, d;
    if (s->stut) {
        uint32_t seg = L / 4u;
        if (seg < STUT_MIN)
            seg = L < STUT_MIN ? L : STUT_MIN;
        L = seg;
    }
    s->inc = pitch_inc(p);
    d = (s->inc + 255u) >> 8;
    s->passlen = ((L - 2u) << 8) / d;                     /* never reads past a + L - 1 */
    if (!s->passlen)
        s->passlen = 1;
    s->left = s->passlen;
    s->pos = s->dir ? (s->a + L - 2u) << 16 : s->a << 16;
}

static void begin(cv_t *s, const uint8_t *p, uint32_t a, uint32_t len, int32_t vel, int stut)
{
    s->a = a;
    s->len = len;
    s->vel = vel;
    s->stut = (uint8_t)stut;
    s->dir = p[VB_CH_REV] ? 1 : 0;
    s->env = 0;
    s->decay = 0;
    s->xf = 0;
    s->pend = 0;
    s->on = 1;
    pass(s, p);
}

static void init(void *v, uint32_t seed)
{
    cv_t *s = v;
    (void)seed;
    s->on = s->gate = s->pend = 0;
    s->xf = 0;
    s->env = 0;
    step = 0;
}

static void note_on(void *v, const uint8_t *p, int note, int vel)
{
    cv_t *s = v;
    const int16_t *buf;
    uint32_t blen, a, e, L;
    int n, k, i, order = p[VB_CH_ORDER] & 3;
    last_sens = p[VB_CH_SENS];
    n = ensure(last_sens, &buf, &blen);
    if (!n) {
        s->on = s->pend = 0;
        return;
    }
    k = note - 60;
    if (p[VB_CH_MAP] == VB_CH_STEPS)
        k += (int)(step++ % (uint32_t)n);
    i = k % n;
    if (i < 0)
        i += n;
    if (order == VB_CH_SHUF)
        i = C.perm[i];
    else if (order == VB_CH_RORD)
        i = n - 1 - i;
    a = C.start[i];
    e = i + 1 < n ? C.start[i + 1] : blen;
    L = (e - a) * vb_clamp(p[VB_CH_LEN], 1, 127) / 127u;
    if (L < 64u)
        L = e - a < 64u ? e - a : 64u;
    s->gate = 1;
    if (L < 4u) {
        s->on = s->pend = 0;
        return;
    }
    if (s->on) {                              /* a retrigger: fade the old slice out first */
        s->pa = a;
        s->plen = L;
        s->pvel = vel * 258;
        s->pstut = order == VB_CH_STUT;
        s->pend = 1;
        if (!s->xf)
            s->xf = XFADE;
    } else
        begin(s, p, a, L, vel * 258, order == VB_CH_STUT);
}

static void note_off(void *v, const uint8_t *p)
{
    (void)p;
    ((cv_t *)v)->gate = 0;
}

static int render(void *v, const uint8_t *p, int32_t *out, int n)
{
    cv_t *s = v;
    uint32_t blen;
    const int16_t *buf = vb_samp_buffer(&blen);
    int32_t att, dec;
    int i;
    last_sens = p[VB_CH_SENS];                /* only remembered, for vb_chop_slices */
    if (!s->on)
        return 0;
    if (!buf || blen < s->a + s->len) {       /* the print changed under the voice */
        s->on = s->pend = 0;
        return 0;
    }
    {
        int t = vb_clamp(p[VB_CH_ATK], 0, 127), d = vb_clamp(p[VB_CH_DEC], 0, 127);
        att = 32767 / (16 + 2 * t * t) + 1;
        dec = d >= 127 ? 0 : 32767 / (441 + 6 * d * d) + 1;
    }
    for (i = 0; i < n; i++) {
        uint32_t j, done;
        int32_t x0, x1, y, g, edge;
        if (!s->left) {                       /* the pass ended */
            if (s->pend) {
                begin(s, p, s->pa, s->plen, s->pvel, s->pstut);
            } else if (s->stut && s->gate) {
                pass(s, p);
            } else {
                s->on = 0;
                return 0;
            }
        }
        j = s->pos >> 16;
        x0 = buf[j];
        x1 = buf[j + 1u];
        y = x0 + (((x1 - x0) * (int32_t)((s->pos >> 2) & 0x3FFFu)) >> 14);
        if (!s->decay) {
            if ((s->env += att) >= 32767) {
                s->env = 32767;
                s->decay = 1;
            }
        } else if (dec && (s->env -= dec) <= 0) {
            s->env = 0;
            if (!s->pend) {
                s->on = 0;
                return 0;
            }
        }
        done = s->passlen - s->left;
        edge = s->left < EDGE_OUT ? (int32_t)s->left * (32767 / EDGE_OUT) : 32767;
        if (done < EDGE_IN && (int32_t)(done + 1u) * (32767 / EDGE_IN) < edge)
            edge = (int32_t)(done + 1u) * (32767 / EDGE_IN);
        g = (s->env * edge) >> 15;
        if (s->xf) {
            g = (g * s->xf) >> 6;
            if (--s->xf == 0) {
                if (s->pend)
                    begin(s, p, s->pa, s->plen, s->pvel, s->pstut);
                else
                    s->on = 0;
                if (!s->on)
                    return 0;
                continue;                     /* the faded sample is 0 */
            }
        }
        out[i] += (((y * g) >> 15) * s->vel) >> 15;
        if (s->dir)
            s->pos -= s->inc;
        else
            s->pos += s->inc;
        s->left--;
    }
    return 1;
}

const vb_engine_t vb_chop = {{"CHOP", 1, VB_CH_NPARAMS, PARAMS}, sizeof(cv_t), init, note_on, note_off, render};
