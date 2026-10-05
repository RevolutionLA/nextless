#include "audio_capture.h"
#include "nextless_config.h"
#include "diagnostic_log.h"

#include <pulse/mainloop.h>
#include <pulse/error.h>
#include <speex/speex_preprocess.h>
#include <soxr.h>
#include <unistd.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <ebur128.h>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

extern char **environ;

namespace nextless {

namespace {
std::atomic<uint64_t> nextCaptureId{0};
std::mutex deepFilterMutex;
std::mutex pulseCaptureMutex;
std::mutex audioProcessingMutex;
std::condition_variable audioProcessingReady;
uint64_t nextProcessingTicket = 0;
uint64_t nextProcessingId = 0;

class ProcessingTurn {
public:
    explicit ProcessingTurn(uint64_t id) : lock_(audioProcessingMutex) {
        audioProcessingReady.wait(lock_, [id] { return id == nextProcessingId; });
    }

    ~ProcessingTurn() {
        ++nextProcessingId;
        lock_.unlock();
        audioProcessingReady.notify_all();
    }

private:
    std::unique_lock<std::mutex> lock_;
};
}

double AudioCapture::lufsTarget_ = -16.0;
int AudioCapture::speexLevel_ = -15;
double AudioCapture::crestThreshold_ = 2.4;

static std::string jsonGetString(const std::string &json, const std::string &key) {
    std::string q = "\"" + key + "\"";
    auto pos = json.find(q);
    if (pos == std::string::npos) return "";
    pos += q.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        pos++;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }

    if (json[pos] == 't' || json[pos] == 'f') {
        return json.substr(pos, json.find_first_of(",}\n\r \t", pos) - pos);
    }

    return "";
}

static void loadAudioConfig(std::string &denoiseMethod) {
    auto json = readConfigFile("audio.json");
    if (json.empty()) return;
    auto val = jsonGetString(json, "denoise");
    if (val == "deepfilter") denoiseMethod = "deepfilter";
    else if (val == "speexdsp") denoiseMethod = "speexdsp";
    else if (val == "true") denoiseMethod = "speexdsp";
}

// 获取安全的 capture 目录：$XDG_RUNTIME_DIR/nextless/（模式 0700）。
// XDG_RUNTIME_DIR 缺失时兜底到 /tmp，用 mkdtemp 生成不可预测的目录名。
// 结果缓存在函数级 static 里：mkdtemp 每进程只执行一次，start() 反复调用
// 不会泄漏目录（issue #28）。
static std::string secureCaptureDir() {
    static std::string cached;
    if (!cached.empty()) return cached;

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !*xdg) xdg = "/tmp"; // fallback

    std::string dir;
    bool useMkdtemp = (strcmp(xdg, "/tmp") == 0);

    if (useMkdtemp) {
        // Fallback 到 /tmp 时使用 mkdtemp 创建不可预测的目录名
        std::string tpl = std::string(xdg) + "/nextless_XXXXXX";
        std::vector<char> buf(tpl.begin(), tpl.end());
        buf.push_back('\0');
        char *result = mkdtemp(buf.data());
        if (!result) {
            fprintf(stderr, "Nextless Capture: cannot create temp dir in %s: %s\n",
                    xdg, strerror(errno));
            return "";
        }
        dir = result;
        chmod(dir.c_str(), 0700);
    } else {
        // $XDG_RUNTIME_DIR 是可信的，使用固定路径
        dir = std::string(xdg) + "/nextless";
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            // 目录不存在，创建它
            if (mkdir(dir.c_str(), 0700) != 0) {
                fprintf(stderr, "Nextless Capture: cannot create %s: %s\n",
                        dir.c_str(), strerror(errno));
                return "";
            }
        } else if (!S_ISDIR(st.st_mode)) {
            // 存在但不是目录（可能是 symlink attack）
            fprintf(stderr, "Nextless Capture: %s exists but is not a directory\n", dir.c_str());
            return "";
        } else {
            // 目录已存在，确保权限是 0700
            chmod(dir.c_str(), 0700);
        }
    }
    cached = dir;
    return cached;
}

// 删除 dirPath 里所有 nextless_cap_*.wav；返回是否删掉了至少一个文件。
static bool sweepWavsIn(const std::filesystem::path &dirPath) {
    std::error_code ec;
    bool removed = false;
    for (const auto &entry : std::filesystem::directory_iterator(dirPath, ec)) {
        if (ec) break;
        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec) continue;
        auto filename = entry.path().filename().string();
        // 匹配 nextless_cap_<pid>_<n>.wav 模式
        if (filename.rfind("nextless_cap_", 0) == 0 &&
            filename.size() > 13 &&
            filename.substr(filename.size() - 4) == ".wav") {
            std::filesystem::remove(entry.path(), ec);
            if (!ec) {
                removed = true;
                fprintf(stderr, "Nextless Capture: swept orphaned WAV %s\n", filename.c_str());
            }
        }
    }
    return removed;
}

