/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: fixed-point helpers. Integer only (the FM-1 has no FPU and no 64-bit divide;
 * wide multiplies only through vb_mul_sh). Audio is Q15 in int32 between stages. */
#ifndef VB_DSP_H
#define VB_DSP_H
#include <stdint.h>
#include "vb_tables.h"

#define VB_SR 44100
/* 2^32 / 44100 in Q8: phase increment per Hz (Q8 Hz in, u32 phase out) */
#define VB_INC_PER_HZ_Q8 24932236u   /* round(2^32 / 44100 * 256) */

static inline int32_t vb_clamp(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline int32_t vb_sat16(int32_t x) { return vb_clamp(x, -32768, 32767); }
/* Wide (32x32->64) products live out of line, one per call, consumed before returning: the FM-1's CPU can corrupt
 * a wide multiply issued next to a load (MNM-1's tools/check_mul64.py scans the linked image for it). Everything
 * else is arranged to fit in 32 bits. */
#define VB_MULFN static __attribute__((noinline))
VB_MULFN int32_t vb_mul_sh(int32_t a, int32_t b, int sh) { return (int32_t)(((int64_t)a * b) >> sh); }
static inline int32_t vb_mulq15(int32_t a, int32_t b) { return vb_mul_sh(a, b, 15); }
static inline int32_t vb_mulq30(int32_t a, int32_t b) { return vb_mul_sh(a, b, 30); }

/* phase increment for a frequency in Q8 Hz */
static inline uint32_t vb_inc_hz_q8(int32_t hz_q8) { return (uint32_t)vb_mul_sh(hz_q8, (int32_t)VB_INC_PER_HZ_Q8, 16); }

/* sine of a 32-bit phase, Q15, linearly interpolated */
static inline int32_t vb_sin(uint32_t ph)
{
    uint32_t i = ph >> 22, f = (ph >> 7) & 0x7FFF;
    int32_t a = VB_SIN[i], b = VB_SIN[i + 1];
    return a + (((b - a) * (int32_t)f) >> 15);
}
static inline int32_t vb_square(uint32_t ph) { return ph < 0x80000000u ? 16384 : -16384; }

/* tanh of a Q15 value interpreted as x/8192 (so +-32767 ~ +-4), Q15 out */
static inline int32_t vb_tanh(int32_t x)
{
    x = vb_clamp(x, -32767, 32767);
    uint32_t u = (uint32_t)(x + 32768);            /* 0 .. 65535 */
    uint32_t i = u >> 8, f = u & 0xFF;
    int32_t a = VB_TANH[i], b = VB_TANH[i + 1];
    return a + (((b - a) * (int32_t)f) >> 8);
}

/* xorshift32: the only randomness in a render, seeded from the recipe, so renders repeat bit-exactly */
static inline uint32_t vb_rng(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline int32_t vb_noise(uint32_t *s) { return (int32_t)(vb_rng(s) >> 16) - 32768; }

/* one-pole low-pass, coefficient Q15 */
static inline int32_t vb_lp1(int32_t *z, int32_t x, int32_t k) { *z += vb_mul_sh(x - *z, k, 15); return *z; }

#endif
