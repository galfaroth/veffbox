/* SPDX-License-Identifier: GPL-3.0-only */
/* Veffbox core: the player. Renders one sound (a recipe) in 32-sample blocks: the transport and the play mode
 * (notes, a 16-step line, or a strummed chord sequence), the voices of the source engine, modulation (6 LFOs and
 * 3 envelopes, 4 routes each, applied to effective copies of the patches), the FX chain, and PRINT capture.
 *
 * Threads: the UI (main loop) edits the recipe in place (byte stores) and sends notes and transport commands through
 * a small single-producer ring; the audio ISR calls vb_player_render. On the host both run in one thread. */
#ifndef VB_PLAYER_H
#define VB_PLAYER_H
#include <stdint.h>
#include "vb_sound.h"

#define VB_EVQ 64
#define VB_CMDQ 32

typedef struct { uint32_t t; uint8_t note, vel, on; } vb_ev_t;

typedef struct {
    vb_recipe_t *rc;
    int bpm10;
    /* voices */
    const vb_engine_t *eng;
    uint8_t eng_src;
    uint32_t vmem[VB_POLY][VB_VOICE_BYTES / 4];
    uint8_t vnote[VB_POLY], vgate[VB_POLY], vlive[VB_POLY];
    uint32_t vage[VB_POLY], agec;
    vb_drum_t drum;
    uint8_t eff[48];                     /* the source patch with modulation */
    vb_drum_patch_t effd;
    vb_fx_slot_t efx[VB_FX_SLOTS];
    /* modulation */
    vb_lfo_t lfo[6];
    int32_t env[3];                      /* Q15 */
    uint8_t env_stage[3];
    int32_t mval[VB_MODSRC];             /* each source's last value, Q15 (the UI draws it) */
    int32_t d_mod[VB_MODSRC * 8];        /* modulation of the modulation sources (one block late) */
    int32_t vol_q8;
    /* FX */
    vb_fx_state_t fx[VB_FX_SLOTS];
    /* transport */
    uint8_t playing, step, last_note, gates_held;
    int32_t step_left, seq_sub;
    uint32_t now, tick_acc, tick_q8, chord_count;
    vb_ev_t ev[VB_EVQ];
    uint32_t nev;
    uint32_t rng;
    /* commands from the UI */
    volatile uint32_t cmd[VB_CMDQ];
    volatile uint32_t cmd_w;
    uint32_t cmd_r;
    /* PRINT: the sampler plays buf[cur]; a print records into the other one */
    int16_t *pbuf[2];
    uint32_t pcap, plen[2];
    uint8_t cur;
    uint8_t rec, rec_done, rec_started_play;
    uint32_t rec_pos, rec_target, rec_quiet, rec_note_off;
    int32_t rec_pair;
    int32_t peak;                        /* output peak since the UI last read it */
    uint8_t poly;                        /* the voices a note may take (0: all VB_POLY): fewer for a track in the background */
    uint32_t hush;                       /* blocks of silence, stopped and with nothing pending: it sleeps after 2000 */
    int32_t block[32];
    void (*idle)(void);                  /* called during long offline renders (the watchdog) */
} vb_player_t;

void vb_player_init(vb_player_t *p, vb_recipe_t *rc, int16_t *fxbuf /* VB_FX_SLOTS * VB_FX_BUF */,
                    int16_t *print_a, int16_t *print_b, uint32_t print_cap);
/* fxbuf and the prints may be 0: a track in the background (its buffered FX bypassed, a sampler source silent).
 * They move between players: detach takes them out, attach gives them (the FX buffers cleared in between); both
 * cheap, for the audio thread */
typedef struct { int16_t *fx[VB_FX_SLOTS]; int16_t *pbuf[2]; uint32_t plen[2], pcap; uint8_t cur; } vb_bufs_t;
void vb_player_detach(vb_player_t *p, vb_bufs_t *b);
void vb_player_attach(vb_player_t *p, const vb_bufs_t *b);
/* UI side (any thread): a note on/off from the keys, transport, print */
void vb_player_note(vb_player_t *p, int note, int vel, int on);
void vb_player_play(vb_player_t *p, int on);
void vb_player_print(vb_player_t *p);
void vb_player_panic(vb_player_t *p);
/* the print is finished (call from the UI): normalizes if asked, makes it the SAMP source; returns 1 once */
int vb_player_print_take(vb_player_t *p);
/* PRINT without the audio thread (REBUILD): resets to the canonical state, records the recipe's print into the
 * free buffer, swaps it in; returns the print's length. A live print renders the same bits. */
uint32_t vb_player_print_offline(vb_player_t *p);
/* a print loaded from storage: the caller filled pbuf[cur ^ 1] with len samples; it becomes the SAMP source */
void vb_player_load(vb_player_t *p, uint32_t len);
/* audio side: n <= 32 mono Q15 samples */
void vb_player_render(vb_player_t *p, int32_t *out, int n);

#endif
