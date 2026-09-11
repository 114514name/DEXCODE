#!/usr/bin/env python3
"""DEXCODE 健壮性回归测试。

运行: python tests/test_robust.py

覆盖此前实测确认过的一批缺陷(修复后作为回归护栏):
  1. 畸形字节码加载期校验:nlocals<arity(堆溢出)、函数 code 区间越界、指令对齐
  2. 循环引用对象不再耗尽 C 栈(打印/比较)
  3. 对象逐字段比较语义(-1/0/1 保序),且 C VM 与 pyvm 一致
  4. 编译器拒绝把对象赋给基础类型字段(循环对象的根因)
  5. 合法结构体赋值不受影响(无误伤)
"""

import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

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


def vm_path():
    return os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")


def has_vm():
    return os.path.exists(vm_path())


def run_vm(bc_bytes, timeout=30):
    """把字节码喂给 C VM,返回 (stdout, stderr, returncode)。"""
    tmp = os.path.join(ROOT, "_tmp_robust.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc_bytes)
    try:
        r = subprocess.run([vm_path(), tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return r.stdout, r.stderr, r.returncode
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def compile_src(src):
    tokens = Lexer(src, "<test>").tokenize()
    ast = Parser(tokens, "<test>").parse_program()
    return compile_program(ast, source_path=None, include_dirs=[])


def run_py(src):
    unit = compile_src(src)
    out, err = run_program(unit.to_program())
    if err:
        raise RuntimeError(err)
    return out


def run_c(src):
    return run_vm(assemble(compile_src(src).to_program()))


# ---------- 手工构造字节码(用于畸形输入测试) ----------

def build_bc(code, consts, funcs):
    """consts: str 作 STRING 常量,int 作 INT 常量。
    funcs:  [(name_idx, arity, nlocals, code_off, code_len), ...]"""
    pool = b""
    for c in consts:
        if isinstance(c, str):
            b = c.encode("utf-8")
            pool += bytes([2]) + struct.pack("<H", len(b)) + b
        else:
            pool += bytes([0]) + struct.pack("<q", c)
    code = bytes(bytearray(code))
    hdr = (b"DEXC" + bytes([3, 0])
           + struct.pack("<HHHHI", len(consts), len(funcs), 0, 0, len(code)))
    ft = b"".join(struct.pack("<HBHII", *f) for f in funcs)
    return hdr + pool + ft + code


def test_malformed_bytecode():
    print("畸形字节码加载期校验")
    if not has_vm():
        print("  SKIP  (C VM 未构建)")
        return

    # (a) nlocals < arity:旧版会 calloc(0) 后按 arity 越界写堆
    code = bytearray()
    code += bytes([0x16]) + struct.pack("<I", 1)   # CALL f1
    code += bytes([0x1B])                          # HALT
    mal = build_bc(code, ["main", "f"],
                   [(0, 0, 0, 0, len(code)), (1, 8, 0, len(code) - 1, 1)])
    out, err, rc = run_vm(mal)
    check("nlocals<arity 被拒绝", rc != 0 and "nlocals" in err, f"rc={rc} err={err[:120]}")

    # (b) 函数 code 区间越界(code_off + code_len > code_size)
    mal = build_bc([0x1B], ["main"], [(0, 0, 0, 999, 1)])
    out, err, rc = run_vm(mal)
    check("函数 code 区间越界被拒绝", rc != 0 and "exceeds code section" in err,
          f"rc={rc} err={err[:120]}")

    # (c) 指令对齐:code_len 只声明到操作数中间
    code = bytes([0x01]) + struct.pack("<I", 0)     # PUSH 常量(5 字节)
    mal = build_bc(code, ["main", 0], [(0, 0, 1, 0, 3)])   # code_len=3 < 5
    out, err, rc = run_vm(mal)
    check("截断指令被拒绝", rc != 0 and "truncated instruction" in err,
          f"rc={rc} err={err[:120]}")

    # (d) 对照:同一份字节码把 code_len 写对,必须正常运行
    code = bytes([0x1B])                            # HALT
    good = build_bc(code, ["main"], [(0, 0, 0, 0, 1)])
    out, err, rc = run_vm(good)
    check("合法字节码仍正常运行", rc == 0, f"rc={rc} err={err[:120]}")


# ---------- 循环引用对象 ----------

CYCLE_PRINT = """
type N { v: int; }
let a = N { v: 0 };
a.v = a;
print a;
"""

CYCLE_CMP = """
type N { v: int; }
let a = N { v: 0 };
let b = N { v: 1 };
a.v = a;
b.v = b;
print a == b;
"""

CMP_SRC = """
type P { x: int; y: int; }
let a = P { x: 1, y: 2 };
let b = P { x: 5, y: 6 };
print a == b;
print a < b;
print a > b;
print a <= b;
print a >= b;
"""


def test_compiler_rejects_object_into_scalar():
    print("编译器拒绝 对象→基础类型字段 赋值")
    for src, label in ((CYCLE_PRINT, "a.v = a"), (CYCLE_CMP, "a.v = a / b.v = b")):
        try:
            compile_src(src)
            check(f"{label} 被编译期拒绝", False, "未报错")
        except DexError as e:
            check(f"{label} 被编译期拒绝",
                  "cannot assign" in str(e), str(e)[:140])


def test_object_comparison_order():
    print("对象逐字段比较保序")
    py = run_py(CMP_SRC)
    check("pyvm 比较结果保序", py == ["0", "1", "0", "1", "0"], str(py))
    if has_vm():
        out, err, rc = run_c(CMP_SRC)
        cvm = out.strip().splitlines()
        check("C VM 比较结果保序", cvm == ["0", "1", "0", "1", "0"], str(cvm))
        check("C VM 与 pyvm 对象比较一致", cvm == py, f"C={cvm} PY={py}")


def test_valid_struct_assign_still_works():
    print("合法结构体赋值不受影响")
    src = """
type P { x: int; y: float; s: string; }
let p = P { x: 1, y: 2.5, s: "a" };
p.x = 10;
p.y = 3;
p.s = "b";
print p.x;
print p.y;
print p.s;
let q = P { x: 0, y: 0.0, s: "" };
q.x = p.x;
print q.x;
"""
    py = run_py(src)
    check("pyvm 合法赋值", py == ["10", "3", "b", "10"], str(py))
    if has_vm():
        out, err, rc = run_c(src)
        check("C VM 合法赋值", out.strip().splitlines() == ["10", "3", "b", "10"],
              f"{out!r} {err[:100]}")


def test_float_literal_into_int_field_rejected():
    print("类型错误的赋值被拒绝")
    bad = [
        ('type P { x: int; }\nlet p = P { x: 0 };\np.x = "s";', "string → int"),
        ('type P { s: string; }\nlet p = P { s: "" };\np.s = 5;', "int → string"),
    ]
    for src, label in bad:
        try:
            compile_src(src)
            check(f"{label} 被拒绝", False, "未报错")
        except DexError:
            check(f"{label} 被拒绝", True)


# ---------- 循环对象在 VM 层兜底(绕过编译器的字节码) ----------


def test_cycle_guard_in_vm():
    """环已被三道措施排除,这里验证 VM 的兜底:即使字节码试图构造自引用,
    也不会崩溃、不会双重释放。

    排除环的三道措施:
      1) 编译期拒绝递归类型(类型层面不可表达环);
      2) 编译期校验字段赋值类型;
      3) VM 侧 SET_FIELD 与 STORE 一样按**值语义**拷贝(P3/P4),
         因此 `a.v = a` 会把 a 的一份副本放进字段,而不是让字段指向 a 自身。"""
    print("VM 层环防护兜底")
    if not has_vm():
        print("  SKIP  (C VM 未构建)")
        return

    from dexlang.ir import (AsmFunc, Insn, AssemblyProgram,
                            ConstOperand, LocalOperand, TypeOperand, FieldOperand)
    from dexlang import opcodes as O

    # 绕过编译器直接构造字节码:建对象 -> 把自己的引用填进字段 -> 打印 + 比较
    # 若是引用语义,这会形成自引用环;值语义下字段得到一份副本,故不成环。
    def build(with_cmp):
        f = AsmFunc(name="main", arity=0, nlocals=1)
        insns = [
            Insn(op=O.PUSH, operand=ConstOperand(0)),      # 字段初值 int 0
            Insn(op=O.MAKE_OBJ, operand=TypeOperand("N", 1)),
            Insn(op=O.STORE, operand=LocalOperand(0)),
            Insn(op=O.LOAD, operand=LocalOperand(0)),      # obj
            Insn(op=O.LOAD, operand=LocalOperand(0)),      # value = 同一 obj
            Insn(op=O.SET_FIELD, operand=FieldOperand(0)),
            Insn(op=O.LOAD, operand=LocalOperand(0)),
            Insn(op=O.PRINT),
        ]
        if with_cmp:
            insns += [Insn(op=O.LOAD, operand=LocalOperand(0)),
                      Insn(op=O.LOAD, operand=LocalOperand(0)),
                      Insn(op=O.EQ), Insn(op=O.PRINT)]
        insns.append(Insn(op=O.HALT))
        f.insns = insns
        return assemble(AssemblyProgram(funcs=[f], natives=[], libs=[]))

    out, err, rc = run_vm(build(with_cmp=False), timeout=30)
    check("手写字节码构造自引用不崩溃", rc == 0, f"rc={rc} err={err[:140]}")
    check("打印结果不是无限递归(值语义:字段是副本)",
          "..." not in out and out.count("N(") <= 2, repr(out[:90]))

    out2, err2, rc2 = run_vm(build(with_cmp=True), timeout=30)
    check("自引用对象比较不崩溃", rc2 == 0, f"rc={rc2} err={err2[:140]}")
    check("比较有确定结果", out2.strip().splitlines()[-1] in ("0", "1") if out2.strip() else False,
          repr(out2[:90]))


def test_int64_mod_edge():
    print("INT64_MIN % -1 不触发陷阱")
    if not has_vm():
        print("  SKIP  (C VM 未构建)")
        return
    # -9223372036854775808 是 INT64_MIN;C 里 INT64_MIN % -1 是 UB(x86-64 上 SIGFPE)
    src = ("let m = 0 - 9223372036854775807 - 1;\n"
           "print m % (0 - 1);\n")
    out, err, rc = run_c(src)
    check("INT64_MIN % -1 正常返回 0", rc == 0 and out.strip() == "0",
          f"rc={rc} out={out!r} err={err[:120]}")


# ---------- 字符串所有权(内存管理)与别名安全 ----------

STR_ALIAS = """
func mk(n) {
    let s = "x";
    let i = 0;
    while i < n { s = s + "y"; i = i + 1; }
    return s;
}
let a = mk(30);
print a;
let p = mk(3);
let q = p;
let r = p + "!";
p = "gone";
print q;
print r;
type Box { name: string; n: int; }
let b = Box { name: mk(4), n: 1 };
let tmp = mk(2);
b.name = tmp;
tmp = "overwritten";
print b.name;
print b;
let k = 0;
let acc = "";
while k < 100 { acc = acc + "ab"; k = k + 1; }
print k;
print acc;
"""

# 期望输出(注意 mk(1)="xy"、mk(2)="xyy",故 mk(1)+mk(2)="xyxyy")
STR_ALIAS_EXPECT = [
    "x" + "y" * 30,
    "xyyy",
    "xyyy!",
    "xyy",
    'Box("xyy", 1)',
    "100",
    "ab" * 100,
]


def test_string_flow_aliasing():
    """堆字符串在 局部槽/栈/对象字段/函数返回 之间流转时,内容必须正确。

    覆盖一个真实且已修复的陷阱:`return s;` 时返回值与局部槽指向同一块内存,
    任何"帧销毁时释放自有字符串"的策略都会让返回值悬垂(实测堆损坏)。
    当前 VM 选择不释放运行期字符串(见 README「已知限制」),因此本用例
    既验证内容正确,也防止将来有人在没有引用计数的前提下重新引入释放逻辑。"""
    print("堆字符串流转内容正确性")
    py = run_py(STR_ALIAS)
    check("pyvm 流转场景正确", py == STR_ALIAS_EXPECT,
          f"got={py[:3]}... want={STR_ALIAS_EXPECT[:3]}...")
    if has_vm():
        out, err, rc = run_c(STR_ALIAS)
        cvm = out.strip().splitlines()
        check("C VM 流转场景不悬垂", rc == 0, f"rc={rc} err={err[:120]}")
        check("C VM 与 pyvm 逐行一致", cvm == STR_ALIAS_EXPECT,
              f"C={cvm[:3]}... WANT={STR_ALIAS_EXPECT[:3]}...")


def test_return_value_in_concat():
    """无返回类型标注的函数,其返回值参与 + 时必须走 CONCAT 而不是 ADD。

    此前 infer() 对未标注返回类型的调用一律返回 None,`mk(1) + mk(2)` 被降级成
    数值 ADD,运行期报 unsupported operand types for arithmetic。"""
    print("函数返回值参与字符串拼接")
    src = ('func mk(n) { let s = "x"; let i = 0;\n'
           '  while i < n { s = s + "y"; i = i + 1; } return s; }\n'
           'let c1 = mk(1);\n'          # "xy"
           'let c2 = mk(2);\n'          # "xyy"
           'print c1 + c2 + "end";\n'   # "xyxyyend"
           'if c1 + c2 == "xyxyy" { print "ok"; }\n')
    exp = ["xyxyyend", "ok"]
    py = run_py(src)
    check("pyvm 拼接正确", py == exp, str(py))
    if has_vm():
        out, err, rc = run_c(src)
        check("C VM 拼接正确", out.strip().splitlines() == exp,
              f"{out!r} {err[:120]}")


def test_recursive_return_type_inference():
    """递归函数的返回类型推断必须终止(防无限递归)。"""
    print("递归函数返回类型推断")
    src = ('func fib(n) { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }\n'
           'print fib(10);\n'
           'print fib(5) + fib(5);\n')
    py = run_py(src)
    check("pyvm 递归正常", py == ["55", "10"], str(py))
    if has_vm():
        out, err, rc = run_c(src)
        check("C VM 递归正常", out.strip().splitlines() == ["55", "10"],
              f"{out!r} {err[:120]}")


if __name__ == "__main__":
    print("=== 畸形字节码 ===")
    test_malformed_bytecode()
    print("=== 编译器类型校验 ===")
    test_compiler_rejects_object_into_scalar()
    test_float_literal_into_int_field_rejected()
    test_valid_struct_assign_still_works()
    print("=== 对象比较语义 ===")
    test_object_comparison_order()
    print("=== 整型边界 ===")
    test_int64_mod_edge()
    print("=== 字符串流转/所有权 ===")
    test_string_flow_aliasing()
    print("=== 返回类型推断 ===")
    test_return_value_in_concat()
    test_recursive_return_type_inference()
    print("=== VM 兜底 ===")
    test_cycle_guard_in_vm()
    print()
    print(f"结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
