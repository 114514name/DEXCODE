#!/usr/bin/env python3
"""生成 tests/fixtures/atlas2x2.png —— 2x2 的四色图集。

用途:dexgame 的图集子矩形采样测试(test_dexgame.py)。四个像素:
    (0,0) 红  (1,0) 绿
    (0,1) 蓝  (1,1) 白

刻意只用标准库(zlib + struct)手写 PNG,理由:
  - 项目零第三方依赖(不用 Pillow)
  - 生成物是**输入数据**(不是构建产物),可入库,这样 clone 后测试直接可跑
  - 顺带覆盖 stb_image 的解码路径(而不是绕过它直接喂内存)

运行: python tests/fixtures/make_atlas.py
"""
import os
import struct
import zlib

W, H = 2, 2
# 每行:(R,G,B,A)
ROWS = [
    [(255, 0, 0, 255), (0, 255, 0, 255)],      # y=0: 红 绿
    [(0, 0, 255, 255), (255, 255, 255, 255)],  # y=1: 蓝 白
]


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def main():
    raw = bytearray()
    for y in range(H):
        raw.append(0)                       # 每行的 filter 字节:0 = None
        for x in range(W):
            raw.extend(ROWS[y][x])

    ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)   # 8 位、RGBA
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "atlas2x2.png")
    with open(out, "wb") as f:
        f.write(png)
    print("生成", out, len(png), "字节 (%dx%d RGBA)" % (W, H))


if __name__ == "__main__":
    main()
