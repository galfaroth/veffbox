/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: OP4, a 4-operator phase-modulation FM voice (vb_engine_t).
 *
 * Patch: 38 bytes. For each operator A, B, C, D (9 each): WAVE CRS FINE LVL ATK DEC SUS REL FIX; then ALG, FB.
 *   WAVE  SIN SIN3 SIN5 SAW SQR TRI NOIS
 *   CRS   0..32 (0 = x0.5), FINE 0..99: ratio = crs + fine/100; with FIX on, Hz = crs*100 + fine*10
 *   LVL   0..127: output level on a carrier, modulation index on a modulator (index ~ (lvl/127)^2 * 4 pi rad)
 *   ATK DEC SUS REL: 1 ms .. 10 s (exponential decay and release, linear attack)
 *   ALG   0..10: the routings of the mockup's ALG table; FB: self-feedback on D */
#ifndef VB_OP4_H
#define VB_OP4_H
#include "vb_engine.h"

enum { VB_OP4_WAVE, VB_OP4_CRS, VB_OP4_FINE, VB_OP4_LVL, VB_OP4_ATK, VB_OP4_DEC, VB_OP4_SUS, VB_OP4_REL, VB_OP4_FIX,
       VB_OP4_PER_OP };
#define VB_OP4_ALG (4 * VB_OP4_PER_OP)   /* 36 */
#define VB_OP4_FB (VB_OP4_ALG + 1)        /* 37 */
#define VB_OP4_ALGS 11

extern const vb_engine_t vb_op4;
/* the routings: modulation edges "from to" separated by spaces, and the carriers (the screen draws them too) */
extern const char *const vb_op4_alg_edges[VB_OP4_ALGS], *const vb_op4_alg_carriers[VB_OP4_ALGS];

#endif
