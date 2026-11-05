/* SPDX-License-Identifier: GPL-3.0-only */
#include "vb_player.h"
#include "vb_dsp.h"
#include "vb_op4.h"
#include "vb_anlg.h"
#include "vb_samp.h"
#include "vb_gran.h"
#include "vb_chop.h"

enum { C_NOTE_ON = 1, C_NOTE_OFF, C_PLAY, C_STOP, C_PRINT, C_PANIC, C_SWAP };
#define CMD(t, a, b) ((uint32_t)(t) << 24 | (uint32_t)((a) & 0xFF) << 16 | (uint32_t)((b) & 0xFF) << 8)
#define BARRIER() __asm__ volatile("" ::: "memory")

/* per-block scratch (one player renders at a time) */
static int32_t d_src[48], d_drum[VB_DRUM_VOICES * 9], d_fx[VB_FX_SLOTS * 5], d_play[4], d_glob[2], d_mod_new[VB_MODSRC * 8];
static uint8_t eplay[4];                    /* TIME HUMAN VEL SPRD with modulation */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static const vb_engine_t *engine_of(int src)
{
    switch (src) {
    case VB_SRC_OP4: return &vb_op4;
    case VB_SRC_ANLG: return &vb_anlg;
    case VB_SRC_DRUM: return 0;
    case VB_SRC_GRAN: return &vb_gran;
    case VB_SRC_CHOP: return &vb_chop;
    default: return &vb_samp;
    }
}

static void cmd_push(vb_player_t *p, uint32_t c)
{
    uint32_t w = p->cmd_w;
    if (w - p->cmd_r >= VB_CMDQ)
        return;                              /* full: drop (the UI sends a few per frame) */
    p->cmd[w % VB_CMDQ] = c;
    BARRIER();
    p->cmd_w = w + 1u;
}
void vb_player_note(vb_player_t *p, int note, int vel, int on) { cmd_push(p, CMD(on ? C_NOTE_ON : C_NOTE_OFF, note, vel)); }
void vb_player_play(vb_player_t *p, int on) { cmd_push(p, CMD(on ? C_PLAY : C_STOP, 0, 0)); }
void vb_player_print(vb_player_t *p) { cmd_push(p, CMD(C_PRINT, 0, 0)); }
void vb_player_panic(vb_player_t *p) { cmd_push(p, CMD(C_PANIC, 0, 0)); }

static void voices_reset(vb_player_t *p)
{
    uint32_t v;
    for (v = 0; v < VB_POLY; v++) {
        if (p->eng)
            p->eng->voice_init(p->vmem[v], p->rc->seed + v);
        p->vlive[v] = p->vgate[v] = 0;
    }
    vb_drum_init(&p->drum, p->rc->seed ^ 0x5EED1234u);
}

static void tempo(vb_player_t *p)
{
    int b = p->bpm10 < 400 ? 400 : p->bpm10 > 3000 ? 3000 : p->bpm10;
    p->tick_q8 = (uint32_t)VB_SR * 150u * 256u / (uint32_t)b;      /* samples per 16th, Q8 */
}

void vb_player_init(vb_player_t *p, vb_recipe_t *rc, int16_t *fxbuf, int16_t *print_a, int16_t *print_b, uint32_t print_cap)
{
    uint32_t i;
    uint8_t *z = (uint8_t *)p;
    for (i = 0; i < sizeof *p; i++)
        z[i] = 0;
    p->rc = rc;
    p->bpm10 = 1200;
    tempo(p);
    p->eng_src = rc->src;
    p->eng = engine_of(rc->src);
    p->rng = rc->seed ? rc->seed : 1u;
    for (i = 0; i < 6; i++) {
        vb_lfo_init(&p->lfo[i], rc->seed + 77u * (i + 1u));
    }
    for (i = 0; i < VB_FX_SLOTS; i++)
        vb_fx_init(&p->fx[i], fxbuf ? fxbuf + i * VB_FX_BUF : 0, rc->seed + 1013u * (i + 1u));
    p->pbuf[0] = print_a;
    p->pbuf[1] = print_b;
    p->pcap = print_cap > 65535u ? 65535u : print_cap;
    if (print_a)
        vb_samp_set_buffer(print_a, 0);
    p->vol_q8 = 100 * 256;
    voices_reset(p);
}

/* ---- notes and voices */
static void env_gate(vb_player_t *p, int on)
{
    uint32_t i;
    for (i = 0; i < 3; i++) {
        if (on)
            p->env_stage[i] = 1;
        else if (p->env_stage[i] >= 1 && p->env_stage[i] <= 3)
            p->env_stage[i] = 4;
    }
}

