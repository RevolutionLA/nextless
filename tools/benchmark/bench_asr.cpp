// issue #8: A/B benchmark runner. Drives one ASR backend over a WAV corpus
// through the exact code path the add-on uses (AsrProviderRegistry +
// provider->transcribe(samples, wavPath)), and prints one JSON record per
// file plus a summary with peak RSS. Timing, not transcription quality, is
// this program's job; CER/WER live in tools/benchmark/cer.py so the same
// numbers can be recomputed from the published raw outputs.
//
// Corpus format: <dir>/manifest.tsv with lines "<relpath>.wav\treference",
// or a bare directory of *.wav files each paired with a same-stem .txt.
// Every WAV must be 16 kHz mono PCM16 (what record_corpus.sh produces).

#include "asr_provider.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>
#include <sys/resource.h>

namespace {

using Clock = std::chrono::steady_clock;

long nowMs(const Clock::time_point &t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

bool readU32(std::istream &f, uint32_t &v) {
    return !!f.read(reinterpret_cast<char *>(&v), 4);
}

struct WavPcm {
    std::vector<int16_t> samples;
    double audioMs = 0.0;
    std::string error;
};

// Minimal RIFF walker (the previous one-shot tools assumed a fixed 44-byte
// header; corpus WAVs from ffmpeg/audacity are not that polite).
WavPcm readWav16kMono(const std::string &path) {
    WavPcm out;
    std::ifstream f(path, std::ios::binary);
    if (!f) { out.error = "cannot open " + path; return out; }
    char riff[4] = {};
    if (!f.read(riff, 4) || std::memcmp(riff, "RIFF", 4) != 0) { out.error = "not RIFF"; return out; }
    uint32_t len = 0;
    if (!readU32(f, len)) { out.error = "truncated"; return out; }
    char wave[4] = {};
    if (!f.read(wave, 4) || std::memcmp(wave, "WAVE", 4) != 0) { out.error = "not WAVE"; return out; }
    bool haveFmt = false;
    uint16_t channels = 0, bits = 0, fmt = 0;
    uint32_t rate = 0;
    while (f) {
        char id[4] = {};
        uint32_t sz = 0;
        if (!f.read(id, 4) || !readU32(f, sz)) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            uint16_t audioFormat = 0, ch = 0;
            uint32_t sr = 0;
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0, b = 0;
            if (!f.read(reinterpret_cast<char *>(&audioFormat), 2) ||
                !f.read(reinterpret_cast<char *>(&ch), 2) ||
                !f.read(reinterpret_cast<char *>(&sr), 4) ||
                !f.read(reinterpret_cast<char *>(&byteRate), 4) ||
                !f.read(reinterpret_cast<char *>(&blockAlign), 2) ||
                !f.read(reinterpret_cast<char *>(&b), 2)) { out.error = "bad fmt"; return out; }
            f.seekg(sz > 16 ? sz - 16 : 0, std::ios::cur);
            fmt = audioFormat; channels = ch; rate = sr; bits = b;
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0 && haveFmt) {
            if (fmt != 1 || channels != 1 || bits != 16 || rate != 16000) {
                out.error = "need 16kHz mono PCM16, got fmt=" + std::to_string(fmt) +
                            " ch=" + std::to_string(channels) +
                            " rate=" + std::to_string(rate) +
                            " bits=" + std::to_string(bits);
                return out;
            }
            size_t n = sz / 2;
            out.samples.resize(n);
            if (!f.read(reinterpret_cast<char *>(out.samples.data()), long(sz))) {
                out.error = "short data"; return out;
            }
            out.audioMs = double(n) / 16.0;
            return out;
        } else {
            f.seekg(sz + (sz & 1), std::ios::cur);
        }
    }
    out.error = "no data chunk";
    return out;
}

std::string jsonEscape(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); o += buf; }
            else o += char(c);
        }
    }
    return o;
}

