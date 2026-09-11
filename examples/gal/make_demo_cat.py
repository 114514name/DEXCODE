#!/usr/bin/env python3
"""生成「猫猫物语 · 黄昏初遇」演示蓝图(复刻 JASONBASEENG 主线演示)。

运行: python examples/gal/make_demo_cat.py
产物: examples/gal/demo_cat.bluescene

流程(闭环):
  场景1「黄昏初遇」:黄昏背景 → 旁白 → 小黑(左)登场对话 → 小白(右)登场对话
    → 好感度+10(变量+消息) → 选项三选一:
       公园看日落 → 场景2「公园约会」   (好感+15)
       街角咖啡店  → 场景3「咖啡馆」     (好感+10)
       留下聊天    → 场景4「夜色」       (好感+5)
  每个约会场景:立绘对话 → 好感度递增 → 消息提示 → 结束。
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from bluedit import model as M
from bluedit import sceneio

OUT = os.path.join(ROOT, "examples", "gal", "demo_cat.bluescene")


def build():
    p = M.Project()
    p.title = "猫猫物语 · 黄昏初遇"
    p.resDir = "res"
    p.description = "复刻 JASONBASEENG 演示:立绘/对话/变量/选项分支/场景切换"
    p.win_w = 960
    p.win_h = 540

    def L(sc_, fn, fp, tn, tp):
        sc_.links.append(M.Link(sc_.lid, fn, fp, tn, tp))
        sc_.lid += 1

    # ---------- 场景1: 黄昏初遇 ----------
    sc1 = M.scene_defaults("黄昏初遇")
    sc1.bg = "res/park_dusk.jpg"
    sc1.useBgImg = 1
    sc1.bg_fit = 0
    e1 = sc1.node(sc1.entry_id)

    nar1 = sc1.new_node(M.N_SPEAK, 120, 40)
    nar1.text = "黄昏时分,金色的阳光洒在安静的街道上……"
    # 小黑(左)登场
    blk = sc1.new_node(M.N_SPRITE, 120, 120)
    blk.spr_path = "res/cat_black.png"
    blk.spr_layer = 0
    blk.spr_pos_mode = 1          # 左
    blk.spr_fit = 2
    spk1 = sc1.new_node(M.N_SPEAK, 120, 190)
    spk1.name = "小黑"; spk1.text = "……(望着远方的夕阳,眼中带着一丝落寞)"
    spk2 = sc1.new_node(M.N_SPEAK, 120, 260)
    spk2.name = "小黑"; spk2.text = "又一天过去了啊。黄昏……总是让人安心又寂寞。"
    # 小白(右)登场
    wht = sc1.new_node(M.N_SPRITE, 120, 330)
    wht.spr_path = "res/cat_white.png"
    wht.spr_layer = 1
    wht.spr_pos_mode = 2          # 右
    wht.spr_fit = 2
    spk3 = sc1.new_node(M.N_SPEAK, 120, 400)
    spk3.name = "小白"; spk3.text = "嗨!找到你了!!"
    spk4 = sc1.new_node(M.N_SPEAK, 120, 470)
    spk4.name = "小黑"; spk4.text = "!!……你、你好。你是……?"
    spk5 = sc1.new_node(M.N_SPEAK, 120, 540)
    spk5.name = "小白"; spk5.text = "我是小白!刚搬来附近的。远远看到你在看夕阳,觉得……你好像需要一个朋友。"
    spk6 = sc1.new_node(M.N_SPEAK, 120, 610)
    spk6.name = "小黑"; spk6.text = "(愣了一下)……朋友?"
    # 好感度 +10
    defvar = sc1.new_node(M.N_DEFVAR, 700, 40)
    defvar.var_name = "affection"; defvar.var_type = 0; defvar.var_value = "0"
    gv = sc1.new_node(M.N_GETVAR, 700, 120); gv.var_name = "affection"; gv.var_type = 0
    n10 = sc1.new_node(M.N_NUMLIT, 700, 190); n10.lit_num = "10"
    math = sc1.new_node(M.N_MATH, 700, 260); math.op = "+"
    sv = sc1.new_node(M.N_SETVAR, 700, 330)
    sv.var_name = "affection"; sv.var_type = 0; sv.set_mode = 1
    toast1 = sc1.new_node(M.N_TOAST, 700, 400)
    toast1.toast_text = "[好感] 好感度 +10"; toast1.toast_corner = 3; toast1.toast_ms = 1500
    ch = sc1.new_node(M.N_CHOICE, 120, 680)
    ch.choice_text = "小白: 你想去哪里呢?"
    ch.options = ["一起去公园看日落!", "去街角咖啡店坐坐吧", "就在这里聊聊天就好"]
    ch.sync_choice_outputs()
    flow_park = sc1.new_node(M.N_FLOW, 620, 700); flow_park.flow_target = 1; flow_park.flow_scene = 1
    flow_cafe = sc1.new_node(M.N_FLOW, 620, 780); flow_cafe.flow_target = 1; flow_cafe.flow_scene = 2
    flow_stay = sc1.new_node(M.N_FLOW, 620, 860); flow_stay.flow_target = 1; flow_stay.flow_scene = 3

    L(sc1, e1.id, 0, nar1.id, 0)
    L(sc1, nar1.id, 0, blk.id, 0)
    L(sc1, blk.id, 0, spk1.id, 0)
    L(sc1, spk1.id, 0, spk2.id, 0)
    L(sc1, spk2.id, 0, wht.id, 0)
    L(sc1, wht.id, 0, spk3.id, 0)
    L(sc1, spk3.id, 0, spk4.id, 0)
    L(sc1, spk4.id, 0, spk5.id, 0)
    L(sc1, spk5.id, 0, spk6.id, 0)
    L(sc1, spk6.id, 0, defvar.id, 0)
    L(sc1, defvar.id, 0, sv.id, 0)
    L(sc1, sv.id, 0, toast1.id, 0)
    L(sc1, toast1.id, 0, ch.id, 0)
    L(sc1, ch.id, 0, flow_park.id, 0)   # 选项0
    L(sc1, ch.id, 1, flow_cafe.id, 0)   # 选项1
    L(sc1, ch.id, 2, flow_stay.id, 0)   # 选项2
    L(sc1, gv.id, 0, math.id, 0)        # 好感度 → 加法 A
    L(sc1, n10.id, 0, math.id, 1)       # 10 → 加法 B
    L(sc1, math.id, 0, sv.id, 1)        # 结果 → 设置变量

    # ---------- 场景2: 公园约会 ----------
    sc2 = M.scene_defaults("公园约会")
    sc2.bg = "res/park_day.jpg"
    sc2.useBgImg = 1
    sc2.bg_fit = 0
    e2 = sc2.node(sc2.entry_id); ex2 = sc2.node(sc2.exit_id)
    spk = sc2.new_node(M.N_SPEAK, 200, 80)
    spk.name = "小白"; spk.text = "太好了!!公园山顶能看到整座城市的日落!"
    spk2b = sc2.new_node(M.N_SPEAK, 200, 160)
    spk2b.name = "小黑"; spk2b.text = "(嘴角微微上扬)好,走吧。我知道一条近路。"
    gv2 = sc2.new_node(M.N_GETVAR, 640, 80); gv2.var_name = "affection"; gv2.var_type = 0
    n15 = sc2.new_node(M.N_NUMLIT, 640, 160); n15.lit_num = "15"
    math2 = sc2.new_node(M.N_MATH, 640, 240); math2.op = "+"
    sv2 = sc2.new_node(M.N_SETVAR, 640, 320)
    sv2.var_name = "affection"; sv2.var_type = 0; sv2.set_mode = 1
    toast2 = sc2.new_node(M.N_TOAST, 640, 400)
    toast2.toast_text = "[好感] 好感度 +15"; toast2.toast_corner = 3
    L(sc2, e2.id, 0, spk.id, 0)
    L(sc2, spk.id, 0, spk2b.id, 0)
    L(sc2, spk2b.id, 0, sv2.id, 0)
    L(sc2, sv2.id, 0, toast2.id, 0)
    L(sc2, toast2.id, 0, ex2.id, 0)
    L(sc2, gv2.id, 0, math2.id, 0)
    L(sc2, n15.id, 0, math2.id, 1)
    L(sc2, math2.id, 0, sv2.id, 1)

    # ---------- 场景3: 咖啡馆 ----------
    sc3 = M.scene_defaults("咖啡馆")
    sc3.bg = "res/cafe.jpg"
    sc3.useBgImg = 1
    sc3.bg_fit = 0
    e3 = sc3.node(sc3.entry_id); ex3 = sc3.node(sc3.exit_id)
    spk3a = sc3.new_node(M.N_SPEAK, 200, 80)
    spk3a.name = "小白"; spk3a.text = "好呀!我听说那家的热牛奶特别好喝!"
    spk3b = sc3.new_node(M.N_SPEAK, 200, 160)
    spk3b.name = "小黑"; spk3b.text = "……热牛奶?嗯,听起来不错。其实……我平时都是一个人去那家店的。"
    spk3c = sc3.new_node(M.N_SPEAK, 200, 240)
    spk3c.name = "小白"; spk3c.text = "那今天就是两个人啦!走吧走吧~"
    gv3 = sc3.new_node(M.N_GETVAR, 640, 80); gv3.var_name = "affection"; gv3.var_type = 0
    n10b = sc3.new_node(M.N_NUMLIT, 640, 160); n10b.lit_num = "10"
    math3 = sc3.new_node(M.N_MATH, 640, 240); math3.op = "+"
    sv3 = sc3.new_node(M.N_SETVAR, 640, 320)
    sv3.var_name = "affection"; sv3.var_type = 0; sv3.set_mode = 1
    toast3 = sc3.new_node(M.N_TOAST, 640, 400)
    toast3.toast_text = "[好感] 好感度 +10"; toast3.toast_corner = 3
    L(sc3, e3.id, 0, spk3a.id, 0)
    L(sc3, spk3a.id, 0, spk3b.id, 0)
    L(sc3, spk3b.id, 0, spk3c.id, 0)
    L(sc3, spk3c.id, 0, sv3.id, 0)
    L(sc3, sv3.id, 0, toast3.id, 0)
    L(sc3, toast3.id, 0, ex3.id, 0)
    L(sc3, gv3.id, 0, math3.id, 0)
    L(sc3, n10b.id, 0, math3.id, 1)
    L(sc3, math3.id, 0, sv3.id, 1)

    # ---------- 场景4: 夜色 ----------
    sc4 = M.scene_defaults("夜色")
    sc4.bg = "res/street_night.jpg"
    sc4.useBgImg = 1
    sc4.bg_fit = 0
    e4 = sc4.node(sc4.entry_id); ex4 = sc4.node(sc4.exit_id)
    spk4a = sc4.new_node(M.N_SPEAK, 200, 80)
    spk4a.name = "小白"; spk4a.text = "嗯!在这儿一边看夕阳一边聊天也很好!"
    spk4b = sc4.new_node(M.N_SPEAK, 200, 160)
    spk4b.name = "小黑"; spk4b.text = "(有些意外)你……不觉得无聊吗?"
    spk4c = sc4.new_node(M.N_SPEAK, 200, 240)
    spk4c.name = "小白"; spk4c.text = "怎么会!和朋友在一起,就算只是看天空,也很有意思呀!"
    spk4d = sc4.new_node(M.N_SPEAK, 200, 320)
    spk4d.name = "小黑"; spk4d.text = "……朋友。这个词,已经很久没有听到过了。"
    gv4 = sc4.new_node(M.N_GETVAR, 640, 80); gv4.var_name = "affection"; gv4.var_type = 0
    n5 = sc4.new_node(M.N_NUMLIT, 640, 160); n5.lit_num = "5"
    math4 = sc4.new_node(M.N_MATH, 640, 240); math4.op = "+"
    sv4 = sc4.new_node(M.N_SETVAR, 640, 320)
    sv4.var_name = "affection"; sv4.var_type = 0; sv4.set_mode = 1
    toast4 = sc4.new_node(M.N_TOAST, 640, 400)
    toast4.toast_text = "[好感] 好感度 +5"; toast4.toast_corner = 3
    L(sc4, e4.id, 0, spk4a.id, 0)
    L(sc4, spk4a.id, 0, spk4b.id, 0)
    L(sc4, spk4b.id, 0, spk4c.id, 0)
    L(sc4, spk4c.id, 0, spk4d.id, 0)
    L(sc4, spk4d.id, 0, sv4.id, 0)
    L(sc4, sv4.id, 0, toast4.id, 0)
    L(sc4, toast4.id, 0, ex4.id, 0)
    L(sc4, gv4.id, 0, math4.id, 0)
    L(sc4, n5.id, 0, math4.id, 1)
    L(sc4, math4.id, 0, sv4.id, 1)

    p.scenes = [sc1, sc2, sc3, sc4]
    return p


if __name__ == "__main__":
    sceneio.save_project(OUT, build())
    print("已生成:", OUT)
