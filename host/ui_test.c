/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware's UI on the host with a flash in RAM (host/vb_host.h): chord steps INS / DUP / DEL, OCT on PLAY,
 * VOICING and BASS, songs saved and loaded, and everything back after a restart on the same flash.
 *   make uitest */
#include "vb_host.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void pass(uint32_t ms)                              /* the main loop for ms milliseconds */
{
    uint32_t end = fm1_ms + ms;
    while (fm1_ms < end) {
        fm1_ms += 20;
        vb_input();
        vb_draw();
    }
}
static void press_b(uint32_t b) { host_btn |= 1u << panel.btn[b]; pass(40); }
static void sel(void)                                      /* a SEL tap: press, release */
{
    host_btn |= 1u << panel.btn[B_SCL];
    pass(40);
    host_rel |= 1u << panel.btn[B_SCL];
    pass(40);
}
static void keyset(uint32_t n, int on)                    /* a key down / up, and a block of audio (it takes the notes) */
{
    int32_t o[32];
    fm1_in.notes = on ? fm1_in.notes | 1u << n : fm1_in.notes & ~(1u << n);
    pass(40);
    vb_player_render(&vb_pl, o, 32);
}
static void turn(uint32_t e, int d) { host_enc[panel.enc[e]] += d * panel.dir[e]; pass(40); }
static void boot(int fresh)
{
    if (fresh)
        memset(host_flash, 0xFF, sizeof host_flash);
    memset(&ui, 0, sizeof ui);
    memset(vb_dirty_snd, 0, sizeof vb_dirty_snd);
    memset(&vb_st, 0, sizeof vb_st);
    vb_ngen = vb_curgen = 0;
    {
        uint32_t t;
        for (t = 0; t < VB_TRACKS; t++)
            vb_track_sound[t] = (uint8_t)t;
    }
    panel = PANEL_DEFAULT;
    vb_theme();
    vb_init();
    pass(100);
}

