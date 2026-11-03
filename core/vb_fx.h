/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox core: the FX chain. 8 slots in any order, types may repeat; each slot has 4 parameters, MIX and ON.
 * Mono, Q15 in int32, processed in place in 32-sample blocks. Memory: each slot owns one buffer handed in by the app
 * (VB_FX_BUF int16 samples); a type uses as much of it as it needs. */
#ifndef VB_FX_H
#define VB_FX_H
#include <stdint.h>

enum {
    VB_FX_NONE, VB_FX_DRIVE, VB_FX_FOLD, VB_FX_CRUNCH, VB_FX_BITS, VB_FX_FILTER, VB_FX_TAPE, VB_FX_REVERB,
    VB_FX_CHORUS, VB_FX_PHASER, VB_FX_RING, VB_FX_RESON, VB_FX_FREEZE, VB_FX_SHIFT, VB_FX_TYPES
};
#define VB_FX_SLOTS 8
#define VB_FX_BUF 8192                    /* int16 per slot: 186 ms at 44.1 kHz */

typedef struct {
    const char *name;                     /* "TAPE DLY" (up to 8 chars) */
    const char *shortname;                /* "DLY" (3 chars) */
    const char *label[4];                 /* up to 5 chars each */
    uint8_t def[4];
} vb_fx_desc_t;
extern const vb_fx_desc_t vb_fx_types[VB_FX_TYPES];

/* what a recipe stores per slot */
typedef struct { uint8_t type, p[4], mix, on; } vb_fx_slot_t;

/* runtime state of one slot */
typedef struct {
    uint8_t type;                         /* the type the state belongs to (a change resets it) */
    int16_t *buf;
    uint32_t pos, rng;
    uint32_t seed;                        /* rng restarts from it when the type changes (a fresh slot sounds the same) */
    int32_t z[12];                        /* filter / phase / envelope states, meaning per type */
    uint32_t ph[4];
    int32_t quiet;                        /* blocks of silence in and out: the slot sleeps */
} vb_fx_state_t;

void vb_fx_init(vb_fx_state_t *s, int16_t *buf, uint32_t seed);
/* processes n samples in place. note: the last played MIDI note (RING / RESON follow it), bpm10: tempo x 10 */
void vb_fx_process(vb_fx_state_t *s, const vb_fx_slot_t *slot, int32_t *io, int n, int note, int bpm10);

#endif
