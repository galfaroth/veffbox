/* SPDX-License-Identifier: GPL-3.0-only */
#include "vb_mod.h"
#include "vb_dsp.h"

/* tempo-synced rates: cycle length in eighths of a beat, 8/1 .. 1/32 (the knob picks one of 9) */
static const uint16_t SYNC_EIGHTHS[9] = { 256, 128, 64, 32, 16, 8, 4, 2, 1 };
/* 2^32 / (600 * 44100) in Q8: phase increment per sample for one cycle per beat, per tenth of a BPM */
#define VB_BEAT_INC_Q8 41554u

void vb_lfo_init(vb_lfo_t *l, uint32_t seed)
{
    l->ph = (uint32_t)l->phase << 25;
    l->rng = seed ? seed : 1;
    l->sh = 0;
    l->held = 0;
    l->stopped = 0;
}

static int32_t wave(vb_lfo_t *l, uint32_t ph)
{
    switch (l->wave) {
    case VB_LFO_SIN: return vb_sin(ph);
    case VB_LFO_TRI: { int32_t x = (int32_t)(ph >> 16); return x < 32768 ? -32767 + x * 2 : 32767 - (x - 32768) * 2; }
    case VB_LFO_SQR: return ph < 0x80000000u ? 32767 : -32767;
    case VB_LFO_SAW: return 32767 - (int32_t)(ph >> 16);
    case VB_LFO_RAMP: return (int32_t)(ph >> 16) - 32767;
    case VB_LFO_SH: return l->sh;
    default: {   /* EXP: a cubic fall over the cycle */
        int32_t e = 32767 - (int32_t)(ph >> 17);
        int32_t cube = ((e * e) >> 15) * e >> 15;   /* e < 2^15: fits */
        return cube * 2 - 32767;
    }
    }
}

void vb_lfo_note(vb_lfo_t *l)
{
    if (l->trig == VB_TRIG_FREE) return;
    if (l->trig == VB_TRIG_HOLD) { l->held = wave(l, l->ph); return; }
    l->ph = (uint32_t)l->phase << 25;
    l->stopped = 0;
}

int32_t vb_lfo_step(vb_lfo_t *l, int bpm10, int n)
{
    uint32_t inc;
    if (l->sync) {
        uint32_t beat = (uint32_t)((bpm10 * VB_BEAT_INC_Q8) >> 8);
        inc = beat * 8 / SYNC_EIGHTHS[(l->rate * 8 + 63) / 127];
    } else {
        inc = VB_LFO_INC[l->rate & 127];
    }
    if (!l->stopped) {
        uint32_t before = l->ph;
        l->ph += inc * (uint32_t)n;
        int wrapped = l->ph < before;
        if (wrapped && l->wave == VB_LFO_SH) l->sh = (int32_t)(vb_rng(&l->rng) >> 16) - 32768;
        uint32_t start = (uint32_t)l->phase << 25;
        if (l->trig == VB_TRIG_ONE && wrapped) { l->ph = start; l->stopped = 1; }
        if (l->trig == VB_TRIG_HALF && (l->ph - start) >= 0x80000000u) { l->ph = start + 0x80000000u; l->stopped = 1; }
    }
    return l->trig == VB_TRIG_HOLD ? l->held : wave(l, l->ph);
}

void vb_mod_add(const vb_route_t *r, int32_t val_q15, int32_t *delta_q8, int ndest)
{
    for (int i = 0; i < VB_ROUTES; i++) {
        if (r[i].dest == VB_NO_DEST || r[i].dest >= ndest || !r[i].amt) continue;
        delta_q8[r[i].dest] += (val_q15 * r[i].amt) >> 6;   /* amt 64 at full scale = 128 knob steps */
    }
}
