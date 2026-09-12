#!/usr/bin/env python3
"""DEXCODE M1 测试:dexgame 引擎的 D3D11 渲染核心。

覆盖:
  1) 接口层:`include "dexgame"` 编译通过、原生函数全部走值数组 ABI
  2) 离屏渲染:清除色、矩形覆盖范围(半开区间)、alpha 混合的**精确**数值
  3) 纹理:程序化纯色纹理、缩放绘制
  4) 图片解码:PNG(stb_image)+ 图集的**半纹素内缩**采样约定
  5) 代际句柄:释放后的旧 id 必须失效(而不是误指新纹理)
  6) 失败必须带得出原因(eng_last_error 非空),不静默失败
  7) 回读:BMP 落盘的头与尺寸有效
  8) 双 VM 一致性:pyvm 与 C VM 的像素结果必须相同
  9) 静态链接变体:dexgame_static 把 DLL 字节内嵌进 .dexbc
 10) 窗口模式:建窗口跑若干帧再关闭不崩

运行: python tests/test_dexgame.py
"""

import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program, assemble, DexError  # noqa: E402
from dexlang.pyvm import run_program  # noqa: E402

LIBS = os.path.join(ROOT, "libs")
DLL = os.path.join(LIBS, "dexgame", "libdexgame.dll")
ATLAS = os.path.join(ROOT, "tests", "fixtures", "atlas2x2.png")
VM = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
TMP_SRC = os.path.join(ROOT, "_dexgame_test.dex")      # 根目录 _ 前缀已被 gitignore
TMP_BC = os.path.join(ROOT, "_dexgame_test.dexbc")
TMP_BMP = os.path.join(ROOT, "_dexgame_test.bmp")

PASS = 0
FAIL = 0
SKIP = 0


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
                       encoding="utf-8", errors="replace", timeout=60)
    return r.returncode, r.stdout, r.stderr


def run_dex(src):
    """编译 + 用 C VM 跑,返回 (rc, [输出行])。"""
    bc = assemble(compile_src(src).to_program())
    rc, out, err = run_c(bc)
    return rc, [l for l in out.splitlines() if l != ""], err


# ---------- 1) 接口层 ----------
def test_interface():
    print("[接口层:值数组 ABI]")
    if not os.path.exists(DLL):
        skip("接口层", "libs/dexgame/libdexgame.dll 未构建(先跑 build_libs.bat)")
        return None
    unit = compile_src('include "dexgame";\neng_init_offscreen(8, 8);\neng_shutdown();\n')
    check("include \"dexgame\" 编译通过", unit is not None)
    check("注册了原生函数", len(unit.natives) >= 25, len(unit.natives))
    check("全部原生函数使用值数组 ABI",
          all(n.abi == "value_array" for n in unit.natives),
          sorted({n.abi for n in unit.natives}))
    widest = max(unit.natives, key=lambda n: n.arity)
    check("存在 10 参的函数(直接 ABI 的 3 参上限做不到)",
          widest.arity == 10, f"{widest.name} arity={widest.arity}")

    # 静态链接变体:应把 DLL 字节内嵌进 .dexbc
    dyn = assemble(compile_src('include "dexgame";\neng_init_offscreen(8,8);\n'
                               'eng_shutdown();\n').to_program())
    st = assemble(compile_src('include "dexgame_static";\neng_init_offscreen(8,8);\n'
                              'eng_shutdown();\n').to_program())
    check("dexgame_static 编译通过", len(st) > 0)
    check("静态版把 DLL 内嵌进字节码",
          len(st) > 200000 and len(dyn) < 5000 and b"MZ" in st,
          f"static={len(st)} dynamic={len(dyn)}")
    return unit


# ---------- 2) 离屏渲染 ----------
OFFSCREEN_PROG = '''include "dexgame";
print eng_version();
print eng_init_offscreen(32, 16);
print eng_width();
print eng_height();
eng_set_clear_color(0xFF102030);
print eng_frame_begin();
eng_rect(8, 4, 16, 8, 0xFFFF0000);
eng_frame_end();
print eng_pixel(12, 6);
print eng_pixel(0, 0);
print eng_pixel(8, 4);
print eng_pixel(23, 11);
print eng_pixel(24, 12);
eng_shutdown();
'''


