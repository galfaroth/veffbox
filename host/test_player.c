/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests for the player: a strummed chord sequence on OP4 and ANLG, a SEQ line on DRUM, PRINT into the
 * buffer and playing the print back as SAMP, LFO routes into the engine and the FX chain, random FX chains.
 * Checks: repeatable renders, no overflow, the print has audio and its length, modulation changes the sound. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/vb_player.h"
#include "../core/vb_dsp.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int16_t fxbuf[VB_FX_SLOTS * VB_FX_BUF], pa[36864], pb[36864];
static vb_player_t P;
static vb_recipe_t R;

static void wav(const char *path, const int32_t *x, int n)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)n * 2, riff = 36 + data, sr = VB_SR, br = VB_SR * 2, fmt = 16;
    uint16_t pcm = 1, ch = 1, al = 2, bits = 16;
    int i;
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&al, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (i = 0; i < n; i++) { int16_t s = (int16_t)x[i]; fwrite(&s, 2, 1, f); }
    fclose(f);
}

static int32_t out[VB_SR * 6];
/* renders seconds of the player; returns the peak, fills out[] */
static int run(double sec)
{
    int n = (int)(sec * VB_SR) / 32 * 32, i, pk = 0;
    for (i = 0; i < n; i += 32)
        vb_player_render(&P, out + i, 32);
    for (i = 0; i < n; i++)
        pk = abs(out[i]) > pk ? abs(out[i]) : pk;
    return pk;
}
static uint32_t hash(int n) { uint32_t h = 2166136261u; int i; for (i = 0; i < n; i++) h = (h ^ (uint32_t)out[i]) * 16777619u; return h; }

