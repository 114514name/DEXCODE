#!/usr/bin/env python3
"""DEXCODE include .dex 语言模块测试。

运行: python tests/test_module.py
覆盖:模块函数调用、模块类型、模块内 include 原生库、递归模块、
重复 include 去重、非法顶层语句报错、C VM 一致性。
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _tmpdir import mktempdir  # noqa: E402
from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, DexError,
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
    """编译并(用 pyvm)运行,返回输出行。"""
    unit = compile_program(Parser(Lexer(src, "<test>").tokenize(), "<test>").parse_program(),
                           source_path=None, include_dirs=include_dirs or [os.path.join(ROOT, "libs")])
    prog = unit.to_program()
    out, err = run_program(prog)
    if err:
        raise RuntimeError(err)
    return out


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


MAIN = """
include "mylib";
print double(21);
print fact5();
let w = make_w(7);
print w;
print w.v;
print w.label;
let b = bump(w, 3);
print b.v;
"""


def test_module_funcs_types():
    print("语言模块:函数与类型")
    out = run_src(MAIN)
    expected = ["42", "120", 'Wrapper(7, "w")', "7", "w", "10"]
    check("模块函数/类型输出", out == expected, f"got {out}")


def test_module_cvm():
    print("语言模块:C VM")
    unit = compile_program(
        Parser(Lexer(MAIN, "<test>").tokenize(), "<test>").parse_program(),
        source_path=None, include_dirs=[os.path.join(ROOT, "libs")])
    r = run_vm(assemble(unit.to_program()))
    if r is None:
        return
    out, err, code = r
    expected = ["42", "120", 'Wrapper(7, "w")', "7", "w", "10"]
    check("C VM 退出码 0", code == 0, f"code={code} stderr={err}")
    check("C VM 输出匹配", out.strip().splitlines() == expected, f"got {out!r}")


def test_recursive_module():
    print("递归模块 include")
    # 模块 A include 模块 B;主文件只 include A
    tmp = mktempdir("dexmod_")
    try:
        os.makedirs(os.path.join(tmp, "libb"))
        os.makedirs(os.path.join(tmp, "liba"))
        with open(os.path.join(tmp, "libb", "libb.dex"), "w", encoding="utf-8") as f:
            f.write("func from_b() -> int { return 99; }\n")
        with open(os.path.join(tmp, "liba", "liba.dex"), "w", encoding="utf-8") as f:
            f.write("include \"libb\";\nfunc from_a() -> int { return from_b() + 1; }\n")
        src = 'include "liba";\nprint from_a();\nprint from_b();\n'
        out = run_src(src, include_dirs=[tmp])
        check("递归模块输出", out == ["100", "99"], f"got {out}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_duplicate_include_dedup():
    print("重复 include 去重")
    tmp = mktempdir("dexmod_")
    try:
        with open(os.path.join(tmp, "dup.dex"), "w", encoding="utf-8") as f:
            f.write("func dup_f() -> int { return 5; }\n")
        src = 'include "dup";\ninclude "dup";\nprint dup_f();\n'
        out = run_src(src, include_dirs=[tmp])
        check("重复 include 只合并一次", out == ["5"], f"got {out}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_library_may_not_execute():
    print("模块非法顶层语句")
    tmp = mktempdir("dexmod_")
    try:
        with open(os.path.join(tmp, "bad.dex"), "w", encoding="utf-8") as f:
            f.write("let x = 1;\n")
        src = 'include "bad";\nprint 1;\n'
        try:
            run_src(src, include_dirs=[tmp])
            check("应报错: 模块顶层可执行", False, "未抛出")
        except DexError as e:
            check("报错含 'only contain declarations'",
                  "only contain declarations" in str(e), str(e))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_missing_lib_error():
    print("找不到库报错")
    try:
        run_src('include "no_such_lib_xyz";\nprint 1;\n',
                include_dirs=[os.path.join(ROOT, "libs")])
        check("应报错: 找不到库", False, "未抛出")
    except DexError as e:
        check("报错含 'cannot find library'", "cannot find library" in str(e), str(e))


if __name__ == "__main__":
    test_module_funcs_types()
    test_module_cvm()
    test_recursive_module()
    test_duplicate_include_dedup()
    test_library_may_not_execute()
    test_missing_lib_error()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
