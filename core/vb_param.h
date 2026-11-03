/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: parameter descriptors and the engine / effect registry.
 *
 * An engine or an effect is a table entry, not a hand-written page. Its descriptors drive:
 *   - the 4-knob rows on the screen,
 *   - the list of modulation destinations (every VB_P_MOD parameter can be modulated),
 *   - the recipe format (parameters are stored by index, in descriptor order).
 * Recipes must re-render bit-exactly, so an engine that has shipped never changes its sound:
 * a fix is a new `version`, and old recipes keep rendering with the old one. */
#ifndef VB_PARAM_H
#define VB_PARAM_H
#include <stdint.h>

enum { VB_P_MOD = 1, VB_P_LIST = 2, VB_P_BIPOLAR = 4 };

typedef struct {
    const char *label;          /* up to 5 characters, shown above the value */
    uint8_t min, max, def;
    uint8_t flags;              /* VB_P_* */
    const char *const *names;   /* list values for VB_P_LIST, else NULL */
} vb_param_t;

typedef struct {
    const char *name;           /* "DRUM", "OP4", "ANLG" ... */
    uint8_t version;            /* bumped whenever the sound would change */
    uint8_t nparams;
    const vb_param_t *params;
} vb_engine_desc_t;

#endif
