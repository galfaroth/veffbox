/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests for the OP4 and ANLG synth engines. Renders phrases to host/out/op4_*.wav and host/out/anlg_*.wav, and checks:
 *   - descriptors (parameter counts, labels) and voice state size,
 *   - renders are bit-exact repeatable, also across block sizes,
 *   - no voice overflows 16 bits at fuzzed patches,
 *   - every voice goes silent within a bounded time after note_off, and stays silent,
 *   - pitch (zero crossings of a pure sine at known notes, ratios and fixed frequencies),
 *   - OP4's 11 algorithms route exactly like the mockup's ALG table,
 *   - a retrigger of a sounding voice doesn't click. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/vb_op4.h"
#include "../core/vb_anlg.h"
#include "../core/vb_dsp.h"

#define MAXN (VB_SR * 20)
#define REL_BOUND (VB_SR * 151 / 10)   /* the longest release (10 s to -60 dB) reaches -90 dB in 15 s */
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
static int crossings(const int32_t *x, int from, int to) { int c = 0; for (int i = from + 1; i < to; i++) if ((x[i - 1] < 0) != (x[i] < 0)) c++; return c; }

static void defaults(const vb_engine_t *e, uint8_t *p) { for (int i = 0; i < e->desc.nparams; i++) p[i] = e->desc.params[i].def; }

/* one note on one voice: note_off after `gate` samples; renders up to n samples in blocks of `blk`.
 * Returns the sample at which render() first returned 0 (-1 if it never did); checks that it stays silent after. */
static int note(const vb_engine_t *e, const uint8_t *p, int nt, int vel, int gate, int32_t *out, int n, int blk, uint32_t seed)
{
    _Alignas(8) uint8_t v[VB_VOICE_BYTES];
    e->voice_init(v, seed);
    e->note_on(v, p, nt, vel);
    memset(out, 0, sizeof(int32_t) * (size_t)n);
    int stop = -1;
    for (int i = 0; i < n; i += blk) {
        if (i >= gate && i - blk < gate) e->note_off(v, p);   /* on the first block boundary at or after gate */
        int m = n - i < blk ? n - i : blk;
        int on = e->render(v, p, out + i, m);
        if (!on) {
            if (stop < 0) stop = i;
            int32_t z[32] = { 0 };
            int again = e->render(v, p, z, 32);
            CHECK(!again && peak(z, 32) == 0, "%s: a stopped voice rendered again", e->desc.name);
            break;
        }
    }
    return stop;
}

/* a little phrase: notes in round robin over 4 voices, each held `gate` samples */
static void phrase(const vb_engine_t *e, const uint8_t *p, const int *notes, int nn, int step, int gate, int32_t *out, int n)
{
    _Alignas(8) uint8_t v[4][VB_VOICE_BYTES];
    int off_at[4] = { -1, -1, -1, -1 };
    for (int k = 0; k < 4; k++) e->voice_init(v[k], 100 + (uint32_t)k);
    memset(out, 0, sizeof(int32_t) * (size_t)n);
    for (int i = 0; i < n; i += 32) {
        if (i % step == 0 && i / step < nn) { int k = (i / step) % 4; e->note_on(v[k], p, notes[i / step], 100); off_at[k] = i + gate; }
        for (int k = 0; k < 4; k++) {
            if (off_at[k] >= 0 && i >= off_at[k]) { e->note_off(v[k], p); off_at[k] = -1; }
            e->render(v[k], p, out + i, n - i < 32 ? n - i : 32);
        }
    }
}

static void desc_checks(const vb_engine_t *e, int np)
{
    CHECK(e->desc.nparams == np, "%s: %d params, want %d", e->desc.name, e->desc.nparams, np);
    CHECK(e->voice_size <= VB_VOICE_BYTES, "%s: voice %d bytes > %d", e->desc.name, e->voice_size, VB_VOICE_BYTES);
    for (int i = 0; i < e->desc.nparams; i++) {
        const vb_param_t *q = &e->desc.params[i];
        CHECK(strlen(q->label) <= 5, "%s: label %s too long", e->desc.name, q->label);
        CHECK(q->def >= q->min && q->def <= q->max, "%s: %s default out of range", e->desc.name, q->label);
        CHECK(!(q->flags & VB_P_LIST) || q->names, "%s: list %s without names", e->desc.name, q->label);
    }
    printf("%-4s: %d params, voice %d bytes\n", e->desc.name, e->desc.nparams, e->voice_size);
}

