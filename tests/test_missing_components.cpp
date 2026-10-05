// 缺运行时/缺模型时, provider 的 error 要说清缺了什么、指到 README 的
// "Get models (local backends)"; 面板映射 (adapter/src/panel_status.h) 要把它们变成
// 用户能照做的提示。全部用假 HOME + 空壳二进制驱动, 不需要真的 sherpa-onnx。

#include "asr_provider.h"
#include "fire_red_provider.h"
#include "panel_status.h"
#include "zipformer_provider.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct Capture {
    std::mutex mutex;
    std::condition_variable cv;
    std::string error;
    bool fired = false;

    nextless::AsrErrorCallback callback() {
        return [this](const std::string &e) {
            std::lock_guard<std::mutex> lock(mutex);
            error = e;
            fired = true;
            cv.notify_one();
        };
    }

    bool wait() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [this] { return fired; });
    }
};

bool expectMissing(const fs::path &base, const std::string &name,
                   std::unique_ptr<nextless::IAsrProvider> provider,
                   const std::string &marker, const std::string &piece,
                   const std::string &panelNeedle) {
    auto wav = base / (name + ".wav");
    std::ofstream(wav) << "placeholder";

    Capture capture;
    provider->setErrorCallback(capture.callback());
    provider->transcribe({}, wav.string());
    if (!capture.wait()) {
        std::cerr << name << ": no error callback\n";
        return false;
    }
    if (capture.error.find(marker) == std::string::npos) {
        std::cerr << name << ": error does not name the failure: " << capture.error << "\n";
        return false;
    }
    if (capture.error.find(piece) == std::string::npos) {
        std::cerr << name << ": error does not name the missing piece: " << capture.error << "\n";
        return false;
    }
    if (capture.error.find(base.string()) == std::string::npos) {
        std::cerr << name << ": error does not carry the configured path: " << capture.error << "\n";
        return false;
    }
    if (capture.error.find("Get models (local backends)") == std::string::npos) {
        std::cerr << name << ": error does not point at the README: " << capture.error << "\n";
        return false;
    }

    std::string panel = nextless::panelStatusForError(capture.error);
    if (panel.find(panelNeedle) == std::string::npos ||
        panel.find("nextless-get-models") == std::string::npos) {
        std::cerr << name << ": panel status lost the hint: " << panel << "\n";
        return false;
    }

    if (fs::exists(wav)) {
        std::cerr << name << ": temp WAV survived the error\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    fs::path base = (tmp && *tmp)
                        ? fs::path(tmp) / "missing-components"
                        : fs::temp_directory_path() /
                              ("nextless-missing-" + std::to_string(getpid()));
    fs::remove_all(base);
    setenv("HOME", base.c_str(), 1);

    bool ok = true;

    // 1) 什么都没装: 先报缺运行时, 错误里带 ~ 展开后的真实路径
    ok = expectMissing(base, "zipformer-no-runtime",
                       std::make_unique<nextless::ZipformerAsrProvider>(),
                       "sherpa-onnx runtime not found at",
                       "sherpa-onnx/bin/sherpa-onnx", "runtime missing") && ok;
    ok = expectMissing(base, "fire-red-no-runtime",
                       std::make_unique<nextless::FireRedAsrProvider>(),
                       "sherpa-onnx runtime not found at",
                       "sherpa-onnx/bin/sherpa-onnx-offline", "runtime missing") && ok;

    // 2) 运行时就位、模型没下载: 报缺的是哪个模型文件
    auto fakeBin = base / ".local/share/nextless/sherpa-onnx/bin";
    fs::create_directories(fakeBin);
    for (const char *name : {"sherpa-onnx", "sherpa-onnx-offline"}) {
        std::ofstream(fakeBin / name) << "placeholder\n";
        fs::permissions(fakeBin / name, fs::perms::owner_all);
    }
    ok = expectMissing(base, "zipformer-no-model",
                       std::make_unique<nextless::ZipformerAsrProvider>(),
                       "model file not found at", "encoder-epoch-99-avg-1.onnx",
                       "model missing") && ok;
    ok = expectMissing(base, "fire-red-no-model",
                       std::make_unique<nextless::FireRedAsrProvider>(),
                       "model file not found at", "encoder.int8.onnx",
                       "model missing") && ok;

    // 3) 面板映射本身: no speech 复位、网络可重试、未知错误兜底
    if (!nextless::panelStatusForError("Zipformer: no speech").empty()) {
        std::cerr << "no speech must reset the panel\n";
        ok = false;
    }
    if (nextless::panelStatusForError("Doubao: network unreachable").find("network") ==
        std::string::npos) {
        std::cerr << "network error must keep its retry hint\n";
        ok = false;
    }
    if (nextless::panelStatusForError("Doubao: recognition failed (500)") !=
        "Nextless: recognition failed") {
        std::cerr << "unknown errors must fall back to the generic message\n";
        ok = false;
    }

    fs::remove_all(base);
    return ok ? 0 : 1;
}
