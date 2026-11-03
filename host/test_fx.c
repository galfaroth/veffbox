/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests for the veffbox FX chain. Every type processes a test signal (DRUM hits, then a decaying saw) at its
 * default settings and at hundreds of fuzzed ones (extremes included, some with the knobs moving every block), and:
 *   - the output never leaves 16 bits,
 *   - renders are bit-exact repeatable,
 *   - once the input stops, every tail ends in exact zeros and the slot falls asleep (s->pos stops), except where
 *     the settings hold the sound on purpose (TAPE DLY feedback over unity, FREEZE HOLD),
 *   - a type change leaves the slot exactly as a fresh one,
 *   - ON = 0 passes the input unchanged and leaves the state alone.
 * Writes host/out/fx_<short name>.wav (the default settings, 4 s). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/vb_fx.h"
#include "../core/vb_drum.h"
#include "../core/vb_dsp.h"

#define SIG (VB_SR * 3 / 2)          /* the test signal: sound in the first ~1.3 s */
#define WAVN (VB_SR * 4)
#define TAIL_MAX (VB_SR * 300)       /* the longest tail allowed after the input stops */
#define BLK 32
#define FUZZ 240
#define VB_NOTE_INC_A4 42852281u      /* 440 Hz */                     /* random settings per type */
static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wav(const char *path, const int32_t *x, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return; }
    uint32_t data = (uint32_t)n * 2, riff = 36 + data, sr = VB_SR, br = VB_SR * 2, fmt = 16;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < n; i++) { int16_t s = (int16_t)vb_sat16(x[i]); fwrite(&s, 2, 1, f); }
    fclose(f);
}

static uint32_t hash_step(uint32_t h, int32_t v) { return (h ^ (uint32_t)v) * 16777619u; }

static int32_t sig[SIG], hot[SIG];
static const int32_t *cur = sig;            /* the signal run() plays */
static int16_t bufa[VB_FX_BUF], bufb[VB_FX_BUF];

/* DRUM hits (kick, snare, hat, kick, clap) then a 110 Hz saw with a decaying envelope */
static void make_signal(void)
{
    vb_drum_t d;
    vb_drum_patch_t p;
    vb_drum_patch_default(&p);
    vb_drum_init(&d, 77);
    memset(sig, 0, sizeof sig);
    static const struct { int at, voice; } hits[] = {
        { 0, VB_KICK }, { 11025, VB_SNARE }, { 16538, VB_HAT }, { 22050, VB_KICK }, { 27563, VB_CLAP },
    };
    for (int i = 0; i < SIG; i += BLK) {
        for (unsigned h = 0; h < sizeof hits / sizeof hits[0]; h++)
            if (hits[h].at / BLK * BLK == i)
                vb_drum_trigger(&d, &p, hits[h].voice, 120);
        vb_drum_render(&d, &p, sig + i, SIG - i < BLK ? SIG - i : BLK);
    }
    uint32_t ph = 0, inc = (uint32_t)(110.0 * 4294967296.0 / VB_SR);
    int32_t env = 1 << 30;
    for (int i = 28665; i < 55000; i++) {
        env = vb_mulq30(env, VB_DECAY[100]);
        sig[i] += vb_mulq30(((int32_t)(ph >> 16) - 32768) * 3 / 4, env);
        ph += inc;
    }
    /* hot: a full-scale 55 Hz square with full-scale noise bursts, partly past 16 bits (the slot clamps its input) */
    uint32_t r = 5;
    for (int i = 0; i < SIG; i++) {
        int32_t v = (i / 401) & 1 ? 32767 : -32767;
        if ((i / 2205) % 3 == 1) v = vb_noise(&r);
        hot[i] = i > 52000 ? 0 : (i / 8000) % 2 ? v * 2 : v;
    }
}

