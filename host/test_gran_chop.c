/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests for GRAN and CHOP, the engines that play a print. A synthetic drum loop (8 decaying noise / sine hits at
 * known positions, 22.05 kHz) is the print. Renders host/out/gran_*.wav and host/out/chop_*.wav, and checks:
 *   - descriptors and voice state size,
 *   - CHOP finds the 8 hits within one detection block at a middle SENS, fewer at a low SENS, and caches the result,
 *   - each key plays the right slice (by correlation with the print), in every ORDER, and MAP STEPS walks the slices,
 *   - STUT repeats while held and stops after note_off, a retrigger doesn't click,
 *   - GRAN sounds at every WIN, renders bit-exactly again and independently of the block size, never overflows
 *     16 bits under fuzzed patches (with extremes) on the loop and on a tiny print, and stops after note_off,
 *   - no print (or a tiny one) is silence, without a crash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/vb_gran.h"
#include "../core/vb_chop.h"
#include "../core/vb_samp.h"
#include "../core/vb_dsp.h"

#define PSR 22050
#define PLEN (PSR * 2)
#define MAXN (VB_SR * 6)
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

static uint32_t hash(const int32_t *x, int n) { uint32_t h = 2166136261u; for (int i = 0; i < n; i++) h = (h ^ (uint32_t)x[i]) * 16777619u; return h; }
static int peak(const int32_t *x, int n) { int p = 0; for (int i = 0; i < n; i++) if (abs(x[i]) > p) p = abs(x[i]); return p; }
static int maxstep(const int32_t *x, int from, int to) { int m = 0; for (int i = from + 1; i < to; i++) if (abs(x[i] - x[i - 1]) > m) m = abs(x[i] - x[i - 1]); return m; }
static void defaults(const vb_engine_t *e, uint8_t *p) { for (int i = 0; i < e->desc.nparams; i++) p[i] = e->desc.params[i].def; }

/* ---------------- the print: 8 hits ---------------- */

static int16_t loop[PLEN];
static const int HIT[8] = { 0, 5200, 11025, 16300, 22050, 27900, 33075, 38800 };
static const double AMP[8] = { 1.0, 0.6, 0.85, 0.5, 0.9, 0.55, 0.75, 0.45 };

static void make_loop(void)
{
    uint32_t r = 12345;
    double acc[PLEN];
    memset(acc, 0, sizeof acc);
    for (int h = 0; h < 8; h++) {
        int sine = h % 2 == 0;
        double env = AMP[h] * 30000, dk = sine ? 0.99955 : 0.9992;
        uint32_t ph = 0, inc = (uint32_t)(4294967296.0 * (sine ? 70.0 + 30 * h : 0) / PSR);
        for (int i = HIT[h]; i < PLEN && env > 1; i++, env *= dk) {
            double x = sine ? vb_sin(ph) / 32768.0 : (double)vb_noise(&r) / 32768.0;
            ph += inc;
            if (sine) inc = (uint32_t)(inc * 0.99995);   /* a kick's pitch drop */
            acc[i] += x * env;
        }
    }
    for (int i = 0; i < PLEN; i++) loop[i] = (int16_t)vb_sat16((int32_t)acc[i]);
}

/* one note on one voice, note_off after `gate` samples, blocks of `blk`; returns where render() returned 0 (-1: never) */
static int note(const vb_engine_t *e, const uint8_t *p, int nt, int vel, int gate, int32_t *out, int n, int blk, uint32_t seed)
{
    _Alignas(8) uint8_t v[VB_VOICE_BYTES];
    e->voice_init(v, seed);
    e->note_on(v, p, nt, vel);
    memset(out, 0, sizeof(int32_t) * (size_t)n);
    int stop = -1;
    for (int i = 0; i < n; i += blk) {
        if (i >= gate && i - blk < gate) e->note_off(v, p);
        int m = n - i < blk ? n - i : blk;
        if (!e->render(v, p, out + i, m)) {
            stop = i;
            int32_t z[32] = { 0 };
            int again = e->render(v, p, z, 32);
            CHECK(!again && peak(z, 32) == 0, "%s: a stopped voice rendered again", e->desc.name);
            break;
        }
    }
    return stop;
}

