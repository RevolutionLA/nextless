#!/usr/bin/env bash
# Issue #5: tools/get_models.sh must (1) map arches to the exact v1.13.8
# asset names (aarch64 uses the -shared-cpu variant; a sed over the x64 name
# 404s upstream), (2) install into ~/.local/share/nextless and nowhere else,
# (3) be idempotent, and (4) leave no half-extracted model when a download
# is corrupt or extracts an unexpected directory. The install legs run
# against fake file:// release trees via the NEXTLESS_RELEASE_BASE /
# NEXTLESS_MODEL_BASE test hook, so CI never touches the 1.8 GB assets.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
script="$here/../tools/get_models.sh"
fail() { echo "FAIL: $*" >&2; exit 1; }

# Pull the constants the script itself declares, so the test cannot drift
# from the script silently.
V=$(sed -n 's/^VERSION="\([^"]*\)".*/\1/p' "$script")
ZIPDIR=$(sed -n 's/^ZIPFORMER_DIR="\([^"]*\)".*/\1/p' "$script")
FIRDIR=$(sed -n 's/^FIRERED_DIR="\([^"]*\)".*/\1/p' "$script")
PUNDIR=$(sed -n 's/^PUNCT_DIR="\([^"]*\)".*/\1/p' "$script")

# --- constants vs advanced.json.example --------------------------------------
cfg="$here/../config/advanced.json.example"
cfg_zip=$(sed -n 's|.*"model_dir": ".*models/\(sherpa-onnx-streaming-zipformer[^"]*\)".*|\1|p' "$cfg")
cfg_fir=$(sed -n 's|.*"model_dir": ".*models/\(sherpa-onnx-fire-red[^"]*\)".*|\1|p' "$cfg")
[ "$ZIPDIR" = "$cfg_zip" ] || fail "script Zipformer dir '$ZIPDIR' != advanced.json.example '$cfg_zip'"
[ "$FIRDIR" = "$cfg_fir" ] || fail "script FireRed dir '$FIRDIR' != advanced.json.example '$cfg_fir'"

cfg_punct=$(sed -n 's|.*"model_dir": ".*models/\(sherpa-onnx-punct[^"]*\)".*|\1|p' "$cfg")
[ -n "$cfg_punct" ] || fail "advanced.json.example lost its punctuation section"
[ "$PUNDIR" = "$cfg_punct" ] || fail "script PUNCT_DIR '$PUNDIR' != advanced.json default '$cfg_punct'"

# --- dry-run URL mapping -------------------------------------------------------
out=$(NEXTLESS_DATA_HOME=/nonexistent bash "$script" --dry-run --arch=x86_64 --backend=both)
grep -q "sherpa-onnx-v${V}-linux-x64-shared.tar.bz2" <<<"$out" || fail "x86_64 runtime URL wrong"
out=$(NEXTLESS_DATA_HOME=/nonexistent bash "$script" --dry-run --arch=aarch64 --backend=both)
grep -q "sherpa-onnx-v${V}-linux-aarch64-shared-cpu.tar.bz2" <<<"$out" \
    || fail "aarch64 must use the -shared-cpu asset"
if grep -q -- "-linux-aarch64-shared\.tar\.bz2" <<<"$out"; then
    fail "aarch64 URL is the sed-over-x64 name that 404s upstream"
fi
if NEXTLESS_DATA_HOME=/nonexistent bash "$script" --dry-run --arch=riscv64 >/dev/null 2>&1; then
    fail "unknown arch should exit non-zero"
fi
# punctuation opt-in comes from the punctuation-models tag, only with the flag
out=$(NEXTLESS_DATA_HOME=/nonexistent bash "$script" --dry-run --backend=zipformer --punctuation)
grep -q "punctuation-models/$PUNDIR.tar.bz2" <<<"$out" || fail "punctuation URL wrong"
out=$(NEXTLESS_DATA_HOME=/nonexistent bash "$script" --dry-run --backend=zipformer)
if grep -q "Punct" <<<"$out"; then fail "punctuation shown without --punctuation"; fi

# --- fake release trees ---------------------------------------------------------
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
rel="$tmp/rel" models="$tmp/models"
mkdir -p "$rel" "$models" "$tmp/stage"

rt_top="sherpa-onnx-v${V}-linux-x64-shared"
mkdir -p "$tmp/stage/$rt_top/bin" "$tmp/stage/$rt_top/lib"
printf '#!/bin/sh\n' > "$tmp/stage/$rt_top/bin/sherpa-onnx"
printf '#!/bin/sh\n' > "$tmp/stage/$rt_top/bin/sherpa-onnx-offline"
chmod +x "$tmp/stage/$rt_top/bin/"*
: > "$tmp/stage/$rt_top/lib/libonnxruntime.so.1.13.1"
tar -cjf "$rel/$rt_top.tar.bz2" -C "$tmp/stage" "$rt_top"

mkdir -p "$tmp/stage/$PUNDIR"
for f in model.int8.onnx tokens.json config.yaml; do
    echo "fake" > "$tmp/stage/$PUNDIR/$f"
done
tar -cjf "$models/$PUNDIR.tar.bz2" -C "$tmp/stage" "$PUNDIR"

mkdir -p "$tmp/stage/$ZIPDIR"
for f in encoder-epoch-99-avg-1.onnx decoder-epoch-99-avg-1.onnx \
         joiner-epoch-99-avg-1.onnx tokens.txt; do
    echo "fake" > "$tmp/stage/$ZIPDIR/$f"
done
tar -cjf "$models/$ZIPDIR.tar.bz2" -C "$tmp/stage" "$ZIPDIR"

