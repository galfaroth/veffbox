/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox core: the FX chain. Thirteen effect types for the 8 slots, integer only, deterministic.
 *
 * Every slot works on 32-sample chunks: the input is clamped to 16 bits (the dry copy), the type writes its wet signal
 * in place, then MIX blends dry and wet (gliding, ramped over the chunk) and the result is clamped to 16 bits again.
 *
 * Silence and sleep (after MNM-1's FX_SLEEP): every loop rounds toward zero, and the one-pole states step with
 * lpz() (the step rounded away from zero, so a state reaches its target exactly; used where the coefficient is large,
 * as a small-step follower it would slew) or hpz() (low cutoffs: DC blockers), so once the input stops every tail
 * ends in exact zeros. A chunk with no input and a wet peak of at most QUIET_LVL counts as quiet; after `need` quiet
 * chunks (enough to flush whatever the type keeps in its buffer) the type's filter states are cleared and the slot
 * sleeps: no work until the input sounds again. s->pos counts the samples processed while awake.
 *
 * Wide products only through vb_mul_sh (out of line). Everything per sample is arranged to fit 32 bits: signals are
 * clamped to 16 bits at the slot's input, coefficients are Q15 or less, and the products are commented with their
 * bounds. Divisions are 32-bit and per chunk (or per grain), never per sample, except by constants. */
#include "vb_fx.h"
#include "vb_dsp.h"

#define P(name, sh, a, b, c, d, e, f, g, h) { name, sh, { a, b, c, d }, { e, f, g, h } }
const vb_fx_desc_t vb_fx_types[VB_FX_TYPES] = {
    P("-", ".", "", "", "", "", 0, 0, 0, 0),     /* the empty slot */
    P("DRIVE", "DRV", "DRV", "TONE", "BIAS", "OUT", 50, 90, 0, 64),
    P("FOLD", "FLD", "AMT", "SYM", "PRE", "OUT", 40, 64, 110, 64),
    P("CRUNCH", "ADP", "AMT", "RATE", "HISS", "OUT", 40, 90, 20, 64),
    P("BITS", "BIT", "BITS", "SR", "JIT", "OUT", 50, 80, 0, 64),
    P("FILTER", "FLT", "CUT", "RES", "TYPE", "DRV", 70, 50, 0, 0),
    P("TAPE DLY", "DLY", "TIME", "FB", "HP", "LP", 48, 60, 10, 90),
    P("REVERB", "REV", "SIZE", "DEC", "DAMP", "GATE", 80, 80, 50, 0),
    P("CHORUS", "CHO", "RATE", "DEP", "FB", "WIDE", 40, 60, 0, 100),
    P("PHASER", "PHS", "RATE", "DEP", "CNTR", "FB", 30, 80, 60, 60),
    P("RING", "RNG", "TUNE", "WAVE", "EXT", "KEY", 72, 0, 0, 127),
    P("RESON", "RES", "TUNE", "DEC", "TONE", "CHRD", 64, 70, 80, 0),
    P("FREEZE", "FRZ", "SIZE", "SPRY", "PTCH", "HOLD", 60, 30, 64, 0),
    P("SHIFT", "SFT", "PTCH", "FB", "WIN", "TONE", 96, 40, 60, 100),
};
#undef P

#define FX_CH 32                          /* samples a chunk */
#define MASK (VB_FX_BUF - 1)
#define QUIET_LVL 2                       /* a wet peak this small counts as silence (-84 dB) */
#define SLEEP_BUF (VB_FX_BUF / FX_CH + 4) /* chunks that flush the whole buffer */
#define MIX_UNSET (-1)                    /* z[11]: the mix reached (Q16, 256 << 8 = all wet), unset after a reset */

/* the z[] entries that are filter states (cleared when the slot falls asleep); the others are bookkeeping */
static const uint8_t NSTATE[VB_FX_TYPES] = { 0, 5, 3, 6, 2, 4, 3, 4, 0, 9, 1, 4, 0, 1 };

/* 2^(i/256), Q16: 65536 * 2^(i/256), i = 0..256 */
static const uint32_t FX_EXP2[257] = {
    65536, 65714, 65892, 66071, 66250, 66429, 66609, 66790, 66971, 67153, 67335, 67517,
    67700, 67884, 68068, 68252, 68438, 68623, 68809, 68996, 69183, 69370, 69558, 69747,
    69936, 70126, 70316, 70507, 70698, 70889, 71082, 71274, 71468, 71661, 71856, 72050,
    72246, 72442, 72638, 72835, 73032, 73230, 73429, 73628, 73828, 74028, 74229, 74430,
    74632, 74834, 75037, 75240, 75444, 75649, 75854, 76060, 76266, 76473, 76680, 76888,
    77096, 77305, 77515, 77725, 77936, 78147, 78359, 78572, 78785, 78998, 79212, 79427,
    79642, 79858, 80075, 80292, 80510, 80728, 80947, 81166, 81386, 81607, 81828, 82050,
    82273, 82496, 82719, 82944, 83169, 83394, 83620, 83847, 84074, 84302, 84531, 84760,
    84990, 85220, 85451, 85683, 85915, 86148, 86382, 86616, 86851, 87086, 87322, 87559,
    87796, 88034, 88273, 88513, 88752, 88993, 89234, 89476, 89719, 89962, 90206, 90451,
    90696, 90942, 91188, 91436, 91684, 91932, 92181, 92431, 92682, 92933, 93185, 93438,
    93691, 93945, 94200, 94455, 94711, 94968, 95226, 95484, 95743, 96002, 96263, 96524,
    96785, 97048, 97311, 97575, 97839, 98104, 98370, 98637, 98905, 99173, 99442, 99711,
    99982, 100253, 100524, 100797, 101070, 101344, 101619, 101895, 102171, 102448, 102726, 103004,
    103283, 103564, 103844, 104126, 104408, 104691, 104975, 105260, 105545, 105831, 106118, 106406,
    106694, 106984, 107274, 107565, 107856, 108149, 108442, 108736, 109031, 109326, 109623, 109920,
    110218, 110517, 110816, 111117, 111418, 111720, 112023, 112327, 112631, 112937, 113243, 113550,
    113858, 114167, 114476, 114787, 115098, 115410, 115723, 116036, 116351, 116667, 116983, 117300,
    117618, 117937, 118257, 118577, 118899, 119221, 119544, 119869, 120194, 120519, 120846, 121174,
    121502, 121832, 122162, 122493, 122825, 123158, 123492, 123827, 124163, 124500, 124837, 125176,
    125515, 125855, 126197, 126539, 126882, 127226, 127571, 127917, 128263, 128611, 128960, 129310,
    129660, 130012, 130364, 130718, 131072,
};
/* tan(pi fc / fs), Q16, fc = 20 Hz * 1000^(i/128) (20 Hz .. 20 kHz), fc held at 0.45 fs */
static const int32_t FX_TANG[129] = {
    93, 99, 104, 110, 116, 122, 129, 136, 144, 152,
    160, 169, 178, 188, 199, 210, 221, 234, 247, 260,
    275, 290, 306, 323, 341, 360, 380, 401, 423, 447,
    471, 497, 525, 554, 585, 617, 652, 688, 726, 766,
    809, 853, 901, 951, 1003, 1059, 1118, 1180, 1245, 1314,
    1387, 1464, 1545, 1631, 1722, 1817, 1918, 2024, 2137, 2255,
    2380, 2513, 2652, 2799, 2955, 3119, 3292, 3475, 3668, 3872,
    4087, 4314, 4554, 4808, 5075, 5358, 5656, 5972, 6305, 6657,
    7029, 7422, 7837, 8276, 8740, 9231, 9750, 10300, 10881, 11496,
    12148, 12838, 13570, 14345, 15168, 16041, 16969, 17955, 19004, 20121,
    21312, 22583, 23940, 25393, 26951, 28624, 30424, 32366, 34466, 36745,
    39226, 41937, 44914, 48199, 51846, 55922, 60514, 65735, 71739, 78731,
    87003, 96979, 109297, 124971, 145709, 174640, 218174, 291850, 413778,
};

/* IMA ADPCM */
static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int8_t IMA_IDX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* RESON chords: semitones above the root of each line, CHRD (0..127) picks one of nine */
static const int8_t CHORD[9][4] = {
    { 0, 0, 0, 0 }, { 0, 12, 0, 0 }, { 0, 7, 12, 0 }, { 0, 4, 7, 0 }, { 0, 3, 7, 0 },
    { 0, 5, 7, 0 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 7, 12, 19 },
};
static const uint8_t CHORD_N[9] = { 1, 2, 3, 3, 3, 3, 4, 4, 4 };
static const int32_t OUT_N[5] = { 0, 32767, 23170, 18919, 16384 };   /* 1 / sqrt(strings) */

#define INC_C0 796254                     /* u32 phase increment of MIDI note 0 (8.18 Hz) */
#define PERIOD_C0_Q8 1380856              /* the period of MIDI note 0 in samples, Q8 */

/* ---------------------------------------------------------------- helpers */

/* one-pole low-pass whose step is rounded away from zero: the state reaches its target exactly (no dead band that
 * would hold a loop at a small constant for ever). |x - *z| * k must fit 31 bits. */
static inline int32_t lpz(int32_t *z, int32_t x, int32_t k)
{
    int32_t e = x - *z;
    *z += ((e * k) >> 15) + (e > 0);
    return *z;
}

/* x minus its one-pole low-pass at fs / (2 pi 2^sh), the low-pass (Q6) carrying its step's remainder (MNM-1's
 * dc_block): no dead band and no minimum step, so it is linear down to the last bit and its output reaches exact 0
 * once x stays 0. |x| < 2^24. */
static inline int32_t hpz(int32_t *lp, int32_t *err, int32_t x, int sh)
{
    int32_t e = x * 64 - *lp + *err, d = e >> sh;
    *err = e - d * (1 << sh);
    *lp += d;
    return x - ((*lp + 32) >> 6);
}

/* soft clip: linear to +-16384, then a tanh knee up to +-32767 */
static inline int32_t softclip(int32_t x)
{
    if (x > 16384)
        return 16384 + (vb_tanh((x - 16384) >> 1) >> 1);
    if (x < -16384)
        return -16384 - (vb_tanh((-16384 - x) >> 1) >> 1);
    return x;
}

/* 2^(x / 65536) in Q16, x in -16 .. +14 octaves */
static uint32_t exp2_q16(int32_t x)
{
    int32_t i = x >> 16;
    uint32_t f = (uint32_t)x & 0xFFFFu, j = f >> 8;
    uint32_t m = FX_EXP2[j] + (((FX_EXP2[j + 1] - FX_EXP2[j]) * (f & 255u)) >> 8);
    if (i > 14)
        i = 14;
    return i >= 0 ? m << i : i > -31 ? m >> -i : 0;
}

/* the phase increment of a pitch in semitones over MIDI note 0, Q8 */
static uint32_t note_inc(int32_t semis_q8)
{
    semis_q8 = vb_clamp(semis_q8, 0, 135 << 8);
    return (uint32_t)vb_mul_sh(INC_C0, (int32_t)exp2_q16(semis_q8 * 64 / 3), 16);
}

/* tan(pi fc / fs) at a Q8 index into FX_TANG */
static int32_t tang(int32_t c)
{
    int32_t i = c >> 8;
    return FX_TANG[i] + (((FX_TANG[i + 1] - FX_TANG[i]) * (c & 255)) >> 8);
}

/* triangle, Q15, in phase with vb_sin (0 at 0, rising) */
static inline int32_t tri15(uint32_t ph)
{
    int32_t t = (int32_t)((ph + 0x40000000u) >> 15);
    return t < 65536 ? t - 32768 : 98303 - t;
}

/* a read between samples of the whole buffer, the position Q16 */
static inline int32_t rd(const int16_t *b, uint32_t pos)
{
    uint32_t i = (pos >> 16) & MASK;
    int32_t a = b[i], c = b[(i + 1) & MASK];
    return a + (((c - a) * (int32_t)((pos >> 1) & 0x7FFF)) >> 15);
}

static inline int32_t sat(int32_t x) { return vb_clamp(x, -32768, 32767); }   /* (one ssat) */
static inline int32_t iabs(int32_t x) { return x < 0 ? -x : x; }

/* ---------------------------------------------------------------- DRIVE: DRV TONE BIAS OUT
 * MNM-1's DIST: low cut (~60 Hz, keeps the bass out of the clipper) -> gain 1x .. 9x (gentle at first) -> tanh,
 * offset by BIAS (even harmonics; the offset's own output taken off, so silence stays 0) -> DC blocker -> one-pole
 * TONE -> make-up that falls with the drive, times OUT (64 = x1). z: 0..1 low cut (55 Hz), 2..3 DC (14 Hz), 4 tone. */
static void fx_drive(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t d = p[0], g = 4096 + d * d * 2, b = p[2] * 40, b0 = vb_tanh(b);
    int32_t k = VB_ONEPOLE[40 + p[1] * 87 / 127], gain = ((32767 - d * 120) * (p[3] * 512)) >> 15;
    int32_t z[5];                                        /* (the states in locals: the loop is the cost) */
    for (int j = 0; j < 5; j++) z[j] = s->z[j];
    for (int i = 0; i < n; i++) {
        int32_t v = hpz(&z[0], &z[1], x[i], 7);         /* |v| < 65536 */
        v = vb_tanh((((v >> 1) * g) >> 13) + b) - b0;   /* 32767 * 36354 < 2^31 */
        if (b)
            v = sat(hpz(&z[2], &z[3], v, 9));           /* (an asymmetric clip leaves a DC) */
        x[i] = (lpz(&z[4], v, k) * gain) >> 15;         /* gain < 65024 */
    }
    for (int j = 0; j < 5; j++) s->z[j] = z[j];
}

/* ---------------------------------------------------------------- FOLD: AMT SYM PRE OUT
 * a sine wavefolder: PRE low-passes what goes in (127 open: the folds of the highs alias less when closed), AMT the
 * gain into the folder (x1 .. x13, unity small-signal gain at 0), SYM an offset (64 symmetric: odd harmonics only),
 * a DC blocker after it (14 Hz), OUT (64 = x1). z: 0 pre, 1..2 DC. */
static void fx_fold(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t g = 2608 + p[0] * p[0] * 2, off = (p[1] - 64) * 512, y0 = vb_sin((uint32_t)off << 15);
    int32_t k = VB_ONEPOLE[30 + p[2] * 97 / 127], gain = p[3] * 512;
    int32_t z0 = s->z[0], z1 = s->z[1], z2 = s->z[2];
    for (int i = 0; i < n; i++) {
        int32_t u = ((lpz(&z0, x[i], k) * g) >> 12) + off;        /* 32767 * 34866 < 2^31 */
        int32_t v = vb_sin((uint32_t)u << 15) - y0;               /* (a quarter turn per 32768: wraps = folds) */
        x[i] = (sat(hpz(&z1, &z2, v, 9)) * gain) >> 15;
    }
    s->z[0] = z0;
    s->z[1] = z1;
    s->z[2] = z2;
}

/* ---------------------------------------------------------------- CRUNCH: AMT RATE HISS OUT
 * MNM-1's bus CRUNCH: an IMA ADPCM round trip (4 bits a sample) at a lower rate, held between its samples. AMT drives
 * the coder (x1 .. x4, hard clipped) and takes bits off the code (4, 3, then 2 bits). RATE: the coder's rate,
 * 44.1 kHz (127) down 4.5 octaves (1.9 kHz). HISS: filtered noise at the input's level (an envelope follower: no hiss
 * in silence), crunched with it. OUT (64 = x1).
 * z: 0 predictor, 1 step index, 2 held sample, 3 envelope, 4 hiss filter, 5 rate accumulator. */
static int32_t ima(int32_t *pred, int32_t *idx, int32_t x, int mask)
{
    int32_t step = IMA_STEP[*idx], diff = x - *pred, code = 0, delta = step >> 3;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
        delta += step;
    }
    if ((mask & 2) && diff >= step >> 1) {
        code |= 2;
        diff -= step >> 1;
        delta += step >> 1;
    }
    if ((mask & 1) && diff >= step >> 2) {
        code |= 1;
        delta += step >> 2;
    }
    if (x == 0 && iabs(*pred) <= step)                 /* silence decodes to exact 0 (no limit cycle around it) */
        *pred = 0;
    else
        *pred = sat(*pred + ((code & 8) ? -delta : delta));
    *idx = vb_clamp(*idx + IMA_IDX[code & 7], 0, 88);
    return *pred;
}

