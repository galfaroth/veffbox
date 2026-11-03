/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests for the flash formats: a sound with 12 generations packs into one sector and comes back identical;
 * a corrupted sector is refused; a sound with very different generations drops the oldest; ADPCM round trip. */
#include <stdio.h>
#include <string.h>
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
