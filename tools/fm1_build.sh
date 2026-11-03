#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Builds the Veffbox FM-1 package on the Linux x86-64 build host (the JieLi toolchain runs only there).
#   tools/fm1_build.sh [MNM-1 tree]
# The platform (HAL, LCD, USB + update entry, flash, loader, packager) is MNM-1's: its tree is copied to
# build-fm1/, Veffbox's sources are added, and MNM-1's build.py builds fw/veffbox.c instead of felucca.c.
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
     'worker_errors, worker_notes = ([], ["audio worker: none (Veffbox)"]) if os.environ.get("APP_ENTRY") '
     'else check_audio_worker.check(rt, syms)'),
    ("""    elf = OUT / "felucca.elf"
    extra = []
""", """    elf = OUT / "felucca.elf"
    extra = []
    for src in os.environ.get("APP_EXTRA", "").split():     # Veffbox: the core, one object per file
        obj = OUT / (Path(src).stem + ".o")
        tc("cc", *flags, "-c", FW / "src" / src, "-o", obj)
        extra.append(obj)
"""),
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
# Veffbox's own identity and USB name (must contain "FM-1": the installers look for it)
python3 - "$T" <<'PY2'
import re, sys
root = sys.argv[1]
v = root + "/tools/version.py"
t = open(v).read()
t2 = re.sub(r'^PRODUCT = "FM-1_\d+"', 'PRODUCT = "FM-1_62000100"', t, count=1, flags=re.M)
if t2 == t:
    sys.exit("fm1_build: tools/version.py PRODUCT not found")
open(v, "w").write(t2)
u = root + "/firmware/src/usb_app.c"
t = open(u).read()
name = "FM-1 VEFFBOX"
arr = ", ".join(f"'{c}', 0" for c in name)
t2 = re.sub(r"static const uint8_t STR2\[\] = \{22, 3, 'F', 0, 'M', 0, '-', 0, '1', 0, ' ', 0, 'M', 0, 'N', 0, 'M', 0, '-', 0, '1', 0\};",
            "static const uint8_t STR2[] = {%d, 3, %s};" % (2 + 2 * len(name), arr), t, count=1)
if t2 == t:
    sys.exit("fm1_build: usb_app.c STR2 not found")
open(u, "w").write(t2)
PY2
EXTRA="$(cd "$T/firmware/src" && ls vb/*.c | tr '\n' ' ')"
cd "$T"
APP_EXTRA="$EXTRA" APP_ENTRY=veffbox.c FELUCCA_DUAL_CORE=0 DRUM_PACKAGE=1 DRUM_SITE=0 VB_VERSION="VEFFBOX $VER" python3 tools/build.py
cp build/felucca-UNTESTED.fwsc "$VB/dist/veffbox-$VER.fwsc"
echo "veffbox package: $VB/dist/veffbox-$VER.fwsc"
