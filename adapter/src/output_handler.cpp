#include "output_handler.h"
#include "desktop_strategy.h"
#include "vinput_config.h"

#include <fcitx/inputpanel.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/text.h>
#include <fcitx-utils/log.h>

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace vinput {

OutputHandler::OutputHandler(fcitx::Instance *instance) : instance_(instance) {
    desktop_ = DesktopStrategy::autoDetect();
    FCITX_INFO() << "Vinput OutputHandler: auto-detected strategy=" << desktop_->name();

    if (pipe(wakePipe_) != 0) {
        throw std::runtime_error("Vinput: OutputHandler pipe() failed");
    }
    fcntl(wakePipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(wakePipe_[1], F_SETFL, O_NONBLOCK);

    wakeWatcher_ = instance_->eventLoop().addIOEvent(
        wakePipe_[0], fcitx::IOEventFlag::In,
        [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) -> bool {
            drainAndCommit();
            return true;
        });
    if (!wakeWatcher_) {
        close(wakePipe_[0]);
        close(wakePipe_[1]);
        wakePipe_[0] = wakePipe_[1] = -1;
        throw std::runtime_error("Vinput: failed to create output event source");
    }
}

OutputHandler::~OutputHandler() {
    if (wakePipe_[0] >= 0) close(wakePipe_[0]);
    if (wakePipe_[1] >= 0) close(wakePipe_[1]);
}

void OutputHandler::submit(const OutputTarget &target, const std::string &text,
                           std::function<void()> onCommitted) {
    enqueue(target, text, false, std::move(onCommitted));
}

void OutputHandler::showStatus(const OutputTarget &target,
                               const std::string &text,
                               std::function<void()> onShown) {
    enqueue(target, text, true, std::move(onShown));
}

OutputTarget OutputHandler::captureCurrentWindow() {
    FCITX_INFO() << "Vinput [capture] detected desktop=" << desktop_->name();
    auto capturedWinId = desktop_->getFocusedWindowId();
    OutputTarget target;
    target.windowId = capturedWinId;
    target.statusSequence = ++latestStatusSequence_;
    if (auto *ic = instance_->mostRecentInputContext()) {
        target.uuid = ic->uuid();
    }
    return target;
}

void OutputHandler::wake() {
    char c = 1;
    while (write(wakePipe_[1], &c, 1) < 0 && errno == EINTR) {}
}

void OutputHandler::enqueue(const OutputTarget &target, const std::string &text,
                            bool isStatus,
                            std::function<void()> completion) {
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pending_.push_back({text, isStatus, target.uuid, target.windowId,
                            target.statusSequence, std::move(completion)});
    }
    wake();
}

void OutputHandler::drainAndCommit() {
    char buf[64];
    while (read(wakePipe_[0], buf, sizeof(buf)) > 0) {}

    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        batch.swap(pending_);
    }

    // Status belongs in the input panel, never in the application's document.
    for (auto &p : batch) {
        if (!p.isStatus) continue;
        if (p.statusSequence < latestStatusSequence_) {
            if (p.completion) p.completion();
            continue;
        }
        auto *ic = instance_->inputContextManager().findByUUID(p.targetUuid);
        if (ic) {
            ic->inputPanel().setAuxUp(fcitx::Text(p.text));
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        if (p.completion) p.completion();
    }

    for (auto &p : batch) {
        if (!p.isStatus) commitQueue_.push_back(std::move(p));
    }
    dispatchNextCommit();
}

void OutputHandler::dispatchNextCommit() {
    if (pendingNiriCommit_) return;
    niriPollTimer_.reset();

    while (!commitQueue_.empty()) {
        Pending pending = std::move(commitQueue_.front());
        commitQueue_.pop_front();
        const auto capturedId = pending.capturedWinId;

        if (capturedId.empty() || !desktop_->supportsSwitching()) {
            commitPending(std::move(pending), "commit");
            continue;
        }

        auto restoreId = desktop_->getFocusedWindowId();
        if (restoreId.empty()) {
            FCITX_INFO() << "Vinput [" << desktop_->name()
                         << "] failed to get focused window, direct commit";
            commitPending(std::move(pending), "commit");
            continue;
        }
        if (restoreId == capturedId) {
            FCITX_INFO() << "Vinput [" << desktop_->name()
                         << "] window unchanged, direct commit";
            commitPending(std::move(pending), "commit");
            continue;
        }

        FCITX_INFO() << "Vinput [" << desktop_->name() << "] captured="
                     << capturedId << " restore=" << restoreId;
        desktop_->focusWindow(capturedId);
        pendingNiriCommit_ = std::move(pending);
        pendingCapturedId_ = capturedId;
        pendingRestoreId_ = std::move(restoreId);
        niriRetryCount_ = 0;

        niriPollTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC,
            fcitx::now(CLOCK_MONOTONIC) + kNiriRetryIntervalUsec,
            kNiriRetryIntervalUsec,
            [this](fcitx::EventSourceTime *, uint64_t) -> bool {
                return niriPollTick();
            });
        if (niriPollTimer_) return;

        FCITX_ERROR() << "Vinput: failed to create focus polling timer; committing directly";
        commitPending(std::move(*pendingNiriCommit_), "commit");
        pendingNiriCommit_.reset();
        if (!pendingRestoreId_.empty() && pendingRestoreId_ != pendingCapturedId_) {
            desktop_->focusWindow(pendingRestoreId_);
        }
        pendingCapturedId_.clear();
        pendingRestoreId_.clear();
    }
}

bool OutputHandler::niriPollTick() {
    niriRetryCount_++;
    auto cur = desktop_->getFocusedWindowId();

    if (cur == pendingCapturedId_ || niriRetryCount_ >= kNiriRetryMax) {
        if (cur != pendingCapturedId_) {
            fprintf(stderr, "Vinput [%s] focus switch timeout after %dms, committing anyway\n",
                    desktop_->name(),
                    niriRetryCount_ * kNiriRetryIntervalUsec / 1000);
        }

        commitPending(std::move(*pendingNiriCommit_), "commit");
        pendingNiriCommit_.reset();

        if (!pendingRestoreId_.empty() && pendingRestoreId_ != pendingCapturedId_) {
            desktop_->focusWindow(pendingRestoreId_);
        }

        pendingCapturedId_.clear();
        pendingRestoreId_.clear();
        wake();
        return false;
    }

    return true;
}

void OutputHandler::commitPending(Pending pending, const char *label) {
    auto *ic = instance_->inputContextManager().findByUUID(pending.targetUuid);
    if (!ic) {
        FCITX_INFO() << "Vinput [" << label << "] no focused ic, drop";
    } else {
        FCITX_INFO() << "Vinput [" << label << "] ic=" << ic
                     << " program=" << ic->program()
                     << " text=\"" << pending.text << "\"";
        if (!pending.text.empty()) ic->commitString(pending.text);
    }
    if (pending.completion) pending.completion();
}

} // namespace vinput
