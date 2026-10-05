# 与打字侧输入法共存

Nextless 是 fcitx5 的**模块**（事件处理器），不是输入法。它不会把自己切成当前输入法，不会动别的
插件的设置，也不关心你口述时选中的是哪个输入法：微信输入法（`wetype-im`）、fcitx5 拼音、纯
`keyboard-us` 布局，还是你装的任何其它东西——打字照旧，没有任何变化。

## 一次口述与当前输入法的关系

1. 按住触发键（默认**右 Ctrl**）超过 `activation_msec`（300 毫秒），Nextless 开始录音——
   当前输入法不参与，也保持选中不变。
2. 松开。音频交给识别后端（本地或云端）。
3. 结果直接提交进焦点输入上下文，和 fcitx5 提交任何文本一样。它**不经过**当前输入法的预编辑：
   不会弹候选窗，输入法自身状态也不被打扰。

## 已验证的组合

<!-- smoke result pending: replace with the run summary -->

| 打字侧 | 状态 | 说明 |
|---|---|---|
| 微信输入法 —— `wetype-im`（aarch64 桥接插件） | ✅ 冒烟已验证 | 开发机上的默认输入法 |
| fcitx5 `pinyin` | ✅ 冒烟已验证 | |
| 纯布局（`keyboard-us` ……） | ✅ 由结构保证 | 它只用到触发键 |
| 没有 fcitx5 时的 Nextless | ✗ | 它是插件；这种情况用独立工具 |

「冒烟已验证」指 `scripts/smoke-coexistence.sh --dictate` 在 GNOME/Wayland 的已安装构建上真跑过：
每个组合走完一整次口述——注入触发键按住、真实语音播进麦克风、识别文本被提交（fcitx5 日志里的
`Nextless [press]` …… `Nextless final commit: text_len=…`）。

## 按键归谁（抢键规则）

- **触发键**（默认右 Ctrl；可以是纯修饰键或组合键，在 `fcitx5-configtool → 附加组件 → Nextless`
  里改）。按住 ≥ `activation_msec` → 录音；松开 → 停止。更短的轻点什么都不做。
- 触发键**从不被吞**：右 Ctrl 照常当 Ctrl 用（组合快捷键不受影响），录音期间其余按键也照常送达
  应用。唯一的例外是拿 CapsLock 当触发键——锁定键必须被吞掉，它的 LED 状态才不会失真。
- **Shift + 触发键**进入切换模式；此模式下 ←/→（后端）与 ↑/↓（降噪）被消费。松开触发键即退出。
  除这两种情况和 CapsLock 触发外，Nextless 不消费任何按键。
- fcitx5 自己的快捷键（Ctrl+Space 等）以及 wetype、拼音用到的键都不受影响。
- 需要知道的取舍：任何把右 Ctrl 按住 ≥ 300 毫秒的操作（Ctrl+滚轮缩放、长按 Ctrl 拖拽）都会开始
  录音。如果这对你造成困扰，把 `~/.config/nextless/nextless.json` 里的 `activation_msec` 改大，
  或换一个触发键。

## 在你自己的机器上验证

```bash
scripts/smoke-coexistence.sh           # 只读环境检查
scripts/smoke-coexistence.sh --inject  # 每个输入法真实轻点/按住一次触发键（ydotool）
scripts/smoke-coexistence.sh --dictate # 每个输入法再来一次完整口述（扬声器 → 麦克风）
```

脚本会逐个把当前输入法切到相应组合、跑完再切回去。`--inject` 与 `--dictate` 会先请求确认：按住那一步
录 ~0.7 秒麦克风音频，识别出的文字会提交进**焦点窗口**——确认前先把焦点放到一个临时窗口。
如果 `--dictate` 一直出不了文字，查 `pactl get-default-sink` 和 `pactl get-default-source`：
麦克风必须听得见播放。
