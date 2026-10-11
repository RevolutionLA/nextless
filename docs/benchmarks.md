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

Machine header: [2026-10-08-i7-1260p-machine.txt](benchmarks/2026-10-08-i7-1260p-machine.txt)
· raw outputs: [2026-10-08-i7-1260p-raw.jsonl](benchmarks/2026-10-08-i7-1260p-raw.jsonl)

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

## 2026-10-11 — the optional DeepFilterNet denoiser (roadmap row 5)

Run with `BACKENDS="deepfilter" tools/benchmark/run_bench.sh DIR` — that mode
measures `AudioCapture::dfDenoise()` itself, not a hand-rolled call around it,
so the resample, the `posix_spawn`, the 5 s bounded wait and the fallback
decision are all inside the number.

Machine header: [2026-10-11-i7-1260p-deepfilter-machine.txt](benchmarks/2026-10-11-i7-1260p-deepfilter-machine.txt)
· raw outputs: [2026-10-11-i7-1260p-deepfilter-raw.jsonl](benchmarks/2026-10-11-i7-1260p-deepfilter-raw.jsonl)

| backend | ok | CER | WER | first token (ms) | RTF (warm) | cold wall (ms) | wall median (ms) | peak RSS (GB) |
|---|---|---|---|---|---|---|---|---|
| deepfilter | 7/7 | -- | -- | -- | 0.156 | -- | 1223 | 0.06 |

Same 7 demo wavs as above (4.69–10.05 s each), `deep_filter 0.5.6` (the
x86_64 musl CLI, 36 MB, model compiled in) installed at the documented opt-in
path — sha256 `70775e251eee44c0f2451a1e833326cf8bcbbe304d3e7cd12851e6fce72ef7da`,
so this row is attributable to one specific upstream binary rather than "some
deep-filter". `first token` and `cold` are `--` because this is a denoiser: it has no
streaming output and no warm/cold split — **every** call spawns the process
again, so the ~280 ms of spawn + model load is paid once per utterance. Fit
over the seven rows: `wall_ms ≈ 282 + 0.110 × audio_ms` (R² 0.89), median RTF
0.156, per-row range 0.131–0.163. A second run minutes later printed 0.151, which
looked like stability — the third run (after the fix below) printed 0.131, so the
repeat that agreed was luck: treat single-digit differences between runs as noise.
RSS is 26 MB harness + 59 MB child — two orders of magnitude below
the ASR models, so "heavy" in the docs refers to CPU, not memory.

What this decides:

- **The option stays.** 1.2 s per utterance on a 2022 laptop is well inside the
  5 s deadline; the deadline only binds past ~43 s of continuous speech, and
  past that the utterance falls back to speexdsp rather than hanging. That was
  the open question in row 5, and it is now measured rather than assumed.
- **It is not free.** ~1.2 s added to the end-of-speech → first-character gap,
  per utterance, on every take. `speexdsp` (default) costs single-digit ms.
- **It lost audio** (fixed further down this page, same day). 69–97 ms off the
  *end* of every take, most of it from our own unflushed resampler round trip
  rather than the model. That is
  [issue #61](https://github.com/RevolutionLA/nextless/issues/61), found by this
  measurement; `samples_out` per row in the raw JSONL is the evidence.

### Same corpus after the issue #61 fix (same day)

Machine header: [2026-10-11-i7-1260p-deepfilter-fixed-machine.txt](benchmarks/2026-10-11-i7-1260p-deepfilter-fixed-machine.txt)
· raw outputs: [2026-10-11-i7-1260p-deepfilter-fixed-raw.jsonl](benchmarks/2026-10-11-i7-1260p-deepfilter-fixed-raw.jsonl)

| backend | ok | RTF (warm) | wall median (ms) | peak RSS (GB) |
|---|---|---|---|---|
| deepfilter | 7/7 | 0.131 | 1043 | 0.05 |

`deficit = audio_ms × 16 − samples_out` per row, before → after:

| wav | before | after |
|---|---|---|
| f-henan.wav | 1102 (69 ms) | 0 |
| f-sichuan.wav | 1558 (97 ms) | 0 |
| utt10s.wav | 1325 (83 ms) | −2 (0 ms) |
| z1.wav | 1349 (84 ms) | 0 |
| z2.wav | 1150 (72 ms) | 0 |

Two changes: both resampler legs are now flushed with `in = NULL` (soxr's actual
end-of-input signal — `ilen = 0` is not one), which on a length-preserving stub
takes the round trip from `16000 in → 15170 out` to exact, and the model's own
30 ms framing loss (1440 samples at 48 kHz, measured with a burst probe: lag 0,
corr 0.983, so it is truncation not delay) is padded back from the raw input
instead of being dropped. The −2 on the 10 s clips is 2 samples *over*
`audio_ms × 16`, i.e. WAV-header rounding in the expected value, not extra
audio: the invariant under test is `samples_out == samples_in`.

**Do not read 0.156 → 0.131 as a speed-up from the fix.** Three runs of this
corpus on this machine within twenty minutes gave wall medians of
1223 / 1275 / 1043 ms (RTF 0.156 / 0.163 / 0.131): thermal state and scheduling
move this harness by ~20%. Quote the denoiser as **RTF ≈ 0.13–0.16, about 1.0–1.3 s
per 5–10 s utterance**, and read the deficit column above as the point of this
row. The same caveat applies to the single-run ASR rows at the top of the file.

## Relation to the old README numbers

The README used to quote RTF ≈ 0.13 (zipformer) and ≈ 0.38 (FireRed) plus
~1 GB / ~1.7 GB RSS — hand-measured decode-side figures on this same laptop.
The harness confirms the memory side and moves the *per-utterance wall*
numbers up (0.37 / 0.82): process + model startup is real and users feel it.
