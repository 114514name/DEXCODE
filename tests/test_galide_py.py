#!/usr/bin/env python3
"""GAL 编辑器(Python 版)核心逻辑测试。

运行: python tests/test_galide_py.py
覆盖:数据模型 / 块树工具 / .galscene 往返 / C 版兼容 / 旧格式迁移 / 导出 DEX。
"""

import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from galide import model as M
from galide import sceneio, export

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


def test_model():
    print("数据模型 + 块树工具")
    p = M.project_defaults()
    check("新项目 1 镜头", p.nScenes == 1)
    check("默认块为说话", p.scenes[0].blocks[0].type == M.BL_SPEAK)
    sc = p.scenes[0]
    # 插入一个 CHOICE + 两个 OPTION + 子块
    M.insert_block(sc, M.BL_CHOICE, 0)
    op1 = M.block_defaults(M.BL_OPTION, 1)
    sc.blocks.insert(sc.cur + 1, op1)
    sc.cur = sc.cur + 1
    op2 = M.block_defaults(M.BL_OPTION, 1)
    sc.blocks.insert(sc.cur + 1, op2)
    sc.cur = sc.cur + 1
    spk = M.block_defaults(M.BL_SPEAK, 2)   # 选项1的子块
    spk.speaker = "A"; spk.text = "你好"
    sc.blocks.insert(sc.cur + 1, spk)
    sc.cur = sc.cur + 1
    sc.nBlocks = len(sc.blocks)
    check("块计数", sc.nBlocks == 5)
    # block_end(CHOICE 在 1)应到 5
    e = M.block_end(sc, 1)
    check("CHOICE 子树结束", e == 5, f"got {e}")
    # block_parent(SPEAK at 4, depth2) = OPTION at 3
    check("parent 找到", M.block_parent(sc, 4) == 3)
    # 移动:把 speak(4, depth2) 移到 index0 前
    M.block_move(sc, 4, 0, 0)
    check("移动后仍 5 块", sc.nBlocks == 5)
    check("移动后首位是 speak", sc.blocks[0].type == M.BL_SPEAK and sc.blocks[0].depth == 2)


def test_roundtrip():
    print(".galscene 往返")
    p = M.project_defaults()
    p.title = "测试项目"
    p.chars.append(M.Character())
    p.chars[0].name = "小羽"
    p.chars[0].states[0] = "res/xiaoyu_happy.png"
    p.chars[0].stateNames[0] = "微笑"
    p.chars[0].nStates = 1
    p.nChars = 1
    sc = p.scenes[0]
    sc.title = "海边"
    sc.bg = "res/beach.png"
    b0 = sc.blocks[0]
    b0.speaker = "小羽"; b0.text = "今天的海真美"
    b0.style_avatar = 1; b0.text_pos = 2
    M.insert_block(sc, M.BL_SPRITE, 0)
    b1 = sc.blocks[1]
    b1.charIdx = 0; b1.stateIdx = 0; b1.spr_layer = 1; b1.spr_anim = 1
    M.insert_block(sc, M.BL_CHOICE, 0)
    op = M.block_defaults(M.BL_OPTION, 1)
    op.option_text = "嗯嗯"
    sc.blocks.insert(3, op)
    sc.nBlocks = len(sc.blocks)
    sc.cur = 0

    fd, path = tempfile.mkstemp(suffix=".galscene")
    os.close(fd)
    sceneio.save_scene(path, p)
    p2 = sceneio.load_scene(path)
    os.unlink(path)
    check("标题保留", p2.title == "测试项目", p2.title)
    check("人物保留", p2.nChars == 1 and p2.chars[0].name == "小羽")
    check("人物状态保留", p2.chars[0].states[0].endswith("res/xiaoyu_happy.png"))
    check("镜头标题", p2.scenes[0].title == "海边")
    check("镜头背景", p2.scenes[0].bg.endswith("res/beach.png"))
    check("块数保留", p2.scenes[0].nBlocks == 4, str(p2.scenes[0].nBlocks))
    b0b = p2.scenes[0].blocks[0]
    check("说话内容", b0b.speaker == "小羽" and b0b.text == "今天的海真美")
    check("说话样式", b0b.style_avatar == 1 and b0b.text_pos == 2)
    b1b = p2.scenes[0].blocks[1]
    check("立绘引用", b1b.charIdx == 0 and b1b.stateIdx == 0 and b1b.spr_anim == 1)
    b2b = p2.scenes[0].blocks[2]
    check("选项块", b2b.type == M.BL_CHOICE)
    check("选项项", p2.scenes[0].blocks[3].option_text == "嗯嗯")