def test_offscreen():
    print("[离屏渲染与像素断言]")
    if not os.path.exists(DLL) or not os.path.exists(VM):
        skip("离屏渲染", "需要 DLL 与 vm.exe")
        return
    rc, lines, err = run_dex(OFFSCREEN_PROG)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("eng_version = 2(库版本:M2)", lines[0] == "2", lines[:1])
    check("init_offscreen 返回 0", lines[1] == "0", lines[1:2])
    check("宽度 = 32", lines[2] == "32", lines[2:3])
    check("高度 = 16", lines[3] == "16", lines[3:4])
    check("frame_begin 返回 1", lines[4] == "1", lines[4:5])
    # 0xFFFF0000 = 4294901760, 0xFF102030 = 4279246896
    check("矩形内部 = 红", lines[5] == "4294901760", lines[5:6])
    check("矩形外部 = 清除色", lines[6] == "4279246896", lines[6:7])
    check("矩形左上角(含) = 红", lines[7] == "4294901760", lines[7:8])
    check("矩形右下角(含) = 红", lines[8] == "4294901760", lines[8:9])
    check("矩形右下一格(不含) = 清除色 —— 半开区间", lines[9] == "4279246896", lines[9:10])


# ---------- 3) alpha 混合的精确数值 ----------
BLEND_PROG = '''include "dexgame";
eng_init_offscreen(16, 16);
eng_set_clear_color(0xFF102030);
eng_frame_begin();
eng_rect(0.0, 0.0, 16.0, 16.0, 0x8000FF00);
eng_frame_end();
print eng_pixel(4, 4);
eng_shutdown();
'''


def test_alpha_blend():
    print("[alpha 混合的精确数值]")
    if not os.path.exists(DLL) or not os.path.exists(VM):
        skip("alpha 混合", "需要 DLL 与 vm.exe")
        return
    rc, lines, err = run_dex(BLEND_PROG)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    got = int(lines[0]) & 0xFFFFFFFF
    r, g, b, a = (got >> 16) & 0xFF, (got >> 8) & 0xFF, got & 0xFF, (got >> 24) & 0xFF
    # src=(0,255,0) alpha=0x80/255=0.502,dst=清除色(0x10,0x20,0x30)
    # 结果 = src*a + dst*(1-a)
    def mix(s, d):
        return round(s * (0x80 / 255.0) + d * (1 - 0x80 / 255.0))
    check("混合后 R 精确", r == mix(0, 0x10), f"R={r} 期望 {mix(0, 0x10)}")
    check("混合后 G 精确", g == mix(255, 0x20), f"G={g} 期望 {mix(255, 0x20)}")
    check("混合后 B 精确", b == mix(0, 0x30), f"B={b} 期望 {mix(0, 0x30)}")
    check("混合后 A 饱和为 255", a == 255, a)


# ---------- 4) 纹理与图集 ----------
TEX_PROG = '''include "dexgame";
eng_init_offscreen(32, 32);
let t = eng_tex_solid(2, 2, 0xFF00FF00);
print eng_tex_valid(t);
print eng_tex_width(t);
print eng_tex_height(t);
eng_set_clear_color(0xFF000000);
eng_frame_begin();
eng_draw(t, 4.0, 2.0, 8.0, 8.0, 0xFFFFFFFF);
eng_frame_end();
print eng_pixel(6, 4);
print eng_pixel(3, 4);
eng_shutdown();
'''


def test_texture():
    print("[程序化纹理与缩放绘制]")
    if not os.path.exists(DLL) or not os.path.exists(VM):
        skip("纹理", "需要 DLL 与 vm.exe")
        return
    rc, lines, err = run_dex(TEX_PROG)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("句柄有效", lines[0] == "1", lines[0:1])
    check("纹理宽 2", lines[1] == "2", lines[1:2])
    check("纹理高 2", lines[2] == "2", lines[2:3])
    check("缩放绘制后内部 = 绿(0xFF00FF00=4278255360)",
          lines[3] == "4278255360", lines[3:4])
    check("绘制区域外 = 黑(0xFF000000=4278190080)", lines[4] == "4278190080", lines[4:5])


