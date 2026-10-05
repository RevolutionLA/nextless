#include "fire_red_provider.h"
#include "nextless_config.h"
#include "diagnostic_log.h"
#include "temp_wav.h"

#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <string_view>
#include <thread>
#include <string>
#include <vector>

namespace nextless {

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

// FireRed 的控制记号: 模型 tokens.txt 里全部非语音项(静音、语言与方言标签)。
// 只认这张表, 是为了保住用户真的在念代码时写下的 "<int>" 之类内容。
constexpr std::array<std::string_view, 40> kFireRedMarkers = {
    "<blank>", "<unk>", "<pad>", "<sos>", "<eos>", "<sil>",
    "<unk_lang>", "<unk_lang2>", "<zh>", "<en>", "<zh_en>",
    "<predict_lid>", "<not_predict_lid>",
    "<xinan>", "<yue>", "<wu>", "<minnan>", "<p2>", "<p3>",
    "<anhui>", "<fujian>", "<gansu>", "<guizhou>", "<hebei>", "<henan>",
    "<hubei>", "<hunan>", "<jiangxi>", "<liaoning>", "<ningxia>",
    "<shaanxi>", "<shandong>", "<shanghai>", "<shanxi>", "<sichuan>",
    "<tianjin>", "<wenzhou>", "<yunnan>", "<guangdong>", "<hongkong>",
};

void stripControlTokens(std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '<') {
            const auto end = text.find('>', i + 1);
            if (end != std::string::npos && end - i <= 32) {
                const std::string_view token(text.data() + i, end - i + 1);
                if (std::find(kFireRedMarkers.begin(), kFireRedMarkers.end(), token) !=
                    kFireRedMarkers.end()) {
                    i = end + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i++]);
    }
    text = std::move(out);
}

FireRedAsrProvider::FireRedAsrProvider()
    : modelDir_("~/.local/share/nextless/models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26") {
    auto adv = advancedSection("fire_red");
    if (!adv.empty()) {
        auto d = jsonStr(adv, "model_dir");
        if (!d.empty()) modelDir_ = d;
        numThreads_ = jsonInt(adv, "num_threads", numThreads_);
        timeoutSec_ = jsonInt(adv, "timeout_sec", timeoutSec_);
        auto b = jsonStr(adv, "bin_path");
        if (!b.empty()) sherpaBin_ = b;
    }
}

FireRedAsrProvider::~FireRedAsrProvider() {
    diagnosticLog().event("provider", "provider_shutdown", {
        {"provider", "fire_red"},
        {"recognition_id", std::to_string(diagnosticId_)}
    });
    cancel_->store(true);
    joinAsrWorker(worker_);
}

void FireRedAsrProvider::setConfig(const std::string &key,
                                    const std::string &value) {
    if (key == "model_dir") {
        modelDir_ = value;
    }
}

void FireRedAsrProvider::transcribe(std::vector<int16_t>, const std::string &wavPath) {
    diagnosticLog().event("provider", "request_started", {
        {"provider", "fire_red"},
        {"recognition_id", std::to_string(diagnosticId_)},
        {"wav_hash", hashDiagnosticValue(wavPath).substr(0, 16)}
    });
    cancel_->store(true);
    joinAsrWorker(worker_);
    cancel_ = std::make_shared<std::atomic_bool>(false);
    startAsrWorker(
        worker_,
        [wavPath, modelDir = expandPath(modelDir_),
         sherpaBin = expandPath(sherpaBin_), numThreads = numThreads_,
          timeoutSec = timeoutSec_, cancel = cancel_, onR = onResult_,
          onE = onError_, diagnosticId = diagnosticId_]() mutable {
            runTranscribe(wavPath, modelDir, std::move(sherpaBin), numThreads,
                          timeoutSec, std::move(cancel), std::move(onR),
                          std::move(onE), diagnosticId);
        });
}

