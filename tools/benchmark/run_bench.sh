#!/usr/bin/env bash
# issue #8: one-command benchmark. Runs bench_asr per backend over a corpus,
# collects machine info so numbers are attributable, and renders the table
# with cer.py. Raw JSONL is kept next to the table: publish the outputs, not
# just the summary.
#
# usage: tools/benchmark/run_bench.sh CORPUS_DIR [OUT_DIR]
#        BACKENDS="zipformer firered" tools/benchmark/run_bench.sh ...
set -euo pipefail

CORPUS=${1:?usage: run_bench.sh CORPUS_DIR [OUT_DIR]}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${2:-"$ROOT/bench-$(date +%F-%H%M)"}

BIN=${BENCH_BIN:-}
if [ -z "$BIN" ]; then
  for c in "$ROOT"/obj-*/tools/bench_asr "$ROOT"/build/tools/bench_asr; do
    [ -x "$c" ] && BIN=$c && break
  done
fi
if [ -z "$BIN" ]; then
  echo "bench_asr not built; run: meson compile -C <builddir> (or set BENCH_BIN)" >&2
  exit 1
fi

BACKENDS=${BACKENDS:-zipformer fire_red}
mkdir -p "$OUT"

{
  echo "date: $(date -Is)"
  echo "host: $(hostname)"
  echo "kernel: $(uname -r)"
  grep -m1 'model name' /proc/cpuinfo | sed 's/^model name\s*:\s*/cpu: /'
  echo "threads: $(nproc)"
  echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  grep -m1 MemTotal /proc/meminfo | sed 's/^MemTotal:\s*/mem: /'
  echo "backends: $BACKENDS"
  echo "corpus: $CORPUS"
} > "$OUT/machine.txt"

: > "$OUT/raw.jsonl"
for b in $BACKENDS; do
  echo "== $b" >&2
  # exit 2 = provider/model unavailable on this box; not a benchmark failure,
  # the row then shows 0 files in the raw output and cer.py prints "--".
  "$BIN" --backend "$b" --corpus "$CORPUS" --out "$OUT/raw-$b.jsonl" || true
  cat "$OUT/raw-$b.jsonl" >> "$OUT/raw.jsonl"
done

python3 "$HERE/cer.py" "$OUT/raw.jsonl" --machine "$OUT/machine.txt" > "$OUT/table.md"
cat "$OUT/machine.txt"
echo
cat "$OUT/table.md"
echo
echo "raw outputs: $OUT/raw.jsonl" >&2
