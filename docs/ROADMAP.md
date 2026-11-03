# Veffbox roadmap

What is left, in the order agreed. Each firmware step ends with a CPU measurement on the FM-1 (`tools/fm1_remote.py stats`)
and screenshots of the new screens (`make shots`).

## Next

1. **Chords on the device.** Up to 8 notes per step, a BASS note per step (played first by every strum), and VOICING:
   CLOSE, OPEN, DROP2, SPREAD and VEFF. VEFF puts the root and fifth low, then the 3rd, 7th and 9th (plus the 11th in
   minor) packed in seconds up high. The recipe grows, so the flash format needs a version and a migration that keeps
   the stored bank. The mockup already does all of this.
2. **Four tracks sounding at once.** The background tracks get only FX that need no buffer, because RAM is nearly full:
   95.7 of 96 KiB, and the pool has 14 KiB free.
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
- The Firmware Hub: install from V15 (not tested yet), then send the mail (zven@thefirmwarehub.com).

## Done

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