static void fx_crunch(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t amt = p[0], gin = 256 + amt * 768 / 127, mask = amt < 43 ? 7 : amt < 86 ? 6 : 4;
    int32_t div = (int32_t)(exp2_q16((127 - p[1]) * 9 * 65536 / 254) >> 8);   /* Q8 samples a coder step */
    int32_t hg = p[2] * p[2] / 4, gain = p[3] * 512, z[6];
    for (int j = 0; j < 6; j++) z[j] = s->z[j];
    for (int i = 0; i < n; i++) {
        int32_t v = sat((x[i] * gin) >> 8);
        if (hg) {
            int32_t a = iabs(x[i]);
            lpz(&z[3], a, a > z[3] ? 8192 : 60);       /* the level: fast up, ~150 ms down */
            z[4] += ((vb_noise(&s->rng) - z[4]) * 9000) >> 15;
            v = sat(v + ((((z[4] * z[3]) >> 15) * hg) >> 15));
        }
        z[5] += 256;
        if (z[5] >= div) {
            z[5] -= div;
            z[2] = ima(&z[0], &z[1], v, mask);
        }
        x[i] = (z[2] * gain) >> 15;
    }
    for (int j = 0; j < 6; j++) s->z[j] = z[j];
}

/* ---------------------------------------------------------------- BITS: BITS SR JIT OUT
 * bit depth 1 .. 16 (127: 16, rounded to the nearest step, 0 stays 0), sample and hold at 44.1 kHz (127) down to
 * 340 Hz, JIT stretches each held sample by a random part of a period (an unsteady clock). z: held sample, clock. */