/* one pass of n samples through a fresh slot, no tail */
static void pass(vb_fx_slot_t sl, const int32_t *in, int32_t *out, int n, int note)
{
    static vb_fx_state_t st;
    static int16_t b[VB_FX_BUF];
    vb_fx_init(&st, b, 1);
    for (int i = 0; i < n; i += BLK) {
        int m = n - i < BLK ? n - i : BLK;
        memcpy(out + i, in + i, sizeof(int32_t) * (size_t)m);
        vb_fx_process(&st, &sl, out + i, m, note, 1200);
    }
}

static int crossings(const int32_t *x, int from, int to) { int c = 0; for (int i = from + 1; i < to; i++) if ((x[i - 1] < 0) != (x[i] < 0)) c++; return c; }
static double rms(const int32_t *x, int from, int to) { double a = 0; for (int i = from; i < to; i++) a += (double)x[i] * x[i]; return a / (to - from); }

typedef struct {
    uint32_t hash;
    int peak, clipped, last_nz, asleep_at, over;   /* asleep_at: the first sample of a block the slot slept through (-1),
                                                   * over: samples outside 16 bits */
    double ns;
} res_t;

/* one slot through the signal and then silence until it sleeps (32 blocks in a row asleep) or TAIL_MAX. With wobble,
 * the knobs move every block (a random walk, now and then a jump) while the signal plays. out: the first nout
 * samples of the output. *sl ends as the knobs ended. */
static void run(vb_fx_state_t *s, int16_t *buf, vb_fx_slot_t *sl, int note, int bpm10, uint32_t seed, int wobble,
                int32_t *out, int nout, res_t *r)
{
    int32_t blk[BLK];
    uint32_t w = seed * 2654435761u + 1u;
    vb_fx_init(s, buf, seed);
    *r = (res_t){ 2166136261u, 0, 0, -1, -1, 0, 0 };
    clock_t t0 = clock();
    int asleep_run = 0;
    for (int i = 0; i < SIG + TAIL_MAX; i += BLK) {
        if (i < SIG) {
            for (int k = 0; k < BLK; k++)
                blk[k] = i + k < SIG ? cur[i + k] : 0;
            if (wobble && i < SIG - VB_SR / 4) {
                int k = (int)(vb_rng(&w) % 5);
                uint8_t *v = k < 4 ? &sl->p[k] : &sl->mix;
                *v = (uint8_t)(vb_rng(&w) % 16 == 0 ? vb_rng(&w) % 128
                                                    : (uint32_t)vb_clamp(*v + (int)(vb_rng(&w) % 9) - 4, 0, 127));
            }
        } else {
            memset(blk, 0, sizeof blk);
        }
        uint32_t pos = s->pos;
        vb_fx_process(s, sl, blk, BLK, note, bpm10);
        for (int k = 0; k < BLK; k++) {
            int32_t v = blk[k], a = v < 0 ? -v : v;
            r->hash = hash_step(r->hash, v);
            if (a > r->peak) r->peak = a;
            if (a >= 32767) r->clipped++;
            if (v > 32767 || v < -32768) r->over++;
            if (v) r->last_nz = i + k;
            if (out && i + k < nout) out[i + k] = v;
        }
        if (i >= SIG && s->pos == pos) {
            if (r->asleep_at < 0) r->asleep_at = i;
            if (++asleep_run >= 32 && i >= WAVN) break;
        } else if (i >= SIG) {
            r->asleep_at = -1;
            asleep_run = 0;
        }
    }
    r->ns = (double)(clock() - t0) * 1e9 / CLOCKS_PER_SEC;
}

/* settings where the sound is held on purpose: no tail to wait for */
static int holds(const vb_fx_slot_t *sl)
{
    return (sl->type == VB_FX_TAPE && sl->p[1] > 90) || (sl->type == VB_FX_FREEZE && sl->p[3] >= 64);
}

