#!/usr/bin/env python3
"""DEXCODE M2 测试:dexgame 的实体 / 组件 / 描述符驱动场景读写。

覆盖:
  1) 实体池与**世代句柄**(释放后旧 id 立即失效;槽位复用时不会误指新对象)
  2) 组件的挂载/查询/卸载;重复挂载与对已死对象操作要报错
  3) 字段读写:三种类型 + 类型不匹配必须被拒且给出可读原因
  4) 描述符表自省(节点式 IDE 生成属性面板要用它)
  5) 从组件渲染:位置、layer/order 排序、相机
  6) transform 层级(父链累加世界坐标)
  7) JSON 往返:内存 → JSON → 内存,**渲染结果像素一致**
  8) **向前兼容**:未知组件 / 未知字段必须被跳过而不是报错(这是 JSON 相对
     二进制格式的关键优势,docs/DEXGAME_DESIGN.md §7.4)
  9) 格式版本比支持的新 → 明确拒绝
 10) 落盘/读回;读不存在的文件要带得出原因
 11) 双 VM 一致性

运行: python tests/test_scene.py
"""

import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program, assemble, DexError  # noqa: E402
from dexlang.pyvm import run_program  # noqa: E402

LIBS = os.path.join(ROOT, "libs")
DLL = os.path.join(LIBS, "dexgame", "libdexgame.dll")
ATLAS = "tests/fixtures/atlas2x2.png"          # 相对进程 CWD(VM 在仓库根跑)
VM = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
TMP_SRC = os.path.join(ROOT, "_scene_test.dex")
TMP_BC = os.path.join(ROOT, "_scene_test.dexbc")
TMP_JSON = os.path.join(ROOT, "_scene_test.json")

PASS = 0
FAIL = 0
SKIP = 0

# 图集四个纹素的颜色(与 tests/fixtures/make_atlas.py 一致)
RED, GREEN, BLUE, WHITE = 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF
BLACK = 0xFF000000


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def get(lines, key, default=None):
    """按 `key=value` 取值 —— 比数行号稳,增删一行不会让整段错位。"""
    pre = key + "="
    for l in lines:
        if l.startswith(pre):
            return l[len(pre):]
    return default


def skip(name, why):
    global SKIP
    SKIP += 1
    print(f"  SKIP  {name}  ({why})")


