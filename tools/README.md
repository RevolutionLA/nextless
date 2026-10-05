# Tools

This directory contains manual diagnostics and integration helpers. They are not part of the default automated test suite.

## Automated Tests

- `tests/test_asr_registry.cpp` verifies provider registration and the `mock` provider contract.
- `tests/test_audio_capture_file.cpp` verifies deterministic audio preprocessing and WAV writing without hardware.

Run automated tests with:

```bash
meson test -C build
```

## Manual Diagnostics

### `test_audio_file`

Process a WAV file through the audio pipeline without recording from the microphone.

```bash
./build/tools/test_audio_file input.wav output.wav [none|speexdsp|deepfilter]
```

Use this to check normalization, denoise, VAD trim, and WAV output on a known file.

### `test_audio_pipeline`

Record from PulseAudio and run the full capture pipeline.

```bash
./build/tools/test_audio_pipeline output.wav [duration_sec=0] [denoiser=none|speexdsp|deepfilter]
```

Use this to validate the live microphone path, buffer detection, and pipeline timing.

### `tail_loss_test`

Legacy tail-loss diagnosis utility for checking stop timing and long-read behavior on specific capture devices.

### `record_test`, `record_timing`, `calibrate_silence`

Additional capture diagnostics used during audio troubleshooting.

### `uinput_key`

Inject a synthetic key press/hold/release through `/dev/uinput`, so push-to-talk can be self-tested without typing.

```bash
g++ -O1 tools/uinput_key.cpp -o /tmp/uinput_key && /tmp/uinput_key 97 1200
```

`97` is `KEY_RIGHTCTRL`; hold long enough (default activation threshold 300 ms) to start a recording.

### `gen_sounds.py`

Regenerate the bundled notification sounds into `sounds/` after changing pitch, length or level. The generated files are committed — Meson installs them and the adapter plays the first of `paplay` / `pw-play` it finds.

```bash
python3 tools/gen_sounds.py
```

Put a file of the same name in `~/.local/share/nextless/sounds/` to override one sound without rebuilding.

## Rule of Thumb

If a tool needs a microphone, a compositor, or an external binary/model, keep it here as a manual helper. If it can run without hardware or network, move it into `tests/` and register it with Meson `test()`.
