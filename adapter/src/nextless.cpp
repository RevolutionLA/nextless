// fcitx5 核心头文件
#include <fcitx/addonfactory.h>    // AddonFactory 基类
#include <fcitx/addoninstance.h>   // AddonInstance 基类
#include <fcitx/addonmanager.h>    // AddonManager, 用于获取 fcitx 实例
#include <fcitx/instance.h>        // Instance, fcitx 服务器实例
#include <fcitx/event.h>           // KeyEvent
#include <fcitx/inputcontext.h>    // InputContext
#include <fcitx/inputpanel.h>       // InputPanel, setClientPreedit
#include <fcitx/inputcontextmanager.h>  // findByUUID
#include <fcitx-config/configuration.h>   // FCITX_CONFIGURATION
#include <fcitx-config/option.h>          // KeyListOption（可绑定纯修饰键）
#include <fcitx-config/iniparser.h>       // readAsIni, safeSaveAsIni
#include <fcitx-utils/i18n.h>             // _() translation macro
#include <fcitx-utils/event.h>     // EventLoop, addTimeEvent
#include <fcitx-utils/key.h>       // Key
#include <fcitx-utils/keysym.h>    // FcitxKey_Caps_Lock 等键值常量
#include <fcitx-utils/log.h>       // 日志宏 FCITX_INFO/FCITX_DEBUG 等

// uinput: 内核级常驻虚键设备, 兼容所有 Wayland compositor
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/uinput.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <spawn.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>
#include <fstream>
#include <optional>
#include <string_view>

// Nextless ASR provider 接口
#include "asr_provider.h"
#include "mock_provider.h"         // 确保 Mock 后端被链接并自动注册
#include "doubao_provider.h"      // 确保豆包后端被链接并自动注册
#include "qwen_provider.h"        // 确保千问后端被链接并自动注册
#include "audio_capture.h"
#include "diagnostic_log.h"
#include "output_handler.h"
#include "nextless_config.h"
#include "panel_status.h"

// notifications addon 公共 API (跨 addon 调用, 仅用于显示切换信息)
#include <fcitx-module/notifications/notifications_public.h>

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

static std::atomic<uint64_t> nextRecognitionId{1};

static std::string diagnosticHash(std::string_view value) {
    return nextless::hashDiagnosticValue(value).substr(0, 16);
}

// 配置: 定义 addon 的可配置选项
FCITX_CONFIGURATION(
    NextlessConfig,
    fcitx::KeyListOption hotkey{
        this,
        "Hotkey",
        _("Push-to-talk key (hold to speak, release to finish)"),
        {fcitx::Key("Control_R")},
        fcitx::KeyListConstrain({fcitx::KeyConstrainFlag::AllowModifierOnly,
                                 fcitx::KeyConstrainFlag::AllowModifierLess})};
    fcitx::Option<std::string> defaultProvider{
        this, "DefaultProvider", _("Default ASR Provider"), "zipformer"};
);

// NextlessAddon — Nextless 语音输入插件的 addon 主体
// 继承 AddonInstance, fcitx5 加载 addon 时实例化此类
class NextlessAddon : public fcitx::AddonInstance {
    struct CallbackGate {
        std::mutex mutex;
        NextlessAddon *owner = nullptr;
    };

    template<typename Callback>
    static void withOwner(const std::shared_ptr<CallbackGate> &gate,
                          Callback &&callback) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->owner) callback(*gate->owner);
    }

