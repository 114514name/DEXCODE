#!/usr/bin/env python3
"""DEXCODE M4-b 测试:音频(XAudio2 + 手写 WAV 解析)。

覆盖:
  1) WAV 解析:16 位单声道 / 8 位双声道 的时长/取样率/声道/位深;按路径缓存;
     释放后旧 id 失效
  2) 解析器的**拒绝路径**:不是 RIFF、非 PCM(float)、24 位、3 声道、文件不存在
  3) 播放:XAudio2 播放实例(play/playing/stop/count/volume/master volume)、
     同一声音同时多次播放、实例上限;设备不可用时**跳过而不是失败**(库侧不崩)
  4) audio 组件:场景里能带声音(path 立刻解码、音量、循环、play_on_start),
     JSON 往返只存持久字段,卸载/删除时停掉实例
  5) 双 VM 一致性

运行: python tests/test_audio.py
"""

import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from dexlang import Lexer, Parser, compile_program, assemble, DexError  # noqa: E402
from dexlang.pyvm import run_program  # noqa: E402

LIBS = os.path.join(ROOT, "libs")
DLL = os.path.join(LIBS, "dexgame", "libdexgame.dll")
VM = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
TMP_SRC = os.path.join(ROOT, "_audio_test.dex")
TMP_BC = os.path.join(ROOT, "_audio_test.dexbc")
TMP_BAD = os.path.join(ROOT, "_audio_bad.wav")
TMP_JSON = os.path.join(ROOT, "_audio_scene.json")

WAV16 = "tests/fixtures/silence16m.wav"      # 16 位单声道 8000Hz 0.1s
WAV8S = "tests/fixtures/silence8s.wav"       # 8 位双声道 22050Hz 0.1s

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


def wav(channels=1, rate=8000, bits=16, frames=100, tag=1, value=0):
    """手写一个 WAV(用于构造"该被拒绝"的样本)。"""
    ba = channels * bits // 8
    fmt_size = 16 if tag == 1 else 18
    data = bytearray()
    for _ in range(frames):
        for _c in range(channels):
            if bits == 16:
                data += struct.pack("<h", value)
            elif bits == 24:
                data += b"\x00\x00\x00"
            else:
                data += struct.pack("<B", value & 0xFF)
    fmt_body = struct.pack("<HHIIHH", tag, channels, rate, rate * ba, ba, bits)
    if fmt_size == 18:
        fmt_body += struct.pack("<H", 0)
    body = (b"RIFF" + struct.pack("<I", 4 + 8 + fmt_size + 8 + len(data)) + b"WAVE"
            + b"fmt " + struct.pack("<I", fmt_size) + fmt_body
            + b"data" + struct.pack("<I", len(data)) + bytes(data))
    return body


HEAD = '''include "dexgame";
eng_init_offscreen(8, 8);
'''


