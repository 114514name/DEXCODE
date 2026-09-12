#!/usr/bin/env python3
"""DEXCODE 静态链接(静态库内嵌)功能测试。

覆盖:定义文件 static 解析、编译器读取 DLL 字节内嵌、字节码 v3 往返、
以及(若 DLL/VM 已构建)C VM 与 pyvm 在"外部 DLL 被移走"的情况下仍能运行。

运行: python tests/test_static.py
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, disassemble, render,
    parse_asm_text, parse_def, DexError,
)

LIBS = os.path.join(ROOT, "libs")
NATIVE_DIR = os.path.join(LIBS, "math")
DEF_STATIC = os.path.join(NATIVE_DIR, "math_static.dexdef")
DLL_PATH = os.path.join(NATIVE_DIR, "libdexmath.dll")
TMP_SRC = os.path.join(ROOT, "_static_test.dex")

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


# ---------- 定义文件 static 解析 ----------
def test_def_parser():
    print("[def parser static]")
    def_file = parse_def('refer static "libx.dll";\nextern func f(a: int) -> int;\n', "<t>")
    check("解析 refer static", def_file.is_static is True)
    check("路径保留", def_file.lib_path == "libx.dll")

    def_file2 = parse_def('refer "libx.dll";\nextern func f(a: int) -> int;\n', "<t>")
    check("普通 refer 非静态", def_file2.is_static is False)


# ---------- 编译期静态库 ----------
def test_compile_static():
    print("[compile static]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 libdexmath.dll,先运行构建脚本)")
        return
    unit = compile_ok('include "math_static";\nprint dex_add(2, 3);\n')
    check("注册了 1 个库", len(unit.libs) == 1)
    lib = unit.libs[0]
    check("is_static 为 True", lib.is_static is True)
    check("内嵌了 DLL 字节", len(lib.data) > 1000, len(lib.data))
    check("路径仍指向 DLL", lib.path == DLL_PATH)
    check("原生函数可用", "dex_add" in [n.name for n in unit.natives])


# ---------- 静态库字节码往返 ----------
def test_roundtrip():
    print("[static roundtrip]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 DLL)")
        return
    unit = compile_ok('include "math_static";\nprint dex_add(2, 3);\nprint dex_fact(5);\n')
    prog = unit.to_program()
    bc = assemble(prog)

    # 字节码 v3
    check("版本为 3", bc[4] == 3, bc[4])

    asm = render(prog)
    check("汇编含 static 标记", "static " in asm)
    text = disassemble(bc)
    prog2 = parse_asm_text(text)
    check("静态库往返字节码一致", assemble(prog2) == bc)
    check("往返后 is_static 保留", prog2.libs[0].is_static is True)
    check("往返后数据一致", prog2.libs[0].data == prog.libs[0].data)


# ---------- C VM:外部 DLL 被移走后仍能运行 ----------
def test_vm_without_dll():
    print("[C VM static,no external DLL]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 DLL)")
        return
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("  SKIP  (C VM 未构建)")
        return
    unit = compile_ok('include "math_static";\nprint dex_add(2, 3);\n'
                      'print dex_fact(5);\nprint dex_sqrt(9.0);\nprint dex_hello();\n')
    bc = assemble(unit.to_program())
    tmp = os.path.join(ROOT, "_static_test.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)

    # 临时移走外部 DLL,证明运行不需要它
    hidden = DLL_PATH + ".hidden"
    moved = False
    if os.path.exists(hidden):
        os.remove(hidden)
    if os.path.exists(DLL_PATH):
        os.rename(DLL_PATH, hidden)
        moved = True
    try:
        try:
            r = subprocess.run([vm, tmp], capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=30)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
    finally:
        if moved:
            os.rename(hidden, DLL_PATH)

    check("静态库无需外部 DLL", r.returncode == 0 and r.stdout.splitlines() == [
        "5", "120", "3", "hello from dll"], repr(r.stdout) + repr(r.stderr))

    # 临时文件应已清理
    leftovers = [n for n in os.listdir(os.environ.get("TEMP", os.environ.get("TMP", "/tmp")))
                 if n.startswith("dexvm_") and n.endswith(".dll")]
    check("临时文件已清理", len(leftovers) == 0, leftovers[:5])


# ---------- pyvm:静态库运行 ----------
def test_pyvm_static():
    print("[pyvm static]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 DLL)")
        return
    unit = compile_ok('include "math_static";\nprint dex_add(2, 3);\nprint dex_fact(5);\n')
    from dexlang.pyvm import run_program
    out, err = run_program(unit.to_program())
    check("pyvm 静态库运行", err is None and out == ["5", "120"], repr(out) + repr(err))


# ---------- dexgame:静态内嵌(M4-d)----------
DG_DLL = os.path.join(LIBS, "dexgame", "libdexgame.dll")


def test_dexgame_static():
    """游戏引擎的静态变体:把 400KB+ 的 libdexgame.dll 内嵌进字节码,
    外部 DLL 改名后仍能跑(并且离屏像素断言仍然成立)。"""
    print("[dexgame static]")
    if not os.path.exists(DG_DLL):
        print("  SKIP  (未构建 libdexgame.dll)")
        return
    unit = compile_ok('''include "dexgame_static";
eng_init_offscreen(32, 32);
eng_set_clear_color(0xFF000000);
eng_frame_begin();
eng_rect(4.0, 4.0, 8.0, 8.0, 0xFFFF0000);
eng_frame_end();
print eng_pixel(8, 8);
print eng_version();
eng_shutdown();
''')
    check("注册了 1 个库", len(unit.libs) == 1, len(unit.libs))
    lib = unit.libs[0]
    check("dexgame 静态标记", lib.is_static is True)
    dll_size = os.path.getsize(DG_DLL)
    check("**内嵌了整个引擎 DLL 字节**",
          len(lib.data) == dll_size, (len(lib.data), dll_size))
    check("eng_* 原生函数可用",
          "eng_init_offscreen" in [n.name for n in unit.natives])

    bc = assemble(unit.to_program())
    check("字节码体积包含内嵌 DLL", len(bc) > dll_size, len(bc))

    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("  SKIP  (未构建 vm.exe)")
        return
    tmp = os.path.join(ROOT, "_static_dg.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    hidden = DG_DLL + ".hidden"
    moved = False
    if os.path.exists(hidden):
        os.remove(hidden)
    if os.path.exists(DG_DLL):
        os.rename(DG_DLL, hidden)
        moved = True
    try:
        try:
            r = subprocess.run([vm, tmp], capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=60)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
    finally:
        if moved:
            os.rename(hidden, DG_DLL)
    lines = [l for l in r.stdout.splitlines() if l]
    check("**外部 DLL 移走后引擎仍能跑**",
          r.returncode == 0 and len(lines) >= 2, repr(r.stdout) + repr(r.stderr))
    if lines:
        check("离屏渲染像素断言仍成立(红)",
              lines[0] == str(0xFFFF0000), lines[:2])
    if len(lines) > 1:
        check("引擎版本号可读", lines[1] == "2", lines[:2])


def main():
    print("DEXCODE 静态链接测试")
    test_def_parser()
    test_compile_static()
    test_roundtrip()
    test_vm_without_dll()
    test_pyvm_static()
    test_dexgame_static()
    if os.path.exists(TMP_SRC):
        os.remove(TMP_SRC)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
