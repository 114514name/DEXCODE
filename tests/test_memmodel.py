#!/usr/bin/env python3
"""DEXCODE 内存模型与 FFI 所有权回归测试(P0 / P1 / P2)。

运行: python tests/test_memmodel.py

覆盖 docs/MEMORY_DESIGN.md 已实施的三层:

  P0 统一堆入口   —— vm_alloc/vm_calloc/vm_free 收口所有 VM 堆分配(纯重构)
  P1 内存预算     —— DEXCODE_MAX_MEM_MB;超限时给出可诊断的错误而非静默涨到被 OOM 杀
  P2 FFI 所有权   —— 定义文件里 (string)->void 的函数被认作该库的释放函数,
                      VM 在复制原生返回的字符串后用它释放原生缓冲区;
                      未声明时给出编译期警告(该库须返回静态缓冲区)

P2 用 tests/fixtures/libleaky.{c,dll} 验证:该库返回 malloc 的字符串,
并用指针登记表精确区分"自己分配的"与"借来的"指针。
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, decode, render, DexError,
)
from dexlang.pyvm import run_program  # noqa: E402

FIXTURES = os.path.join(ROOT, "tests", "fixtures")
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


TMPDIR = os.path.join(ROOT, "tests", "_tmp_memmodel")


def compile_src(src, fname=None):
    """把源码落到 tests/_tmp_memmodel/ 下再编译,使 `include "../fixtures/x"` 能解析。"""
    os.makedirs(TMPDIR, exist_ok=True)
    if fname is None or not os.path.isabs(fname):
        fname = os.path.join(TMPDIR, "case.dex")
    with open(fname, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    tokens = Lexer(src, fname).tokenize()
    ast = Parser(tokens, fname).parse_program()
    return compile_program(ast, source_path=fname, include_dirs=[FIXTURES])


def run_vm(bc, env_extra=None, timeout=60):
    """把字节码交给 C VM;返回 (stdout, stderr, returncode)。"""
    tmp = os.path.join(ROOT, "_tmp_memmodel.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    try:
        r = subprocess.run([vm_path(), tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout,
                           env=env)
        return r.stdout, r.stderr, r.returncode
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def run_c(src, env_extra=None, **kw):
    return run_vm(assemble(compile_src(src).to_program()), env_extra, **kw)


def run_py(src):
    unit = compile_src(src)
    out, err = run_program(unit.to_program())
    if err:
        raise RuntimeError(err)
    return out


def has_vm():
    return os.path.exists(vm_path())


def has_fixture():
    return os.path.exists(os.path.join(FIXTURES, "libleaky.dll"))


# ============================================================
# P1:内存预算
# ============================================================

ACCUM = """
let s = "";
let i = 0;
while i < 20000 { s = s + "0123456789"; i = i + 1; }
print "done";
"""


def test_budget():
    print("P1 内存预算(DEXCODE_MAX_MEM_MB)")
    if not has_vm():
        print("  SKIP  (C VM 未构建)")
        return

    # 0 / 未设置 = 不限(默认不干扰既有程序)
    out, err, rc = run_c(ACCUM, {"DEXCODE_MAX_MEM_MB": "0"})
    check("0 = 不限制,程序正常跑完", rc == 0 and "done" in out, f"rc={rc} {err[:120]}")

    # 小预算:必须在超限时停下,并给出指向性的诊断
    out, err, rc = run_c(ACCUM, {"DEXCODE_MAX_MEM_MB": "8"})
    check("小预算触发上限并退出", rc != 0, f"rc={rc}")
    check("错误信息点明预算上限", "超过预算上限" in err, err[:200])
    check("错误信息给出最常见原因(循环内字符串累加)",
          "字符串累加" in err, err[:200])
    check("错误信息提示如何调整", "DEXCODE_MAX_MEM_MB" in err, err[:200])

    # 预算足够时不受影响
    src = 'let s = ""; let i = 0; while i < 100 { s = s + "x"; i = i + 1; } print "ok";'
    out, err, rc = run_c(src, {"DEXCODE_MAX_MEM_MB": "64"})
    check("预算充足时不受影响", rc == 0 and "ok" in out, f"rc={rc} {err[:120]}")


# ============================================================
# P0:统一堆入口(重构不变量)
# ============================================================

def test_allocator_is_single_entry():
    print("P0 统一堆入口")
    src = open(os.path.join(ROOT, "vm", "vm.c"), encoding="utf-8").read()
    # VM 的堆分配应经 vm_alloc / vm_calloc;仅允许分配器内部与成对使用的非预算堆
    # (临时库路径、文件读取缓冲)出现裸调用。
    # 允许的裸调用(有意不走预算堆,且 malloc/free 成对):
    #   vm_alloc 内部实现、临时库路径、字节码文件读取缓冲
    ALLOWED_SNIPPETS = (
        "sizeof(size_t) + n",                  # vm_alloc 自身
        'char *path = malloc(n);',              # create_temp_lib 的临时路径
        "uint8_t *data = malloc(n > 0 ?",       # 读取 .dexbc 的缓冲
    )
    offenders = []
    in_block_comment = False
    for i, line in enumerate(src.splitlines(), 1):
        s = line.strip()
        # 跳过注释(块注释内的行也含 "calloc(" 这类字样,不是代码)
        if in_block_comment:
            if "*/" in s:
                in_block_comment = False
            continue
        if s.startswith("/*"):
            if "*/" not in s:
                in_block_comment = True
            continue
        if s.startswith("*") or s.startswith("//"):
            continue
        for fn in ("malloc(", "calloc("):
            if fn in s and "vm_alloc" not in s and "vm_calloc" not in s:
                if any(a in s for a in ALLOWED_SNIPPETS):
                    continue
                offenders.append((i, s))
    check("VM 堆分配已收口到 vm_alloc/vm_calloc",
          not offenders, f"仍有 {len(offenders)} 处: {offenders[:4]}")
    # 预算相关符号必须存在
    for sym in ("vm_alloc", "vm_calloc", "vm_free", "DEXCODE_MAX_MEM_MB"):
        check(f"存在 {sym}", sym in src)


# ============================================================
# P2:FFI 字符串所有权
# ============================================================

SOLO = """
include "../fixtures/leaky";
print leaky_greet("solo");
print leaky_live();
print leaky_owned_frees();
print leaky_foreign_frees();
"""


def test_ffi_release_declared():
    print("P2 声明了释放函数:原生缓冲区被释放")
    if not has_vm() or not has_fixture():
        print("  SKIP  (需要 C VM 与 tests/fixtures/libleaky.dll)")
        return
    out, err, rc = run_c(SOLO)
    lines = out.strip().splitlines()
    check("调用成功且返回值正确", rc == 0 and lines[:1] == ["hello, solo"],
          f"rc={rc} out={out!r} err={err[:160]}")
    if len(lines) >= 4:
        check("原生缓冲区已释放(存活块=0)", lines[1] == "0", f"live={lines[1]}")
        check("确实调用了一次释放函数", lines[2] == "1", f"owned_frees={lines[2]}")
        check("没有误释放借来的指针", lines[3] == "0", f"foreign_frees={lines[3]}")
    else:
        check("输出行数足够", False, repr(out))


def test_ffi_release_many_calls():
    print("P2 循环内反复调用:不累积泄漏")
    if not has_vm() or not has_fixture():
        print("  SKIP  (需要 C VM 与 tests/fixtures/libleaky.dll)")
        return
    src = """
