# Veffbox roadmap

What is left, in the order agreed. Each firmware step ends with a CPU measurement on the FM-1 (`tools/fm1_remote.py stats`)
and screenshots of the new screens (`make shots`).

## Next

The features asked for on 2026-11-05, one release each (docs/RELEASING.md), in the order agreed on 2026-11-05:

1. ~~Chords by degree (I, III ...), entered HiChord / Orchid style, jamming with them~~: done in 0.4.0.
2. ~~Several tracks at once~~: done in 0.5.0.
3. ~~Chords per track or global~~: done in 0.6.0.
4. ~~Play in sync~~: done in 0.7.0.
5. ~~A built-in library~~: done in 0.8.0.
6. ~~Magic arp~~: done in 0.9.0.
7. ~~Notes and melodies that land on the chord~~: done in 0.10.0.
8. ~~A planned sequence, played by any keys~~: done in 0.11.0.
9. **A plugin version** for computers (not ordered yet): the core is portable C, so a CLAP / VST3 plugin around it.

The order agreed before:

2. ~~Four tracks sounding at once~~ (0.5.0).
3. **Sidechain on the device**, as in the mockup:
   - an envelope follower per track;
   - DUCK and GATE (AMT, ATK, REL, SRC track);
   - MUTE that keeps the track as a key;
   - SC as a modulation source.
4. **Spectral FX**, as in the mockup. A 256-point fixed-point FFT with 50% overlap: about 4 KB per slot, 6–12 ms of latency, and an estimated
   150–200 instructions per sample (not yet measured on the hardware).
   - SPFRZ (HOLD BLUR TILT MIX): holds the band amplitudes and randomizes the phases.
   - SMEAR (TIME SPRD DAMP MIX): averages the amplitudes over time.
   - SPSHFT (SHIFT STRCH FB MIX): shifts and stretches the bins, for inharmonic, metallic tones.
   - SPGATE (THR KEEP TILT MIX): keeps only the loudest bins, or only the quiet ones.
   - ROBOT (MODE PTCH TONE MIX): zero phases (robot) or random phases (whisper).
   - VOCDR (SRC BANDS Q MIX): a 16-band filterbank vocoder; the carrier is keyed by another track.

## Also open

- PITCH as a modulation destination.
- Longer resamples: now at most 1.67 s.
- CHOP's transient detection runs in the audio ISR (52% peak on the first detection): move it to the main loop.
- The 63% spike at the start of a resample: the FX buffers are cleared in the ISR.
- USB audio not yet verified by recording, because macOS blocks the microphone for the build session.
- Knob jumps: TAPE TIME, CHORUS, SHIFT, PHASER and RESON step 4-14x the signal's own steepest step after a big knob
  jump (host/test_fx.c "known:" lines; found when CI ran the tests on Linux). Their glides (~12 ms) are too fast.
- The Firmware Hub: the reply to Sven (the V15 test is done), then the listing.

## Done

- 0.11.0: KEYS NEXT on PLAY (SEQ, CHORD, ARP; per track, in the settings): any key plays the next step, held as long as
  the key; the sequencer leaves that track to the keys.