static void fx_bits(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t sh = 15 - p[0] * 15 / 127, half = sh ? 1 << (sh - 1) : 0, jit = p[2], gain = p[3] * 512;
    int32_t inc = (int32_t)exp2_q16(((int32_t)p[1] - 127) * 7 * 65536 / 127);
    int32_t hold = s->z[0], clk = s->z[1];
    for (int i = 0; i < n; i++) {
        clk += inc;
        if (clk >= 65536) {
            clk -= 65536;
            if (jit)
                clk -= (int32_t)(((vb_rng(&s->rng) >> 16) * (uint32_t)jit) >> 7);
            hold = sat(((x[i] + half) >> sh) * (1 << sh));
        }
        x[i] = (hold * gain) >> 15;
    }
    s->z[0] = hold;
    s->z[1] = clk;
}

/* ---------------------------------------------------------------- FILTER: CUT RES TYPE DRV
 * a trapezoidal (TPT) state-variable filter: stable at any cutoff and resonance. CUT 20 Hz .. 16 kHz (glides),
 * RES up to the edge of self-oscillation, TYPE sweeps LP (0) -> BP (64) -> HP (127), DRV a tanh in front (0: clean).
 * States at 256x the signal (Q23), clamped to 4x full scale; a product splits v into (v >> 10) and (v & 1023) to
 * keep 32 bits and the full precision (at low cutoffs a1 is close to 1: with fewer bits, quiet signals fell into the
 * rounding and vanished). a1 ic1, the band-pass state's own decay, rounds toward zero: with it floored too, high
 * resonance held small limit cycles for ever.
 * The rounded loop can come to rest on a small constant once the input stops: a DC blocker (6 Hz) on the output
 * takes it off, so the tail still ends in exact zeros. z: 0 ic1, 1 ic2, 2..3 DC; 8 the cutoff (Q8 index, glides), 9 set. */
