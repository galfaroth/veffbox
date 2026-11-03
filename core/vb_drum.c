/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox core: DRUM engine, eight 808-style voices. Fixed point, deterministic. */
#include "vb_drum.h"
#include "vb_dsp.h"

#define P(l, d) { l, 0, 127, d, VB_P_MOD, 0 }
const vb_drum_voice_desc_t vb_drum_voices[VB_DRUM_VOICES] = {
    { "KICK",  "KCK", { P("TUNE", 30), P("DEC", 70), P("SWEEP", 80), P("SWP T", 30), P("CLICK", 40), P("DRIVE", 20), P("SHAPE", 64), P("TONE", 60) } },
    { "SNARE", "SNR", { P("TUNE", 60), P("DEC", 40), P("SNAPY", 70), P("N DEC", 50), P("N TON", 70), P("BODY2", 64), P("DRIVE", 10), P("TONE", 70) } },
    { "CLAP",  "CLP", { P("TONE", 70), P("DEC", 40), P("SPRD", 50), P("BURST", 64), P("ROOM", 50), P("DRIVE", 10), P("Q", 40), P("PAN", 64) } },
    { "HAT",   "HAT", { P("TUNE", 64), P("DEC", 20), P("METAL", 80), P("TONE", 90), P("OPEN", 0), P("CHOKE", 127), P("DRIVE", 0), P("PAN", 64) } },
    { "TOM",   "TOM", { P("TUNE", 60), P("DEC", 60), P("SWEEP", 50), P("SWP T", 40), P("NOISE", 20), P("DRIVE", 10), P("TONE", 60), P("PAN", 64) } },
    { "RIM",   "RIM", { P("TUNE", 80), P("DEC", 20), P("CLICK", 90), P("TONE", 80), P("DRIVE", 10), P("RING", 40), P("Q", 60), P("PAN", 64) } },
    { "COWB",  "COW", { P("TUNE", 64), P("DEC", 40), P("DETUN", 60), P("TONE", 70), P("DRIVE", 10), P("RING", 50), P("Q", 60), P("PAN", 64) } },
    { "CYMB",  "CYM", { P("TUNE", 64), P("DEC", 90), P("METAL", 90), P("TONE", 100), P("SPRD", 60), P("DRIVE", 0), P("Q", 40), P("PAN", 64) } },
};
#undef P

static const vb_param_t engine_params[1] = { { "VOICE", 0, VB_DRUM_VOICES - 1, 0, VB_P_LIST, 0 } };
const vb_engine_desc_t vb_drum_engine = { "DRUM", 1, 1, engine_params };

/* where DRIVE sits in each voice's parameter list */
static const uint8_t DRIVE_AT[VB_DRUM_VOICES] = { 5, 6, 5, 6, 5, 4, 4, 5 };
/* the 808 metal bank: six square oscillators, Hz in Q8 */
static const int32_t METAL_Q8[6] = { 52557, 77926, 94618, 133811, 138240, 204800 };

#define ONE_Q30 (1 << 30)
#define SILENT_Q30 (1 << 15)          /* ~ -90 dB: the voice stops */

static inline int dec_idx(int base, int p, int span) { return vb_clamp(base + p * span / 127, 0, 127); }

void vb_drum_patch_default(vb_drum_patch_t *patch)
{
    for (int v = 0; v < VB_DRUM_VOICES; v++) {
        for (int i = 0; i < VB_DRUM_NP; i++) patch->p[v][i] = vb_drum_voices[v].p[i].def;
        patch->lvl[v] = 100;
    }
}

void vb_drum_init(vb_drum_t *d, uint32_t seed)
{
    for (int v = 0; v < VB_DRUM_VOICES; v++) d->v[v] = (vb_drum_voice_t){ 0 };
    d->rng = seed ? seed : 0x9E3779B9u;
}

void vb_drum_trigger(vb_drum_t *d, const vb_drum_patch_t *patch, int voice, int vel)
{
    if (voice < 0 || voice >= VB_DRUM_VOICES) return;
    vb_drum_voice_t *s = &d->v[voice];
    const uint8_t *p = patch->p[voice];
    *s = (vb_drum_voice_t){ 0 };
    s->on = 1;
    s->amp = ONE_Q30;
    s->pitch = ONE_Q30;
    s->noise_env = ONE_Q30;
    s->click_env = ONE_Q30;
    s->vel = vb_clamp(vel, 1, 127) * 258;
    if (voice == VB_CLAP) s->amp = 0;              /* the clap's tail starts after its bursts */
    /* drive: gain 1..7, normalized so a full-scale hit stays full scale */
    int32_t g_q8 = 256 + p[DRIVE_AT[voice]] * 6 * 256 / 127;
    int32_t tmax = vb_tanh((32767 * g_q8) >> 10);
    s->drive_norm = tmax > 0 ? (32767 << 15) / tmax : 32767;
}

