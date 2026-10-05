# Contributing to Nextless

Thanks for spending your attention here. This is a small C++20 fcitx5 add-on with one job:
**hold a key, speak, text appears at the cursor, on any Linux desktop, without a daemon.**

## Scope

Keep it. Concretely, a change is in scope when it preserves all of:

- no new mandatory background service or tray app;
- no clipboard round-trip and no simulated keystrokes for committing text (we are the input
  method; use the fcitx5 input-context path);
- the trigger key stays configurable, and choosing a non-locking key must not create a
  `/dev/uinput` device;
- recognition failures never insert text into the user's document.

Things we would love: better local accuracy, punctuation, packaging, distro coverage, and
benchmarks with real numbers.

## Getting a build running

```bash
# Debian / Ubuntu (verified on Ubuntu 26.04)
sudo apt install -y g++ meson ninja-build git \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev fcitx5-modules-dev \
  libpulse-dev libebur128-dev libcurl4-openssl-dev libspeexdsp-dev libsoxr-dev

meson setup build --buildtype=debug
ninja -C build
meson test -C build
```

`meson test` needs a running PulseAudio server, because the capture tests open a real stream. On
a container or CI runner without a desktop session, start one and add a null sink:

```bash
pulseaudio --daemonize=no --exit-idle-time=-1 &
pactl load-module module-null-sink sink_name=ci
```

`-Dwerror=true` is the project default and CI does not switch it off. If your compiler is
newer than the last CI run and a fresh warning breaks the build, **fix the warning in the
patch that touches that file** — do not disable `werror` and do not silence it with a cast.

Install locally with `sudo meson install -C build`, then `fcitx5 -r -d`.

## Testing your change

The unit suite (`meson test`) is the contract. Add a test with the change, not afterwards —
`tests/` is cheap to extend: each file is a `main()` returning non-zero on failure, registered
in `tests/meson.build`.

For anything touching keys or capture, `tools/uinput_key.cpp` injects a synthetic key hold so
you can test push-to-talk without typing:

```bash
g++ -O2 -o /tmp/uinput_key tools/uinput_key.cpp
/tmp/uinput_key 97 1200        # keycode 97 = Right Ctrl, hold 1200 ms
/tmp/uinput_key 100 80 3       # keycode 100 = Right Alt, 80 ms × 3 (must NOT trigger)
```

Then look for the round trip in the verbose log:
`Nextless [press]` → `Nextless activated` → `Nextless deactivated (record=…)` →
`Nextless ASR state: off`.

Finally, do one real dictation. A green test suite with a broken microphone path has happened.

## Where things live

| Path | What it holds |
|---|---|
| `adapter/src/nextless.cpp` | the fcitx5 add-on: key handling, timing, queue, status panel, commit |
| `ASR_provider/src/` | audio capture + pipeline, provider implementations, config helpers |
| `docs/nextless/` | design notes — read `05-voice-input-design.md` and `06-asr-provider-api.md` first |
| `docs/fcitx5/` | how the add-on and configuration system work (keys hook at `PreInputMethod`) |
| `FINDINGS.md` | measurement log; the reason most defaults are what they are |
| `tools/`, `tests/` | standalone harnesses and the unit suite |

### Adding an ASR backend

Implement `IAsrProvider` in `ASR_provider/src/`, register a factory (see the mock provider for
the smallest working example), and add the id to `DefaultProvider`. Two rules:

- when nothing was recognised, report `"<Provider>: no speech"` — the adapter treats that as a
  no-op. Anything else is shown to the user as a failure;
- never log audio, API keys, or recognised text.

## Style

C++20, 4-space indent, no exceptions across provider boundaries, and match the file you are in.
Comments are frequently Chinese in this codebase because they explain *why* a number or a
workaround exists — keep that intent, in whichever language you write.

Commits follow [Conventional Commits](https://www.conventionalcommits.org/)
(`feat:`, `fix:`, `test:`, `docs:`, `refactor:`, `chore:`) with a body that explains the
reasoning. One commit per logical change; do not mix a rename with a behaviour change.

## Pull requests

- Fill in the PR template, including the verification checklist with **real** results.
- If you change user-visible behaviour, update `README.md`, `README.zh.md`, `docs/nextless.1`
  and `CHANGELOG.md` in the same PR. The English and Chinese READMEs are edited together.
- Keep unrelated cleanups out. A separate `chore:` PR is fine and welcome.
- Never commit real credentials: `~/.config/nextless/*.json` lives outside the repository and
  `config/*.json` is gitignored on purpose — commit `config/*.json.example` instead. Check
  `git diff --cached` before every commit, and prefer `git add <paths>` over `git add -A`.

## Before you open an issue

Run the three checks the bug template asks for: a working microphone in another app, the
`mock` backend, and a restart (`fcitx5 -r -d`). They separate key handling from capture from
recognition, which is most of the debugging.