int main(void)
{
    vb_play_t *pl;
    vb_chord_t c4;
    uint8_t n0;
    boot(1);
    press_b(B_ARP);                                        /* PLAY, CHORD (the default sound's mode) */
    pl = &RC->play;
    CHECK(ui.page == PG_PLAY && pl->mode == VB_PLAY_CHORD, "PLAY / CHORD (page %u mode %u)", ui.page, pl->mode);
    pl->len = 6;                                           /* room to insert */
    pl->sel = 2;
    n0 = pl->len;
    c4 = pl->chord[2];
    ui.cact = 1;                                           /* DUP */
    sel();
    CHECK(pl->len == n0 + 1 && pl->sel == 3 && !memcmp(&pl->chord[3], &c4, sizeof c4), "DUP: len %u sel %u", pl->len, pl->sel);
    ui.cact = 2;                                           /* DEL */
    sel();
    CHECK(pl->len == n0 && !memcmp(&pl->chord[2], &c4, sizeof c4), "DEL: len %u", pl->len);
    pl->sel = 5;
    sel();
    CHECK(pl->len == 5 && pl->sel == 4, "DEL the last: len %u sel %u", pl->len, pl->sel);
    ui.cact = 0;                                           /* INS: an empty step after the selected */
    pl->sel = 0;
    sel();
    CHECK(pl->len == 6 && pl->sel == 1 && pl->chord[1].n == 0, "INS: len %u sel %u n %u", pl->len, pl->sel, pl->chord[1].n);
    while (pl->len < VB_CHORD_STEPS)
        sel();
    CHECK(pl->len == 16, "INS to 16: %u", pl->len);
    sel();
    CHECK(pl->len == 16 && !strcmp(ui.msg, "16 CHORDS: FULL"), "INS when full: %u %s", pl->len, ui.msg);
    pl->len = 8;

    pl->sel = 0;                                           /* OCT on PLAY moves the chord, not the keys */
    {
        uint8_t a = pl->chord[0].note[0], oct = ui.oct;
        press_b(B_OCTUP);
        CHECK(pl->chord[0].note[0] == a + 12 && ui.oct == oct, "OCT+: %u -> %u, keys oct %u", a, pl->chord[0].note[0], ui.oct);
        press_b(B_OCTDN);
        CHECK(pl->chord[0].note[0] == a, "OCT-: back to %u (%u)", a, pl->chord[0].note[0]);
    }
    {   /* VOICING and BASS on row 3 */
        char b[24];
        ui.row[PG_PLAY] = 2;
        int k;
        for (k = 0; k < 4; k++)
            turn(EN_K1, 1);                                /* VOICING: CLOSE OPEN DROP2 SPREAD VEFF */
        for (k = 0; k < 2; k++)
            turn(EN_K2, 1);                                /* BASS: the chord's 2nd tone */
        CHECK(pl->chord[0].voi == VB_VOI_VEFF, "VOICING %u", pl->chord[0].voi);
        CHECK(pl->chord[0].bass == 2 && vb_chord_bass(&pl->chord[0]) == 36 + 3, "BASS %u -> %d (D# under Cm7)",
              pl->chord[0].bass, vb_chord_bass(&pl->chord[0]));
        for (k = 0; k < 20; k++)
            turn(EN_K2, 1);
        CHECK(pl->chord[0].bass <= 8, "BASS stops at the chord's tones (%u)", pl->chord[0].bass);
        for (k = 0; k < 20; k++)
            turn(EN_K2, -1);
        CHECK(pl->chord[0].bass == 0, "BASS off (%u)", pl->chord[0].bass);
        for (k = 0; k < 2; k++)
            turn(EN_K2, 1);
        vb_chord_name(&pl->chord[0], b, sizeof b);
        printf("step 1 in VEFF: %s, %u notes\n", b, pl->chord[0].n);
    }
    c4 = pl->chord[0];
    n0 = pl->len;

    /* a song: the tracks' sounds, BPM; save, change, load */
    press_b(B_SAVE);
    press_b(B_SAVE);                                       /* the second press: the SONG row */
    CHECK(ui.page == PG_BANK && ui.row[PG_BANK] == 1, "SONG row (%u %u)", ui.page, ui.row[PG_BANK]);
    turn(EN_SELECT, 1);
    turn(EN_SELECT, 1);
    CHECK(ui.song == 2, "SELECT picks song %u", ui.song);
    ui.song_do = 1;                                        /* SAVE */
    sel();
    CHECK(!strcmp(ui.msg, "SONG SAVED"), "save: %s", ui.msg);
    {
        uint8_t was = vb_track_sound[0];
        turn(EN_PRESET, 1);
        vb_pl.bpm10 = 900;
        CHECK(vb_track_sound[0] != was, "PRESETS moved track 1");
        press_b(B_SAVE);                                   /* (PRESETS reloaded the page: back to the SONG row) */
        ui.row[PG_BANK] = 1;
        ui.song_do = 0;                                    /* LOAD */
        sel();
        CHECK(!strcmp(ui.msg, "SONG LOADED"), "load: %s", ui.msg);
        CHECK(vb_track_sound[0] == was && vb_pl.bpm10 == 1200, "the song came back (sound %u bpm %d)", vb_track_sound[0], vb_pl.bpm10);
        CHECK(!memcmp(&RC->play.chord[0], &c4, sizeof c4) && RC->play.len == n0, "its part (len %u)", RC->play.len);
    }

    /* the part saves itself; a restart on the same flash keeps it, and the song */
    pass(3000);
    CHECK(!vb_st.dirty, "autosave");
    boot(0);
    CHECK(!memcmp(&RC->play.chord[0], &c4, sizeof c4), "the chord after a restart");
    CHECK(song_read(2, &vb_song) == 0 && vb_song.snd[0] == vb_track_sound[0], "song 3 after a restart");
    CHECK(song_read(1, &vb_song) != 0, "song 2 is empty");

    /* COPY: the sound into a free slot, the track onto it */
    {
        uint8_t was = vb_track_sound[0];
        vb_recipe_t orig = *RC;
        press_b(B_SAVE);
        ui.row[PG_BANK] = 0;
        ui.bank_do = 3;
        sel();
        CHECK(vb_track_sound[0] != was && !memcmp(RC, &orig, sizeof orig), "COPY: track 1 on %u (was %u), same recipe",
              vb_track_sound[0], was);
        CHECK(!memcmp(snd(was), &orig, sizeof orig), "COPY: the original stays");
        ui.bank_do = 0;
    }

    /* MOD: SEL on the shape row rolls the LFO (and gives it a destination); on a destination row it learns */
    {
        vb_modsrc_t before;
        press_b(B_LFO);
        ui.op = 3;
        before = RC->mod[3];
        ui.row[PG_MOD] = 0;
        sel();
        CHECK(memcmp(&RC->mod[3], &before, sizeof before) != 0 && ui.learn_src < 0, "RANDOM LFO 4");
        CHECK(RC->mod[3].r[0].dest != VB_ADDR_NONE || RC->mod[3].r[1].dest != VB_ADDR_NONE, "a destination");
        ui.row[PG_MOD] = 1;
        sel();
        CHECK(ui.learn_src == 3, "LEARN on the destination row (%d)", ui.learn_src);
        sel();
    }

    /* edits on another track's sound are saved too (not only the one on screen) */
    turn(EN_ALGO, 1);
    CHECK(ui.track == 1, "track 2");
    RC->play.vel = 33;
    touch();
    turn(EN_ALGO, -1);
    pass(3000);
    boot(0);
    CHECK(snd(vb_track_sound[1])->play.vel == 33, "track 2's edit after a restart (%u)", snd(vb_track_sound[1])->play.vel);

    /* RAM holds a few bank sounds (a cache): an edit survives browsing past 20 others, which reuse its slot */
    {
        uint8_t s0 = vb_track_sound[ui.track];
        int k;
        RC->play.vel = 77;
        touch();
        for (k = 0; k < 20; k++)
            turn(EN_PRESET, 1);
        CHECK(vb_track_sound[ui.track] == s0 + 20, "PRESETS +20 (%u)", vb_track_sound[ui.track]);
        for (k = 0; k < 20; k++)
            turn(EN_PRESET, -1);
        CHECK(vb_track_sound[ui.track] == s0 && RC->play.vel == 77, "back on sound %u: VEL %u", vb_track_sound[ui.track],
              RC->play.vel);
        RC->play.vel = 100;
        touch();
    }

    /* CHORD INPUT DEGREE: the keys are a chord pad; on PLAY, stopped, each chord also fills the selected step */
    {
        vb_play_t *p2;
        char b[24];
        press_b(B_ARP);
        if (vb_pl.playing)
            press_b(B_PLAY);
        p2 = &RC->play;
        p2->mode = VB_PLAY_CHORD;
        RC->cin = 1;
        RC->key = 0;
        RC->scale = VB_SC_MAJ;
        p2->len = 4;
        p2->sel = 0;
        for (int k = 0; k < 4; k++)
            p2->chord[k].voi = VB_VOI_CLOSE;
        ui.oct = 2;
        pass(40);
        keyset(0, 1);                                      /* F3 held: 7 */
        keyset(7, 1);                                      /* C4: I */
        keyset(7, 0);
        keyset(0, 0);
        vb_chord_roman(&p2->chord[0], 0, VB_SC_MAJ, b, sizeof b);
        CHECK(!strcmp(b, "Imaj7") && p2->sel == 1, "pad C4 + 7: %s, next step %u", b, p2->sel);
        keyset(16, 1);                                     /* A4: vi */
        keyset(16, 0);
        vb_chord_roman(&p2->chord[1], 0, VB_SC_MAJ, b, sizeof b);
        CHECK(!strcmp(b, "VIm"), "pad A4: %s", b);
        keyset(9, 1);                                      /* D4, then SWAP while it is held: II */
        keyset(2, 1);
        keyset(2, 0);
        keyset(9, 0);
        vb_chord_roman(&p2->chord[2], 0, VB_SC_MAJ, b, sizeof b);
        CHECK(!strcmp(b, "II") && p2->sel == 3, "pad D4 + SWAP after: %s (one step, sel %u)", b, p2->sel);
        keyset(17, 1);                                     /* A#4: bVII */
        keyset(17, 0);
        vb_chord_roman(&p2->chord[3], 0, VB_SC_MAJ, b, sizeof b);
        CHECK(!strcmp(b, "bVII") && p2->sel == 0, "pad A#4: %s, wraps to step %u", b, p2->sel);
        RC->key = 9;
        RC->scale = VB_SC_MIN;
        press_b(B_PLAY);                                   /* playing: the pad plays, the steps stay */
        {
            int32_t o[32];
            vb_player_render(&vb_pl, o, 32);                   /* (the audio side takes the command) */
        }
        CHECK(vb_pl.playing, "playing");
        keyset(14, 1);                                     /* G4: v of A minor */
        keyset(14, 0);
        vb_chord_roman(&p2->chord[0], 0, VB_SC_MAJ, b, sizeof b);
        CHECK(!strcmp(b, "Imaj7"), "playing: step 1 kept (%s)", b);
        press_b(B_PLAY);
        {
            int32_t o[32];
            vb_player_render(&vb_pl, o, 32);
        }
        RC->cin = 0;
    }

    /* four tracks at once: PLAY starts all; the selected one holds the FX buffers and the print; MUTE on GLO */
    {
        int32_t out[2 * CTL];
        uint32_t k, b;
        boot(1);
        for (k = 0; k < VB_TRACKS; k++)
            vb_P[k]->peak = 0;
        snd(vb_track_sound[0])->fx[0].type = VB_FX_REVERB;  /* T1 with a reverb, which needs a buffer */
        snd(vb_track_sound[0])->fx[0].on = 1;
        press_b(B_PLAY);
        for (b = 0; b < 2000u; b++)                        /* 1.5 s */
            vb_audio_block(out, CTL);
        for (k = 0; k < VB_TRACKS; k++)
            CHECK(vb_P[k]->playing, "track %u plays", k + 1);
        CHECK(vb_P[1]->peak > 1000 && vb_P[2]->peak > 1000, "T2 and T3 sound in the background (%d %d)", vb_P[1]->peak,
              vb_P[2]->peak);
        CHECK(vb_P[0]->fx[0].buf && !vb_P[1]->fx[0].buf && vb_P[0]->pbuf[0] && !vb_P[1]->pbuf[0], "T1 holds the buffers");
        vb_sel_to = 0xFFu;                                 /* the audio thread mid-move: no player holds the buffers */
        for (b = 0; b < 4u; b++)
            vb_audio_block(out, CTL);
        CHECK(vb_sel == 0xFFu && !vb_P[0]->fx[0].buf, "mid-move: the buffers held by none");
        vb_sel_to = 0;
        for (b = 0; b < 4u; b++)
            vb_audio_block(out, CTL);
        CHECK(vb_sel == 0 && vb_P[0]->fx[0].buf, "back on T1");
        turn(EN_ALGO, 1);                                  /* track 2 selected: the buffers move */
        for (b = 0; b < 8u; b++)
            vb_audio_block(out, CTL);
        CHECK(ui.track == 1 && vb_P[1]->fx[0].buf && !vb_P[0]->fx[0].buf && vb_P[1]->pbuf[0] && !vb_P[0]->pbuf[0],
              "T2 holds the buffers now");
        vb_P[0]->peak = 0;
        for (b = 0; b < 600u; b++)
            vb_audio_block(out, CTL);
        CHECK(vb_P[0]->playing && vb_P[0]->peak > 1000, "T1 plays on in the background, its reverb bypassed (%d)",
              vb_P[0]->peak);
        press_b(B_GLO);                                    /* GLO, row 2: mute T3 */
        ui.row[PG_GLO] = 1;
        turn(EN_K3, 1);
        CHECK(vb_tmute[2] == 1, "T3 muted");
        press_b(B_PLAY);
        for (b = 0; b < 8u; b++)
            vb_audio_block(out, CTL);
        CHECK(!vb_P[0]->playing && !vb_P[3]->playing, "STOP stops all");
        pass(3000);
        boot(0);
        CHECK(vb_tmute[2] == 1 && ui.track == 1 && vb_P[1]->fx[0].buf, "after a restart: T3 muted, T2 selected with the buffers");
        vb_tmute[2] = 0;
    }

    /* CHORDS on GLO: every track in CHORD plays T1's progression, in step; its PLAY page edits T1's */
    {
        int32_t out[2 * CTL];
        uint32_t b;
        vb_recipe_t *r1, *r4;
        boot(1);
        r1 = snd(vb_track_sound[0]);
        r4 = snd(vb_track_sound[3]);
        r4->play.mode = VB_PLAY_CHORD;
        r4->play.len = 2;                                  /* its own: two steps of A minor */
        vb_chord_fill(&r4->play.chord[0], 9, 1, 48);
        vb_chord_fill(&r4->play.chord[1], 9, 1, 48);
        press_b(B_GLO);
        ui.row[PG_GLO] = 0;
        turn(EN_K3, 1);                                    /* CHORDS: T1 */
        CHECK(vb_hsrc == 1 && vb_P[3]->harm == &r1->play && !vb_P[0]->harm, "CHORDS T1 (%u)", vb_hsrc);
        turn(EN_ALGO, 1);
        turn(EN_ALGO, 1);
        turn(EN_ALGO, 1);
        CHECK(ui.track == 3 && PLC() == &r1->play, "T4's PLAY page edits T1's chords");
        press_b(B_PLAY);
        for (b = 0; b < 14000u; b++) {                      /* 10 s: T4 on T1's steps, past its own 2 */
            vb_audio_block(out, CTL);
            if (vb_P[3]->playing)
                CHECK(vb_P[3]->step == vb_P[0]->step, "in step (%u %u)", vb_P[3]->step, vb_P[0]->step);
            if (vb_P[3]->step != vb_P[0]->step)
                break;
        }
        CHECK(vb_P[3]->step >= 2, "T4 got past its own 2 steps (step %u)", vb_P[3]->step);
        press_b(B_PLAY);
        press_b(B_GLO);
        ui.row[PG_GLO] = 0;
        turn(EN_K3, -1);
        CHECK(vb_hsrc == 0 && !vb_P[3]->harm && PLC() == &r4->play, "CHORDS OWN");
        pass(3000);
    }

    /* LIB on SRC and FX: turning applies the next library entry; hold SEL brings back what was there before browsing */
    {
        vb_recipe_t before;
        boot(1);
        press_b(B_EDIT);
        before = *RC;
        build_rows();
        ui.row[PG_SRC] = (uint8_t)(NR - 1u);
        turn(EN_K1, 1);
        CHECK(ui.lib_src == 1 && RC->src == VB_SRC_OP4 && RC->op4[37] == 50, "LIB 1: LATEX (src %u fb %u)", RC->src, RC->op4[37]);
        turn(EN_K1, 1);
        turn(EN_K1, 1);
        turn(EN_K1, 1);
        turn(EN_K1, 1);
        turn(EN_K1, 1);
        turn(EN_K1, 1);
        CHECK(ui.lib_src == 7 && RC->src == VB_SRC_ANLG, "LIB 7: SUPERSAW (src %u)", RC->src);
        undo();
        CHECK(!memcmp(RC, &before, sizeof before), "hold SEL: the sound from before browsing");
        press_b(B_FX);
        ui.row[PG_FX] = 2;
        turn(EN_K1, 1);
        CHECK(RC->fx[0].type == VB_FX_DRIVE && RC->fx[3].type == VB_FX_REVERB && RC->fx[4].type == 0, "LIB FX: HYPER");
        pass(3000);
    }

    /* ARP from the keys, its notes out over MIDI (GLO MIDI: NOTES), on the track's channel */
    {
        int32_t out[2 * CTL];
        uint32_t b, k, on60 = 0, on64 = 0, other = 0;
        boot(1);
        press_b(B_GLO);
        ui.row[PG_GLO] = 2;
        turn(EN_K1, 1);
        CHECK(vb_nout == 1 && vb_P[0]->note_out, "MIDI: NOTES");
        RC->play.mode = VB_PLAY_ARP;
        RC->cin = 0;
        host_nmout = 0;
        keyset(7, 1);                                      /* C4 and E4 held */
        keyset(11, 1);
        for (b = 0; b < 1378u; b++)                        /* 1 s */
            vb_audio_block(out, CTL);
        keyset(7, 0);
        keyset(11, 0);
        for (k = 0; k < host_nmout; k++) {
            uint32_t m = host_mout[k];
            if ((m & 0xFFFFu) == (0x09u | 0x90u << 8))
                *(((m >> 16) & 0x7Fu) == 60u ? &on60 : ((m >> 16) & 0x7Fu) == 64u ? &on64 : &other) += 1u;
        }
        CHECK(on60 >= 3 && on64 >= 3 && !other, "ARP C4 E4 out on channel 1: %u x C4, %u x E4, %u others", on60, on64, other);
        vb_nout = 0;
        nout_bind();
        RC->play.mode = VB_PLAY_CHORD;
        pass(3000);
    }

    /* SYNC OUT: START, 24 ticks a quarter at the tempo; SYNC IN: a clock at 125 BPM is followed, tempo and phase */
    {
        int32_t out[2 * CTL];
        uint32_t b, k, ticks = 0, starts = 0;
        boot(1);
        vb_sync = 1;
        host_nmout = 0;
        press_b(B_PLAY);
        for (b = 0; b < 2756u; b++)                        /* 2 s at 120 BPM: 4 beats, 96 ticks */
            vb_audio_block(out, CTL);
        for (k = 0; k < host_nmout; k++) {
            ticks += host_mout[k] == (0x0Fu | 0xF8u << 8);
            starts += host_mout[k] == (0x0Fu | 0xFAu << 8);
        }
        CHECK(starts == 1 && ticks >= 95 && ticks <= 97, "clock out: START %u, %u ticks in 2 s", starts, ticks);
        press_b(B_PLAY);
        for (b = 0; b < 4u; b++)
            vb_audio_block(out, CTL);
        CHECK(host_mout[host_nmout - 1] == (0x0Fu | 0xFCu << 8) || host_mout[host_nmout - 2] == (0x0Fu | 0xFCu << 8),
              "STOP sent");

        vb_sync = 2;                                       /* in: 125 BPM, the ticks 20 ms apart */
        {
            uint32_t smp = 0, next_tick_us = 0, nt = 0, worst = 0;
            int started = 0;
            fm1_ms = 100000;
            for (b = 0; b < 27563u; b++) {                 /* 20 s */
                uint32_t us = (uint32_t)((uint64_t)smp * 1000000u / 44100u);
                fm1_ms = 100000u + us / 1000u;
                while (us >= next_tick_us) {
                    uint32_t pkt = !started ? 0x0Fu | 0xFAu << 8 : 0x0Fu | 0xF8u << 8;
                    midi_in_q[mi_w % MQ] = pkt;
                    midi_in_ms[mi_w % MQ] = 100000u + next_tick_us / 1000u;
                    mi_w++;
                    if (started) {
                        nt++;
                        next_tick_us += 20000u;
                    } else
                        started = 1;
                }
                vb_midi_in();
                {   /* the commands reach the players with the next block */
                    vb_audio_block(out, CTL);
                }
                smp += CTL;
                if (b > 22050u && nt % 6u == 1u && vb_last16 == (nt - 1u) / 6u + 1u) {
                    uint32_t e = vb_ms16 > (100000u + (nt - 1u) * 20u) ? vb_ms16 - (100000u + (nt - 1u) * 20u)
                                                                       : (100000u + (nt - 1u) * 20u) - vb_ms16;
                    worst = e > worst ? e : worst;
                }
            }
            CHECK(vb_pl.playing && vb_pl.bpm10 >= 1245 && vb_pl.bpm10 <= 1255, "clock in: playing at %d.%d BPM", vb_pl.bpm10 / 10,
                  vb_pl.bpm10 % 10);
            CHECK(vb_last16 + 1u >= (nt - 1u) / 6u + 1u && vb_last16 <= (nt - 1u) / 6u + 2u,
                  "clock in: in phase after 20 s (ours %u, the clock's %u)", vb_last16, (nt - 1u) / 6u + 1u);
            CHECK(worst <= 3u, "clock in: the sixteenths within %u ms of the clock's (16 s on)", worst);
            printf("clock in: %d.%d BPM, worst phase %u ms\n", vb_pl.bpm10 / 10, vb_pl.bpm10 % 10, worst);
        }
        vb_sync = 0;
        vb_player_play(&vb_pl, 0);
    }

    printf(failures ? "\n%d FAILED\n" : "\nall ui tests passed\n", failures);
    return failures != 0;
}
