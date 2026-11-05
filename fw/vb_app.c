/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox application: the pages of the mockup (ui/index.html) on the FM-1.
 *
 *   EDIT  SRC: the source (OP4 ANLG DRUM SAMP GRAN CHOP)        SEL = RANDOMIZE, hold SEL = undo
 *   ARP   PLAY row 1 (mode, strum)   SEQ  PLAY step rows          keys enter chord notes / step-record
 *   LFO   MOD L1..L6   ENV  MOD E1..E3                            SEL = LEARN a destination (then turn any knob)
 *   FX    the 8-slot chain                                        SEL = SCRAMBLE, hold SEL = undo
 *   HOME  PRINT settings   REC  PRINT from any page               SAVE BANK (generations)   GLO tempo, CHAOS
 *   SELECT the engine on SRC (the keys pick the DRUM voice; OP4's operator is the OP knob), the item on the other
 *   pages, ALGORITHM the track, PRESETS the bank sound, OCT- / OCT+ the keys' octave,
 *   OCT- + OCT+ held 5 s: UBOOT.
 * Pressing the page's button again steps through its rows; KNOB 1-4 edit the lit row. */

static void vb_midi_in(void);
static void load_gens(uint32_t sound);
static void undo_mark(void);
static void vb_draw_now(void);
/* ---- panel: which matrix button / encoder carries which label (MNM-1's panel.c defaults) */
enum { B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC, B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };
#define PANEL_MAGIC 0x50414E35u
typedef struct {
    uint32_t magic;
    uint8_t btn[NB];
    uint8_t enc[NE];
    int8_t dir[NE];
} panel_t;
panel_t panel __attribute__((section(".noinit")));
static const panel_t PANEL_DEFAULT = {
    PANEL_MAGIC, {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1}, {0, 1, 6, 2, 3, 4, 5}, {1, 1, 1, 1, 1, 1, 1},
};
static void panel_init(void)
{
    uint32_t i, buttons = 0, encoders = 0, ok = panel.magic == PANEL_MAGIC;
    for (i = 0; ok && i < NB; i++) {
        ok = panel.btn[i] < 14u && !(buttons & (1u << panel.btn[i]));
        buttons |= ok ? 1u << panel.btn[i] : 0u;
    }
    for (i = 0; ok && i < NE; i++) {
        ok = panel.enc[i] < 7u && !(encoders & (1u << panel.enc[i])) && (panel.dir[i] == 1 || panel.dir[i] == -1);
        encoders |= ok ? 1u << panel.enc[i] : 0u;
    }
    if (!ok)
        panel = PANEL_DEFAULT;
}
static uint32_t panel_btn_of(uint32_t matrix_id)
{
    uint32_t b;
    for (b = 0; b < NB; b++)
        if (panel.btn[b] == matrix_id)
            return b;
    return NB;
}
static void panel_led(uint32_t label, int on) { fm1_led_key(panel.btn[label], on); }
static int8_t vb_rem_enc[NE];                         /* turns sent over USB (vb_remote) */
static int32_t panel_enc(uint32_t role)
{
    int32_t d = enc_take(panel.enc[role]) * panel.dir[role] + vb_rem_enc[role];
    vb_rem_enc[role] = 0;
    return d;
}

/* ---- memory: the FX buffers and one print in the pool, the other print in .bss */
#define VB_PRINT_CAP 36864u                                 /* 1.67 s at 22.05 kHz */
static int16_t vb_fxbuf[VB_FX_SLOTS * VB_FX_BUF] __attribute__((section(".pool")));
static int16_t vb_print_a[VB_PRINT_CAP] __attribute__((section(".pool")));
static int16_t vb_print_b[VB_PRINT_CAP];
#define VB_BANK 64
#define VB_GENS VB_MAXGEN
#define VB_TRACKS 4
/* the bank's 64 sounds live in flash; RAM holds the ones in use (snd()): the 4 tracks' and the latest others */
#define VB_SLOTS 8
static vb_recipe_t vb_slot[VB_SLOTS] __attribute__((section(".pool")));
static uint8_t vb_slot_snd[VB_SLOTS];                       /* the sound in each slot, 0xFF: none */
static uint32_t vb_slot_used[VB_SLOTS], vb_slot_clock;
static vb_gen_t vb_gen[VB_GENS] __attribute__((section(".pool")));
static uint8_t vb_ngen, vb_curgen;
static vb_recipe_t vb_undo_rc __attribute__((section(".pool")));
/* a player per track, all sounding at once. The selected one holds the FX buffers and the prints (it moves them on a
 * track change, in the audio thread); the others play their synths and their FX that need no buffer */
static vb_player_t vb_p0;                                  /* track 1 in .bss, the others in the pool */
static vb_player_t vb_pbg[VB_TRACKS - 1] __attribute__((section(".pool")));
static vb_player_t *const vb_P[VB_TRACKS] = {&vb_p0, &vb_pbg[0], &vb_pbg[1], &vb_pbg[2]};
static volatile uint8_t vb_sel, vb_sel_to;                 /* the player holding the buffers (0xFF: none), the one asked to */
static vb_bufs_t vb_bufs;                                  /* the buffers while no player holds them */
static volatile uint8_t vb_lead;                           /* the last track that held them (always 0..3): the tempo */
static uint8_t vb_tmute[VB_TRACKS];                        /* GLO: 1 = the track is muted */
static uint8_t vb_bound[VB_TRACKS];
static uint8_t vb_sync;                                    /* GLO SYNC: 0 INT, 1 OUT (sends MIDI clock), 2 IN (follows one) */
static volatile uint8_t vb_clk_cmd;                        /* main loop -> audio: 1 send START, 2 send STOP */
static uint32_t vb_clk_acc;                                /* Q8 samples toward the next clock tick out */
static volatile uint32_t vb_last16, vb_ms16;               /* the lead's last sixteenth and when it began (ms) */
static volatile int32_t vb_nudge;                          /* clock in: samples (Q8) to move every track by */
static uint8_t vb_hsrc;
static uint8_t vb_knext[VB_TRACKS];                       /* PLAY KEYS: 0 PLAY (the notes), 1 NEXT (any key, the next step) */
static uint8_t vb_nout;                                    /* GLO MIDI OUT: every track's notes out on channel 1..4 */
static void note_out(uint8_t ch, uint8_t note, uint8_t vel, uint8_t on)   /* (the audio thread) */
{
    midi_out_event((on ? 0x09u : 0x08u) | (uint32_t)((on ? 0x90u : 0x80u) | ch) << 8 | (uint32_t)note << 16 |
                   (uint32_t)vel << 24);
}
static void nout_bind(void)
{
    uint32_t k;
    for (k = 0; k < VB_TRACKS; k++) {
        vb_P[k]->manual = vb_knext[k];
        vb_P[k]->out_ch = (uint8_t)k;
        vb_P[k]->note_out = vb_nout ? note_out : 0;
    }
}                                    /* GLO CHORDS: 0 each track its own, 1..4 every track plays T1..T4's */                        /* the sound each player was given last (0xFF: none) */
#define vb_pl (*vb_P[ui.track])                            /* the selected track's player */
#define VB_BG_POLY 5                                       /* voices for a track in the background (a 4-note chord + BASS) */
static void vb_sel_step(void)                              /* the audio thread's part of a track change */
{
    if (vb_sel_to == vb_sel)
        return;
    if (vb_sel != 0xFFu && vb_sel_to == 0xFFu) {           /* 1: out of the old player */
        vb_player_detach(vb_P[vb_sel], &vb_bufs);
        vb_P[vb_sel]->poly = VB_BG_POLY;
        vb_sel = 0xFFu;
    } else if (vb_sel == 0xFFu && vb_sel_to < VB_TRACKS) { /* 3: into the new one (cleared in between) */
        vb_player_attach(vb_P[vb_sel_to], &vb_bufs);
        vb_P[vb_sel_to]->poly = 0;
        vb_lead = vb_sel_to;
        vb_sel = vb_sel_to;
    }
}
static volatile uint32_t vb_master_q12 = 2048;

static vb_recipe_t *RC;                                    /* the sound on the selected track */
/* the chords the selected track plays (and its PLAY page edits): CHORDS on GLO may take another track's */
static vb_play_t *PLC(void)
{
    return vb_hsrc && RC->play.mode >= VB_PLAY_CHORD && vb_P[vb_hsrc - 1u]->rc ? &vb_P[vb_hsrc - 1u]->rc->play : &RC->play;
}
static void harm_bind(void)                                /* every player on the chords CHORDS asks for */
{
    uint32_t k;
    for (k = 0; k < VB_TRACKS; k++)
        vb_P[k]->harm = vb_hsrc && vb_hsrc - 1u != k ? &vb_P[vb_hsrc - 1u]->rc->play : 0;
}
static uint8_t vb_track_sound[VB_TRACKS] = {0, 1, 2, 3};

static int32_t vb_mono[CTL], vb_mix[CTL];
static volatile uint8_t vb_mute;                           /* REBUILD renders offline: the ISR stays out */
static void vb_audio_block(int32_t *out, uint32_t n)        /* audio ISR */
{
    uint32_t i, t;
    vb_sel_step();                                         /* a new track selected: the buffers move to its player */
    if (vb_mute) {
        for (i = 0; i < 2u * n; i++)
            out[i] = 0;
        return;
    }
    for (i = 0; i < n; i++)
        vb_mix[i] = 0;
    for (t = 0; t < VB_TRACKS; t++) {                       /* (a muted track runs on, unheard: it stays in time) */
        vb_P[t]->bpm10 = vb_P[vb_lead]->bpm10;
        vb_player_render(vb_P[t], vb_mono, (int)n);
        if (!vb_tmute[t])
            for (i = 0; i < n; i++)
                vb_mix[i] += vb_mono[i];
    }
    if (vb_nudge) {                                        /* clock in: the phase pulled toward the clock's */
        int32_t d = vb_nudge;
        vb_nudge = 0;
        for (t = 0; t < VB_TRACKS; t++) {
            vb_player_t *q = vb_P[t];
            q->tick_acc = d < 0 && (uint32_t)-d > q->tick_acc ? 0u : (uint32_t)((int32_t)q->tick_acc + d);
        }
    }
    if (vb_P[vb_lead]->n16 != vb_last16) {
        vb_last16 = vb_P[vb_lead]->n16;
        vb_ms16 = fm1_ms;
    }
    if (vb_sync == 1u) {                                   /* clock out: 24 a quarter, START with PLAY */
        uint32_t per = (uint32_t)(44100ull * 600u * 256u / (24u * (uint32_t)(vb_P[vb_lead]->bpm10 > 100 ? vb_P[vb_lead]->bpm10 : 1200)));
        if (vb_clk_cmd) {
            midi_out_event(0x0Fu | (vb_clk_cmd == 1u ? 0xFAu : 0xFCu) << 8);
            vb_clk_acc = 0;
            vb_clk_cmd = 0;
        }
        vb_clk_acc += n << 8;
        while (vb_clk_acc >= per) {
            vb_clk_acc -= per;
            midi_out_event(0x0Fu | 0xF8u << 8);
        }
    }
    for (i = 0; i < n; i++) {
        int32_t s = vb_mix[i] > 32767 ? 32767 : vb_mix[i] < -32768 ? -32768 : vb_mix[i];
        out[2u * i] = s;
        out[2u * i + 1u] = s;
    }
}
/* the buffers to track t's player, from the audio thread at its next block (directly when no audio runs: the host) */
static void sel_wait(uint32_t want)
{
    uint32_t i;
    for (i = 0; i < 40u && vb_sel != want; i++)
        fm1_delay_ms(1);
    if (vb_sel != want)                                    /* (no audio thread: the host) */
        vb_sel_step();
}
static void sel_player(uint32_t t)
{
    uint32_t i, k;
    if (vb_sel == t)
        return;
    vb_sel_to = 0xFFu;                                     /* 1: the audio thread takes them out */
    sel_wait(0xFFu);
    for (i = 0; i < VB_FX_SLOTS; i++)                      /* 2: cleared here, not in the audio thread */
        for (k = 0; vb_bufs.fx[i] && k < VB_FX_BUF; k++)
            vb_bufs.fx[i][k] = 0;
    vb_sel_to = (uint8_t)t;                                /* 3: and into the new player */
    sel_wait(t);
}
static void transport(int on)                              /* PLAY / STOP: every track together */
{
    uint32_t t;
    for (t = 0; t < VB_TRACKS; t++)
        vb_player_play(vb_P[t], on);
    if (vb_sync == 1u)
        vb_clk_cmd = on ? 1u : 2u;
}
static void vb_master(int32_t *out, uint32_t n)            /* the MASTER knob, for the speaker and headphones */
{
    uint32_t i;
    int32_t g = (int32_t)(vb_master_q12 >> 1);
    for (i = 0; i < 2u * n; i++)
        out[i] = (out[i] * g) >> 11;
}
static void vb_all_off(void)
{
    uint32_t t;
    for (t = 0; t < VB_TRACKS; t++)
        vb_player_panic(vb_P[t]);
}

/* ---- UI state */
enum { PG_SRC, PG_PLAY, PG_MOD, PG_FX, PG_PRINT, PG_BANK, PG_GLO, NPG };
static const char *const PG_NAME[NPG] = {"SRC", "PLAY", "MOD", "FX", "RESAMPLE", "BANK", "GLO"};
static struct {
    uint8_t page, row[NPG], track, op, opsel, dvoice, cact, sact, song, song_do, oct, chaos, bank_do;
    int8_t learn_src, learn_slot;
    uint8_t force, d_hdr, d_vis, d_prm, printing, undo_ok, lib_src, lib_fx;
    uint32_t vis_due, msg_until, sel_t0, rolls, rng;
    char msg[28];
} ui;

static void vb_theme(void)
{
    C_BG = RGB(0xe0, 0x7f, 0xb5);                          /* powder pink, saturated */
    C_INK = RGB(0x1d, 0x08, 0x19);
}
static void vb_boot_screen(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    draw_text_box(0, 96, 240, &FONT_L, "VEFFBOX", C_INK, 1);
    draw_text_box(0, 140, 240, &FONT_S, FELUCCA_VERSION, C_INK, 1);
}
static void dirty(void) { ui.d_hdr = ui.d_vis = ui.d_prm = 1; }
static void say(const char *s)
{
    str_cpy(ui.msg, s, sizeof ui.msg);
    ui.msg_until = fm1_ms + 1600u;
    ui.d_hdr = 1;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void utoa10(char *b, uint32_t v)
{
    char t[12];
    uint32_t n = 0, i = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        b[i++] = t[--n];
    b[i] = 0;
}
static void itoa_s(char *b, int v)                          /* signed, with + */
{
    if (v > 0)
        *b++ = '+';
    else if (v < 0) {
        *b++ = '-';
        v = -v;
    }
    utoa10(b, (uint32_t)v);
}
static void cat(char *d, const char *s, uint32_t max) { str_cpy(d + str_len(d), s, max - str_len(d)); }
static const char *const NOTE_NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void note_name(char *b, int n)
{
    str_cpy(b, NOTE_NM[n % 12], 4);
    utoa10(b + str_len(b), (uint32_t)(n / 12 - 1));
}

/* ---- flash: one sector per bank sound (its recipe and generations), a settings sector, the last print's audio */
#define VB_FL_SOUND(s) (0x00097000u + (uint32_t)(s) * 0x1000u)   /* 64 sectors, to 0xD7000 */
#define VB_FL_SET 0x000D7000u
#define VB_FL_AUDIO 0x000D8000u                                    /* 5 sectors: header + 4-bit ADPCM */
#define VB_AUDIO_MAGIC 0x31414256u                                 /* "VBA1" */
#define VB_SET_MAGIC 0x31534556u                                   /* "VES1" */
static uint8_t vb_sec[VB_SECTOR] __attribute__((section(".pool")));
static uint8_t vb_sec2[VB_SECTOR] __attribute__((section(".pool")));
static struct { uint8_t dirty, audio_dirty, saving; uint32_t due; } vb_st;

static int fl_put_sector(uint32_t off, const uint8_t *data)      /* erase + program, skipped when unchanged */
{
    uint32_t i;
    if (!flash_ok)
        return -1;
    if (st_read(off, vb_sec2, VB_SECTOR))
        return -2;
    for (i = 0; i < VB_SECTOR && vb_sec2[i] == data[i]; i++)
        ;
    if (i == VB_SECTOR)
        return 0;
    if (st_erase(off))
        return -3;
    return st_prog(off, data, VB_SECTOR);
}

static void save_settings(void)
{
    uint32_t i;
    for (i = 0; i < VB_SECTOR; i++)
        vb_sec[i] = 0xFFu;
    vb_sec[0] = (uint8_t)VB_SET_MAGIC; vb_sec[1] = (uint8_t)(VB_SET_MAGIC >> 8);
    vb_sec[2] = (uint8_t)(VB_SET_MAGIC >> 16); vb_sec[3] = (uint8_t)(VB_SET_MAGIC >> 24);
    for (i = 0; i < VB_TRACKS; i++)
        vb_sec[8 + i] = vb_track_sound[i];
    vb_sec[12] = ui.track;
    vb_sec[13] = ui.chaos;
    vb_sec[14] = (uint8_t)(vb_pl.bpm10 & 0xFF);
    vb_sec[15] = (uint8_t)(vb_pl.bpm10 >> 8);
    vb_sec[16] = (uint8_t)(vb_tmute[0] | vb_tmute[1] << 1 | vb_tmute[2] << 2 | vb_tmute[3] << 3 | 0xF0u);
    vb_sec[17] = vb_hsrc;
    vb_sec[18] = vb_sync;
    vb_sec[19] = vb_nout;
    for (i = 0; i < VB_TRACKS; i++)
        vb_sec[20 + i] = vb_knext[i];
    {
        uint16_t c = vb_crc16(vb_sec + 8, 24);
        vb_sec[4] = (uint8_t)c;
        vb_sec[5] = (uint8_t)(c >> 8);
    }
    fl_put_sector(VB_FL_SET, vb_sec);
}

static void save_audio(void)                                       /* the current print, as ADPCM */
{
    const int16_t *b = vb_pl.pbuf[vb_pl.cur];
    uint32_t len = vb_pl.plen[vb_pl.cur], done = 0, sec = 0, i;
    vb_adpcm_t st = {0, 0};
    uint32_t gen = vb_curgen;
    for (i = 0; i < VB_SECTOR; i++)
        vb_sec[i] = 0;
    vb_sec[0] = (uint8_t)VB_AUDIO_MAGIC; vb_sec[1] = (uint8_t)(VB_AUDIO_MAGIC >> 8);
    vb_sec[2] = (uint8_t)(VB_AUDIO_MAGIC >> 16); vb_sec[3] = (uint8_t)(VB_AUDIO_MAGIC >> 24);
    vb_sec[4] = vb_track_sound[ui.track];
    vb_sec[5] = (uint8_t)gen;
    vb_sec[6] = (uint8_t)len;
    vb_sec[7] = (uint8_t)(len >> 8);
    {   /* sector 0: 16 header bytes, then codes; the others: codes */
        uint32_t room = (VB_SECTOR - 16u) * 2u, n = len < room ? len : room;
        vb_adpcm_encode(&st, b, n, vb_sec + 16);
        done = n;
        fl_put_sector(VB_FL_AUDIO, vb_sec);
    }
    for (sec = 1; done < len && sec < 5u; sec++) {
        uint32_t n = len - done < VB_SECTOR * 2u ? len - done : VB_SECTOR * 2u;
        for (i = 0; i < VB_SECTOR; i++)
            vb_sec[i] = 0;
        vb_adpcm_encode(&st, b + done, n, vb_sec);
        done += n;
        fl_put_sector(VB_FL_AUDIO + sec * 0x1000u, vb_sec);
    }
}

static int load_audio(uint32_t sound)                              /* the stored print, if it is this sound's */
{
    uint32_t len, done, sec, n, gen;
    int16_t *dst = vb_pl.pbuf[vb_pl.cur ^ 1u];
    vb_adpcm_t st = {0, 0};
    if (!flash_ok || st_read(VB_FL_AUDIO, vb_sec, 16))            /* the header first: most often not this sound's */
        return -1;
    if (((uint32_t)vb_sec[0] | (uint32_t)vb_sec[1] << 8 | (uint32_t)vb_sec[2] << 16 | (uint32_t)vb_sec[3] << 24) !=
        VB_AUDIO_MAGIC || vb_sec[4] != sound || st_read(VB_FL_AUDIO, vb_sec, VB_SECTOR))
        return -1;
    len = (uint32_t)vb_sec[6] | (uint32_t)vb_sec[7] << 8;
    gen = vb_sec[5];
    if (len > vb_pl.pcap)
        return -1;
    n = len < (VB_SECTOR - 16u) * 2u ? len : (VB_SECTOR - 16u) * 2u;
    vb_adpcm_decode(&st, vb_sec + 16, n, dst);
    for (done = n, sec = 1; done < len && sec < 5u; sec++) {
        if (st_read(VB_FL_AUDIO + sec * 0x1000u, vb_sec, VB_SECTOR))
            return -1;
        n = len - done < VB_SECTOR * 2u ? len - done : VB_SECTOR * 2u;
        vb_adpcm_decode(&st, vb_sec, n, dst + done);
        done += n;
    }
    vb_player_load(&vb_pl, len);
    return (int)gen;
}

static void save_sound(void)
{
    int kept = vb_sound_pack(RC, vb_gen, vb_ngen, vb_curgen, vb_sec);
    if (kept < vb_ngen) {                                          /* the oldest no longer fit: forget them here too */
        uint32_t drop = vb_ngen - (uint32_t)kept, k;
        for (k = 0; k < (uint32_t)kept; k++) {
            vb_gen[k] = vb_gen[k + drop];
            vb_gen[k].parent = (int8_t)(vb_gen[k].parent >= (int)drop ? vb_gen[k].parent - (int)drop : -1);
        }
        vb_ngen = (uint8_t)kept;
        vb_curgen = (uint8_t)(vb_curgen >= drop ? vb_curgen - drop : 0u);
    }
    fl_put_sector(VB_FL_SOUND(vb_track_sound[ui.track]), vb_sec);
}

static uint32_t vb_dirty_snd[2];                                   /* bank sounds changed since the last save */
static vb_recipe_t vb_scratch __attribute__((section(".pool")));
static int snd_find(uint32_t s)
{
    int i;
    for (i = 0; i < (int)VB_SLOTS; i++)
        if (vb_slot_snd[i] == s)
            return i;
    return -1;
}
static void save_other(uint32_t s)                                 /* a sound not on screen: its stored generations kept */
{
    int cg = 0, n = -1, i = snd_find(s);
    if (i < 0)                                                     /* (not in RAM: nothing new to save) */
        return;
    if (!st_read(VB_FL_SOUND(s), vb_sec, VB_SECTOR))
        n = vb_sound_unpack(vb_sec, &vb_scratch, vb_gen, VB_GENS, &cg);
    if (vb_sound_pack(&vb_slot[i], vb_gen, n > 0 ? n : 0, cg, vb_sec) >= 0)
        fl_put_sector(VB_FL_SOUND(s), vb_sec);
}
static void vb_save_now(void)
{
    uint32_t s, cur = vb_track_sound[ui.track], others = 0;
    vb_st.saving = 1;
    save_sound();
    for (s = 0; s < VB_BANK; s++)
        if (s != cur && ((vb_dirty_snd[s >> 5] >> (s & 31u)) & 1u)) {
            save_other(s);
            others = 1;
        }
    vb_dirty_snd[0] = vb_dirty_snd[1] = 0;
    if (others)
        load_gens(cur);                                            /* (vb_gen held the others for a moment) */
    save_settings();
    if (vb_st.audio_dirty)
        save_audio();
    vb_st.audio_dirty = 0;
    vb_st.dirty = 0;
    vb_st.saving = 0;
}
static void touch(void)                                            /* something changed: save when it's quiet */
{
    uint32_t s = vb_track_sound[ui.track];
    vb_dirty_snd[s >> 5] |= 1u << (s & 31u);
    vb_st.dirty = 1;
    vb_st.due = fm1_ms + 2500u;
}
/* a new bank sound's recipe (sounds 2 and 3 start as a line and a beat) */
static void sound_default(uint32_t s, vb_recipe_t *r)
{
    uint32_t i;
    vb_recipe_default(r, 0x1000u + s * 7919u);
    if (s == 1u) {
        r->src = VB_SRC_ANLG;
        r->play.mode = VB_PLAY_SEQ;
    } else if (s == 2u) {
        r->src = VB_SRC_DRUM;
        r->play.mode = VB_PLAY_SEQ;
        for (i = 0; i < VB_SEQ_STEPS; i++) {                       /* kick, snare, hats */
            r->play.seq[i].on = 1;
            r->play.seq[i].note = (uint8_t)(48 + (i % 4 == 0 ? 0 : i % 8 == 4 ? 1 : 3));
        }
        r->play.rate = 2;
    } else if (s == 3u) {                                          /* (track 4: the keys, not a second progression) */
        r->play.mode = VB_PLAY_NOTE;
    }
}
/* bank sound s in RAM: its slot, loaded from flash (or new) into the slot no track uses for longest; a changed sound
 * is saved before its slot is reused */
static vb_recipe_t *snd(uint32_t s)
{
    int i = snd_find(s), v = -1, k;
    uint32_t t;
    if (i >= 0) {
        vb_slot_used[i] = ++vb_slot_clock;
        return &vb_slot[i];
    }
    for (k = 0; k < (int)VB_SLOTS; k++) {
        int busy = 0;
        if (vb_slot_snd[k] == 0xFFu) {
            v = k;
            break;
        }
        for (t = 0; t < VB_TRACKS; t++)
            busy |= vb_track_sound[t] == vb_slot_snd[k];
        if (!busy && (v < 0 || vb_slot_used[k] < vb_slot_used[v]))
            v = k;
    }
    if (vb_slot_snd[v] != 0xFFu && ((vb_dirty_snd[vb_slot_snd[v] >> 5] >> (vb_slot_snd[v] & 31u)) & 1u))
        vb_save_now();
    vb_slot_snd[v] = (uint8_t)s;
    vb_slot_used[v] = ++vb_slot_clock;
    {
        int cg = 0;
        if (!flash_ok || st_read(VB_FL_SOUND(s), vb_sec, VB_SECTOR) || vb_sound_unpack(vb_sec, &vb_slot[v], vb_gen, 0, &cg) < 0)
            sound_default(s, &vb_slot[v]);
    }
    return &vb_slot[v];
}

static void vb_autosave(void)
{
    if (vb_st.dirty && !vb_pl.playing && !vb_pl.rec && !ui.printing && (int32_t)(fm1_ms - vb_st.due) >= 0) {
        say("SAVING");
        vb_draw_now();
        vb_save_now();
        say("SAVED");
    }
}

/* ---- songs: which bank sound each of the 4 tracks plays, the tempo and CHAOS. The parts (chords, steps) live in the
 * sounds, so a song is small: 64 of them in one sector. Two songs with different parts of one sound: COPY it (BANK). */
#define VB_FL_SONG 0x000DD000u                                     /* 1 sector; 0xDE000..0xE0000 free */
#define VB_SONGS 64
#define VB_SONG_BYTES 64u
#define VB_SONG_MAGIC 0x32475356u                                  /* "VSG2" */
typedef struct {
    uint32_t magic;
    uint16_t crc, bpm10;
    uint8_t snd[VB_TRACKS];
    uint8_t chaos, pad[3];
} vb_song_t;
typedef char vb_song_fits[sizeof(vb_song_t) <= VB_SONG_BYTES ? 1 : -1];
static uint32_t song_off(uint32_t i) { return VB_FL_SONG + i * VB_SONG_BYTES; }
static uint16_t song_crc(const vb_song_t *g) { return vb_crc16((const uint8_t *)g + 8, sizeof *g - 8u); }
static int song_read(uint32_t i, vb_song_t *g)                     /* 0: a stored song */
{
    if (!flash_ok || i >= VB_SONGS || st_read(song_off(i), g, sizeof *g))
        return -1;
    return g->magic == VB_SONG_MAGIC && g->crc == song_crc(g) ? 0 : -1;
}
static vb_song_t vb_song;
static void load_track(uint32_t t);
static void song_save(uint32_t i)
{
    uint32_t t, k;
    const uint8_t *b = (const uint8_t *)&vb_song;
    if (vb_pl.playing || vb_pl.rec) {
        say("STOP, THEN SAVE");
        return;
    }
    vb_song.magic = VB_SONG_MAGIC;
    vb_song.bpm10 = (uint16_t)vb_pl.bpm10;
    vb_song.chaos = ui.chaos;
    vb_song.pad[0] = (uint8_t)(vb_tmute[0] | vb_tmute[1] << 1 | vb_tmute[2] << 2 | vb_tmute[3] << 3);   /* the mutes */
    vb_song.pad[1] = vb_hsrc;                                      /* CHORDS */
    vb_song.pad[2] = 0;
    for (t = 0; t < VB_TRACKS; t++)
        vb_song.snd[t] = vb_track_sound[t];
    vb_song.crc = song_crc(&vb_song);
    say("SAVING SONG");
    vb_draw_now();
    vb_save_now();                                                 /* the parts it plays, saved with it (uses vb_sec) */
    if (st_read(VB_FL_SONG, vb_sec, VB_SECTOR))
        return;
    for (k = 0; k < VB_SONG_BYTES; k++)
        vb_sec[i * VB_SONG_BYTES + k] = k < sizeof vb_song ? b[k] : 0xFFu;
    fl_put_sector(VB_FL_SONG, vb_sec);
    say("SONG SAVED");
}
static void song_load(uint32_t i)
{
    uint32_t t;
    if (song_read(i, &vb_song)) {
        say("EMPTY SONG");
        return;
    }
    for (t = 0; t < VB_TRACKS; t++) {
        vb_track_sound[t] = vb_song.snd[t] < VB_BANK ? vb_song.snd[t] : (uint8_t)t;
        vb_tmute[t] = (uint8_t)((vb_song.pad[0] >> t) & 1u);
    }
    vb_hsrc = vb_song.pad[1] <= VB_TRACKS ? vb_song.pad[1] : 0;
    vb_pl.bpm10 = vb_song.bpm10 >= 400u && vb_song.bpm10 <= 3000u ? vb_song.bpm10 : 1200;
    ui.chaos = vb_song.chaos <= 100 ? vb_song.chaos : ui.chaos;
    load_track(ui.track);
    touch();
    say("SONG LOADED");
}
/* COPY (BANK): the sound on the track into the first bank slot nothing uses yet, and the track onto it */
static int slot_free(uint32_t s)
{
    uint32_t t;
    for (t = 0; t < VB_TRACKS; t++)
        if (vb_track_sound[t] == s)
            return 0;
    if (!flash_ok || st_read(VB_FL_SOUND(s), vb_sec2, 4))
        return 0;
    return vb_sec2[0] == 0xFFu && vb_sec2[1] == 0xFFu && !((vb_dirty_snd[s >> 5] >> (s & 31u)) & 1u);
}
static void sound_copy(void)
{
    uint32_t s;
    char m[24] = "COPIED TO ";
    if (vb_pl.playing || vb_pl.rec) {
        say("STOP, THEN COPY");
        return;
    }
    for (s = 0; s < VB_BANK && !slot_free(s); s++)
        ;
    if (s == VB_BANK) {
        say("BANK FULL");
        return;
    }
    *snd(s) = *RC;
    vb_dirty_snd[s >> 5] |= 1u << (s & 31u);
    vb_save_now();                                                 /* the original and the copy */
    vb_track_sound[ui.track] = (uint8_t)s;
    load_track(ui.track);
    touch();
    utoa10(m + 10, s + 1u);
    say(m);
}

static void load_gens(uint32_t sound)
{
    int cg = 0, n;
    vb_ngen = 0;
    vb_curgen = 0;
    if (!flash_ok || st_read(VB_FL_SOUND(sound), vb_sec, VB_SECTOR))
        return;
    n = vb_sound_unpack(vb_sec, &vb_scratch, vb_gen, VB_GENS, &cg);   /* (the recipe in RAM may be newer) */
    if (n > 0) {
        vb_ngen = (uint8_t)n;
        vb_curgen = (uint8_t)cg;
    }
}

static void boot_load(void)
{
    uint32_t s, i;
    if (!flash_ok)
        return;
    if (!st_read(VB_FL_SET, vb_sec, 64) &&
        ((uint32_t)vb_sec[0] | (uint32_t)vb_sec[1] << 8 | (uint32_t)vb_sec[2] << 16 | (uint32_t)vb_sec[3] << 24) == VB_SET_MAGIC &&
        vb_crc16(vb_sec + 8, 24) == ((uint32_t)vb_sec[4] | (uint32_t)vb_sec[5] << 8)) {
        for (i = 0; i < VB_TRACKS; i++)
            vb_track_sound[i] = vb_sec[8 + i] < VB_BANK ? vb_sec[8 + i] : (uint8_t)i;
        ui.track = vb_sec[12] < VB_TRACKS ? vb_sec[12] : 0;
        ui.chaos = vb_sec[13] <= 100 ? vb_sec[13] : 70;
        s = (uint32_t)vb_sec[14] | (uint32_t)vb_sec[15] << 8;
        vb_pl.bpm10 = s >= 400u && s <= 3000u ? (int)s : 1200;
        for (i = 0; i < VB_TRACKS; i++)                    /* (0xFF: settings from before the mutes) */
            vb_tmute[i] = vb_sec[16] == 0xFFu ? 0 : (uint8_t)((vb_sec[16] >> i) & 1u);
        vb_hsrc = vb_sec[17] <= VB_TRACKS ? vb_sec[17] : 0;
        vb_sync = vb_sec[18] <= 2u ? vb_sec[18] : 0;
        vb_nout = vb_sec[19] <= 1u ? vb_sec[19] : 0;
        for (i = 0; i < VB_TRACKS; i++)
            vb_knext[i] = vb_sec[20 + i] == 1u;
    }
}

/* ---- sound selection: every track's player on its sound; track t selected (its sound on screen, its player with the
 * buffers and the print). A track whose sound changed starts fresh; the others play on. */
static void load_track(uint32_t t)
{
    uint32_t k;
    ui.track = (uint8_t)t;
    for (k = 0; k < VB_TRACKS; k++) {
        vb_recipe_t *r = snd(vb_track_sound[k]);
        if (vb_bound[k] != vb_track_sound[k]) {
            vb_player_panic(vb_P[k]);
            vb_bound[k] = vb_track_sound[k];
        }
        vb_P[k]->rc = r;                                   /* (the player picks up the new recipe next block) */
    }
    RC = vb_P[t]->rc;
    harm_bind();
    nout_bind();
    sel_player(t);
    load_gens(vb_track_sound[t]);
    {
        int g = load_audio(vb_track_sound[t]), k;
        for (k = 0; k < vb_ngen; k++)
            vb_gen[k].audio = (uint8_t)(k == g);
    }
    dirty();
}

static void vb_init(void)
{
    uint32_t i;
    for (i = 0; i < VB_SLOTS; i++)
        vb_slot_snd[i] = 0xFFu;
    vb_slot_clock = 0;
    RC = &vb_slot[0];                                       /* (load_track picks the track's sound after boot_load) */
    sound_default(0, RC);
    vb_player_init(vb_P[0], RC, vb_fxbuf, vb_print_a, vb_print_b, VB_PRINT_CAP);
    for (i = 1; i < VB_TRACKS; i++)
        vb_player_init(vb_P[i], RC, 0, 0, 0, 0);
    for (i = 0; i < VB_TRACKS; i++) {
        vb_P[i]->idle = fm1_wdt_feed;
        vb_tmute[i] = 0;
        vb_knext[i] = 0;
        vb_bound[i] = 0xFFu;
        vb_P[i]->harm = 0;
        vb_P[i]->poly = i ? VB_BG_POLY : 0;
    }
    vb_sel = vb_sel_to = vb_lead = 0;
    vb_hsrc = 0;
    ui.track = 0;
    ui.chaos = 70;
    ui.learn_src = -1;
    ui.rng = 0x2545F491u;
    ui.oct = 2;
    boot_load();
    load_track(ui.track);
}

/* ---- cells: what a knob edits */
enum { F_NUM, F_LIST, F_S24, F_S64, F_PCT, F_NOTE, F_LEN, F_HALF, F_DEST, F_AMT, F_HZ };
enum { K_U8, K_ROOT, K_CTYPE, K_CNOTE, K_FXTYPE, K_MOVE, K_DEST, K_AMT, K_BPM, K_CHAOS, K_SOUND, K_GEN, K_DO, K_STEPS, K_VOI,
       K_BASS, K_SONG, K_MUTE, K_HSRC, K_LIBSRC, K_LIBFX, K_NOUT, K_KNEXT };
typedef struct {
    const char *label;
    uint8_t kind, fmt;
    uint8_t *p;
    int16_t min, max;
    const char *const *names;
    vb_addr_t addr;                                        /* modulation destination (VB_ADDR_NONE: none) */
    uint8_t arg;
} cell_t;
#define NROWS 7
static cell_t C[NROWS][4];
static uint32_t NR;

static void cl(uint32_t r, uint32_t c, const char *label, uint8_t kind, uint8_t fmt, uint8_t *p, int min, int max,
               const char *const *names, vb_addr_t addr, uint8_t arg)
{
    cell_t *x = &C[r][c];
    x->label = label;
    x->kind = kind;
    x->fmt = fmt;
    x->p = p;
    x->min = (int16_t)min;
    x->max = (int16_t)max;
    x->names = names;
    x->addr = addr;
    x->arg = arg;
}
/* a cell from an engine descriptor */
static void cd(uint32_t r, uint32_t c, const vb_param_t *d, uint8_t *p, vb_addr_t addr)
{
    uint8_t fmt = d->flags & VB_P_LIST ? F_LIST : F_NUM;
    cl(r, c, d->label, K_U8, fmt, p, d->min, d->max, d->names, d->flags & VB_P_MOD ? addr : VB_ADDR_NONE, 0);
}

static const char *const SRC_N[6] = {"OP4", "ANLG", "DRUM", "SAMP", "GRAN", "CHOP"};
static const char *const OP_N[4] = {"A", "B", "C", "D"};
static const char *const MODE_N[4] = {"NOTE", "SEQ", "CHORD", "ARP"};
static const char *const ARATE_N[4] = {"1/16", "1/8", "1/4", "1/32"};
static const char *const NOUT_N[2] = {"OFF", "NOTES"};
static const char *const OCT_N[4] = {"1", "2", "3", "4"};
static const char *const RATE_N[4] = {"1/4", "1/8", "1/16", "1/16"};
static const char *const ONOFF_N[2] = {"OFF", "ON"};
static const char *const LFOW_N[7] = {"SIN", "TRI", "SQR", "SAW", "RAMP", "S&H", "EXP"};
static const char *const TRIG_N[5] = {"FREE", "TRIG", "HOLD", "ONE", "HALF"};
static const char *const ELOOP_N[3] = {"OFF", "LOOP", "BEAT"};
static const char *const PLEN_N[4] = {"NOTE", "1 BAR", "2 BAR", "4 BAR"};
static const char *const START_N[2] = {"GRID", "FREE"};
static const char *const AFTER_N[2] = {"FLAT", "KEEP"};
static const char *const TAIL_N[4] = {"AUTO", ".5S", "1S", "2S"};
static const char *const FMT_N[2] = {"PCM", "ADPCM"};
static const char *const DO_N[4] = {"LOAD", "EDIT", "RBLD", "COPY"};
static const char *const SONGDO_N[2] = {"LOAD", "SAVE"};
static const char *const CACT_N[3] = {"INS", "DUP", "DEL"};
static const char *const CIN_N[2] = {"NOTES", "DEGREE"};
static const char *const MUTE_N[2] = {"PLAY", "MUTE"};
static const char *const HSRC_N[VB_TRACKS + 1] = {"OWN", "T1", "T2", "T3", "T4"};
static const char *const SYNC_N[3] = {"INT", "OUT", "IN"};
static const char *const SACT_N[6] = {"ON/OFF", "INS", "DUP", "DEL", "RND 3", "RND 4"};
static const char *const SNAP_N[3] = {"OFF", "CHORD", "SCALE"};
static const char *const KEYS_N[2] = {"PLAY", "NEXT"};
static const char *const RANGE_N[3] = {"1 OCT", "2 OCT", "3 OCT"};
static const char *const SAMP_LBL[8] = {"START", "LEN", "LOOP", "REV", "PITCH", "FINE", "FADE", "GAIN"};
static const char *const SAMP_LOOP_N[3] = {"OFF", "FWD", "P-P"};

static void build_rows(void)
{
    uint32_t r, c;
    vb_play_t *pl = &RC->play;
    for (r = 0; r < NROWS; r++)
        for (c = 0; c < 4u; c++)
            C[r][c].label = 0;
    NR = 1;
    switch (ui.page) {
    case PG_SRC:
        if (RC->src == VB_SRC_OP4) {
            static const uint8_t K[3][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {36, 37, 8, 255}};
            for (r = 0; r < 3u; r++)
                for (c = 0; c < 4u; c++) {
                    uint32_t k = K[r][c];
                    if (k == 255u)
                        continue;
                    if (k < 9u)
                        k += (ui.opsel & 3u) * 9u;
                    cd(r, c, &vb_op4.desc.params[k], &RC->op4[k], VB_ADDR(VB_D_OP4, k));
                }
            cl(2, 3, "OP", K_U8, F_LIST, &ui.opsel, 0, 3, OP_N, VB_ADDR_NONE, 0);
            NR = 3;
        } else if (RC->src == VB_SRC_ANLG) {
            for (r = 0; r < 4u; r++)
                for (c = 0; c < 4u; c++)
                    cd(r, c, &vb_anlg.desc.params[r * 4 + c], &RC->anlg[r * 4 + c], VB_ADDR(VB_D_ANLG, r * 4 + c));
            NR = 4;
        } else if (RC->src == VB_SRC_DRUM) {
            uint32_t v = ui.dvoice, i;
            const vb_drum_voice_desc_t *d = &vb_drum_voices[v];
            static const char *const VN[8] = {"KICK", "SNARE", "CLAP", "HAT", "TOM", "RIM", "COWB", "CYMB"};
            for (i = 0; i < 8u; i++)
                cl(i / 4u, i % 4u, d->p[i].label, K_U8, F_NUM, &RC->drum.p[v][i], 0, 127, 0,
                   VB_ADDR(VB_D_DRUM, v * 9 + i), 0);
            cl(2, 0, "LEVEL", K_U8, F_NUM, &RC->drum.lvl[v], 0, 127, 0, VB_ADDR(VB_D_DRUM, v * 9 + 8), 0);
            cl(2, 1, "VOICE", K_U8, F_LIST, &ui.dvoice, 0, 7, VN, VB_ADDR_NONE, 0);
            NR = 3;
        } else if (RC->src == VB_SRC_CHOP) {                 /* SPLIT first; SENS (TRANS) or SLICES (EVEN) next to it */
            static const uint8_t K[11] = {VB_CH_SPLIT, VB_CH_SENS, VB_CH_SLICE, VB_CH_PITCH, VB_CH_REV, VB_CH_ORDER,
                                          VB_CH_LEN, VB_CH_ATK, VB_CH_DEC, VB_CH_MAP, 255};
            uint32_t i;
            for (i = 0; i < 10u; i++) {
                uint32_t k = K[i] == VB_CH_SENS && RC->chop[VB_CH_SPLIT] == VB_CH_EVEN ? VB_CH_SLICES : K[i];
                cd(i / 4u, i % 4u, &vb_chop.desc.params[k], &RC->chop[k], VB_ADDR(VB_D_CHOP, k));
            }
            NR = 3;
        } else if (RC->src == VB_SRC_GRAN) {
            const vb_engine_t *e = RC->src == VB_SRC_GRAN ? &vb_gran : &vb_chop;
            uint8_t *base = RC->src == VB_SRC_GRAN ? RC->gran : RC->chop;
            uint32_t i, dom = RC->src == VB_SRC_GRAN ? VB_D_GRAN : VB_D_CHOP;
            for (i = 0; i < e->desc.nparams; i++)
                cd(i / 4u, i % 4u, &e->desc.params[i], &base[i], VB_ADDR(dom, i));
            NR = (e->desc.nparams + 3u) / 4u;
        } else {
            uint32_t i;
            for (i = 0; i < 8u; i++)
                cl(i / 4u, i % 4u, SAMP_LBL[i], K_U8, i == 2 ? F_LIST : i == 3 ? F_LIST : i == 4 ? F_S24 : i == 5 ? F_S64 : F_NUM,
                   &RC->samp[i], i == 1 ? 1 : 0, i == 0 ? 126 : i == 2 ? 2 : i == 3 ? 1 : i == 4 ? 48 : i == 5 ? 99 : 127,
                   i == 2 ? SAMP_LOOP_N : i == 3 ? ONOFF_N : 0, (i == 2 || i == 3) ? VB_ADDR_NONE : VB_ADDR(VB_D_SAMP, i), 0);
            NR = 2;
        }
        cl(NR, 0, "LIB", K_LIBSRC, F_LIST, &ui.lib_src, 0, vb_lib_nsrc(), vb_lib_src_names, VB_ADDR_NONE, 0);
        NR++;
        break;
    case PG_PLAY:
        cl(0, 0, "MODE", K_U8, F_LIST, &pl->mode, 0, 3, MODE_N, VB_ADDR_NONE, 0);
        cl(0, 1, "STRUM", K_U8, F_LIST, &pl->strum, 0, VB_STRUMS - 1, vb_strum_name, VB_ADDR_NONE, 0);
        cl(0, 2, "TIME", K_U8, F_NUM, &pl->time, 0, 127, 0, VB_ADDR(VB_D_PLAY, 0), 0);
        cl(0, 3, "HUMAN", K_U8, F_NUM, &pl->hum, 0, 127, 0, VB_ADDR(VB_D_PLAY, 1), 0);
        if (pl->mode == VB_PLAY_ARP) {                     /* the arp instead of the strum */
            cl(0, 1, "PATRN", K_U8, F_LIST, &RC->arp_pat, 0, VB_ARPS - 1, vb_arp_name, VB_ADDR_NONE, 0);
            cl(0, 2, "OCTS", K_U8, F_LIST, &RC->arp_oct, 0, 3, OCT_N, VB_ADDR_NONE, 0);
            cl(0, 3, "RATE", K_U8, F_LIST, &RC->arp_rate, 0, 3, ARATE_N, VB_ADDR_NONE, 0);
        }
        if (pl->mode >= VB_PLAY_CHORD) {
            vb_play_t *cp = PLC();
            vb_chord_t *ch = &cp->chord[cp->sel % VB_CHORD_STEPS];
            uint32_t i;
            cl(1, 0, "ROOT", K_ROOT, F_LIST, &ch->root, 0, 11, NOTE_NM, VB_ADDR_NONE, 0);
            cl(1, 1, "TYPE", K_CTYPE, F_LIST, &ch->type, 0, VB_CHORD_TYPES - 1, vb_chord_type_name, VB_ADDR_NONE, 0);
            cl(1, 2, "LEN", K_U8, F_LEN, &ch->len_i, 0, 11, 0, VB_ADDR_NONE, 0);
            cl(1, 3, "GATE", K_U8, F_PCT, &ch->gate, 5, 100, 0, VB_ADDR_NONE, 0);
            cl(2, 0, "VOICING", K_VOI, F_LIST, &ch->voi, 0, VB_VOICINGS - 1, vb_voicing_name, VB_ADDR_NONE, 0);
            cl(2, 1, "BASS", K_BASS, F_NUM, &ch->bass, 0, VB_CHORD_NOTES, 0, VB_ADDR_NONE, 0);
            cl(2, 2, "STEPS", K_STEPS, F_NUM, &cp->len, 1, VB_CHORD_STEPS, 0, VB_ADDR_NONE, 0);
            cl(2, 3, "EDIT", K_U8, F_LIST, &ui.cact, 0, 2, CACT_N, VB_ADDR_NONE, 0);
            for (i = 0; i < 8u; i++) {
                static const char *const NL[8] = {"N1", "N2", "N3", "N4", "N5", "N6", "N7", "N8"};
                cl(3 + i / 4u, i % 4u, NL[i], K_CNOTE, F_NOTE, 0, 23, 108, 0, VB_ADDR_NONE, (uint8_t)i);
            }
            cl(5, 0, "VEL", K_U8, F_NUM, &pl->vel, 1, 127, 0, VB_ADDR(VB_D_PLAY, 2), 0);
            cl(5, 1, "SPRD", K_U8, F_NUM, &pl->sprd, 0, 127, 0, VB_ADDR(VB_D_PLAY, 3), 0);
            cl(6, 0, "INPUT", K_U8, F_LIST, &RC->cin, 0, 1, CIN_N, VB_ADDR_NONE, 0);
            cl(6, 1, "KEY", K_U8, F_LIST, &RC->key, 0, 11, NOTE_NM, VB_ADDR_NONE, 0);
            cl(6, 2, "SCALE", K_U8, F_LIST, &RC->scale, 0, VB_SCALES - 1, vb_scale_name, VB_ADDR_NONE, 0);
            cl(6, 3, "KEYS", K_KNEXT, F_LIST, &vb_knext[ui.track], 0, 1, KEYS_N, VB_ADDR_NONE, 0);
            NR = 7;
        } else if (pl->mode == VB_PLAY_SEQ) {
            vb_step_t *st = &pl->seq[pl->sel % VB_SEQ_STEPS];
            cl(1, 0, "NOTE", K_U8, F_NOTE, &st->note, 24, 108, 0, VB_ADDR_NONE, 0);
            cl(1, 1, "ON", K_U8, F_LIST, &st->on, 0, 1, ONOFF_N, VB_ADDR_NONE, 0);
            cl(1, 2, "SLIDE", K_U8, F_LIST, &st->slide, 0, 1, ONOFF_N, VB_ADDR_NONE, 0);
            cl(1, 3, "PROB", K_U8, F_PCT, &st->prob, 0, 100, 0, VB_ADDR_NONE, 0);
            cl(2, 0, "VEL", K_U8, F_NUM, &pl->vel, 1, 127, 0, VB_ADDR(VB_D_PLAY, 2), 0);
            cl(2, 1, "RATE", K_U8, F_LIST, &pl->rate, 0, 2, RATE_N, VB_ADDR_NONE, 0);
            cl(2, 2, "STEPS", K_STEPS, F_NUM, &pl->len, 1, VB_SEQ_STEPS, 0, VB_ADDR_NONE, 0);
            cl(2, 3, "EDIT", K_U8, F_LIST, &ui.sact, 0, 5, SACT_N, VB_ADDR_NONE, 0);
            cl(3, 0, "SNAP", K_U8, F_LIST, &RC->snap, 0, 2, SNAP_N, VB_ADDR_NONE, 0);
            cl(3, 1, "RANGE", K_U8, F_LIST, &RC->mel_oct, 0, 2, RANGE_N, VB_ADDR_NONE, 0);
            cl(3, 2, "KEY", K_U8, F_LIST, &RC->key, 0, 11, NOTE_NM, VB_ADDR_NONE, 0);
            cl(3, 3, "SCALE", K_U8, F_LIST, &RC->scale, 0, VB_SCALES - 1, vb_scale_name, VB_ADDR_NONE, 0);
            cl(4, 0, "KEYS", K_KNEXT, F_LIST, &vb_knext[ui.track], 0, 1, KEYS_N, VB_ADDR_NONE, 0);
            NR = 5;
        } else {
            cl(1, 0, "VEL", K_U8, F_NUM, &pl->vel, 1, 127, 0, VB_ADDR(VB_D_PLAY, 2), 0);
            cl(1, 1, "SNAP", K_U8, F_LIST, &RC->snap, 0, 2, SNAP_N, VB_ADDR_NONE, 0);
            cl(1, 2, "KEY", K_U8, F_LIST, &RC->key, 0, 11, NOTE_NM, VB_ADDR_NONE, 0);
            cl(1, 3, "SCALE", K_U8, F_LIST, &RC->scale, 0, VB_SCALES - 1, vb_scale_name, VB_ADDR_NONE, 0);
            NR = 2;
        }
        break;
    case PG_MOD: {
        vb_modsrc_t *m;
        uint32_t s = ui.op % VB_MODSRC, i;                             /* (ui.op doubles as the selected source on MOD) */
        static const char *const DL[4] = {"D1", "D2", "D3", "D4"}, *const AL[4] = {"A1", "A2", "A3", "A4"};
        m = &RC->mod[s];
        if (!m->kind) {
            cl(0, 0, "WAVE", K_U8, F_LIST, &m->wave, 0, 6, LFOW_N, VB_ADDR_NONE, 0);
            cl(0, 1, "RATE", K_U8, F_HZ, &m->rate, 0, 127, 0, VB_ADDR(VB_D_MOD, s * 8), 0);
            cl(0, 2, "SYNC", K_U8, F_LIST, &m->sync, 0, 1, ONOFF_N, VB_ADDR_NONE, 0);
            cl(0, 3, "TRIG", K_U8, F_LIST, &m->trig, 0, 4, TRIG_N, VB_ADDR_NONE, 0);
            cl(3, 0, "PHASE", K_U8, F_NUM, &m->phase, 0, 127, 0, VB_ADDR(VB_D_MOD, s * 8 + 1), 0);
        } else {
            cl(0, 0, "ATK", K_U8, F_NUM, &m->atk, 0, 127, 0, VB_ADDR(VB_D_MOD, s * 8), 0);
            cl(0, 1, "DEC", K_U8, F_NUM, &m->dec, 0, 127, 0, VB_ADDR(VB_D_MOD, s * 8 + 1), 0);
            cl(0, 2, "SUS", K_U8, F_NUM, &m->sus, 0, 127, 0, VB_ADDR(VB_D_MOD, s * 8 + 2), 0);
            cl(0, 3, "REL", K_U8, F_NUM, &m->rel, 0, 127, 0, VB_ADDR(VB_D_MOD, s * 8 + 3), 0);
            cl(3, 0, "LOOP", K_U8, F_LIST, &m->loop, 0, 2, ELOOP_N, VB_ADDR_NONE, 0);
        }
        for (i = 0; i < 4u; i++) {
            cl(1 + i / 2u, (i % 2u) * 2u, DL[i], K_DEST, F_DEST, 0, 0, 0, 0, VB_ADDR_NONE, (uint8_t)i);
            cl(1 + i / 2u, (i % 2u) * 2u + 1u, AL[i], K_AMT, F_AMT, 0, -64, 63, 0, VB_ADDR_NONE, (uint8_t)i);
        }
        NR = 4;
        break;
    }
    case PG_FX: {
        vb_fx_slot_t *f = &RC->fx[ui.op & 7u];
        uint32_t i, s = ui.op & 7u;
        if (f->type)
            for (i = 0; i < 4u; i++)
                cl(0, i, vb_fx_types[f->type].label[i], K_U8, F_NUM, &f->p[i], 0, 127, 0, VB_ADDR(VB_D_FX, s * 5 + i), 0);
        else
            cl(0, 0, "TYPE", K_FXTYPE, F_LIST, &f->type, 0, VB_FX_TYPES - 1, 0, VB_ADDR_NONE, 0);
        cl(1, 0, "TYPE", K_FXTYPE, F_LIST, &f->type, 0, VB_FX_TYPES - 1, 0, VB_ADDR_NONE, 0);
        cl(1, 1, "MIX", K_U8, F_PCT, &f->mix, 0, 127, 0, VB_ADDR(VB_D_FX, s * 5 + 4), 0);
        cl(1, 2, "ON", K_U8, F_LIST, &f->on, 0, 1, ONOFF_N, VB_ADDR_NONE, 0);
        cl(1, 3, "MOVE", K_MOVE, F_NUM, 0, 1, 8, 0, VB_ADDR_NONE, 0);
        cl(2, 0, "LIB", K_LIBFX, F_LIST, &ui.lib_fx, 0, vb_lib_nfx(), vb_lib_fx_names, VB_ADDR_NONE, 0);
        NR = 3;
        break;
    }
    case PG_PRINT:
        cl(0, 0, "LEN", K_U8, F_LIST, &RC->print[0], 0, 3, PLEN_N, VB_ADDR_NONE, 0);
        cl(0, 1, "START", K_U8, F_LIST, &RC->print[1], 0, 1, START_N, VB_ADDR_NONE, 0);
        cl(0, 2, "NORM", K_U8, F_LIST, &RC->print[2], 0, 1, ONOFF_N, VB_ADDR_NONE, 0);
        cl(0, 3, "AFTER", K_U8, F_LIST, &RC->print[3], 0, 1, AFTER_N, VB_ADDR_NONE, 0);
        cl(1, 0, "TAIL", K_U8, F_LIST, &RC->print[4], 0, 3, TAIL_N, VB_ADDR_NONE, 0);
        cl(1, 1, "FMT", K_U8, F_LIST, &RC->print[5], 0, 0, FMT_N, VB_ADDR_NONE, 0);
        NR = 2;
        break;
    case PG_BANK:
        cl(0, 0, "SOUND", K_SOUND, F_NUM, 0, 1, VB_BANK, 0, VB_ADDR_NONE, 0);
        cl(0, 1, "GEN", K_GEN, F_NUM, 0, 0, VB_GENS, 0, VB_ADDR_NONE, 0);
        cl(0, 2, "DO", K_DO, F_LIST, &ui.bank_do, 0, 3, DO_N, VB_ADDR_NONE, 0);
        cl(1, 0, "SONG", K_SONG, F_NUM, &ui.song, 0, VB_SONGS - 1, 0, VB_ADDR_NONE, 0);
        cl(1, 1, "DO", K_U8, F_LIST, &ui.song_do, 0, 1, SONGDO_N, VB_ADDR_NONE, 0);
        NR = 2;
        break;
    case PG_GLO:
        cl(0, 0, "BPM", K_BPM, F_NUM, 0, 40, 300, 0, VB_ADDR_NONE, 0);
        cl(0, 1, "CHAOS", K_CHAOS, F_PCT, &ui.chaos, 0, 100, 0, VB_ADDR_NONE, 0);
        cl(0, 2, "CHORDS", K_HSRC, F_LIST, &vb_hsrc, 0, VB_TRACKS, HSRC_N, VB_ADDR_NONE, 0);
        cl(0, 3, "SYNC", K_MUTE, F_LIST, &vb_sync, 0, 2, SYNC_N, VB_ADDR_NONE, 0);
        cl(2, 0, "MIDI", K_NOUT, F_LIST, &vb_nout, 0, 1, NOUT_N, VB_ADDR_NONE, 0);
        {
            static const char *const TN[VB_TRACKS] = {"T1", "T2", "T3", "T4"};
            uint32_t k;
            for (k = 0; k < VB_TRACKS; k++)
                cl(1, k, TN[k], K_MUTE, F_LIST, &vb_tmute[k], 0, 1, MUTE_N, VB_ADDR_NONE, 0);
        }
        NR = 3;
        break;
    }
    if (ui.row[ui.page] >= NR)
        ui.row[ui.page] = 0;
}

/* a cell's value (for display and stepping) */
static int cell_get(const cell_t *x)
{
    vb_modsrc_t *m = &RC->mod[ui.op % VB_MODSRC];
    vb_chord_t *ch = &PLC()->chord[PLC()->sel % VB_CHORD_STEPS];
    switch (x->kind) {
    case K_CNOTE: return x->arg < ch->n ? ch->note[x->arg] : 23;
    case K_BASS: return ch->bass;
    case K_MOVE: return (ui.op & 7) + 1;
    case K_DEST: {
        vb_addr_t list[200];
        int n = vb_addr_list(RC, list, 200), i;
        for (i = 0; i < n; i++)
            if (list[i] == m->r[x->arg].dest)
                return i + 1;
        return 0;
    }
    case K_AMT: return m->r[x->arg].amt;
    case K_BPM: return vb_pl.bpm10 / 10;
    case K_SOUND: return vb_track_sound[ui.track] + 1;
    case K_GEN: return vb_curgen;
    default: return x->p ? *x->p : 0;
    }
}

static void cell_text(const cell_t *x, char *b)
{
    int v = cell_get(x);
    vb_modsrc_t *m = &RC->mod[ui.op % VB_MODSRC];
    b[0] = 0;
    switch (x->kind) {
    case K_FXTYPE: str_cpy(b, vb_fx_types[v].name, 12); return;
    case K_DEST: vb_addr_name(RC, m->r[x->arg].dest, b, 12); return;
    case K_GEN: str_cpy(b, "G", 4); utoa10(b + 1, (uint32_t)v); return;
    case K_SONG: str_cpy(b, "S", 4); utoa10(b + 1, (uint32_t)v + 1u); return;
    case K_BASS: {                                         /* the chord tone in the bass: "2 E" */
        uint8_t pcs[12];
        int n = vb_chord_tones(&PLC()->chord[PLC()->sel % VB_CHORD_STEPS], pcs);
        if (!v || !n) {
            str_cpy(b, "-", 4);
            return;
        }
        utoa10(b, (uint32_t)v);
        cat(b, NOTE_NM[pcs[(v - 1) % n]], 8);
        return;
    }
    case K_CNOTE:
        if (v == 23)
            str_cpy(b, "-", 4);
        else
            note_name(b, v);
        return;
    default: break;
    }
    switch (x->fmt) {
    case F_LIST: str_cpy(b, x->names && v >= 0 ? x->names[v] : "?", 12); return;
    case F_S24: itoa_s(b, v - 24); return;
    case F_S64: itoa_s(b, v - 50); return;
    case F_PCT: utoa10(b, (uint32_t)(x->max == 127 ? v * 100 / 127 : v)); cat(b, "%", 12); return;
    case F_NOTE: note_name(b, v); return;
    case F_LEN: str_cpy(b, vb_len_name[v % 12], 12); return;
    case F_AMT: itoa_s(b, v); return;
    case F_HZ: {
        static const char *const DIV[9] = {"8/1", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32"};
        if (m->sync) {
            str_cpy(b, DIV[(v * 8 + 63) / 127], 12);
            return;
        }
        utoa10(b, (uint32_t)v);
        return;
    }
    default: utoa10(b, (uint32_t)(v < 0 ? -v : v)); return;
    }
}

/* ---- edits */
static int routes_to(vb_addr_t a)
{
    uint32_t i, k;
    if (a == VB_ADDR_NONE)
        return 0;
    for (i = 0; i < VB_MODSRC; i++)
        for (k = 0; k < VB_ROUTES; k++)
            if (RC->mod[i].r[k].dest == a && RC->mod[i].r[k].amt)
                return 1;
    return 0;
}

static void chord_set_note(vb_chord_t *ch, uint32_t i, int v)
{
    if (v <= 23) {                                         /* "-": remove the note */
        if (i < ch->n) {
            uint32_t k;
            for (k = i; k + 1u < ch->n; k++)
                ch->note[k] = ch->note[k + 1u];
            ch->n--;
        }
    } else if (i < ch->n) {
        ch->note[i] = (uint8_t)v;
    } else if (ch->n < VB_CHORD_NOTES) {
        ch->note[ch->n++] = (uint8_t)v;
    }
}

static void cell_set(const cell_t *x, int v)
{
    vb_modsrc_t *m = &RC->mod[ui.op % VB_MODSRC];
    vb_chord_t *ch = &PLC()->chord[PLC()->sel % VB_CHORD_STEPS];
    v = clampi(v, x->min, x->max);
    switch (x->kind) {
    case K_ROOT: {                                         /* moves the whole voicing, and the BASS */
        int d = v - ch->root, i;
        for (i = 0; i < ch->n; i++)
            ch->note[i] = (uint8_t)clampi(ch->note[i] + d, 24, 108);

        ch->root = (uint8_t)v;
        break;
    }
    case K_CTYPE: {                                        /* fills from the template, at the voicing's octave */
        int lo = ch->n ? ch->note[0] : 48 + ch->root, i;
        for (i = 1; i < ch->n; i++)
            lo = ch->note[i] < lo ? ch->note[i] : lo;
        lo -= ((lo - ch->root) % 12 + 12) % 12;
        vb_chord_fill(ch, ch->root, v, lo - ch->root);
        break;
    }
    case K_CNOTE: chord_set_note(ch, x->arg, v); break;
    case K_BASS: ch->bass = (uint8_t)v; break;
    case K_VOI: vb_chord_voice(ch, v); break;
    case K_SONG: ui.song = (uint8_t)v; ui.d_vis = ui.d_prm = 1; return;
    case K_LIBSRC:                                         /* the library: applied as you turn; hold SEL: back */
    case K_LIBFX:
        undo_mark();
        *x->p = (uint8_t)v;
        if (v && x->kind == K_LIBSRC)
            vb_lib_src(RC, v - 1);
        else if (v)
            vb_lib_fx(RC, v - 1);
        say(x->kind == K_LIBSRC ? vb_lib_src_names[v] : vb_lib_fx_names[v]);
        touch();
        dirty();
        return;
    case K_KNEXT:
        *x->p = (uint8_t)v;
        vb_P[ui.track]->manual = (uint8_t)v;
        say(v ? "ANY KEY: THE NEXT STEP" : "KEYS PLAY NOTES");
        vb_st.dirty = 1;
        vb_st.due = fm1_ms + 2500u;
        dirty();
        return;
    case K_NOUT: vb_nout = (uint8_t)v; nout_bind(); vb_st.dirty = 1; vb_st.due = fm1_ms + 2500u; dirty(); return;
    case K_HSRC: vb_hsrc = (uint8_t)v; harm_bind(); vb_st.dirty = 1; vb_st.due = fm1_ms + 2500u; dirty(); return;
    case K_MUTE: *x->p = (uint8_t)v; vb_st.dirty = 1; vb_st.due = fm1_ms + 2500u; ui.d_vis = ui.d_prm = 1; return;
    case K_FXTYPE: {
        vb_fx_slot_t *f = &RC->fx[ui.op & 7u];
        f->type = (uint8_t)v;
        if (v) {
            uint32_t k;
            for (k = 0; k < 4u; k++)
                f->p[k] = vb_fx_types[v].def[k];
        }
        dirty();
        return;
    }
    case K_MOVE: {
        uint32_t a = ui.op & 7u, b = (uint32_t)v - 1u;
        vb_fx_slot_t t = RC->fx[a];
        RC->fx[a] = RC->fx[b];
        RC->fx[b] = t;
        ui.op = (uint8_t)b;
        dirty();
        return;
    }
    case K_DEST: {
        vb_addr_t list[200];
        int n = vb_addr_list(RC, list, 200);
        v = clampi(v, 0, n);
        m->r[x->arg].dest = v ? list[v - 1] : VB_ADDR_NONE;
        if (v && !m->r[x->arg].amt)
            m->r[x->arg].amt = 32;
        break;
    }
    case K_AMT: m->r[x->arg].amt = (int8_t)clampi(v, -64, 63); break;
    case K_BPM: vb_pl.bpm10 = v * 10; break;
    case K_SOUND:
        vb_track_sound[ui.track] = (uint8_t)(v - 1);
        load_track(ui.track);
        return;
    case K_GEN: vb_curgen = (uint8_t)clampi(v, 0, vb_ngen ? vb_ngen - 1 : 0); break;
    case K_STEPS: *x->p = (uint8_t)v; PLC()->sel = (uint8_t)(PLC()->sel % v); break;
    default:
        if (x->p)
            *x->p = (uint8_t)v;
        if (x->p == &ui.dvoice || x->p == &RC->play.mode)
            dirty();
        break;
    }
    ui.d_prm = ui.d_vis = 1;
    touch();
}

static int cell_max(const cell_t *x)
{
    if (x->kind == K_DEST) {
        vb_addr_t list[200];
        return vb_addr_list(RC, list, 200);
    }
    if (x->kind == K_GEN)
        return vb_ngen ? vb_ngen - 1 : 0;
    if (x->kind == K_BASS) {
        uint8_t pcs[12];
        return vb_chord_tones(&PLC()->chord[PLC()->sel % VB_CHORD_STEPS], pcs);
    }
    return x->max;
}

/* ---- RANDOMIZE, SCRAMBLE, undo, LEARN */
static uint32_t rnd(void) { return vb_rng(&ui.rng); }
static uint8_t mixr(int cur, int lo, int hi)
{
    int t = lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
    return (uint8_t)clampi(cur + (t - cur) * ui.chaos / 100, lo, hi);
}
static void undo_mark(void)
{
    if (!ui.undo_ok) {
        vb_undo_rc = *RC;
        ui.undo_ok = 1;
    }
}
static void randomize(void)
{
    char b[28];
    uint32_t i;
    const vb_engine_t *e = RC->src == VB_SRC_OP4 ? &vb_op4 : RC->src == VB_SRC_ANLG ? &vb_anlg :
                           RC->src == VB_SRC_GRAN ? &vb_gran : RC->src == VB_SRC_CHOP ? &vb_chop : 0;
    undo_mark();
    if ((RC->src == VB_SRC_OP4 || RC->src == VB_SRC_ANLG) && (int)(rnd() % 100u) < ui.chaos * 4 / 10) {
        RC->src = RC->src == VB_SRC_OP4 ? VB_SRC_ANLG : VB_SRC_OP4;
        e = RC->src == VB_SRC_OP4 ? &vb_op4 : &vb_anlg;
    }
    if (e) {
        uint8_t *p = RC->src == VB_SRC_OP4 ? RC->op4 : RC->src == VB_SRC_ANLG ? RC->anlg :
                     RC->src == VB_SRC_GRAN ? RC->gran : RC->chop;
        for (i = 0; i < e->desc.nparams; i++) {
            const vb_param_t *d = &e->desc.params[i];
            if (d->flags & VB_P_LIST) {
                if ((int)(rnd() % 100u) < ui.chaos)
                    p[i] = (uint8_t)(d->min + rnd() % (uint32_t)(d->max - d->min + 1));
            } else {
                p[i] = mixr(p[i], d->min, d->max);
            }
        }
    } else if (RC->src == VB_SRC_DRUM) {
        for (i = 0; i < 8u; i++)
            RC->drum.p[ui.dvoice][i] = mixr(RC->drum.p[ui.dvoice][i], 0, 127);
    } else {
        for (i = 0; i < 8u; i++)
            if (i != 2 && i != 3)
                RC->samp[i] = mixr(RC->samp[i], i == 1 ? 10 : 0, i == 4 ? 48 : i == 5 ? 99 : i == 0 ? 90 : 127);
    }
    ui.rolls++;
    str_cpy(b, "RANDOM #", sizeof b);
    utoa10(b + 8, ui.rolls);
    cat(b, " HOLD=UNDO", sizeof b);
    say(b);
    touch();
    vb_player_note(&vb_pl, vb_pl.last_note ? vb_pl.last_note : 60, 110, 1);
    vb_player_note(&vb_pl, vb_pl.last_note ? vb_pl.last_note : 60, 0, 0);
    dirty();
}
static void scramble(void)
{
    uint32_t i, n, k, j, dests = 0;
    char b[28];
    undo_mark();
    n = 2u + rnd() % (1u + (uint32_t)ui.chaos * 5u / 100u);
    if (n > VB_FX_SLOTS)
        n = VB_FX_SLOTS;
    for (i = 0; i < VB_FX_SLOTS; i++) {
        vb_fx_slot_t *f = &RC->fx[i];
        f->type = 0;
        f->on = 1;
        f->mix = 127;
        if (i < n) {
            f->type = (uint8_t)(1u + rnd() % (VB_FX_TYPES - 1u));
            for (k = 0; k < 4u; k++)
                f->p[k] = mixr(64, 0, 127);
            f->mix = (uint8_t)(57u + rnd() % 71u);
        }
    }
    for (i = 0; i < VB_MODSRC; i++)                        /* routes into the old chain go */
        for (k = 0; k < VB_ROUTES; k++)
            if ((RC->mod[i].r[k].dest >> 8) == VB_D_FX)
                RC->mod[i].r[k].dest = VB_ADDR_NONE;
    for (j = 0; j < 1u + rnd() % (1u + (uint32_t)ui.chaos * 3u / 100u); j++) {
        vb_modsrc_t *m = &RC->mod[rnd() % 6u];
        uint32_t want = 1u + rnd() % (1u + (uint32_t)ui.chaos * 3u / 100u);
        m->wave = (uint8_t)(rnd() % 7u);
        m->rate = (uint8_t)(rnd() % 128u);
        m->sync = rnd() % 10u < 7u;
        for (k = 0; k < VB_ROUTES && want; k++)
            if (m->r[k].dest == VB_ADDR_NONE) {
                uint32_t s = rnd() % n, prm = rnd() % 5u;
                m->r[k].dest = VB_ADDR(VB_D_FX, s * 5u + prm);
                m->r[k].amt = (int8_t)((rnd() & 1u ? 1 : -1) * (int)(12u + rnd() % 52u) * (40 + 60 * ui.chaos / 100) / 100);
                want--;
                dests++;
            }
    }
    ui.rolls++;
    str_cpy(b, "SCRAMBLE ", sizeof b);
    utoa10(b + str_len(b), n);
    cat(b, "FX ", sizeof b);
    utoa10(b + str_len(b), dests);
    cat(b, "M", sizeof b);
    say(b);
    touch();
    ui.op = 0;
    dirty();
}
static void undo(void)
{
    if (!ui.undo_ok) {
        say("NOTHING TO UNDO");
        return;
    }
    *RC = vb_undo_rc;
    ui.undo_ok = 0;
    say("UNDO");
    dirty();
}

/* ---- PRINT and generations */
static void print_start(void)
{
    if (RC->src >= VB_SRC_SAMP && !vb_pl.plen[vb_pl.cur]) {
        say("NO RESAMPLE YET");
        return;
    }
    if (PLC() != &RC->play) {                              /* following CHORDS: the chords heard become its own, so */
        uint32_t i;                                        /* the resample (and REBUILD) holds them */
        const vb_play_t *h = PLC();
        for (i = 0; i < VB_CHORD_STEPS; i++)
            RC->play.chord[i] = h->chord[i];
        RC->play.len = h->len;
        touch();
    }
    vb_player_print(&vb_pl);
    ui.printing = 1;
    say("RESAMPLING...");
}
static void print_poll(void)
{
    char b[28];
    uint32_t k;
    if (!ui.printing || !vb_player_print_take(&vb_pl))
        return;
    ui.printing = 0;
    if (vb_ngen == VB_GENS) {                              /* full: forget the oldest */
        for (k = 1; k < VB_GENS; k++) {
            vb_gen[k - 1] = vb_gen[k];
            vb_gen[k - 1].parent = (int8_t)(vb_gen[k - 1].parent > 0 ? vb_gen[k - 1].parent - 1 : -1);
        }
        vb_ngen--;
        vb_curgen = (uint8_t)(vb_curgen ? vb_curgen - 1u : 0u);
    }
    for (k = 0; k < vb_ngen; k++)
        vb_gen[k].audio = 0;
    vb_gen[vb_ngen].rc = *RC;
    vb_gen[vb_ngen].parent = (int8_t)(vb_ngen ? vb_curgen : -1);
    vb_gen[vb_ngen].audio = 1;
    vb_curgen = vb_ngen++;
    if (RC->print[3] == 0) {                               /* FLAT: start clean on the print */
        for (k = 0; k < VB_FX_SLOTS; k++)
            RC->fx[k].type = 0;
        for (k = 0; k < VB_MODSRC; k++) {
            uint32_t r;
            for (r = 0; r < VB_ROUTES; r++)
                RC->mod[k].r[r].dest = VB_ADDR_NONE;
        }
        if (RC->src < VB_SRC_SAMP)
            RC->src = VB_SRC_SAMP;
    }
    vb_st.audio_dirty = 1;
    touch();
    str_cpy(b, "G", sizeof b);
    utoa10(b + 1, vb_curgen + 1u);
    cat(b, " RESAMPLED", sizeof b);
    say(b);
    dirty();
}
static void bank_do(void)
{
    if (ui.bank_do == 3) {
        sound_copy();
        dirty();
        return;
    }
    if (!vb_ngen) {
        say("NO GENERATIONS");
        return;
    }
    if (ui.bank_do == 0) {                                 /* LOAD: the print as the source */
        if (!vb_gen[vb_curgen].audio) {
            say("NO AUDIO: USE RBLD");
            return;
        }
        if (RC->src < VB_SRC_SAMP)
            RC->src = VB_SRC_SAMP;
        say("LOADED");
    } else if (ui.bank_do == 1) {                          /* EDIT: the recipe that made it */
        *RC = vb_gen[vb_curgen].rc;
        say("EDIT: NEXT ONE BRANCHES");
    } else {                                               /* REBUILD: render the chain from its root, offline */
        uint8_t path[VB_GENS];
        int g = vb_curgen, n = 0, k;
        static vb_recipe_t tmp;
        while (g >= 0 && n < VB_GENS) {
            path[n++] = (uint8_t)g;
            g = vb_gen[g].parent;
        }
        if (vb_gen[path[n - 1]].rc.src >= VB_SRC_SAMP) {
            say("ROOT HAS NO SOURCE");
            return;
        }
        vb_player_panic(&vb_pl);
        vb_mute = 1;
        fm1_delay_ms(15);                                  /* (an audio half: the ISR is out of the player) */
        for (k = n - 1; k >= 0; k--) {
            char m[24] = "REBUILD G";
            utoa10(m + 9, path[k] + 1u);
            say(m);
            vb_draw_now();
            tmp = vb_gen[path[k]].rc;
            vb_pl.rc = &tmp;
            vb_player_print_offline(&vb_pl);
            fm1_wdt_feed();
        }
        vb_pl.rc = RC;
        for (k = 0; k < vb_ngen; k++)
            vb_gen[k].audio = (uint8_t)(k == vb_curgen);
        if (RC->src < VB_SRC_SAMP)
            RC->src = VB_SRC_SAMP;
        vb_mute = 0;
        vb_st.audio_dirty = 1;
        touch();
        say("REBUILT, BIT-EXACT");
    }
    dirty();
}

/* ---- input */
static void goto_page(uint32_t pg)
{
    if (ui.page == pg) {
        build_rows();
        ui.row[pg] = (uint8_t)((ui.row[pg] + 1u) % (NR ? NR : 1u));
    } else {
        ui.page = (uint8_t)pg;
    }
    dirty();
}
/* EDIT on PLAY (SEL runs it): insert an empty step, duplicate the selected one, delete it; SEQ also toggles ON */
static void step_edit(void)
{
    vb_play_t *pl = RC->play.mode >= VB_PLAY_CHORD ? PLC() : &RC->play;
    int seq = RC->play.mode == VB_PLAY_SEQ, max = seq ? VB_SEQ_STEPS : VB_CHORD_STEPS, len = pl->len ? pl->len : 1, i;
    int sel = pl->sel % max, act = seq ? ui.sact : ui.cact + 1;        /* 0 ON/OFF, 1 INS, 2 DUP, 3 DEL */
    if (RC->play.mode == VB_PLAY_NOTE)
        return;
    if (seq && act >= 4) {                                 /* a melody of motifs of 3 or 4, in KEY / SCALE */
        undo_mark();
        vb_melody(pl, RC->key, RC->scale, act == 4 ? 3 : 4, 1 + RC->mel_oct, &ui.rng);
        say(act == 4 ? "MELODY IN THREES" : "MELODY IN FOURS");
        touch();
        dirty();
        return;
    }
    if (act == 0) {
        pl->seq[sel].on ^= 1u;
    } else if (act == 3) {
        if (len <= 1) {
            say("ONE STEP LEFT");
            return;
        }
        for (i = sel; i + 1 < max; i++) {
            if (seq)
                pl->seq[i] = pl->seq[i + 1];
            else
                pl->chord[i] = pl->chord[i + 1];
        }
        pl->len = (uint8_t)(len - 1);
        pl->sel = (uint8_t)(sel >= len - 1 ? len - 2 : sel);
        say("STEP DELETED");
    } else {
        if (len >= max) {
            say(seq ? "16 STEPS: FULL" : "16 CHORDS: FULL");
            return;
        }
        for (i = max - 1; i > sel + 1; i--) {
            if (seq)
                pl->seq[i] = pl->seq[i - 1];
            else
                pl->chord[i] = pl->chord[i - 1];
        }
        if (seq) {
            pl->seq[sel + 1] = pl->seq[sel];
            if (act == 1)
                pl->seq[sel + 1].on = 0;
        } else {
            pl->chord[sel + 1] = pl->chord[sel];
            if (act == 1) {
                pl->chord[sel + 1].n = 0;
                pl->chord[sel + 1].bass = 0;
            }
        }
        pl->len = (uint8_t)(len + 1);
        pl->sel = (uint8_t)(sel + 1);
        say(act == 1 ? "STEP INSERTED" : "STEP COPIED");
    }
    touch();
    dirty();
}
/* OCT- / OCT+ on PLAY: the selected chord (its BASS too) or step an octave down / up */
static int step_octave(int d)
{
    vb_play_t *pl = &RC->play;
    int i;
    if (ui.page != PG_PLAY || pl->mode == VB_PLAY_NOTE)
        return 0;
    if (pl->mode == VB_PLAY_SEQ) {
        vb_step_t *st = &pl->seq[pl->sel % VB_SEQ_STEPS];
        if (st->note + d < 24 || st->note + d > 108) {
            say("OUT OF RANGE");
            return 1;
        }
        st->note = (uint8_t)(st->note + d);
    } else {
        vb_chord_t *ch = &PLC()->chord[PLC()->sel % VB_CHORD_STEPS];
        for (i = 0; i < ch->n; i++)
            if (ch->note[i] + d < 24 || ch->note[i] + d > 108) {
                say("OUT OF RANGE");
                return 1;
            }
        for (i = 0; i < ch->n; i++)
            ch->note[i] = (uint8_t)(ch->note[i] + d);
    }
    say(d > 0 ? "STEP +12" : "STEP -12");
    touch();
    ui.d_vis = ui.d_prm = 1;
    return 1;
}
/* SEL on the MOD page's shape rows: a new shape for the LFO / envelope; with no destination yet, one or two random ones */
static void mod_randomize(void)
{
    vb_modsrc_t *m = &RC->mod[ui.op % VB_MODSRC];
    uint32_t k, live = 0, want;
    char b[28];
    undo_mark();
    if (!m->kind) {
        if ((int)(rnd() % 100u) < ui.chaos)
            m->wave = (uint8_t)(rnd() % 7u);
        m->rate = mixr(m->rate, 0, 127);
        if ((int)(rnd() % 100u) < ui.chaos)
            m->sync = (uint8_t)(rnd() % 10u < 6u);
        if ((int)(rnd() % 100u) < ui.chaos)
            m->trig = (uint8_t)(rnd() % 5u);
        m->phase = mixr(m->phase, 0, 127);
    } else {
        m->atk = mixr(m->atk, 0, 127);
        m->dec = mixr(m->dec, 0, 127);
        m->sus = mixr(m->sus, 0, 127);
        m->rel = mixr(m->rel, 0, 127);
        if ((int)(rnd() % 100u) < ui.chaos / 2)
            m->loop = (uint8_t)(rnd() % 3u);
    }
    for (k = 0; k < VB_ROUTES; k++)
        live += m->r[k].dest != VB_ADDR_NONE && m->r[k].amt;
    if (!live) {
        vb_addr_t list[200];
        int n = vb_addr_list(RC, list, 200);
        want = 1u + rnd() % 2u;
        for (k = 0; k < VB_ROUTES && want && n > 0; k++)
            if (m->r[k].dest == VB_ADDR_NONE || !m->r[k].amt) {
                vb_addr_t a = list[rnd() % (uint32_t)n];
                if ((a >> 8) == VB_D_MOD && (a & 0xFFu) / 8u == ui.op % VB_MODSRC)
                    continue;                                  /* not onto itself */
                m->r[k].dest = a;
                m->r[k].amt = (int8_t)((rnd() & 1u ? 1 : -1) * (int)(12u + rnd() % 40u) * (40 + 60 * ui.chaos / 100) / 100);
                want--;
            }
    }
    ui.rolls++;
    str_cpy(b, m->kind ? "RANDOM ENV #" : "RANDOM LFO #", sizeof b);
    utoa10(b + str_len(b), ui.rolls);
    say(b);
    touch();
    dirty();
}
static void sel_tap(void)
{
    switch (ui.page) {
    case PG_SRC: randomize(); break;
    case PG_FX: scramble(); break;
    case PG_MOD: {
        vb_modsrc_t *m = &RC->mod[ui.op % VB_MODSRC];
        int k;
        if (ui.learn_src < 0 && ui.row[PG_MOD] != 1 && ui.row[PG_MOD] != 2) {
            mod_randomize();
            break;
        }
        if (ui.learn_src >= 0) {
            ui.learn_src = -1;
            say("LEARN OFF");
            break;
        }
        for (k = 0; k < VB_ROUTES && m->r[k].dest != VB_ADDR_NONE; k++)
            ;
        if (k == VB_ROUTES)
            k = ui.row[PG_MOD] == 2 ? 2 : 0;
        ui.learn_src = (int8_t)(ui.op % VB_MODSRC);
        ui.learn_slot = (int8_t)k;
        say("LEARN: TURN A KNOB");
        break;
    }
    case PG_PRINT: print_start(); break;
    case PG_BANK:
        if (ui.row[PG_BANK] == 1) {
            if (ui.song_do)
                song_save(ui.song);
            else
                song_load(ui.song);
            dirty();
        } else {
            bank_do();
        }
        break;
    case PG_PLAY: step_edit(); break;
    }
}
static void press(uint32_t b)
{
    switch (b) {
    case B_EDIT: goto_page(PG_SRC); break;
    case B_ARP:
        if (ui.page == PG_PLAY && ui.row[PG_PLAY] == 0)
            goto_page(PG_PLAY);
        else {
            ui.page = PG_PLAY;
            ui.row[PG_PLAY] = 0;
            dirty();
        }
        break;
    case B_SEQ:
        if (ui.page == PG_PLAY && ui.row[PG_PLAY] > 0)
            goto_page(PG_PLAY);
        else {
            ui.page = PG_PLAY;
            ui.row[PG_PLAY] = 1;
            dirty();
        }
        break;
    case B_LFO: case B_ENV: {
        int env = b == B_ENV;
        if (ui.page == PG_MOD && (ui.op >= 6) == env)
            goto_page(PG_MOD);
        else {
            ui.page = PG_MOD;
            ui.row[PG_MOD] = 0;
            ui.op = (uint8_t)(env ? 6 : 0);
            dirty();
        }
        break;
    }
    case B_FX:
        if (ui.page != PG_FX)
            ui.op = 0;
        goto_page(PG_FX);
        break;
    case B_HOME: goto_page(PG_PRINT); break;
    case B_SAVE: goto_page(PG_BANK); break;
    case B_GLO: goto_page(PG_GLO); break;
    case B_PLAY:
        transport(!vb_pl.playing);
        say(vb_pl.playing ? "STOP" : "PLAY");
        break;
    case B_REC: print_start(); break;
    case B_SCL: ui.sel_t0 = fm1_ms ? fm1_ms : 1u; break;
    case B_OCTDN: case B_OCTUP:
        if (step_octave(b == B_OCTUP ? 12 : -12))
            break;
        ui.oct = (uint8_t)clampi(ui.oct + (b == B_OCTUP ? 1 : -1), 0, 4);
        {
            char m[12] = "OCT ";
            itoa_s(m + 4, ui.oct - 2);
            say(m);
        }
        break;
    }
}

static uint32_t keys_held;

/* CHORD with INPUT DEGREE: the keys are a chord pad, on every page. C4..B4 play the degrees I..VII of KEY / SCALE,
 * C5..G5 I..V an octave up; the black keys from C#4 major chords on bII bIII bV bVI bVII (an octave up from C#5).
 * F3..B3 held with them: 7, 6, SWAP (major <-> minor), SUS2, SUS4, DIM, 9; pressed while a chord is held, they change
 * it (letting go of them does not). On PLAY, stopped, each chord is also
 * written into the selected step, and the next step is selected. */
static const int8_t PAD[27] = {-1, -1, -1, -1, -1, -1, -1,                      /* F3..B3: the modifiers */
                               0, 11, 1, 13, 2, 3, 16, 4, 18, 5, 20, 6,          /* C4..B4 (10 + semis: borrowed) */
                               32, 43, 33, 45, 34, 35, 48, 36};                  /* C5..G5 (+ 32: an octave up) */
static const uint8_t PAD_MOD[7] = {VB_CM_7, VB_CM_6, VB_CM_SWAP, VB_CM_SUS2, VB_CM_SUS4, VB_CM_DIM, VB_CM_9};
static struct { int8_t key, step; uint8_t n, note[VB_CHORD_NOTES + 1]; } jam = {-1, -1, 0, {0}};

static void jam_off(void)
{
    uint32_t i;
    for (i = 0; i < jam.n; i++)
        vb_player_note(&vb_pl, jam.note[i], 0, 0);
    jam.n = 0;
}
static void jam_on(uint32_t n)
{
    vb_play_t *pl = PLC();
    int p = PAD[n], deg = (p & 31) < 10 ? p & 31 : -1, semis = (p & 31) - 10, root, type, mods = 0;
    int base = 48 + 12 * ((int)ui.oct - 2) + (p & 32 ? 12 : 0);
    uint32_t i;
    vb_chord_t c, *dst;
    for (i = 0; i < 7u; i++)
        if ((fm1_in.notes >> i) & 1u)
            mods |= PAD_MOD[i];
    type = vb_degree_chord(RC->key, RC->scale, deg, semis, mods, &root);
    jam_off();
    dst = 0;
    if (ui.page == PG_PLAY && !vb_pl.playing) {           /* write: the step played last time if the chord changes */
        uint32_t s = jam.key >= 0 && jam.step >= 0 ? (uint32_t)jam.step : pl->sel % VB_CHORD_STEPS;
        dst = &pl->chord[s];
        if (jam.key < 0 || jam.step < 0) {
            jam.step = (int8_t)s;
            pl->sel = (uint8_t)((s + 1u) % (pl->len ? pl->len : 1u));
        }
    }
    c = dst ? *dst : pl->chord[pl->sel % VB_CHORD_STEPS];
    vb_chord_fill(&c, root, type, base);
    if (dst) {
        *dst = c;
        touch();
        ui.d_vis = ui.d_prm = 1;
    }
    for (i = 0; i < c.n; i++)
        jam.note[jam.n++] = c.note[i];
    if (vb_chord_bass(&c))
        jam.note[jam.n++] = (uint8_t)vb_chord_bass(&c);
    for (i = 0; i < jam.n; i++)
        vb_player_note(&vb_pl, jam.note[i], RC->play.vel, 1);
    jam.key = (int8_t)n;
}
static void chord_pad(uint32_t n, int down)
{
    if (n >= 27u)
        return;
    if (PAD[n] < 0) {                                      /* a modifier pressed: the held chord changes with it */
        if (down && jam.key >= 0)
            jam_on((uint32_t)jam.key);
        return;
    }
    if (down) {
        jam.step = -1;
        jam.key = -1;
        jam_on(n);
    } else if ((int)n == jam.key) {
        jam_off();
        jam.key = jam.step = -1;
    }
}

/* SNAP: a key's note onto the chord playing now (CHORDS' track, or the track's own progression; stopped: the selected
 * chord), or onto KEY / SCALE */
static int snap_key(int note)
{
    uint8_t pcs[12];
    int n;
    if (RC->snap == 2u) {
        vb_scale_pcs(RC->key, RC->scale, pcs);
        n = 7;
    } else {
        const vb_player_t *s = vb_hsrc ? vb_P[vb_hsrc - 1u] : &vb_pl;
        const vb_play_t *h = &s->rc->play;
        int st = s->playing ? vb_chord_at(h, s->n16 ? s->n16 - 1u : 0u) : h->sel % VB_CHORD_STEPS;
        n = vb_chord_tones(&h->chord[st], pcs);
    }
    return vb_snap(note, pcs, n);
}
static uint8_t key_note[27];                               /* the note each key sounds (SNAP may move it) */

/* KEYS NEXT: any key plays the next step (a note, or a chord: its notes, or the arp of them), as long as it is held */
static uint8_t key_step[27];                               /* the step each key plays, + 1 (0: none) */
static void next_key(uint32_t n, int down)
{
    vb_play_t *pl = RC->play.mode == VB_PLAY_SEQ ? &RC->play : PLC();
    int seq = RC->play.mode == VB_PLAY_SEQ, len = pl->len ? pl->len : 1, s, i;
    if (n >= 27u)
        return;
    if (!down) {                                           /* the notes it started, off */
        if (key_step[n]) {
            s = key_step[n] - 1;
            if (seq)
                vb_player_note(&vb_pl, key_note[n], 0, 0);
            else {
                const vb_chord_t *c = &pl->chord[s % VB_CHORD_STEPS];
                for (i = 0; i < c->n; i++)
                    vb_player_note(&vb_pl, c->note[i], 0, 0);
                if (vb_chord_bass(c))
                    vb_player_note(&vb_pl, vb_chord_bass(c), 0, 0);
            }
            key_step[n] = 0;
        }
        return;
    }
    s = pl->sel % len;
    if (seq) {                                             /* the next step that is on */
        for (i = 0; i < len && !pl->seq[(s + i) % len].on; i++)
            ;
        s = (s + i) % len;
        key_note[n] = (uint8_t)(RC->snap ? snap_key(pl->seq[s].note) : pl->seq[s].note);
        vb_player_note(&vb_pl, key_note[n], RC->play.vel, 1);
    } else {
        const vb_chord_t *c = &pl->chord[s % VB_CHORD_STEPS];
        for (i = 0; i < c->n; i++)
            vb_player_note(&vb_pl, c->note[i], RC->play.vel, 1);
        if (vb_chord_bass(c))
            vb_player_note(&vb_pl, vb_chord_bass(c), RC->play.vel, 1);
    }
    key_step[n] = (uint8_t)(s + 1);
    pl->sel = (uint8_t)((s + 1) % len);                    /* the screen shows the one up next */
    ui.d_vis = 1;
}

static void key(uint32_t n, int down)
{
    int note = 53 + (int)n + 12 * ((int)ui.oct - 2);       /* key 0 = F3 at OCT 0 */
    vb_play_t *pl = &RC->play;
    if (vb_knext[ui.track] && pl->mode != VB_PLAY_NOTE) {
        next_key(n, down);
        return;
    }
    if (pl->mode >= VB_PLAY_CHORD && RC->cin) {
        chord_pad(n, down);
        return;
    }
    if (!down) {
        if (n < 27u && key_note[n]) {
            note = key_note[n];
            key_note[n] = 0;
        }
        vb_player_note(&vb_pl, note, 0, 0);
        return;
    }
    if (RC->snap && n < 27u && (pl->mode == VB_PLAY_NOTE || (pl->mode == VB_PLAY_SEQ && ui.page != PG_PLAY)))
        note = snap_key(note);
    if (n < 27u)
        key_note[n] = (uint8_t)note;
    if (ui.page == PG_PLAY && pl->mode == VB_PLAY_CHORD) {
        vb_chord_t *ch = &PLC()->chord[PLC()->sel % VB_CHORD_STEPS];
        uint32_t i;
        for (i = 0; i < ch->n && ch->note[i] != note; i++)
            ;
        if (i < ch->n)
            chord_set_note(ch, i, 0);
        else if (ch->n < VB_CHORD_NOTES)
            ch->note[ch->n++] = (uint8_t)note;
        else
            say("MAX 8 NOTES");
        ui.d_vis = ui.d_prm = 1;
    } else if (ui.page == PG_PLAY && pl->mode == VB_PLAY_SEQ) {
        vb_step_t *st = &pl->seq[pl->sel % VB_SEQ_STEPS];
        st->note = (uint8_t)note;
        st->on = 1;
        pl->sel = (uint8_t)((pl->sel + 1u) % (pl->len ? pl->len : 1u));
        ui.d_vis = ui.d_prm = 1;
    }
    if (RC->src == VB_SRC_CHOP && ui.page == PG_SRC) {     /* the slices are known once the note has played */
        ui.d_vis = 1;
        ui.vis_due = fm1_ms + 80u;
    }
    if (RC->src == VB_SRC_DRUM && ui.dvoice != (uint8_t)(note % 8)) {
        ui.dvoice = (uint8_t)(note % 8);
        if (ui.page == PG_SRC)
            dirty();
    }
    vb_player_note(&vb_pl, note, pl->vel, 1);
}

static void vb_input(void)
{
    uint32_t rel, pr = fm1_input_edges(&rel), id, c;
    uint32_t notes = fm1_in.notes, ed = notes ^ keys_held;
    int32_t d;
    for (id = 0; id < 14u; id++) {
        uint32_t b = panel_btn_of(id);
        if (b == NB)
            continue;
        if ((pr >> id) & 1u)
            press(b);
        if (((rel >> id) & 1u) && b == B_SCL && ui.sel_t0) {
            if (fm1_ms - ui.sel_t0 >= 600u)
                undo();
            else
                sel_tap();
            ui.sel_t0 = 0;
        }
    }
    (void)fm1_input_note_edges();
    for (id = 0; ed; id++, ed >>= 1)
        if (ed & 1u)
            key(id, (int)((notes >> id) & 1u));
    keys_held = notes;
    build_rows();
    if ((d = panel_enc(EN_SELECT)) != 0) {
        int s = d > 0 ? 1 : -1;
        switch (ui.page) {
        case PG_SRC: {                                     /* the engine (the keys pick a DRUM voice) */
            int v = clampi(RC->src + s, 0, 5);
            if (v != RC->src) {
                RC->src = (uint8_t)v;
                ui.row[PG_SRC] = 0;
                ui.undo_ok = 0;
                say(SRC_N[v]);
                touch();
            }
            break;
        }
        case PG_PLAY:
            if (RC->play.mode >= VB_PLAY_CHORD)
                PLC()->sel = (uint8_t)clampi(PLC()->sel + s, 0, VB_CHORD_STEPS - 1);
            else
                RC->play.sel = (uint8_t)clampi(RC->play.sel + s, 0, VB_SEQ_STEPS - 1);
            break;
        case PG_MOD: ui.op = (uint8_t)clampi(ui.op + s, 0, VB_MODSRC - 1); break;
        case PG_FX: ui.op = (uint8_t)clampi(ui.op + s, 0, VB_FX_SLOTS - 1); break;
        case PG_BANK:
            if (ui.row[PG_BANK] == 1)
                ui.song = (uint8_t)clampi(ui.song + s, 0, VB_SONGS - 1);
            else
                vb_curgen = (uint8_t)clampi(vb_curgen + s, 0, vb_ngen ? vb_ngen - 1 : 0);
            break;
        }
        dirty();
    }
    if ((d = panel_enc(EN_ALGO)) != 0) {
        load_track((uint32_t)clampi(ui.track + (d > 0 ? 1 : -1), 0, VB_TRACKS - 1));
        say(ui.track == 0 ? "TRACK 1" : ui.track == 1 ? "TRACK 2" : ui.track == 2 ? "TRACK 3" : "TRACK 4");
    }
    if ((d = panel_enc(EN_PRESET)) != 0) {
        vb_track_sound[ui.track] = (uint8_t)clampi(vb_track_sound[ui.track] + (d > 0 ? 1 : -1), 0, VB_BANK - 1);
        load_track(ui.track);
    }
    for (c = 0; c < 4u; c++) {
        cell_t *x = &C[ui.row[ui.page]][c];
        int step;
        d = panel_enc(EN_K1 + c);
        if (!d || !x->label)
            continue;
        if (ui.learn_src >= 0 && ui.page != PG_MOD && x->addr != VB_ADDR_NONE) {
            vb_route_t *r = &RC->mod[ui.learn_src].r[ui.learn_slot];
            char b[24];
            r->dest = x->addr;
            if (!r->amt)
                r->amt = 32;
            vb_addr_name(RC, x->addr, b, 16);
            say(b);
            ui.page = PG_MOD;
            ui.op = (uint8_t)ui.learn_src;
            ui.row[PG_MOD] = (uint8_t)(1 + ui.learn_slot / 2);
            ui.learn_src = -1;
            dirty();
            continue;
        }
        if (x->kind != K_LIBSRC && x->kind != K_LIBFX)
            ui.undo_ok = 0;                                /* a hand edit ends the series of rolls (not LIB browsing) */
        step = (x->fmt == F_LIST || x->kind == K_DEST || x->kind == K_CNOTE || x->kind == K_GEN || x->kind == K_BASS)
               ? (d > 0 ? 1 : -1)
               : d * (d > 1 || d < -1 ? 4 : 1);
        {
            int nv = cell_get(x) + step;
            if (x->kind == K_DEST || x->kind == K_BASS)
                nv = clampi(nv, 0, cell_max(x));
            cell_set(x, nv);
        }
        ui.vis_due = fm1_ms + 60u;
    }
    vb_midi_in();
    vb_remote();
    print_poll();
    vb_autosave();
}

/* ---- MIDI clock in (SYNC IN): START / STOP / CONTINUE run the tracks; the tempo from the ticks' times (ms) over
 * a beat, and at every sixteenth the tracks' phase pulled a quarter of the way to the clock's */
static struct { uint32_t ticks, beat_ms, run, ext, checked, ext_ms[8]; } clk;
static void clock_in(uint32_t rt, uint32_t ms)
{
    if (rt == 0xFAu || rt == 0xFBu) {                     /* the first tick after START is the first sixteenth's */
        clk.ticks = clk.ext = clk.checked = 0;
        clk.run = 1;
        if (!vb_pl.playing)
            transport(1);
        return;
    }
    if (rt == 0xFCu) {
        clk.run = 0;
        if (vb_pl.playing)
            transport(0);
        return;
    }
    if (rt != 0xF8u || !clk.run)
        return;
    clk.ticks++;
    if (clk.ticks % 24u == 1u) {                           /* a beat begins: the tempo over the last one */
        uint32_t dt = ms - clk.beat_ms;
        if (clk.ticks > 1u && dt >= 20u && dt <= 1500u) {
            int b = (int)((600000u + dt / 2u) / dt);
            vb_pl.bpm10 = clampi(clk.ticks == 25u ? b : (vb_pl.bpm10 * 3 + b) / 4, 400, 3000);
        }
        clk.beat_ms = ms;
    }
    if (clk.ticks % 6u == 1u) {                            /* a sixteenth begins: when (the phase, below) */
        uint32_t ext = (clk.ticks - 1u) / 6u + 1u;
        clk.ext_ms[ext % 8u] = ms;
        clk.ext = ext;
    }
}
/* the same sixteenth, the clock's and ours, once both began: ours pulled half way to the clock's */
static void clock_phase(void)
{
    uint32_t ours = vb_last16;
    if (vb_sync != 2u || !clk.run || !vb_pl.playing || !ours || ours == clk.checked)
        return;
    if (ours + 8u <= clk.ext || ours > clk.ext + 8u) {     /* far apart (a stall): start over from the clock's */
        clk.checked = ours;
        return;
    }
    if (ours > clk.ext)                                    /* ours began, the clock's not yet: wait */
        return;
    clk.checked = ours;
    vb_nudge = (int32_t)(vb_ms16 - clk.ext_ms[ours % 8u]) * 441 * 256 / 20;   /* + late: sooner, - early: later */
    if (ours + 2u <= clk.ext)                              /* (and a whole sixteenth or more behind: catch up) */
        vb_nudge += 5 * 441 * 256 / 10;
}

/* ---- USB MIDI notes in (a DAW or another controller plays the sound) */
static void vb_midi_in(void)
{
    if (midi_in_overflow) {
        mi_r = mi_w;
        midi_in_overflow = 0;
        return;
    }
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ], st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        uint32_t ms = midi_in_ms[mi_r % MQ], rt = (pkt >> 8) & 0xFFu;
        mi_r++;
        if ((pkt & 15u) == 0xFu && rt >= 0xF8u) {
            if (vb_sync == 2u)
                clock_in(rt, ms);
            continue;
        }
        if (st == 0x90u && d2)
            vb_player_note(&vb_pl, (int)d1, (int)d2, 1);
        else if (st == 0x80u || st == 0x90u)
            vb_player_note(&vb_pl, (int)d1, 0, 0);
    }
    clock_phase();
}

/* ---- remote control and diagnostics: SysEx F0 7D 56 42 <cmd> <a> <b> F7 (tools/fm1_remote.py)
 *   01 stats -> F0 7D 56 42 7F <fields, 5 x 7 bits each> F7     02 b: press button b (B_* order; SEL = tap)
 *   03 e d: turn encoder role e by d - 64                         04 n on: key n (0..26) down / up
 *   05 reset the max / late counters    06 hold SEL (undo)    07 save to flash now                         */
static void put32(uint8_t *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 5u; i++) {
        b[i] = (uint8_t)(v & 0x7Fu);
        v >>= 7;
    }
}
static void vb_remote(void)
{
    const uint8_t *p;
    uint32_t n, a, b, cmd;
    if (!ota_frame_get(&p, &n))
        return;
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x56u || p[2] != 0x42u)
        return;                                            /* not ours: the update handshake's */
    cmd = p[3];
    a = n > 4u ? p[4] : 0u;
    b = n > 5u ? p[5] : 0u;
    ota_frame_done();
    switch (cmd) {
    case 1: {
        static uint8_t r[6 + 5 * 21];
        uint32_t k = 5, v, live = 0;
        uint32_t f[21];
        for (v = 0; v < VB_POLY; v++)
            live += vb_pl.vlive[v];
        f[0] = vb_dbg.halves; f[1] = vb_dbg.late; f[2] = vb_dbg.max_us; f[3] = vb_dbg.last_us; f[4] = vb_dbg.cpu_q8;
        f[5] = vb_dbg.nested; f[6] = fm1_ms; f[7] = ui.page; f[8] = RC->src; f[9] = RC->play.mode;
        f[10] = vb_pl.playing; f[11] = live; f[12] = vb_ngen; f[13] = vb_pl.plen[vb_pl.cur]; f[14] = (uint32_t)vb_pl.peak;
        f[15] = ui.row[ui.page]; f[16] = vb_pl.rec; f[17] = ui.op; f[18] = vb_pl.nev; f[19] = (uint32_t)vb_pl.bpm10; f[20] = vb_master_q12;
        vb_pl.peak = 0;
        r[0] = 0xF0u; r[1] = 0x7Du; r[2] = 0x56u; r[3] = 0x42u; r[4] = 0x7Fu;
        for (v = 0; v < 21u; v++, k += 5u)
            put32(r + k, f[v]);
        r[k++] = 0xF7u;
        ota_wire_send(r, k);
        break;
    }
    case 2:
        if (a < NB) {
            if (a == B_SCL)
                sel_tap();
            else
                press(a);
        }
        break;
    case 3:
        if (a < NE)
            vb_rem_enc[a] = (int8_t)((int)b - 64);
        break;
    case 4:
        if (a < 27u)
            key(a, (int)b);
        break;
    case 5:
        vb_dbg.max_us = 0;
        vb_dbg.late = 0;
        break;
    case 6: undo(); break;
    case 7: vb_save_now(); break;
    }
    dirty();
}

