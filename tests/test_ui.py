#!/usr/bin/env python3
"""DEXCODE WinAPI UI 库测试(libdexui.dll,纯 C 实现)。

覆盖:定义文件解析、10 个函数注册、创建窗口+控件+文本读写(含中文)、
以及 C VM 与 pyvm 两条执行路径。交互事件(poll/run)不在自动化中模拟。

运行: python tests/test_ui.py
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, DexError,
)

LIBS = os.path.join(ROOT, "libs")
UI_DIR = os.path.join(LIBS, "ui")
DLL_PATH = os.path.join(UI_DIR, "libdexui.dll")
TMP_SRC = os.path.join(ROOT, "_ui_test.dex")

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


def compile_src(src):
    with open(TMP_SRC, "w", encoding="utf-8") as f:
        f.write(src)
    toks = Lexer(src, TMP_SRC).tokenize()
    ast = Parser(toks, TMP_SRC).parse_program()
    return compile_program(ast, source_path=TMP_SRC,
                           include_dirs=[LIBS])


def run_c_vm(prog):
    bc = assemble(prog)
    tmp = os.path.join(ROOT, "_ui_test.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    try:
        r = subprocess.run([vm, tmp], capture_output=True, timeout=60)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return r


AUTO_SRC = (
    'include "ui";\n'
    'let win = dex_ui_create("t", 300, 160);\n'
    'if win < 1 { print "skip"; }\n'
    'else {\n'
    '  let lbl = dex_ui_label(win, 12);\n'
    '  let ed = dex_ui_edit(win, 44);\n'
    '  let btn = dex_ui_button(win, 80);\n'
    '  dex_ui_set_text(lbl, "hello");\n'
    '  dex_ui_set_text(ed, "world 世界");\n'
    '  print dex_ui_get_text(lbl);\n'
    '  print dex_ui_get_text(ed);\n'
    '  print dex_ui_running(win);\n'
    '  dex_ui_destroy(win);\n'
    '  print "ok";\n'
    '}\n'
)


def test_registration():
    print("[registration]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (未构建 libdexui.dll)")
        return
    unit = compile_src('include "ui";\nprint 1;\n')
    names = {n.name for n in unit.natives}
    check("注册 10 个 UI 函数", len(unit.natives) == 10, len(unit.natives))
    for need in ("dex_ui_create", "dex_ui_label", "dex_ui_button", "dex_ui_edit",
                 "dex_ui_set_text", "dex_ui_get_text", "dex_ui_running",
                 "dex_ui_poll", "dex_ui_run", "dex_ui_destroy"):
        check(f"含 {need}", need in names)
    sigs = {n.name: n.sig for n in unit.natives}
    check("create 签名 s:i:i→i", sigs.get("dex_ui_create") == "sii:i", sigs)
    check("set_text 签名 i:s→i", sigs.get("dex_ui_set_text") == "is:i", sigs)
    check("get_text 签名 i:s", sigs.get("dex_ui_get_text") == "i:s", sigs)


def test_c_vm():
    print("[C VM]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (缺 DLL)")
        return
    unit = compile_src(AUTO_SRC)
    r = run_c_vm(unit.to_program())
    out = r.stdout.decode("utf-8", "replace")
    lines = [l for l in out.splitlines()]
    if lines and lines[0] == "skip":
        print("  SKIP  (本环境无法创建窗口)")
        return
    check("退出码 0", r.returncode == 0, repr(r.stderr))
    check("标签文本", lines and lines[0] == "hello", repr(out))
    check("输入框文本(含中文)", lines and lines[1] == "world 世界", repr(out))
    check("窗口运行中=1", lines and lines[2] == "1", repr(out))
    check("正常结束", lines and lines[-1] == "ok", repr(out))


def test_pyvm():
    print("[pyvm]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (缺 DLL)")
        return
    from dexlang.pyvm import run_program
    unit = compile_src(AUTO_SRC)
    out_l, err_l = run_program(unit.to_program())
    if out_l and out_l[0] == "skip":
        print("  SKIP  (本环境无法创建窗口)")
        return
    check("pyvm 运行无错", err_l is None, repr(err_l))
    check("pyvm 标签文本", out_l and out_l[0] == "hello", repr(out_l))
    check("pyvm 中文往返", out_l and out_l[1] == "world 世界", repr(out_l))


def test_static():
    print("[static ui]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (缺 DLL)")
        return
    src = 'include "ui_static";\nlet win = dex_ui_create("s", 200, 100);\n' \
          'if win < 1 { print "skip"; } else { dex_ui_destroy(win); print "ok"; }\n'
    unit = compile_src(src)
    check("静态库标记", unit.libs[0].is_static is True)
    check("内嵌 DLL 字节", len(unit.libs[0].data) > 100000, len(unit.libs[0].data))


def test_event():
    """模拟按钮点击(WM_COMMAND):验证 dex_ui_poll 返回正确的控件 id。"""
    print("[event]")
    if not os.path.exists(DLL_PATH):
        print("  SKIP  (缺 DLL)")
        return
    try:
        import ctypes
        from ctypes import wintypes
    except Exception as e:
        print(f"  SKIP  无 ctypes: {e}")
        return
    dll = ctypes.CDLL(DLL_PATH)
    dll.dex_ui_create.restype = ctypes.c_int64
    dll.dex_ui_create.argtypes = [ctypes.c_char_p, ctypes.c_int64, ctypes.c_int64]
    dll.dex_ui_label.restype = ctypes.c_int64
    dll.dex_ui_label.argtypes = [ctypes.c_int64, ctypes.c_int64]
    dll.dex_ui_button.restype = ctypes.c_int64
    dll.dex_ui_button.argtypes = [ctypes.c_int64, ctypes.c_int64]
    dll.dex_ui_poll.restype = ctypes.c_int64
    dll.dex_ui_poll.argtypes = [ctypes.c_int64]
    dll.dex_ui_destroy.restype = ctypes.c_int64
    dll.dex_ui_destroy.argtypes = [ctypes.c_int64]

    user32 = ctypes.windll.user32
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.FindWindowW.restype = wintypes.HWND
    user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
                                    wintypes.WPARAM, wintypes.LPARAM]
    user32.SendMessageW.restype = wintypes.LPARAM
    WM_COMMAND = 0x0111

    win = dll.dex_ui_create(b"dexui_event_test", 300, 160)
    if win < 1:
        print("  SKIP  无法创建窗口")
        return
    try:
        btn = dll.dex_ui_button(win, 80)
        hwnd = user32.FindWindowW(None, "dexui_event_test")
        check("找到窗口", bool(hwnd), hwnd)
        if hwnd:
            # 模拟按钮点击:wParam = MAKEWPARAM(btn, BN_CLICKED=0) = btn
            user32.SendMessageW(hwnd, WM_COMMAND, btn, 0)
            e = dll.dex_ui_poll(win)
            check("poll 返回按钮 id", e == btn, (e, btn))
            e2 = dll.dex_ui_poll(win)
            check("无事件返回 0", e2 == 0, e2)
    finally:
        dll.dex_ui_destroy(win)


def main():
    print("DEXCODE WinAPI UI 库测试(纯 C 实现)")
    test_registration()
    test_c_vm()
    test_pyvm()
    test_event()
    test_static()
    if os.path.exists(TMP_SRC):
        os.remove(TMP_SRC)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
