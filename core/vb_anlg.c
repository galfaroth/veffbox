/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: ANLG, two oscillators + sub + noise -> tanh drive -> 24 dB/oct ladder low-pass -> VCA.
 * Fixed point, deterministic. The envelope runs every 8 samples; the VCA gain and the filter coefficient are ramped
 * linearly in between.
 *
 * Filter: u = in * (1 + k/2) - k * y4; x = tanh(u); four one-poles y_j += g * (y_{j-1} - y_j). Every stage is a convex
 * mix of bounded values, so the state never leaves +-32767 whatever RES does; g is capped at 0.8 (~11 kHz) so the
 * feedback loop can't ring at Nyquist (k * (g / (2 - g))^4 < 1). RES 127 (k = 4) sits at self-oscillation. */
#include "vb_anlg.h"
#include "vb_dsp.h"

static const char *const OSCW[] = { "SAW", "SQR", "TRI", "PWM", "SIN", "NOIS" };
static const char *const ONOFF[] = { "OFF", "ON" };
#define M(l, hi, d) { l, 0, hi, d, VB_P_MOD, 0 }
static const vb_param_t params[VB_AN_NPARAMS] = {
    { "OSC1", 0, 5, 0, VB_P_LIST, OSCW }, { "OSC2", 0, 5, 1, VB_P_LIST, OSCW }, M("DET", 100, 14), M("MIX", 127, 64),
    M("SUB", 127, 40), M("NOIS", 127, 0), { "SYNC", 0, 1, 0, VB_P_LIST, ONOFF }, M("PW", 127, 64),
    M("CUT", 127, 70), M("RES", 127, 40), { "ENV", 0, 127, 84, VB_P_MOD | VB_P_BIPOLAR, 0 }, M("DRV", 127, 20),
    M("ATK", 127, 2), M("DEC", 127, 50), M("SUS", 127, 90), M("REL", 127, 40),
};
#undef M

#define ONE_Q30 (1 << 30)
#define SILENT_Q30 (1 << 15)   /* ~ -90 dB */
#define G_MAX 26214            /* 0.8, Q15 */
#define PWM_INC8 467000u       /* the PWM sweep: ~0.6 Hz, stepped every 8 samples */
enum { ST_OFF, ST_ATK, ST_DEC, ST_REL };

typedef struct {
    uint32_t ph1, ph2, lfo, rng;
    int32_t env;               /* Q30 */
    int32_t amp, damp;         /* VCA, Q16 */
    int32_t g, dg;             /* filter coefficient, Q15 */
    int32_t y1, y2, y3, y4;    /* filter stages */
    uint8_t stage, note, vel, sub, active, subsq;
} anlg_voice_t;
_Static_assert(sizeof(anlg_voice_t) <= VB_VOICE_BYTES, "ANLG voice too big");

static inline int imin(int a, int b) { return a < b ? a : b; }

/* one 8-sample envelope step (same curves as OP4). a = ATK DEC SUS REL */
static int32_t env_step(int32_t e, uint8_t *st, const uint8_t *a)
{
    switch (*st) {
    case ST_ATK:
        e += VB_ENV_ATK8[a[0] & 127];
        if (e >= ONE_Q30) { e = ONE_Q30; *st = ST_DEC; }
        break;
    case ST_DEC: {
        int32_t s = (a[2] & 127) * (a[2] & 127) * 66572;   /* (sus/127)^2, Q30 */
        e = s + vb_mulq30(e - s, VB_ENV_K8[a[1] & 127]);
        break;
    }
    case ST_REL:
        e = vb_mulq30(e, VB_ENV_K8[a[3] & 127]);
        if (e < SILENT_Q30) { e = 0; *st = ST_OFF; }
        break;
    default: e = 0; break;
    }
    return e;
}

/* polyBLEP residual of a +2 step at phase 0, Q15. r = 2^30 / (inc >> 8), so (ph >> 8) * r >> 15 = ph / inc in Q15 */
static inline int32_t blep(uint32_t ph, uint32_t inc, uint32_t r)
{
    if (ph < inc) {
        int32_t d = 32768 - (int32_t)(((ph >> 8) * r) >> 15);
        return -((d * d) >> 15);
    }
    uint32_t m = 0u - ph;
    if (m < inc) {
        int32_t d = 32768 - (int32_t)(((m >> 8) * r) >> 15);
        return (d * d) >> 15;
    }
    return 0;
}