static void vb_leds(void)
{
    static const uint8_t PAGE_BTN[NPG] = {B_EDIT, B_ARP, B_LFO, B_FX, B_HOME, B_SAVE, B_GLO};
    uint32_t i;
    for (i = 0; i < NPG; i++)
        panel_led(PAGE_BTN[i], ui.page == i && !(i == PG_MOD && ui.op >= 6));
    panel_led(B_ENV, ui.page == PG_MOD && ui.op >= 6);
    panel_led(B_SEQ, ui.page == PG_PLAY && ui.row[PG_PLAY] > 0);
    panel_led(B_PLAY, vb_pl.playing);
    panel_led(B_REC, ui.printing || vb_pl.rec);
    panel_led(B_SCL, ui.undo_ok || ui.learn_src >= 0);
}

/* ---- drawing: header (40 px), the page's picture (124 px), the knob row (76 px); one strip per frame */
static void text_c(int32_t x, int32_t w, int32_t y, uint32_t sc, const char *s, uint16_t c)
{
    md_text(x + (w - md_text_w(&MD_F3, sc, s)) / 2, y, &MD_F3, sc, s, c);
}
static void text_fit(int32_t x, int32_t w, int32_t y, uint32_t sc, const char *s, uint16_t c)   /* shrinks to fit */
{
    uint32_t want = sc;                                    /* y is the top of the text at the size asked for */
    while (sc > 1u && md_text_w(&MD_F3, sc, s) > w - 4)
        sc--;
    text_c(x, w, y + (int32_t)(want - sc) * 5 / 2, sc, s, c);
}

