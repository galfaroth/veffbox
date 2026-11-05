#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# The host tests on GitHub: the core (make test) and the firmware's UI with a flash in RAM (make uitest, which needs
# MNM-1's tree and its generated headers; tools/ci_build.sh cloned it).
set -euo pipefail
MNM_DIR="${MNM_DIR:-$PWD/../mnm}"
make test
[ -d "$MNM_DIR/build/gen" ] || (cd "$MNM_DIR" && python3 tools/build.py --gen-only)
make uitest MNM="$MNM_DIR"
