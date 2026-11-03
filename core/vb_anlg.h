/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: ANLG, a two-oscillator analog-style voice (vb_engine_t).
 *
 * Patch: 16 bytes: OSC1 OSC2 DET MIX / SUB NOIS SYNC PW / CUT RES ENV DRV / ATK DEC SUS REL.
 *   OSC1, OSC2  SAW SQR TRI PWM SIN NOIS (SAW and the pulses are polyBLEP band-limited; PWM = PW + a slow internal sweep)
 *   DET   osc2 detune up, 0..100 cents; with SYNC on it spans 0..+24 semitones (a sync sweep needs osc2 above osc1)
 *   MIX   osc1 (0) .. osc2 (127); SUB a square one octave below osc1; NOIS white noise
 *   CUT RES  24 dB/oct ladder low-pass (4 one-poles, tanh-limited feedback: stable up to self-oscillation)
 *   ENV   64 = none, bipolar amount of the envelope on the cutoff (+-10 octaves); DRV tanh drive before the filter
 *   ATK DEC SUS REL  the envelope (amp and filter), 1 ms .. 10 s */
#ifndef VB_ANLG_H
#define VB_ANLG_H
#include "vb_engine.h"

enum { VB_AN_OSC1, VB_AN_OSC2, VB_AN_DET, VB_AN_MIX, VB_AN_SUB, VB_AN_NOIS, VB_AN_SYNC, VB_AN_PW,
       VB_AN_CUT, VB_AN_RES, VB_AN_ENV, VB_AN_DRV, VB_AN_ATK, VB_AN_DEC, VB_AN_SUS, VB_AN_REL, VB_AN_NPARAMS };
enum { VB_AN_SAW, VB_AN_SQR, VB_AN_TRI, VB_AN_PWM, VB_AN_SIN, VB_AN_NOISE, VB_AN_WAVES };

extern const vb_engine_t vb_anlg;

#endif