/* ---- rounded shapes, as the mockup draws them (a radius in px; the corners per row) */
static int32_t isqrt(int32_t v)
{
    int32_t r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}
static int32_t corner_in(int32_t r, int32_t j)                 /* how far row j (0 = the edge) of a corner is inset */
{
    int32_t d = 2 * r - 2 * j - 1;
    return (2 * r - isqrt(4 * r * r - d * d) + 1) / 2;
}
static void rfill(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t j;
    if (r > h / 2)
        r = h / 2;
    if (r > w / 2)
        r = w / 2;
    for (j = 0; j < r; j++) {
        int32_t in = corner_in(r, j);
        cv_rect(x + in, y + j, w - 2 * in, 1, c);
        cv_rect(x + in, y + h - 1 - j, w - 2 * in, 1, c);
    }
    cv_rect(x, y + r, w, h - 2 * r, c);
}
/* a rounded box: a t px border in ink, filled with fill */
static void rbox(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t t, uint16_t ink, uint16_t fill)
{
    rfill(x, y, w, h, r, ink);
    rfill(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 1, fill);
}
static void line2(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)   /* 2 px */
{
    cv_line(x0, y0, x1, y1, c);
    cv_line(x0 + 1, y0, x1 + 1, y1, c);
    cv_line(x0, y0 + 1, x1, y1 + 1, c);
}
static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t dy;
    for (dy = -r; dy <= r; dy++) {
        int32_t dx = isqrt(r * r - dy * dy);
        cv_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}
