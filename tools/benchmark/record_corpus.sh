#!/usr/bin/env bash
# issue #8: build a license-clean corpus by recording your own voice. Self-
# recorded audio sidesteps the whole "which corpus can we redistribute"
# problem: it never leaves your machine, and the repo stores only the text
# references you choose to publish with a benchmark run.
#
# usage: tools/benchmark/record_corpus.sh CORPUS_DIR [count]
#   reads from the default ALSA capture device (16 kHz mono PCM16, exactly
#   what bench_asr requires). Spacebar-ish flow: it records one clip per
#   Enter, you type the reference right after.
set -euo pipefail

DIR=${1:?usage: record_corpus.sh CORPUS_DIR [count]}
COUNT=${2:-10}
mkdir -p "$DIR"
MANIFEST="$DIR/manifest.tsv"; touch "$MANIFEST"

REC=""
for c in arecord pw-record; do
  command -v "$c" >/dev/null 2>&1 && REC=$c && break
done
[ -n "$REC" ] || { echo "need arecord (alsa-utils) or pw-record (pipewire-bin)" >&2; exit 1; }

n=$(wc -l < "$MANIFEST" | tr -d ' ')
for i in $(seq 1 "$COUNT"); do
  n=$((n + 1))
  read -r -p "clip $n: Enter to record (Ctrl-C twice to quit) " _
  if [ "$REC" = arecord ]; then
    arecord -q -f S16_LE -r 16000 -c 1 -t wav "$DIR/clip$((n - 1)).wav"
    echo "(recording stopped; silence included is fine)" >&2
  else
    timeout 30 pw-record --rate=16000 --channels=1 "$DIR/clip$((n - 1)).wav"
  fi
  printf 'clip %s heard as (say it plainly, punctuation optional): ' "$((n - 1))" >&2
  read -r ref < /dev/tty
  printf 'clip%s.wav\t%s\n' "$((n - 1))" "$ref" >> "$MANIFEST"
done
echo "corpus ready: $DIR ($n clips)" >&2