static vb_fx_slot_t slot_default(int t)
{
    vb_fx_slot_t sl = { (uint8_t)t, { 0 }, 100, 1 };
    for (int i = 0; i < 4; i++) sl.p[i] = vb_fx_types[t].def[i];
    return sl;
}

int main(void)
{
    static int32_t out[WAVN], blk[BLK];
    static vb_fx_state_t s, s2;
    char path[64];
    make_signal();

    printf("type      peak  clip  tail ms  sleeps  ns/sample (host)  wet/dry rms\n");
    for (int t = 1; t < VB_FX_TYPES; t++) {
        const vb_fx_desc_t *d = &vb_fx_types[t];
        vb_fx_slot_t sl = slot_default(t);
        res_t r, r2;
        run(&s, bufa, &sl, 57, 1200, 1234, 0, out, WAVN, &r);
        snprintf(path, sizeof path, "host/out/fx_%s.wav", d->shortname);
        wav(path, out, WAVN);
        run(&s2, bufb, &sl, 57, 1200, 1234, 0, NULL, 0, &r2);
        CHECK(r.hash == r2.hash, "%s: not repeatable", d->name);
        CHECK(!r.over, "%s: %d samples outside 16 bits", d->name, r.over);
        CHECK(r.peak > 1000, "%s: silent (peak %d)", d->name, r.peak);
        CHECK(r.asleep_at >= 0 && r.last_nz < r.asleep_at, "%s: the tail does not end (last %d, asleep %d)", d->name,
              r.last_nz, r.asleep_at);
        CHECK(s.quiet > 0, "%s: asleep but not quiet", d->name);
        clock_t t0 = clock();                            /* the cost: the signal 20 times, awake throughout */
        for (int j = 0; j < 20; j++) pass(sl, sig, out, 52000, 57);
        double ns = (double)(clock() - t0) * 1e9 / CLOCKS_PER_SEC / (20.0 * 52000);
        sl.mix = 127;
        pass(sl, sig, out, SIG, 57);
        printf("%-8s %5d %5d %8d %7s %10.1f %16.2f\n", d->name, r.peak, r.clipped,
               r.last_nz < SIG ? 0 : (r.last_nz - SIG) * 1000 / VB_SR, r.asleep_at >= 0 ? "yes" : "NO", ns,
               rms(out, 0, SIG) / rms(sig, 0, SIG));
    }

    /* the labels as the mockup has them */
    CHECK(!strcmp(vb_fx_types[VB_FX_TAPE].name, "TAPE DLY") && !strcmp(vb_fx_types[VB_FX_SHIFT].label[2], "WIN"),
          "descriptor labels");

    /* ON = 0: the input unchanged, the state untouched; ON again carries on from that state */
    for (int t = 1; t < VB_FX_TYPES; t++) {
        vb_fx_slot_t sl = slot_default(t);
        vb_fx_init(&s, bufa, 99);
        for (int i = 0; i < VB_SR / 2; i += BLK) {
            memcpy(blk, sig + i, sizeof blk);
            vb_fx_process(&s, &sl, blk, BLK, 60, 1200);
        }
        s2 = s;
        memcpy(bufb, bufa, sizeof bufa);
        sl.on = 0;
        int same = 1;
        for (int i = VB_SR / 2; i < VB_SR; i += BLK) {
            for (int k = 0; k < BLK; k++) blk[k] = sig[i + k] * 3;   /* (past 16 bits too: passed as it is) */
            vb_fx_process(&s, &sl, blk, BLK, 60, 1200);
            for (int k = 0; k < BLK; k++) same &= blk[k] == sig[i + k] * 3;
        }
        CHECK(same, "%s: ON 0 changes the signal", vb_fx_types[t].name);
        CHECK(!memcmp(&s, &s2, sizeof s) && !memcmp(bufa, bufb, sizeof bufa), "%s: ON 0 changes the state",
              vb_fx_types[t].name);
    }

    /* a type change: from then on exactly a fresh slot of the new type */
    for (int t = 1; t < VB_FX_TYPES; t++) {
        vb_fx_slot_t a = slot_default(t % (VB_FX_TYPES - 1) + 1), b = slot_default(t);
        a.p[1] = 127;                                    /* loud state */
        vb_fx_init(&s, bufa, 555);
        vb_fx_init(&s2, bufb, 555);
        for (int i = 0; i < VB_SR / 2; i += BLK) {
            memcpy(blk, sig + i, sizeof blk);
            vb_fx_process(&s, &a, blk, BLK, 60, 1200);
        }
        uint32_t h1 = 2166136261u, h2 = 2166136261u;
        for (int i = 0; i < SIG - BLK; i += BLK) {
            memcpy(blk, sig + i, sizeof blk);
            vb_fx_process(&s, &b, blk, BLK, 60, 1200);
            for (int k = 0; k < BLK; k++) h1 = hash_step(h1, blk[k]);
            memcpy(blk, sig + i, sizeof blk);
            vb_fx_process(&s2, &b, blk, BLK, 60, 1200);
            for (int k = 0; k < BLK; k++) h2 = hash_step(h2, blk[k]);
        }
        CHECK(h1 == h2, "%s after %s: not a clean reset", vb_fx_types[t].name, vb_fx_types[a.type].name);
    }

    /* what the types do */
    {
        static int32_t in[VB_SR], o[VB_SR];
        vb_fx_slot_t sl;
        /* TAPE DLY at 120 BPM: TIME 16 = a sixteenth (5512 samples, a cell a sample), 64 = a beat (22050, slowed) */
        static const int tm[2] = { 16, 64 };
        for (int j = 0; j < 2; j++) {
            memset(in, 0, sizeof in);
            for (int i = 0; i < 64; i++) in[i] = 20000;
            sl = (vb_fx_slot_t){ VB_FX_TAPE, { (uint8_t)tm[j], 0, 0, 127 }, 127, 1 };
            pass(sl, in, o, VB_SR, 60);
            int at = 0;
            for (int i = 1000; i < VB_SR; i++) if (abs(o[i]) > abs(o[at])) at = i;
            int want = 22050 * tm[j] / 64;
            CHECK(abs(at - want) < 100, "TAPE DLY TIME %d: the echo at %d, not %d", tm[j], at, want);
            printf("TAPE DLY TIME %d: echo at %d samples (%d expected), %d\n", tm[j], at, want, o[at]);
        }
        /* SHIFT: +12 and -12 semitones of a 330 Hz sine */
        for (int i = 0; i < VB_SR; i++) in[i] = vb_sin((uint32_t)i * (uint32_t)(330.0 * 4294967296.0 / VB_SR)) / 2;
        static const int pt[2] = { 96, 32 };
        for (int j = 0; j < 2; j++) {
            sl = (vb_fx_slot_t){ VB_FX_SHIFT, { (uint8_t)pt[j], 0, 30, 127 }, 127, 1 };
            pass(sl, in, o, VB_SR, 60);
            double ratio = crossings(o, 8820, VB_SR) / (double)crossings(in, 8820, VB_SR);
            CHECK(ratio > (j ? 0.45 : 1.9) && ratio < (j ? 0.55 : 2.1), "SHIFT PTCH %d: pitch x %.3f", pt[j], ratio);
            printf("SHIFT PTCH %d: pitch x %.3f\n", pt[j], ratio);
        }
        /* RESON: struck once, rings at the played note (A3, 220 Hz), a fifth over it with TUNE */
        static const int tu[2] = { 64, 82 };
        for (int j = 0; j < 2; j++) {
            memset(in, 0, sizeof in);
            for (int i = 0; i < 32; i++) in[i] = i < 16 ? 15000 : -15000;
            sl = (vb_fx_slot_t){ VB_FX_RESON, { (uint8_t)tu[j], 100, 90, 0 }, 127, 1 };
            pass(sl, in, o, VB_SR, 57);
            double hz = crossings(o, 4410, 26460) / 2.0 / 0.5, want = j ? 329.6 : 220.0;
            CHECK(hz > want * 0.98 && hz < want * 1.02, "RESON TUNE %d: %.1f Hz, not %.1f", tu[j], hz, want);
            printf("RESON TUNE %d: %.1f Hz (%.1f expected)\n", tu[j], hz, want);
        }
        /* FILTER: a 5 kHz sine through a 150 Hz low-pass, band-pass and high-pass */
        for (int i = 0; i < VB_SR; i++) in[i] = vb_sin((uint32_t)i * (uint32_t)(5000.0 * 4294967296.0 / VB_SR)) / 2;
        double g[3];
        for (int j = 0; j < 3; j++) {
            sl = (vb_fx_slot_t){ VB_FX_FILTER, { 38, 0, (uint8_t)(j * 63 + (j == 2)), 0 }, 127, 1 };
            pass(sl, in, o, VB_SR / 4, 60);
            g[j] = rms(o, 4410, VB_SR / 4) / rms(in, 4410, VB_SR / 4);
        }
        CHECK(g[0] < 0.01 && g[1] < 0.1 && g[2] > 0.9, "FILTER LP %.4f BP %.4f HP %.4f", g[0], g[1], g[2]);
        printf("FILTER at 150 Hz, a 5 kHz sine: LP %.4f BP %.4f HP %.4f (power)\n", g[0], g[1], g[2]);
        /* RING on a constant: the carrier itself. KEY 127, TUNE 60: at the played note; KEY 0, TUNE 69: 440 Hz fixed */
        for (int i = 0; i < VB_SR; i++) in[i] = 10000;
        static const uint8_t rk[2][3] = { { 60, 127, 69 }, { 69, 0, 40 } };
        for (int j = 0; j < 2; j++) {
            sl = (vb_fx_slot_t){ VB_FX_RING, { rk[j][0], 0, 0, rk[j][1] }, 127, 1 };
            pass(sl, in, o, VB_SR, rk[j][2]);
            double hz = crossings(o, 0, VB_SR) / 2.0;
            CHECK(hz > 436 && hz < 444, "RING TUNE %d KEY %d: carrier %.1f Hz", rk[j][0], rk[j][1], hz);
            printf("RING TUNE %d KEY %d note %d: carrier %.1f Hz (440 expected)\n", rk[j][0], rk[j][1], rk[j][2], hz);
        }
    }

    /* knob jumps glide: a 200 Hz sine through, one knob jumped half way; no step in the output much over the
     * sine's own steepest step (x 3: the glides bend the pitch) */
    {
        static int32_t in[VB_SR], o[VB_SR];
        static const struct { uint8_t type, p[4], k, to; } jump[] = {
            { VB_FX_TAPE, { 16, 60, 10, 127 }, 0, 100 }, { VB_FX_TAPE, { 16, 60, 10, 127 }, 4, 0 },
            { VB_FX_CHORUS, { 40, 20, 40, 100 }, 1, 127 }, { VB_FX_SHIFT, { 96, 40, 10, 127 }, 2, 127 },
            { VB_FX_FILTER, { 127, 60, 0, 0 }, 0, 20 }, { VB_FX_PHASER, { 30, 80, 10, 60 }, 2, 120 },
            { VB_FX_RESON, { 64, 60, 100, 0 }, 3, 100 }, { VB_FX_FREEZE, { 30, 30, 64, 0 }, 0, 127 },
        };
        for (int i = 0; i < VB_SR; i++) in[i] = vb_sin((uint32_t)i * (uint32_t)(200.0 * 4294967296.0 / VB_SR)) / 3;
        for (unsigned j = 0; j < sizeof jump / sizeof jump[0]; j++) {
            vb_fx_slot_t sl = { jump[j].type, { jump[j].p[0], jump[j].p[1], jump[j].p[2], jump[j].p[3] }, 127, 1 };
            vb_fx_init(&s, bufa, 3);
            int before = 0, after = 0;
            for (int i = 0; i < VB_SR; i += BLK) {
                if (i == VB_SR / 2) {
                    if (jump[j].k < 4) sl.p[jump[j].k] = jump[j].to; else sl.mix = jump[j].to;
                }
                memcpy(o + i, in + i, sizeof(int32_t) * BLK);
                vb_fx_process(&s, &sl, o + i, BLK, 57, 1200);
            }
            for (int i = VB_SR / 4; i < VB_SR; i++) {
                int d = abs(o[i] - o[i - 1]);
                if (i < VB_SR / 2) { if (d > before) before = d; } else if (d > after) after = d;
            }
            CHECK(after <= before * 3 + 300, "%s: knob %d jump steps %d (before %d)", vb_fx_types[jump[j].type].name,
                  jump[j].k, after, before);
            printf("%-8s knob %d %3d -> %3d: steepest step %5d after, %5d before\n", vb_fx_types[jump[j].type].name,
                   jump[j].k, jump[j].k < 4 ? jump[j].p[jump[j].k] : 127, jump[j].to, after, before);
        }
    }

    /* fuzz: extremes and random settings, half of them with the knobs moving, a third on a hot signal */
    uint32_t rng = 4242;
    for (int t = 1; t < VB_FX_TYPES; t++) {
        int worst_clip = 0, longest = 0, held = 0;
        for (int k = 0; k < FUZZ; k++) {
            vb_fx_slot_t sl = { (uint8_t)t, { 0 }, 0, 1 };
            for (int i = 0; i < 5; i++) {
                uint32_t c = vb_rng(&rng) % 4;
                uint8_t v = (uint8_t)(c == 0 ? 0 : c == 1 ? 127 : vb_rng(&rng) % 128);
                if (i < 4) sl.p[i] = v; else sl.mix = v;
            }
            int note = 12 + (int)(vb_rng(&rng) % 100), bpm10 = 300 + (int)(vb_rng(&rng) % 2700), wob = k & 1;
            res_t r, r2;
            vb_fx_slot_t start = sl;
            cur = k % 3 == 2 ? hot : sig;
            run(&s, bufa, &sl, note, bpm10, k + 1u, wob, NULL, 0, &r);
            const char *nm = vb_fx_types[t].name;
            CHECK(!r.over, "fuzz %s %d: %d samples outside 16 bits", nm, k, r.over);
            if (k % 8 == 0) {
                vb_fx_slot_t again = start;
                run(&s2, bufb, &again, note, bpm10, k + 1u, wob, NULL, 0, &r2);
                CHECK(r.hash == r2.hash, "fuzz %s %d: not repeatable", nm, k);
            }
            /* the knobs as they ended (sl: the wobble moved them) decide whether the tail may hold */
            if (holds(&sl)) {
                held++;
            } else {
                CHECK(r.asleep_at >= 0 && r.last_nz < r.asleep_at,
                      "fuzz %s %d (%d %d %d %d mix %d%s): the tail does not end", nm, k, sl.p[0], sl.p[1], sl.p[2],
                      sl.p[3], sl.mix, wob ? ", moved" : "");
            }
            if (cur == sig && r.clipped > worst_clip) worst_clip = r.clipped;
            if (r.asleep_at > longest) longest = r.asleep_at;
        }
        printf("fuzz %-8s %d settings: most clipped samples (drums + saw) %6d, longest until asleep %5.1f s, %d held on purpose\n",
               vb_fx_types[t].name, FUZZ, worst_clip, (longest - SIG) / (double)VB_SR, held);
    }

    printf(failures ? "\n%d FAILED\n" : "\nall fx tests passed\n", failures);
    return failures != 0;
}
