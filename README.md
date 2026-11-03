# veffbox

A resampling sound-design instrument for the M-VAVE FM-1.

One sound at a time: pick a source (OP4 FM, ANLG, DRUM, or a print as SAMP / GRAN / CHOP), play it
as notes, a sequence or a strummed chord sequence, modulate anything with 6 LFOs and 3 envelopes
(4 destinations each), stack up to 8 effects, then **PRINT**. The print becomes the next source.
Every print keeps its recipe, so each of the 64 bank sounds has a generation tree you can branch
from or rebuild bit-exactly.

**Status:** early. The interface is a clickable mockup; the firmware core is being built.

## Try the interface

```
python3 -m http.server 8137 --directory ui
```

Open <http://localhost:8137/>. The page is the FM-1 panel: click or drag the knobs, press the
buttons, play the keys (or Z S X D C… on the computer keyboard). The side panel explains every
click. Hash links open a screen directly, e.g. `#t4/SRC` (the DRUM kit) or `#t2/MOD`.

| FM-1 control | veffbox |
| --- | --- |
| EDIT | source (SRC) · SEL = RANDOMIZE |
| ARP / SEQ | play mode and strum / step row |
| LFO / ENV | modulation · SEL = LEARN a destination |
| FX | FX chain · SEL = SCRAMBLE |
| HOME / REC | print settings / print from any page |
| SAVE | bank: the generation tree, LOAD / EDIT / REBUILD |
| GLO | tempo, scale, CHAOS |
| hold SEL | undo the random rolls |
| SELECT / ALGORITHM / PRESETS | item on the page / track / bank sound |

## Core

Portable, integer-only C in `core/`, tested on the host:

```
make test
```

This renders every DRUM voice to `host/out/*.wav` and checks repeatability, overflow (fuzzed),
the kick's pitch sweep, voice release, LFO routing and tempo sync.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design, budgets and milestones.

## License

GPL-3.0-only. veffbox builds on [MNM-1](https://github.com/galfaroth/mnm1).
