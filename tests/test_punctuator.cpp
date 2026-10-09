// Issue #4: local punctuation post-processing.
// Punctuating is decoration: it must apply to the two local backends only,
// must never eat or block an utterance (every failure mode keeps the raw
// text), and must respect the advanced.json switches. Drives the real
// spawn path with a fake shell binary, so it runs headless in CI.
#include "punctuator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <system_error>
#include <thread>
#include <sys/wait.h>

namespace fs = std::filesystem;

static int failures = 0;

static void check(const std::string &what, bool ok) {
    if (ok) return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

// Issue #47: asserting "it timed out and the raw text survived" is not enough -
// delete the SIGKILL escalation from runPunctuation and that test still passes
// while production leaks one process per utterance. So the fake script records
// the pid of the grandchild it spawns, and we check the process group actually
// died: gone from /proc, or dead-but-unreaped (Z). Anything else is a leak.
static bool childProcessGone(pid_t pid) {
    if (pid <= 0) return false;
    std::error_code ec;
    if (!fs::exists(fs::path("/proc") / std::to_string(pid), ec)) return true;
    std::ifstream f(fs::path("/proc") / (std::to_string(pid) + "/stat"));
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    auto rp = content.rfind(')');
    if (rp == std::string::npos || rp + 2 >= content.size()) return false;
    return content[rp + 2] == 'Z';
}

static pid_t readPidFile(const fs::path &path) {
    std::ifstream f(path);
    long v = -1;
    if (f >> v) return static_cast<pid_t>(v);
    return -1;
}

static void writeFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

int main() {
    auto base = fs::temp_directory_path() /
                ("nextless-punctuator-" + std::to_string(getpid()));
    fs::remove_all(base);
    setenv("HOME", base.c_str(), 1);

    auto binPath = base / "bin/fake-offline-punctuation";
    auto modelDir = base / "models/punct";
    fs::create_directories(modelDir);
    writeFile(modelDir / "model.int8.onnx", "fake model");

    // Echoes "text。" when the model flag points at a readable file; the
    // FAKE_MODE env selects the failure shapes.
    writeFile(binPath,
              "#!/bin/sh\n"
              "for a in \"$@\"; do\n"
              "  case \"$a\" in\n"
              "    --ct-transformer=*) m=\"${a#*=}\" ;;\n"
              "    -*) ;;\n"
              "    *) t=\"$a\" ;;\n"
              "  esac\n"
              "done\n"
              "[ -f \"$m\" ] || exit 2\n"
              "if [ \"$FAKE_MODE\" = fail ]; then echo boom >&2; exit 1; fi\n"
              "if [ \"$FAKE_MODE\" = empty ]; then exit 0; fi\n"
              // 超时用的形态。两层都忽略 SIGTERM（ignored  dispositions 会穿过
              // fork+exec 传给孙进程），所以只有走到 SIGKILL 升级才杀得掉；同时
              // 把孙进程 pid 记下来，事后能验证整个进程组确实没了。6 秒是自爆
              // 兜底：万一回归把 SIGKILL 删了，测试是失败而不是挂死。
              "if [ \"$FAKE_MODE\" = sleep ]; then\n"
              "  trap '' TERM\n"
              "  sh -c 'trap \"\" TERM; exec sleep 30' & echo \"$!\" > \"$FAKE_PIDFILE\"\n"
              "  wait\n"
              "  printf late; exit 0\n"
              "fi\n"
              "printf '%s。\\n' \"$t\"\n");
    fs::permissions(binPath, fs::perms::owner_all);

    auto configPath = base / ".config/nextless/advanced.json";
    writeFile(configPath,
              "{\"punctuation\":{\"model_dir\":\"" + modelDir.string() +
              "\",\"bin_path\":\"" + binPath.string() + "\"}}");

    // 1) local backends get punctuated
    std::string text = "你好吗";
    nextless::punctuateLocalText(text, "zipformer");
    check("zipformer result was not punctuated: " + text, text == "你好吗。");

    text = "今天天气不错";
    nextless::punctuateLocalText(text, "fire_red");
    check("fire_red result was not punctuated: " + text, text == "今天天气不错。");

    // 2) cloud backends and mock are a hard no-op (they ship their own
    //    punctuation; double-processing would also slow every utterance)
    text = "hello there";
    nextless::punctuateLocalText(text, "doubao");
    check("doubao must not be punctuated: " + text, text == "hello there");
    text = "hello world";
    nextless::punctuateLocalText(text, "mock");
    check("mock must not be punctuated: " + text, text == "hello world");

    // 3) child exits non-zero → raw text kept
    text = "出错了也要有字";
    setenv("FAKE_MODE", "fail", 1);
    nextless::punctuateLocalText(text, "zipformer");
    check("failed child must keep raw text: " + text, text == "出错了也要有字");

    // 4) child prints nothing → raw text kept
    text = "空输出";
    setenv("FAKE_MODE", "empty", 1);
    nextless::punctuateLocalText(text, "zipformer");
    check("empty stdout must keep raw text: " + text, text == "空输出");
    unsetenv("FAKE_MODE");

    // 5) timeout → runPunctuation returns false AND the whole process group is
    //    gone. The fake script ignores SIGTERM and its child inherits that, so
    //    only the SIGKILL escalation can clear it - which is what makes this
    //    assertion sensitive to that line being removed (issue #47).
    auto pidFile = base / "sleep.pid";
    setenv("FAKE_PIDFILE", pidFile.c_str(), 1);
    setenv("FAKE_MODE", "sleep", 1);
    std::string out;
    auto tTimeout0 = std::chrono::steady_clock::now();
    bool ok = nextless::runPunctuation(binPath.string(), (modelDir / "model.int8.onnx").string(),
                                       "慢", out, 1);
    check("sleeping child must time out", !ok && out.empty());
    auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - tTimeout0).count();
    // 1s budget + 500ms SIGTERM grace + slack. If the SIGKILL escalation is
    // removed this only returns when the 30s sleep dies by itself, so the
    // duration is what proves we killed it rather than waited it out.
    check("the timeout path waited for the child to die on its own (" +
              std::to_string(timeoutMs) + "ms)",
          timeoutMs < 5000);
    pid_t grandson = readPidFile(pidFile);
    check("timeout path never recorded a child pid (fake script broken?)", grandson > 0);
    bool reapedByNow = false;
    for (int i = 0; i < 20 && !reapedByNow; i++) {
        reapedByNow = childProcessGone(grandson);
        if (!reapedByNow) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check("timed-out child survived the timeout (process-group kill missing?)",
          reapedByNow);
    int leftover = 0, st = 0;
    while (waitpid(-1, &st, WNOHANG) > 0) ++leftover;
    check("timed-out child was killed but never waited for", leftover == 0);
    unsetenv("FAKE_PIDFILE");
    unsetenv("FAKE_MODE");

    // 6) model file missing → skipped before even spawning, raw kept
    fs::remove(modelDir / "model.int8.onnx");
    text = "没模型就原样";
    nextless::punctuateLocalText(text, "zipformer");
    check("missing model must keep raw text: " + text, text == "没模型就原样");
    writeFile(modelDir / "model.int8.onnx", "fake model");

    // 7) enabled=false in advanced.json is respected
    writeFile(configPath,
              "{\"punctuation\":{\"enabled\":false,\"model_dir\":\"" +
              modelDir.string() + "\",\"bin_path\":\"" + binPath.string() + "\"}}");
    text = "关掉就不动";
    nextless::punctuateLocalText(text, "zipformer");
    check("enabled:false must keep raw text: " + text, text == "关掉就不动");

    // 8) text starting with '-' would be parsed as a flag by sherpa
    writeFile(configPath,
              "{\"punctuation\":{\"model_dir\":\"" + modelDir.string() +
              "\",\"bin_path\":\"" + binPath.string() + "\"}}");
    text = "-dash-start";
    nextless::punctuateLocalText(text, "zipformer");
    check("leading-dash text must be passed through: " + text,
          text == "-dash-start");

    // 9) fp32 layout fallback: only model.onnx present
    fs::remove(modelDir / "model.int8.onnx");
    writeFile(modelDir / "model.onnx", "fake fp32");
    text = "用fp32也行";
    nextless::punctuateLocalText(text, "zipformer");
    check("model.onnx fallback failed: " + text, text == "用fp32也行。");

    // 10) consecutive-failure breaker (issue #47). Before this, a broken or
    //     quarantined punctuation binary cost every single utterance the full
    //     timeout_sec, forever - DeepFilter already gave up after 3 failures and
    //     users learn that shape once, so punctuation follows it. Must stay the
    //     last case: the breaker is session state and does not reset.
    {
        std::string reset = "先成功一次清零";
        nextless::punctuateLocalText(reset, "zipformer");
        check("a healthy run must clear the failure count: " + reset,
              reset == "先成功一次清零。");
    }

    setenv("FAKE_MODE", "fail", 1);
    for (int i = 1; i <= 3; i++) {
        std::string t = "坏掉第" + std::to_string(i) + "次";
        nextless::punctuateLocalText(t, "zipformer");
        check("a failing punctuator must still keep raw text: " + t,
              t == "坏掉第" + std::to_string(i) + "次");
    }
    unsetenv("FAKE_MODE");

    auto tBreak = std::chrono::steady_clock::now();
    std::string afterBreaker = "熔断之后";
    nextless::punctuateLocalText(afterBreaker, "zipformer");
    auto msBreak = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - tBreak).count();
    check("three consecutive failures did not disable punctuation: " + afterBreaker,
          afterBreaker == "熔断之后");
    check("a disabled punctuator still spawned something (took " +
              std::to_string(msBreak) + "ms)",
          msBreak < 200);

    fs::remove_all(base);
    return failures == 0 ? 0 : 1;
}
