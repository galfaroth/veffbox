/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox core: what goes into flash. A sound is one 4 KiB sector: its current recipe and its generations, each
 * generation stored as the bytes that differ from the current recipe. A print's audio is stored as 4-bit IMA ADPCM.
 * Pure functions on RAM buffers: the firmware erases and writes the sectors (fw/vb_app.c). */
#ifndef VB_STORE_H
#define VB_STORE_H
#include <stdint.h>
#include "vb_sound.h"

#define VB_SECTOR 4096u
#define VB_MAXGEN 12

typedef struct {
    vb_recipe_t rc;
    int8_t parent;                       /* index of the generation it was printed from, -1 = none */
    uint8_t audio;                       /* its audio is the one stored */
} vb_gen_t;

/* packs the sound into sector[VB_SECTOR]; drops the oldest generations while they don't fit. Returns the number of
 * generations kept (they are the last ones of gens[]). */
int vb_sound_pack(const vb_recipe_t *cur, const vb_gen_t *gens, int ngen, int curgen, uint8_t *sector);
/* unpacks; returns the number of generations, or -1 when the sector holds no valid sound of this format */
int vb_sound_unpack(const uint8_t *sector, vb_recipe_t *cur, vb_gen_t *gens, int maxgen, int *curgen);

uint16_t vb_crc16(const uint8_t *p, uint32_t n);

/* IMA ADPCM, 4 bits per sample, two per byte (low nibble first) */
typedef struct { int32_t pred, idx; } vb_adpcm_t;
void vb_adpcm_encode(vb_adpcm_t *st, const int16_t *in, uint32_t n, uint8_t *out);
void vb_adpcm_decode(vb_adpcm_t *st, const uint8_t *in, uint32_t n, int16_t *out);

#endif
