#!/usr/bin/env python3
"""DEXCODE 自定义数据类型(type/struct)测试。

运行: python tests/test_struct.py
覆盖:type 定义、结构体字面量、成员读写、成员赋值、函数返回对象、
对象相等比较、默认字段、嵌套字段、pyvm 与 C VM 双实现一致性、往返。
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, decode, disassemble,
    render, parse_asm_text, DexError,
)
from dexlang.pyvm import run_program  # noqa: E402

TAG = "struct"   # 本测试文件专用的临时文件名后缀

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


def compile_source(src):
    tokens = Lexer(src, "<test>").tokenize()
    ast = Parser(tokens, "<test>").parse_program()
    unit = compile_program(ast)
    return unit


def run_py(src):
    """Python 参考虚拟机执行,返回输出行。"""
    unit = compile_source(src)
    prog = unit.to_program()
    out, err = run_program(prog)
    if err:
        raise RuntimeError(err)
    return out


def run_vm(bc_bytes):
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(vm):
        print("  SKIP  (C VM 未构建,先运行: python main.py build-vm)")
        return None
    # 临时字节码路径带本文件唯一后缀:多个测试文件曾共用 _tmp_test.dexbc,
    # 互相覆盖/删除会让 VM 读到被截断的文件而异常退出。
    tmp = os.path.join(ROOT, f"_tmp_{TAG}.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc_bytes)
    try:
        r = subprocess.run([vm, tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
        return r.stdout, r.stderr, r.returncode
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


BASIC = """
type Point {
    x: int;
    y: int;
    name: string;
}

func make() -> Point {
    return Point{x: 3, y: 4, name: "P"};
}

let p = Point{x: 1, y: 2, name: "A"};
print p;
print p.x;
print p.y;
print p.name;
p.x = 10;
print p.x;
let q = make();
print q;
print q.name;
let r = p;
print r == p;
print p == q;
"""


def test_basic_pyvm():
    print("基本结构体(Python VM)")
    out = run_py(BASIC)
    expected = [
        'Point(1, 2, "A")',
        "1",
        "2",
        "A",
        "10",
        'Point(3, 4, "P")',
        "P",
        "1",
        "0",
    ]
    check("pyvm 输出匹配", out == expected, f"got {out}")


def test_basic_cvm():
    print("基本结构体(C VM)")
    unit = compile_source(BASIC)
    r = run_vm(assemble(unit.to_program()))
    if r is None:
        return
    out, err, code = r
    expected = [
        'Point(1, 2, "A")',
        "1",
        "2",
        "A",
        "10",
        'Point(3, 4, "P")',
        "P",
        "1",
        "0",
    ]
    check("C VM 退出码 0", code == 0, f"code={code} stderr={err}")
    check("C VM 输出匹配", out.strip().splitlines() == expected,
          f"got {out!r}")


def test_default_fields():
    print("默认字段")
    src = """
type Rect { w: int; h: int; label: string; }
let z = Rect{w: 5};
print z;
"""
    out = run_py(src)
    check("缺省字段取默认值", out == ['Rect(5, 0, "")'], f"got {out}")


def test_nested_obj():
    print("嵌套结构体")
    src = """
type Inner { v: int; }
type Outer { a: int; in: Inner; }
let o = Outer{a: 1, in: Inner{v: 9}};
print o;
print o.in.v;
o.in.v = 42;
print o.in.v;
"""
    out = run_py(src)
    expected = ['Outer(1, Inner(9))', "9", "42"]
    check("嵌套对象输出/读写", out == expected, f"got {out}")


def test_obj_in_math():
    print("结构体与算术")
    src = """
type Vec { x: float; y: float; }
func len2(v: Vec) -> float {
    return v.x * v.x + v.y * v.y;
}
let v = Vec{x: 3, y: 4};
print len2(v);
"""
    out = run_py(src)
    check("对象传给函数/算术", out == ["25"], f"got {out}")


def test_cmp_mixed():
    print("对象比较语义")
    src = """
type A { x: int; }
let a1 = A{x: 1};
let a2 = A{x: 1};
let a3 = A{x: 2};
print a1 == a2;
print a1 == a3;
print a1 != a2;
"""
    out = run_py(src)
    check("字段相等比较", out == ["1", "0", "0"], f"got {out}")


def test_errors():
    print("错误处理")
    cases = [
        ("type A { x: int; } type A { y: int; }", "duplicate type"),
        ("print A{x: 1};", "unknown type"),
        ("type A { x: int; } let a = A{x: 1}; print a.b;", "no field"),
        ("type A { x: int; } func f() { type B { y: int; } return 0; }", "only allowed at the top level"),
    ]
    for src, keyword in cases:
        try:
            compile_source(src)
            check(f"应报错: {keyword}", False, "未抛出")
        except DexError as e:
            check(f"报错含关键字 '{keyword}'", keyword in str(e), str(e))


def test_roundtrip():
    print("汇编与字节码往返")
    src = """
type P { x: int; y: int; }
let p = P{x: 7, y: 8};
print p.x + p.y;
"""
    unit = compile_source(src)
    bc = assemble(unit.to_program())
    ir2 = decode(bc)
    bc2 = assemble(ir2)
    check("往返字节码一致", bc == bc2, "字节码不同")
    # 文本渲染/解析往返
    text = render(ir2)
    ir3 = parse_asm_text(text)
    bc3 = assemble(ir3)
    check("文本往返字节码一致", bc == bc3, "文本往返字节码不同")
    check("文本含 MAKE_OBJ", "MAKE_OBJ P(2)" in text, text)


def test_pyvm_cvm_consistency():
    print("pyvm 与 C VM 输出一致性")
    srcs = [
        BASIC,
        "type Rect { w: int; h: int; label: string; } let z = Rect{w: 5}; print z;",
        """
type Inner { v: int; }
type Outer { a: int; in: Inner; }
let o = Outer{a: 1, in: Inner{v: 9}};
print o;
print o.in.v;
o.in.v = 42;
print o.in.v;
""",
        """
type Vec { x: float; y: float; }
func len2(v: Vec) -> float { return v.x * v.x + v.y * v.y; }
let v = Vec{x: 3, y: 4};
print len2(v);
""",
    ]
    for i, src in enumerate(srcs):
        unit = compile_source(src)
        py = run_py(src)
        r = run_vm(assemble(unit.to_program()))
        if r is None:
            continue
        out, err, code = r
        cvm = out.strip().splitlines()
        check(f"一致性 #{i}: pyvm==C VM", py == cvm and code == 0,
              f"py={py} c={cvm} err={err}")


if __name__ == "__main__":
    test_basic_pyvm()
    test_basic_cvm()
    test_default_fields()
    test_nested_obj()
    test_obj_in_math()
    test_cmp_mixed()
    test_errors()
    test_roundtrip()
    test_pyvm_cvm_consistency()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
