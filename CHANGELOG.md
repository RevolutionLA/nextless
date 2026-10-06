# Changelog

All notable changes to Nextless are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), and this project keeps one line per
user-visible change rather than one per commit.

## [Unreleased]

### Added
- GitHub Actions CI: build (debug + release) with `-Dwerror` left on, full unit suite against a
  headless PulseAudio null sink, plus a job that keeps the opt-in diagnostics build compiling.
- Issue and PR templates, `CONTRIBUTING.md`, `SECURITY.md`, `.editorconfig`.
- Badges (CI / release / license / bilingual docs) and cross-links to the contribution docs at
  the top of both READMEs.
- Local punctuation (issue #4): Zipformer and FireRed results are post-processed through
  `sherpa-onnx-offline-punctuation` (ct-transformer) on the recognition thread — measured ~0.19 s
  per 44-character utterance (i7-1260P, int8 model), far below the ASR leg, so it is always-on
  rather than per-backend-toggled, and killable via `[punctuation].enabled` in `advanced.json`.
  The decoration can never eat an utterance: missing model, non-zero exit, empty output or a
  timeout all commit the raw text (covered by `test_punctuator`, including the process-group
  kill on timeout). Cloud backends and mock are a hard no-op. The ~62 MB model is opt-in via
  `nextless-get-models --punctuation`; the "no punctuation from local models" limitation is
  gone from both READMEs and the man page gained a post-processing section.
- `nextless-get-models` (`tools/get_models.sh`): first-run model helper closing the step where
  new users bail (issue #5). Detects the CPU architecture (x86_64 / aarch64 map to their exact
  v1.13.8 asset names — the aarch64 `-shared-cpu` variant is not derivable from the x64 name),
  reports download sizes before fetching, asks Zipformer / FireRed / both, is idempotent, and
  stages inside `~/.local/share/nextless/` so a failed download leaves no half-extracted model
  — extracted directories are checked against the names `advanced.json` expects before anything
  moves into place. `--dry-run` and `--arch=` make the URL mapping testable without ARM hardware;
  a contract test (`test_get_models`) runs the install / skip / corrupt-archive / wrong-directory
  legs against fake `file://` release trees. Installed via `meson install` into the bindir, so
  PKGBUILD packages pick it up automatically. The missing-runtime/-model panel messages now name
  this command instead of pointing at the README, whose section 4 leads with the one-liner and
  keeps the manual curl block as the do-it-yourself alternative.

### Fixed
- First run without the offline pieces is now actionable. Both local providers check the
  sherpa-onnx binary (executable) and their model files (readable) before spawning, and the error
  names the exact path — `Zipformer: sherpa-onnx runtime not found at <path> (see README "Offline
  backends")` — instead of a bare `spawn failed` / `recognition failed`. The input panel maps
  these to "sherpa-onnx runtime missing" / "offline model missing" with the README pointer, via
  the new pure `panelStatusForError()` (`adapter/src/panel_status.h`), and a stub-driven test
  (`test_missing_components`) covers all four provider failure paths plus the panel mapping. A
  non-zero child exit also logs the first line the child printed, so a corrupt model is not
  silent either.
- `config/advanced.json.example` no longer contradicts the built-in defaults it shadows: the
  zipformer `model_dir` pointed at `.../models/zipformer-zh-en`, a directory no download step
  creates (the README unpacks `sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20`), and
  both sections pinned `num_threads` back to the old hardcoded 30. Now that the build installs
  these files and the loader copies them into `~/.config/nextless/` on first read, a wrong value
  here would silently override the fix.
- The packaged default configs are installed for real now: `meson install` ships the five
  `config/*.json.example` files, renamed to `*.json`, into the build's `sysconfdir`/nextless —
  `/etc/nextless` for distro builds (the PKGBUILD passes `--sysconfdir=/etc`), `/usr/local/etc/nextless`
  for a plain source install — and the loader reads that same path, compiled in as
  `NEXTLESS_PACKAGED_CONFIG_DIR`. Before this, the loader read a hardcoded `/etc/nextless` that no
  source install ever populated, so the examples were repository decoration. The PKGBUILD's own
  `/etc` install loop is gone; `backup=...` still protects local edits during upgrades.
- The temporary WAV of an utterance is now deleted *before* the result/error callback fires, in all
  four providers (`temp_wav.h`). Previously the deletion lived in a scope guard, so "the callback
  fired" no longer implied "the file is gone" — `cloud_provider_queue` failed intermittently in CI's
  debug job (runs 37298301668 and 37266963582 show leftover WAVs), and the same window existed on
  every early return in the local providers. Local reproduction under `taskset -c 0` is probabilistic
  (~5% failure rate on the old code).
- Build on older distros: the adapter dropped its `fcitx-utils/eventloopinterface.h` include. That
  header only exists in recent fcitx5 releases; the one thing it was commented as providing
  (`now()`) is actually `std::chrono::steady_clock::now()`, and `event.h` already gives us
  `EventLoop`/`addTimeEvent`. CI runs on ubuntu-latest precisely to keep this path honest.
- Build on older distros: the adapter now pulls fcitx5 headers in as *system* includes
  (`dependency(..., include_type: 'system')`) and the project moved from `warning_level=3` to
  `warning_level=2`. Distro-packaged fcitx5 is not `-Wpedantic`-clean — `FCITX_CONFIGURATION` and
  `FCITX_DECLARE_PRIVATE` leave a class-scope semicolon, which GCC reported against our own lines
  on Ubuntu 24.04 while 5.1.19 was fine. `-Wall -Wextra -Werror` still applies, so a new warning in
  Nextless' code still fails CI; `-Wpedantic` is documented as deliberately off, not silently gone.
- README/README.zh: removed the claim that Debian ships a `libfcitx5-dev` meta package — that
  package name does not exist in either archive, only the four split `-dev` packages do.
- README/README.zh: unit-test count corrected to 12, and the model-download block now creates
  `~/.local/share/nextless/sherpa-onnx/` before copying into it.
- The strict-build note now reflects reality: the `-Wunused-result` in the curl-cancellation test
  helper is fixed, so `-Dwerror=true` builds clean on GCC 15 and CI keeps it that way.

## [0.2.0] - 2026-10-05

First release under the Nextless name. Forked from [xander-lin/vinput](https://github.com/xander-lin/vinput) (MIT);
the audio pipeline, provider abstraction and the recording/commit ordering design come from upstream.

### Added
- **Configurable push-to-talk key.** The trigger moved out of the source and into an fcitx5
  `KeyListOption` (`Hotkey`), editable in `fcitx5-configtool` or
  `~/.config/fcitx5/conf/nextless.conf`. Bare modifiers are allowed — including Right Ctrl,
  which is what most dictation-on-Linux setups want and what GNOME global shortcuts cannot bind.
- `tools/uinput_key.cpp`: injects a synthetic key hold so push-to-talk can be self-tested
  without a human pressing anything.
- Silence is now a no-op. Providers report `"<Backend>: no speech"`, the adapter clears the input
  panel and commits nothing, instead of showing a recognition failure. FireRed's `<sil>` marker
  can no longer reach the document.
- Notification sounds ship with the package (`sounds/*.wav`, installed to
  `/usr/share/nextless/sounds`); `tools/gen_sounds.py` regenerates them.
- DeepFilterNet3 denoiser made actually usable: bounded subprocess, and a fallback to speexdsp
  when the model or binary is missing, instead of silently producing nothing.

### Changed
- Renamed the project vinput → Nextless throughout: add-on and library (`nextless.so`),
  manifest, C++ namespace, diagnostics macro, config `~/.config/nextless`, data
  `~/.local/share/nextless`, fcitx5 option page, man page, systemd units, `PKGBUILD`.
  Model directories move with the rename — see the migration notes in `README.md`.
- The `/dev/uinput` virtual keyboard is only created when the trigger key is a locking key
  (CapsLock). For any other key nothing is injected and no key event is swallowed, so the key
  keeps working as a normal modifier while you type.
- Provider/denoiser switching is `Shift + trigger` + arrows (`Ctrl+CapsLock` still works when the
  trigger is CapsLock).
- Local ASR thread count is derived from the CPU and capped at 12 instead of a hardcoded 30
  (`defaultAsrThreads()`); measured zipformer RTF 0.13 @ 8 threads, FireRed 0.38 @ 12 threads.
- Default Zipformer model directory now points at an archive that exists upstream
  (`sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20`). Both READMEs previously pointed
  at `sherpa-onnx-zipformer-zh-en-2023-06-26`, which is not published.
- README rewritten (English) and mirrored in Chinese; man page documents the configurable trigger.

### Fixed
- The build is warning-free with `-Dwerror=true` on GCC 15, so a fresh warning now fails CI
  instead of failing the build for everyone else.
- Removed the stale `.SRCINFO` (it is generated at AUR submit time) and stopped tracking `.idea/`.

## [0.1.0] — upstream `vinput`

Everything before the fork: PulseAudio capture with EBU R128 loudness normalisation, speexdsp /
DeepFilterNet denoising, VAD trimming with a retained audio tail, five ASR backends
(zipformer, fire_red, doubao, qwen, mock), a three-deep recognition queue that preserves capture
and commit order per input context, and status feedback in the input panel instead of the document.

[Unreleased]: https://github.com/RevolutionLA/nextless/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/RevolutionLA/nextless/releases/tag/v0.2.0
[0.1.0]: https://github.com/xander-lin/vinput
