// Issue #28 regression test: with XDG_RUNTIME_DIR unset the capture dir comes
// from mkdtemp. It must be created ONCE per process (cached), not once per
// start(), and the crash sweep must actually reach orphan files that live in
// OLD random /tmp/nextless_* dirs. Runs headless: start() only needs to set
// the WAV path and spawn the thread; no PulseAudio server required.
//
// Two planted dirs reproduce the CI flake found in the first cut of the fix:
// a FRESH recording in a live sibling dir must survive the sweep, AND that
// non-empty sibling must not abort the scan before the OLD orphan dir is
// reached (a shared error_code + `if (ec) break` did exactly that on GitHub
// runners, where /tmp holds leftover dirs from parallel test binaries).
//
// Issue #42 changed what "survive" is keyed on: the owner pid in the file name
// (alive = somebody is writing; dead = crash residue), with the age guard left
// as the fallback for names we cannot parse or owners that got reused. The
// fixtures therefore plant REAL pids, and the XDG half of this test runs the
// shared fixed-name dir through both directions of that rule.
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
#include <signal.h>
#include <sys/wait.h>

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

// A stand-in for a session that is recording right now, and a pid that is
// provably gone (reaped here, so no zombie left for kill(2) to report alive).
// Issue #42: the sweep reads the owner out of the file name, so the fixtures
// have to carry REAL pids - the old hard-coded 9999 is a dead pid on any
// machine, which now correctly reads as "crash residue, collect it".
static pid_t spawnStandIn() {
    pid_t p = fork();
    if (p == 0) {
        pause();
        _exit(0);
    }
    return p;
}

static pid_t spawnAndReap() {
    pid_t p = fork();
    if (p == 0) _exit(0);
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
    return p;
}

static void stopStandIn(pid_t p) {
    if (p <= 0) return;
    kill(p, SIGTERM);
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
}

static Planted plantDir(const std::string &kind, const std::string &stamp,
                        bool backdate, pid_t owner) {
    std::error_code ec;
    Planted p;
    p.dir = fs::path("/tmp") / ("nextless_" + kind + "_" + stamp);
    fs::remove_all(p.dir, ec);
    fs::create_directories(p.dir);
    // Pattern check is on the prefix, so the stamp suffix is fine.
    p.wav = p.dir / ("nextless_cap_" + std::to_string(owner) + "_1_" + stamp + ".wav");
    std::ofstream f(p.wav);
    f << "residue";
    f.close();
    if (backdate) {
        auto oldTime = fs::last_write_time(p.wav) - std::chrono::hours(1);
        fs::last_write_time(p.wav, oldTime);
    }
    return p;
}

// ---- issue #42: the shared XDG dir needs ownership, not "this dir is mine" ----
// With XDG_RUNTIME_DIR set the capture dir is a FIXED path (/run/user/<uid>/nextless)
// that every process of this user shares, which is exactly where another session's
// in-flight recording sits. The sweep therefore keys on the pid baked into the file
// name: dead owner -> residue, alive owner -> not ours to touch.
//
// Both the capture dir and the sweep are once-per-process (secureCaptureDir caches,
// and the ctor guards the sweep with a static), so the two phases below run in
// exec'd copies of this binary and the parent owns the "other session" process.
static std::string wavNameFor(pid_t owner, int slot = 1) {
    return "nextless_cap_" + std::to_string(owner) + "_" + std::to_string(slot) + ".wav";
}

static fs::path plantIn(const fs::path &dir, const std::string &name,
                        bool backdate = false) {
    fs::path p = dir / name;
    std::ofstream f(p);
    f << "residue";
    f.close();
    if (backdate) fs::last_write_time(p, fs::last_write_time(p) - std::chrono::hours(1));
    return p;
}

// Child phases: return 1 on the first broken expectation. FAIL lines go to stderr
// for whoever runs the suite; the parent only sees the exit code.
static int phaseOwnershipCheck() {
    const char *xdgRaw = getenv("NEXTLESS_SWEEP_XDG");
    pid_t live = static_cast<pid_t>(atol(getenv("NEXTLESS_SWEEP_LIVEPID")));
    if (!xdgRaw || live <= 0) return 1;
    setenv("XDG_RUNTIME_DIR", xdgRaw, 1);
    fs::path shared = fs::path(xdgRaw) / "nextless";

    pid_t dead = spawnAndReap();

    auto liveWav = plantIn(shared, wavNameFor(live));
    auto deadWav = plantIn(shared, wavNameFor(dead));
    auto junkWav = plantIn(shared, "nextless_cap_notanumber_1.wav");
    auto oldWav = plantIn(shared, "nextless_cap_ancient_1.wav", /*backdate=*/true);
    // A live pid is not a licence to keep a file forever: if the pid got reused,
    // or a wedged session sat on it, the age guard still has to win.
    auto staleOwned = plantIn(shared, wavNameFor(live, 2), /*backdate=*/true);

    nextless::AudioCapture capture;  // ctor runs the sweep

    int bad = 0;
    if (!fs::exists(liveWav)) {
        std::cerr << "FAIL(#42): sweep deleted a LIVE recording of another session in the shared dir\n";
        ++bad;
    }
    if (fs::exists(deadWav)) {
        std::cerr << "FAIL(#42): sweep left a crashed session's residue in the shared dir\n";
        ++bad;
    }
    if (!fs::exists(junkWav)) {
        std::cerr << "FAIL(#42): sweep deleted a fresh file whose owner it cannot read\n";
        ++bad;
    }
    if (fs::exists(oldWav)) {
        std::cerr << "FAIL(#42): the age fallback stopped working for unparseable names\n";
        ++bad;
    }
    if (fs::exists(staleOwned)) {
        std::cerr << "FAIL(#42): an alive owner's ANCIENT file was kept forever\n";
        ++bad;
    }
    return bad == 0 ? 0 : 1;
}

