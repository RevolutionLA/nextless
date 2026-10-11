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

    // 真实 deep-filter 会砍掉结尾：每个文件正好少 1440 个 48k 采样（30 ms），且与
    // 输入零时移。chop.sh 复刻这个形状（头里的长度故意不修，readWavPcm 会按文件
    // 实际大小夹住）。gut.sh 是返回只剩一半的模型 —— 那种结果必须算失败。
    writeScript(root / "chop.sh",
        "#!/bin/sh\n"
        "f=\"$4\"\n"
        "n=$(wc -c < \"$f\")\n"
        "body=$(( n - 44 - 2880 ))\n"
        "head -c 44 \"$f\" > \"$f.tmp\"\n"
        "dd if=/dev/zero bs=1024 count=$(( (body + 1023) / 1024 )) >> \"$f.tmp\" 2>/dev/null\n"
        "truncate -s $(( 44 + body )) \"$f.tmp\"\n"
        "mv \"$f.tmp\" \"$f\"\n");
    writeScript(root / "gut.sh",
        "#!/bin/sh\n"
        "f=\"$4\"\n"
        "n=$(wc -c < \"$f\")\n"
        "body=$(( (n - 44) / 2 ))\n"
        "head -c 44 \"$f\" > \"$f.tmp\"\n"
        "dd if=/dev/zero bs=1024 count=$(( (body + 1023) / 1024 )) >> \"$f.tmp\" 2>/dev/null\n"
        "truncate -s $(( 44 + body )) \"$f.tmp\"\n"
        "mv \"$f.tmp\" \"$f\"\n");

    // 这两条排在 zero.sh 之前：一次成功就会把连续失败计数清零，后面的
    // 「三次失败停用」序列不受影响。
    const auto gutted = run((root / "gut.sh").string());
    check("a mostly-missing result is not a success", !gutted.ok);
    check("a mostly-missing result keeps the audio", audioIntact(gutted));

    const auto chopped = run((root / "chop.sh").string());
    check("a 30 ms-short result still counts as success", chopped.ok);
    check("the denoised audio keeps the full utterance",
          chopped.samples.size() == kToneSamples);
    // body 被 stub 刷成 0，所以「结尾非 0」只可能来自原样补回的采样。断点位置是
    // 有分量的：1440 个 48k 采样回到 16k 正好 480 个，所以 -481 必须还是模型给的
    // 0、-480 必须是原始波形。少 flush 一段（重采样自己就会少 341+830 个）会把断点
    // 往前推，这条就会红 —— 它同时证明两段重采样都冲干净了，而不只是长度对得上。
    check("compensation stops exactly at the model's 30 ms",
          chopped.samples.size() == kToneSamples &&
          chopped.samples[kToneSamples - 481] == 0 &&
          chopped.samples[kToneSamples - 480] != 0 &&
          chopped.samples[kToneSamples - 1] != 0);
    check("the body still comes back from the model",
          chopped.samples.size() == kToneSamples &&
          chopped.samples[kToneSamples / 2] == 0);

    const auto processed = run((root / "zero.sh").string());
    check("a working deep-filter succeeds", processed.ok);
    check("a working deep-filter is prompt", processed.ms < 3000);
    check("a length-preserving deep-filter keeps the full utterance",
          processed.samples.size() == kToneSamples);
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