include "../fixtures/leaky";
let i = 0;
while i < 500 { let s = leaky_greet("world"); i = i + 1; }
print leaky_live();
print leaky_owned_frees();
"""
    out, err, rc = run_c(src)
    lines = out.strip().splitlines()
    check("500 次调用后存活块为 0", rc == 0 and lines[:1] == ["0"],
          f"rc={rc} out={out!r} err={err[:160]}")
    if len(lines) >= 2:
        check("500 次都被释放", lines[1] == "500", f"owned_frees={lines[1]}")


def test_ffi_release_absent():
    """对照:未声明释放函数时不释放 —— 证明释放机制确实必要。

    注意:这不产生编译警告。默认契约是"原生返回的字符串是借用,归原生库所有",
    未声明释放函数即表示该库返回静态/常驻缓冲区(现有六个库皆如此),
    因此给每个项目都报一条警告只会污染 IDE 问题面板,得不偿失。"""
    print("P2 未声明释放函数:不释放(对照)")
    src = """
include "../fixtures/leaky_norel";
print leaky_greet("x");
print leaky_live();
"""
    unit = compile_src(src)
    check("未设置释放函数(库表 release_name 为空)",
          all(not l.release_name for l in unit.libs), str([l.release_name for l in unit.libs]))
    if has_vm() and has_fixture():
        out, err, rc = run_c(src)
        # 输出:第 1 行是 greet 的返回值,第 2 行是 leaky_live() 的存活块数
        lines = out.strip().splitlines()
        check("对照:确实泄漏(存活块=1)",
              rc == 0 and len(lines) == 2 and lines[1] == "1",
              f"rc={rc} out={out!r}")
        check("对照:返回值仍然正确", lines[:1] == ["hello, x"], repr(out))


def test_release_roundtrip():
    print("P2 字节码 DXRL trailer 与往返一致性")
    unit = compile_src('include "../fixtures/leaky";\nprint leaky_greet("x");\n')
    bc = assemble(unit.to_program())
    check("字节码含 DXRL trailer", b"DXRL" in bc, str(bc[-24:]))
    prog = decode(bc)
    rels = [l.release_name for l in prog.libs]
    check("解码后能读回释放函数名", "leaky_free" in rels, str(rels))
    check("字节码往返逐字节一致", assemble(prog) == bc)

    # 无释放函数的字节码不得带 trailer,且往返一致
    plain = assemble(compile_src("print 1;").to_program())
    check("无释放函数时不写 trailer", b"DXRL" not in plain)
    check("普通字节码往返一致", assemble(decode(plain)) == plain)


def test_release_signature_detection():
    print("P2 释放函数的识别规则")
    good = compile_src('include "../fixtures/leaky";\nprint leaky_greet("a");\n')
    check("(string)->void 被认作释放函数",
          good.lib_release.get(next(iter(good.lib_release), "")) == "leaky_free"
          if good.lib_release else False, str(good.lib_release))
    none = compile_src('include "../fixtures/leaky_norel";\nprint leaky_greet("a");\n')
    check("无 (string)->void 时不设释放函数", not none.lib_release, str(none.lib_release))


if __name__ == "__main__":
    print("=== P0 统一堆入口 ===")
    test_allocator_is_single_entry()
    print("=== P1 内存预算 ===")
    test_budget()
    print("=== P2 FFI 所有权 ===")
    test_ffi_release_declared()
    test_ffi_release_many_calls()
    test_ffi_release_absent()
    test_release_roundtrip()
    test_release_signature_detection()
    import shutil
    shutil.rmtree(TMPDIR, ignore_errors=True)
    print()
    print(f"结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
