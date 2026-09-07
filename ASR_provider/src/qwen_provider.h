#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

namespace vinput {

class QwenAsrProvider : public IAsrProvider {
public:
    QwenAsrProvider();
    ~QwenAsrProvider() override;

    void transcribe(std::vector<int16_t> samples, const std::string &wavPath) override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    struct Task {
        std::vector<int16_t> samples;
        std::string wavPath;
        std::string apiKey;
        long timeout;
        std::shared_ptr<std::atomic_bool> cancel;
        AsrResultCallback onResult;
        AsrErrorCallback onError;
        uint64_t diagnosticId;
    };
    struct WorkerState {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Task> tasks;
        std::shared_ptr<std::atomic_bool> activeCancel;
        bool stopping = false;
    };

    static void workerLoop(const std::shared_ptr<WorkerState> &state);
    static void processRecording(std::vector<int16_t> samples,
                                 const std::string &wavPath,
                                 std::string apiKey, long timeout,
                                 std::shared_ptr<std::atomic_bool> cancel,
                                  AsrResultCallback onR, AsrErrorCallback onE,
                                  uint64_t diagnosticId);

    std::string apiKey_;
    long timeout_ = 60;
    std::shared_ptr<WorkerState> state_;
    std::thread worker_;
};

class QwenAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "qwen"; }
    std::string name() const override { return "Qwen3-ASR-Flash (Alibaba DashScope)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
