/* SPDX-License-Identifier: GPL-3.0-only */
#include "vb_sound.h"
#include "vb_op4.h"
#include "vb_anlg.h"
#include "vb_samp.h"
#include "vb_gran.h"
#include "vb_chop.h"

const char *const vb_src_name[VB_SRCS] = {"OP4", "ANLG", "DRUM", "SAMP", "GRAN", "CHOP"};
const char *const vb_strum_name[VB_STRUMS] = {"UP", "DOWN", "ALT", "RND", "HARP", "LUSH", "ARP"};
const uint8_t vb_len16[12] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
const char *const vb_len_name[12] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "3/4", "1 BAR", "1.5BR", "2 BAR",
                                     "3 BAR", "4 BAR"};
const char *const vb_voicing_name[VB_VOICINGS] = {"CLOSE", "OPEN", "DROP2", "SPREAD", "VEFF"};
const char *const vb_chord_type_name[VB_CHORD_TYPES] = {
    "MAJ", "MIN", "7", "MAJ7", "MIN7", "SUS2", "SUS4", "ADD9", "MIN9", "DIM", "5",
    "6", "MIN6", "6/9", "MAJ9", "9", "MIN11", "7SUS4", "MAJ7#11", "M7B5", "AUG", "MADD9"};
static const char *const CHORD_SUFFIX[VB_CHORD_TYPES] = {
    "", "m", "7", "maj7", "m7", "sus2", "sus4", "add9", "m9", "dim", "5",
    "6", "m6", "6/9", "maj9", "9", "m11", "7sus4", "maj7#11", "m7b5", "aug", "madd9"};
static const int8_t CHORD_IV[VB_CHORD_TYPES][7] = {
    {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 4, 7, 10, -1}, {0, 4, 7, 11, -1}, {0, 3, 7, 10, -1}, {0, 2, 7, -1},
    {0, 5, 7, -1}, {0, 4, 7, 14, -1}, {0, 3, 7, 10, 14, -1}, {0, 3, 6, -1}, {0, 7, 12, -1},
    {0, 4, 7, 9, -1}, {0, 3, 7, 9, -1}, {0, 4, 7, 9, 14, -1}, {0, 4, 7, 11, 14, -1}, {0, 4, 7, 10, 14, -1},
    {0, 3, 7, 10, 14, 17, -1}, {0, 5, 7, 10, -1}, {0, 4, 7, 11, 18, -1}, {0, 3, 6, 10, -1}, {0, 4, 8, -1},
    {0, 3, 7, 14, -1},
};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const OPN[4] = {"A", "B", "C", "D"};
static const char *const SAMP_LABEL[VB_SAMP_NP] = {"START", "LEN", "LOOP", "REV", "PITCH", "FINE", "FADE", "GAIN"};
static const char *const PLAY_LABEL[4] = {"TIME", "HUMAN", "VEL", "SPRD"};
static const char *const LFO_LABEL[2] = {"RATE", "PHASE"};
static const char *const ENV_LABEL[4] = {"ATK", "DEC", "SUS", "REL"};

static int vb_strcpy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
    return i;
}
static int vb_cat(char *d, int at, const char *s, int max) { return at + vb_strcpy(d + at, s, max - at); }
static int vb_catnum(char *d, int at, int v, int max)
{
    char t[8];
    int n = 0;
    if (v < 0) {
        at = vb_cat(d, at, "-", max);
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    while (n && at < max - 1)
        d[at++] = t[--n];
    d[at] = 0;
    return at;
}

void vb_chord_fill(vb_chord_t *c, int root, int type, int base_note)
{
    int i;
    c->root = (uint8_t)root;
    c->type = (uint8_t)type;
    c->n = 0;
    for (i = 0; i < 7 && CHORD_IV[type][i] >= 0; i++)
        c->note[c->n++] = (uint8_t)(base_note + root + CHORD_IV[type][i]);
    if (c->voi)
        vb_chord_voice(c, c->voi);
}

static void sort_u8(uint8_t *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && a[j - 1] > a[j]; j--) {
            uint8_t t = a[j];
            a[j] = a[j - 1];
            a[j - 1] = t;
        }
}

