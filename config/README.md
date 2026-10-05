# Configuration Examples

This directory stores example configuration files only. Packaged defaults are installed to `/etc/nextless/`; per-user overrides live in `~/.config/nextless/`.

## Boundary

- Track `*.json.example` in Git.
- Do not track `*.json` in this directory.
- Real API keys, local paths, and user preferences belong in `/etc/nextless/*.json` or `~/.config/nextless/*.json`.
- Runtime lookup order is `~/.config/nextless/*.json` first, then `/etc/nextless/*.json`; missing user files are copied from `/etc/nextless/` on first read.
- `.gitignore` ignores `config/*.json` to reduce the chance of committing real credentials.

## Initial Setup

```bash
mkdir -p ~/.config/nextless
cp config/doubao.json.example ~/.config/nextless/doubao.json
cp config/qwen.json.example ~/.config/nextless/qwen.json
cp config/audio.json.example ~/.config/nextless/audio.json
cp config/nextless.json.example ~/.config/nextless/nextless.json
cp config/advanced.json.example ~/.config/nextless/advanced.json
```

After copying, edit the files under `~/.config/nextless/`. Do not put real keys into files under this repository.

## Files

| Example | Runtime file | Purpose |
|---------|--------------|---------|
| `doubao.json.example` | `~/.config/nextless/doubao.json` | Doubao API key and resource ID |
| `qwen.json.example` | `~/.config/nextless/qwen.json` | Alibaba DashScope API key |
| `audio.json.example` | `~/.config/nextless/audio.json` | Active denoiser: `none`, `speexdsp`, or `deepfilter` |
| `nextless.json.example` | `~/.config/nextless/nextless.json` | Activation delay, notifications, CapsLock debounce |
| `advanced.json.example` | `~/.config/nextless/advanced.json` | Optional model paths, timeouts, thread counts, and audio tuning |

## Update Rule

When adding a new runtime config key, update the relevant `.json.example`, this README, and the user-facing configuration section in `README.md`.