#define SVP(c, v) (((((v) >> 10) * (c)) >> 5) + ((((v) & 1023) * (c)) >> 15))
static inline int32_t SVZ(int32_t c, int32_t v) { return v < 0 ? -SVP(c, -v) : SVP(c, v); }  /* toward zero */
#define SVC(x) vb_clamp(x, -(1 << 25), (1 << 25) - 1)
static void fx_filter(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t tgt = vb_clamp(p[0] * 124 / 127, 0, 124) << 8, *z = s->z;
    if (!z[9]) {
        z[8] = tgt;
        z[9] = 1;
    }
    z[8] += (tgt - z[8]) / 4;
    int32_t g = tang(z[8]), k = 65536 - p[1] * 64224 / 127;                 /* k: 2 .. 0.04, Q15 */
    int32_t den = 65536 + vb_mul_sh(g, g + 2 * k, 16);                       /* 1 + g (g + k), Q16 */
    int32_t a1 = (int32_t)(0x80000000u / (uint32_t)den), a2 = vb_mul_sh(g, a1, 16), a3 = vb_mul_sh(g, a2, 16);
    int32_t t = p[2], wl = t < 64 ? 64 - t : 0, wb = t < 64 ? t : 127 - t, wh = t < 64 ? 0 : t - 64;
    int32_t gd = 4096 + p[3] * p[3] * 2, drv = p[3];
    int32_t ic1 = z[0], ic2 = z[1];
    for (int i = 0; i < n; i++) {
        int32_t v0 = (drv ? vb_tanh((x[i] * gd) >> 14) : x[i]) * 256;
        int32_t v3 = v0 - ic2;
        int32_t v1 = SVC(SVZ(a1, ic1) + SVP(a2, v3));
        int32_t v2 = SVC(ic2 + SVP(a2, ic1) + SVP(a3, v3));
        ic1 = SVC(2 * v1 - ic1);
        ic2 = SVC(2 * v2 - ic2);
        int32_t y = (v2 >> 8) * wl + (v1 >> 8) * wb;
        if (wh)
            y += (SVC(v0 - SVP(k, v1) - v2) >> 8) * wh;   /* the high-pass */
        y = sat(y / 64);
        x[i] = sat(hpz(&z[2], &z[3], y, 10));
    }
    z[0] = ic1;
    z[1] = ic2;
}

/* ---------------------------------------------------------------- TAPE DLY: TIME FB HP LP
 * MNM-1's track delay, mono: TIME in 1/256 notes of the tempo (64 = a beat, 127 ~ half a bar), up to 2 s. A tape of
 * 8190 cells written at a rate that makes the loop the delay's length: a cell a sample up to 186 ms, longer = fewer
 * cells a second, each the average of its samples (the lower fidelity of a slowed tape). A new TIME glides the rate
 * (the pitch bends, no click). The feedback runs once a cell through a high-pass (HP) and a low-pass (LP) and a soft
 * clip: FB 0 .. 1.1, so it self-oscillates at the top and stays bounded.
 * z: 0 the cell's sum, 1 high-pass (Q8), 2 low-pass; 8 rate (cells a sample, Q16), 9 the loop (cells, Q16).
 * ph[0]: the write head (cells, Q16). */
