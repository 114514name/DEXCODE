#!/usr/bin/env python3
"""DEXCODE 图片渲染原生库测试(libdeximg.dll)。

覆盖:定义文件解析、尺寸查询、渲染文本含 ANSI 真彩色转义与半块字符,
以及 C VM 与 pyvm 两条执行路径。

运行: python tests/test_img.py
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, DexError,
)

LIBS = os.path.join(ROOT, "libs")
NATIVE_DIR = os.path.join(LIBS, "img")
DLL_PATH = os.path.join(NATIVE_DIR, "libdeximg.dll")
BMP_PATH = os.path.join(NATIVE_DIR, "test_img.bmp")
REL_BMP = os.path.relpath(BMP_PATH, ROOT).replace(os.sep, "/")   # libs/img/test_img.bmp
TMP_SRC = os.path.join(ROOT, "_img_test.dex")

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def compile_ok(src):
    with open(TMP_SRC, "w", encoding="utf-8") as f:
        f.write(src)
    toks = Lexer(src, TMP_SRC).tokenize()
    ast = Parser(toks, TMP_SRC).parse_program()
    return compile_program(ast, source_path=TMP_SRC, include_dirs=[LIBS])


def test_def_and_compile():
    print("[def & compile]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 libdeximg.dll,先运行构建命令)")
        return
    unit = compile_ok('include "img";\nprint dex_img_width("x.bmp");\n')
    check("注册 3 个原生函数", len(unit.natives) == 3, len(unit.natives))
    names = [n.name for n in unit.natives]
    check("含 dex_render/width/height",
          "dex_render" in names and "dex_img_width" in names and "dex_img_height" in names)
    check("库指向 libdeximg.dll", unit.libs[0].path == DLL_PATH)
    # (string)->string 与 (string)->int 签名
    sigs = {n.name: n.sig for n in unit.natives}
    check("dex_render 签名 s:s", sigs.get("dex_render") == "s:s", sigs)
    check("dex_img_width 签名 s:i", sigs.get("dex_img_width") == "s:i", sigs)


def test_c_vm():
    print("[C VM]")
    if not os.path.exists(DLL_PATH) or not os.path.exists(BMP_PATH):
        print("  SKIP  (缺 DLL 或测试图)")
        return
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("  SKIP  (C VM 未构建)")
        return
    unit = compile_ok(f'include "img";\n'
                      f'print dex_img_width("{REL_BMP}");\n'
                      f'print dex_img_height("{REL_BMP}");\n'
                      f'print dex_render("{REL_BMP}");\n')
    bc = assemble(unit.to_program())
    tmp = os.path.join(ROOT, "_img_test.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    try:
        r = subprocess.run([vm, tmp], capture_output=True, timeout=30)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    lines = r.stdout.decode("utf-8", "replace").splitlines()
    check("宽度=40", len(lines) > 0 and lines[0] == "40", lines[:3])
    check("高度=20", len(lines) > 1 and lines[1] == "20", lines[:3])
    rest = "\n".join(lines[2:])
    check("渲染含真彩色转义", "\x1b[38;2;" in rest and "\x1b[48;2;" in rest)
    check("渲染含半块字符", "\u2580" in rest)   # ▀
    # 颜色应为渐变(不是单一颜色)
    colors = set()
    for seg in rest.split("\x1b["):
        if seg.startswith("38;2;"):
            colors.add(seg.split("m")[0])
    check("存在多种前景色(渐变)", len(colors) > 10, len(colors))


def test_pyvm():
    print("[pyvm]")
    if not os.path.exists(DLL_PATH) or not os.path.exists(BMP_PATH):
        print("  SKIP  (缺 DLL 或测试图)")
        return
    from dexlang.pyvm import run_program
    unit = compile_ok(f'include "img";\nprint dex_render("{REL_BMP}");\n')
    out, err = run_program(unit.to_program())
    check("pyvm 运行无错", err is None, repr(err))
    if out:
        text = out[0]
        check("pyvm 渲染含转义", "\x1b[38;2;" in text)
        check("pyvm 渲染含半块字符", "\u2580" in text)


def main():
    print("DEXCODE 图片渲染原生库测试")
    test_def_and_compile()
    test_c_vm()
    test_pyvm()
    if os.path.exists(TMP_SRC):
        os.remove(TMP_SRC)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