long peakRssKb() {
    std::ifstream f("/proc/self/status");
    std::string k, v;
    while (f >> k) {
        std::getline(f, v);
        if (k == "VmHWM:") return std::atol(v.c_str());
    }
    return -1;
}

// The local backends do their inference in a per-request sherpa-onnx child;
// the harness's own RSS would happily stay at ~20 MB and completely miss the
// ~1-1.7 GB the models actually cost. RUSAGE_CHILDREN reports the max RSS of
// all waited-for terminated children - that is the number worth publishing.
long childPeakRssKb() {
    struct rusage ru {};
    if (getrusage(RUSAGE_CHILDREN, &ru) != 0) return -1;
    return ru.ru_maxrss;  // kB on Linux
}

struct CorpusItem {
    std::string wav;      // absolute path
    std::string name;     // stable id for the report
    std::string reference;
};

std::vector<CorpusItem> loadCorpus(const std::string &dir) {
    std::vector<CorpusItem> items;
    std::ifstream mf(dir + "/manifest.tsv");
    if (mf) {
        std::string line;
        while (std::getline(mf, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos || line.empty() || line[0] == '#') continue;
            CorpusItem it;
            it.wav = dir + "/" + line.substr(0, tab);
            it.reference = line.substr(tab + 1);
            auto slash = it.wav.find_last_of('/');
            it.name = it.wav.substr(slash + 1);
            items.push_back(std::move(it));
        }
        return items;
    }
    // Fallback: *.wav with same-stem *.txt next to them.
    // (system() with a fixed pattern is fine here; the tool is a dev script.)
    std::string cmd = "ls -1 " + dir + "/*.wav 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return items;
    char buf[4096];
    while (fgets(buf, sizeof buf, p)) {
        std::string wav = buf;
        while (!wav.empty() && (wav.back() == '\n' || wav.back() == '\r')) wav.pop_back();
        if (wav.empty()) continue;
        CorpusItem it;
        it.wav = wav;
        auto slash = wav.find_last_of('/');
        std::string stem = wav.substr(0, slash + 1) +
                           wav.substr(slash + 1).erase(wav.substr(slash + 1).find_last_of('.'));
        std::ifstream tf(stem + ".txt");
        if (tf) std::getline(tf, it.reference);
        it.name = wav.substr(slash + 1);
        items.push_back(std::move(it));
    }
    pclose(p);
    std::sort(items.begin(), items.end(), [](const CorpusItem &a, const CorpusItem &b) {
        return a.name < b.name;
    });
    return items;
}

// The add-on always hands providers a throwaway temp WAV; do the same so a
// provider that unlinks its path (queue-cleanup behaviour) can never eat a
// corpus file.
std::string copyToTempWav(const std::string &src) {
    char tmpl[] = "/tmp/nextless-bench-XXXXXX.wav";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) return {};
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(tmpl, std::ios::binary);
    out << in.rdbuf();
    out.close();
    close(fd);
    return tmpl;
}

} // namespace

