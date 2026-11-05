/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: the built-in library: sources (a source with its patch) and FX chains (the 8 slots). */
#ifndef VB_LIB_H
#define VB_LIB_H
#include "vb_sound.h"

extern const char *const vb_lib_src_names[];   /* "-", then the sources (for a list: index i + 1) */
extern const char *const vb_lib_fx_names[];
int vb_lib_nsrc(void);
int vb_lib_nfx(void);
const char *vb_lib_src_name(int i);
const char *vb_lib_fx_name(int i);
void vb_lib_src(vb_recipe_t *r, int i);   /* the source and its patch; play, modulation and FX stay */
void vb_lib_fx(vb_recipe_t *r, int i);    /* the whole chain */

#endif
