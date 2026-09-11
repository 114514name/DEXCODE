#!/usr/bin/env python3
"""生成「MENU 数据输入口 + 控件文字流化」阶段2/3 演示蓝图。

运行: python examples/gal/make_demo_menu_input.py
产物: examples/gal/demo_menu_input.bluescene

演示流程:
  镜头「开场」:文本 → 菜单块(引用「问候弹窗」,数据入口「问候语」连字面值
              「欢迎光临,冒险者!」)→ 运行独占菜单 → 退出「确定」→ 继续文本。
  菜单「问候弹窗」:开场起点 → 面板控件 → 按钮控件(「文字」输入口连数据入口
               「问候语」,按钮动态显示镜头传入的值)→ 退出「确定」。
  效果:菜单按钮上显示的是镜头传入的文字,而不是写死在菜单里。
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from bluedit import model as M
from bluedit import sceneio

OUT = os.path.join(ROOT, "examples", "gal", "demo_menu_input.bluescene")


def build():
    p = M.Project()
    p.title = "数据入口演示 · 菜单控件文字流化"
    p.resDir = "res"
    p.description = "阶段2/3:MENU 数据入口(M_IN)+ 控件文字数据流化"
    p.win_w = 960
    p.win_h = 540

    def L(sc_, fn, fp, tn, tp):
        sc_.links.append(M.Link(sc_.lid, fn, fp, tn, tp))
        sc_.lid += 1

    # ---------- 菜单「问候弹窗」(独占) ----------
    m = M.MENUScene("问候弹窗")
    m.mode = 0
    m.bg = "res/menu_bg.png"
    start = m.new_node(M.M_START, 60, 60)
    panel = m.new_node(M.M_CTRL, 300, 60)
    panel.ctrl = "panel"; panel.ctype = 3
    panel.px = 250; panel.py = 150; panel.pw = 460; panel.ph = 200
    panel.bg = "#1c2530"
    btn = m.new_node(M.M_CTRL, 300, 220)
    btn.ctrl = "ok_btn"; btn.ctype = 0
    btn.px = 360; btn.py = 200; btn.pw = 240; btn.ph = 64
    btn.font = 26
    greeting = m.new_node(M.M_IN, 560, 60)
    greeting.param_name = "问候语"
    # 点击起点:点按钮才退出(开场链只做初始化,若直接连退出会立即结束菜单)
    click_ok = m.new_node(M.M_CLICK, 300, 380)
    click_ok.target_ctrl = "ok_btn"
    exit_node = m.new_node(M.M_EXIT, 560, 300)
    exit_node.exit_name = "确定"
    L(m, start.id, 0, panel.id, 0)
    L(m, panel.id, 0, btn.id, 0)
    L(m, greeting.id, 0, btn.id, 1)     # 数据入口 → 按钮「文字」
    L(m, click_ok.id, 0, exit_node.id, 0)  # 点击 ok_btn → 退出「确定」
    p.menus.append(m)

    # ---------- 镜头「开场」 ----------
    sc = M.scene_defaults("开场")
    sc.bg = "res/bg.png"
    sc.useBgImg = 1
    sc.bg_fit = 0
    e = sc.node(sc.entry_id); ex = sc.node(sc.exit_id)
    spk1 = sc.new_node(M.N_SPEAK, 200, 60)
    spk1.text = "【演示】镜头会把「问候语」传给菜单,菜单按钮上的文字由镜头决定。"
    lit = sc.new_node(M.N_TEXTLIT, 600, 200)
    lit.lit_text = "欢迎光临,冒险者!"
    mnode = sc.new_node(M.N_MENU, 200, 200)
    mnode.menu_idx = 0
    mnode.action = 0
    mnode.sync_menu_outputs(m)
    spk2 = sc.new_node(M.N_SPEAK, 200, 380)
    spk2.text = "你点了确定。菜单按钮显示的是镜头传入的问候语——数据流化的效果。"
    L(sc, e.id, 0, spk1.id, 0)
    L(sc, spk1.id, 0, mnode.id, 0)
    L(sc, mnode.id, 0, spk2.id, 0)      # 退出「确定」→ 继续
    L(sc, spk2.id, 0, ex.id, 0)
    L(sc, lit.id, 0, mnode.id, 1)       # 字面值 → 菜单块「问候语」输入口

    p.scenes = [sc]
    return p


if __name__ == "__main__":
    sceneio.save_project(OUT, build())
    print("已生成:", OUT)
