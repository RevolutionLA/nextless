# Security Policy

Nextless sits in a sensitive spot on purpose: it reads your microphone, receives every key event
before your input method does, and types recognised text into whichever window is focused. This
document states what we consider a security problem here and how to report it.

## What this software does with your data

| Concern | Behaviour |
|---|---|
| Audio | Captured to a temporary WAV, processed locally, then unlinked. Nothing is stored or uploaded by the local backends. |
| Local ASR (`zipformer`, `fire_red`) | Entirely offline: a `sherpa-onnx` process on your machine. No network I/O. |
| Cloud ASR (`doubao`, `qwen`) | Your audio is sent to the provider you configured. Read their data-handling terms; do not use a cloud backend for material you cannot send to a third party. |
| Credentials | `~/.config/nextless/doubao.json` and `qwen.json` are read-only user files (chmod them to `600`). They live outside the repository and are never logged. |
| Recognised text | Committed to the focused input context. Not written to disk. |
| Diagnostics log | Opt-in (`-Ddiagnostic_logging=true`). One JSON object per line: ids, durations, sizes. Text and identifiers appear only as length plus truncated SHA-256. No audio, no keys, no recognised text. Caps at 1 GiB and stops accepting events. |

`/dev/uinput` is used in exactly one situation: when the trigger key is a locking key such as
CapsLock, to undo the lock toggle. Bind any other key and no virtual device is created at all.

## Reporting a vulnerability

Use [GitHub Security Advisories](https://github.com/RevolutionLA/nextless/security/advisories/new)
— private, and the right channel. Please do not open a public issue.

I aim to reply within 7 days and ship a fix or a mitigation within 30. Reportedly affected
versions: `0.2.0` and everything before it, including upstream `vinput`.

Not valid on its own: an open port, a missing HTTP header, a dependency advisory without an
exploitable path through this add-on, or "the cloud backend sends my audio to a cloud backend".

## If you are reporting a bug, not a vulnerability

Check first whether your log or screenshot contains an API key. Real keys live only in
`~/.config/nextless/*.json`; when in doubt, replace the value with `REDACTED`.
