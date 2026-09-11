#!/usr/bin/env python3
"""DEXCODE 标准库测试(libdexstd.dll,纯 C 实现)。

覆盖:定义文件解析、30 个原生函数注册、C VM 运行(动态/静态)、
pyvm 运行,以及"完全脱离 Python + 无外部 DLL"运行(静态内嵌 +
直接用 vm.exe 执行 .dexbc)。

运行: python tests/test_stdlib.py
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
STDLIB_DIR = os.path.join(LIBS, "std")
STD_EXAMPLES = os.path.join(ROOT, "examples", "std")
DLL_PATH = os.path.join(STDLIB_DIR, "libdexstd.dll")
TMP_SRC = os.path.join(ROOT, "_std_test.dex")

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


def compile_src(src, inc="std"):
    with open(TMP_SRC, "w", encoding="utf-8") as f:
        f.write(src)
    toks = Lexer(src, TMP_SRC).tokenize()
    ast = Parser(toks, TMP_SRC).parse_program()
    return compile_program(ast, source_path=TMP_SRC,
                           include_dirs=[LIBS])


def run_c_vm(prog):
    bc = assemble(prog)
    tmp = os.path.join(ROOT, "_std_test.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    try:
        r = subprocess.run([vm, tmp], capture_output=True, timeout=60)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return r


def test_registration():
    print("[registration]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 libdexstd.dll)")
        return
    unit = compile_src('include "std";\nprint dex_abs(1);\n')
    check("注册 33 个原生函数", len(unit.natives) == 33, len(unit.natives))
    names = {n.name for n in unit.natives}
    for need in ("dex_abs", "dex_pow", "dex_str_concat", "dex_str_sub",
                 "dex_write_file", "dex_read_file", "dex_input",
                 "dex_now_ms", "dex_rand", "dex_system",
                 "dex_itoa", "dex_ftoa", "dex_out"):
        check(f"含 {need}", need in names)


def test_dynamic():
    print("[C VM dynamic]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (缺 DLL)")
        return
    src = open(os.path.join(STD_EXAMPLES, "demo_std.dex"), encoding="utf-8").read()
    unit = compile_src(src)
    r = run_c_vm(unit.to_program())
    out = r.stdout.decode("utf-8", "replace")
    lines = [l for l in out.splitlines() if l.strip() != ""]
    check("退出码 0", r.returncode == 0, repr(r.stderr))
    check("数学 abs=5", "5" in lines)
    check("幂 2^10=1024", "1024" in lines)
    check("clamp=5", lines.count("5") >= 2)
    check("大写 HELLO", "HELLO" in lines)
    check("拼接 foobar", "foobar" in lines)
    check("子串 ell", "ell" in lines)
    check("UTF-8 长度 6", "6" in lines)
    check("文件存在标记", lines.count("1") >= 2)
    check("done 在末尾", lines and lines[-1] == "done", lines[-3:])
    try:
        os.remove(os.path.join(ROOT, "_demo_std.txt"))
    except OSError:
        pass


def test_pyvm():
    print("[pyvm]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (缺 DLL)")
        return
    from dexlang.pyvm import run_program
    src = open(os.path.join(STD_EXAMPLES, "demo_std.dex"), encoding="utf-8").read()
    unit = compile_src(src)
    out, err = run_program(unit.to_program())
    check("pyvm 运行无错", err is None, repr(err))
    text = "\n".join(out)
    check("pyvm 数学", "1024" in text and "HELLO" in text and "foobar" in text)
    try:
        os.remove(os.path.join(ROOT, "_demo_std.txt"))
    except OSError:
        pass


def test_static_no_python():
    """静态内嵌标准库:移走 DLL,直接用 vm.exe 运行(完全脱离 Python)。"""
    print("[static,no python,no dll]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (缺 DLL)")
        return
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("  SKIP  (C VM 未构建)")
        return
    src = open(os.path.join(STD_EXAMPLES, "demo_std_static.dex"), encoding="utf-8").read()
    unit = compile_src(src, inc="std_static")
    check("静态库标记", unit.libs[0].is_static is True)
    check("内嵌 DLL 字节", len(unit.libs[0].data) > 100000, len(unit.libs[0].data))
    bc = assemble(unit.to_program())
    tmp = os.path.join(ROOT, "_std_static.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)

    hidden = DLL_PATH + ".hidden"
    moved = False
    if os.path.exists(hidden):
        os.remove(hidden)
    if os.path.exists(DLL_PATH):
        os.rename(DLL_PATH, hidden)
        moved = True
    try:
        r = subprocess.run([vm, tmp], capture_output=True, timeout=60)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
        if moved:
            os.rename(hidden, DLL_PATH)
        for p in ("_static_demo.txt", "_demo_std.txt"):
            try:
                os.remove(os.path.join(ROOT, p))
            except OSError:
                pass

    out = r.stdout.decode("utf-8", "replace")
    lines = [l for l in out.splitlines() if l.strip() != ""]
    check("无外部 DLL 运行", r.returncode == 0, repr(r.stderr))
    check("静态:abs=42", "42" in lines)
    check("静态:pow=81", "81" in lines)
    check("静态:repeat", "gogogo" in lines)
    check("静态:done 在末尾", lines and lines[-1] == "done", lines[-3:])


def test_dex_out():
    """dex_out:输出不自动换行;字符串里的 \n 手动换行。"""
    print("[dex_out]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (缺 DLL)")
        return
    src = ('include "std";\n'
           'dex_out("ab");\n'      # 无换行
           'dex_out("cd\\n");\n'   # 手动换行
           'dex_out("ef");\n'       # 无换行
           'print "g";\n')           # print 自动换行
    unit = compile_src(src)
    r = run_c_vm(unit.to_program())
    out = r.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    check("dex_out 不自动换行 + \\n 换行", r.returncode == 0 and out == "abcd\nefg\n",
          repr(out) + repr(r.stderr))
    # 汇编应含 dex_out 调用(表达式语句 + NCALL)
    from dexlang import render
    check("汇编含 NCALL dex_out", "NCALL dex_out" in render(unit.to_program()))
    # pyvm:dex_out 走 ctypes 写真实 stdout,run_program 不捕获输出,但不应报错
    from dexlang.pyvm import run_program
    out_l, err_l = run_program(unit.to_program())
    check("pyvm dex_out 可调用", err_l is None, repr(err_l))


def main():
    print("DEXCODE 标准库测试(纯 C 实现,零 Python)")
    test_registration()
    test_dynamic()
    test_dex_out()
    test_pyvm()
    test_static_no_python()
    if os.path.exists(TMP_SRC):
        os.remove(TMP_SRC)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
