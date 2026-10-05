#pragma once

#include <unistd.h>

#include <string>
#include <utility>

namespace nextless {

// The recorded audio is spilled to a temp WAV that must not outlive the task that produced it —
// `SECURITY.md` promises the file is gone as soon as recognition finishes. "Finishes" means the
// moment the result/error callback fires, not the moment the worker function returns: callers
// (and `tests/test_cloud_provider_queue.cpp`) treat a fired callback as "this utterance is done,
// the file is gone".
//
// A plain scope guard deletes too late — the callback runs while the guard is still alive, so on a
// slow build the file is still on disk when the caller looks. Hence an explicit `drop()` that the
// callback wrappers call first.
class TempWav {
public:
    explicit TempWav(std::string path) : path_(std::move(path)) {}
    ~TempWav() { drop(); }

    TempWav(const TempWav &) = delete;
    TempWav &operator=(const TempWav &) = delete;

    // Idempotent: safe to call from both the callback wrapper and the destructor.
    void drop() {
        if (!path_.empty()) {
            unlink(path_.c_str());
            path_.clear();
        }
    }

private:
    std::string path_;
};

} // namespace nextless