def compile_src(src):
    with open(TMP_SRC, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    toks = Lexer(src, TMP_SRC).tokenize()
    ast = Parser(toks, TMP_SRC).parse_program()
    return compile_program(ast, source_path=TMP_SRC, include_dirs=[LIBS])


def run_c(bc):
    with open(TMP_BC, "wb") as f:
        f.write(bc)
    r = subprocess.run([VM, TMP_BC], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=60)
    return r.returncode, [l for l in r.stdout.splitlines() if l != ""], r.stderr


def run_dex(src):
    bc = assemble(compile_src(src).to_program())
    return run_c(bc)


def need_vm():
    return os.path.exists(DLL) and os.path.exists(VM)


# ---------- 1) 实体与世代句柄 ----------
def test_entities():
    print("[实体与世代句柄]")
    if not need_vm():
        skip("实体", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(32, 32);
let a = eng_object_new();
let b = eng_object_new();
print a;
print b;
print eng_object_count();
print eng_object_alive(a);
print eng_object_free(a);
print eng_object_alive(a);
let c = eng_object_new();
print c;
print c != a;
print eng_object_alive(a);
print eng_object_count();
print eng_scene_clear();
print eng_object_count();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("第一个实体 id 非 0", L[0] != "0", L[0:1])
    check("两个实体 id 不同", L[0] != L[1], L[0:2])
    check("实体计数 = 2", L[2] == "2", L[2:3])
    check("实体存活", L[3] == "1", L[3:4])
    check("释放返回 0", L[4] == "0", L[4:5])
    check("释放后立即失效", L[5] == "0", L[5:6])
    check("槽位复用后拿到新 id", L[7 - 1] != "0" and L[7] == "1", L[6:8])
    check("旧 id 仍失效(不会误指新对象)", L[8] == "0", L[8:9])
    check("计数 = 2(存活的 b + 复用槽位的 c)", L[9] == "2", L[9:10])
    check("clear 返回 0", L[10] == "0", L[10:11])
    check("clear 后计数 0", L[11] == "0", L[11:12])


# ---------- 2) 组件挂载 ----------
def test_components():
    print("[组件挂载/卸载]")
    if not need_vm():
        skip("组件", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
let a = eng_object_new();
print eng_has(a, "sprite");
print eng_attach(a, "transform");
print eng_attach(a, "sprite");
print eng_has(a, "sprite");
print eng_attach(a, "sprite");
print "err=" + eng_last_error();
print eng_detach(a, "sprite");
print eng_has(a, "sprite");
print eng_detach(a, "sprite");
print eng_attach(a, "nosuch");
print "err2=" + eng_last_error();
eng_object_free(a);
print eng_attach(a, "transform");
print "err3=" + eng_last_error();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("初始无 sprite", L[0] == "0", L[0:1])
    check("挂载 transform = 0", L[1] == "0", L[1:2])
    check("挂载 sprite = 0", L[2] == "0", L[2:3])
    check("has sprite = 1", L[3] == "1", L[3:4])
    check("重复挂载被拒", L[4] == "-1", L[4:5])
    check("重复挂载原因可读", L[5].startswith("err=") and "already" in L[5], L[5:6])
    check("卸载返回 0", L[6] == "0", L[6:7])
    check("卸载后 has = 0", L[7] == "0", L[7:8])
    check("重复卸载被拒", L[8] == "-1", L[8:9])
    check("未知组件被拒", L[9] == "-1", L[9:10])
    check("未知组件原因列出可用名",
          "transform" in L[10] and "sprite" in L[10], L[10:11])
    check("对已死对象挂载被拒", L[11] == "-1", L[11:12])
    check("原因指出对象不存活", "alive" in L[12], L[12:13])


# ---------- 3) 字段读写与类型检查 ----------
def test_fields():
    print("[字段读写与类型检查]")
    if not need_vm():
        skip("字段", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "sprite");
print eng_set_f(a, "transform", "x", 12.5);
print eng_get_f(a, "transform", "x");
print eng_set_i(a, "sprite", "layer", 3);
print eng_get_i(a, "sprite", "layer");
print eng_set_s(a, "sprite", "tex_path", "%s");
print eng_get_s(a, "sprite", "tex_path");
print eng_set_i(a, "transform", "x", 1);
print "err=" + eng_last_error();
print eng_set_f(a, "sprite", "layer", 1.0);
print "err2=" + eng_last_error();
print eng_set_s(a, "sprite", "layer", "x");
print "err3=" + eng_last_error();
print eng_set_f(a, "sprite", "nofield", 1.0);
print "err4=" + eng_last_error();
print eng_get_f(a, "transform", "nofield");
print "err5=" + eng_last_error();
eng_shutdown();
''' % ATLAS)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("设浮点返回 0", L[0] == "0", L[0:1])
    check("读到刚设的浮点 12.5", L[1] == "12.5", L[1:2])
    check("设整数返回 0", L[2] == "0", L[2:3])
    check("读到刚设的整数 3", L[3] == "3", L[3:4])
    check("设字符串返回 0", L[4] == "0", L[4:5])
    check("读回字符串一致", L[5] == ATLAS, L[5:6])
    check("int 写 float 字段被拒", L[6] == "-1", L[6:7])
    check("类型错误原因指名道姓",
          "float" in L[7] and "int" in L[7], L[7:8])
    check("float 写 int 字段被拒", L[8] == "-1", L[8:9])
    check("string 写 int 字段被拒", L[10] == "-1", L[10:11])
    check("未知字段被拒", L[12] == "-1", L[12:13])
    check("未知字段原因可读", "no field" in L[13] or "nofield" in L[13], L[13:14])
    check("读未知字段也报错", L[15].startswith("err5=") and len(L[15]) > 5, L[15:16])


# ---------- 4) 描述符表自省 ----------
def test_introspection():
    print("[描述符表自省(给 IDE 用)]")
    if not need_vm():
        skip("自省", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
print eng_comp_count();
print "n1=[" + eng_comp_name_at(1) + "]";
print "n2=[" + eng_comp_name_at(2) + "]";
print "n99=[" + eng_comp_name_at(99) + "]";
print "err=" + eng_last_error();
print eng_field_count("sprite");
print "f0=[" + eng_field_name("sprite", 0) + "]";
print "f1=[" + eng_field_name("sprite", 1) + "]";
print eng_field_type("sprite", 0);
print eng_field_type("sprite", 1);
print eng_field_persist("sprite", 0);
print eng_field_persist("sprite", 1);
print eng_field_persist("sprite", 5);
print eng_field_count("nosuch");
print "err2=" + eng_last_error();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("组件种类数 = 8(M4 起含 audio)", L[0] == "8", L[0:1])
    check("第 1 种是 transform", L[1] == "n1=[transform]", L[1:2])
    check("第 2 种是 sprite", L[2] == "n2=[sprite]", L[2:3])
    check("越界索引返回空串", L[3] == "n99=[]", repr(L[3]))
    check("越界索引给出原因", "out of range" in L[4], L[4:5])
    check("sprite 字段数 = 12", L[5] == "12", L[5:6])
    check("字段 0 是 tex_path", L[6] == "f0=[tex_path]", L[6:7])
    check("字段 1 是 texture", L[7] == "f1=[texture]", L[7:8])
    check("tex_path 类型 = 3(字符串)", L[8] == "3", L[8:9])
    check("texture 类型 = 0(整数)", L[9] == "0", L[9:10])
    check("tex_path 是持久字段", L[10] == "1", L[10:11])
    check("**texture 是运行时字段(persist=0)**", L[11] == "0", L[11:12])
    check("px 是持久字段(该字段存在且 persist=1)", L[12] == "1", L[12:13])
    check("未知组件取字段数 = -1", L[13] == "-1", L[13:14])
    check("未知组件原因可读", "sprite" in L[14], L[14:15])


# ---------- 5) 渲染:位置 / 排序 / 相机 / 层级 ----------
def _scene_src(body, w=32, h=32):
    return ('include "dexgame";\n'
            'eng_init_offscreen(%d, %d);\n' % (w, h)) + body + 'eng_shutdown();\n'


def test_render_components():
    print("[从组件渲染:位置]")
    if not need_vm():
        skip("渲染组件", "需要 DLL 与 vm.exe")
        return
    # 每个精灵:px/py=0、sw/sh=2 → 覆盖 world (x,y) 起 2x2 的屏幕块
    src = _scene_src('''
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "sprite");
eng_set_f(a, "transform", "x", 4.0);
eng_set_f(a, "transform", "y", 6.0);
eng_set_s(a, "sprite", "tex_path", "%s");
eng_set_f(a, "sprite", "px", 0.0);
eng_set_f(a, "sprite", "py", 0.0);
eng_set_f(a, "sprite", "sw", 2.0);
eng_set_f(a, "sprite", "sh", 2.0);
eng_set_clear_color(0xFF000000);
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(4, 6);
print eng_pixel(5, 7);
print eng_pixel(6, 8);
eng_draw_scene();
''' % ATLAS)
    rc, L, err = run_dex(src)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("绘制了 1 个精灵", L[0] == "1", L[0:1])
    check("spawn 点像素 = 红纹素(图集 (0,0))", L[1] == str(RED), L[1:2])
    check("块内右下像素 = 白纹素(图集 (1,1),1:1 纹素映射)",
          L[2] == str(WHITE), L[2:3] + ["期望 " + str(WHITE)])
    check("块外像素 = 清除色", L[3] == str(BLACK), L[3:4])


def test_sprite_texel_rule():
    print("[单纹素精灵:必须采样纹素中心(不掺邻居)]")
    if not need_vm():
        skip("纹素规则", "需要 DLL 与 vm.exe")
        return
    # 取图集 (1,0) 的绿色单纹素,放大 4 倍画到屏幕 8..12。
    # 如果 uv 用区域边界(0.5..1.0),线性过滤会把左边的红色掺进来 —— M1 踩过。
    src = _scene_src('''
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "sprite");
eng_set_f(a, "transform", "x", 2.0);
eng_set_f(a, "transform", "y", 2.0);
eng_set_s(a, "sprite", "tex_path", "%s");
eng_set_f(a, "sprite", "sx", 1.0);
eng_set_f(a, "sprite", "sy", 0.0);
eng_set_f(a, "sprite", "sw", 1.0);
eng_set_f(a, "sprite", "sh", 1.0);
eng_set_f(a, "sprite", "px", 0.0);
eng_set_f(a, "sprite", "py", 0.0);

let cam = eng_object_new();
eng_attach(cam, "transform");
eng_attach(cam, "camera");
eng_set_f(cam, "camera", "zoom", 4.0);
eng_set_i(cam, "camera", "active", 1);

eng_set_clear_color(0xFF000000);
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(8, 8);
print eng_pixel(11, 11);
print eng_pixel(9, 10);
''' % ATLAS)
    rc, L, err = run_dex(src)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("绘制了 1 个精灵", L[0] == "1", L[0:1])
    check("放大 4 倍左上角仍是纯绿", L[1] == str(GREEN), L[1:2])
    check("放大 4 倍右下角仍是纯绿", L[2] == str(GREEN), L[2:3])
    check("**块中心没有掺进邻居的红色**", L[3] == str(GREEN), L[3:4])


def test_layer_order():
    print("[从组件渲染:layer/order 排序]")
    if not need_vm():
        skip("排序", "需要 DLL 与 vm.exe")
        return
    # 两个精灵完全重叠:layer 高的必须盖住低的
    src = _scene_src('''
let lo = eng_object_new();
eng_attach(lo, "transform");
eng_attach(lo, "sprite");
eng_set_s(lo, "sprite", "tex_path", "%s");
eng_set_f(lo, "sprite", "px", 0.0);
eng_set_f(lo, "sprite", "py", 0.0);
eng_set_f(lo, "sprite", "sw", 2.0);
eng_set_f(lo, "sprite", "sh", 2.0);
eng_set_i(lo, "sprite", "layer", 0);

let hi = eng_object_new();
eng_attach(hi, "transform");
eng_attach(hi, "sprite");
eng_set_f(hi, "transform", "x", 4.0);
eng_set_f(hi, "transform", "y", 4.0);
eng_set_s(hi, "sprite", "tex_path", "%s");
eng_set_f(hi, "sprite", "px", 0.0);
eng_set_f(hi, "sprite", "py", 0.0);
eng_set_f(hi, "sprite", "sw", 2.0);
eng_set_f(hi, "sprite", "sh", 2.0);
eng_set_i(hi, "sprite", "layer", 5);
eng_set_f(hi, "sprite", "sx", 1.0);   // 取绿色纹素
eng_set_i(hi, "sprite", "tint", 0xFFFFFFFF);

eng_set_clear_color(0xFF000000);
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(4, 4);
eng_shutdown();
''' % (ATLAS, ATLAS))
    rc, L, err = run_dex(src)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("绘制了 2 个精灵", L[0] == "2", L[0:2])
    check("重叠处被高 layer 覆盖(绿)",
          L[1] == str(GREEN), L[1:2] + ["期望 " + str(GREEN)])


def test_camera():
    print("[从组件渲染:相机]")
    if not need_vm():
        skip("相机", "需要 DLL 与 vm.exe")
        return
    src = _scene_src('''
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "sprite");
eng_set_f(a, "transform", "x", 8.0);
eng_set_f(a, "transform", "y", 8.0);
eng_set_s(a, "sprite", "tex_path", "%s");
eng_set_f(a, "sprite", "px", 0.0);
eng_set_f(a, "sprite", "py", 0.0);
eng_set_f(a, "sprite", "sw", 2.0);
eng_set_f(a, "sprite", "sh", 2.0);

