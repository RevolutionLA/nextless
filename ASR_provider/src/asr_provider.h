#pragma once

#include <functional>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

namespace nextless {

using AsrResultCallback = std::function<void(const std::string &text, bool isFinal)>;
using AsrErrorCallback = std::function<void(const std::string &error)>;

// 松手时其实没说话不算故障。各家 provider 用 "<名字>: no speech" 报告这种情况
// ("empty result" 是历史写法, 保留兼容), adapter 据此把它当作无操作。
inline bool isNoSpeechError(const std::string &error) {
    return error.find("no speech") != std::string::npos ||
           error.find("empty result") != std::string::npos;
}

// 静音时模型可能返回空串或只剩空白字符, 这种结果不该上屏。
inline bool isBlankAsrText(const std::string &text) {
    for (char c : text) {
        if (static_cast<unsigned char>(c) > ' ') return false;
    }
    return true;
}

inline void joinAsrWorker(std::thread &worker) {
    if (!worker.joinable()) return;
    if (worker.get_id() == std::this_thread::get_id()) {
        worker.detach();
    } else {
        worker.join();
    }
}

template<typename Task>
inline void startAsrWorker(std::thread &worker, Task &&task) {
    auto ready = std::make_shared<std::latch>(1);
    worker = std::thread(
        [ready, task = std::forward<Task>(task)]() mutable {
            ready->wait();
            task();
        });
    ready->count_down();
}

class IAsrProvider {
public:
    virtual ~IAsrProvider() = default;

    virtual void transcribe(std::vector<int16_t> samples, const std::string &wavPath) = 0;

    virtual void setConfig(const std::string &key, const std::string &value) { (void)key; (void)value; }
    virtual void setDiagnosticId(uint64_t id) { diagnosticId_ = id; }

    // Asynchronous providers invoke callbacks on their worker thread. Callbacks
    // must hand UI work to the host event loop.
    void setResultCallback(AsrResultCallback cb) { onResult_ = std::move(cb); }
    void setErrorCallback(AsrErrorCallback cb) { onError_ = std::move(cb); }

protected:
    AsrResultCallback onResult_;
    AsrErrorCallback onError_;
    uint64_t diagnosticId_ = 0;
};

class IAsrProviderFactory {
public:
    virtual ~IAsrProviderFactory() = default;
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    virtual std::unique_ptr<IAsrProvider> create() = 0;
};

class AsrProviderRegistry {
public:
    static AsrProviderRegistry &instance();

    void registerFactory(std::unique_ptr<IAsrProviderFactory> factory);
    std::vector<std::pair<std::string, std::string>> listFactories() const;
    std::unique_ptr<IAsrProvider> create(const std::string &id) const;

private:
    AsrProviderRegistry() = default;
    std::vector<std::unique_ptr<IAsrProviderFactory>> factories_;
};

} // namespace nextless