int main(int argc, char **argv) {
    std::string backend, corpus, outPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto take = [&](std::string &dst) {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(1); }
            dst = argv[++i];
        };
        if (a == "--backend") take(backend);
        else if (a == "--corpus") take(corpus);
        else if (a == "--out") take(outPath);
        else { std::fprintf(stderr, "usage: %s --backend ID --corpus DIR [--out FILE]\n", argv[0]); return 1; }
    }
    if (backend.empty() || corpus.empty()) return 1;

    FILE *out = stdout;
    if (!outPath.empty()) {
        out = fopen(outPath.c_str(), "w");
        if (!out) { std::fprintf(stderr, "cannot open %s\n", outPath.c_str()); return 1; }
    }

    auto emit = [&](const std::string &line) {
        std::fprintf(out, "%s\n", line.c_str());
        std::fflush(out);
    };

    auto items = loadCorpus(corpus);
    if (items.empty()) {
        emit("{\"backend\":\"" + jsonEscape(backend) + "\",\"error\":\"empty corpus\"}");
        return 2;
    }

    auto provider = nextless::AsrProviderRegistry::instance().create(backend);
    if (!provider) {
        emit("{\"backend\":\"" + jsonEscape(backend) + "\",\"error\":\"provider unavailable\"}");
        return 2;
    }

    std::mutex m;
    std::condition_variable cv;
    std::string text;
    bool gotFinal = false;
    long firstMs = -1;
    Clock::time_point tCall{};
    provider->setResultCallback([&](const std::string &t, bool isFinal) {
        std::lock_guard<std::mutex> lk(m);
        if (!nextless::isBlankAsrText(t) && firstMs < 0)
            firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now() - tCall).count();
        text = t;
        if (isFinal) { gotFinal = true; cv.notify_all(); }
    });
    std::string lastError;
    provider->setErrorCallback([&](const std::string &e) {
        std::lock_guard<std::mutex> lk(m);
        lastError = e;
        gotFinal = true;   // errors terminate the wait the same way
        cv.notify_all();
    });

    size_t failures = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto &it = items[i];
        auto wav = readWav16kMono(it.wav);
        if (!wav.error.empty() || wav.samples.empty()) {
            ++failures;
            emit("{\"backend\":\"" + jsonEscape(backend) + "\",\"wav\":\"" + jsonEscape(it.name) +
                 "\",\"error\":\"" + jsonEscape(wav.error.empty() ? "empty wav" : wav.error) + "\"}");
            continue;
        }
        std::string tmp = copyToTempWav(it.wav);
        {
            std::unique_lock<std::mutex> lk(m);
            text.clear(); gotFinal = false; lastError.clear(); firstMs = -1;
        }
        auto t0 = Clock::now();
        {
            std::lock_guard<std::mutex> lk(m);
            tCall = t0;   // the callback stamps first token against this call
        }
        provider->transcribe(std::vector<int16_t>(wav.samples), tmp);
        long wall = -1;
        long first = -1;
        std::string finalText;
        std::string err;
        bool timedOut = false;
        {
            std::unique_lock<std::mutex> lk(m);
            // generous bound: RTF up to ~1 is plausible on weak hardware, x4
            // audio length plus a 60 s floor keeps model cold-start honest.
            auto budget = std::chrono::milliseconds(
                std::max<int64_t>(int64_t(60000), int64_t(wav.audioMs * 4)));
            if (!cv.wait_for(lk, budget, [&] { return gotFinal; })) timedOut = true;
            wall = nowMs(t0);
            first = firstMs;
            finalText = text;
            err = lastError;
        }
        if (!tmp.empty()) unlink(tmp.c_str());
        if (timedOut || (!err.empty() && !nextless::isNoSpeechError(err))) ++failures;
        std::string rec = "{\"backend\":\"" + jsonEscape(backend) +
            "\",\"wav\":\"" + jsonEscape(it.name) +
            "\",\"ref\":\"" + jsonEscape(it.reference) +
            "\",\"cold\":" + (i == 0 ? "true" : "false") +
            ",\"audio_ms\":" + std::to_string(static_cast<long>(wav.audioMs)) +
            ",\"first_ms\":" + std::to_string(first) +
            ",\"wall_ms\":" + std::to_string(wall) +
            ",\"error\":\"" + jsonEscape(timedOut ? "timeout" : err) +
            "\",\"text\":\"" + jsonEscape(finalText) + "\"}";
        emit(rec);
    }

    emit("{\"backend\":\"" + jsonEscape(backend) + "\",\"summary\":true,\"files\":" +
         std::to_string(items.size()) + ",\"failures\":" + std::to_string(failures) +
         ",\"peak_rss_kb\":" + std::to_string(peakRssKb()) +
         ",\"child_peak_rss_kb\":" + std::to_string(childPeakRssKb()) + "}");
    if (out != stdout) fclose(out);
    return failures == items.size() ? 3 : 0;
}
