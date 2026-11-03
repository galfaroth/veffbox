/* SPDX-License-Identifier: GPL-3.0-only */
#include "vb_gran.h"
#include "vb_samp.h"
#include "vb_dsp.h"

static const char *const WINN[4] = {"HANN", "TRI", "RECT", "EXP"};
static const char *const ONOFF[2] = {"OFF", "ON"};
static const vb_param_t PARAMS[VB_GR_NPARAMS] = {
    {"POS", 0, 127, 40, VB_P_MOD, 0},    {"SIZE", 0, 127, 30, VB_P_MOD, 0},
    {"DENS", 0, 127, 64, VB_P_MOD, 0},   {"SPRAY", 0, 127, 20, VB_P_MOD, 0},
    {"PITCH", 0, 48, 24, VB_P_MOD | VB_P_BIPOLAR, 0}, {"PRND", 0, 127, 6, VB_P_MOD, 0},
    {"REV", 0, 127, 12, VB_P_MOD, 0},    {"WIN", 0, 3, 0, VB_P_LIST, WINN},
    {"FRZ", 0, 1, 0, VB_P_LIST, ONOFF},  {"WIDTH", 0, 127, 80, VB_P_MOD, 0},
    {"SEED", 0, 127, 17, 0, 0},
};

/* grain windows, Q15, index = window phase >> 24 (generated: Hann; triangle; flat with 8/256 linear edges;
 * a 6/256 linear attack into exp(-4.5 t) with a 12/256 linear tail) */
