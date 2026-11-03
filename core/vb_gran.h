/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox core: GRAN, a granular player over the print (the buffer of vb_samp_buffer(): 22.05 kHz, 16-bit mono).
 *
 * Patch: 11 bytes, in this order:
 *   POS    0..127  where in the print grains are taken (0 = start, 127 = end); grains are centred on it
 *   SIZE   0..127  grain length, 5 .. 250 ms (quadratic: fine control of short grains)
 *   DENS   0..127  grains per second, 2 .. ~265 Hz (exponential, an octave every 18 steps); at most 8 grains sound
 *                  at once per voice, a grain due while all 8 sound is skipped. The grain level is scaled down with
 *                  the overlap (SIZE x rate, ~1/sqrt), so dense clouds stay level; the voice saturates at 16 bits.
 *   SPRAY  0..127  random scatter of each grain's position around POS, up to +-half the print
 *   PITCH  0..48   24 = 0 semitones (+-24); the played note adds note - 60
 *   PRND   0..127  random pitch per grain, up to +-12 semitones (1/16 semitone steps)
 *   REV    0..127  chance in 127 that a grain plays backwards (0 never, 127 always)
 *   WIN    HANN TRI RECT EXP  the grain window (256-entry tables; RECT and EXP have short anti-click edges)
 *   FRZ    OFF ON  ON: grains repeat a fixed cycle of 8 seeded random draws (positions, pitch, direction, level)
 *   WIDTH  0..127  grain-to-grain level variation (the engine is mono): each grain loses a random 0 .. 75% * WIDTH/127
 *   SEED   0..127  the grain randomness; a voice's generator is SEED mixed with the voice seed, reset on every note_on,
 *                  so a recipe re-renders bit-exactly
 * Amplitude: 3 ms attack on note_on, 60 ms release after note_off (grains keep spawning during the release).
 * A buffer shorter than 64 samples, or none, is silence. */
#ifndef VB_GRAN_H
#define VB_GRAN_H
#include "vb_engine.h"

enum { VB_GR_POS, VB_GR_SIZE, VB_GR_DENS, VB_GR_SPRAY, VB_GR_PITCH, VB_GR_PRND, VB_GR_REV, VB_GR_WIN, VB_GR_FRZ, VB_GR_WIDTH,
       VB_GR_SEED, VB_GR_NPARAMS };
enum { VB_GR_HANN, VB_GR_TRI, VB_GR_RECT, VB_GR_EXP };
#define VB_GR_GRAINS 8

extern const vb_engine_t vb_gran;

#endif
