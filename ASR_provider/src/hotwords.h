#pragma once

#include <string>
#include <utility>
#include <vector>

namespace nextless {

// 热词/纠错表 (issue #6, 方案 1): ~/.config/nextless/hotwords.json 里放
// {"被错听的词": "想上屏的词"}, 最终文本在标点之前先过这张替换表。
// 所有后端统一生效(云端后端偶尔也会听错专有名词, 多一道确定性保险没有坏处)。
// 表缺失/为空/JSON 畸形 → 原样返回, 永远不能因为它丢字。
// 读文件按内容缓存: 按键路径外(识别线程)每句一次, 文件没变就零解析。
void applyHotwordReplacements(std::string &text);

// 扁平 string→string 映射解析 + 应用, 供单测直接驱动。
// 只支持顶层 {"k":"v",...}; 值里不嵌套对象。占位键(YOUR_ 前缀)、空键、
// 超长条目会被丢弃 —— 打包模板第一次落进 ~/.config 时不能真改用户的话。
std::vector<std::pair<std::string, std::string>> parseHotwordTable(
    const std::string &json);
void applyHotwordTable(const std::vector<std::pair<std::string, std::string>> &table,
                       std::string &text);

} // namespace nextless