#define TAPE_CELLS (VB_FX_BUF - 2)
static void fx_tape(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n, int bpm10)
{
    uint32_t beat = 26460000u / (uint32_t)vb_clamp(bpm10, 200, 3000);     /* samples a beat */
    uint32_t d = (uint32_t)(p[0] ? p[0] : 1) * beat / 64u, rt, dt;
    d = d < 64u ? 64u : d > 88200u ? 88200u : d;
    if (d <= TAPE_CELLS) {
        rt = 65536u;
        dt = d << 16;
    } else {
        rt = ((uint32_t)TAPE_CELLS << 16) / d;
        dt = (uint32_t)TAPE_CELLS << 16;
    }
    int32_t *z = s->z;
    if (!z[8]) {                                         /* first chunk: at once */
        z[8] = (int32_t)rt;
        z[9] = (int32_t)dt;
    }
    int32_t rate = z[8], dc = z[9];
    int32_t r1 = rate + ((int32_t)rt - rate) / 16, d1 = dc + ((int32_t)dt - dc) / 16;   /* a new time glides */
    int32_t drate = (r1 - rate) / n, ddc = (d1 - dc) / n;
    int32_t fb = p[1] * 284, ah = VB_ONEPOLE[p[2] * 80 / 127], al = VB_ONEPOLE[40 + p[3] * 87 / 127];
    int32_t acc = z[0], h = z[1], l = z[2];
    uint32_t wp = s->ph[0];
    int16_t *b = s->buf;
    for (int i = 0; i < n; i++) {
        uint32_t nx = wp + (uint32_t)rate;
        int32_t y = rd(b, wp - (uint32_t)dc);
        acc += x[i];
        if ((nx ^ wp) >> 16) {                           /* a cell full: its average plus the loop */
            uint32_t c = (wp >> 16) & MASK, r = (c - ((uint32_t)dc >> 16)) & MASK;
            int32_t in = b[r] * 256;
            h += vb_mul_sh(in - h, ah, 15);             /* the high-pass' low-pass, Q8 (low cutoffs: precision) */
            lpz(&l, sat((in - h) / 256), al);
            int32_t a = rate >= 65536 ? acc : vb_mul_sh(acc, rate, 16);
            b[c] = (int16_t)sat(softclip(a + l * fb / 32768));   /* (toward 0: the loop dies out) */
            acc = 0;
        }
        wp = nx;
        rate += drate;
        dc += ddc;
        x[i] = y;
    }
    z[0] = acc;
    z[1] = h;
    z[2] = l;
    z[8] = r1;
    z[9] = d1;
    s->ph[0] = wp;
}

/* ---------------------------------------------------------------- REVERB: SIZE DEC DAMP GATE
 * Schroeder / Freeverb: four damped combs in parallel into two allpasses (g = 0.5), 7415 samples of the buffer. SIZE
 * scales the comb lengths 0.35x .. 1.3x (gliding a sample a chunk at most), DEC their feedback (0.70 .. 0.98), DAMP
 * the low-pass in their loops. GATE 0: off; else the tail closes GATE x 10 ms after the input last sounded (a 3 ms
 * fade, the combs' feedback with it, so the room empties) and opens again with the input (4 ms).
 * z: 0..3 comb low-passes, 4 gate count, 5 gate gain, 6..7 allpass positions; 8 size (Q16). ph[0..3]: comb positions. */
static const uint16_t REV_COMB[4] = { 1451, 1544, 1660, 1763 };
static const uint16_t REV_AP[2] = { 556, 441 };
#define REV_AP0 (1451 + 1544 + 1660 + 1763)
static void fx_reverb(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t *z = s->z, tgt = (69 + p[0] * 187 / 127) << 8;   /* of 256: 0.27 .. 1.0 of the regions */
    if (!z[8])
        z[8] = tgt;
    z[8] += vb_clamp(tgt - z[8], -64, 64);
    int32_t len[4], base[4], fb = 22938 + p[1] * 72, damp = VB_ONEPOLE[120 - p[2] * 80 / 127];
    int32_t gt = p[3] * 441, o = 0;
    for (int c = 0; c < 4; c++) {
        len[c] = (REV_COMB[c] * z[8]) >> 16;
        base[c] = o;
        o += REV_COMB[c];
    }
    if (!gt)
        z[5] = 32767;
    int16_t *b = s->buf;
    int32_t lp[4], ix[4], ap0 = z[6], ap1 = z[7], hold = z[4], gg = z[5];   /* (locals: the loop is the cost) */
    for (int c = 0; c < 4; c++) {
        lp[c] = z[c];
        ix[c] = (int32_t)s->ph[c];
    }
    for (int i = 0; i < n; i++) {
        int32_t in = x[i] / 4, wet = 0, fbe = fb;
        if (gt) {
            if (x[i] > 1024 || x[i] < -1024)
                hold = gt;
            if (hold) {
                hold--;
                gg = gg + 1024 > 32767 ? 32767 : gg + 1024;
            } else {
                gg = gg > 256 ? gg - 256 : 0;
            }
            fbe = fb * gg / 32768;
        }
        for (int c = 0; c < 4; c++) {                   /* the combs */
            int16_t *q = b + base[c] + ix[c];
            int32_t y = *q;
            *q = (int16_t)sat(in + lpz(&lp[c], y, damp) * fbe / 32768);
            if (++ix[c] >= len[c])
                ix[c] = 0;
            wet += y;
        }
        int32_t u = wet / 2, d = b[REV_AP0 + ap0], ov = d - u / 2;   /* the allpasses */
        b[REV_AP0 + ap0] = (int16_t)sat(u + ov / 2);
        if (++ap0 >= REV_AP[0])
            ap0 = 0;
        u = ov;
        d = b[REV_AP0 + REV_AP[0] + ap1];
        ov = d - u / 2;
        b[REV_AP0 + REV_AP[0] + ap1] = (int16_t)sat(u + ov / 2);
        if (++ap1 >= REV_AP[1])
            ap1 = 0;
        x[i] = sat(gt ? ov * gg / 32768 : ov);
    }
    for (int c = 0; c < 4; c++) {
        z[c] = lp[c];
        s->ph[c] = (uint32_t)ix[c];
    }
    z[4] = hold;
    z[5] = gg;
    z[6] = ap0;
    z[7] = ap1;
}

/* ---------------------------------------------------------------- CHORUS: RATE DEP FB WIDE
 * MNM-1's CHRS: three taps of a 15 ms delay swung by triangles (DEP up to +-8 ms, RATE 0.05 .. 27 Hz), FB fed back
 * (0 .. 0.89). Mono out, so WIDE spreads the taps' phases (0: one tap thrice, 127: 120 degrees apart).
 * Each tap's delay ramps over the chunk to its new value. z: 8..10 the taps' delays (Q8). ph[0] LFO, ph[1] write. */
