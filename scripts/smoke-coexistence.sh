#!/usr/bin/env bash
# Nextless coexistence smoke test — dictation next to a typing IME.
#
#   scripts/smoke-coexistence.sh            environment checks (read-only, always safe)
#   scripts/smoke-coexistence.sh --inject   also drive a real trigger tap + hold per IME
#                                           via ydotool. The hold records ~0.7 s of
#                                           microphone audio (a short beep plays) and any
#                                           result is committed to the focused window —
#                                           focus a scratch window first (--inject asks
#                                           for confirmation).
#   scripts/smoke-coexistence.sh --dictate [wav]
#                                           --inject plus one full end-to-end run: plays
#                                           a speech wav through the default sink while
#                                           recording, expects a committed transcription.
#
# Exit status: 0 = all checks passed, 1 = at least one failed.
#
# Evidence comes from the log fcitx5 writes to. It is auto-detected from the running
# process (fd 1); override with NEXTLESS_FCITX5_LOG=/path/file.log.
set -u

INJECT=0 DICTATE=0 WAV=""
for arg in "$@"; do
    case "$arg" in
        --inject)  INJECT=1 ;;
        --dictate) DICTATE=1; INJECT=1 ;;
        *.wav)     WAV="$arg" ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
    esac
done

PASS=0 FAIL=0
ok()   { echo "  ok   $*"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL $*"; FAIL=$((FAIL + 1)); }
note() { echo "  note $*"; }

IM_LIST=(wetype-im pinyin)

# ---------------------------------------------------------------- environment
echo "Nextless coexistence smoke — $(date '+%F %T')"

PID=$(pgrep -x fcitx5 | head -1)
if [ -n "$PID" ]; then ok "fcitx5 is running (pid $PID)"; else bad "fcitx5 is not running"; fi

find_addon_conf() {
    local name=$1 dir
    for dir in "${XDG_DATA_HOME:-$HOME/.local/share}/fcitx5/addon" \
               /usr/local/share/fcitx5/addon /usr/share/fcitx5/addon; do
        [ -f "$dir/$name.conf" ] && { echo "$dir/$name.conf"; return 0; }
    done
    return 1
}
loaded_in_maps() {
    [ -n "$PID" ] && grep -q -- "$1" "/proc/$PID/maps" 2>/dev/null
}

for addon in nextless wetype; do
    conf=$(find_addon_conf "$addon") \
        && ok "$addon add-on is installed ($conf)" \
        || bad "$addon add-on manifest not found in any fcitx5 addon dir"
done
find_addon_conf pinyin >/dev/null \
    && ok "pinyin add-on is installed" \
    || bad "pinyin add-on manifest not found"

loaded_in_maps 'nextless\.so' \
    && ok "nextless is loaded in the running fcitx5" \
    || bad "nextless is not loaded in the running fcitx5 (restart with: fcitx5 -r -d)"
loaded_in_maps 'wetype' \
    && ok "wetype is loaded in the running fcitx5" \
    || bad "wetype is not loaded in the running fcitx5"

PROFILE="${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/profile"
for im in "${IM_LIST[@]}"; do
    grep -q "^Name=$im\$" "$PROFILE" 2>/dev/null \
        && ok "IME '$im' is in the configured group" \
        || bad "IME '$im' is not in $PROFILE — add it in fcitx5-configtool first"
done

# ------------------------------------------------------------------ log file
LOG="${NEXTLESS_FCITX5_LOG:-}"
[ -z "$LOG" ] && [ -n "$PID" ] && [ -e "/proc/$PID/fd/1" ] && LOG=$(readlink "/proc/$PID/fd/1")
if [ -n "$LOG" ] && [ -r "$LOG" ] && [ -f "$LOG" ]; then
    ok "reading evidence from $LOG"
else
    LOG=""
    note "fcitx5's output is not a readable file ($LOG) — key-path checks unavailable."
    note "restart it as: fcitx5 -r -d >/tmp/fcitx5.log 2>&1, or set NEXTLESS_FCITX5_LOG."
fi

# -------------------------------------------------------------- trigger key
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/conf/nextless.conf"
HOTKEY=$(sed -nE 's/^0=(.*)$/\1/p' "$CONF" 2>/dev/null | tr -d ' \r' | head -1)
[ -z "$HOTKEY" ] && HOTKEY="Control_R" && note "no [Hotkey] in $CONF — using the built-in default (Control_R)"
ok "trigger key: $HOTKEY"

keycode_for() {
    case "$1" in
        Control_R) echo 97 ;; Control_L) echo 29 ;;
        Alt_R)     echo 100 ;; Alt_L)    echo 56 ;;
        Shift_R)   echo 54 ;; Shift_L)   echo 42 ;;
        Super_R)   echo 126 ;; Super_L)  echo 125 ;;
        *)         return 1 ;;
    esac
}
TRIGGER_CODE=$(keycode_for "$HOTKEY") || TRIGGER_CODE=""

# ------------------------------------------------------------------- summary
if [ "$INJECT" = 0 ]; then
    echo "environment-only mode: $PASS ok, $FAIL failed"
    [ "$FAIL" = 0 ] || exit 1
    echo "run with --inject to drive the real key path (see --help)"
    exit 0
fi

# =================================================================== injection
command -v ydotool >/dev/null || { bad "ydotool is not installed"; echo "smoke: cannot inject — aborting"; exit 1; }
YT=(ydotool)
for sock in /tmp/.ydotool_socket "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/.ydotool_socket"; do
    [ -S "$sock" ] && { YT=(env YDOTOOL_SOCKET="$sock" ydotool); break; }