# ---------- 1) 解析 ----------
def test_parse():
    print("[WAV 解析与缓存]")
    if not need_vm():
        skip("解析", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + '''
print "ok=" + eng_audio_ok();
let s = eng_sound_load("%s");
print "load=" + s;
print "path=" + eng_sound_path(s);
print "dur=" + eng_sound_duration(s);
print "rate=" + eng_sound_sample_rate(s);
print "ch=" + eng_sound_channels(s);
print "bits=" + eng_sound_bits(s);
print "count=" + eng_sound_count();
let again = eng_sound_load("%s");
print "cached=" + (again == s);
let s8 = eng_sound_load("%s");
print "rate8=" + eng_sound_sample_rate(s8);
print "ch8=" + eng_sound_channels(s8);
print "bits8=" + eng_sound_bits(s8);
print "dur8=" + eng_sound_duration(s8);
print "count2=" + eng_sound_count();
// 释放后旧 id 失效
print "free=" + eng_sound_free(s);
print "dur_after=" + eng_sound_duration(s);
print "err_after=[" + eng_last_error() + "]";
print "count3=" + eng_sound_count();
// 重新加载:同一个槽位但新句柄
let s3 = eng_sound_load("%s");
print "reload_ok=" + (s3 > 0);
print "count4=" + eng_sound_count();
print "bad_id=" + eng_sound_sample_rate(999999);
print "bad_id_err=[" + eng_last_error() + "]";
eng_shutdown();
''' % (WAV16, WAV16, WAV8S, WAV16))
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("声音加载成功", get(L, "load") not in (None, "-1"), get(L, "load"))
    check("路径读回一致", get(L, "path") == WAV16, get(L, "path"))
    check("**时长 = 0.1 秒**", abs(float(get(L, "dur", "0")) - 0.1) < 1e-6, get(L, "dur"))
    check("取样率 = 8000", get(L, "rate") == "8000", get(L, "rate"))
    check("声道 = 1", get(L, "ch") == "1", get(L, "ch"))
    check("位深 = 16", get(L, "bits") == "16", get(L, "bits"))
    check("声音计数 = 1", get(L, "count") == "1", get(L, "count"))
    check("**同一路径命中缓存(同一个 id)**", get(L, "cached") == "1", get(L, "cached"))
    check("8 位双声道:取样率 22050", get(L, "rate8") == "22050", get(L, "rate8"))
    check("8 位双声道:声道 2", get(L, "ch8") == "2", get(L, "ch8"))
    check("8 位双声道:位深 8", get(L, "bits8") == "8", get(L, "bits8"))
    check("8 位双声道:时长 0.1", abs(float(get(L, "dur8", "0")) - 0.1) < 1e-6, get(L, "dur8"))
    check("两个不同路径 → 2 个资源", get(L, "count2") == "2", get(L, "count2"))
    check("释放返回 0", get(L, "free") == "0", get(L, "free"))
    check("**释放后旧 id 立刻失效**", get(L, "dur_after") == "0", get(L, "dur_after"))
    check("失效原因可读", "does not exist" in (get(L, "err_after") or ""), get(L, "err_after"))
    check("释放后计数 -1", get(L, "count3") == "1", get(L, "count3"))
    check("可以重新加载", get(L, "reload_ok") == "1", get(L, "reload_ok"))
    check("重新加载后计数回到 2", get(L, "count4") == "2", get(L, "count4"))
    check("不存在的 id 取样率 = -1", get(L, "bad_id") == "-1", get(L, "bad_id"))
    check("不存在 id 的原因可读", "does not exist" in (get(L, "bad_id_err") or ""),
          get(L, "bad_id_err"))


# ---------- 2) 拒绝路径 ----------
def test_bad_files():
    print("[解析器的拒绝路径]")
    if not need_vm():
        skip("坏文件", "需要 DLL 与 vm.exe")
        return
    cases = [
        ("garbage.bin", b"this is not a wav file at all, just text padding........", "not a RIFF"),
        ("float.wav", wav(tag=3, value=0), "not PCM"),
        ("b24.wav", wav(bits=24), "8/16-bit"),
        ("ch3.wav", wav(channels=3), "mono/stereo"),
        ("tiny.wav", b"RIFF", "too small"),
    ]
    names = []
    for name, blob, _kw in cases:
        with open(TMP_BAD, "wb") as f:
            f.write(blob)
        names.append((name, _kw))
    # 逐个跑:每换一次内容就要重新加载,所以分成多次调用
    for name, kw in names:
        blob = dict((n, b) for n, b, _ in cases)[name]
        with open(TMP_BAD, "wb") as f:
            f.write(blob)
        rc, L, err = run_dex(HEAD + '''
print "r=" + eng_sound_load("%s");
print "err=[" + eng_last_error() + "]";
eng_shutdown();
''' % os.path.basename(TMP_BAD))
        check(f"拒绝 {name}", rc == 0 and get(L, "r") == "-1", (rc, L[:2]))
        check(f"拒因提到 {kw}", kw in (get(L, "err") or ""), get(L, "err"))
    # 不存在的文件
    rc, L, err = run_dex(HEAD + '''
print "r=" + eng_sound_load("no/such/sound.wav");
print "err=[" + eng_last_error() + "]";
print "empty=" + eng_sound_load("");
print "empty_err=[" + eng_last_error() + "]";
eng_shutdown();
''')
    check("不存在的文件返回 -1", get(L, "r") == "-1", get(L, "r"))
    check("拒因含路径", "no/such/sound.wav" in (get(L, "err") or ""), get(L, "err"))
    check("空路径返回 -1", get(L, "empty") == "-1", get(L, "empty"))
    check("空路径原因可读", "empty" in (get(L, "empty_err") or ""), get(L, "empty_err"))


