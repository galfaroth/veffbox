/* SPDX-License-Identifier: GPL-3.0-only */
/* Host tests for the Veffbox core. Renders every DRUM voice to host/out/ as WAV files and checks:
 *   - renders are bit-exact repeatable (recipes must rebuild identically),
 *   - no voice overflows 16 bits at any parameter setting (fuzzed),
 *   - the kick's SWEEP really sweeps (more zero crossings at the start than later),
 *   - every voice stops on its own,
 *   - LFO routes add the expected Q8 deltas, and synced LFOs follow the tempo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/vb_drum.h"
#include "../core/vb_mod.h"
#include "../core/vb_dsp.h"

#define N (VB_SR * 2)
#define NLONG (VB_SR * 30)   /* the longest decay (tau 2 s) reaches -90 dB in ~21 s */
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

/* renders one hit of a voice in 32-sample blocks, as the device will */
static int render(const vb_drum_patch_t *p, int voice, int32_t *out, int n, int *stopped_at)
{
    vb_drum_t d;
    vb_drum_init(&d, 1234);
    vb_drum_trigger(&d, p, voice, 127);
    memset(out, 0, sizeof(int32_t) * (size_t)n);
    *stopped_at = -1;
    int peak = 0;
    for (int i = 0; i < n; i += 32) {
        vb_drum_render(&d, p, out + i, n - i < 32 ? n - i : 32);
        if (*stopped_at < 0 && !d.v[voice].on) *stopped_at = i;
    }
    for (int i = 0; i < n; i++) if (abs(out[i]) > peak) peak = abs(out[i]);
    return peak;
}

static int crossings(const int32_t *x, int from, int to) { int c = 0; for (int i = from + 1; i < to; i++) if ((x[i - 1] < 0) != (x[i] < 0)) c++; return c; }

int main(void)
{
    static int32_t a[NLONG], b[N];
    vb_drum_patch_t p;
    vb_drum_patch_default(&p);
    char path[64];
    int stop;

    for (int v = 0; v < VB_DRUM_VOICES; v++) {
        int peak = render(&p, v, a, NLONG, &stop);
        snprintf(path, sizeof path, "host/out/%s.wav", vb_drum_voices[v].name);
        wav(path, a, N);
        uint32_t h1 = hash(a, N);
        int stop2;
        render(&p, v, b, N, &stop2);
        CHECK(h1 == hash(b, N), "%s: render is not repeatable", vb_drum_voices[v].name);
        CHECK(peak > 2000, "%s: too quiet (peak %d)", vb_drum_voices[v].name, peak);
        CHECK(peak <= 32767, "%s: overflow (peak %d)", vb_drum_voices[v].name, peak);
        CHECK(stop >= 0, "%s: never stops", vb_drum_voices[v].name);
        printf("%-6s peak %5d  stops after %4d ms  hash %08x\n", vb_drum_voices[v].name, peak, stop * 1000 / VB_SR, h1);
    }

    /* the kick's sweep: 0..30 ms must cross zero far more often than 150..180 ms */
    p.p[VB_KICK][2] = 127; p.p[VB_KICK][3] = 90; p.p[VB_KICK][4] = 0;   /* long sweep, no click */
    render(&p, VB_KICK, a, N, &stop);
    int early = crossings(a, 0, 1323), late = crossings(a, 6615, 7938);
    CHECK(early > late * 2, "kick sweep: early %d vs late %d crossings", early, late);
    wav("host/out/KICK_sweep.wav", a, N);
    p.p[VB_KICK][2] = 0;
    render(&p, VB_KICK, a, N, &stop);
    int flat_early = crossings(a, 0, 1323), flat_late = crossings(a, 6615, 7938);
    CHECK(abs(flat_early - flat_late) <= 2, "kick without sweep should hold pitch: %d vs %d", flat_early, flat_late);
    printf("kick sweep: %d -> %d crossings per 30 ms (no sweep: %d -> %d)\n", early, late, flat_early, flat_late);

    /* fuzz: random settings never overflow and always stop */
    uint32_t rng = 99;
    for (int k = 0; k < 400; k++) {
        vb_drum_patch_t q;
        for (int v = 0; v < VB_DRUM_VOICES; v++) {
            for (int i = 0; i < VB_DRUM_NP; i++) q.p[v][i] = (uint8_t)(vb_rng(&rng) % 128);
            q.lvl[v] = (uint8_t)(vb_rng(&rng) % 128);
        }
        int v = (int)(vb_rng(&rng) % VB_DRUM_VOICES);
        int peak = render(&q, v, a, NLONG, &stop);
        CHECK(peak <= 32767, "fuzz %d: %s overflow %d", k, vb_drum_voices[v].name, peak);
        CHECK(stop >= 0, "fuzz %d: %s never stops", k, vb_drum_voices[v].name);
    }

    /* modulation: a square LFO at amount +64 moves its destination by +-128 knob steps */
    vb_lfo_t l = { .wave = VB_LFO_SQR, .rate = 127, .sync = 0, .trig = VB_TRIG_FREE };
    for (int i = 0; i < VB_ROUTES; i++) l.r[i].dest = VB_NO_DEST;
    l.r[0] = (vb_route_t){ 3, 64 };
    l.r[1] = (vb_route_t){ 5, -32 };
    vb_lfo_init(&l, 7);
    int32_t delta[8] = { 0 };
    vb_mod_add(l.r, vb_lfo_step(&l, 1200, 1), delta, 8);
    CHECK(delta[3] == 32767 && delta[5] == -16384, "route deltas %d %d", delta[3], delta[5]);
    CHECK(vb_mod_eff_q8(100, delta[3], 0, 127) == 127 * 256, "modulated value clamps to the range");

    /* a synced LFO at 1/1 (one bar = 4 beats = 2 s at 120 BPM) wraps once in 3 s */
    vb_lfo_t s = { .wave = VB_LFO_RAMP, .rate = 48, .sync = 1, .trig = VB_TRIG_FREE };   /* index 3: 1/1 = 4 beats */
    vb_lfo_init(&s, 1);
    int wraps = 0; uint32_t prev = s.ph;
    for (int i = 0; i < VB_SR * 3 / 32; i++) { vb_lfo_step(&s, 1200, 32); if (s.ph < prev) wraps++; prev = s.ph; }
    CHECK(wraps == 1, "at 120 BPM a 1/1 LFO wraps once in 3 s (got %d)", wraps);

    printf(failures ? "\n%d FAILED\n" : "\nall core tests passed\n", failures);
    return failures != 0;
}
