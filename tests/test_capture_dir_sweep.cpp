// Issue #28 regression test: with XDG_RUNTIME_DIR unset the capture dir comes
// from mkdtemp. It must be created ONCE per process (cached), not once per
// start(), and the crash sweep must actually reach orphan files that live in
// OLD random /tmp/nextless_* dirs. Runs headless: start() only needs to set
// the WAV path and spawn the thread; no PulseAudio server required.
//
// Two planted dirs reproduce the CI flake found in the first cut of the fix:
// a FRESH recording in a live sibling dir must survive the sweep (age guard),
// AND that non-empty sibling must not abort the scan before the OLD orphan
// dir is reached (a shared error_code + `if (ec) break` did exactly that on
// GitHub runners, where /tmp holds leftover dirs from parallel test binaries).
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

struct Planted {
    fs::path dir;
    fs::path wav;
};

static Planted plantDir(const std::string &kind, const std::string &stamp,
                        bool backdate) {
    std::error_code ec;
    Planted p;
    p.dir = fs::path("/tmp") / ("nextless_" + kind + "_" + stamp);
    fs::remove_all(p.dir, ec);
    fs::create_directories(p.dir);
    // Pattern check is on the prefix, so the stamp suffix is fine.
    p.wav = p.dir / ("nextless_cap_9999_1_" + stamp + ".wav");
    std::ofstream f(p.wav);
    f << "residue";
    f.close();
    if (backdate) {
        auto oldTime = fs::last_write_time(p.wav) - std::chrono::hours(1);
        fs::last_write_time(p.wav, oldTime);
    }
    return p;
}

int main() {
    // Force the /tmp fallback branch for this process.
    unsetenv("XDG_RUNTIME_DIR");

    auto stamp = std::to_string(getpid());
    // Old crash residue: must be swept (file gone, then the empty dir rmdir'd).
    auto old = plantDir("orphan", stamp, /*backdate=*/true);
    // Live sibling with a fresh file: must be left completely alone.
    auto live = plantDir("live", stamp, /*backdate=*/false);

    nextless::AudioCapture capture;

    check("planted orphan WAV was not swept", !fs::exists(old.wav));
    check("planted orphan dir was not removed", !fs::exists(old.dir));
    check("sweep killed a concurrent session's fresh recording",
          fs::exists(live.wav));

    // Three full capture cycles must reuse ONE directory (was: mkdtemp per
    // start() leaked one dir per utterance).
    std::string parent;
    for (int i = 0; i < 3; i++) {
        capture.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        capture.stop();
        capture.wait();
        const auto &wp = capture.wavPath();
        check("wavPath empty after start cycle " + std::to_string(i), !wp.empty());
        auto pos = wp.rfind('/');
        std::string p = pos == std::string::npos ? "" : wp.substr(0, pos);
        check("capture dir not under /tmp/nextless_*: " + p,
              p.rfind("/tmp/nextless_", 0) == 0);
        if (i == 0) parent = p;
        else check("capture cycles leaked a new directory per utterance", p == parent);
    }

    // The live dir must still exist (non-empty dirs are never removed).
    check("non-empty sibling dir was removed", fs::exists(live.dir));

    // Clean up planted dirs and our session dir so repeated runs do not
    // accumulate.
    std::error_code ec;
    fs::remove_all(live.dir, ec);
    fs::remove_all(old.dir, ec);
    fs::remove_all(parent, ec);
    return failures == 0 ? 0 : 1;
}
