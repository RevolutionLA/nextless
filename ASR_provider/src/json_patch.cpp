#include "json_patch.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace nextless {
namespace {

constexpr size_t npos = std::string::npos;

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

size_t skipWs(const std::string &t, size_t i, size_t limit) {
    while (i < limit && isWs(t[i])) i++;
    return i;
}

// Given the index of an opening '{', return the index of its matching '}'
// (string/escape aware), or npos if unbalanced.
size_t matchClose(const std::string &t, size_t open) {
    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = open; i < t.size(); ++i) {
        char c = t[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') depth++;
        else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return npos;
}

// Within [from,to), find a quoted token that is used as a KEY (followed by ':').
// Returns the index of the opening quote, or npos.
size_t findKey(const std::string &t, size_t from, size_t to, const std::string &key) {
    size_t i = from;
    while (i < to) {
        if (t[i] != '"') { i++; continue; }
        size_t qs = i;
        size_t j = i + 1;
        while (j < to) {
            if (t[j] == '\\') { j += 2; continue; }
            if (t[j] == '"') break;
            j++;
        }
        if (j >= to) return npos;
        size_t after = skipWs(t, j + 1, to);
        if (after < to && t[after] == ':') {
            if (t.compare(qs + 1, j - (qs + 1), key) == 0) return qs;
        }
        i = j + 1;
    }
    return npos;
}

// Index just past the ':' that follows the key whose opening quote is at keyPos.
size_t colonAfter(const std::string &t, size_t keyPos, size_t to) {
    size_t j = keyPos + 1;
    while (j < to) {
        if (t[j] == '\\') { j += 2; continue; }
        if (t[j] == '"') break;
        j++;
    }
    size_t c = skipWs(t, j + 1, to);
    return (c < to && t[c] == ':') ? c : npos;
}

// [vs,ve) span of the scalar value that starts at `v`.
void valueSpan(const std::string &t, size_t v, size_t to, size_t &vs, size_t &ve) {
    vs = skipWs(t, v, to);
    if (vs >= to) { ve = vs; return; }
    if (t[vs] == '"') {
        size_t j = vs + 1;
        while (j < to) {
            if (t[j] == '\\') { j += 2; continue; }
            if (t[j] == '"') { j++; break; }
            j++;
        }
        ve = j;
        return;
    }
    size_t j = vs;
    while (j < to && t[j] != ',' && t[j] != '}' && t[j] != ']') j++;
    ve = j;
    while (ve > vs && isWs(t[ve - 1])) ve--;
}

bool replaceInObject(std::string &t, size_t os, size_t oe,
                     const std::string &key, const std::string &raw) {
    size_t kp = findKey(t, os, oe, key);
    if (kp != npos) {
        size_t colon = colonAfter(t, kp, oe);
        if (colon == npos) return false;
        size_t vs, ve;
        valueSpan(t, colon + 1, oe, vs, ve);
        if (t.compare(vs, ve - vs, raw) == 0) return false;
        t.replace(vs, ve - vs, raw);
        return true;
    }
    // Insert before the object's close.
    size_t close = skipWs(t, os, oe);
    bool empty = (close >= oe);
    std::string ins = std::string(empty ? "" : ", ") + "\"" + key + "\": " + raw;
    t.insert(oe, ins);
    return true;
}

} // namespace

std::string jsonQuote(const std::string &v) {
    std::string out = "\"";
    for (char c : v) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char *hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

bool patchJson(std::string &text, const std::string &section,
               const std::string &key, const std::string &rawValue) {
    if (key.empty()) return false;
    size_t topOpen = text.find('{');
    if (topOpen == npos) return false;
    size_t topClose = matchClose(text, topOpen);
    if (topClose == npos) return false;
    size_t os = topOpen + 1, oe = topClose;

    if (!section.empty()) {
        size_t sp = findKey(text, os, oe, section);
        if (sp == npos) {
            // create the section with the one key inside it
            std::string body = "\"" + section + "\": { \"" + key + "\": " + rawValue + " }";
            bool empty = (skipWs(text, os, oe) >= oe);
            text.insert(topClose, (empty ? "" : ", ") + body);
            return true;
        }
        size_t colon = colonAfter(text, sp, oe);
        if (colon == npos) return false;
        size_t v = skipWs(text, colon + 1, oe);
        if (v >= oe || text[v] != '{') return false; // section is not an object
        size_t sc = matchClose(text, v);
        if (sc == npos) return false;
        os = v + 1;
        oe = sc;
    }

    return replaceInObject(text, os, oe, key, rawValue);
}

bool writeFileAtomic(const std::string &path, const std::string &contents) {
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << contents;
        if (!f) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace nextless
