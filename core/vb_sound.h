/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: the sound. A recipe holds everything a print needs (source patches, play data, modulation, FX
 * chain, print settings, seed); the player renders it. Parameters are addressed uniformly (vb_addr_t) so the UI,
 * LEARN and modulation share one scheme. */
#ifndef VB_SOUND_H
#define VB_SOUND_H
#include <stdint.h>
#include "vb_param.h"
#include "vb_engine.h"
#include "vb_drum.h"
#include "vb_mod.h"
#include "vb_fx.h"

enum { VB_SRC_OP4, VB_SRC_ANLG, VB_SRC_DRUM, VB_SRC_SAMP, VB_SRC_GRAN, VB_SRC_CHOP, VB_SRCS };
extern const char *const vb_src_name[VB_SRCS];

#define VB_OP4_NP 38
#define VB_ANLG_NP 16
#define VB_GRAN_NP 11                     /* POS SIZE DENS SPRAY PITCH PRND REV WIN FRZ WIDTH SEED */
#define VB_CHOP_NP 11                     /* SENS SLICE PITCH REV ORDER LEN ATK DEC MAP SPLIT SLICES */
#define VB_SAMP_NP 8                      /* START LEN LOOP REV PITCH FINE FADE GAIN */
#define VB_CHORD_STEPS 16
#define VB_CHORD_TYPES 22
#define VB_CHORD_NOTES 8
#define VB_SEQ_STEPS 16
#define VB_MODSRC 9                       /* L1..L6, E1..E3 */
#define VB_PRINT_NP 6                     /* LEN START NORM AFTER TAIL FMT */

enum { VB_PLAY_NOTE, VB_PLAY_SEQ, VB_PLAY_CHORD, VB_PLAY_ARP };
enum { VB_ARP_UP, VB_ARP_DOWN, VB_ARP_UPDN, VB_ARP_RND, VB_ARP_ORDER, VB_ARPS };
extern const char *const vb_arp_name[VB_ARPS];
enum { VB_STRUM_UP, VB_STRUM_DOWN, VB_STRUM_ALT, VB_STRUM_RND, VB_STRUM_HARP, VB_STRUM_LUSH, VB_STRUM_ARP, VB_STRUMS };
extern const char *const vb_strum_name[VB_STRUMS];
extern const uint8_t vb_len16[12];        /* chord step lengths in sixteenths */
extern const char *const vb_len_name[12];

/* a chord step: up to 8 exact notes, its BASS (0 = none, n = the chord's n-th tone from the root, an octave or two
 * down, played first by every strum: C with E in the bass = 2) and its VOICING */
typedef struct { uint8_t note[VB_CHORD_NOTES]; uint8_t n, root, type, len_i, gate, bass, voi; } vb_chord_t;
enum { VB_VOI_CLOSE, VB_VOI_OPEN, VB_VOI_DROP2, VB_VOI_SPREAD, VB_VOI_VEFF, VB_VOICINGS };
extern const char *const vb_voicing_name[VB_VOICINGS];
typedef struct { uint8_t on, note, slide, prob; } vb_step_t;
typedef struct {
    uint8_t mode, strum, time, hum, vel, sprd, rate, len, sel;
    vb_chord_t chord[VB_CHORD_STEPS];
    vb_step_t seq[VB_SEQ_STEPS];
} vb_play_t;

/* a modulation source: LFO (kind 0) or envelope (kind 1) */
typedef struct {
    uint8_t kind;
    uint8_t wave, rate, sync, trig, phase;      /* LFO */
    uint8_t atk, dec, sus, rel, loop, vel;      /* envelope */
    vb_route_t r[VB_ROUTES];
} vb_modsrc_t;

typedef struct {
    uint8_t src;
    uint8_t op4[VB_OP4_NP];
    uint8_t anlg[VB_ANLG_NP];
    vb_drum_patch_t drum;
    uint8_t samp[VB_SAMP_NP];
    uint8_t gran[VB_GRAN_NP];
    uint8_t chop[VB_CHOP_NP];
    vb_play_t play;
    vb_modsrc_t mod[VB_MODSRC];
    vb_fx_slot_t fx[VB_FX_SLOTS];
    uint8_t print[VB_PRINT_NP];
    uint8_t pnote;                        /* the note a NOTE print plays (the last key) */
    uint16_t bpm10;                       /* the tempo at the print: a REBUILD renders at it */
    uint32_t seed;
    uint8_t key, scale;                   /* the harmony: KEY (0 = C) and SCALE (VB_SC_*); chords by degree use it */
    uint8_t cin;                          /* CHORD INPUT: 0 NOTES (the keys add notes), 1 DEGREE (the keys play chords) */
    uint8_t arp_pat, arp_oct, arp_rate;   /* ARP: the pattern (VB_ARP_*), octaves - 1, rate (0 1/16 1 1/8 2 1/4 3 1/32) */
    uint8_t spare[2];                     /* zero; room for new parameters whose 0 keeps the old sound */
} vb_recipe_t;