static int phaseOwnerDied() {
    const char *xdgRaw = getenv("NEXTLESS_SWEEP_XDG");
    pid_t live = static_cast<pid_t>(atol(getenv("NEXTLESS_SWEEP_LIVEPID")));
    if (!xdgRaw || live <= 0) return 1;
    setenv("XDG_RUNTIME_DIR", xdgRaw, 1);
    fs::path liveWav = (fs::path(xdgRaw) / "nextless") / wavNameFor(live);

    nextless::AudioCapture capture;
    if (fs::exists(liveWav)) {
        std::cerr << "FAIL(#42): the recording outlived its owner but the sweep still skips it"
                  << " (guard is 'never touch the shared dir', not ownership)\n";
        return 1;
    }
    return 0;
}

static int runPhase(const char *phase, const fs::path &xdg, pid_t live) {
    pid_t p = fork();
    if (p == 0) {
        std::string xdgStr = xdg.string();
        std::string liveStr = std::to_string(live);
        setenv("NEXTLESS_SWEEP_PHASE", phase, 1);
        setenv("NEXTLESS_SWEEP_XDG", xdgStr.c_str(), 1);
        setenv("NEXTLESS_SWEEP_LIVEPID", liveStr.c_str(), 1);
        char *argv[] = {const_cast<char *>("/proc/self/exe"), nullptr};
        execv("/proc/self/exe", argv);
        _exit(127);
    }
    if (p < 0) return -1;
    int status = 0;
    while (waitpid(p, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

int main() {
    if (const char *phase = getenv("NEXTLESS_SWEEP_PHASE")) {
        if (strcmp(phase, "ownership") == 0) return phaseOwnershipCheck();
        if (strcmp(phase, "owner-died") == 0) return phaseOwnerDied();
        return 2;
    }

    // Force the /tmp fallback branch for this process.
    unsetenv("XDG_RUNTIME_DIR");

    auto stamp = std::to_string(getpid());
    pid_t standIn = spawnStandIn();      // "another session, right now"
    pid_t gone = spawnAndReap();         // a crashed session's pid
    // Old crash residue: must be swept (file gone, then the empty dir rmdir'd).
    auto old = plantDir("orphan", stamp, /*backdate=*/true, gone);
    // Live sibling with a fresh file: must be left completely alone.
    auto live = plantDir("live", stamp, /*backdate=*/false, standIn);

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

    // --- issue #42: the SHARED XDG dir gets swept by ownership, not by "mine" ---
    {
        auto xdg = fs::path("/tmp/nextless-xdg-" + stamp);
        std::error_code xec;
        fs::remove_all(xdg, xec);
        fs::create_directories(xdg / "nextless", xec);

        // A second stand-in, alive across the first sweep and then killed and
        // reaped HERE - so the second sweep sees a provably dead owner rather
        // than a zombie that kill(2) still reports as running.
        pid_t livePid = spawnStandIn();
        check("could not fork a stand-in session", livePid > 0);

        check("XDG ownership phase failed (see the FAIL(#42) lines above)",
              runPhase("ownership", xdg, livePid) == 0);

        auto liveWav = (xdg / "nextless") / wavNameFor(livePid);
        bool wasKept = fs::exists(liveWav);
        stopStandIn(livePid);

        if (wasKept) {
            check("the file stayed after its owner died (guard = never touch the shared dir?)",
                  runPhase("owner-died", xdg, livePid) == 0);
        } else {
            std::cerr << "FAIL: live file vanished before the second phase could judge it\n";
            ++failures;
        }
        fs::remove_all(xdg, xec);
    }

    // Clean up planted dirs and our session dir so repeated runs do not
    // accumulate.
    stopStandIn(standIn);
    std::error_code ec;
    fs::remove_all(live.dir, ec);
    fs::remove_all(old.dir, ec);
    fs::remove_all(parent, ec);
    return failures == 0 ? 0 : 1;
}
