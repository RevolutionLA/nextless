#!/usr/bin/env python3
"""生成 Nextless 的三个提示音 (48 kHz / 16-bit / 单声道 PCM WAV)。

adapter 只负责播 ~/.local/share/nextless/sounds/<name>.wav 和随包目录里的同名文件,
音色全部在这里定义; 改完音色重新生成并提交结果:

    python3 tools/gen_sounds.py

提示音必须够短(整体 <250ms)、够轻(-14 dBFS 左右), 因为它是按住右 Ctrl 时在
用户耳边重复出现的; 每个音符都带淡入淡出, 否则起音处的阶跃会变成"哒"的一声爆点。
"""

import argparse
import math
import os
import struct
import wave

RATE = 48000
FADE_MS = 4.0


def note(freq, ms, amp):
    """一个正弦音符, 两端各 FADE_MS 毫秒线性淡入淡出。"""
    n = int(RATE * ms / 1000.0)
    fade = max(1, int(RATE * FADE_MS / 1000.0))
    out = []
    for i in range(n):
        env = min(1.0, i / fade, (n - 1 - i) / fade)
        out.append(amp * env * math.sin(2.0 * math.pi * freq * i / RATE))
    return out


def silence(ms):
    return [0.0] * int(RATE * ms / 1000.0)


def write(path, samples):
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(struct.pack("<%dh" % len(samples),
                                  *[int(round(s * 32767)) for s in samples]))
    print("%-16s %5.0f ms  %6.1f kB" % (
        os.path.basename(path), len(samples) * 1000.0 / RATE,
        os.path.getsize(path) / 1024.0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--outdir", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "sounds"))
    args = parser.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    full = 10 ** (-14.0 / 20.0)   # -14 dBFS
    quiet = 10 ** (-18.0 / 20.0)  # 切换音更轻: 一次按键可能连按好几下

    # 起录: 上扬的两个音, "我在听了"
    write(os.path.join(args.outdir, "activate.wav"),
          note(659.25, 55, full) + silence(8) + note(880.0, 70, full))

    # 松手: 下沉的两个音(代码里就叫"结束音: 低音"), "交给我处理"
    write(os.path.join(args.outdir, "deactivate.wav"),
          note(523.25, 60, full) + silence(8) + note(392.0, 90, full))

    # 切换后端/降噪: 单声轻响
    write(os.path.join(args.outdir, "switch.wav"),
          note(783.99, 40, quiet))


if __name__ == "__main__":
    main()
