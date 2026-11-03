/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox for the M-VAVE FM-1: one compilation unit.
 *
 * The platform is MNM-1's, unchanged (copied into the build tree by tools/fm1_build.sh): the HAL, the LCD and
 * its canvas, the USB driver with the M-UPGRADE update entry, and the flash driver. Keeping the update path
 * identical to MNM-1's means a Veffbox image can always be replaced with the normal installer.
 * The application (fw/vb_app.c) and the sound (core/) are Veffbox's own. */
#include <stdint.h>
#define FELUCCA_DUAL_CORE 0
#define FELUCCA_CDC 0
#define FELUCCA_UAC 1
#define FELUCCA_UART 0
#define FELUCCA_OTA 1
#define FELUCCA_FLASH 1
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"
#include "felucca_tables.h"          /* FS, CTL */

#include "libc.c"
#include "lcd.c"
#include "gfx.c"

/* what MNM-1's core.h gives the platform modules */
#define HALF_FRAMES 256              /* I2S half buffer: 5.8 ms at 44.1 kHz */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static volatile uint32_t fm1_ms;     /* milliseconds since boot (TIMER5 ISR) */
#define BOOTGUARD_MAGIC 0x42475244u  /* two boots in a row that die in the first 30 s -> UBOOT */
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));

/* the audio ISR's diagnostics (TIMER5 nests into the render; see fm1_timer5_irq) */
static struct { uint32_t in_audio, nested, halves, late, max_us, last_us, timer_irqs, cpu_q8, cpu_rem; } vb_dbg;
static volatile uint32_t t5_nested_ticks;

#include "enc.c"
#include "led_dim.c"
#include "usb_app.c"

/* ---- flash and the update entry (as MNM-1's felucca.c) */
#include "fm1_flash.h"
static uint8_t flash_ok;
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static void audio_silence(void)
{
    uint32_t i;
    for (i = 0; i < sizeof abuf / sizeof abuf[0]; i++)
        abuf[i] = 0;
}
static int st_erase(uint32_t off)                   /* IRQs off: the DMA would loop stale audio, so silence first */
{
    uint32_t took, f;
    int rc;
    if (!FL_STORE_OK(off, 0x1000u))
        return -8;
    f = irq_save();
    audio_silence();
    rc = FL_FAR(fl_erase4k_ram)(off, &took);
    irq_restore(f);
    return rc;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!FL_STORE_OK(off, n))
        return -8;
    return fl_write(off, src, n);
}
#include "ota.c"
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    audio_silence();
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    lcd_fill(0, 0, 240, 240, C_BG);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_INK, 1);
    if (step < 9u) {
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_INK, 1);
        return;
    }
    draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_INK, 1);
    fm1_delay_ms(1500);
}
static void ota_commit(const uint8_t *parm)
{
    bootguard.pending = 0;                              /* intentional reset */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}
static void vb_remote(void);                    /* vb_app.c: SysEx remote control and diagnostics */
static void ed_service(void) { vb_remote(); }

/* ---- the application */
#include "vb/vb_dsp.h"
#include "vb/vb_player.h"           /* the core is compiled as separate objects (tools/fm1_build.sh APP_EXTRA) */
#include "vb/vb_op4.h"
#include "vb/vb_anlg.h"
#include "vb/vb_samp.h"
#include "vb/vb_gran.h"
#include "vb/vb_chop.h"
#include "vb/vb_store.h"
#include "vb_app.c"

