#include "zipformer_provider.h"
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
#include <cstdio>
#include <chrono>
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

ZipformerAsrProvider::ZipformerAsrProvider()
    : modelDir_("~/.local/share/nextless/models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20") {
    auto adv = advancedSection("zipformer");
    if (!adv.empty()) {
        auto d = jsonStr(adv, "model_dir");
        if (!d.empty()) modelDir_ = d;
        numThreads_ = jsonInt(adv, "num_threads", numThreads_);
        timeoutSec_ = jsonInt(adv, "timeout_sec", timeoutSec_);
        auto b = jsonStr(adv, "bin_path");
        if (!b.empty()) sherpaBin_ = b;
    }
}

ZipformerAsrProvider::~ZipformerAsrProvider() {
    diagnosticLog().event("provider", "provider_shutdown", {
        {"provider", "zipformer"},
        {"recognition_id", std::to_string(diagnosticId_)}
    });
    cancel_->store(true);
    joinAsrWorker(worker_);
}

void ZipformerAsrProvider::setConfig(const std::string &key,
                                      const std::string &value) {
    if (key == "model_dir") {
        modelDir_ = value;
    }
}

void ZipformerAsrProvider::transcribe(std::vector<int16_t>, const std::string &wavPath) {
    diagnosticLog().event("provider", "request_started", {
        {"provider", "zipformer"},
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

void ZipformerAsrProvider::runTranscribe(const std::string &wav,
                                         const std::string &dir,
                                         std::string sherpaBin, int numThreads,
                                         int timeoutSec,
                                         std::shared_ptr<std::atomic_bool> cancel,
                                         AsrResultCallback onResultRaw,
                                         AsrErrorCallback onErrorRaw,
                                         uint64_t diagnosticId) {
    auto t0 = std::chrono::steady_clock::now();
    // The temp WAV is deleted before either callback fires — see temp_wav.h. The wrappers keep
    // every `onE(...)` call site below unchanged, and they replace the manual unlink()
    // calls that used to sit right before the callbacks (and missed the early returns).
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
            onE("Zipformer: pipe failed");
            return;
        }

        std::vector<std::string> args = {
            sherpaBin,
            "--encoder=" + dir + "/encoder-epoch-99-avg-1.onnx",
            "--decoder=" + dir + "/decoder-epoch-99-avg-1.onnx",
            "--joiner="  + dir + "/joiner-epoch-99-avg-1.onnx",
            "--tokens="  + dir + "/tokens.txt",
            "--provider=cpu",
            "--num-threads=" + std::to_string(numThreads),
            wav
        };
        std::vector<const char*> argv;
        for (auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
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
            onE("Zipformer: spawn failed");
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
                    {"provider", "zipformer"},
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
                if (timedOut) onE("Zipformer: recognition timed out");
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
            fprintf(stderr, "Nextless Zipformer: waitpid failed\n");
            onE("Zipformer: recognition failed");
            return;
        }
        auto tRecv = std::chrono::steady_clock::now();

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Nextless Zipformer: child exit=%d\n",
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            onE("Zipformer: recognition failed");
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
        fprintf(stderr, "Nextless Zipformer [timer] exec_total=%ldms parse=%ldms text_len=%zu\n",
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecv - t0).count(),
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tRecv).count(),
                text.size());

        if (!isBlankAsrText(text)) {
            diagnosticLog().event("provider", "request_result", {
                {"provider", "zipformer"},
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
                {"provider", "zipformer"},
                {"recognition_id", std::to_string(diagnosticId)},
                {"reason", "no_speech"}
            });
            onE("Zipformer: no speech");
        }
}

std::unique_ptr<IAsrProvider> ZipformerAsrProviderFactory::create() {
    return std::make_unique<ZipformerAsrProvider>();
}

static bool _zipReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<ZipformerAsrProviderFactory>());
    return true;
}();

} // namespace nextless
