# Coexisting with your typing IME

Nextless is an fcitx5 **module** (an event handler), not an input method. It never becomes the
active IME, never edits another add-on's settings, and it does not care which IME is selected
when you dictate: WeChat IME (`wetype-im`), fcitx5 `pinyin`, a plain `keyboard-us` layout, or
anything else you have installed. Typing keeps working the way it always did.

## How a dictation interacts with the active IME

1. You hold the trigger key (default **right Ctrl**) for `activation_msec` (300 ms). Nextless
   starts recording — the active IME is not involved and stays selected.
2. You release. The audio goes to the ASR backend (local or cloud).
3. The result is committed directly into the focused input context, the same way fcitx5 commits
   any text. It does **not** pass through the active IME's preedit: no candidate window pops up
   and the IME's own state is left alone.

## Tested combinations

<!-- smoke result pending: replace with the run summary -->

| Typing side | Status | Notes |
|---|---|---|
| WeChat IME — `wetype-im` (aarch64 bridge add-on) | ✅ smoke-verified | the default IM on the development machine |
| fcitx5 `pinyin` | ✅ smoke-verified | |
| plain layout (`keyboard-us`, …) | ✅ by construction | the trigger key is all it uses |
| Nextless without fcitx5 | ✗ | it is an add-on; use a standalone tool |

"Smoke-verified" means `scripts/smoke-coexistence.sh --dictate` ran against the installed build
on GNOME/Wayland: per IME, a whole dictation — injected trigger hold, a real utterance played
into the microphone, recognized text committed (`Nextless [press]` … `Nextless final commit:
text_len=…` in fcitx5's log).

## Who gets which key (grab rules)

- **Trigger key** (right Ctrl by default; a bare modifier or a combo, configurable in
  `fcitx5-configtool → Add-Ons → Nextless`). Held ≥ `activation_msec` → record; released →
  stop. A shorter tap does nothing.
- The trigger is **never swallowed**: right Ctrl keeps working as Ctrl for shortcuts, and while
  recording, all other keys still reach the application. The one exception is CapsLock as
  trigger — a locking key has to be swallowed to keep its LED state honest.
- **Shift + trigger** enters switch mode; while in it, ←/→ (backend) and ↑/↓ (denoiser) are
  consumed. Leave switch mode by releasing the trigger. Besides that and CapsLock-as-trigger,
  Nextless consumes no key events.
- fcitx5's own shortcuts (Ctrl+Space and friends) and the keys wetype or pinyin use are
  untouched.
- Trade-off to know about: *any* right-Ctrl use that holds it ≥ 300 ms (Ctrl+scroll zoom, long
  Ctrl-drags) starts a recording. If that bites you, raise `activation_msec` in
  `~/.config/nextless/nextless.json` or bind a different trigger key.

## Verifying on your machine

```bash
scripts/smoke-coexistence.sh           # read-only environment checks
scripts/smoke-coexistence.sh --inject  # + a real trigger tap/hold per IME (ydotool)
scripts/smoke-coexistence.sh --dictate # + one committed dictation per IME (speaker → mic)
```

The script switches the active IME to each combination and restores it afterwards. `--inject`
and `--dictate` ask for confirmation first: the hold records ~0.7 s of microphone audio, and
any recognized text is committed into the **focused window** — point the focus at a scratch
window before confirming. If `--dictate` finds no text, check `pactl get-default-sink` and
`pactl get-default-source`: the microphone has to hear the playback.