static int32_t drive(const vb_drum_voice_t *s, const uint8_t *p, int voice, int32_t y)
{
    int32_t g_q8 = 256 + p[DRIVE_AT[voice]] * 6 * 256 / 127;
    return vb_mulq15(vb_tanh((y * g_q8) >> 10), s->drive_norm);   /* |y| < 2^15, g < 2^11: fits */
}

/* KICK and TOM: a sine with an exponential pitch sweep */
static int32_t swept(vb_drum_voice_t *s, const uint8_t *p, int voice, uint32_t *rng)
{
    const int kick = voice == VB_KICK;
    int32_t f0 = kick ? (35 << 8) + p[0] * (70 << 8) / 127 : (90 << 8) + p[0] * (220 << 8) / 127;
    int32_t sw_q8 = p[2] * (kick ? 9 : 4) * 256 / 127;
    int32_t shape = kick ? p[6] : 64;
    s->amp = vb_mulq30(s->amp, VB_DECAY[dec_idx(40, p[1], 80)]);
    s->pitch = vb_mulq30(s->pitch, VB_DECAY[dec_idx(0, p[3], 64)]);
    int32_t e2 = vb_mulq30(s->pitch, s->pitch);
    int32_t es = s->pitch + vb_mul_sh(e2 - s->pitch, shape, 7);
    int32_t f = f0 + vb_mul_sh((f0 * sw_q8) >> 8, es, 30);   /* f0 * sw_q8 < 2^26 */
    s->ph += vb_inc_hz_q8(f);
    int32_t y = vb_mulq30(vb_sin(s->ph), s->amp);
    if (kick) {
        s->click_env = vb_mulq30(s->click_env, VB_DECAY[8]);
        y += vb_mulq15(vb_mulq30(vb_noise(rng), s->click_env), p[4] * 258);
        y = vb_lp1(&s->lp, y, VB_ONEPOLE[dec_idx(60, p[7], 67)]);
    } else {
        s->noise_env = vb_mulq30(s->noise_env, VB_DECAY[20]);
        y += vb_mulq15(vb_mulq30(vb_noise(rng), s->noise_env), p[4] * 129);
        y = vb_lp1(&s->lp, y, VB_ONEPOLE[dec_idx(60, p[6], 67)]);
    }
    return y;
}

static int32_t snare(vb_drum_voice_t *s, const uint8_t *p, uint32_t *rng)
{
    int32_t f1 = (140 << 8) + p[0] * (160 << 8) / 127;
    s->pitch = vb_mulq30(s->pitch, VB_DECAY[15]);
    int32_t f = f1 + vb_mul_sh(f1 * 154 >> 8, s->pitch, 30);   /* +60 % at the hit */
    int32_t f2 = (f * (256 + p[5] * 256 / 127)) >> 8;          /* < 2^26 */
    s->ph += vb_inc_hz_q8(f);
    s->ph2 += vb_inc_hz_q8(f2);
    s->amp = vb_mulq30(s->amp, VB_DECAY[dec_idx(25, p[1], 55)]);
    s->noise_env = vb_mulq30(s->noise_env, VB_DECAY[dec_idx(25, p[3], 60)]);
    int32_t body = vb_mulq30((vb_sin(s->ph) * 2 + vb_sin(s->ph2)) / 3, s->amp);
    int32_t n = vb_lp1(&s->lp, vb_noise(rng), VB_ONEPOLE[dec_idx(70, p[4], 57)]);
    int32_t nhp = n - vb_lp1(&s->lp2, n, VB_ONEPOLE[52]);
    int32_t y = body + vb_mulq15(vb_mulq30(nhp, s->noise_env), p[2] * 258);
    return vb_lp1(&s->hp, y, VB_ONEPOLE[dec_idx(70, p[7], 57)]);
}

static int32_t clap(vb_drum_voice_t *s, const uint8_t *p, uint32_t *rng)
{
    int32_t bursts = 1 + p[3] * 5 / 127;
    int32_t sp = 176 + p[2] * 882 / 127;                 /* 4 .. 24 ms between bursts */
    if (s->t < bursts * sp) {
        if (s->t % sp == 0) s->noise_env = ONE_Q30;
    } else if (s->t == bursts * sp) {
        s->amp = (ONE_Q30 / 10) * 4 + (ONE_Q30 / 1270) * 6 * p[4];
    }
    s->noise_env = vb_mulq30(s->noise_env, VB_DECAY[10]);
    s->amp = vb_mulq30(s->amp, VB_DECAY[dec_idx(25, p[1], 60)]);
    int32_t env = s->noise_env > s->amp ? s->noise_env : s->amp;
    int32_t n = vb_lp1(&s->lp, vb_noise(rng), VB_ONEPOLE[dec_idx(75, p[0], 45)]);
    n -= vb_lp1(&s->lp2, n, VB_ONEPOLE[55]);
    if (s->t > bursts * sp && s->amp < SILENT_Q30) s->on = 0;
    return vb_mulq30(n, env);
}