let cam = eng_object_new();
eng_attach(cam, "transform");
eng_attach(cam, "camera");
eng_set_f(cam, "camera", "x", 8.0);
eng_set_f(cam, "camera", "y", 8.0);
eng_set_i(cam, "camera", "active", 1);

eng_set_clear_color(0xFF000000);
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(0, 0);
print eng_pixel(8, 8);
eng_shutdown();
''' % ATLAS)
    rc, L, err = run_dex(src)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("绘制了 1 个精灵(相机本身不画)", L[0] == "1", L[0:1])
    check("相机 (8,8) 把世界 (8,8) 移到屏幕 (0,0)", L[1] == str(RED), L[1:2])
    check("原位置屏幕 (8,8) 变空", L[2] == str(BLACK), L[2:3])


def test_hierarchy():
    print("[transform 层级与世界坐标]")
    if not need_vm():
        skip("层级", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(32, 32);
let parent = eng_object_new();
eng_attach(parent, "transform");
eng_set_f(parent, "transform", "x", 10.0);
eng_set_f(parent, "transform", "y", 20.0);
let child = eng_object_new();
eng_attach(child, "transform");
eng_set_f(child, "transform", "x", 3.0);
eng_set_f(child, "transform", "y", 4.0);
eng_set_i(child, "transform", "parent", parent);
print eng_world_x(child);
print eng_world_y(child);
print eng_world_x(parent);
eng_set_f(parent, "transform", "x", 100.0);
print eng_world_x(child);
let root = eng_object_new();
eng_attach(root, "transform");
print eng_world_x(root);
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("子世界 x = 父(10) + 子(3) = 13", L[0] == "13", L[0:1])
    check("子世界 y = 父(20) + 子(4) = 24", L[1] == "24", L[1:2])
    check("父自己世界 x = 10", L[2] == "10", L[2:3])
    check("移动父后子跟随 = 103", L[3] == "103", L[3:4])
    check("无父对象世界 x = 0", L[4] == "0", L[4:5])


# ---------- 6) JSON 往返 ----------
ROUNDTRIP_SRC = '''include "dexgame";
eng_init_offscreen(32, 32);

