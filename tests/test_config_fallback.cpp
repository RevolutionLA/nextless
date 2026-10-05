#include "nextless_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static void writeFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

int main() {
    auto base = fs::temp_directory_path() /
                ("nextless-config-test-" + std::to_string(getpid()));
    auto userDir = base / "user";
    auto systemDir = base / "system";

    writeFile(systemDir / "audio.json", "{\"denoise\":\"none\"}");
    auto fallback = nextless::readConfigFileFromDirs("audio.json", userDir, systemDir);
    if (nextless::jsonStr(fallback, "denoise") != "none") {
        std::cerr << "system fallback was not used\n";
        fs::remove_all(base);
        return 1;
    }
    if (!fs::exists(userDir / "audio.json")) {
        std::cerr << "missing user config was not created from system config\n";
        fs::remove_all(base);
        return 1;
    }

    writeFile(userDir / "audio.json", "{\"denoise\":\"speexdsp\"}");
    auto user = nextless::readConfigFileFromDirs("audio.json", userDir, systemDir);
    if (nextless::jsonStr(user, "denoise") != "speexdsp") {
        std::cerr << "user config did not override system fallback\n";
        fs::remove_all(base);
        return 1;
    }

    writeFile(systemDir / "audio.json", "{\"denoise\":\"deepfilter\"}");
    auto preserved = nextless::readConfigFileFromDirs("audio.json", userDir, systemDir);
    if (nextless::jsonStr(preserved, "denoise") != "speexdsp") {
        std::cerr << "existing user config was overwritten\n";
        fs::remove_all(base);
        return 1;
    }

    if (!nextless::readConfigFileFromDirs("missing.json", userDir, systemDir).empty()) {
        std::cerr << "missing config should return empty content\n";
        fs::remove_all(base);
        return 1;
    }

    fs::remove_all(base);
    return 0;
}