// 兜底模式下兄弟目录里可能有别的会话正在写的录音，只清理放了超过
// kOrphanMaxAgeSec 秒的文件；正常录音整个生命周期只有几秒。
static constexpr int kOrphanMaxAgeSec = 600;

// 启动时清理孤儿文件（来自之前崩溃的会话）。除当前目录外，还扫描其
// 同级的 nextless* 目录 —— 兜底模式下旧会话的 mkdtemp 目录名不可预测，
// 不清扫就永远删不掉（issue #28）。当前目录本身绝不删除，避免 start()
// 拿到一个已消失的路径。
static void sweepOrphanedWavs(const std::string &dir) {
    if (dir.empty()) return;

    std::error_code ec;
    auto current = std::filesystem::weakly_canonical(dir, ec);
    if (ec) current = std::filesystem::path(dir);

    // 当前目录：固定路径模式下崩溃残留就落在这里，无条件清理。
    sweepWavsIn(current);

    auto parent = current.parent_path();
    for (const auto &entry : std::filesystem::directory_iterator(parent, ec)) {
        if (ec) break;
        std::error_code dec;
        if (!entry.is_directory(dec) || dec) continue;
        auto name = entry.path().filename().string();
        if (name.rfind("nextless", 0) != 0) continue;
        auto sibling = std::filesystem::weakly_canonical(entry.path(), ec);
        if (ec) sibling = entry.path();
        if (sibling == current) continue;

        // 兄弟目录：只清理足够"陈旧"的文件，避开并发会话的活动录音。
        for (const auto &f : std::filesystem::directory_iterator(sibling, ec)) {
            if (ec) break;
            std::error_code fec;
            if (!f.is_regular_file(fec) || fec) continue;
            auto filename = f.path().filename().string();
            if (filename.rfind("nextless_cap_", 0) != 0 ||
                filename.size() <= 13 ||
                filename.substr(filename.size() - 4) != ".wav") continue;
            auto mtime = std::filesystem::last_write_time(f.path(), ec);
            if (ec) continue;
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::file_clock::now() - mtime).count();
            if (age < kOrphanMaxAgeSec) continue;
            std::filesystem::remove(f.path(), ec);
            if (!ec) {
                fprintf(stderr, "Nextless Capture: swept orphaned WAV %s/%s\n",
                        name.c_str(), filename.c_str());
            }
        }
        // 目录变空了就收掉；非空（别的会话还在用）remove 会失败，忽略即可。
        std::filesystem::remove(sibling, ec);
    }
}

