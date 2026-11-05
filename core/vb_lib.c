/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: the built-in library. Sources (a source and its patch) and FX chains (all 8 slots), to start from;
 * applying one changes only that part of the recipe. */
#include "vb_lib.h"

typedef struct { uint8_t w, crs, fine, lvl, a, d, s, r, fix; } op_t;
typedef struct {
    const char *name;
    uint8_t src;
    op_t op[4];
    uint8_t alg, fb;
    uint8_t an[VB_ANLG_NP];
    uint8_t kit[VB_DRUM_VOICES][VB_DRUM_NP];
} src_t;

#define OP4(n, alg, fb, ...) { n, VB_SRC_OP4, { __VA_ARGS__ }, alg, fb, { 0 }, { { 0 } } }
#define AN(n, ...) { n, VB_SRC_ANLG, { { 0 } }, 0, 0, { __VA_ARGS__ }, { { 0 } } }
#define KIT(n, ...) { n, VB_SRC_DRUM, { { 0 } }, 0, 0, { 0 }, { __VA_ARGS__ } }
/* operators: WAVE (0 SIN 1 SIN3 2 SIN5 3 SAW 4 SQR 5 TRI 6 NOIS) CRS FINE LVL ATK DEC SUS REL FIX.
 * ALG: 0 D>C>B>A, 4 D>C>B + A, 5 D>C + B>A, 6 D>A,B,C, 8 B,C,D>A. ANLG: OSC1 OSC2 (0 SAW 1 SQR 2 TRI 3 PWM 4 SIN)
 * DET MIX SUB NOIS SYNC PW CUT RES ENV (64 = 0) DRV ATK DEC SUS REL. Kits: the 8 voices' 8 shaping parameters. */
