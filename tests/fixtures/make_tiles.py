#!/usr/bin/env python3
"""生成 tests/fixtures/tiles.png —— 平台跳跃示例用的 7 格图集(116x16,每格 16x16)。

格序(与 atlas_cols=7 配合,瓦片 id / 精灵 sx 都按它算):
    0 地面(土砖)   1 平台(石)   2 墙(深板岩)
    3 砖块          4 玩家        5 金币        6 敌人

为什么手写 PNG:与 make_atlas.py / make_wav.py 同样的理由 —— 零第三方依赖,
生成物是**输入数据**(可入库),clone 后示例直接有资源可跑,而且顺带覆盖 stb_image。

运行: python tests/fixtures/make_tiles.py
"""
import os
import struct
import zlib

W, H, T = 116, 16, 16      # 116 = 7 格 × 16 + 4 空隙(空隙是透明的,防止采样越界)

# 颜色(R,G,B,A)
GROUND, GROUND_HI, GROUND_LO = (139, 90, 43, 255), (176, 122, 66, 255), (92, 56, 22, 255)
STONE, STONE_HI, STONE_LO = (110, 123, 139, 255), (146, 160, 176, 255), (70, 80, 94, 255)
SLATE, SLATE_HI, SLATE_LO = (62, 70, 84, 255), (92, 102, 118, 255), (38, 44, 56, 255)
BRICK, BRICK_HI, BRICK_LO = (166, 74, 58, 255), (200, 104, 84, 255), (112, 46, 36, 255)
SKIN = (88, 200, 90, 255)
SKIN_D = (44, 120, 48, 255)
EYE = (24, 28, 36, 255)
COIN, COIN_HI = (255, 210, 74, 255), (255, 240, 170, 255)
FOE, FOE_HI = (192, 80, 77, 255), (222, 118, 114, 255)


class Sheet:
    def __init__(self):
        self.px = [[(0, 0, 0, 0)] * W for _ in range(H)]

    def put(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.put(x, y, c)

    def block(self, tile, base, hi, lo):
        """砖块:主体 + 顶部高光 + 底部阴影 + 右侧描边(看起来有体积)"""
        x = tile * T
        self.rect(x, 0, x + T - 1, T - 1, base)
        self.rect(x, 0, x + T - 1, 1, hi)
        self.rect(x, T - 2, x + T - 1, T - 1, lo)
        self.rect(x + T - 2, 0, x + T - 1, T - 1, lo)
        self.rect(x, 0, x, T - 1, hi)


def main():
    s = Sheet()
    s.block(0, GROUND, GROUND_HI, GROUND_LO)
    s.block(1, STONE, STONE_HI, STONE_LO)
    s.block(2, SLATE, SLATE_HI, SLATE_LO)
    s.block(3, BRICK, BRICK_HI, BRICK_LO)

    # 4 玩家:圆角身体 + 眼睛
    x = 4 * T
    s.rect(x + 3, 1, x + 12, 14, SKIN)
    s.rect(x + 2, 3, x + 2, 12, SKIN)
    s.rect(x + 13, 3, x + 13, 12, SKIN)
    s.rect(x + 3, 0, x + 12, 0, SKIN_D)
    s.rect(x + 3, 15, x + 12, 15, SKIN_D)
    s.rect(x + 5, 5, x + 6, 7, EYE)
    s.rect(x + 10, 5, x + 11, 7, EYE)

    # 5 金币:圆
    x = 5 * T
    cx, cy, r = x + 7.5, 7.5, 6.0
    for yy in range(T):
        for xx in range(T):
            d = ((xx - cx) ** 2 + (yy - cy) ** 2) ** 0.5
            if d <= r:
                s.put(x + xx, yy, COIN_HI if d <= r - 2 else COIN)

    # 6 敌人:带锯齿的团子 + 眼睛
    x = 6 * T
    s.rect(x + 2, 2, x + 13, 14, FOE)
    s.rect(x + 2, 2, x + 13, 3, FOE_HI)
    for i in range(4):                      # 底部锯齿
        s.rect(x + 2 + i * 3, 13, x + 3 + i * 3, 15, (0, 0, 0, 0))
    s.rect(x + 5, 6, x + 6, 8, EYE)
    s.rect(x + 9, 6, x + 10, 8, EYE)

    raw = bytearray()
    for y in range(H):
        raw.append(0)                       # filter: None
        for x2 in range(W):
            raw.extend(s.px[y][x2])

    ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tiles.png")
    with open(out, "wb") as f:
        f.write(png)
    print("生成", out, len(png), "字节 (%dx%d RGBA, 7 格)" % (W, H))


if __name__ == "__main__":
    main()
