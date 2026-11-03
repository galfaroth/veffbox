/* SPDX-License-Identifier: GPL-3.0-only */
/* The web emulator: the Veffbox firmware (fw/vb_app.c, core/, MNM-1's gfx.c) compiled to WebAssembly. The page
 * (site/emu/) calls these each animation frame and each audio buffer: the same code as on the FM-1, so the screen and
 * the sound are the device's. The flash region lives in RAM; the page keeps it in the browser's storage.
 *
 *   make emu                (emscripten: emcc on the PATH) -> site/emu/veffbox.js + veffbox.wasm */
#include "vb_host.h"
#include <emscripten/emscripten.h>

static float emu_out[4096];
static int32_t emu_blk[2u * CTL];

EMSCRIPTEN_KEEPALIVE uint16_t *emu_fb(void) { return fb; }
EMSCRIPTEN_KEEPALIVE uint8_t *emu_flash(void) { return host_flash; }
EMSCRIPTEN_KEEPALIVE uint32_t emu_flash_size(void) { return sizeof host_flash; }
EMSCRIPTEN_KEEPALIVE uint32_t emu_flash_writes(void) { return host_flash_writes; }
EMSCRIPTEN_KEEPALIVE float *emu_audio_buf(void) { return emu_out; }

/* restored: 1 = the page put a stored flash image into emu_flash() first */
EMSCRIPTEN_KEEPALIVE void emu_init(int restored)
{
    if (!restored)
        memset(host_flash, 0xFF, sizeof host_flash);
    panel = PANEL_DEFAULT;
    vb_theme();
    vb_init();
    ui.force = 1;
}

/* one UI pass, as the main loop does it (input, LEDs, a few screen strips) */
EMSCRIPTEN_KEEPALIVE void emu_frame(uint32_t ms)
{
    uint32_t i;
    fm1_ms = ms;
    vb_input();
    vb_leds();
    for (i = 0; i < 4u; i++)
        vb_draw();
}

/* buttons in Veffbox's order (B_FX, B_SCL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
 * B_OCTDN, B_OCTUP): down / up edges, as the matrix scan reports them */
EMSCRIPTEN_KEEPALIVE void emu_button(uint32_t b, int down)
{
    if (b >= NB)
        return;
    if (down)
        host_btn |= 1u << panel.btn[b];
    else
        host_rel |= 1u << panel.btn[b];
}
EMSCRIPTEN_KEEPALIVE int emu_led(uint32_t b) { return b < NB && ((host_leds >> panel.btn[b]) & 1u); }
/* encoders: SELECT, ALGORITHM, PRESETS, KNOB 1-4; d detents */
EMSCRIPTEN_KEEPALIVE void emu_encoder(uint32_t e, int d)
{
    if (e < NE)
        host_enc[panel.enc[e]] += d * panel.dir[e];
}
/* the 27 keys, 0 = the lowest */
EMSCRIPTEN_KEEPALIVE void emu_key(uint32_t n, int down)
{
    if (n < 27u)
        fm1_in.notes = down ? fm1_in.notes | 1u << n : fm1_in.notes & ~(1u << n);
}
EMSCRIPTEN_KEEPALIVE void emu_master(uint32_t q12) { vb_master_q12 = q12 > 4095u ? 4095u : q12; }

/* n mono samples at 44.1 kHz into emu_audio_buf(), n a multiple of 32, at most 4096 */
EMSCRIPTEN_KEEPALIVE void emu_audio(uint32_t n)
{
    uint32_t i, k;
    for (i = 0; i + CTL <= n && i + CTL <= 4096u; i += CTL) {
        vb_audio_block(emu_blk, CTL);
        vb_master(emu_blk, CTL);
        for (k = 0; k < CTL; k++)
            emu_out[i + k] = (float)emu_blk[2u * k] * (1.0f / 32768.0f);
    }
}
/* the page's own state for its labels */
EMSCRIPTEN_KEEPALIVE int emu_playing(void) { return vb_pl.playing; }