done
[ "$FAIL" = 0 ] || { echo "passive checks failed — fix them before --inject"; exit 1; }
[ -n "$LOG" ] || { echo "no readable fcitx5 log — --inject needs it (see above)"; exit 1; }
[ -n "$TRIGGER_CODE" ] || { echo "trigger key '$HOTKEY' is not a bare modifier — --inject supports Control/Alt/Shift/Super taps only"; exit 1; }

echo
echo "--inject will now, for each IME:"
echo "  1. switch the active IME to it and back afterwards,"
echo "  2. tap the trigger (no effect anywhere),"
echo "  3. hold it for 0.7 s (a short beep plays, ~0.7 s of mic audio is recorded)."
echo "Any recognition result is committed to the currently focused window,"
echo "so point the focus at a scratch window (or stay quiet). Ctrl+C aborts."
read -r -p "proceed? [y/N] " answer
case "$answer" in [yY]*) ;; *) echo "aborted"; exit 1 ;; esac

PREV_IM=$(fcitx5-remote -n 2>/dev/null || true)
restore_im() { [ -n "$PREV_IM" ] && fcitx5-remote -s "$PREV_IM" >/dev/null 2>&1 || true; }
trap restore_im EXIT

MARK=0
mark()      { MARK=$(wc -l < "$LOG"); }
new_lines() { tail -n "+$((MARK + 1))" "$LOG"; }
wait_line() {  # wait_line <fixed string> <timeout-seconds>
    local i
    for ((i = 0; i < $2 * 10; i++)); do
        new_lines | grep -qF -- "$1" && return 0
        sleep 0.1
    done
    return 1
}

# --- tap: proves injection reaches fcitx5 and must NOT start recording
echo
mark
"${YT[@]}" key "$TRIGGER_CODE:1" || bad "ydotool key (tap) failed"
sleep 0.08
"${YT[@]}" key "$TRIGGER_CODE:0" || bad "ydotool key (tap release) failed"
if wait_line "Nextless [press]" 3; then ok "tap: trigger press reached Nextless"; else bad "tap: no '[press]' line in the log"; fi
sleep 0.5
if new_lines | grep -qF "Nextless activated"; then
    bad "tap: a <300 ms tap started recording (activation_msec too small?)"
else
    ok "tap: stayed below activation threshold — short press is inert"
fi

# --- hold per IME
for im in "${IM_LIST[@]}"; do
    echo
    echo "-- combo: $im"
    fcitx5-remote -s "$im" >/dev/null 2>&1
    sleep 0.3
    if [ "$(fcitx5-remote -n)" = "$im" ]; then ok "$im: switched"; else bad "$im: could not switch IME"; continue; fi

    mark
    "${YT[@]}" key "$TRIGGER_CODE:1"
    sleep 0.7
    "${YT[@]}" key "$TRIGGER_CODE:0"

    wait_line "Nextless [press]"                    3 && ok "$im: press seen"      || bad "$im: no '[press]' line"
    wait_line "Nextless activated (press→activate=" 3 && ok "$im: recording started" || bad "$im: no activation line"
    wait_line "Nextless deactivated (record="      10 && ok "$im: recording stopped" || bad "$im: no deactivation line"
    if wait_line "Nextless final commit: text_len=" 8; then
        ok "$im: recognition produced text and committed it (check the log)"
    elif wait_line "Nextless ASR error: " 8; then
        ok "$im: recognition finished through the error path (expected 'no speech' in a quiet room)"
    else
        bad "$im: no terminal result line — check the log tail"
    fi

    if [ "$(fcitx5-remote -n)" = "$im" ]; then ok "$im: IME state intact after dictation"; else bad "$im: IME state changed during dictation"; fi
    restore_im; sleep 0.2
    [ "$(fcitx5-remote -n)" = "$PREV_IM" ] && ok "$im: switched back to $PREV_IM" || bad "$im: failed to restore $PREV_IM"
done

# --- optional end-to-end: speech through the speakers, text into the window
if [ "$DICTATE" = 1 ]; then
    echo
    echo "-- end-to-end dictation"
    if [ -z "$WAV" ]; then
        WAV=$(find "$HOME/.local/share/nextless/models" -maxdepth 3 -path '*test_wavs/0.wav' 2>/dev/null | head -1)
    fi
    if [ -n "$WAV" ] && [ -f "$WAV" ]; then
        note "playing $(basename "$(dirname "$WAV")")/$(basename "$WAV") through the default sink"
        note "default sink: $(pactl get-default-sink 2>/dev/null || echo '?') — the mic must hear it"
        echo "focus a scratch window now (recognized text lands there) — dictating in 5 s"
        sleep 5
        for im in "${IM_LIST[@]}"; do
            echo "-- end-to-end under $im"
            fcitx5-remote -s "$im" >/dev/null 2>&1; sleep 0.3
            mark
            "${YT[@]}" key "$TRIGGER_CODE:1"
            sleep 0.4                               # past activation_msec, capture open
            paplay "$WAV" 2>/dev/null || pw-play "$WAV" 2>/dev/null || bad "neither paplay nor pw-play could play $WAV"
            sleep 0.3
            "${YT[@]}" key "$TRIGGER_CODE:0"
            if wait_line "Nextless deactivated (record=" 10; then ok "$im: recording stopped"; else bad "$im: no deactivation line"; fi
            if wait_line "Nextless final commit: text_len=" 20; then
                ok "$im: speech was recognized and committed (see log for length)"
            else
                bad "$im: no commit — check speaker volume/mic routing (see docs/coexistence.md)"
            fi
            restore_im; sleep 0.2
        done
    else
        bad "dictate: no wav given and none found under ~/.local/share/nextless/models/*/test_wavs/0.wav"
    fi
fi

echo
echo "smoke: $PASS ok, $FAIL failed"
[ "$FAIL" = 0 ] || exit 1
