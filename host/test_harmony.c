/* SPDX-License-Identifier: GPL-3.0-only */
/* Harmony: chords on scale degrees with the modifiers, and their roman numerals. */
#include <stdio.h>
#include <string.h>
#include "../core/vb_sound.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *roman(int key, int scale, int deg, int semis, int mods)
{
    static char b[24];
    vb_chord_t c;
    int root, ty = vb_degree_chord(key, scale, deg, semis, mods, &root);
    memset(&c, 0, sizeof c);
    vb_chord_fill(&c, root, ty, 48);
    vb_chord_roman(&c, key, scale, b, sizeof b);
    return b;
}

int main(void)
{
    static const char *const MAJ[7] = {"I", "IIm", "IIIm", "IV", "V", "VIm", "VIIdim"};
    static const char *const MAJ7[7] = {"Imaj7", "IIm7", "IIIm7", "IVmaj7", "V7", "VIm7", "VIIm7b5"};
    static const char *const MIN[7] = {"Im", "IIdim", "III", "IVm", "Vm", "VI", "VII"};
    int d, root;
    for (d = 0; d < 7; d++) {
        CHECK(!strcmp(roman(0, VB_SC_MAJ, d, 0, 0), MAJ[d]), "C major degree %d: %s", d + 1, roman(0, VB_SC_MAJ, d, 0, 0));
        CHECK(!strcmp(roman(0, VB_SC_MAJ, d, 0, VB_CM_7), MAJ7[d]), "C major degree %d + 7: %s", d + 1,
              roman(0, VB_SC_MAJ, d, 0, VB_CM_7));
        CHECK(!strcmp(roman(9, VB_SC_MIN, d, 0, 0), MIN[d]), "A minor degree %d: %s", d + 1, roman(9, VB_SC_MIN, d, 0, 0));
    }
    vb_degree_chord(2, VB_SC_MAJ, 4, 0, 0, &root);
    CHECK(root == 9, "D major V: A (%d)", root);
    CHECK(!strcmp(roman(9, VB_SC_HMIN, 4, 0, VB_CM_7), "V7"), "A harmonic minor V7: %s", roman(9, VB_SC_HMIN, 4, 0, VB_CM_7));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 1, 0, VB_CM_SWAP), "II"), "SWAP on ii: %s", roman(0, VB_SC_MAJ, 1, 0, VB_CM_SWAP));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 1, 0, VB_CM_SWAP | VB_CM_7), "II7"), "SWAP + 7 on ii: the secondary dominant (%s)",
          roman(0, VB_SC_MAJ, 1, 0, VB_CM_SWAP | VB_CM_7));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 3, 0, VB_CM_SWAP), "IVm"), "SWAP on IV: %s", roman(0, VB_SC_MAJ, 3, 0, VB_CM_SWAP));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, -1, 10, 0), "bVII"), "borrowed bVII: %s", roman(0, VB_SC_MAJ, -1, 10, 0));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, -1, 3, 0), "bIII"), "borrowed bIII: %s", roman(0, VB_SC_MAJ, -1, 3, 0));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 0, 0, VB_CM_7 | VB_CM_9), "Imaj9"), "I 7+9: %s", roman(0, VB_SC_MAJ, 0, 0, VB_CM_7 | VB_CM_9));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 4, 0, VB_CM_7 | VB_CM_9), "V9"), "V 7+9: %s", roman(0, VB_SC_MAJ, 4, 0, VB_CM_7 | VB_CM_9));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 5, 0, VB_CM_7 | VB_CM_9), "VIm9"), "vi 7+9: %s", roman(0, VB_SC_MAJ, 5, 0, VB_CM_7 | VB_CM_9));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 4, 0, VB_CM_SUS4 | VB_CM_7), "V7sus4"), "V sus4 + 7: %s",
          roman(0, VB_SC_MAJ, 4, 0, VB_CM_SUS4 | VB_CM_7));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 0, 0, VB_CM_6 | VB_CM_9), "I6/9"), "I 6+9: %s", roman(0, VB_SC_MAJ, 0, 0, VB_CM_6 | VB_CM_9));
    CHECK(!strcmp(roman(0, VB_SC_MAJ, 0, 0, VB_CM_DIM), "Idim"), "I DIM: %s", roman(0, VB_SC_MAJ, 0, 0, VB_CM_DIM));
    CHECK(!strcmp(roman(7, VB_SC_MIX, 6, 0, 0), "VII"), "G mixolydian VII (F major): %s", roman(7, VB_SC_MIX, 6, 0, 0));
    {   /* SNAP, the chord at a sixteenth, a melody of motifs */
        static const uint8_t CEG[3] = {0, 4, 7};
        uint8_t sc[7];
        vb_play_t pl;
        uint32_t rng = 7;
        int i, g;
        CHECK(vb_snap(61, CEG, 3) == 60 && vb_snap(62, CEG, 3) == 60 && vb_snap(63, CEG, 3) == 64 && vb_snap(66, CEG, 3) == 67 &&
              vb_snap(70, CEG, 3) == 72 && vb_snap(64, CEG, 3) == 64, "SNAP to C E G");
        memset(&pl, 0, sizeof pl);
        pl.len = 3;
        pl.chord[0].len_i = 7;                             /* 16, 4, 8 sixteenths */
        pl.chord[1].len_i = 3;
        pl.chord[2].len_i = 5;
        CHECK(vb_chord_at(&pl, 0) == 0 && vb_chord_at(&pl, 15) == 0 && vb_chord_at(&pl, 16) == 1 && vb_chord_at(&pl, 20) == 2 &&
              vb_chord_at(&pl, 27) == 2 && vb_chord_at(&pl, 28) == 0, "the chord at a sixteenth, looping");
        vb_scale_pcs(9, VB_SC_MIN, sc);
        for (g = 3; g <= 4; g++) {
            vb_melody(&pl, 9, VB_SC_MIN, g, 2, &rng);
            CHECK(pl.len == g * 4, "melody of %d: %u steps", g, pl.len);
            for (i = 0; i < pl.len; i++) {
                int k, in = 0;
                for (k = 0; k < 7; k++)
                    in |= pl.seq[i].note % 12 == sc[k];
                CHECK(in && pl.seq[i].note >= 57 && pl.seq[i].note <= 57 + 25, "melody note %u in A minor, 2 octaves", pl.seq[i].note);
            }
            for (i = 0; i < g; i++)
                CHECK(pl.seq[i].note == pl.seq[2 * g + i].note, "A comes back as the 3rd motif");
            printf("melody of %d:", g);
            for (i = 0; i < pl.len; i++)
                printf(" %d%s", pl.seq[i].note, pl.seq[i].on ? "" : "(rest)");
            printf("\n");
        }
    }
    for (d = 0; d < 7; d++)
        printf("C major %-5s %-8s  A minor %s\n", MAJ[d], MAJ7[d], MIN[d]);
    printf(failures ? "\n%d FAILED\n" : "\nall harmony tests passed\n", failures);
    return failures != 0;
}
