# Changelog

All notable changes to Nextless are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), and this project keeps one line per
user-visible change rather than one per commit.

## [Unreleased]

### Added
- `nextless-get-models` now **verifies every archive before extracting it**
  (issue #44). The runtime (x86_64 and aarch64), Zipformer, FireRed and the
  punctuation model each carry a pinned sha256 in the script; a mismatch aborts
  with nothing moved into place and no staging left behind. The pin list is the
  same one-line-per-version maintenance the URL pins already are, and the digests
  were produced by streaming each asset from upstream and hashing it rather than
  copied from a page. Trust model written into SECURITY.md: these are pins, not
  signatures - they catch corruption and a substituted asset, not a legitimately
  re-published upstream.
- The local punctuator now gives up for the session after **three consecutive
  failures** (issue #47), the same rule DeepFilterNet already follows. Before
  this, a broken, quarantined or hanging `sherpa-onnx-offline-punctuation`
  cost every single utterance the full `timeout_sec` (0.2 s healthy vs 5 s
  broken) with nothing in the panel to reveal why. The trip is logged once to
  stderr and to the diagnostics log; raw text keeps flowing either way.

### Changed
- Roadmap rows moved with the work (issue #48): the long-term list splits
  `.deb packaging` (done, #34 + #38) from "an apt repository" and "AUR" (both
  open, tracked as #35), and ordered-table row 4 now names what landed and what
  its own finish line still demands - dictation on a clean Ubuntu VM and a
  rollback paragraph - instead of reading as if nothing had shipped. Both
  READMEs, same as every previous row. No behaviour changed; the table's "each
  item shows who fixed it" convention now holds where the most visible
  milestone lives.
- `nextless-get-models` no longer installs **both** models (~1.8 GB) just because
  nobody was at the keyboard (issue #45). With no terminal and no `--backend` it
  now stops before touching the network and prints the three explicit choices
  with their sizes; `--backend=both -y` (or any `--backend=…`) is the scripted
  route, and `--dry-run` still works headless. The interactive picker is
  unchanged, and `install-deb.sh` already printed an explicit
  `--zipformer --punctuation` line, so the documented first-run path is
  unaffected.

### Fixed
- `--punctuation` can no longer install "successfully" on a runtime that cannot
  punctuate (issue #44). `runtime_present()` looked for `bin/sherpa-onnx` and
  `bin/sherpa-onnx-offline` only, so `sherpa-onnx-offline-punctuation` - the
  binary the punctuator actually spawns - was never checked by the installer or
  the contract test. A version bump that drops or renames it would have left the
  post-processing silently no-oping (`skipped_missing_pieces`, log-only) with the
  model downloaded right next to it. `install_runtime` now asserts the binary is
  in the freshly extracted runtime when `--punctuation` was asked, refuses loudly
  if not, and treats an installed runtime without it as *not* ready so the next
  run re-fetches instead of skipping forever.
- The startup sweep no longer deletes a **live recording of another session**
  (issue #42). With `XDG_RUNTIME_DIR` set the capture dir is one fixed path
  shared by every process of the user (`/run/user/<uid>/nextless`), and the
  constructor swept it unconditionally - so a restart race, two graphical
  sessions, or a developer running the test suite while dictating could delete
  the WAV another session was still writing, which surfaced to that user as
  "failed to read WAV". Ownership now comes from the pid already in the file
  name (`nextless_cap_<pid>_<n>.wav`): alive owner = hands off, dead owner =
  crash residue, collected immediately. The 10-minute age guard stays as the
  fallback for names without a parseable pid and for reused pids, and one rule
  now governs both the current dir and the `/tmp` siblings (they had diverged:
  the unpredictable fallback dirs were guarded, the shared fixed-name one - the
  only place concurrency can actually happen - was not).
- `tests/test_capture_dir_sweep.cpp` covers the shared-dir case for real (issue
  #42). It could not: it forced the `/tmp` fallback branch only, and its
  fixtures carried a hard-coded pid 9999 that is a dead pid on any machine. The
  file name's pid is now load-bearing in the test - the suite forks a stand-in
  "other session" and a provably-dead one - and the XDG branch runs in exec'd
  phases (both the capture dir and the sweep are once-per-process), asserting
  five outcomes: live owner kept, dead owner collected, unparseable fresh kept,
  unparseable ancient collected, and an alive owner's ancient file still
  collected. Both directions were mutation-tested: sweeping unconditionally
  fails the live-session checks, never sweeping the current dir fails the
  owner-died phase.
- `tools/install-deb.sh` now **fails closed when it cannot verify the digest**
  (issue #43). Previously a missing `sha256sums.txt` or an asset not listed in it
  printed a warning and installed anyway; both are exactly what a tampered or
  half-replaced publish looks like, so they now stop, and continuing requires an
  explicit `--allow-unverified` / `ALLOW_UNVERIFIED=1`. The old path could also
  print "sha256 verified" for an asset it had never checked. Trust model stated in
  SECURITY.md: the checksums ship on the same release, so this is TOFU.
- Several of the installer's own error messages were unreachable. Under
  `set -euo pipefail` a `grep` that found nothing made the enclosing assignment
  exit the script silently, killing the "could not resolve latest release",
  "no amd64 .deb asset" and "asset not listed" branches. Those greps are now
  guarded, so the script stops where it intends to stop and says why.
- The `Release` workflow is triggered by `v*` tags only (issue #46). Its
  `workflow_dispatch` entry was broken by construction: dispatching from a branch
  sets `GITHUB_REF_NAME` to that branch, so the tag-vs-`debian/changelog` gate
  could only ever fail. Manual dry-runs belong on a branch, not on the release
  contract.
- New contract test `tests/test_install_deb.sh` runs the shipped installer against
  stubbed `curl`/`id`/`apt-get`/`dpkg-query` (20 tests): verified install, tampered
  asset, wrong digest, unlisted asset, unreachable sums file, release without
  checksums, both opt-ins, the API-403 redirect fallback (still verified, and still
  refusing when it cannot verify), and non-root. Reverting the digest gate fails the
  suite; it is not an assertion that can silently rot.
- `debian/control` now declares everything the build actually needs (issue #41).
  `Fcitx5Module`, `Fcitx5Config` and `Fcitx5Utils` live in `fcitx5-modules-dev`,
  `libfcitx5config-dev` and `libfcitx5utils-dev` - not in `libfcitx5core-dev`, which
  was the only one listed; and since `dh_auto_test` runs the suite, `python3` and
  `bzip2` are build dependencies as well. `apt build-dep`, sbuild and pbuilder all
  failed at meson configure while CI stayed green, because the CI and Release jobs
  typed the list out by hand and passed `-d` to skip the dependency check. Both jobs
  now install the toolchain, resolve the rest with `apt-get build-dep ./`, and build
  without `-d`, so a dependency added in `meson.build` but not declared in control
  breaks here exactly the way it breaks a sponsor's build. README/CONTRIBUTING point
  at `sudo apt build-dep ./` rather than carrying a fourth copy of the list.
- `tests/test_punctuator.cpp` now proves the timeout path really kills the
  process group (issue #47). It only asserted "timed out, raw text kept", so
  deleting `kill(-pid, SIGKILL)` left the suite green while production leaked a
  process per utterance. The fake binary now ignores SIGTERM - ignored
  dispositions survive fork+exec, so only the escalation can clear it - records
  its grandchild's pid, and the test checks that pid is gone from /proc *and*
  that `runPunctuation` came back in time instead of waiting the child out.
  Both mutations were run: dropping the SIGKILL and signalling the leader
  instead of the group each fail the suite now. A final case drives three
  consecutive failures and asserts the punctuator stays disabled and cheap.

## [0.3.0] - 2026-10-09

### Added
- Full settings panel (the "one panel, every option" ask): the fcitx5 add-on
  configuration now carries grouped options for behavior/timing, denoise +
  loudness, local engines (threads, timeouts, model dirs, sherpa binaries),
  punctuation and cloud credentials. Saving normalizes (clamps ints, validates
  denoiser/provider ids, parses the two float fields), **patches the JSON files
  in place** via a new `json_patch` writer that preserves any key the panel does
  not own, and pushes new values into the running provider — every provider's
  `setConfig` grew the keys its panel fields map to, so changes take effect on
  the next utterance without restarting fcitx5. Opening the panel seeds from the
  files (JSON stays the source of truth; hand-edits remain supported). api-key
  writes chmod the credential files to 0600. New unit test round-trips the
  patcher through the same extractors the providers use (19 tests).
- One-command install (finishes the promise behind issue #7): `v*` tags trigger a
  `Release` workflow that rebuilds the `.deb` on Ubuntu 24.04, refuses to publish when
  the tag and `debian/changelog` disagree, and attaches the package plus `sha256sums.txt`
  to a GitHub Release. `tools/install-deb.sh` consumes exactly that: verify checksum,
  `apt install` the deb (the package now hard-depends on `fcitx5`, so apt pulls
  everything from the distro archive), then print the two first-run steps it refuses to
  do silently (model download, `fcitx5 -r -d`). Anonymous `api.github.com` rate limits
  are normal behind shared NAT, so resolution falls back to the `/releases/latest`
  redirect + the deterministic asset naming the Release workflow enforces; checksums are
  compared as digests (the asset lands as `install.deb`, so `sha256sum -c` by filename
  would not match). README (en/zh) lead with the one-liner;
  building the package yourself stays documented for non-amd64.
- Reproducible A/B benchmark harness (issue #8): `tools/benchmark/` — `bench_asr`
  drives each backend through the **exact add-on code path** (`AsrProviderRegistry`
  + `transcribe(samples, wavPath)`), `cer.py` implements a documented zh/en
  normaliser (NFKC, letters/digits only, CJK = one token per char; empty references
  are timed but never scored), and `run_bench.sh` prints one machine-attributable
  table: first-token latency, warm RTF, cold wall, and peak RSS **including the
  per-request inference child** via `RUSAGE_CHILDREN` (the harness process alone
  would report ~20 MB and hide the gigabyte the models actually cost). The repo
  ships no speech: `record_corpus.sh` builds a self-recorded 16 kHz corpus.
  `docs/benchmarks.md` carries the first published run (i7-1260P, Apache-2.0 demo
  wavs, timing-only — CER awaits verified transcripts) together with its raw JSONL.
- `.deb` packaging for Debian/Ubuntu (issue #7, first half): shipped `debian/` sources —
  debhelper 13 + the meson buildsystem, `Rules-Requires-Root: no`, distro layout
  (`/usr/lib/<multiarch>/fcitx5/nextless.so`, manifest in `/usr/share/fcitx5/addon`,
  six defaults in `/etc/nextless`), dependency set resolved against the real Ubuntu
  archive (there is no `libfcitx5-dev` meta package), and **no models in the payload by
  design** — `nextless-get-models` stays the first-run step. A new CI `deb-package` job
  builds the package from a clean copy, `dpkg -i`s it into a fresh runner, checks every
  scan path fcitx5 needs plus an `ldd` without "not found", then removes and purges it
  and fails on any debris — that job is the clean-environment guarantee; the local build
  was payload-checked on Ubuntu, no chroot claims. The `PKGBUILD`
  gained the missing `hotwords.json` backup entry (sixth packaged config). README (en/zh)
  move packaging from "no .deb" to an Install "pick your route" section; the man page
  gained an INSTALLING section. Remaining for #7: publishing `fcitx5-nextless-git` to the
  AUR (the PKGBUILD ships with the repo; submission is a human step on aur.archlinux.org).
- Hotwords / mishearing correction table (issue #6): `~/.config/nextless/hotwords.json`
  maps `{"what it heard": "what you meant"}` (e.g. Zipformer's habit of turning
  `礼拜二` into `LIBR`). The table post-processes the final text of **every** backend,
  local and cloud, before punctuation: single simultaneous pass, longest key wins at
  each position, replacements are never re-matched (`A→B` plus `B→C` cannot turn `A`
  into `C`). Missing, empty or malformed file = documented no-op; edits take effect
  on the next utterance without a restart (the table is cached by file content, so
  nothing is parsed per keypress). The packaged template contains only `YOUR_`
  placeholder keys — dropped at parse time — so the first-run copy can never rewrite
  real speech. `test_hotwords` covers parse/apply/bounds/caching; `hotwords.json` is
  the sixth installed config and the CI staged-install guard now checks all six.
  Per-model biasing (sherpa-onnx `--hotword` on Zipformer, issue #6 option 2) stays
  open — it needs a measured CER delta, not an assumed one.
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
