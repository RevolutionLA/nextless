#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include "asr_provider.h"
#include "nextless_config.h"

namespace nextless {

// FireRed 用 <sil> 这类控制记号(tokens.txt 里那 40 个非语音项)表示"这段不是语音"。
// sherpa-onnx 会把它们原样带进结果文本, 直接上屏就是在用户文档里留下 "<sil>"。
// 剥掉表里的记号; 剥完只剩空白就说明这段确实没说话, 按静音处理。
// 只认表内的词, 用户真的在念代码时 "<int>" 这种内容不会被误删。
void stripControlTokens(std::string &text);

class FireRedAsrProvider : public IAsrProvider {
public:
    FireRedAsrProvider();
    ~FireRedAsrProvider() override;

    void transcribe(std::vector<int16_t>, const std::string &wavPath) override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    static void runTranscribe(const std::string &wav, const std::string &dir,
                              std::string sherpaBin, int numThreads,
                              int timeoutSec,
                              std::shared_ptr<std::atomic_bool> cancel,
                              AsrResultCallback onResultRaw, AsrErrorCallback onErrorRaw,
                              uint64_t diagnosticId);

    std::string modelDir_;
    std::string sherpaBin_ = "~/.local/share/nextless/sherpa-onnx/bin/sherpa-onnx-offline";
    int numThreads_ = defaultAsrThreads();
    int timeoutSec_ = 120;
    std::shared_ptr<std::atomic_bool> cancel_ =
        std::make_shared<std::atomic_bool>(false);
    std::thread worker_;
};

class FireRedAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "fire_red"; }
    std::string name() const override { return "FireRed ASR (sherpa-onnx)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace nextless