let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "sprite");
eng_set_name(a, "player");            // 名字也要跟场景一起存
eng_set_f(a, "transform", "x", 4.0);
eng_set_f(a, "transform", "y", 4.0);
eng_set_s(a, "sprite", "tex_path", "%s");
eng_set_f(a, "sprite", "px", 0.0);
eng_set_f(a, "sprite", "py", 0.0);
eng_set_f(a, "sprite", "sw", 2.0);
eng_set_f(a, "sprite", "sh", 2.0);

let b = eng_object_new();
eng_attach(b, "transform");
eng_attach(b, "sprite");
eng_set_f(b, "transform", "x", 10.0);
eng_set_f(b, "transform", "y", 12.0);
eng_set_i(b, "transform", "parent", a);
eng_set_s(b, "sprite", "tex_path", "%s");
eng_set_f(b, "sprite", "px", 0.0);
eng_set_f(b, "sprite", "py", 0.0);
eng_set_f(b, "sprite", "sw", 2.0);
eng_set_f(b, "sprite", "sh", 2.0);
eng_set_f(b, "sprite", "sx", 1.0);      // 绿色纹素
eng_set_i(b, "sprite", "layer", 3);

eng_set_clear_color(0xFF000000);
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(4, 4);
print eng_pixel(14, 16);
print eng_world_x(b);
print eng_scene_save("%s");
eng_scene_clear();
print eng_object_count();
print eng_scene_load("%s");
print eng_object_count();
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(4, 4);
print eng_pixel(14, 16);
eng_object_free(a);
print eng_object_alive(a);
let n = eng_object_new();
print n != 0;
print eng_object_alive(n);
eng_object_free(n);
print eng_object_count();
print "found=" + eng_find("player");       // 加载后名字应该还在
print "found_name=" + eng_name(eng_find("player"));
eng_shutdown();
'''


def _roundtrip_src():
    import pathlib
    j = pathlib.Path(TMP_JSON).name
    return ROUNDTRIP_SRC % (ATLAS, ATLAS, j, j)


def test_roundtrip():
    print("[JSON 往返:渲染结果必须一致]")
    if not need_vm():
        skip("往返", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(_roundtrip_src())
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("往返前绘制 2 个精灵", L[0] == "2", L[0:2])
    check("往返前 (4,4) = 红", L[1] == str(RED), L[1:2])
    check("往返前子对象 (14,16) = 绿(父4+子10, 父4+子12)", L[2] == str(GREEN), L[2:3])
    check("往返前子世界 x = 14", L[3] == "14", L[3:4])
    check("保存返回 0", L[4] == "0", L[4:5])
    check("clear 后计数 0", L[5] == "0", L[5:6])
    check("加载返回 0", L[6] == "0", L[6:7])
    check("加载后计数 2", L[7] == "2", L[7:8])
    check("往返后绘制 2 个精灵", L[8] == "2", L[8:9])
    check("往返后 (4,4) 仍是红", L[9] == str(RED), L[9:10])
    check("往返后 (14,16) 仍是绿 —— 层级与纹理都保住了",
          L[10] == str(GREEN), L[10:11])
    check("**加载后旧 id 立即失效(代际句柄)**", L[11] == "0", L[11:12])
    check("**名字随场景存下来了(eng_find)**", get(L, "found") not in (None, "0"),
          get(L, "found"))
    check("找到的名字就是 player", get(L, "found_name") == "player",
          get(L, "found_name"))
    check("加载后新建实体拿到非 0 句柄", L[12] == "1", L[12:13])
    check("加载后新建实体存活", L[13] == "1", L[13:14])
    check("释放后计数回到 2", L[14] == "2", L[14:15])


def test_json_shape():
    print("[JSON 结构]")
    if not need_vm():
        skip("JSON 结构", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(_roundtrip_src())
    if rc != 0 or not os.path.exists(TMP_JSON):
        check("场景文件已生成", False, err[:200])
        return
    with open(TMP_JSON, encoding="utf-8") as f:
        doc = json.load(f)
    check("是合法 JSON", isinstance(doc, dict))
    check("format = 1", doc.get("format") == 1, doc.get("format"))
    check("count = 2", doc.get("count") == 2, doc.get("count"))
    objs = doc.get("objects")
    check("objects 是数组且长度 2", isinstance(objs, list) and len(objs) == 2)
    if not (isinstance(objs, list) and len(objs) == 2):
        return
    check("对象带 index", "index" in objs[0], list(objs[0].keys()))
    check("含 transform 组件", "transform" in objs[0])
    check("含 sprite 组件", "sprite" in objs[0])
    check("**运行时字段 texture 不进 JSON**", "texture" not in objs[0]["sprite"],
          list(objs[0]["sprite"].keys()))
    check("持久字段 tex_path 进 JSON", objs[0]["sprite"].get("tex_path") == ATLAS,
          objs[0]["sprite"].get("tex_path"))
    check("浮点写成数字而不是字符串",
          isinstance(objs[0]["transform"].get("x"), (int, float)),
          type(objs[0]["transform"].get("x")).__name__)
    # 第二个对象通过场景内索引引用父对象(而不是运行时实体 id)
    check("parent 存的是场景内索引 0", objs[1]["transform"].get("parent") == 0,
          objs[1]["transform"].get("parent"))
    check("**parent 不是运行时实体 id**",
          objs[1]["transform"].get("parent") != 1, objs[1]["transform"].get("parent"))


# ---------- 7) 向前兼容 / 版本 ----------
def test_forward_compat():
    print("[向前兼容:未知组件/字段被跳过]")
    if not need_vm():
        skip("向前兼容", "需要 DLL 与 vm.exe")
        return
    doc = {
        "format": 1,
        "count": 1,
        "future_top_level": {"whatever": [1, 2, 3]},        # 顶层未知键
        "objects": [{
            "index": 0,
            "transform": {"x": 4, "y": 4, "future_field": "ignored", "sx": 1, "sy": 1,
                          "rot": 0, "parent": -1},
            "sprite": {"tex_path": ATLAS, "px": 0, "py": 0, "sw": 2, "sh": 2,
                       "sx": 0, "sy": 0, "tint": -1, "flip": 0, "layer": 0, "order": 0},
            "future_component": {"a": 1, "b": [2, 3], "c": {"d": 4}},   # 未知组件
        }],
    }
    with open(TMP_JSON, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    name = os.path.basename(TMP_JSON)
    name_out = os.path.basename(TMP_JSON.replace(".json", "_out.json"))
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(32, 32);
print eng_scene_load("%s");
print eng_object_count();
eng_set_clear_color(0xFF000000);
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(4, 4);
print "err=[" + eng_last_error() + "]";
print eng_scene_save("%s");
eng_shutdown();
''' % (name, name_out))
    check("含未知组件/字段的场景仍能加载", rc == 0 and L[0] == "0", (rc, L[:3], err[:200]))
    if rc != 0 or L[0] != "0":
        return
    check("对象数 = 1", L[1] == "1", L[1:2])
    check("已知组件解析成功且能渲染", L[2] == "1" and L[3] == str(RED), L[2:5])
    check("加载后无残留错误", L[4] == "err=[]", L[4:5])
    check("回写场景成功", len(L) > 5 and L[5] == "0", L[5:6])
    # 未知的东西必须被**丢掉**而不是留在内存里(回写一遍即可验证)
    out_path = TMP_JSON.replace(".json", "_out.json")
    if not os.path.exists(out_path):
        check("回写文件存在", False, out_path)
        return
    with open(out_path, encoding="utf-8") as f:
        back = json.load(f)
    o0 = back["objects"][0]
    check("已知组件 sprite 被保留", "sprite" in o0, list(o0.keys()))
    check("**未知字段被丢弃**", "future_field" not in o0["transform"],
          list(o0["transform"].keys()))
    check("**未知组件被丢弃**", "future_component" not in o0, list(o0.keys()))
    check("**顶层未知键被丢弃**", "future_top_level" not in back, list(back.keys()))


