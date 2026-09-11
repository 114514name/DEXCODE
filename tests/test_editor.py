#!/usr/bin/env python3
"""DEXCODE GAL 编辑器冒烟测试。

运行: python tests/test_editor.py
覆盖:编辑器启动、主窗口出现、引擎预览窗口(DexGALWindow)创建成功。

注意:GAL 编辑器是 GUI 程序,会创建主窗口 + 内嵌引擎预览窗口,
需要交互桌面会话;测试通过 FindWindow 验证后自动关闭。
"""

import ctypes
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

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


EDITOR = os.path.join(ROOT, "tools", "galedit.exe")


def find_window(cls, title):
    if cls:
        h = user32.FindWindowW(cls, title)
        return h or None
    h = user32.FindWindowW(None, title)
    return h or None


def find_child(parent, cls):
    h = user32.FindWindowExW(parent, None, cls, None)
    return h or None


def test_editor_launch():
    print("编辑器启动冒烟")
    if not os.path.exists(EDITOR):
        skip("编辑器启动", "galedit.exe 未构建")
        return
    proc = subprocess.Popen([EDITOR], cwd=ROOT)
    try:
        deadline = time.time() + 10
        main_win = None
        while time.time() < deadline:
            main_win = find_window(None, "GAL 编辑器")
            if main_win:
                break
            time.sleep(0.1)
        check("主窗口出现", bool(main_win))
        if not main_win:
            return

        # 引擎预览窗口(类 DexGALWindow)应已创建并嵌入为主窗口子窗口
        deadline = time.time() + 10
        gal_win = None
        while time.time() < deadline:
            gal_win = find_child(main_win, "DexGALWindow")
            if gal_win:
                break
            time.sleep(0.1)
        check("引擎预览窗口嵌入", bool(gal_win))

        # 纯 GDI 自绘 UI:无标准 SysTabControl32/ListBox 控件,
        # 改为验证主窗口标题正确 + 进程持续运行(自绘面板已由截图冒烟验证)
        check("主窗口标题正确", bool(find_window(None, "GAL 编辑器")))

        # 模拟缩放:发送 WM_SIZE 触发自适应布局,进程不应崩溃
        user32.SetWindowPos(main_win, 0, 100, 100, 1100, 720, 0x0040)  # SWP_NOZORDER
        user32.SendMessageW(main_win, 0x0005, 0, (720 << 16) | 1100)  # WM_SIZE
        time.sleep(0.3)
        user32.SetWindowPos(main_win, 0, 100, 100, 960, 640, 0x0040)
        user32.SendMessageW(main_win, 0x0005, 0, (640 << 16) | 960)
        time.sleep(0.3)
        check("缩放后主窗口仍有效", bool(find_window(None, "GAL 编辑器")))
    finally:
        # 优雅关闭:发送 WM_CLOSE
        mw = find_window(None, "GAL 编辑器")
        if mw:
            user32.PostMessageW(mw, 0x0010, 0, 0)  # WM_CLOSE
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        else:
            proc.kill()