static void voice_on(vb_player_t *p, int note, int vel)
{
    uint32_t v, best = 0, i, np = p->poly && p->poly < VB_POLY ? p->poly : VB_POLY;
    note = clampi(note, 0, 127);
    vel = clampi(vel, 1, 127);
    p->last_note = (uint8_t)note;
    if (p->note_out && !p->rec)
        p->note_out(p->out_ch, (uint8_t)note, (uint8_t)vel, 1);
    env_gate(p, 1);
    for (i = 0; i < 6; i++)
        vb_lfo_note(&p->lfo[i]);
    if (!p->eng) {
        vb_drum_trigger(&p->drum, &p->effd, note % VB_DRUM_VOICES, vel);
        return;
    }
    for (v = 0; v < np; v++)                 /* the same note again: retrigger its voice */
        if (p->vlive[v] && p->vnote[v] == note)
            break;
    if (v == np)
        for (v = 0; v < np && p->vlive[v]; v++)
            ;
    if (v == np) {                           /* steal: the oldest released voice, else the oldest */
        uint32_t age = 0xFFFFFFFFu;
        for (i = 0; i < np; i++)
            if (!p->vgate[i] && p->vage[i] < age) {
                age = p->vage[i];
                best = i;
            }
        if (age == 0xFFFFFFFFu)
            for (i = 0; i < np; i++)
                if (p->vage[i] < age) {
                    age = p->vage[i];
                    best = i;
                }
        v = best;
    }
    p->eng->note_on(p->vmem[v], p->eff, note, vel);
    p->vnote[v] = (uint8_t)note;
    p->vgate[v] = 1;
    p->vlive[v] = 1;
    p->vage[v] = ++p->agec;
}

static void voice_off(vb_player_t *p, int note)
{
    uint32_t v, held = 0;
    if (p->note_out && !p->rec)
        p->note_out(p->out_ch, (uint8_t)note, 0, 0);
    if (!p->eng) {
        env_gate(p, 0);
        return;
    }
    for (v = 0; v < VB_POLY; v++) {
        if (p->vlive[v] && p->vgate[v] && p->vnote[v] == note) {
            p->eng->note_off(p->vmem[v], p->eff);
            p->vgate[v] = 0;
        }
        held |= p->vgate[v];
    }
    if (!held)
        env_gate(p, 0);
}

static void all_off(vb_player_t *p)
{
    uint32_t v;
    for (v = 0; v < VB_POLY; v++)
        if (p->vlive[v] && p->vgate[v]) {
            p->eng->note_off(p->vmem[v], p->eff);
            p->vgate[v] = 0;
        }
    p->nev = 0;
    env_gate(p, 0);
}

/* ---- the event queue (strums, gates) */
static void ev_add(vb_player_t *p, uint32_t t, int note, int vel, int on)
{
    if (p->nev >= VB_EVQ)
        return;
    p->ev[p->nev].t = t;
    p->ev[p->nev].note = (uint8_t)note;
    p->ev[p->nev].vel = (uint8_t)vel;
    p->ev[p->nev].on = (uint8_t)on;
    p->nev++;
}
static void ev_run(vb_player_t *p, uint32_t until)
{
    uint32_t pass, i;
    for (pass = 0; pass < 2u; pass++)        /* offs before ons: a repeated note retriggers */
        for (i = 0; i < p->nev;) {
            vb_ev_t e = p->ev[i];
            if ((int32_t)(e.t - until) < 0 && e.on == pass) {
                p->ev[i] = p->ev[--p->nev];
                if (e.on)
                    voice_on(p, e.note, e.vel);
                else
                    voice_off(p, e.note);
            } else {
                i++;
            }
        }
}

