/* SPDX-License-Identifier: GPL-3.0-only */
#include "vb_store.h"

#define MAGIC 0x31534256u                       /* "VBS1" */
/* header: magic u32, payload bytes u16, crc16 u16, recipe size u16, ngen u8, curgen u8 */
#define HDR 12u
#if defined(FELUCCA_VERSION) && !defined(__EMSCRIPTEN__)       /* the FM-1 build: big buffers in the pool, not .bss */
#define VB_POOL __attribute__((section(".pool")))
#else
#define VB_POOL
#endif

typedef char vb_formats_differ[sizeof(vb_recipe_t) != 640u && sizeof(vb_recipe_t) != 672u ? 1 : -1];

/* older formats, read and converted on unpack. Format 1 (to 0.2.2): 8 chord steps of 6 notes. Format 2 (0.3.0
 * test builds): 8 steps of 8 notes, the BASS a MIDI note. Format 3: 16 steps, the BASS a chord tone. */
typedef struct { uint8_t note[6]; uint8_t n, root, type, len_i, gate; } chord_v1_t;
typedef struct { uint8_t note[8]; uint8_t n, root, type, len_i, gate, bass, voi; } chord_v2_t;
#define OLD_RECIPE(name, chord_t)                                                                    \
    typedef struct {                                                                                 \
        uint8_t mode, strum, time, hum, vel, sprd, rate, len, sel;                                   \
        chord_t chord[8];                                                                            \
        vb_step_t seq[VB_SEQ_STEPS];                                                                 \
    } name##_play_t;                                                                                 \
    typedef struct {                                                                                 \
        uint8_t src;                                                                                 \
        uint8_t op4[VB_OP4_NP];                                                                      \
        uint8_t anlg[VB_ANLG_NP];                                                                    \
        vb_drum_patch_t drum;                                                                        \
        uint8_t samp[VB_SAMP_NP];                                                                    \
        uint8_t gran[VB_GRAN_NP];                                                                    \
        uint8_t chop[VB_CHOP_NP];                                                                    \
        name##_play_t play;                                                                          \
        vb_modsrc_t mod[VB_MODSRC];                                                                  \
        vb_fx_slot_t fx[VB_FX_SLOTS];                                                                \
        uint8_t print[VB_PRINT_NP];                                                                  \
        uint8_t pnote;                                                                               \
        uint16_t bpm10;                                                                              \
        uint32_t seed;                                                                               \
    } name##_t;
OLD_RECIPE(recipe_v1, chord_v1_t)
OLD_RECIPE(recipe_v2, chord_v2_t)

/* everything but the chords, from an older recipe o into r */
#define COPY_REST(r, o)                                                                              \
    do {                                                                                             \
        uint32_t i_;                                                                                 \
        (r)->src = (o)->src;                                                                         \
        for (i_ = 0; i_ < VB_OP4_NP; i_++) (r)->op4[i_] = (o)->op4[i_];                              \
        for (i_ = 0; i_ < VB_ANLG_NP; i_++) (r)->anlg[i_] = (o)->anlg[i_];                           \
        (r)->drum = (o)->drum;                                                                       \
        for (i_ = 0; i_ < VB_SAMP_NP; i_++) (r)->samp[i_] = (o)->samp[i_];                           \
        for (i_ = 0; i_ < VB_GRAN_NP; i_++) (r)->gran[i_] = (o)->gran[i_];                           \
        for (i_ = 0; i_ < VB_CHOP_NP; i_++) (r)->chop[i_] = (o)->chop[i_];                           \
        (r)->play.mode = (o)->play.mode; (r)->play.strum = (o)->play.strum;                          \
        (r)->play.time = (o)->play.time; (r)->play.hum = (o)->play.hum; (r)->play.vel = (o)->play.vel; \
        (r)->play.sprd = (o)->play.sprd; (r)->play.rate = (o)->play.rate;                            \
        (r)->play.len = (o)->play.len; (r)->play.sel = (o)->play.sel;                                \
        for (i_ = 0; i_ < VB_SEQ_STEPS; i_++) (r)->play.seq[i_] = (o)->play.seq[i_];                 \
        for (i_ = 0; i_ < VB_MODSRC; i_++) (r)->mod[i_] = (o)->mod[i_];                              \
        for (i_ = 0; i_ < VB_FX_SLOTS; i_++) (r)->fx[i_] = (o)->fx[i_];                              \
        for (i_ = 0; i_ < VB_PRINT_NP; i_++) (r)->print[i_] = (o)->print[i_];                        \
        (r)->pnote = (o)->pnote; (r)->bpm10 = (o)->bpm10; (r)->seed = (o)->seed;                     \
    } while (0)

