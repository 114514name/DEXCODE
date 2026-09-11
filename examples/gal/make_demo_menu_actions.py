#!/usr/bin/env python3
"""生成「MENU 蓝图扩块」阶段4 演示蓝图。

运行: python examples/gal/make_demo_menu_actions.py
产物: examples/gal/demo_menu_actions.bluescene

演示流程(闭环):
  场景1「开场」:文本 → 菜单块(设置菜单)→ 菜单内:
    输入框输入名字 → 点「确定」→ 菜单内把输入框值存入全局变量 player_name
    → 消息提示「名字已保存!」→ 退出「确定」→ 镜头动态显示「你好,{player_name}」。
  体现:菜单内 读输入框 / 设置变量 / 消息(Toast)/ 动态文本。
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from bluedit import model as M
from bluedit import sceneio

OUT = os.path.join(ROOT, "examples", "gal", "demo_menu_actions.bluescene")


def build():
    p = M.Project()
    p.title = "菜单扩块演示 · 设置名字"
    p.resDir = "res"
    p.description = "阶段4:MENU 蓝图扩块(变量/读输入框/消息/动态文本)"
    p.win_w = 960
    p.win_h = 540

    def L(sc_, fn, fp, tn, tp):
        sc_.links.append(M.Link(sc_.lid, fn, fp, tn, tp))
        sc_.lid += 1

    # ---------- 菜单「设置」(独占) ----------
    m = M.MENUScene("设置")
    m.mode = 0
    m.bg = "res/menu_bg.png"
    start = m.new_node(M.M_START, 60, 60)
    panel = m.new_node(M.M_CTRL, 300, 60)
    panel.ctrl = "panel"; panel.ctype = 3
    panel.px = 250; panel.py = 110; panel.pw = 460; panel.ph = 280
    panel.bg = "#1c2530"
    title = m.new_node(M.M_CTRL, 300, 200)
    title.ctrl = "title"; title.ctype = 1
    title.px = 320; title.py = 140; title.pw = 320; title.ph = 40
    title.text = "请输入你的名字:"
    name_in = m.new_node(M.M_CTRL, 300, 280)
    name_in.ctrl = "name_input"; name_in.ctype = 4
    name_in.px = 320; name_in.py = 200; name_in.pw = 320; name_in.ph = 50
    ok_btn = m.new_node(M.M_CTRL, 300, 360)
    ok_btn.ctrl = "ok"; ok_btn.ctype = 0
    ok_btn.px = 400; ok_btn.py = 280; ok_btn.pw = 160; ok_btn.ph = 60
    ok_btn.text = "确定"
    # 数据:读取输入框内容
    ninput = m.new_node(M.N_INPUT, 640, 120)
    ninput.ctrl = "name_input"
    # 动作:设置变量 + 消息 + 退出
    sv = m.new_node(M.N_SETVAR, 640, 220)
    sv.var_name = "player_name"; sv.var_type = 2; sv.set_mode = 1
    toast = m.new_node(M.N_TOAST, 640, 320)
    toast.toast_text = "名字已保存!"
    toast.toast_corner = 3
    toast.toast_ms = 1500
    click_ok = m.new_node(M.M_CLICK, 360, 440)
    click_ok.target_ctrl = "ok"
    exit_node = m.new_node(M.M_EXIT, 640, 420)
    exit_node.exit_name = "确定"
    L(m, start.id, 0, panel.id, 0)
    L(m, panel.id, 0, title.id, 0)
    L(m, title.id, 0, name_in.id, 0)
    L(m, name_in.id, 0, ok_btn.id, 0)
    L(m, ninput.id, 0, sv.id, 1)        # 输入框值 → 设置变量 A
    L(m, click_ok.id, 0, sv.id, 0)      # 点「确定」→ 设置变量
    L(m, sv.id, 0, toast.id, 0)
    L(m, toast.id, 0, exit_node.id, 0)
    p.menus.append(m)

    # ---------- 镜头「开场」 ----------
    sc = M.scene_defaults("开场")
    sc.bg = "res/bg.png"
    sc.useBgImg = 1
    sc.bg_fit = 0
    e = sc.node(sc.entry_id); ex = sc.node(sc.exit_id)
    spk1 = sc.new_node(M.N_SPEAK, 200, 60)
    spk1.text = "【演示】打开「设置」菜单,输入名字后点确定,镜头会向你问好。"
    mnode = sc.new_node(M.N_MENU, 200, 200)
    mnode.menu_idx = 0
    mnode.action = 0
    mnode.sync_menu_outputs(m)
    # 动态文本:你好, {player_name}
    greet = sc.new_node(M.N_TEXTLIT, 640, 300); greet.lit_text = "你好, "
    getvar = sc.new_node(M.N_GETVAR, 640, 380)
    getvar.var_name = "player_name"; getvar.var_type = 2
    txt = sc.new_node(M.N_TEXT, 640, 460)
    say = sc.new_node(M.N_SAY, 640, 540)
    L(sc, e.id, 0, spk1.id, 0)
    L(sc, spk1.id, 0, mnode.id, 0)
    L(sc, mnode.id, 0, say.id, 0)       # 菜单「确定」→ 动态文本
    L(sc, say.id, 0, ex.id, 0)
    L(sc, greet.id, 0, txt.id, 0)       # 文本 A
    L(sc, getvar.id, 0, txt.id, 1)      # 文本 B
    L(sc, txt.id, 0, say.id, 1)         # 拼接 → 动态文本

    p.scenes = [sc]
    return p


if __name__ == "__main__":
    sceneio.save_project(OUT, build())
    print("已生成:", OUT)
