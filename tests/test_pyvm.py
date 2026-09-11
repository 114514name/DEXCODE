#!/usr/bin/env python3
"""Python 调试 VM(pyvm)测试。

运行: python tests/test_pyvm.py
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program  # noqa: E402
from dexlang.pyvm import PyVM, run_program  # noqa: E402

LIBS = os.path.join(ROOT, "libs")
DLL_PATH = os.path.join(LIBS, "math", "libdexmath.dll")

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


def make_prog(src, path):
    toks = Lexer(src, path).tokenize()
    ast = Parser(toks, path).parse_program()
    return compile_program(ast, source_path=path, include_dirs=[LIBS]).to_program()


FIB = (
    "func fib(n) {\n"
    "    if n < 2 {\n"
    "        return n;\n"
    "    }\n"
    "    return fib(n - 1) + fib(n - 2);\n"
    "}\n"
    "let i = 0;\n"
    "while i <= 10 {\n"
    "    print fib(i);\n"
    "    i = i + 1;\n"
    "}\n"
)


def test_run():
    print("[run]")
    out, err = run_program(make_prog(FIB, "<fib>"))
    check("fib 输出与 C VM 一致",
          err is None and out == ["0", "1", "1", "2", "3", "5", "8", "13", "21", "34", "55"],
          repr(out))

    out, err = run_program(make_prog("print 7 / 2; print 10 % 3; print 2.5 * 4;", "<t>"))
    check("算术", err is None and out == ["3.5", "1", "10"], repr(out))

    out, err = run_program(make_prog("let z = 0; print 5 / z;", "<t>"))
    check("运行期除零错误", err is not None and "division by zero" in err, repr(err))

    cn = "你好,世界!!"
    out, err = run_program(make_prog(f'print "{cn}";', "<cn>"))
    check("中文输出不乱码", err is None and out == [cn], repr(out))


def test_breakpoints():
    print("[breakpoints]")
    prog = make_prog(FIB, "<fib>")
    vm = PyVM(prog)
    vm.breakpoints = {8}   # 第 8 行: print fib(i);
    vm.run()
    stopped = vm.run_until_stop()
    check("断点命中", stopped and vm.current_line() == 8, f"line={vm.current_line()}")
    check("停因", vm._stop_reason == "breakpoint", vm._stop_reason)

    # 继续应停在下一个断点命中(跳过同一行)
    vm.run()
    stopped = vm.run_until_stop()
    check("继续后再次命中", stopped and vm.current_line() == 8, f"line={vm.current_line()}")

    # 不设断点则跑完
    vm2 = PyVM(prog)
    vm2.run()
    vm2.run_until_stop()
    check("无断点跑完", vm2.done and vm2.error is None)


def test_stepping():
    print("[stepping]")
    prog = make_prog(
        "func f(x) {\n"
        "    let y = x + 1;\n"
        "    return y * 2;\n"
        "}\n"
        "let r = f(3);\n"
        "print r;\n", "<t>")
    vm = PyVM(prog)
    vm.breakpoints = {2}   # 函数 f 内部
    vm.run()
    vm.run_until_stop()
    check("停在 f 内", vm.current_line() == 2 and vm.current_func().name == "f",
          f"line={vm.current_line()} func={vm.current_func().name}")

    # step_into 若干步,应离开第 2 行
    lines = []
    for _ in range(8):
        vm.step_into()
        vm.run_until_stop()
        lines.append(vm.current_line())
    check("step_into 推进多行", len(set(lines)) >= 2, repr(lines))

    # 调用栈:断点打在被调函数内
    vm2 = PyVM(prog)
    vm2.breakpoints = {2}
    vm2.run()
    vm2.run_until_stop()
    info = vm2.frame_info()
    check("调用栈含 f 与 main", info[0]["func"] == "f" and len(info) >= 2,
          [(f["func"], f["line"]) for f in info])
    check("帧局部变量", "%0" in info[0]["locals"] and "%1" in info[0]["locals"],
          repr(info[0]["locals"]))

    # step_out 返回调用者
    depth0 = vm2.depth
    vm2.step_out()
    vm2.run_until_stop()
    check("step_out 回到调用者", vm2.depth < depth0, f"depth {vm2.depth} < {depth0}")


def test_native():
    print("[native via ctypes]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 libdexmath.dll)")
        return
    src = 'include "math";\n' + \
          "print dex_add(2, 3);\nprint dex_fact(5);\nprint dex_hello();\n"
    out, err = run_program(make_prog(src, os.path.join(LIBS, "_t.dex")))
    check("原生调用输出", err is None and out == ["5", "120", "hello from dll"], repr(out) + repr(err))


def main():
    print("pyvm 调试虚拟机测试")
    test_run()
    test_breakpoints()
    test_stepping()
    test_native()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
