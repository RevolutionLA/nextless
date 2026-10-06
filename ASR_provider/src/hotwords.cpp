#include "hotwords.h"
#include "nextless_config.h"
#include "diagnostic_log.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

namespace nextless {
namespace {

// A hotword entry longer than this is not a word someone mishears — treat it
// as a mis-edit and drop it, so one pathological line can't make every
// utterance scan a huge needle. Table size is likewise bounded.
constexpr size_t kMaxKeyLen = 64;
constexpr size_t kMaxValueLen = 256;
constexpr size_t kMaxEntries = 500;

// Un-escape a JSON string body (only the escapes we ever need in a config
// value; \uXXXX is left literal rather than guessed at).
std::string unescapeJson(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size()) {
            out.push_back(c);
            continue;
        }
        char n = raw[++i];
        switch (n) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            default: out.push_back('\\'); out.push_back(n); break;
        }
    }
    return out;
}

// Scan a flat JSON object body (text between the outer braces) for
// "key":"value" pairs. Values that are not strings (objects/arrays/numbers)
// are skipped, so a nested config blob never becomes a bogus replacement.
void collectPairs(const std::string &body,
                  std::vector<std::pair<std::string, std::string>> &out) {
    size_t i = 0;
    auto readString = [&](std::string &dst) -> bool {
        while (i < body.size() && body[i] != '"') i++;
        if (i >= body.size()) return false;
        i++;  // past opening quote
        std::string acc;
        while (i < body.size()) {
            char c = body[i];
            if (c == '\\' && i + 1 < body.size()) {
                acc.push_back(c);
                acc.push_back(body[i + 1]);
                i += 2;
                continue;
            }
            if (c == '"') { i++; dst = unescapeJson(acc); return true; }
            acc.push_back(c);
            i++;
        }
        return false;  // unterminated
    };
    while (out.size() < kMaxEntries) {
        std::string key, value;
        if (!readString(key)) break;
        while (i < body.size() && body[i] != ':') i++;
        if (i >= body.size()) break;
        i++;  // past colon
        while (i < body.size() && std::isspace((unsigned char)body[i])) i++;
        if (i >= body.size()) break;
        if (body[i] != '"') {
            // Non-string value: skip to the next comma at this depth so the
            // following pair still parses. Quotes are tracked so a comma
            // inside a nested string ("a", "b") does not end the skip early.
            size_t depth = 0;
            bool inStr = false;
            while (i < body.size()) {
                char c = body[i];
                if (inStr) {
                    if (c == '\\' && i + 1 < body.size()) i++;
                    else if (c == '"') inStr = false;
                } else if (c == '"') inStr = true;
                else if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') { if (depth) depth--; }
                else if (c == ',' && depth == 0) { i++; break; }
                i++;
            }
            continue;
        }
        if (!readString(value)) break;
        out.emplace_back(std::move(key), std::move(value));
    }
}

bool isPlaceholder(const std::string &s) {
    return s.rfind("YOUR_", 0) == 0 || s.rfind("sk-YOUR", 0) == 0;
}

// Longest-key-first so overlapping entries prefer the more specific match;
// ties keep file order for determinism.
void sortByKeyLenDesc(std::vector<std::pair<std::string, std::string>> &t) {
    std::stable_sort(t.begin(), t.end(),
                     [](const auto &a, const auto &b) {
                         return a.first.size() > b.first.size();
                     });
}

// Guarded load + parse, cached by raw file content: an unchanged file is
// never re-read or re-parsed, and a malformed one yields an empty table
// (documented no-op) rather than throwing anything at the caller.
std::vector<std::pair<std::string, std::string>> loadTable() {
    static std::mutex mu;
    static std::string cachedRaw;
    static std::vector<std::pair<std::string, std::string>> cachedTable;
    static bool loaded = false;

    std::string raw = readConfigFile("hotwords.json");
    std::lock_guard<std::mutex> lock(mu);
    if (loaded && raw == cachedRaw) return cachedTable;

    std::vector<std::pair<std::string, std::string>> table;
    auto open = raw.find('{');
    auto close = raw.rfind('}');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        collectPairs(raw.substr(open + 1, close - open - 1), table);
    }
    // Drop placeholder / oversized entries so the packaged template (copied
    // into ~/.config on first read) can't rewrite real speech.
    std::erase_if(table, [](const auto &kv) {
        return kv.first.empty() || kv.first.size() > kMaxKeyLen ||
               kv.second.size() > kMaxValueLen ||
               isPlaceholder(kv.first) || isPlaceholder(kv.second);
    });
    sortByKeyLenDesc(table);
    cachedRaw = std::move(raw);
    cachedTable = table;
    loaded = true;
    return cachedTable;
}

} // namespace

std::vector<std::pair<std::string, std::string>> parseHotwordTable(
    const std::string &json) {
    std::vector<std::pair<std::string, std::string>> table;
    auto open = json.find('{');
    auto close = json.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close <= open)
        return table;
    collectPairs(json.substr(open + 1, close - open - 1), table);
    std::erase_if(table, [](const auto &kv) {
        return kv.first.empty() || kv.first.size() > kMaxKeyLen ||
               kv.second.size() > kMaxValueLen ||
               isPlaceholder(kv.first) || isPlaceholder(kv.second);
    });
    sortByKeyLenDesc(table);
    return table;
}

void applyHotwordTable(
    const std::vector<std::pair<std::string, std::string>> &table,
    std::string &text) {
    if (table.empty() || text.empty()) return;
    // Single simultaneous pass: at each byte offset, take the longest match
    // (table is longest-first) and jump past it. No entry feeds another's
    // replacement, so "A"->"B" plus "B"->"C" cannot turn A into C.
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        bool matched = false;
        for (const auto &kv : table) {
            const std::string &k = kv.first;
            if (k.size() <= text.size() - i &&
                text.compare(i, k.size(), k) == 0) {
                out.append(kv.second);
                i += k.size();
                matched = true;
                break;
            }
        }
        if (!matched) out.push_back(text[i++]);
    }
    text = std::move(out);
}

void applyHotwordReplacements(std::string &text) {
    if (text.empty()) return;
    auto table = loadTable();
    if (table.empty()) return;
    applyHotwordTable(table, text);
    diagnosticLog().event("hotwords", "applied", {});
}

} // namespace nextless