int main(void)
{
    int pk, i, k;
    uint32_t h1, h2;
    /* 1. a strummed chord sequence on OP4, twice: identical */
    vb_recipe_default(&R, 42);
    vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
    vb_player_play(&P, 1);
    pk = run(4.0);
    h1 = hash(VB_SR * 4 / 32 * 32);
    wav("host/out/player_op4_chords.wav", out, VB_SR * 4 / 32 * 32);
    CHECK(pk > 1000, "OP4 chords are silent (peak %d)", pk);
    vb_recipe_default(&R, 42);
    vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
    vb_player_play(&P, 1);
    run(4.0);
    h2 = hash(VB_SR * 4 / 32 * 32);
    CHECK(h1 == h2, "the same recipe renders differently");
    printf("OP4 chords: peak %d hash %08x\n", pk, h1);

    /* 2. every strum type and the ANLG engine */
    for (k = 0; k < VB_STRUMS; k++) {
        vb_recipe_default(&R, 7);
        R.src = VB_SRC_ANLG;
        R.play.strum = (uint8_t)k;
        R.play.time = 90;
        vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
        vb_player_play(&P, 1);
        pk = run(2.0);
        CHECK(pk > 500 && pk <= 32767, "ANLG strum %s peak %d", vb_strum_name[k], pk);
    }

    /* 3. DRUM with a SEQ line */
    vb_recipe_default(&R, 9);
    R.src = VB_SRC_DRUM;
    R.play.mode = VB_PLAY_SEQ;
    R.play.rate = 2;
    vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
    vb_player_play(&P, 1);
    pk = run(2.0);
    CHECK(pk > 1000, "DRUM sequence silent (peak %d)", pk);
    wav("host/out/player_drum_seq.wav", out, VB_SR * 2 / 32 * 32);

    /* 4. PRINT one bar of OP4 chords, then play it as SAMP */
    vb_recipe_default(&R, 42);
    vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
    R.print[0] = 1;
    vb_player_print(&P);
    for (i = 0; i < 400 && !P.rec_done; i++)
        run(0.01);
    run(1.0);
    CHECK(P.rec_done || P.plen[P.cur ^ 1] > 0, "the print did not finish");
    CHECK(vb_player_print_take(&P) == 1, "print_take");
    run(0.01);                                             /* the swap happens in the audio block */
    CHECK(P.plen[P.cur] > 30000, "print length %u", P.plen[P.cur]);
    R.src = VB_SRC_SAMP;
    vb_player_note(&P, 60, 110, 1);
    pk = run(1.0);
    CHECK(pk > 1000, "SAMP plays the print (peak %d)", pk);
    wav("host/out/player_samp.wav", out, VB_SR / 32 * 32);
    printf("print: %u samples at 22.05 kHz, SAMP peak %d\n", P.plen[P.cur], pk);

    /* 5. an LFO on ANLG's cutoff changes the sound */
    vb_recipe_default(&R, 5);
    R.src = VB_SRC_ANLG;
    R.play.mode = VB_PLAY_NOTE;
    vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
    vb_player_note(&P, 48, 110, 1);
    run(1.0);
    h1 = hash(VB_SR / 32 * 32);
    vb_recipe_default(&R, 5);
    R.src = VB_SRC_ANLG;
    R.play.mode = VB_PLAY_NOTE;
    R.mod[0].r[0].dest = VB_ADDR(VB_D_ANLG, 8);
    R.mod[0].r[0].amt = 60;
    R.mod[0].sync = 0;
    R.mod[0].rate = 100;
    vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
    vb_player_note(&P, 48, 110, 1);
    run(1.0);
    h2 = hash(VB_SR / 32 * 32);
    CHECK(h1 != h2, "an LFO route to AN CUT changed nothing");
    wav("host/out/player_anlg_lfo.wav", out, VB_SR / 32 * 32);

    /* 6. random FX chains with routes never overflow */
    {
        uint32_t rng = 1234;
        for (k = 0; k < 60; k++) {
            vb_recipe_default(&R, 100 + k);
            R.src = (uint8_t)(k % 3);
            for (i = 0; i < VB_FX_SLOTS; i++) {
                R.fx[i].type = (uint8_t)(vb_rng(&rng) % VB_FX_TYPES);
                R.fx[i].p[0] = (uint8_t)(vb_rng(&rng) % 128);
                R.fx[i].p[1] = (uint8_t)(vb_rng(&rng) % 128);
                R.fx[i].p[2] = (uint8_t)(vb_rng(&rng) % 128);
                R.fx[i].p[3] = (uint8_t)(vb_rng(&rng) % 128);
            }
            for (i = 0; i < 6; i++) {
                R.mod[i].r[0].dest = VB_ADDR(VB_D_FX, (vb_rng(&rng) % 8) * 5 + vb_rng(&rng) % 5);
                R.mod[i].r[0].amt = (int8_t)((int)(vb_rng(&rng) % 128) - 64);
            }
            vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
            vb_player_play(&P, 1);
            pk = run(1.0);
            CHECK(pk <= 32767, "random chain %d overflow %d", k, pk);
        }
    }

    /* 7. the address names and lists */
    {
        char b[32];
        vb_addr_t list[256];
        int n;
        vb_recipe_default(&R, 1);
        R.fx[1].type = VB_FX_TAPE;
        vb_addr_name(&R, VB_ADDR(VB_D_FX, 1 * 5 + 0), b, sizeof b);
        CHECK(strcmp(b, "FX2 TIME") == 0, "addr name %s", b);
        vb_addr_name(&R, VB_ADDR(VB_D_OP4, 9 + 3), b, sizeof b);
        CHECK(strcmp(b, "B LVL") == 0, "addr name %s", b);
        n = vb_addr_list(&R, list, 256);
        CHECK(n > 40, "addr list %d", n);
        vb_chord_name(&R.play.chord[0], b, sizeof b);
        CHECK(strcmp(b, "Cm7") == 0, "chord name %s", b);
    }
    printf(failures ? "\n%d FAILED\n" : "\nall player tests passed\n", failures);
    return failures != 0;
}
