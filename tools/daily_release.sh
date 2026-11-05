#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# The daily release (.github/workflows/daily.yml): staging -> main, a tag, and release.yml started for it.
# Nothing happens when staging has nothing new or its CI run on that commit is not green.
set -euo pipefail
git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
git fetch --quiet --tags origin main staging
S="$(git rev-parse origin/staging)"
if [ -z "$(git rev-list origin/main..origin/staging)" ]; then
    echo "staging has nothing new: no release today"
    exit 0
fi
if ! git merge-base --is-ancestor origin/main origin/staging; then
    echo "main has commits that staging lacks: merge main into staging first"
    exit 1
fi
if git diff --name-only origin/main origin/staging | grep -q '^\.github/workflows/'; then
    echo "staging changes workflow files, which a GitHub token cannot push: push them to main by hand, then merge main into staging"
    exit 1
fi
ci="$(gh run list --workflow ci.yml --branch staging --commit "$S" --limit 1 --json status,conclusion --jq '.[0] | "\(.status) \(.conclusion)"')"
if [ "$ci" != "completed success" ]; then
    echo "CI on staging ${S:0:7}: ${ci:-no run}: no release today"
    exit 1
fi
git checkout --quiet -B staging "$S"
V="$(tr -d '[:space:]' < VERSION.txt)"
if git rev-parse --quiet --verify "refs/tags/v$V" > /dev/null; then
    IFS=. read -r a b c <<< "$V"
    while git rev-parse --quiet --verify "refs/tags/v$a.$b.$c" > /dev/null; do c=$((c + 1)); done
    V="$a.$b.$c"
    echo "$V" > VERSION.txt
    git commit --quiet -m "Veffbox $V" VERSION.txt
    git push --quiet origin staging
fi
git push --quiet origin HEAD:main
git tag -a "v$V" -m "Veffbox $V"
git push --quiet origin "v$V"
gh workflow run release.yml --ref "v$V"
echo "released v$V ($(git rev-parse --short HEAD))"
