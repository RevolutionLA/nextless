#include "qwen_provider.h"
#include "nextless_config.h"
#include "diagnostic_log.h"
#include "temp_wav.h"

#include <curl/curl.h>
#include <unistd.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <fstream>
#include <memory>

namespace nextless {

static std::string base64Encode(const uint8_t *data, size_t len) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += T[(v >> 18) & 0x3F];
        out += T[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? T[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? T[v & 0x3F] : '=';
    }
    return out;
}

static std::string jsonGetString(const std::string &json, const std::string &key) {
    std::string q = "\"" + key + "\"";
    auto pos = json.find(q);
    if (pos == std::string::npos) return "";
    pos += q.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static size_t writeCb(void *ptr, size_t size, size_t nmemb, std::string *out) {
    out->append((const char *)ptr, size * nmemb);
    return size * nmemb;
}

static void loadConfig(std::string &apiKey) {
    const char *home = getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/.config/nextless/qwen.json";
    std::ifstream f(path);
    if (!f) {
        fprintf(stderr, "Nextless Qwen: no config at %s\n", path.c_str());
        return;
    }
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    apiKey = jsonGetString(json, "api_key");
}

QwenAsrProvider::QwenAsrProvider() {
    loadConfig(apiKey_);
    auto adv = advancedSection("qwen");
    if (!adv.empty()) {
        timeout_ = (long)jsonInt(adv, "timeout_sec", (int)timeout_);
    }
    state_ = std::make_shared<WorkerState>();
    worker_ = std::thread([state = state_] { workerLoop(state); });
}

QwenAsrProvider::~QwenAsrProvider() {
    diagnosticLog().event("provider", "provider_shutdown", {
        {"provider", "qwen"},
        {"recognition_id", std::to_string(diagnosticId_)}
    });
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = true;
        if (state_->activeCancel) state_->activeCancel->store(true);
        for (const auto &task : state_->tasks) unlink(task.wavPath.c_str());
        state_->tasks.clear();
    }
    state_->ready.notify_one();
    joinAsrWorker(worker_);
}

void QwenAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "api_key") apiKey_ = value;
}

void QwenAsrProvider::transcribe(std::vector<int16_t> samples, const std::string &wavPath) {
    Task task{std::move(samples), wavPath, apiKey_, timeout_,
              std::make_shared<std::atomic_bool>(false), onResult_, onError_,
              diagnosticId_};
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping) {
            unlink(wavPath.c_str());
            return;
        }
        state_->tasks.push_back(std::move(task));
    }
    state_->ready.notify_one();
}

void QwenAsrProvider::workerLoop(const std::shared_ptr<WorkerState> &state) {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->ready.wait(lock, [&] {
                return state->stopping || !state->tasks.empty();
            });
            if (state->stopping) return;
            task = std::move(state->tasks.front());
            state->tasks.pop_front();
            state->activeCancel = task.cancel;
        }
        diagnosticLog().event("provider", "request_worker_begin", {
            {"provider", "qwen"},
            {"recognition_id", std::to_string(task.diagnosticId)},
            {"wav_hash", hashDiagnosticValue(task.wavPath).substr(0, 16)}
        });
        processRecording(std::move(task.samples), task.wavPath,
                         std::move(task.apiKey), task.timeout,
                         std::move(task.cancel), std::move(task.onResult),
                         std::move(task.onError), task.diagnosticId);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->activeCancel.reset();
            if (state->stopping) return;
        }
    }
}

