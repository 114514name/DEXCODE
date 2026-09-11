#!/usr/bin/env python3
"""生成「存档点 + 跳转到存档点」阶段1演示蓝图。

运行: python examples/gal/make_demo_checkpoint.py
产物: examples/gal/demo_checkpoint.bluescene

演示流程(闭环):
  场景1「主线」:入口 → 文本 → 存档点A(记录位置+写 save/check1.dat)
              → 文本(A之后,跳回时从这段继续) → 选项「去读档页 / 继续主线」
                 ├ 去读档页 → 柔和切到场景2
                 └ 继续主线 → 文本(A之后) → 选项「再走一段 / 结束演示」
                                ├ 再走一段 → 文本 → 结束
                                └ 结束演示 → 结束
  场景2「读档页」:入口 → 文本 → 跳转到存档点(check1.dat)
              → 读档恢复 → 回场景1 → 从存档点A之后继续

  效果:任何时候跳回,主线都从「A之后」那段继续(位置+变量全部恢复)。
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from bluedit import model as M
from bluedit import sceneio

OUT = os.path.join(ROOT, "examples", "gal", "demo_checkpoint.bluescene")


def build():
    p = M.Project()
    p.title = "存档点演示 · 跨镜头跳转"
    p.resDir = "res"
    p.author = ""
    p.version = "1.0"
    p.description = "阶段1:存档点(N_CHECKPOINT)+ 跳转到存档点(N_JUMP)"
    p.win_w = 960
    p.win_h = 540

    def L(sc, fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    # ---------- 场景1:主线 · 岔路口 ----------
    sc0 = M.scene_defaults("主线 · 岔路口")
    sc0.bg = "res/bg.png"
    sc0.useBgImg = 1
    sc0.bgColor = 0x101018
    sc0.bg_fit = 0
    e0 = sc0.node(sc0.entry_id)

    spk1 = sc0.new_node(M.N_SPEAK, 200, 60)
    spk1.text = "【主线】这是存档点演示。剧情经过存档点 A 时,会记录当前位置,并写入存档文件。"
    spk2 = sc0.new_node(M.N_SPEAK, 200, 140)
    spk2.text = "现在经过了存档点 A:位置已记录,变量已存盘。"
    ckA = sc0.new_node(M.N_CHECKPOINT, 200, 220)
    ckA.ck_name = "A"
    ckA.file_path = "save/check1.dat"
    spkA = sc0.new_node(M.N_SPEAK, 200, 300)
    spkA.text = "【A之后】主线第 1 段 —— 每次跳回存档点,都会从这里继续。"
    ch1 = sc0.new_node(M.N_CHOICE, 200, 380)
    ch1.choice_text = "接下来怎么走?"
    ch1.options = ["去读档页", "继续主线"]
    ch1.sync_choice_outputs()
    trans = sc0.new_node(M.N_SCENE_TRANS, 560, 380)
    trans.flow_target = 1
    trans.flow_scene = 1
    trans.scene_trans_ms = 300
    spkB = sc0.new_node(M.N_SPEAK, 560, 460)
    spkB.text = "【A之后】主线第 2 段:你选择继续在主线前进。"
    ch2 = sc0.new_node(M.N_CHOICE, 560, 540)
    ch2.choice_text = "要做什么?"
    ch2.options = ["再走一段", "结束演示"]
    ch2.sync_choice_outputs()
    spkC = sc0.new_node(M.N_SPEAK, 920, 540)
    spkC.text = "【A之后】主线第 3 段:再走一段后,演示结束。"
    flow_end = sc0.new_node(M.N_FLOW, 920, 620)
    flow_end.flow_target = 2

    L(sc0, e0.id, 0, spk1.id, 0)
    L(sc0, spk1.id, 0, spk2.id, 0)
    L(sc0, spk2.id, 0, ckA.id, 0)
    L(sc0, ckA.id, 0, spkA.id, 0)
    L(sc0, spkA.id, 0, ch1.id, 0)
    L(sc0, ch1.id, 0, trans.id, 0)      # 选项0:去读档页
    L(sc0, ch1.id, 1, spkB.id, 0)       # 选项1:继续主线
    L(sc0, spkB.id, 0, ch2.id, 0)
    L(sc0, ch2.id, 0, spkC.id, 0)       # 选项0:再走一段
    L(sc0, ch2.id, 1, flow_end.id, 0)   # 选项1:结束演示
    L(sc0, spkC.id, 0, flow_end.id, 0)

    # ---------- 场景2:读档页 ----------
    sc1 = M.scene_defaults("读档页")
    sc1.bg = "res/bg_room.png"
    sc1.useBgImg = 1
    sc1.bgColor = 0x101018
    sc1.bg_fit = 0
    e1 = sc1.node(sc1.entry_id)
    spkD = sc1.new_node(M.N_SPEAK, 200, 80)
    spkD.text = "【读档页】读取 check1.dat,跳转到存档点 A —— 位置与变量都会恢复。"
    jmpA = sc1.new_node(M.N_JUMP, 200, 160)
    jmpA.file_path = "save/check1.dat"
    L(sc1, e1.id, 0, spkD.id, 0)
    L(sc1, spkD.id, 0, jmpA.id, 0)

    p.scenes = [sc0, sc1]
    return p


if __name__ == "__main__":
    sceneio.save_project(OUT, build())
    print("已生成:", OUT)
