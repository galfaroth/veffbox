#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The release zip: the package, the licence texts and a short README.

  python3 tools/make_zip.py dist/veffbox-0.3.2.fwsc --mnm ../mnm     -> dist/veffbox-0.3.2.zip
"""
import argparse
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SITE = "https://galfaroth.github.io/veffbox"
REPO = "https://github.com/galfaroth/veffbox"

README = """Veffbox {v}, resampling sound-design firmware for the M-VAVE FM-1 (identity FM-1_62000100).

Install: open {site}/install/ in Chrome or Edge, connect the FM-1 by USB, and
choose {name} (or let the page fetch it). Any installer that takes .fwsc packages works too.
Do not unplug the FM-1 while it installs. Banks from earlier versions convert automatically.

Try it first in the browser, with a step-by-step tutorial: {site}/
Source (GPL-3.0-only): {repo}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg")
    ap.add_argument("--mnm", default=str(ROOT.parent), help="the MNM-1 tree (its LICENSES/)")
    a = ap.parse_args()
    pkg = Path(a.pkg)
    m = re.fullmatch(r"veffbox-(.+)\.fwsc", pkg.name)
    if not m:
        raise SystemExit(f"{pkg}: expected veffbox-VERSION.fwsc")
    v = m.group(1)
    top = f"veffbox-{v}"
    out = pkg.with_suffix(".zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(pkg, f"{top}/{pkg.name}")
        z.write(ROOT / "LICENSE", f"{top}/LICENSES/GPL-3.0.txt")
        for f in sorted((Path(a.mnm) / "LICENSES").glob("*.txt")):
            z.write(f, f"{top}/LICENSES/{f.name}")
        z.writestr(f"{top}/README.txt", README.format(v=v, name=pkg.name, site=SITE, repo=REPO))
    print(out)


if __name__ == "__main__":
    main()