/* an ADSR as a 2 px line in a w x h box: attack, decay, a quarter of sustain, release */
static void adsr(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t a, uint32_t d, uint32_t su, uint32_t rl)
{
    int32_t q = w / 4, xa = x + (int32_t)a * q / 127, xd = xa + (int32_t)d * q / 127, ys = y + h - 1 - (int32_t)su * (h - 2) / 127;
    int32_t xs = xd + q, xr = xs + (int32_t)rl * q / 127;
    line2(x, y + h - 2, xa, y, C_INK);
    line2(xa, y, xd, ys, C_INK);
    line2(xd, ys, xs, ys, C_INK);
    line2(xs, ys, xr, y + h - 2, C_INK);
}

static void draw_hdr(void)
{
    char t[24];
    const char *right;
    uint32_t r;
    build_rows();
    cv_begin(240, 40, C_BG);
    str_cpy(t, ui.page == PG_SRC ? SRC_N[RC->src] : ui.page == PG_PLAY ? MODE_N[RC->play.mode] :
               ui.page == PG_BANK && ui.row[PG_BANK] == 1 ? "SONG" : PG_NAME[ui.page], sizeof t);
    if (md_text_w(&MD_F3, 4, t) > 104)
        md_text(8, 12, &MD_F3, 3, t, C_INK);
    else
        md_text(8, 9, &MD_F3, 4, t, C_INK);
    for (r = 0; r < NR && NR > 1u; r++) {
        cv_rect(118 + (int32_t)r * 9, 16, 7, 7, C_INK);
        if (r != ui.row[ui.page])
            cv_rect(119 + (int32_t)r * 9, 17, 5, 5, C_BG);
    }
    if (fm1_ms < ui.msg_until) {
        right = ui.msg;
    } else {
        str_cpy(t, "T", sizeof t);
        utoa10(t + 1, ui.track + 1u);
        cat(t, " G", sizeof t);
        utoa10(t + str_len(t), vb_ngen ? vb_curgen + 1u : 0u);
        if (vb_pl.playing)
            cat(t, " >", sizeof t);
        right = t;
    }
    {
        uint32_t sc = md_text_w(&MD_F3, 2, right) > 110 ? 1u : 2u;
        md_text(232 - md_text_w(&MD_F3, sc, right), sc == 2u ? 15 : 18, &MD_F3, sc, right, C_INK);
    }
    cv_rect(0, 37, 240, 3, C_INK);
    cv_blit(0, 0);
}