static const int16_t WINT[4][256] = {
    { /* HANN */
        1, 11, 31, 60, 100, 149, 208, 277, 355, 443, 541, 648, 765, 891, 1027, 1171,
        1325, 1488, 1660, 1841, 2030, 2229, 2435, 2650, 2874, 3105, 3345, 3592, 3847, 4110, 4380, 4657,
        4942, 5233, 5531, 5835, 6146, 6463, 6786, 7115, 7449, 7789, 8134, 8484, 8838, 9197, 9561, 9929,
        10300, 10675, 11054, 11436, 11820, 12208, 12598, 12990, 13385, 13781, 14179, 14578, 14978, 15379, 15780, 16182,
        16585, 16987, 17388, 17789, 18189, 18588, 18986, 19382, 19777, 20169, 20559, 20947, 21331, 21713, 22092, 22467,
        22838, 23206, 23570, 23929, 24283, 24633, 24978, 25318, 25652, 25981, 26304, 26621, 26932, 27236, 27534, 27825,
        28110, 28387, 28657, 28920, 29175, 29422, 29662, 29893, 30117, 30332, 30538, 30737, 30926, 31107, 31279, 31442,
        31596, 31740, 31876, 32002, 32119, 32226, 32324, 32412, 32490, 32559, 32618, 32667, 32707, 32736, 32756, 32766,
        32766, 32756, 32736, 32707, 32667, 32618, 32559, 32490, 32412, 32324, 32226, 32119, 32002, 31876, 31740, 31596,
        31442, 31279, 31107, 30926, 30737, 30538, 30332, 30117, 29893, 29662, 29422, 29175, 28920, 28657, 28387, 28110,
        27825, 27534, 27236, 26932, 26621, 26304, 25981, 25652, 25318, 24978, 24633, 24283, 23929, 23570, 23206, 22838,
        22467, 22092, 21713, 21331, 20947, 20559, 20169, 19777, 19382, 18986, 18588, 18189, 17789, 17388, 16987, 16585,
        16182, 15780, 15379, 14978, 14578, 14179, 13781, 13385, 12990, 12598, 12208, 11820, 11436, 11054, 10675, 10300,
        9929, 9561, 9197, 8838, 8484, 8134, 7789, 7449, 7115, 6786, 6463, 6146, 5835, 5531, 5233, 4942,
        4657, 4380, 4110, 3847, 3592, 3345, 3105, 2874, 2650, 2435, 2229, 2030, 1841, 1660, 1488, 1325,
        1171, 1027, 891, 765, 648, 541, 443, 355, 277, 208, 149, 100, 60, 31, 11, 1,
    },
    { /* TRI */
        128, 384, 640, 896, 1152, 1408, 1664, 1920, 2176, 2432, 2688, 2944, 3200, 3456, 3712, 3968,
        4224, 4480, 4736, 4992, 5248, 5504, 5760, 6016, 6272, 6528, 6784, 7040, 7296, 7552, 7808, 8064,
        8320, 8576, 8832, 9088, 9344, 9600, 9856, 10112, 10368, 10624, 10880, 11136, 11392, 11648, 11904, 12160,
        12416, 12672, 12928, 13184, 13440, 13696, 13952, 14208, 14464, 14720, 14976, 15232, 15488, 15744, 16000, 16256,
        16511, 16767, 17023, 17279, 17535, 17791, 18047, 18303, 18559, 18815, 19071, 19327, 19583, 19839, 20095, 20351,
        20607, 20863, 21119, 21375, 21631, 21887, 22143, 22399, 22655, 22911, 23167, 23423, 23679, 23935, 24191, 24447,
        24703, 24959, 25215, 25471, 25727, 25983, 26239, 26495, 26751, 27007, 27263, 27519, 27775, 28031, 28287, 28543,
        28799, 29055, 29311, 29567, 29823, 30079, 30335, 30591, 30847, 31103, 31359, 31615, 31871, 32127, 32383, 32639,
        32639, 32383, 32127, 31871, 31615, 31359, 31103, 30847, 30591, 30335, 30079, 29823, 29567, 29311, 29055, 28799,
        28543, 28287, 28031, 27775, 27519, 27263, 27007, 26751, 26495, 26239, 25983, 25727, 25471, 25215, 24959, 24703,
        24447, 24191, 23935, 23679, 23423, 23167, 22911, 22655, 22399, 22143, 21887, 21631, 21375, 21119, 20863, 20607,
        20351, 20095, 19839, 19583, 19327, 19071, 18815, 18559, 18303, 18047, 17791, 17535, 17279, 17023, 16767, 16511,
        16256, 16000, 15744, 15488, 15232, 14976, 14720, 14464, 14208, 13952, 13696, 13440, 13184, 12928, 12672, 12416,
        12160, 11904, 11648, 11392, 11136, 10880, 10624, 10368, 10112, 9856, 9600, 9344, 9088, 8832, 8576, 8320,
        8064, 7808, 7552, 7296, 7040, 6784, 6528, 6272, 6016, 5760, 5504, 5248, 4992, 4736, 4480, 4224,
        3968, 3712, 3456, 3200, 2944, 2688, 2432, 2176, 1920, 1664, 1408, 1152, 896, 640, 384, 128,
    },
    { /* RECT */
        2048, 6144, 10240, 14336, 18431, 22527, 26623, 30719, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
        32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 30719, 26623, 22527, 18431, 14336, 10240, 6144, 2048,
    },
    { /* EXP */
        2731, 8192, 13653, 19114, 24575, 30036, 32767, 32182, 31608, 31045, 30491, 29947, 29413, 28888, 28373, 27866,
        27369, 26881, 26402, 25931, 25468, 25014, 24567, 24129, 23699, 23276, 22861, 22453, 22052, 21659, 21273, 20893,
        20520, 20154, 19795, 19442, 19095, 18754, 18420, 18091, 17768, 17451, 17140, 16834, 16534, 16239, 15949, 15665,
        15385, 15111, 14841, 14577, 14317, 14061, 13810, 13564, 13322, 13084, 12851, 12622, 12397, 12175, 11958, 11745,
        11535, 11330, 11128, 10929, 10734, 10543, 10355, 10170, 9988, 9810, 9635, 9463, 9294, 9129, 8966, 8806,
        8649, 8495, 8343, 8194, 8048, 7904, 7763, 7625, 7489, 7355, 7224, 7095, 6969, 6844, 6722, 6602,
        6485, 6369, 6255, 6144, 6034, 5926, 5821, 5717, 5615, 5515, 5416, 5320, 5225, 5132, 5040, 4950,
        4862, 4775, 4690, 4606, 4524, 4443, 4364, 4286, 4210, 4135, 4061, 3989, 3917, 3847, 3779, 3711,
        3645, 3580, 3516, 3454, 3392, 3331, 3272, 3214, 3156, 3100, 3045, 2990, 2937, 2885, 2833, 2783,
        2733, 2684, 2636, 2589, 2543, 2498, 2453, 2410, 2367, 2324, 2283, 2242, 2202, 2163, 2124, 2086,
        2049, 2013, 1977, 1941, 1907, 1873, 1839, 1807, 1774, 1743, 1712, 1681, 1651, 1622, 1593, 1564,
        1536, 1509, 1482, 1456, 1430, 1404, 1379, 1354, 1330, 1307, 1283, 1260, 1238, 1216, 1194, 1173,
        1152, 1131, 1111, 1091, 1072, 1053, 1034, 1016, 997, 980, 962, 945, 928, 912, 895, 879,
        864, 848, 833, 818, 804, 789, 775, 761, 748, 734, 721, 709, 696, 683, 671, 659,
        648, 636, 625, 613, 603, 592, 581, 571, 561, 551, 541, 531, 522, 512, 503, 494,
        485, 477, 468, 460, 433, 388, 345, 303, 263, 224, 186, 149, 114, 80, 47, 15,
    },
};

