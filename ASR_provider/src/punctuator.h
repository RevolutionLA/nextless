#pragma once

#include <string>

namespace nextless {

// 本地后处理: 把 Zipformer / FireRed 的裸文本过一遍
// sherpa-onnx-offline-punctuation (ct-transformer), 补上中英文标点。
// 云后端的 providerId 原样返回 —— 它们自己带标点, 这里必须是 no-op。
// 任何失败(缺模型、子进程报错、超时、输出为空)都保留原文:
// 标点只是装饰, 永远不能让它吃掉一句话 (issue #4)。
// 实测(本机, int8): spawn + 模型加载 + 推理 ≈ 0.2 s, 远低于 ASR 本身,
// 所以默认常开, 不做 per-backend 开关; advanced.json [punctuation]
// {"enabled": false} 可整体关掉。必须在识别线程调用, 不在按键回调里。
void punctuateLocalText(std::string &text, const std::string &providerId);

// 单次执行标点二进制, 供 punctuate 和单测使用。成功(子进程 exit 0 且
// stdout 有非空行)时把结果写入 out 并返回 true; 其余一律 false。
bool runPunctuation(const std::string &binPath, const std::string &modelPath,
                    const std::string &text, std::string &out, int timeoutSec);

} // namespace nextless