int vb_chord_tones(const vb_chord_t *c, uint8_t *pcs)
{
    uint32_t have = 0;
    int i, n = 0;
    for (i = 0; i < c->n; i++)
        have |= 1u << (((c->note[i] - c->root) % 12 + 12) % 12);
    {   /* in chord order: root, third, fifth, sixth / seventh, ninth, eleventh (sus seconds and fourths last) */
        static const uint8_t ORDER[12] = {0, 4, 3, 7, 6, 8, 9, 10, 11, 2, 1, 5};
        for (i = 0; i < 12; i++)
            if ((have >> ORDER[i]) & 1u)
                pcs[n++] = (uint8_t)((c->root + ORDER[i]) % 12);
    }
    return n;
}

int vb_chord_bass(const vb_chord_t *c)
{
    uint8_t pcs[12];
    int n = vb_chord_tones(c, pcs);
    if (!c->bass || !n)
        return 0;
    return 36 + pcs[(c->bass - 1) % n];                 /* C2 .. B2 */
}

/* the voicings, as in the mockup (ui/index.html revoice): a = the root in the octave of C3 */
void vb_chord_voice(vb_chord_t *c, int v)
{
    int pcs[12], np = 0, out[16], no = 0, a, i, k, last;
    uint32_t have = 0;
    if (!c->n || v < 0 || v >= VB_VOICINGS)
        return;
    for (i = 0; i < c->n; i++)
        have |= 1u << (((c->note[i] - c->root) % 12 + 12) % 12);
    for (i = 0; i < 12; i++)
        if ((have >> i) & 1u)
            pcs[np++] = i;
    a = 48 + c->root;
    if (v == VB_VOI_CLOSE || v == VB_VOI_DROP2) {
        for (i = 0; i < np; i++)
            out[no++] = a + pcs[i];
        if (v == VB_VOI_DROP2 && no > 2)
            out[no - 2] -= 12;
    } else if (v == VB_VOI_OPEN) {
        for (i = 0; i < np; i++)
            out[no++] = a + pcs[i] + (i & 1 ? 12 : 0);
    } else if (v == VB_VOI_SPREAD) {
        out[no++] = last = a - 12;
        for (i = 0, k = 0; i < np; i++) {
            int n;
            if (!pcs[i])
                continue;
            for (n = a - 12 + pcs[i]; n < last + (k ? 3 : 7); n += 12)
                ;
            out[no++] = last = n;
            k++;
        }
    } else {                                                /* VEFF: root and fifth low, 3 7 9 (11 in minor) packed high */
        uint32_t up = (have & ~(1u | 1u << 7)) | 1u << 2 | (((have >> 3) & 1u) ? 1u << 5 : 0u);
        out[no++] = a - 12;
        out[no++] = a - 5;
        for (i = 1; i < 12; i++)
            if ((up >> i) & 1u)
                out[no++] = a + i + (i < 3 ? 12 : 0);
        out[no++] = a + 19;
    }
    {
        uint8_t ns[16];
        int m = 0;
        for (i = 0; i < no; i++) {
            int n = out[i] < 24 ? out[i] + 12 : out[i] > 108 ? out[i] - 12 : out[i];
            for (k = 0; k < m && ns[k] != n; k++)
                ;
            if (k == m)
                ns[m++] = (uint8_t)n;
        }
        sort_u8(ns, m);
        c->n = (uint8_t)(m > VB_CHORD_NOTES ? VB_CHORD_NOTES : m);
        for (i = 0; i < c->n; i++)
            c->note[i] = ns[i];
    }
    c->voi = (uint8_t)v;
}