static double hz(const int32_t *x, int from, int to) { return crossings(x, from, to) * 0.5 * VB_SR / (to - from); }
static void pitch_check(const char *what, const int32_t *x, double want)
{
    double f = hz(x, VB_SR / 10, VB_SR / 10 + VB_SR);
    CHECK(f > want * 0.99 && f < want * 1.01, "%s: %.2f Hz, want %.2f", what, f, want);
    printf("pitch %-22s %8.2f Hz (want %.2f)\n", what, f, want);
}

/* largest sample-to-sample step in [from, to) */
static int maxstep(const int32_t *x, int from, int to) { int m = 0; for (int i = from + 1; i < to; i++) if (abs(x[i] - x[i - 1]) > m) m = abs(x[i] - x[i - 1]); return m; }

static void fuzz(const vb_engine_t *e, int count, uint32_t seed, int32_t *a)
{
    uint32_t rng = seed;
    int worst = 0, longest = 0;
    uint8_t p[64];
    for (int k = 0; k < count; k++) {
        for (int i = 0; i < e->desc.nparams; i++) {
            const vb_param_t *q = &e->desc.params[i];
            p[i] = (uint8_t)(q->min + vb_rng(&rng) % (uint32_t)(q->max - q->min + 1));
            if (k % 4 == 0 && !(q->flags & VB_P_LIST) && (vb_rng(&rng) & 1)) p[i] = (vb_rng(&rng) & 1) ? q->max : q->min;   /* corners */
        }
        int nt = (int)(vb_rng(&rng) % 128), vel = 1 + (int)(vb_rng(&rng) % 127);
        if (k % 3 == 0) vel = 127;
        int gate = (int)(vb_rng(&rng) % (VB_SR * 2));
        int stop = note(e, p, nt, vel, gate, a, MAXN, 32, rng);
        int pk = peak(a, stop < 0 ? MAXN : stop + 32);
        if (pk > worst) worst = pk;
        CHECK(pk <= 32767, "%s fuzz %d: overflow %d", e->desc.name, k, pk);
        CHECK(stop >= 0 && stop - gate <= REL_BOUND, "%s fuzz %d: still sounding %d samples after note_off", e->desc.name, k, stop - gate);
        if (stop - gate > longest) longest = stop - gate;
    }
    printf("%-4s fuzz: %d patches, peak %d, longest tail %d ms after note_off\n", e->desc.name, count, worst, longest * 1000 / VB_SR);
}

/* ---------------- OP4 ---------------- */

/* the mockup's ALG table, transcribed independently from ui/index.html: edges {from,to} and carriers */
static const struct { const char *e[3]; const char *c; } MOCK_ALG[11] = {
    { { "DC", "CB", "BA" }, "A" }, { { "CB", "DB", "BA" }, "A" }, { { "DC", "CA", "BA" }, "A" }, { { "DB", "BA", "CA" }, "A" },
    { { "DC", "CB" }, "BA" }, { { "DC", "BA" }, "CA" }, { { "DA", "DB", "DC" }, "ABC" }, { { "DC" }, "CAB" },
    { { "BA", "CA", "DA" }, "A" }, { { "DC", "CA", "CB" }, "AB" }, { { 0 }, "ABCD" },
};
static int edge(int alg, int f, int t)
{
    for (int i = 0; i < 3 && MOCK_ALG[alg].e[i]; i++) if (MOCK_ALG[alg].e[i][0] - 'A' == f && MOCK_ALG[alg].e[i][1] - 'A' == t) return 1;
    return 0;
}
static int carrier(int alg, int o) { return strchr(MOCK_ALG[alg].c, 'A' + o) != 0; }

static void op4_set(uint8_t *p, int o, int prm, int val) { p[o * VB_OP4_PER_OP + prm] = (uint8_t)val; }

