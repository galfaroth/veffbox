/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox core: CHOP, a transient slicer over the print (the buffer of vb_samp_buffer(): 22.05 kHz, 16-bit mono).
 *
 * Slices: energy-flux onset detection over blocks of 256 samples (mean |x| per block, positive block-to-block rise,
 * peak-picked, at least 3 blocks apart, each onset then refined to the first sample over a quarter of the hit's peak,
 * minus a 16-sample pre-roll). Slice 0 always starts at 0; at most 64 slices. Run lazily on note_on (or by
 * vb_chop_slices) and cached, keyed on the buffer pointer, length, a 64-sample fingerprint of its contents, and SENS.
 *
 * Patch: 9 bytes, in this order:
 *   SENS   0..127  detection sensitivity: an onset is kept when its flux is at least ((127 - SENS) / 127)^2 of the
 *                  strongest one (and above 1/32 of it, and a 25% level rise); 0 = only the strongest, 127 = all
 *   SLICE  0..63   the slice the UI edits / auditions (UI state: the sound doesn't depend on it; under ORDER SEQ and
 *                  MAP KEYS, slice s is note 60 + s)
 *   PITCH  0..48   24 = 0 semitones (+-24); keys pick slices, they don't transpose
 *   REV    OFF ON  play the slice backwards (from the end of the LEN portion back to the slice start)
 *   ORDER  SEQ SHUF STUT REV  how keys map to slices: k = note - 60 (+ the STEPS counter), wrapped into the slices;
 *                  SEQ slice k; SHUF a fixed seeded permutation of the slices; STUT slice k as a stutter: the first
 *                  quarter of the LEN portion (at least 20 ms) repeats while the key is held, the repeat in progress
 *                  finishes after note_off; REV slice n-1-k
 *   LEN    1..127  the portion of the slice played, LEN/127 of it (at least 64 samples)
 *   ATK    0..127  attack, ~0.4 ms .. ~0.7 s (linear)
 *   DEC    0..127  linear decay from full level to silence over 10 ms .. 2.2 s, starting after the attack;
 *                  127 = no decay
 *   MAP    KEYS STEPS  KEYS: k = note - 60. STEPS: k = note - 60 + a shared step counter that advances on every
 *                  note_on (so repeating one key walks through the slices: sequenced chopping); voice_init resets the
 *                  counter, so a render that starts by initialising its voices is deterministic
 * Except under STUT the slices are one-shots: note_off doesn't cut them, they end with the LEN portion (a 64-sample
 * fade) or the decay. A retrigger of a sounding voice fades the old slice out over 64 samples before starting. */
#ifndef VB_CHOP_H
#define VB_CHOP_H
#include <stdint.h>
#include "vb_engine.h"

enum { VB_CH_SENS, VB_CH_SLICE, VB_CH_PITCH, VB_CH_REV, VB_CH_ORDER, VB_CH_LEN, VB_CH_ATK, VB_CH_DEC, VB_CH_MAP,
       VB_CH_NPARAMS };
enum { VB_CH_SEQ, VB_CH_SHUF, VB_CH_STUT, VB_CH_RORD };
enum { VB_CH_KEYS, VB_CH_STEPS };
#define VB_CH_MAXSLICES 64

extern const vb_engine_t vb_chop;
/* the current print's slice starts (in samples, ascending, starts[0] = 0) at the SENS last seen by a CHOP voice
 * (64 before any); returns the number of slices (0 without a print). May run the detection (a few ms): call it from
 * the audio context or while it is paused, like note_on. */
int vb_chop_slices(uint16_t *starts, int max);

#endif