int vb_chord_name(const vb_chord_t *c, char *out, int max)
{
    int lo = 127, i, k, r;
    uint32_t pcs = 0;
    if (!c->n)
        return vb_strcpy(out, "-", max);
    for (i = 0; i < c->n; i++) {
        lo = c->note[i] < lo ? c->note[i] : lo;
        pcs |= 1u << (c->note[i] % 12);
    }
    for (k = -1; k < 12; k++) {                      /* the bass note's class first, then any root */
        r = k < 0 ? lo % 12 : k;
        if (!((pcs >> r) & 1u))
            continue;
        for (i = 0; i < VB_CHORD_TYPES; i++) {
            uint32_t tp = 0, set = 0;
            int j;
            for (j = 0; j < 7 && CHORD_IV[i][j] >= 0; j++)
                tp |= 1u << (CHORD_IV[i][j] % 12);
            for (j = 0; j < 12; j++)
                if ((pcs >> j) & 1u)
                    set |= 1u << ((j - r + 12) % 12);
            if (set == tp) {
                int at = vb_strcpy(out, NOTE_NAME[r], max);
                at = vb_cat(out, at, CHORD_SUFFIX[i], max);
                if (r != lo % 12) {
                    at = vb_cat(out, at, "/", max);
                    at = vb_cat(out, at, NOTE_NAME[lo % 12], max);
                }
                return at;
            }
        }
    }
    if (c->n == 1) {
        int at = vb_strcpy(out, NOTE_NAME[lo % 12], max);
        return vb_catnum(out, at, lo / 12 - 1, max);
    }
    {   /* extended: root, then m, 7 / maj7, (9, 11), sus */
        int r0 = c->root % 12, at;
        uint32_t h = 0;
        for (i = 0; i < 12; i++)
            if ((pcs >> i) & 1u)
                h |= 1u << ((i - r0 + 12) % 12);
#define H(x) ((h >> (x)) & 1u)
        at = vb_strcpy(out, NOTE_NAME[r0], max);
        if (H(3) && !H(4))
            at = vb_cat(out, at, "m", max);
        if (H(10))
            at = vb_cat(out, at, "7", max);
        else if (H(11))
            at = vb_cat(out, at, "maj7", max);
        if (H(10) || H(11)) {
            if (H(2) && H(5) && (H(3) || H(4)))
                at = vb_cat(out, at, "(9,11)", max);
            else if (H(2))
                at = vb_cat(out, at, "(9)", max);
            else if (H(5) && (H(3) || H(4)))
                at = vb_cat(out, at, "(11)", max);
        } else if (H(2) && (H(3) || H(4))) {
            at = vb_cat(out, at, "add9", max);
        }
        if (!H(3) && !H(4))
            at = vb_cat(out, at, H(5) ? "sus4" : H(2) ? "sus2" : "", max);
        if (lo % 12 != r0) {
            at = vb_cat(out, at, "/", max);
            at = vb_cat(out, at, NOTE_NAME[lo % 12], max);
        }
#undef H
        return at;
    }
}

void vb_recipe_default(vb_recipe_t *r, uint32_t seed)
{
    int i;
    static const uint8_t SAMP_DEF[VB_SAMP_NP] = {0, 127, 0, 0, 24, 50, 6, 100};
    static const uint8_t ROOTS[8][3] = {{0, 4, 7}, {5, 3, 5}, {9, 4, 5}, {7, 0, 7}, {0, 4, 7}, {8, 3, 3}, {5, 7, 5}, {7, 5, 9}};
    uint8_t *b = (uint8_t *)r;
    for (i = 0; i < (int)sizeof *r; i++)
        b[i] = 0;
    r->src = VB_SRC_OP4;
    for (i = 0; i < VB_OP4_NP; i++)
        r->op4[i] = vb_op4.desc.params[i].def;
    for (i = 0; i < VB_ANLG_NP; i++)
        r->anlg[i] = vb_anlg.desc.params[i].def;
    vb_drum_patch_default(&r->drum);
    for (i = 0; i < VB_SAMP_NP; i++)
        r->samp[i] = SAMP_DEF[i];
    for (i = 0; i < VB_GRAN_NP; i++)
        r->gran[i] = vb_gran.desc.params[i].def;
    for (i = 0; i < VB_CHOP_NP; i++)
        r->chop[i] = vb_chop.desc.params[i].def;
    r->play.mode = VB_PLAY_CHORD;
    r->play.strum = VB_STRUM_UP;
    r->play.time = 30;
    r->play.hum = 10;
    r->play.vel = 96;
    r->play.sprd = 20;
    r->play.rate = 1;
    r->play.len = 8;
    for (i = 0; i < VB_CHORD_STEPS; i++) {                 /* 8 chords; the steps after them empty, a bar each */
        if (i < 8)
            vb_chord_fill(&r->play.chord[i], ROOTS[i][0], ROOTS[i][1], 48);
        r->play.chord[i].len_i = i < 8 ? ROOTS[i][2] : 7;
        r->play.chord[i].gate = 90;
    }
    for (i = 0; i < VB_SEQ_STEPS; i++) {
        static const uint8_t OFFS[16] = {0, 0, 3, 0, 0, 7, 0, 0, 5, 0, 0, 3, 0, 0, 10, 0};
        r->play.seq[i].on = (uint8_t)(i == 0 || i == 3 || i == 6 || i == 8 || i == 11 || i == 14);
        r->play.seq[i].note = (uint8_t)(48 + OFFS[i]);
        r->play.seq[i].prob = 100;
    }
    for (i = 0; i < VB_MODSRC; i++) {
        vb_modsrc_t *m = &r->mod[i];
        int k;
        m->kind = i >= 6;
        m->wave = (uint8_t)(i % 7);
        m->rate = (uint8_t)(40 + i * 10);
        m->sync = 1;
        m->atk = 5;
        m->dec = 40;
        m->rel = 20;
        m->vel = 64;
        for (k = 0; k < VB_ROUTES; k++)
            m->r[k].dest = VB_ADDR_NONE;
    }
    for (i = 0; i < VB_FX_SLOTS; i++) {
        r->fx[i].mix = 127;
        r->fx[i].on = 1;
        r->fx[i].p[0] = r->fx[i].p[1] = r->fx[i].p[2] = r->fx[i].p[3] = 64;
    }
    r->print[0] = 1;                                 /* LEN 1 BAR */
    r->print[2] = 1;                                 /* NORM on */
    r->pnote = 60;
    r->bpm10 = 1200;
    r->seed = seed;
}

