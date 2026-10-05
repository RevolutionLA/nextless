// 静音不该被当成故障: provider 用 "<名字>: no speech" 报告没听懂任何语音, 模型
// 返回空串或纯空白也算。adapter 据此把面板复位, 不上屏也不报错误。
#include "asr_provider.h"
#include "fire_red_provider.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const std::string &what, bool ok) {
    if (ok) return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

} // namespace

int main() {
    const std::vector<std::string> silence = {
        "Zipformer: no speech",
        "FireRed: no speech",
        "Qwen: no speech",
        "Doubao: no speech recognized",
        "SomeProvider: empty result",  // 历史措辞, 仍然要认
    };
    for (const auto &message : silence) {
        check("silence: " + message, nextless::isNoSpeechError(message));
    }

    const std::vector<std::string> realErrors = {
        "Zipformer: recognition failed",
        "Doubao: network error while submitting",
        "Qwen: request timed out",
        "Doubao: recognition service unavailable",
        "Nextless: microphone read failed",
    };
    for (const auto &message : realErrors) {
        check("not silence: " + message, !nextless::isNoSpeechError(message));
    }

    for (const auto &text : {std::string(""), std::string(" "),
                             std::string("  \t\n ")}) {
        check("blank text", nextless::isBlankAsrText(text));
    }
    for (const auto &text : {std::string("hello"), std::string("你好"),
                             std::string(" ok ")}) {
        check("non-blank text", !nextless::isBlankAsrText(text));
    }

    // FireRed 对纯静音/噪声返回 "<sil>", 原样上屏就是在文档里留下这个记号
    for (const auto &input : {std::string("<sil>"), std::string("<sil> <sil>"),
                              std::string("<unk>")}) {
        auto text = input;
        nextless::stripControlTokens(text);
        check("marker-only text becomes blank: " + input,
              nextless::isBlankAsrText(text));
    }
    {
        std::string text = "昨天是 <sil> 星期三";
        nextless::stripControlTokens(text);
        check("speech keeps its words: " + text, text == "昨天是  星期三");
    }
    {
        std::string text = "<xinan><p2>你好<sichuan>";
        nextless::stripControlTokens(text);
        check("dialect tags removed: " + text, text == "你好");
    }
    {
        std::string text = "声明 vector<int> 和 map<string, int>";
        nextless::stripControlTokens(text);
        check("code-shaped text survives: " + text,
              text == "声明 vector<int> 和 map<string, int>");
    }

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "no-speech handling ok\n";
    return 0;
}