# ---------- 3) 播放 ----------
def test_playback():
    print("[播放实例(没有设备时跳过)]")
    if not need_vm():
        skip("播放", "需要 DLL 与 vm.exe")
        return
    rc, L, err = run_dex(HEAD + '''
print "ok=" + eng_audio_ok();
let s = eng_sound_load("%s");
// loop=1(无限循环)+音量 0:不出声,而且不会因为“声音自己播完了”而让状态断言变得不稳定
// (第一版用了 loop=0 的 0.1 秒样本,机器一忙就会偶尔“已播完”)
let v = eng_sound_play(s, 1, 0.0);
print "v=" + v;
print "playing=" + eng_voice_playing(v);
print "count=" + eng_audio_playing_count();
print "vsound=" + (eng_voice_sound(v) == s);
print "vol=" + eng_voice_set_volume(v, 0.25);
print "stop=" + eng_voice_stop(v);
print "playing2=" + eng_voice_playing(v);
print "count2=" + eng_audio_playing_count();
print "stop_again=" + eng_voice_stop(v);
print "stop_again_err=[" + eng_last_error() + "]";
// 循环播放 + 同一声音多次播放
let l1 = eng_sound_play(s, 1, 0.0);
let l2 = eng_sound_play(s, 1, 0.0);
print "loop_a=" + eng_voice_playing(l1);
print "loop_b=" + eng_voice_playing(l2);
print "both=" + (l1 != l2);
print "count3=" + eng_audio_playing_count();
print "stopall=" + eng_audio_stop_all();
print "count4=" + eng_audio_playing_count();
// 主音量
print "master=" + eng_audio_set_master_volume(0.5);
print "master_v=" + eng_audio_master_volume();
print "master_bad=" + eng_audio_set_master_volume(1.0);
// 不存在的实例
print "noplay=" + eng_voice_playing(999);
print "novol=" + eng_voice_set_volume(999, 0.5);
print "novol_err=[" + eng_last_error() + "]";
eng_shutdown();
''' % WAV16)
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    if get(L, "ok") != "1":
        skip("播放相关断言", "本机没有可用的音频设备(eng_audio_ok=0)")
        return
    check("播放返回正实例 id", int(get(L, "v", "0")) > 0, get(L, "v"))
    check("**播放中 playing = 1**", get(L, "playing") == "1", get(L, "playing"))
    check("播放计数 = 1", get(L, "count") == "1", get(L, "count"))
    check("实例能查到来源声音", get(L, "vsound") == "1", get(L, "vsound"))
    check("播放中调音量返回 0", get(L, "vol") == "0", get(L, "vol"))
    check("停止返回 0", get(L, "stop") == "0", get(L, "stop"))
    check("停止后 playing = 0", get(L, "playing2") == "0", get(L, "playing2"))
    check("停止后计数 0", get(L, "count2") == "0", get(L, "count2"))
    check("**重复停止是幂等的**(声音自然播完后再 stop 不该报错)",
          get(L, "stop_again") == "0", get(L, "stop_again"))
    check("循环播放 A 在响", get(L, "loop_a") == "1", get(L, "loop_a"))
    check("循环播放 B 也在响", get(L, "loop_b") == "1", get(L, "loop_b"))
    check("**同一个声音可以同时响两次**", get(L, "both") == "1", get(L, "both"))
    check("播放计数 = 2", get(L, "count3") == "2", get(L, "count3"))
    check("全部停止返回 0", get(L, "stopall") == "0", get(L, "stopall"))
    check("全部停止后计数 0", get(L, "count4") == "0", get(L, "count4"))
    check("主音量设置返回 0", get(L, "master") == "0", get(L, "master"))
    check("主音量读回 0.5", get(L, "master_v") == "0.5", get(L, "master_v"))
    check("主音量可复原", get(L, "master_bad") == "0", get(L, "master_bad"))
    check("不存在的实例 playing = 0", get(L, "noplay") == "0", get(L, "noplay"))
    check("对不存在实例调音量返回 -1", get(L, "novol") == "-1", get(L, "novol"))
    check("原因可读", "not playing" in (get(L, "novol_err") or ""), get(L, "novol_err"))


