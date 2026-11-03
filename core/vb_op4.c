/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: OP4, 4-operator phase-modulation FM. Fixed point, deterministic.
 *
 * Per sample each operator is: phase += inc; y = wave(phase + modulation) * amp. amp (Q16) is level x velocity x
 * envelope; the envelopes run every 8 samples and amp is ramped linearly in between, so the per-sample work is a
 * table lookup, two 32-bit multiplies and a few adds per operator. Modulation: a modulator at full output shifts
 * its targets' phase by 2 cycles (4 pi rad). Operators are computed D, C, B, A: every edge of every algorithm
 * goes from a later letter to an earlier one. */
#include "vb_op4.h"
#include "vb_dsp.h"

static const char *const WAVES[] = { "SIN", "SIN3", "SIN5", "SAW", "SQR", "TRI", "NOIS" };
static const char *const ONOFF[] = { "OFF", "ON" };
#define M(l, hi, d) { l, 0, hi, d, VB_P_MOD, 0 }
#define OP(w, crs, fine, lvl, a, d, s, r) \
    { "WAVE", 0, 6, w, VB_P_LIST, WAVES }, M("CRS", 32, crs), M("FINE", 99, fine), M("LVL", 127, lvl), \
    M("ATK", 127, a), M("DEC", 127, d), M("SUS", 127, s), M("REL", 127, r), { "FIX", 0, 1, 0, VB_P_LIST, ONOFF }
static const vb_param_t params[VB_OP4_FB + 1] = {
    OP(0, 1, 0, 127, 2, 50, 90, 40),     /* A */
    OP(0, 2, 0, 70, 0, 30, 40, 30),      /* B */
    OP(0, 3, 7, 40, 10, 60, 20, 30),     /* C */
    OP(0, 1, 0, 20, 0, 20, 0, 10),       /* D */
    { "ALG", 0, VB_OP4_ALGS - 1, 0, 0, 0 },
    M("FB", 127, 24),
};
#undef OP
#undef M

/* The mockup's ALG table, verbatim (ui/index.html `const ALG=[`): modulation edges "from to", then the carriers. */
static const char *const ALG_EDGES[VB_OP4_ALGS] = {
    "DC CB BA", "CB DB BA", "DC CA BA", "DB BA CA", "DC CB", "DC BA", "DA DB DC", "DC", "BA CA DA", "DC CA CB", "",
};
static const char *const ALG_CARRIERS[VB_OP4_ALGS] = {
    "A", "A", "A", "A", "BA", "CA", "ABC", "CAB", "A", "AB", "ABCD",
};

static const int16_t *const TAB[7] = { VB_SIN, VB_OPW_SIN3, VB_OPW_SIN5, VB_OPW_SAW, VB_OPW_SQR, VB_OPW_TRI, 0 };
/* carriers are summed and scaled by 1 / count, so four full-level carriers stay in range */
static const int16_t CAR_GAIN[5] = { 0, 32767, 16384, 10922, 8192 };

#define ONE_Q30 (1 << 30)
#define SILENT_Q30 (1 << 15)   /* ~ -90 dB */
enum { ST_OFF, ST_ATK, ST_DEC, ST_REL };

typedef struct {
    uint32_t ph[4];            /* A B C D */
    int32_t env[4];            /* Q30 */
    int32_t amp[4], damp[4];   /* Q16 level x velocity x envelope, and its per-sample ramp */
    int32_t fb1, fb2;          /* D's last two outputs */
    uint32_t rng;
    uint8_t stage[4], note, vel, sub, active;
} op4_voice_t;
_Static_assert(sizeof(op4_voice_t) <= VB_VOICE_BYTES, "OP4 voice too big");

static inline int imin(int a, int b) { return a < b ? a : b; }

/* one 8-sample envelope step. a = ATK DEC SUS REL */
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

/* phase increment of one operator */
static uint32_t op_inc(const uint8_t *p, int note)
{
    int crs = imin(p[VB_OP4_CRS], 32), fine = imin(p[VB_OP4_FINE], 99);
    if (p[VB_OP4_FIX]) return vb_inc_hz_q8((crs * 100 + fine * 10) << 8);
    int32_t r100 = (crs ? crs * 100 : 50) + fine;              /* ratio x 100, <= 3299 */
    int32_t rq16 = (r100 * 41943) >> 6;                         /* ratio, Q16 (41943 / 64 = 65536 / 100) */
    int32_t half = vb_mul_sh((int32_t)VB_NOTE_INC[note], rq16, 17);
    return half >= 0x40000000 ? 0x7FFFFFFFu : (uint32_t)half << 1;   /* clamp at Nyquist */
}

/* one wave sample, Q15: table with linear interpolation, or noise */
static inline int32_t wave(const int16_t *t, uint32_t ph, uint32_t *rng)
{
    if (!t) return vb_noise(rng);
    uint32_t i = ph >> 22;
    int32_t f = (int32_t)((ph >> 7) & 0x7FFF), a = t[i];
    return a + (((t[i + 1] - a) * f) >> 15);
}

static void op4_init(void *voice, uint32_t seed)
{
    op4_voice_t *v = voice;
    *v = (op4_voice_t){ 0 };
    v->rng = seed ? seed : 0x9E3779B9u;
}

static void op4_note_on(void *voice, const uint8_t *patch, int note, int vel)
{
    op4_voice_t *v = voice;
    (void)patch;
    if (!v->active) {          /* a fresh voice starts at phase 0; a sounding one keeps its phases and levels */
        for (int o = 0; o < 4; o++) v->ph[o] = 0, v->env[o] = 0, v->amp[o] = 0, v->damp[o] = 0;
        v->fb1 = v->fb2 = 0;
        v->sub = 0;
    }
    v->note = (uint8_t)vb_clamp(note, 0, 127);
    v->vel = (uint8_t)vb_clamp(vel, 1, 127);
    for (int o = 0; o < 4; o++) v->stage[o] = ST_ATK;   /* the attack rises from the current level: no click */
    v->active = 1;
}