static void sched_chord(vb_player_t *p, const vb_chord_t *c, uint32_t steplen)
{
    uint8_t ns[16], ord[16];
    int n = c->n, i, j, k, strum = p->rc->play.strum;
    uint32_t gap, jit, off_t;
    if (!n)
        return;
    for (i = 0; i < n; i++)
        ns[i] = c->note[i];
    for (i = 1; i < n; i++)                  /* low to high */
        for (j = i; j > 0 && ns[j - 1] > ns[j]; j--) {
            uint8_t t = ns[j];
            ns[j] = ns[j - 1];
            ns[j - 1] = t;
        }
    if (strum == VB_STRUM_HARP) {            /* the chord and its octave, wide */
        for (i = 0; i < n && n + i < 16; i++)
            ns[n + i] = (uint8_t)(ns[i] + 12 > 127 ? 127 : ns[i] + 12);
        n = n * 2 > 16 ? 16 : n * 2;
    }
    for (i = 0; i < n; i++)
        ord[i] = (uint8_t)i;
    if (strum == VB_STRUM_DOWN || (strum == VB_STRUM_ALT && (p->chord_count & 1u)))
        for (i = 0; i < n / 2; i++) {
            uint8_t t = ord[i];
            ord[i] = ord[n - 1 - i];
            ord[n - 1 - i] = t;
        }
    if (strum == VB_STRUM_RND)
        for (i = n - 1; i > 0; i--) {
            uint8_t t;
            j = (int)(vb_rng(&p->rng) % (uint32_t)(i + 1));
            t = ord[i];
            ord[i] = ord[j];
            ord[j] = t;
        }
    gap = (uint32_t)eplay[0] * 250u * 441u / 1270u;          /* TIME: 0 .. 250 ms */
    if (strum == VB_STRUM_LUSH)
        gap = 0;
    if (strum == VB_STRUM_ARP && n)
        gap = steplen / (uint32_t)n;
    jit = (uint32_t)eplay[1] * 882u / 127u;                   /* HUMAN: up to 20 ms */
    off_t = p->now + steplen * c->gate / 100u;
    if (vb_chord_bass(c)) {                  /* the BASS: first, on the beat, for the whole gate */
        ev_add(p, p->now, vb_chord_bass(c), eplay[2], 1);
        ev_add(p, off_t, vb_chord_bass(c), 0, 0);
    }
    if (!n)
        return;
    for (k = 0; k < n; k++) {
        uint32_t dt = (uint32_t)k * gap + (jit ? vb_rng(&p->rng) % (jit + 1u) : 0u);
        int vel = eplay[2];
        if (dt >= off_t - p->now)
            break;
        if (eplay[1])
            vel += (int)(vb_rng(&p->rng) % (eplay[1] / 4u + 1u)) - eplay[1] / 8;
        if (strum == VB_STRUM_LUSH)          /* SPRD: a softer, spread top */
            vel -= k * eplay[3] / 64;
        ev_add(p, p->now + dt, ns[ord[k]], clampi(vel, 1, 127), 1);
    }
    for (k = 0; k < n; k++)
        ev_add(p, off_t, ns[k], 0, 0);
}

/* ARP: the held keys, else the chord playing (its own progression, or CHORDS'), over the octaves, in the pattern */
static void arp_16th(vb_player_t *p)
{
    static const uint8_t PER[4] = {1, 2, 4, 1};          /* sixteenths a note: 1/16 1/8 1/4, 1/32 (two a sixteenth) */
    vb_recipe_t *rc = p->rc;
    const vb_play_t *pl = &rc->play, *hp = p->harm && !p->rec ? p->harm : pl;
    uint8_t ns[32];
    int n = 0, i, j, oct = 1 + (rc->arp_oct & 3u), rate = rc->arp_rate & 3u, idx, note;
    uint32_t s16 = p->tick_q8 >> 8, dur;
    if (p->playing) {                                     /* the progression moves on as in CHORD */
        if (p->step_left <= 0) {
            int len = clampi(hp->len, 1, VB_CHORD_STEPS);
            p->step = (uint8_t)(p->chord_count ? (p->step + 1) % len : 0);
            p->step_left = vb_len16[hp->chord[p->step].len_i % 12u];
            p->chord_count++;
        }
        p->step_left--;
    }
    if (p->arp_sub > 0) {
        p->arp_sub--;
        return;
    }
    p->arp_sub = (uint8_t)(PER[rate] - 1u);
    if (p->nheld) {
        for (i = 0; i < p->nheld; i++)
            ns[n++] = p->held[i];
    } else if (p->playing) {
        const vb_chord_t *c = &hp->chord[p->step % VB_CHORD_STEPS];
        for (i = 0; i < c->n && i < 8; i++)
            ns[n++] = c->note[i];
    }
    if (!n)
        return;
    if (rc->arp_pat != VB_ARP_ORDER)                       /* low to high */
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && ns[j - 1] > ns[j]; j--) {
                uint8_t t = ns[j];
                ns[j] = ns[j - 1];
                ns[j - 1] = t;
            }
    {
        int base = n, o;
        for (o = 1; o < oct; o++)                         /* the octaves above */
            for (j = 0; j < base && n < 32; j++)
                if (ns[j] + 12 * o <= 127)
                    ns[n++] = (uint8_t)(ns[j] + 12 * o);
    }
    switch (rc->arp_pat) {
    case VB_ARP_DOWN: idx = n - 1 - (int)(p->arp_i % (uint32_t)n); break;
    case VB_ARP_UPDN: {
        int per = n > 1 ? 2 * n - 2 : 1, k = (int)(p->arp_i % (uint32_t)per);
        idx = k < n ? k : per - k;
        break;
    }
    case VB_ARP_RND: idx = (int)(vb_rng(&p->rng) % (uint32_t)n); break;
    default: idx = (int)(p->arp_i % (uint32_t)n); break;
    }
    p->arp_i++;
    note = ns[idx];
    dur = (uint32_t)PER[rate] * s16 / (rate == 3 ? 2u : 1u);
    ev_add(p, p->now, note, eplay[2], 1);
    ev_add(p, p->now + dur * 3u / 4u, note, 0, 0);
    if (rate == 3) {                                      /* 1/32: a second note half way */
        int n2 = ns[(idx + 1) % n];
        p->arp_i++;
        ev_add(p, p->now + dur, n2, eplay[2], 1);
        ev_add(p, p->now + dur + dur * 3u / 4u, n2, 0, 0);
    }
}