// 向已打开的 FILE* 写 PCM16 单声道 WAV。writeWav（自有 open 流程）和
// dfDenoise（mkstemps 拿到 fd）共用这一份头/数据序列化，避免两处各写
// 一份、日后格式改动只在一条路径生效（issue #29）。不 fclose，所有权
// 归调用方。
static void writeWavToFp(FILE *f, const std::vector<int16_t> &samples,
                         uint32_t sampleRate) {
    long dataSize = (long)samples.size() * 2;
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + (uint32_t)dataSize;
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sub1 = 16, sr = sampleRate, br = sampleRate * 2;
    uint16_t fmtTag = 1, ch = 1, bps = 16, ba = 2;
    fwrite(&sub1, 4, 1, f); fwrite(&fmtTag, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = (uint32_t)dataSize;
    fwrite(&ds, 4, 1, f);
    fwrite(samples.data(), 2, samples.size(), f);
}

AudioCapture::AudioCapture() {
    loadAudioConfig(denoiseMethod_);
    if (!denoiseMethod_.empty())
        fprintf(stderr, "Nextless Capture: denoise=%s\n", denoiseMethod_.c_str());
    diagnosticLog().event("audio", "capture_created", {
        {"denoiser", denoiseMethod_.empty() ? "none" : denoiseMethod_}
    });

    // 启动时清理孤儿 WAV 文件（来自之前崩溃的会话）
    static bool sweepDone = false;
    if (!sweepDone) {
        auto dir = secureCaptureDir();
        if (!dir.empty()) {
            sweepOrphanedWavs(dir);
            sweepDone = true;
        }
    }

    static bool configLoaded = false;
    if (!configLoaded) {
        auto adv = advancedSection("audio");
        if (!adv.empty()) {
            auto t = jsonDouble(adv, "lufs_target", lufsTarget_);
            if (t > -100.0 && t < 0.0) lufsTarget_ = t;
            speexLevel_ = jsonInt(adv, "speex_level", speexLevel_);
            crestThreshold_ = jsonDouble(adv, "crest_threshold", crestThreshold_);
            fprintf(stderr, "Nextless Capture: config audio section parsed: crest_threshold=%.2f lufs_target=%.1f speex_level=%d\n",
                    crestThreshold_, lufsTarget_, speexLevel_);
        } else {
            fprintf(stderr, "Nextless Capture: advanced.json [audio] section not found, using defaults\n");
        }
        configLoaded = true;
    }
}

AudioCapture::~AudioCapture() {
    stop();
    wait();
}

void AudioCapture::processSamples(std::vector<int16_t> &samples, const std::string &denoiser) {
    auto t0 = std::chrono::steady_clock::now();

    double loudness = normalizeSamples(samples);
    bool isBlank = !hasVoice(samples);

    bool wantDenoise = !denoiser.empty() && denoiser != "none";
    if (!isBlank && wantDenoise) applyDenoise(samples, denoiser);

    trimSilence(samples);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Nextless Pipeline [summary] loudness=%.1f isBlank=%d denoiser=%s time=%ldms samples=%zu\n",
            loudness, (int)isBlank, denoiser.c_str(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            samples.size());
}

void AudioCapture::start() {
    if (recordThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
    }
    stopRequested_ = false;
    finished_ = false;
    captureId_ = nextCaptureId.fetch_add(1);

    // 使用安全目录：$XDG_RUNTIME_DIR/nextless/（模式 0700）
    auto dir = secureCaptureDir();
    if (dir.empty()) {
        fprintf(stderr, "Nextless Capture: cannot determine safe capture directory\n");
        return;
    }
    wavPath_ = dir + "/nextless_cap_" + std::to_string(getpid()) + "_" +
               std::to_string(captureId_) + ".wav";
    diagnosticLog().event("audio", "capture_start", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"wav_hash", hashDiagnosticValue(wavPath_).substr(0, 16)}
    });
    if (onState_) onState_(true);
    recordThread_ = std::thread(&AudioCapture::recordLoop, this);
}

void AudioCapture::stop() {
    stopRequested_ = true;
    diagnosticLog().event("audio", "capture_stop", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)}
    });
    std::lock_guard<std::mutex> lock(pulseMutex_);
    if (pulseMainloop_) pa_mainloop_wakeup(pulseMainloop_);
}

void AudioCapture::wait() {
    if (recordThread_.joinable()) recordThread_.join();
}

void AudioCapture::contextStateCallback(pa_context *, void *userdata) {
    auto *self = static_cast<AudioCapture *>(userdata);
    std::lock_guard<std::mutex> lock(self->pulseMutex_);
    if (self->pulseMainloop_) pa_mainloop_wakeup(self->pulseMainloop_);
}

void AudioCapture::streamStateCallback(pa_stream *, void *userdata) {
    auto *self = static_cast<AudioCapture *>(userdata);
    std::lock_guard<std::mutex> lock(self->pulseMutex_);
    if (self->pulseMainloop_) pa_mainloop_wakeup(self->pulseMainloop_);
}

void AudioCapture::streamReadCallback(pa_stream *stream, size_t, void *userdata) {
    auto *self = static_cast<AudioCapture *>(userdata);
    while (true) {
        const void *data = nullptr;
        size_t bytes = 0;
        if (pa_stream_peek(stream, &data, &bytes) < 0 || bytes == 0) break;
        if (data) {
            const auto *samples = static_cast<const int16_t *>(data);
            self->samples_.insert(self->samples_.end(), samples,
                                  samples + bytes / sizeof(int16_t));
        }
        if (pa_stream_drop(stream) < 0) break;
    }
}