/* 2^(k/12), Q16, k = 0..12 */
static const uint32_t SEMI_Q16[13] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715, 131072};
/* 2^(-k/18), Q16: DENS within an octave */
static const uint32_t DENS_Q16[18] = {65536, 63060, 60678, 58386, 56180, 54058, 52016, 50051, 48160,
                                      46341, 44590, 42906, 41285, 39726, 38225, 36781, 35391, 34054};

/* grain level for an overlap of 0..8 grains (SIZE / grain interval; at most 8 sound): ~1/sqrt of the overlap of
 * windows averaging 1/2, since grains from different places add up incoherently. Coherent peaks are clipped by the
 * voice's final saturation. */
static const int16_t OV_GAIN[VB_GR_GRAINS + 1] = {32767, 32767, 29000, 24500, 21500, 19500, 18000, 16800, 15800};

#define GR_MINLEN 64u          /* shorter prints are silence */
#define GR_CHUNK 32            /* grains are mixed grain by grain over chunks of at most this many samples */
#define GR_ATT (32767 / 132 + 1)    /* 3 ms */
#define GR_REL (32767 / 2646 + 1)   /* 60 ms */

typedef struct {
    uint32_t pos;          /* Q16 source sample index */
    int32_t inc;           /* Q16 source samples per output sample, negative when reversed */
    uint32_t wph, winc;    /* window phase: 0 .. 2^32 over the grain */
    uint16_t left;         /* output samples left; 0 = free */
    int16_t gain;          /* Q15 */
} grain_t;

typedef struct {
    grain_t g[VB_GR_GRAINS];
    uint32_t vseed, seed, rng;   /* voice seed (voice_init), seed mixed with SEED (note_on), running generator */
    uint32_t next;               /* output samples to the next grain */
    int32_t env, vel;            /* Q15 */
    uint16_t blen;               /* the print length the grains were placed in */
    uint8_t note, gate, on, fidx;
} gv_t;

typedef char gv_fits[sizeof(gv_t) <= VB_VOICE_BYTES ? 1 : -1];

static uint32_t mix32(uint32_t h)
{
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h ? h : 0x9E3779B9u;
}

/* Q16 source samples per output sample for st16 sixteenths of a semitone (clamped to +-48 semitones);
 * the print is at half the output rate, so 0 is 0.5 */
static uint32_t inc_of(int st16)
{
    uint32_t r, a, b;
    int oct, k, f;
    st16 = vb_clamp(st16, -768, 768) + 768;            /* 0 .. 1536 = 8 octaves */
    oct = st16 / 192;
    k = (st16 % 192) >> 4;
    f = st16 & 15;
    a = SEMI_Q16[k];
    b = SEMI_Q16[k + 1];
    r = a + (((b - a) * (uint32_t)f) >> 4);
    return oct >= 5 ? r << (oct - 5) : r >> (5 - oct);
}