# ---------- 4) audio 组件 ----------
def test_component():
    print("[audio 组件]")
    if not need_vm():
        skip("组件", "需要 DLL 与 vm.exe")
        return
    name = os.path.basename(TMP_JSON)
    rc, L, err = run_dex(HEAD + '''
print "comps=" + eng_comp_count();
let a = eng_object_new();
eng_attach(a, "transform");
eng_attach(a, "audio");
print "has=" + eng_has(a, "audio");
print "fields=" + eng_field_count("audio");
print "f0=[" + eng_field_name("audio", 0) + "]";
print "f0p=" + eng_field_persist("audio", 0);
print "voice_p=" + eng_field_persist("audio", 5);
print "set_path=" + eng_set_s(a, "audio", "path", "%s");
print "handle_ok=" + (eng_get_i(a, "audio", "handle") > 0);
print "set_vol=" + eng_set_f(a, "audio", "volume", 0.5);
print "set_loop=" + eng_set_i(a, "audio", "loop", 1);
print "save=" + eng_scene_save("%s");
// 换一个声音:句柄要跟着换
print "set_path2=" + eng_set_s(a, "audio", "path", "%s");
print "handle2_ok=" + (eng_get_i(a, "audio", "handle") > 0);
// 坏路径立刻报(不等播放)
print "bad_path=" + eng_set_s(a, "audio", "path", "no/such.wav");
print "bad_err=[" + eng_last_error() + "]";
eng_shutdown();
''' % (WAV16, name, WAV8S))
    check("运行退出码 0", rc == 0, err[:200])
    if rc != 0:
        return
    check("组件种类数 = 8(M4 起含 audio)", get(L, "comps") == "8", get(L, "comps"))
    check("可以挂 audio 组件", get(L, "has") == "1", get(L, "has"))
    check("audio 字段数 = 7", get(L, "fields") == "7", get(L, "fields"))
    check("字段 0 是 path", get(L, "f0") == "[path]", get(L, "f0"))
    check("path 是持久字段", get(L, "f0p") == "1", get(L, "f0p"))
    check("**voice 是运行时字段**", get(L, "voice_p") == "0", get(L, "voice_p"))
    check("设置路径成功", get(L, "set_path") == "0", get(L, "set_path"))
    check("**设置路径后立刻拿到句柄**", get(L, "handle_ok") == "1", get(L, "handle_ok"))
    check("设置音量成功", get(L, "set_vol") == "0", get(L, "set_vol"))
    check("设置循环成功", get(L, "set_loop") == "0", get(L, "set_loop"))
    check("保存场景成功", get(L, "save") == "0", get(L, "save"))
    check("换声音后句柄更新", get(L, "set_path2") == "0" and get(L, "handle2_ok") == "1",
          (get(L, "set_path2"), get(L, "handle2_ok")))
    check("坏路径立刻失败", get(L, "bad_path") == "-1", get(L, "bad_path"))
    check("坏路径原因含路径", "no/such.wav" in (get(L, "bad_err") or ""), get(L, "bad_err"))
    # JSON 里只应出现持久字段
    path = os.path.join(ROOT, TMP_JSON)
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        import json as _json
        doc = _json.loads(text)
        au = doc["objects"][0]["audio"]
        check("JSON 里有 audio 组件", isinstance(au, dict), au)
        # 注意:场景是在第二次 set_path **之前**保存的所以里面是第一个声音
        check("JSON 存了 path", au.get("path") == WAV16, au.get("path"))
        check("JSON 存了 volume", au.get("volume") == 0.5, au.get("volume"))
        check("JSON 存了 loop", au.get("loop") == 1, au.get("loop"))
        check("**JSON 不含运行时字段 handle/voice/playing**",
              "handle" not in au and "voice" not in au and "playing" not in au, list(au.keys()))
    else:
        check("场景文件已生成", False, path)


# ---------- 5) 双 VM ----------
DUAL_SRC = '''include "dexgame";
eng_init_offscreen(8, 8);
let s = eng_sound_load("tests/fixtures/silence16m.wav");
print eng_audio_ok();
print eng_sound_sample_rate(s);
print eng_sound_channels(s);
print eng_sound_bits(s);
print eng_sound_duration(s);
print eng_sound_count();
print eng_sound_path(s);
print eng_sound_load("no/such.wav");
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
    print("DEXCODE M4-b:音频(XAudio2 + 手写 WAV 解析)")
    test_parse()
    test_bad_files()
    test_playback()
    test_component()
    test_dual_vm()
    for p in (TMP_SRC, TMP_BC, TMP_BAD, TMP_JSON,
              os.path.join(ROOT, "_audio_test.dxasm")):
        if os.path.exists(p):
            os.remove(p)
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
