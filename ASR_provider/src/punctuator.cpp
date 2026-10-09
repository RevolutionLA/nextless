#include "punctuator.h"
#include "nextless_config.h"
#include "diagnostic_log.h"

#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace nextless {
namespace {

constexpr const char *kDefaultModelDir =
    "~/.local/share/nextless/models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8";
constexpr const char *kDefaultBinPath =
    "~/.local/share/nextless/sherpa-onnx/bin/sherpa-onnx-offline-punctuation";

std::string expandTilde(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\n\r");
    return s.substr(b, e - b + 1);
}

// 哪些 providerId 需要补标点: 两个本地后端。云后端自带标点, mock 是
// 测试桩, 都必须原样通过 (issue #4: no-op for the cloud backends)。
bool needsPunctuation(const std::string &providerId) {
    return providerId == "zipformer" || providerId == "fire_red";
}

// 连续失败多少次就放弃本会话。和 DeepFilter 的 kDfMaxConsecutiveFailures 同
// 一个数、同一套语义：坏掉的标点二进制不会自己变好，却能让之后的每一句话都
// 白等满 timeout_sec（健康路径 0.2 s，坏路径 5 s —— 差 25 倍且没有面板可读）。
constexpr int kPunctMaxConsecutiveFailures = 3;

} // namespace

bool runPunctuation(const std::string &binPath, const std::string &modelPath,
                    const std::string &text, std::string &out, int timeoutSec) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) return false;

    std::vector<std::string> args = {
        binPath,
        "--ct-transformer=" + modelPath,
        "--provider=cpu",
        text
    };
    std::vector<const char *> argv;
    for (auto &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull < 0) devnull = STDERR_FILENO;  // 兜底: 别把管道当 stderr 用

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    if (devnull != STDERR_FILENO)
        posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    pid_t pid;
    int ret = posix_spawn(&pid, binPath.c_str(), &actions, &attr,
                          (char *const *)argv.data(), ::environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (devnull != STDERR_FILENO) close(devnull);

    if (ret != 0) {
        close(pipefd[0]);
        return false;
    }

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    std::string output;
    std::array<char, 4096> buf{};
    int status = 0;
    bool reaped = false;
    bool timedOut = false;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(std::max(timeoutSec, 1));

    while (!reaped) {
        ssize_t n;
        while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) {
            output.append(buf.data(), (size_t)n);
        }
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            reaped = true;
            break;
        }
        if (waited < 0 && errno != EINTR) break;

        timedOut = std::chrono::steady_clock::now() >= deadline;
        if (timedOut) {
            kill(-pid, SIGTERM);
            for (int i = 0; i < 20 && !reaped; i++) {
                waited = waitpid(pid, &status, WNOHANG);
                if (waited == pid) reaped = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            kill(-pid, SIGKILL);
            if (!reaped) {
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
            break;
        }
        pollfd pfd = {pipefd[0], POLLIN, 0};
        (void)poll(&pfd, 1, 100);
    }

    if (!timedOut) {
        ssize_t n;
        while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) {
            output.append(buf.data(), (size_t)n);
        }
    }
    close(pipefd[0]);

    if (!reaped || timedOut || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }
    // stdout 只有一行: 带标点的文本(诊断信息全在 stderr, 见 issue #4
    // 调研时的实测输出)。取最后一个非空行, 防上游哪天加横幅也不受影响。
    std::string result;
    size_t pos = 0;
    while (pos < output.size()) {
        size_t nl = output.find('\n', pos);
        if (nl == std::string::npos) nl = output.size();
        std::string line = trim(output.substr(pos, nl - pos));
        if (!line.empty()) result = std::move(line);
        pos = nl + 1;
    }
    if (result.empty()) return false;
    out = std::move(result);
    return true;
}

void punctuateLocalText(std::string &text, const std::string &providerId) {
    if (text.empty() || !needsPunctuation(providerId)) return;
    // 以 '-' 开头的文本会被 parse-options 当成 flag; 概率极低, 但宁可跳过。
    if (text.front() == '-') return;

    auto adv = advancedSection("punctuation");
    bool enabled = true;
    int timeoutSec = 5;
    std::string modelDir = kDefaultModelDir;
    std::string binPath = kDefaultBinPath;
    if (!adv.empty()) {
        enabled = jsonBool(adv, "enabled", enabled);
        auto d = jsonStr(adv, "model_dir");
        if (!d.empty()) modelDir = d;
        auto b = jsonStr(adv, "bin_path");
        if (!b.empty()) binPath = b;
        timeoutSec = jsonInt(adv, "timeout_sec", timeoutSec);
    }
    if (!enabled) return;

    // 只在识别线程调用（见 punctuator.h），所以会话状态用函数内静态即可。
    static int consecutiveFailures = 0;
    static bool disabledForSession = false;
    if (disabledForSession) return;

    const std::string bin = expandTilde(binPath);
    const std::string dir = expandTilde(modelDir);
    // int8 是推荐下载(62 MB), 也接受 fp32 目录(266 MB)——按存在的来。
    std::string model = dir + "/model.int8.onnx";
    if (access(model.c_str(), R_OK) != 0) model = dir + "/model.onnx";

    if (access(bin.c_str(), X_OK) != 0 || access(model.c_str(), R_OK) != 0) {
        // 缺标点模型不是故障: 原样提交, 只在日志留一次线索。
        diagnosticLog().event("punctuator", "skipped_missing_pieces", {});
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    std::string punctuated;
    if (!runPunctuation(bin, model, text, punctuated, timeoutSec)) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "Nextless Punctuator: kept raw text after %ldms\n", ms);
        ++consecutiveFailures;
        diagnosticLog().event("punctuator", "failed_kept_raw",
                              {{"elapsed_ms", std::to_string(ms)},
                               {"consecutive_failures",
                                std::to_string(consecutiveFailures)}});
        if (consecutiveFailures >= kPunctMaxConsecutiveFailures) {
            disabledForSession = true;
            fprintf(stderr, "Nextless Punctuator: %d consecutive failures, giving up "
                            "for this session (raw text from now on; restart fcitx5 "
                            "to retry, or set punctuation.enabled=false)\n",
                    consecutiveFailures);
            diagnosticLog().event("punctuator", "disabled_for_session",
                                  {{"consecutive_failures",
                                    std::to_string(consecutiveFailures)}});
        }
        return;
    }
    consecutiveFailures = 0;
    text = std::move(punctuated);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "Nextless [timer] punctuation=%ldms\n", ms);
    diagnosticLog().event("punctuator", "applied",
                          {{"elapsed_ms", std::to_string(ms)}});
}

} // namespace nextless