void FireRedAsrProvider::runTranscribe(const std::string &wav,
                                         const std::string &dir,
                                         std::string sherpaBin, int numThreads,
                                         int timeoutSec,
                                         std::shared_ptr<std::atomic_bool> cancel,
                                         AsrResultCallback onResultRaw,
                                         AsrErrorCallback onErrorRaw,
                                         uint64_t diagnosticId) {
    auto t0 = std::chrono::steady_clock::now();
    // Temp WAV outlives nothing: deleted before either callback fires — see temp_wav.h.
    nextless::TempWav wavFile(wav);
    const AsrResultCallback onResult = std::move(onResultRaw);
    const AsrErrorCallback onError = std::move(onErrorRaw);
    auto onR = [&wavFile, onResult](const std::string &text, bool isFinal) {
        wavFile.drop();
        if (onResult) onResult(text, isFinal);
    };
    auto onE = [&wavFile, onError](const std::string &error) {
        wavFile.drop();
        if (onError) onError(error);
    };

        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC) < 0) {
            onE("FireRed: pipe failed");
            return;
        }

        std::vector<std::string> args = {
            sherpaBin,
            "--fire-red-asr-encoder=" + dir + "/encoder.int8.onnx",
            "--fire-red-asr-decoder=" + dir + "/decoder.int8.onnx",
            "--tokens=" + dir + "/tokens.txt",
            "--num-threads=" + std::to_string(numThreads),
            wav
        };
        std::vector<const char*> argv;
        for (auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);

        pid_t pid;
        int ret = posix_spawn(&pid, sherpaBin.c_str(), &actions, &attr,
                              (char *const *)argv.data(), ::environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[1]);

        if (ret != 0) {
            close(pipefd[0]);
            onE("FireRed: spawn failed");
            return;
        }

        int flags = fcntl(pipefd[0], F_GETFL, 0);
        if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

        std::string output;
        char buf[4096];
        int status = 0;
        bool reaped = false;
        bool timedOut = false;
        auto deadline = t0 + std::chrono::seconds(std::max(timeoutSec, 1));

        while (!reaped) {
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                output.append(buf, (size_t)n);
            }

            pid_t waitResult = waitpid(pid, &status, WNOHANG);
            if (waitResult == pid) {
                reaped = true;
                break;
            }
            if (waitResult < 0 && errno != EINTR) break;

            timedOut = std::chrono::steady_clock::now() >= deadline;
            if (cancel->load() || timedOut) {
                diagnosticLog().event("provider", timedOut ? "request_timeout" : "request_cancelled", {
                    {"provider", "fire_red"},
                    {"recognition_id", std::to_string(diagnosticId)},
                    {"stage", "process"}
                });
                kill(-pid, SIGTERM);
                for (int i = 0; i < 20; i++) {
                    waitResult = waitpid(pid, &status, WNOHANG);
                    if (waitResult == pid) {
                        reaped = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                // The leader may exit while a descendant ignores SIGTERM.
                kill(-pid, SIGKILL);
                if (!reaped) {
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                }
                close(pipefd[0]);
                if (timedOut) onE("FireRed: recognition timed out");
                return;
            }

            pollfd pfd = {pipefd[0], POLLIN, 0};
            (void)poll(&pfd, 1, 100);
        }

        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            output.append(buf, (size_t)n);
        }
        close(pipefd[0]);

        if (!reaped) {
            fprintf(stderr, "Nextless FireRed: waitpid failed\n");
            onE("FireRed: recognition failed");
            return;
        }
        auto tRecv = std::chrono::steady_clock::now();

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Nextless FireRed: child exit=%d\n",
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            onE("FireRed: recognition failed");
            return;
        }

        auto pos = output.rfind("\"text\"");
        std::string text;
        if (pos != std::string::npos) {
            pos += 9;
            auto end = output.find('"', pos);
            if (end != std::string::npos)
                text = output.substr(pos, end - pos);
        }

        auto tParse = std::chrono::steady_clock::now();
        fprintf(stderr, "Nextless FireRed [timer] exec_total=%ldms parse=%ldms text_len=%zu\n",
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecv - t0).count(),
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tRecv).count(),
                text.size());

        stripControlTokens(text);
        if (!isBlankAsrText(text)) {
            diagnosticLog().event("provider", "request_result", {
                {"provider", "fire_red"},
                {"recognition_id", std::to_string(diagnosticId)},
                {"text_length", std::to_string(text.size())},
                {"exec_ms", std::to_string(std::chrono::duration_cast<
                    std::chrono::milliseconds>(tRecv - t0).count())},
                {"parse_ms", std::to_string(std::chrono::duration_cast<
                    std::chrono::milliseconds>(tParse - tRecv).count())}
            });
            onR(text, true);
        } else {
            diagnosticLog().event("provider", "request_error", {
                {"provider", "fire_red"},
                {"recognition_id", std::to_string(diagnosticId)},
                {"reason", "no_speech"}
            });
            onE("FireRed: no speech");
        }
}

std::unique_ptr<IAsrProvider> FireRedAsrProviderFactory::create() {
    return std::make_unique<FireRedAsrProvider>();
}

static bool _frReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<FireRedAsrProviderFactory>());
    return true;
}();

} // namespace nextless
