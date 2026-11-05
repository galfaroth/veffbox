/* SPDX-License-Identifier: GPL-3.0-only */
/* The built-in library: every source sounds, in range of its parameters; every FX chain passes sound, bounded. */
#include <stdio.h>
#include <string.h>
#include "../core/vb_player.h"
#include "../core/vb_lib.h"
#include "../core/vb_op4.h"
#include "../core/vb_anlg.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int16_t fxbuf[VB_FX_SLOTS * VB_FX_BUF], pa[36864], pb[36864];
static vb_recipe_t R;
static vb_player_t P;

static int32_t play(int notes, uint32_t blocks)          /* a note (or a C major chord) for blocks; the peak */
{
    int32_t out[32], peak = 0;
    uint32_t b;
    int i;
    vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
    for (i = 0; i < notes; i++)
        vb_player_note(&P, 48 + (i == 1 ? 4 : i == 2 ? 7 : 0), 110, 1);
    for (b = 0; b < blocks; b++) {
        if (b == blocks / 2)
            for (i = 0; i < notes; i++)
                vb_player_note(&P, 48 + (i == 1 ? 4 : i == 2 ? 7 : 0), 0, 0);
        vb_player_render(&P, out, 32);
        for (i = 0; i < 32; i++)
            peak = out[i] > peak ? out[i] : -out[i] > peak ? -out[i] : peak;
    }
    return peak;
}

int main(void)
{
    int i, k;
    for (i = 0; i < vb_lib_nsrc(); i++) {
        int32_t pk;
        vb_recipe_default(&R, 1234);
        vb_lib_src(&R, i);
        if (R.src == VB_SRC_OP4)
            for (k = 0; k < VB_OP4_NP; k++)
                CHECK(R.op4[k] >= vb_op4.desc.params[k].min && R.op4[k] <= vb_op4.desc.params[k].max, "%s: OP4 %d = %u",
                      vb_lib_src_name(i), k, R.op4[k]);
        if (R.src == VB_SRC_ANLG)
            for (k = 0; k < VB_ANLG_NP; k++)
                CHECK(R.anlg[k] >= vb_anlg.desc.params[k].min && R.anlg[k] <= vb_anlg.desc.params[k].max, "%s: ANLG %d = %u",
                      vb_lib_src_name(i), k, R.anlg[k]);
        pk = R.src == VB_SRC_DRUM ? 0 : play(1, 1400);
        if (R.src == VB_SRC_DRUM) {                       /* the kit: each voice */
            int32_t out[32];
            uint32_t b;
            vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
            for (k = 0; k < 8; k++) {
                vb_player_note(&P, 48 + k, 110, 1);
                for (b = 0; b < 200u; b++) {
                    int j;
                    vb_player_render(&P, out, 32);
                    for (j = 0; j < 32; j++)
                        pk = out[j] > pk ? out[j] : pk;
                }
            }
        }
        CHECK(pk > 3000, "%s sounds (peak %d)", vb_lib_src_name(i), pk);
        CHECK(!strcmp(vb_lib_src_names[i + 1], vb_lib_src_name(i)), "list name %d", i);
        printf("source %-10s %-4s peak %5d\n", vb_lib_src_name(i), vb_src_name[R.src], pk);
    }
    for (i = 0; i < vb_lib_nfx(); i++) {
        int32_t pk;
        vb_recipe_default(&R, 1234);
        R.src = VB_SRC_ANLG;
        vb_lib_fx(&R, i);
        pk = play(3, 2800);
        CHECK(pk > 3000 && pk <= 32767, "FX %s passes sound (peak %d)", vb_lib_fx_name(i), pk);
        printf("fx     %-10s peak %5d\n", vb_lib_fx_name(i), pk);
    }
    printf(failures ? "\n%d FAILED\n" : "\nall library tests passed\n", failures);
    return failures != 0;
}