def test_atlas_png():
    print("[PNG 解码 + 图集半纹素内缩采样]")
    if not os.path.exists(DLL) or not os.path.exists(VM):
        skip("图集", "需要 DLL 与 vm.exe")
        return
    if not os.path.exists(ATLAS):
        skip("图集", "tests/fixtures/atlas2x2.png 缺失(先跑 make_atlas.py)")
        return
    # 2x2 图集:红(0,0) 绿(1,0) 蓝(0,1) 白(1,1)。
    # 单纹素源必须用**纹素中心** uv(退化区间),否则线性过滤会掺入邻居
    # —— 这就是图集渗漏,半纹素内缩是标准解法。
    rel = os.path.relpath(ATLAS, ROOT).replace(os.sep, "/")
    src = 'include "dexgame";\neng_init_offscreen(32, 32);\n'
    src += 'let t = eng_tex_load("%s");\n' % rel
    src += 'print eng_tex_width(t);\nprint eng_tex_height(t);\n'
    src += 'eng_set_clear_color(0xFF000000);\n'
    for u, v in ((0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75)):
        src += ('eng_frame_begin();\n'
                'eng_draw_uv(t, 0.0, 0.0, 32.0, 32.0, %.2f, %.2f, %.2f, %.2f, 0xFFFFFFFF);\n'
                'eng_frame_end();\nprint eng_pixel(16, 16);\n' % (u, v, u, v))
    src += 'eng_shutdown();\n'
    rc, lines, err = run_dex(src)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("PNG 解码尺寸 = 2x2", lines[0] == "2" and lines[1] == "2", lines[:2])
    expect = [("红", 0xFFFF0000), ("绿", 0xFF00FF00), ("蓝", 0xFF0000FF), ("白", 0xFFFFFFFF)]
    for (name, exp), got in zip(expect, lines[2:6]):
        check(f"纹素 {name} = 0x{exp:08X}",
              int(got) & 0xFFFFFFFF == exp, f"got 0x{int(got) & 0xFFFFFFFF:08X}")


# ---------- 5) 代际句柄与错误上报 ----------
HANDLE_PROG = '''include "dexgame";
eng_init_offscreen(8, 8);
let a = eng_tex_solid(2, 2, 0xFF112233);
print eng_tex_valid(a);
print eng_tex_free(a);
print eng_tex_valid(a);
print eng_tex_width(a);
print "err1=" + eng_last_error();
let b = eng_tex_solid(2, 2, 0xFF445566);
print b;
print "err2=" + eng_last_error();
eng_shutdown();
'''


def test_handles_and_errors():
    print("[代际句柄 + 失败必须带原因]")
    if not os.path.exists(DLL) or not os.path.exists(VM):
        skip("代际句柄", "需要 DLL 与 vm.exe")
        return
    rc, lines, err = run_dex(HANDLE_PROG)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("新建句柄有效", lines[0] == "1", lines[0:1])
    check("释放返回 0", lines[1] == "0", lines[1:2])
    check("释放后旧句柄失效(不是误指新纹理)", lines[2] == "0", lines[2:3])
    check("对失效句柄取宽度返回 -1", lines[3] == "-1", lines[3:4])
    # 空错误消息会被空行吞掉,故用哨兵前缀断言(否则无法区分"空"与"没输出")
    check("失败原因非空且指出句柄问题",
          lines[4].startswith("err1=") and len(lines[4]) > len("err1=")
          and "texture" in lines[4].lower(), repr(lines[4]))
    check("新纹理拿到新句柄", lines[5].isdigit() and lines[5] != "0", lines[5:6])
    check("成功调用后错误为空(clear_error/成功时不残留旧错误)",
          lines[6] == "err2=", repr(lines[6]))


# ---------- 6) BMP 回读落盘 ----------
BMP_PROG = '''include "dexgame";
eng_init_offscreen(16, 8);
eng_set_clear_color(0xFF204080);
eng_frame_begin();
eng_rect(0.0, 0.0, 8.0, 8.0, 0xFF80FF00);
eng_frame_end();
print eng_save_bmp("%s");
eng_shutdown();
'''