static void on_16th(vb_player_t *p)
{
    vb_play_t *pl = &p->rc->play;
    uint32_t s16 = p->tick_q8 >> 8;
    if (pl->mode == VB_PLAY_ARP) {
        arp_16th(p);
    } else if (pl->mode == VB_PLAY_CHORD) {
        if (p->step_left <= 0) {
            const vb_play_t *hp = p->harm && !p->rec ? p->harm : pl;   /* (a print plays its own: REBUILD) */
            int len = clampi(hp->len, 1, VB_CHORD_STEPS);
            const vb_chord_t *c;
            p->step = (uint8_t)(p->chord_count ? (p->step + 1) % len : 0);
            c = &hp->chord[p->step];
            p->step_left = vb_len16[c->len_i % 12u];
            sched_chord(p, c, (uint32_t)p->step_left * s16);
            p->chord_count++;
        }
        p->step_left--;
    } else if (pl->mode == VB_PLAY_SEQ) {
        static const uint8_t PER[4] = {4, 2, 1, 1};
        if (p->seq_sub <= 0) {
            int len = clampi(pl->len, 1, VB_SEQ_STEPS);
            const vb_step_t *st;
            p->step = (uint8_t)(p->chord_count ? (p->step + 1) % len : 0);
            p->chord_count++;
            st = &pl->seq[p->step];
            p->seq_sub = PER[pl->rate & 3u];
            if (st->on && vb_rng(&p->rng) % 100u < st->prob) {
                uint32_t dur = (uint32_t)p->seq_sub * s16;
                ev_add(p, p->now, st->note, eplay[2], 1);
                ev_add(p, p->now + (st->slide ? dur + 32u : dur / 2u), st->note, 0, 0);
            }
        }
        p->seq_sub--;
    }
}

static void transport_start(vb_player_t *p)
{
    p->playing = 1;
    p->n16 = 0;
    p->arp_i = 0;
    p->arp_sub = 0;
    p->chord_count = 0;
    p->step = 0;
    p->step_left = 0;
    p->seq_sub = 0;
    p->tick_acc = p->tick_q8;                 /* the first 16th now */
}

/* ---- modulation */
static int32_t env_inc(int v, int n)
{
    uint32_t ms = 1u + (uint32_t)(v * v * v) / 410u;          /* 1 ms .. ~5 s */
    return (int32_t)(32767u * (uint32_t)n / (ms * 441u / 10u + 1u)) + 1;
}
static int32_t env_step(vb_player_t *p, int i, int n)
{
    const vb_modsrc_t *m = &p->rc->mod[6 + i];
    int32_t *lv = &p->env[i];
    int atk = clampi(m->atk + (p->d_mod[(6 + i) * 8 + 0] >> 8), 0, 127);
    int dec = clampi(m->dec + (p->d_mod[(6 + i) * 8 + 1] >> 8), 0, 127);
    int sus = clampi(m->sus + (p->d_mod[(6 + i) * 8 + 2] >> 8), 0, 127) * 258;
    int rel = clampi(m->rel + (p->d_mod[(6 + i) * 8 + 3] >> 8), 0, 127);
    switch (p->env_stage[i]) {
    case 1:
        if ((*lv += env_inc(atk, n)) >= 32767) {
            *lv = 32767;
            p->env_stage[i] = 2;
        }
        break;
    case 2:
        if ((*lv -= env_inc(dec, n)) <= sus) {
            *lv = sus;
            p->env_stage[i] = m->loop ? 1 : 3;
        }
        break;
    case 3:
        *lv = sus;
        break;
    case 4:
        if ((*lv -= env_inc(rel, n)) <= 0) {
            *lv = 0;
            p->env_stage[i] = 0;
        }
        break;
    default:
        *lv = 0;
    }
    return *lv;
}