static void op4_note_off(void *voice, const uint8_t *patch)
{
    op4_voice_t *v = voice;
    (void)patch;
    for (int o = 0; o < 4; o++) if (v->stage[o] != ST_OFF) v->stage[o] = ST_REL;
}

static int op4_render(void *voice, const uint8_t *patch, int32_t *out, int n)
{
    op4_voice_t *v = voice;
    if (!v->active) return 0;

    /* per block: pitch, levels, waves, routing */
    uint32_t inc[4];
    int32_t gain[4];           /* Q15 level x velocity */
    const int16_t *tab[4];
    const int32_t velg = v->vel * 258;
    for (int o = 0; o < 4; o++) {
        const uint8_t *p = patch + o * VB_OP4_PER_OP;
        int l = imin(p[VB_OP4_LVL], 127);
        inc[o] = op_inc(p, v->note);
        gain[o] = (l * l * 2 * velg) >> 15;
        tab[o] = TAB[imin(p[VB_OP4_WAVE], 6)];
    }
    const int alg = imin(patch[VB_OP4_ALG], VB_OP4_ALGS - 1);
    unsigned src[4] = { 0, 0, 0, 0 }, car = 0;
    for (const char *e = ALG_EDGES[alg]; e[0] && e[1]; e += e[2] ? 3 : 2) src[e[1] - 'A'] |= 1u << (e[0] - 'A');
    for (const char *c = ALG_CARRIERS[alg]; *c; c++) car |= 1u << (*c - 'A');
    const int32_t mAB = -(int32_t)(src[0] >> 1 & 1), mAC = -(int32_t)(src[0] >> 2 & 1), mAD = -(int32_t)(src[0] >> 3 & 1);
    const int32_t mBC = -(int32_t)(src[1] >> 2 & 1), mBD = -(int32_t)(src[1] >> 3 & 1), mCD = -(int32_t)(src[2] >> 3 & 1);
    const int32_t cA = -(int32_t)(car & 1), cB = -(int32_t)(car >> 1 & 1), cC = -(int32_t)(car >> 2 & 1), cD = -(int32_t)(car >> 3 & 1);
    const int32_t cg = CAR_GAIN[(car & 1) + (car >> 1 & 1) + (car >> 2 & 1) + (car >> 3 & 1)];
    const int32_t fbg = imin(patch[VB_OP4_FB], 127) * imin(patch[VB_OP4_FB], 127);   /* Q14 */

    for (int i = 0; i < n;) {
        if (v->sub == 0) {     /* envelopes, every 8 samples */
            int live = 0;
            for (int o = 0; o < 4; o++) {
                v->env[o] = env_step(v->env[o], &v->stage[o], patch + o * VB_OP4_PER_OP + VB_OP4_ATK);
                int32_t tgt = (gain[o] * (v->env[o] >> 15)) >> 14;     /* Q16, < 65536 */
                v->damp[o] = (tgt - v->amp[o]) >> 3;
                if ((car >> o & 1) && (v->stage[o] == ST_ATK || v->env[o] >= SILENT_Q30)) live = 1;
            }
            if (!live) { v->active = 0; return 0; }
            v->sub = 8;
        }
        int k = imin(v->sub, n - i);
        v->sub = (uint8_t)(v->sub - k);

        uint32_t pA = v->ph[0], pB = v->ph[1], pC = v->ph[2], pD = v->ph[3];
        const uint32_t iA = inc[0], iB = inc[1], iC = inc[2], iD = inc[3];
        int32_t aA = v->amp[0], aB = v->amp[1], aC = v->amp[2], aD = v->amp[3];
        const int32_t dA = v->damp[0], dB = v->damp[1], dC = v->damp[2], dD = v->damp[3];
        const int16_t *tA = tab[0], *tB = tab[1], *tC = tab[2], *tD = tab[3];
        int32_t f1 = v->fb1, f2 = v->fb2;
        uint32_t rng = v->rng;
        for (; k > 0; k--, i++) {
            pA += iA; pB += iB; pC += iC; pD += iD;
            /* feedback: the mean of D's last two outputs, up to half a cycle at FB 127 */
            int32_t yD = (wave(tD, pD + ((uint32_t)((f1 + f2) * fbg) << 1), &rng) * aD) >> 16;
            f2 = f1; f1 = yD;
            int32_t yC = (wave(tC, pC + ((uint32_t)(yD & mCD) << 18), &rng) * aC) >> 16;
            int32_t yB = (wave(tB, pB + ((uint32_t)((yC & mBC) + (yD & mBD)) << 18), &rng) * aB) >> 16;
            int32_t yA = (wave(tA, pA + ((uint32_t)((yB & mAB) + (yC & mAC) + (yD & mAD)) << 18), &rng) * aA) >> 16;
            out[i] += (((yA & cA) + (yB & cB) + (yC & cC) + (yD & cD)) * cg) >> 15;
            aA += dA; aB += dB; aC += dC; aD += dD;
        }
        v->ph[0] = pA; v->ph[1] = pB; v->ph[2] = pC; v->ph[3] = pD;
        v->amp[0] = aA; v->amp[1] = aB; v->amp[2] = aC; v->amp[3] = aD;
        v->fb1 = f1; v->fb2 = f2;
        v->rng = rng;
    }
    return 1;
}

const vb_engine_t vb_op4 = {
    { "OP4", 1, VB_OP4_FB + 1, params },
    sizeof(op4_voice_t), op4_init, op4_note_on, op4_note_off, op4_render,
};
