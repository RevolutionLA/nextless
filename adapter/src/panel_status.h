#pragma once

#include <string>

#include "asr_provider.h"

namespace nextless {

// Provider 的 error 文本 → 输入面板状态。抽成纯函数是为了能单测
// (tests/test_missing_components.cpp)；此前这段映射埋在 adapter 的
// lambda 里，provider 说了什么、面板显示什么，谁都没测过。
//
// 顺序沿用历史行为: no speech 清空面板(当作无操作)，网络/超时/服务不可用
// 给可重试的提示；缺运行时/模型时, 面板指出缺的是哪一块, 并让用户跑
// nextless-get-models(issue #5 的首次运行下载助手, tools/get_models.sh)。
// 具体的缺失路径在 provider 的 error 里, 完整进日志(FCITX_INFO)。
inline std::string panelStatusForError(const std::string &error) {
    if (isNoSpeechError(error)) return "";
    if (error.find("network") != std::string::npos) {
        return "Nextless: network error; try again";
    }
    if (error.find("timed out") != std::string::npos) {
        return "Nextless: recognition timed out; try again";
    }
    if (error.find("service unavailable") != std::string::npos) {
        return "Nextless: recognition service unavailable; try again";
    }
    if (error.find("runtime not found") != std::string::npos) {
        return "Nextless: sherpa-onnx runtime missing; run nextless-get-models";
    }
    if (error.find("model file not found") != std::string::npos) {
        return "Nextless: offline model missing; run nextless-get-models";
    }
    return "Nextless: recognition failed";
}

} // namespace nextless
