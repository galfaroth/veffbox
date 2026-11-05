# Releasing

New work goes to the `staging` branch. `main` holds what is released.

1. **Push to `staging`.** CI (`.github/workflows/ci.yml`) builds the FM-1 package on GitHub with
   [zvenson/fm1-build](https://github.com/zvenson/fm1-build) and runs the host tests (`make test`, `make uitest`).
   The package is the run's artifact `packages`: download it from the run page and install it on an FM-1 to test.
2. **A feature ends with the commit that changes `VERSION.txt`** (0.4.0, 0.5.0 ...). Commits before it are work in
   progress, and nothing releases them on their own. Push each finished feature's commit by itself, so CI runs on it.
3. **Once a day, at 09:00 UTC** (10:00 in Poland in winter, 11:00 in summer), `daily.yml` runs
   `tools/daily_release.sh` and releases **one** feature, so the updates spread out over the days:
   - `main` moves to the oldest finished feature not on `main` yet (with the work in progress before it), and that
     commit gets the tag `vVERSION`;
   - nothing happens if no finished feature waits, or if CI on that commit failed. With no CI run of its own (it
     was pushed together with later commits), the run checks it with the host tests instead;
   - `release.yml` then builds and attests the package from the tag, publishes the release with the `.fwsc`, the
     zip (package, licences, README) and the list of commits, and starts `pages.yml`, which rebuilds the site
     (installer, download, emulator) with the new package.
   Five finished features on `staging` go out on five days, one each day.

Start a run by hand: Actions → "daily release" → Run workflow. A tag pushed by hand (`git tag v0.4.0 && git push
origin v0.4.0`) builds a release too.

Workflow files (`.github/workflows/`) can't be pushed by the daily run, because a GitHub token isn't allowed to
change them. Push such changes to `main` yourself, then merge `main` into `staging`. The daily run stops with a
message when `staging` has commits that `main` lacks in that folder, or when `main` has commits that `staging` lacks.

Every release reaches the Firmware Hub within about three hours, so test on the FM-1 from the CI artifact before the
next daily run.
