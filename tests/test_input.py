#!/usr/bin/env python3
"""DEXCODE M4-a 测试:输入(键盘/鼠标/手柄)+ 动作映射 + eng_run 主循环。

覆盖:
  1) 动作映射:绑定/解绑/多来源/上限/自省,以及"没绑就报原因"
  2) **边沿检测**:pressed/released 在同一帧语义下只触发一次(begin 不拷贝、
     end 才把当前记成上一帧)
  3) 统一按键码:键盘/鼠标/手柄/摇杆轴都能当动作来源;模拟量取摇杆值
  4) 鼠标:位置、世界坐标(相机换算)、按键、滚轮;聚焦丢失清状态(库内行为)
  5) 合成输入(eng_input_feed*):无人值守环境里唯一能测输入的通道
  6) eng_run/eng_run_frames:帧序正确(begin → update → draw → end)、回调被调用次数、
     断线不调(dt 在手动物理模式下也要有帧间隔)
  7) 错误路径:非法按键码/轴/鼠标键/未绑动作/槽位用满
  8) 双 VM 一致性

运行: python tests/test_input.py
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
TMP_SRC = os.path.join(ROOT, "_input_test.dex")
TMP_BC = os.path.join(ROOT, "_input_test.dexbc")

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


HEAD = '''include "dexgame_fast";
eng_init_offscreen(32, 32);
eng_physics_set_auto(0);
eng_bind_default_actions();
'''


# ---------- 1) 动作映射 ----------
def test_actions():
    print("[动作映射]")
    if not need_vm():
        skip("动作", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + '''
print "count=" + eng_action_count();
print "n0=" + eng_action_name_at(0);
print "n1=" + eng_action_name_at(1);
print "jump_bound=" + eng_action_bound("jump");
print "nope_bound=" + eng_action_bound("nope");
print "jump_slots=" + eng_action_code("jump", 0) + "," + eng_action_code("jump", 1)
      + "," + eng_action_code("jump", 2);
print "jump_slot3=" + eng_action_code("jump", 3);
// 重复绑同一个键无害
print "rebind=" + eng_action_bind("jump", 32);
print "count2=" + eng_action_count();
// 新动作 + 绑定上限(4 个来源)
print "new=" + eng_action_bind("fire", 256);
eng_action_bind("fire", 257);
eng_action_bind("fire", 258);
eng_action_bind("fire", 310);
print "full=" + eng_action_bind("fire", 311);
print "full_err=" + eng_last_error();
print "fire0=" + eng_action_code("fire", 0);
print "fire3=" + eng_action_code("fire", 3);
// 解绑
print "unbind=" + eng_action_unbind("fire");
print "fire_bound=" + eng_action_bound("fire");
print "unbind2=" + eng_action_unbind("fire");
print "unbind2_err=" + eng_last_error();
// 越界自省
print "oob=[" + eng_action_name_at(99) + "]";
print "oob_err=" + eng_last_error();
// 未绑动作:读它要报原因
print "ghost=" + eng_action_down("ghost");
print "ghost_err=" + eng_last_error();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("默认绑定 7 个动作", get(L, "count") == "7", get(L, "count"))
    check("第 0 个动作是 left", get(L, "n0") == "left", get(L, "n0"))
    check("第 1 个动作是 right", get(L, "n1") == "right", get(L, "n1"))
    check("jump 已绑定", get(L, "jump_bound") == "1", get(L, "jump_bound"))
    check("未绑动作 bound = 0", get(L, "nope_bound") == "0", get(L, "nope_bound"))
    check("**jump 绑了 3 个来源(空格/W/手柄A)**",
          get(L, "jump_slots") == "32,87,310", get(L, "jump_slots"))
    check("越界槽位返回 -1", get(L, "jump_slot3") == "-1", get(L, "jump_slot3"))
    check("重复绑同一个键无害", get(L, "rebind") == "0", get(L, "rebind"))
    check("重复绑不增加动作数", get(L, "count2") == "7", get(L, "count2"))
    check("新动作可绑定", get(L, "new") == "0", get(L, "new"))
    check("**第 5 个来源被拒**", get(L, "full") == "-1", get(L, "full"))
    check("上限原因可读", "bindings" in (get(L, "full_err") or ""), get(L, "full_err"))
    check("槽位 0 = 鼠标左键", get(L, "fire0") == "256", get(L, "fire0"))
    check("槽位 3 = 手柄 A", get(L, "fire3") == "310", get(L, "fire3"))
    check("解绑返回 0", get(L, "unbind") == "0", get(L, "unbind"))
    check("解绑后 bound = 0", get(L, "fire_bound") == "0", get(L, "fire_bound"))
    check("重复解绑被拒", get(L, "unbind2") == "-1", get(L, "unbind2"))
    check("重复解绑原因可读", "not bound" in (get(L, "unbind2_err") or ""),
          get(L, "unbind2_err"))
    check("越界索引返回空串", get(L, "oob") == "[]", get(L, "oob"))
    check("越界索引原因含范围", "out of range" in (get(L, "oob_err") or ""), get(L, "oob_err"))
    check("未绑动作读取返回 0", get(L, "ghost") == "0", get(L, "ghost"))
    check("未绑动作原因提示 bind", "not bound" in (get(L, "ghost_err") or ""),
          get(L, "ghost_err"))


# ---------- 2) 边沿检测 ----------
def test_edges():
    print("[边沿检测:同一帧只触发一次]")
    if not need_vm():
        skip("边沿", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + '''
// 第 1 帧前按下 W(等价于 pump 里收到 WM_KEYDOWN)
eng_input_feed(87, 1);
eng_frame_begin();
print "f1_down=" + eng_action_down("jump");
print "f1_pressed=" + eng_action_pressed("jump");
print "f1_key_pressed=" + eng_key_pressed(87);
eng_frame_end();
// 第 2 帧:仍按着
eng_frame_begin();
print "f2_down=" + eng_action_down("jump");
print "f2_pressed=" + eng_action_pressed("jump");
eng_frame_end();
// 第 3 帧前松开
eng_input_feed(87, 0);
eng_frame_begin();
print "f3_down=" + eng_action_down("jump");
print "f3_released=" + eng_action_released("jump");
print "f3_key_released=" + eng_key_released(87);
eng_frame_end();
// 第 4 帧:都归零
eng_frame_begin();
print "f4_down=" + eng_action_down("jump");
print "f4_pressed=" + eng_action_pressed("jump");
print "f4_released=" + eng_action_released("jump");
eng_frame_end();
// 另一个来源也能触发同一个动作(手柄 A = 310)
eng_input_feed(310, 1);
eng_frame_begin();
print "pad_down=" + eng_action_down("jump");
print "pad_pressed=" + eng_action_pressed("jump");
eng_frame_end();
// 鼠标左键 -> action
eng_input_feed(256, 1);
eng_frame_begin();
print "mb_down=" + eng_action_down("action");
print "mb_pressed=" + eng_action_pressed("action");
eng_frame_end();
// 清空
eng_input_clear();
eng_frame_begin();
print "clear_down=" + eng_input_clear() + "," + eng_action_down("jump");
eng_frame_end();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("首帧 down = 1", get(L, "f1_down") == "1", get(L, "f1_down"))
    check("**首帧 pressed = 1**", get(L, "f1_pressed") == "1", get(L, "f1_pressed"))
    check("底层 key_pressed 同帧为 1", get(L, "f1_key_pressed") == "1", get(L, "f1_key_pressed"))
    check("第 2 帧 down 仍为 1", get(L, "f2_down") == "1", get(L, "f2_down"))
    check("**第 2 帧 pressed = 0(不重复触发)**", get(L, "f2_pressed") == "0", get(L, "f2_pressed"))
    check("松开帧 down = 0", get(L, "f3_down") == "0", get(L, "f3_down"))
    check("**松开帧 released = 1**", get(L, "f3_released") == "1", get(L, "f3_released"))
    check("底层 key_released 同帧为 1", get(L, "f3_key_released") == "1", get(L, "f3_key_released"))
    check("第 4 帧 released 归零", get(L, "f4_released") == "0", get(L, "f4_released"))
    check("第 4 帧 pressed 归零", get(L, "f4_pressed") == "0", get(L, "f4_pressed"))
    check("**手柄按键也能触发同一个动作**", get(L, "pad_pressed") == "1", get(L, "pad_pressed"))
    check("鼠标左键映射到 action", get(L, "mb_down") == "1", get(L, "mb_down"))
    check("鼠标左键边沿正确", get(L, "mb_pressed") == "1", get(L, "mb_pressed"))
    check("eng_input_clear 清掉状态", get(L, "clear_down") == "0,0", get(L, "clear_down"))


# ---------- 3) 摇杆 / 模拟量 ----------
def test_axis():
    print("[摇杆轴与模拟量]")
    if not need_vm():
        skip("轴", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + '''
print "axis0=" + eng_pad_axis(0);
eng_input_feed_axis(0, 0.8);
print "axis1=" + eng_pad_axis(0);
print "act_right=" + eng_action_value("right");
print "act_right_down=" + eng_action_down("right");
print "act_left=" + eng_action_value("left");
print "act_left_down=" + eng_action_down("left");
// 反向:axis 0 负 -> left 的 501 绑定
eng_input_feed_axis(0, -0.9);
print "act_left2=" + eng_action_value("left");
print "act_left2_down=" + eng_action_down("left");
print "act_right2_down=" + eng_action_down("right");
// 数字键当模拟量:0/1
eng_input_feed(87, 1);
print "act_jump=" + eng_action_value("jump");
// 越界轴
print "oob_axis=" + eng_pad_axis(9);
print "oob_axis_err=" + eng_last_error();
print "oob_feed=" + eng_input_feed_axis(9, 1.0);
print "oob_feed_err=" + eng_last_error();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("初始轴为 0", get(L, "axis0") == "0", get(L, "axis0"))
    check("注入轴值生效", get(L, "axis1") == "0.8", get(L, "axis1"))
    check("**动作模拟量取绑定的轴值**", get(L, "act_right") == "0.8", get(L, "act_right"))
    check("轴当数字源:正向 > 0.5 算按下", get(L, "act_right_down") == "1", get(L, "act_right_down"))
    check("反向轴不给正向动作值(0 或负数)", float(get(L, "act_left", "9")) <= 0.0,
          get(L, "act_left"))
    check("反向时 left 的模拟量 = 0.9", get(L, "act_left2") == "0.9", get(L, "act_left2"))
    check("反向时 left 按下", get(L, "act_left2_down") == "1", get(L, "act_left2_down"))
    check("反向时 right 不按下", get(L, "act_right2_down") == "0", get(L, "act_right2_down"))
    check("数字键的模拟量 = 1", get(L, "act_jump") == "1", get(L, "act_jump"))
    check("越界轴返回 0", get(L, "oob_axis") == "0", get(L, "oob_axis"))
    check("越界轴原因含范围", "out of range" in (get(L, "oob_axis_err") or ""),
          get(L, "oob_axis_err"))
    check("越界轴注入返回 -1", get(L, "oob_feed") == "-1", get(L, "oob_feed"))


# ---------- 4) 鼠标 ----------
def test_mouse():
    print("[鼠标:位置 / 世界坐标 / 按键 / 滚轮]")
    if not need_vm():
        skip("鼠标", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + '''
eng_input_feed_mouse(64.0, 32.0);
print "mx=" + eng_mouse_x() + "," + eng_mouse_y();
print "world_no_cam=" + eng_mouse_world_x() + "," + eng_mouse_world_y();
// 相机 (10,20) + 2 倍缩放:世界 = 屏幕/2 + (10,20) -> (42,36)
let cam = eng_object_new();
eng_attach(cam, "transform");
eng_attach(cam, "camera");
eng_set_f(cam, "camera", "x", 10.0);
eng_set_f(cam, "camera", "y", 20.0);
eng_set_f(cam, "camera", "zoom", 2.0);
eng_set_i(cam, "camera", "active", 1);
print "world_cam=" + eng_mouse_world_x() + "," + eng_mouse_world_y();
// 按键 + 滚轮
eng_input_feed(256, 1);
eng_input_feed(258, 1);
print "btn_l=" + eng_mouse_down(0) + " btn_r=" + eng_mouse_down(1) + " btn_m=" + eng_mouse_down(2);
print "wheel0=" + eng_mouse_wheel();
print "wheel_feed=" + eng_input_feed_wheel(3);
print "wheel1=" + eng_mouse_wheel();
// 帧边界清滚轮
eng_frame_begin();
print "wheel2=" + eng_mouse_wheel();
eng_frame_end();
print "wheel3=" + eng_mouse_wheel();
// 越界鼠标键
print "bad_btn=" + eng_mouse_down(5);
print "bad_btn_err=" + eng_last_error();
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("鼠标位置可注入", get(L, "mx") == "64,32", get(L, "mx"))
    check("无相机时世界 == 屏幕", get(L, "world_no_cam") == "64,32", get(L, "world_no_cam"))
    check("**相机换算正确(64/2+10, 32/2+20)**", get(L, "world_cam") == "42,36",
          get(L, "world_cam"))
    check("鼠标左右中键状态", get(L, "btn_l") == "1 btn_r=0 btn_m=1", get(L, "btn_l"))
    check("初始滚轮为 0", get(L, "wheel0") == "0", get(L, "wheel0"))
    check("注入滚轮成功", get(L, "wheel_feed") == "0", get(L, "wheel_feed"))
    check("滚轮累加", get(L, "wheel1") == "3", get(L, "wheel1"))
    check("滚轮在帧开始时清零", get(L, "wheel2") == "0", get(L, "wheel2"))
    check("帧结束后仍为 0", get(L, "wheel3") == "0", get(L, "wheel3"))
    check("越界鼠标键返回 0", get(L, "bad_btn") == "0", get(L, "bad_btn"))
    check("越界鼠标键原因可读", "out of range" in (get(L, "bad_btn_err") or ""),
          get(L, "bad_btn_err"))


# ---------- 5) eng_run 主循环 ----------
def test_run_loop():
    print("[eng_run / eng_run_frames 主循环]")
    if not need_vm():
        skip("主循环", "需要 DLL 与 vm.exe")
        return
    # 语言没有全局变量,所以计数器放在组件字段里(这本来也是引擎的状态模型)
    rc, L, err = run_dex('''include "dexgame_fast";
eng_init_offscreen(16, 16);
eng_physics_set_auto(0);              // 手动物理:eng_dt 仍要有帧间隔
eng_bind_default_actions();

eng_object_new();                     // 废弃一个:验证名字查找不依赖 id
let st = eng_object_new();
eng_attach(st, "transform");
eng_attach(st, "sprite");             // 借字段当计数器:layer = update 次数,order = draw 次数
eng_set_i(st, "sprite", "layer", 0);
eng_set_i(st, "sprite", "order", 0);
eng_set_i(st, "sprite", "tint", 0);
print "named=" + eng_set_name(st, "state");
print "roundtrip=" + eng_name(st);

// 回调里看不到主程序的 let(语言没有全局变量),所以靠名字找实体
func on_start() {
    let s = eng_find("state");
    eng_set_i(s, "sprite", "tint", 1);
}
func on_update(dt: float) {
    let s = eng_find("state");
    eng_set_i(s, "sprite", "layer", eng_get_i(s, "sprite", "layer") + 1);
    // eng_dt() 是墙钟毫秒精度:2帧之间不到 1ms 就会是 0。
    // 这里故意忙等到下一毫秒,让时间到帧间隔真正非零。
    let t0 = eng_now_ms();
    while eng_now_ms() == t0 { }
    if dt > 0.0 {
        eng_set_i(s, "sprite", "tint", eng_get_i(s, "sprite", "tint") + 1);
    }
}
func on_draw() {
    let s = eng_find("state");
    eng_set_i(s, "sprite", "order", eng_get_i(s, "sprite", "order") + 1);
}

eng_run_frames("on_start", "on_update", "on_draw", 5);
print "updates=" + eng_get_i(st, "sprite", "layer");
print "draws=" + eng_get_i(st, "sprite", "order");
print "started=" + eng_get_i(st, "sprite", "tint");   // 1 + 帧间隔>0 的次数
print "running=" + eng_running();
print "dt0=" + eng_dt();
print "find_missing=" + eng_find("nobody");
print "name_missing=[" + eng_name(999999) + "]";
eng_shutdown();
''')
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("**on_update 被调用 5 次**", get(L, "updates") == "5", get(L, "updates"))
    check("**on_draw 被调用 5 次**", get(L, "draws") == "5", get(L, "draws"))
    check("on_start 被调用过", int(get(L, "started", "0")) >= 1, get(L, "started"))
    check("**循环里 eng_dt() 确实是正的帧间隔**(手动物理也一样)",
          int(get(L, "started", "0")) >= 2, get(L, "started"))
    check("循环期间引擎仍在运行", get(L, "running") == "1", get(L, "running"))
    check("手动物理模式下 eng_dt 也有值", float(get(L, "dt0", "0")) >= 0.0, get(L, "dt0"))
    check("实体命名成功", get(L, "named") == "0", get(L, "named"))
    check("名字读回一致", get(L, "roundtrip") == "state", get(L, "roundtrip"))
    check("回调靠 eng_find 拿到状态实体(上面的计数器就是证据)",
          get(L, "updates") == "5", get(L, "updates"))
    check("查不到的名字返回 0", get(L, "find_missing") == "0",
          get(L, "find_missing"))
    check("不存在实体的名字是空串", get(L, "name_missing") == "[]",
          get(L, "name_missing"))


# ---------- 6) 双 VM ----------
DUAL_SRC = '''include "dexgame_fast";
eng_init_offscreen(16, 16);
eng_physics_set_auto(0);
eng_bind_default_actions();
eng_input_feed(87, 1);
eng_input_feed(256, 1);
eng_input_feed_axis(0, 0.5);
eng_frame_begin();
print eng_action_down("jump");
print eng_action_pressed("jump");
print eng_key_down(87);
print eng_mouse_down(0);
print eng_action_value("right");
print eng_pad_axis(0);
print eng_action_count();
eng_frame_end();
eng_frame_begin();
print eng_action_pressed("jump");
print eng_action_down("jump");
eng_frame_end();
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
    print("DEXCODE M4-a:输入(键盘/鼠标/手柄)+ 动作映射 + 主循环")
    test_actions()
    test_edges()
    test_axis()
    test_mouse()
    test_run_loop()
    test_dual_vm()
    for p in (TMP_SRC, TMP_BC, os.path.join(ROOT, "_input_test.dxasm")):
        if os.path.exists(p):
            os.remove(p)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