void QwenAsrProvider::processRecording(std::vector<int16_t> samples,
                                        const std::string &wavPath,
                                        std::string apiKey, long timeout,
                                        std::shared_ptr<std::atomic_bool> cancel,
                                        AsrResultCallback onResultRaw,
                                        AsrErrorCallback onErrorRaw,
                                        uint64_t diagnosticId) {
    fprintf(stderr, "Nextless Qwen: recorded %zu samples to %s\n",
            samples.size(), wavPath.c_str());
    diagnosticLog().event("provider", "request_started", {
        {"provider", "qwen"},
        {"recognition_id", std::to_string(diagnosticId)},
        {"wav_hash", hashDiagnosticValue(wavPath).substr(0, 16)},
        {"sample_count", std::to_string(samples.size())}
    });
    // Delete the temp WAV before either callback runs — see temp_wav.h. The wrappers below keep
    // every existing `onE(...)` call site working unchanged while guaranteeing the order.
    nextless::TempWav wavFile(wavPath);
    const AsrResultCallback onResult = std::move(onResultRaw);
    const AsrErrorCallback onError = std::move(onErrorRaw);
    auto onR = [&wavFile, onResult](const std::string &text, bool isFinal) {
        wavFile.drop();
        if (onResult) onResult(text, isFinal);
    };
    auto onE = [&wavFile, onError](const std::string &error) {
        wavFile.drop();
        if (onError) onError(error);
    };

    if (apiKey.empty()) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "missing_api_key"}
        });
        onE("Qwen: missing api_key in ~/.config/nextless/qwen.json");
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    std::ifstream wf(wavPath, std::ios::binary);
    if (!wf) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "wav_read_failed"}
        });
        onE("Qwen: failed to read WAV");
        return;
    }
    std::vector<uint8_t> wavData((std::istreambuf_iterator<char>(wf)),
                                  std::istreambuf_iterator<char>());
    if (wavData.empty()) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "empty_wav"}
        });
        onE("Qwen: empty WAV file");
        return;
    }
    std::string b64 = base64Encode(wavData.data(), wavData.size());
    std::string dataUri = "data:audio/wav;base64," + b64;
    auto tEncode = std::chrono::steady_clock::now();

    CURL *curl = getCurl();
    if (!curl) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "curl_init_failed"}
        });
        onE("Qwen: curl init failed");
        return;
    }
    std::string requestBody =
        "{"
        "\"model\":\"qwen3-asr-flash\","
        "\"input\":{"
        "\"messages\":["
        "{\"content\":[{\"audio\":\"" + dataUri + "\"}],\"role\":\"user\"}"
        "]"
        "},"
        "\"parameters\":{"
        "\"asr_options\":{"
        "\"enable_itn\":false"
        "}"
        "}"
        "}";

    curl_easy_reset(curl);
    std::string respBody;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers,
        ("Authorization: Bearer " + apiKey).c_str());

    curl_easy_setopt(curl, CURLOPT_URL,
        "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)requestBody.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(timeout, 10L));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CurlCancellationScope cancellation(curl, cancel);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);

    auto tNetwork = std::chrono::steady_clock::now();

    fprintf(stderr, "Nextless Qwen: HTTP %ld\n", httpCode);
    fprintf(stderr, "Nextless Qwen: response bytes=%zu\n", respBody.size());

    if (res != CURLE_OK) {
        if (cancel->load()) {
            diagnosticLog().event("provider", "request_cancelled", {
                {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)}
            });
            return;
        }
        fprintf(stderr, "Nextless Qwen: transport failed, curl=%d (%s)\n",
                (int)res, curl_easy_strerror(res));
        evictCurlHandle();
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "transport"}, {"handle_evicted", "true"}
        });
        {
            onE("Qwen: network request failed (" +
                std::string(curl_easy_strerror(res)) + ")");
        }
        return;
    }
    if (httpCode != 200) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "http_status"}, {"http_code", std::to_string(httpCode)}
        });
        {
            if (httpCode == 429 || httpCode >= 500) {
                onE("Qwen: service unavailable (HTTP " +
                    std::to_string(httpCode) + ")");
            } else {
                onE("Qwen: service request failed (HTTP " +
                    std::to_string(httpCode) + ")");
            }
        }
        return;
    }

    std::string text;
    auto pos = respBody.find("\"text\":\"");
    if (pos != std::string::npos) {
        pos += 8;
        auto end = respBody.find('"', pos);
        if (end != std::string::npos)
            text = respBody.substr(pos, end - pos);
    }

    auto tParse = std::chrono::steady_clock::now();
    fprintf(stderr, "Nextless Qwen [timer] encode=%ldms network=%ldms parse=%ldms text_len=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEncode - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tNetwork - tEncode).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tNetwork).count(),
            text.size());

    if (!isBlankAsrText(text)) {
        diagnosticLog().event("provider", "request_result", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"text_length", std::to_string(text.size())},
            {"encode_ms", std::to_string(std::chrono::duration_cast<
                std::chrono::milliseconds>(tEncode - t0).count())},
            {"network_ms", std::to_string(std::chrono::duration_cast<
                std::chrono::milliseconds>(tNetwork - tEncode).count())},
            {"parse_ms", std::to_string(std::chrono::duration_cast<
                std::chrono::milliseconds>(tParse - tNetwork).count())}
        });
        onR(text, true);
    } else {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "no_speech"}
        });
        onE("Qwen: no speech");
    }
}

std::unique_ptr<IAsrProvider> QwenAsrProviderFactory::create() {
    return std::make_unique<QwenAsrProvider>();
}

static struct CurlInit {
    CurlInit() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlInit() { curl_global_cleanup(); }
} _curlInit;

static bool _qwenReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<QwenAsrProviderFactory>());
    return true;
}();

} // namespace nextless
