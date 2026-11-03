/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox core: DRUM, eight 808-style voices with shapeable pitch sweeps.
 * Every voice has its own state, so a whole kit rings at once (a hat over a kick tail). */
#ifndef VB_DRUM_H
#define VB_DRUM_H
#include <stdint.h>
#include "vb_param.h"

enum { VB_KICK, VB_SNARE, VB_CLAP, VB_HAT, VB_TOM, VB_RIM, VB_COWB, VB_CYMB, VB_DRUM_VOICES };
#define VB_DRUM_NP 8   /* shaping parameters per voice, plus LEVEL */

typedef struct {
    const char *name, *shortname;
    vb_param_t p[VB_DRUM_NP];
} vb_drum_voice_desc_t;
extern const vb_drum_voice_desc_t vb_drum_voices[VB_DRUM_VOICES];
extern const vb_engine_desc_t vb_drum_engine;

/* the patch: what a recipe stores for DRUM */
typedef struct {
    uint8_t p[VB_DRUM_VOICES][VB_DRUM_NP];
    uint8_t lvl[VB_DRUM_VOICES];
} vb_drum_patch_t;

typedef struct {
    uint32_t ph, ph2, ph_m[6];   /* oscillator phases (ph_m: the metal square bank) */
    int32_t amp, pitch, noise_env, click_env;   /* Q30 envelopes */
    int32_t lp, lp2, hp;         /* filter states */
    int32_t t;                   /* samples since the trigger */
    int32_t vel;                 /* Q15 */
    int32_t drive_norm;          /* Q15: 1 / tanh(drive gain), set at the trigger */
    uint8_t on;
} vb_drum_voice_t;

typedef struct {
    vb_drum_voice_t v[VB_DRUM_VOICES];
    uint32_t rng;
} vb_drum_t;

void vb_drum_patch_default(vb_drum_patch_t *patch);
void vb_drum_init(vb_drum_t *d, uint32_t seed);
void vb_drum_trigger(vb_drum_t *d, const vb_drum_patch_t *patch, int voice, int vel /* 1..127 */);
/* adds n mono samples (Q15) to out[] */
void vb_drum_render(vb_drum_t *d, const vb_drum_patch_t *patch, int32_t *out, int n);

#endif
