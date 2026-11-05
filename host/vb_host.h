/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 as the Veffbox firmware sees it, on a computer: the platform calls fw/vb_app.c makes (time, input, LEDs,
 * flash, the USB queues) as host state, a 240x240 RGB565 framebuffer for the LCD, MNM-1's drawing layer (gfx.c) and
 * the app itself. Used by the screenshots (host/shot.c) and the web emulator (host/emu.c). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- what the platform gives vb_app.c */
#define CTL 32u
#define FELUCCA_VERSION "Veffbox host"
static uint32_t fm1_ms = 1000;
/* flash: the data region (0x97000..0xE0000) in RAM, erased; VB_HOST_FLASH 0 = no flash (the screenshots) */
#ifndef VB_HOST_FLASH
#define VB_HOST_FLASH 1
#endif
#define VB_FLASH_LO 0x97000u
#define VB_FLASH_HI 0xE0000u
static uint8_t host_flash[VB_FLASH_HI - VB_FLASH_LO];
static uint32_t host_flash_writes;                    /* the page stores the region when this moves */
static const int flash_ok = VB_HOST_FLASH;
static int fl_in(uint32_t a, uint32_t n) { return a >= VB_FLASH_LO && a + n <= VB_FLASH_HI; }
static int st_read(uint32_t a, void *d, uint32_t n)
{
    if (!VB_HOST_FLASH || !fl_in(a, n))
        return -1;
    memcpy(d, host_flash + (a - VB_FLASH_LO), n);
    return 0;
}
static int st_erase(uint32_t a)
{
    if (!VB_HOST_FLASH || !fl_in(a, 4096u))
        return -1;
    memset(host_flash + (a - VB_FLASH_LO), 0xFF, 4096u);
    host_flash_writes++;
    return 0;
}
static int st_prog(uint32_t a, const void *d, uint32_t n)
{
    uint32_t i;
    if (!VB_HOST_FLASH || !fl_in(a, n))
        return -1;
    for (i = 0; i < n; i++)
        host_flash[a - VB_FLASH_LO + i] &= ((const uint8_t *)d)[i];   /* programming only clears bits */
    host_flash_writes++;
    return 0;
}
static void fm1_wdt_feed(void) {}
static uint32_t host_leds;
static void fm1_led_key(uint32_t id, int on) { host_leds = on ? host_leds | 1u << id : host_leds & ~(1u << id); }
static uint32_t host_btn, host_rel;
static uint32_t fm1_input_edges(uint32_t *rel) { uint32_t p = host_btn; host_btn = 0; *rel = host_rel; host_rel = 0; return p; }
static uint32_t fm1_input_note_edges(void) { return 0; }
static struct { uint32_t notes; } fm1_in;
static int32_t host_enc[7];
static int32_t enc_take(uint32_t e) { int32_t s = host_enc[e % 7u]; host_enc[e % 7u] = 0; return s; }
#define MQ 64u
static uint32_t midi_in_q[MQ], mi_r, mi_w, midi_in_overflow;
static uint32_t midi_in_ms[MQ];
static uint32_t host_mout[4096], host_nmout;                /* what the firmware sent out over MIDI */
static void midi_out_event(uint32_t pkt) { if (host_nmout < 4096u) host_mout[host_nmout++] = pkt; }
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
