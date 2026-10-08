# Benchmark harness (issue #8)

Reproducible A/B numbers per backend: **CER, WER, first-token latency, RTF,
wall time, peak RSS** — measured through the exact code path the add-on uses
(`AsrProviderRegistry::create(id)` → `provider->transcribe(samples, wavPath)`),
not a parallel reimplementation.

## Files

| file | job |
|---|---|
| `bench_asr.cpp` | runner: one backend × one corpus dir → JSONL (`meson compile` 后在 `obj-*/tools/bench_asr`) |
| `cer.py` | normaliser + CER/WER + markdown table; rules documented in its docstring |
| `record_corpus.sh` | build a self-recorded 16 kHz mono corpus (`arecord`/`pw-record`) |
| `run_bench.sh` | one command: machine header → run each backend → table + raw JSONL |

## Corpus policy

The repo ships **no speech data** (weight, and every real corpus drags a
licence). Two supported ways to get one:

1. `record_corpus.sh DIR` — ten sentences of your own voice is the honest
   default: licence-clean by construction, and it tests the mic you dictate
   with. References go into `DIR/manifest.tsv` (`clipN.wav<TAB>reference`).
2. Any 16 kHz mono PCM16 WAVs + `manifest.tsv` you trust. The demo wavs that
   ship inside the model directories (`~/.local/share/nextless/models/*/test_wavs`,
   Apache-2.0) work for *timing* out of the box — leave the reference column
   empty and CER/WER render as `--` instead of pretending to be measured.

## Run

```bash
meson compile -C build
BACKENDS="zipformer firered" tools/benchmark/run_bench.sh ~/nextless-corpus
```

Cloud backends (`doubao`, `qwen`) run through the same harness but need your
`~/.config/nextless/*.json` keys and count network time into `wall_ms` — the
table says which backend it scored, attribution is the reader's job. `mock`
exists for plumbing checks only; never publish its row.

Every run leaves `bench-<date>/machine.txt`, `raw.jsonl` and `table.md`
behind. **Publish the raw JSONL next to any table** (`docs/benchmarks.md`
links the copies committed here); a table without its raw rows is decoration.
