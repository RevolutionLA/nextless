#!/usr/bin/env bash
# Roadmap row 5: the deepfilter row in docs/benchmarks.md is produced by
# `bench_asr --denoise`, so the *harness* needs a contract too - it is the only
# thing standing between "the denoiser silently did nothing" and a published
# timing number. Checked here:
#   1. a binary that changes the audio reports ok + changed + samples_out back at
#      16 kHz (the resample round trip is inside dfDenoise, not around it),
#   2. a binary that exits 0 without touching the file reports ok:false AND an
#      `error` field - cer.py counts a row as measured unless it carries one, so
#      without this the table would print 4/4 over four dead rows,
#   3. after three failures in a row the remaining files say "not measured"
#      instead of leaking wall_ms=0 rows that read like results,
#   4. missing binary is a failure, not a silent zero.
# DeepFilterNet itself is exercised against stubs: the real 36 MB binary is
# opt-in and cannot be a CI dependency.
set -euo pipefail

BIN=${1:?usage: test_bench_denoise.sh /path/to/bench_asr}
[ -x "$BIN" ] || { echo "FAIL: bench_asr not executable: $BIN" >&2; exit 1; }

here="$(cd "$(dirname "$0")" && pwd)"
cer="$here/../tools/benchmark/cer.py"
fail() { echo "FAIL: $*" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
corpus="$tmp/corpus"
mkdir -p "$corpus"

# 0.4 s of 16 kHz mono PCM16 tone, four identical files (no ffmpeg in CI).
python3 - "$corpus" <<'PY'
import struct, sys, math, os
d = sys.argv[1]
sr, n = 16000, 6400
pcm = b"".join(struct.pack("<h", int(9000 * math.sin(2 * math.pi * 220 * i / sr)))
               for i in range(n))
hdr = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE" + b"fmt " +
       struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 2, 2, 16) +
       b"data" + struct.pack("<I", len(pcm)))
for i in range(4):
    with open(os.path.join(d, f"utt{i}.wav"), "wb") as f:
        f.write(hdr + pcm)
PY

# --- stubs --------------------------------------------------------------------
# deep-filter is invoked as: <bin> -D -o OUT_DIR IN.wav, and rewrites IN.wav in
# place (IN lives in OUT_DIR). Both stubs take the real argument shape so a
# regression in the argv would not silently pass.
cat > "$tmp/changes.sh" <<'STUB'
#!/usr/bin/env bash
file="${@: -1}"
[ -f "$file" ] || exit 1
# flip the first sample after the canonical 44-byte header
printf '\x01\x27' | dd of="$file" bs=1 seek=44 count=2 conv=notrunc status=none
STUB
cat > "$tmp/noop.sh" <<'STUB'
#!/usr/bin/env bash
# clean exit, file untouched: the failure mode that looks most like success
file="${@: -1}"
[ -f "$file" ] || exit 1
exit 0
STUB
chmod +x "$tmp/changes.sh" "$tmp/noop.sh"

# dfBinaryPath() falls back to $HOME/.local/share/nextless/bin/deep-filter, so a
# developer box that really installed the binary would make the "missing binary"
# case find it. Pin HOME at an empty dir: every run is then deterministic.
mkdir -p "$tmp/nohome"