def test_c_format_compat():
    print("C 版格式兼容(直接构造 C 版输出文本)")
    sample = """title=海边物语
res=res
[char]
cname=小羽
nstates=2
stname=微笑
stimg=res/xy_smile.png
stname=生气
stimg=res/xy_angry.png
[/char]
[scene]
stitle=第一幕
bgdir=res/bg
bg=res/bg/day.png
bgcolor=4279879
usebgimg=1
spr0=res/bg/char.png
[block]
type=0
depth=0
speaker=小羽
text=早上好
avatar=1
name=1
textpos=0
autofit=0
[/block]
[block]
type=6
depth=0
speaker=
text=要出发吗?
[/block]
[block]
type=7
depth=1
option_text=出发
[/block]
[block]
type=7
depth=1
option_text=再等等
[/block]
[/scene]
"""
    fd, path = tempfile.mkstemp(suffix=".galscene")
    os.close(fd)
    with open(path, "w", encoding="utf-8") as f:
        f.write(sample)
    p = sceneio.load_scene(path)
    os.unlink(path)
    check("C 版标题", p.title == "海边物语", p.title)
    check("C 版人物", p.nChars == 1 and p.chars[0].nStates == 2)
    check("C 版镜头背景", p.scenes[0].bg == "res/bg/day.png")
    check("C 版镜头级立绘", p.scenes[0].spr[0] == "res/bg/char.png")
    check("C 版 4 块", p.scenes[0].nBlocks == 4, str(p.scenes[0].nBlocks))
    check("C 版背景色", p.scenes[0].bgColor == 4279879)
    b = p.scenes[0].blocks[0]
    check("C 版说话样式", b.style_avatar == 1 and b.text == "早上好")
    ch = p.scenes[0].blocks[1]
    check("C 版选项", ch.type == M.BL_CHOICE and ch.choice_text == "要出发吗?")
    check("C 版选项项1", p.scenes[0].blocks[2].option_text == "出发")
    check("C 版选项项2", p.scenes[0].blocks[3].option_text == "再等等")


def test_old_migrate():
    print("旧格式迁移([shot])")
    sample = """title=老作品
bg=res/old/day.png
bgcolor=16777215
usebgimg=1
[shot]
bg=res/old/day.png
speaker=小明
text=你好呀
[/shot]
[shot]
bg=res/old/night.png
spr0=res/old/hero.png
speaker=小明
text=天黑了
choice0=回家
choice1=继续
nchoices=2
[/shot]
"""
    fd, path = tempfile.mkstemp(suffix=".galscene")
    os.close(fd)
    with open(path, "w", encoding="utf-8") as f:
        f.write(sample)
    p = sceneio.load_scene(path)
    os.unlink(path)
    sc = p.scenes[0]
    check("迁移后镜头", p.nScenes == 1 and sc.title == "老作品")
    check("迁移首背景", sc.bg == "res/old/day.png")
    check("迁移背景色", sc.bgColor == 16777215)
    check("迁移有块", sc.nBlocks > 0, str(sc.nBlocks))
    kinds = [b.type for b in sc.blocks]
    check("迁移含背景块", M.BL_BG in kinds)
    check("迁移含立绘块", M.BL_SPRITE in kinds)
    check("迁移含选项", M.BL_CHOICE in kinds)
    opts = [b.option_text for b in sc.blocks if b.type == M.BL_OPTION]
    check("迁移选项项", opts == ["回家", "继续"], str(opts))


def test_export():
    print("导出 DEX 代码")
    p = M.project_defaults()
    p.title = "导出示例"
    sc = p.scenes[0]
    sc.title = "第一幕"
    sc.bg = "res/day.png"
    b0 = sc.blocks[0]
    b0.speaker = "小羽"; b0.text = "出发吧"
    M.insert_block(sc, M.BL_SPRITE, 0)
    b1 = sc.blocks[1]
    b1.charIdx = -1; b1.sprPath = "res/hero.png"; b1.spr_layer = 0
    M.insert_block(sc, M.BL_CHOICE, 0)
    op = M.block_defaults(M.BL_OPTION, 1)
    op.option_text = "好"
    sc.blocks.insert(3, op)
    sc.nBlocks = len(sc.blocks)

    fd, path = tempfile.mkstemp(suffix=".dex")
    os.close(fd)
    ok = export.write_scene_dex(p, path)
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    os.unlink(path)
    check("导出成功", ok)
    check("导出含 include", 'include "gal";' in text)
    check("导出含背景", 'gal_bg("res/day.png");' in text)
    check("导出含说话", 'gal_text("出发吧");' in text)
    check("导出含立绘", 'gal_sprite(0, "res/hero.png");' in text)
    check("导出含选项", "gal_show_choices();" in text)
    check("导出含分支", "if p == 0 {" in text)
    check("导出以 scene_0 开始", text.strip().endswith("scene_0();") or "scene_0();" in text)


def test_engine_sig():
    print("引擎接口可加载(ctypes)")
    from galide.engine import Engine
    e = Engine()
    ok = e.load()
    check("引擎 DLL 可加载", ok, e.dll_path)
    if ok:
        # 只验证符号存在,不真正 init(会开窗口)
        need = ["gal_init", "gal_poll", "gal_text", "gal_bg", "gal_sprite",
                "gal_show_choices", "gal_picked", "gal_set_choice"]
        miss = [n for n in need if not hasattr(e.lib, n)]
        check("引擎导出符号齐全", not miss, str(miss))


if __name__ == "__main__":
    test_model()
    test_roundtrip()
    test_c_format_compat()
    test_old_migrate()
    test_export()
    test_engine_sig()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
