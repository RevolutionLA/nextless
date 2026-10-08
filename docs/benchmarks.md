# Benchmarks (issue #8)

Numbers produced by `tools/benchmark/run_bench.sh` — the same code path the
add-on uses (`AsrProviderRegistry::create` → `provider->transcribe`), with the
machine header committed next to the raw JSONL. If a number here cannot be
reproduced with the harness, it does not belong in this file.

**What "RTF" means here:** wall clock per second of audio *as production
experiences it*. The local backends spawn a per-request sherpa-onnx child, so
model load is inside the measurement for `zipformer` (and for the first
request of a session). A decode-only RTF measured inside the model binary is
a different number; do not mix them into one table.

**RSS** is the max across the harness *and its waited-for children*
(`RUSAGE_CHILDREN`); the parent process alone sits near 20 MB and would be a
lie about the gigabytes these models cost.

## 2026-10-08 — i7-1260P, Ubuntu 26.04, powersave governor

Machine header: [2026-10-08-i7-1260p-machine.txt](2026-10-08-i7-1260p-machine.txt)
· raw outputs: [2026-10-08-i7-1260p-raw.jsonl](2026-10-08-i7-1260p-raw.jsonl)

| backend | ok | CER | WER | first token (ms) | RTF (warm) | cold wall (ms) | wall median (ms) | peak RSS (GB) |
|---|---|---|---|---|---|---|---|---|
| fire_red | 7/7 | -- | -- | 6106 | 0.815 | 6474 | 6106 | 1.63 |
| zipformer | 7/7 | -- | -- | 2336 | 0.370 | 2075 | 2336 | 0.48 |

Corpus: the 7 demo wavs shipped inside the Apache-2.0 model packages (zh +
Sichuan/Tianjin/Henan dialect, ~53 s total). **CER/WER are `--` on purpose:**
those wavs have no verified transcripts, and `cer.py` refuses to score
against an empty reference. The timing columns are fully valid.

To add a scored row: record your own corpus (`tools/benchmark/record_corpus.sh`),
run `BACKENDS="zipformer fire_red" tools/benchmark/run_bench.sh DIR`, and open
a PR with `machine.txt` + `raw.jsonl` + the rendered table. Second data points
from different hardware are the whole reason this file exists.

## Relation to the old README numbers

The README used to quote RTF ≈ 0.13 (zipformer) and ≈ 0.38 (FireRed) plus
~1 GB / ~1.7 GB RSS — hand-measured decode-side figures on this same laptop.
The harness confirms the memory side and moves the *per-utterance wall*
numbers up (0.37 / 0.82): process + model startup is real and users feel it.
