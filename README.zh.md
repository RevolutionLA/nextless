# Nextless（中文）

[![CI](https://github.com/RevolutionLA/nextless/actions/workflows/ci.yml/badge.svg)](https://github.com/RevolutionLA/nextless/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/RevolutionLA/nextless)](https://github.com/RevolutionLA/nextless/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![en · zh](https://img.shields.io/badge/docs-en%20%C2%B7%20zh-blue)](README.md)

**fcitx5 的按住说话语音输入 —— 按住一个键、说话、松手，文字直接落在光标处。**

Nextless 是 Linux 桌面版的 Typeless 式听写工具（Wayland 与 X11 都可用），以 fcitx5 原生插件的
形式实现。它 fork 自 [xander-lin/vinput](https://github.com/xander-lin/vinput)，改造的核心只有一件事：
**触发键由你自己定**——包括右 Ctrl 这类纯修饰键——并且不改变键盘的任何其它行为。

English README: [README.md](README.md)

[后续计划](#后续计划) · [贡献指南](CONTRIBUTING.md) · [变更记录](CHANGELOG.md) ·
[安全说明](SECURITY.md) · [归属](#归属)

```
   按住右 Ctrl         说话          松手
  ─────────────────────────────────────────▶  文字出现在光标处
```

## 项目目标

**Nextless 要补上 Linux（优先 Ubuntu）桌面缺的那套「高级输入体验」——语音只是入口，不是全部。**

做完的标准是，一个 Linux 用户能：

1. **装得上**：一条命令安装，也能卸干净，不留残余；
2. **立刻能用**：模型、提示音、默认触发键都已就位，不需要手工下载文件；
3. **是「我自己的」**：触发键、识别后端、降噪、提示音全部可替换——可玩可改正是这个 fork 存在的理由；
4. **信得过**：每个提交都过 CI，失败路径有测试兜底，错误只出现在输入法面板里，绝不进你的文档。

今天的形态是**语音优先的 fcitx5 插件**；方向是把 Linux 上真正的高级输入套件缺的环节补齐：离线
识别的标点与润色、首次运行的引导、真正的发行包、以及和打字侧输入法（你本来就在用的中文输入法）
的舒适共存。**不做的**：再造一个输入法内核，或自营任何云服务。

## 为什么要做这个

Linux 上的语音转文字工具不少，但**不打断你工作的听写**很少：

| | Typeless / Wispr Flow | [openless](https://github.com/Open-Less/openless) | Electron 听写应用 | **Nextless** |
|---|---|---|---|---|
| 平台 | macOS / Windows | macOS / Windows | 跨平台 | **Linux（Wayland + X11）** |
| 触发键 | 可配置 | 可配置 | 全局快捷键，分不出左右 Ctrl | **任意键，含右 Ctrl / Alt / Super** |
| 任意应用可用 | 是 | 是 | 靠模拟打字 | **是——它本身就是输入法** |
| 离线识别 | 否 | 否 | 部分 | **是（sherpa-onnx）** |
| 常驻后台进程 | 是 | 是 | 是 | **否** |
| 响度归一 + 降噪 + VAD | 是 | 看实现 | 看实现 | **是** |

因为 Nextless 是 fcitx5 模块，文字通过浏览器、编辑器、终端本来就在用的输入法路径上屏：
不需要无障碍权限，不需要 `xdotool`，不走剪贴板，也不会抢焦点。

## 特性

- **触发键可配。** `fcitx5 配置工具 → 插件 → Nextless → Push-to-talk key`，或直接改
  `~/.config/fcitx5/conf/nextless.conf`。支持纯修饰键（左右 Ctrl / Alt / Shift / Super）和组合键。
  短按不触发，这个键照常打字；长按 300 ms（可调）开始录音。
- **仍然支持 CapsLock，但不再乱改大写状态。** 选 CapsLock 时，Nextless 通过 `/dev/uinput`
  把它补回去，锁定状态不会翻转；选其它键时**根本不创建**虚拟键盘。
- **五种识别后端，一个手势热切换。** 两个完全离线（Zipformer 中英混说、FireRed ASR2 int8，走
  [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)），两个云端（豆包流式 2.0、Qwen3-ASR），
  一个 mock 用于自测。`Shift+触发键` 后按 ←/→ 换后端，↑/↓ 换降噪。
- **完整的音频链路**，不是把麦克风原始数据直接丢给模型：PulseAudio 采集 → EBU R128 响度归一
  （−16 LUFS）→ 降噪（默认 speexdsp，装了 DeepFilterNet3 也能用）→ VAD 静音裁剪，并保留一小段尾音，最后一个字不会被切掉。
- **听得见的状态变化。** 起录、松手、换后端各有一声短提示，不用盯面板就知道键生效了。想换成自己的
  音色，把 `activate.wav` / `deactivate.wav` / `switch.wav` 放进 `~/.local/share/nextless/sounds/`
  即可；随包的三个音由 `tools/gen_sounds.py` 生成。
- **顺序可靠。** 上一句还在识别时就能开始下一句。每次结果绑定“开始录音那一刻”的窗口与输入上下文，
  并按录音顺序上屏（最多 3 个在途）。
- **听错的词可以自己修。** `~/.config/nextless/hotwords.json` 是一张错词纠正表：`{"LIBR": "礼拜二"}`
  把“模型听成的”改回“你本来说的”。**所有后端**（本地和云端）的最终文本上屏前都先过这张表，再过标点：
  同位置最长键优先、替换不连环（A→B、B→C 不会把 A 变成 C）、文件缺失/为空/格式坏就什么都不改。
  改完表下一句立即生效，不用重启 fcitx5。
- **报错不进正文。** 网络、服务、超时、麦克风故障都只显示在光标旁的输入法状态面板里。没说话就是
  no-op：什么都不上屏，也不报错。

## 已知限制（装之前先看）

- **本地标点是一个单独的可选模型。** 装上 ct-transformer 标点模型（`nextless-get-models
  --punctuation`，约 62 MB）后，Zipformer 和 FireRed 的结果会自动带上 ，。？——i7-1260P
  实测 44 字约 0.19 s，跑在识别线程上，不碰按键路径。模型缺失或处理失败就原样提交裸文本，
  装饰步骤不会吃掉一句话。豆包后端除了标点还有数字规整和它自己的服务端热词。
- **错词纠正表是逐字节匹配。** `hotwords.json` 是纯文本替换，不分词、不忽略大小写：
  `LIBR` 修得了，`libr` 就得另配一条；同一个词的几种听错写法要各写一行。
- **中英混说**在 Zipformer bilingual 上能用，方言和专有名词偏弱；FireRed 明显更准但更慢。
- **模型不随仓库分发**，需要自己下载（约 360 MB 或 1.2 GB，见下）。
- **还没有 `.deb`**，`fcitx5-nextless-git` 也还没进 AUR —— `PKGBUILD` 已提供，打包进度见
  [Roadmap](#后续计划)。

## 环境要求

- Linux，fcitx5 **≥ 5.1**，PulseAudio（PipeWire 的 Pulse 兼容层即可，已在 PipeWire 1.x 实测）
- 任意桌面：GNOME / KDE / Hyprland，Wayland 或 X11；一个能用的麦克风输入设备
- 默认模型覆盖中文、英文与中英混说

## 安装

### 1. 构建

```bash
# Debian / Ubuntu
sudo apt install -y g++ meson ninja-build git \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev fcitx5-modules-dev \
  libpulse-dev libebur128-dev libcurl4-openssl-dev libspeexdsp-dev libsoxr-dev

# 上面的包名在 Ubuntu 26.04 上用 apt-cache 逐个查过；Ubuntu 源里没有 libfcitx5-dev
# 这个元包。Debian 的包名看起来一样但未经实机验证，如果有出入欢迎提 issue。
# Fedora：dnf install gcc-c++ meson ninja-build pkgconf-pkg-config fcitx5-devel libpulse-devel \
#         libebur128-devel libcurl-devel speexdsp-devel soxr-devel   （CI 还没验过）

git clone https://github.com/RevolutionLA/nextless.git
cd nextless
meson setup build --prefix=/usr --buildtype=release
ninja -C build
sudo meson install -C build
```

> `meson.build` 里默认 `warning_level=2` + `werror=true`，CI 就按这个配置构建，所以任何新的
> `-Wall`/`-Wextra` 告警都会让构建失败。本地想临时放宽：`meson configure build -Dwerror=false`。
> `-Wpedantic` 是**故意不开**的：fcitx5 自己的 `FCITX_CONFIGURATION` / `FCITX_DECLARE_PRIVATE`
> 宏展开后会在类作用域多出一个 `;`，各发行版打包的 fcitx5 版本会因此报错（fcitx5 头文件也因此按
> 系统头引入）。这个坑不该由我们来填。

Arch 用户直接 `makepkg -si`（产出 `fcitx5-nextless-git`）。

### 2. 加载插件

清单装在 `/usr/share/fcitx5/addon/nextless.conf`，`OnDemand=False`，所以重启 fcitx5 就够了。
它是 module 不是 ime，**不用**加进输入法列表。

```bash
fcitx5 -r -d
```

### 3. 选你的触发键

`fcitx5 配置工具 → 插件 → Nextless → Push-to-talk key`，或者：

```bash
mkdir -p ~/.config/fcitx5/conf
cat > ~/.config/fcitx5/conf/nextless.conf <<'EOF'
[Hotkey]
0=Control_R

DefaultProvider=zipformer
EOF
fcitx5 -r -d
```

### 4. 下载模型（离线后端）

```bash
nextless-get-models
```

整步就这一条命令。这个助手（随插件一起安装；装之前想跑就是仓库里的
`tools/get_models.sh`）会检测 CPU 架构、下载前先把体积报给你、并让你在三选一里挑
Zipformer / FireRed / 都要（非交互用 `--backend=...`）。它只写 `~/.local/share/nextless/`
这一个目录；重复运行会跳过已装好的部分；解压出来的目录名和 `advanced.json` 期望的
一致才会落位，所以下载失败不会留下半个模型。`--dry-run` 只打印计划，不动任何东西；
`--punctuation` 会额外下载可选的标点模型（约 62 MB），装上后本地后端自动补 ，。？
——交互式运行会直接问你要不要装。

想自己动手的话，手工路径：

```bash
mkdir -p ~/.local/share/nextless/sherpa-onnx ~/.local/share/nextless/models
cd /tmp

# sherpa-onnx 运行时：bin/ 和 lib/ 都要拷，二进制用的是 rpath $ORIGIN/../lib
# aarch64 的资产名是 ...-linux-aarch64-shared-cpu.tar.bz2（去掉 -cpu 会 404）
V=1.13.8
curl -LO "https://github.com/k2-fsa/sherpa-onnx/releases/download/v${V}/sherpa-onnx-v${V}-linux-x64-shared.tar.bz2"
tar xf "sherpa-onnx-v${V}-linux-x64-shared.tar.bz2"
cp -r "sherpa-onnx-v${V}-linux-x64-shared/"{bin,lib} ~/.local/share/nextless/sherpa-onnx/

M=https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models
mkdir -p ~/.local/share/nextless/models

# Zipformer 中英混说（流式 transducer，fp32 约 360 MB）
curl -LO "$M/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2"
tar xf sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2 -C ~/.local/share/nextless/models/

# FireRed ASR2 int8（离线 AED，约 1.2 GB，更准更慢）
curl -LO "$M/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2"
tar xf sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2 -C ~/.local/share/nextless/models/

rm -f sherpa-onnx-*.tar.bz2
```

解压出来的目录名和代码里的默认路径完全一致，不需要改名。想放别处就改
`~/.config/nextless/advanced.json`。

听写时如果运行时或某个模型文件缺失，输入法面板会直接说缺的是哪一块、让你跑
`nextless-get-models`——不再是干巴巴的 `spawn failed` / `recognition failed`；它实际
找过的路径会写进 fcitx5 日志。

云端后端不需要模型，只要凭据：

```bash
mkdir -p ~/.config/nextless
echo '{"api_key":"<火山引擎 APP Key>","resource_id":"volc.seedasr.auc"}' > ~/.config/nextless/doubao.json
echo '{"api_key":"<sk-...>"}' > ~/.config/nextless/qwen.json
chmod 600 ~/.config/nextless/*.json
```

### 5. 可选：DeepFilterNet3 降噪

speexdsp 随构建一起装好，安静环境够用。环境吵就把 DeepFilterNet 的
`deep-filter` 命令行交给它 —— 刻意不做成构建依赖，因为它带着约 100 MB 模型和一个 Rust 二进制：

```bash
mkdir -p ~/.local/share/nextless/bin
# 发行版 tar 包、或者 pip install deepfilternet，任选
ln -s "$(command -v deep-filter)" ~/.local/share/nextless/bin/deep-filter
```

不放这个路径也行，用 `NEXTLESS_DEEP_FILTER=/usr/bin/deep-filter` 指过去（需要写进 fcitx5 的运行
环境）。开关是 `Shift+触发键` 后按 ↑/↓，选择会写进 `~/.config/nextless/audio.json`。

Nextless 会先重采样到 48 kHz 交给模型，再采回 16 kHz 送识别。因为降噪是外部进程，一律当成不可信：
二进制不存在、非零退出、卡住（5 秒后杀掉）或者原样把音频退回来，这一句都会退回 speexdsp 而不是
丢掉你说的话；连续失败三次，本次会话就不再尝试。这四种情况都有 `meson test` 覆盖。

## 使用

| 操作 | 按键 |
|---|---|
| 听写 | 按住**你设的触发键** → 说话 → 松手 |
| 换识别后端 | 按住 **Shift + 触发键**，再按 **← / →** |
| 换降噪 | 按住 **Shift + 触发键**，再按 **↑ / ↓** |

录音时光标旁的输入法面板显示 `listening → processing audio → recognizing → 文本`。
松手立即把控制权还给 fcitx5，识别在后台异步进行。

## 后端

| Provider | 类型 | 体积 | 模型 | 实测 |
|---|---|---|---|---|
| `zipformer` | 本地 | ~360 MB | `sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20` | i7-1260P 8 线程 RTF ≈ 0.13，约 1 GB RSS；装标点模型后自动带标点 |
| `fire_red` | 本地 | ~1.2 GB | `sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26` | 12 线程 RTF ≈ 0.38，峰值 RSS ~1.7 GB，方言和长句明显更好 |
| `doubao` | 云端 | — | 豆包流式识别 2.0 | 有标点、数字规整、服务端热词，需要 API key |
| `qwen` | 云端 | — | Qwen3-ASR-Flash | 需要 API key |
| `mock` | 测试 | — | — | 固定返回 `hello world`，用来验证按键和采集链路 |

`hotwords.json` 纠正表对**所有**后端生效——它改写的是最终文本，与是谁识别的无关。还没接的是
模型内部的偏置（sherpa-onnx 的 `--hotword` 解码钩子）；专名、同音词这类需求目前由纠正表覆盖。

两个本地后端刻意“每句起一个 `sherpa-onnx` 进程”：没有常驻模型服务、没有孤儿进程、没有需要维护的
预热状态。（仓库里的 `systemd/` 单元是实验性的常驻服务路线，**不是**必需的。）

## 配置文件

| 文件 | 作用 |
|---|---|
| `~/.config/fcitx5/conf/nextless.conf` | 触发键（`[Hotkey]`）、`DefaultProvider` |
| `~/.config/nextless/nextless.json` | `activation_msec`（默认 300）、通知超时、防抖 |
| `~/.config/nextless/audio.json` | `denoise`：`none` \| `speexdsp` \| `deepfilter` |
| `NEXTLESS_DEEP_FILTER`（环境变量） | 指向系统里的 `deep-filter`，优先于默认安装路径 |
| `~/.config/nextless/advanced.json` | 模型路径、`num_threads`、超时、LUFS 目标、VAD 阈值 |
| `~/.config/nextless/hotwords.json` | 错词纠正表 `{"听错的": "本意"}`，所有后端上屏前都会过它 |
| `~/.config/nextless/doubao.json` / `qwen.json` | 云端凭据 |
| `~/.config/nextless/pa_buffer.json` | 自动探测的 PulseAudio buffer（自生成） |
| `~/.local/share/nextless/sounds/` | 覆盖随包的 `activate` / `deactivate` / `switch` 提示音（可选） |

`meson install` 会把 `config/*.json.example`（去掉 `.example` 后缀）装进构建时的
`sysconfdir`/nextless——发行版构建是 `/etc/nextless`（PKGBUILD 显式传 `--sysconfdir=/etc`，
`prefix=/usr` 时 meson 本身也会解析到那里），普通源码安装是 `/usr/local/etc/nextless`。
运行时读取的就是同一个目录（编译期注入）：用户文件优先，缺失时首次读取会从打包目录拷一份，
不会覆盖已有文件。`hotwords.json` 随包的是占位符模板（`YOUR_` 开头的键会在解析时被丢弃），
拷到 `~/.config` 后不改写任何真实语音——想生效就自己把条目填进去。

## 开发

```bash
meson setup build --buildtype=debug
ninja -C build
meson test -C build            # 17 个单测：注册表、配置回落、采集、队列、静音、curl 取消、降噪回落、缺失组件提示、采集目录清扫、标点、热词、get_models 契约
```

跑测试需要一个 PulseAudio 服务（采集用例会真的开一条流）；无桌面的 runner 上
`pulseaudio --start` 加 `pactl load-module module-null-sink` 就够了，CI 就是这么做的。

- `docs/nextless/` —— 设计文档：交互模型、ASR provider 接口、失败场景分析。
- `docs/fcitx5/` —— fcitx5 插件与配置系统简介。Nextless 在 `PreInputMethod` 阶段挂钩按键事件，
  这部分值得先看。
- `FINDINGS.md` —— 所有调参数字背后的测量记录（buffer 大小、模型延迟、降噪对比）。保留它是因为
  它解释了默认值为什么是默认值。
- `tools/` —— 独立小工具：`record_test`、`calibrate_silence`、`tail_loss_test`、
  `uinput_key`（注入一次合成的“按住某键”，不用手按也能自测 push-to-talk）、
  `gen_sounds.py`（重新生成随包的提示音）。

诊断日志（每行一个 JSON，不含音频、不含识别文本、不含 key）默认关闭：

```bash
meson configure build -Ddiagnostic_logging=true && ninja -C build
# 输出在 ~/.local/share/nextless/diagnostic.log
```

## 归属

Nextless fork 自 **xander-lin 的 [vinput](https://github.com/xander-lin/vinput)**（MIT）。
音频链路、provider 抽象、多句排队与上屏顺序的设计都来自上游。fork 新增：

- `feat(hotkey)`：触发键从写死的 `CapsLock` 改为 fcitx5 `KeyListOption`，默认右 Ctrl；
  只有当选了锁定键时才启用 `/dev/uinput` 反弹
- 更名为 Nextless：插件名、库名、配置与数据路径（`~/.config/nextless`、`~/.local/share/nextless`）
- `fix(defaults)`：本地 ASR 线程数改为跟随 CPU（不再写死 30）；Zipformer 默认模型目录改成
  sherpa-onnx 实际发布的压缩包目录名

许可证：**MIT**，见 [LICENSE](LICENSE)（保留了上游版权声明）。

## 后续计划

在 [GitHub Issues](https://github.com/RevolutionLA/nextless/issues) 里跟踪的是具体待办，
下面是方向。

### 按「能不能装、能不能交接」排序（2026-10-05 审查）

每条都写了完成标准，谁接手都知道做到什么程度算完。

| | 事项 | 完成标准 |
|---|---|---|
| 1 | **CI 是红的**：debug 任务里的 `cloud_provider_queue` 失败。provider 先触发错误回调、再在作用域退出时删除临时 WAV，测试立刻 `exists()` 检查，撞上清理时序——release 只是侥幸跑赢。**已修复，[#10](https://github.com/RevolutionLA/nextless/pull/10)**——四个 provider 现在都在回调触发前删掉临时 WAV（CI debug 任务间歇性捕获；本地用 `taskset -c 0` 复现概率约 5%）。 | ✅ 两种构建都绿；修复比这条完成标准更严——「先回调、后清理」已不合法，断言保持严格而非放宽 |
| 2 | **`/etc/nextless` 的默认配置从未被安装**：配置读取逻辑会去读它、并在首次使用时复制到 `~/.config/nextless/`，但没有任何 install 规则把它装进去——所以源码安装实际上一直在用编译进代码的默认值。**已修复，[#11](https://github.com/RevolutionLA/nextless/pull/11)**——meson 现在把示例以 `*.json` 装进构建时的 `sysconfdir`/nextless，并把同一路径编译进 loader，安装与读取不可能再漂移；CI 会做 staged install，随包示例（自 `hotwords.json` 起共六份）少装一个就红。 | ✅ meson 确实会装——发行版构建是 `/etc/nextless`（`--sysconfdir=/etc`），普通源码安装是 `/usr/local/etc/nextless`，与 loader 读取的路径同源 |
| 3 | **没模型/没二进制时的首次运行**：现在只会显示 `Zipformer: spawn failed` / `recognition failed`，完全不提示要下载什么。**已修复，[#12](https://github.com/RevolutionLA/nextless/pull/12)**——provider 在 spawn 前预检运行时与每个模型文件，报出具体路径；面板显示「sherpa-onnx runtime missing / offline model missing」并指向 README（`test_missing_components` 用假 `$HOME` 覆盖全部四条失败路径）。 | ✅ 面板说清缺哪一块、并指到 README 的下载小节；实际找过的路径进 fcitx5 日志（也是下面向导的前置） |
| 4 | **真正的安装与卸载路径**：先 `.deb`（依赖、模型获取、干净卸载），再 AUR。 | 干净的 Ubuntu 虚拟机上装完能听写，`apt remove` 不留残余，回滚有文档 |
| 5 | **DeepFilterNet 从没真跑过**：测试全用桩二进制驱动，真实 `deep-filter` 单句耗时从未测过。 | 用真实二进制跑一句 10 秒音频，记录墙钟与 RTF——否则就把它下架 |
| 6 | **与打字侧输入法共存**：写清支持的组合（wetype、fcitx5 拼音）和抢键规则。 | 一份短文档，外加每个组合一次冒烟验证 |

方向清单：

- [x] 本地标点：接 `sherpa-onnx-offline-punctuation`（ct-transformer）
- [x] 热词 / 自定义词组：`~/.config/nextless/hotwords.json` 错词纠正表（所有后端）；模型内部偏置仍未接
- [x] 首次运行向导，自动选对 sherpa-onnx 构建（x86_64 / aarch64）—— `nextless-get-models`
- [ ] `.deb` 打包（以及 AUR 上的 `fcitx5-nextless`）
- [ ] A/B 基准脚手架，公开每个模型的 CER / 延迟 / RTF / RSS
- [ ] 声音克隆 / 个人纠错学习 —— 还没定，先去看讨论区
- [x] CI：GCC 下 build × test 矩阵 + `-Dwerror=true`，另加一个开诊断日志的构建
- [x] 中文文档（就是这一份）
- [x] 静音是 no-op：不再报 `ASR error: empty result`，FireRed 的 `<sil>` 也不会进文档
- [x] 提示音已随包附带（`tools/gen_sounds.py` 可重新生成）
- [x] DeepFilterNet3 作为可选降噪：外部进程有超时上限，失败一律退回 speexdsp