static void chords_clear(vb_recipe_t *r)
{
    uint32_t i, k;
    for (i = 0; i < VB_CHORD_STEPS; i++) {
        vb_chord_t *c = &r->play.chord[i];
        for (k = 0; k < VB_CHORD_NOTES; k++)
            c->note[k] = 0;
        c->n = c->root = c->type = c->bass = c->voi = 0;
        c->len_i = 7;
        c->gate = 90;
    }
}

/* r holds an older recipe (version 1 or 2) in its first bytes: rewritten in place as the current format */
static void convert(vb_recipe_t *r, uint32_t version)
{
    static union { recipe_v1_t a; recipe_v2_t b; } o VB_POOL;   /* (static: the main loop's stack is small) */
    uint32_t i, k, n = version == 1u ? sizeof o.a : sizeof o.b;
    const uint8_t *src = (const uint8_t *)r;
    uint8_t *dst = (uint8_t *)&o;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    if (version == 1u)
        COPY_REST(r, &o.a);
    else
        COPY_REST(r, &o.b);
    chords_clear(r);
    for (i = 0; i < 8u; i++) {
        vb_chord_t *c = &r->play.chord[i];
        if (version == 1u) {
            const chord_v1_t *q = &o.a.play.chord[i];
            for (k = 0; k < 6u; k++)
                c->note[k] = q->note[k];
            c->n = q->n > 6 ? 6 : q->n;
            c->root = q->root; c->type = q->type; c->len_i = q->len_i; c->gate = q->gate;
        } else {
            const chord_v2_t *q = &o.b.play.chord[i];
            for (k = 0; k < 8u; k++)
                c->note[k] = q->note[k];
            c->n = q->n > 8 ? 8 : q->n;
            c->root = q->root; c->type = q->type; c->len_i = q->len_i; c->gate = q->gate; c->voi = q->voi;
            if (q->bass) {                                 /* the old BASS note: the chord tone of its class, else the root */
                uint8_t pcs[12];
                int t = vb_chord_tones(c, pcs), j;
                c->bass = 1;
                for (j = 0; j < t; j++)
                    if (pcs[j] == q->bass % 12)
                        c->bass = (uint8_t)(j + 1);
            }
        }
    }
}

uint16_t vb_crc16(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFu, i, k;
    for (i = 0; i < n; i++) {
        c ^= (uint32_t)p[i] << 8;
        for (k = 0; k < 8u; k++)
            c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1;
        c &= 0xFFFFu;
    }
    return (uint16_t)c;
}

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t r16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* the bytes of cur that differ from base as runs: [u16 offset][u8 length][bytes]; returns the size or -1 if > max */
static int delta(const uint8_t *base, const uint8_t *cur, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t s, len;
        if (base[i] == cur[i]) {
            i++;
            continue;
        }
        s = i;
        while (i < n && i - s < 255u && (base[i] != cur[i] || (i + 1u < n && base[i + 1u] != cur[i + 1u])))
            i++;                                /* a single equal byte inside a run stays in it */
        len = i - s;
        if (o + 3u + len > max)
            return -1;
        w16(out + o, s);
        out[o + 2u] = (uint8_t)len;
        for (o += 3u; s < i; s++)
            out[o++] = cur[s];
    }
    return (int)o;
}

int vb_sound_pack(const vb_recipe_t *cur, const vb_gen_t *gens, int ngen, int curgen, uint8_t *sector)
{
    const uint32_t rs = sizeof(vb_recipe_t);
    int first;
    for (first = 0; first <= ngen; first++) {   /* drop the oldest until the rest fits */
        uint32_t o = HDR, ok = 1;
        int g;
        const uint8_t *c = (const uint8_t *)cur;
        for (g = 0; g < (int)rs; g++)
            sector[o + (uint32_t)g] = c[g];
        o += rs;
        for (g = first; g < ngen && ok; g++) {
            int d, par = gens[g].parent < first ? -1 : gens[g].parent - first;
            if (o + 4u > VB_SECTOR) {
                ok = 0;
                break;
            }
            sector[o] = (uint8_t)(int8_t)par;
            sector[o + 1u] = gens[g].audio;
            d = delta(c, (const uint8_t *)&gens[g].rc, rs, sector + o + 4u, VB_SECTOR - o - 4u);
            if (d < 0) {
                ok = 0;
                break;
            }
            w16(sector + o + 2u, (uint32_t)d);
            o += 4u + (uint32_t)d;
        }
        if (!ok)
            continue;
        sector[0] = (uint8_t)MAGIC;
        sector[1] = (uint8_t)(MAGIC >> 8);
        sector[2] = (uint8_t)(MAGIC >> 16);
        sector[3] = (uint8_t)(MAGIC >> 24);
        w16(sector + 4, o - HDR);
        w16(sector + 6, vb_crc16(sector + HDR, o - HDR));
        w16(sector + 8, rs);
        sector[10] = (uint8_t)(ngen - first);
        sector[11] = (uint8_t)(curgen - first < 0 ? 0 : curgen - first);
        return ngen - first;
    }
    return 0;
}

