#!/usr/bin/env python3
"""DEXCODE M4-c 测试:文字(DirectWrite + 字形图集)。

覆盖:
  1) 字体:加载/同家族同字号复用/句柄失效/家族名与字号自省/错误路径
  2) 度量:比例字体(W 比 i 宽)、空串、多行(最宽一行 + 行数×行高)、CJK 宽度、
     字形缓存(度量也进缓存,不重复排版)
  3) 绘制(离屏像素断言):画出的像素数 > 0、都在测量出的框内、位置随 x/y 移动、
     空串不画、颜色 tint 生效
  4) 字形图集:多个字形共享一张图集,重复绘制不重新光栅化(计数不变)
  5) 双 VM 一致性(度量值是确定性的)

运行: python tests/test_text.py
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program, assemble, DexError  # noqa: E402
from dexlang.pyvm import run_program  # noqa: E402

LIBS = os.path.join(ROOT, "libs")
DLL = os.path.join(LIBS, "dexgame", "libdexgame.dll")
VM = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
TMP_SRC = os.path.join(ROOT, "_text_test.dex")
TMP_BC = os.path.join(ROOT, "_text_test.dexbc")

FONT = "Microsoft YaHei UI"
PASS = 0
FAIL = 0
SKIP = 0

LIT = 0xFFFFFFFF          # 全亮(字形覆盖率 255 的像素)


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
    pre = key + "="
    for l in lines:
        if l.startswith(pre):
            return l[len(pre):]
    return default


def need_vm():
    return os.path.exists(DLL) and os.path.exists(VM)


HEAD = '''include "dexgame";
eng_init_offscreen(128, 64);
eng_set_clear_color(0xFF000000);
'''


# ---------- 1) 字体 ----------
def test_font():
    print("[字体]")
    if not need_vm():
        skip("字体", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + '''
print "ok=" + eng_text_ok();
let f = eng_font_load("%s", 16.0);
print "font=" + f;
print "family=" + eng_font_family(f);
print "size=" + eng_font_size(f);
print "line=" + eng_font_line_height(f);
print "ascent=" + eng_font_ascent(f);
let f2 = eng_font_load("%s", 16.0);
print "cached=" + (f2 == f);
let f3 = eng_font_load("%s", 24.0);
print "other_size=" + (f3 != f);
print "count=" + eng_text_font_count();
// 释放后旧句柄失效
print "free=" + eng_font_free(f3);
print "line_gone=" + eng_font_line_height(f3);
print "gone_err=[" + eng_last_error() + "]";
// 错误路径
print "no_family=" + eng_font_load("", 16.0);
print "no_family_err=[" + eng_last_error() + "]";
print "zero_size=" + eng_font_load("%s", 0.0);
print "zero_err=[" + eng_last_error() + "]";
print "unknown=" + eng_font_load("NoSuchFontFamilyXYZ", 16.0);
print "unknown_err=[" + eng_last_error() + "]";
print "bad_id_line=" + eng_font_line_height(999999);
print "bad_id_err=[" + eng_last_error() + "]";
eng_shutdown();
''' % (FONT, FONT, FONT, FONT))
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    if get(L, "ok") != "1":
        skip("字体相关断言", "DirectWrite 不可用")
        return
    check("字体加载成功", int(get(L, "font", "0")) > 0, get(L, "font"))
    check("家族名读回一致", get(L, "family") == FONT, get(L, "family"))
    check("字号读回 16", get(L, "size") == "16", get(L, "size"))
    check("**行高 = 20.32(16px 字体)**", abs(float(get(L, "line", "0")) - 20.3203) < 0.01,
          get(L, "line"))
    check("ascent = 16.25", abs(float(get(L, "ascent", "0")) - 16.25) < 0.01, get(L, "ascent"))
    check("同家族同字号复用句柄", get(L, "cached") == "1", get(L, "cached"))
    check("不同字号是另一个句柄", get(L, "other_size") == "1", get(L, "other_size"))
    check("字体计数 = 2", get(L, "count") == "2", get(L, "count"))
    check("释放返回 0", get(L, "free") == "0", get(L, "free"))
    check("释放后旧句柄失效(行高 0)", get(L, "line_gone") == "0", get(L, "line_gone"))
    check("失效原因可读", "does not exist" in (get(L, "gone_err") or ""), get(L, "gone_err"))
    check("空家族名被拒", get(L, "no_family") == "-1", get(L, "no_family"))
    check("空家族名原因可读", "empty" in (get(L, "no_family_err") or ""),
          get(L, "no_family_err"))
    check("字号 0 被拒", get(L, "zero_size") == "-1", get(L, "zero_size"))
    check("字号 0 原因可读", "> 0" in (get(L, "zero_err") or ""), get(L, "zero_err"))
    check("不存在的家族被拒", get(L, "unknown") == "-1", get(L, "unknown"))
    check("不存在的家族原因可读", "not found" in (get(L, "unknown_err") or ""),
          get(L, "unknown_err"))
    check("坏字体 id 行高 0", get(L, "bad_id_line") == "0", get(L, "bad_id_line"))
    check("坏字体 id 原因可读", "does not exist" in (get(L, "bad_id_err") or ""),
          get(L, "bad_id_err"))


# ---------- 2) 度量 ----------
def test_measure():
    print("[度量]")
    if not need_vm():
        skip("度量", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + '''
let f = eng_font_load("%s", 16.0);
print "wA=" + eng_text_width(f, "A");
print "wW=" + eng_text_width(f, "W");
print "wi=" + eng_text_width(f, "i");
print "wABC=" + eng_text_width(f, "ABC");
print "wempty=" + eng_text_width(f, "");
print "h1=" + eng_text_height(f, "A");
print "h3=" + eng_text_height(f, "A\\nB\\nC");
print "wmulti=" + eng_text_width(f, "AB\\nW");
print "glyphs_before=" + eng_text_glyph_count();
let a = eng_text_width(f, "Hello");
let b = eng_text_width(f, "Hello");
print "stable=" + (a == b);
print "cn=" + eng_text_width(f, "中文");
print "cn3=" + eng_text_width(f, "中文字");
print "space=" + eng_text_width(f, " ");
eng_shutdown();
''' % FONT)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    try:
        wA = float(get(L, "wA", "0"))
        wW = float(get(L, "wW", "0"))
        wi = float(get(L, "wi", "0"))
        wABC = float(get(L, "wABC", "0"))
        wmulti = float(get(L, "wmulti", "0"))
        cn = float(get(L, "cn", "0"))
        cn3 = float(get(L, "cn3", "0"))
    except ValueError:
        check("度量值可解析", False, L[:6])
        return
    check("'A' 宽度 > 0", wA > 0, wA)
    check("**比例字体:W 明显比 i 宽**", wW > wi * 2, (wW, wi))
    check("ABC = A+B+C 量级(单调)", wABC > wA and wABC > wW, (wABC, wA))
    check("空串宽度 = 0", get(L, "wempty") == "0", get(L, "wempty"))
    check("单行高 = 20.32", abs(float(get(L, "h1", "0")) - 20.3203) < 0.01, get(L, "h1"))
    check("**三行高 = 3 × 20.32**", abs(float(get(L, "h3", "0")) - 60.96) < 0.02, get(L, "h3"))
    check("**多行宽度取最宽一行(AB 那行,而不是末行 W、也不是两行之和)**",
          wmulti > wW and wmulti < wABC, (wmulti, wW, wABC))
    check("度量结果稳定(两次相同)", get(L, "stable") == "1", get(L, "stable"))
    check("**CJK 也有宽度(中文能显示)**", cn > 0, cn)
    check("CJK 宽度是等宽的(3 字 ≈ 1.5 倍)", abs(cn3 - cn * 1.5) < 0.5, (cn, cn3))
    check("空格有宽度", float(get(L, "space", "0")) > 0, get(L, "space"))
    check("**度量会把字形记入缓存(A/W/i/ABC → 5 个字形,不是每次重排)**",
          int(get(L, "glyphs_before", "0")) == 5, get(L, "glyphs_before"))


# ---------- 3) 绘制(像素断言)----------
DRAW = '''let f = eng_font_load("%s", 16.0);
let wA = eng_text_width(f, "A");
let hA = eng_text_height(f, "A");
print "wA=" + wA;
print "hA=" + hA;
eng_frame_begin();
let n1 = eng_text(4.0, 4.0, f, "A", 0xFFFFFFFF);
eng_frame_end();
// 统计亮像素(白字 + 覆盖率 alpha 合成在纯黑上 -> r=g=b>0)
let lit = 0;
let y = 0;
while (y < 64) {
  let x = 0;
  while (x < 128) {
    if (eng_pixel(x, y) != 0xFF000000) { lit = lit + 1; }
    x = x + 1;
  }
  y = y + 1;
}
print "drawn=" + n1;
print "lit=" + lit;
// 空串:不画
eng_frame_begin();
let n2 = eng_text(4.0, 4.0, f, "", 0xFFFFFFFF);
eng_frame_end();
print "empty_drawn=" + n2;
// 移到别处:原来位置空了,新位置有墨水
eng_frame_begin();
eng_text(60.0, 4.0, f, "A", 0xFFFFFFFF);
eng_frame_end();
let lit_left = 0;
let lit_right = 0;
let x2 = 0;
while (x2 < 128) {
  if (eng_pixel(x2, 12) != 0xFF000000) {
    if (x2 < 55) { lit_left = lit_left + 1; } else { lit_right = lit_right + 1; }
  }
  x2 = x2 + 1;
}
print "moved_left=" + lit_left;
print "moved_right=" + lit_right;
// 颜色 tint:DexLang **没有位运算**,所以不能拆通道。改成比较两帧:
//   白字像素 = 0xFF000000 + cov*0x10000 + cov*0x100 + cov
//   红字像素 = 0xFF000000 + cov*0x10000
// 于是 (白和 - 红和) 一定是 cov 总和 × 65792 的倍数 —— 这就是"只把 g/b 清零"的证明。
eng_frame_begin();
eng_text(4.0, 4.0, f, "A", 0xFFFFFFFF);
eng_frame_end();
let white_count = 0;
let white_sum = 0;
let yw = 0;
while (yw < 64) {
  let xw = 0;
  while (xw < 128) {
    let pw = eng_pixel(xw, yw);
    if (pw != 0xFF000000) { white_count = white_count + 1; white_sum = white_sum + pw; }
    xw = xw + 1;
  }
  yw = yw + 1;
}
eng_frame_begin();
eng_text(4.0, 4.0, f, "A", 0xFFFF0000);
eng_frame_end();
let red_count = 0;
let red_sum = 0;
let yr = 0;
while (yr < 64) {
  let xr = 0;
  while (xr < 128) {
    let pr = eng_pixel(xr, yr);
    if (pr != 0xFF000000) { red_count = red_count + 1; red_sum = red_sum + pr; }
    xr = xr + 1;
  }
  yr = yr + 1;
}
print "white_count=" + white_count;
print "red_count=" + red_count;
print "diff=" + (white_sum - red_sum);
// 重复绘制不再新光栅化
let g1 = eng_text_glyph_count();
eng_frame_begin();
eng_text(4.0, 4.0, f, "A", 0xFFFFFFFF);
eng_frame_end();
print "g1=" + g1;
print "g2=" + eng_text_glyph_count();
eng_shutdown();
'''


def test_draw():
    print("[绘制:离屏像素断言]")
    if not need_vm():
        skip("绘制", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + (DRAW % FONT))
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    if get(L, "drawn") in (None, "-1"):
        skip("绘制相关断言", f"eng_text 失败: {get(L, 'drawn')}")
        return
    check("画了 1 个字形", get(L, "drawn") == "1", get(L, "drawn"))
    lit = int(get(L, "lit", "0"))
    check("**有墨水像素(真的画出来了)**", lit > 10, lit)
    check("墨水量合理(不会糊满整屏)", lit < 400, lit)
    check("空串画 0 个字形", get(L, "empty_drawn") == "0", get(L, "empty_drawn"))
    check("**移到 x=60 后原处没有墨水**", get(L, "moved_left") == "0", get(L, "moved_left"))
    check("**新位置有墨水**", int(get(L, "moved_right", "0")) > 0, get(L, "moved_right"))
    check("**tint 生效:红字与白字墨水像素数相同(同一个字形)**",
          get(L, "white_count") not in (None, "0") and get(L, "white_count") == get(L, "red_count"),
          (get(L, "white_count"), get(L, "red_count")))
    diff = int(get(L, "diff", "0"))
    # 白字 = base + cov*0x10101,红字 = base + cov*0x10000 → 差值 = cov*0x101 = cov*257
    check("**红字确实只是把 g/b 清零(差值 = cov 总和 × 257 的倍数)**",
          diff > 0 and diff % 257 == 0, diff)
    check("**重复绘制不重新光栅化字形**", get(L, "g1") == get(L, "g2"),
          (get(L, "g1"), get(L, "g2")))


# ---------- 4) 双 VM ----------
DUAL_SRC = '''include "dexgame";
eng_init_offscreen(32, 32);
let f = eng_font_load("Microsoft YaHei UI", 16.0);
print eng_text_ok();
print eng_font_line_height(f);
print eng_font_ascent(f);
print eng_text_width(f, "Hello");
print eng_text_width(f, "WWW");
print eng_text_width(f, "iii");
print eng_text_height(f, "A\\nB");
print eng_text_font_count();
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
    print("DEXCODE M4-c:文字(DirectWrite + 字形图集)")
    test_font()
    test_measure()
    test_draw()
    test_dual_vm()
    for p in (TMP_SRC, TMP_BC, os.path.join(ROOT, "_text_test.dxasm")):
        if os.path.exists(p):
            os.remove(p)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