static void fx_chorus(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t dq[3], dd[3], dep = p[1] * 711, fb = p[2] * 230;      /* dep: Q8 samples (+-353) */
    uint32_t spread = (0x55555555u / 127u) * p[3];
    s->ph[0] += VB_LFO_INC[p[0]] * (uint32_t)n;
    for (int j = 0; j < 3; j++) {
        int32_t d1 = (661 << 8) + (((tri15(s->ph[0] + spread * (uint32_t)j) >> 3) * dep) >> 12);   /* 4096 * 90297 */
        dq[j] = s->z[8 + j] ? s->z[8 + j] : d1;
        dd[j] = (d1 - dq[j]) / n;
        s->z[8 + j] = d1;
    }
    uint32_t w = s->ph[1];
    int16_t *b = s->buf;
    for (int i = 0; i < n; i++, w++) {
        int32_t wet = 0;
        for (int j = 0; j < 3; j++) {
            dq[j] += dd[j];
            wet += rd(b, (w << 16) - ((uint32_t)dq[j] << 8));
        }
        wet = (wet * 21845) >> 16;                      /* / 3 (|wet| < 98302) */
        b[w & MASK] = (int16_t)sat(x[i] + wet * fb / 32768);
        x[i] = wet;
    }
    s->ph[1] = w;
}

/* ---------------------------------------------------------------- PHASER: RATE DEP CNTR FB
 * four first-order allpasses (two notches, as a Phase 90) swept together by a triangle (RATE) around CNTR (110 Hz .. 9 kHz), DEP octaves
 * (+-3 at 127); the chain's output fed back (FB 0 .. 0.89); the wet is the classic sum of input and chain (notches).
 * The coefficient ramps over the chunk. Near a = -1 (low centres) the rounded allpasses can rest on a constant once
 * the input stops: a DC blocker (6 Hz) on the output takes it off.
 * z: 0..3 the allpasses, 6 the feedback, 7..8 DC; 9 the coefficient (Q13). ph[0]: LFO. */
static void fx_phaser(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t *z = s->z;
    s->ph[0] += VB_LFO_INC[p[0]] * (uint32_t)n;
    int32_t c = (32 << 8) + p[2] * (73 << 8) / 127 + (((tri15(s->ph[0]) >> 3) * (p[1] * 9728 / 127)) >> 12);
    int32_t g = tang(vb_clamp(c, 0, 112 << 8));
    int32_t a1 = (g - 65536) * 8192 / (g + 65536), a = z[9], da = (a1 - a) / n, fb = p[3] * 230;
    int32_t s0 = z[0], s1 = z[1], s2 = z[2], s3 = z[3], last = z[6], dc = z[7], de = z[8], y;
    for (int i = 0; i < n; i++) {
        int32_t f = last * fb / 32768;                  /* the feedback, a step nearer 0 (with floored allpasses,
                                                         * the loop could otherwise keep a small cycle going) */
        int32_t u = vb_clamp(x[i] + f - vb_clamp(f, -1, 1), -65536, 65535);
        a += da;
#define AP(st) y = vb_clamp(((a * u) >> 13) + st, -65536, 65535); st = u - ((a * y) >> 13); u = y;
        AP(s0) AP(s1) AP(s2) AP(s3)                     /* (a + z^-1) / (1 + a z^-1), transposed; |a u| < 2^29 */
#undef AP
        last = u;
        x[i] = sat(hpz(&dc, &de, (x[i] + u) >> 1, 10));
    }
    z[0] = s0;
    z[1] = s1;
    z[2] = s2;
    z[3] = s3;
    z[6] = last;
    z[7] = dc;
    z[8] = de;
    z[9] = a1;
}

/* ---------------------------------------------------------------- RING: TUNE WAVE EXT KEY
 * the input times a carrier. Its pitch: TUNE as a MIDI note (60 = C4) plus the played note's distance from C4 times
 * KEY/127 (KEY 127, TUNE 60: the carrier at the played note; KEY 0: a fixed carrier, 8 Hz .. 12.5 kHz). WAVE morphs
 * sine (0) -> triangle (64) -> square (127). EXT leaks the carrier itself in at the input's level (an envelope
 * follower: a tone that sounds with the input, silent without). z: envelope. ph[0]: carrier. */
static void fx_ring(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n, int note)
{
    int32_t nt = note < 0 || note > 127 ? 60 : note, wv = p[1], ext = p[2] * 258;
    int32_t wq = wv < 64 ? wv * 512 : (wv - 64) * 520;  /* the morph, Q15 (< 32761: (65534 * wq) fits) */
    uint32_t inc = note_inc(p[0] * 256 + (nt - 60) * p[3] * 256 / 127), ph = s->ph[0];
    int32_t env = s->z[0];
    for (int i = 0; i < n; i++, ph += inc) {
        int32_t c, a = iabs(x[i]);
        if (wv < 64) {
            int32_t sn = vb_sin(ph);
            c = wv ? sn + (((tri15(ph) - sn) * wq) >> 15) : sn;
        } else {
            int32_t t = tri15(ph);
            c = t + ((((ph < 0x80000000u ? 32767 : -32767) - t) * wq) >> 15);
        }
        int32_t y = (x[i] * c) >> 15;
        if (ext) {
            lpz(&env, a, a > env ? 8192 : 100);
            y += (((c * env) >> 15) * ext) >> 15;
        }
        x[i] = sat(y);
    }
    s->ph[0] = ph;
    s->z[0] = env;
}

/* ---------------------------------------------------------------- RESON: TUNE DEC TONE CHRD
 * Karplus-Strong strings excited by the input, tuned to the played note + TUNE (-24 .. +24 semitones, 64 = the note).
 * DEC: the ring time (a per-loop gain from the loop's length, so all strings ring as long), TONE: the loop's low-pass
 * (its delay taken off the line, so it stays in tune). CHRD 0: one string; above: 2 to 4 strings in a chord (octave,
 * fifth+octave, major, minor, sus4, maj7, min7, power+9th). Each string has a 2048-sample quarter of the buffer
 * (down to 21.5 Hz). The strings' sum is scaled by 1 / sqrt(strings) and DC-blocked.
 * z: 0..3 the strings' low-passes; 4..7 their delays (Q8, glide); 8..9 DC. ph[0]: write position. */