int vb_sound_unpack(const uint8_t *sector, vb_recipe_t *cur, vb_gen_t *gens, int maxgen, int *curgen)
{
    uint32_t rs = sizeof(vb_recipe_t), len, o, g, n, old = 0;
    uint8_t *c = (uint8_t *)cur;
    if (((uint32_t)sector[0] | (uint32_t)sector[1] << 8 | (uint32_t)sector[2] << 16 | (uint32_t)sector[3] << 24) != MAGIC)
        return -1;
    len = r16(sector + 4);
    if (r16(sector + 8) == sizeof(recipe_v1_t)) {     /* an older sound: unpacked as it was, then converted */
        rs = sizeof(recipe_v1_t);
        old = 1;
    } else if (r16(sector + 8) == sizeof(recipe_v2_t)) {
        rs = sizeof(recipe_v2_t);
        old = 2;
    }
    if (len > VB_SECTOR - HDR || len < rs || r16(sector + 8) != rs || vb_crc16(sector + HDR, len) != r16(sector + 6))
        return -1;
    for (g = 0; g < rs; g++)
        c[g] = sector[HDR + g];
    n = sector[10];
    if ((int)n > maxgen)
        n = (uint32_t)maxgen;
    for (o = HDR + rs, g = 0; g < n; g++) {
        uint32_t d, e;
        uint8_t *dst = (uint8_t *)&gens[g].rc;
        if (o + 4u > HDR + len)
            return -1;
        gens[g].parent = (int8_t)sector[o];
        gens[g].audio = sector[o + 1u];
        d = r16(sector + o + 2u);
        o += 4u;
        for (e = 0; e < rs; e++)
            dst[e] = c[e];
        for (e = o; e < o + d;) {               /* runs */
            uint32_t at = r16(sector + e), k = sector[e + 2u], j;
            e += 3u;
            for (j = 0; j < k && at + j < rs; j++)
                dst[at + j] = sector[e + j];
            e += k;
        }
        o += d;
    }
    if (old) {                                          /* the generations' deltas were against the old bytes */
        for (g = 0; g < n; g++)
            convert(&gens[g].rc, old);
        convert(cur, old);
    }
    *curgen = sector[11] < n ? sector[11] : (n ? (int)n - 1 : 0);
    return (int)n;
}

/* ---- IMA ADPCM */
static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t IDX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int32_t step_decode(vb_adpcm_t *st, uint32_t code)
{
    int32_t step = STEP[st->idx], diff = step >> 3;
    if (code & 4u)
        diff += step;
    if (code & 2u)
        diff += step >> 1;
    if (code & 1u)
        diff += step >> 2;
    st->pred += code & 8u ? -diff : diff;
    st->pred = st->pred > 32767 ? 32767 : st->pred < -32768 ? -32768 : st->pred;
    st->idx += IDX[code & 15u];
    st->idx = st->idx < 0 ? 0 : st->idx > 88 ? 88 : st->idx;
    return st->pred;
}

void vb_adpcm_encode(vb_adpcm_t *st, const int16_t *in, uint32_t n, uint8_t *out)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t diff = in[i] - st->pred, step = STEP[st->idx];
        uint32_t code = 0;
        if (diff < 0) {
            code = 8u;
            diff = -diff;
        }
        if (diff >= step) {
            code |= 4u;
            diff -= step;
        }
        if (diff >= step >> 1) {
            code |= 2u;
            diff -= step >> 1;
        }
        if (diff >= step >> 2)
            code |= 1u;
        step_decode(st, code);                  /* the encoder tracks the decoder exactly */
        if (i & 1u)
            out[i >> 1] |= (uint8_t)(code << 4);
        else
            out[i >> 1] = (uint8_t)code;
    }
}

void vb_adpcm_decode(vb_adpcm_t *st, const uint8_t *in, uint32_t n, int16_t *out)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        out[i] = (int16_t)step_decode(st, i & 1u ? in[i >> 1] >> 4 : in[i >> 1] & 15u);
}