static uint8_t *addr_ptr(vb_recipe_t *r, vb_addr_t a)
{
    int d = a >> 8, i = a & 0xFF;
    switch (d) {
    case VB_D_OP4: return i < VB_OP4_NP ? &r->op4[i] : 0;
    case VB_D_ANLG: return i < VB_ANLG_NP ? &r->anlg[i] : 0;
    case VB_D_DRUM:
        if (i / 9 >= VB_DRUM_VOICES)
            return 0;
        return i % 9 == 8 ? &r->drum.lvl[i / 9] : &r->drum.p[i / 9][i % 9];
    case VB_D_SAMP: return i < VB_SAMP_NP ? &r->samp[i] : 0;
    case VB_D_GRAN: return i < VB_GRAN_NP ? &r->gran[i] : 0;
    case VB_D_CHOP: return i < VB_CHOP_NP ? &r->chop[i] : 0;
    case VB_D_FX:
        if (i / 5 >= VB_FX_SLOTS)
            return 0;
        return i % 5 == 4 ? &r->fx[i / 5].mix : &r->fx[i / 5].p[i % 5];
    case VB_D_MOD: {
        vb_modsrc_t *m;
        if (i / 8 >= VB_MODSRC)
            return 0;
        m = &r->mod[i / 8];
        if (!m->kind)
            return i % 8 == 0 ? &m->rate : i % 8 == 1 ? &m->phase : 0;
        switch (i % 8) {
        case 0: return &m->atk;
        case 1: return &m->dec;
        case 2: return &m->sus;
        case 3: return &m->rel;
        }
        return 0;
    }
    case VB_D_PLAY:
        switch (i) {
        case 0: return &r->play.time;
        case 1: return &r->play.hum;
        case 2: return &r->play.vel;
        case 3: return &r->play.sprd;
        }
        return 0;
    }
    return 0;
}

int vb_addr_get(const vb_recipe_t *r, vb_addr_t a)
{
    uint8_t *p = addr_ptr((vb_recipe_t *)r, a);
    return p ? *p : -1;
}
void vb_addr_set(vb_recipe_t *r, vb_addr_t a, int v)
{
    uint8_t *p = addr_ptr(r, a);
    if (p)
        *p = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
}

