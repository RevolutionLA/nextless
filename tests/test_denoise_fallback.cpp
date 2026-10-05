// DeepFilterNet 是选装的重型降噪器, 装了不等于能用: 二进制可能不存在、进程可能卡死、
// 也可能干净退出却什么都没改。这几种情况都必须退回 speexdsp 且不能挂住调用线程,
// 只有真的拿到改动过的音频才算成功。
#include "audio_capture.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kToneSamples = 16000;
constexpr long kDfTimeoutMs = 5000;

int failures = 0;

void check(const std::string &what, bool ok) {
    if (ok) return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

void writeScript(const std::filesystem::path &path, const std::string &body) {
    std::ofstream f(path, std::ios::binary);
    f << body;
    f.close();
    ::chmod(path.c_str(), 0755);
}

std::vector<int16_t> toneSamples() {
    std::vector<int16_t> samples(kToneSamples);
    for (size_t i = 0; i < samples.size(); i++) {
        samples[i] = (int16_t)(12000.0 *
            std::sin(2.0 * 3.14159265358979323846 * 220.0 * (double)i / 16000.0));
    }
    return samples;
}

struct Result {
    bool ok = false;
    long ms = 0;
    std::vector<int16_t> samples;
};

Result run(const std::string &binary) {
    if (binary.empty()) unsetenv("NEXTLESS_DEEP_FILTER");
    else setenv("NEXTLESS_DEEP_FILTER", binary.c_str(), 1);

    Result result;
    result.samples = toneSamples();
    auto t0 = std::chrono::steady_clock::now();
    result.ok = nextless::AudioCapture::dfDenoise(result.samples);
    result.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    return result;
}

bool audioIntact(const Result &result) {
    if (result.samples.size() != kToneSamples) return false;
    return result.samples[100] != 0;
}

size_t leftoverTempFiles(const std::filesystem::path &dir) {
    std::error_code ec;
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("nextless_df_", 0) == 0) count++;
    }
    return count;
}

} // namespace

int main() {
    char directory[] = "/tmp/nextless_denoise_test_XXXXXX";
    const char *made = mkdtemp(directory);
    if (!made) {
        std::cerr << "cannot create a temp dir\n";
        return 1;
    }
    const std::filesystem::path root(made);
    // 隔离真实安装: HOME 里没有 deep-filter, 临时 wav 也落在可清点的位置
    setenv("HOME", root.c_str(), 1);
    setenv("XDG_RUNTIME_DIR", root.c_str(), 1);

    writeScript(root / "hang.sh", "#!/bin/sh\nexec sleep 600\n");
    writeScript(root / "noop.sh", "#!/bin/sh\nexit 0\n");
    // 假装处理: 保住 44 字节头, 把 PCM 全刷成 0, 文件尺寸不变
    writeScript(root / "zero.sh",
        "#!/bin/sh\n"
        "f=\"$4\"\n"
        "n=$(wc -c < \"$f\")\n"
        "head -c 44 \"$f\" > \"$f.tmp\"\n"
        "dd if=/dev/zero bs=1024 count=$(( (n - 44 + 1023) / 1024 )) >> \"$f.tmp\" 2>/dev/null\n"
        "truncate -s \"$n\" \"$f.tmp\"\n"
        "mv \"$f.tmp\" \"$f\"\n");

    const auto processed = run((root / "zero.sh").string());
    check("a working deep-filter succeeds", processed.ok);
    check("a working deep-filter is prompt", processed.ms < 3000);
    bool silenced = !processed.samples.empty();
    for (auto value : processed.samples) {
        if (value != 0) { silenced = false; break; }
    }
    check("processed audio replaces the input", silenced);

    const auto missing = run("");
    check("an uninstalled deep-filter is skipped", !missing.ok);
    check("an uninstalled deep-filter is instant", missing.ms < 200);
    check("an uninstalled deep-filter keeps the audio", audioIntact(missing));

    const auto noop = run((root / "noop.sh").string());
    check("audio that comes back unchanged is rejected", !noop.ok);
    check("an unmodified file is caught promptly", noop.ms < 3000);
    check("an unmodified file keeps the audio", audioIntact(noop));

    const auto hung = run((root / "hang.sh").string());
    check("a wedged deep-filter does not count as success", !hung.ok);
    check("a wedged deep-filter is killed at the deadline",
          hung.ms >= kDfTimeoutMs - 100 && hung.ms < kDfTimeoutMs + 4000);
    check("a wedged deep-filter keeps the audio", audioIntact(hung));

    run((root / "noop.sh").string());  // 第三次失败 -> 本会话不再尝试
    const auto disabled = run((root / "zero.sh").string());
    check("repeated failures disable deep-filter", !disabled.ok);
    check("a disabled deep-filter costs nothing", disabled.ms < 200);

    std::vector<int16_t> samples = toneSamples();
    nextless::AudioCapture::applyDenoise(samples, "deepfilter");
    check("applyDenoise falls back instead of returning nothing", !samples.empty());

    check("no temp wav is left behind", leftoverTempFiles(root) == 0);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    if (failures == 0) std::cout << "denoise fallback: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
