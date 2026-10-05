# Nextless

**Push-to-talk voice input for fcitx5 — hold a key, speak, release, and the text lands at your cursor.**

Nextless is a [Typeless](https://www.typeless.io)-style dictation tool for Linux desktops
(Wayland *and* X11), built as a native fcitx5 add-on. It is a fork of
[xander-lin/vinput](https://github.com/xander-lin/vinput) reworked around one idea:
**the trigger key is yours to choose** — including bare modifiers like Right Ctrl —
and nothing else about your keyboard behaviour changes.

[中文说明](README.zh.md) · [Roadmap](#roadmap) · [Attribution](#attribution)

```
   hold Right Ctrl        speak         release
  ─────────────────────────────────────────────────▶  your text, at the cursor
```

## Why this exists

Linux has plenty of speech-to-text, but very little *dictation that stays out of your way*:

| | Typeless / Wispr Flow | [openless](https://github.com/Open-Less/openless) | Electron dictation apps | **Nextless** |
|---|---|---|---|---|
| Platforms | macOS / Windows | macOS / Windows | cross-platform | **Linux (Wayland + X11)** |
| Trigger | configurable | configurable | global shortcut, no left/right split | **any key, incl. Right Ctrl / Alt / Super** |
| Works in every app | yes | yes | via simulated typing | **yes — it *is* the input method** |
| Offline backend | no | no | some | **yes (sherpa-onnx)** |
| Background daemon | yes | yes | yes | **no** |
| Audio normalisation + denoise + VAD | yes | varies | varies | **yes** |

Because Nextless is an fcitx5 module, it commits text through the input-method path your
browser, editor and terminal already use. No accessibility permission, no `xdotool`, no
clipboard round-trip, no focus stealing.

## Features

- **Configurable push-to-talk key.** `fcitx5-configtool → Addons → Nextless → Hotkey`, or edit
  `~/.config/fcitx5/conf/nextless.conf`. Bare modifiers are allowed (Left/Right Ctrl, Alt, Shift,
  Super) and combos (Ctrl+Alt+A) work too. A short tap is ignored, so the key keeps typing
  normally; holding it for 300 ms (tunable) starts dictation.
- **CapsLock still supported, harmlessly.** If you pick CapsLock, Nextless re-releases it through
  `/dev/uinput` so the lock state never flips. For any other key no virtual keyboard is created
  at all.
- **Five ASR backends, one hot-swap gesture.** Two fully offline (Zipformer bilingual, FireRed
  ASR2 int8) via [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx), two cloud (Doubao
  Streaming ASR 2.0, Qwen3-ASR) and a mock for testing. Switch with Shift+trigger and ←/→;
  switch denoiser with ↑/↓.
- **A real audio pipeline**, not a raw mic dump: PulseAudio capture → EBU R128 loudness
  normalisation (−16 LUFS) → speexdsp or DeepFilterNet denoising → VAD silence trimming with a
  retained tail so the last syllable is never clipped.
- **Ordering you can trust.** You can start the next dictation while the previous one is still
  being recognised. Each result is bound to the window and input context that was focused when
  that recording started, and commits happen in recording order (up to three in flight).
- **Errors stay out of your document.** Network, service, timeout and microphone failures appear
  as input-panel status text, never as inserted characters. Silence is a no-op: hold the key, say
  nothing, release — nothing is committed and nothing is reported.

## Limitations (read before you install)

- **No punctuation from the local models.** Zipformer and FireRed return bare text. Use the
  Doubao backend for automatic punctuation, or wait for the ct-transformer integration in
  [Roadmap](#roadmap).
- **Chinese/English mixed speech** is workable on Zipformer bilingual but imperfect on dialect
  and proper nouns; FireRed is noticeably better and slower.
- **Local models are not shipped.** You download them yourself (~360 MB or ~1.2 GB, see below).
- **No `.deb` yet**, and `fcitx5-nextless-git` is not in the AUR — the `PKGBUILD` is provided,
  packaging issues are tracked in [Roadmap](#roadmap).

## Requirements

- Linux, fcitx5 **≥ 5.1**, PulseAudio (PipeWire's Pulse compatibility is fine — tested on PipeWire 1.x)
- Any desktop: GNOME / KDE / Hyprland, Wayland or X11. A working microphone input.
- Dictation languages follow the model you pick: the bundled defaults cover Chinese, English and
  zh-en mixed speech.

## Install

### 1. Build

```bash
# Debian / Ubuntu (package names verified on Ubuntu 26.04)
sudo apt install -y g++ meson ninja-build git \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev fcitx5-modules-dev \
  libpulse-dev libebur128-dev libcurl4-openssl-dev libspeexdsp-dev libsoxr-dev
# Debian ships the four fcitx5 dev packages above as one meta package: sudo apt install libfcitx5-dev
# Fedora: dnf install gcc-c++ meson ninja-build pkgconf-pkg-config fcitx5-devel libpulse-devel \
#         libebur128-devel libcurl-devel speexdsp-devel soxr-devel   (not CI-verified yet)

git clone https://github.com/RevolutionLA/nextless.git
cd nextless
meson setup build --prefix=/usr --buildtype=release
ninja -C build
sudo meson install -C build
```

> On GCC 15 the strict default (`-Dwerror=true`) can trip on a `-Wunused-result` in a test
> helper. Either `meson configure build -Dwerror=false` or build with `-Dbuildtype=debugoptimized`.

Arch users can build the provided `PKGBUILD` instead: `makepkg -si` (produces
`fcitx5-nextless-git`).

### 2. Load the add-on

The manifest installs to `/usr/share/fcitx5/addon/nextless.conf` with `OnDemand=False`, so
restarting fcitx5 is enough — no need to add Nextless to an input-method list, it is a module,
not a method.

```bash
fcitx5 -r -d          # replace the running instance; or log out and back in
```

### 3. Pick your key

`fcitx5-configtool → Addons → Nextless → Push-to-talk key`, or:

```bash
mkdir -p ~/.config/fcitx5/conf
cat > ~/.config/fcitx5/conf/nextless.conf <<'EOF'
[Hotkey]
0=Control_R

DefaultProvider=zipformer
EOF
fcitx5 -r -d
```

### 4. Get models (local backends)

```bash
mkdir -p ~/.local/share/nextless
cd /tmp

# sherpa-onnx runtime (shared build: copy bin/ *and* lib/ — the binaries use rpath $ORIGIN/../lib)
V=1.13.8
curl -LO "https://github.com/k2-fsa/sherpa-onnx/releases/download/v${V}/sherpa-onnx-v${V}-linux-x64-shared.tar.bz2"
tar xf "sherpa-onnx-v${V}-linux-x64-shared.tar.bz2"
cp -r "sherpa-onnx-v${V}-linux-x64-shared/"{bin,lib} ~/.local/share/nextless/sherpa-onnx/

# ASR models land under ~/.local/share/nextless/models/<archive-dir>/
M=https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models
mkdir -p ~/.local/share/nextless/models

# Zipformer bilingual zh-en (streaming transducer, ~360 MB of fp32 files)
curl -LO "$M/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2"
tar xf sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2 -C ~/.local/share/nextless/models/

# FireRed ASR2 int8 (offline AED, ~1.2 GB, more accurate, slower)
curl -LO "$M/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2"
tar xf sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2 -C ~/.local/share/nextless/models/

rm -f sherpa-onnx-*.tar.bz2
```

Both extractions create the directory names the built-in defaults expect, so there is nothing to
rename. Different paths can be set in `~/.config/nextless/advanced.json`.

Cloud backends need no models — just credentials:

```bash
mkdir -p ~/.config/nextless
echo '{"api_key":"<your Volcengine APP Key>","resource_id":"volc.seedasr.auc"}' > ~/.config/nextless/doubao.json
echo '{"api_key":"<sk-...>"}' > ~/.config/nextless/qwen.json
chmod 600 ~/.config/nextless/*.json
```

## Usage

| Action | Keys |
|---|---|
| Dictate | hold **your trigger key** → speak → release |
| Switch ASR backend | hold **Shift + trigger**, then **← / →** |
| Switch denoiser | hold **Shift + trigger**, then **↑ / ↓** |

While dictating, the input panel beside the cursor shows `listening → processing audio →
recognizing → text`. Releasing the key returns control to fcitx5 immediately; recognition runs
asynchronously.

## Backends

| Provider | Type | Disk | Model | Notes |
|---|---|---|---|---|
| `zipformer` | local | ~360 MB | `sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20` | RTF ≈ 0.13 on an i7-1260P @ 8 threads, ~1 GB RSS, no punctuation |
| `fire_red` | local | ~1.2 GB | `sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26` | RTF ≈ 0.38 @ 12 threads, peak ~1.7 GB RSS, clearly better on dialects and long sentences |
| `doubao` | cloud | — | Doubao Streaming ASR 2.0 | punctuation, ITN and hotwords; needs an API key |
| `qwen` | cloud | — | Qwen3-ASR-Flash | needs an API key |
| `mock` | test | — | — | returns `hello world`; useful for verifying the key/capture path |

Both local backends spawn one `sherpa-onnx` process per utterance on purpose: no resident model
server, no orphan process, no warm-up state to leak. (The `systemd/` units in this repo are an
experimental resident-server path and are **not** required.)

## Configuration

| File | Purpose |
|---|---|
| `~/.config/fcitx5/conf/nextless.conf` | trigger key (`[Hotkey]`), `DefaultProvider` |
| `~/.config/nextless/nextless.json` | `activation_msec` (default 300), notification timeout, debounce |
| `~/.config/nextless/audio.json` | `denoise`: `none` \| `speexdsp` \| `deepfilter` |
| `~/.config/nextless/advanced.json` | model dirs, `num_threads`, timeouts, LUFS target, VAD thresholds |
| `~/.config/nextless/doubao.json` / `qwen.json` | API credentials |
| `~/.config/nextless/pa_buffer.json` | auto-detected PulseAudio buffer size (self-generated) |

Copy the `config/*.json.example` files as a starting point. User files win over `/etc/nextless/`
(the packaged defaults), and missing user files are created from `/etc/nextless/` on first read.

## Development

```bash
meson setup build --buildtype=debug -Dwerror=false
ninja -C build
meson test -C build            # 9 unit tests: registry, config fallback, capture, queue, cancellation
```

- `docs/nextless/` — design notes: interaction model, ASR provider API, failure analysis.
- `docs/fcitx5/` — a short primer on the fcitx5 add-on and configuration system, since Nextless
  hooks key events at `PreInputMethod` phase.
- `FINDINGS.md` — the measurement log behind the tuning numbers (buffer sizes, model latency,
  denoiser comparisons). Kept because it explains *why* defaults are what they are.
- `tools/` — standalone harnesses: `record_test`, `calibrate_silence`, `tail_loss_test`,
  `uinput_key` (inject a synthetic key hold so you can self-test push-to-talk without typing).

Diagnostics (one JSON object per line, no audio and no recognised text) are opt-in:

```bash
meson configure build -Ddiagnostic_logging=true && ninja -C build
~/.local/share/nextless/diagnostic.log
```

## Attribution

Nextless is a fork of **[vinput](https://github.com/xander-lin/vinput)** by xander-lin (MIT),
which it thanks for the audio pipeline, provider abstraction and the ordering/queue design.
The fork adds:

- `feat(hotkey)`: trigger key moved from a hardcoded `CapsLock` to an fcitx5 `KeyListOption`,
  default Right Ctrl, with `/dev/uinput` revert enabled only when the trigger is a locking key
- rename to Nextless: add-on/library/config/data paths (`~/.config/nextless`,
  `~/.local/share/nextless`)
- `fix(defaults)`: local ASR thread count derived from the CPU instead of a hardcoded 30, and the
  default Zipformer model directory now matches the real sherpa-onnx archive name

License: **MIT** — see [LICENSE](LICENSE), which retains the upstream copyright notice.

## Roadmap

- [ ] Local punctuation: integrate `sherpa-onnx-offline-punctuation` (ct-transformer)
- [ ] Hotwords / custom phrases for the local backends
- [ ] First-run wizard that downloads the right sherpa-onnx build (x86_64 / aarch64)
- [ ] `.deb` + CI (build × test matrix, so `-Dwerror` surprises surface before release)
- [ ] A/B benchmark harness publishing CER / latency / RTF / RSS per model
- [x] Silence is a no-op — no `ASR error: empty result`, and FireRed's `<sil>` never reaches the document
- [ ] Ship the notification sounds the code already tries to play
