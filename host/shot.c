/* SPDX-License-Identifier: GPL-3.0-only */
/* Screenshots of the FM-1 screen without the FM-1: the firmware's UI (fw/vb_app.c) and MNM-1's drawing layer (gfx.c)
 * compiled on the host against a 240x240 framebuffer. Each scenario drives the UI through the same entry points the
 * panel and the remote use (press, key, encoders, cell_set) and writes host/shots/<name>.png.
 *
 *   make shots            (MNM=<the MNM-1 tree>, default ..: gfx.c and the generated font) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

/* ---- what the platform gives vb_app.c */
#define CTL 32u
#define FELUCCA_VERSION "veffbox host"
static uint32_t fm1_ms = 1000;
static const int flash_ok = 0;
static int st_read(uint32_t a, void *d, uint32_t n) { (void)a; (void)d; (void)n; return -1; }
static int st_erase(uint32_t a) { (void)a; return -1; }
static int st_prog(uint32_t a, const void *d, uint32_t n) { (void)a; (void)d; (void)n; return -1; }
static void fm1_wdt_feed(void) {}
static void fm1_led_key(uint32_t id, int on) { (void)id; (void)on; }
static uint32_t host_btn;
static uint32_t fm1_input_edges(uint32_t *rel) { uint32_t p = host_btn; host_btn = 0; *rel = 0; return p; }
static uint32_t fm1_input_note_edges(void) { return 0; }
static struct { uint32_t notes; } fm1_in;
static int32_t host_enc[7];
static int32_t enc_take(uint32_t e) { int32_t s = host_enc[e % 7u]; host_enc[e % 7u] = 0; return s; }
#define MQ 64u
static uint32_t midi_in_q[MQ], mi_r, mi_w, midi_in_overflow;
static int ota_frame_get(const uint8_t **p, uint32_t *n) { (void)p; (void)n; return 0; }
static void ota_frame_done(void) {}
static void ota_wire_send(const uint8_t *p, uint32_t n) { (void)p; (void)n; }
static struct { uint32_t in_audio, nested, halves, late, max_us, last_us, timer_irqs, cpu_q8, cpu_rem; } vb_dbg;
static unsigned str_len(const char *s) { return (unsigned)strlen(s); }
static void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i;
    if (!max)
        return;
    for (i = 0; i + 1u < max && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* ---- the LCD */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = (uint16_t)((px[j * w + i] >> 8) | (px[j * w + i] << 8));
}

#define __attribute__(x)                                   /* (the sections are the device's memory map) */
static void fm1_delay_ms(uint32_t ms) { fm1_ms += ms; }
static void vb_remote(void);
#include "gfx.c"
#include "../core/vb_dsp.h"
#include "../core/vb_player.h"
#include "../core/vb_op4.h"
#include "../core/vb_anlg.h"
#include "../core/vb_samp.h"
#include "../core/vb_gran.h"
#include "../core/vb_chop.h"
#include "../core/vb_store.h"
#include "../fw/vb_app.c"

/* ---- PNG, 3x scaled (the screen is small) */
#define SC 3u
static void put_be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t b[4];
    uint32_t c = (uint32_t)crc32(0, (const uint8_t *)type, 4);
    c = (uint32_t)crc32(c, d, n);
    put_be(b, n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    fwrite(d, 1, n, f);
    put_be(b, c);
    fwrite(b, 1, 4, f);
}
static void png(const char *name)
{
    static uint8_t raw[240u * SC * (1u + 240u * SC * 3u)], z[sizeof raw + 1024u];
    uLongf zn = sizeof z;
    uint8_t hdr[13] = {0};
    uint32_t x, y, o = 0;
    char path[128];
    FILE *f;
    for (y = 0; y < 240u * SC; y++) {
        raw[o++] = 0;
        for (x = 0; x < 240u * SC; x++) {
            uint16_t c = fb[(y / SC) * 240u + x / SC];
            raw[o++] = (uint8_t)(((c >> 11) & 31u) * 255u / 31u);
            raw[o++] = (uint8_t)(((c >> 5) & 63u) * 255u / 63u);
            raw[o++] = (uint8_t)((c & 31u) * 255u / 31u);
        }
    }
    compress2(z, &zn, raw, o, 6);
    put_be(hdr, 240u * SC);
    put_be(hdr + 4, 240u * SC);
    hdr[8] = 8;
    hdr[9] = 2;
    snprintf(path, sizeof path, "host/shots/%s.png", name);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", hdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", hdr, 0);
    fclose(f);
}

/* ---- driving the UI */
static void frame(void)
{
    vb_input();
    ui.force = 1;
    ui.vis_due = 0;
    vb_draw();                                             /* one strip per call, as on the device */
    ui.vis_due = 0;
    vb_draw();
    ui.vis_due = 0;
    vb_draw();
    ui.vis_due = 0;
    vb_draw();
}
static void btn(uint32_t b) { host_btn |= 1u << panel.btn[b]; frame(); }
static void enc(uint32_t role, int d) { host_enc[panel.enc[role]] += d * panel.dir[role]; frame(); }
static void shot(const char *name)
{
    frame();
    png(name);
    printf("host/shots/%s.png\n", name);
}
/* every row of the page */
static void rows(const char *base, uint32_t pagebtn)
{
    uint32_t r, n;
    char nm[64];
    build_rows();
    n = NR;
    for (r = 0; r < n; r++) {
        ui.row[ui.page] = (uint8_t)r;
        snprintf(nm, sizeof nm, "%s_r%u", base, (unsigned)r + 1u);
        shot(nm);
    }
    (void)pagebtn;
    ui.row[ui.page] = 0;
}

int main(void)
{
    static const char *const SRCS[6] = {"op4", "anlg", "drum", "samp", "gran", "chop"};
    uint32_t s;
    char nm[64];
    panel = PANEL_DEFAULT;
    vb_theme();
    vb_init();
    frame();
    for (s = 0; s < 6u; s++) {                             /* SRC: every engine, every row */
        RC->src = (uint8_t)s;
        ui.page = PG_SRC;
        snprintf(nm, sizeof nm, "src_%s", SRCS[s]);
        rows(nm, B_EDIT);
    }
    RC->src = VB_SRC_DRUM;
    ui.dvoice = 3;
    shot("src_drum_hat");
    RC->src = VB_SRC_OP4;
    btn(B_ARP);
    for (s = 0; s < 3u; s++) {
        static const char *const M[3] = {"play_note", "play_seq", "play_chord"};
        RC->play.mode = (uint8_t)s;
        rows(M[s], B_ARP);
    }
    btn(B_LFO);
    rows("mod_lfo", B_LFO);
    btn(B_ENV);
    rows("mod_env", B_ENV);
    btn(B_FX);
    rows("fx_empty", B_FX);
    RC->fx[0].type = 1;
    RC->fx[0].on = 1;
    for (s = 1; s < 8u && s < VB_FX_TYPES; s++) {
        RC->fx[s].type = (uint8_t)(1u + (s * 3u) % (VB_FX_TYPES - 1u));
        RC->fx[s].on = 1;
    }
    for (s = 0; s < 8u; s++) {
        ui.op = (uint8_t)s;
        snprintf(nm, sizeof nm, "fx_slot%u", (unsigned)s + 1u);
        rows(nm, B_FX);
    }
    btn(B_HOME);
    rows("print", B_HOME);
    btn(B_SAVE);
    rows("bank", B_SAVE);
    btn(B_GLO);
    rows("glo", B_GLO);
    (void)enc;
    return 0;
}