void AudioCapture::recordLoop() {
    std::unique_lock<std::mutex> pulseSessionLock(pulseCaptureMutex);
    const uint64_t processingTicket = nextProcessingTicket++;
    diagnosticLog().event("audio", "capture_processing_ticket_assigned", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"ticket", std::to_string(processingTicket)}
    });
    const pa_sample_spec ss{PA_SAMPLE_S16LE, 16000, 1};
    auto t0 = std::chrono::steady_clock::now();
    pa_mainloop *mainloop = pa_mainloop_new();
    pa_context *context = nullptr;
    pa_stream *stream = nullptr;
    bool readFailed = false;
    int error = PA_OK;
    int nReads = 0;

    if (!mainloop) {
        readFailed = true;
    } else {
        {
            std::lock_guard<std::mutex> lock(pulseMutex_);
            pulseMainloop_ = mainloop;
        }
        context = pa_context_new(pa_mainloop_get_api(mainloop), "nextless-cap");
        if (!context) {
            readFailed = true;
        } else {
            pa_context_set_state_callback(context, contextStateCallback, this);
            if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
                readFailed = true;
            }
        }
    }

    while (!readFailed && !stopRequested_) {
        auto state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(state) || pa_mainloop_iterate(mainloop, 1, nullptr) < 0) {
            readFailed = true;
        }
    }

    if (!readFailed && !stopRequested_) {
        stream = pa_stream_new(context, "voice", &ss, nullptr);
        if (!stream) {
            readFailed = true;
        } else {
            pa_stream_set_state_callback(stream, streamStateCallback, this);
            pa_stream_set_read_callback(stream, streamReadCallback, this);
            pa_buffer_attr attr{};
            attr.maxlength = static_cast<uint32_t>(-1);
            attr.fragsize = 3200; // 100 ms at 16 kHz mono S16.
            if (pa_stream_connect_record(stream, nullptr, &attr,
                                         PA_STREAM_ADJUST_LATENCY) < 0) {
                readFailed = true;
            }
        }
    }

    while (!readFailed && !stopRequested_) {
        auto state = pa_stream_get_state(stream);
        if (state == PA_STREAM_READY) break;
        if (!PA_STREAM_IS_GOOD(state) || pa_mainloop_iterate(mainloop, 1, nullptr) < 0) {
            readFailed = true;
        }
    }

    auto tPaOpen = std::chrono::steady_clock::now();
    bool stopping = stopRequested_;
    auto stopDeadline = std::chrono::steady_clock::time_point::max();
    while (!readFailed && stream) {
        if (stopRequested_ && !stopping) stopping = true;
        if (stopping && stopDeadline == std::chrono::steady_clock::time_point::max()) {
            stopDeadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(120);
        }
        if (stopping && std::chrono::steady_clock::now() >= stopDeadline) break;

        int dispatched = pa_mainloop_iterate(mainloop, stopping ? 0 : 1, nullptr);
        if (dispatched < 0 || !PA_STREAM_IS_GOOD(pa_stream_get_state(stream))) {
            readFailed = true;
            break;
        }
        if (dispatched > 0) nReads += dispatched;
        if (stopping) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto tRecordEnd = std::chrono::steady_clock::now();

    if (stream) {
        pa_stream_set_read_callback(stream, nullptr, nullptr);
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
    }
    if (context) {
        error = pa_context_errno(context);
        pa_context_disconnect(context);
        pa_context_unref(context);
    }
    {
        std::lock_guard<std::mutex> lock(pulseMutex_);
        pulseMainloop_ = nullptr;
    }
    if (mainloop) pa_mainloop_free(mainloop);
    pulseSessionLock.unlock();
    diagnosticLog().event("audio", "capture_processing_wait", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"ticket", std::to_string(processingTicket)}
    });
    ProcessingTurn processingTurn(processingTicket);
    diagnosticLog().event("audio", "capture_processing_begin", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"ticket", std::to_string(processingTicket)}
    });
    auto tPaClose = std::chrono::steady_clock::now();

    if (onState_) onState_(false);

    fprintf(stderr, "Nextless Capture [timer] pa_open=%ldms record=%ldms pa_close=%ldms events=%d samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tPaOpen - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecordEnd - tPaOpen).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tPaClose - tRecordEnd).count(),
            nReads, samples_.size());

    if (readFailed) {
        diagnosticLog().event("audio", "capture_read_failed", {
            {"recognition_id", std::to_string(diagnosticId_)},
            {"capture_id", std::to_string(captureId_)},
            {"error_code", std::to_string(error)}
        });
        fprintf(stderr, "Nextless Capture: read error: %s\n", pa_strerror(error));
        if (onStatusText_) onStatusText_("Nextless: microphone read failed");
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
        unlink(wavPath_.c_str());
        finished_ = true;
        return;
    }

    std::vector<int16_t> batch;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        batch.swap(samples_);
    }

    if (!batch.empty()) {
        // Step 1: ebur128 loudness normalization
        double loudness = normalizeSamples(batch);

        // Step 2: VAD check (but don't trim yet — denoiser needs leading noise)
        bool isBlank = !hasVoice(batch);
        bool wantDenoise = !denoiseMethod_.empty();

        // Step 3: denoise (with full audio for noise profile learning)
        if (!isBlank && wantDenoise) applyDenoise(batch, denoiseMethod_);

        // Step 4: trim silence (now that denoiser has processed the noise)
        trimSilence(batch);

        // ASR providers consume the WAV; blank or unobserved captures do not
        // need a temporary file.
        if (!isBlank && onRecorded_) {
            writeWav(batch, wavPath_);
            diagnosticLog().event("audio", "capture_recorded", {
                {"recognition_id", std::to_string(diagnosticId_)},
                {"capture_id", std::to_string(captureId_)},
                {"sample_count", std::to_string(batch.size())},
                {"wav_hash", hashDiagnosticValue(wavPath_).substr(0, 16)},
                {"voice", "true"}
            });
            onRecorded_(batch, wavPath_);
        } else if (isBlank) {
            diagnosticLog().event("audio", "capture_no_speech", {
                {"recognition_id", std::to_string(diagnosticId_)},
                {"capture_id", std::to_string(captureId_)},
                {"sample_count", std::to_string(batch.size())},
                {"voice", "false"}
            });
            unlink(wavPath_.c_str());
            if (onSilence_) onSilence_();
        } else {
            unlink(wavPath_.c_str());
        }

        fprintf(stderr, "Nextless Capture [pipeline] loudness=%.1f isBlank=%d denoiser=%s samples=%zu\n",
                loudness, (int)isBlank, denoiseMethod_.c_str(), batch.size());

        {
            std::lock_guard<std::mutex> lk(sampleMutex_);
            samples_ = std::move(batch);
        }
    } else if (!readFailed && onStatusText_) {
        diagnosticLog().event("audio", "capture_no_audio", {
            {"recognition_id", std::to_string(diagnosticId_)},
            {"capture_id", std::to_string(captureId_)}
        });
        onStatusText_("Nextless: no audio captured");
    }
    size_t finalSampleCount = 0;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        finalSampleCount = samples_.size();
    }
    finished_ = true;
    diagnosticLog().event("audio", "capture_processing_end", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"ticket", std::to_string(processingTicket)},
        {"sample_count", std::to_string(finalSampleCount)},
        {"elapsed_ms", std::to_string(std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count())}
    });
}

