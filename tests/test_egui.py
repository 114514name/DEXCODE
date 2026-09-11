#!/usr/bin/env python3
"""DEXCODE EGUI(easyGUI)测试。

运行: python tests/test_egui.py
覆盖:窗口/控件创建、文本与值读写、项目添加、信号连接、
事件队列(poll/next/event_*)、eg_quit/eg_close、
端到端信号模拟(真实窗口+模拟点击)、fast 模式(eg_app_run)、静态链接。

注意:会真实创建/闪现 Windows 窗口,需要交互桌面会话。
"""

import ctypes
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import (  # noqa: E402
    Lexer, Parser, compile_program, assemble, DexError,
)
from dexlang.pyvm import run_program  # noqa: E402

PASS = 0
FAIL = 0
SKIP = 0

user32 = ctypes.windll.user32


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


def compile_unit(src):
    return compile_program(
        Parser(Lexer(src, "<test>").tokenize(), "<test>").parse_program(),
        source_path=None, include_dirs=[os.path.join(ROOT, "libs")])


def run_py(src):
    unit = compile_unit(src)
    prog = unit.to_program()
    out, err = run_program(prog)
    if err:
        raise RuntimeError(err)
    return out


VM = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")


def run_vm_sync(bc):
    tmp = os.path.join(ROOT, "_tmp_egui.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    try:
        r = subprocess.run([VM, tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
        return r.stdout, r.stderr, r.returncode
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


# ---------- API 单元测试 ----------
API_SRC = """
include "egui";
let win = eg_window("EGUI API Test", 320, 240);
print win > 0;
print eg_running();
let b = eg_add("button", win, 1);
print b;
eg_set_pos(1, 10, 10);
eg_set_size(1, 120, 30);
eg_set_text(1, "你好");
print eg_get_text(1);
let cb = eg_add("checkbox", win, 2);
eg_set_value(2, 1);
print eg_get_value(2);
eg_set_value(2, 0);
print eg_get_value(2);
let sl = eg_add("slider", win, 3);
eg_set_value(3, 50);
print eg_get_value(3);
let pg = eg_add("progress", win, 4);
eg_set_value(4, 70);
print eg_get_value(4);
let cm = eg_add("combo", win, 5);
eg_add_item(5, "AA");
eg_add_item(5, "BB");
print eg_get_value(5);
let lb = eg_add("list", win, 6);
eg_add_item(6, "X");
eg_add_item(6, "Y");
eg_set_value(6, 1);
print eg_get_value(6);
eg_connect(1, "clicked", "on_click");
eg_poll();
print eg_next();
print eg_event_id();
eg_quit();
print eg_running();
eg_close(win);
print 1;
"""

API_EXPECTED = ["1", "1", "1", "你好", "1", "0", "50", "70",
                "-1", "1", "0", "0", "0", "1"]


def test_api_pyvm():
    print("EGUI API(Python VM)")
    out = run_py(API_SRC)
    check("API 输出匹配", out == API_EXPECTED, f"got {out}")


def test_api_cvm():
    print("EGUI API(C VM)")
    unit = compile_unit(API_SRC)
    out, err, code = run_vm_sync(assemble(unit.to_program()))
    check("C VM 退出码 0", code == 0, f"code={code} stderr={err}")
    check("C VM 输出匹配", out.strip().splitlines() == API_EXPECTED, f"got {out!r}")


# ---------- 端到端信号模拟 ----------
def find_window(title):
    hwnd = user32.FindWindowW(None, title)
    return hwnd or None


def find_child(parent, cls):
    hwnd = user32.FindWindowExW(parent, None, cls, None)
    return hwnd or None


def send_command(winhwnd, ctrl_id, code, ctrlhwnd):
    wparam = ctrl_id | (code << 16)
    user32.SendMessageW(winhwnd, 0x0111, wparam, ctrlhwnd)  # WM_COMMAND


CLICK_SRC = """
include "egui";
include "std";
func on_click(id) {
    print "clicked " + id;
    eg_quit();
}
let win = eg_window("EGUI Test Window", 320, 240);
eg_add("button", win, 1);
eg_set_pos(1, 10, 10);
eg_set_size(1, 120, 30);
eg_set_text(1, "点击");
eg_connect(1, "clicked", "on_click");
dex_sleep_ms(2500);
let n = 0;
while eg_running() && n < 5000 {
    eg_poll();
    while eg_next() != 0 {
        let cb = eg_event_cb();
        if cb != "" { call(cb, eg_event_id()); }
    }
    n = n + 1;
}
print "done";
"""


def _run_vm_async(bc):
    tmp = os.path.join(ROOT, "_tmp_egui_async.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    proc = subprocess.Popen(
        [VM, tmp], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW)
    return proc, tmp


def _wait_window(title, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        hwnd = find_window(title)
        if hwnd:
            return hwnd
        time.sleep(0.05)
    return None


def _finish_proc(proc, tmp, timeout=10.0):
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
    if os.path.exists(tmp):
        os.remove(tmp)
    return out, err, proc.returncode


def test_click_signal():
    print("端到端信号:按钮点击 → 回调")
    if not os.path.exists(VM):
        skip("click 信号", "C VM 未构建")
        return
    unit = compile_unit(CLICK_SRC)
    proc, tmp = _run_vm_async(assemble(unit.to_program()))
    try:
        win = _wait_window("EGUI Test Window")
        if not win:
            proc.kill()
            check("找到测试窗口", False, "窗口未出现")
            return
        btn = find_child(win, "Button")
        if not btn:
            proc.kill()
            check("找到按钮控件", False, "按钮未找到")
            return
        time.sleep(0.3)
        send_command(win, 1, 0, btn)   # WM_COMMAND, BN_CLICKED
        out, err, code = _finish_proc(proc, tmp)
        check("程序正常退出", code == 0, f"code={code} err={err}")
        check("回调执行(clicked 1)", "clicked 1" in out, f"out={out!r}")
        check("循环结束(done)", "done" in out, f"out={out!r}")
    except Exception as e:
        proc.kill()
        check("信号测试无异常", False, str(e))


FAST_SRC = """
include "egui_fast";
include "std";
func on_click(id) {
    print "fast clicked " + id;
    eg_quit();
}
func on_close(id) {
    eg_quit();
}
let win = eg_window("EGUI Fast Window", 320, 240);
eg_add("button", win, 1);
eg_set_pos(1, 10, 10);
eg_set_size(1, 120, 30);
eg_set_text(1, "点我");
eg_connect(1, "clicked", "on_click");
eg_connect(win, "closed", "on_close");
dex_sleep_ms(2500);
eg_app_run();
print "fast done";
"""


def test_fast_mode():
    print("fast 模式:eg_app_run 自动分发")
    if not os.path.exists(VM):
        skip("fast 模式", "C VM 未构建")
        return
    unit = compile_unit(FAST_SRC)
    proc, tmp = _run_vm_async(assemble(unit.to_program()))
    try:
        win = _wait_window("EGUI Fast Window")
        if not win:
            proc.kill()
            check("找到 fast 窗口", False, "窗口未出现")
            return
        btn = find_child(win, "Button")
        time.sleep(0.3)
        send_command(win, 1, 0, btn)
        out, err, code = _finish_proc(proc, tmp)
        check("fast 程序退出", code == 0, f"code={code} err={err}")
        check("fast 回调执行", "fast clicked 1" in out, f"out={out!r}")
        check("fast 循环结束", "fast done" in out, f"out={out!r}")
    except Exception as e:
        proc.kill()
        check("fast 测试无异常", False, str(e))


STATIC_SRC = """
include "egui_static";
let win = eg_window("EGUI Static Window", 200, 120);
print eg_running();
eg_add("label", win, 1);
eg_set_text(1, "静态链接");
print eg_get_text(1);
eg_quit();
print eg_running();
"""


def test_static_linking():
    print("静态链接:无外部 DLL")
    if not os.path.exists(VM):
        skip("静态", "C VM 未构建")
        return
    unit = compile_unit(STATIC_SRC)
    out, err, code = run_vm_sync(assemble(unit.to_program()))
    check("静态程序退出码 0", code == 0, f"code={code} err={err}")
    check("静态输出", out.strip().splitlines() == ["1", "静态链接", "0"],
          f"got {out!r}")


def test_egui_fast_static_compile():
    print("fast 模式静态编译检查(编译通过)")
    try:
        compile_unit(FAST_SRC)
        check("fast 源码可编译", True)
    except DexError as e:
        check("fast 源码可编译", False, str(e))


if __name__ == "__main__":
    test_api_pyvm()
    test_api_cvm()
    test_click_signal()
    test_fast_mode()
    test_static_linking()
    test_egui_fast_static_compile()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    sys.exit(1 if FAIL else 0)
