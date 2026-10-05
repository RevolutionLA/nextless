#include "mock_provider.h"
#include "temp_wav.h"

namespace nextless {

static bool _mockRegistered = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<MockAsrProviderFactory>());
    return true;
}();

void MockAsrProvider::transcribe(std::vector<int16_t>, const std::string &wavPath) {
    // TempWav is a no-op when the path is empty (test_asr_registry passes "").
    nextless::TempWav wavFile(wavPath);
    wavFile.drop();
    if (onResult_) onResult_("hello world", true);
}

} // namespace nextless
