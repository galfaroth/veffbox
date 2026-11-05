#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# The FM-1 package on GitHub (zvenson/fm1-build gives $JIELI_TOOLCHAIN and $AC79_SDK): MNM-1's tree at a fixed
# tag next to this repository, then tools/fm1_build.sh -> dist/veffbox-VERSION.fwsc.
#   bash tools/ci_build.sh
set -euo pipefail
MNM_TAG="${MNM_TAG:-v1.0.1}"
MNM_DIR="${MNM_DIR:-$PWD/../mnm}"
python3 -m pip install --quiet numpy
[ -d "$MNM_DIR" ] || git clone --quiet --depth 1 --branch "$MNM_TAG" https://github.com/galfaroth/mnm1.git "$MNM_DIR"
rm -f dist/veffbox-*.fwsc
sh tools/fm1_build.sh "$MNM_DIR"
