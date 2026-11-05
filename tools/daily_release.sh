#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# The daily release (.github/workflows/daily.yml): ONE feature a day from staging to main, so the updates spread out.
# A feature ends with the commit that changes VERSION.txt; the commits before it are work in progress. Each day main
# moves to the oldest such commit not yet on main, it gets the tag vVERSION, and release.yml builds the release.
# Nothing happens when no finished feature waits, or when that commit's CI run failed.
set -euo pipefail
git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
git fetch --quiet --tags origin main staging
if ! git merge-base --is-ancestor origin/main origin/staging; then
    echo "main has commits that staging lacks: merge main into staging first"
    exit 1
fi
F=""
for c in $(git rev-list --reverse --first-parent origin/main..origin/staging); do
    if ! git diff --quiet "$c^" "$c" -- VERSION.txt; then
        F="$c"
        break
    fi
done
if [ -z "$F" ]; then
    echo "no finished feature on staging (no commit changes VERSION.txt): no release today"
    exit 0
fi
V="$(git show "$F:VERSION.txt" | tr -d '[:space:]')"
echo "today's feature: $(git log -1 --format='%h %s' "$F") -> v$V"
if git rev-parse --quiet --verify "refs/tags/v$V" > /dev/null; then
    echo "v$V is released already: give that feature a new VERSION.txt"
    exit 1
fi
if git diff --name-only origin/main "$F" | grep -q '^\.github/workflows/'; then
    echo "the feature changes workflow files, which a GitHub token cannot push: push them to main by hand"
    exit 1
fi
ci="$(gh run list --workflow ci.yml --branch staging --commit "$F" --limit 1 --json status,conclusion \
      --jq '.[0] | "\(.status) \(.conclusion)"')"
case "$ci" in
"completed success") ;;
"")                                               # (pushed with later commits: no CI run of its own) the host tests
    echo "no CI run for ${F:0:7}: running the host tests"
    git checkout --quiet "$F"
    bash tools/ci_build_host.sh
    ;;
*)
    echo "CI on ${F:0:7}: $ci: no release today"
    exit 1
    ;;
esac
git push --quiet origin "$F:refs/heads/main"
git tag -a "v$V" -m "Veffbox $V" "$F"
git push --quiet origin "v$V"
gh workflow run release.yml --ref "v$V"
left="$(git rev-list --first-parent "$F..origin/staging" | while read -r c; do
    git diff --quiet "$c^" "$c" -- VERSION.txt || echo x; done | wc -l | tr -d ' ')"
echo "released v$V (${F:0:7}); finished features still waiting: $left"