static void op4_tests(int32_t *a, int32_t *b)
{
    const vb_engine_t *e = &vb_op4;
    uint8_t p[VB_OP4_FB + 1], q[VB_OP4_FB + 1];
    char path[80];
    desc_checks(e, 38);
    CHECK(!strcmp(e->desc.params[0].label, "WAVE") && !strcmp(e->desc.params[8].label, "FIX") && !strcmp(e->desc.params[36].label, "ALG")
          && !strcmp(e->desc.params[37].label, "FB"), "OP4 descriptor order");

    /* phrases for a few algorithms */
    static const int notes[] = { 48, 55, 60, 63, 67, 72, 70, 67 };
    static const int algs[] = { 0, 1, 4, 6, 8, 9, 10 };
    for (unsigned k = 0; k < sizeof algs / sizeof algs[0]; k++) {
        defaults(e, p);
        p[VB_OP4_ALG] = (uint8_t)algs[k];
        phrase(e, p, notes, 8, VB_SR / 3, VB_SR / 4, a, VB_SR * 4);
        snprintf(path, sizeof path, "host/out/op4_alg%d.wav", algs[k] + 1);
        wav(path, a, VB_SR * 4);
        int pk = peak(a, VB_SR * 4);
        CHECK(pk > 1000, "OP4 alg %d too quiet (%d)", algs[k] + 1, pk);
        printf("OP4 alg %-2d peak %5d  hash %08x\n", algs[k] + 1, pk, hash(a, VB_SR * 4));
    }
    /* feedback, other waves, a fixed-frequency noise modulator */
    defaults(e, p); p[VB_OP4_FB] = 127; p[VB_OP4_ALG] = 10;
    for (int o = 1; o < 4; o++) op4_set(p, o, VB_OP4_LVL, 0);
    op4_set(p, 3, VB_OP4_LVL, 127); op4_set(p, 3, VB_OP4_SUS, 127); op4_set(p, 3, VB_OP4_DEC, 127);
    phrase(e, p, notes, 8, VB_SR / 3, VB_SR / 4, a, VB_SR * 4);
    wav("host/out/op4_feedback.wav", a, VB_SR * 4);
    defaults(e, p); p[VB_OP4_ALG] = 2;
    op4_set(p, 0, VB_OP4_WAVE, 3); op4_set(p, 1, VB_OP4_WAVE, 6); op4_set(p, 1, VB_OP4_FIX, 1); op4_set(p, 1, VB_OP4_LVL, 50);
    op4_set(p, 2, VB_OP4_WAVE, 4); op4_set(p, 3, VB_OP4_WAVE, 5); op4_set(p, 3, VB_OP4_LVL, 90);
    phrase(e, p, notes, 8, VB_SR / 3, VB_SR / 4, a, VB_SR * 4);
    wav("host/out/op4_waves.wav", a, VB_SR * 4);

    /* repeatability, and independence of the block size */
    defaults(e, p); p[VB_OP4_FB] = 90; op4_set(p, 2, VB_OP4_WAVE, 6);
    note(e, p, 60, 100, VB_SR, a, VB_SR * 3, 32, 5);
    note(e, p, 60, 100, VB_SR, b, VB_SR * 3, 32, 5);
    CHECK(hash(a, VB_SR * 3) == hash(b, VB_SR * 3), "OP4 render is not repeatable");
    note(e, p, 60, 100, VB_SR, b, VB_SR * 3, 7, 5);   /* note_off then lands 3 samples later: compare up to the gate */
    CHECK(hash(a, VB_SR) == hash(b, VB_SR), "OP4 render depends on the block size");

    /* pitch: a lone sine carrier */
    defaults(e, p); p[VB_OP4_FB] = 0;
    for (int o = 1; o < 4; o++) op4_set(p, o, VB_OP4_LVL, 0);
    op4_set(p, 0, VB_OP4_ATK, 0); op4_set(p, 0, VB_OP4_SUS, 127);
    note(e, p, 69, 127, MAXN, a, VB_SR * 2, 32, 1); pitch_check("OP4 A4 x1", a, 440);
    note(e, p, 57, 127, MAXN, a, VB_SR * 2, 32, 1); pitch_check("OP4 A3 x1", a, 220);
    op4_set(p, 0, VB_OP4_CRS, 0); note(e, p, 69, 127, MAXN, a, VB_SR * 2, 32, 1); pitch_check("OP4 A4 x0.5", a, 220);
    op4_set(p, 0, VB_OP4_CRS, 3); op4_set(p, 0, VB_OP4_FINE, 50); note(e, p, 45, 127, MAXN, a, VB_SR * 2, 32, 1); pitch_check("OP4 A2 x3.5", a, 385);
    op4_set(p, 0, VB_OP4_FIX, 1); op4_set(p, 0, VB_OP4_CRS, 4); op4_set(p, 0, VB_OP4_FINE, 5);
    note(e, p, 30, 127, MAXN, a, VB_SR * 2, 32, 1); pitch_check("OP4 FIX 450 Hz", a, 450);

    /* routing: with only the ops in S at full level, removing f changes the sound iff f is a carrier or modulates an op of S */
    int bad = 0;
    for (int alg = 0; alg < 11; alg++) {
        for (int f = 0; f < 4; f++) for (int t = 0; t < 4; t++) {
            if (t == f) continue;
            unsigned S = 1u << f | 1u << t;
            for (int x = t; !carrier(alg, x);) {   /* follow t to a carrier */
                int nx = -1;
                for (int y = 0; y < 4; y++) if (edge(alg, x, y)) { nx = y; break; }
                if (nx < 0) break;
                S |= 1u << nx; x = nx;
            }
            defaults(e, p); p[VB_OP4_ALG] = (uint8_t)alg; p[VB_OP4_FB] = 0;
            for (int o = 0; o < 4; o++) {
                op4_set(p, o, VB_OP4_CRS, o + 1); op4_set(p, o, VB_OP4_ATK, 0); op4_set(p, o, VB_OP4_SUS, 127);
                op4_set(p, o, VB_OP4_LVL, (S >> o & 1) ? 127 : 0);
            }
            memcpy(q, p, sizeof q); op4_set(q, f, VB_OP4_LVL, 0);
            note(e, p, 60, 127, MAXN, a, VB_SR / 5, 32, 1);
            note(e, q, 60, 127, MAXN, b, VB_SR / 5, 32, 1);
            int want = carrier(alg, f);
            for (int s = 0; s < 4; s++) if ((S >> s & 1) && s != f && edge(alg, f, s)) want = 1;
            int got = hash(a, VB_SR / 5) != hash(b, VB_SR / 5);
            if (got != want) { bad++; printf("FAIL alg %d: op %c with S=%x: changes %d, expected %d\n", alg + 1, 'A' + f, S, got, want); }
        }
    }
    CHECK(bad == 0, "OP4 algorithms differ from the mockup in %d cases", bad);
    printf("OP4 routing: 11 algorithms match the mockup's ALG table\n");

    /* retrigger: no step much larger than the steady signal's */
    defaults(e, p);
    {
        _Alignas(8) uint8_t v[VB_VOICE_BYTES];
        e->voice_init(v, 3); e->note_on(v, p, 60, 120);
        memset(a, 0, sizeof(int32_t) * VB_SR);
        for (int i = 0; i < VB_SR; i += 32) { if (i == VB_SR / 2) e->note_on(v, p, 60, 120); e->render(v, p, a + i, 32); }
        int before = maxstep(a, VB_SR / 2 - 2000, VB_SR / 2), at = maxstep(a, VB_SR / 2, VB_SR / 2 + 200);
        CHECK(at <= before * 2 + 200, "OP4 retrigger clicks: step %d vs %d", at, before);
    }

    fuzz(e, 400, 77, a);
}

