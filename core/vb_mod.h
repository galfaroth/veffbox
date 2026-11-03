/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: modulation. 6 LFOs and 3 envelopes per sound; each source has up to 4
 * destinations with its own bipolar amount. Destinations are flat parameter indices that the
 * sound's descriptor tables define (engine params, FX slot params, other LFOs' params).
 *
 * Modulation never writes the stored knob value: it adds a Q8 (1/256 knob step) delta on top,
 * like MNM-1's calibrated machines, so the recipe always holds the knob as you set it. */
#ifndef VB_MOD_H
#define VB_MOD_H
#include <stdint.h>

#define VB_ROUTES 4
#define VB_NO_DEST 0xFFFF

enum { VB_LFO_SIN, VB_LFO_TRI, VB_LFO_SQR, VB_LFO_SAW, VB_LFO_RAMP, VB_LFO_SH, VB_LFO_EXP, VB_LFO_WAVES };
enum { VB_TRIG_FREE, VB_TRIG_TRIG, VB_TRIG_HOLD, VB_TRIG_ONE, VB_TRIG_HALF };

typedef struct { uint16_t dest; int8_t amt; } vb_route_t;   /* amt -64..63: +-64 = +-128 knob steps */

typedef struct {
    uint8_t wave, rate, sync, trig, phase;
    vb_route_t r[VB_ROUTES];
    /* runtime */
    uint32_t ph, rng;
    int32_t sh, held;
    uint8_t stopped;
} vb_lfo_t;

void vb_lfo_init(vb_lfo_t *l, uint32_t seed);
void vb_lfo_note(vb_lfo_t *l);                       /* a note arrived: TRIG/ONE/HALF restart, HOLD samples */
/* advances the LFO by n samples at the tempo bpm10 (tenths of BPM) and returns its value, Q15 */
int32_t vb_lfo_step(vb_lfo_t *l, int bpm10, int n);
/* adds this source's routes to delta_q8[dest] for a source value in Q15 */
void vb_mod_add(const vb_route_t *r, int32_t val_q15, int32_t *delta_q8, int ndest);
/* the effective value of a 0..127 knob with its modulation delta, Q8, clamped to the range */
static inline int32_t vb_mod_eff_q8(uint8_t knob, int32_t delta_q8, uint8_t lo, uint8_t hi)
{
    int32_t v = (int32_t)knob * 256 + delta_q8;
    return v < lo * 256 ? lo * 256 : v > hi * 256 ? hi * 256 : v;
}

#endif
