#!/usr/bin/env python3
"""DEXCODE 内置 call() 动态按名调用测试。

运行: python tests/test_call.py
覆盖:动态调用语言函数、传参/返回值、动态函数名(变量)、
调用原生库函数、调用语言模块函数、错误处理、C VM 一致性、往返。
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, decode, render,
    parse_asm_text, DexError,
)
from dexlang.pyvm import run_program  # noqa: E402

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


def run_src(src, include_dirs=None):
    unit = compile_program(
        Parser(Lexer(src, "<test>").tokenize(), "<test>").parse_program(),
        source_path=None,
        include_dirs=include_dirs or [os.path.join(ROOT, "libs")])
    prog = unit.to_program()
    out, err = run_program(prog)
    if err:
        raise RuntimeError(err)
    return out, unit


def run_vm(bc_bytes):
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("  SKIP  (C VM 未构建)")
        return None
    tmp = os.path.join(ROOT, "_tmp_test.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc_bytes)
    try:
        r = subprocess.run([vm, tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
        return r.stdout, r.stderr, r.returncode
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


SRC = """
func greet(n) {
    print "hi " + n;
}
func add(a, b) {
    return a + b;
}
func noarg() -> string {
    return "none";
}
call("greet", "bob");
print call("add", 3, 4);
print call("noarg");
let name = "add";
print call(name, 10, 20);
"""


def test_call_pyvm():
    print("动态调用(Python VM)")
    out, _ = run_src(SRC)
    expected = ["hi bob", "7", "none", "30"]
    check("call 输出匹配", out == expected, f"got {out}")


def test_call_cvm():
    print("动态调用(C VM)")
    _, unit = run_src(SRC)
    r = run_vm(assemble(unit.to_program()))
    if r is None:
        return
    out, err, code = r
    expected = ["hi bob", "7", "none", "30"]
    check("C VM 退出码 0", code == 0, f"code={code} stderr={err}")
    check("C VM 输出匹配", out.strip().splitlines() == expected, f"got {out!r}")


def test_call_native_and_module():
    print("call 调用语言模块函数")
    src = """
include "mylib";
print call("double", 21);
print call("fact5");
print call("make_w", 9);
"""
    out, _ = run_src(src)
    expected = ["42", "120", 'Wrapper(9, "w")']
    check("call 模块函数输出", out == expected, f"got {out}")


def test_call_errors():
    print("call 错误处理")
    # 缺函数名
    try:
        run_src("print call();\n")
        check("应报错: 缺函数名", False, "未抛出")
    except DexError as e:
        check("缺函数名报错", "expects at least" in str(e), str(e))
    # 运行期:未知函数名
    try:
        run_src('print call("nope_xyz");\n')
        check("应报错: 未知函数", False, "未抛出")
    except RuntimeError as e:
        check("未知函数报错", "unknown function" in str(e), str(e))
    # 运行期:参数个数不符
    try:
        run_src('func f(a) { return a; }\nprint call("f", 1, 2);\n')
        check("应报错: 参数个数不符", False, "未抛出")
    except RuntimeError as e:
        check("参数个数不符报错", "expects 1 argument" in str(e), str(e))


def test_call_roundtrip():
    print("call 汇编往返")
    _, unit = run_src(SRC)
    bc = assemble(unit.to_program())
    ir2 = decode(bc)
    bc2 = assemble(ir2)
    check("往返字节码一致", bc == bc2, "字节码不同")
    text = render(ir2)
    check("文本含 CALL_NAME", "CALL_NAME" in text, text)
    ir3 = parse_asm_text(text)
    bc3 = assemble(ir3)
    check("文本往返一致", bc == bc3, "文本往返不同")


def test_call_pyvm_cvm_consistency():
    print("pyvm 与 C VM 一致性")
    srcs = [SRC,
            'include "mylib";\nprint call("double", 21);\nprint call("fact5");\nprint call("make_w", 9);\n']
    for i, src in enumerate(srcs):
        out, unit = run_src(src)
        r = run_vm(assemble(unit.to_program()))
        if r is None:
            continue
        out2, err, code = r
        check(f"一致性 #{i}: pyvm==C VM", out == out2.strip().splitlines() and code == 0,
              f"py={out} c={out2} err={err}")


if __name__ == "__main__":
    test_call_pyvm()
    test_call_cvm()
    test_call_native_and_module()
    test_call_errors()
    test_call_roundtrip()
    test_call_pyvm_cvm_consistency()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