/* HAT and CYMB: the six-square metal bank mixed with noise, high-passed */
static int32_t metal(vb_drum_voice_t *s, const uint8_t *p, int voice, uint32_t *rng)
{
    const int cym = voice == VB_CYMB;
    int32_t tune = 128 + p[0];                           /* x 0.67 .. 1.33 (of 192) */
    int32_t spread = cym ? p[4] : 0;
    int32_t m = 0;
    for (int i = 0; i < 6; i++) {
        int32_t f = METAL_Q8[i] * tune / 192;                  /* < 2^26 */
        f += f * spread * (i - 3) / (127 * 40);               /* < 2^27 */
        s->ph_m[i] += vb_inc_hz_q8(f);
        m += vb_square(s->ph_m[i]);
    }
    m /= 3;
    int32_t mamt = p[2] * 258;
    int32_t x = vb_mulq15(m, mamt) + vb_mulq15(vb_noise(rng), 32767 - mamt);
    int32_t hp = x - vb_lp1(&s->lp, x, VB_ONEPOLE[dec_idx(80, p[3], 30)]);
    int idx = cym ? dec_idx(60, p[1], 60) : vb_clamp(10 + p[1] * 30 / 127 + p[4] * 50 / 127, 0, 127);
    s->amp = vb_mulq30(s->amp, VB_DECAY[idx]);
    return vb_mulq30(hp, s->amp) * 4;   /* the high-passed bank is quiet: bring it up to the kit level */
}

static int32_t rim_cow(vb_drum_voice_t *s, const uint8_t *p, int voice)
{
    int32_t y;
    if (voice == VB_RIM) {
        int32_t f = (400 << 8) + p[0] * (900 << 8) / 127;
        s->ph += vb_inc_hz_q8(f);
        s->ph2 += vb_inc_hz_q8(f * 21 / 10);
        s->amp = vb_mulq30(s->amp, VB_DECAY[dec_idx(5, p[1], 25) + p[5] * 15 / 127]);
        s->click_env = vb_mulq30(s->click_env, VB_DECAY[2]);
        y = vb_mulq30(vb_square(s->ph) + vb_sin(s->ph2) / 2, s->amp) + vb_mulq15(s->click_env >> 15, p[2] * 258);
        y = vb_lp1(&s->lp, y, VB_ONEPOLE[dec_idx(70, p[3], 57)]);
    } else {
        int32_t f1 = (540 << 8) * (179 + p[0] * 154 / 127) >> 8;   /* x 0.7 .. 1.3 */
        int32_t f2 = f1 * (379 + p[2] * 77 / 127) >> 8;             /* x 1.48 .. 1.78 */
        s->ph += vb_inc_hz_q8(f1);
        s->ph2 += vb_inc_hz_q8(f2);
        s->amp = vb_mulq30(s->amp, VB_DECAY[vb_clamp(dec_idx(20, p[1], 40) + p[5] * 20 / 127, 0, 127)]);
        y = vb_mulq30(vb_square(s->ph) + vb_square(s->ph2), s->amp);
        y = vb_lp1(&s->lp, y, VB_ONEPOLE[dec_idx(80, p[3], 40)]);
    }
    return y;
}

void vb_drum_render(vb_drum_t *d, const vb_drum_patch_t *patch, int32_t *out, int n)
{
    for (int voice = 0; voice < VB_DRUM_VOICES; voice++) {
        vb_drum_voice_t *s = &d->v[voice];
        if (!s->on) continue;
        const uint8_t *p = patch->p[voice];
        int32_t gain = vb_mulq15(s->vel, patch->lvl[voice] * 258);
        for (int i = 0; i < n && s->on; i++, s->t++) {
            int32_t y;
            switch (voice) {
            case VB_KICK: case VB_TOM: y = swept(s, p, voice, &d->rng); break;
            case VB_SNARE: y = snare(s, p, &d->rng); break;
            case VB_CLAP: y = clap(s, p, &d->rng); break;
            case VB_HAT: case VB_CYMB: y = metal(s, p, voice, &d->rng); break;
            default: y = rim_cow(s, p, voice); break;
            }
            out[i] += vb_mulq15(drive(s, p, voice, vb_sat16(y)), gain);
            int32_t live = voice == VB_SNARE && s->noise_env > s->amp ? s->noise_env : s->amp;
            if (voice != VB_CLAP && live < SILENT_Q30 && s->t > 64) s->on = 0;
        }
    }
}
