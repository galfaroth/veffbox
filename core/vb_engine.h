/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: the generic voice-engine interface (OP4, ANLG; DRUM keeps its own kit interface).
 *
 * A patch is a flat array of 0..127 bytes, one per descriptor, in descriptor order. The app applies modulation by
 * passing an effective copy of the patch (knob + modulation delta, clamped) to render(), once per 32-sample block,
 * so engines never see modulation directly. Rows of 4 on the screen follow descriptor order. */
#ifndef VB_ENGINE_H
#define VB_ENGINE_H
#include <stdint.h>
#include "vb_param.h"

typedef struct vb_engine {
    vb_engine_desc_t desc;                 /* name, version, nparams, params */
    uint16_t voice_size;                   /* bytes of one voice's state */
    void (*voice_init)(void *voice, uint32_t seed);
    /* note 0..127 (MIDI), vel 1..127. A retrigger of a sounding voice must not click. */
    void (*note_on)(void *voice, const uint8_t *patch, int note, int vel);
    void (*note_off)(void *voice, const uint8_t *patch);
    /* ADDS n mono Q15 samples to out[]; returns 1 while the voice sounds, 0 once it is silent and free */
    int (*render)(void *voice, const uint8_t *patch, int32_t *out, int n);
} vb_engine_t;

/* the most voice state any engine may use (the app allocates voices statically) */
#define VB_VOICE_BYTES 256
/* polyphony: one chord of up to 6 notes, plus release tails */
#define VB_POLY 10                       /* 8 chord notes and a BASS, with one to spare for a release */

#endif
