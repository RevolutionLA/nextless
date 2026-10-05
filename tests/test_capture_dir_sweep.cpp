// Issue #28 regression test: with XDG_RUNTIME_DIR unset the capture dir comes
// from mkdtemp. It must be created ONCE per process (cached), not once per
// start(), and the crash sweep must actually reach orphan files that live in
// OLD random /tmp/nextless_* dirs. Runs headless: start() only needs to set
// the WAV path and spawn the thread; no PulseAudio server required.
#include "audio_capture.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

static int failures = 0;

static void check(const std::string &what, bool ok) {
    if (ok) return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static int countNextlessTmpDirs() {
    std::error_code ec;
    int n = 0;
    for (const auto &entry : fs::directory_iterator("/tmp", ec)) {
        if (ec) break;
        std::error_code dec;
        if (!entry.is_directory(dec) && !dec) continue;
        auto name = entry.path().filename().string();
        if (name.rfind("nextless_", 0) == 0) n++;
    }
    return n;
}

int main() {
    // Force the /tmp fallback branch for this process.
    unsetenv("XDG_RUNTIME_DIR");

    // Plant a fake crashed session: old random dir with an old orphan WAV.
    // The sweep ignores files younger than 10 minutes (live recordings from
    // concurrent sessions), so backdate the mtime by an hour.
    auto stamp = std::to_string(getpid());
    auto orphanDir = fs::path("/tmp") / ("nextless_orphan_" + stamp);
    std::error_code ec;
    fs::remove_all(orphanDir, ec);
    fs::create_directories(orphanDir);
    auto orphanWav = orphanDir / ("nextless_cap_9999_1_" + stamp + ".wav");
    // Pattern check is on the prefix, so the stamp suffix is fine.
    {
        std::ofstream f(orphanWav);
        f << "crash residue";
    }
    auto oldTime = fs::last_write_time(orphanWav) - std::chrono::hours(1);
    fs::last_write_time(orphanWav, oldTime);

    int dirsBefore = countNextlessTmpDirs();

    nextless::AudioCapture capture;

    // The planted orphan (file + dir) must be gone after the startup sweep.
    check("planted orphan WAV was not swept", !fs::exists(orphanWav));
    check("planted orphan dir was not removed", !fs::exists(orphanDir));

    // Three full capture cycles must reuse ONE directory (was: mkdtemp per
    // start() leaked one dir per utterance).
    std::string parent1, parent2, parent3;
    for (int i = 0; i < 3; i++) {
        capture.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        capture.stop();
        capture.wait();
        const auto &wp = capture.wavPath();
        check("wavPath empty after start cycle " + std::to_string(i), !wp.empty());
        auto pos = wp.rfind('/');
        std::string parent = pos == std::string::npos ? "" : wp.substr(0, pos);
        check("capture dir not under /tmp/nextless_*: " + parent,
              parent.rfind("/tmp/nextless_", 0) == 0);
        if (i == 0) parent1 = parent;
        if (i == 1) parent2 = parent;
        if (i == 2) parent3 = parent;
    }
    check("capture cycles leaked a new directory per utterance",
          parent1 == parent2 && parent2 == parent3);

    int dirsAfter = countNextlessTmpDirs();
    // Our process created at most one dir; siblings from parallel test
    // binaries may exist, so assert the DELTA of dirs we might have added is
    // 1 rather than an absolute count. (parent1 is ours and persists.)
    check("expected at most one new /tmp/nextless_* dir from 3 cycles",
          dirsAfter - dirsBefore <= 1);

    // Clean up our session dir so repeated runs do not accumulate.
    fs::remove_all(parent1, ec);
    return failures == 0 ? 0 : 1;
}
