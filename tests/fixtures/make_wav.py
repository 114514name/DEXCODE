#!/usr/bin/env python3
"""生成 tests/fixtures/*.wav —— WAV 解析的输入数据(只用标准库)。

用途:dexgame 音频测试与示例。刻意手写 RIFF 头,理由与 make_atlas.py 相同:
  - 项目零第三方依赖(不引入音频库)
  - 生成物是**输入数据**(不是构建产物),可入库,clone 后测试直接可跑
  - 顺带覆盖 dg_audio.c 里那个手写 WAV 解析器的各条分支(16/8 位、单/双声道)

生成:
  silence16m.wav  16 位单声道 8000Hz  0.1s  全静音(测试用:不会发出声音)
  silence8s.wav    8 位双声道 22050Hz 0.1s  全静音(测试用)
  beep.wav        16 位单声道 22050Hz 0.12s 440Hz 正弦(示例用,振幅只有 25%)

运行: python tests/fixtures/make_wav.py
"""
import math
import os
import struct


def wav_bytes(channels, rate, bits, frames):
    """frames: list[list[int]] 或 list of (left, right) —— 每个采样点一个 tuple。"""
    ba = channels * bits // 8
    data = bytearray()
    for fr in frames:
        for v in fr:
            if bits == 16:
                data += struct.pack("<h", int(v))
            else:                                   # 8 位 WAV 是无符号
                data += struct.pack("<B", int(v) & 0xFF)
    riff = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    fmt = b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                rate * ba, ba, bits)
    return riff + fmt + b"data" + struct.pack("<I", len(data)) + bytes(data)


def write(name, blob):
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    with open(out, "wb") as f:
        f.write(blob)
    print("生成", out, len(blob), "字节")


def main():
    # 16 位单声道 8000Hz 0.1 秒(全 0)
    n = 800
    write("silence16m.wav", wav_bytes(1, 8000, 16, [(0,)] * n))

    # 8 位双声道 22050Hz 0.1 秒(全 128 = 静音中点)
    n = 2205
    write("silence8s.wav", wav_bytes(2, 22050, 8, [(128, 128)] * n))

    # 440Hz 正弦,16 位单声道 22050Hz,0.12 秒,振幅 25%(示例里能听到但不吵)
    rate, dur = 22050, 0.12
    frames = []
    for i in range(int(rate * dur)):
        frames.append((int(0.25 * 32767 * math.sin(2 * math.pi * 440 * i / rate)),))
    write("beep.wav", wav_bytes(1, rate, 16, frames))


if __name__ == "__main__":
    main()
