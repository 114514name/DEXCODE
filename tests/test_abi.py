#!/usr/bin/env python3
"""DEXCODE M0.5 测试:原生调用的「值数组 ABI」。

覆盖:
  1) .dexdef 的 `abi value_array;` 声明(上限、顺序无关、重复、未知值)
  2) 字节码线上格式:ret_type 字节的 bit7 是调用约定标记,old 字节码该位恒 0
  3) 汇编文本的 `.abi` 指令与往返一致;现有库不产生任何 `.abi` 行
  4) 直接 ABI 的 3 参上限不回退(value_array 才放宽到 8)
  5) C VM 端到端:arity 8、混合类型、字符串/浮点/void 返回
  6) 双 VM 一致性(pyvm 与 C VM 必须给出相同结果)
  7) 常量池回归:int N 与 float N.0 不得被合并(值数组 ABI 让类型标签可观测)

运行: python tests/test_abi.py
"""

import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, disassemble, render,
    parse_asm_text, parse_def, parse_sig, DexError,
)
from dexlang import opcodes as O  # noqa: E402
from dexlang.pyvm import run_program  # noqa: E402

FIXTURES = os.path.join(ROOT, "tests", "fixtures")
LIBS = os.path.join(ROOT, "libs")
DLL = os.path.join(FIXTURES, "libabiarr.dll")
VM = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
BC_TMP = os.path.join(ROOT, "_abi_test.dexbc")   # 根目录 _ 前缀已被 .gitignore 忽略

PASS = 0
FAIL = 0
SKIP = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def skip(name, why):
    global SKIP
    SKIP += 1
    print(f"  SKIP  {name}  ({why})")


def compile_src(src):
    """编译一段源码,include 与 refer 都以 tests/fixtures/ 为搜索路径。"""
    p = os.path.join(ROOT, "_abi_test.dex")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    toks = Lexer(src, p).tokenize()
    ast = Parser(toks, p).parse_program()
    return compile_program(ast, source_path=p, include_dirs=[FIXTURES, LIBS])


def native_table(bc):
    """走一遍字节码,返回 [(name_idx, lib_idx, arity, ret_type_byte), ...]。"""
    assert bc[:4] == b"DEXC", bc[:4]
    n_consts, _n_funcs, n_libs, n_natives = struct.unpack_from("<HHHH", bc, 6)
    off = O.HEADER_SIZE
    for _ in range(n_consts):
        tag = bc[off]
        off += 1
        if tag in (O.TAG_INT, O.TAG_FLOAT):
            off += 8
        elif tag == O.TAG_STRING:
            (ln,) = struct.unpack_from("<H", bc, off)
            off += 2 + ln
        else:
            raise AssertionError("bad const tag %d" % tag)
    for _ in range(n_libs):
        off += 2 + 1
        (dlen,) = struct.unpack_from("<I", bc, off)
        off += 4 + dlen
    out = []
    for _ in range(n_natives):
        name_idx, lib_idx, arity, ret_raw = struct.unpack_from("<HHBB", bc, off)
        off += O.NATIVE_FIXED_SIZE + arity
        out.append((name_idx, lib_idx, arity, ret_raw))
    return out


def run_c(bc):
    with open(BC_TMP, "wb") as f:
        f.write(bc)
    r = subprocess.run([VM, BC_TMP], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=30)
    return r.returncode, r.stdout, r.stderr