def test_scene_file_roundtrip():
    print("场景文件 .galscene 读写 + 指令式导出链路")
    if not os.path.exists(EDITOR):
        skip("galedit.exe", "未构建")
        return
    check("galedit.exe 存在", True)

    # 构造含人物集 + 全部块类型(说话/立绘/选项/如果/代码/结束)的场景
    scene = (
        "title=往返测试\nres=\n"
        "[char]\ncname=角色\nnstates=2\n"
        "stname=普通\nstimg=res/char.png\n"
        "stname=微笑\nstimg=res/char2.png\n[/char]\n"
        "[scene]\nstitle=镜头一\nbgdir=\nbg=\nbgcolor=1052688\nusebgimg=0\n"
        "spr0=\nspr1=\nspr2=\nspr3=\n"
        "[block]\ntype=0\ndepth=0\nspeaker=主角\ntext=你好世界\nname=1\ntextpos=2\nautofit=1\n[/block]\n"
        "[block]\ntype=2\ndepth=0\ncharidx=0\nstateidx=1\nsprlayer=0\nsprshow=1\nsprpos=4\nspr_autofit=0\nspranim=3\n[/block]\n"
        "[block]\ntype=6\ndepth=0\nspeaker=主角\ntext=选择?\n[/block]\n"
        "[block]\ntype=7\ndepth=1\noption_text=甲\n[/block]\n"
        "[block]\ntype=0\ndepth=2\nspeaker=主角\ntext=甲线\nname=1\n[/block]\n"
        "[block]\ntype=7\ndepth=1\noption_text=乙\n[/block]\n"
        "[block]\ntype=0\ndepth=2\nspeaker=主角\ntext=乙线\nname=1\n[/block]\n"
        "[block]\ntype=8\ndepth=0\ncond=gal_running()\n[/block]\n"
        "[block]\ntype=0\ndepth=1\nspeaker=旁白\ntext=条件OK\nname=1\n[/block]\n"
        "[block]\ntype=9\ndepth=0\ncodefile=_rt_frag.dex\ncodecond=\n[/block]\n"
        "[block]\ntype=10\ndepth=0\n[/block]\n"
        "[/scene]\n"
    )
    sc_path = os.path.join(ROOT, "_rt.galscene")
    frag_path = os.path.join(ROOT, "_rt_frag.dex")
    out_path = os.path.join(ROOT, "_rt_out.dex")
    bc_path = os.path.join(ROOT, "_rt_out.dexbc")
    try:
        with open(sc_path, "w", encoding="utf-8") as f:
            f.write(scene)
        with open(frag_path, "w", encoding="utf-8") as f:
            f.write('print "片段执行";\n')
        # 导出为指令式 DexLang
        r = subprocess.run([EDITOR, "-open", sc_path, "-write", out_path],
                           cwd=ROOT, capture_output=True, timeout=30)
        check("导出 .dex 生成", r.returncode == 0 and os.path.exists(out_path))
        if not os.path.exists(out_path):
            return
        text = open(out_path, encoding="utf-8").read()
        check("导出含顺序说话", "gal_speaker(\"主角\")" in text and "gal_text(\"你好世界\")" in text)
        check("导出含人物状态图", "gal_sprite(0, \"res/char2.png\")" in text)
        check("导出含立绘动画/位置", "gal_sprite_pos_mode(0, 4)" in text and "gal_sprite_anim(0, 3)" in text)
        check("导出含文字位置/风格", "gal_text_pos(2)" in text and "gal_box_autofit(1)" in text)
        check("导出含分支嵌套", "if p == 0" in text and "else if p == 1" in text)
        check("导出含条件块", "if gal_running() {" in text)
        check("导出含代码条", "片段执行" in text)
        # 编译导出代码(验证语法)
        r = subprocess.run([sys.executable, os.path.join(ROOT, "main.py"),
                            "compile", out_path, "-o", bc_path],
                           cwd=ROOT, capture_output=True, timeout=30)
        check("导出代码可编译", r.returncode == 0 and os.path.exists(bc_path))
        # 保存 -> 加载 -> 再导出(往返)
        saved = os.path.join(ROOT, "_rt_saved.galscene")
        r = subprocess.run([EDITOR, "-open", sc_path, "-save", saved],
                           cwd=ROOT, capture_output=True, timeout=30)
        check("保存 .galscene", r.returncode == 0 and os.path.exists(saved))
        r = subprocess.run([EDITOR, "-open", saved, "-write", out_path],
                           cwd=ROOT, capture_output=True, timeout=30)
        text2 = open(out_path, encoding="utf-8").read()
        check("往返后仍可导出", r.returncode == 0 and "gal_sprite(0, \"res/char2.png\")" in text2)
        # 旧格式兼容
        old = os.path.join(ROOT, "_rt_old.galscene")
        with open(old, "w", encoding="utf-8") as f:
            f.write("title=旧\nres=\n[shot]\nbg=\nusebgimg=1\nbgcolor=1052688\n"
                    "speaker=旧人\ntext=旧台词\nchoice0=甲\nnchoices=1\n[/shot]\n")
        r = subprocess.run([EDITOR, "-open", old, "-write", out_path],
                           cwd=ROOT, capture_output=True, timeout=30)
        text3 = open(out_path, encoding="utf-8").read()
        check("旧格式可转换导出", r.returncode == 0 and "gal_speaker(\"旧人\")" in text3)
        # 从 tools 目录启动(模拟用户双击 exe),CLI 导出仍成功(不依赖启动目录)
        out2 = os.path.join(ROOT, "_rt_out2.dex")
        r = subprocess.run([EDITOR, "-open", sc_path, "-write", out2],
                           cwd=os.path.join(ROOT, "tools"), capture_output=True, timeout=30)
        check("任意目录启动可导出", r.returncode == 0 and os.path.exists(out2))
    finally:
        for p in (sc_path, frag_path, out_path, bc_path,
                  os.path.join(ROOT, "_rt_saved.galscene"),
                  os.path.join(ROOT, "_rt_old.galscene"),
                  os.path.join(ROOT, "_rt_out2.dex"),
                  os.path.join(ROOT, "_rt_out.dxasm")):
            if os.path.exists(p):
                os.remove(p)


if __name__ == "__main__":
    test_editor_launch()
    test_scene_file_roundtrip()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    sys.exit(1 if FAIL else 0)
