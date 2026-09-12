#!/usr/bin/env python3
"""把 dexstudio/web/ 的前端资源**生成成 C 数组**(B7 的"单 exe"基础)。

为什么要内嵌:发布出去的 DexStudio 应该是"双击就能用"的 —— 不该要求用户旁边
摆一个 `web/` 目录。开发期仍然优先用磁盘上的 `web/`(`--web` 或 exe 同目录),
只有找不到时才用内嵌的那份解包到缓存目录。

生成物:`dexstudio/host/ds_embed.c`(由 `python main.py build-dexstudio` 调用)。

用法:python tools/embed_web.py [web 目录] [输出 .c]
"""
import hashlib
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "dexstudio", "web")
OUT = os.path.join(ROOT, "dexstudio", "host", "ds_embed.c")


def c_bytes(data):
    """把一段字节写成 C 字符串字面量的若干行(可读性优先,顺便避免单行过长)。"""
    parts = []
    line = []
    for i, b in enumerate(data):
        c = chr(b)
        if c == '"':
            line.append('\\"')
        elif c == '\\':
            line.append('\\\\')
        elif c == '\n':
            line.append('\\n')
        elif c == '\r':
            line.append('\\r')
        elif c == '\t':
            line.append('\\t')
        elif 32 <= b < 127:
            line.append(c)
        else:
            line.append('\\%03o' % b)
        if len(line) >= 16:
            parts.append('"' + ''.join(line) + '"')
            line = []
    if line:
        parts.append('"' + ''.join(line) + '"')
    # C 里相邻字符串字面量会自动拼接:每 8 段换一行,便于阅读
    out = []
    for i in range(0, len(parts), 8):
        out.append('    ' + '\n    '.join(parts[i:i + 8]))
    return '\n'.join(out) if out else '    ""'


def main():
    web = sys.argv[1] if len(sys.argv) > 1 else WEB
    out_path = sys.argv[2] if len(sys.argv) > 2 else OUT
    names = []
    for name in sorted(os.listdir(web)):
        p = os.path.join(web, name)
        if os.path.isfile(p):
            names.append(name)
    if not names:
        print(f"embed_web: {web} 里没有文件", file=sys.stderr)
        return 1

    h = hashlib.sha256()
    blobs = []
    for name in names:
        with open(os.path.join(web, name), "rb") as f:
            data = f.read()
        h.update(name.encode("utf-8"))
        h.update(b"\0")
        h.update(data)
        blobs.append((name, data))
    digest = h.hexdigest()[:16]

    with io.open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 由 tools/embed_web.py 生成 —— 不要手改(改前端资源后重新构建)。\n"
                " * 内容哈希 %s;共 %d 个文件。 */\n" % (digest, len(blobs)))
        f.write('#include "ds_embed.h"\n\n')
        for i, (name, data) in enumerate(blobs):
            f.write("static const unsigned char asset_%d[] = {\n" % i)
            f.write(c_bytes(data))
            f.write("\n};\n\n")
        f.write("static const DsAsset g_assets[] = {\n")
        for i, (name, data) in enumerate(blobs):
            f.write('    {"%s", asset_%d, %d},\n' % (name, i, len(data)))
        f.write("};\n\n")
        f.write('const char *ds_embed_hash(void) { return "%s"; }\n' % digest)
        f.write("int ds_embed_count(void) { return %d; }\n" % len(blobs))
        f.write("const DsAsset *ds_embed_at(int i) {\n"
                "    if (i < 0 || i >= (int)(sizeof g_assets / sizeof g_assets[0]))"
                " return 0;\n"
                "    return &g_assets[i];\n"
                "}\n")
    print("embed_web: %d 个文件 → %s (哈希 %s)" % (len(blobs), out_path, digest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