def test_format_version():
    print("[格式版本比支持的新 → 明确拒绝]")
    if not need_vm():
        skip("版本", "需要 DLL 与 vm.exe")
        return
    with open(TMP_JSON, "w", encoding="utf-8") as f:
        json.dump({"format": 999, "count": 0, "objects": []}, f)
    name = os.path.basename(TMP_JSON)
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
print eng_scene_load("%s");
print "err=[" + eng_last_error() + "]";
eng_shutdown();
''' % name)
    check("返回 -1", L and L[0] == "-1", L[:1])
    check("原因指出格式过新",
          len(L) > 1 and "newer" in L[1] and "999" in L[1], L[1:2])


def test_missing_file():
    print("[读不存在的场景文件]")
    if not need_vm():
        skip("缺失文件", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
print eng_scene_load("_no_such_scene_xyz.json");
print "err=[" + eng_last_error() + "]";
print eng_scene_save("_no_such_dir_xyz/a.json");
print "err2=[" + eng_last_error() + "]";
eng_shutdown();
''')
    check("加载失败返回 -1", L and L[0] == "-1", L[:1])
    check("原因含文件名", len(L) > 1 and "_no_such_scene_xyz" in L[1], L[1:2])
    check("保存失败返回 -1", len(L) > 2 and L[2] == "-1", L[2:3])
    check("保存失败原因可读", len(L) > 3 and "_no_such_dir_xyz" in L[3], L[3:4])


