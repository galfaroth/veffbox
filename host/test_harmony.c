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
    for (d = 0; d < 7; d++)
        printf("C major %-5s %-8s  A minor %s\n", MAJ[d], MAJ7[d], MIN[d]);
    printf(failures ? "\n%d FAILED\n" : "\nall harmony tests passed\n", failures);
    return failures != 0;
}
