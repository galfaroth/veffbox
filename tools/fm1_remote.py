#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Remote control and diagnostics for Veffbox on a connected FM-1 (USB MIDI SysEx, fw/vb_app.c vb_remote).

  fm1_remote.py stats                       the audio load and the app's state
  fm1_remote.py press PLAY|EDIT|...|SEL     a button
  fm1_remote.py enc SELECT|ALGO|PRESET|K1..K4 DELTA
  fm1_remote.py key N on|off                key 0..26 (0 = F3)
  fm1_remote.py reset                       clear the max / late counters
  fm1_remote.py undo                        hold SEL
  fm1_remote.py save                        save to flash now
  fm1_remote.py script FILE                 one command per line ("wait 1.5" sleeps)
Needs mido + python-rtmidi; the port is the one whose name contains FM-1."""
import sys
import time

import mido

BUTTONS = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"]
ENCODERS = ["SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"]
FIELDS = ["halves", "late", "max_us", "last_us", "cpu_q8", "nested", "ms", "page", "src", "mode", "playing", "voices",
          "gens", "print_len", "peak", "row", "rec", "item", "events", "bpm10", "master_q12", "glo"]
PAGES = ["SRC", "PLAY", "MOD", "FX", "PRINT", "BANK", "GLO"]
SRCS = ["OP4", "ANLG", "DRUM", "SAMP", "GRAN", "CHOP"]
HALF_US = 5805


def port_name(names):
    for n in names:
        if "FM-1" in n:
            return n
    raise SystemExit(f"no FM-1 MIDI port in {names}")


class Remote:
    def __init__(self):
        self.out = mido.open_output(port_name(mido.get_output_names()))
        self.inp = mido.open_input(port_name(mido.get_input_names()))

    def send(self, *data):
        # the FM-1 holds one SysEx frame at a time and reads it between screen updates: pace the commands
        self.out.send(mido.Message("sysex", data=[0x7D, 0x56, 0x42, *data]))
        time.sleep(0.08)

    def stats(self, timeout=1.0):
        for _ in self.inp.iter_pending():
            pass
        self.send(1)
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.inp.iter_pending():
                if m.type == "sysex" and list(m.data[:4]) == [0x7D, 0x56, 0x42, 0x7F]:
                    d = list(m.data[4:])
                    vals = [sum(d[i * 5 + k] << (7 * k) for k in range(5)) for i in range(len(d) // 5)]
                    s = dict(zip(FIELDS, vals))
                    s["cpu_avg_pct"] = s["cpu_q8"] / 256
                    s["cpu_max_pct"] = 100 * s["max_us"] / HALF_US
                    s["cpu_last_pct"] = 100 * s["last_us"] / HALF_US
                    return s
            time.sleep(0.01)
        raise SystemExit("no answer from the FM-1 (is Veffbox running?)")

    def press(self, b):
        self.send(2, BUTTONS.index(b.upper()))

    def enc(self, e, d):
        self.send(3, ENCODERS.index(e.upper()), max(0, min(127, 64 + int(d))))

    def key(self, n, on):
        self.send(4, int(n), 1 if on else 0)

    def reset(self):
        self.send(5)

    def undo(self):
        self.send(6)


def show(s):
    print(f"cpu avg {s['cpu_avg_pct']:.1f}%  last {s['cpu_last_pct']:.1f}%  max {s['cpu_max_pct']:.1f}%  "
          f"late {s['late']}  halves {s['halves']}  | page {PAGES[s['page']] if s['page'] < 7 else s['page']} "
          f"src {SRCS[s['src']] if s['src'] < 6 else s['src']} mode {s['mode']} playing {s['playing']} "
          f"voices {s['voices']} gens {s['gens']} print {s['print_len']} peak {s['peak']} rec {s['rec']} "
          f"bpm {s['bpm10'] / 10} master {s.get('master_q12', 0)} row {s['row']} item {s['item']}"
          + (f"  | T{(s['glo'] >> 16 & 3) + 1} mute {s['glo'] & 15:04b} chords {s['glo'] >> 4 & 7} sync {s['glo'] >> 8 & 3} "
             f"midi {s['glo'] >> 10 & 1} next {s['glo'] >> 12 & 15:04b}" if "glo" in s else ""))


def run(r, words):
    c = words[0].lower()
    if c == "stats":
        show(r.stats())
    elif c == "press":
        r.press(words[1])
    elif c == "enc":
        r.enc(words[1], words[2])
    elif c == "key":
        r.key(words[1], words[2] == "on")
    elif c == "reset":
        r.reset()
    elif c == "undo":
        r.undo()
    elif c == "save":
        r.send(7)
    elif c == "wait":
        time.sleep(float(words[1]))
    else:
        raise SystemExit(f"unknown command {words}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    r = Remote()
    if sys.argv[1] == "script":
        for ln in open(sys.argv[2]):
            w = ln.split("#")[0].split()
            if w:
                run(r, w)
                time.sleep(0.05)
    else:
        run(r, sys.argv[1:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
