#!/usr/bin/env python3
"""DEXCODE M3 测试:dexgame 的碰撞 / 查询 / 运动学 / 瓦片地图。

覆盖:
  1) 形状与重叠:AABB / 圆 / 竖直胶囊,以及 layer/mask 过滤
  2) **游标式查询**(语言没有数组):rect / circle / point、count/at/next/reset/end、
     嵌套、游标上限、关闭后的报错
  3) 射线与扫掠:**结果槽**语义(最近命中、法线、t、打到地形)
  4) move-and-slide:轴分离解算的**精确**落点、贴墙滑动、防穿透子步
  5) 动态体:落地静止、休眠/唤醒、弹性、gravity_scale
  6) **固定步长**累加器:每次调用跑几个子步、alpha、时间累积、两次运行逐行一致
  7) 瓦片地图:CSV 加载/保存、实心判定、网格碰撞、批量渲染(纯色像素断言)
  8) 渲染插值:120Hz 物理 + 更高刷新率时按 alpha 取中间位置(像素断言)
  9) 失败必须带原因(关闭的游标、零距离射线、越界瓦片、坏 CSV…)
 10) 双 VM 一致性

运行: python tests/test_phys.py
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
TMP_SRC = os.path.join(ROOT, "_phys_test.dex")
TMP_BC = os.path.join(ROOT, "_phys_test.dexbc")
TMP_CSV = os.path.join(ROOT, "_phys_test.csv")

PASS = 0
FAIL = 0
SKIP = 0

RED, GREEN, BLUE, WHITE = 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF
BLACK = 0xFF000000

STEP = "1.0 / 120.0"          # **精确的** 1/120:写字面量 0.008333333 会比 1/120 略小,
                              # 累加器会周期性少跑一个子步(实测第 4 次调用就少跑了)
STEP15 = "1.5 / 120.0"        # 1.5 个子步 → 跑 1 个,alpha ≈ 0.5


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
                       encoding="utf-8", errors="replace", timeout=120)
    return r.returncode, [l for l in r.stdout.splitlines() if l != ""], r.stderr


def run_dex(src):
    return run_c(assemble(compile_src(src).to_program()))


def get(lines, key, default=None):
    """按 `key=value` 取值 —— 比数行号稳,增删一行不会让整段错位。"""
    pre = key + "="
    for l in lines:
        if l.startswith(pre):
            return l[len(pre):]
    return default


def need_vm():
    return os.path.exists(DLL) and os.path.exists(VM)


def scene(body, w=64, h=64, interp=0):
    """通用外壳:离屏 + 关掉自动物理(手动步进才可复现)+ 关掉渲染插值。"""
    return ('include "dexgame";\n'
            'eng_init_offscreen(%d, %d);\n'
            'eng_physics_set_auto(0);\n'
            'eng_render_set_interp(%d);\n' % (w, h, interp)) + body + 'eng_shutdown();\n'


# ---------- 1) 形状与重叠 ----------
def test_shapes():
    print("[形状与重叠]")
    if not need_vm():
        skip("形状", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
// a:AABB 世界矩形 0..8(transform 0 + offset 4,hw 4)
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "collider");
eng_set_f(a, "collider", "hw", 4.0);
eng_set_f(a, "collider", "hh", 4.0);
eng_set_f(a, "collider", "ox", 4.0);
eng_set_f(a, "collider", "oy", 4.0);
// b:远处不重叠
let b = eng_object_new();
eng_attach(b, "transform");
eng_attach(b, "collider");
eng_set_f(b, "transform", "x", 20.0);
eng_set_f(b, "collider", "hw", 2.0);
eng_set_f(b, "collider", "hh", 2.0);
eng_set_f(b, "collider", "ox", 2.0);
eng_set_f(b, "collider", "oy", 2.0);
// c:**刚好相切**(x=8 边界)—— 相切不算重叠
let c = eng_object_new();
eng_attach(c, "transform");
eng_attach(c, "collider");
eng_set_f(c, "transform", "x", 8.0);
eng_set_f(c, "collider", "hw", 2.0);
eng_set_f(c, "collider", "hh", 2.0);
eng_set_f(c, "collider", "ox", 2.0);
eng_set_f(c, "collider", "oy", 2.0);
// d:圆,半径 2 压在 a 上
let d = eng_object_new();
eng_attach(d, "transform");
eng_attach(d, "collider");
eng_set_i(d, "collider", "kind", 1);
eng_set_f(d, "collider", "hw", 2.0);
eng_set_f(d, "collider", "ox", 2.0);
eng_set_f(d, "collider", "oy", 2.0);
// e:胶囊,离得远
let e = eng_object_new();
eng_attach(e, "transform");
eng_attach(e, "collider");
eng_set_i(e, "collider", "kind", 2);
eng_set_f(e, "collider", "hw", 1.0);
eng_set_f(e, "collider", "hh", 3.0);
eng_set_f(e, "transform", "x", 30.0);
eng_set_f(e, "collider", "ox", 1.0);
eng_set_f(e, "collider", "oy", 5.0);
// f:胶囊,a 的右边线上 -> 应该算重叠(距离 0 < 半径 1)
let f = eng_object_new();
eng_attach(f, "transform");
eng_attach(f, "collider");
eng_set_i(f, "collider", "kind", 2);
eng_set_f(f, "collider", "hw", 1.0);
eng_set_f(f, "collider", "hh", 3.0);
eng_set_f(f, "transform", "x", 7.0);
eng_set_f(f, "collider", "ox", 1.0);
eng_set_f(f, "collider", "oy", 5.0);
print "ab=" + eng_overlap(a, b);
print "ac=" + eng_overlap(a, c);
print "ad=" + eng_overlap(a, d);
print "ae=" + eng_overlap(a, e);
print "af=" + eng_overlap(a, f);
print "de=" + eng_overlap(d, e);
// layer/mask:d 与 a 重叠,但互相的 mask 都不含对方 layer 时不算
eng_set_i(a, "collider", "layer", 1);
eng_set_i(a, "collider", "mask", 2);
eng_set_i(d, "collider", "layer", 2);
eng_set_i(d, "collider", "mask", 1);
print "layers_ok=" + eng_overlap(a, d);
eng_set_i(a, "collider", "mask", 4);
print "layers_no=" + eng_overlap(a, d);
// mask = 0 = 不筛选
eng_set_i(a, "collider", "mask", 0);
print "mask0=" + eng_overlap(a, d);
// 没有碰撞体的对象要带原因失败
let g = eng_object_new();
eng_attach(g, "transform");
print "no_col=" + eng_overlap(a, g);
print "err=[" + eng_last_error() + "]";
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("AABB 分离 = 0", get(L, "ab") == "0", L[:4])
    check("AABB **相切**不算重叠", get(L, "ac") == "0", get(L, "ac"))
    check("AABB vs 圆 重叠 = 1", get(L, "ad") == "1", get(L, "ad"))
    check("AABB vs 远处胶囊 = 0", get(L, "ae") == "0", get(L, "ae"))
    check("AABB vs 边上胶囊 = 1", get(L, "af") == "1", get(L, "af"))
    check("圆 vs 胶囊 = 0", get(L, "de") == "0", get(L, "de"))
    check("layer/mask 互相匹配 = 1", get(L, "layers_ok") == "1", get(L, "layers_ok"))
    check("**mask 不含对方 layer = 0**", get(L, "layers_no") == "0", get(L, "layers_no"))
    check("mask=0 表示不筛选", get(L, "mask0") == "1", get(L, "mask0"))
    check("缺碰撞体返回 -1", get(L, "no_col") == "-1", get(L, "no_col"))
    check("原因指出缺少 collider", "collider" in (get(L, "err") or ""), get(L, "err"))


# ---------- 2) 游标式查询 ----------
def test_query():
    print("[游标式查询]")
    if not need_vm():
        skip("查询", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "collider");
eng_set_f(a, "collider", "hw", 4.0);
eng_set_f(a, "collider", "hh", 4.0);
eng_set_f(a, "collider", "ox", 4.0);
eng_set_f(a, "collider", "oy", 4.0);
let b = eng_object_new();
eng_attach(b, "transform");
eng_attach(b, "collider");
eng_set_f(b, "transform", "x", 10.0);
eng_set_f(b, "collider", "hw", 4.0);
eng_set_f(b, "collider", "hh", 4.0);
eng_set_f(b, "collider", "ox", 4.0);
eng_set_f(b, "collider", "oy", 4.0);
let c = eng_object_new();
eng_attach(c, "transform");
eng_attach(c, "collider");
eng_set_f(c, "transform", "x", 20.0);
eng_set_f(c, "collider", "hw", 4.0);
eng_set_f(c, "collider", "hh", 4.0);
eng_set_f(c, "collider", "ox", 4.0);
eng_set_f(c, "collider", "oy", 4.0);
print "ida=" + a;
print "idb=" + b;
print "idc=" + c;

let q = eng_query_rect(0.0, 0.0, 12.0, 12.0, 0);
print "count=" + eng_query_count(q);
print "at0=" + (eng_query_at(q, 0) == a);
print "at1=" + (eng_query_at(q, 1) == b);
print "next1=" + (eng_query_next(q) == a);
print "next2=" + (eng_query_next(q) == b);
print "next3=" + eng_query_next(q);
print "reset=" + eng_query_reset(q);
print "again=" + (eng_query_next(q) == a);
print "end=" + eng_query_end(q);
print "closed=" + eng_query_count(q);
print "closed_err=" + eng_last_error();

// 点查询
let p = eng_query_point(5.0, 5.0, 0);
print "pcount=" + eng_query_count(p);
print "pat0=" + (eng_query_at(p, 0) == a);
print "pend=" + eng_query_end(p);
// 圆查询:中心 (14,4) 半径 5 -> 只命中 b
let cq = eng_query_circle(14.0, 4.0, 5.0, 0);
print "ccount=" + eng_query_count(cq);
print "cat0=" + (eng_query_at(cq, 0) == b);
eng_query_end(cq);
// 越界全命中
let all = eng_query_rect(0.0, 0.0, 64.0, 64.0, 0);
print "allcount=" + eng_query_count(all);
eng_query_end(all);

// mask 过滤:只留 c(layer 2)
eng_set_i(a, "collider", "layer", 1);
eng_set_i(a, "collider", "mask", 1);
eng_set_i(b, "collider", "layer", 1);
eng_set_i(b, "collider", "mask", 1);
eng_set_i(c, "collider", "layer", 2);
eng_set_i(c, "collider", "mask", 2);
let mq = eng_query_rect(0.0, 0.0, 64.0, 64.0, 2);
print "mcount=" + eng_query_count(mq);
print "mat0=" + (eng_query_at(mq, 0) == c);
eng_query_end(mq);

// 嵌套两条查询各自独立
let q1 = eng_query_rect(0.0, 0.0, 64.0, 64.0, 0);
let q2 = eng_query_rect(0.0, 0.0, 12.0, 12.0, 0);
print "nest1=" + eng_query_count(q1);
print "nest2=" + eng_query_count(q2);
print "nest2b=" + eng_query_count(q2);
eng_query_end(q1);
eng_query_end(q2);

// 游标上限:最多 8 条同时打开
let opened = 0;
let i = 0;
let cur = 0;
while (i < 10) {
  cur = eng_query_rect(0.0, 0.0, 1.0, 1.0, 0);
  if (cur > 0) { opened = opened + 1; }
  i = i + 1;
}
print "opened=" + opened;
print "last=" + cur;
print "limit_err=" + eng_last_error();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("三个实体 id 互不相同",
          len({get(L, "ida"), get(L, "idb"), get(L, "idc")}) == 3, L[:3])
    check("矩形查询命中 2 个", get(L, "count") == "2", get(L, "count"))
    check("at(0) 是第一个", get(L, "at0") == "1", get(L, "at0"))
    check("at(1) 是第二个", get(L, "at1") == "1", get(L, "at1"))
    check("next 逐个给出(1)", get(L, "next1") == "1", get(L, "next1"))
    check("next 逐个给出(2)", get(L, "next2") == "1", get(L, "next2"))
    check("到头返回 0", get(L, "next3") == "0", get(L, "next3"))
    check("reset 返回 0", get(L, "reset") == "0", get(L, "reset"))
    check("reset 后能重新遍历", get(L, "again") == "1", get(L, "again"))
    check("end 返回 0", get(L, "end") == "0", get(L, "end"))
    check("关闭后查询报 -1", get(L, "closed") == "-1", get(L, "closed"))
    check("关闭后原因可读", "not open" in (get(L, "closed_err") or ""), get(L, "closed_err"))
    check("点查询只命中 1 个", get(L, "pcount") == "1", get(L, "pcount"))
    check("点查询命中的是 a", get(L, "pat0") == "1", get(L, "pat0"))
    check("圆查询命中 1 个", get(L, "ccount") == "1", get(L, "ccount"))
    check("圆查询命中的是 b", get(L, "cat0") == "1", get(L, "cat0"))
    check("大矩形命中 3 个", get(L, "allcount") == "3", get(L, "allcount"))
    check("mask 过滤后只剩 1 个", get(L, "mcount") == "1", get(L, "mcount"))
    check("mask 过滤命中的是 c", get(L, "mat0") == "1", get(L, "mat0"))
    check("嵌套查询 1 计数正确", get(L, "nest1") == "3", get(L, "nest1"))
    check("嵌套查询 2 计数正确", get(L, "nest2") == "2", get(L, "nest2"))
    check("嵌套互不干扰(重复读仍对)", get(L, "nest2b") == "2", get(L, "nest2b"))
    check("**游标上限 8**", get(L, "opened") == "8", get(L, "opened"))
    check("第 9 条返回 -1", get(L, "last") == "-1", get(L, "last"))
    check("上限原因提示 eng_query_end", "query" in (get(L, "limit_err") or ""),
          get(L, "limit_err"))


# ---------- 3) 射线 / 扫掠 ----------
def test_ray():
    print("[射线与扫掠]")
    if not need_vm():
        skip("射线", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
// a:AABB 世界矩形 x 0..8, y 20..28
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "collider");
eng_set_f(a, "transform", "y", 20.0);
eng_set_f(a, "collider", "hw", 4.0);
eng_set_f(a, "collider", "hh", 4.0);
eng_set_f(a, "collider", "ox", 4.0);
eng_set_f(a, "collider", "oy", 4.0);
// c:圆心 (22,2) 半径 2
let c = eng_object_new();
eng_attach(c, "transform");
eng_attach(c, "collider");
eng_set_i(c, "collider", "kind", 1);
eng_set_f(c, "collider", "hw", 2.0);
eng_set_f(c, "transform", "x", 20.0);
eng_set_f(c, "collider", "ox", 2.0);
eng_set_f(c, "collider", "oy", 2.0);
// d:圆后面的 AABB(世界 x 28..32)
let d = eng_object_new();
eng_attach(d, "transform");
eng_attach(d, "collider");
eng_set_f(d, "transform", "x", 28.0);
eng_set_f(d, "collider", "hw", 2.0);
eng_set_f(d, "collider", "hh", 2.0);
eng_set_f(d, "collider", "ox", 2.0);
eng_set_f(d, "collider", "oy", 2.0);
// k:胶囊,线段 (41,1)-(41,9),半径 1
let k = eng_object_new();
eng_attach(k, "transform");
eng_attach(k, "collider");
eng_set_i(k, "collider", "kind", 2);
eng_set_f(k, "collider", "hw", 1.0);
eng_set_f(k, "collider", "hh", 4.0);
eng_set_f(k, "transform", "x", 40.0);
eng_set_f(k, "collider", "ox", 1.0);
eng_set_f(k, "collider", "oy", 5.0);

print "hit_a=" + eng_raycast(-10.0, 24.0, 1.0, 0.0, 100.0, 0, 0);
print "a_obj=" + (eng_hit_obj() == a);
print "a_t=" + eng_hit_t();
print "a_x=" + eng_hit_x();
print "a_nx=" + eng_hit_nx();
print "a_ny=" + eng_hit_ny();

print "hit_c=" + eng_raycast(-10.0, 2.0, 1.0, 0.0, 100.0, 0, 0);
print "c_obj=" + (eng_hit_obj() == c);
print "c_t=" + eng_hit_t();

// 把 c 过滤掉(mask 不含它的 layer)-> 命中后面的 d
eng_set_i(c, "collider", "layer", 2);
eng_set_i(c, "collider", "mask", 2);
eng_set_i(d, "collider", "layer", 1);
eng_set_i(d, "collider", "mask", 0);
print "hit_d=" + eng_raycast(-10.0, 2.0, 1.0, 0.0, 100.0, 1, 0);
print "d_obj=" + (eng_hit_obj() == d);
print "d_t=" + eng_hit_t();

print "hit_k=" + eng_raycast(-10.0, 5.0, 1.0, 0.0, 100.0, 0, 0);
print "k_t=" + eng_hit_t();

// 从上往下打 a 的底面 -> 法线朝上 (0,1)
print "hit_up=" + eng_raycast(4.0, 40.0, 0.0, -1.0, 100.0, 0, 0);
print "up_t=" + eng_hit_t();
print "up_ny=" + eng_hit_ny();

// 打空
print "miss=" + eng_raycast(-10.0, 100.0, 1.0, 0.0, 50.0, 0, 0);
print "miss_obj=" + eng_hit_obj();
print "miss_t=" + eng_hit_t();

// 距离不够
print "short=" + eng_raycast(-10.0, 24.0, 1.0, 0.0, 5.0, 0, 0);

// 扫掠:半宽高 2 的盒子从 (-10,24) 向右 100
print "sweep=" + eng_sweep_box(-10.0, 24.0, 2.0, 2.0, 100.0, 0.0, 0, 0);
print "sweep_t=" + eng_hit_t();
print "sweep_obj=" + (eng_hit_obj() == a);
print "sweep_miss=" + eng_sweep_box(-10.0, 100.0, 2.0, 2.0, 100.0, 0.0, 0, 0);

// 从**自己体内**发出射线:默认会命中自己(t=0),ignore 参数可以跳过自己
print "self_hit=" + eng_raycast(4.0, 24.0, 1.0, 0.0, 100.0, 0, 0);
print "self_obj=" + (eng_hit_obj() == a);
print "self_t=" + eng_hit_t();
print "skip_self=" + eng_raycast(4.0, 24.0, 1.0, 0.0, 100.0, 0, a);

// 错误路径
print "bad_dist=" + eng_raycast(0.0, 0.0, 1.0, 0.0, 0.0, 0, 0);
print "bad_dist_err=" + eng_last_error();
print "bad_dir=" + eng_raycast(0.0, 0.0, 0.0, 0.0, 10.0, 0, 0);
print "bad_dir_err=" + eng_last_error();
print "bad_sweep=" + eng_sweep_box(0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0, 0);
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("命中 AABB = 1", get(L, "hit_a") == "1", get(L, "hit_a"))
    check("命中对象正确", get(L, "a_obj") == "1", get(L, "a_obj"))
    check("t = 10(左边缘 x=0)", get(L, "a_t") == "10", get(L, "a_t"))
    check("命中点 x = 0", get(L, "a_x") == "0", get(L, "a_x"))
    check("法线 nx = -1", get(L, "a_nx") == "-1", get(L, "a_nx"))
    check("法线 ny = 0", get(L, "a_ny") == "0", get(L, "a_ny"))
    check("命中圆 = 1", get(L, "hit_c") == "1", get(L, "hit_c"))
    check("命中圆对象正确", get(L, "c_obj") == "1", get(L, "c_obj"))
    check("圆 t = 30(左边缘 x=20)", get(L, "c_t") == "30", get(L, "c_t"))
    check("**mask 过滤后命中后面的 d**", get(L, "d_obj") == "1", get(L, "d_obj"))
    check("d 的 t = 38", get(L, "d_t") == "38", get(L, "d_t"))
    check("命中胶囊 = 1", get(L, "hit_k") == "1", get(L, "hit_k"))
    check("胶囊 t = 50(左边缘 x=40)", get(L, "k_t") == "50", get(L, "k_t"))
    check("从下往上打命中", get(L, "hit_up") == "1", get(L, "hit_up"))
    check("底面 t = 12(y 40 -> 28)", get(L, "up_t") == "12", get(L, "up_t"))
    check("底面法线朝上 ny = 1", get(L, "up_ny") == "1", get(L, "up_ny"))
    check("打空返回 0", get(L, "miss") == "0", get(L, "miss"))
    check("打空时结果槽被清空(obj=0)", get(L, "miss_obj") == "0", get(L, "miss_obj"))
    check("打空时 t = 0", get(L, "miss_t") == "0", get(L, "miss_t"))
    check("距离不够打不中", get(L, "short") == "0", get(L, "short"))
    check("扫掠命中 = 1", get(L, "sweep") == "1", get(L, "sweep"))
    check("扫掠 t = 8(盒子右缘 x=-8 -> 0)", get(L, "sweep_t") == "8", get(L, "sweep_t"))
    check("扫掠命中对象正确", get(L, "sweep_obj") == "1", get(L, "sweep_obj"))
    check("扫掠打空 = 0", get(L, "sweep_miss") == "0", get(L, "sweep_miss"))
    check("零距离射线返回 -1", get(L, "bad_dist") == "-1", get(L, "bad_dist"))
    check("零距离原因可读", "distance" in (get(L, "bad_dist_err") or ""), get(L, "bad_dist_err"))
    check("零方向射线返回 -1", get(L, "bad_dir") == "-1", get(L, "bad_dir"))
    check("零方向原因可读", "direction" in (get(L, "bad_dir_err") or ""), get(L, "bad_dir_err"))
    check("零方向扫掠返回 -1", get(L, "bad_sweep") == "-1", get(L, "bad_sweep"))
    check("从自己体内发出射线会命中自己(t=0)", get(L, "self_hit") == "1", get(L, "self_hit"))
    check("自击命中的对象就是自己", get(L, "self_obj") == "1", get(L, "self_obj"))
    check("自击 t = 0", get(L, "self_t") == "0", get(L, "self_t"))
    check("**ignore 参数跳过自己(后面没有东西 → 0)**",
          get(L, "skip_self") == "0", get(L, "skip_self"))


# ---------- 4) move-and-slide ----------
TILEMAP_WALL = "1,1,1,1,1,1,1,1\\n1,-1,-1,-1,-1,-1,-1,-1\\n" \
               "1,-1,-1,-1,-1,-1,-1,-1\\n1,1,1,1,1,1,1,1"


def test_move_slide():
    print("[move-and-slide(轴分离解算)]")
    if not need_vm():
        skip("移动", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(scene('''
// 瓦片:16px;第 0 列是墙,第 0/3 行是天花板/地板
let tm = eng_object_new();
eng_attach(tm, "transform");
eng_attach(tm, "tilemap");
eng_set_f(tm, "tilemap", "tw", 16.0);
eng_set_f(tm, "tilemap", "th", 16.0);
print "load=" + eng_tilemap_load_csv(tm, "%s");

let box = eng_object_new();
eng_attach(box, "transform");
eng_attach(box, "collider");
eng_attach(box, "body");
eng_set_f(box, "collider", "hw", 4.0);
eng_set_f(box, "collider", "hh", 4.0);
eng_set_f(box, "collider", "ox", 4.0);
eng_set_f(box, "collider", "oy", 4.0);
eng_set_i(box, "body", "motion", 1);
eng_set_f(box, "transform", "x", 32.0);
eng_set_f(box, "transform", "y", 24.0);

// 向左撞墙:世界矩形左缘停在 16
print "hit1=" + eng_move(box, -100.0, 0.0);
print "x1=" + eng_world_x(box);
print "wall1=" + eng_on_wall(box);
print "ground1=" + eng_on_ground(box);
// 已经贴墙:再往左推不动
print "hit2=" + eng_move(box, -5.0, 0.0);
print "x2=" + eng_world_x(box);
// 贴着墙斜向下: X 受阻,Y 照走 = 滑动
print "hit3=" + eng_move(box, -4.0, 8.0);
print "x3=" + eng_world_x(box);
print "y3=" + eng_world_y(box);
// 落到地板(y 冻结在 40 = 地板顶 48 - 半高 8)
let i = 0;
while (i < 40) { eng_move(box, 0.0, 8.0); i = i + 1; }
print "yfloor=" + eng_world_y(box);
print "ground2=" + eng_on_ground(box);

// 防穿透:一步 -1000 也不能穿过 16px 的墙
let fast = eng_object_new();
eng_attach(fast, "transform");
eng_attach(fast, "collider");
eng_set_f(fast, "collider", "hw", 4.0);
eng_set_f(fast, "collider", "hh", 4.0);
eng_set_f(fast, "collider", "ox", 4.0);
eng_set_f(fast, "collider", "oy", 4.0);
eng_set_f(fast, "transform", "x", 64.0);
eng_set_f(fast, "transform", "y", 24.0);
print "fast_hit=" + eng_move(fast, -1000.0, 0.0);
print "fast_x=" + eng_world_x(fast);

// 向上撞天花板(第 0 行 y 0..16):盒子世界矩形上缘停在 16
let up = eng_object_new();
eng_attach(up, "transform");
eng_attach(up, "collider");
eng_attach(up, "body");
eng_set_f(up, "collider", "hw", 4.0);
eng_set_f(up, "collider", "hh", 4.0);
eng_set_f(up, "collider", "ox", 4.0);
eng_set_f(up, "collider", "oy", 4.0);
eng_set_i(up, "body", "motion", 1);
eng_set_f(up, "transform", "x", 32.0);
eng_set_f(up, "transform", "y", 24.0);
print "up_hit=" + eng_move(up, 0.0, -100.0);
print "up_y=" + eng_world_y(up);
print "ceil=" + eng_on_ceiling(up);

// 没有碰撞体的对象:纯位移,不报错
let ghost = eng_object_new();
eng_attach(ghost, "transform");
print "ghost=" + eng_move(ghost, 5.0, 7.0);
print "gx=" + eng_world_x(ghost);
print "gy=" + eng_world_y(ghost);

// 对已死对象移动:失败带原因
let dead = eng_object_new();
eng_attach(dead, "transform");
eng_attach(dead, "collider");
eng_object_free(dead);
print "dead=" + eng_move(dead, 1.0, 1.0);
print "dead_err=" + eng_last_error();
''' % TILEMAP_WALL))
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("瓦片地图加载成功", get(L, "load") == "0", get(L, "load"))
    check("X 向受阻返回位 1", get(L, "hit1") == "1", get(L, "hit1"))
    check("**撞墙精确停在 x=16**", get(L, "x1") == "16", get(L, "x1"))
    check("on_wall = 1", get(L, "wall1") == "1", get(L, "wall1"))
    check("on_ground = 0(还没落地)", get(L, "ground1") == "0", get(L, "ground1"))
    check("已贴墙再推仍受阻", get(L, "hit2") == "1", get(L, "hit2"))
    check("位置不再变化", get(L, "x2") == "16", get(L, "x2"))
    check("斜向:只有 X 受阻(返回 1)", get(L, "hit3") == "1", get(L, "hit3"))
    check("斜向:X 保持贴墙", get(L, "x3") == "16", get(L, "x3"))
    check("斜向:Y **照常滑动**到 32", get(L, "y3") == "32", get(L, "y3"))
    check("落到地板 y = 40(地板顶 48 - 半高 8)", get(L, "yfloor") == "40", get(L, "yfloor"))
    check("on_ground = 1", get(L, "ground2") == "1", get(L, "ground2"))
    check("**超快移动不穿透**(一步 -1000)", get(L, "fast_hit") == "1", get(L, "fast_hit"))
    check("超快移动仍停在 x=16", get(L, "fast_x") == "16", get(L, "fast_x"))
    check("向上撞天花板返回 2", get(L, "up_hit") == "2", get(L, "up_hit"))
    check("天花板下缘 y = 16", get(L, "up_y") == "16", get(L, "up_y"))
    check("on_ceiling = 1", get(L, "ceil") == "1", get(L, "ceil"))
    check("无碰撞体:纯位移返回 0", get(L, "ghost") == "0", get(L, "ghost"))
    check("无碰撞体:x 正常位移", get(L, "gx") == "5", get(L, "gx"))
    check("无碰撞体:y 正常位移", get(L, "gy") == "7", get(L, "gy"))
    check("已死对象返回 -1", get(L, "dead") == "-1", get(L, "dead"))
    check("已死对象原因可读", "alive" in (get(L, "dead_err") or ""), get(L, "dead_err"))


# ---------- 5) 动态体 ----------
def test_dynamic():
    print("[动态体:重力 / 落地 / 休眠 / 弹性]")
    if not need_vm():
        skip("动态", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(scene('''
// 地板:世界矩形 x 0..64, y 60..68
let floor = eng_object_new();
eng_attach(floor, "transform");
eng_attach(floor, "collider");
eng_set_f(floor, "transform", "y", 60.0);
eng_set_f(floor, "collider", "hw", 32.0);
eng_set_f(floor, "collider", "hh", 4.0);
eng_set_f(floor, "collider", "ox", 32.0);
eng_set_f(floor, "collider", "oy", 4.0);

let box = eng_object_new();
eng_attach(box, "transform");
eng_attach(box, "collider");
eng_attach(box, "body");
eng_set_f(box, "collider", "hw", 4.0);
eng_set_f(box, "collider", "hh", 4.0);
eng_set_f(box, "collider", "ox", 4.0);
eng_set_f(box, "collider", "oy", 4.0);
eng_set_i(box, "body", "motion", 2);
eng_set_f(box, "body", "restitution", 0.0);
eng_set_f(box, "transform", "x", 16.0);
print "start_y=" + eng_world_y(box);

// 自由落体:1 秒 120 步
let i = 0;
while (i < 120) { eng_physics_step(%s); i = i + 1; }
print "land_y=" + eng_world_y(box);
print "land_vy=" + eng_velocity_y(box);
print "land_ground=" + eng_on_ground(box);
print "sleeping=" + eng_body_sleeping(box);
print "sub=" + eng_physics_substeps();
// 继续跑:休眠体不该再动
while (i < 240) { eng_physics_step(%s); i = i + 1; }
print "still_y=" + eng_world_y(box);
print "still_sleep=" + eng_body_sleeping(box);
// 唤醒:直接设速度
print "wake_set=" + eng_set_velocity(box, 0.0, -400.0);
print "awake=" + eng_body_sleeping(box);
eng_physics_step(%s);
print "after_wake_y=" + eng_world_y(box);

// gravity_scale = 0 的物体悬停
let hover = eng_object_new();
eng_attach(hover, "transform");
eng_attach(hover, "collider");
eng_attach(hover, "body");
eng_set_f(hover, "collider", "hw", 4.0);
eng_set_f(hover, "collider", "hh", 4.0);
eng_set_i(hover, "body", "motion", 2);
eng_set_f(hover, "body", "gravity_scale", 0.0);
eng_set_f(hover, "transform", "y", 10.0);
let j = 0;
while (j < 60) { eng_physics_step(%s); j = j + 1; }
print "hover_y=" + eng_world_y(hover);

// 弹性:以 1920px/s(每步 16px)撞地板,restitution 0.5 -> 反弹
let bb = eng_object_new();
eng_attach(bb, "transform");
eng_attach(bb, "collider");
eng_attach(bb, "body");
eng_set_f(bb, "collider", "hw", 4.0);
eng_set_f(bb, "collider", "hh", 4.0);
eng_set_f(bb, "collider", "ox", 4.0);
eng_set_f(bb, "collider", "oy", 4.0);
eng_set_i(bb, "body", "motion", 2);
eng_set_f(bb, "body", "gravity_scale", 0.0);
eng_set_f(bb, "body", "restitution", 0.5);
eng_set_f(bb, "transform", "x", 0.0);
eng_set_f(bb, "transform", "y", 44.0);
eng_set_velocity(bb, 0.0, 1920.0);
eng_physics_step(%s);
print "bounce_vy=" + eng_velocity_y(bb);
print "bounce_y=" + eng_world_y(bb);

// restitution = 0 的同一个实验:撞上就停
let nb = eng_object_new();
eng_attach(nb, "transform");
eng_attach(nb, "collider");
eng_attach(nb, "body");
eng_set_f(nb, "collider", "hw", 4.0);
eng_set_f(nb, "collider", "hh", 4.0);
eng_set_f(nb, "collider", "ox", 4.0);
eng_set_f(nb, "collider", "oy", 4.0);
eng_set_i(nb, "body", "motion", 2);
eng_set_f(nb, "body", "gravity_scale", 0.0);
eng_set_f(nb, "body", "restitution", 0.0);
eng_set_f(nb, "transform", "x", 48.0);
eng_set_f(nb, "transform", "y", 44.0);
eng_set_velocity(nb, 0.0, 1920.0);
eng_physics_step(%s);
print "nobounce_vy=" + eng_velocity_y(nb);
print "nobounce_y=" + eng_world_y(nb);
eng_shutdown();
''' % (STEP, STEP, STEP, STEP, STEP, STEP)))
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("初始 y = 0", get(L, "start_y") == "0", get(L, "start_y"))
    check("**落地 y = 52(地板顶 60 - 半高 8)**", get(L, "land_y") == "52", get(L, "land_y"))
    check("落地后速度归零", get(L, "land_vy") == "0", get(L, "land_vy"))
    check("on_ground = 1", get(L, "land_ground") == "1", get(L, "land_ground"))
    check("静止后自动休眠", get(L, "sleeping") == "1", get(L, "sleeping"))
    check("一次 1/120 调用只跑 1 个子步", get(L, "sub") == "1", get(L, "sub"))
    check("休眠后不再移动", get(L, "still_y") == "52", get(L, "still_y"))
    check("仍然保持休眠", get(L, "still_sleep") == "1", get(L, "still_sleep"))
    check("设速度成功", get(L, "wake_set") == "0", get(L, "wake_set"))
    check("设速度后唤醒", get(L, "awake") == "0", get(L, "awake"))
    check("唤醒后向上移动(y < 52)", float(get(L, "after_wake_y", "99")) < 52.0,
          get(L, "after_wake_y"))
    check("gravity_scale=0 悬浮不动", get(L, "hover_y") == "10", get(L, "hover_y"))
    check("**弹性 0.5:速度反向并减半**", get(L, "bounce_vy") == "-960", get(L, "bounce_vy"))
    check("弹性体停在接触面(y = 52)", get(L, "bounce_y") == "52", get(L, "bounce_y"))
    check("弹性 0:撞上就停", get(L, "nobounce_vy") == "0", get(L, "nobounce_vy"))
    check("弹性 0 也停在接触面", get(L, "nobounce_y") == "52", get(L, "nobounce_y"))


# ---------- 6) 固定步长 ----------
def test_fixed_step():
    print("[固定步长累加器]")
    if not need_vm():
        skip("步长", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(scene('''
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "collider");
eng_attach(a, "body");
eng_set_f(a, "collider", "hw", 1.0);
eng_set_f(a, "collider", "hh", 1.0);
eng_set_i(a, "body", "motion", 1);
eng_set_f(a, "body", "gravity_scale", 0.0);
eng_set_velocity(a, 120.0, 0.0);

print "sub_1=" + eng_physics_step(%s);
print "x_1=" + eng_world_x(a);
print "sub_15=" + eng_physics_step(%s);
print "x_15=" + eng_world_x(a);
print "alpha_15=" + eng_physics_alpha();
print "dt_15=" + eng_dt();
print "sub_0=" + eng_physics_step(0.0);
print "x_0=" + eng_world_x(a);

// 改步长为 240Hz:上一次剩下的半个子步还在累加器里,所以 1/120 会跑 3 个子步
print "set240=" + eng_physics_set_step(240.0);
print "sub_240=" + eng_physics_step(%s);
print "x_240=" + eng_world_x(a);

// 暂停
print "pause=" + eng_physics_pause(1);
print "sub_pause=" + eng_physics_step(1.0);
print "x_pause=" + eng_world_x(a);
print "resume=" + eng_physics_pause(0);
eng_shutdown();
''' % (STEP, STEP15, STEP)))
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("1/120 一次 = 1 个子步", get(L, "sub_1") == "1", get(L, "sub_1"))
    check("移动了 1px(120px/s × 1/120)", get(L, "x_1") == "1", get(L, "x_1"))
    check("1.5 个子步只跑 1 个", get(L, "sub_15") == "1", get(L, "sub_15"))
    check("位置再 +1 = 2(剩下的进累加器)", get(L, "x_15") == "2", get(L, "x_15"))
    check("**alpha ≈ 0.5**", abs(float(get(L, "alpha_15", "0")) - 0.5) < 0.01,
          get(L, "alpha_15"))
    # eng_dt() 是**帧**的墙钟间隔(不是物理步长):这里没有调 eng_frame_begin,
    # 所以它应该是 0;eng_physics_step 只负责物理。
    check("**eng_dt 是帧 dt 而不是物理步长**(没开帧就是 0)", get(L, "dt_15") == "0",
          get(L, "dt_15"))
    check("dt=0 不跑子步", get(L, "sub_0") == "0", get(L, "sub_0"))
    check("步长改 240Hz 后跑 3 个子步(含上次剩的半个)", get(L, "sub_240") == "3",
          get(L, "sub_240"))
    check("240Hz 下移动 1.5px -> x = 3.5", get(L, "x_240") == "3.5", get(L, "x_240"))
    check("暂停时 0 子步", get(L, "sub_pause") == "0", get(L, "sub_pause"))
    check("暂停时位置不变", get(L, "x_pause") == "3.5", get(L, "x_pause"))
    check("恢复返回 0", get(L, "resume") == "0", get(L, "resume"))


def test_gravity_units():
    print("[重力单位:像素/秒²]")
    if not need_vm():
        skip("重力", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(scene('''
eng_physics_set_gravity(0.0, 12.0);
let b = eng_object_new();
eng_attach(b, "transform");
eng_attach(b, "collider");
eng_attach(b, "body");
eng_set_f(b, "collider", "hw", 1.0);
eng_set_f(b, "collider", "hh", 1.0);
eng_set_i(b, "body", "motion", 2);
let i = 0;
let n = 0;
while (i < 10) { n = n + eng_physics_step(%s); i = i + 1; }
print "subs=" + n;
print "grav_vy=" + eng_velocity_y(b);
print "grav_y=" + eng_world_y(b);
print "t=" + eng_time();
print "alpha=" + eng_physics_alpha();
eng_shutdown();
''' % STEP))
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("10 次调用共 10 个子步", get(L, "subs") == "10", get(L, "subs"))
    check("速度 = g·t = 12 × 10/120 = 1.0",
          abs(float(get(L, "grav_vy", "9")) - 1.0) < 0.01, get(L, "grav_vy"))
    # 半隐式欧拉:y = h·Σ v_i = (1/120)·0.1·(1+..+10) = 0.04583
    check("位移 = ½·g·t² 量级(0.0458)",
          abs(float(get(L, "grav_y", "9")) - 0.045833) < 0.005, get(L, "grav_y"))
    check("时间累积 = 10/120", abs(float(get(L, "t", "0")) - 10.0 / 120.0) < 0.001,
          get(L, "t"))
    check("整帧跑完后 alpha = 0", float(get(L, "alpha", "9")) < 1e-6, get(L, "alpha"))


def test_determinism():
    print("[确定性:同样的输入两次跑必须逐行一致]")
    if not need_vm():
        skip("确定性", "需要 DLL 与 vm.exe")
        return
    src = scene('''
let floor = eng_object_new();
eng_attach(floor, "transform");
eng_attach(floor, "collider");
eng_set_f(floor, "transform", "y", 60.0);
eng_set_f(floor, "collider", "hw", 32.0);
eng_set_f(floor, "collider", "hh", 4.0);
eng_set_f(floor, "collider", "ox", 32.0);
eng_set_f(floor, "collider", "oy", 4.0);
let box = eng_object_new();
eng_attach(box, "transform");
eng_attach(box, "collider");
eng_attach(box, "body");
eng_set_f(box, "collider", "hw", 4.0);
eng_set_f(box, "collider", "hh", 4.0);
eng_set_i(box, "body", "motion", 2);
eng_set_f(box, "body", "restitution", 0.6);
eng_set_velocity(box, 60.0, 0.0);
let i = 0;
while (i < 200) {
  eng_physics_step(%s);
  if (i == 50) { print "y50=" + eng_world_y(box); }
  if (i == 100) { print "y100=" + eng_world_y(box); }
  if (i == 199) { print "y199=" + eng_world_y(box); }
  i = i + 1;
}
print "vx=" + eng_velocity_x(box);
''' % STEP)
    rc1, L1, err1 = run_dex(src)
    rc2, L2, err2 = run_dex(src)
    check("两次都退出码 0", rc1 == 0 and rc2 == 0, err1[:200] + err2[:200])
    check("**两次逐行一致**", L1 == L2, f"{L1} vs {L2}")
    check("确实跑了运动(y100 != y50)", get(L1, "y50") != get(L1, "y100"), L1[:4])


# ---------- 7) 瓦片地图 ----------
def test_tilemap():
    print("[瓦片地图:加载 / 查询 / 保存]")
    if not need_vm():
        skip("瓦片", "需要 DLL 与 vm.exe")
        return
    with open(TMP_CSV, "w", encoding="utf-8", newline="\n") as f:
        f.write("0,1,2\n3,-1,1\n")
    name = os.path.basename(TMP_CSV)
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
let tm = eng_object_new();
eng_attach(tm, "transform");
eng_attach(tm, "tilemap");
print "csv=" + eng_tilemap_load_csv(tm, "0,1\\n2,3");
print "cols=" + eng_tilemap_cols(tm);
print "rows=" + eng_tilemap_rows(tm);
print "t00=" + eng_tilemap_tile(tm, 0, 0);
print "t11=" + eng_tilemap_tile(tm, 1, 1);
print "solid00=" + eng_tilemap_solid_at(tm, 1.0, 1.0);
print "solid_out=" + eng_tilemap_solid_at(tm, 100.0, 1.0);
print "is_solid1=" + eng_tilemap_is_solid(tm, 1);
print "unset=" + eng_tilemap_set_solid(tm, 1, 0);
print "is_solid1b=" + eng_tilemap_is_solid(tm, 1);
print "solid00b=" + eng_tilemap_solid_at(tm, 20.0, 1.0);
eng_tilemap_set_solid(tm, 1, 1);
print "file=" + eng_tilemap_load_file(tm, "%s");
print "fcols=" + eng_tilemap_cols(tm);
print "frows=" + eng_tilemap_rows(tm);
print "f11=" + eng_tilemap_tile(tm, 1, 1);
print "f12=" + eng_tilemap_tile(tm, 2, 1);
print "save=" + eng_tilemap_save_csv(tm, "_phys_out.csv");
print "oob=" + eng_tilemap_tile(tm, 9, 9);
print "oob_err=" + eng_last_error();
let none = eng_object_new();
eng_attach(none, "transform");
print "notm=" + eng_tilemap_tile(none, 0, 0);
print "notm_err=" + eng_last_error();
let bad = eng_object_new();
eng_attach(bad, "transform");
eng_attach(bad, "tilemap");
print "badcsv=" + eng_tilemap_load_csv(bad, "0,1,x");
print "badcsv_err=" + eng_last_error();
print "badfile=" + eng_tilemap_load_file(bad, "_no_such_map.csv");
print "badfile_err=" + eng_last_error();
eng_shutdown();
''' % name)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("CSV 加载成功", get(L, "csv") == "0", get(L, "csv"))
    check("cols = 2", get(L, "cols") == "2", get(L, "cols"))
    check("rows = 2", get(L, "rows") == "2", get(L, "rows"))
    check("tile(0,0) = 0(**0 是合法图块**)", get(L, "t00") == "0", get(L, "t00"))
    check("tile(1,1) = 3", get(L, "t11") == "3", get(L, "t11"))
    check("非空格子默认实心", get(L, "solid00") == "1", get(L, "solid00"))
    check("地图外不算实心", get(L, "solid_out") == "0", get(L, "solid_out"))
    check("默认 is_solid = 1", get(L, "is_solid1") == "1", get(L, "is_solid1"))
    check("set_solid 成功", get(L, "unset") == "0", get(L, "unset"))
    check("**改过之后不是实心**", get(L, "is_solid1b") == "0", get(L, "is_solid1b"))
    check("实心判定跟着改(x=20 落在 tile 1 上)", get(L, "solid00b") == "0",
          get(L, "solid00b"))
    check("从文件加载成功", get(L, "file") == "0", get(L, "file"))
    check("文件 cols = 3", get(L, "fcols") == "3", get(L, "fcols"))
    check("文件 rows = 2", get(L, "frows") == "2", get(L, "frows"))
    check("文件 tile(1,1) = -1(空)", get(L, "f11") == "-1", get(L, "f11"))
    check("文件 tile(col=2,row=1) = 1", get(L, "f12") == "1", get(L, "f12"))
    check("保存 CSV 成功", get(L, "save") == "0", get(L, "save"))
    check("越界瓦片返回 -1", get(L, "oob") == "-1", get(L, "oob"))
    check("越界原因指出范围", "out of range" in (get(L, "oob_err") or ""), get(L, "oob_err"))
    check("没有 tilemap 组件返回 -1", get(L, "notm") == "-1", get(L, "notm"))
    check("原因指出缺少 tilemap", "tilemap" in (get(L, "notm_err") or ""), get(L, "notm_err"))
    check("坏 CSV 返回 -1", get(L, "badcsv") == "-1", get(L, "badcsv"))
    check("坏 CSV 原因指出字符", "unexpected char" in (get(L, "badcsv_err") or ""),
          get(L, "badcsv_err"))
    check("缺文件返回 -1", get(L, "badfile") == "-1", get(L, "badfile"))
    check("缺文件原因含路径", "_no_such_map.csv" in (get(L, "badfile_err") or ""),
          get(L, "badfile_err"))
    out = os.path.join(ROOT, "_phys_out.csv")
    if os.path.exists(out):
        with open(out, encoding="utf-8") as f:
            rows = [r.strip() for r in f.read().strip().splitlines()]
        check("回写的 CSV 行数 = 2", len(rows) == 2, rows)
        check("回写的 CSV 内容正确", rows[0] == "0,1,2" and rows[1] == "3,-1,1", rows)
    else:
        check("回写的 CSV 存在", False, out)


def test_tilemap_render():
    print("[瓦片地图:批量渲染 + 纯色像素]")
    if not need_vm():
        skip("瓦片渲染", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(64, 64);
eng_physics_set_auto(0);
let tm = eng_object_new();
eng_attach(tm, "transform");
eng_attach(tm, "tilemap");
eng_set_s(tm, "tilemap", "tex_path", "%s");
eng_set_i(tm, "tilemap", "atlas_tile", 1);
eng_set_i(tm, "tilemap", "atlas_cols", 2);
eng_set_f(tm, "tilemap", "tw", 16.0);
eng_set_f(tm, "tilemap", "th", 16.0);
eng_tilemap_load_csv(tm, "0,1\\n2,3");
eng_set_clear_color(0xFF000000);
eng_frame_begin();
print "drawn=" + eng_draw_scene();
eng_frame_end();
print "p_red=" + eng_pixel(8, 8);
print "p_green=" + eng_pixel(24, 8);
print "p_blue=" + eng_pixel(8, 24);
print "p_white=" + eng_pixel(24, 24);
print "p_out=" + eng_pixel(56, 8);
// 隐藏后不再绘制
eng_set_i(tm, "tilemap", "visible", 0);
eng_frame_begin();
print "hidden=" + eng_draw_scene();
eng_frame_end();
print "p_hidden=" + eng_pixel(8, 8);
eng_shutdown();
''' % ATLAS)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("绘制了 4 块瓦片", get(L, "drawn") == "4", get(L, "drawn"))
    check("瓦片(0,0) = 纯红", get(L, "p_red") == str(RED), get(L, "p_red"))
    check("瓦片(1,0) = 纯绿", get(L, "p_green") == str(GREEN), get(L, "p_green"))
    check("瓦片(0,1) = 纯蓝", get(L, "p_blue") == str(BLUE), get(L, "p_blue"))
    check("瓦片(1,1) = 纯白", get(L, "p_white") == str(WHITE), get(L, "p_white"))
    check("地图外 = 清除色", get(L, "p_out") == str(BLACK), get(L, "p_out"))
    check("visible=0 时不绘制", get(L, "hidden") == "0", get(L, "hidden"))
    check("visible=0 后像素为清除色", get(L, "p_hidden") == str(BLACK), get(L, "p_hidden"))


def test_tile_hit():
    print("[射线打到地形]")
    if not need_vm():
        skip("地形命中", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex('''include "dexgame";
eng_init_offscreen(8, 8);
let tm = eng_object_new();
eng_attach(tm, "transform");
eng_attach(tm, "tilemap");
eng_set_f(tm, "tilemap", "tw", 16.0);
eng_set_f(tm, "tilemap", "th", 16.0);
eng_tilemap_load_csv(tm, "1,1\\n1,1");
print "hit=" + eng_raycast(-10.0, 8.0, 1.0, 0.0, 100.0, 0, 0);
print "t=" + eng_hit_t();
print "tile=" + eng_hit_tile();
print "obj=" + eng_hit_obj();
print "nx=" + eng_hit_nx();
// 扫掠也一样
print "sweep=" + eng_sweep_box(-10.0, 8.0, 0.0, 0.0, 100.0, 0.0, 0, 0);
print "sweep_t=" + eng_hit_t();
print "sweep_tile=" + eng_hit_tile();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("射线打到地形 = 1", get(L, "hit") == "1", get(L, "hit"))
    check("地形命中 t = 10(地图左缘 0)", get(L, "t") == "10", get(L, "t"))
    check("**命中瓦片 id 非 0**", get(L, "tile") not in (None, "0"), get(L, "tile"))
    check("地形命中 obj = 0(不是实体)", get(L, "obj") == "0", get(L, "obj"))
    check("地形法线朝左 nx = -1", get(L, "nx") == "-1", get(L, "nx"))
    check("扫掠也能打到地形", get(L, "sweep") == "1", get(L, "sweep"))
    check("扫掠地形 t = 10", get(L, "sweep_t") == "10", get(L, "sweep_t"))
    check("扫掠地形 tile 非 0", get(L, "sweep_tile") not in (None, "0"), get(L, "sweep_tile"))


# ---------- 8) 渲染插值 ----------
def test_interp():
    print("[渲染插值:120Hz 物理 + 更高刷新率不顿]")
    if not need_vm():
        skip("插值", "需要 DLL 与 vm.exe")
        return
    # vx = 480px/s → 一个 1/120 子步正好走 4px。
    # 一次 1.5 子步的调用 → 跑 1 个子步,alpha = 0.5 → 渲染位置是上一位置与当前位置的中点。
    body = '''
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "sprite");
eng_attach(a, "body");
eng_set_s(a, "sprite", "tex_path", "%s");
eng_set_f(a, "sprite", "sx", 0.0);
eng_set_f(a, "sprite", "sy", 0.0);
eng_set_f(a, "sprite", "sw", 1.0);
eng_set_f(a, "sprite", "sh", 1.0);
eng_set_f(a, "sprite", "px", 0.0);
eng_set_f(a, "sprite", "py", 0.0);
eng_set_f(a, "sprite", "layer", 1);
eng_set_i(a, "body", "motion", 1);
eng_set_f(a, "body", "gravity_scale", 0.0);
eng_set_velocity(a, 480.0, 0.0);
print "sub=" + eng_physics_step(%s);
print "x=" + eng_world_x(a);
print "alpha=" + eng_physics_alpha();
eng_set_clear_color(0xFF000000);
eng_frame_begin();
print "drawn=" + eng_draw_scene();
eng_frame_end();
print "px0=" + eng_pixel(0, 0);
print "px2=" + eng_pixel(2, 0);
print "px4=" + eng_pixel(4, 0);
''' % (ATLAS, STEP15)
    # interp=1:渲染位置 = lerp(上一位置 0, 当前位置 4, alpha 0.5) = 2 → 精灵覆盖 2..3
    rc, L, err = run_dex(scene(body, interp=1))
    check("插值开:运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("插值开:只跑 1 个子步", get(L, "sub") == "1", get(L, "sub"))
    check("插值开:逻辑位置 = 4", get(L, "x") == "4", get(L, "x"))
    check("插值开:alpha ≈ 0.5", abs(float(get(L, "alpha", "0")) - 0.5) < 0.01, get(L, "alpha"))
    check("插值开:绘制 1 个精灵", get(L, "drawn") == "1", get(L, "drawn"))
    check("**插值开:精灵画在中点 2..3(原点空、4 处还没到)**",
          get(L, "px0") == str(BLACK) and get(L, "px2") == str(RED)
          and get(L, "px4") == str(BLACK),
          [get(L, "px0"), get(L, "px2"), get(L, "px4")])
    # interp=0:渲染用逻辑位置 4 → 精灵覆盖 4..5
    rc, L2, err2 = run_dex(scene(body, interp=0))
    check("插值关:运行退出码 0", rc == 0, err2[:200])
    if rc != 0:
        return
    check("插值关:绘制 1 个精灵", get(L2, "drawn") == "1", get(L2, "drawn"))
    check("插值关:精灵直接在逻辑位置 4..5", get(L2, "px4") == str(RED), get(L2, "px4"))
    check("插值关:(2,2) 是空的", get(L2, "px2") == str(BLACK), get(L2, "px2"))


# ---------- 9) 双 VM 一致性 ----------
DUAL_SRC = '''include "dexgame";
eng_init_offscreen(16, 16);
eng_physics_set_auto(0);
eng_render_set_interp(0);
let floor = eng_object_new();
eng_attach(floor, "transform");
eng_attach(floor, "collider");
eng_set_f(floor, "transform", "y", 24.0);
eng_set_f(floor, "collider", "hw", 16.0);
eng_set_f(floor, "collider", "hh", 2.0);
eng_set_f(floor, "collider", "ox", 16.0);
eng_set_f(floor, "collider", "oy", 2.0);
let box = eng_object_new();
eng_attach(box, "transform");
eng_attach(box, "collider");
eng_attach(box, "body");
eng_set_f(box, "collider", "hw", 2.0);
eng_set_f(box, "collider", "hh", 2.0);
eng_set_i(box, "body", "motion", 2);
eng_set_f(box, "body", "restitution", 0.4);
eng_physics_set_gravity(0.0, 400.0);
let i = 0;
while (i < 90) { eng_physics_step(0.008333333); i = i + 1; }
print eng_world_y(box);
print eng_velocity_y(box);
print eng_on_ground(box);
print eng_query_count(eng_query_rect(0.0, 0.0, 32.0, 32.0, 0));
print eng_raycast(0.0, 25.0, 1.0, 0.0, 40.0, 0, 0);
print eng_hit_t();
print eng_hit_obj();
eng_shutdown();
'''


def test_dual_vm():
    print("[双 VM 一致性]")
    if not need_vm():
        skip("双 VM", "需要 DLL 与 vm.exe")
        return
    unit = compile_src(DUAL_SRC)
    prog = unit.to_program()
    py_out, py_err = run_program(prog)
    rc, c_out, err = run_c(assemble(prog))
    check("pyvm 无错误", py_err is None, str(py_err))
    check("C VM 退出码 0", rc == 0, err[:200])
    check("两个 VM 结果逐行一致", py_out == c_out, f"py={py_out} c={c_out}")


def main():
    print("DEXCODE M3:dexgame 碰撞 / 查询 / 运动学 / 瓦片地图")
    test_shapes()
    test_query()
    test_ray()
    test_move_slide()
    test_dynamic()
    test_fixed_step()
    test_gravity_units()
    test_determinism()
    test_tilemap()
    test_tilemap_render()
    test_tile_hit()
    test_interp()
    test_dual_vm()
    for p in (TMP_SRC, TMP_BC, TMP_CSV, os.path.join(ROOT, "_phys_test.dxasm"),
              os.path.join(ROOT, "_phys_out.csv")):
        if os.path.exists(p):
            os.remove(p)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