static inline int32_t osc(int w, uint32_t ph, uint32_t inc, uint32_t r, uint32_t thr, uint32_t *rng)
{
    switch (w) {
    case VB_AN_SAW: return (int32_t)(ph >> 16) - 32768 - blep(ph, inc, r);
    case VB_AN_SQR: case VB_AN_PWM:
        return vb_clamp((ph < thr ? 32767 : -32767) + blep(ph, inc, r) - blep(ph - thr, inc, r), -32767, 32767);
    case VB_AN_TRI: { int32_t t = (int32_t)(ph >> 15); return t < 65536 ? t - 32768 : 98303 - t; }
    case VB_AN_SIN: return vb_sin(ph);
    default: return vb_noise(rng);
    }
}

static void anlg_init(void *voice, uint32_t seed)
{
    anlg_voice_t *v = voice;
    *v = (anlg_voice_t){ 0 };
    v->rng = seed ? seed : 0x9E3779B9u;
}

static void anlg_note_on(void *voice, const uint8_t *patch, int note, int vel)
{
    anlg_voice_t *v = voice;
    (void)patch;
    if (!v->active) {          /* a sounding voice keeps its phases, filter and level: a retrigger doesn't click */
        v->ph1 = v->ph2 = 0;
        v->env = v->amp = v->damp = v->dg = 0;
        v->g = G_MAX;
        v->y1 = v->y2 = v->y3 = v->y4 = 0;
        v->sub = 0;
        v->subsq = 0;
    }
    v->note = (uint8_t)vb_clamp(note, 0, 127);
    v->vel = (uint8_t)vb_clamp(vel, 1, 127);
    v->stage = ST_ATK;
    v->active = 1;
}

static void anlg_note_off(void *voice, const uint8_t *patch)
{
    anlg_voice_t *v = voice;
    (void)patch;
    if (v->stage != ST_OFF) v->stage = ST_REL;
}