static void desc_checks(const vb_engine_t *e, int np, const char *const *labels)
{
    CHECK(e->desc.nparams == np, "%s: %d params, want %d", e->desc.name, e->desc.nparams, np);
    CHECK(e->voice_size <= VB_VOICE_BYTES, "%s: voice %d bytes > %d", e->desc.name, e->voice_size, VB_VOICE_BYTES);
    for (int i = 0; i < e->desc.nparams && i < np; i++) {
        const vb_param_t *q = &e->desc.params[i];
        CHECK(!strcmp(q->label, labels[i]), "%s: param %d is %s, want %s", e->desc.name, i, q->label, labels[i]);
        CHECK(q->def >= q->min && q->def <= q->max, "%s: %s default out of range", e->desc.name, q->label);
        CHECK(!(q->flags & VB_P_LIST) || q->names, "%s: list %s without names", e->desc.name, q->label);
    }
    printf("%-4s: %d params, voice %d bytes\n", e->desc.name, e->desc.nparams, e->voice_size);
}

static void fuzz(const vb_engine_t *e, int count, uint32_t seed, int32_t *a)
{
    uint32_t rng = seed;
    int worst = 0, longest = 0;
    uint8_t p[16];
    for (int k = 0; k < count; k++) {
        for (int i = 0; i < e->desc.nparams; i++) {
            const vb_param_t *q = &e->desc.params[i];
            p[i] = (uint8_t)(q->min + vb_rng(&rng) % (uint32_t)(q->max - q->min + 1));
            if (k % 3 == 0 && (vb_rng(&rng) & 1)) p[i] = (vb_rng(&rng) & 1) ? q->max : q->min;   /* extremes */
        }
        int nt = (int)(vb_rng(&rng) % 128), vel = k % 3 == 0 ? 127 : 1 + (int)(vb_rng(&rng) % 127);
        int gate = (int)(vb_rng(&rng) % (VB_SR * 2));
        int stop = note(e, p, nt, vel, gate, a, MAXN, 32, rng);
        int pk = peak(a, stop < 0 ? MAXN : stop + 32);
        if (pk > worst) worst = pk;
        CHECK(pk <= 32767, "%s fuzz %d: overflow %d", e->desc.name, k, pk);
        if (e == &vb_gran) {
            CHECK(stop >= 0 && stop - gate <= VB_SR / 10, "GRAN fuzz %d: still sounding %d samples after note_off", k, stop - gate);
            if (stop - gate > longest) longest = stop - gate;
        } else {
            CHECK(stop >= 0, "CHOP fuzz %d: never stopped", k);
        }
    }
    printf("%-4s fuzz: %d patches, peak %d, longest tail %d ms after note_off\n", e->desc.name, count, worst, longest * 1000 / VB_SR);
}

/* ---------------- CHOP ---------------- */

