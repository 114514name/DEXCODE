#!/usr/bin/env python3
"""DEXCODE 无 Python 依赖 + 静态编译验证。

运行: python tests/test_nopydep.py
证明:
  1) EGUI 程序可静态编译(DLL 字节内嵌进 .dexbc)
  2) 移走 DLL 后仍能用 vm.exe 独立运行(无需 Python 解释器)
  3) fast+static 模式(egui_fast_static)也能运行
  4) 临时解包文件会被清理
"""

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program, assemble  # noqa: E402

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


VM = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
EGUI_DLL = os.path.join(ROOT, "libs", "egui", "libegui.dll")


def compile_src(src):
    return compile_program(
        Parser(Lexer(src, "<t>").tokenize(), "<t>").parse_program(),
        source_path=None, include_dirs=[os.path.join(ROOT, "libs")])


def run_vm(bc, timeout=30):
    tmp = os.path.join(ROOT, "_tmp_nopydep.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    try:
        r = subprocess.run([VM, tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return r.stdout, r.stderr, r.returncode
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


STATIC_SRC = """
include "egui_static";
let win = eg_window("NoPy Static", 220, 140);
print win > 0;
eg_add("label", win, 1);
eg_set_pos(1, 20, 20);
eg_set_text(1, "静态无 DLL");
print eg_get_text(1);
eg_quit();
print eg_running();
"""

FAST_STATIC_SRC = """
include "egui_fast_static";
let win = eg_window("NoPy Fast", 220, 140);
print win > 0;
eg_add("label", win, 1);
eg_set_text(1, "fast 静态");
print eg_get_text(1);
eg_quit();
print eg_running();
"""


def temp_count():
    try:
        return len([f for f in os.listdir(tempfile.gettempdir())
                    if f.startswith("dexvm_") and f.endswith(".dll")])
    except OSError:
        return 0


def test_static_without_dll():
    print("静态编译 + 移走 DLL 后运行(仅 vm.exe)")
    if not os.path.exists(EGUI_DLL):
        check("EGUI DLL 存在", False, "找不到 libegui.dll")
        return
    unit = compile_src(STATIC_SRC)
    bc = assemble(unit.to_program())
    # 移走 DLL
    backup = EGUI_DLL + ".bak"
    os.rename(EGUI_DLL, backup)
    try:
        out, err, code = run_vm(bc)
        check("退出码 0(无 DLL)", code == 0, f"code={code} err={err}")
        check("输出正确", out.strip().splitlines() == ["1", "静态无 DLL", "0"],
              f"got {out!r}")
    finally:
        os.rename(backup, EGUI_DLL)
    check("DLL 已恢复", os.path.exists(EGUI_DLL))


def test_fast_static_without_dll():
    print("fast+static 模式无 DLL 运行")
    if not os.path.exists(EGUI_DLL):
        check("EGUI DLL 存在", False)
        return
    unit = compile_src(FAST_STATIC_SRC)
    bc = assemble(unit.to_program())
    backup = EGUI_DLL + ".bak"
    os.rename(EGUI_DLL, backup)
    try:
        out, err, code = run_vm(bc)
        check("退出码 0", code == 0, f"code={code} err={err}")
        check("输出正确", out.strip().splitlines() == ["1", "fast 静态", "0"],
              f"got {out!r}")
    finally:
        os.rename(backup, EGUI_DLL)


def test_temp_cleaned():
    print("临时解包 DLL 清理")
    before = temp_count()
    unit = compile_src(STATIC_SRC)
    run_vm(assemble(unit.to_program()))
    after = temp_count()
    check("运行后无新增临时 DLL", after <= before, f"{before} -> {after}")


def test_vm_only_no_python():
    print("字节码可脱离 Python 运行(子进程独立)")
    # 该程序不含任何 Python:直接子进程运行 vm.exe 即证明
    unit = compile_src('include "std";\nprint dex_add(2, 3);\n'
                       if False else 'include "math";\nprint dex_add(2, 3);\n')
    out, err, code = run_vm(assemble(unit.to_program()))
    check("纯 VM 运行", code == 0 and out.strip() == "5",
          f"code={code} out={out!r} err={err}")


if __name__ == "__main__":
    test_static_without_dll()
    test_fast_static_without_dll()
    test_temp_cleaned()
    test_vm_only_no_python()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