# ---------- 1) .dexdef 的 abi 声明 ----------
def test_def_directive():
    print("[dexdef abi 指令]")
    d = parse_def('abi value_array;\nrefer "x.dll";\n'
                  'extern func f(a: int, b: int, c: int, d: int) -> int;\n', "<t>")
    check("abi 被解析", d.abi == "value_array", d.abi)
    check("arity=4 在 value_array 下可用", d.natives[0].arity == 4)

    check("默认 ABI 是 direct",
          parse_def('refer "x.dll";\nextern func f(a: int) -> int;\n', "<t>").abi == "direct")
    check("可显式写 direct",
          parse_def('abi direct;\nrefer "x.dll";\nextern func f(a: int) -> int;\n', "<t>").abi
          == "direct")

    # 顺序无关:`abi` 出现在 extern 之后也要生效
    d2 = parse_def('refer "x.dll";\nextern func f(a: int, b: int, c: int, d: int) -> int;\n'
                   'abi value_array;\n', "<t>")
    check("abi 写在 extern 之后也生效", d2.abi == "value_array")

    check("value_array 上限为 8",
          parse_def('abi value_array;\nrefer "x";\n'
                    'extern func f(a: int, b: int, c: int, d: int, e: int, f: int, '
                    'g: int, h: int) -> int;\n', "<t>").natives[0].arity == 8)

    for name, body, frag in (
        ("arity=4 在 direct 下被拒绝", 'refer "x";\n'
         'extern func f(a: int, b: int, c: int, d: int) -> int;\n', "at most 3"),
        ("arity=9 在 value_array 下被拒绝", 'abi value_array;\nrefer "x";\n'
         'extern func f(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int, '
         'i: int) -> int;\n', "at most 8"),
        ("重复 abi 被拒绝", 'abi value_array;\nabi direct;\nrefer "x";\n', "duplicate 'abi'"),
        ("未知 abi 被拒绝", 'abi nonsense;\nrefer "x";\n', "unknown abi"),
    ):
        try:
            parse_def(body, "<t>")
            check(name, False, "未报错")
        except DexError as e:
            check(name, frag in str(e), str(e))

    # direct 的报错应提示可以改用 value_array
    try:
        parse_def('refer "x";\nextern func f(a: int, b: int, c: int, d: int) -> int;\n', "<t>")
    except DexError as e:
        check("direct 越界时提示 value_array", "abi value_array" in str(e), str(e))

    check("parse_sig 默认限 3", parse_sig("iii:i") == ([1, 1, 1], 1))
    check("parse_sig max_arity=8 放行 8 参",
          parse_sig("iiiiiiii:i", max_arity=8) == ([1] * 8, 1))
    try:
        parse_sig("iiii:i")
        check("parse_sig 默认拒 4 参", False, "未报错")
    except DexError as e:
        check("parse_sig 默认拒 4 参", "at most 3" in str(e), str(e))


# ---------- 2) 字节码线上格式 ----------
def test_wire_format():
    print("[字节码线上格式:ret_type 的 bit7]")
    u_new = compile_src('include "abiarr";\nprint abi_sum8(1,2,3,4,5,6,7,8);\n')
    bc_new = assemble(u_new.to_program())
    u_old = compile_src('include "abiarr";\nprint abi_noop(1);\n')
    bc_old = assemble(compile_src('include "math";\nprint dex_add(1, 2);\n').to_program())

    ent = native_table(bc_new)[0]
    check("新 ABI:ret_type 字节 bit7 = 1", (ent[3] & O.NATIVE_ABI_MASK) != 0, hex(ent[3]))
    check("新 ABI:低 7 位仍是真实返回类型", (ent[3] & ~O.NATIVE_ABI_MASK) == O.NAT_INT,
          hex(ent[3]))
    old_ent = native_table(bc_old)[0]
    check("老 ABI:ret_type 字节 bit7 = 0(旧字节码逐字节兼容)",
          (old_ent[3] & O.NATIVE_ABI_MASK) == 0, hex(old_ent[3]))
    check("arity 本来就是 u8,无需改格式", ent[2] == 8, ent[2])


# ---------- 3) 汇编文本往返 ----------
def test_asmtext_roundtrip():
    print("[汇编文本 .abi 与往返]")
    u = compile_src('include "abiarr";\nprint abi_sum8(1,2,3,4,5,6,7,8);\n'
                    'print abi_greet("x", 1, 2.0);\n')
    bc = assemble(u.to_program())
    text = disassemble(bc)
    check("反汇编含 .abi value_array", ".abi value_array" in text)
    check("往返逐字节一致", assemble(parse_asm_text(text)) == bc)

    # 混合 ABI:同一个字节码里两种约定并存,且各自往返正确
    mixed = ('include "math";\ninclude "abiarr";\n'
             'print dex_add(1, 2);\nprint abi_noop(1);\n')
    um = compile_src(mixed)
    bcm = assemble(um.to_program())
    tm = disassemble(bcm)
    check("混合:出现 .native direct 与 .abi value_array",
          ".native dex_add 0 ii:i" in tm and ".abi value_array" in tm)
    check("混合:往返逐字节一致", assemble(parse_asm_text(tm)) == bcm)

    # 现有全 direct 的库不应产生任何 .abi 行(否则等于改了老字节码的汇编文本)
    u2 = compile_src('include "math";\nprint dex_add(1, 2);\n')
    t2 = disassemble(assemble(u2.to_program()))
    check("现有库的汇编文本不含 .abi", not any(l.startswith(".abi") for l in t2.splitlines()))
    check("现有库 natives 的 abi 均为 direct",
          all(n.abi == "direct" for n in u2.natives))

    # 未知 .abi 名要被拒
    try:
        parse_asm_text('.abi nonsense\n.func main 0\n  HALT\n')
        check("汇编文本拒绝未知 .abi", False, "未报错")
    except DexError as e:
        check("汇编文本拒绝未知 .abi", ".abi" in str(e), str(e))


