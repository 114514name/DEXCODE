#!/usr/bin/env python3
"""生成一张测试用的 24 位 BMP(40x20 渐变图),供 libdeximg 渲染。

运行: python samples/native/make_test_bmp.py
输出: samples/native/test_img.bmp
"""
import os
import struct

W, H = 40, 20


def make(path):
    stride = (W * 3 + 3) & ~3
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            r = int(255 * x / (W - 1))
            g = int(255 * y / (H - 1))
            b = int(255 * (1 - x / (W - 1)) * (1 - y / (H - 1)))
            row += bytes((b, g, r))  # BGR
        row += b"\x00" * (stride - W * 3)
        rows.append(bytes(row))
    pixels = b"".join(reversed(rows))  # bottom-up
    data_off = 54
    file_size = data_off + len(pixels)
    with open(path, "wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<IHHI", file_size, 0, 0, data_off))
        f.write(struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(pixels), 2835, 2835, 0, 0))
        f.write(pixels)
    print(f"已生成 {path} ({W}x{H} BMP, {file_size} 字节)")


if __name__ == "__main__":
    make(os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_img.bmp"))
