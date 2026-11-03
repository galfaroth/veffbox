/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox core: SAMP, the sampler that plays a print. Prints are stored at 22.05 kHz, 16-bit mono.
 * Patch: START LEN LOOP(OFF FWD P-P) REV PITCH(24 = 0 semitones) FINE(50 = 0) FADE GAIN. */
#ifndef VB_SAMP_H
#define VB_SAMP_H
#include <stdint.h>
#include "vb_engine.h"

extern const vb_engine_t vb_samp;
/* the print the sampler plays (all SAMP voices share it); len in samples, at most 65535 */
void vb_samp_set_buffer(const int16_t *buf, uint32_t len);

#endif