/* output samples between grains for DENS */
static uint32_t interval_of(int dens)
{
    dens = vb_clamp(dens, 0, 127);
    return ((22050u >> (dens / 18)) * DENS_Q16[dens % 18]) >> 16;   /* 22050 (2 Hz) .. 165 (267 Hz) */
}

static uint32_t grain_len(int size)
{
    size = vb_clamp(size, 0, 127);
    return 220u + (uint32_t)(size * size) * 10805u / 16129u;        /* 5 .. 250 ms */
}

static void kill(gv_t *s)
{
    int i;
    for (i = 0; i < VB_GR_GRAINS; i++)
        s->g[i].left = 0;
}

static void init(void *v, uint32_t seed)
{
    gv_t *s = v;
    kill(s);
    s->vseed = seed;
    s->seed = s->rng = mix32(seed);
    s->next = 0;
    s->env = s->vel = 0;
    s->blen = 0;
    s->note = 60;
    s->gate = s->on = s->fidx = 0;
}

/* start one grain (if one is free and fits in the print) */
static void spawn(gv_t *s, const uint8_t *p, uint32_t blen, uint32_t interval)
{
    grain_t *g = 0;
    uint32_t r1, r2, r3, len, inc, span, lim, center, amt, ov, lo;
    int32_t off, st16, spread, gain, wv;
    int i;
    for (i = 0; i < VB_GR_GRAINS; i++)
        if (!s->g[i].left) { g = &s->g[i]; break; }
    if (!g)
        return;
    if (p[VB_GR_FRZ]) {                       /* frozen: a fixed cycle of 8 draws */
        uint32_t t = mix32(s->seed ^ ((uint32_t)(s->fidx + 1u) * 0x9E3779B9u));
        s->fidx = (uint8_t)((s->fidx + 1u) & 7u);
        r1 = vb_rng(&t); r2 = vb_rng(&t); r3 = vb_rng(&t);
    } else {
        r1 = vb_rng(&s->rng); r2 = vb_rng(&s->rng); r3 = vb_rng(&s->rng);
    }
    /* pitch */
    spread = vb_clamp(p[VB_GR_PRND], 0, 127) * 192 / 127;            /* up to 12 semitones */
    st16 = ((int)s->note - 60 + (int)p[VB_GR_PITCH] - 24) * 16 + ((((int32_t)(r2 >> 16) - 32768) * spread) >> 15);
    inc = inc_of(st16);
    /* length, shortened when the grain's source span doesn't fit the print */
    lim = blen - 2u;
    len = grain_len(p[VB_GR_SIZE]);
    span = ((len * (inc >> 4)) >> 12) + 5u;                          /* source samples read, with rounding margin */
    if (span > lim) {
        len = ((lim - 5u) << 12) / (inc >> 4);
        if (len < 16u)
            return;
        span = ((len * (inc >> 4)) >> 12) + 5u;
    }
    /* position: centred on POS + spray, kept inside the print */
    center = blen * vb_clamp(p[VB_GR_POS], 0, 127) / 127u;
    amt = (blen * (uint32_t)vb_clamp(p[VB_GR_SPRAY], 0, 127)) >> 8;  /* up to blen / 2 */
    off = (((int32_t)(r1 >> 17) - 16384) * (int32_t)amt) >> 14;
    off += (int32_t)center - (int32_t)(span >> 1);
    lo = (uint32_t)vb_clamp(off, 0, (int32_t)(lim - span));
    /* level: scaled with the overlap, then WIDTH's random variation */
    ov = len / interval;
    gain = OV_GAIN[ov < VB_GR_GRAINS ? ov : VB_GR_GRAINS];
    wv = (vb_clamp(p[VB_GR_WIDTH], 0, 127) * (int32_t)(r3 >> 24)) >> 7;   /* 0 .. 254 */
    gain -= (gain * wv * 3) >> 10;
    g->gain = (int16_t)gain;
    if ((int)((r3 >> 8) % 127u) < (int)p[VB_GR_REV]) {
        g->pos = (lo + span - 1u) << 16;
        g->inc = -(int32_t)inc;
    } else {
        g->pos = lo << 16;
        g->inc = (int32_t)inc;
    }
    g->wph = 0;
    g->winc = 0xFFFFFFFFu / len;
    g->left = (uint16_t)len;
}