export HOME="$tmp/home"
DATA="$HOME/.local/share/nextless"
run() { NEXTLESS_RELEASE_BASE="file://$rel" NEXTLESS_MODEL_BASE="file://$models" \
        NEXTLESS_PUNCT_BASE="file://$models" bash "$script" "$@"; }

# --- full install (zipformer leg) -----------------------------------------------
run --backend=zipformer -y > "$tmp/log1" 2>&1 || fail "install failed: $(cat "$tmp/log1")"
[ -x "$DATA/sherpa-onnx/bin/sherpa-onnx" ] || fail "runtime bin missing"
[ -x "$DATA/sherpa-onnx/bin/sherpa-onnx-offline" ] || fail "runtime offline bin missing"
[ -f "$DATA/sherpa-onnx/lib/libonnxruntime.so.1.13.1" ] || fail "runtime lib missing"
[ -f "$DATA/models/$ZIPDIR/tokens.txt" ] || fail "zipformer model missing"
# FireRed was not requested: must not exist
[ ! -e "$DATA/models/$FIRDIR" ] || fail "installed FireRed without being asked"
# punctuation is opt-in: absent without the flag
[ ! -e "$DATA/models/$PUNDIR" ] || fail "installed punctuation without --punctuation"

# --- punctuation opt-in leg ------------------------------------------------------
run --backend=zipformer --punctuation -y > "$tmp/log5" 2>&1 \
    || fail "--punctuation install failed: $(cat "$tmp/log5")"
[ -f "$DATA/models/$PUNDIR/model.int8.onnx" ] || fail "punctuation model missing"
run --backend=zipformer --punctuation -y > "$tmp/log6" 2>&1 \
    || fail "punctuation re-run failed: $(cat "$tmp/log6")"
grep -q "Punctuation: already installed" "$tmp/log6" || fail "punctuation re-run did not skip"
# nothing outside the data home (the parent dirs mkdir -p had to create are
# fine; no files may live outside DATA)
if find "$HOME" -type f -not -path "$DATA*" | grep -q .; then
    fail "wrote files outside ~/.local/share/nextless: $(find "$HOME" -type f -not -path "$DATA*")"
fi
# staging cleaned
if ls -A "$DATA" | grep -q ".get-models-tmp"; then fail "staging dir left behind"; fi

# --- idempotency -------------------------------------------------------------------
run --backend=zipformer -y > "$tmp/log2" 2>&1 || fail "re-run failed: $(cat "$tmp/log2")"
grep -q "already installed" "$tmp/log2" || fail "re-run did not skip installed pieces"
if ls -A "$DATA" | grep -q ".get-models-tmp"; then fail "re-run left staging"; fi

# --- corrupt download leaves no half-extracted model ---------------------------------
printf 'not a bzip2 file' > "$models/$FIRDIR.tar.bz2"
if run --backend=firered -y > "$tmp/log3" 2>&1; then
    fail "corrupt archive must abort"
fi
grep -qi "extract" "$tmp/log3" || fail "no clear extraction error: $(cat "$tmp/log3")"
if ls -A "$DATA/models" | grep -q "$FIRDIR"; then fail "half-extracted FireRed present"; fi
if ls -A "$DATA" | grep -q ".get-models-tmp"; then fail "staging left after failure"; fi

# --- unexpected top-level directory is refused ---------------------------------------
mkdir -p "$tmp/stage/wrong-name" && echo x > "$tmp/stage/wrong-name/tokens.txt"
tar -cjf "$models/$ZIPDIR.tar.bz2" -C "$tmp/stage" wrong-name
rm -rf "$DATA/models/$ZIPDIR"
if run --backend=zipformer -y > "$tmp/log4" 2>&1; then
    fail "unexpected dir name must abort"
fi
grep -qi "other than" "$tmp/log4" || fail "no directory-name error: $(cat "$tmp/log4")"
[ ! -e "$DATA/models/wrong-name" ] || fail "wrong-name dir escaped into models/"
if ls -A "$DATA" | grep -q ".get-models-tmp"; then fail "staging left after name failure"; fi

# --- issue #45: no terminal and no --backend must ask, not download ---------------
# The two cases above deliberately corrupt the archives; rebuild valid ones so the
# "both" leg below proves real behaviour rather than a download error.
tar -cjf "$models/$ZIPDIR.tar.bz2" -C "$tmp/stage" "$ZIPDIR"
mkdir -p "$tmp/stage/$FIRDIR"
for f in encoder.int8.onnx decoder.int8.onnx tokens.txt; do echo fake > "$tmp/stage/$FIRDIR/$f"; done
tar -cjf "$models/$FIRDIR.tar.bz2" -C "$tmp/stage" "$FIRDIR"

# `< /dev/null` matters: a developer running this file from a terminal would
# otherwise land in the interactive picker instead of the branch under test.
rm -rf "$DATA"
if run < /dev/null > "$tmp/log7" 2>&1; then
    fail "no-terminal + no --backend must stop instead of quietly taking both (~1.8 GB)"
fi
grep -qi "no --backend" "$tmp/log7" || fail "refusal should name the flag: $(cat "$tmp/log7")"
if [ -e "$DATA/models/$ZIPDIR" ] || [ -e "$DATA/models/$FIRDIR" ]; then
    fail "it fetched models nobody consented to"
fi

# -y is the scripted opt-in: same call, no terminal, now it goes all the way.
if ! run -y < /dev/null > "$tmp/log8" 2>&1; then
    fail "-y should be enough to install both without a terminal: $(cat "$tmp/log8")"
fi
[ -e "$DATA/models/$ZIPDIR/tokens.txt" ] || fail "-y path skipped Zipformer"
[ -e "$DATA/models/$FIRDIR/tokens.txt" ] || fail "-y path skipped FireRed"

echo "get_models.sh: all checks passed"
