#!/usr/bin/env python3
"""DEXCODE EGUI Designer 测试。

运行: python tests/test_designer.py
覆盖:数据模型、序列化往返、生成 .dex 代码、生成的代码可编译、
信号连接生成、保存/加载设计文件。
"""

import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program, DexError  # noqa: E402
from dexide.designer import (  # noqa: E402
    Widget, design_to_dict, design_from_dict, gen_dex,
    save_design, load_design, KIND_SIGNALS,
)

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


def make_design():
    widgets = [
        Widget("button", 1, text="开始", x=20, y=20, w=100, h=30,
               signals=[("clicked", "on_start")]),
        Widget("checkbox", 2, text="启用", x=20, y=70, w=120, h=26,
               signals=[("changed", "on_check")]),
        Widget("edit", 3, text="输入", x=20, y=110, w=160, h=26),
        Widget("combo", 4, text="", x=20, y=150, w=140, h=26,
               signals=[("selected", "on_pick")]),
        Widget("slider", 5, text="", x=20, y=190, w=180, h=26,
               signals=[("changed", "on_slide")]),
        Widget("progress", 6, text="", x=20, y=230, w=180, h=26),
        Widget("group", 7, text="选项", x=20, y=270, w=220, h=90),
    ]
    return design_from_dict(design_to_dict("我的窗口", 520, 400, widgets))


def test_model():
    print("数据模型")
    w = Widget("button", 9, text="OK")
    check("默认尺寸", w.w == 100 and w.h == 30, f"{w.w}x{w.h}")
    check("默认文本", w.text == "OK", w.text)
    check("信号可选", "clicked" in KIND_SIGNALS["button"] and KIND_SIGNALS["progress"] == [],
          str(KIND_SIGNALS))


def test_roundtrip():
    print("序列化往返")
    d = make_design()
    d2 = design_from_dict(design_to_dict(d["title"], d["width"], d["height"], d["widgets"]))
    check("控件数一致", len(d2["widgets"]) == 7, len(d2["widgets"]))
    check("标题一致", d2["title"] == "我的窗口", d2["title"])
    b = d2["widgets"][0]
    check("按钮字段一致", (b.kind, b.wid, b.text, b.x, b.y, b.w, b.h, b.signals) ==
          ("button", 1, "开始", 20, 20, 100, 30, [("clicked", "on_start")]),
          str((b.kind, b.wid, b.signals)))


def test_gen_dex():
    print("生成代码")
    d = make_design()
    code = gen_dex(d)
    for frag in ['include "egui_fast";', 'eg_window("我的窗口", 520, 400)',
                 'eg_add("button", win, 1);', 'eg_set_pos(1, 20, 20);',
                 'eg_set_size(1, 100, 30);', 'eg_set_text(1, "开始");',
                 'eg_connect(1, "clicked", "on_start");', 'func on_start(id) {',
                 'eg_app_run();', 'eg_connect(win, "closed", "on_win_closed");']:
        check(f"含 {frag!r}", frag in code, "")
    check("重复回调只生成一次", code.count("func on_start(id)") == 1, "")


def test_gen_dex_compiles():
    print("生成代码可编译")
    d = make_design()
    code = gen_dex(d)
    try:
        compile_program(
            Parser(Lexer(code, "<gen>").tokenize(), "<gen>").parse_program(),
            source_path=None, include_dirs=[os.path.join(ROOT, "libs")])
        check("编译成功", True)
    except DexError as e:
        check("编译成功", False, str(e))


def test_save_load():
    print("保存/加载设计文件")
    d = make_design()
    with tempfile.TemporaryDirectory(prefix="dexds_") as tmp:
        path = os.path.join(tmp, "ui.egui")
        save_design(path, d)
        d2 = load_design(path)
        check("加载控件数一致", len(d2["widgets"]) == 7, len(d2["widgets"]))
        check("加载信号一致", d2["widgets"][0].signals == [("clicked", "on_start")],
              d2["widgets"][0].signals)


def test_escaped_text():
    print("文本转义")
    d = design_to_dict('带"引号"', 300, 200,
                       [Widget("label", 1, text='他说"你好"')])
    code = gen_dex(design_from_dict(d))
    check("引号被转义", 'eg_set_text(1, "他说\\"你好\\"");' in code, code)
    try:
        compile_program(
            Parser(Lexer(code, "<g>").tokenize(), "<g>").parse_program(),
            source_path=None, include_dirs=[os.path.join(ROOT, "libs")])
        check("转义后仍可编译", True)
    except DexError as e:
        check("转义后仍可编译", False, str(e))


if __name__ == "__main__":
    test_model()
    test_roundtrip()
    test_gen_dex()
    test_gen_dex_compiles()
    test_save_load()
    test_escaped_text()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
