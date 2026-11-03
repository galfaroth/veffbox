/* SPDX-License-Identifier: GPL-3.0-only */
#include "vb_samp.h"
#include "vb_dsp.h"

static const char *const LOOPN[3] = {"OFF", "FWD", "P-P"};
static const char *const ONOFF[2] = {"OFF", "ON"};
static const vb_param_t PARAMS[8] = {
    {"START", 0, 126, 0, VB_P_MOD, 0},   {"LEN", 1, 127, 127, VB_P_MOD, 0},
    {"LOOP", 0, 2, 0, VB_P_LIST, LOOPN}, {"REV", 0, 1, 0, VB_P_LIST, ONOFF},
    {"PITCH", 0, 48, 24, VB_P_MOD | VB_P_BIPOLAR, 0}, {"FINE", 0, 99, 50, VB_P_MOD | VB_P_BIPOLAR, 0},
    {"FADE", 0, 127, 6, VB_P_MOD, 0},    {"GAIN", 0, 127, 100, VB_P_MOD, 0},
};

static const int16_t *sbuf;
static uint32_t slen;
void vb_samp_set_buffer(const int16_t *buf, uint32_t len)
{
    sbuf = buf;
    slen = len > 65535u ? 65535u : len;
}
const int16_t *vb_samp_buffer(uint32_t *len)
{
    *len = sbuf ? slen : 0u;
    return sbuf;
}

/* 2^(k/12), Q16 */
static const uint32_t SEMI_Q16[12] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715};

typedef struct {
    uint32_t pos;          /* Q16 sample index */
    uint32_t inc;          /* Q16 per output sample */
    int32_t env, vel;      /* Q15 */
    uint8_t note, gate, dir, on;
} sv_t;

static void init(void *v, uint32_t seed)
{
    sv_t *s = v;
    (void)seed;
    s->on = 0;
    s->gate = 0;
    s->env = 0;
}

static void region(const uint8_t *p, uint32_t *a, uint32_t *b)
{
    uint32_t start = slen * p[0] / 127u, len = slen * p[1] / 127u;
    if (len < 2u)
        len = 2u;
    if (start + len > slen)
        len = slen > start ? slen - start : 0;
    *a = start;
    *b = start + len;
}

static uint32_t pitch_inc(const uint8_t *p, int note)
{
    int st = note - 60 + (int)p[4] - 24, oct;
    uint32_t inc;
    while (st < -48)
        st += 12;
    while (st > 48)
        st -= 12;
    oct = st >= 0 ? st / 12 : -((-st + 11) / 12);
    st -= oct * 12;
    inc = SEMI_Q16[st] >> 1;                              /* storage is half rate: 0.5 x at the root */
    inc = oct >= 0 ? inc << oct : inc >> -oct;
    return inc + (uint32_t)((int32_t)(inc / 50u) * ((int32_t)p[5] - 50) * 595 / 10000);   /* FINE: +-1 semitone */
}

static void note_on(void *v, const uint8_t *p, int note, int vel)
{
    sv_t *s = v;
    uint32_t a, b;
    region(p, &a, &b);
    if (!sbuf || b <= a + 1u) {
        s->on = 0;
        return;
    }
    s->note = (uint8_t)note;
    s->vel = vel * 258;
    s->dir = p[3] ? 1 : 0;
    s->pos = s->dir ? (b - 2u) << 16 : a << 16;
    s->inc = pitch_inc(p, note);
    s->env = s->on ? s->env : 0;                          /* a retrigger fades from where it is: no click */
    s->gate = 1;
    s->on = 1;
}
static void note_off(void *v, const uint8_t *p)
{
    (void)p;
    ((sv_t *)v)->gate = 0;
}

static int render(void *v, const uint8_t *p, int32_t *out, int n)
{
    sv_t *s = v;
    uint32_t a, b, i;
    int32_t att, rel = 32767 / 1323 + 1;                  /* release: 30 ms */
    int32_t gain;
    if (!s->on || !sbuf)
        return 0;
    region(p, &a, &b);
    if (b <= a + 1u) {
        s->on = 0;
        return 0;
    }
    s->inc = pitch_inc(p, s->note);
    att = 32767 / (1 + p[6] * 66) + 1;                   /* FADE: 0 .. ~190 ms */
    gain = vb_mulq15(s->vel, p[7] * 258);
    for (i = 0; i < (uint32_t)n; i++) {
        uint32_t k = s->pos >> 16, f = (s->pos >> 2) & 0x3FFFu;   /* 14-bit fraction: no overflow */
        int32_t x0, x1, y;
        if (k < a || k + 1u >= b) {                       /* the region's end */
            if (p[2] == 1) {                              /* FWD: wrap */
                s->pos = s->dir ? (b - 2u) << 16 : a << 16;
            } else if (p[2] == 2) {                       /* P-P: turn */
                s->dir ^= 1u;
                s->pos = s->dir ? (b - 2u) << 16 : a << 16;
            } else {
                s->on = 0;
                return 0;
            }
            k = s->pos >> 16;
            f = 0;
        }
        x0 = sbuf[k];
        x1 = sbuf[k + 1u];
        y = x0 + (((x1 - x0) * (int32_t)f) >> 14);
        if (s->gate)
            s->env = s->env + att > 32767 ? 32767 : s->env + att;
        else if ((s->env -= rel) <= 0) {
            s->on = 0;
            return 0;
        }
        out[i] += vb_mulq15(vb_mulq15(y, s->env), gain);
        if (s->dir)
            s->pos = s->pos >= s->inc ? s->pos - s->inc : 0;
        else
            s->pos += s->inc;
    }
    return 1;
}

const vb_engine_t vb_samp = {{"SAMP", 1, 8, PARAMS}, sizeof(sv_t), init, note_on, note_off, render};