public:
    NextlessAddon(fcitx::Instance *instance) : instance_(instance) {
        callbackGate_->owner = this;
        reloadConfig();

        auto vjson = nextless::readConfigFile("nextless.json");
        if (!vjson.empty()) {
            activationUsec_ = (uint64_t)nextless::jsonInt(vjson, "activation_msec", 300) * 1000;
            notificationTimeout_ = nextless::jsonInt(vjson, "notification_timeout", 2000);
            debounceCount_ = nextless::jsonInt(vjson, "debounce_count", 2);
        }

        FCITX_INFO() << "Nextless addon loaded";
        nextless::diagnosticLog().event("adapter", "addon_loaded", {
            {"diagnostics", NEXTLESS_DIAGNOSTICS_ENABLED ? "enabled" : "disabled"}
        });

        // 创建常驻 uinput 虚键盘, 用于还原 CapsLock
        initUinput();

        outputHandler_ = std::make_unique<nextless::OutputHandler>(instance_);

        // 在 PreInputMethod 阶段监听键盘事件 (早于输入法引擎)
        keyWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
                onKeyEvent(keyEvent);
            });
    }

    ~NextlessAddon() override {
        nextless::diagnosticLog().event("adapter", "addon_shutdown_begin");
        {
            std::lock_guard<std::mutex> lock(callbackGate_->mutex);
            callbackGate_->owner = nullptr;
        }
        shuttingDown_ = true;
        if (audioCapture_) audioCapture_->stop();
        for (auto &capture : finishingCaptures_) capture->stop();
        if (audioCapture_) audioCapture_->wait();
        for (auto &capture : finishingCaptures_) capture->wait();
        audioCapture_.reset();
        finishingCaptures_.clear();
        asr_.reset();
        for (const auto &request : recognitionQueue_) {
            unlink(request.wavPath.c_str());
        }
        recognitionQueue_.clear();
        if (uinputFd_ >= 0) {
            ioctl(uinputFd_, UI_DEV_DESTROY);
            close(uinputFd_);
        }
        nextless::diagnosticLog().event("adapter", "addon_shutdown_end");
    }

    // 配置读写
    void reloadConfig() override {
        readAsIni(config_, confFile);
        FCITX_INFO() << "Nextless: hotkeys " << hotkeyToString()
                     << ", provider " << config_.defaultProvider.value();
    }
    const fcitx::Configuration *getConfig() const override {
        return &config_;
    }
    void setConfig(const fcitx::RawConfig &config) override {
        config_.load(config, true);
        safeSaveAsIni(config_, confFile);
        // 换键立即生效（onKeyEvent 每次读 config_）；只有从别的键换成
        // CapsLock 时才需要额外的 uinput 反弹设备，那种情况重启一次 fcitx5。
        if (triggerNeedsRevert() && uinputFd_ < 0) initUinput();
        FCITX_INFO() << "Nextless: hotkeys now " << hotkeyToString();
    }