static int anlg_render(void *voice, const uint8_t *p, int32_t *out, int n)
{
    anlg_voice_t *v = voice;
    if (!v->active) return 0;

    /* per block */
    const int w1 = imin(p[VB_AN_OSC1], 5), w2 = imin(p[VB_AN_OSC2], 5);
    const int sync = p[VB_AN_SYNC] != 0;
    const int32_t fac = (sync ? VB_DET_SYNC : VB_DET_CENTS)[imin(p[VB_AN_DET], 100)];   /* osc2 / osc1, Q16 */
    const uint32_t inc1 = VB_NOTE_INC[v->note];
    const int32_t h2 = vb_mul_sh((int32_t)inc1, fac, 17);
    const uint32_t inc2 = h2 >= 0x40000000 ? 0x7FFFFFFFu : (uint32_t)h2 << 1;
    const uint32_t r1 = (1u << 30) / ((inc1 >> 8) ? (inc1 >> 8) : 1), r2 = (1u << 30) / ((inc2 >> 8) ? (inc2 >> 8) : 1);
    const int mix = imin(p[VB_AN_MIX], 127);
    const int32_t g1 = (127 - mix) * 258, g2 = mix * 258;
    const int32_t gs = imin(p[VB_AN_SUB], 127) * 258, gn = imin(p[VB_AN_NOIS], 127) * 258;
    /* duty 5 % .. 95 %, 50 % at PW 64 */
    const int32_t pwoff = (imin(p[VB_AN_PW], 127) - 64) * 30198989;
    const uint32_t thr = 0x80000000u + (uint32_t)pwoff;
    /* drive: tanh gain 0.5 .. 8 for a full-scale input, normalized so full scale stays full scale */
    const int32_t dgain = 32 + imin(p[VB_AN_DRV], 127) * 480 / 127;
    const int32_t norm14 = (32767 << 14) / vb_tanh((32767 * dgain) >> 8);
    const int32_t kq12 = imin(p[VB_AN_RES], 127) * 16384 / 127;               /* 0 .. 4.0 */
    const int32_t cut256 = imin(p[VB_AN_CUT], 127) * 256, envamt = imin(p[VB_AN_ENV], 127) - 64;
    const int32_t velg = v->vel * 258;

    for (int i = 0; i < n;) {
        if (v->sub == 0) {     /* envelope, every 8 samples */
            v->env = env_step(v->env, &v->stage, p + VB_AN_ATK);
            if (v->stage != ST_ATK && v->env < SILENT_Q30) { v->active = 0; return 0; }
            int32_t e15 = v->env >> 15;
            v->damp = (((velg * e15) >> 14) - v->amp) >> 3;
            int32_t idx = vb_clamp(cut256 + ((envamt * e15) >> 6), 0, 127 * 256);   /* +-128 steps at full env */
            int ii = idx >> 8, f = idx & 255;
            int32_t gt = VB_ONEPOLE[ii] + (ii < 127 ? ((VB_ONEPOLE[ii + 1] - VB_ONEPOLE[ii]) * f) >> 8 : 0);
            if (gt > G_MAX) gt = G_MAX;
            v->dg = (gt - v->g) >> 3;
            v->lfo += PWM_INC8;
            v->sub = 8;
        }
        int k = imin(v->sub, n - i);
        v->sub = (uint8_t)(v->sub - k);

        /* PWM: PW swept by a triangle, +-30 % */
        int32_t tri = (int32_t)(v->lfo >> 15);
        tri = tri < 65536 ? tri - 32768 : 98303 - tri;
        int32_t off = (pwoff >> 16) + ((tri * 39321) >> 16);                 /* duty - 50 %, Q16 of a cycle */
        const uint32_t thp = 0x80000000u + ((uint32_t)vb_clamp(off, -29491, 29491) << 16);
        const uint32_t t1 = w1 == VB_AN_PWM ? thp : thr, t2 = w2 == VB_AN_PWM ? thp : thr;

        uint32_t ph1 = v->ph1, ph2 = v->ph2, rng = v->rng;
        int32_t y1 = v->y1, y2 = v->y2, y3 = v->y3, y4 = v->y4, g = v->g, amp = v->amp;
        const int32_t dg = v->dg, damp = v->damp;
        int subsq = v->subsq;
        for (; k > 0; k--, i++) {
            ph1 += inc1;
            if (ph1 < inc1) {  /* osc1 wrapped */
                subsq ^= 1;
                ph2 = sync ? (uint32_t)vb_mul_sh((int32_t)(ph1 >> 1), fac, 15) : ph2 + inc2;
            } else {
                ph2 += inc2;
            }
            int32_t o1 = osc(w1, ph1, inc1, r1, t1, &rng);
            int32_t o2 = osc(w2, ph2, inc2, r2, t2, &rng);
            int32_t x = ((o1 * g1 + o2 * g2) >> 16) + (((subsq ? 32767 : -32767) * gs) >> 17);
            if (gn) x += (vb_noise(&rng) * gn) >> 17;
            int32_t d = (vb_tanh((x * dgain) >> 8) * norm14) >> 14;
            int32_t u = d + ((d * kq12) >> 13) - ((kq12 * y4) >> 12);
            int32_t in = vb_tanh(u >> 2);       /* tanh(u / 32768), Q15 */
            y1 += ((in - y1) * g) >> 15;
            y2 += ((y1 - y2) * g) >> 15;
            y3 += ((y2 - y3) * g) >> 15;
            y4 += ((y3 - y4) * g) >> 15;
            g += dg;
            out[i] += (y4 * amp) >> 16;
            amp += damp;
        }
        v->ph1 = ph1; v->ph2 = ph2; v->rng = rng;
        v->y1 = y1; v->y2 = y2; v->y3 = y3; v->y4 = y4; v->g = g; v->amp = amp;
        v->subsq = (uint8_t)subsq;
    }
    return 1;
}

const vb_engine_t vb_anlg = {
    { "ANLG", 1, VB_AN_NPARAMS, params },
    sizeof(anlg_voice_t), anlg_init, anlg_note_on, anlg_note_off, anlg_render,
};
