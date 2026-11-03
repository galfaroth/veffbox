#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Builds the veffbox FM-1 package on the Linux x86-64 build host (the JieLi toolchain runs only there).
#   tools/fm1_build.sh [MNM-1 tree]
# The platform (HAL, LCD, USB + update entry, flash, loader, packager) is MNM-1's: its tree is copied to
# build-fm1/, veffbox's sources are added, and MNM-1's build.py builds fw/veffbox.c instead of felucca.c.
# Env: JIELI_TOOLCHAIN, AC79_SDK (defaults: the build host's checkout next to the MNM-1 tree).
set -e
VB="$(cd "$(dirname "$0")/.." && pwd)"
MNM="$(cd "${1:-$HOME/Documents/Projects/experiments/test/mnm}" && pwd)"
BASE="$(dirname "$MNM")"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$(ls -d "$BASE"/jieli/*/ | head -1)}"
export AC79_SDK="${AC79_SDK:-$BASE/sdk}"
T="$VB/build-fm1"
VER="$(cat "$VB/VERSION.txt")"
rm -rf "$T"
mkdir -p "$T" "$VB/dist"
(cd "$MNM" && tar cf - --exclude=./build --exclude=./dist --exclude=./.git .) | (cd "$T" && tar xf -)
mkdir -p "$T/firmware/src/vb"
cp "$VB"/core/*.h "$VB"/core/*.c "$T/firmware/src/vb/"
cp "$VB"/fw/*.c "$T/firmware/src/"
python3 - "$T/tools/build.py" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
for a, b in [
    ('FW / "src" / "felucca.c", "-o", OUT / "felucca.o"',
     'FW / "src" / os.environ.get("APP_ENTRY", "felucca.c"), "-o", OUT / "felucca.o"'),
    ('worker_errors, worker_notes = check_audio_worker.check(rt, syms)',
     'worker_errors, worker_notes = ([], ["audio worker: none (veffbox)"]) if os.environ.get("APP_ENTRY") '
     'else check_audio_worker.check(rt, syms)'),
    ("""f'-DFELUCCA_VERSION="{VERSION or drum_version.firmware_string(drum_version.read())}"'""",
     """f'-DFELUCCA_VERSION="{os.environ.get("VB_VERSION") or VERSION or drum_version.firmware_string(drum_version.read())}"'"""),
]:
    if a in t:
        t = t.replace(a, b)
    elif "check_audio_worker" in a and "check_audio_worker" not in t:
        pass                                     # an MNM-1 tree without the dual-core check
    else:
        sys.exit("fm1_build: build.py changed, patch not applied: " + a[:60])
open(p, "w").write(t)
PY
cd "$T"
APP_ENTRY=veffbox.c FELUCCA_DUAL_CORE=0 DRUM_PACKAGE=1 DRUM_SITE=0 VB_VERSION="VEFFBOX $VER" python3 tools/build.py
cp build/felucca-UNTESTED.fwsc "$VB/dist/veffbox-$VER.fwsc"
echo "veffbox package: $VB/dist/veffbox-$VER.fwsc"
