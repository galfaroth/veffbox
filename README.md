# Veffbox

A resampling sound-design instrument for the M-VAVE FM-1.

One sound at a time: pick a source (OP4 FM, ANLG, DRUM, or a resample as SAMP / GRAN / CHOP), play it
as notes, a sequence or a strummed chord sequence, modulate anything with 6 LFOs and 3 envelopes
(4 destinations each), stack up to 8 effects, then **RESAMPLE**. The resample becomes the next source.
Every resample keeps its recipe, so each of the 64 bank sounds has a generation tree you can branch
from or rebuild bit-exactly.

**Status:** firmware 0.5.0 runs on the FM-1 (identity `FM-1_62000100`, USB name `FM-1 VEFFBOX`): all six sources,
chord sequences (16 chords of up to 8 notes, a BASS tone, voicings), chords by degree from the keys (KEY, SCALE, a
chord pad with modifiers), songs, modulation, the FX chain, RESAMPLE, the 64-sound bank with generations in flash, and bit-exact REBUILD.
All four tracks sound at once (the track in focus holds the resample and the buffered effects; the others up to 5
voices each, MUTE on GLO).
Not yet: PITCH as a modulation target, resamples longer than 1.67 s.

## Build, install, test on the hardware

```
tools/fm1_build.sh ../mnm-1.0.1                        # on the Linux x86-64 host with the JieLi toolchain
python3 ../tools/fm1_install.py dist/veffbox-X.Y.Z.fwsc --port FM-1 --yes   # MNM-1's installer
python3 tools/fm1_remote.py stats                       # CPU load, late buffers, the app's state
python3 tools/fm1_remote.py press PLAY                  # buttons, encoders and keys over USB SysEx
```
USB audio is the output at full level, independent of MASTER. USB MIDI notes play the sound.

## On the web

`site/` is the published site: the landing page, the emulator (`site/emu/`, built with `make emu`: the firmware
itself as WebAssembly, with sound), the mockup (`site/demo/`) and the web installer (`site/install/`, assembled with
`python3 tools/make_site.py dist/veffbox-X.Y.Z.fwsc`). `.github/workflows/pages.yml` publishes it.

## Try the interface

```
python3 -m http.server 8137 --directory ui
```

Open <http://localhost:8137/>. The page is the FM-1 panel: click or drag the knobs, press the
buttons, play the keys (or Z S X D C… on the computer keyboard). The side panel explains every
click. Hash links open a screen directly, e.g. `#t4/SRC` (the DRUM kit) or `#t2/MOD`.

| FM-1 control | Veffbox |
| --- | --- |
| EDIT | source (SRC): SELECT picks the engine, the keys pick the DRUM voice · SEL = RANDOMIZE |
| ARP / SEQ | play mode and strum / step row |
| LFO / ENV | modulation · SEL = LEARN a destination |
| FX | FX chain · SEL = SCRAMBLE |
| HOME / REC | resample settings / resample from any page |
| SAVE | bank: the generation tree, LOAD / EDIT / REBUILD |
| GLO | tempo, scale, CHAOS |
| hold SEL | undo the random rolls |
| SELECT / ALGORITHM / PRESETS | item on the page / track / bank sound |

## Core

Portable, integer-only C in `core/`, tested on the host:

```
make test
```

`make shots` draws every FM-1 screen to `host/shots/*.png` with the firmware's own UI code (needs the MNM-1 tree
at `MNM=..`). What is left: [docs/ROADMAP.md](docs/ROADMAP.md).

`make test` renders every DRUM voice to `host/out/*.wav` and checks repeatability, overflow (fuzzed),
the kick's pitch sweep, voice release, LFO routing and tempo sync.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design, budgets and milestones.

## License

GPL-3.0-only. Veffbox builds on [MNM-1](https://github.com/galfaroth/mnm1).