- 0.10.0: SNAP (NOTE, SEQ: OFF, CHORD, SCALE): the keys and the sequence's notes moved to the nearest tone of the chord
  playing (CHORDS' track or the own progression, by the sixteenth) or of KEY / SCALE; SEQ EDIT RND 3 / RND 4: a melody
  of motifs (A, A up, A, B) in KEY / SCALE over RANGE 1-3 octaves, undoable.
- 0.9.0: MODE ARP: the chord playing (own progression or CHORDS'), or the keys / pad chord held, UP DOWN UP-DN RND
  ORDER, 1-4 octaves, 1/16 1/8 1/4 1/32; held keys run it while stopped. MIDI on GLO: every track's notes out over USB
  (track n on channel n). host/test_arp.c.
- 0.8.0: the library (core/vb_lib.c): 15 sources (LATEX, RUBBER BS, GLASS, METAL HIT, FM BELL, WOOD, BUZZ BASS,
  SUPERSAW, PLASTIC, SUB BOOM, SYNC LEAD, BLIP, three kits) and 10 FX chains; LIB, the last row of SRC and a row of FX,
  applies as you turn, hold SEL undoes the browsing. host/test_lib.c: every entry sounds, in range, bounded.
- 0.7.0: SYNC on GLO. OUT: MIDI clock over USB from the audio thread (24 a quarter, START / STOP with PLAY). IN:
  START / STOP / CONTINUE run the tracks; the tempo from the ticks' times over each beat; at every sixteenth the phase
  is pulled half way to the clock's (host test: 125 BPM followed exactly, in phase after 20 s).
- 0.6.0: CHORDS on GLO: OWN, or every track in CHORD plays T1..T4's progression in step with its own sound, strum
  and voices; that track's PLAY page edits the source's chords; RESAMPLE copies them into the recipe (REBUILD stays exact).
- 0.5.0: four tracks at once. A player per track, mixed, PLAY / STOP for all; the selected track holds the FX
  buffers and the print (moved in the audio thread in two steps, cleared in the main loop between); in the background a
  track bypasses the buffered FX, a sampler source is silent, 5 voices; MUTE per track on GLO; players sleep after 1.5 s
  of silence. The bank in RAM is an 8-sound cache (saved before reuse), which freed ~47 KB of the pool. On the FM-1:
  idle 2.5%; T1 chords + T2 bass + T3 drums 56% avg, 68% max; two chord tracks + drums 78% max; 18 track changes while
  playing: no late buffer.
- 0.4.0: chords by degree. KEY, SCALE (9 scales) and CHORD INPUT DEGREE: the keys are a chord pad on every page
  (C4..B4 I..VII, C5..G5 an octave up, the black keys bII bIII bV bVI bVII; F3..B3 held: 7 6 SWAP SUS2 SUS4 DIM 9); on PLAY,
  stopped, each chord fills the selected step; the roman numeral on the screen.
- Tested with the official V15 on the FM-1 (2026-11-04): Veffbox 0.3.2 -> V15 (the hub's file, SHA-256 db1642b2..., the
  FM-1 reported FM-1_015), then V15 -> the 0.3.2 release file (dccb1783...), FM-1_62000100, keys and sound fine.
- 0.3.1: CHOP SPLIT: TRANS (the transients, SENS) or EVEN (SLICES equal parts, 2..64); the screen marks the cuts
- 0.3.2: the screen as the mockup draws it: OP4's algorithm drawn with the operator's envelope, the LFO's cycle with its moving position, the ENV's ADSR, rounded chips, boxes and pills
  (from the analysis the audio thread already made: the UI never analyses). Flash format 4 (8 spare bytes in the
  recipe for later parameters); formats 1-3 convert, tested on the FM-1 from format 3.

- 0.3.0: chords: 16 steps of up to 8 notes, 22 types (6, m6, 6/9, maj9, 9, m11, 7sus4, maj7#11, m7b5, aug, madd9
  added), the BASS as a chord tone (1 root, 2 third, 3 fifth ...) in octave 2, VOICING (CLOSE OPEN DROP2 SPREAD VEFF),
  extended names. EDIT on PLAY (SEL runs it): INS, DUP, DEL, ON/OFF in SEQ. OCT on PLAY moves the selected chord or
  step by 12. SONG (SAVE pressed again): 64 songs, each the 4 tracks' sounds, BPM and CHAOS (the parts live in the
  sounds); COPY (BANK) puts a sound in a free slot so two songs can play different parts of it. SEL on the LFO / ENV
  shape rows = RANDOM, on the destination rows = LEARN. Every changed sound is saved. Older banks (formats 1 and 2)
  convert on load. 10 voices: 48% CPU with ten OP4 notes held. `make uitest`. Two flash sectors (0xDE000..0xE0000)
  are free now.
- The site: the emulator (the firmware as WebAssembly, `make emu`: real screen, real sound, the bank kept in the
  browser), the web installer (MNM-1's, `tools/make_site.py`), `fmhub.json`, and a Pages workflow.
  The repo is public and the site is at https://galfaroth.github.io/veffbox/. Left Veffbox to MNM-1 1.0.1 and came
  back on the FM-1, identity checked both ways. `FM-1_62000100` is free on The Firmware Hub (checked against its catalogue of 151 firmwares on 2026-11-03).

- Mockup: DUCK, GATE and VOCODER keyed by any track; the SC modulation source; MUTE per track on GLO (row 3); a muted
  track still keys. The spectral FX (SPEC FREEZE, SMEAR, SPEC SHIFT, SPEC GATE, ROBOT). Chords with 8 notes, BASS and
  VOICING. SCRAMBLE leaves the keyed effects out.
- "Print" is called "resample" everywhere: the firmware screens, the mockup, the site and the docs.

- 0.2.x: SELECT on SRC picks the engine. The keys pick the DRUM voice and the OP knob picks OP4's operator. The labels in
  the small boxes (DRUM voices, LFO/ENV, FX slots) fit. OP4 no longer reads past its parameters after the FX or MOD page.
  Host screenshots: `make shots`.
- 0.2.0: the bank in flash with generations, REBUILD, GRAN and CHOP, USB remote diagnostics.
- 0.1.0: the whole instrument on the FM-1, its own identity, the showcase site.
