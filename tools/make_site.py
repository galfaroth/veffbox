#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Assembles the published site in site/ for GitHub Pages:

  site/install/index.html       the web installer: MNM-1's (web/index_pkg.html with fm1pkg.js and fm1ota.js inlined),
                                in Veffbox's colours and words, pointing at the package below
  site/firmware/veffbox-VER.fwsc  the package (only the current one), with the licence texts next to it
  site/emu/veffbox.{js,wasm}    the emulator (make emu)

  python3 tools/make_site.py dist/veffbox-0.2.2.fwsc [--mnm ..]
"""
import argparse
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F
REPO = "https://github.com/galfaroth/veffbox"


def product_of(raw):
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def one(html, old, new):
    if html.count(old) != 1:
        raise SystemExit(f"installer: expected {old[:60]!r} once (MNM-1's web/index_pkg.html changed?)")
    return html.replace(old, new)


def installer(web, info):
    html = (web / "index_pkg.html").read_text(encoding="utf-8")
    lib = "\n".join(strip_module((web / f).read_text(encoding="utf-8")) for f in ("fm1pkg.js", "fm1ota.js"))
    # the install counter is MNM-1's: Veffbox counts nothing
    lib += "\nconst loadPageCounter = () => false, counterOn = () => false, countInstall = () => false;\n"
    html = html.replace("/*LIB*/", lib).replace("/*META*/", json.dumps(info))
    html = re.sub(r"<!-- SPDX-License-Identifier: GPL-3.0-only.*?-->",
                  "<!-- SPDX-License-Identifier: GPL-3.0-only\n     Veffbox: 2026 Veff. The installer is MNM-1's "
                  "(Felucca by Leo Kuroshita, Hügelton Instruments; Hortator by DEADACTIVE) -->", html, count=1, flags=re.S)
    html = one(html, "<title>Install MNM-1</title>", "<title>Install Veffbox</title>")
    # Veffbox's pink instead of MNM-1's grey (the same tokens, light and dark)
    html = one(html, "--backdrop: #c3c8ce; --ink: #23272c; --ink-soft: #4d545c; --panel: #5a5e65; --panel-ink: #e6e4dc;\n"
                     "    --card: #d3d7dc; --rule: #a9afb6; --warn-bg: #e9ddc7; --warn-ink: #4a3a1c; --accent: #23272c; --on-accent: #f3f4f6;",
               "--backdrop: #fcf2f8; --ink: #1d0819; --ink-soft: #4a2a40; --panel: #e07fb5; --panel-ink: #1d0819;\n"
               "    --card: #f4d9ea; --rule: #e5b9d4; --warn-bg: #fbe3ef; --warn-ink: #6b1d47; --accent: #1d0819; --on-accent: #fcf2f8;")
    html = re.sub(r"--backdrop: #1f2226; --ink: #e3e6ea; --ink-soft: #a7aeb6; --card: #2a2e33;\s*--rule: #3c4249; "
                  r"--warn-bg: #3a3222; --warn-ink: #ead9b5; --accent: #e6e4dc; --on-accent: #1f2226;",
                  "--backdrop: #1d0819; --ink: #fbe3ef; --ink-soft: #d9a9c5; --card: #2c1027; --rule: #4a1f40; "
                  "--warn-bg: #3d1431; --warn-ink: #f6c6de; --accent: #e07fb5; --on-accent: #1d0819;", html)
    html = html.replace("background: linear-gradient(170deg, #63676e, var(--panel) 45%, #474b51);",
                        "background: linear-gradient(170deg, #ea9cc8, var(--panel) 45%, #d76aa6);")
    html = html.replace(".plate .silk { font: 13px/1 \"Silkscreen\", ui-monospace, monospace; letter-spacing: .04em; color: #c9ccd1;",
                        ".plate .silk { font: 13px/1 \"Silkscreen\", ui-monospace, monospace; letter-spacing: .04em; color: #4a1534;")
    html = html.replace(".plate p { margin: 8px 0 0; color: #d9dce0;", ".plate p { margin: 8px 0 0; color: #2a0c22;")
    html = one(html, '<a class="back" href="../../">← MNM-1: play it in the browser</a>',
               '<a class="back" href="../">← Veffbox: play it in the browser</a>')
    html = one(html, '<p class="silk">HORTATOR</p>', '<p class="silk">VEFFBOX</p>')
    html = one(html, "<h1>Install MNM-1 on your FM-1</h1>", "<h1>Install Veffbox on your FM-1</h1>")
    html = one(html, "<p>Six-track synth firmware for the M-VAVE FM-1. It installs over USB from this page, in a few minutes.</p>",
               "<p>Resampling sound-design firmware for the M-VAVE FM-1. It installs over USB from this page, in a few minutes.</p>")
    html = re.sub(r"<footer>.*?</footer>", f"""<footer>
    <p>Veffbox by Veff. It runs on MNM-1's platform: Felucca by Leo Kuroshita (@kurogedelic),
      <a href="https://hugelton.com" target="_blank" rel="noopener">Hügelton Instruments</a>, and Hortator by DEADACTIVE.</p>
    <p>M-VAVE and FM-1 are trademarks of their respective owners.</p>
    <p><span data-t="license"></span> <a href="{REPO}">Source code</a> · <a href="../firmware/LICENSES/">Licences</a></p>
  </footer>""", html, count=1, flags=re.S)
    html = one(html, 'beta: "MNM-1 is beta firmware and may be unstable.', 'beta: "Veffbox is early firmware and may be unstable.')
    html = one(html, 'done: "Done. The FM-1 restarts with MNM-1."', 'done: "Done. The FM-1 restarts with Veffbox."')
    html = one(html, "say(`MNM-1 ${meta.version}`);", "say(`Veffbox ${meta.version}`);")
    html = re.sub(r'v_std: "MNM-1",\s*v_ble: ".*?",\s*v_ble_note: ".*?",', 'v_std: "Veffbox", v_ble: "", v_ble_note: "",', html, flags=re.S)
    return html


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg")
    ap.add_argument("--mnm", default=str(ROOT.parent), help="the MNM-1 tree (its web/ and LICENSES/)")
    a = ap.parse_args()
    pkg, mnm = Path(a.pkg), Path(a.mnm)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if product != "FM-1_62000100" or b"FELUCCA-LOADER-1" not in raw:
        raise SystemExit(f"{pkg}: identity {product!r}; expected FM-1_62000100 with the Felucca loader")
    version = (ROOT / "VERSION.txt").read_text().strip()
    name = f"veffbox-{version}.fwsc"
    site = ROOT / "site"
    inst, fw = site / "install", site / "firmware"
    inst.mkdir(parents=True, exist_ok=True)
    fw.mkdir(parents=True, exist_ok=True)
    for old in fw.glob("veffbox-*.fwsc"):
        old.unlink()
    shutil.copy(pkg, fw / name)
    lic = fw / "LICENSES"
    lic.mkdir(exist_ok=True)
    names = sorted(f.name for f in (mnm / "LICENSES").glob("*.txt"))
    for n in names:
        shutil.copy(mnm / "LICENSES" / n, lic / n)
    shutil.copy(ROOT / "LICENSE", lic / "GPL-3.0.txt")
    (lic / "index.html").write_text('<!doctype html><meta charset="utf-8"><title>Licences</title><h1>Licence texts</h1><ul>'
                                    + "".join(f'<li><a href="{n}">{n}</a></li>' for n in ["GPL-3.0.txt"] + names)
                                    + "</ul>\n", encoding="utf-8")
    info = {"version": version, "product": product, "pkg": "../firmware/" + name}
    (inst / "index.html").write_text(installer(mnm / "web", info), encoding="utf-8")
    (site / ".nojekyll").write_text("")
    print(f"site: install/ ({product} {version}), firmware/{name}")


if __name__ == "__main__":
    main()