static void chips(const char *const *names, uint32_t n, uint32_t sel, uint32_t lit_mask)
{
    uint32_t i, w = 232u / n;
    for (i = 0; i < n; i++) {
        int32_t x = 4 + (int32_t)(i * w);
        int s = i == sel;
        rbox(x, 3, (int32_t)w - 3, 22, 4, 2, C_INK, s ? C_INK : C_BG);   /* 22 high: the 2x text (10 px) in the middle */
        if (!s && ((lit_mask >> i) & 1u))
            cv_rect(x + 3, 19, (int32_t)w - 9, 3, C_INK);
        text_fit(x, (int32_t)w - 3, 9, 2, names[i], s ? C_BG : C_INK);
    }
}

static vb_drum_t vb_prev;
static int32_t vb_pbuf[80];
static void vis_wave_drum(void)
{
    uint32_t i, x;
    static const char *const VS[8] = {"KCK", "SNR", "CLP", "HAT", "TOM", "RIM", "COW", "CYM"};
    chips(VS, 8, ui.dvoice, 0);
    vb_drum_init(&vb_prev, 0x5EED1234u);
    vb_drum_trigger(&vb_prev, &RC->drum, ui.dvoice, 127);
    for (x = 0; x < 220u; x++) {
        int32_t lo = 0, hi = 0;
        for (i = 0; i < 80u; i++)
            vb_pbuf[i] = 0;
        vb_drum_render(&vb_prev, &RC->drum, vb_pbuf, 80);
        for (i = 0; i < 80u; i++) {
            lo = vb_pbuf[i] < lo ? vb_pbuf[i] : lo;
            hi = vb_pbuf[i] > hi ? vb_pbuf[i] : hi;
        }
        {
            int32_t y0 = 74 - vb_clamp(hi, -32767, 32767) * 44 / 32768, y1 = 74 - vb_clamp(lo, -32767, 32767) * 44 / 32768;
            cv_rect(10 + (int32_t)x, y0, 1, y1 - y0 + 1, C_INK);
        }
    }
}
static void vis_print(void)
{
    const int16_t *b = vb_pl.pbuf[vb_pl.cur];
    uint32_t len = vb_pl.plen[vb_pl.cur], x, i;
    char t[24];
    if (!len) {
        text_c(0, 240, 40, 4, "NOTHING YET", C_INK);
        text_c(0, 240, 74, 2, "REC TO RESAMPLE", C_INK);
        return;
    }
    for (x = 0; x < 220u; x++) {
        uint32_t a = len * x / 220u, e = len * (x + 1u) / 220u;
        int32_t lo = 0, hi = 0;
        for (i = a; i < e; i++) {
            lo = b[i] < lo ? b[i] : lo;
            hi = b[i] > hi ? b[i] : hi;
        }
        cv_rect(10 + (int32_t)x, 56 - hi * 44 / 32768, 1, (hi - lo) * 44 / 32768 + 1, C_INK);
    }
    if (RC->src == VB_SRC_SAMP) {
        int32_t s = (int32_t)(RC->samp[0] * 220u / 127u), l = (int32_t)(RC->samp[1] * 220u / 127u);
        cv_rect(10 + s, 8, 2, 96, C_INK);
        cv_rect(10 + (s + l > 219 ? 219 : s + l), 8, 2, 96, C_INK);
    } else if (RC->src == VB_SRC_GRAN) {                   /* POS, and the SPRAY around it */
        int32_t s = (int32_t)(RC->gran[0] * 220u / 127u), w = (int32_t)(RC->gran[3] * 110u / 127u);
        cv_rect(10 + s, 4, 3, 104, C_INK);
        cv_hdot(10 + (s - w < 0 ? 0 : s - w), 10 + (s + w > 219 ? 219 : s + w), 6, C_INK);
    } else if (RC->src == VB_SRC_CHOP) {                   /* where it cuts, and how many */
        static uint16_t st[VB_CH_MAXSLICES];
        char m[24] = "";
        int n = vb_chop_cached(RC->chop, st, VB_CH_MAXSLICES), k;
        for (k = 1; k < n; k++)
            cv_vdot(10 + (int32_t)(st[k] * 220u / len), 4, 104, C_INK);
        if (n < 0) {
            str_cpy(m, "PLAY A KEY", sizeof m);
        } else {
            utoa10(m, (uint32_t)n);
            cat(m, RC->chop[VB_CH_SPLIT] == VB_CH_EVEN ? " EVEN SLICES" : " SLICES", sizeof m);
        }
        md_text(10, 4, &MD_F3, 2, m, C_INK);
    }
    {
        uint32_t tenths = len * 10u / 22050u;
        utoa10(t, tenths / 10u);
        cat(t, ".", sizeof t);
        utoa10(t + str_len(t), tenths % 10u);
    }
    cat(t, "S", sizeof t);
    text_c(0, 240, 108, 2, t, C_INK);
}