static void fx_reson(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n, int note)
{
    int32_t nt = note < 0 || note > 127 ? 60 : note, ci = (p[3] + 15) >> 4, nl = CHORD_N[ci];
    int32_t dt = p[0] - 64, root = (nt + (dt * 24 + (dt < 0 ? -31 : 31)) / 63) * 256;
    int32_t k = VB_ONEPOLE[50 + p[2] * 77 / 127];
    int32_t comp = ((32768 - k) << 8) / k, e = (1 << 30) - VB_DECAY[40 + p[1] * 87 / 127];
    int32_t g[4], dq[4], dd[4], *z = s->z;
    for (int j = 0; j < nl; j++) {
        int32_t sq8 = root + CHORD[ci][j] * 256;
        int32_t per = vb_mul_sh(PERIOD_C0_Q8, (int32_t)exp2_q16(-(sq8 * 64 / 3)), 16);
        per = vb_clamp(per, 8 << 8, 2040 << 8);
        int32_t t = vb_mul_sh(per, e, 22);              /* per * e / 2^30, Q16: the loop's decay in nepers */
        int32_t gq = (int32_t)(exp2_q16(-vb_mul_sh(t, 94548, 16)) >> 1);   /* / ln 2 */
        g[j] = gq > 32767 ? 32767 : gq;
        int32_t dl = vb_clamp(per - comp, 2 << 8, 2040 << 8);
        if (!z[4 + j])
            z[4 + j] = dl;
        dq[j] = z[4 + j];                                /* a new pitch glides, the delay ramped sample by sample */
        z[4 + j] += (dl - z[4 + j]) / 4;
        dd[j] = (z[4 + j] - dq[j]) / n;
    }
    uint32_t w = s->ph[0];
    int32_t lp[4], dc = z[8], de = z[9], on = OUT_N[nl];
    for (int j = 0; j < nl; j++)
        lp[j] = z[j];
    int16_t *b = s->buf;
    for (int i = 0; i < n; i++, w = (w + 1) & 2047u) {
        int32_t in = x[i] / 4, out = 0;
        for (int j = 0; j < nl; j++) {
            int16_t *q = b + j * 2048;
            dq[j] += dd[j];
            uint32_t r = (w - ((uint32_t)dq[j] >> 8)) & 2047u;
            int32_t a = q[r], c = q[(r - 1) & 2047u];
            int32_t y = lpz(&lp[j], a + (((c - a) * (dq[j] & 255)) >> 8), k);
            q[w] = (int16_t)sat(in + y * g[j] / 32768);
            out += y;
        }
        x[i] = sat(hpz(&dc, &de, (out * on) >> 15, 10));   /* (a DC blocker: a one-sided strike leaves a DC) */
    }
    for (int j = 0; j < nl; j++)
        z[j] = lp[j];
    z[8] = dc;
    z[9] = de;
    s->ph[0] = w;
}

/* ---------------------------------------------------------------- FREEZE: SIZE SPRY PTCH HOLD
 * after MNM-1's PERFORM FREEZE: two grains with triangle windows half a grain apart read the recent buffer. SIZE: the
 * grain, 6 .. 58 ms; SPRY: how far back a grain may start at random (0: always the newest); PTCH: the grains'
 * pitch, -12 .. +12 semitones (64: as recorded); HOLD 64 and over stops the recording: the capture is frozen.
 * A grain takes the current SIZE and pitch when it starts. z: grain g at 3g: its sample count, length, 2^27/length.
 * ph[0]: write position, ph[1..2]: the grains' read positions (Q16). */
static void fx_freeze(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t gl = 256 + p[0] * 2304 / 127, *z = s->z;
    uint32_t r = exp2_q16((p[2] - 64) * 1024), w = s->ph[0];
    int hold = p[3] >= 64;
    int16_t *b = s->buf;
    if (!z[1]) {                                         /* after a reset: grain 1 half a grain behind grain 0 */
        z[1] = z[4] = gl;
        z[2] = z[5] = (1 << 27) / gl;
        z[0] = gl;
        z[3] = gl / 2;
        s->ph[2] = (w - (uint32_t)gl) << 16;
    }
    for (int i = 0; i < n; i++) {
        int32_t y = 0;
        if (!hold)
            b[w++ & MASK] = (int16_t)x[i];
        for (int g = 0; g < 2; g++) {
            int32_t *gz = z + 3 * g;
            if (gz[0] >= gz[1]) {                        /* a new grain */
                int32_t span = (int32_t)((gl * r) >> 16), avail = 8180 - gl - span;
                uint32_t back = (uint32_t)span + 2u;
                if (avail > 0 && p[1])
                    back += ((vb_rng(&s->rng) >> 16) * (uint32_t)(avail * p[1] / 127)) >> 16;
                gz[0] = 0;
                gz[1] = gl;
                gz[2] = (1 << 27) / gl;
                s->ph[1 + g] = (w - back) << 16;
            }
            int32_t fr = (gz[0]++ * gz[2]) >> 12, win = fr < 16384 ? fr * 2 : (32768 - fr) * 2;
            y += rd(b, s->ph[1 + g]) * win / 32768;
            s->ph[1 + g] += r;
        }
        x[i] = sat(y);
    }
    s->ph[0] = w;
}

/* ---------------------------------------------------------------- SHIFT: PTCH FB WIN TONE
 * after MNM-1's PERFORM OCT: two taps sweep a delay of WIN (6 .. 93 ms) half a turn apart, each faded by a raised
 * cosine that is silent where its delay jumps. PTCH -24 .. +24 semitones (64: none). TONE low-passes the shifted
 * signal; FB (0 .. 0.9) sends it back into the delay through a soft clip: each pass shifts again (the shimmer).
 * z: 0 tone; 1 the window (Q8, glides). ph[0]: write position, ph[1]: the sweep. */