static const src_t SRC[] = {
    OP4("LATEX", 0, 50, { 0, 1, 0, 127, 0, 55, 70, 30, 0 }, { 0, 1, 0, 110, 0, 30, 30, 25, 0 },
        { 0, 2, 0, 80, 0, 20, 0, 20, 0 }, { 0, 1, 0, 60, 0, 15, 0, 10, 0 }),
    OP4("RUBBER BS", 0, 90, { 0, 0, 0, 127, 0, 60, 90, 20, 0 }, { 0, 1, 0, 100, 0, 35, 40, 20, 0 },
        { 0, 1, 0, 90, 0, 25, 20, 20, 0 }, { 0, 2, 0, 60, 0, 20, 0, 15, 0 }),
    OP4("GLASS", 5, 0, { 0, 1, 0, 127, 0, 90, 0, 70, 0 }, { 0, 3, 50, 80, 0, 80, 0, 60, 0 },
        { 0, 2, 0, 100, 0, 85, 0, 60, 0 }, { 0, 7, 0, 60, 0, 70, 0, 50, 0 }),
    OP4("METAL HIT", 6, 70, { 0, 1, 0, 127, 0, 45, 0, 40, 0 }, { 0, 3, 41, 110, 0, 40, 0, 40, 0 },
        { 0, 5, 13, 100, 0, 35, 0, 35, 0 }, { 0, 11, 37, 100, 0, 40, 0, 40, 0 }),
    OP4("FM BELL", 8, 0, { 0, 1, 0, 127, 0, 95, 0, 80, 0 }, { 0, 3, 50, 70, 0, 70, 0, 60, 0 },
        { 0, 5, 0, 50, 0, 60, 0, 50, 0 }, { 0, 9, 0, 40, 0, 50, 0, 40, 0 }),
    OP4("WOOD", 4, 0, { 5, 1, 0, 110, 0, 30, 0, 20, 0 }, { 0, 2, 0, 110, 0, 25, 0, 20, 0 },
        { 0, 5, 0, 90, 0, 15, 0, 10, 0 }, { 6, 1, 0, 40, 0, 8, 0, 5, 0 }),
    AN("SUPERSAW", 0, 0, 30, 64, 30, 0, 0, 64, 95, 20, 70, 20, 5, 60, 100, 50),
    AN("PLASTIC", 1, 0, 8, 50, 20, 0, 0, 64, 35, 95, 110, 60, 0, 28, 0, 20),
    AN("SUB BOOM", 4, 2, 0, 30, 127, 0, 0, 64, 50, 10, 70, 40, 0, 70, 80, 40),
    AN("SYNC LEAD", 0, 0, 60, 90, 0, 0, 1, 64, 100, 50, 80, 90, 2, 50, 90, 40),
    KIT("HARD KIT", { 24, 60, 100, 25, 70, 70, 64, 50 }, { 66, 35, 90, 40, 80, 64, 50, 80 },
        { 80, 30, 40, 80, 40, 40, 50, 64 }, { 70, 12, 90, 100, 0, 127, 30, 64 }, { 50, 50, 70, 30, 10, 30, 60, 64 },
        { 90, 15, 100, 90, 30, 50, 70, 64 }, { 64, 30, 60, 70, 40, 50, 60, 64 }, { 70, 80, 100, 110, 60, 30, 40, 64 }),
    KIT("TINY KIT", { 50, 30, 60, 20, 80, 10, 64, 90 }, { 90, 15, 60, 30, 100, 64, 0, 100 },
        { 100, 15, 20, 40, 10, 0, 60, 64 }, { 100, 6, 60, 127, 0, 127, 0, 64 }, { 90, 25, 30, 20, 10, 0, 90, 64 },
        { 110, 8, 100, 110, 0, 60, 90, 64 }, { 100, 15, 40, 100, 0, 60, 80, 64 }, { 100, 40, 70, 127, 40, 0, 60, 64 }),
    KIT("CLANG KIT", { 30, 80, 90, 40, 50, 90, 30, 40 }, { 40, 60, 40, 70, 40, 90, 80, 40 },
        { 50, 60, 80, 50, 90, 70, 20, 64 }, { 40, 40, 127, 60, 40, 127, 60, 64 }, { 35, 80, 90, 60, 60, 80, 40, 64 },
        { 60, 40, 60, 60, 80, 100, 100, 64 }, { 40, 70, 100, 50, 90, 110, 100, 64 }, { 40, 110, 127, 80, 90, 60, 80, 64 }),
    OP4("BUZZ BASS", 4, 110, { 3, 0, 0, 110, 0, 50, 80, 20, 0 }, { 0, 1, 0, 110, 0, 40, 60, 20, 0 },
        { 0, 1, 0, 80, 0, 30, 40, 20, 0 }, { 4, 2, 0, 50, 0, 20, 20, 10, 0 }),
    AN("BLIP", 1, 1, 0, 64, 0, 0, 0, 64, 60, 70, 120, 0, 0, 12, 0, 8),
};
#define NSRC (sizeof SRC / sizeof SRC[0])

typedef struct { const char *name; vb_fx_slot_t s[4]; } chain_t;
#define S(t, a, b, c, d, mix) { t, { a, b, c, d }, mix, 1 }
static const chain_t CHAIN[] = {
    { "HYPER", { S(VB_FX_DRIVE, 60, 100, 0, 64, 127), S(VB_FX_BITS, 70, 100, 0, 64, 50), S(VB_FX_SHIFT, 110, 30, 50, 100, 40),
                 S(VB_FX_REVERB, 50, 50, 60, 0, 30) } },
    { "SPACE", { S(VB_FX_CHORUS, 30, 70, 20, 110, 90), S(VB_FX_TAPE, 60, 60, 20, 80, 35), S(VB_FX_REVERB, 100, 100, 40, 0, 55) } },
    { "WARP", { S(VB_FX_TAPE, 80, 85, 10, 70, 55), S(VB_FX_CHORUS, 10, 110, 40, 100, 70), S(VB_FX_FILTER, 90, 40, 0, 10, 127) } },
    { "CRUSH", { S(VB_FX_CRUNCH, 90, 40, 40, 64, 127), S(VB_FX_BITS, 30, 40, 30, 64, 100), S(VB_FX_FILTER, 60, 70, 0, 30, 127) } },
    { "METAL", { S(VB_FX_RING, 72, 0, 0, 127, 60), S(VB_FX_RESON, 64, 100, 90, 0, 70), S(VB_FX_REVERB, 60, 70, 50, 0, 30) } },
    { "SQUEAK", { S(VB_FX_SHIFT, 127, 20, 30, 110, 70), S(VB_FX_PHASER, 50, 100, 70, 80, 100), S(VB_FX_FILTER, 100, 100, 64, 0, 80) } },
    { "FROZEN", { S(VB_FX_FREEZE, 90, 40, 64, 0, 70), S(VB_FX_REVERB, 120, 120, 30, 0, 70) } },
    { "DIRT BASS", { S(VB_FX_DRIVE, 90, 50, 20, 60, 127), S(VB_FX_FOLD, 50, 64, 90, 60, 60), S(VB_FX_FILTER, 55, 80, 0, 40, 127) } },
    { "WIDE ROOM", { S(VB_FX_CHORUS, 20, 40, 0, 127, 60), S(VB_FX_REVERB, 70, 60, 70, 0, 40) } },
    { "CLEAN", { { 0 } } },
};
#undef S
#define NCHAIN (sizeof CHAIN / sizeof CHAIN[0])

