// Issue #4: local punctuation post-processing.
// Punctuating is decoration: it must apply to the two local backends only,
// must never eat or block an utterance (every failure mode keeps the raw
// text), and must respect the advanced.json switches. Drives the real
// spawn path with a fake shell binary, so it runs headless in CI.
#include "punctuator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

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

int main() {
    auto base = fs::temp_directory_path() /
                ("nextless-punctuator-" + std::to_string(getpid()));
    fs::remove_all(base);
    setenv("HOME", base.c_str(), 1);

    auto binPath = base / "bin/fake-offline-punctuation";
    auto modelDir = base / "models/punct";
    fs::create_directories(modelDir);
    writeFile(modelDir / "model.int8.onnx", "fake model");

    // Echoes "text。" when the model flag points at a readable file; the
    // FAKE_MODE env selects the failure shapes.
    writeFile(binPath,
              "#!/bin/sh\n"
              "for a in \"$@\"; do\n"
              "  case \"$a\" in\n"
              "    --ct-transformer=*) m=\"${a#*=}\" ;;\n"
              "    -*) ;;\n"
              "    *) t=\"$a\" ;;\n"
              "  esac\n"
              "done\n"
              "[ -f \"$m\" ] || exit 2\n"
              "if [ \"$FAKE_MODE\" = fail ]; then echo boom >&2; exit 1; fi\n"
              "if [ \"$FAKE_MODE\" = empty ]; then exit 0; fi\n"
              "if [ \"$FAKE_MODE\" = sleep ]; then sleep 30; printf late; exit 0; fi\n"
              "printf '%s。\\n' \"$t\"\n");
    fs::permissions(binPath, fs::perms::owner_all);

    auto configPath = base / ".config/nextless/advanced.json";
    writeFile(configPath,
              "{\"punctuation\":{\"model_dir\":\"" + modelDir.string() +
              "\",\"bin_path\":\"" + binPath.string() + "\"}}");

    // 1) local backends get punctuated
    std::string text = "你好吗";
    nextless::punctuateLocalText(text, "zipformer");
    check("zipformer result was not punctuated: " + text, text == "你好吗。");

    text = "今天天气不错";
    nextless::punctuateLocalText(text, "fire_red");
    check("fire_red result was not punctuated: " + text, text == "今天天气不错。");

    // 2) cloud backends and mock are a hard no-op (they ship their own
    //    punctuation; double-processing would also slow every utterance)
    text = "hello there";
    nextless::punctuateLocalText(text, "doubao");
    check("doubao must not be punctuated: " + text, text == "hello there");
    text = "hello world";
    nextless::punctuateLocalText(text, "mock");
    check("mock must not be punctuated: " + text, text == "hello world");

    // 3) child exits non-zero → raw text kept
    text = "出错了也要有字";
    setenv("FAKE_MODE", "fail", 1);
    nextless::punctuateLocalText(text, "zipformer");
    check("failed child must keep raw text: " + text, text == "出错了也要有字");

    // 4) child prints nothing → raw text kept
    text = "空输出";
    setenv("FAKE_MODE", "empty", 1);
    nextless::punctuateLocalText(text, "zipformer");
    check("empty stdout must keep raw text: " + text, text == "空输出");
    unsetenv("FAKE_MODE");

    // 5) timeout → runPunctuation returns false and the child is gone
    //    (30s sleep vs 1s budget; the process-group kill must reap it)
    setenv("FAKE_MODE", "sleep", 1);
    std::string out;
    bool ok = nextless::runPunctuation(binPath.string(), (modelDir / "model.int8.onnx").string(),
                                       "慢", out, 1);
    check("sleeping child must time out", !ok && out.empty());
    unsetenv("FAKE_MODE");

    // 6) model file missing → skipped before even spawning, raw kept
    fs::remove(modelDir / "model.int8.onnx");
    text = "没模型就原样";
    nextless::punctuateLocalText(text, "zipformer");
    check("missing model must keep raw text: " + text, text == "没模型就原样");
    writeFile(modelDir / "model.int8.onnx", "fake model");

    // 7) enabled=false in advanced.json is respected
    writeFile(configPath,
              "{\"punctuation\":{\"enabled\":false,\"model_dir\":\"" +
              modelDir.string() + "\",\"bin_path\":\"" + binPath.string() + "\"}}");
    text = "关掉就不动";
    nextless::punctuateLocalText(text, "zipformer");
    check("enabled:false must keep raw text: " + text, text == "关掉就不动");

    // 8) text starting with '-' would be parsed as a flag by sherpa
    writeFile(configPath,
              "{\"punctuation\":{\"model_dir\":\"" + modelDir.string() +
              "\",\"bin_path\":\"" + binPath.string() + "\"}}");
    text = "-dash-start";
    nextless::punctuateLocalText(text, "zipformer");
    check("leading-dash text must be passed through: " + text,
          text == "-dash-start");

    // 9) fp32 layout fallback: only model.onnx present
    fs::remove(modelDir / "model.int8.onnx");
    writeFile(modelDir / "model.onnx", "fake fp32");
    text = "用fp32也行";
    nextless::punctuateLocalText(text, "zipformer");
    check("model.onnx fallback failed: " + text, text == "用fp32也行。");

    fs::remove_all(base);
    return failures == 0 ? 0 : 1;
}
