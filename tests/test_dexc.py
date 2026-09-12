#!/usr/bin/env python3
"""DEXCODE 纯 C 工具链(dexc.exe)与 Python 前端的**逐字节一致性**测试。

为什么这么测:dexc 存在的意义就是"把编译这一步从 Python 手里拿走",所以它唯一
可接受的正确性标准是 —— 同一份源码,两边产出**完全相同**的:
  ① .dexbc 字节  ② .dxasm 文本  ③ 错误信息  ④ 警告
只要有一条不同,IDE 与命令行就会对同一份工程给出不同的结果,那是比"编译失败"
更糟的状态。因此这里不做"看起来对"的抽查,而是拿全仓库的 .dex 语料 + 一批
故意写错的源码逐条对照。

dexc.exe 未构建时跳过 C 侧对照(与 test_dexgame.py 等对二进制的处理一致):
    python main.py build-dexc

运行: python tests/test_dexc.py
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _tmpdir import tempdir  # noqa: E402

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, render, disassemble, decode,
    parse_asm_text, DexError,
)

DEXC = os.path.join(ROOT, "tools", "dexc", "dexc.exe" if os.name == "nt" else "dexc")
LIBS = os.path.join(ROOT, "libs")

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


# --------------------------------------------------------------- 两侧编译入口

def py_compile(path, include_dirs=None):
    """返回 (bc_bytes, asm_text, warnings, error_text_or_None)。"""
    try:
        with open(path, encoding="utf-8-sig") as f:
            src = f.read()
        toks = Lexer(src, path).tokenize()
        ast = Parser(toks, path).parse_program()
        unit = compile_program(ast, source_path=path,
                               include_dirs=include_dirs or [LIBS])
        prog = unit.to_program()
        return assemble(prog), render(prog), list(unit.warnings), None
    except DexError as e:
        return None, None, [], str(e)


def c_run(args):
    return subprocess.run([DEXC] + args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", cwd=ROOT)


def c_compile(path, include_dirs=None, out_bc=None, no_asm=False):
    """返回 (bc_bytes, asm_text, warnings, error_text_or_None)。"""
    args = ["compile", path, "-L", (include_dirs or [LIBS])[0]]
    for d in (include_dirs or [LIBS])[1:]:
        args += ["-L", d]
    if out_bc:
        args += ["-o", out_bc]
    if no_asm:
        args += ["--no-asm"]
    r = c_run(args)
    asm_path = os.path.splitext(os.path.abspath(path))[0] + ".dxasm"
    bc_path = out_bc or (os.path.splitext(os.path.abspath(path))[0] + ".dexbc")
    bc = open(bc_path, "rb").read() if os.path.exists(bc_path) else None
    asm = None
    if not no_asm and os.path.exists(asm_path):
        with open(asm_path, encoding="utf-8") as f:
            asm = f.read()
    warns = []
    err = None
    errlines = []
    for line in (r.stderr or "").splitlines():
        if line.startswith("warning: "):
            warns.append(line[len("warning: "):])
        elif line.strip():
            errlines.append(line)
    if errlines:
        # 多行错误(如"递归类型"会附一行解释)必须整段保留,否则会误判
        err = "\n".join(errlines)
    # 清理生成产物(都在 .gitignore 里,但不必留在树里)
    for q in (asm_path, bc_path):
        if q and os.path.exists(q):
            os.remove(q)
    if r.returncode != 0 and bc is None:
        return None, None, warns, err or ("rc=%d" % r.returncode)
    return bc, asm, warns, err


def compare_file(path, tag=None):
    """同一个文件两侧编译,比较字节码/汇编/警告/错误。

    "都编译失败"也算一致 —— 判定失败原因的是最后那条"错误一致"。"""
    tag = tag or os.path.relpath(path, ROOT)
    py_bc, py_asm, py_warn, py_err = py_compile(path)
    c_bc, c_asm, c_warn, c_err = c_compile(path)
    check(f"{tag} 字节码一致", py_bc == c_bc,
          f"py={None if py_bc is None else len(py_bc)}B "
          f"dexc={None if c_bc is None else len(c_bc)}B")
    check(f"{tag} 汇编文本一致", py_asm == c_asm, _first_diff(py_asm, c_asm))
    check(f"{tag} 警告一致", py_warn == c_warn, f"py={py_warn} dexc={c_warn}")
    check(f"{tag} 错误一致", py_err == c_err, f"py={py_err!r} dexc={c_err!r}")


def _first_diff(a, b):
    if a is None or b is None:
        return f"py={None if a is None else 'ok'} dexc={None if b is None else 'ok'}"
    la, lb = a.splitlines(), b.splitlines()
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else "<none>"
        y = lb[i] if i < len(lb) else "<none>"
        if x != y:
            return f"line {i+1}: py={x!r} dexc={y!r}"
    return ""


def compare_source(src, name, include_dirs=None, tmp=None):
    """把源码写进临时目录再两侧对照(用于故意写错的用例)。

    错误用例两边都编译失败(都是 None),所以"字节码一致"只比较相等性;
    真正判定失败原因的是下面的"错误一致"。"""
    d = tmp or tempdir("dexc_")
    path = os.path.join(d, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    py_bc, py_asm, py_warn, py_err = py_compile(path, include_dirs)
    c_bc, c_asm, c_warn, c_err = c_compile(path, include_dirs)
    check(f"{name} 字节码一致", py_bc == c_bc,
          f"py={None if py_bc is None else len(py_bc)}B "
          f"dexc={None if c_bc is None else len(c_bc)}B py_err={py_err} c_err={c_err}")
    check(f"{name} 汇编文本一致", py_asm == c_asm, _first_diff(py_asm, c_asm))
    check(f"{name} 警告一致", py_warn == c_warn, f"py={py_warn} dexc={c_warn}")
    check(f"{name} 错误一致", py_err == c_err, f"py={py_err!r} dexc={c_err!r}")
    return py_bc, py_err


# ------------------------------------------------------------------ 语料

# 只拿"仓库里跟踪的 .dex":工作区里还有一批 gitignore 掉的调试文件
# (_at.dex、scene_out.dex …)与用户数据目录,它们不代表语言能力,却会让
# 测试结果随本机残留文件漂移。
_GENERATED = {"scene_out.dex", "snow_out.dex", "scene_test.dex"}


def find_corpus():
    try:
        r = subprocess.run(["git", "ls-files", "*.dex"], capture_output=True,
                           text=True, encoding="utf-8", errors="replace", cwd=ROOT)
        if r.returncode == 0 and (r.stdout or "").strip():
            return [os.path.join(ROOT, ln.strip().replace("/", os.sep))
                    for ln in r.stdout.splitlines() if ln.strip()]
    except OSError:
        pass
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs
                   if d not in (".git", ".venv", "_zigcache", "_zigtmp", "__pycache__",
                                "build", "build312", "DEXCODE-发布", "_t", "_zt")]
        for fn in files:
            if fn.endswith(".dex") and not fn.startswith("_") and fn not in _GENERATED:
                out.append(os.path.join(base, fn))
    return sorted(out)


# ------------------------------------------------------------------ 测试函数

def test_corpus():
    print("[全仓库 .dex 语料]")
    files = find_corpus()
    check("语料非空", len(files) >= 5, files)
    for p in files:
        compare_file(p)


def test_language_features():
    print("[语言特性逐项对照]")
    with tempdir("dexc_") as tmp:
        cases = [
            ("empty.dex", ""),
            ("let.dex", "let a = 1;\nlet b = a + 2;\nprint b;\n"),
            ("float.dex", "print 0.5;\nprint 1.0 / 3.0;\nprint 1e16;\n"
                          "print 1e-5;\nprint 2.5e3;\nprint 0.008333333;\n"),
            ("neg.dex", "print -1;\nprint - 2.5;\nprint !0;\nprint !(1 == 2);\n"),
            ("hex.dex", "print 0xFF;\nprint 0X10;\nprint 0xff + 1;\n"),
            ("str_esc.dex", 'print "a\\nb";\nprint "tab\\there";\n'
                            'print "quote\\"x";\nprint "back\\\\slash";\n'),
            ("concat.dex", 'let s = "n=" + 1 + 2.5;\nprint s;\n'
                           'print "a" + "b" + "c";\n'),
            ("prec.dex", "print 1 + 2 * 3 - 4 / 2 % 3;\n"
                         "print (1 + 2) * 3;\nprint 1 < 2 && 3 >= 3 || !1;\n"),
            ("ifelse.dex", "let x = 2;\nif x == 1 { print 1; } "
                           "else if x == 2 { print 2; } else { print 3; }\n"),
            ("empty_else.dex", "if 1 { print 1; } else { }\n"),
            ("while.dex", "let i = 0;\nwhile i < 3 { print i; i = i + 1; }\n"),
            ("break_none.dex", "let i = 0;\nwhile i < 5 {\n  if i == 3 { i = 5; } "
                               "else { i = i + 1; }\n}\nprint i;\n"),
            ("func.dex", "func add(a: int, b: int) -> int { return a + b; }\n"
                         "func noval() { print 1; }\n"
                         "func mk() { return \"s\"; }\n"
                         "print add(1, 2);\nnoval();\nprint mk() + mk();\n"),
            ("recursion.dex", "func fib(n: int) -> int {\n"
                              "  if n < 2 { return n; }\n"
                              "  return fib(n - 1) + fib(n - 2);\n}\nprint fib(10);\n"),
            ("early_return.dex", "func f(n: int) -> int {\n  return n;\n  print n;\n}\n"
                                 "print f(1);\n"),
            ("type.dex", "type P { x: int; y: float; s: string; b: bool; }\n"
                         "let p = P{ x: 1, y: 2.5, s: \"h\", b: 1 };\n"
                         "print p.x;\nprint p.y;\nprint p.s;\nprint p.b;\n"
                         "p.x = 9;\nprint p.x;\n"),
            ("type_default.dex", "type Q { a: int; b: float; c: string; }\n"
                                 "let q = Q{};\nprint q.a;\nprint q.b;\nprint q.c;\n"),
            ("type_nested.dex", "type A { v: int; }\ntype B { a: A; k: int; }\n"
                                "let b = B{ a: A{ v: 3 }, k: 4 };\nprint b.a.v;\n"),
            ("getfield_chain.dex", "type A { v: int; }\ntype B { a: A; }\n"
                                   "let b = B{ a: A{ v: 7 } };\n"
                                   "print b.a.v;\nb.a.v = 8;\nprint b.a.v;\n"),
            ("call_dyn.dex", "func f(a: int) -> int { return a * 2; }\n"
                             "print call(\"f\", 21);\n"),
            ("unreachable.dex", "func f() -> int { return 1; print 2; print 3; }\n"
                                "print f();\n"),
            ("comment.dex", "# 行注释\nlet x = 1; // 也是注释\n"
                            "/* 块\n注释 */\nprint x;\n"),
            ("cjk.dex", "let 玩家 = 1;\nlet 金币 = 玩家 + 2;\nprint 金币;\n"
                        "print \"中文\";\n"),
            ("cjk_str.dex", 'print "你好,世界";\nprint "emoji: ok";\n'),
            ("deep_expr.dex", "print ((((1 + 2) * (3 - 4)) + (5 * 6)) - ((7 % 2) + 8));\n"),
            ("many_locals.dex", "".join(f"let v{i} = {i};\n" for i in range(40))
                                + "print v39;\n"),
            ("many_consts.dex", "".join(f'print "s{i}";\n' for i in range(40))),
            ("lib_math.dex", 'include "math";\nprint dex_add(1, 2);\n'),
            ("lib_std.dex", 'include "std";\nprint dex_len("abc");\n'),
            ("lib_dexgame.dex", 'include "dexgame";\nprint eng_rgba(1, 2, 3, 4);\n'
                                'print eng_audio_ok();\n'),
            ("lib_static.dex", 'include "dexgame_static";\n'
                               'print eng_rgba(1, 2, 3, 4);\n'),
            ("module_fast.dex", 'include "dexgame";\ninclude "dexgame_fast";\n'
                                'func on_start() { print 1; }\n'
                                'func on_update() { print 2; }\n'
                                'func on_draw() { print 3; }\n'
                                'eng_run_frames("on_start", "on_update", "on_draw", 2);\n'),
            ("refer_path.dex", 'refer "libs/math/math.dexdef";\nprint dex_add(2, 3);\n'),
        ]
        for name, src in cases:
            compare_source(src, name, tmp=tmp)


def test_errors():
    print("[错误路径:两边必须给出同一条消息]")
    with tempdir("dexc_") as tmp:
        cases = [
            ("e_semi.dex", "let x = 1\n"),
            ("e_paren.dex", "print (1 + 2;\n"),
            ("e_brace.dex", "if 1 { print 1;\n"),
            ("e_unterm_str.dex", 'print "abc;\n'),
            ("e_unterm_comment.dex", "/* abc\nprint 1;\n"),
            ("e_bad_char.dex", "print 1 & 2;\n"),
            ("e_undef_var.dex", "print nope;\n"),
            ("e_undef_fn.dex", "nope();\n"),
            ("e_dup_var.dex", "let x = 1;\nlet x = 2;\n"),
            ("e_dup_fn.dex", "func f() { }\nfunc f() { }\n"),
            ("e_dup_main.dex", "func main() { }\n"),
            ("e_dup_type.dex", "type T { a: int; }\ntype T { b: int; }\n"),
            ("e_recursive.dex", "type A { b: B; }\ntype B { a: A; }\n"),
            ("e_self_type.dex", "type N { self: N; }\n"),
            ("e_unknown_type.dex", "let x = T{ a: 1 };\n"),
            ("e_bad_field.dex", "type T { a: int; }\nlet t = T{ a: 1 };\nprint t.b;\n"),
            ("e_setfield.dex", "type T { a: int; }\nlet t = T{ a: 1 };\n"
                               "print t.a.b;\n"),
            ("e_assign_type.dex", "type T { a: int; s: string; }\n"
                                  "let t = T{ a: 1, s: \"x\" };\nt.a = \"str\";\n"),
            ("e_argc.dex", "func f(a: int) { }\nf(1, 2);\n"),
            ("e_top_return.dex", "return 1;\n"),
            ("e_top_include.dex", "if 1 { include \"math\"; }\n"),
            ("e_top_type.dex", "if 1 { type T { a: int; } }\n"),
            ("e_field_default.dex", "type A { v: int; }\ntype B { a: A; }\n"
                                    "let b = B{};\n"),
            ("e_div0.dex", "print 1 / 0;\n"),
            ("e_mod0.dex", "print 1 % 0;\n"),
            ("e_div0f.dex", "print 1.0 / 0.0;\n"),
            ("e_include_missing.dex", 'include "nosuchlib";\n'),
            ("e_refer_missing.dex", 'refer "nosuch.dexdef";\n'),
            ("e_bad_native_arity.dex", 'include "math";\nprint dex_add(1);\n'),
            ("e_bad_native_type.dex", 'include "math";\nprint dex_add("a", 1);\n'),
            ("e_bad_native_type2.dex", 'include "math";\nprint dex_add(1.5, 1);\n'),
            ("e_float_to_int.dex", 'include "math";\nprint dex_add(1.5, 2);\n'),
            ("e_unexpected.dex", "print ;\n"),
            ("e_bad_expr.dex", "let x = *;\n"),
            ("e_type_nofields.dex", "type T { }\nlet t = T{};\nprint 1;\n"),
        ]
        for name, src in cases:
            compare_source(src, name, tmp=tmp)


def test_error_messages_are_meaningful():
    print("[错误信息本身可读(不只是两边一致)]")
    with tempdir("dexc_") as tmp:
        p = os.path.join(tmp, "m.dex")
        with open(p, "w", encoding="utf-8") as f:
            f.write("print nope;\n")
        r = c_run(["compile", p, "-L", LIBS])
        check("未定义变量带行号与名字",
              "undefined variable 'nope'" in (r.stderr or "")
              and "1:7" in (r.stderr or ""), r.stderr)
        p2 = os.path.join(tmp, "m2.dex")
        with open(p2, "w", encoding="utf-8") as f:
            f.write('include "nosuchlib";\n')
        r = c_run(["compile", p2, "-L", LIBS])
        check("找不到库给出原因", "cannot find library 'nosuchlib'" in (r.stderr or ""),
              r.stderr)
        check("错误也走退出码 1", r.returncode == 1, r.returncode)


def test_asm_disasm_roundtrip():
    print("[反汇编 / 汇编往返]")
    for p in find_corpus()[:6] + [os.path.join(ROOT, "examples", "dexgame",
                                               "demo_m1.dex")]:
        if not os.path.exists(p):
            continue
        py_bc, _, _, err = py_compile(p)
        if py_bc is None:
            continue
        tag = os.path.relpath(p, ROOT)
        with tempdir("dexc_rt_") as tmp:
            bc_path = os.path.join(tmp, "t.dexbc")
            with open(bc_path, "wb") as f:
                f.write(py_bc)
            r = c_run(["disasm", bc_path, "-o", os.path.join(tmp, "t.dxasm")])
            check(f"{tag} dexc disasm 成功", r.returncode == 0, r.stderr)
            if r.returncode != 0 or not os.path.exists(os.path.join(tmp, "t.dxasm")):
                continue
            c_text = open(os.path.join(tmp, "t.dxasm"), encoding="utf-8").read()
            check(f"{tag} 反汇编文本与 Python 一致", c_text == disassemble(py_bc),
                  _first_diff(disassemble(py_bc), c_text))
            r = c_run(["asm", os.path.join(tmp, "t.dxasm"),
                       "-o", os.path.join(tmp, "t2.dexbc")])
            check(f"{tag} dexc asm 成功", r.returncode == 0, r.stderr)
            if r.returncode != 0 or not os.path.exists(os.path.join(tmp, "t2.dexbc")):
                continue
            bc2 = open(os.path.join(tmp, "t2.dexbc"), "rb").read()
            check(f"{tag} 往返后字节码一致", bc2 == py_bc,
                  f"{len(bc2)}B vs {len(py_bc)}B")


def test_decode_matches():
    print("[decode() 结构一致]")
    p = os.path.join(ROOT, "examples", "dexgame", "demo_m3.dex")
    if not os.path.exists(p):
        skip("decode 对照", "示例不存在")
        return
    py_bc, _, _, err = py_compile(p)
    check("demo_m3 可编译", py_bc is not None, err)
    if py_bc is None:
        return
    prog = decode(py_bc)
    check("函数表非空", len(prog.funcs) >= 1, len(prog.funcs))
    check("原生函数数 > 10", len(prog.natives) > 10, len(prog.natives))
    check("库表非空", len(prog.libs) >= 1, len(prog.libs))
    check("main 存在", any(f.name == "main" for f in prog.funcs),
          [f.name for f in prog.funcs])


def test_cli_surface():
    print("[命令行表面]")
    r = c_run(["version"])
    check("version 可用", r.returncode == 0 and "dexc" in (r.stdout or ""), r.stdout)
    r = c_run([])
    check("无参数给出用法", r.returncode == 2 and "用法" in (r.stderr or ""), r.stderr)
    r = c_run(["nosuchcmd"])
    check("未知子命令报错", r.returncode == 2, r.stderr)
    r = c_run(["compile"])
    check("compile 缺参数报错", r.returncode == 2, r.stderr)
    r = c_run(["compile", os.path.join(ROOT, "no_such_file.dex")])
    check("源文件不存在报错", r.returncode == 1, r.stderr)


def test_run_command():
    print("[dexc run:编译并交给 vm.exe]")
    vm = os.path.join(ROOT, "vm", "vm.exe")
    if not os.path.exists(vm):
        skip("dexc run", "vm.exe 未构建")
        return
    with tempdir("dexc_run_") as tmp:
        p = os.path.join(tmp, "hello.dex")
        with open(p, "w", encoding="utf-8") as f:
            f.write('print "hello from dexc";\nprint 40 + 2;\n')
        r = subprocess.run([DEXC, "run", p, "-L", LIBS], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", cwd=ROOT)
        out = (r.stdout or "").replace("\r\n", "\n")
        check("run 输出正确", "hello from dexc" in out and "42" in out, out)
        check("run 退出码 0", r.returncode == 0, r.returncode)


def test_dexdef_direct():
    print("[.dexdef 解析对照]")
    with tempdir("dexc_def_") as tmp:
        defs = {
            "ok.dexdef": 'refer "libdexmath.dll";\n'
                         'extern func dex_add(a: int, b: int) -> int;\n',
            "no_refer.dexdef": 'extern func f() -> void;\n',
            "dup_refer.dexdef": 'refer "a.dll";\nrefer "b.dll";\n',
            "bad_type.dexdef": 'refer "a.dll";\nextern func f(x: bad) -> int;\n',
            "bad_ret.dexdef": 'refer "a.dll";\nextern func f() -> bad;\n',
            "abi_twice.dexdef": 'refer "a.dll";\nabi direct;\nabi value_array;\n',
            "abi_bad.dexdef": 'refer "a.dll";\nabi weird;\n',
            "dup_release.dexdef": 'refer "a.dll";\nrelease f;\nrelease g;\n',
            "too_many.dexdef": 'refer "a.dll";\n'
                               'extern func f(a: int, b: int, c: int, d: int) -> int;\n',
            "value_array.dexdef": 'abi value_array;\nrefer "a.dll";\n'
                                  'extern func wide(a: int, b: int, c: int, d: int, '
                                  'e: int) -> int;\n',
            "no_params.dexdef": 'refer "a.dll";\nextern func f() -> void;\n',
            "weird.dexdef": 'refer "a.dll";\nextern func f(a: void) -> void;\n',
            "trailing.dexdef": 'refer "a.dll";\nxyz;\n',
        }
        for name, text in defs.items():
            with open(os.path.join(tmp, name), "w", encoding="utf-8") as f:
                f.write(text)
            # 两边都走"编译一个 refer 该定义文件的源码"这条真实路径:
            # 缺 refer、arity 上限、命名冲突等检查都在编译器里,单独 parse_def 比不出来。
            srcname = name.replace(".dexdef", ".dex")
            src = os.path.join(tmp, srcname)
            with open(src, "w", encoding="utf-8") as f:
                f.write('refer "%s";\nprint 1;\n' % name)
            py_bc, _, _, py_err = py_compile(src)
            r = c_run(["compile", src, "-o", os.path.join(tmp, "o.dexbc")])
            c_ok = r.returncode == 0
            check(f"{name} 编译结果一致", (py_err is None) == c_ok,
                  f"py_err={py_err!r} dexc_rc={r.returncode} {r.stderr}")
            if py_err is not None:
                check(f"{name} 错误信息一致", py_err in (r.stderr or ""),
                      f"py={py_err!r} dexc={r.stderr!r}")


def test_float_repr():
    print("[浮点文本:repr 规则必须与 Python 一致]")
    from dexlang import opcodes as O  # noqa: F401
    with tempdir("dexc_f_") as tmp:
        vals = ["0.0", "-0.0", "1.0", "0.1", "0.5", "100.0", "1e15", "1e16",
                "1e-4", "1e-5", "3.14159265358979", "2.5e3", "0.008333333",
                "1.7976931348623157e308", "5e-324", "123456.789"]
        src = "".join(f"print {v};\n" for v in vals)
        compare_source(src, "floats.dex", tmp=tmp)


def main():
    print("DEXCODE dexc 一致性测试(纯 C 工具链 vs Python 前端)")
    if not os.path.exists(DEXC):
        skip("全部 C 侧对照", "dexc.exe 未构建,先跑 python main.py build-dexc")
        print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
        return 0 if FAIL == 0 else 1
    print(f"dexc: {DEXC}")
    test_corpus()
    test_language_features()
    test_errors()
    test_error_messages_are_meaningful()
    test_float_repr()
    test_asm_disasm_roundtrip()
    test_decode_matches()
    test_cli_surface()
    test_run_command()
    test_dexdef_direct()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败" + (f", {SKIP} 跳过" if SKIP else ""))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
