/* SPDX-License-Identifier: GPL-3.0-only */
/* ARP: the chord playing (or the keys held) in the pattern, over the octaves, at the rate; every note out (MIDI). */
#include <stdio.h>
#include <string.h>
#include "../core/vb_player.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int16_t fxbuf[VB_FX_SLOTS * VB_FX_BUF], pa[36864], pb[36864];
static vb_recipe_t R;
static vb_player_t P;
static uint8_t got[256];
static uint32_t ngot, nout_on, nout_off;
static void out(uint8_t ch, uint8_t note, uint8_t vel, uint8_t on)
{
    (void)ch;
    (void)vel;
    if (on && ngot < 256u)
        got[ngot++] = note;
    nout_on += on;
    nout_off += !on;
}
static void run(uint32_t blocks)
{
    int32_t o[32];
    while (blocks--)
        vb_player_render(&P, o, 32);
}
static void setup(int pat, int oct, int rate)
{
    vb_recipe_default(&R, 99);
    R.src = VB_SRC_ANLG;
    R.play.mode = VB_PLAY_ARP;
    R.play.len = 1;
    R.play.chord[0].voi = 0;
    vb_chord_fill(&R.play.chord[0], 0, 0, 48);              /* C E G from C3 */
    R.play.chord[0].len_i = 11;
    R.arp_pat = (uint8_t)pat;
    R.arp_oct = (uint8_t)(oct - 1);
    R.arp_rate = (uint8_t)rate;
    vb_player_init(&P, &R, fxbuf, pa, pb, 36864);
    P.note_out = out;
    ngot = nout_on = nout_off = 0;
}

int main(void)
{
    static const uint8_t UP2[6] = {48, 52, 55, 60, 64, 67};
    uint32_t i;
    setup(VB_ARP_UP, 2, 0);
    vb_player_play(&P, 1);
    run(1378 * 2);                                          /* 2 s at 120 BPM: 16 sixteenths */
    CHECK(ngot >= 15 && ngot <= 17, "1/16: %u notes in 2 s", ngot);
    for (i = 0; i < ngot; i++)
        CHECK(got[i] == UP2[i % 6u], "UP over 2 octaves: note %u is %u (want %u)", i, got[i], UP2[i % 6u]);
    CHECK(nout_off + 1u >= nout_on, "every note ends (on %u off %u)", nout_on, nout_off);

    setup(VB_ARP_DOWN, 1, 1);
    vb_player_play(&P, 1);
    run(1378 * 2);
    CHECK(ngot >= 7 && ngot <= 9, "1/8: %u notes in 2 s", ngot);
    CHECK(got[0] == 55 && got[1] == 52 && got[2] == 48 && got[3] == 55, "DOWN: %u %u %u %u", got[0], got[1], got[2], got[3]);

    setup(VB_ARP_UPDN, 1, 0);
    vb_player_play(&P, 1);
    run(1378);
    CHECK(got[0] == 48 && got[1] == 52 && got[2] == 55 && got[3] == 52 && got[4] == 48 && got[5] == 52, "UP-DN: %u %u %u %u %u",
          got[0], got[1], got[2], got[3], got[4]);

    setup(VB_ARP_UP, 1, 3);                                 /* 1/32 */
    vb_player_play(&P, 1);
    run(1378 * 2);
    CHECK(ngot >= 30 && ngot <= 34, "1/32: %u notes in 2 s", ngot);

    setup(VB_ARP_UP, 1, 0);                                 /* stopped: the keys held run it */
    vb_player_note(&P, 62, 100, 1);
    vb_player_note(&P, 69, 100, 1);
    run(1378);
    CHECK(ngot >= 7 && got[0] == 62 && got[1] == 69 && got[2] == 62, "held D4 A4, stopped: %u notes: %u %u %u", ngot, got[0],
          got[1], got[2]);
    vb_player_note(&P, 62, 0, 0);
    vb_player_note(&P, 69, 0, 0);
    i = ngot;
    run(1378);
    CHECK(ngot == i, "keys up: the arp stops (%u more)", ngot - i);

    setup(VB_ARP_RND, 3, 0);
    vb_player_play(&P, 1);
    run(1378 * 4);
    for (i = 0; i < ngot; i++)
        CHECK((got[i] % 12 == 0 || got[i] % 12 == 4 || got[i] % 12 == 7) && got[i] >= 48 && got[i] <= 79, "RND in the chord: %u",
              got[i]);
    printf(failures ? "\n%d FAILED\n" : "\nall arp tests passed\n", failures);
    return failures != 0;
}
