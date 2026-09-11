#!/usr/bin/env python3
"""DEXCODE GAL 引擎测试。

运行: python tests/test_gal.py
覆盖:引擎初始化、纯色/图片背景、文本渲染、立绘合成、选项按钮、
截图输出(BMP)与像素内容验证、C VM 完整链路。

注意:会真实创建窗口(短暂闪现),需要交互桌面会话。
"""

import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program, assemble  # noqa: E402

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


VM = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")


def compile_src(src):
    return compile_program(
        Parser(Lexer(src, "<gal>").tokenize(), "<gal>").parse_program(),
        source_path=None, include_dirs=[os.path.join(ROOT, "libs")])


def read_bmp(path):
    """读取 32/24bpp BMP,返回 (w, h, [(r,g,b), ...]) 自上而下。"""
    with open(path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    topdown = h < 0
    h = abs(h)
    bytespp = bpp // 8
    row = ((w * bytespp) + 3) // 4 * 4
    px = []
    for y in range(h):
        sy = y if topdown else (h - 1 - y)
        base = off + sy * row
        for x in range(w):
            b = data[base + x * bytespp]
            g = data[base + x * bytespp + 1]
            r = data[base + x * bytespp + 2]
            px.append((r, g, b))
    return w, h, px


GAL_SRC = """
include "gal";
gal_init(400, 300);
gal_set_title("GAL Test");
gal_bg_color(0x2050A0);
gal_speaker("测试");
gal_text("你好, GAL 引擎!");
let n = 0;
while n < 12 { gal_poll(); gal_wait(16); n = n + 1; }
gal_screenshot("_gal_shot1.bmp");
gal_bg("examples/gal/res/bg.png");
gal_sprite(1, "examples/gal/res/char.png");
gal_sprite_pos(1, 60);
gal_sprite_show(1, 1);
gal_set_choice(0, "选项一");
gal_set_choice(1, "选项二");
gal_show_choices();
gal_text("请选择路线");
while n < 24 { gal_poll(); gal_wait(16); n = n + 1; }
gal_screenshot("_gal_shot2.bmp");
gal_close();
print "done";
"""


def run_gal(src):
    unit = compile_src(src)
    bc = assemble(unit.to_program())
    tmp = os.path.join(ROOT, "_tmp_gal.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    try:
        r = subprocess.run([VM, tmp], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=40)
        return r.stdout, r.stderr, r.returncode
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def test_engine_render():
    print("引擎渲染(截图+像素)")
    if not os.path.exists(VM):
        skip("引擎渲染", "C VM 未构建")
        return
    out, err, code = run_gal(GAL_SRC)
    check("退出码 0", code == 0, f"code={code} err={err}")
    check("程序结束(done)", "done" in out, f"out={out!r}")

    s1 = os.path.join(ROOT, "_gal_shot1.bmp")
    s2 = os.path.join(ROOT, "_gal_shot2.bmp")
    try:
        check("截图1 存在", os.path.exists(s1))
        check("截图2 存在", os.path.exists(s2))
        if os.path.exists(s1):
            w, h, px = read_bmp(s1)
            check("截图1 尺寸 400x300", (w, h) == (400, 300), f"{w}x{h}")
            # 对话框从 y = 300-170-10 = 120 开始;顶部 y=30 应在对话框上方 → 纯蓝背景
            r, g, b = px[30 * w + w // 2]
            check("截图1 顶部为蓝色背景", abs(r - 32) < 30 and abs(g - 80) < 30 and abs(b - 160) < 30,
                  f"RGB({r},{g},{b})")
            # 对话框内(y=200)应被半透明黑压暗 → 整体亮度明显更低
            r2, g2, b2 = px[200 * w + w // 2]
            darker = (r2 + g2 + b2) < (r + g + b) - 30
            check("截图1 对话框区域变暗", darker, f"RGB({r2},{g2},{b2}) vs RGB({r},{g},{b})")
        if os.path.exists(s2):
            w, h, px = read_bmp(s2)
            check("截图2 尺寸 400x300", (w, h) == (400, 300), f"{w}x{h}")
            # 截图2 背景是渐变图(顶部),不再是纯蓝
            r, g, b = px[30 * w + w // 2]
            is_grad = not (abs(r - 32) < 30 and abs(g - 80) < 30 and abs(b - 160) < 30)
            check("截图2 背景已切换为图片", is_grad, f"RGB({r},{g},{b})")
    finally:
        for s in (s1, s2):
            if os.path.exists(s):
                os.remove(s)


def test_static_link():
    print("静态链接(egui 式)编译检查")
    # 生成静态定义后应能编译(运行时无 DLL)——单独验证 dexdef 可解析
    try:
        compile_src(GAL_SRC)
        check("gal.dexdef 可解析/编译", True)
    except Exception as e:
        check("gal.dexdef 可解析/编译", False, str(e))


RAND_SRC = """
include "gal";
gal_init(320, 240);
let i = 0;
let ok = 1;
while i < 50 {
    let r = gal_rand(100);
    if r < 0 || r >= 100 { ok = 0; }
    i = i + 1;
}
if ok { print "rand_ok"; }
gal_close();
print "done";
"""


def test_rand():
    print("gal_rand 随机数范围")
    if not os.path.exists(VM):
        skip("gal_rand", "C VM 未构建")
        return
    out, err, code = run_gal(RAND_SRC)
    check("退出码 0", code == 0, f"code={code} err={err}")
    check("随机数 0..99", "rand_ok" in out, f"out={out!r}")


TRANS_SRC = """
include "gal";
gal_init(320, 240);
gal_bg("examples/gal/res/bg.png");
gal_bg_trans("examples/gal/res/bg.png", 60);   // 交叉淡化
let n = 0;
while n < 12 { gal_poll(); gal_wait(16); n = n + 1; }
gal_fade_out(30);        // 淡出到黑(保持)
gal_fade_in(30);         // 从黑淡入
gal_stop_se();
gal_stop_sound();
gal_close();
print "done";
"""


def test_trans():
    print("背景交叉淡化 + 镜头淡入淡出 + 声音停止")
    if not os.path.exists(VM):
        skip("过渡/声音", "C VM 未构建")
        return
    out, err, code = run_gal(TRANS_SRC)
    check("退出码 0", code == 0, f"code={code} err={err}")
    check("程序结束(done)", "done" in out, f"out={out!r}")
    check("无异常输出", "bad" not in out, f"out={out!r}")


TOAST_SRC = """
include "gal";
gal_init(320, 240);
gal_toast("存档成功", 3, 300);
gal_toast("获得道具", 0, 300);
gal_toast("等级提升", 1, 300);
let n = 0;
while n < 10 { gal_poll(); gal_wait(16); n = n + 1; }
gal_close();
print "done";
"""


def test_toast():
    print("快速消息提示(四角 Toast)")
    if not os.path.exists(VM):
        skip("消息提示", "C VM 未构建")
        return
    out, err, code = run_gal(TOAST_SRC)
    check("退出码 0", code == 0, f"code={code} err={err}")
    check("程序结束(done)", "done" in out, f"out={out!r}")


FILEIO_SRC = """
include "gal";
gal_init(320, 240);
gal_var_set("hp", "100");
gal_var_set("name", "小羽");
gal_save_vars("_io_test.dat");
gal_var_set("hp", "0");
gal_load_vars("_io_test.dat");
if gal_var_get("hp") == "100" && gal_var_get("name") == "小羽" { print "io_ok"; }
gal_file_write("_io_test2.txt", "hello world");
if gal_file_read("_io_test2.txt") == "hello world" { print "wr_ok"; }
gal_close();
print "done";
"""


def test_fileio():
    print("文件 IO(存档/读档/读写文本)")
    if not os.path.exists(VM):
        skip("文件 IO", "C VM 未构建")
        return
    out, err, code = run_gal(FILEIO_SRC)
    check("退出码 0", code == 0, f"code={code} err={err}")
    check("变量存档/读档", "io_ok" in out, f"out={out!r}")
    check("文本读写", "wr_ok" in out, f"out={out!r}")
    for f in ("_io_test.dat", "_io_test2.txt"):
        fp = os.path.join(ROOT, f)
        if os.path.exists(fp):
            os.remove(fp)


CPK_SRC = """
include "gal";
gal_init(320, 240);
gal_set_title("CPK");
// 第一次经过存档点 A:记录位置并写文件
let _ckpt = "A";
gal_var_set("_ckpt", _ckpt);
gal_var_set("_ckpt_scene", gal_itoa(0));
gal_save_vars("_ckp_test.dat");
// 之后继续玩到别处(位置被改动)
gal_var_set("_ckpt", "X");
gal_var_set("_ckpt_scene", gal_itoa(9));
// 读档跳转:恢复全部变量
let ok = gal_load_vars("_ckp_test.dat");
if ok != 0 { print "loadfail"; }
let _s = gal_var_num("_ckpt_scene");
if _s == 0 {
  let _enter = gal_var_get("_ckpt");
  if _enter == "A" { print "fromA"; }
  else if _enter == "B" { print "fromB"; }
  else { print "normal"; }
} else {
  print "wrong-scene";
}
gal_close();
print "done";
"""


def test_checkpoint():
    print("存档点(存档→读档→进入点分支)")
    if not os.path.exists(VM):
        skip("存档点", "C VM 未构建")
        return
    out, err, code = run_gal(CPK_SRC)
    check("退出码 0", code == 0, f"code={code} err={err}")
    check("读档成功", "loadfail" not in out, f"out={out!r}")
    check("进入点分支从存档点 A 继续", "fromA" in out, f"out={out!r}")
    check("正确收尾", "done" in out, f"out={out!r}")
    fp = os.path.join(ROOT, "_ckp_test.dat")
    if os.path.exists(fp):
        os.remove(fp)


def test_menu_zorder():
    """菜单控件命中 z-order:按钮画在面板上方,点击按钮位置必须命中按钮而非面板。"""
    print("菜单 z-order(面板上按钮可点)")
    if not os.path.exists(VM):
        skip("菜单 z-order", "C VM 未构建")
        return
    src = """
include "gal";
gal_init(960, 540);
gal_set_title("MENU Z");
gal_menu_begin();
gal_menu_new("panel", 3);
gal_menu_set("panel", "x", "250");
gal_menu_set("panel", "y", "150");
gal_menu_set("panel", "w", "460");
gal_menu_set("panel", "h", "200");
gal_menu_new("ok_btn", 0);
gal_menu_set("ok_btn", "x", "360");
gal_menu_set("ok_btn", "y", "200");
gal_menu_set("ok_btn", "w", "240");
gal_menu_set("ok_btn", "h", "64");
while gal_menu_active() {
  gal_poll();
  let e = gal_menu_event();
  if e == "ok_btn" { print "btn_hit"; gal_menu_exit("确定"); }
  else if e == "panel" { print "panel_hit"; }
  gal_wait(16);
}
print "done";
"""
    unit = compile_src(src)
    bc = assemble(unit.to_program())
    tmp = os.path.join(ROOT, "_tmp_galz.dexbc")
    with open(tmp, "wb") as f:
        f.write(bc)
    try:
        import ctypes
        user32 = ctypes.WinDLL("user32")
        proc = subprocess.Popen([VM, tmp], cwd=ROOT,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)
        import time
        time.sleep(1.2)
        hwnd = user32.FindWindowW(None, "MENU Z")
        # 点击按钮中心(面板区域内)→ 必须命中按钮
        lparam = (232 << 16) | 480
        user32.PostMessageW(hwnd, 0x0201, 1, lparam)   # WM_LBUTTONDOWN
        user32.PostMessageW(hwnd, 0x0202, 0, lparam)   # WM_LBUTTONUP
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
        out, err = proc.communicate()
        check("点击按钮命中按钮(非面板)", "btn_hit" in out, f"out={out!r}")
        check("未误触面板", "panel_hit" not in out, f"out={out!r}")
        check("菜单正常收尾", "done" in out, f"out={out!r}")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def test_theme_trail():
    """主题特效(樱花对话框装饰/形状)+ 鼠标拖尾开关。"""
    print("主题特效 + 鼠标拖尾开关")
    if not os.path.exists(VM):
        skip("主题特效", "C VM 未构建")
        return
    src = """
include "gal";
gal_init(960, 540);
gal_theme_apply("樱花");
gal_trail_enable(1);
if gal_trail_get() == 1 { print "trail_on"; }
gal_trail_enable(0);
if gal_trail_get() == 0 { print "trail_off"; }
gal_trail_enable(1);
gal_bg_color(0x202030);
gal_speaker("小樱");
gal_text("樱花的对话框:上圆下直 + 花瓣装饰 + 流光边框。");
let n = 0;
while n < 24 { gal_poll(); gal_wait(16); n = n + 1; }
gal_screenshot("_theme_shot.bmp");
gal_close();
print "done";
"""
    out, err, code = run_gal(src)
    check("退出码 0", code == 0, f"code={code} err={err}")
    check("拖尾开关查询", "trail_on" in out and "trail_off" in out, f"out={out!r}")
    shot = os.path.join(ROOT, "_theme_shot.bmp")
    if os.path.exists(shot):
        w, h, px = read_bmp(shot)
        pink = 0
        for y in range(int(h * 0.72), h):
            for x in range(0, w, 3):
                r, g, b = px[y * w + x]
                if r > 190 and 120 < g < 220 and b > 150:
                    pink += 1
        check("樱花对话框粉色装饰", pink > 60, f"pink={pink}")
        os.remove(shot)
    else:
        check("樱花截图生成", False)


if __name__ == "__main__":
    test_engine_render()
    test_static_link()
    test_rand()
    test_trans()
    test_toast()
    test_fileio()
    test_checkpoint()
    test_menu_zorder()
    test_theme_trail()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    sys.exit(1 if FAIL else 0)