static void note_on(void *v, const uint8_t *p, int note, int vel)
{
    gv_t *s = v;
    uint32_t blen;
    const int16_t *buf = vb_samp_buffer(&blen);
    if (!buf || blen < GR_MINLEN) {
        kill(s);
        s->on = s->gate = 0;
        s->env = 0;
        return;
    }
    if (!s->on) {                             /* a retrigger keeps its grains and fades on from where it is */
        kill(s);
        s->env = 0;
    }
    s->seed = s->rng = mix32(s->vseed * 0x9E3779B1u ^ ((uint32_t)p[VB_GR_SEED] + 1u) * 0x85EBCA77u);
    s->fidx = 0;
    s->note = (uint8_t)note;
    s->vel = vel * 258;
    s->next = 0;                              /* the first grain starts now */
    if (!s->on || blen < s->blen)
        s->blen = (uint16_t)blen;
    s->gate = 1;
    s->on = 1;
}

static void note_off(void *v, const uint8_t *p)
{
    (void)p;
    ((gv_t *)v)->gate = 0;
}

static int render(void *v, const uint8_t *p, int32_t *out, int n)
{
    gv_t *s = v;
    uint32_t blen, interval;
    const int16_t *buf = vb_samp_buffer(&blen);
    const int16_t *win;
    int32_t acc[GR_CHUNK];
    if (!s->on)
        return 0;
    if (!buf || blen < GR_MINLEN) {
        kill(s);
        s->on = 0;
        return 0;
    }
    if (blen < s->blen) {                     /* the print got shorter under the grains */
        kill(s);
        s->blen = (uint16_t)blen;
    }
    win = WINT[p[VB_GR_WIN] & 3u];
    interval = interval_of(p[VB_GR_DENS]);
    if (s->next > interval)
        s->next = interval;
    while (n > 0) {
        int m, i, k;
        int32_t ev;
        if (!s->next) {
            spawn(s, p, s->blen, interval);
            s->next = interval;
        }
        m = n < GR_CHUNK ? n : GR_CHUNK;
        if ((uint32_t)m > s->next)
            m = (int)s->next;
        for (i = 0; i < m; i++)
            acc[i] = 0;
        for (k = 0; k < VB_GR_GRAINS; k++) {   /* grain by grain: the state stays in registers */
            grain_t *g = &s->g[k];
            uint32_t pos, wph, winc;
            int32_t inc, gg;
            int c;
            if (!g->left)
                continue;
            pos = g->pos; wph = g->wph; winc = g->winc; inc = g->inc; gg = g->gain;
            c = m < (int)g->left ? m : (int)g->left;
            for (i = 0; i < c; i++) {
                uint32_t j = pos >> 16;
                int32_t x0 = buf[j], x1 = buf[j + 1u], y;
                y = x0 + (((x1 - x0) * (int32_t)((pos >> 2) & 0x3FFFu)) >> 14);
                y = (y * win[wph >> 24]) >> 15;
                acc[i] += (y * gg) >> 15;
                pos += (uint32_t)inc;
                wph += winc;
            }
            g->pos = pos; g->wph = wph;
            g->left = (uint16_t)(g->left - c);
        }
        for (i = 0; i < m; i++) {
            if (s->gate)
                s->env = s->env + GR_ATT > 32767 ? 32767 : s->env + GR_ATT;
            else if ((s->env -= GR_REL) <= 0) {
                s->env = 0;
                s->on = 0;
                kill(s);
                return 0;
            }
            ev = (s->env * s->vel) >> 15;
            out[i] += (vb_sat16(acc[i]) * ev) >> 15;
        }
        s->next -= (uint32_t)m;
        out += m;
        n -= m;
    }
    return 1;
}

const vb_engine_t vb_gran = {{"GRAN", 1, VB_GR_NPARAMS, PARAMS}, sizeof(gv_t), init, note_on, note_off, render};
