# Veffbox architecture

Veffbox is a resampling sound-design instrument for the M-VAVE FM-1. You build one sound at a time:
a source, a way of playing it, modulation and an FX chain. Then you RESAMPLE it, and the resample becomes
the next source. Every resample keeps the recipe that made it, so any sound can be rebuilt from scratch.

The interface is specified by the clickable mockup in [`ui/index.html`](../ui/index.html). This
document maps that mockup onto firmware.

## Layers

| Layer | Where | What |
| --- | --- | --- |
| Platform | `platform/` (to come, from MNM-1) | FM-1 HAL: audio I2S, keys/encoders/LEDs, LCD, flash, USB MIDI, loader and package tools |
| Core | `core/` | Portable C, integer only: engines, FX, modulation, recipes. Built for the FM-1 and for the host (tests, later WASM for the mockup) |
| UI | `ui/` (mockup today), `app/` (firmware) | Pages, rows of 4 knobs, SELECT and SEL, the bank tree |

The core never touches hardware. The same C renders on the FM-1, in the host tests and, later, in the
browser, so the mockup can play the real engines.

## Core rules

- **Integer only.** The FM-1 has no FPU and no 64-bit divide. `tools/gen_tables.py` precomputes
  every curve (sine, tanh, decay, LFO rates, one-pole coefficients).
- **Deterministic.** All randomness comes from a seed stored in the recipe. The same recipe renders
  the same bits on every run. `host/test_core.c` checks this for every engine.
- **Descriptor-driven.** An engine or an effect is a table entry (`core/vb_param.h`). Its
  descriptors create the knob rows, the modulation destinations and the recipe layout, so adding an
  engine or an effect does not touch the UI or the modulation code.
- **Versioned sound.** A shipped engine never changes how it sounds. A fix is a new `version`, and
  old recipes keep using the old one, otherwise REBUILD would stop reproducing old generations.
- **Modulation adds a delta.** LFOs and envelopes add a Q8 (1/256 knob step) delta on top of the
  stored knob value (`core/vb_mod.h`). The recipe always holds the knob as you set it.

## Data model

```
bank[64]                    sound slots, browsed with PRESETS
  sound
    gens[]                  the generation tree: G0 (init) and every resample
      gen = { parent, seed, recipe (delta vs parent), audio cache? }
    working state           what the pages edit; becomes the recipe of the next resample
      src                   OP4 | ANLG | DRUM | SAMP | GRAN | CHOP  (+ its patch)
      play                  NOTE | SEQ (16 steps) | CHORD (8 steps: exact notes, LEN, GATE, strum)
      mod[9]                6 LFOs + 3 envelopes, each with 4 routes { dest, amount }
      fx[8]                 { type, 4 params, mix, on }
      resample                 { len, start, norm, after FLAT/KEEP, tail, fmt }
tracks[4]                   which bank sound each track plays; one is edited at a time
```

## Budget on the FM-1 (estimates, to be measured)

| Resource | Total | Plan |
| --- | --- | --- |
| SRAM | 512 KiB | ~100 KB FX buffers (8 slots), ~100 KB resample/sample buffer (~2.3 s at 22 kHz PCM), the rest code and state |
| Flash data | ~290 KiB writable | 64 sounds of recipes. A full recipe is ~550 B; stored as deltas vs the parent (~50–150 B), 64 sounds × 16 generations fit in ~150 KB |
| Audio cache | small | Only a few resamples keep their audio in flash. The rest are rebuilt from recipes on load (offline render, faster than real time) |
| CPU | ~3300 instructions/sample | One DRUM voice is tens of instructions per sample; an OP4 4-note chord is estimated at 15–20 % |
| Flash writes | silence the audio | Saves and resample commits happen with the transport stopped |

The audio cache is the real limit: 64 sounds can each have a tree, but only a handful can keep
their audio at once. REBUILD makes that invisible as long as rendering is fast enough.

## Milestones

1. **M0 – mockup.** Done: `ui/index.html`.
2. **M1 – portable core.** Started: DRUM (8 voices), LFOs and routes, host tests.
   Next: OP4, ANLG, the FX slots, envelopes, the recipe codec.
3. **M2 – platform.** Bring MNM-1's HAL, loader and build tools into `platform/`. First image: DRUM
   on the keys, the screen showing one page. Needs the JieLi toolchain and Docker (see MNM-1's `BUILDING.md`).
4. **M3 – UI on the device.** Pages, rows, SELECT/SEL, RANDOMIZE/SCRAMBLE with hold-SEL undo, LEARN.
5. **M4 – RESAMPLE.** Render the chain into the RAM buffer, then SAMP/GRAN/CHOP play it.
6. **M5 – bank.** Recipes as deltas in flash, the generation tree, LOAD/EDIT/REBUILD.
7. **M6 – WASM.** Build the core for the browser so the mockup plays the real sound.

## From MNM-1

MNM-1 (github.com/galfaroth/mnm1, GPL-3.0-only) provides the platform layer and proven DSP ideas.
Veffbox is GPL-3.0-only for that reason. Worth reusing: the HAL and loader, the integer filters, the
tape delay with a variable write rate, ADPCM crunch, the FX sleep-when-quiet rule, and the
save/apply/restore modulation overlay.
