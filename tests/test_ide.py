#!/usr/bin/env python3
"""DEXIDE 测试:分析器(无需 GUI)+ 可选 GUI 冒烟。

运行: python tests/test_ide.py
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

SAMPLES = os.path.join(ROOT, "examples")

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


def test_analyzer():
    print("[analyzer]")
    from dexide.analyzer import ProjectAnalyzer
    an = ProjectAnalyzer(SAMPLES)
    an.reanalyze()
    dex_count = sum(1 for p in an.files if p.endswith(".dex"))
    check("扫描到多个 .dex", dex_count >= 7, list(an.files))
    fa = an.files.get(os.path.join(SAMPLES, "fib.dex"))
    check("fib 函数被分析", fa is not None and "fib" in fa.funcs)
    if fa and "fib" in fa.funcs:
        check("fib arity=1", fa.funcs["fib"].arity == 1)
        check("fib 递归调用检测", "fib" in fa.funcs["fib"].callees)
    um = an.files.get(os.path.join(SAMPLES, "math", "use_math.dex"))
    check("use_math 原生函数", um is not None and len(um.natives) == 6, len(um.natives) if um else 0)
    ns = an.native_names()
    check("聚合原生库(math/img/std)",
          {"dex_add", "dex_render", "dex_str_concat", "dex_rand"} <= ns, sorted(ns))
    check("include 解析", um and um.includes and um.includes[0][3].endswith("math.dexdef"))
    check("无编译错误", all(not fa.problems for fa in an.files.values()))

    # 补全与悬停
    cands = an.autocomplete(os.path.join(SAMPLES, "math", "use_math.dex"), "main", "dex_")
    check("补全原生函数", any(c[0] == "dex_add" for c in cands), cands)
    info = an.hover(os.path.join(SAMPLES, "math", "use_math.dex"), "dex_add")
    check("悬停原生签名", info is not None and "ii:i" in info[1] and "-> i" in info[0],
          info)
    info2 = an.hover(os.path.join(SAMPLES, "fib.dex"), "fib")
    check("悬停函数信息", info2 is not None and "arity=1" in info2[1], info2)

    # 错误问题
    err_file = os.path.join(SAMPLES, "_ide_err.dex")
    with open(err_file, "w", encoding="utf-8") as f:
        f.write("func f() { return 1; }\nprint nope(2);\n")
    fa_err = an.analyze_path(err_file)
    check("捕获未定义函数错误", any("undefined function" in p.msg for p in fa_err.problems),
          [p.msg for p in fa_err.problems])
    os.remove(err_file)


def test_gui():
    print("[gui smoke]")
    try:
        import tkinter as tk
    except Exception as e:
        print(f"  SKIP  无 tkinter: {e}")
        return
    try:
        from dexide.ide import DexIDE
        import time
        app = DexIDE(root_dir=SAMPLES, start_file=os.path.join(SAMPLES, "fib.dex"))

        def cycle():
            try:
                ed = app.tabs[app.active_path]
                ed.breakpoints = {10}
                app.debug_start()
                time.sleep(0.4)
                ok1 = app.debug is not None and app.debug.vm.current_line() == 10
                app.debug_command("step_over")
                time.sleep(0.3)
                ok2 = app.debug is not None and app.debug.vm._stop_reason == "step"
                app.debug_stop()
                check("GUI 断点暂停", ok1)
                check("GUI 单步", ok2)
            except Exception as e:
                check("GUI 冒烟异常", False, repr(e))
            finally:
                app.destroy()

        app.after(300, cycle)
        app.mainloop()
    except tk.TclError as e:
        print(f"  SKIP  无显示环境: {e}")


def _key(keysym, char="", state=0):
    return type("E", (), {"keysym": keysym, "char": char, "state": state})()


def test_ime_safety():
    """自动补全兼容中文输入法:弹窗为被动形式、绝不抢焦点(默认开启)。"""
    print("[ime safety]")
    try:
        import tkinter as tk
    except Exception as e:
        print(f"  SKIP  无 tkinter: {e}")
        return
    try:
        from dexide.ide import DexIDE
        import time
        app = DexIDE(root_dir=SAMPLES, start_file=os.path.join(SAMPLES, "fib.dex"))

        def pump(ms=0.3):
            end = time.time() + ms
            while time.time() < end:
                app.update()
                time.sleep(0.02)

        def run():
            try:
                ed = app.tabs[app.active_path]
                # 默认开启:输入 ASCII 字母触发补全
                ed.text.delete("1.0", "end")
                ed.text.insert("insert", "pr")
                ed.text.mark_set("insert", "end")
                ed._on_key(_key("i", "i"))
                pump(0.5)
                check("默认开启时输入字母触发补全", ed._ac_popup is not None)

                # 关键:弹窗打开时焦点仍在编辑器(不抢焦点 → IME 组合不被终止)
                check("补全弹窗不抢焦点", "text" in str(app.focus_get()),
                      str(app.focus_get()))

                # 编辑器内导航并提交,无多余换行
                ed._ac_navigate(_key("Down"))
                ed._ac_navigate(_key("Return"))
                pump(0.1)
                txt = ed.text.get("1.0", "end-1c")
                check("提交插入候选", "print" in txt, repr(txt))
                check("提交后无多余换行", not txt.endswith("\n"))

                # 关闭后输入字母不再触发
                ed.set_auto_complete(False)
                ed.text.delete("1.0", "end")
                ed.text.insert("insert", "de")
                ed.text.mark_set("insert", "end")
                ed._on_key(_key("e", "e"))
                pump(0.4)
                check("关闭后不再触发", ed._ac_popup is None)
                # 重新开启恢复
                ed.set_auto_complete(True)
                ed._on_key(_key("e", "e"))
                pump(0.5)
                check("重新开启恢复自动补全", ed._ac_popup is not None)
            except Exception as e:
                check("IME 测试异常", False, repr(e))
            finally:
                app.destroy()

        app.after(300, run)
        app.mainloop()
    except tk.TclError as e:
        print(f"  SKIP  无显示环境: {e}")


def test_compile_tools():
    """IDE 的"仅编译为汇编"与"汇编→字节码"功能。"""
    print("[compile tools]")
    try:
        import tkinter as tk
    except Exception as e:
        print(f"  SKIP  无 tkinter: {e}")
        return
    try:
        from dexide.ide import DexIDE
        import time
        tmp = os.path.join(ROOT, "_ide_compile_test.dex")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write("func sq(x) { return x * x; }\nprint sq(4);\n")
        app = DexIDE(root_dir=ROOT, start_file=tmp)

        def pump(ms=0.3):
            end = time.time() + ms
            while time.time() < end:
                app.update()
                time.sleep(0.02)

        def run():
            try:
                # 仅编译为汇编
                app.compile_to_asm()
                pump()
                asm_path = os.path.splitext(tmp)[0] + ".dxasm"
                check("生成 .dxasm", os.path.exists(asm_path))
                check("汇编文件被打开", asm_path in app.tabs, list(app.tabs))
                if os.path.exists(asm_path):
                    with open(asm_path, encoding="utf-8") as f:
                        content = f.read()
                    check("汇编内容含 .func 与 CALL",
                          ".func main 0" in content and "@sq" in content)

                # 汇编 → 字节码
                app.assemble_to_bc()
                pump()
                bc_path = os.path.splitext(tmp)[0] + ".dexbc"
                check("生成 .dexbc", os.path.exists(bc_path))
                if os.path.exists(bc_path):
                    from dexlang import decode
                    prog = decode(open(bc_path, "rb").read())
                    check(".dexbc 可反汇编(2 个函数)", len(prog.funcs) == 2, len(prog.funcs))
            except Exception as e:
                check("编译工具测试异常", False, repr(e))
            finally:
                for p in (tmp, os.path.splitext(tmp)[0] + ".dxasm",
                          os.path.splitext(tmp)[0] + ".dexbc"):
                    try:
                        os.remove(p)
                    except OSError:
                        pass
                app.destroy()

        app.after(300, run)
        app.mainloop()
    except tk.TclError as e:
        print(f"  SKIP  无显示环境: {e}")


def test_terminal_run():
    """run_in_terminal:编译当前 .dex → 在系统终端(新控制台)运行 vm.exe。"""
    print("[terminal run]")
    try:
        import tkinter as tk
        import subprocess as _sp
    except Exception as e:
        print(f"  SKIP  无 tkinter: {e}")
        return
    try:
        from dexide.ide import DexIDE
        tmp = os.path.join(ROOT, "_ide_term_test.dex")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write('print "hi from terminal";\n')
        app = DexIDE(root_dir=ROOT)
        calls = []
        orig = _sp.Popen

        class _FakeProc:
            def __init__(self, *a, **k):
                pass

        def fake_popen(*a, **k):
            calls.append((a, k))
            return _FakeProc()

        try:
            _sp.Popen = fake_popen
            app._create_editor(tmp, 'print "hi from terminal";\n')
            app.active_path = tmp
            app.run_in_terminal()
            check("Popen 被调用", len(calls) == 1, calls)
            if calls:
                args, kw = calls[0]
                joined = " ".join(str(x) for x in args)
                check("命令含 vm.exe", "vm" in joined, joined)
                check("命令含 .dexbc", "_ide_term_test.dexbc" in joined, joined)
                check("新控制台标志", kw.get("creationflags", 0) != 0, kw)
            bc = os.path.splitext(tmp)[0] + ".dexbc"
            check("生成了 .dexbc", os.path.exists(bc), bc)
            if os.path.exists(bc):
                with open(bc, "rb") as f:
                    check(".dexbc 是 DEXC 字节码", f.read(4) == b"DEXC")
        finally:
            _sp.Popen = orig
            # 关键回归:cmd /k 必须用"字符串命令行+双引号包裹",否则 cmd 报
            # "不是内部或外部命令"。Popen 恢复后,用 /c 前台执行验证(引号规则同 /k)。
            if calls and isinstance(calls[0][0][0], str):
                cmdline = calls[0][0][0]
                check("cmd /k 字符串命令", cmdline.startswith('cmd /k "'), cmdline[:60])
                r = _sp.run(cmdline.replace("/k ", "/c ", 1),
                            capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=30)
                check("cmd 能解析并运行",
                      r.returncode == 0 and "hi from terminal" in r.stdout
                      and "不是内部或外部命令" not in r.stderr,
                      repr(r.stdout) + repr(r.stderr))
            for p in (tmp, os.path.splitext(tmp)[0] + ".dexbc",
                      os.path.splitext(tmp)[0] + ".dxasm"):
                try:
                    os.remove(p)
                except OSError:
                    pass
            app.destroy()
    except tk.TclError as e:
        print(f"  SKIP  无显示环境: {e}")


def test_include_dirs_and_settings():
    """IDE 适配项目结构(libs)+ 设置(vm 位置 / 库目录)。"""
    print("[include dirs & settings]")
    try:
        import tkinter as tk
    except Exception as e:
        print(f"  SKIP  无 tkinter: {e}")
        return
    try:
        from dexide.ide import DexIDE
        from dexide import settings as st

        app = DexIDE(root_dir=ROOT)
        try:
            # 1) include_dirs 自动含项目 libs
            dirs = app._include_dirs()
            check("include_dirs 含项目 libs", os.path.join(ROOT, "libs") in dirs, dirs)

            # 2) 设置:保存/加载往返(vm 路径 + 库目录)
            tmp_cfg = os.path.join(ROOT, "_ide_cfg_test.json")
            custom_lib = ROOT   # 一个真实存在的目录
            st.save({"vm_exe": r"C:\fake\vm.exe", "lib_dirs": [custom_lib]}, tmp_cfg)
            data = st.load(tmp_cfg)
            check("设置保存/加载", data["vm_exe"] == r"C:\fake\vm.exe"
                  and custom_lib in data["lib_dirs"], data)

            # 3) 应用设置后自定义库目录进入 include
            app.settings = data
            check("自定义库目录加入 include", custom_lib in app._include_dirs(),
                  app._include_dirs())
            # 自定义 vm 不存在 → 回退项目默认
            v = app._vm_exe()
            check("vm_exe 回退默认", v.endswith(("vm.exe", "vm")), v)

            # 4) 编译带 include 的示例应成功(适配 libs 结构)
            ui_path = os.path.join(ROOT, "examples", "ui", "demo_ui.dex")
            if os.path.exists(ui_path):
                app.open_file(ui_path)
                app.compile_to_asm()
                asm_path = os.path.splitext(ui_path)[0] + ".dxasm"
                check("编译含 include 示例成功", os.path.exists(asm_path), asm_path)
                if os.path.exists(asm_path):
                    os.remove(asm_path)
        finally:
            for p in (tmp_cfg,):
                try:
                    os.remove(p)
                except OSError:
                    pass
            app.destroy()
    except tk.TclError as e:
        print(f"  SKIP  无显示环境: {e}")


def test_editing():
    """编辑手感:智能换行、补全下箭头导航、input 卡死检测。"""
    print("[editing]")
    try:
        import tkinter as tk
        import subprocess as _sp
    except Exception as e:
        print(f"  SKIP  无 tkinter: {e}")
        return
    try:
        from dexide.ide import DexIDE
        app = DexIDE(root_dir=ROOT)
        tmp = os.path.join(ROOT, "_ide_edit_test.dex")
        app._create_editor(tmp, "")
        ed = app.tabs[tmp]
        app.active_path = tmp

        # 1) 智能换行:行尾 { → 下一行缩进 4;只产生一个换行(不是双空行)
        ed.text.delete("1.0", "end")
        ed.text.insert("1.0", "if x {")
        ed.text.mark_set("insert", "end")
        ed._ac_navigate(_key("Return"))
        check("回车只产生一行新行", ed._n_lines() == 2, ed._n_lines())
        line2 = ed.text.get("2.0", "2.0 lineend")
        check("自动缩进 4 空格", line2 == "    ", repr(line2))
        ed._on_key(_key("Return"))          # KeyRelease 不应二次换行
        check("KeyRelease 不双换行", ed._n_lines() == 2, ed._n_lines())

        # 2) 补全下箭头导航(不关弹窗;KeyRelease 也不误关)
        ed.text.delete("1.0", "end")
        ed.text.insert("1.0", "print de")
        ed.text.mark_set("insert", "end")
        ed._show_ac()
        check("补全弹窗打开", ed._ac_popup is not None)
        if ed._ac_popup is not None:
            sel0 = ed._ac_sel
            ed._ac_navigate(_key("Down"))
            check("下箭头改选", ed._ac_sel == sel0 + 1 and ed._ac_popup is not None,
                  (ed._ac_sel, sel0))
            ed._on_key(_key("Down"))         # KeyRelease
            check("KeyRelease 不误关弹窗", ed._ac_popup is not None)
            ed._close_ac()

        # 3) run_program 检测 dex_input → 自动改用系统终端(不卡死)
        src = 'include "std";\nprint "start";\nlet s = dex_input();\nprint s;\n'
        ed.text.delete("1.0", "end")
        ed.text.insert("1.0", src)
        calls = []
        orig = _sp.Popen

        class _FakeProc:
            def __init__(self, *a, **k):
                pass

        def fake_popen(*a, **k):
            calls.append((a, k))
            return _FakeProc()

        _sp.Popen = fake_popen
        try:
            app.run_program()
            check("input 自动转终端运行", len(calls) == 1, calls)
        finally:
            _sp.Popen = orig
        for p in (tmp, os.path.splitext(tmp)[0] + ".dxasm",
                  os.path.splitext(tmp)[0] + ".dexbc"):
            try:
                os.remove(p)
            except OSError:
                pass
        app.destroy()
    except tk.TclError as e:
        print(f"  SKIP  无显示环境: {e}")


def main():
    print("DEXIDE 测试")
    test_analyzer()
    test_gui()
    test_ime_safety()
    test_compile_tools()
    test_terminal_run()
    test_include_dirs_and_settings()
    test_editing()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