/* the names for the LIB lists ("-" first: nothing picked yet) */
const char *const vb_lib_src_names[] = {"-", "LATEX", "RUBBER BS", "GLASS", "METAL HIT", "FM BELL", "WOOD", "SUPERSAW", "PLASTIC", "SUB BOOM", "SYNC LEAD", "HARD KIT", "TINY KIT", "CLANG KIT", "BUZZ BASS", "BLIP"};
const char *const vb_lib_fx_names[] = {"-", "HYPER", "SPACE", "WARP", "CRUSH", "METAL", "SQUEAK", "FROZEN", "DIRT BASS", "WIDE ROOM", "CLEAN"};
_Static_assert(sizeof vb_lib_src_names / sizeof vb_lib_src_names[0] == NSRC + 1, "LIB source names");
_Static_assert(sizeof vb_lib_fx_names / sizeof vb_lib_fx_names[0] == NCHAIN + 1, "LIB FX names");

int vb_lib_nsrc(void) { return (int)NSRC; }
int vb_lib_nfx(void) { return (int)NCHAIN; }
const char *vb_lib_src_name(int i) { return i >= 0 && i < (int)NSRC ? SRC[i].name : "-"; }
const char *vb_lib_fx_name(int i) { return i >= 0 && i < (int)NCHAIN ? CHAIN[i].name : "-"; }

void vb_lib_src(vb_recipe_t *r, int i)
{
    const src_t *s;
    uint32_t k, v;
    if (i < 0 || i >= (int)NSRC)
        return;
    s = &SRC[i];
    r->src = s->src;
    if (s->src == VB_SRC_OP4) {
        for (k = 0; k < 4u; k++) {
            const op_t *o = &s->op[k];
            uint8_t *p = &r->op4[k * 9u];
            p[0] = o->w; p[1] = o->crs; p[2] = o->fine; p[3] = o->lvl; p[4] = o->a;
            p[5] = o->d; p[6] = o->s; p[7] = o->r; p[8] = o->fix;
        }
        r->op4[36] = s->alg;
        r->op4[37] = s->fb;
    } else if (s->src == VB_SRC_ANLG) {
        for (k = 0; k < VB_ANLG_NP; k++)
            r->anlg[k] = s->an[k];
    } else {
        for (v = 0; v < VB_DRUM_VOICES; v++) {
            for (k = 0; k < VB_DRUM_NP; k++)
                r->drum.p[v][k] = s->kit[v][k];
            r->drum.lvl[v] = 100;
        }
    }
}

void vb_lib_fx(vb_recipe_t *r, int i)
{
    uint32_t k;
    if (i < 0 || i >= (int)NCHAIN)
        return;
    for (k = 0; k < VB_FX_SLOTS; k++) {
        vb_fx_slot_t e = { 0, { 64, 64, 64, 64 }, 127, 1 };    /* (an empty slot, as a new recipe has) */
        r->fx[k] = k < 4u && CHAIN[i].s[k].type ? CHAIN[i].s[k] : e;
    }
}
