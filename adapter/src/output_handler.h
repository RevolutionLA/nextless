#pragma once

#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vinput {

class DesktopStrategy;

struct OutputTarget {
    fcitx::ICUUID uuid = {};
    std::string windowId;
    uint64_t statusSequence = 0;
};

class OutputHandler {
public:
    explicit OutputHandler(fcitx::Instance *instance);
    ~OutputHandler();

    OutputHandler(const OutputHandler &) = delete;
    OutputHandler &operator=(const OutputHandler &) = delete;

    // Thread-safe: submit ASR result text for display
    void submit(const OutputTarget &target, const std::string &text,
                std::function<void()> onCommitted = {});

    // Thread-safe: show transient status text
    void showStatus(const OutputTarget &target, const std::string &text,
                    std::function<void()> onShown = {});

    // Capture current focused window via desktop strategy
    OutputTarget captureCurrentWindow();

private:
    fcitx::Instance *instance_;

    int wakePipe_[2] = {-1, -1};
    std::unique_ptr<fcitx::EventSourceIO> wakeWatcher_;

    struct Pending {
        std::string text;
        bool isStatus = false;
        fcitx::ICUUID targetUuid = {};
        std::string capturedWinId;
        uint64_t statusSequence = 0;
        std::function<void()> completion;
    };
    std::mutex pendingMutex_;
    std::vector<Pending> pending_;

    std::unique_ptr<DesktopStrategy> desktop_;
    uint64_t latestStatusSequence_ = 0;

    // Focus-switch commits are serialized so results for different windows
    // cannot overwrite one another while a switch is in progress.
    std::deque<Pending> commitQueue_;
    std::optional<Pending> pendingNiriCommit_;
    std::string pendingCapturedId_;
    std::string pendingRestoreId_;
    int niriRetryCount_ = 0;
    std::unique_ptr<fcitx::EventSourceTime> niriPollTimer_;

    static constexpr int kNiriRetryMax = 20;
    static constexpr int kNiriRetryIntervalUsec = 25000;

    void enqueue(const OutputTarget &target, const std::string &text,
                 bool isStatus, std::function<void()> completion);
    void drainAndCommit();
    void dispatchNextCommit();
    void commitPending(Pending pending, const char *label);
    void wake();
    bool niriPollTick();
};

} // namespace vinput