private:
    static constexpr char confFile[] = "conf/nextless.conf";
    uint64_t activationUsec_ = 300 * 1000;  // from nextless.json: activation_msec
    int notificationTimeout_ = 2000;         // from nextless.json: notification_timeout
    int debounceCount_ = 2;                   // from nextless.json: debounce_count

    // ---- 触发键判定（可配置，支持纯修饰键）----
    // 修饰键（左/右 Ctrl、Alt、Shift、Super）的 press 事件里 states 已经带上了
    // 自身和其它修饰键，所以只能比 sym；普通键走 check()（要求修饰键组合一致）。
    bool isTriggerPress(const fcitx::KeyEvent &keyEvent) const {
        const fcitx::Key &key = keyEvent.key();
        for (const fcitx::Key &k : *config_.hotkey) {
            if (k.isModifier() ? key.sym() == k.sym() : key.check(k)) return true;
        }
        return false;
    }
    // release 事件里修饰键状态已经在变，比 sym 就够
    bool isTriggerRelease(const fcitx::KeyEvent &keyEvent) const {
        const fcitx::Key &key = keyEvent.key();
        for (const fcitx::Key &k : *config_.hotkey) {
            if (key.sym() == k.sym()) return true;
        }
        return false;
    }
    // 只有 CapsLock 这类"锁定键"当触发键时才需要 uinput 反弹还原；
    // 换成普通修饰键后没有锁定状态要还原，整套 uinput hack 直接不启用。
    bool triggerNeedsRevert() const {
        for (const fcitx::Key &k : *config_.hotkey) {
            if (k.sym() == FcitxKey_Caps_Lock) return true;
        }
        return false;
    }
    std::string hotkeyToString() const {
        return fcitx::Key::keyListToString(*config_.hotkey);
    }

    // 创建常驻 uinput 虚拟键盘设备, 用于还原 CapsLock
    void initUinput() {
        if (!triggerNeedsRevert()) {
            FCITX_INFO() << "Nextless: trigger key is " << hotkeyToString()
                         << ", no locking-key revert needed (uinput disabled)";
            return;
        }
        uinputFd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (uinputFd_ < 0) {
            FCITX_INFO() << "Nextless: cannot open /dev/uinput";
            return;
        }
        ioctl(uinputFd_, UI_SET_EVBIT, EV_KEY);
        ioctl(uinputFd_, UI_SET_KEYBIT, KEY_CAPSLOCK);
        ioctl(uinputFd_, UI_SET_EVBIT, EV_LED);
        ioctl(uinputFd_, UI_SET_LEDBIT, LED_CAPSL);

        struct uinput_setup usetup = {};
        strcpy(usetup.name, "Nextless vkbd");
        usetup.id.bustype = BUS_VIRTUAL;
        ioctl(uinputFd_, UI_DEV_SETUP, &usetup);
        ioctl(uinputFd_, UI_DEV_CREATE);
        FCITX_INFO() << "Nextless uinput device created";
    }

    // 还原 CapsLock — 通过 uinput 虚键发送 CapsLock (还原 LED, 会触发 IM 切换但马上恢复)
    void revertCapsLock() {
        if (uinputFd_ < 0) return;

        struct input_event ev = {};
        ev.type = EV_KEY;
        ev.code = KEY_CAPSLOCK;
        ev.value = 1;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        (void)!write(uinputFd_, &ev, sizeof(ev));

        ev = {};
        ev.type = EV_KEY;
        ev.code = KEY_CAPSLOCK;
        ev.value = 0;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        FCITX_INFO() << "Nextless revert CapsLock via uinput";
    }

    fcitx::Instance *instance_;
    NextlessConfig config_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyWatcher_;
    int uinputFd_ = -1;
    int revertDebounce_ = 0;            // uinput CapsLock 反弹去抖计数
    std::vector<pid_t> soundPlayers_;   // 还没退出的提示音播放器

    // Output: encapsulates self-pipe and commit
    std::unique_ptr<nextless::OutputHandler> outputHandler_;

    // 运行时依赖: notifications addon (仅用于切换显示)
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());

    // 提示音: 用户目录优先, 其次随包安装的 sounds 目录; 播放器先试 paplay 再试 pw-play
    void playSound(const std::string &name) {
        reapSoundPlayers();
        const std::string path = findSound(name);
        if (path.empty()) return;

        // paplay 需要 PULSE_RUNTIME_PATH 环境变量
        const char *pulsePath = getenv("PULSE_RUNTIME_PATH");
        const char *xdgRuntime = getenv("XDG_RUNTIME_DIR");
        std::string paEnv;
        if (pulsePath) paEnv = std::string("PULSE_RUNTIME_PATH=") + pulsePath;
        else if (xdgRuntime) paEnv = std::string("PULSE_RUNTIME_PATH=") + xdgRuntime + "/pulse";

        const char *envp[2] = {paEnv.empty() ? nullptr : paEnv.c_str(), nullptr};
        for (const char *player : {"paplay", "pw-play"}) {
            const char *argv[] = {player, path.c_str(), nullptr};
            pid_t pid;
            if (posix_spawnp(&pid, player, nullptr, nullptr, (char *const *)argv,
                             envp[0] ? (char *const *)envp : nullptr) != 0) {
                continue;   // 这个播放器没装, 换下一个
            }
            soundPlayers_.push_back(pid);
            return;
        }
    }

    std::string findSound(const std::string &name) const {
        const std::string file = name + ".wav";
        std::vector<std::string> dirs = {
            expandPath("~/.local/share/nextless/sounds"),
#ifdef NEXTLESS_PACKAGED_SOUND_DIR
            NEXTLESS_PACKAGED_SOUND_DIR,
#endif
        };
        for (const auto &dir : dirs) {
            std::string path = dir + "/" + file;
            if (access(path.c_str(), R_OK) == 0) return path;
        }
        return {};
    }

    // 播放器是 fcitx5 的子进程。放音前非阻塞回收之前记下的那些，僵尸最多攒一两个。
    // 只 waitpid 自己记的 pid，绝不装 SIGCHLD 处理器去抢 provider 那些 sherpa 子进程。
    void reapSoundPlayers() {
        std::erase_if(soundPlayers_, [](pid_t pid) {
            int status = 0;
            return waitpid(pid, &status, WNOHANG) != 0;
        });
    }

    // 状态
    bool active_ = false;               // 语音录音中
    bool switchActive_ = false;         // Ctrl+CapsLock 切换模式
    std::atomic_bool shuttingDown_{false};
    std::shared_ptr<CallbackGate> callbackGate_ =
        std::make_shared<CallbackGate>();

    // 性能计时
    std::chrono::steady_clock::time_point tPress_, tActivate_, tStop_;
    std::unique_ptr<fcitx::EventSourceTime> timer_;
    std::unique_ptr<nextless::IAsrProvider> asr_;
    std::string asrProviderId_;
    std::unique_ptr<nextless::AudioCapture> audioCapture_;
    std::vector<std::unique_ptr<nextless::AudioCapture>> finishingCaptures_;
    nextless::OutputTarget currentTarget_;
    fcitx::InputContext *currentIC_ = nullptr;
    fcitx::ICUUID currentUuid_ = {};  // 用于 deactivate 后仍能查找 IC
    int providerIndex_ = 0;
    int denoiserIndex_ = 0;
    uint64_t currentRecognitionId_ = 0;

    struct RecognitionRequest {
        std::vector<int16_t> samples;
        std::string wavPath;
        std::string providerId;
        nextless::OutputTarget target;
        std::chrono::steady_clock::time_point pressTime;
        uint64_t recognitionId = 0;
    };
    static constexpr size_t kMaxPendingRecognitions = 3;
    std::deque<RecognitionRequest> recognitionQueue_;
    std::optional<RecognitionRequest> activeRecognition_;

    static const std::vector<std::string>& denoiserList() {
        static const std::vector<std::string> list = {"none", "speexdsp", "deepfilter"};
        return list;
    }

    // 从 keyEvent 提取切换方向, 0 表示非方向键
    static int switchDirection(const fcitx::KeyEvent &keyEvent) {
        switch (keyEvent.key().sym()) {
            case FcitxKey_Left:  case FcitxKey_h: return -1;
            case FcitxKey_Right: case FcitxKey_l: return  1;
            default: return 0;
        }
    }

    static int switchVertical(const fcitx::KeyEvent &keyEvent) {
        switch (keyEvent.key().sym()) {
            case FcitxKey_Up:   case FcitxKey_k: return -1;
            case FcitxKey_Down: case FcitxKey_j: return  1;
            default: return 0;
        }
    }

    // 仅切换 provider index + 通知 + 持久化, 不创建/启动 ASR 实例
    void doProviderSwitch(int direction) {
        auto list = nextless::AsrProviderRegistry::instance().listFactories();
        if (list.empty()) return;

        providerIndex_ = (providerIndex_ + direction + (int)list.size()) % (int)list.size();
        const auto &[nextId, nextName] = list[providerIndex_];

        auto total = (int)list.size();
        auto msg = nextName + " (" + std::to_string(providerIndex_ + 1)
                   + "/" + std::to_string(total) + ")";
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-nextless", 0, "fcitx-nextless",
            "Nextless", msg,
            std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);

        FCITX_INFO() << "Nextless switch ASR provider: " << nextName;
        config_.defaultProvider.setValue(nextId);
        safeSaveAsIni(config_, confFile);
        playSound("switch");
    }

    // 切换降噪后端 + 通知 + 持久化
    void doDenoiserSwitch(int direction) {
        auto &list = denoiserList();
        denoiserIndex_ = (denoiserIndex_ + direction + (int)list.size()) % (int)list.size();
        const auto &name = list[denoiserIndex_];

        auto total = (int)list.size();
        auto msg = std::string("Denoiser: ") + name
                   + " (" + std::to_string(denoiserIndex_ + 1)
                   + "/" + std::to_string(total) + ")";
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-nextless", 0, "fcitx-nextless",
            "Nextless", msg,
            std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);

        // 持久化到 audio.json
        const char *home = getenv("HOME");
        if (home) {
            std::string path = std::string(home) + "/.config/nextless/audio.json";
            std::string content = "{\"denoise\": \"" + name + "\"}\n";
            FILE *f = fopen(path.c_str(), "w");
            if (f) {
                fwrite(content.c_str(), 1, content.size(), f);
                fclose(f);
            }
        }

        FCITX_INFO() << "Nextless switch denoiser: " << name;
        playSound("switch");
    }

    // 键盘事件回调
    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        // 触发键可配置（默认右 Ctrl）；press 与 release 的匹配方式不同
        const bool trigger = keyEvent.isRelease()
            ? isTriggerRelease(keyEvent)
            : isTriggerPress(keyEvent);

        // 放行触发键; 切换模式或录音中放行所有键
        if (!trigger && !active_ && !switchActive_ && !timer_) return;

        if (keyEvent.isRelease()) {
            if (trigger) {
                nextless::diagnosticLog().event("input", "trigger_release", {
                    {"recognition_id", std::to_string(currentRecognitionId_)},
                    {"hotkey", hotkeyToString()},
                    {"active", active_ ? "true" : "false"},
                    {"switch_active", switchActive_ ? "true" : "false"},
                    {"timer", timer_ ? "true" : "false"},
                    {"revert_debounce", std::to_string(revertDebounce_)}
                });
                if (revertDebounce_ > 0) {
                    revertDebounce_--;
                    return;
                }
                if (active_) {
                    onDeactivate();
                } else if (switchActive_) {
                    switchActive_ = false;
                    revertDebounce_ = triggerNeedsRevert() ? debounceCount_ : 0;
                    playSound("deactivate");
                    revertCapsLock();
                } else {
                    timer_.reset();
                }
                // 只有 CapsLock（锁定键）必须被吞掉，否则大写锁会被真切下去；
                // 普通修饰键触发键不吞，右 Ctrl 仍然可以正常当 Ctrl 用。
                if (keyEvent.key().sym() == FcitxKey_Caps_Lock) {
                    keyEvent.filterAndAccept();
                }
            }
            return;
        }

        // ---- 按下事件 ----
        if (trigger) {
            nextless::diagnosticLog().event("input", "trigger_press", {
                {"recognition_id", std::to_string(currentRecognitionId_)},
                {"hotkey", hotkeyToString()},
                {"active", active_ ? "true" : "false"},
                {"switch_active", switchActive_ ? "true" : "false"},
                {"timer", timer_ ? "true" : "false"},
                {"revert_debounce", std::to_string(revertDebounce_)}
            });
            if (revertDebounce_ > 0) {
                revertDebounce_--;
                return;
            }
            if (timer_ || active_ || switchActive_) return;
            tPress_ = std::chrono::steady_clock::now();
            currentIC_ = keyEvent.inputContext();
            if (currentIC_) {
                currentUuid_ = currentIC_->uuid();
                FCITX_INFO() << "Nextless [press] ic=" << currentIC_
                             << " program=" << currentIC_->program()
                             << " frontend=" << currentIC_->frontendName();
            } else {
                FCITX_INFO() << "Nextless [press] no input context";
            }

            uint32_t states = (uint32_t)keyEvent.key().states().toInteger();
            bool shiftHeld = (states & (uint32_t)fcitx::KeyState::Shift) != 0;
            bool ctrlHeld = (states & (uint32_t)fcitx::KeyState::Ctrl) != 0;
            // 进入切换模式的组合键：Shift+触发键；若触发键仍是 CapsLock，
            // 兼容老用户的习惯（Ctrl+CapsLock）
            bool switchCombo = shiftHeld || (triggerNeedsRevert() && ctrlHeld);

            if (switchCombo) {
                // 组合键: 进入切换模式 (不启用录音)
                switchActive_ = true;
                FCITX_INFO() << "Nextless switch mode active";

                auto list = nextless::AsrProviderRegistry::instance().listFactories();
                if (!list.empty()) {
                    auto &dnList = denoiserList();
                    int di = denoiserIndex_;
                    if (di < 0 || di >= (int)dnList.size()) di = 0;
                    auto msg = std::string("ASR: ") + list[providerIndex_].second
                               + " (" + std::to_string(providerIndex_ + 1)
                               + "/" + std::to_string((int)list.size()) + ")\n"
                               + "Denoiser: " + dnList[di]
                               + " (" + std::to_string(di + 1)
                               + "/" + std::to_string((int)dnList.size()) + ")";
                    notifications()->call<fcitx::INotifications::sendNotification>(
                        "fcitx5-nextless", 0, "fcitx-nextless",
                        "Nextless", msg,
                        std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);
                }
            } else {
                // 单独按住触发键: 启动长按计时器
                timer_ = instance_->eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC,
                    fcitx::now(CLOCK_MONOTONIC) + activationUsec_, 0,
                    [this](fcitx::EventSourceTime *, uint64_t) {
                        onActivate();
                        return false;
                    });
            }
            // CapsLock 必须吞掉，否则大写锁定状态被真切下去；切换模式的组合键也吞。
            // 普通修饰键（默认右 Ctrl）不吞 —— 按住它照常打字、照常做 Ctrl+组合键。
            if (keyEvent.key().sym() == FcitxKey_Caps_Lock || switchCombo) {
                keyEvent.filterAndAccept();
            }
            return;
        }

        // 切换模式下: 箭头/h/l/j/k 键切换 (可多次)
        if (switchActive_) {
            int dir = switchDirection(keyEvent);
            if (dir != 0) {
                doProviderSwitch(dir);
                keyEvent.filterAndAccept();
                return;
            }
            dir = switchVertical(keyEvent);
            if (dir != 0) {
                doDenoiserSwitch(dir);
                keyEvent.filterAndAccept();
                return;
            }
        }
    }

    // 长按 500ms 后触发
    void onActivate() {
        reapFinishedCaptures();
        timer_.reset();
        tActivate_ = std::chrono::steady_clock::now();
        auto pressMs = std::chrono::duration_cast<std::chrono::milliseconds>(tActivate_ - tPress_).count();
        FCITX_INFO() << "Nextless activated (press→activate=" << pressMs << "ms)";
        const auto recognitionId = nextRecognitionId.fetch_add(1);
        currentRecognitionId_ = recognitionId;

        // 重新捕获当前焦点窗口 (比 KeyEvent::inputContext 更可靠)
        auto *ic = instance_->mostRecentInputContext();
        if (ic) {
            currentIC_ = ic;
            currentUuid_ = ic->uuid();
            FCITX_INFO() << "Nextless [activate] ic=" << ic
                         << " program=" << ic->program()
                         << " frontend=" << ic->frontendName();
        } else {
            nextless::diagnosticLog().event("adapter", "activation_no_input_context", {
                {"recognition_id", std::to_string(recognitionId)}
            });
            FCITX_INFO() << "Nextless [activate] no input context";
            return;
        }

        // 捕获当前焦点窗口 (通过 OutputHandler 的桌面策略)
        if (outputHandler_) {
            currentTarget_ = outputHandler_->captureCurrentUuid(recognitionId);
            outputHandler_->showStatus(currentTarget_, "Nextless: listening...");
        }
        FCITX_INFO() << "Nextless [activate] captured window";
        nextless::diagnosticLog().event("adapter", "recognition_activated", {
            {"recognition_id", std::to_string(recognitionId)},
            {"provider_config", config_.defaultProvider.value()},
            {"press_to_activate_ms", std::to_string(pressMs)}
        });

        auto list = nextless::AsrProviderRegistry::instance().listFactories();
        if (list.empty()) {
            FCITX_INFO() << "Nextless: no ASR provider registered";
            return;
        }

        // 根据配置中的默认后端 ID 查找索引
        const auto &defaultId = config_.defaultProvider.value();
        for (int i = 0; i < (int)list.size(); i++) {
            if (list[i].first == defaultId) {
                providerIndex_ = i;
                break;
            }
        }

        active_ = true;
        const auto providerId = list[providerIndex_].first;
        const auto target = currentTarget_;
        const auto pressTime = tPress_;
        auto callbackGate = callbackGate_;

        audioCapture_ = std::make_unique<nextless::AudioCapture>();
        audioCapture_->setDiagnosticId(recognitionId);
        {
            // 从 audio.json 读取初始降噪方法，设置到 AudioCapture
            const char *home = getenv("HOME");
            if (home) {
                std::string path = std::string(home) + "/.config/nextless/audio.json";
                std::ifstream f(path);
                if (f) {
                    std::string json((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
                    auto pos = json.find("\"denoise\"");
                    if (pos != std::string::npos) {
                        pos = json.find('"', json.find(':', pos) + 1);
                        if (pos != std::string::npos) {
                            pos++;
                            auto end = json.find('"', pos);
                            if (end != std::string::npos) {
                                auto method = json.substr(pos, end - pos);
                                auto &list = denoiserList();
                                for (int i = 0; i < (int)list.size(); i++) {
                                    if (list[i] == method) { denoiserIndex_ = i; break; }
                                }
                            }
                        }
                    }
                }
            }
        }
        audioCapture_->setRecordedCallback([callbackGate, providerId, target, pressTime,
                                             recognitionId](
                                               const std::vector<int16_t> &samples,
                                               const std::string &wav) {
            std::lock_guard<std::mutex> lock(callbackGate->mutex);
            auto *owner = callbackGate->owner;
            if (!owner) {
                nextless::diagnosticLog().event("adapter", "capture_callback_after_shutdown", {
                    {"recognition_id", std::to_string(recognitionId)},
                    {"wav_hash", diagnosticHash(wav)}
                });
                unlink(wav.c_str());
                return;
            }
            nextless::diagnosticLog().event("adapter", "capture_recorded_callback", {
                {"recognition_id", std::to_string(recognitionId)},
                {"sample_count", std::to_string(samples.size())},
                {"wav_hash", diagnosticHash(wav)}
            });
            auto request = std::make_shared<RecognitionRequest>(RecognitionRequest{
                samples, wav, providerId, target, pressTime, recognitionId});
            owner->outputHandler_->showStatus(
                target, "Nextless: recognizing...",
                [callbackGate, request = std::move(request)] {
                    withOwner(callbackGate, [&](NextlessAddon &owner) {
                        owner.enqueueRecognition(std::move(*request));
                    });
                });
        });
        audioCapture_->setStateCallback([](bool active) {
            FCITX_INFO() << "Nextless ASR state: " << (active ? "on" : "off");
        });
        audioCapture_->setStatusTextCallback([callbackGate, target, recognitionId](const std::string &text) {
            nextless::diagnosticLog().event("adapter", "capture_status", {
                {"recognition_id", std::to_string(recognitionId)},
                {"status_hash", diagnosticHash(text)},
                {"status_length", std::to_string(text.size())}
            });
            withOwner(callbackGate, [&](NextlessAddon &owner) {
                owner.outputHandler_->showStatus(target, text);
            });
        });
        // 整段录音里没有语音: 什么都不上屏, 把面板上残留的状态清掉就好
        audioCapture_->setSilenceCallback([callbackGate, target]() {
            withOwner(callbackGate, [&](NextlessAddon &owner) {
                owner.outputHandler_->showStatus(target, "");
            });
        });

        playSound("activate");
        audioCapture_->start();
    }

    void enqueueRecognition(RecognitionRequest request) {
        nextless::diagnosticLog().event("adapter", "recognition_enqueue_attempt", {
            {"recognition_id", std::to_string(request.recognitionId)},
            {"provider", request.providerId},
            {"queue_size", std::to_string(recognitionQueue_.size())},
            {"active_id", activeRecognition_ ?
                std::to_string(activeRecognition_->recognitionId) : "0"}
        });
        if (shuttingDown_) {
            nextless::diagnosticLog().event("adapter", "recognition_dropped_shutdown", {
                {"recognition_id", std::to_string(request.recognitionId)}
            });
            unlink(request.wavPath.c_str());
            return;
        }
        if (recognitionQueue_.size() + (activeRecognition_ ? 1 : 0) >=
            kMaxPendingRecognitions) {
            unlink(request.wavPath.c_str());
            nextless::diagnosticLog().event("adapter", "recognition_dropped_queue_full", {
                {"recognition_id", std::to_string(request.recognitionId)},
                {"queue_size", std::to_string(recognitionQueue_.size())},
                {"active_id", activeRecognition_ ?
                    std::to_string(activeRecognition_->recognitionId) : "0"}
            });
            outputHandler_->showStatus(request.target,
                                       "Nextless: recognition queue full; try again");
            return;
        }
        recognitionQueue_.push_back(std::move(request));
        nextless::diagnosticLog().event("adapter", "recognition_enqueued", {
            {"recognition_id", std::to_string(recognitionQueue_.back().recognitionId)},
            {"queue_size", std::to_string(recognitionQueue_.size())}
        });
        dispatchNextRecognition();
    }

    bool ensureAsrProvider(const std::string &providerId) {
        if (asr_ && asrProviderId_ == providerId) return true;
        if (asr_) {
            nextless::diagnosticLog().event("adapter", "provider_replaced", {
                {"old_provider", asrProviderId_},
                {"new_provider", providerId},
                {"recognition_id", activeRecognition_ ?
                    std::to_string(activeRecognition_->recognitionId) : "0"}
            });
        }
        asr_.reset();
        asr_ = nextless::AsrProviderRegistry::instance().create(providerId);
        asrProviderId_ = asr_ ? providerId : std::string{};
        return static_cast<bool>(asr_);
    }

    void dispatchNextRecognition() {
        if (activeRecognition_ || recognitionQueue_.empty() || shuttingDown_) {
            nextless::diagnosticLog().event("adapter", "recognition_dispatch_blocked", {
                {"reason", activeRecognition_ ? "active" :
                           (recognitionQueue_.empty() ? "empty" : "shutting_down")},
                {"active_id", activeRecognition_ ?
                    std::to_string(activeRecognition_->recognitionId) : "0"},
                {"queue_size", std::to_string(recognitionQueue_.size())}
            });
            return;
        }
        activeRecognition_ = std::move(recognitionQueue_.front());
        recognitionQueue_.pop_front();
        nextless::diagnosticLog().event("adapter", "recognition_dispatch_begin", {
            {"recognition_id", std::to_string(activeRecognition_->recognitionId)},
            {"provider", activeRecognition_->providerId},
            {"wav_hash", diagnosticHash(activeRecognition_->wavPath)},
            {"queue_size", std::to_string(recognitionQueue_.size())}
        });

        if (!ensureAsrProvider(activeRecognition_->providerId)) {
            auto target = activeRecognition_->target;
            const auto recognitionId = activeRecognition_->recognitionId;
            unlink(activeRecognition_->wavPath.c_str());
            activeRecognition_.reset();
            nextless::diagnosticLog().event("adapter", "recognition_provider_unavailable", {
                {"recognition_id", std::to_string(recognitionId)}
            });
            outputHandler_->showStatus(target, "Nextless: ASR provider unavailable",
                                       [callbackGate = callbackGate_] {
                                           withOwner(callbackGate, [](NextlessAddon &owner) {
                                               owner.dispatchNextRecognition();
                                           });
                                       });
            return;
        }

        auto target = activeRecognition_->target;
        auto tPress = activeRecognition_->pressTime;
        const auto recognitionId = activeRecognition_->recognitionId;
        auto callbackGate = callbackGate_;
        asr_->setDiagnosticId(recognitionId);
        asr_->setResultCallback([callbackGate, target, tPress, recognitionId](const std::string &text, bool isFinal) {
            auto tResult = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tPress).count();
            fprintf(stderr, "Nextless [timer] press→result=%ldms\n", ms);
            FCITX_INFO() << "Nextless ASR result: text_len=" << text.size()
                         << " (final=" << isFinal << ")";
            nextless::diagnosticLog().event("adapter", "recognition_result_callback", {
                {"recognition_id", std::to_string(recognitionId)},
                {"is_final", isFinal ? "true" : "false"},
                {"text_length", std::to_string(text.size())},
                {"text_hash", diagnosticHash(text)}
            });
            withOwner(callbackGate, [&](NextlessAddon &owner) {
                owner.outputHandler_->submit(target, text, [callbackGate, target] {
                    withOwner(callbackGate, [&](NextlessAddon &owner) {
                        owner.outputHandler_->showStatus(target, "");
                        owner.finishRecognition();
                    });
                });
            });
        });
        asr_->setErrorCallback([callbackGate, target, recognitionId](const std::string &error) {
            FCITX_INFO() << "Nextless ASR error: " << error;
            nextless::diagnosticLog().event("adapter", "recognition_error_callback", {
                {"recognition_id", std::to_string(recognitionId)},
                {"error_length", std::to_string(error.size())},
                {"error_hash", diagnosticHash(error)}
            });
            // 错在哪、面板说什么, 见 panel_status.h；空串表示按 no-op 处理。
            std::string status = nextless::panelStatusForError(error);
            withOwner(callbackGate, [&](NextlessAddon &owner) {
                owner.outputHandler_->showStatus(target, status, [callbackGate] {
                    withOwner(callbackGate, [](NextlessAddon &owner) {
                        owner.finishRecognition();
                    });
                });
            });
        });
        asr_->transcribe(std::move(activeRecognition_->samples),
                         activeRecognition_->wavPath);
        nextless::diagnosticLog().event("adapter", "recognition_provider_called", {
            {"recognition_id", std::to_string(recognitionId)},
            {"provider", activeRecognition_->providerId}
        });
    }

    void finishRecognition() {
        const auto recognitionId = activeRecognition_ ? activeRecognition_->recognitionId : 0;
        nextless::diagnosticLog().event("adapter", "recognition_finished", {
            {"recognition_id", std::to_string(recognitionId)},
            {"queue_size", std::to_string(recognitionQueue_.size())}
        });
        activeRecognition_.reset();
        dispatchNextRecognition();
    }

    void reapFinishedCaptures() {
        std::erase_if(finishingCaptures_, [](const auto &capture) {
            return capture->finished();
        });
    }

    // 松键后结束
    void onDeactivate() {
        active_ = false;
        tStop_ = std::chrono::steady_clock::now();
        auto recMs = std::chrono::duration_cast<std::chrono::milliseconds>(tStop_ - tActivate_).count();
        FCITX_INFO() << "Nextless deactivated (record=" << recMs << "ms)";
        nextless::diagnosticLog().event("adapter", "capture_stop_requested", {
            {"recognition_id", std::to_string(currentRecognitionId_)},
            {"record_ms", std::to_string(recMs)}
        });

        if (audioCapture_) {
            outputHandler_->showStatus(currentTarget_, "Nextless: processing audio...");
            audioCapture_->stop();
            finishingCaptures_.push_back(std::move(audioCapture_));
        }
        currentIC_ = nullptr;

        playSound("deactivate");  // 结束音: 低音

        // 仅当触发键是 CapsLock 时才需要补一个假按键还原锁定状态
        revertDebounce_ = triggerNeedsRevert() ? debounceCount_ : 0;
        revertCapsLock();
    }

};

// NextlessFactory — Nextless 插件的工厂类
class NextlessFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new NextlessAddon(manager->instance());
    }
};

FCITX_ADDON_FACTORY(NextlessFactory);
