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

### Fixed
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
- README/README.zh: unit-test count corrected to 11, and the model-download block now creates
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