/* ---- interrupts */
void fm1_timer5_irq(void)
{
    static uint32_t sub, owed;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    vb_dbg.timer_irqs++;
    fm1_input_tick();
    led_dim_tick();                             /* (enc.c's decoder runs from here) */
    {
        static uint32_t last, acc;
        acc += t0 - last;
        last = t0;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
    if (usb_due)
        owed |= 1u;
    if (++sub == 10u)
        sub = 0;
    if (vb_dbg.in_audio) {                      /* nested in the render: only the USB audio stream */
        vb_dbg.nested++;
        if (usb_due)
            uac_service();
        t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed & 1u)
        usb_poll();
    owed = 0;
}
extern void isr_timer5(void);

#define OUT_SHIFT 7                             /* Q15 -> 24-bit, -6 dBFS ceiling */
void fm1_alnk0_irq(void)
{
    uint8_t p;
    uint32_t t0;
    vb_dbg.in_audio = 1;
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b, i, us;
        int32_t *o = &abuf[half * HALF_WORDS];
        uac_render_start();
        for (b = 0; b < HALF_FRAMES; b += CTL) {
            int32_t *out = o + 2u * b;
            vb_audio_block(out, CTL);
            uac_tap(out, CTL);                      /* USB audio: always at full level (record it in a DAW) */
            vb_master(out, CTL);
            for (i = 0; i < 2u * CTL; i++)
                out[i] *= 1 << OUT_SHIFT;
        }
        fm1_audio_ack_half();
        us = (fm1_ticks() - t0 - t5_nested_ticks) / FM1_TICKS_PER_US;
        vb_dbg.last_us = us;
        {   /* the average load, % x 256 of the half's time (an IIR that keeps its remainder) */
            uint32_t load = vb_dbg.cpu_q8 * 15u + us * 25600u / 5805u + vb_dbg.cpu_rem;
            vb_dbg.cpu_q8 = load / 16u;
            vb_dbg.cpu_rem = load % 16u;
        }
        if (us > vb_dbg.max_us)
            vb_dbg.max_us = us;
        if (fm1_audio_free_half() != half)
            vb_dbg.late++;
        vb_dbg.halves++;
    }
    vb_dbg.in_audio = 0;
}
extern void isr_alnk0(void);

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 8, 240, &FONT_S, "VEFFBOX CRASH", C_WHITE, 1);
    hexs(b, c->vec);
    draw_text_box(10, 40, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->pc);
    draw_text_box(10, 60, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->rets);
    draw_text_box(10, 80, 220, &FONT_S, b, C_WHITE, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void enter_uboot(const char *why)
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_S, why, C_WHITE, 1);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;                      /* intentional reset: not a failed boot */
    fm1_enter_uboot();
}

static void fm1_main(void)
{
    int32_t knob = 512 * 16;
    {
        uint32_t f = irq_save();
        flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
        irq_restore(f);
        if (flash_ok)
            fl_plain_window_init();
    }
    if (flash_ok)
        ota_boot_cleanup();                     /* staging area left by an update */
    lcd_init();
    vb_theme();
    vb_boot_screen();
    fm1_input_init();
    fm1_adc_init();
    panel_init();
    vb_init();
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    usb_start();
    fm1_timer5_start(isr_timer5, 4);            /* above ALNK0 (3): the scan nests into the render */
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(400);
    ui.force = 1;

    for (;;) {
        uint32_t m = fm1_ms;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        {
            int32_t a = fm1_adc_read(FM1_ADC_MASTER);
            if (a >= 0) {
                uint32_t k10;
                knob += (a * 16 - knob) / 8;
                k10 = (uint32_t)(knob / 16);
                vb_master_q12 = (k10 * k10) >> 8;            /* 0 .. ~4096 */
            }
        }
        {   /* OCT- + OCT+ held 5 s: UBOOT (the way back if anything goes wrong) */
            static uint32_t t0;
            uint32_t both = (1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]);
            if ((fm1_in.buttons & both) != both)
                t0 = fm1_ms;
            else if (fm1_ms - t0 > 5000u)
                enter_uboot("UBOOT");
        }
        ed_service();
        ota_service();                                  /* M-UPGRADE handshake */
        if (usb.ota_req) {
            usb.ota_req = 0;
            vb_all_off();
            if (flash_ok)
                ota_session();                          /* returns only if nothing was committed */
            lcd_fill(0, 0, 240, 240, C_BG);
            ui.force = 1;
        }
        if (usb.uboot_req)
            enter_uboot("UBOOT (USB)");
        vb_input();
        vb_leds();
        vb_draw();
        while (fm1_ms - m < 15u)
            vb_input();
    }
}

void fm1_cstart(void)
{
    extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
    extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    fm1_core1_stop();
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {               /* two failed boots in a row: UBOOT, the installer can reach it */
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