namespace {
// DeepFilterNet 是选装的重型降噪器: 模型加载慢、CPU 占用高, 而且经常压根没装。
// 它必须永远有退路 —— 超时、进程失败、文件没被改动都退回 speexdsp,
// 绝不能让一次录音挂在子进程上, 更不能让音频静默绕过降噪。
constexpr int kDfTimeoutMs = 5000;
constexpr int kDfMaxConsecutiveFailures = 3;

// 打包者/发行版可以用 NEXTLESS_DEEP_FILTER 指向系统里的 deep-filter,
// 否则用随包安装在 ~/.local/share/nextless/bin 的那一份。
std::string dfBinaryPath() {
    const char *env = getenv("NEXTLESS_DEEP_FILTER");
    if (env && *env && access(env, X_OK) == 0) return std::string(env);
    const char *home = getenv("HOME");
    if (!home) return {};
    std::string candidate = std::string(home) + "/.local/share/nextless/bin/deep-filter";
    return access(candidate.c_str(), X_OK) == 0 ? candidate : std::string();
}

std::string dfTempDir() {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg && access(xdg, W_OK) == 0) return std::string(xdg);
    return "/tmp";
}

uint64_t fnv1a(const void *data, size_t len) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t fnv1a(const std::vector<int16_t> &v) {
    return fnv1a(v.data(), v.size() * sizeof(int16_t));
}

// 读回一个 PCM WAV: 按 chunk 找 fmt/data, 不假设头部正好 44 字节 —— deep-filter
// 之外任何实现加一个 metadata chunk 都会让写死的偏移读出错位的音频。
bool readWavPcm(const std::string &path, std::vector<int16_t> &out, uint32_t &sampleRate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() < 12) return false;
    if (memcmp(bytes.data(), "RIFF", 4) != 0 || memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return false;

    uint32_t channels = 0, sr = 0, bps = 0;
    size_t dataOffset = 0, dataBytes = 0;
    bool haveFmt = false, haveData = false;

    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        uint32_t size = 0;
        memcpy(&size, bytes.data() + pos + 4, 4);
        const size_t payload = pos + 8;
        if (payload + size > bytes.size()) size = (uint32_t)(bytes.size() - payload);
        const char *id = bytes.data() + pos;
        if (size >= 16 && memcmp(id, "fmt ", 4) == 0) {
            memcpy(&channels, bytes.data() + payload + 2, 2);
            memcpy(&sr, bytes.data() + payload + 4, 4);
            memcpy(&bps, bytes.data() + payload + 14, 2);
            haveFmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            dataOffset = payload;
            dataBytes = size;
            haveData = true;
        }
        pos = payload + size + (size & 1);
    }

    if (!haveFmt || !haveData || sr == 0 || bps != 16 || channels != 1) return false;
    if (dataBytes > bytes.size() - dataOffset) dataBytes = bytes.size() - dataOffset;
    out.assign(dataBytes / sizeof(int16_t), 0);
    if (out.empty()) return false;
    memcpy(out.data(), bytes.data() + dataOffset, out.size() * sizeof(int16_t));
    sampleRate = sr;
    return true;
}

} // namespace