/* where each operator sits in the drawing of each algorithm: column << 4 | row, for A B C D (the mockup's ALG) */
static const uint8_t ALG_POS[VB_OP4_ALGS][4] = {
    {0x13, 0x12, 0x11, 0x10}, {0x13, 0x12, 0x01, 0x21}, {0x13, 0x22, 0x02, 0x01}, {0x13, 0x22, 0x02, 0x21},
    {0x23, 0x03, 0x02, 0x01}, {0x23, 0x22, 0x03, 0x02}, {0x03, 0x13, 0x23, 0x12}, {0x13, 0x23, 0x03, 0x02},
    {0x13, 0x02, 0x12, 0x22}, {0x03, 0x23, 0x12, 0x11}, {0x03, 0x13, 0x23, 0x33},
};
#define OPX(o) (8 + (int32_t)(ALG_POS[alg][o] >> 4) * 27)
#define OPY(o) (4 + (int32_t)(ALG_POS[alg][o] & 15u) * 26)
static void vis_op4(void)
{
    char t[24];
    uint32_t op = ui.opsel & 3u, alg = RC->op4[VB_OP4_ALG] % VB_OP4_ALGS, i;
    const uint8_t *o = &RC->op4[op * VB_OP4_PER_OP];
    const char *e;
    static const char *const OPL[4] = {"A", "B", "C", "D"};
    for (e = vb_op4_alg_edges[alg]; e[0] && e[1]; e += e[2] ? 3 : 2) {   /* modulator -> target */
        uint32_t f = (uint32_t)(e[0] - 'A'), g = (uint32_t)(e[1] - 'A');
        line2(OPX(f) + 9, OPY(f) + 20, OPX(g) + 9, OPY(g), C_INK);
    }
    for (e = vb_op4_alg_carriers[alg]; *e; e++) {                       /* carriers -> the output */
        uint32_t c = (uint32_t)(*e - 'A');
        cv_rect(OPX(c) + 9, OPY(c) + 20, 2, 113 - OPY(c) - 20, C_INK);
    }
    cv_rect(4, 113, 104, 4, C_INK);
    {   /* the feedback loop on D */
        int32_t dx = OPX(3), dy = OPY(3);
        cv_rect(dx + 20, dy + 6, 6, 2, C_INK);
        cv_rect(dx + 24, dy - 4, 2, 12, C_INK);
        cv_rect(dx + 10, dy - 4, 16, 2, C_INK);
        cv_rect(dx + 10, dy - 4, 2, 4, C_INK);
    }
    for (i = 0; i < 4u; i++) {
        int s = i == op;
        rbox(OPX(i), OPY(i), 20, 20, 3, 2, C_INK, s ? C_INK : C_BG);
        text_c(OPX(i), 20, OPY(i) + 5, 2, OPL[i], s ? C_BG : C_INK);
    }
    md_text(120, 4, &MD_F3, 9, OPL[op], C_INK);
    str_cpy(t, "ALG ", sizeof t);
    utoa10(t + 4, alg + 1u);
    md_text(120 + (int32_t)md_text_w(&MD_F3, 9, OPL[op]) + 8, 34, &MD_F3, 2, t, C_INK);
    str_cpy(t, vb_op4.desc.params[op * VB_OP4_PER_OP].names ? vb_op4.desc.params[op * VB_OP4_PER_OP].names[o[0]] : "SIN", sizeof t);
    md_text(120, 56, &MD_F3, 3, t, C_INK);
    if (o[VB_OP4_FIX]) {
        utoa10(t, (uint32_t)(o[1] * 100 + o[2] * 10));
        cat(t, "HZ", sizeof t);
    } else {                                                            /* the ratio: CRS (0 = 0.5) + FINE / 100 */
        uint32_t r100 = (o[1] ? o[1] * 100u : 50u) + o[2];
        str_cpy(t, "X", sizeof t);
        utoa10(t + 1, r100 / 100u);
        cat(t, ".", sizeof t);
        t[str_len(t) + 1] = 0;
        t[str_len(t)] = (char)('0' + r100 / 10u % 10u);
        t[str_len(t) + 1] = 0;
        t[str_len(t)] = (char)('0' + r100 % 10u);
    }
    md_text(120, 78, &MD_F3, 3, t, C_INK);
    adsr(120, 98, 112, 22, o[VB_OP4_ATK], o[VB_OP4_DEC], o[VB_OP4_SUS], o[VB_OP4_REL]);
}
#undef OPX
#undef OPY

