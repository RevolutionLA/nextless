// Unit test for the config-panel JSON writer (issue: full settings panel).
// Contract: the adapter writes the files, and the ONLY readers that matter are
// nextless_config.h's extractors (jsonStr/jsonInt/jsonBool/advancedSection),
// so every assertion round-trips through exactly those readers.
#include "json_patch.h"
#include "nextless_config.h"

#include <cstdio>
#include <fstream>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);\
            failures++;                                                        \
        }                                                                      \
    } while (0)

int main() {
    using namespace nextless;

    // 1. replace an existing top-level key in place; sibling keys survive.
    std::string t = "{\n    \"activation_msec\": 300,\n    \"keep_me\": 42\n}";
    CHECK(patchJson(t, "", "activation_msec", jsonNum(500)));
    CHECK(jsonInt(t, "activation_msec", -1) == 500);
    CHECK(t.find("keep_me") != std::string::npos);
    CHECK(jsonInt(t, "keep_me", -1) == 42);
    // idempotent second write reports "unchanged"
    CHECK(!patchJson(t, "", "activation_msec", jsonNum(500)));

    // 2. insert a missing key into a flat object.
    std::string f = "{\"denoise\": \"speexdsp\"}";
    CHECK(patchJson(f, "", "extra", jsonQuote("v1")));
    CHECK(jsonStr(f, "extra") == "v1");

    // 3. section replace + section-key insert + whole missing section.
    std::string adv =
        "{\"audio\": {\"lufs_target\": -16.0}, \"zipformer\": {\"model_dir\": \"~/a\"}}";
    CHECK(patchJson(adv, "audio", "lufs_target", "-18"));
    CHECK(jsonDouble(adv, "lufs_target", 0.0) == -18.0);
    CHECK(adv.find("speex_level") == std::string::npos);
    CHECK(patchJson(adv, "audio", "speex_level", jsonNum(-15)));
    CHECK(jsonInt(adv, "speex_level", 0) == -15);
    CHECK(patchJson(adv, "punctuation", "enabled", jsonBool(true)));
    std::string psec = adv.substr(adv.find("\"punctuation\""));
    CHECK(jsonBool(psec, "enabled", false) == true);

    // 4. empty object gets a member without a stray comma (valid JSON).
    std::string e = "{}";
    CHECK(patchJson(e, "sec", "k", jsonQuote("x")));
    CHECK(e.find(",") == std::string::npos);

    // 5. strings with quotes/backslashes serialize correctly.
    std::string q = jsonQuote("a\"b\\c");
    CHECK(q == "\"a\\\"b\\\\c\"");

    // 6. writeFileAtomic creates dirs and content.
    std::string path = "/tmp/nextless-jsonpatch-test/dir/x.json";
    std::remove(path.c_str());
    CHECK(writeFileAtomic(path, "{\"a\": 1}\n"));
    std::ifstream in(path);
    std::string back((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(back == "{\"a\": 1}\n");
    std::remove(path.c_str());
    std::remove("/tmp/nextless-jsonpatch-test/dir");
    std::remove("/tmp/nextless-jsonpatch-test");

    // 7. malformed input is a no-op, never a corruption.
    std::string bad = "not json";
    CHECK(!patchJson(bad, "", "k", "1"));
    CHECK(bad == "not json");

    std::printf(failures ? "JSON_PATCH_TESTS_FAILED %d\n" : "JSON_PATCH_TESTS_OK\n",
                failures);
    return failures ? 1 : 0;
}