static void fx_shift(vb_fx_state_t *s, const uint8_t *p, int32_t *x, int n)
{
    int32_t dp = p[0] - 64, semis = (dp * 24 + (dp < 0 ? -31 : 31)) / 63;
    int32_t r = (int32_t)exp2_q16(semis * 65536 / 12), wn = 256 + p[2] * 3840 / 127;
    int32_t inc = vb_mul_sh(r - 65536, (int32_t)(0xFFFFFFFFu / (uint32_t)wn), 16);
    int32_t fb = p[1] * 232, k = VB_ONEPOLE[40 + p[3] * 87 / 127];
    uint32_t w = s->ph[0], ph = s->ph[1];
    int16_t *b = s->buf;
    int32_t tone = s->z[0], wq = s->z[1] ? s->z[1] : wn << 8;   /* the window glides (Q8), no jump in the taps */
    s->z[1] = wq + ((wn << 8) - wq) / 4;
    int32_t dw = (s->z[1] - wq) / n;
    for (int i = 0; i < n; i++, w++, ph -= (uint32_t)inc) {
        uint32_t wcur = (uint32_t)(wq += dw) >> 8;
        uint32_t da = (16u << 16) + (ph >> 16) * wcur, db = (16u << 16) + ((ph + 0x80000000u) >> 16) * wcur;
        int32_t ya = rd(b, (w << 16) - da), yb = rd(b, (w << 16) - db);
        int32_t wa = (32768 - vb_sin(ph + 0x40000000u)) >> 1;
        int32_t y = lpz(&tone, (ya * wa + yb * (32768 - wa)) >> 15, k);
        b[w & MASK] = (int16_t)sat(softclip(x[i] + y * fb / 32768));
        x[i] = y;
    }
    s->ph[0] = w;
    s->ph[1] = ph;
    s->z[0] = tone;
}

/* ---------------------------------------------------------------- the slot */

static void reset(vb_fx_state_t *s, uint8_t type)
{
    s->type = type;
    s->pos = 0;
    s->rng = s->seed;
    s->quiet = 0;
    for (int i = 0; i < 12; i++)
        s->z[i] = 0;
    for (int i = 0; i < 4; i++)
        s->ph[i] = 0;
    s->z[11] = MIX_UNSET;
    for (int i = 0; i < VB_FX_BUF; i++)
        s->buf[i] = 0;
}

void vb_fx_init(vb_fx_state_t *s, int16_t *buf, uint32_t seed)
{
    s->buf = buf;
    s->seed = seed ? seed : 0x9E3779B9u;
    reset(s, VB_FX_NONE);
}

/* quiet chunks before the slot sleeps: long enough for anything the type keeps to have come out */
static int32_t sleep_need(const vb_fx_state_t *s)
{
    switch (s->type) {
    case VB_FX_DRIVE: case VB_FX_FOLD: case VB_FX_CRUNCH: case VB_FX_BITS: case VB_FX_RING:
        return 16;
    case VB_FX_FILTER: case VB_FX_PHASER: case VB_FX_CHORUS:
        return 64;
    case VB_FX_TAPE:                                     /* the whole tape at the current rate */
        return s->z[8] > 0 ? (int32_t)((((uint32_t)VB_FX_BUF << 16) / (uint32_t)s->z[8]) / FX_CH) + 8 : SLEEP_BUF;
    default:
        return SLEEP_BUF;
    }
}

static void chunk(vb_fx_state_t *s, const vb_fx_slot_t *sl, int32_t *io, int n, int note, int bpm10)
{
    int32_t dry[FX_CH], need = sleep_need(s);
    uint32_t ipk = 0, wpk = 0;
    for (int i = 0; i < n; i++) {
        int32_t v = sat(io[i]);
        dry[i] = io[i] = v;
        ipk |= (uint32_t)iabs(v);
    }
    if (!ipk && s->quiet >= need)
        return;                                          /* asleep: the (silent) input passes */
    switch (s->type) {
    case VB_FX_DRIVE: fx_drive(s, sl->p, io, n); break;
    case VB_FX_FOLD: fx_fold(s, sl->p, io, n); break;
    case VB_FX_CRUNCH: fx_crunch(s, sl->p, io, n); break;
    case VB_FX_BITS: fx_bits(s, sl->p, io, n); break;
    case VB_FX_FILTER: fx_filter(s, sl->p, io, n); break;
    case VB_FX_TAPE: fx_tape(s, sl->p, io, n, bpm10); break;
    case VB_FX_REVERB: fx_reverb(s, sl->p, io, n); break;
    case VB_FX_CHORUS: fx_chorus(s, sl->p, io, n); break;
    case VB_FX_PHASER: fx_phaser(s, sl->p, io, n); break;
    case VB_FX_RING: fx_ring(s, sl->p, io, n, note); break;
    case VB_FX_RESON: fx_reson(s, sl->p, io, n, note); break;
    case VB_FX_FREEZE: fx_freeze(s, sl->p, io, n); break;
    case VB_FX_SHIFT: fx_shift(s, sl->p, io, n); break;
    default: break;
    }
    s->pos += (uint32_t)n;
    int32_t mix = sl->mix > 127 ? 127 : sl->mix, t1 = (mix >= 127 ? 256 : mix * 2) << 8;   /* Q16 */
    int32_t m0 = s->z[11] == MIX_UNSET ? t1 : s->z[11], d = t1 - m0;
    int32_t m1 = d > -1024 && d < 1024 ? t1 : m0 + d / 4;   /* MIX glides (~3 ms), ramped over the chunk */
    int32_t mq = m0, dm = (m1 - m0) / n;
    if (m1 != m0) {
        for (int i = 0; i < n; i++) {                   /* (|wet| < 2^17: the product fits) */
            int32_t w = io[i];
            wpk |= (uint32_t)iabs(w);
            mq += dm;
            io[i] = sat(dry[i] + (((w - dry[i]) * (mq >> 8)) >> 8));
        }
    } else {
        for (int i = 0; i < n; i++) {
            int32_t w = io[i];
            wpk |= (uint32_t)iabs(w);
            io[i] = sat(dry[i] + (((w - dry[i]) * (m1 >> 8)) >> 8));
        }
    }
    s->z[11] = m1;
    if (!ipk && wpk <= QUIET_LVL) {
        if (s->quiet < need && ++s->quiet >= need)
            for (int i = 0; i < NSTATE[s->type]; i++)    /* asleep from here: the states start clean */
                s->z[i] = 0;
    } else {
        s->quiet = 0;
    }
}

void vb_fx_process(vb_fx_state_t *s, const vb_fx_slot_t *slot, int32_t *io, int n, int note, int bpm10)
{
    uint8_t type = slot->type < VB_FX_TYPES ? slot->type : VB_FX_NONE;
    if (type != s->type)
        reset(s, type);
    if (!slot->on || s->type == VB_FX_NONE)
        return;
    for (int i = 0; i < n; i += FX_CH)
        chunk(s, slot, io + i, n - i < FX_CH ? n - i : FX_CH, note, bpm10);
}