static void route_add(vb_player_t *p, vb_addr_t a, int32_t d)
{
    int dom = a >> 8, i = a & 0xFF;
    switch (dom) {
    case VB_D_OP4:
        if (p->eng_src == VB_SRC_OP4 && i < VB_OP4_NP)
            d_src[i] += d;
        break;
    case VB_D_ANLG:
        if (p->eng_src == VB_SRC_ANLG && i < VB_ANLG_NP)
            d_src[i] += d;
        break;
    case VB_D_SAMP:
        if (p->eng_src == VB_SRC_SAMP && i < VB_SAMP_NP)
            d_src[i] += d;
        break;
    case VB_D_GRAN:
        if (p->eng_src == VB_SRC_GRAN && i < VB_GRAN_NP)
            d_src[i] += d;
        break;
    case VB_D_CHOP:
        if (p->eng_src == VB_SRC_CHOP && i < VB_CHOP_NP)
            d_src[i] += d;
        break;
    case VB_D_DRUM:
        if (i < VB_DRUM_VOICES * 9)
            d_drum[i] += d;
        break;
    case VB_D_FX:
        if (i < VB_FX_SLOTS * 5)
            d_fx[i] += d;
        break;
    case VB_D_MOD:
        if (i < VB_MODSRC * 8)
            d_mod_new[i] += d;
        break;
    case VB_D_PLAY:
        if (i < 4)
            d_play[i] += d;
        break;
    case VB_D_GLOBAL:
        if (i < 2)
            d_glob[i] += d;
        break;
    }
}

static void mod_block(vb_player_t *p, int n)
{
    vb_recipe_t *rc = p->rc;
    uint32_t i, k;
    const vb_param_t *pd;
    int np;
    for (i = 0; i < 48u; i++)
        d_src[i] = 0;
    for (i = 0; i < VB_DRUM_VOICES * 9u; i++)
        d_drum[i] = 0;
    for (i = 0; i < VB_FX_SLOTS * 5u; i++)
        d_fx[i] = 0;
    for (i = 0; i < VB_MODSRC * 8u; i++)
        d_mod_new[i] = 0;
    d_play[0] = d_play[1] = d_play[2] = d_play[3] = d_glob[0] = d_glob[1] = 0;
    for (i = 0; i < VB_MODSRC; i++) {
        const vb_modsrc_t *m = &rc->mod[i];
        int32_t v;
        if (i < 6) {
            vb_lfo_t *l = &p->lfo[i];
            l->wave = m->wave;
            l->sync = m->sync;
            l->trig = m->trig;
            l->rate = (uint8_t)clampi(m->rate + (p->d_mod[i * 8] >> 8), 0, 127);
            l->phase = (uint8_t)clampi(m->phase + (p->d_mod[i * 8 + 1] >> 8), 0, 127);
            v = vb_lfo_step(l, p->bpm10, n);
        } else {
            v = env_step(p, (int)i - 6, n);
        }
        p->mval[i] = v;
        for (k = 0; k < VB_ROUTES; k++)
            if (m->r[k].dest != VB_ADDR_NONE && m->r[k].amt)
                route_add(p, m->r[k].dest, (v * m->r[k].amt) >> 6);
    }
    for (i = 0; i < VB_MODSRC * 8u; i++)
        p->d_mod[i] = d_mod_new[i];
    /* the effective patches */
    if (p->eng) {
        const uint8_t *base = p->eng_src == VB_SRC_OP4 ? rc->op4 : p->eng_src == VB_SRC_ANLG ? rc->anlg :
                              p->eng_src == VB_SRC_GRAN ? rc->gran : p->eng_src == VB_SRC_CHOP ? rc->chop : rc->samp;
        pd = p->eng->desc.params;
        np = p->eng->desc.nparams;
        for (i = 0; i < (uint32_t)np && i < 48u; i++)
            p->eff[i] = (uint8_t)clampi(base[i] + (d_src[i] >> 8), pd[i].min, pd[i].max);
    }
    p->effd = rc->drum;
    for (i = 0; i < VB_DRUM_VOICES * 9u; i++)
        if (d_drum[i]) {
            uint8_t *q = i % 9u == 8u ? &p->effd.lvl[i / 9u] : &p->effd.p[i / 9u][i % 9u];
            *q = (uint8_t)clampi(*q + (d_drum[i] >> 8), 0, 127);
        }
    for (i = 0; i < VB_FX_SLOTS; i++) {
        p->efx[i] = rc->fx[i];
        for (k = 0; k < 4u; k++)
            p->efx[i].p[k] = (uint8_t)clampi(rc->fx[i].p[k] + (d_fx[i * 5 + k] >> 8), 0, 127);
        p->efx[i].mix = (uint8_t)clampi(rc->fx[i].mix + (d_fx[i * 5 + 4] >> 8), 0, 127);
    }
    eplay[0] = (uint8_t)clampi(rc->play.time + (d_play[0] >> 8), 0, 127);
    eplay[1] = (uint8_t)clampi(rc->play.hum + (d_play[1] >> 8), 0, 127);
    eplay[2] = (uint8_t)clampi(rc->play.vel + (d_play[2] >> 8), 1, 127);
    eplay[3] = (uint8_t)clampi(rc->play.sprd + (d_play[3] >> 8), 0, 127);
    p->vol_q8 = clampi(100 * 256 + d_glob[1], 0, 127 * 256);
}

