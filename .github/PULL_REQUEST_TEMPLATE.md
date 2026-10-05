<!--
Two things make a review here fast: (1) the checklist below is filled in with real
results, not hopes, and (2) the "How I verified" section says exactly what was run.
-->

## What changes

## Why

## How I verified
- [ ] `meson setup build --buildtype=debug` then `ninja -C build` — clean, with `-Dwerror` left **on**
- [ ] `meson test -C build` — all tests pass (say how many)
- [ ] Real dictation on a live desktop: hold the trigger key → speak → text committed
- [ ] Checked `fcitx5 -r -d` log for the new lines and no new errors

## Scope discipline
- [ ] No new mandatory background daemon
- [ ] No new hard dependency without a fallback path
- [ ] No API key, token, recognised text or audio ever committed (including in fixtures — use generated or clearly synthetic data)
- [ ] User-visible behaviour changes are reflected in `README.md`, `README.zh.md` and `docs/nextless.1`
- [ ] `CHANGELOG.md` has an entry under **Unreleased**

## Notes for the reviewer
<!-- Anything you tried that did not work, edge cases you know are still open, or a perf number worth looking at. -->