def test_bmp_dump():
    print("[回读落盘为 BMP]")
    if not os.path.exists(DLL) or not os.path.exists(VM):
        skip("BMP", "需要 DLL 与 vm.exe")
        return
    rel = os.path.relpath(TMP_BMP, ROOT).replace(os.sep, "/")
    rc, lines, err = run_dex(BMP_PROG % rel)
    check("运行退出码 0", rc == 0, err[:200])
    check("save_bmp 返回 0", lines and lines[0] == "0", lines[:1])
    if not os.path.exists(TMP_BMP):
        check("BMP 文件已生成", False, TMP_BMP)
        return
    with open(TMP_BMP, "rb") as f:
        data = f.read()
    check("BMP 文件已生成", True)
    check("BMP 魔数为 'BM'", data[:2] == b"BM", data[:2])
    w, h = struct.unpack_from("<ii", data, 18)
    check("BMP 尺寸 16x8", (w, h) == (16, 8), f"{w}x{h}")
    declared = struct.unpack_from("<I", data, 2)[0]
    check("BMP 文件长度与头部一致", declared == len(data), f"{declared} vs {len(data)}")
    # 左下角应是左半的亮绿(像素 (0, 7)),右上角应是清除色
    rowbytes = w * 3
    pad = (4 - rowbytes % 4) % 4
    stride = rowbytes + pad
    def px(x, y):                       # BMP 自底向上
        off = 54 + (h - 1 - y) * stride + x * 3
        b, g, r = data[off], data[off + 1], data[off + 2]
        return (r << 16) | (g << 8) | b
    check("BMP 像素 (0,7) = 亮绿(0x80FF00)", px(0, 7) == 0x80FF00, hex(px(0, 7)))
    check("BMP 像素 (15,0) = 清除色(0x204080)", px(15, 0) == 0x204080, hex(px(15, 0)))


# ---------- 7) 双 VM 一致性 ----------
DUAL_PROG = '''include "dexgame";
eng_init_offscreen(16, 16);
eng_set_clear_color(0xFF102030);
eng_frame_begin();
eng_rect(0.0, 0.0, 8.0, 8.0, 0xC0FF8000);
eng_frame_end();
print eng_pixel(4, 4);
print eng_pixel(12, 12);
print eng_width();
eng_shutdown();
'''


def test_dual_vm():
    print("[双 VM 一致性]")
    if not os.path.exists(DLL) or not os.path.exists(VM):
        skip("双 VM 一致性", "需要 DLL 与 vm.exe")
        return
    unit = compile_src(DUAL_PROG)
    prog = unit.to_program()
    py_out, py_err = run_program(prog)
    rc, out, err = run_c(assemble(prog))
    c_out = [l for l in out.splitlines() if l != ""]
    check("pyvm 无错误", py_err is None, str(py_err))
    check("C VM 退出码 0", rc == 0, err[:200])
    check("两个 VM 的像素结果逐行一致", py_out == c_out, f"py={py_out} c={c_out}")


# ---------- 8) 窗口模式冒烟 ----------
WINDOW_PROG = '''include "dexgame";
print eng_init("dexgame 测试窗口", 320, 200, 0);
print eng_running();
let i = 0;
while i < 3 && eng_running() {
    eng_frame_begin();
    eng_rect(10.0, 10.0, 100.0, 60.0, 0xFF00A0FF);
    eng_frame_end();
    i = i + 1;
}
print eng_frame_index();
print eng_shutdown();
print "window-ok";
'''


def test_window_mode():
    print("[窗口模式冒烟]")
    if not os.path.exists(DLL) or not os.path.exists(VM):
        skip("窗口模式", "需要 DLL 与 vm.exe")
        return
    rc, lines, err = run_dex(WINDOW_PROG)
    check("窗口模式运行退出码 0", rc == 0, err[:300])
    if rc != 0:
        return
    check("eng_init 返回 0", lines[0] == "0", lines[:1])
    check("窗口建立后 running = 1", lines[1] == "1", lines[1:2])
    check("渲染了 3 帧", lines[2] == "3", lines[2:3])
    check("shutdown 返回 0", lines[3] == "0", lines[3:4])
    check("正常收尾", lines[4] == "window-ok", lines[4:5])


def main():
    print("DEXCODE M1:dexgame 引擎渲染核心测试")
    test_interface()
    test_offscreen()
    test_alpha_blend()
    test_texture()
    test_atlas_png()
    test_handles_and_errors()
    test_bmp_dump()
    test_dual_vm()
    test_window_mode()
    for p in (TMP_SRC, TMP_BC, TMP_BMP, os.path.join(ROOT, "_dexgame_test.dxasm")):
        if os.path.exists(p):
            os.remove(p)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