void AudioCapture::applyDenoise(std::vector<int16_t> &samples, const std::string &method) {
    if (method.empty() || method == "none") return;

    if (method == "deepfilter") {
        if (dfDenoise(samples)) return;
    }
    auto t0 = std::chrono::steady_clock::now();

    constexpr int kFrameSize = 320;
    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_DENOISE, &enable);
    int level = speexLevel_;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &level);

    size_t frameCount = samples.size() / kFrameSize;
    for (size_t i = 0; i < frameCount; i++) {
        speex_preprocess_run(st, samples.data() + i * kFrameSize);
    }
    size_t remainder = samples.size() % kFrameSize;
    if (remainder > 0) {
        std::vector<int16_t> pad(samples.end() - remainder, samples.end());
        pad.resize(kFrameSize, 0);
        speex_preprocess_run(st, pad.data());
        std::copy(pad.begin(), pad.begin() + remainder, samples.end() - remainder);
    }

    speex_preprocess_state_destroy(st);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Nextless Capture [timer] denoise=%ldms samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            samples.size());
}

bool AudioCapture::dfDenoise(std::vector<int16_t> &samples) {
    std::lock_guard<std::mutex> lock(deepFilterMutex);
    auto t0 = std::chrono::steady_clock::now();

    static uint64_t dfSeq = 0;
    static int dfFailures = 0;
    static bool dfDisabled = false;

    auto giveUp = [&](const char *msg) -> bool {
        fprintf(stderr, "Nextless DF: %s\n", msg);
        diagnosticLog().event("audio", "denoise_fallback", {
            {"denoiser", "deepfilter"},
            {"reason", msg},
            {"consecutive_failures", std::to_string(dfFailures + 1)}
        });
        if (dfFailures + 1 >= kDfMaxConsecutiveFailures) {
            dfDisabled = true;
            fprintf(stderr, "Nextless DF: disabled, staying on speexdsp for this session\n");
        }
        ++dfFailures;
        return false;
    };

    const std::string binary = dfBinaryPath();
    if (binary.empty()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "Nextless DF: deep-filter not installed "
                            "(~/.local/share/nextless/bin/deep-filter), using speexdsp\n");
        }
        return false;
    }
    if (dfDisabled) return false;
    if (samples.empty()) return true;

    // Resample 16kHz → 48kHz
    size_t inLen = samples.size();
    double ratio = 48000.0 / 16000.0;
    size_t outLen48k = (size_t)(inLen * ratio) + 64;

    std::vector<float> fin(inLen);
    for (size_t i = 0; i < inLen; i++) fin[i] = samples[i] / 32768.0f;

    std::vector<float> fout(outLen48k);
    soxr_error_t err;
    soxr_t resampler = soxr_create(16000, 48000, 1, &err, nullptr, nullptr, nullptr);
    if (err) return giveUp("soxr create error");

    size_t consumed, generated;
    err = soxr_process(resampler, fin.data(), fin.size(), &consumed,
                       fout.data(), fout.size(), &generated);
    soxr_delete(resampler);
    if (err) return giveUp("soxr process error");

    std::vector<int16_t> samples48k(generated);
    for (size_t i = 0; i < generated; i++)
        samples48k[i] = (int16_t)std::clamp((int)(fout[i] * 32768.0f), -32768, 32767);

    // 每次调用一个独立临时文件: 并发录音不会互相覆盖
    const std::string tmpDir = dfTempDir();
    std::string tpl = tmpDir + "/nextless_df_" + std::to_string(getpid()) + "_"
                      + std::to_string(dfSeq++) + "_XXXXXX.wav";
    std::vector<char> name(tpl.begin(), tpl.end());
    name.push_back('\0');
    // 使用 mkstemps 创建文件并获取 fd，直接写入 WAV 数据，避免 unlink+recreate 的 race window
    int tmpFd = mkstemps(name.data(), 4);
    if (tmpFd < 0) return giveUp("cannot create a temp wav");
    const std::string tmp48k(name.data());
    struct Cleanup {
        const std::string &path;
        ~Cleanup() { unlink(path.c_str()); }
    } cleanup{tmp48k};

    // mkstemps 原子地创建文件并拿到 fd，fdopen 后直接写 WAV —— 没有
    // unlink+recreate 的时间窗（issue #25），头/数据序列化与 writeWav 共用
    // writeWavToFp（issue #29）。
    FILE *f = fdopen(tmpFd, "wb");
    if (!f) {
        close(tmpFd);
        return giveUp("cannot open temp wav for writing");
    }

    writeWavToFp(f, samples48k, 48000);
    fclose(f); // 关闭文件，deep-filter 可以读取

    const uint64_t writtenHash = fnv1a(samples48k);

    pid_t pid = -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0600);
    const char *argv[] = {binary.c_str(), "-D", "-o", tmpDir.c_str(), tmp48k.c_str(), nullptr};
    const int spawnErr = posix_spawn(&pid, binary.c_str(), &actions, nullptr,
                                     const_cast<char *const *>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawnErr != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "could not start deep-filter: %s", strerror(spawnErr));
        return giveUp(msg);
    }

    // 有界等待: 超时必须杀掉进程, 不能把调用线程挂在死掉的模型上
    int status = 0;
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return giveUp("waitpid failed");
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(kDfTimeoutMs)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return giveUp("timed out, deep-filter killed");
        }
        usleep(10 * 1000);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return giveUp("deep-filter exited with an error");
    }

    std::vector<int16_t> denoised;
    uint32_t denoisedRate = 0;
    if (!readWavPcm(tmp48k, denoised, denoisedRate)) return giveUp("processed wav is unreadable");
    if (fnv1a(denoised) == writtenHash) return giveUp("audio came back unchanged");
    dfFailures = 0;

    // Resample 处理后的采样率 → 16kHz
    std::vector<float> fIn(denoised.size());
    for (size_t i = 0; i < denoised.size(); i++) fIn[i] = denoised[i] / 32768.0f;

    size_t outLen16k = (size_t)(fIn.size() * 16000.0 / denoisedRate) + 64;
    std::vector<float> f16k(outLen16k);

    resampler = soxr_create(denoisedRate, 16000, 1, &err, nullptr, nullptr, nullptr);
    if (!err) {
        err = soxr_process(resampler, fIn.data(), fIn.size(), &consumed,
                           f16k.data(), f16k.size(), &generated);
        soxr_delete(resampler);
    }
    if (err) return giveUp("soxr downsample error");

    samples.resize(generated);
    for (size_t i = 0; i < generated; i++)
        samples[i] = (int16_t)std::clamp((int)(f16k[i] * 32768.0f), -32768, 32767);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Nextless Capture [timer] df_denoise=%ldms samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            samples.size());
    return true;
}

