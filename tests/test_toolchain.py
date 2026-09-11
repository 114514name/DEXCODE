#!/usr/bin/env python3
"""DEXCODE 工具链端到端测试。

运行: python tests/test_toolchain.py
覆盖:词法、语法、编译降级、汇编、反汇编往返、C VM 执行。
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
from dexlang.tokens import TokKind  # noqa: E402

TAG = "toolchain"   # 本测试文件专用的临时文件名后缀

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


# ---------- 词法 ----------
def test_lexer():
    print("[lexer]")
    toks = Lexer('let x = 42; // c\nif x <= 3.5 { print "hi"; }', "<t>").tokenize()
    kinds = [t.kind for t in toks]
    check("关键词/标识符", TokKind.LET in kinds and TokKind.IDENT in kinds)
    check("数字 42", any(t.value == 42 for t in toks))
    check("浮点 3.5", any(t.value == 3.5 for t in toks))
    check("字符串", any(t.value == "hi" for t in toks))
    check("运算符", TokKind.LE in kinds and TokKind.LBRACE in kinds)


# ---------- 语法 ----------
def test_parser():
    print("[parser]")
    toks = Lexer("let a = 1 + 2 * 3; print a;", "<t>").tokenize()
    prog = Parser(toks, "<t>").parse_program()
    check("程序含 2 条语句", len(prog.stmts) == 2)
    from dexlang import ast
    check("优先级 a+(2*3) 的 AST 结构",
          isinstance(prog.stmts[0], ast.Let)
          and isinstance(prog.stmts[0].value, ast.BinOp)
          and prog.stmts[0].value.op == "+"
          and isinstance(prog.stmts[0].value.right, ast.BinOp)
          and prog.stmts[0].value.right.op == "*")


# ---------- 编译 → 汇编 → 字节码 → 反汇编往返 ----------
def test_roundtrip():
    print("[compile/assemble/disasm roundtrip]")
    src = """
func max(a, b) {
    if a > b {
        return a;
    }
    return b;
}
let n = 0;
let msg = "hello world";
while n < 5 {
    print max(n, 2);
    n = n + 1;
}
print msg;
"""
    unit = compile_source(src)
    asm_text = render(unit.to_program())
    check("汇编含 .func main", ".func main 0" in asm_text)
    check("汇编含 .func max", ".func max 2" in asm_text)
    check("汇编含 CALL", "@max" in asm_text)
    check("汇编含跳转标签", ":" in asm_text)
    check("汇编保留含空格字符串", '"hello world"' in asm_text)

    bc = assemble(unit.to_program())
    check("魔数", bc[:4] == b"DEXC")

    text2 = disassemble(bc)
    check("反汇编含 PUSH", "PUSH" in text2)
    check("反汇编含 CALL @max", "@max" in text2)
    check("反汇编保留含空格字符串", '"hello world"' in text2)

    # 字节码 → 汇编 → 字节码 往返一致
    prog2 = parse_asm_text(text2)
    bc2 = assemble(prog2)
    check("往返字节码一致", bc == bc2, f"(len {len(bc)} vs {len(bc2)})")

    # 反汇编后的汇编可再次通过 asm 命令汇编(用 parse_asm_text 验证)
    check("反汇编文本可再解析", len(prog2.funcs) == 2)


# ---------- C VM 执行 ----------
def test_vm():
    print("[C VM]")

    # 1) 算术
    prog = compile_source("print 1 + 2 * 3; print 7 / 2; print 10 % 3;").to_program()
    r = run_vm(assemble(prog))
    if r is None:
        return
    out, err, rc = r
    check("算术输出", rc == 0 and out.splitlines() == ["7", "3.5", "1"], repr(out))

    # 2) if/else 与比较
    prog = compile_source(
        'let x = 5; if x > 3 { print "big"; } else { print "small"; }'
    ).to_program()
    r = run_vm(assemble(prog))
    check("if/else", r is not None and r[0].strip() == "big", repr(r))

    # 3) while 求和
    prog = compile_source(
        "let s = 0; let i = 1; while i <= 100 { s = s + i; i = i + 1; } print s;"
    ).to_program()
    r = run_vm(assemble(prog))
    check("while 1..100 求和 = 5050", r is not None and r[0].strip() == "5050", repr(r))

    # 4) 递归 fib(10) = 55
    prog = compile_source(
        "func fib(n) { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); } print fib(10);"
    ).to_program()
    r = run_vm(assemble(prog))
    check("递归 fib(10)=55", r is not None and r[0].strip() == "55", repr(r))

    # 5) 短路逻辑
    prog = compile_source(
        'print 0 && 1; print 1 || 0; print !0; print 1 && (2 > 1);'
    ).to_program()
    r = run_vm(assemble(prog))
    check("逻辑运算", r is not None and r[0].splitlines() == ["0", "1", "1", "1"], repr(r))

    # 6) 字符串拼接(+ 操作数为字符串时,数字自动转字符串)
    prog = compile_source(
        'let s = "hi"; print "a" + "b"; print "n=" + 42; print "x=" + 3.5;'
        ' print s + "!"; print 1 + 2;'
    ).to_program()
    r = run_vm(assemble(prog))
    check("字符串拼接", r is not None and r[0].splitlines() ==
          ["ab", "n=42", "x=3.5", "hi!", "3"], repr(r))
    check("拼接降级为 CONCAT", "CONCAT" in render(prog), render(prog))
    from dexlang.pyvm import run_program as rp
    out_l, err_l = rp(prog)
    check("pyvm 字符串拼接一致", err_l is None and out_l ==
          ["ab", "n=42", "x=3.5", "hi!", "3"], repr(out_l))


# ---------- 错误处理 ----------
def test_errors():
    print("[errors]")
    try:
        compile_source("let x = ;")
        check("未定义语法错误被捕获", False)
    except DexError as e:
        check("语法错误被捕获", "parser" in str(e), str(e))

    try:
        compile_source("print y;")  # y 未声明
        check("未定义变量被捕获", False)
    except DexError as e:
        check("未定义变量被捕获", "compiler" in str(e), str(e))

    try:
        compile_source("func f() { return 1; } print f(1, 2);")  # 参数个数不符
        check("参数个数错误被捕获", False)
    except DexError as e:
        check("参数个数错误被捕获", "compiler" in str(e), str(e))


def main():
    print("DEXCODE 工具链测试")
    test_lexer()
    test_parser()
    test_roundtrip()
    test_vm()
    test_errors()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