/* ---- PRINT */
/* the canonical state a print starts from, so REBUILD reproduces it exactly: voices, LFOs, envelopes, FX (buffers
 * cleared), transport, events and randomness all restart from the recipe's seed and tempo */
static void canonical(vb_player_t *p)
{
    uint32_t i;
    vb_recipe_t *rc = p->rc;
    p->bpm10 = rc->bpm10 ? rc->bpm10 : p->bpm10;
    tempo(p);
    p->nev = 0;
    p->playing = 0;
    p->rng = rc->seed ? rc->seed : 1u;
    p->agec = 0;
    p->last_note = rc->pnote;
    for (i = 0; i < VB_POLY; i++)
        p->vage[i] = 0;
    voices_reset(p);
    for (i = 0; i < 6; i++) {
        uint8_t ph = rc->mod[i].phase;
        p->lfo[i].phase = ph;
        vb_lfo_init(&p->lfo[i], rc->seed + 77u * (i + 1u));
    }
    for (i = 0; i < 3; i++) {
        p->env[i] = 0;
        p->env_stage[i] = 0;
    }
    for (i = 0; i < VB_MODSRC * 8u; i++)
        p->d_mod[i] = 0;
    for (i = 0; i < VB_FX_SLOTS; i++) {
        int16_t *b = p->fx[i].buf;
        uint32_t k;
        for (k = 0; b && k < VB_FX_BUF; k++)
            b[k] = 0;
        vb_fx_init(&p->fx[i], b, rc->seed + 1013u * (i + 1u));
    }
    mod_block(p, 32);                         /* the effective patches for the first notes */
}

static void print_begin(vb_player_t *p)
{
    uint32_t s16, len = p->rc->print[0];
    if (p->eng_src != p->rc->src) {
        p->eng_src = p->rc->src;
        p->eng = engine_of(p->rc->src);
    }
    p->rc->bpm10 = (uint16_t)p->bpm10;        /* the recipe remembers the tempo and the note it printed at */
    if (len == 0)
        p->rc->pnote = p->last_note ? p->last_note : 60;
    canonical(p);
    s16 = p->tick_q8 >> 8;
    p->rec = 1;
    p->rec_pos = 0;
    p->rec_done = 0;
    p->rec_quiet = 0;
    if (len == 0) {                          /* NOTE: one note and its tail, until silence */
        int note = p->rc->pnote;
        p->rec_target = p->pcap;
        p->rec_started_play = 0;
        voice_on(p, note, p->rc->play.vel);
        p->rec_note_off = p->now + 17640u;   /* held 0.4 s */
        ev_add(p, p->rec_note_off, note, 0, 0);
    } else {                                 /* 1, 2 or 4 bars of the play mode, from its start */
        uint32_t bars = len >= 3u ? 4u : len, want = bars * 16u * s16 / 2u;
        p->rec_target = want < p->pcap ? want : p->pcap;
        p->rec_started_play = 1;
        p->rec_note_off = 0;
        transport_start(p);
    }
}

