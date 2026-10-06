// Issue #6: hotword / mishearing correction table.
// The table is plain data in ~/.config/nextless/hotwords.json and runs as a
// post-pass over every backend's result, so its contract is: never crash,
// never chain replacements, prefer the longest key, stay inert when the
// packaged placeholder template lands in ~/.config, and stay a no-op when
// the file is missing or malformed. Parsing and applying are tested directly
// (deterministic), plus one end-to-end pass through the cached file loader.
#include "hotwords.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;

static void check(const std::string &what, bool ok) {
    if (ok) return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void writeFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

using Table = std::vector<std::pair<std::string, std::string>>;

static std::string apply(const std::string &json, const std::string &text) {
    Table table = nextless::parseHotwordTable(json);
    std::string out = text;
    nextless::applyHotwordTable(table, out);
    return out;
}

int main() {
    // ---- parseHotwordTable ------------------------------------------------
    {
        Table t = nextless::parseHotwordTable(
            "{\"LIB\": \"x\", \"LIBR\": \"礼拜二\", \"alpha\": \"beta\"}");
        check("three pairs parsed", t.size() == 3);
        // Sorted by descending key length (alpha=5 > LIBR=4 > LIB=3), so an
        // overlapping pair always tries the more specific key first.
        check("longest key sorts first", !t.empty() && t.front().first == "alpha");
        auto it = std::find_if(t.begin(), t.end(),
                               [](const auto &kv) { return kv.first == "LIBR"; });
        auto lib = std::find_if(t.begin(), t.end(),
                                [](const auto &kv) { return kv.first == "LIB"; });
        check("LIBR ordered before LIB", it != t.end() && lib != t.end() &&
              it < lib);
        check("value kept", it != t.end() && it->second == "礼拜二");
    }
    {
        // Packaged template must parse to an empty (inert) table.
        Table t = nextless::parseHotwordTable(
            "{\n  \"YOUR_MISHEARD_WORD\": \"word you meant\"\n}\n");
        check("YOUR_ placeholder key dropped", t.empty());
        t = nextless::parseHotwordTable("{\"k\": \"YOUR_secret\"}");
        check("YOUR_ placeholder value dropped", t.empty());
        t = nextless::parseHotwordTable("{\"sk-YOUR\": \"v\"}");
        check("sk-YOUR placeholder key dropped", t.empty());
    }
    {
        // Oversized / empty entries are mis-edits, not words.
        std::string bigKey(65, 'a'), bigVal(257, 'b'), okKey(64, 'c'), okVal(256, 'd');
        Table t = nextless::parseHotwordTable("{\"" + bigKey + "\": \"x\", \"" +
                                              okKey + "\": \"" + okVal +
                                              "\", \"\": \"y\", \"k\": \"" + bigVal + "\"}");
        check("boundary sizes accepted, oversized dropped", t.size() == 1 &&
              t[0].first == okKey);
    }
    {
        // Non-string values are skipped, not turned into replacements.
        Table t = nextless::parseHotwordTable(
            "{\"a\": {\"nested\": \"x\"}, \"b\": [1,2], \"c\": 3, \"d\": \"ok\"}");
        check("non-string values skipped", t.size() == 1 && t[0].first == "d");
        // ...even when the skipped blob contains a quoted comma.
        t = nextless::parseHotwordTable("{\"a\": {\"n\": \"x,y\"}, \"d\": \"ok\"}");
        check("comma inside skipped value handled",
              t.size() == 1 && t[0].first == "d");
    }
    {
        // Malformed input must yield an empty table, never a crash.
        check("empty input", nextless::parseHotwordTable("").empty());
        check("no braces", nextless::parseHotwordTable("not json at all").empty());
        check("unterminated", nextless::parseHotwordTable("{\"a\": \"b").empty());
        check("truncated tail",
              nextless::parseHotwordTable("{\"a\": \"b\", \"c\": }").size() == 1);
    }
    {
        // Escapes in keys and values.
        Table t = nextless::parseHotwordTable("{\"a\\\"b\": \"c\\\\d\"}");
        check("escaped quote in key",
              t.size() == 1 && t[0].first == "a\"b" && t[0].second == "c\\d");
    }

    // ---- applyHotwordTable -------------------------------------------------
    {
        std::string out = apply("{\"LIBR\": \"礼拜二\", \"LIB\": \"里壁\"}",
                                "LIBR 开头");
        check("longest key wins: " + out, out == "礼拜二 开头");
    }
    {
        // Simultaneous pass: A->B and B->C must not compose into A->C.
        std::string out = apply("{\"A\": \"B\", \"B\": \"C\"}", "A and B");
        check("no chaining: " + out, out == "B and C");
    }
    {
        std::string out = apply("{\"你好\": \"nǐ hǎo\"}", "你好你好");
        check("repeated matches replaced: " + out, out == "nǐ hǎonǐ hǎo");
    }
    {
        std::string out = apply("{}", "untouched");
        check("empty table is a no-op: " + out, out == "untouched");
    }

    // ---- applyHotwordReplacements (file + cache path) ----------------------
    auto base = fs::temp_directory_path() /
                ("nextless-hotwords-" + std::to_string(getpid()));
    fs::remove_all(base);
    setenv("HOME", base.c_str(), 1);
    auto userCfg = base / ".config/nextless/hotwords.json";

    // 1) missing user file: whatever the packaged template or an absent
    //    /etc leaves behind, real speech must come out unchanged.
    std::string text = "我 LIBR 去开会";
    nextless::applyHotwordReplacements(text);
    check("template/absent file must stay inert: " + text, text == "我 LIBR 去开会");

    // 2) a real table takes effect...
    writeFile(userCfg, "{\"LIBR\": \"礼拜二\"}");
    text = "我 LIBR 去开会";
    nextless::applyHotwordReplacements(text);
    check("table applied: " + text, text == "我 礼拜二 去开会");

    // 3) ...and editing the file re-loads without a restart (cache keyed
    //    by content, not by process lifetime).
    writeFile(userCfg, "{\"LIBR\": \"星期二\", \"开会\": \"站会\"}");
    text = "LIBR 开会";
    nextless::applyHotwordReplacements(text);
    check("edited table reloaded: " + text, text == "星期二 站会");

    // 4) malformed file → documented no-op, and the old text is kept.
    writeFile(userCfg, "this is not json");
    text = "LIBR 开会";
    nextless::applyHotwordReplacements(text);
    check("malformed file is a no-op: " + text, text == "LIBR 开会");

    // 5) placeholders mixed with a real entry: only the real one survives.
    writeFile(userCfg, "{\"YOUR_\": \"x\", \"LIBR\": \"礼拜二\"}");
    text = "YOUR_ LIBR";
    nextless::applyHotwordReplacements(text);
    check("placeholders dropped, real entry kept: " + text,
          text == "YOUR_ 礼拜二");

    fs::remove_all(base);
    if (failures == 0) std::cout << "test_hotwords: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