# ---------- 4) C VM 端到端 + 双 VM 一致性 ----------
ABI_PROG = (
    'include "abiarr";\n'
    'print abi_sum8(1, 2, 3, 4, 5, 6, 7, 8);\n'
    'print abi_greet("dex", 3, 2.5);\n'
    'print abi_avg(1, 2, 4);\n'
    'abi_noop(7);\n'
    'print abi_tags(0, 0.0, "s");\n'
    'print "end";\n'
)


def test_vm_end_to_end():
    print("[C VM 端到端(值数组 ABI)]")
    if not os.path.exists(VM):
        skip("C VM 端到端", "vm.exe 未构建")
        return
    if not os.path.exists(DLL):
        skip("C VM 端到端", "tests/fixtures/libabiarr.dll 未构建")
        return
    unit = compile_src(ABI_PROG)
    bc = assemble(unit.to_program())
    rc, out, err = run_c(bc)
    lines = [l for l in out.splitlines() if l != ""]
    check("arity=8 调用成功", rc == 0, f"rc={rc} err={err[:160]}")
    check("arity=8 求和正确", lines[:1] == ["36"], lines[:1])
    check("混合类型 + 字符串返回",
          len(lines) > 1 and lines[1] == "hello dex x3 scale=2.5", lines[1:2])
    check("浮点返回", len(lines) > 2 and lines[2].startswith("2.333"), lines[2:3])
    check("void 返回不崩", rc == 0)
    check("类型标签未被常量池合并污染(0 与 0.0 必须是 i/f)",
          len(lines) > 3 and lines[3] == "ifs", lines[3:4])


def test_dual_vm():
    print("[双 VM 一致性]")
    if not os.path.exists(VM) or not os.path.exists(DLL):
        skip("双 VM 一致性", "需要 C VM 与 tests/fixtures/libabiarr.dll")
        return
    unit = compile_src(ABI_PROG)
    prog = unit.to_program()
    py_out, py_err = run_program(prog)
    rc, out, err = run_c(assemble(prog))
    c_out = [l for l in out.splitlines() if l != ""]
    check("pyvm 无错误", py_err is None, str(py_err))
    check("C VM 退出码 0", rc == 0, err[:160])
    check("两个 VM 输出逐行一致", py_out == c_out, f"py={py_out} c={c_out}")


# ---------- 5) 常量池回归(int N 与 float N.0 不得合并) ----------
def test_const_pool_types():
    print("[常量池:int 与 float 不撞键]")
    src = 'print 0;\nprint 0.0;\nprint 1;\nprint 1.0;\n'
    bc = assemble(compile_src(src).to_program())
    text = disassemble(bc)
    pushes = [l.strip() for l in text.splitlines() if l.strip().startswith("PUSH")]
    check("int 0 与 float 0.0 各自保留", pushes == ["PUSH 0", "PUSH 0.0", "PUSH 1", "PUSH 1.0"],
          pushes)
    check("往返仍逐字节一致", assemble(parse_asm_text(text)) == bc)


def main():
    print("DEXCODE M0.5:值数组 ABI 测试")
    test_def_directive()
    test_wire_format()
    test_asmtext_roundtrip()
    test_vm_end_to_end()
    test_dual_vm()
    test_const_pool_types()
    for p in (BC_TMP, os.path.join(ROOT, "_abi_test.dex")):
        if os.path.exists(p):
            os.remove(p)
    dxasm = os.path.join(ROOT, "_abi_test.dxasm")
    if os.path.exists(dxasm):
        os.remove(dxasm)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