static void vis_anlg(void)
{
    char t[24];
    const char *const *w = vb_anlg.desc.params[0].names;
    str_cpy(t, w ? w[RC->anlg[0]] : "?", sizeof t);
    cat(t, " + ", sizeof t);
    cat(t, w ? w[RC->anlg[1]] : "?", sizeof t);
    text_fit(0, 240, 14, 4, t, C_INK);
    /* the filter: cutoff as a bar, resonance as its peak */
    {
        int32_t cx = 10 + RC->anlg[8] * 220 / 127, pk = RC->anlg[9] * 30 / 127;
        cv_rect(10, 84, cx - 10, 3, C_INK);
        cv_rect(cx - 2, 84 - pk, 4, pk + 3, C_INK);
        cv_line(cx, 86, cx + 40 > 230 ? 230 : cx + 40, 110, C_INK);
    }
    text_c(0, 240, 112, 2, "LP24 FILTER", C_INK);
}

static void vis_play(void)
{
    vb_play_t *pl = &RC->play;
    char t[24];
    uint32_t i;
    if (pl->mode >= VB_PLAY_CHORD) {
        pl = PLC();                                        /* (CHORDS on GLO: another track's, edited here) */
        uint32_t tot = 0, x = 4;
        for (i = 0; i < pl->len; i++)
            tot += vb_len16[pl->chord[i].len_i % 12u];
        for (i = 0; i < pl->len; i++) {
            const vb_chord_t *ch = &pl->chord[i];
            uint32_t w = vb_len16[ch->len_i % 12u] * 232u / (tot ? tot : 1u);
            int s = i == pl->sel, play = vb_pl.playing && vb_pl.step == i;
            rbox((int32_t)x, 6, (int32_t)w - 2, 40, 4, s ? 4 : 2, C_INK, play ? C_INK : C_BG);
            if (w > 30u) {
                vb_chord_name(ch, t, 8);
                text_fit((int32_t)x, (int32_t)w - 2, 20, 2, t, play ? C_BG : C_INK);
            }
            x += w;
        }
        vb_chord_name(&pl->chord[pl->sel % VB_CHORD_STEPS], t, 16);
        {
            uint32_t sc = 5;                                /* long names (Cm7(9,11)) one size down, then more */
            while (sc > 2u && md_text_w(&MD_F3, sc, t) > 136)
                sc--;
            md_text(8, 58 + (int32_t)(5u - sc) * 5 / 2, &MD_F3, sc, t, C_INK);
        }
        vb_chord_roman(&pl->chord[pl->sel % VB_CHORD_STEPS], RC->key, RC->scale, t, sizeof t);   /* the degree in KEY */
        md_text(150, 52, &MD_F3, md_text_w(&MD_F3, 2, t) > 86 ? 1 : 2, t, C_INK);
        str_cpy(t, vb_len_name[pl->chord[pl->sel].len_i % 12u], sizeof t);
        md_text(150, 68, &MD_F3, 2, t, C_INK);
        str_cpy(t, RC->play.mode == VB_PLAY_ARP ? vb_arp_name[RC->arp_pat % VB_ARPS] : vb_strum_name[RC->play.strum], sizeof t);
        md_text(150, 84, &MD_F3, 2, t, C_INK);
        if (pl != &RC->play) {                             /* following: whose chords these are */
            static const char *const FROM[VB_TRACKS] = {"T1 CHORDS", "T2 CHORDS", "T3 CHORDS", "T4 CHORDS"};
            md_text(150, 36, &MD_F3, 1, FROM[(vb_hsrc - 1u) % VB_TRACKS], C_INK);
        }
        {   /* the BASS, then the notes */
            const vb_chord_t *ch = &pl->chord[pl->sel % VB_CHORD_STEPS];
            char ln[64], nn[6];
            ln[0] = 0;
            if (vb_chord_bass(ch)) {
                note_name(nn, vb_chord_bass(ch));
                cat(ln, nn, sizeof ln);
                cat(ln, " |", sizeof ln);
            }
            for (i = 0; i < ch->n; i++) {
                note_name(nn, ch->note[i]);
                if (ln[0])
                    cat(ln, " ", sizeof ln);
                cat(ln, nn, sizeof ln);
            }
            if (!ch->n)
                str_cpy(ln, RC->cin ? "C4-B4: CHORDS I-VII" : "KEYS ADD NOTES", sizeof ln);
            {
                uint32_t sc = 2;
                if (md_text_w(&MD_F3, 2, ln) > 228)
                    sc = 1;
                md_text(8, sc == 2u ? 102 : 104, &MD_F3, sc, ln, C_INK);
            }
        }
    } else if (pl->mode == VB_PLAY_SEQ) {
        for (i = 0; i < VB_SEQ_STEPS; i++) {
            const vb_step_t *st = &pl->seq[i];
            int32_t x = 4 + (int32_t)(i % 8u) * 29, y = 8 + (int32_t)(i / 8u) * 40;
            int s = i == pl->sel, play = vb_pl.playing && vb_pl.step == i;
            rbox(x, y, 27, 34, 4, s ? 4 : 2, C_INK, play ? C_INK : C_BG);
            if (st->on && i < pl->len)
                text_fit(x, 27, y + 12, 2, NOTE_NM[st->note % 12], play ? C_BG : C_INK);
        }
        note_name(t, pl->seq[pl->sel].note);
        md_text(8, 94, &MD_F3, 4, t, C_INK);
    } else {
        text_c(0, 240, 30, 5, "PLAY", C_INK);
        text_c(0, 240, 70, 4, "THE KEYS", C_INK);
    }
}

