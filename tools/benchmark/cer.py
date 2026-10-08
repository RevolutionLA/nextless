#!/usr/bin/env python3
"""issue #8: turn bench_asr JSONL into a comparable markdown table.

CER/WER only make sense after normalisation: models add or drop punctuation,
cases, and spaces around English inside Chinese ("你好World" vs "你好 world"),
so the raw string distance would be noise. The rules implemented here are
deliberately boring and documented, because a normaliser nobody can read is
just a way to lie with a number:

  1. NFKC (full-width forms fold onto half-width), lowercase.
  2. Drop every character that is not a letter/digit (CJK counts; so
     punctuation, spaces, and stray symbols disappear entirely).
  3. CER = edit distance over characters / len(reference chars).
  4. WER mixes granularities on purpose: each CJK char is one token (there
     is no word segmentation here and inventing one adds a second error
     source), each run of latin letters/digits is one token.

Records whose reference is empty are timed but never scored (CER shows as
"--"), which is how a corpus without verified transcripts stays honest.
"""
import argparse
import json
import statistics
import sys
import unicodedata


def normalize(text: str) -> str:
    out = []
    for ch in unicodedata.normalize("NFKC", text).lower():
        if ch.isspace():
            continue
        if ch.isalnum():
            out.append(ch)
    return "".join(out)


def tokenize(text: str):
    """CJK chars become single tokens; latin/digit runs become word tokens."""
    norm = unicodedata.normalize("NFKC", text).lower()
    tokens, word = [], []
    for ch in norm:
        if ch.isspace() or not ch.isalnum():
            if word:
                tokens.append("".join(word))
                word = []
        elif ord(ch) >= 0x2E80:  # CJK blocks start around here
            if word:
                tokens.append("".join(word))
                word = []
            tokens.append(ch)
        else:
            word.append(ch)
    if word:
        tokens.append("".join(word))
    return tokens


def edit_distance(a: str, b: str) -> int:
    if a == b:
        return 0
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def cer(ref: str, hyp: str):
    r, h = normalize(ref), normalize(hyp)
    if not r:
        return None
    return edit_distance(r, h) / len(r)


def wer(ref: str, hyp: str):
    r, h = tokenize(ref), tokenize(hyp)
    if not r:
        return None
    prev = list(range(len(h) + 1))
    for i, ta in enumerate(r, 1):
        cur = [i]
        for j, tb in enumerate(h, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ta != tb)))
        prev = cur
    return prev[-1] / len(r)


def load(path):
    per_backend = {}
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        b = rec.get("backend", "?")
        slot = per_backend.setdefault(b, {"rows": [], "summary": None})
        if rec.get("summary"):
            slot["summary"] = rec
        else:
            slot["rows"].append(rec)
    return per_backend


def fmt_ms(v):
    return "--" if v is None or v < 0 else str(int(round(v)))


def table(per_backend):
    header = ("| backend | ok | CER | WER | first token (ms) | RTF (warm) | "
              "cold wall (ms) | wall median (ms) | peak RSS (GB) |")
    sep = "|---|---|---|---|---|---|---|---|---|"
    lines = [header, sep]
    for b in sorted(per_backend):
        slot = per_backend[b]
        rows, summ = slot["rows"], slot["summary"]
        scored = [r for r in rows if not r.get("error") and r.get("ref")]
        cers = [c for c in (cer(r["ref"], r.get("text", "")) for r in scored) if c is not None]
        wers = [w for w in (wer(r["ref"], r.get("text", "")) for r in scored) if w is not None]
        ok = [r for r in rows if not r.get("error")]
        firsts = [r["first_ms"] for r in ok if r.get("first_ms", -1) >= 0]
        warm = [r for r in ok if not r.get("cold")]
        rtfs = [r["wall_ms"] / r["audio_ms"] for r in warm if r.get("audio_ms") and r.get("wall_ms")]
        colds = [r["wall_ms"] for r in rows if r.get("cold") and not r.get("error")]
        walls = [r["wall_ms"] for r in ok]
        rss = summ.get("peak_rss_kb") if summ else None
        # local backends infer inside a per-request child process; publish the
        # larger of harness and child peaks so "RSS" means what people think.
        child = summ.get("child_peak_rss_kb", 0) if summ else 0
        if rss is not None and child:
            rss = max(rss, child)
        lines.append("| {} | {}/{} | {} | {} | {} | {} | {} | {} | {} |".format(
            b, len(ok), len(rows),
            "{:.2%}".format(statistics.mean(cers)) if cers else "--",
            "{:.2%}".format(statistics.mean(wers)) if wers else "--",
            fmt_ms(statistics.median(firsts)) if firsts else "--",
            "{:.3f}".format(statistics.median(rtfs)) if rtfs else "--",
            fmt_ms(colds[0]) if colds else "--",
            fmt_ms(statistics.median(walls)) if walls else "--",
            "{:.2f}".format(rss / 1048576.0) if rss and rss > 0 else "--",
        ))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("raw", help="JSONL produced by bench_asr (concatenated per backend)")
    ap.add_argument("--machine", help="text file with CPU/kernel/thread info for the header")
    ap.add_argument("--out", default="-")
    args = ap.parse_args()

    text = []
    if args.machine:
        text.append("```\n" + open(args.machine, encoding="utf-8").read().strip() + "\n```")
    text.append(table(load(args.raw)))
    out = sys.stdout if args.out == "-" else open(args.out, "w", encoding="utf-8")
    out.write("\n\n".join(text) + "\n")


if __name__ == "__main__":
    main()