static void print_capture(vb_player_t *p, const int32_t *out, int n)
{
    int16_t *dst = p->pbuf[p->cur ^ 1u];
    int32_t pk = 0;
    int i;
    for (i = 0; i < n && p->rec_pos < p->rec_target; i++) {
        if (i & 1)
            dst[p->rec_pos++] = (int16_t)((p->rec_pair + out[i]) >> 1);   /* stored at 22.05 kHz */
        else
            p->rec_pair = out[i];
        pk = out[i] > pk ? out[i] : -out[i] > pk ? -out[i] : pk;
    }
    if (!p->rec_started_play && (int32_t)(p->now - p->rec_note_off) >= 0) {
        p->rec_quiet = pk < 64 ? p->rec_quiet + (uint32_t)n : 0;
        if (p->rec_quiet >= 4410u)           /* 100 ms of silence: the tail is over */
            p->rec_target = p->rec_pos;
    }
    if (p->rec_pos >= p->rec_target) {
        p->rec = 0;
        p->plen[p->cur ^ 1u] = p->rec_pos;
        p->rec_done = 1;
        if (p->rec_started_play) {
            p->playing = 0;
            all_off(p);
        }
    }
}

int vb_player_print_take(vb_player_t *p)
{
    uint32_t nb, len, i;
    int16_t *b;
    int32_t peak = 0;
    if (!p->rec_done)
        return 0;
    p->rec_done = 0;
    nb = p->cur ^ 1u;
    len = p->plen[nb];
    b = p->pbuf[nb];
    if (p->rc->print[2]) {                   /* NORM: the peak to 90 % */
        for (i = 0; i < len; i++)
            peak = b[i] > peak ? b[i] : -b[i] > peak ? -b[i] : peak;
        if (peak > 64)
            for (i = 0; i < len; i++)
                b[i] = (int16_t)(b[i] * 29491 / peak);
    }
    cmd_push(p, CMD(C_SWAP, 0, 0));          /* the audio side switches buffers between blocks */
    return 1;
}

uint32_t vb_player_print_offline(vb_player_t *p)
{
    int32_t tmp[32];
    uint32_t guard = 0;
    cmd_push(p, CMD(C_PRINT, 0, 0));          /* the same path as a live print: the same bits */
    do {
        vb_player_render(p, tmp, 32);
        if (p->idle && (guard & 255u) == 0u)
            p->idle();
    } while (!p->rec_done && guard++ < 200000u);
    vb_player_print_take(p);
    vb_player_render(p, tmp, 32);             /* applies the swap */
    return p->plen[p->cur];
}

void vb_player_load(vb_player_t *p, uint32_t len)
{
    p->plen[p->cur ^ 1u] = len > p->pcap ? p->pcap : len;
    cmd_push(p, CMD(C_SWAP, 0, 0));
}

/* ---- the buffers move between players (the selected track has them), in two cheap steps in the audio thread; the
 * FX buffers are cleared between them, outside it */
void vb_player_detach(vb_player_t *p, vb_bufs_t *b)
{
    uint32_t i;
    for (i = 0; i < VB_FX_SLOTS; i++) {
        b->fx[i] = p->fx[i].buf;
        vb_fx_attach(&p->fx[i], 0);
    }
    b->pbuf[0] = p->pbuf[0];
    b->pbuf[1] = p->pbuf[1];
    b->plen[0] = p->plen[0];
    b->plen[1] = p->plen[1];
    b->pcap = p->pcap;
    b->cur = p->cur;
    p->pbuf[0] = p->pbuf[1] = 0;
    p->plen[0] = p->plen[1] = 0;
    p->rec = 0;
    if (p->eng == &vb_samp || p->eng == &vb_gran || p->eng == &vb_chop)
        voices_reset(p);
}
void vb_player_attach(vb_player_t *p, const vb_bufs_t *b)
{
    uint32_t i;
    for (i = 0; i < VB_FX_SLOTS; i++)
        vb_fx_attach(&p->fx[i], b->fx[i]);
    p->pbuf[0] = b->pbuf[0];
    p->pbuf[1] = b->pbuf[1];
    p->plen[0] = b->plen[0];
    p->plen[1] = b->plen[1];
    p->pcap = b->pcap;
    p->cur = b->cur;
    if (p->pbuf[0])
        vb_samp_set_buffer(p->pbuf[p->cur], p->plen[p->cur]);
    if (p->eng == &vb_samp || p->eng == &vb_gran || p->eng == &vb_chop)
        voices_reset(p);
}