# field <raw.jsonl> <python expr over r / s / n> [row index]
# r = that row, s = the summary row, n = number of data rows.
field() { python3 -c "
import json,sys
rows=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
r=rows[int(sys.argv[3])]; s=rows[-1]; n=len(rows)-1
print(eval(sys.argv[2]))" "$1" "$2" "${3:-0}"; }

run() { # run <rawfile> <env assignments...>
    local out=$1; shift
    env HOME="$tmp/nohome" "$@" "$BIN" --denoise --corpus "$corpus" \
        --out "$out" >/dev/null 2>"$out.err" || true
}

# --- 1. a working denoiser is reported as working ------------------------------
run "$tmp/good.jsonl" NEXTLESS_DEEP_FILTER="$tmp/changes.sh"
[ "$(wc -l < "$tmp/good.jsonl")" = 5 ] || fail "expected 4 rows + summary, got $(wc -l < "$tmp/good.jsonl")"
[ "$(field "$tmp/good.jsonl" 'r["ok"]' 0)" = "True" ] || fail "working stub reported ok=false"
[ "$(field "$tmp/good.jsonl" 'r["changed"]' 0)" = "True" ] || fail "working stub reported changed=false"
if field "$tmp/good.jsonl" '",".join(r)' 0 | grep -q error; then
    fail "a measured row must not carry an error field"
fi
# dfDenoise resamples 16k->48k->16k and guarantees the buffer comes back the same
# length it went in (issue #61: both legs used to drop their resampler tail, 830
# samples of 6400, and deep-filter's own 30 ms was then truncated off too). The
# stub preserves the file length, so the row must be exactly the input length -
# 3x would mean the audio never came back down to 16 kHz.
got=$(field "$tmp/good.jsonl" 'r["samples_out"]' 0)
[ "$got" = "6400" ] || fail "samples_out=$got != the 6400 input samples (length-preserving invariant broken)"
[ "$(field "$tmp/good.jsonl" 'r["wall_ms"] > 0' 0)" = "True" ] || fail "a real spawn measured 0 ms"
[ "$(field "$tmp/good.jsonl" 'r["wall_ms"] > 0' 0)" = "True" ] || fail "a real spawn measured 0 ms"
[ "$(field "$tmp/good.jsonl" 's["failures"]' 0)" = "0" ] || fail "summary counted failures for a clean run"
grep -q 'df_denoise=' "$tmp/good.jsonl.err" || fail "stderr lost the df_denoise timing line"

# --- 2/3. a no-op binary is a failure, and three of them stop the run ---------
run "$tmp/noop.jsonl" NEXTLESS_DEEP_FILTER="$tmp/noop.sh"
[ "$(field "$tmp/noop.jsonl" 'r["ok"]' 0)" = "False" ] || fail "no-op binary reported ok=true"
[ "$(field "$tmp/noop.jsonl" 'r["changed"]' 0)" = "False" ] || fail "no-op binary reported changed=true"
[ -n "$(field "$tmp/noop.jsonl" 'r.get("error","")' 0)" ] || fail "no-op row has no error field"
# the 4th file is inside the breaker: no timing number may survive it
[ "$(field "$tmp/noop.jsonl" 'r.get("error","").startswith("not measured")' 3)" = "True" ] ||
    fail "row after three failures is not labelled 'not measured'"
[ "$(field "$tmp/noop.jsonl" 'str(r.get("wall_ms"))' 3)" = "None" ] ||
    fail "row after the breaker leaked a wall_ms timing number"
[ "$(field "$tmp/noop.jsonl" 's["failures"]' 0)" = "4" ] || fail "summary failures != 4 for a dead binary"

# --- 4. missing binary ---------------------------------------------------------
run "$tmp/gone.jsonl" NEXTLESS_DEEP_FILTER="$tmp/does-not-exist"
[ "$(field "$tmp/gone.jsonl" 'r["ok"]' 0)" = "False" ] || fail "missing binary reported ok=true"
[ "$(wc -l < "$tmp/gone.jsonl")" = 5 ] || fail "missing-binary run lost rows"
grep -q 'not installed' "$tmp/gone.jsonl.err" || fail "missing-binary run said nothing about it"

# --- 5. the published table cannot read a dead run as measured ----------------
for f in good noop; do
    python3 "$cer" "$tmp/$f.jsonl" > "$tmp/$f.table" || fail "cer.py crashed on $f rows"
    okcol=$(grep '^| deepfilter' "$tmp/$f.table" | cut -d'|' -f3 | tr -d ' ')
    case "$f" in
        good) [ "$okcol" = "4/4" ] || fail "table shows $okcol for a working denoiser" ;;
        noop) [ "$okcol" = "0/4" ] || fail "table shows $okcol for a dead denoiser" ;;
    esac
done

echo "OK bench_denoise"