def test_bad_texture_path():
    print("[资源路径错误要立刻报]")
    if not need_vm():
        skip("坏路径", "需要 DLL 与 vm.exe")
        return
    # 场景文件里引用不存在的贴图:必须在**加载期**失败,而且要留下不半成品的场景
    with open(TMP_JSON, "w", encoding="utf-8") as f:
        json.dump({"format": 1, "count": 1, "objects": [{
            "index": 0,
            "transform": {"x": 0, "y": 0, "rot": 0, "sx": 1, "sy": 1, "parent": -1},
            "sprite": {"tex_path": "no/such/tex.png", "sx": 0, "sy": 0, "sw": 0, "sh": 0,
                       "px": 0, "py": 0, "tint": -1, "flip": 0, "layer": 0, "order": 0},
        }]}, f)
    name = os.path.basename(TMP_JSON)
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
let a = eng_object_new();
eng_attach(a, "sprite");
print eng_set_s(a, "sprite", "tex_path", "no/such/image.png");
print "err=[" + eng_last_error() + "]";
print eng_scene_load("%s");
print "err2=[" + eng_last_error() + "]";
print eng_object_count();
print eng_scene_clear();
eng_shutdown();
''' % name)
    check("返回 -1", L and L[0] == "-1", L[:1])
    check("原因含路径", len(L) > 1 and "no/such/image.png" in L[1], L[1:2])
    check("**坏贴图的场景加载失败**", len(L) > 2 and L[2] == "-1", L[2:3])
    check("加载失败原因含贴图路径", len(L) > 3 and "no/such/tex.png" in L[3], L[3:4])
    check("**加载失败不留半成品场景(计数 0)**",
          len(L) > 4 and L[4] == "0", L[4:5])


def test_load_json_direct():
    print("[从字符串加载场景]")
    if not need_vm():
        skip("字符串加载", "需要 DLL 与 vm.exe")
        return
    doc = {"format": 1, "count": 1, "objects": [{
        "index": 0,
        "transform": {"x": 5, "y": 5, "rot": 0, "sx": 1, "sy": 1, "parent": -1},
        "sprite": {"tex_path": ATLAS, "px": 0, "py": 0, "sw": 2, "sh": 2,
                   "sx": 0, "sy": 0, "tint": -1, "flip": 0, "layer": 0, "order": 0},
    }]}
    text = json.dumps(doc).replace('"', '\\"')
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(32, 32);
print eng_scene_load_json("%s");
print eng_object_count();
eng_set_clear_color(0xFF000000);
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(5, 5);
eng_shutdown();
''' % text)
    check("加载成功", L and L[0] == "0", (L[:1], err[:200]))
    check("对象数 1", len(L) > 1 and L[1] == "1", L[1:2])
    check("渲染正确", len(L) > 2 and L[2] == "1" and L[3] == str(RED), L[2:4])