std::vector<int16_t> AudioCapture::takeSamples() {
    std::lock_guard<std::mutex> lk(sampleMutex_);
    return std::move(samples_);
}

double AudioCapture::normalizeSamples(std::vector<int16_t> &samples) {
    auto t0 = std::chrono::steady_clock::now();

    ebur128_state *ebur = ebur128_init(1, 16000, EBUR128_MODE_I);
    ebur128_add_frames_short(ebur, samples.data(), samples.size());
    double loudness = 0.0;
    ebur128_loudness_global(ebur, &loudness);
    ebur128_destroy(&ebur);

    double gain;
    if (loudness < -70.0 || !std::isfinite(loudness)) {
        fprintf(stderr, "Nextless Capture: audio too quiet (%.1f LUFS), skip norm\n", loudness);
        gain = 1.0;
    } else {
        gain = std::pow(10.0, (lufsTarget_ - loudness) / 20.0);
    }

    auto tEbur = std::chrono::steady_clock::now();

    for (auto &s : samples) {
        double vd = static_cast<double>(s) * gain;
        if (vd > 32767.0) vd = 32767.0;
        else if (vd < -32768.0) vd = -32768.0;
        s = static_cast<int16_t>(static_cast<int32_t>(vd));
    }

    auto tGain = std::chrono::steady_clock::now();
    fprintf(stderr, "Nextless Capture [timer] ebur128=%ldms gain=%ldms loudness=%.1f gain=%.2f\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEbur - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tGain - tEbur).count(),
            loudness, gain);

    return loudness;
}