/* parameter addresses: domain << 8 | index */
enum {
    VB_D_OP4 = 1,      /* index 0..37 */
    VB_D_ANLG,         /* 0..15 */
    VB_D_DRUM,         /* voice * 9 + i, i 0..7 the shaping params, 8 the LEVEL */
    VB_D_SAMP,         /* 0..7 */
    VB_D_FX,           /* slot * 5 + i, i 0..3 the params, 4 MIX */
    VB_D_MOD,          /* source * 8 + i: LFO RATE PHASE / ENV ATK DEC SUS REL (see vb_addr_name) */
    VB_D_PLAY,         /* 0 TIME 1 HUMAN 2 VEL 3 SPRD */
    VB_D_GLOBAL,       /* 0 PITCH (+-24 semitones around 64) 1 VOL */
    VB_D_GRAN,         /* 0..10 */
    VB_D_CHOP,         /* 0..8 */
};
typedef uint16_t vb_addr_t;
#define VB_ADDR(d, i) ((vb_addr_t)((d) << 8 | (i)))
#define VB_ADDR_NONE VB_NO_DEST

void vb_recipe_default(vb_recipe_t *r, uint32_t seed);
/* the stored knob value at an address (0..127), or -1 for none */
int vb_addr_get(const vb_recipe_t *r, vb_addr_t a);
void vb_addr_set(vb_recipe_t *r, vb_addr_t a, int v);
/* the short name a destination shows ("B LVL", "AN CUT", "KCK SWEEP", "FX2 TIME", "L1 RATE", "PITCH") */
void vb_addr_name(const vb_recipe_t *r, vb_addr_t a, char *out, int max);
/* every modulatable address for the current source and chain, in display order; returns the count */
int vb_addr_list(const vb_recipe_t *r, vb_addr_t *out, int max);

/* harmony: scales, chords on a scale degree, roman numerals */
enum { VB_SC_MAJ, VB_SC_MIN, VB_SC_DOR, VB_SC_PHR, VB_SC_LYD, VB_SC_MIX, VB_SC_LOC, VB_SC_HMIN, VB_SC_MMIN, VB_SCALES };
extern const char *const vb_scale_name[VB_SCALES];
extern const uint8_t vb_scale_iv[VB_SCALES][7];
/* chord modifiers, held with a degree (VB_CM_*); a few make sense together (7 + 9, 6 + 9, SWAP + 7) */
enum { VB_CM_7 = 1, VB_CM_6 = 2, VB_CM_SWAP = 4, VB_CM_SUS2 = 8, VB_CM_SUS4 = 16, VB_CM_DIM = 32, VB_CM_9 = 64 };
/* the chord type on degree deg (0..6) of the scale, or with deg < 0 a major chord on the root `semis` above the key
 * (a borrowed chord: bIII, bVII ...), changed by the modifiers mods; *root_pc gets the root's pitch class */
int vb_degree_chord(int key, int scale, int deg, int semis, int mods, int *root_pc);
/* the chord's roman numeral in key / scale, in the chord names' style: "I", "IIm7", "V7", "bVIImaj7"; returns length */
int vb_chord_roman(const vb_chord_t *c, int key, int scale, char *out, int max);

/* chord helpers */
void vb_chord_fill(vb_chord_t *c, int root, int type, int base_note);
int vb_chord_name(const vb_chord_t *c, char *out, int max);
/* rewrites the step's notes from its pitch classes in voicing v (VB_VOI_*); the notes stay editable after */
void vb_chord_voice(vb_chord_t *c, int v);
extern const char *const vb_chord_type_name[VB_CHORD_TYPES];
/* the chord's tones as pitch classes in chord order (root, 3rd, 5th, 6th / 7th, 9th, 11th); returns the count */
int vb_chord_tones(const vb_chord_t *c, uint8_t *pcs);
/* the BASS note to play (0 = none) */
int vb_chord_bass(const vb_chord_t *c);

#endif