# ---------- 8) 双 VM 一致性 ----------
DUAL_SRC = '''include "dexgame";
eng_init_offscreen(16, 16);
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "sprite");
eng_set_f(a, "transform", "x", 3.0);
eng_set_s(a, "sprite", "tex_path", "%s");
eng_set_f(a, "sprite", "px", 0.0);
eng_set_f(a, "sprite", "py", 0.0);
eng_set_f(a, "sprite", "sw", 2.0);
eng_set_f(a, "sprite", "sh", 2.0);
eng_set_clear_color(0xFF000000);
eng_frame_begin();
print eng_draw_scene();
eng_frame_end();
print eng_pixel(3, 3);
print eng_world_x(a);
print eng_object_alive(a);
eng_shutdown();
'''


def test_dual_vm():
    print("[双 VM 一致性]")
    if not need_vm():
        skip("双 VM", "需要 DLL 与 vm.exe")
        return
    unit = compile_src(DUAL_SRC % ATLAS)
    prog = unit.to_program()
    py_out, py_err = run_program(prog)
    rc, c_out, err = run_c(assemble(prog))
    check("pyvm 无错误", py_err is None, str(py_err))
    check("C VM 退出码 0", rc == 0, err[:200])
    check("两个 VM 结果逐行一致", py_out == c_out, f"py={py_out} c={c_out}")


def main():
    print("DEXCODE M2:dexgame 实体 / 组件 / 场景测试")
    test_entities()
    test_components()
    test_fields()
    test_introspection()
    test_render_components()
    test_sprite_texel_rule()
    test_layer_order()
    test_camera()
    test_hierarchy()
    test_roundtrip()
    test_json_shape()
    test_forward_compat()
    test_format_version()
    test_missing_file()
    test_bad_texture_path()
    test_load_json_direct()
    test_dual_vm()
    for p in (TMP_SRC, TMP_BC, TMP_JSON, TMP_JSON.replace(".json", "_out.json"),
              os.path.join(ROOT, "_scene_test.dxasm"), os.path.join(ROOT, "_m2scene.json")):
        if os.path.exists(p):
            os.remove(p)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