static int32_t lfo_shape(uint32_t wave, uint32_t ph)              /* Q15, as core/vb_mod.c draws a cycle */
{
    int32_t x = (int32_t)(ph >> 16);
    switch (wave) {
    case VB_LFO_SIN: return vb_sin(ph);
    case VB_LFO_TRI: return x < 32768 ? -32767 + x * 2 : 32767 - (x - 32768) * 2;
    case VB_LFO_SQR: return ph < 0x80000000u ? 32767 : -32767;
    case VB_LFO_SAW: return 32767 - x;
    case VB_LFO_RAMP: return x - 32767;
    case VB_LFO_SH: { uint32_t k = ph >> 29; return (int32_t)((k * 9301u + 49297u) % 233280u * 65534u / 233280u) - 32767; }
    default: { int32_t e = 32767 - (int32_t)(ph >> 17), cube = ((e * e) >> 15) * e >> 15; return cube * 2 - 32767; }
    }
}
#define MOD_CY 55                                                       /* the curve's middle line */
static void vis_mod(void)
{
    static const char *const MS[9] = {"L1", "L2", "L3", "L4", "L5", "L6", "E1", "E2", "E3"};
    uint32_t i, lit = 0, k, src = ui.op % VB_MODSRC;
    const vb_modsrc_t *m = &RC->mod[src];
    char t[32];
    for (i = 0; i < VB_MODSRC; i++)
        for (k = 0; k < VB_ROUTES; k++)
            if (RC->mod[i].r[k].dest != VB_ADDR_NONE && RC->mod[i].r[k].amt)
                lit |= 1u << i;
    chips(MS, 9, src, lit);
    str_cpy(t, m->kind ? "ENV " : "LFO ", sizeof t);
    utoa10(t + 4, m->kind ? src - 5u : src + 1u);
    md_text(8, 30, &MD_F3, 3, t, C_INK);
    if (!m->kind) {                                                     /* SIN 1/4 FREE */
        char v[12];
        str_cpy(t, LFOW_N[m->wave % 7u], sizeof t);
        cat(t, " ", sizeof t);
        cell_text(&C[0][1], v);
        cat(t, v, sizeof t);
        cat(t, " ", sizeof t);
        cat(t, TRIG_N[m->trig % 5u], sizeof t);
    } else {
        str_cpy(t, "ADSR ", sizeof t);
        cat(t, ELOOP_N[m->loop % 3u], sizeof t);
    }
    {
        uint32_t sc = md_text_w(&MD_F3, 2, t) > 130 ? 1u : 2u;
        md_text(232 - (int32_t)md_text_w(&MD_F3, sc, t), sc == 2u ? 33 : 35, &MD_F3, sc, t, C_INK);
    }
    if (!m->kind) {                                                     /* one cycle, and where the LFO is now */
        int32_t px = 0, py = 0, x;
        for (x = 0; x <= 220; x += 2) {
            int32_t y = MOD_CY - lfo_shape(m->wave, (uint32_t)x * 19522579u) * 8 / 32768;
            if (x)
                line2(10 + px, py, 10 + x, y, C_INK);
            px = x;
            py = y;
        }
        if (src < 6u) {
            uint32_t ph = vb_pl.lfo[src].ph;
            int32_t cx = 10 + (int32_t)((ph >> 24) * 220u / 256u), cy = MOD_CY - lfo_shape(m->wave, ph) * 8 / 32768;
            disc(cx, cy, 5, C_INK);
            disc(cx, cy, 3, C_BG);
        }
    } else {
        adsr(10, MOD_CY - 9, 220, 19, m->atk, m->dec, m->sus, m->rel);
    }
    for (k = 0; k < VB_ROUTES; k++) {
        int32_t y = 70 + (int32_t)k * 13, w;
        int lrn = ui.learn_src == (int)src && ui.learn_slot == (int)k;
        if (lrn)
            rfill(4, y - 2, 232, 13, 3, C_INK);
        t[0] = (char)('1' + k);
        t[1] = 0;
        md_text(8, y, &MD_F3, 2, t, lrn ? C_BG : C_INK);
        if (lrn)
            str_cpy(t, "TURN A KNOB", sizeof t);
        else
            vb_addr_name(RC, m->r[k].dest, t, 16);
        md_text(24, y, &MD_F3, 2, t, lrn ? C_BG : C_INK);
        if (lrn)
            continue;
        rbox(162, y, 68, 10, 5, 2, C_INK, C_BG);                       /* a pill: the amount from its middle */
        w = m->r[k].dest == VB_ADDR_NONE ? 0 : m->r[k].amt * 31 / 64;
        if (w > 0)
            cv_rect(196, y + 2, w, 6, C_INK);
        else if (w < 0)
            cv_rect(196 + w, y + 2, -w, 6, C_INK);
        cv_rect(195, y + 1, 2, 8, C_INK);
    }
}

static void vis_fx(void)
{
    const char *sn[VB_FX_SLOTS];
    uint32_t i, lit = 0;
    char t[48];
    const vb_fx_slot_t *f = &RC->fx[ui.op & 7u];
    for (i = 0; i < VB_FX_SLOTS; i++) {
        sn[i] = RC->fx[i].type ? vb_fx_types[RC->fx[i].type].shortname : ".";
        lit |= (RC->fx[i].type && RC->fx[i].on) ? 1u << i : 0u;
    }
    chips(sn, VB_FX_SLOTS, ui.op & 7u, lit);
    str_cpy(t, "SLOT ", sizeof t);
    utoa10(t + 5, (ui.op & 7u) + 1u);
    md_text(8, 30, &MD_F3, 2, t, C_INK);
    text_fit(0, 240, 46, 5, f->type ? vb_fx_types[f->type].name : "EMPTY", C_INK);
    t[0] = 0;
    for (i = 0; i < VB_FX_SLOTS; i++)
        if (RC->fx[i].type && RC->fx[i].on) {
            if (t[0])
                cat(t, ">", sizeof t);
            cat(t, vb_fx_types[RC->fx[i].type].shortname, sizeof t);
        }
    text_fit(0, 240, 96, 2, t[0] ? t : "SEL = SCRAMBLE", C_INK);
}

static void vis_print_page(void)
{
    char t[24];
    cv_ring(52, 58, 40, C_INK);
    cv_ring(52, 58, 39, C_INK);
    {
        int32_t dy, dx;
        for (dy = -30; dy <= 30; dy++) {                   /* a solid disc: the label reads on it */
            for (dx = 0; dx * dx + dy * dy <= 900; dx++)
                ;
            cv_rect(52 - dx + 1, 58 + dy, 2 * dx - 1, 1, C_INK);
        }
    }
    if (ui.printing) {
        text_c(12, 80, 52, 2, "REC", C_BG);
    } else {
        text_c(12, 80, 46, 2, "RE", C_BG);
        text_c(12, 80, 60, 2, "SAMPLE", C_BG);
    }
    str_cpy(t, "G", sizeof t);
    utoa10(t + 1, vb_ngen + 1u);
    md_text(112, 30, &MD_F3, 2, "NEXT", C_INK);
    md_text(112, 46, &MD_F3, 5, t, C_INK);
    if (vb_pl.rec && vb_pl.rec_target) {
        rbox(112, 92, 118, 12, 6, 2, C_INK, C_BG);
        rfill(114, 94, 4 + (int32_t)(vb_pl.rec_pos * 110u / vb_pl.rec_target), 8, 4, C_INK);
    } else {
        md_text(112, 96, &MD_F3, 2, "REC ANYWHERE", C_INK);
    }
}

static void vis_songs(void)
{
    char t[40];
    uint32_t i, first = ui.song > 1u ? ui.song - 1u : 0u, k;
    if (first + 4u > VB_SONGS)
        first = VB_SONGS - 4u;
    md_text(8, 6, &MD_F3, 2, "4 TRACKS, THEIR PARTS, BPM", C_INK);
    for (i = first; i < first + 4u; i++) {
        int32_t y = 26 + (int32_t)(i - first) * 19;
        int s = i == ui.song;
        vb_song_t *g = &vb_song;                            /* (the song buffer: only used here and in LOAD / SAVE) */
        if (s)
            rfill(4, y - 3, 232, 17, 4, C_INK);
        str_cpy(t, "S", sizeof t);
        utoa10(t + 1, i + 1u);
        cat(t, i < 9u ? "  " : " ", sizeof t);
        if (song_read(i, g)) {
            cat(t, "-", sizeof t);
        } else {
            for (k = 0; k < VB_TRACKS; k++) {
                char n[4];
                utoa10(n, g->snd[k] + 1u);
                cat(t, n, sizeof t);
                cat(t, k + 1u < VB_TRACKS ? "." : " ", sizeof t);
            }
            utoa10(t + str_len(t), g->bpm10 / 10u);
        }
        md_text(10, y, &MD_F3, 2, t, s ? C_BG : C_INK);
    }
    md_text(8, 106, &MD_F3, 2, "SEL = LOAD / SAVE", C_INK);
}

static void vis_bank(void)
{
    if (ui.row[PG_BANK] == 1) {
        vis_songs();
        return;
    }
    char t[24];
    uint32_t i, first = vb_curgen > 3u ? vb_curgen - 3u : 0u;
    str_cpy(t, "SOUND ", sizeof t);
    utoa10(t + 6, vb_track_sound[ui.track] + 1u);
    cat(t, "/64", sizeof t);
    md_text(8, 6, &MD_F3, 3, t, C_INK);
    if (!vb_ngen)
        md_text(8, 50, &MD_F3, 2, "NOTHING RESAMPLED", C_INK);
    for (i = first; i < vb_ngen && i < first + 5u; i++) {
        int32_t y = 30 + (int32_t)(i - first) * 18;
        int s = i == vb_curgen;
        if (s)
            rfill(4, y - 3, 232, 16, 4, C_INK);
        str_cpy(t, "G", sizeof t);
        utoa10(t + 1, i + 1u);
        cat(t, " ", sizeof t);
        cat(t, SRC_N[vb_gen[i].rc.src], sizeof t);
        if (vb_gen[i].audio)
            cat(t, " *", sizeof t);
        md_text(10 + (vb_gen[i].parent >= 0 ? 12 : 0), y, &MD_F3, 2, t, s ? C_BG : C_INK);
    }
}

static void vis_glo(void)
{
    char t[24];
    utoa10(t, (uint32_t)vb_pl.bpm10 / 10u);
    md_text(10, 10, &MD_F3, 9, t, C_INK);
    md_text(10, 62, &MD_F3, 3, "BPM", C_INK);
    str_cpy(t, "CPU ", sizeof t);
    utoa10(t + 4, vb_dbg.last_us * 100u / 5805u);
    cat(t, "%", sizeof t);
    md_text(10, 84, &MD_F3, 2, t, C_INK);
    str_cpy(t, "CHAOS ", sizeof t);
    utoa10(t + 6, ui.chaos);
    cat(t, "%", sizeof t);
    md_text(120, 84, &MD_F3, 2, t, C_INK);
    {   /* the tracks: filled while heard, the selected one in a thick frame */
        uint32_t k;
        for (k = 0; k < VB_TRACKS; k++) {
            int32_t x = 10 + (int32_t)k * 56;
            static const char *const TN[VB_TRACKS] = {"T1", "T2", "T3", "T4"};
            rbox(x, 106, 50, 16, 4, k == ui.track ? 3 : 1, C_INK, vb_tmute[k] ? C_BG : C_INK);
            text_c(x, 50, 108, 2, TN[k], vb_tmute[k] ? C_INK : C_BG);
        }
    }
}

static void draw_vis(void)
{
    cv_begin(240, 124, C_BG);
    switch (ui.page) {
    case PG_SRC:
        if (RC->src == VB_SRC_OP4)
            vis_op4();
        else if (RC->src == VB_SRC_ANLG)
            vis_anlg();
        else if (RC->src == VB_SRC_DRUM)
            vis_wave_drum();
        else
            vis_print();
        break;
    case PG_PLAY: vis_play(); break;
    case PG_MOD: vis_mod(); break;
    case PG_FX: vis_fx(); break;
    case PG_PRINT: vis_print_page(); break;
    case PG_BANK: vis_bank(); break;
    case PG_GLO: vis_glo(); break;
    }
    cv_blit(0, 40);
}

static void draw_prm(void)
{
    uint32_t c, r;
    char b[24];
    build_rows();
    r = ui.row[ui.page];
    cv_begin(240, 76, C_BG);
    cv_rect(0, 0, 240, 3, C_INK);
    for (c = 0; c < 4u; c++) {
        const cell_t *x = &C[r][c];
        int32_t xx = (int32_t)c * 60;
        if (c)
            cv_rect(xx, 10, 1, 58, C_INK);
        if (!x->label) {
            text_c(xx, 60, 14, 2, "-", C_INK);
            continue;
        }
        str_cpy(b, x->label, sizeof b);
        if (routes_to(x->addr))
            cat(b, "~", sizeof b);
        if (ui.learn_src >= 0 && x->addr != VB_ADDR_NONE)
            cv_rect(xx + 4, 8, 52, 2, C_INK);
        text_c(xx, 60, 14, 2, b, C_INK);
        cell_text(x, b);
        text_fit(xx, 60, 38, 4, b, C_INK);
    }
    cv_blit(0, 164);
}

static void draw_hdr(void);
static void vb_draw_now(void)                              /* the header now (long operations show progress) */
{
    draw_hdr();
    lcd_sync();
}

static void vb_draw(void)
{
    static uint8_t had_msg;
    static uint32_t anim;
    uint8_t msg = fm1_ms < ui.msg_until;
    if (ui.force) {
        lcd_fill(0, 0, 240, 240, C_BG);
        ui.force = 0;
        dirty();
    }
    if (had_msg && !msg)
        ui.d_hdr = 1;
    had_msg = msg;
    if ((vb_pl.playing || vb_pl.rec) && fm1_ms - anim > 150u && (ui.page == PG_PLAY || ui.page == PG_PRINT)) {
        anim = fm1_ms;
        ui.d_vis = 1;
    }
    if (ui.d_prm) {
        ui.d_prm = 0;
        draw_prm();
    } else if (ui.d_hdr) {
        ui.d_hdr = 0;
        draw_hdr();
    } else if (ui.d_vis && (int32_t)(fm1_ms - ui.vis_due) >= 0) {
        ui.d_vis = 0;
        draw_vis();
    } else if (ui.page == PG_MOD && ui.op < 6u && fm1_ms - anim > 50u) {   /* the LFO's dot: its strip only */
        anim = fm1_ms;
        cv_begin_win(0, MOD_CY - 10, 240, 21, 40, C_BG);
        vis_mod();
        cv_blit(0, 40);
    }
}