void vb_addr_name(const vb_recipe_t *r, vb_addr_t a, char *out, int max)
{
    int d = a >> 8, i = a & 0xFF, at = 0;
    out[0] = 0;
    if (a == VB_ADDR_NONE) {
        vb_strcpy(out, "-", max);
        return;
    }
    switch (d) {
    case VB_D_OP4:
        if (i < 36) {
            at = vb_cat(out, 0, OPN[i / 9], max);
            at = vb_cat(out, at, " ", max);
        } else {
            at = vb_cat(out, 0, "OP ", max);
        }
        vb_cat(out, at, vb_op4.desc.params[i].label, max);
        return;
    case VB_D_ANLG:
        vb_cat(out, vb_cat(out, 0, "AN ", max), vb_anlg.desc.params[i].label, max);
        return;
    case VB_D_DRUM:
        at = vb_cat(out, 0, vb_drum_voices[i / 9].shortname, max);
        at = vb_cat(out, at, " ", max);
        vb_cat(out, at, i % 9 == 8 ? "LEVEL" : vb_drum_voices[i / 9].p[i % 9].label, max);
        return;
    case VB_D_SAMP:
        vb_cat(out, vb_cat(out, 0, "SM ", max), SAMP_LABEL[i], max);
        return;
    case VB_D_GRAN:
        vb_cat(out, vb_cat(out, 0, "GR ", max), vb_gran.desc.params[i].label, max);
        return;
    case VB_D_CHOP:
        vb_cat(out, vb_cat(out, 0, "CH ", max), vb_chop.desc.params[i].label, max);
        return;
    case VB_D_FX:
        at = vb_cat(out, 0, "FX", max);
        at = vb_catnum(out, at, i / 5 + 1, max);
        at = vb_cat(out, at, " ", max);
        vb_cat(out, at, i % 5 == 4 ? "MIX" : vb_fx_types[r->fx[i / 5].type].label[i % 5], max);
        return;
    case VB_D_MOD:
        at = vb_cat(out, 0, i / 8 < 6 ? "L" : "E", max);
        at = vb_catnum(out, at, i / 8 < 6 ? i / 8 + 1 : i / 8 - 5, max);
        at = vb_cat(out, at, " ", max);
        vb_cat(out, at, r->mod[i / 8].kind ? ENV_LABEL[i % 8 & 3] : LFO_LABEL[i % 8 & 1], max);
        return;
    case VB_D_PLAY:
        vb_cat(out, vb_cat(out, 0, "PL ", max), PLAY_LABEL[i & 3], max);
        return;
    case VB_D_GLOBAL:
        vb_strcpy(out, i == 1 ? "VOL" : "PITCH", max);
        return;
    }
    vb_strcpy(out, "?", max);
}

int vb_addr_list(const vb_recipe_t *r, vb_addr_t *out, int max)
{
    int n = 0, i, k;
#define ADD(a) do { if (n < max) out[n++] = (a); } while (0)
    ADD(VB_ADDR(VB_D_GLOBAL, 1));
    switch (r->src) {
    case VB_SRC_OP4:
        for (i = 0; i < VB_OP4_NP; i++)
            if (vb_op4.desc.params[i].flags & VB_P_MOD)
                ADD(VB_ADDR(VB_D_OP4, i));
        break;
    case VB_SRC_ANLG:
        for (i = 0; i < VB_ANLG_NP; i++)
            if (vb_anlg.desc.params[i].flags & VB_P_MOD)
                ADD(VB_ADDR(VB_D_ANLG, i));
        break;
    case VB_SRC_DRUM:
        for (i = 0; i < VB_DRUM_VOICES * 9; i++)
            ADD(VB_ADDR(VB_D_DRUM, i));
        break;
    case VB_SRC_GRAN:
        for (i = 0; i < VB_GRAN_NP; i++)
            if (vb_gran.desc.params[i].flags & VB_P_MOD)
                ADD(VB_ADDR(VB_D_GRAN, i));
        break;
    case VB_SRC_CHOP:
        for (i = 0; i < VB_CHOP_NP; i++)
            if (vb_chop.desc.params[i].flags & VB_P_MOD)
                ADD(VB_ADDR(VB_D_CHOP, i));
        break;
    default:
        for (i = 0; i < VB_SAMP_NP; i++)
            if (i != 2 && i != 3)
                ADD(VB_ADDR(VB_D_SAMP, i));
        break;
    }
    for (i = 0; i < 4; i++)
        ADD(VB_ADDR(VB_D_PLAY, i));
    for (i = 0; i < VB_FX_SLOTS; i++)
        if (r->fx[i].type)
            for (k = 0; k < 5; k++)
                ADD(VB_ADDR(VB_D_FX, i * 5 + k));
    for (i = 0; i < VB_MODSRC; i++)
        for (k = 0; k < (r->mod[i].kind ? 4 : 2); k++)
            ADD(VB_ADDR(VB_D_MOD, i * 8 + k));
#undef ADD
    return n;
}
