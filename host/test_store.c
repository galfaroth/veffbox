/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests for the flash formats: a sound with 12 generations packs into one sector and comes back identical;
 * a corrupted sector is refused; a sound with very different generations drops the oldest; ADPCM round trip. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "../core/vb_store.h"
#include "../core/vb_dsp.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t sec[VB_SECTOR];
static vb_gen_t gens[VB_MAXGEN], back[VB_MAXGEN];

int main(void)
{
    vb_recipe_t cur, cur2;
    uint32_t rng = 5, i;
    int g, n, cg;
    vb_recipe_default(&cur, 11);
    for (g = 0; g < VB_MAXGEN; g++) {              /* each generation: a few knobs moved, like real use */
        gens[g].rc = cur;
        gens[g].parent = (int8_t)(g - 1);
        gens[g].audio = g == VB_MAXGEN - 1;
        for (i = 0; i < 12u; i++)
            ((uint8_t *)&gens[g].rc)[vb_rng(&rng) % 200u] ^= (uint8_t)(1u + vb_rng(&rng) % 100u);
    }
    n = vb_sound_pack(&cur, gens, VB_MAXGEN, 7, sec);
    CHECK(n == VB_MAXGEN, "kept %d generations", n);
    memset(&cur2, 0, sizeof cur2);
    memset(back, 0, sizeof back);
    CHECK(vb_sound_unpack(sec, &cur2, back, VB_MAXGEN, &cg) == VB_MAXGEN, "unpack");
    CHECK(memcmp(&cur, &cur2, sizeof cur) == 0, "current recipe differs");
    for (g = 0; g < VB_MAXGEN; g++) {
        CHECK(memcmp(&gens[g].rc, &back[g].rc, sizeof cur) == 0, "generation %d differs", g);
        CHECK(gens[g].parent == back[g].parent && gens[g].audio == back[g].audio, "generation %d links", g);
    }
    CHECK(cg == 7, "curgen %d", cg);
    printf("12 generations: %u of %u bytes\n", (unsigned)(sec[4] | sec[5] << 8) + 12u, VB_SECTOR);
    sec[300] ^= 1u;
    CHECK(vb_sound_unpack(sec, &cur2, back, VB_MAXGEN, &cg) < 0, "a corrupted sector was accepted");
    memset(sec, 0xFF, sizeof sec);
    CHECK(vb_sound_unpack(sec, &cur2, back, VB_MAXGEN, &cg) < 0, "an erased sector was accepted");

    /* generations that differ everywhere: the oldest go, the rest stay intact */
    for (g = 0; g < VB_MAXGEN; g++)
        for (i = 0; i < sizeof cur; i++)
            ((uint8_t *)&gens[g].rc)[i] = (uint8_t)vb_rng(&rng);
    n = vb_sound_pack(&cur, gens, VB_MAXGEN, VB_MAXGEN - 1, sec);
    CHECK(n > 0 && n < VB_MAXGEN, "kept %d of very different generations", n);
    CHECK(vb_sound_unpack(sec, &cur2, back, VB_MAXGEN, &cg) == n, "unpack after drops");
    for (g = 0; g < n; g++)
        CHECK(memcmp(&gens[VB_MAXGEN - n + g].rc, &back[g].rc, sizeof cur) == 0, "kept generation %d differs", g);
    printf("very different generations: kept the last %d\n", n);

    /* format 1 (to 0.2.2: chords of 6 notes): a stored sound and its generation come back converted */
    {
        typedef struct { uint8_t note[6]; uint8_t n, root, type, len_i, gate; } c1_t;
        typedef struct { uint8_t mode, strum, time, hum, vel, sprd, rate, len, sel; c1_t chord[8];
                         vb_step_t seq[VB_SEQ_STEPS]; } p1_t;
        typedef struct { uint8_t src, op4[VB_OP4_NP], anlg[VB_ANLG_NP]; vb_drum_patch_t drum; uint8_t samp[VB_SAMP_NP],
                         gran[VB_GRAN_NP], chop[9]; p1_t play; vb_modsrc_t mod[VB_MODSRC];
                         vb_fx_slot_t fx[VB_FX_SLOTS]; uint8_t print[VB_PRINT_NP], pnote; uint16_t bpm10; uint32_t seed; } r1_t;
        static r1_t o;
        vb_recipe_t want;
        uint32_t len, k;
        uint16_t c;
        vb_recipe_default(&want, 77);
        want.src = VB_SRC_ANLG;
        want.play.chord[3].note[2] = 70;
        want.fx[2].type = 5;
        memset(&o, 0, sizeof o);
        o.src = want.src;
        memcpy(o.op4, want.op4, sizeof o.op4);
        memcpy(o.anlg, want.anlg, sizeof o.anlg);
        o.drum = want.drum;
        memcpy(o.samp, want.samp, sizeof o.samp);
        memcpy(o.gran, want.gran, sizeof o.gran);
        memcpy(o.chop, want.chop, sizeof o.chop);
        memcpy(&o.play, &want.play, 9);
        for (k = 0; k < 8; k++) {
            memcpy(o.play.chord[k].note, want.play.chord[k].note, 6);
            o.play.chord[k].n = want.play.chord[k].n;
            o.play.chord[k].root = want.play.chord[k].root;
            o.play.chord[k].type = want.play.chord[k].type;
            o.play.chord[k].len_i = want.play.chord[k].len_i;
            o.play.chord[k].gate = want.play.chord[k].gate;
        }
        memcpy(o.play.seq, want.play.seq, sizeof o.play.seq);
        memcpy(o.mod, want.mod, sizeof o.mod);
        memcpy(o.fx, want.fx, sizeof o.fx);
        memcpy(o.print, want.print, sizeof o.print);
        o.pnote = want.pnote;
        o.bpm10 = want.bpm10;
        o.seed = want.seed;
        memset(sec, 0xFF, sizeof sec);
        memcpy(sec + 12, &o, sizeof o);
        len = sizeof o;
        sec[12 + len] = 0xFF;                              /* one generation: parent -1, no audio, one run: SRC = DRUM */
        sec[12 + len + 1] = 0;
        sec[12 + len + 2] = 4;
        sec[12 + len + 3] = 0;
        sec[12 + len + 4] = 0;
        sec[12 + len + 5] = 0;
        sec[12 + len + 6] = 1;
        sec[12 + len + 7] = VB_SRC_DRUM;
        len += 8;
        memcpy(sec, "VBS1", 4);
        sec[4] = (uint8_t)len; sec[5] = (uint8_t)(len >> 8);
        c = vb_crc16(sec + 12, len);
        sec[6] = (uint8_t)c; sec[7] = (uint8_t)(c >> 8);
        sec[8] = (uint8_t)sizeof o; sec[9] = (uint8_t)(sizeof o >> 8);
        sec[10] = 1;
        sec[11] = 0;
        memset(&cur2, 0, sizeof cur2);
        CHECK(vb_sound_unpack(sec, &cur2, back, VB_MAXGEN, &cg) == 1, "a format 1 sound was refused");
        CHECK(memcmp(&cur2, &want, sizeof want) == 0, "format 1: the recipe changed in the conversion");
        CHECK(back[0].rc.src == VB_SRC_DRUM && back[0].rc.play.chord[3].note[2] == 70 && back[0].rc.fx[2].type == 5,
              "format 1: the generation's delta");
        printf("format 1 (%u B) converts to the current format (%u B)\n", (unsigned)sizeof o, (unsigned)sizeof want);
    }

    /* format 2 (0.3.0 test builds: 8 notes, the BASS a MIDI note): a stored sound and its generation come back converted */
    {
        typedef struct { uint8_t note[8]; uint8_t n, root, type, len_i, gate, bass, voi; } c1_t;
        typedef struct { uint8_t mode, strum, time, hum, vel, sprd, rate, len, sel; c1_t chord[8];
                         vb_step_t seq[VB_SEQ_STEPS]; } p1_t;
        typedef struct { uint8_t src, op4[VB_OP4_NP], anlg[VB_ANLG_NP]; vb_drum_patch_t drum; uint8_t samp[VB_SAMP_NP],
                         gran[VB_GRAN_NP], chop[9]; p1_t play; vb_modsrc_t mod[VB_MODSRC];
                         vb_fx_slot_t fx[VB_FX_SLOTS]; uint8_t print[VB_PRINT_NP], pnote; uint16_t bpm10; uint32_t seed; } r1_t;
        static r1_t o;
        vb_recipe_t want;
        uint32_t len, k;
        uint16_t c;
        vb_recipe_default(&want, 77);
        want.src = VB_SRC_ANLG;
        want.play.chord[3].note[2] = 70;
        want.fx[2].type = 5;
        want.play.chord[1].voi = VB_VOI_OPEN;
        want.play.chord[1].bass = 2;                       /* Fmaj7 (step 2): its 2nd tone, A, in the bass */
        memset(&o, 0, sizeof o);
        o.src = want.src;
        memcpy(o.op4, want.op4, sizeof o.op4);
        memcpy(o.anlg, want.anlg, sizeof o.anlg);
        o.drum = want.drum;
        memcpy(o.samp, want.samp, sizeof o.samp);
        memcpy(o.gran, want.gran, sizeof o.gran);
        memcpy(o.chop, want.chop, sizeof o.chop);
        memcpy(&o.play, &want.play, 9);
        for (k = 0; k < 8; k++) {
            memcpy(o.play.chord[k].note, want.play.chord[k].note, 8);
            o.play.chord[k].n = want.play.chord[k].n;
            o.play.chord[k].root = want.play.chord[k].root;
            o.play.chord[k].type = want.play.chord[k].type;
            o.play.chord[k].len_i = want.play.chord[k].len_i;
            o.play.chord[k].gate = want.play.chord[k].gate;
            o.play.chord[k].voi = want.play.chord[k].voi;
        }
        memcpy(o.play.seq, want.play.seq, sizeof o.play.seq);
        memcpy(o.mod, want.mod, sizeof o.mod);
        memcpy(o.fx, want.fx, sizeof o.fx);
        memcpy(o.print, want.print, sizeof o.print);
        o.pnote = want.pnote;
        o.bpm10 = want.bpm10;
        o.seed = want.seed;
        o.play.chord[1].bass = 33;                         /* A1 as a note in format 2 */
        memset(sec, 0xFF, sizeof sec);
        memcpy(sec + 12, &o, sizeof o);
        len = sizeof o;
        sec[12 + len] = 0xFF;                              /* one generation: parent -1, no audio, one run: SRC = DRUM */
        sec[12 + len + 1] = 0;
        sec[12 + len + 2] = 4;
        sec[12 + len + 3] = 0;
        sec[12 + len + 4] = 0;
        sec[12 + len + 5] = 0;
        sec[12 + len + 6] = 1;
        sec[12 + len + 7] = VB_SRC_DRUM;
        len += 8;
        memcpy(sec, "VBS1", 4);
        sec[4] = (uint8_t)len; sec[5] = (uint8_t)(len >> 8);
        c = vb_crc16(sec + 12, len);
        sec[6] = (uint8_t)c; sec[7] = (uint8_t)(c >> 8);
        sec[8] = (uint8_t)sizeof o; sec[9] = (uint8_t)(sizeof o >> 8);
        sec[10] = 1;
        sec[11] = 0;
        memset(&cur2, 0, sizeof cur2);
        CHECK(vb_sound_unpack(sec, &cur2, back, VB_MAXGEN, &cg) == 1, "a format 2 sound was refused");
        CHECK(memcmp(&cur2, &want, sizeof want) == 0, "format 2: the recipe changed in the conversion");
        for (k = 0; k < sizeof want; k++)
            if (((uint8_t *)&cur2)[k] != ((uint8_t *)&want)[k])
                printf("  byte %u (play at %u): %u != %u\n", k, (unsigned)offsetof(vb_recipe_t, play), ((uint8_t *)&cur2)[k], ((uint8_t *)&want)[k]);
        CHECK(back[0].rc.src == VB_SRC_DRUM && back[0].rc.play.chord[3].note[2] == 70 && back[0].rc.fx[2].type == 5,
              "format 2: the generation's delta");
        printf("format 2 (%u B) converts to the current format (%u B)\n", (unsigned)sizeof o, (unsigned)sizeof want);
    }

    /* format 3 (0.3.0: 16 chords, CHOP of 9 bytes, no spare bytes) */
    {
        typedef struct { uint8_t mode, strum, time, hum, vel, sprd, rate, len, sel; vb_chord_t chord[16];
                         vb_step_t seq[VB_SEQ_STEPS]; } p3_t;
        typedef struct { uint8_t src, op4[VB_OP4_NP], anlg[VB_ANLG_NP]; vb_drum_patch_t drum; uint8_t samp[VB_SAMP_NP],
                         gran[VB_GRAN_NP], chop[9]; p3_t play; vb_modsrc_t mod[VB_MODSRC];
                         vb_fx_slot_t fx[VB_FX_SLOTS]; uint8_t print[VB_PRINT_NP], pnote; uint16_t bpm10; uint32_t seed; } r3_t;
        static r3_t o;
        vb_recipe_t want;
        uint32_t len;
        uint16_t c;
        vb_recipe_default(&want, 99);
        want.src = VB_SRC_CHOP;
        want.chop[0] = 90;
        want.play.chord[12].n = 3;
        want.play.chord[12].note[1] = 66;
        want.play.chord[5].bass = 3;
        want.play.chord[5].voi = VB_VOI_VEFF;
        memset(&o, 0, sizeof o);
        o.src = want.src;
        memcpy(o.op4, want.op4, sizeof o.op4);
        memcpy(o.anlg, want.anlg, sizeof o.anlg);
        o.drum = want.drum;
        memcpy(o.samp, want.samp, sizeof o.samp);
        memcpy(o.gran, want.gran, sizeof o.gran);
        memcpy(o.chop, want.chop, 9);
        memcpy(&o.play, &want.play, sizeof o.play);
        memcpy(o.mod, want.mod, sizeof o.mod);
        memcpy(o.fx, want.fx, sizeof o.fx);
        memcpy(o.print, want.print, sizeof o.print);
        o.pnote = want.pnote;
        o.bpm10 = want.bpm10;
        o.seed = want.seed;
        memset(sec, 0xFF, sizeof sec);
        memcpy(sec + 12, &o, sizeof o);
        len = sizeof o;
        memcpy(sec, "VBS1", 4);
        sec[4] = (uint8_t)len; sec[5] = (uint8_t)(len >> 8);
        c = vb_crc16(sec + 12, len);
        sec[6] = (uint8_t)c; sec[7] = (uint8_t)(c >> 8);
        sec[8] = (uint8_t)sizeof o; sec[9] = (uint8_t)(sizeof o >> 8);
        sec[10] = 0;
        sec[11] = 0;
        memset(&cur2, 0, sizeof cur2);
        CHECK(vb_sound_unpack(sec, &cur2, back, VB_MAXGEN, &cg) == 0, "a format 3 sound was refused");
        CHECK(memcmp(&cur2, &want, sizeof want) == 0, "format 3: the recipe changed in the conversion");
        printf("format 3 (%u B) converts to the current format (%u B)\n", (unsigned)sizeof o, (unsigned)sizeof want);
    }

    /* ADPCM: a decaying chord, the error stays small */
    {
        static int16_t x[22050], y[22050];
        static uint8_t code[11025];
        vb_adpcm_t e = {0, 0}, d = {0, 0};
        uint32_t ph1 = 0, ph2 = 0;
        int64_t es = 0, xs = 0;
        int32_t amp = 30000;
        for (i = 0; i < 22050u; i++) {
            ph1 += 19500000u;
            ph2 += 29300000u;
            x[i] = (int16_t)(((vb_sin(ph1) + vb_sin(ph2)) / 2) * amp / 32768);
            if (i % 64u == 0u)
                amp = amp * 63 / 64;
        }
        vb_adpcm_encode(&e, x, 22050, code);
        vb_adpcm_decode(&d, code, 22050, y);
        for (i = 0; i < 22050u; i++) {
            es += (int64_t)(x[i] - y[i]) * (x[i] - y[i]);
            xs += (int64_t)x[i] * x[i];
        }
        CHECK(es * 100 < xs, "ADPCM error too large (%lld / %lld)", (long long)es, (long long)xs);
        printf("adpcm: signal/error %.1f dB\n", 10.0 * __builtin_log10((double)xs / (double)(es + 1)));
    }
    printf(failures ? "\n%d FAILED\n" : "\nall store tests passed\n", failures);
    return failures != 0;
}
