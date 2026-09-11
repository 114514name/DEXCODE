#!/usr/bin/env python3
"""DEXCODE include/refer 原生库功能测试。

覆盖:定义文件解析、include/refer 解析、编译期错误检查、原生调用往返、
以及(若 DLL 已构建)C VM 对 DLL 函数的实际调用。

运行: python tests/test_native.py
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, disassemble, render,
    parse_asm_text, parse_def, parse_sig, DexError,
)

LIBS = os.path.join(ROOT, "libs")
NATIVE_DIR = os.path.join(LIBS, "math")
DEF_PATH = os.path.join(NATIVE_DIR, "math.dexdef")
DLL_PATH = os.path.join(NATIVE_DIR, "libdexmath.dll")
TMP_SRC = os.path.join(ROOT, "_native_test.dex")  # 源码路径(用于 include 相对解析)

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


def compile_source(src, expect_error=False):
    """编译一段源码(include 以 libs/ 为库搜索路径)。"""
    with open(TMP_SRC, "w", encoding="utf-8") as f:
        f.write(src)
    toks = Lexer(src, TMP_SRC).tokenize()
    ast = Parser(toks, TMP_SRC).parse_program()
    return compile_program(ast, source_path=TMP_SRC, include_dirs=[LIBS])


def compile_ok(src):
    return compile_source(src)


def compile_error(src, fragment):
    try:
        compile_ok(src)
        return False
    except DexError as e:
        return fragment in str(e)


# ---------- 定义文件解析 ----------
def test_def_parser():
    print("[def parser]")
    def_file = parse_def(
        'refer "libx.dll";\n'
        'extern func f(a: int, b: float) -> string;\n'
        'extern func g(s: string) -> int;\n'
        'extern func h() -> void;\n', "<t>")
    check("解析 refer", def_file.lib_path == "libx.dll")
    check("解析 3 个 extern", len(def_file.natives) == 3)
    f0 = def_file.natives[0]
    check("签名 ii 参数", f0.sig == "if:s", f0.sig)
    check("arity=2", f0.arity == 2)
    check("h 返回 void", def_file.natives[2].ret_type == 0)

    check("parse_sig 往返", parse_sig("ii:i") == ([1, 1], 1))
    try:
        parse_sig("xx:y")
        check("非法签名报错", False)
    except DexError:
        check("非法签名报错", True)


# ---------- include/refer 编译 ----------
def test_include():
    print("[include/refer compile]")
    unit = compile_ok('include "math";\nprint dex_add(2, 3);\n')
    check("注册了 6 个原生函数", len(unit.natives) == 6, len(unit.natives))
    check("注册了 1 个库", len(unit.libs) == 1)
    check("库路径指向 DLL", unit.libs[0].path == DLL_PATH, unit.libs)
    names = [n.name for n in unit.natives]
    check("含 dex_add/dex_sqrt/dex_hello",
          "dex_add" in names and "dex_sqrt" in names and "dex_hello" in names)

    prog = unit.to_program()
    asm = render(prog)
    check("汇编含 .lib 行", ".lib" in asm)
    check("汇编含 .native 行", ".native dex_add 0 ii:i" in asm, asm.splitlines()[0:3])
    check("汇编含 NCALL", "NCALL dex_add" in asm)

    bc = assemble(prog)
    check("魔数", bc[:4] == b"DEXC")

    # 往返一致(含原生函数表)
    text = disassemble(bc)
    prog2 = parse_asm_text(text)
    check("往返字节码一致", assemble(prog2) == bc)

    # 直接用 refer 指向定义文件(相对源码目录)
    rel_def = os.path.relpath(DEF_PATH, ROOT).replace(os.sep, "/")
    unit2 = compile_ok(f'refer "{rel_def}";\nprint dex_fact(4);\n')
    check("refer 方式也可注册", len(unit2.natives) == 6)


# ---------- 编译期错误检查 ----------
def test_errors():
    print("[compile-time checks]")
    inc = 'include "math";\n'
    check("原生 arity 错误", compile_error(inc + 'print dex_add(1, 2, 3);', "expects 2 argument"))
    check("字符串给 int 参数", compile_error(inc + 'print dex_fact("abc");', "expects an int"))
    check("int 给 string 参数", compile_error(inc + 'print dex_len(42);', "expects a string"))
    check("非整浮点给 int 参数", compile_error(inc + 'print dex_fact(3.5);', "non-integral"))
    check("重复 include 去重", compile_ok(inc + inc + 'print dex_fact(4);\n') is not None)
    check("非顶层 include",
          compile_error('include "math";\nfunc f() { include "math"; }\nprint 1;',
                        "only allowed at the top level"))
    check("顶层 return", compile_error("return 3;", "only allowed inside a function"))
    check("字面量除零", compile_error("print 5 / 0;", "literal zero"))
    check("字面量取模零", compile_error("print 5 % 0;", "literal zero"))
    check("未定义函数", compile_error("print nope(1);", "undefined function"))

    # 不可达代码警告
    unit = compile_ok("func f() { return 1; print 2; }\nprint 3;\n")
    check("不可达代码警告", len(unit.warnings) == 1, unit.warnings)


# ---------- C VM 调用 DLL ----------
def test_vm_native():
    print("[C VM native call]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 libdexmath.dll,先运行构建脚本)")
        return
    unit = compile_ok('include "math";\nprint dex_add(2, 3);\n'
                      'print dex_fact(5);\nprint dex_sqrt(9.0);\n'
                      'print dex_len("hello");\nprint dex_hello();\n'
                      'print dex_sum3(1, 2, 3);\n')
    bc = assemble(unit.to_program())
    tmp = os.path.join(ROOT, "_native_test.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("  SKIP  (C VM 未构建)")
        return
    try:
        r = subprocess.run([vm, tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    check("原生调用输出", r.returncode == 0 and r.stdout.splitlines() == [
        "5", "120", "3", "5", "hello from dll", "6"], repr(r.stdout) + repr(r.stderr))

    # 运行期错误:DLL 缺失(先写一个指向不存在 DLL 的定义文件)
    missing_def = os.path.join(NATIVE_DIR, "_missing.dexdef")
    with open(missing_def, "w", encoding="utf-8") as f:
        f.write('refer "no_such_lib.dll";\nextern func nope() -> int;\n')
    try:
        unit2 = compile_ok('include "_missing";\nprint nope();\n')
        bc2 = assemble(unit2.to_program())
        tmp2 = os.path.join(ROOT, "_native_test2.dexbc")
        with open(tmp2, "wb") as f:
            f.write(bc2)
        try:
            r2 = subprocess.run([vm, tmp2], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=30)
        finally:
            if os.path.exists(tmp2):
                os.remove(tmp2)
        check("缺失 DLL 报运行期错误", "cannot load library" in r2.stderr, r2.stderr)
    finally:
        if os.path.exists(missing_def):
            os.remove(missing_def)


def test_chinese_output():
    """中文/Unicode 输出不应乱码(UTF-8 全链路 + 捕获解码)。"""
    print("[chinese output]")
    cn = "你好,世界!!"
    src = f'print "{cn}";\n'
    unit = compile_ok(src)
    bc = assemble(unit.to_program())
    tmp = os.path.join(ROOT, "_cn_test.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("  SKIP  (C VM 未构建)")
        return
    try:
        r = subprocess.run([vm, tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    check("C VM 中文输出正确", r.returncode == 0 and r.stdout.strip() == cn,
          repr(r.stdout))

    # pyvm 路径
    from dexlang.pyvm import run_program
    out, err = run_program(unit.to_program())
    check("pyvm 中文输出正确", err is None and out == [cn], repr(out))


def main():
    print("DEXCODE include/refer 原生库测试")
    test_def_parser()
    test_include()
    test_errors()
    test_vm_native()
    test_chinese_output()
    if os.path.exists(TMP_SRC):
        os.remove(TMP_SRC)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
