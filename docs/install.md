# Install, upgrade and remove Nextless

> 从 README 抽出的完整安装文档：三种安装路线、模型下载、升级、卸载与「装完了却没生效」的排查。
> 只想尽快用起来，看 README 的 [Install](../README.md#install) 摘要节即可。


### 0. Pick your route

**Arch** — build the provided `PKGBUILD`: `makepkg -si` (produces `fcitx5-nextless-git`).

**Debian / Ubuntu** — one command; it downloads the `.deb` from the latest GitHub
Release, verifies the sha256, and lets apt resolve every dependency (fcitx5 included):

```bash
curl -fsSL https://raw.githubusercontent.com/RevolutionLA/nextless/main/tools/install-deb.sh | sudo bash
```

The script ends by printing the two steps it will not do for you: `nextless-get-models
--zipformer --punctuation` (models are deliberately not in the .deb) and `fcitx5 -r -d`.
Before installing it stops if it cannot match the `.deb` against the release's
`sha256sums.txt` — an unreachable digest file or an unlisted asset is what a tampered
publish looks like, so that path needs an explicit opt-in:
`... | sudo bash -s -- --allow-unverified`. amd64 only; other architectures build the
package themselves:

```bash
sudo apt install -y build-essential dpkg-dev debhelper git
git clone https://github.com/RevolutionLA/nextless.git
cd nextless
sudo apt build-dep ./          # the dependency list comes from debian/control, not from this README
dpkg-buildpackage -us -uc -b   # without -d, so that list is checked before building
sudo apt install ../fcitx5-nextless_*.deb && fcitx5 -r -d
```

### 1. Build

```bash
# Debian / Ubuntu
sudo apt install -y g++ meson ninja-build git \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev fcitx5-modules-dev \
  libpulse-dev libebur128-dev libcurl4-openssl-dev libspeexdsp-dev libsoxr-dev
# Package names above verified with apt-cache on Ubuntu 26.04. There is no libfcitx5-dev
# meta package in the Ubuntu archive; Debian testing/sid names look the same but are
# unverified — please open an issue if they differ.
# Fedora: dnf install gcc-c++ meson ninja-build pkgconf-pkg-config fcitx5-devel libpulse-devel \
#         libebur128-devel libcurl-devel speexdsp-devel soxr-devel   (not CI-verified yet)

git clone https://github.com/RevolutionLA/nextless.git
cd nextless
meson setup build --prefix=/usr --buildtype=release
ninja -C build
sudo meson install -C build
```

> `meson.build` sets `warning_level=2` with `werror=true`, and CI builds with exactly that, so any
> new `-Wall`/`-Wextra` warning fails the build. Locally you can drop it with
> `meson configure build -Dwerror=false` while experimenting. `-Wpedantic` is deliberately *not*
> enabled: fcitx5's own `FCITX_CONFIGURATION` / `FCITX_DECLARE_PRIVATE` macros expand to an
> "extra `;`" that GCC flags on the fcitx5 versions distros package (and the fcitx5 headers are
> pulled in as system includes for the same reason). That is not our bug to fix.

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
nextless-get-models
```

That is the whole step. The helper (installed next to the addon; `tools/get_models.sh`
in the repo if you want to run it before installing) detects the CPU architecture, shows
the download size before fetching anything, and lets you pick Zipformer, FireRed, or both
(`--backend=...`; with no terminal it asks for that flag instead of assuming you wanted
the biggest option). It writes only inside `~/.local/share/nextless/`, is safe to re-run —
already-installed pieces are skipped — **checks every archive against a pinned sha256
before extracting it**, and only moves an archive into place after verifying the extracted
directory names are the ones `advanced.json` expects, so a corrupt or substituted download
leaves no half-extracted model behind. `--dry-run` prints the plan without touching
anything, and `--punctuation` also fetches the optional ~62 MB ct-transformer model that
gives the local backends ，。？ (an interactive run asks; see [Limitations](../README.md#limitations-read-before-you-install)) —
it refuses to proceed if the pinned sherpa-onnx runtime stops shipping the punctuation
binary, rather than installing a config that silently no-ops.

Manual path, if you would rather do it yourself:

```bash
mkdir -p ~/.local/share/nextless/sherpa-onnx ~/.local/share/nextless/models
cd /tmp

# sherpa-onnx runtime (shared build: copy bin/ *and* lib/ — the binaries use rpath $ORIGIN/../lib)
# aarch64: the asset name is ...-linux-aarch64-shared-cpu.tar.bz2 (plain -shared is a 404)
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

If the runtime or a model file is missing when you dictate, the input panel says which piece is
missing and tells you to run `nextless-get-models` — instead of a bare `spawn failed` /
`recognition failed`. The exact path it looked for goes to the fcitx5 log.

Cloud backends need no models — just credentials:

```bash
mkdir -p ~/.config/nextless
echo '{"api_key":"<your Volcengine APP Key>","resource_id":"volc.seedasr.auc"}' > ~/.config/nextless/doubao.json
echo '{"api_key":"<sk-...>"}' > ~/.config/nextless/qwen.json
chmod 600 ~/.config/nextless/*.json
```

### 5. Optional: DeepFilterNet3 denoiser

speexdsp ships with the build, and it is enough for a quiet room. For a noisy one you can hand
Nextless the `deep-filter` CLI from
[DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) — it is deliberately *not* a build
dependency, because it is a separate Rust binary carrying its own model:

```bash
mkdir -p ~/.local/share/nextless/bin
# whichever you have: a `pip install deepfilternet` binary on PATH, or the
# release asset - the x86_64 musl one is a single 36 MB static binary with the
# model compiled in (no tarball; `deep-filter --version` prints deep_filter 0.5.6),
# so `chmod +x` it and symlink that path directly instead of using command -v.
ln -s "$(command -v deep-filter)" ~/.local/share/nextless/bin/deep-filter
```

Any location works as long as you point at it, either with the path above or with
`NEXTLESS_DEEP_FILTER=/usr/bin/deep-filter` in the fcitx5 environment. Turn it on with
Shift+trigger and ↑/↓ (persisted to `~/.config/nextless/audio.json`).

Nextless resamples to 48 kHz for the model and back to 16 kHz for the recogniser. Because the
denoiser is an external process, it is treated as untrusted: if the binary is missing, exits with
an error, wedges (killed after 5 s) or hands the audio back unchanged, that utterance falls back
to speexdsp instead of losing your words — and after three failures in a row DeepFilterNet is
switched off for the rest of the session. `meson test` covers all four cases.

What it costs, measured with the real binary on a 2022 i7-1260P
([docs/benchmarks.md](benchmarks.md)): **≈1.0–1.3 s per utterance** of 5–10 s speech
(RTF 0.13–0.16 across three runs, ~60 MB) versus single-digit milliseconds for speexdsp. The
process is spawned per utterance, so that second lands between releasing the key and the first
character, and the 5 s deadline stays clear up to well past 30 s of continuous speech. It is also **not** lossless
in the way the bullet above promises: deep-filter hands back 30 ms less than it receives, so
Nextless appends the last 30 ms of your *raw* audio to keep the utterance whole — those final
30 ms are un-denoised, but nothing is cut off
([issue #61](https://github.com/RevolutionLA/nextless/issues/61), fixed; before that the round trip
dropped 69–97 ms off the end). `speexdsp` stays the default because it costs single-digit
milliseconds rather than a second per utterance and never touches the audio length.

### 6. Rolling back

`apt remove fcitx5-nextless` does not touch fcitx5 itself, so your other input methods
(typing-side IMEs, pinyin) stay exactly where they were. One caveat: if nothing else on the
machine depends on fcitx5, a later `apt autoremove` can offer to take it away too - read the
list before saying yes.

**Installed from the `.deb`** - the three paths named below are exactly what CI's
`.deb installs clean` job asserts are gone after removal, so this is not a guess:

```bash
sudo apt remove fcitx5-nextless    # the addon .so, nextless-get-models and nextless.conf go
sudo apt purge fcitx5-nextless     # ...and the /etc/nextless defaults too
fcitx5 -r -d                       # reload without it
```

`remove` deliberately leaves the `/etc/nextless/*.json` conffiles behind (dpkg reports
`deinstall ok config-files`), which is what you want if you mean to reinstall.

To go back to an older release instead - the reason `install-deb.sh` exists is that a new
version can be the broken one:

```bash
sudo dpkg -i fcitx5-nextless_0.3.1-1_amd64.deb   # exactly this version, downgrade included
```

**Installed with `meson install`** - meson has no uninstall, but it wrote the manifest for you:

```bash
grep -v '^#' build/meson-logs/install-log.txt | sudo xargs -d '\n' rm -f --
```

Run that from the checkout and build directory you installed *from*: the log holds absolute
paths recorded at install time, so if you reconfigured with a different `--prefix` since, it no
longer describes your system. It removes files, not the empty directories they leave behind.

**Neither route touches anything in your home**, because those are your files, not the package's:

| path | what lives there | size on a typical box |
|---|---|---|
| `~/.config/nextless/` | your settings, **including any API keys** | ~30 KB |
| `~/.local/share/nextless/models/` | the downloaded ASR + punctuation models | ~1.8 GB |
| `~/.local/share/nextless/bin/deep-filter` | the optional denoiser, if you installed it | ~35 MB |
| `~/.config/autostart/org.fcitx.Fcitx5.desktop` | fcitx5 autostart, only if you added it | 1 KB |

Delete `~/.config/nextless/` before handing a machine over. Deleting the models directory is the
only way to get the 1.8 GB back, and `nextless-get-models` will re-download whatever is missing.

**Check what is actually loaded**, because a stale install is otherwise invisible:

```bash
fcitx5-diagnose | grep -i nextless     # e.g. "Nextless 10.12.0"
```

If that prints an older version than the release you installed, the running addon is the old one -
either the install step did not run, or fcitx5 was never restarted. That is how a machine already
bumped to 0.3.x kept advertising `Nextless 0.2.0`.
