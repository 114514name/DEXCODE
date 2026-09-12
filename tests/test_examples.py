#!/usr/bin/env python3
"""DEXCODE 示例编译守卫:examples/dexgame/*.dex 必须始终能编译。

为什么只编译不运行:三个示例都要**真窗口 + GPU**(demo_m1 测帧率、demo_m3 玩物理、
platformer 是完整游戏),CI/无人值守环境跑不了;而"示例还能不能编译、有没有引用到
已经改名的接口"恰恰是最容易悄悄烂掉的地方 —— 所以这里把它钉住。

(platformer.dex 的实际运行结果见 AGENTS.md 的 M4 记录:自动演示 1050 帧通关。)

运行: python tests/test_examples.py
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program, assemble, render, DexError  # noqa: E402

LIBS = os.path.join(ROOT, "libs")
EX = os.path.join(ROOT, "examples", "dexgame")

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


def compile_file(path):
    with open(path, encoding="utf-8") as f:
        src = f.read()
    toks = Lexer(src, path).tokenize()
    ast = Parser(toks, path).parse_program()
    unit = compile_program(ast, source_path=path, include_dirs=[LIBS])
    prog = unit.to_program()
    return src, unit, prog


def main():
    print("DEXCODE 示例编译守卫")
    print("[示例清单]")
    if not os.path.isdir(EX):
        check("examples/dexgame 存在", False, EX)
        print(f"\n结果: {PASS} 通过, {FAIL} 失败")
        return 1
    names = sorted(n for n in os.listdir(EX) if n.endswith(".dex"))
    check("至少 3 个示例", len(names) >= 3, names)
    check("有完整游戏示例 platformer.dex", "platformer.dex" in names, names)

    compiled = {}
    print("[逐个编译]")
    for n in names:
        path = os.path.join(EX, n)
        try:
            src, unit, prog = compile_file(path)
        except DexError as e:
            check(f"{n} 能编译", False, str(e))
            continue
        bc = assemble(prog)
        compiled[n] = (src, unit, prog, bc, render(prog))
        check(f"{n} 能编译(字节码 {len(bc)} 字节)", len(bc) > 500, len(bc))
        check(f"{n} 字节码版本 = 3", bc[4] == 3, bc[4])

    print("[platformer 用到了各层接口]")
    if "platformer.dex" in compiled:
        asm = compiled["platformer.dex"][4]
        # 主循环与回调(M4-a):eng_run_frames 来自语言模块 dexgame_fast.dex
        check("主循环 eng_run_frames", "eng_run_frames" in asm)
        for fn in ("on_start", "on_update", "on_draw"):
            check(f"回调 {fn} 挂上了", fn in asm)
        check("自动演示的决策函数 auto_should_jump", "auto_should_jump" in asm)
        # 各层接口
        for api, why in (("eng_find", "实体名(回调里取实体)"),
                         ("eng_action_down", "动作映射"),
                         ("eng_input_feed", "合成输入"),
                         ("eng_text", "文字 HUD"),
                         ("eng_font_load", "字体"),
                         ("eng_sound_load", "音频"),
                         ("eng_sound_play", "播放"),
                         ("eng_raycast", "射线探坑"),
                         ("eng_query_rect", "矩形查询(金币/敌人)"),
                         ("eng_tilemap_load_csv", "瓦片地图"),
                         ("eng_overlap", "重叠判定(敌人/终点)"),
                         ("eng_velocity_y", "速度读取"),
                         ("eng_draw_scene", "场景绘制")):
            check(f"用了 {api}({why})", api in asm)
    else:
        check("platformer 编译产物可用", False)

    print("[demo_m3 用到物理与查询]")
    if "demo_m3.dex" in compiled:
        asm = compiled["demo_m3.dex"][4]
        for api in ("eng_physics_step", "eng_tilemap_load_csv", "eng_query_rect", "eng_raycast"):
            check(f"用了 {api}", api in asm)

    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
