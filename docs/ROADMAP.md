# veffbox roadmap

What is left, in the order agreed. Each firmware step ends with a CPU measurement on the FM-1 (`tools/fm1_remote.py stats`)
and screenshots of the new screens (`make shots`).

## Next

1. **Mockup: sidechain and spectral.** In `ui/index.html`, to click through before any firmware:
   - DUCK and GATE FX keyed by any track.
   - Track MUTE that still feeds the sidechain.
   - An SC modulation source.
   - The spectral FX below.
2. **Four tracks sounding at once.** The background tracks get only FX that need no buffer, because RAM is nearly full:
   95.7 of 96 KiB, and the pool has 14 KiB free.
3. **Sidechain on the device:**
   - an envelope follower per track;
   - DUCK and GATE (AMT, ATK, REL, SRC track);
   - MUTE that keeps the track as a key;
   - SC as a modulation source.
4. **Spectral FX.** A 256-point fixed-point FFT with 50% overlap: about 4 KB per slot, 6–12 ms of latency, and an estimated
   150–200 instructions per sample (not yet measured on the hardware).
   - SPFRZ (HOLD BLUR TILT MIX): holds the band amplitudes and randomizes the phases.
   - SMEAR (TIME SPRD DAMP MIX): averages the amplitudes over time.
   - SPSHFT (SHIFT STRCH FB MIX): shifts and stretches the bins, for inharmonic, metallic tones.
   - SPGATE (THR KEEP TILT MIX): keeps only the loudest bins, or only the quiet ones.
   - ROBOT (MODE PTCH TONE MIX): zero phases (robot) or random phases (whisper).
   - VOCDR (SRC BANDS Q MIX): a 16-band filterbank vocoder; the carrier is keyed by another track.

## Also open

- PITCH as a modulation destination.
- Longer prints: now at most 1.67 s.
- CHOP's transient detection runs in the audio ISR (52% peak on the first detection): move it to the main loop.
- The 63% spike at the start of a print: the FX buffers are cleared in the ISR.
- USB audio not yet verified by recording, because macOS blocks the microphone for the build session.
- Publishing the site: the repo is private (GitHub Pages or another host?).
- Checking how the Firmware Hub shows the identity `FM-1_62000100`.

## Done

- 0.2.x: SELECT on SRC picks the engine. The keys pick the DRUM voice and the OP knob picks OP4's operator. The labels in
  the small boxes (DRUM voices, LFO/ENV, FX slots) fit. OP4 no longer reads past its parameters after the FX or MOD page.
  Host screenshots: `make shots`.
- 0.2.0: the bank in flash with generations, REBUILD, GRAN and CHOP, USB remote diagnostics.
- 0.1.0: the whole instrument on the FM-1, its own identity, the showcase site.
