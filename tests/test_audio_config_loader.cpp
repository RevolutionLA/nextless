#include "nextless_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "audio_capture.h"

namespace fs = std::filesystem;

static void writeFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

int main() {
    auto base = fs::temp_directory_path() /
                ("nextless-audio-config-test-" + std::to_string(getpid()));
    auto userDir = base / ".config" / "nextless";
    auto systemDir = base / "system" / "etc" / "nextless";

    // 模拟打包的默认配置：audio.json 包含 speexdsp
    writeFile(systemDir / "audio.json", "{\"denoise\":\"speexdsp\"}");

    // 临时覆盖 HOME 和 NEXTLESS_PACKAGED_CONFIG_DIR
    auto oldHome = getenv("HOME");
    auto homeStr = base.string();
    setenv("HOME", homeStr.c_str(), 1);

    // 注意：NEXTLESS_PACKAGED_CONFIG_DIR 是编译期宏，无法在运行时覆盖。
    // 因此我们直接调用 readConfigFileFromDirs 来验证产品路径。
    auto json = nextless::readConfigFileFromDirs(
        "audio.json", userDir.string(), systemDir.string());

    if (json.empty()) {
        std::cerr << "audio.json was not loaded from system dir\n";
        fs::remove_all(base);
        return 1;
    }

    auto denoise = nextless::jsonStr(json, "denoise");
    if (denoise != "speexdsp") {
        std::cerr << "expected denoise=speexdsp, got: " << denoise << "\n";
        fs::remove_all(base);
        return 1;
    }

    // 验证用户目录中已创建了副本
    if (!fs::exists(userDir / "audio.json")) {
        std::cerr << "user audio.json was not created from system config\n";
        fs::remove_all(base);
        return 1;
    }

    // 恢复 HOME
    if (oldHome) setenv("HOME", oldHome, 1);
    else unsetenv("HOME");

    fs::remove_all(base);
    return 0;
}
