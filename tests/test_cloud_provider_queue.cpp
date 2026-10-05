#include "doubao_provider.h"
#include "qwen_provider.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

template<typename Provider>
bool testProvider(const std::filesystem::path &root, const std::string &name) {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<int> order;
    std::vector<std::thread::id> threads;
    std::vector<std::filesystem::path> paths;
    std::mutex fileMutex;
    bool filesGone = true;
    Provider provider;

    for (int i = 1; i <= 2; ++i) {
        auto path = root / (name + "-" + std::to_string(i) + ".wav");
        std::ofstream(path) << "placeholder";
        paths.push_back(path);
        provider.setErrorCallback([&, i, path](const std::string &) {
            // Contract: the temp WAV must be deleted BEFORE the callback fires.
            // Check inside the callback to make this deterministic, not a race.
            if (std::filesystem::exists(path)) {
                std::lock_guard<std::mutex> lock(fileMutex);
                filesGone = false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(i);
                threads.push_back(std::this_thread::get_id());
                ready.notify_one();
            }
        });
        provider.transcribe({}, path.string());
    }

    std::unique_lock<std::mutex> lock(mutex);
    if (!ready.wait_for(lock, std::chrono::seconds(2),
                        [&] { return order.size() == 2; })) {
        std::cerr << name << " queue did not drain\n";
        return false;
    }
    lock.unlock();

    if (order != std::vector<int>({1, 2}) || threads[0] != threads[1]) {
        std::cerr << name << " did not use one FIFO worker\n";
        return false;
    }
    if (!filesGone) {
        std::cerr << name << " left temporary WAV behind (callback fired before delete)\n";
        return false;
    }
    return true;
}

template<typename Provider>
bool testQueuedCleanup(const std::filesystem::path &root,
                       const std::string &name) {
    std::vector<std::filesystem::path> paths;
    {
        Provider provider;
        for (int i = 1; i <= 8; ++i) {
            auto path = root / (name + "-cleanup-" + std::to_string(i) + ".wav");
            std::ofstream(path) << "placeholder";
            paths.push_back(path);
            provider.transcribe({}, path.string());
        }
    }
    for (const auto &path : paths) {
        if (std::filesystem::exists(path)) {
            std::cerr << name << " destructor left queued WAV " << path << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    fs::path root = (tmp && *tmp) ? tmp : "/tmp/nextless-cloud-provider-queue";
    fs::create_directories(root / ".config/nextless");
    setenv("HOME", root.c_str(), 1);

    return testProvider<nextless::DoubaoAsrProvider>(root, "doubao") &&
                   testProvider<nextless::QwenAsrProvider>(root, "qwen") &&
                   testQueuedCleanup<nextless::DoubaoAsrProvider>(root, "doubao") &&
                   testQueuedCleanup<nextless::QwenAsrProvider>(root, "qwen")
               ? 0
               : 1;
}