bool AudioCapture::hasVoice(const std::vector<int16_t> &samples) {
    if (samples.empty()) return false;

    // Crest factor: speech has high peaks relative to RMS, noise is flat
    int32_t peak = 0;
    double sumSq = 0;
    for (auto s : samples) {
        sumSq += (double)s * (double)s;
        int32_t a = s >= 0 ? (int32_t)s : -(int32_t)s;
        if (a > peak) peak = a;
    }

    double rms = std::sqrt(sumSq / samples.size());
    double crestFactor = (double)peak / (rms > 0 ? rms : 1);
    fprintf(stderr, "Nextless Capture: crestFactor=%.2f crestThreshold_=%.2f peak=%d rms=%.2f %s\n",
            crestFactor, crestThreshold_, (int)peak, rms,
            crestFactor < crestThreshold_ ? "SILENCE (crest below threshold)" : "HAS VOICE (crest passes)");
    if (crestFactor < crestThreshold_) return false;

    // Secondary: speexdsp VAD must also detect voice in majority of frames
    constexpr int kFrameSize = 320;
    constexpr double kMinVoiceRatio = 0.15;
    constexpr size_t kMinVoiceFrames = 5;

    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_VAD, &enable);
    int probStart = 90;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_START, &probStart);
    int probContinue = 70;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &probContinue);

    size_t voiceCount = 0;
    size_t frameCount = samples.size() / kFrameSize;
    for (size_t i = 0; i < frameCount; i++) {
        int16_t frame[kFrameSize];
        memcpy(frame, samples.data() + i * kFrameSize, kFrameSize * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            voiceCount++;
    }
    size_t remainder = samples.size() % kFrameSize;
    if (remainder > 0) {
        int16_t frame[kFrameSize] = {};
        memcpy(frame, samples.data() + frameCount * kFrameSize, remainder * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            voiceCount++;
        frameCount++;
    }
    speex_preprocess_state_destroy(st);

    double ratio = frameCount > 0 ? (double)voiceCount / frameCount : 0;
    fprintf(stderr, "Nextless Capture: VAD crest=%.1f peak=%d voiceCount=%zu/%zu (%.1f%%)\n",
            crestFactor, (int)peak, voiceCount, frameCount, ratio * 100);

    if (voiceCount < kMinVoiceFrames) return false;
    if (ratio < kMinVoiceRatio) return false;

    return true;
}

void AudioCapture::trimSilence(std::vector<int16_t> &samples) {
    constexpr int kFrameSize = 320;
    constexpr size_t kPadFrames = 1;
    constexpr size_t kMinHeadSamples = 3200;

    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_VAD, &enable);
    int probStart = 80;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_START, &probStart);
    int probContinue = 50;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &probContinue);

    size_t frameCount = samples.size() / kFrameSize;
    std::vector<bool> voiceFrames(frameCount, false);

    for (size_t i = 0; i < frameCount; i++) {
        int16_t frame[kFrameSize];
        memcpy(frame, samples.data() + i * kFrameSize, kFrameSize * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            voiceFrames[i] = true;
    }

    speex_preprocess_state_destroy(st);

    size_t firstVoice = 0;
    while (firstVoice < frameCount && !voiceFrames[firstVoice]) firstVoice++;
    size_t lastVoice = frameCount;
    while (lastVoice > 0 && !voiceFrames[lastVoice - 1]) lastVoice--;

    if (firstVoice >= lastVoice) return;

    firstVoice = firstVoice > kPadFrames ? firstVoice - kPadFrames : 0;
    lastVoice = std::min(lastVoice + kPadFrames, frameCount);

    size_t trimStart = firstVoice * kFrameSize;
    size_t trimEnd = lastVoice * kFrameSize;

    if (trimStart < kMinHeadSamples) trimStart = kMinHeadSamples;
    if (trimStart > trimEnd) trimStart = trimEnd;

    size_t origSize = samples.size();
    if (trimStart >= origSize) return;

    if (trimStart > 0) {
        samples.erase(samples.begin(), samples.begin() + trimStart);
    }
    size_t shiftedEnd = trimEnd - trimStart;
    if (shiftedEnd < samples.size()) {
        samples.erase(samples.begin() + shiftedEnd, samples.end());
    }

    fprintf(stderr, "Nextless Capture: trimmed %zu leading + %zu trailing samples (orig=%zu now=%zu)\n",
            trimStart, origSize - trimEnd, origSize, samples.size());
}

void AudioCapture::writeWav(const std::vector<int16_t> &samples, const std::string &path,
                            uint32_t sampleRate) {
    auto t0 = std::chrono::steady_clock::now();

    // 使用 open(O_CREAT|O_EXCL|O_NOFOLLOW) 防止 symlink following，模式 0600
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fprintf(stderr, "Nextless Capture: cannot create WAV at %s: %s\n",
                path.c_str(), strerror(errno));
        return;
    }
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(path.c_str()); // 清理失败的文件
        fprintf(stderr, "Nextless Capture: cannot fdopen WAV at %s: %s\n",
                path.c_str(), strerror(errno));
        return;
    }

    writeWavToFp(f, samples, sampleRate);
    fclose(f);

    auto tWav = std::chrono::steady_clock::now();
    fprintf(stderr, "Nextless Capture [timer] wav_write=%ldms\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tWav - t0).count());
}

} // namespace nextless
