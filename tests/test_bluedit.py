#!/usr/bin/env python3
"""GAL 蓝图编辑器核心逻辑测试。

运行: python tests/test_bluedit.py
覆盖:模型/节点引脚/序列化往返/变量统计/导出 DexLang 可编译。
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from bluedit import model as M
from bluedit import vars as vars_mod
from bluedit import export as ex
from bluedit import sceneio

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


def make_project():
    p = M.project_defaults()
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id)
    exitn = sc.node(sc.exit_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    spk = sc.new_node(M.N_SPEAK, 400, 100)
    spk.text = "你好"
    spk.name = "小羽"
    var = sc.new_node(M.N_DEFVAR, 400, 260)
    var.var_name = "hp"; var.var_type = 0; var.var_value = "100"
    ch = sc.new_node(M.N_CHOICE, 400, 420)
    ch.choice_text = "走?"
    ch.options = ["是", "否"]
    ch.sync_choice_outputs()
    o1 = sc.new_node(M.N_SPEAK, 800, 380); o1.text = "好"
    o2 = sc.new_node(M.N_SPEAK, 800, 460); o2.text = "不"
    L(entry.id, 0, spk.id, 0)
    L(spk.id, 0, var.id, 0)
    L(var.id, 0, ch.id, 0)
    L(ch.id, 0, o1.id, 0)
    L(ch.id, 1, o2.id, 0)
    L(o1.id, 0, exitn.id, 0)
    L(o2.id, 0, exitn.id, 0)
    return p


def test_model():
    print("数据模型 + 节点引脚")
    p = M.project_defaults()
    sc = p.scenes[0]
    check("入口存在", sc.entry_id >= 0 and sc.node(sc.entry_id).type == M.N_ENTRY)
    check("出口存在", sc.exit_id >= 0 and sc.node(sc.exit_id).type == M.N_EXIT)
    spk = sc.new_node(M.N_SPEAK, 0, 0)
    check("说话节点 1入1出", len(spk.inputs) == 1 and len(spk.outputs) == 1)
    ch = sc.new_node(M.N_CHOICE, 0, 0)
    ch.options = ["a", "b"]
    ch.sync_choice_outputs()
    check("选项2输出", len(ch.outputs) == 2)
    gv = sc.new_node(M.N_GETVAR, 0, 0)
    check("取值只有输出", len(gv.inputs) == 0 and len(gv.outputs) == 1)
    math = sc.new_node(M.N_MATH, 0, 0)
    check("运算2入1出", len(math.inputs) == 2 and len(math.outputs) == 1)


def test_roundtrip():
    print("序列化往返")
    p = make_project()
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    check("标题保留", p2.title == p.title)
    check("场景数", len(p2.scenes) == 1)
    sc2 = p2.scenes[0]
    check("节点数保留", len(sc2.nodes) == len(p.scenes[0].nodes))
    check("连线数保留", len(sc2.links) == len(p.scenes[0].links))
    check("入口出口保留", sc2.entry_id == p.scenes[0].entry_id and sc2.exit_id == p.scenes[0].exit_id)
    spk = [n for n in sc2.nodes if n.type == M.N_SPEAK and n.text == "你好"]
    check("说话内容保留", len(spk) == 1)


def test_vars():
    print("变量统计")
    p = make_project()
    vs = vars_mod.collect_vars(p)
    names = [v.name for v in vs]
    check("检测到 hp", "hp" in names, str(names))
    check("存储名全局", vars_mod.storage_name("hp", 0, 0) == "hp")
    check("存储名局部", vars_mod.storage_name("hp", 2, 1) == "_s2_hp")


def test_export_compile():
    print("导出 DexLang 可编译")
    p = make_project()
    d = tempfile.mkdtemp()
    path = ex.export_project(p, d)
    check("导出成功", bool(path))
    with open(path, encoding="utf-8") as f:
        text = f.read()
    check("含 include", 'include "gal_static";' in text or 'include "gal";' in text)
    check("含说话", 'gal_text("你好");' in text)
    check("含变量", 'gal_var_set("hp"' in text)
    check("含选项", "gal_show_choices();" in text)
    check("含分支", re.search(r"if _p\d+ == 0 \{", text) is not None)
    check("含拖尾开关", "gal_trail_enable(" in text)
    check("含镜头函数", "func scene_0() {" in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-300:])


def test_new_blocks():
    print("新增可视化块(拼接/动态文本/输入框/随机数/等待)")
    p = M.project_defaults()
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id)
    exitn = sc.node(sc.exit_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    n_wait = sc.new_node(M.N_WAIT, 400, 100); n_wait.ms = 500
    n_say = sc.new_node(M.N_SAY, 650, 100); n_say.name = "小羽"
    n_in = sc.new_node(M.N_INPUT, 400, 260); n_in.menu_idx = 0; n_in.ctrl = "name"
    n_rand = sc.new_node(M.N_RANDOM, 650, 260); n_rand.max = 10
    n_text = sc.new_node(M.N_TEXT, 900, 100)
    n_text.inputs[0].source = "literal"; n_text.inputs[0].literal = "你好, "
    check("拼接3入1出", len(n_text.inputs) == 3 and len(n_text.outputs) == 1)
    check("动态文本1入1出", len(n_say.inputs) == 2 and len(n_say.outputs) == 1)
    check("输入框仅输出", len(n_in.inputs) == 0 and len(n_in.outputs) == 1)
    check("随机仅输出", len(n_rand.inputs) == 0 and len(n_rand.outputs) == 1)
    check("等待1入1出", len(n_wait.inputs) == 1 and len(n_wait.outputs) == 1)
    # exec 链:entry -> wait -> say -> exit;value 网:input->B, rand->C, text->say.文本
    L(entry.id, 0, n_wait.id, 0)
    L(n_wait.id, 0, n_say.id, 0)
    L(n_say.id, 0, exitn.id, 0)
    L(n_in.id, 0, n_text.id, 1)
    L(n_rand.id, 0, n_text.id, 2)
    L(n_text.id, 0, n_say.id, 1)
    # 序列化往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    sc2 = p2.scenes[0]
    check("往返节点数", len(sc2.nodes) == len(sc.nodes))
    say2 = [n for n in sc2.nodes if n.type == M.N_SAY]
    check("动态文本属性保留", len(say2) == 1 and say2[0].name == "小羽")
    in2 = [n for n in sc2.nodes if n.type == M.N_INPUT]
    check("输入框属性保留", len(in2) == 1 and in2[0].ctrl == "name")
    rand2 = [n for n in sc2.nodes if n.type == M.N_RANDOM]
    check("随机属性保留", len(rand2) == 1 and rand2[0].max == 10)
    wait2 = [n for n in sc2.nodes if n.type == M.N_WAIT]
    check("等待属性保留", len(wait2) == 1 and wait2[0].ms == 500)
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("含等待", "gal_wait(500);" in text)
    check("含输入框读取", 'gal_menu_get_text("name")' in text)
    check("含随机数", "gal_rand(10)" in text)
    check("含拼接", 'gal_text(("你好, " + gal_menu_get_text("name") + gal_rand(10)))' in text)
    check("含点击等待清除", "wait_click();" in text and "gal_text_clear();" in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-300:])


def test_scene_data():
    print("数据输出点 + 场景数据")
    p = M.project_defaults()
    sc0 = p.scenes[0]
    entry0 = sc0.node(sc0.entry_id); exit0 = sc0.node(sc0.exit_id)

    def L(sc, fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    # 场景0:入口 -> 数据输出点(字面 100) -> 出口
    dout = sc0.new_node(M.N_DOUT, 400, 100)
    dout.out_name = "hp_val"
    dout.inputs[1].source = "literal"; dout.inputs[1].literal = "100"
    L(sc0, entry0.id, 0, dout.id, 0)
    L(sc0, dout.id, 0, exit0.id, 0)
    check("数据输出点2入1出", len(dout.inputs) == 2 and len(dout.outputs) == 1)
    check("输出点名", sc0.data_names() == ["hp_val"])
    # 场景1:入口 -> 动态文本(文本←场景数据) -> 出口
    sc1 = M.scene_defaults()
    p.scenes.append(sc1)
    entry1 = sc1.node(sc1.entry_id); exit1 = sc1.node(sc1.exit_id)
    say = sc1.new_node(M.N_SAY, 400, 100)
    dref = sc1.new_node(M.N_DREF, 400, 260)
    dref.out_scene = 0; dref.out_name = "hp_val"
    L(sc1, entry1.id, 0, say.id, 0)
    L(sc1, say.id, 0, exit1.id, 0)
    L(sc1, dref.id, 0, say.id, 1)
    check("场景数据仅输出", len(dref.inputs) == 0 and len(dref.outputs) == 1)
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    db = [n for n in p2.scenes[0].nodes if n.type == M.N_DOUT]
    check("输出点属性保留", len(db) == 1 and db[0].out_name == "hp_val")
    rb = [n for n in p2.scenes[1].nodes if n.type == M.N_DREF]
    check("场景数据属性保留", len(rb) == 1 and rb[0].out_scene == 0 and rb[0].out_name == "hp_val")
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("含数据输出", 'gal_var_set("_scout_0_hp_val", gal_itoa(100));' in text)
    check("含场景数据", 'gal_var_get("_scout_0_hp_val")' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-300:])


def test_menu_data_outputs():
    print("菜单块数据输出口(多输入框)")
    p = M.project_defaults()
    # 菜单:退出点"开始" + 两个输入框
    m = M.MENUScene("主菜单")
    exit_node = m.new_node(M.M_EXIT, 60, 60); exit_node.exit_name = "开始"
    i1 = m.new_node(M.M_CTRL, 320, 60); i1.ctrl = "input_name"; i1.ctype = 4
    i2 = m.new_node(M.M_CTRL, 320, 160); i2.ctrl = "job"; i2.ctype = 4
    p.menus.append(m)
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id); exitn = sc.node(sc.exit_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    mnode = sc.new_node(M.N_MENU, 400, 100)
    mnode.menu_idx = 0; mnode.action = 0
    mnode.sync_menu_outputs(m)
    kinds = [q.kind for q in mnode.outputs]
    names = [q.name for q in mnode.outputs]
    check("菜单块 1 exec + 2 value 输出", kinds == ["exec", "value", "value"], str(kinds))
    check("数据输出口名字=控件名", names == ["开始", "input_name", "job"], str(names))
    # 文本拼接:A=字面, B=菜单 input_name 口, C=菜单 job 口 → 动态文本
    say = sc.new_node(M.N_SAY, 800, 100)
    txt = sc.new_node(M.N_TEXT, 600, 260)
    txt.inputs[0].source = "literal"; txt.inputs[0].literal = "你好, "
    L(entry.id, 0, mnode.id, 0)
    L(mnode.id, 0, say.id, 0)          # exec:菜单"开始" → 动态文本
    L(say.id, 0, exitn.id, 0)
    L(mnode.id, 1, txt.id, 1)          # 菜单 input_name 口 → 文本 B
    L(mnode.id, 2, txt.id, 2)          # 菜单 job 口 → 文本 C
    L(txt.id, 0, say.id, 1)            # 文本 → 动态文本
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    mn2 = [n for n in p2.scenes[0].nodes if n.type == M.N_MENU]
    check("菜单块往返输出口", len(mn2) == 1 and
          [q.kind for q in mn2[0].outputs] == ["exec", "value", "value"])
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("含两个输入框读取",
          'gal_text(("你好, " + gal_menu_get_text("input_name") + gal_menu_get_text("job")))' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-300:])


def test_literal_blocks():
    print("字面量块 + 文本拼接动态段 + value 输入")
    p = M.project_defaults()
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id); exitn = sc.node(sc.exit_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    tl = sc.new_node(M.N_TEXTLIT, 0, 0); tl.lit_text = "欢迎来到星之回响"
    nl = sc.new_node(M.N_NUMLIT, 0, 0); nl.lit_num = "42"
    bl = sc.new_node(M.N_BOLLIT, 0, 0); bl.lit_bool = 1
    check("纯文本仅输出", len(tl.inputs) == 0 and len(tl.outputs) == 1)
    check("纯数字仅输出", len(nl.inputs) == 0 and len(nl.outputs) == 1)
    check("布尔仅输出", len(bl.inputs) == 0 and len(bl.outputs) == 1)
    txt = sc.new_node(M.N_TEXT, 400, 100)
    check("拼接初始3段", len(txt.inputs) == 3)
    txt.text_add_input()
    check("增加输入点后4段", len(txt.inputs) == 4 and txt.inputs[3].name == "D")
    txt.text_remove_input()
    check("删除后3段", len(txt.inputs) == 3)
    # exec:entry -> if -> dout -> say -> exit;数据网:文本→拼接,数字→输出点,布尔→条件
    cond = sc.new_node(M.N_IF, 600, 100)
    dout = sc.new_node(M.N_DOUT, 800, 100); dout.out_name = "score"
    say = sc.new_node(M.N_SAY, 1000, 100)
    L(entry.id, 0, cond.id, 0)
    L(cond.id, 0, dout.id, 0)
    L(dout.id, 0, say.id, 0)
    L(say.id, 0, exitn.id, 0)
    L(tl.id, 0, txt.id, 0)          # 纯文本 → 拼接 A
    L(txt.id, 0, say.id, 1)         # 拼接 → 动态文本
    L(nl.id, 0, dout.id, 1)         # 纯数字 → 数据输出点
    L(bl.id, 0, cond.id, 1)         # 布尔 → if 条件
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    sc2 = p2.scenes[0]
    t2 = [n for n in sc2.nodes if n.type == M.N_TEXTLIT]
    check("纯文本属性保留", len(t2) == 1 and t2[0].lit_text == "欢迎来到星之回响")
    n2 = [n for n in sc2.nodes if n.type == M.N_NUMLIT]
    check("纯数字属性保留", len(n2) == 1 and n2[0].lit_num == "42")
    b2 = [n for n in sc2.nodes if n.type == M.N_BOLLIT]
    check("布尔属性保留", len(b2) == 1 and b2[0].lit_bool == 1)
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("含纯文本", 'gal_text(("欢迎来到星之回响"))' in text)
    check("含纯数字", 'gal_var_set("_scout_0_score", gal_itoa(42));' in text)
    check("含布尔条件", "if 1 {" in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-300:])


def test_auto_layout():
    print("自动整理(分层/排序/防重叠)")
    from bluedit import layout as layout_mod
    p = M.project_defaults()
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id); exitn = sc.node(sc.exit_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    a = sc.new_node(M.N_SPEAK, 0, 0)
    ifn = sc.new_node(M.N_IF, 0, 0)
    b = sc.new_node(M.N_SPEAK, 0, 0)
    c = sc.new_node(M.N_SPEAK, 0, 0)
    d = sc.new_node(M.N_SPEAK, 0, 0)
    L(entry.id, 0, a.id, 0)
    L(a.id, 0, ifn.id, 0)
    L(ifn.id, 0, b.id, 0)
    L(ifn.id, 1, c.id, 0)
    L(c.id, 0, d.id, 0)
    L(d.id, 0, exitn.id, 0)
    # value 数据源链:纯文本 -> 拼接 -> 动态文本(真分支 b -> say -> d)
    lit = sc.new_node(M.N_TEXTLIT, 0, 0); lit.lit_text = "你好"
    txt = sc.new_node(M.N_TEXT, 0, 0)
    say = sc.new_node(M.N_SAY, 0, 0)
    L(b.id, 0, say.id, 0)
    L(say.id, 0, d.id, 0)
    L(lit.id, 0, txt.id, 0)
    L(txt.id, 0, say.id, 1)

    layout_mod.auto_layout(sc, [entry.id])

    rects = {}
    for n in sc.nodes:
        rects[n.id] = (n.x, n.y, n.x + 230, n.y + 100)
    ids = list(rects)
    overlap = False
    for i in range(len(ids)):
        for j in range(i + 1, len(ids)):
            a1, b1, c1, d1 = rects[ids[i]]
            a2, b2, c2, d2 = rects[ids[j]]
            if a1 < c2 and a2 < c1 and b1 < d2 and b2 < d1:
                overlap = True
    check("无重叠", not overlap)
    check("入口最左", entry.x <= a.x <= ifn.x,
          f"entry={entry.x} a={a.x} if={ifn.x}")
    check("分支在后方", b.x > ifn.x and c.x > ifn.x)
    check("汇合更后", d.x >= b.x and d.x >= c.x)
    check("数据源在消费方左侧", lit.x < txt.x < say.x,
          f"lit={lit.x} txt={txt.x} say={say.x}")


def test_sound_trans():
    print("声音/过渡块(BGM/SE/停止/切换/柔和背景/柔和切镜头)")
    p = M.project_defaults()
    p.scenes.append(M.scene_defaults())   # 场景 2 作为切镜头目标
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    bgm = sc.new_node(M.N_BGM, 0, 0); bgm.sound_path = "res/bgm.mp3"; bgm.sound_vol = 60
    se = sc.new_node(M.N_SE, 0, 0); se.sound_path = "res/se.wav"
    stop = sc.new_node(M.N_SOUND_STOP, 0, 0); stop.stop_target = 2
    swap = sc.new_node(M.N_SOUND_SWAP, 0, 0); swap.sound_path = "res/bgm2.mp3"
    bgtr = sc.new_node(M.N_BG_TRANS, 0, 0); bgtr.bg_path = "res/bg2.png"; bgtr.bg_trans_ms = 500
    sctr = sc.new_node(M.N_SCENE_TRANS, 0, 0); sctr.scene_trans_ms = 300
    check("声音块 1入1出", len(bgm.inputs) == 1 and len(bgm.outputs) == 1)
    check("过渡块 1入1出", len(bgtr.inputs) == 1 and len(sctr.outputs) == 1)
    L(entry.id, 0, bgm.id, 0)
    L(bgm.id, 0, se.id, 0)
    L(se.id, 0, stop.id, 0)
    L(stop.id, 0, swap.id, 0)
    L(swap.id, 0, bgtr.id, 0)
    L(bgtr.id, 0, sctr.id, 0)
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    sc2 = p2.scenes[0]
    bgm2 = [n for n in sc2.nodes if n.type == M.N_BGM]
    check("BGM 属性保留", len(bgm2) == 1 and bgm2[0].sound_path == "res/bgm.mp3"
          and bgm2[0].sound_vol == 60)
    tr2 = [n for n in sc2.nodes if n.type == M.N_SCENE_TRANS]
    check("切镜头属性保留", len(tr2) == 1 and tr2[0].scene_trans_ms == 300)
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("含BGM+音量", 'gal_volume(60);' in text
          and 'gal_play_bgm("res/bgm.mp3");' in text)
    check("含SE", 'gal_play_se("res/se.wav");' in text)
    check("含停止全部", "gal_stop_sound();" in text)
    check("含切歌", 'gal_play_bgm("res/bgm2.mp3");' in text)
    check("含柔和背景", 'gal_bg_trans("res/bg2.png", 500);' in text)
    check("含淡出切镜头", "gal_fade_out(300);" in text and "scene_1();" in text)
    check("含淡入", "gal_fade_in(0);" in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-300:])


def test_toast():
    print("消息提示块")
    p = M.project_defaults()
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    t = sc.new_node(M.N_TOAST, 0, 0)
    t.toast_text = "获得道具:神秘钥匙"
    t.toast_corner = 0
    t.toast_ms = 2000
    check("消息块 1入1出", len(t.inputs) == 1 and len(t.outputs) == 1)
    L(entry.id, 0, t.id, 0)
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    t2 = [n for n in p2.scenes[0].nodes if n.type == M.N_TOAST]
    check("属性保留", len(t2) == 1 and t2[0].toast_text == "获得道具:神秘钥匙"
          and t2[0].toast_corner == 0 and t2[0].toast_ms == 2000)
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("含消息提示", 'gal_toast("获得道具:神秘钥匙", 0, 2000);' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-300:])


def test_fileio_blocks():
    print("文件 IO 块(存档/读档/写/读)")
    p = M.project_defaults()
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id); exitn = sc.node(sc.exit_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    sv = sc.new_node(M.N_SAVE, 0, 0); sv.file_path = "save/档1.dat"
    ld = sc.new_node(M.N_LOAD, 0, 0); ld.file_path = "save/档1.dat"
    fw = sc.new_node(M.N_FILE_WRITE, 0, 0); fw.file_path = "log.txt"
    fr = sc.new_node(M.N_FILE_READ, 0, 0); fr.file_path = "log.txt"
    tl = sc.new_node(M.N_TEXTLIT, 0, 0); tl.lit_text = "存档成功"
    say = sc.new_node(M.N_SAY, 0, 0)
    check("存档块 1入1出", len(sv.inputs) == 1 and len(sv.outputs) == 1)
    check("写文件块 2入1出", len(fw.inputs) == 2 and len(fw.outputs) == 1)
    check("读文件块仅输出", len(fr.inputs) == 0 and len(fr.outputs) == 1)
    L(entry.id, 0, sv.id, 0)
    L(sv.id, 0, ld.id, 0)
    L(ld.id, 0, fw.id, 0)
    L(fw.id, 0, say.id, 0)
    L(say.id, 0, exitn.id, 0)
    L(tl.id, 0, fw.id, 1)     # 内容 ← 纯文本
    L(fr.id, 0, say.id, 1)    # 读文件 → 动态文本
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    sv2 = [n for n in p2.scenes[0].nodes if n.type == M.N_SAVE]
    check("存档属性保留", len(sv2) == 1 and sv2[0].file_path == "save/档1.dat")
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("含存档", 'gal_save_vars("save/档1.dat");' in text)
    check("含读档", 'gal_load_vars("save/档1.dat");' in text)
    check("含写文件", 'gal_file_write("log.txt", "存档成功");' in text)
    check("含读文件", 'gal_file_read("log.txt")' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-300:])


def test_checkpoint():
    print("存档点 + 跳转到存档点")
    p = M.project_defaults()
    p.scenes.append(M.scene_defaults())   # 场景2 放跳转块
    sc0 = p.scenes[0]
    sc1 = p.scenes[1]
    entry0 = sc0.node(sc0.entry_id); exit0 = sc0.node(sc0.exit_id)

    def L(sc, fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp))
        sc.lid += 1

    spkA = sc0.new_node(M.N_SPEAK, 0, 0); spkA.text = "第一段"
    ckA = sc0.new_node(M.N_CHECKPOINT, 0, 0)
    ckA.ck_name = "A"; ckA.file_path = "save/档1.dat"
    spkB = sc0.new_node(M.N_SPEAK, 0, 0); spkB.text = "第二段"
    ckB = sc0.new_node(M.N_CHECKPOINT, 0, 0)
    ckB.ck_name = "B"
    spkC = sc0.new_node(M.N_SPEAK, 0, 0); spkC.text = "第三段"
    L(sc0, entry0.id, 0, spkA.id, 0)
    L(sc0, spkA.id, 0, ckA.id, 0)
    L(sc0, ckA.id, 0, spkB.id, 0)
    L(sc0, spkB.id, 0, ckB.id, 0)
    L(sc0, ckB.id, 0, spkC.id, 0)
    L(sc0, spkC.id, 0, exit0.id, 0)
    # 场景2:跳转块
    jmp = sc1.new_node(M.N_JUMP, 0, 0)
    jmp.file_path = "save/档1.dat"
    L(sc1, sc1.node(sc1.entry_id).id, 0, jmp.id, 0)
    check("存档点 1入1出", len(ckA.inputs) == 1 and len(ckA.outputs) == 1)
    check("跳转 1入1出", len(jmp.inputs) == 1 and len(jmp.outputs) == 1)
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    ck = [n for n in p2.scenes[0].nodes if n.type == M.N_CHECKPOINT]
    check("存档点属性保留", len(ck) == 2 and ck[0].ck_name == "A"
          and ck[0].file_path == "save/档1.dat")
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("记录位置", 'gal_var_set("_ckpt", "A");' in text
          and 'gal_var_set("_ckpt_scene", gal_itoa(0));' in text)
    check("存档点写文件", 'gal_save_vars("save/档1.dat");' in text)
    check("进入点分支", 'let _enter = gal_var_get("_ckpt");' in text
          and 'if _enter == "A" {' in text and 'else if _enter == "B" {' in text
          and 'else {' in text)
    check("跳转读档", 'gal_load_vars("save/档1.dat");' in text
          and 'let _s = gal_var_num("_ckpt_scene");' in text
          and 'if _s == 0 {' in text and 'scene_0();' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-400:])


def test_menu_data_inputs():
    print("MENU 数据输入口 + 控件文字流化")
    p = M.project_defaults()
    # 菜单:数据入口「标题」→ 按钮文字,退出「确定」
    m = M.MENUScene("main")
    start = m.new_node(M.M_START, 0, 0)
    mn_in = m.new_node(M.M_IN, 60, 60)
    mn_in.param_name = "标题"
    btn = m.new_node(M.M_CTRL, 320, 60)
    btn.ctrl = "btn"; btn.ctype = 0
    mn_exit = m.new_node(M.M_EXIT, 320, 200); mn_exit.exit_name = "确定"
    p.menus.append(m)
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id); exitn = sc.node(sc.exit_id)

    def L(sc_, fn, fp, tn, tp):
        sc_.links.append(M.Link(sc_.lid, fn, fp, tn, tp))
        sc_.lid += 1

    L(m, start.id, 0, btn.id, 0)
    L(m, mn_in.id, 0, btn.id, 1)     # 数据入口 → 按钮「文字」
    L(m, btn.id, 0, mn_exit.id, 0)
    # 镜头:菜单块 + 传入字面值
    mnode = sc.new_node(M.N_MENU, 400, 100)
    mnode.menu_idx = 0; mnode.action = 0
    mnode.sync_menu_outputs(m)
    check("菜单块 输入口=exec+标题",
          [q.kind for q in mnode.inputs] == ["exec", "value"]
          and mnode.inputs[1].name == "标题", str(mnode.inputs))
    tl = sc.new_node(M.N_TEXTLIT, 300, 300); tl.lit_text = "欢迎光临"
    L(sc, entry.id, 0, mnode.id, 0)
    L(sc, mnode.id, 0, exitn.id, 0)  # 退出「确定」→ 出口
    L(sc, tl.id, 0, mnode.id, 1)     # 字面值 → 菜单块「标题」输入
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    mn2 = [n for n in p2.scenes[0].nodes if n.type == M.N_MENU][0]
    m2 = p2.menus[0]
    mn2.sync_menu_outputs(m2)
    check("往返后输入口保留", [q.kind for q in mn2.inputs] == ["exec", "value"])
    check("往返后 M_IN 保留", [n.param_name for n in m2.param_nodes()] == ["标题"])
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("菜单函数带形参", 'func menu_0_main(_mi0: string) -> string {' in text)
    check("镜头传参", 'let r = menu_0_main("欢迎光临");' in text)
    check("控件文字用形参", 'gal_menu_set("btn", "text", _mi0);' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-400:])


def test_checkpoint_demo():
    print("阶段1演示蓝图(存档点+跳转)")
    blue = os.path.join(ROOT, "examples", "gal", "demo_checkpoint.bluescene")
    if not os.path.exists(blue):
        check("演示蓝图存在", False, blue)
        return
    p = sceneio.load_project(blue)
    check("加载 2 镜头", len(p.scenes) == 2)
    cks = [n for n in p.scenes[0].nodes if n.type == M.N_CHECKPOINT]
    jmps = [n for n in p.scenes[1].nodes if n.type == M.N_JUMP]
    check("含存档点A", len(cks) == 1 and cks[0].ck_name == "A"
          and cks[0].file_path == "save/check1.dat")
    check("含跳转", len(jmps) == 1 and jmps[0].file_path == "save/check1.dat")
    d = tempfile.mkdtemp()
    path = ex.export_project(p, d)
    check("导出成功", bool(path))
    with open(path, encoding="utf-8") as f:
        text = f.read()
    check("进入点分支", 'let _enter = gal_var_get("_ckpt");' in text
          and 'if _enter == "A" {' in text and 'else {' in text)
    check("存档点写文件", 'gal_save_vars("save/check1.dat");' in text)
    check("跳转分派", 'gal_load_vars("save/check1.dat");' in text
          and 'if _s == 0 {' in text and 'scene_1();' in text)
    # 同场景多选项块不得重复声明 let(修复回归)
    r = subprocess.run([sys.executable, "main.py", "compile", path,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("演示蓝图编译通过", r.returncode == 0, r.stderr[-400:])
    check("选项变量唯一", text.count("let _p") >= 2, text[-200:])


def test_menu_input_demo():
    print("阶段2/3 演示蓝图(数据入口+控件流化)")
    blue = os.path.join(ROOT, "examples", "gal", "demo_menu_input.bluescene")
    if not os.path.exists(blue):
        check("演示蓝图存在", False, blue)
        return
    p = sceneio.load_project(blue)
    check("含菜单", len(p.menus) == 1)
    m = p.menus[0]
    check("含数据入口", [n.param_name for n in m.param_nodes()] == ["问候语"])
    clicks = [n for n in m.nodes if n.type == M.M_CLICK]
    check("含点击起点(ok_btn)", len(clicks) == 1 and clicks[0].target_ctrl == "ok_btn")
    ex_node = [n for n in m.nodes if n.type == M.M_EXIT]
    check("退出从点击起点触发(非开场链)",
          len(ex_node) == 1 and len(m.exec_parents(ex_node[0])) == 1
          and m.exec_parents(ex_node[0])[0].type == M.M_CLICK)
    mn = [n for n in p.scenes[0].nodes if n.type == M.N_MENU][0]
    check("菜单块输入口", [q.kind for q in mn.inputs] == ["exec", "value"]
          and mn.inputs[1].name == "问候语")
    d = tempfile.mkdtemp()
    path = ex.export_project(p, d)
    check("导出成功", bool(path))
    with open(path, encoding="utf-8") as f:
        text = f.read()
    check("菜单函数带形参", 'func menu_0_问候弹窗(_mi0: string) -> string {' in text)
    check("镜头传参", 'menu_0_问候弹窗("欢迎光临,冒险者!")' in text)
    check("控件文字用形参", 'gal_menu_set("ok_btn", "text", _mi0);' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("演示蓝图编译通过", r.returncode == 0, r.stderr[-400:])


def test_menu_ext_blocks():
    print("MENU 蓝图扩块(变量/消息/读输入框/跳转)")
    p = M.project_defaults()
    p.scenes.append(M.scene_defaults())   # 场景2 供跳转
    sc0 = p.scenes[0]
    sc1 = p.scenes[1]
    entry0 = sc0.node(sc0.entry_id); exit0 = sc0.node(sc0.exit_id)

    def L(sc_, fn, fp, tn, tp):
        sc_.links.append(M.Link(sc_.lid, fn, fp, tn, tp))
        sc_.lid += 1

    # 菜单1:输入名字 → 存变量 + 消息
    m = M.MENUScene("settings")
    start = m.new_node(M.M_START, 0, 0)
    btn = m.new_node(M.M_CTRL, 0, 0); btn.ctrl = "ok"; btn.ctype = 0
    inp = m.new_node(M.M_CTRL, 0, 0); inp.ctrl = "name_input"; inp.ctype = 4
    ninput = m.new_node(M.N_INPUT, 0, 0); ninput.ctrl = "name_input"
    sv = m.new_node(M.N_SETVAR, 0, 0)
    sv.var_name = "player_name"; sv.var_type = 2; sv.set_mode = 1
    toast = m.new_node(M.N_TOAST, 0, 0); toast.toast_text = "名字已保存"
    ck = m.new_node(M.M_CLICK, 0, 0); ck.target_ctrl = "ok"
    mn_exit = m.new_node(M.M_EXIT, 0, 0); mn_exit.exit_name = "确定"
    L(m, start.id, 0, btn.id, 0)
    L(m, ninput.id, 0, sv.id, 1)      # 输入框值 → 设置变量 A
    L(m, ck.id, 0, sv.id, 0)          # 点按钮 → 设置变量
    L(m, sv.id, 0, toast.id, 0)
    L(m, toast.id, 0, mn_exit.id, 0)
    p.menus.append(m)
    # 菜单2:跳转镜头
    m2 = M.MENUScene("jumpmenu")
    st2 = m2.new_node(M.M_START, 0, 0)
    bt2 = m2.new_node(M.M_CTRL, 0, 0); bt2.ctrl = "b"; bt2.ctype = 0
    ck2 = m2.new_node(M.M_CLICK, 0, 0); ck2.target_ctrl = "b"
    jp = m2.new_node(M.M_JUMP, 0, 0); jp.flow_scene = 1
    mn_exit2 = m2.new_node(M.M_EXIT, 0, 0); mn_exit2.exit_name = "退出"
    L(m2, st2.id, 0, bt2.id, 0)
    L(m2, ck2.id, 0, jp.id, 0)
    L(m2, jp.id, 0, mn_exit2.id, 0)
    p.menus.append(m2)
    # 场景0:菜单1 → 场景1:菜单2
    mnode = sc0.new_node(M.N_MENU, 300, 100)
    mnode.menu_idx = 0; mnode.action = 0
    mnode.sync_menu_outputs(m)
    L(sc0, entry0.id, 0, mnode.id, 0)
    L(sc0, mnode.id, 0, exit0.id, 0)
    mnode2 = sc1.new_node(M.N_MENU, 300, 100)
    mnode2.menu_idx = 1; mnode2.action = 0
    mnode2.sync_menu_outputs(m2)
    L(sc1, sc1.node(sc1.entry_id).id, 0, mnode2.id, 0)
    L(sc1, mnode2.id, 0, sc1.node(sc1.exit_id).id, 0)
    # 往返
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    m2_ = p2.menus[1]
    check("M_JUMP 往返保留", [n.flow_scene for n in m2_.nodes
          if n.type == M.M_JUMP] == [1])
    # 导出 + 编译
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("菜单内读输入框→变量",
          'gal_var_set("player_name", gal_menu_get_text("name_input"));' in text)
    check("菜单内消息", 'gal_toast("名字已保存", 3, 1500);' in text)
    check("菜单跳转结果", 'gal_menu_exit("@scene:1");' in text)
    check("镜头跳转分派", 'if r == "@scene:1" {' in text and 'scene_1();' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-400:])


def test_menu_actions_demo():
    print("阶段4 演示蓝图(菜单扩块)")
    blue = os.path.join(ROOT, "examples", "gal", "demo_menu_actions.bluescene")
    if not os.path.exists(blue):
        check("演示蓝图存在", False, blue)
        return
    p = sceneio.load_project(blue)
    check("含菜单", len(p.menus) == 1)
    m = p.menus[0]
    sv = [n for n in m.nodes if n.type == M.N_SETVAR]
    ck = [n for n in m.nodes if n.type == M.M_CLICK]
    check("菜单内设置变量", len(sv) == 1 and sv[0].var_name == "player_name")
    check("菜单内点击起点", len(ck) == 1 and ck[0].target_ctrl == "ok")
    d = tempfile.mkdtemp()
    path = ex.export_project(p, d)
    check("导出成功", bool(path))
    with open(path, encoding="utf-8") as f:
        text = f.read()
    check("读输入框→变量",
          'gal_var_set("player_name", gal_menu_get_text("name_input"));' in text)
    check("菜单内消息", 'gal_toast("名字已保存!", 3, 1500);' in text)
    check("镜头动态文本", 'gal_text(("你好, " + gal_var_get("player_name")));' in text)
    r = subprocess.run([sys.executable, "main.py", "compile", path,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("演示蓝图编译通过", r.returncode == 0, r.stderr[-400:])


def test_cat_demo():
    print("猫猫物语复刻演示(立绘/选项/变量/多场景)")
    blue = os.path.join(ROOT, "examples", "gal", "demo_cat.bluescene")
    if not os.path.exists(blue):
        check("演示蓝图存在", False, blue)
        return
    p = sceneio.load_project(blue)
    check("4 场景", len(p.scenes) == 4)
    sc0 = p.scenes[0]
    sprs = [n for n in sc0.nodes if n.type == M.N_SPRITE]
    chs = [n for n in sc0.nodes if n.type == M.N_CHOICE]
    dv = [n for n in sc0.nodes if n.type == M.N_DEFVAR]
    check("场景1 双立绘", len(sprs) == 2
          and sprs[0].spr_path == "res/cat_black.png"
          and sprs[1].spr_path == "res/cat_white.png"
          and sprs[0].spr_layer == 0 and sprs[1].spr_layer == 1)
    check("场景1 三选项", len(chs) == 1 and len(chs[0].options) == 3)
    check("好感度变量", len(dv) == 1 and dv[0].var_name == "affection")
    d = tempfile.mkdtemp()
    path = ex.export_project(p, d)
    check("导出成功", bool(path))
    with open(path, encoding="utf-8") as f:
        text = f.read()
    check("黄昏背景", 'gal_bg("res/park_dusk.jpg");' in text)
    check("双立绘导出", 'gal_sprite(0, "res/cat_black.png");' in text
          and 'gal_sprite(1, "res/cat_white.png");' in text)
    check("好感度递增", 'gal_var_set("affection", gal_itoa((gal_var_num("affection") + 10)));' in text)
    check("好感消息", 'gal_toast("[好感] 好感度 +10", 3, 1500);' in text)
    check("多场景函数", all(f"func scene_{i}() {{" in text for i in range(4)))
    r = subprocess.run([sys.executable, "main.py", "compile", path,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("演示蓝图编译通过", r.returncode == 0, r.stderr[-400:])


def test_long_char_controls():
    print("长对话立绘控制 + 人物状态名往返")
    p = M.project_defaults()
    ch = M.Character()
    ch.name = "小羽"
    ch.states[0] = "res/a.png"; ch.stateNames[0] = "微笑"
    ch.states[1] = "res/b.png"; ch.stateNames[1] = "生气"
    ch.nStates = 2
    p.chars.append(ch)
    fd, path = tempfile.mkstemp(suffix=".bluescene")
    os.close(fd)
    sceneio.save_project(path, p)
    p2 = sceneio.load_project(path)
    os.unlink(path)
    check("状态名往返保留", p2.chars[0].stateNames[:p2.chars[0].nStates] == ["微笑", "生气"],
          str(p2.chars[0].stateNames))
    check("立绘往返保留", p2.chars[0].states[:2] == ["res/a.png", "res/b.png"])
    # 长对话块:立绘位置=右, 适配=1
    sc = p2.scenes[0]
    entry = sc.node(sc.entry_id); exitn = sc.node(sc.exit_id)
    longn = sc.new_node(M.N_LONG, 0, 0)
    longn.lines = [(0, "第一行"), (-1, "旁白内容"), (0, "第二行")]
    longn.spr_pos_mode = 3
    longn.spr_fit = 1
    longn.long_dual = 0        # 单立绘模式:旁白行隐藏立绘(空白)
    sc.links.append(M.Link(sc.lid, entry.id, 0, longn.id, 0)); sc.lid += 1
    sc.links.append(M.Link(sc.lid, longn.id, 0, exitn.id, 0)); sc.lid += 1
    d = tempfile.mkdtemp()
    path2 = ex.export_project(p2, d)
    check("导出成功", bool(path2))
    with open(path2, encoding="utf-8") as f:
        text = f.read()
    check("长对话手动位置(右)", 'gal_sprite_pos_mode(0, 2);' in text, text[-300:])
    check("长对话立绘适配", 'gal_sprite_fit(0, 1);' in text)
    check("长对话块前清理立绘层", 'gal_sprite_show(0, 0);' in text
          and 'gal_sprite_show(1, 0);' in text)
    # 旁白行(ci=-1)前应隐藏所有立绘层,保持空白场景
    pi = text.find("旁白内容")
    pre = text[max(0, pi - 400):pi]
    check("旁白行隐藏立绘", "gal_sprite_show(0, 0);" in pre
          and "gal_sprite_show(1, 0);" in pre, pre[-200:])
    r = subprocess.run([sys.executable, "main.py", "compile", path2,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-400:])


def test_long_dual():
    print("长对话同屏双人/旁白隐藏配置")
    p = M.project_defaults()
    for name, img in (("A", "res/a.png"), ("B", "res/b.png")):
        ch = M.Character(); ch.name = name
        ch.states[0] = img; ch.stateNames[0] = "默认"; ch.nStates = 1
        p.chars.append(ch)
    sc = p.scenes[0]
    entry = sc.node(sc.entry_id); exitn = sc.node(sc.exit_id)

    def L(fn, fp, tn, tp):
        sc.links.append(M.Link(sc.lid, fn, fp, tn, tp)); sc.lid += 1

    longn = sc.new_node(M.N_LONG, 0, 0)
    longn.lines = [(0, "A 说话"), (1, "B 说话"), (0, "A 再说")]
    L(entry.id, 0, longn.id, 0); L(longn.id, 0, exitn.id, 0)
    d = tempfile.mkdtemp()
    # 默认:同屏双人
    path = ex.export_project(p, d)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    check("双人立绘都显示", 'gal_sprite(0, "res/a.png");' in text
          and 'gal_sprite(1, "res/b.png");' in text)
    check("同屏压暗另一人", 'gal_sprite_alpha(1, 90);' in text
          and 'gal_sprite_alpha(0, 90);' in text)
    # 关闭同屏:切换说话人时只留当前
    longn.long_dual = 0
    path = ex.export_project(p, d)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    check("单立绘模式无压暗", 'gal_sprite_alpha(1, 90);' not in text)

    def near_lines(txt, n=4):
        ls = txt.splitlines()
        for i, ln in enumerate(ls):
            if "旁白内容" in ln:
                return ls[max(0, i - n):i]
        return []

    # 同屏双人:旁白不打断立绘(保留双人)
    longn.long_dual = 1
    longn.long_narration_hide = 1
    longn.lines = [(0, "A 说话"), (-1, "旁白内容"), (1, "B 说话")]
    path = ex.export_project(p, d)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    seg = near_lines(text)
    check("同屏时旁白不隐藏", not any("gal_sprite_show" in x for x in seg), str(seg))
    # 单立绘模式:旁白隐藏(默认)
    longn.long_dual = 0
    longn.long_narration_hide = 1
    path = ex.export_project(p, d)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    seg = near_lines(text)
    check("单立绘时旁白隐藏", any("gal_sprite_show" in x for x in seg), str(seg))
    # 单立绘 + 关旁白隐藏 → 旁白保留
    longn.long_narration_hide = 0
    path = ex.export_project(p, d)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    seg = near_lines(text)
    check("单立绘关旁白隐藏则保留", not any("gal_sprite_show" in x for x in seg), str(seg))
    r = subprocess.run([sys.executable, "main.py", "compile", path,
                        "-o", os.path.join(d, "o.dexbc")],
                       capture_output=True, text=True, cwd=ROOT)
    check("编译通过", r.returncode == 0, r.stderr[-400:])


if __name__ == "__main__":
    test_model()
    test_roundtrip()
    test_vars()
    test_export_compile()
    test_new_blocks()
    test_scene_data()
    test_menu_data_outputs()
    test_literal_blocks()
    test_auto_layout()
    test_sound_trans()
    test_toast()
    test_fileio_blocks()
    test_checkpoint()
    test_checkpoint_demo()
    test_menu_data_inputs()
    test_menu_input_demo()
    test_menu_ext_blocks()
    test_menu_actions_demo()
    test_cat_demo()
    test_long_char_controls()
    test_long_dual()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    sys.exit(1 if FAIL else 0)