/* ---------------- ANLG ---------------- */

static void anlg_tests(int32_t *a, int32_t *b)
{
    const vb_engine_t *e = &vb_anlg;
    uint8_t p[VB_AN_NPARAMS];
    desc_checks(e, 16);
    static const char *const want[16] = { "OSC1", "OSC2", "DET", "MIX", "SUB", "NOIS", "SYNC", "PW", "CUT", "RES", "ENV", "DRV", "ATK", "DEC", "SUS", "REL" };
    for (int i = 0; i < 16; i++) CHECK(!strcmp(e->desc.params[i].label, want[i]), "ANLG param %d is %s, want %s", i, e->desc.params[i].label, want[i]);

    static const int bass[] = { 36, 36, 48, 36, 39, 36, 43, 34 }, chordn[] = { 48, 55, 60, 63, 51, 58, 63, 67 };
    struct { const char *name; int v[16]; const int *notes; } R[] = {
        { "default", { 0, 1, 14, 64, 40, 0, 0, 64, 70, 40, 84, 20, 2, 50, 90, 40 }, chordn },
        { "acid", { 0, 0, 0, 0, 0, 0, 0, 64, 30, 120, 120, 70, 0, 40, 0, 20 }, bass },
        { "pwm_pad", { 3, 3, 9, 64, 30, 0, 0, 40, 80, 50, 70, 10, 70, 90, 110, 80 }, chordn },
        { "sync", { 0, 0, 60, 127, 0, 0, 1, 64, 110, 30, 64, 30, 0, 60, 80, 30 }, bass },
        { "sub_noise", { 2, 4, 5, 40, 127, 30, 0, 64, 50, 70, 100, 90, 0, 50, 60, 30 }, bass },
        { "selfosc", { 4, 4, 0, 0, 0, 0, 0, 64, 40, 127, 110, 127, 0, 80, 30, 40 }, chordn },
    };
    char path[80];
    for (unsigned k = 0; k < sizeof R / sizeof R[0]; k++) {
        for (int i = 0; i < 16; i++) p[i] = (uint8_t)R[k].v[i];
        phrase(e, p, R[k].notes, 8, VB_SR / 4, VB_SR / 6, a, VB_SR * 3);
        snprintf(path, sizeof path, "host/out/anlg_%s.wav", R[k].name);
        wav(path, a, VB_SR * 3);
        int pk = peak(a, VB_SR * 3);
        CHECK(pk > 1000, "ANLG %s too quiet (%d)", R[k].name, pk);
        printf("ANLG %-9s peak %5d  hash %08x\n", R[k].name, pk, hash(a, VB_SR * 3));
    }

    /* repeatability (noise in the mix) and block-size independence */
    defaults(e, p); p[VB_AN_NOIS] = 60; p[VB_AN_OSC2] = VB_AN_PWM;
    note(e, p, 48, 100, VB_SR, a, VB_SR * 3, 32, 9);
    note(e, p, 48, 100, VB_SR, b, VB_SR * 3, 32, 9);
    CHECK(hash(a, VB_SR * 3) == hash(b, VB_SR * 3), "ANLG render is not repeatable");
    note(e, p, 48, 100, VB_SR, b, VB_SR * 3, 7, 9);
    CHECK(hash(a, VB_SR) == hash(b, VB_SR), "ANLG render depends on the block size");

    /* pitch: osc1 alone, filter open */
    defaults(e, p);
    p[VB_AN_OSC1] = VB_AN_SIN; p[VB_AN_MIX] = 0; p[VB_AN_SUB] = 0; p[VB_AN_CUT] = 127; p[VB_AN_RES] = 0; p[VB_AN_ENV] = 64;
    p[VB_AN_DRV] = 0; p[VB_AN_SUS] = 127;
    note(e, p, 69, 127, MAXN, a, VB_SR * 2, 32, 1); pitch_check("ANLG A4 sin", a, 440);
    p[VB_AN_OSC1] = VB_AN_SAW; p[VB_AN_CUT] = 60;
    note(e, p, 45, 127, MAXN, a, VB_SR * 2, 32, 1); pitch_check("ANLG A2 saw", a, 110);
    p[VB_AN_OSC1] = VB_AN_SIN; p[VB_AN_MIX] = 127; p[VB_AN_OSC2] = VB_AN_SIN; p[VB_AN_DET] = 100; p[VB_AN_CUT] = 127;
    note(e, p, 69, 127, MAXN, a, VB_SR * 2, 32, 1); pitch_check("ANLG osc2 +100c", a, 466.16);

    /* the filter: a saw through CUT 20 has far less high-frequency energy (sum of |steps|) than through CUT 127 */
    p[VB_AN_OSC1] = VB_AN_SAW; p[VB_AN_MIX] = 0; p[VB_AN_DET] = 0; p[VB_AN_CUT] = 127;
    note(e, p, 57, 127, MAXN, a, VB_SR, 32, 1);
    p[VB_AN_CUT] = 20;
    note(e, p, 57, 127, MAXN, b, VB_SR, 32, 1);
    {
        long hi = 0, lo = 0;
        for (int i = VB_SR / 10 + 1; i < VB_SR; i++) { hi += labs((long)a[i] - a[i - 1]); lo += labs((long)b[i] - b[i - 1]); }
        CHECK(lo * 4 < hi, "ANLG filter doesn't darken: %ld vs %ld", lo, hi);
        printf("ANLG filter: HF energy CUT 20 / CUT 127 = %.3f\n", (double)lo / (double)hi);
    }

    /* resonance at maximum with the filter swept everywhere stays bounded and keeps ringing quietly bounded */
    defaults(e, p); p[VB_AN_RES] = 127; p[VB_AN_DRV] = 127; p[VB_AN_ENV] = 127; p[VB_AN_SUS] = 127;
    for (int c = 0; c < 128; c += 9) {
        p[VB_AN_CUT] = (uint8_t)c;
        note(e, p, 40 + c / 3, 127, VB_SR, a, VB_SR * 2, 32, 2);
        CHECK(peak(a, VB_SR * 2) <= 32767, "ANLG res 127 cut %d: overflow", c);
    }

    fuzz(e, 400, 1234, a);
}

int main(void)
{
    static int32_t a[MAXN], b[MAXN];
    op4_tests(a, b);
    anlg_tests(a, b);
    printf(failures ? "\n%d FAILED\n" : "\nall synth tests passed\n", failures);
    return failures != 0;
}