/* ---- the block */
static void commands(vb_player_t *p)
{
    while (p->cmd_r != p->cmd_w) {
        uint32_t c = p->cmd[p->cmd_r % VB_CMDQ];
        int a = (int)((c >> 16) & 0xFFu), b = (int)((c >> 8) & 0xFFu);
        p->cmd_r++;
        switch (c >> 24) {
        case C_NOTE_ON:
            if (p->rc->play.mode == VB_PLAY_ARP) {          /* ARP: a held key feeds the arp */
                uint32_t k;
                for (k = 0; k < p->nheld && p->held[k] != a; k++)
                    ;
                if (k == p->nheld && p->nheld < 8u) {
                    if (!p->nheld && !p->playing) {           /* stopped: the arp runs from this key */
                        p->tick_acc = p->tick_q8;
                        p->arp_i = 0;
                        p->arp_sub = 0;
                    }
                    p->held[p->nheld++] = (uint8_t)a;
                }
            } else if (!p->rec) {            /* keys don't play into a print: REBUILD must reproduce it */
                voice_on(p, a, b);
            }
            break;
        case C_NOTE_OFF: {
            uint32_t k;
            for (k = 0; k < p->nheld; k++)
                if (p->held[k] == a) {
                    p->held[k] = p->held[--p->nheld];
                    break;
                }
            voice_off(p, a);
            break;
        }
        case C_PLAY: transport_start(p); break;
        case C_STOP:
            p->playing = 0;
            p->arp_sub = 0;
            all_off(p);
            break;
        case C_PRINT:
            if (!p->rec)
                print_begin(p);
            break;
        case C_PANIC:
            p->playing = 0;
            p->nheld = 0;
            p->rec = 0;
            p->nev = 0;
            voices_reset(p);
            break;
        case C_SWAP:
            p->cur ^= 1u;
            vb_samp_set_buffer(p->pbuf[p->cur], p->plen[p->cur]);
            if (p->eng == &vb_samp || p->eng == &vb_gran || p->eng == &vb_chop)
                voices_reset(p);
            break;
        }
    }
}

void vb_player_render(vb_player_t *p, int32_t *out, int n)
{
    vb_recipe_t *rc = p->rc;
    int32_t *blk = p->block, gain;
    uint32_t v, any = 0;
    int i;
    if (p->hush >= 2000u && !p->playing && !p->rec && !p->nev && !p->nheld && p->cmd_r == p->cmd_w) {
        for (i = 0; i < n; i++)                  /* asleep (1.5 s of silence, stopped): a note or PLAY wakes it */
            out[i] = 0;
        p->now += (uint32_t)n;
        return;
    }
    if (rc->src != p->eng_src) {
        p->eng_src = rc->src;
        p->eng = engine_of(rc->src);
        voices_reset(p);
    }
    tempo(p);
    mod_block(p, n);
    commands(p);
    if (p->playing) {
        p->tick_acc += (uint32_t)n << 8;
        while (p->tick_acc >= p->tick_q8) {
            p->tick_acc -= p->tick_q8;
            on_16th(p);
            p->n16++;
        }
    } else if (p->nheld && rc->play.mode == VB_PLAY_ARP) {   /* stopped, keys held: the arp on its own clock */
        p->tick_acc += (uint32_t)n << 8;
        while (p->tick_acc >= p->tick_q8) {
            p->tick_acc -= p->tick_q8;
            arp_16th(p);
        }
    }
    ev_run(p, p->now + (uint32_t)n);
    for (i = 0; i < n; i++)
        blk[i] = 0;
    if (p->eng && !p->pbuf[0] && (p->eng == &vb_samp || p->eng == &vb_gran || p->eng == &vb_chop)) {
        /* a sampler source without the print buffers (a track in the background): silent */
    } else if (p->eng) {
        for (v = 0; v < VB_POLY; v++)
            if (p->vlive[v])
                p->vlive[v] = (uint8_t)p->eng->render(p->vmem[v], p->eff, blk, n);
    } else {
        vb_drum_render(&p->drum, &p->effd, blk, n);
    }
    /* headroom for chords: polyphonic engines at half, the kit at 3/4, then VOL */
    gain = (int32_t)((uint32_t)p->vol_q8 * (p->eng ? 16384u : 24576u) / (100u * 256u));
    for (i = 0; i < n; i++)
        blk[i] = vb_sat16(vb_mulq15(vb_clamp(blk[i], -262144, 262143), gain));
    for (v = 0; v < VB_FX_SLOTS; v++)
        if (p->efx[v].type)
            vb_fx_process(&p->fx[v], &p->efx[v], blk, n, p->last_note, p->bpm10);
    for (i = 0; i < n; i++) {
        int32_t s = vb_tanh(vb_clamp(blk[i], -131068, 131068) / 4);   /* a soft limiter: tanh, never past full scale */
        out[i] = s;
        any |= (uint32_t)s;
        if (s > p->peak)
            p->peak = s;
        else if (-s > p->peak)
            p->peak = -s;
    }
    if (p->rec)
        print_capture(p, out, n);
    p->now += (uint32_t)n;
    p->hush = any || p->playing || p->rec ? 0 : p->hush + 1u;
}