/* which slice an output (PITCH 0: two output samples per print sample, forward) plays: best normalised correlation */
static int which(const int32_t *out, const uint16_t *st, int n, double *best_c)
{
    int best = -1;
    double bc = -2;
    for (int j = 0; j < n; j++) {
        double sxy = 0, sxx = 0, syy = 0;
        for (int m = 64; m < 1500 && st[j] + m < PLEN; m++) {
            double x = out[2 * m], y = loop[st[j] + m];
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        double c = sxx > 0 && syy > 0 ? sxy / __builtin_sqrt(sxx * syy) : 0;
        if (c > bc) { bc = c; best = j; }
    }
    if (best_c) *best_c = bc;
    return best;
}

static void chop_phrase(const uint8_t *p, int32_t *a, int n, const char *path)
{
    _Alignas(8) uint8_t v[4][VB_VOICE_BYTES];
    static const int notes[16] = { 60, 61, 62, 63, 64, 65, 66, 67, 60, 60, 62, 60, 67, 66, 61, 60 };
    int step = VB_SR / 6, off[4] = { -1, -1, -1, -1 };
    for (int k = 0; k < 4; k++) vb_chop.voice_init(v[k], (uint32_t)k);
    memset(a, 0, sizeof(int32_t) * (size_t)n);
    for (int i = 0; i < n; i += 32) {
        if (i % step == 0 && i / step < 16) { int k = (i / step) % 4; vb_chop.note_on(v[k], p, notes[i / step], 110); off[k] = i + step / 2; }
        for (int k = 0; k < 4; k++) {
            if (off[k] >= 0 && i >= off[k]) { vb_chop.note_off(v[k], p); off[k] = -1; }
            vb_chop.render(v[k], p, a + i, n - i < 32 ? n - i : 32);
        }
    }
    wav(path, a, n);
}

static void chop_tests(int32_t *a, int32_t *b)
{
    const vb_engine_t *e = &vb_chop;
    static const char *const L[11] = { "SENS", "SLICE", "PITCH", "REV", "ORDER", "LEN", "ATK", "DEC", "MAP", "SPLIT", "SLICES" };
    uint8_t p[VB_CH_NPARAMS];
    uint16_t st[VB_CH_MAXSLICES];
    _Alignas(8) uint8_t v[VB_VOICE_BYTES];
    desc_checks(e, 11, L);

    /* detection at a middle SENS (set through a voice's note_on: vb_chop_slices uses the last SENS seen) */
    defaults(e, p);
    p[VB_CH_SENS] = 64;
    e->voice_init(v, 1); e->note_on(v, p, 60, 100);
    int n = vb_chop_slices(st, VB_CH_MAXSLICES);
    printf("CHOP SENS 64: %d slices:", n);
    for (int i = 0; i < n; i++) printf(" %d", st[i]);
    printf("\n");
    CHECK(n == 8, "CHOP SENS 64: %d slices, want 8", n);
    for (int i = 0; i < n && i < 8; i++)
        CHECK(abs((int)st[i] - HIT[i]) <= 256, "CHOP slice %d at %d, hit at %d", i, st[i], HIT[i]);
    for (int i = 1; i < n; i++) CHECK(st[i] > st[i - 1], "CHOP slices not ascending");
    /* lower and higher SENS */
    int prev = 0;
    for (int s = 0; s <= 127; s += 16) {
        p[VB_CH_SENS] = (uint8_t)s;
        e->note_on(v, p, 60, 100);
        int m = vb_chop_slices(st, VB_CH_MAXSLICES);
        printf("CHOP SENS %3d: %d slices\n", s, m);
        CHECK(m >= prev, "CHOP: SENS %d finds fewer slices (%d) than a lower SENS (%d)", s, m, prev);
        CHECK(m >= 1 && m <= VB_CH_MAXSLICES, "CHOP: SENS %d: %d slices", s, m);
        prev = m;
    }
    p[VB_CH_SENS] = 10;
    e->note_on(v, p, 60, 100);
    int lo = vb_chop_slices(st, VB_CH_MAXSLICES);
    CHECK(lo < 8 && lo >= 1, "CHOP SENS 10: %d slices, want fewer than 8", lo);
    /* the cache follows a change of contents at the same pointer and length */
    p[VB_CH_SENS] = 64;
    e->note_on(v, p, 60, 100);
    {
        static int16_t save[PLEN];
        memcpy(save, loop, sizeof loop);
        memset(loop, 0, sizeof loop);
        for (int i = 0; i < 3000; i++) loop[i] = (int16_t)((i & 64) ? 8000 : -8000);
        uint16_t one[4];
        int m = vb_chop_slices(one, 4);
        CHECK(m == 1, "CHOP: changed contents not re-analysed (%d slices)", m);
        memcpy(loop, save, sizeof loop);
        m = vb_chop_slices(st, VB_CH_MAXSLICES);
        CHECK(m == 8, "CHOP: restored contents: %d slices", m);
    }
    /* SPLIT EVEN: SLICES equal parts, whatever the transients */
    p[VB_CH_SPLIT] = VB_CH_EVEN;
    for (int k = 2; k <= VB_CH_MAXSLICES; k *= 2) {
        p[VB_CH_SLICES] = (uint8_t)k;
        e->note_on(v, p, 60, 100);
        int m = vb_chop_slices(st, VB_CH_MAXSLICES);
        CHECK(m == k, "CHOP EVEN %d: %d slices", k, m);
        for (int i = 0; i < m; i++)
            CHECK(abs((int)st[i] - (int)((uint32_t)PLEN * (uint32_t)i / (uint32_t)k)) <= 1, "CHOP EVEN %d: slice %d at %d", k, i, st[i]);
    }
    printf("CHOP EVEN: 2 .. 64 equal slices\n");
    p[VB_CH_SPLIT] = VB_CH_TRANS;
    e->note_on(v, p, 60, 100);
    n = vb_chop_slices(st, VB_CH_MAXSLICES);
    CHECK(n == 8, "CHOP: back to TRANS: %d slices", n);

    /* each key plays its slice: SEQ, REV, SHUF, and keys wrap */
    defaults(e, p); p[VB_CH_DEC] = 127;
    int seen[8] = { 0 }, ident = 1;
    for (int order = 0; order < 4; order++) {
        if (order == VB_CH_STUT) continue;
        p[VB_CH_ORDER] = (uint8_t)order;
        for (int k = -8; k < 16; k++) {
            double c;
            note(e, p, 60 + k, 127, VB_SR, a, VB_SR / 4, 32, 1);
            int got = which(a, st, n, &c), kk = ((k % n) + n) % n;
            int want = order == VB_CH_SEQ ? kk : order == VB_CH_RORD ? n - 1 - kk : got;
            CHECK(got == want && c > 0.99, "CHOP ORDER %d key %d: plays slice %d (corr %.3f), want %d", order, k, got, c, want);
            if (order == VB_CH_SHUF && k >= 0 && k < 8 && got >= 0) { seen[got]++; if (got != k) ident = 0; }
        }
    }
    for (int j = 0; j < 8; j++) CHECK(seen[j] == 1, "CHOP SHUF: slice %d played by %d of 8 keys", j, seen[j]);
    CHECK(!ident, "CHOP SHUF is the identity");
    printf("CHOP keys: SEQ, REV, SHUF map keys to slices as specified\n");
    /* SHUF is fixed: the same key gives the same slice again */
    {
        p[VB_CH_ORDER] = VB_CH_SHUF;
        note(e, p, 63, 127, VB_SR, a, VB_SR / 4, 32, 1);
        note(e, p, 63, 127, VB_SR, b, VB_SR / 4, 32, 9);
        CHECK(hash(a, VB_SR / 4) == hash(b, VB_SR / 4), "CHOP SHUF not repeatable");
    }
    /* MAP STEPS: one key walks the slices; voice_init restarts the walk */
    defaults(e, p); p[VB_CH_DEC] = 127; p[VB_CH_MAP] = VB_CH_STEPS;
    for (int pass = 0; pass < 2; pass++) {
        e->voice_init(v, 3);
        for (int k = 0; k < 10; k++) {
            e->note_on(v, p, 62, 127);
            memset(a, 0, sizeof(int32_t) * VB_SR / 4);
            for (int i = 0; i < VB_SR / 4; i += 32) e->render(v, p, a + i, 32);
            e->note_off(v, p);
            for (int i = 0; i < 4000; i++) { int32_t z[32] = { 0 }; if (!e->render(v, p, z, 32)) break; }
            int got = which(a, st, n, 0);
            CHECK(got == (2 + k) % n, "CHOP STEPS pass %d note %d: slice %d, want %d", pass, k, got, (2 + k) % n);
        }
    }
    printf("CHOP STEPS: repeating a key walks the slices\n");

    /* STUT: keeps repeating while held, stops after note_off; the slice alone stops by itself */
    defaults(e, p); p[VB_CH_ORDER] = VB_CH_STUT; p[VB_CH_DEC] = 127;
    int stop = note(e, p, 61, 127, VB_SR * 3, a, VB_SR * 5, 32, 1);
    int slice_out = (st[2] - st[1]) * 2;
    CHECK(stop > VB_SR * 3 && stop < VB_SR * 3 + slice_out / 4 + 2000, "CHOP STUT: stopped at %d (note_off at %d)", stop, VB_SR * 3);
    {   /* repeats: the output a segment later is the same */
        int seg = slice_out / 4, same = 1;
        for (int i = 2000; i < 2500; i++) if (a[i] != a[i + seg]) same = 0;
        if (!same) { /* the segment length is rounded: find it */
            same = 0;
            for (int d = seg - 8; d <= seg + 8 && !same; d++) { int ok = 1; for (int i = 2000; i < 2500; i++) if (a[i] != a[i + d]) { ok = 0; break; } same = ok; }
        }
        CHECK(same, "CHOP STUT: no repeat after one segment");
    }
    wav("host/out/chop_stut_note.wav", a, VB_SR * 4);
    p[VB_CH_ORDER] = VB_CH_SEQ;
    stop = note(e, p, 61, 127, VB_SR * 3, a, VB_SR * 5, 32, 1);
    CHECK(stop > 0 && abs(stop - slice_out) <= 64, "CHOP one-shot: stopped at %d, the slice lasts %d", stop, slice_out);
    p[VB_CH_LEN] = 32;
    stop = note(e, p, 61, 127, 100, a, VB_SR * 5, 32, 1);
    CHECK(stop > 0 && abs(stop - slice_out * 32 / 127) <= 64, "CHOP LEN 32: stopped at %d, want ~%d", stop, slice_out * 32 / 127);
    /* REV plays the slice backwards: ends where the hit starts */
    defaults(e, p); p[VB_CH_REV] = 1; p[VB_CH_DEC] = 127; p[VB_CH_LEN] = 80;
    stop = note(e, p, 62, 127, VB_SR, a, VB_SR, 32, 1);
    CHECK(stop > 0 && peak(a + stop - 600, 400) > peak(a + 100, 400) * 2, "CHOP REV: the end isn't the loud part");
    /* DEC: a short decay ends the slice early */
    defaults(e, p); p[VB_CH_DEC] = 10;
    stop = note(e, p, 61, 127, 100, a, VB_SR * 2, 32, 1);
    CHECK(stop > 0 && stop < 2000, "CHOP DEC 10: stopped at %d", stop);
    /* a retrigger of a sounding voice doesn't click */
    defaults(e, p); p[VB_CH_DEC] = 127;
    e->voice_init(v, 1); e->note_on(v, p, 62, 120);
    memset(a, 0, sizeof(int32_t) * VB_SR);
    int at = 4000 + 13;   /* in the middle of the slice's loud part */
    for (int i = 0; i < VB_SR; i += 32) { if (i == (at & ~31)) e->note_on(v, p, 65, 120); e->render(v, p, a + i, 32); }
    {
        int t = at & ~31, before = maxstep(a, t - 1000, t), dur = maxstep(a, t, t + 70);
        CHECK(dur <= before + 500, "CHOP retrigger clicks: step %d vs %d", dur, before);
    }

    /* phrases */
    defaults(e, p);
    static const char *const ON[4] = { "seq", "shuf", "stut", "rev" };
    char path[64];
    for (int o = 0; o < 4; o++) {
        p[VB_CH_ORDER] = (uint8_t)o;
        snprintf(path, sizeof path, "host/out/chop_%s.wav", ON[o]);
        chop_phrase(p, a, VB_SR * 3, path);
        CHECK(peak(a, VB_SR * 3) > 3000, "CHOP %s phrase too quiet", ON[o]);
        printf("CHOP %-4s phrase peak %5d  hash %08x\n", ON[o], peak(a, VB_SR * 3), hash(a, VB_SR * 3));
    }
    p[VB_CH_ORDER] = 0; p[VB_CH_MAP] = VB_CH_STEPS; p[VB_CH_PITCH] = 19; p[VB_CH_LEN] = 70; p[VB_CH_DEC] = 60;
    chop_phrase(p, a, VB_SR * 3, "host/out/chop_steps.wav");
    p[VB_CH_MAP] = 0; p[VB_CH_REV] = 1; p[VB_CH_PITCH] = 31; p[VB_CH_ATK] = 40; p[VB_CH_LEN] = 127;
    chop_phrase(p, a, VB_SR * 3, "host/out/chop_revplay.wav");

    /* repeatability */
    defaults(e, p); p[VB_CH_ORDER] = VB_CH_STUT; p[VB_CH_PITCH] = 30;
    note(e, p, 64, 100, VB_SR, a, VB_SR * 2, 32, 1);
    note(e, p, 64, 100, VB_SR, b, VB_SR * 2, 32, 1);
    CHECK(hash(a, VB_SR * 2) == hash(b, VB_SR * 2), "CHOP render is not repeatable");

    fuzz(e, 300, 4242, a);
}

/* ---------------- GRAN ---------------- */

static void gran_phrase(const uint8_t *p, int32_t *a, int n)
{
    _Alignas(8) uint8_t v[4][VB_VOICE_BYTES];
    static const int notes[8] = { 48, 55, 60, 63, 67, 72, 70, 67 };
    int step = VB_SR / 3, off[4] = { -1, -1, -1, -1 };
    for (int k = 0; k < 4; k++) vb_gran.voice_init(v[k], 100u + (uint32_t)k);
    memset(a, 0, sizeof(int32_t) * (size_t)n);
    for (int i = 0; i < n; i += 32) {
        if (i % step == 0 && i / step < 8) { int k = (i / step) % 4; vb_gran.note_on(v[k], p, notes[i / step], 100); off[k] = i + step; }
        for (int k = 0; k < 4; k++) {
            if (off[k] >= 0 && i >= off[k]) { vb_gran.note_off(v[k], p); off[k] = -1; }
            vb_gran.render(v[k], p, a + i, n - i < 32 ? n - i : 32);
        }
    }
}

static void gran_tests(int32_t *a, int32_t *b)
{
    const vb_engine_t *e = &vb_gran;
    static const char *const L[11] = { "POS", "SIZE", "DENS", "SPRAY", "PITCH", "PRND", "REV", "WIN", "FRZ", "WIDTH", "SEED" };
    static const char *const WN[4] = { "hann", "tri", "rect", "exp" };
    uint8_t p[VB_GR_NPARAMS];
    char path[64];
    desc_checks(e, 11, L);

    for (int w = 0; w < 4; w++) {
        defaults(e, p); p[VB_GR_WIN] = (uint8_t)w; p[VB_GR_SIZE] = 50; p[VB_GR_DENS] = 70;
        gran_phrase(p, a, VB_SR * 4);
        snprintf(path, sizeof path, "host/out/gran_%s.wav", WN[w]);
        wav(path, a, VB_SR * 4);
        int pk = peak(a, VB_SR * 4);
        CHECK(pk > 2000, "GRAN WIN %s too quiet (%d)", WN[w], pk);
        printf("GRAN %-4s phrase peak %5d  hash %08x\n", WN[w], pk, hash(a, VB_SR * 4));
        note(e, p, 60, 100, VB_SR, a, VB_SR * 2, 32, 7);
        note(e, p, 60, 100, VB_SR, b, VB_SR * 2, 32, 7);
        CHECK(hash(a, VB_SR * 2) == hash(b, VB_SR * 2), "GRAN WIN %s: not repeatable", WN[w]);
        CHECK(peak(a, VB_SR) > 1000, "GRAN WIN %s: single note too quiet", WN[w]);
    }
    /* textures */
    struct { const char *name; int v[11]; } R[] = {
        { "cloud", { 64, 90, 110, 80, 24, 20, 30, 0, 0, 100, 5 } },
        { "frozen", { 30, 40, 80, 60, 24, 40, 40, 0, 1, 60, 9 } },
        { "stutter", { 20, 10, 40, 0, 24, 0, 0, 2, 0, 0, 0 } },
        { "reverse_up", { 90, 70, 90, 30, 36, 0, 127, 1, 0, 30, 3 } },
        { "shimmer", { 50, 60, 127, 127, 24, 127, 64, 3, 0, 127, 77 } },
    };
    for (unsigned k = 0; k < sizeof R / sizeof R[0]; k++) {
        for (int i = 0; i < 11; i++) p[i] = (uint8_t)R[k].v[i];
        gran_phrase(p, a, VB_SR * 4);
        snprintf(path, sizeof path, "host/out/gran_%s.wav", R[k].name);
        wav(path, a, VB_SR * 4);
        CHECK(peak(a, VB_SR * 4) > 1000, "GRAN %s too quiet", R[k].name);
        printf("GRAN %-10s peak %5d  hash %08x\n", R[k].name, peak(a, VB_SR * 4), hash(a, VB_SR * 4));
    }

    /* block size independence, seeds: SEED and the voice seed both matter; FRZ repeats a cycle */
    defaults(e, p); p[VB_GR_SPRAY] = 90; p[VB_GR_PRND] = 50; p[VB_GR_REV] = 60;
    note(e, p, 60, 100, VB_SR, a, VB_SR * 2, 32, 7);
    note(e, p, 60, 100, VB_SR, b, VB_SR * 2, 7, 7);
    CHECK(hash(a, VB_SR) == hash(b, VB_SR), "GRAN render depends on the block size");
    note(e, p, 60, 100, VB_SR, b, VB_SR * 2, 32, 8);
    CHECK(hash(a, VB_SR) != hash(b, VB_SR), "GRAN: the voice seed changes nothing");
    p[VB_GR_SEED] = 18;
    note(e, p, 60, 100, VB_SR, b, VB_SR * 2, 32, 7);
    CHECK(hash(a, VB_SR) != hash(b, VB_SR), "GRAN: SEED changes nothing");
    p[VB_GR_FRZ] = 1; p[VB_GR_DENS] = 64; p[VB_GR_WIDTH] = 127;
    note(e, p, 60, 100, VB_SR * 3, a, VB_SR * 3, 32, 7);
    {
        uint32_t iv = 0;   /* DENS 64: 22050 >> 3 * 2^(-10/18) */
        iv = ((22050u >> 3) * 44590u) >> 16;
        int per = (int)iv * 8;
        CHECK(!memcmp(a + VB_SR, a + VB_SR + per, sizeof(int32_t) * 5000), "GRAN FRZ: no repeat after 8 grains (%d samples)", per);
    }

    /* release */
    defaults(e, p);
    int stop = note(e, p, 60, 100, VB_SR / 2, a, VB_SR * 2, 32, 1);
    CHECK(stop >= VB_SR / 2 && stop <= VB_SR / 2 + 2700, "GRAN: stopped at %d, note_off at %d", stop, VB_SR / 2);
    printf("GRAN: stops %d ms after note_off\n", (stop - VB_SR / 2) * 1000 / VB_SR);

    fuzz(e, 300, 999, a);
}

/* ---------------- no print, a tiny print ---------------- */

static void edge_tests(int32_t *a)
{
    const vb_engine_t *E[2] = { &vb_gran, &vb_chop };
    uint8_t p[16];
    uint16_t st[8];
    static int16_t tiny[100];
    for (int i = 0; i < 100; i++) tiny[i] = (int16_t)(i * 300 - 15000);
    for (int k = 0; k < 2; k++) {
        const vb_engine_t *e = E[k];
        defaults(e, p);
        vb_samp_set_buffer(0, 0);
        int stop = note(e, p, 60, 127, VB_SR, a, VB_SR, 32, 1);
        CHECK(stop == 0 && peak(a, VB_SR) == 0, "%s without a print: stop %d peak %d", e->desc.name, stop, peak(a, VB_SR));
        vb_samp_set_buffer(loop, 0);
        stop = note(e, p, 60, 127, VB_SR, a, VB_SR, 32, 1);
        CHECK(stop == 0 && peak(a, VB_SR) == 0, "%s with an empty print: stop %d", e->desc.name, stop);
        vb_samp_set_buffer(tiny, 3);
        stop = note(e, p, 60, 127, VB_SR, a, VB_SR, 32, 1);
        CHECK(stop == 0 && peak(a, VB_SR) == 0, "%s with a 3-sample print: stop %d", e->desc.name, stop);
        vb_samp_set_buffer(tiny, 100);
        fuzz(e, 60, 31 + (uint32_t)k, a);
        /* the print vanishes under a sounding voice */
        vb_samp_set_buffer(loop, PLEN);
        {
            _Alignas(8) uint8_t v[VB_VOICE_BYTES];
            e->voice_init(v, 1); e->note_on(v, p, 60, 127);
            memset(a, 0, sizeof(int32_t) * 64);
            e->render(v, p, a, 32);
            vb_samp_set_buffer(tiny, 100);
            e->render(v, p, a + 32, 32);
            vb_samp_set_buffer(0, 0);
            CHECK(!e->render(v, p, a, 32), "%s: still sounding without a print", e->desc.name);
        }
    }
    vb_samp_set_buffer(0, 0);
    CHECK(vb_chop_slices(st, 8) == 0, "CHOP: slices without a print");
    printf("no print / tiny print: silent, no crash\n");
}

int main(void)
{
    static int32_t a[MAXN], b[MAXN];
    make_loop();
    {
        int32_t x[PLEN];
        for (int i = 0; i < PLEN; i++) x[i] = loop[i];
        wav("host/out/gran_chop_print.wav", x, PLEN);   /* at 44.1 kHz header: plays an octave up */
    }
    vb_samp_set_buffer(loop, PLEN);
    chop_tests(a, b);
    vb_samp_set_buffer(loop, PLEN);
    gran_tests(a, b);
    edge_tests(a);
    printf(failures ? "\n%d FAILED\n" : "\nall gran/chop tests passed\n", failures);
    return failures != 0;
}
