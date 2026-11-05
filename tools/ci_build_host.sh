#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# The host tests without the FM-1 toolchain (the daily release, for a commit with no CI run of its own).
set -euo pipefail
MNM_DIR="${MNM_DIR:-$PWD/../mnm}"
python3 -m pip install --quiet Pillow numpy
[ -d "$MNM_DIR" ] || git clone --quiet --depth 1 --branch "${MNM_TAG:-v1.0.1}" https://github.com/galfaroth/mnm1.git "$MNM_DIR"
bash tools/ci_test.sh
