/* SPDX-License-Identifier: GPL-3.0-only */
/* veffbox application, first hardware test (0.0.x).
 *
 *   keys            play the 8 DRUM voices (every 8 keys repeat) and select the one the knobs shape
 *   KNOB 1-4        the lit row of parameters; EDIT steps through the rows
 *   SELECT          the voice; ALGORITHM the row
 *   SEL (SCL)       tap = RANDOMIZE the voice, hold 0.6 s = undo the rolls
 *   PLAY            a 16-step test beat at 120 BPM
 *   REC             audition the voice
 *   OCT- + OCT+ 5 s UBOOT (the way back), as on MNM-1
 * The other pages of the mockup (ui/index.html) come in later builds. */

/* ---- panel: which matrix button / encoder carries which label (MNM-1's panel.c defaults) */
enum { B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC, B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };
static const char *const B_NAME[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE",
                                       "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"};
#define PANEL_MAGIC 0x50414E35u
typedef struct {
    uint32_t magic;
    uint8_t btn[NB];
    uint8_t enc[NE];
    int8_t dir[NE];
} panel_t;
panel_t panel __attribute__((section(".noinit")));
static const panel_t PANEL_DEFAULT = {
    PANEL_MAGIC,
    {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1},
    {0, 1, 6, 2, 3, 4, 5},
    {1, 1, 1, 1, 1, 1, 1},
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
static int32_t panel_enc(uint32_t role) { return enc_take(panel.enc[role]) * panel.dir[role]; }

/* ---- sound state: the main loop edits the patch and asks for hits, the audio ISR renders */
static vb_drum_patch_t vb_patch;
static vb_drum_t vb_drum;
static volatile uint32_t vb_master_q12 = 2048;
static volatile uint32_t vb_trig;                    /* bit per voice */
static volatile uint8_t vb_vel[VB_DRUM_VOICES];
static volatile uint8_t vb_playing;
static volatile uint32_t vb_step;
static uint32_t vb_seq_acc;
#define VB_STEP_Q8 1411200u                         /* a 16th at 120 BPM: 5512.5 samples, Q8 */
static const uint16_t VB_PAT[VB_DRUM_VOICES] = {0x1111u, 0x1010u, 0, 0x4444u, 0, 0, 0, 0};   /* kick, snare, hat */
static int32_t vb_mono[CTL];

static void vb_audio_block(int32_t *out, uint32_t n)        /* audio ISR */
{
    uint32_t req = vb_trig, v, i;
    vb_trig = 0;
    if (vb_playing) {
        vb_seq_acc += n << 8;
        while (vb_seq_acc >= VB_STEP_Q8) {
            vb_seq_acc -= VB_STEP_Q8;
            vb_step = (vb_step + 1u) & 15u;
            for (v = 0; v < VB_DRUM_VOICES; v++)
                if ((VB_PAT[v] >> vb_step) & 1u) {
                    vb_vel[v] = (vb_step & 3u) ? 90 : 120;
                    req |= 1u << v;
                }
        }
    }
    for (v = 0; v < VB_DRUM_VOICES; v++)
        if ((req >> v) & 1u)
            vb_drum_trigger(&vb_drum, &vb_patch, (int)v, vb_vel[v]);
    for (i = 0; i < n; i++)
        vb_mono[i] = 0;
    vb_drum_render(&vb_drum, &vb_patch, vb_mono, (int)n);
    for (i = 0; i < n; i++) {
        int32_t s = vb_sat16((vb_mono[i] * (int32_t)(vb_master_q12 >> 1)) >> 11);
        out[2u * i] = s;
        out[2u * i + 1u] = s;
    }
}

static void vb_hit(uint32_t v, uint32_t vel)
{
    uint32_t f = irq_save();
    vb_vel[v] = (uint8_t)vel;
    vb_trig |= 1u << v;
    irq_restore(f);
}
static void vb_all_off(void)
{
    uint32_t f = irq_save();
    vb_playing = 0;
    vb_trig = 0;
    vb_drum_init(&vb_drum, 0x5EED1234u);
    irq_restore(f);
}

/* ---- UI state */
static struct {
    uint8_t voice, row, force, d_hdr, d_wave, d_prm;
    uint32_t wave_due, msg_until, sel_t0;
    char msg[24];
} vb_ui;
static uint8_t vb_undo_p[VB_DRUM_NP + 1];
static int8_t vb_undo_voice = -1;
static uint32_t vb_rng_s = 0x2545F491u, vb_rolls;

static void vb_theme(void)
{
    C_BG = RGB(0xea, 0xa7, 0xcc);                  /* powder pink */
    C_INK = RGB(0x1d, 0x08, 0x19);
}
static void vb_boot_screen(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    draw_text_box(0, 96, 240, &FONT_L, "VEFFBOX", C_INK, 1);
    draw_text_box(0, 140, 240, &FONT_S, FELUCCA_VERSION, C_INK, 1);
}
static void vb_init(void)
{
    vb_drum_patch_default(&vb_patch);
    vb_drum_init(&vb_drum, 0x5EED1234u);
}
static void vb_say(const char *s)
{
    str_cpy(vb_ui.msg, s, sizeof vb_ui.msg);
    vb_ui.msg_until = fm1_ms + 1500u;
    vb_ui.d_hdr = 1;
}
static void vb_utoa(char *b, uint32_t v)
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

/* the knob rows: row 0 VOICE LEVEL p0 p1, row 1 p2..p5, row 2 p6 p7 */
#define VB_ROWS 3u
enum { ID_VOICE = -1, ID_LEVEL = -2, ID_NONE = -3 };
static int vb_cell(uint32_t r, uint32_t c)
{
    if (r == 0)
        return c == 0 ? ID_VOICE : c == 1 ? ID_LEVEL : (int)c - 2;
    if (r == 1)
        return (int)c + 2;
    return c < 2u ? (int)c + 6 : ID_NONE;
}
static int vb_get(int id)
{
    uint32_t v = vb_ui.voice;
    return id == ID_VOICE ? (int)v : id == ID_LEVEL ? vb_patch.lvl[v] : vb_patch.p[v][id];
}
static const char *vb_label(int id)
{
    return id == ID_VOICE ? "VOICE" : id == ID_LEVEL ? "LEVEL" : vb_drum_voices[vb_ui.voice].p[id].label;
}
static void vb_set(int id, int x)
{
    uint32_t v = vb_ui.voice;
    if (id == ID_VOICE) {
        vb_ui.voice = (uint8_t)(x < 0 ? 0 : x > VB_DRUM_VOICES - 1 ? VB_DRUM_VOICES - 1 : x);
        vb_ui.d_hdr = vb_ui.d_prm = vb_ui.d_wave = 1;
        return;
    }
    x = x < 0 ? 0 : x > 127 ? 127 : x;
    if (id == ID_LEVEL)
        vb_patch.lvl[v] = (uint8_t)x;
    else
        vb_patch.p[v][id] = (uint8_t)x;
    vb_undo_voice = -1;                             /* a hand edit ends the series of rolls */
    vb_ui.d_prm = vb_ui.d_wave = 1;
}

/* RANDOMIZE the voice: every parameter moves 70 % of the way to a random value (the mockup's CHAOS 70 %) */
static void vb_randomize(void)
{
    uint32_t v = vb_ui.voice, i;
    char b[24];
    if (vb_undo_voice != (int8_t)v) {
        for (i = 0; i < VB_DRUM_NP; i++)
            vb_undo_p[i] = vb_patch.p[v][i];
        vb_undo_p[VB_DRUM_NP] = vb_patch.lvl[v];
        vb_undo_voice = (int8_t)v;
    }
    for (i = 0; i < VB_DRUM_NP; i++) {
        int32_t cur = vb_patch.p[v][i], tgt = (int32_t)(vb_rng(&vb_rng_s) % 128u);
        vb_patch.p[v][i] = (uint8_t)(cur + (tgt - cur) * 70 / 100);
    }
    vb_hit(v, 110);
    vb_rolls++;
    str_cpy(b, "RANDOM #", sizeof b);
    vb_utoa(b + 8, vb_rolls);
    vb_say(b);
    vb_ui.d_prm = vb_ui.d_wave = 1;
}
static void vb_undo(void)
{
    uint32_t i;
    if (vb_undo_voice < 0) {
        vb_say("NOTHING TO UNDO");
        return;
    }
    vb_ui.voice = (uint8_t)vb_undo_voice;
    for (i = 0; i < VB_DRUM_NP; i++)
        vb_patch.p[vb_ui.voice][i] = vb_undo_p[i];
    vb_patch.lvl[vb_ui.voice] = vb_undo_p[VB_DRUM_NP];
    vb_undo_voice = -1;
    vb_hit(vb_ui.voice, 110);
    vb_say("UNDO");
    vb_ui.d_prm = vb_ui.d_wave = vb_ui.d_hdr = 1;
}

static void vb_press(uint32_t b)
{
    char m[24];
    switch (b) {
    case B_EDIT:
        vb_ui.row = (uint8_t)((vb_ui.row + 1u) % VB_ROWS);
        vb_ui.d_prm = vb_ui.d_hdr = 1;
        break;
    case B_PLAY: {
        uint32_t f = irq_save();
        vb_playing = !vb_playing;
        vb_step = 15;
        vb_seq_acc = VB_STEP_Q8;                    /* the first step plays at once */
        irq_restore(f);
        vb_say(vb_playing ? "PLAY 120 BPM" : "STOP");
        break;
    }
    case B_REC:
        vb_hit(vb_ui.voice, 120);
        break;
    case B_SCL:
        vb_ui.sel_t0 = fm1_ms ? fm1_ms : 1u;        /* acted on at release: tap or hold */
        break;
    case B_OCTDN: case B_OCTUP:
        break;
    default:
        str_cpy(m, B_NAME[b], sizeof m);
        str_cpy(m + str_len(m), " SOON", sizeof m - str_len(m));
        vb_say(m);
        break;
    }
}

static void vb_input(void)
{
    uint32_t rel, pr = fm1_input_edges(&rel), id, notes, c;
    int32_t d;
    for (id = 0; id < 14u; id++) {
        uint32_t b = panel_btn_of(id);
        if (b == NB)
            continue;
        if ((pr >> id) & 1u)
            vb_press(b);
        if (((rel >> id) & 1u) && b == B_SCL && vb_ui.sel_t0) {
            if (fm1_ms - vb_ui.sel_t0 >= 600u)
                vb_undo();
            else
                vb_randomize();
            vb_ui.sel_t0 = 0;
        }
    }
    notes = fm1_input_note_edges();
    for (id = 0; notes; id++, notes >>= 1)
        if (notes & 1u) {
            uint32_t v = id % VB_DRUM_VOICES;
            vb_hit(v, 110);
            if (v != vb_ui.voice) {
                vb_ui.voice = (uint8_t)v;
                vb_ui.d_hdr = vb_ui.d_prm = vb_ui.d_wave = 1;
            }
        }
    if ((d = panel_enc(EN_SELECT)) != 0)
        vb_set(ID_VOICE, (int)vb_ui.voice + (d > 0 ? 1 : -1));
    if ((d = panel_enc(EN_ALGO)) != 0) {
        vb_ui.row = (uint8_t)((vb_ui.row + VB_ROWS + (d > 0 ? 1u : VB_ROWS - 1u)) % VB_ROWS);
        vb_ui.d_prm = vb_ui.d_hdr = 1;
    }
    (void)panel_enc(EN_PRESET);
    for (c = 0; c < 4u; c++) {
        int idc = vb_cell(vb_ui.row, c);
        d = panel_enc(EN_K1 + c);
        if (!d || idc == ID_NONE)
            continue;
        if (idc == ID_VOICE)
            vb_set(idc, vb_get(idc) + (d > 0 ? 1 : -1));
        else
            vb_set(idc, vb_get(idc) + d * (d > 1 || d < -1 ? 4 : 1));   /* a quick turn moves faster */
        vb_ui.wave_due = fm1_ms + 120u;
    }
}

static void vb_leds(void)
{
    panel_led(B_PLAY, vb_playing);
    panel_led(B_EDIT, 1);
    panel_led(B_SCL, vb_undo_voice >= 0);
    panel_led(B_REC, 0);
}

/* ---- drawing: three strips, at most one sent per frame (an LCD transfer is the cost) */
static void vb_text_c(int32_t x, int32_t w, int32_t y, uint32_t sc, const char *s, uint16_t c)
{
    md_text(x + (w - md_text_w(&MD_F3, sc, s)) / 2, y, &MD_F3, sc, s, c);
}

static void vb_draw_hdr(void)
{
    const char *right = fm1_ms < vb_ui.msg_until ? vb_ui.msg : vb_drum_voices[vb_ui.voice].name;
    uint32_t r;
    cv_begin(240, 40, C_BG);
    md_text(10, 9, &MD_F3, 4, "DRUM", C_INK);
    for (r = 0; r < VB_ROWS; r++) {
        cv_rect(92 + (int32_t)r * 10, 16, 7, 7, C_INK);
        if (r != vb_ui.row)
            cv_rect(93 + (int32_t)r * 10, 17, 5, 5, C_BG);
    }
    md_text(230 - md_text_w(&MD_F3, 2, right), 15, &MD_F3, 2, right, C_INK);
    cv_rect(0, 37, 240, 3, C_INK);
    cv_blit(0, 0);
}

/* the voice's real shape: one hit rendered with the core, min/max per column */
static vb_drum_t vb_preview;
static int32_t vb_pbuf[80];
static void vb_draw_wave(void)
{
    uint32_t i, x;
    vb_drum_init(&vb_preview, 0x5EED1234u);
    vb_drum_trigger(&vb_preview, &vb_patch, vb_ui.voice, 127);
    cv_begin(240, 124, C_BG);
    for (i = 0; i < VB_DRUM_VOICES; i++) {
        int32_t cx = 4 + (int32_t)i * 29;
        int sel = i == vb_ui.voice;
        cv_rect(cx, 4, 27, 18, C_INK);
        if (!sel)
            cv_rect(cx + 2, 6, 23, 14, C_BG);
        vb_text_c(cx, 27, 8, 2, vb_drum_voices[i].shortname, sel ? C_BG : C_INK);
    }
    for (x = 0; x < 220u; x++) {                     /* 0.4 s: 220 columns of 80 samples */
        int32_t lo = 0, hi = 0, y0, y1;
        for (i = 0; i < 80u; i++)
            vb_pbuf[i] = 0;
        vb_drum_render(&vb_preview, &vb_patch, vb_pbuf, 80);
        for (i = 0; i < 80u; i++) {
            lo = vb_pbuf[i] < lo ? vb_pbuf[i] : lo;
            hi = vb_pbuf[i] > hi ? vb_pbuf[i] : hi;
        }
        y0 = 74 - vb_clamp(hi, -32767, 32767) * 44 / 32768;
        y1 = 74 - vb_clamp(lo, -32767, 32767) * 44 / 32768;
        cv_rect(10 + (int32_t)x, y0, 1, y1 - y0 + 1, C_INK);
    }
    cv_blit(0, 40);
}

static void vb_draw_prm(void)
{
    uint32_t c;
    char b[12];
    cv_begin(240, 76, C_BG);
    cv_rect(0, 0, 240, 3, C_INK);
    for (c = 0; c < 4u; c++) {
        int id = vb_cell(vb_ui.row, c);
        int32_t x = (int32_t)c * 60;
        if (c)
            cv_rect(x, 10, 1, 58, C_INK);
        if (id == ID_NONE) {
            vb_text_c(x, 60, 14, 2, "-", C_INK);
            continue;
        }
        if (id == ID_VOICE)
            str_cpy(b, vb_drum_voices[vb_ui.voice].shortname, sizeof b);
        else
            vb_utoa(b, (uint32_t)vb_get(id));
        vb_text_c(x, 60, 14, 2, vb_label(id), C_INK);
        vb_text_c(x, 60, 38, 4, b, C_INK);
    }
    cv_blit(0, 164);
}

static void vb_draw(void)
{
    static uint8_t had_msg;
    uint8_t msg = fm1_ms < vb_ui.msg_until;
    if (vb_ui.force) {
        lcd_fill(0, 0, 240, 240, C_BG);
        vb_ui.force = 0;
        vb_ui.d_hdr = vb_ui.d_wave = vb_ui.d_prm = 1;
    }
    if (had_msg && !msg)
        vb_ui.d_hdr = 1;
    had_msg = msg;
    if (vb_ui.d_prm) {
        vb_ui.d_prm = 0;
        vb_draw_prm();
    } else if (vb_ui.d_hdr) {
        vb_ui.d_hdr = 0;
        vb_draw_hdr();
    } else if (vb_ui.d_wave && (int32_t)(fm1_ms - vb_ui.wave_due) >= 0) {
        vb_ui.d_wave = 0;
        vb_draw_wave();
    }
}
