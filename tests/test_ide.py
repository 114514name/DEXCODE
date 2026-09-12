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




# ============================================================
# 编辑器语法与缩进(本轮改进)
# ============================================================

def test_syntax_mask():
    """注释/字符串掩码:缩进判断的基础(纯函数,无需 GUI)。"""
    print("[editor] 注释与字符串掩码")
    from dexide import syntax as S

    src = ('let a = 1;\n'
           '// 行注释 { \n'
           'let s = "含 { 的字符串";\n'
           '/* 块注释\n'
           '   跨行 } { */\n'
           '# 井号注释 {\n'
           'func f() {\n'
           '    let b = 2;\n'
           '}\n')
    comments, strings = S.scan(src)
    check("识别行注释", any(c.line == 2 for c in comments))
    check("识别块注释(跨行)", any(c.line == 4 and c.end_line == 5 for c in comments))
    check("识别井号注释", any(c.line == 6 for c in comments))
    check("识别字符串", any(s.line == 3 for s in strings))
    # 行注释 + 块注释 + 井号注释 = 3 处
    check("注释 3 处", len(comments) == 3, len(comments))

    code = S.code_lines(src, comments, strings)
    # 注释/字符串里的花括号必须被掩掉,否则缩进会算错
    check("注释里的 { 被掩码", "{" not in code[1])
    check("字符串里的 { 被掩码", "{" not in code[2])
    check("块注释里的 { 被掩码", "{" not in code[4])
    check("真实代码的 { 保留", "{" in code[6])

    # 缩进:函数体一级 = 4
    check("函数体缩进 4", S.indent_for_line(code, 8) == 4)
    check("闭合行回到 0", S.indent_for_line(code, 9) == 0)


def test_highlight_categories():
    """语法着色:每个 token 种类都要有归属,不得全部落到 operator。

    这正是一个实际缺陷的回归护栏:词法器为 print/include/refer/extern/type
    给出的是**独立 token 种类**(不是 IDENT),旧实现只判断 IDENT,
    于是它们落到 `return "operator"` 兜底分支被当作运算符着色。"""
    print("[editor] 着色分类")
    from dexide import editor as E
    from dexlang.tokens import TokKind

    mapped = set(E._KIND_TAG.keys())
    # 关键:这些关键字必须有显式分类
    for kind in ("PRINT", "INCLUDE", "REFER", "EXTERN", "TYPE", "LET",
                 "FUNC", "IF", "ELSE", "WHILE", "RETURN", "RELEASE"):
        check(f"{kind} 有显式着色", kind in mapped, sorted(mapped))
    # 标点与运算符分开
    check("括号/分号归 delim", E._KIND_TAG["LPAREN"] == "delim" and E._KIND_TAG["SEMI"] == "delim")
    check("算术符号归 operator", E._KIND_TAG["PLUS"] == "operator")
    # 所有非 EOF token 种类都应被覆盖(避免再有"漏网"落到兜底)
    # IDENT 不在此表:它需要语义判断(是声明名/调用/变量/类型),由 _token_tag 单独处理
    missing = [k.name for k in TokKind
               if k.name not in ("EOF", "IDENT") and k.name not in mapped]
    check("除 IDENT 外的 token 种类已覆盖", not missing, missing)


def test_editor_indent_and_editing():
    """编辑器的缩进与编辑便利功能(需要 Tk;失败则跳过)。"""
    print("[editor] 缩进与编辑功能")
    import tkinter as tk
    from dexide.editor import CodeEditor

    try:
        root = tk.Tk()
        root.withdraw()
    except Exception as e:
        print(f"  SKIP  (无 Tk: {e})")
        return
    from dexide.analyzer import ProjectAnalyzer
    an = ProjectAnalyzer(SAMPLES)
    an.reanalyze()
    tmp = os.path.join(SAMPLES, "_ide_edit_tmp.dex")

    class Ev:
        def __init__(self, char="", keysym="", state=0):
            self.char = char
            self.keysym = keysym
            self.state = state

    def make(text):
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        ed = CodeEditor(root, tmp, an)
        ed.set_text(text)
        return ed

    try:
        # --- 回车缩进:按括号深度,而非"上一行是否以 { 结尾"---
        def enter_indent(text, line, want, label):
            ed = make(text)
            ed.text.mark_set("insert", f"{line}.end")
            ed._smart_newline()
            at = int(ed.text.index("insert").split(".")[0])
            seg = ed.text.get(f"{at}.0", f"{at}.0 lineend")
            got = len(seg) - len(seg.lstrip())
            ed.destroy()
            check(label, got == want, f"缩进={got} 期望={want}")

        enter_indent("func f() {\n", line=1, want=4, label="行尾 { 新行缩进 +4")
        enter_indent("func f() {\n    let x = 1;\n}\n", line=3, want=0, label="行尾 } 新行回到 0")
        enter_indent("// 说明 {\n", line=1, want=0, label="行注释里的 { 不缩进")
        enter_indent("/* 说明 {\n", line=1, want=0, label="块注释里的 { 不缩进")
        enter_indent('let s = "a{";\n', line=1, want=0, label="字符串里的 { 不缩进")
        enter_indent("func f() {\n    if 1 {\n        let x = 1;\n    }\n",
                     line=4, want=4, label="嵌套 } 新行回到内层 4")

        # --- 自动配对 ---
        ed = make("")
        ed._on_key_press(Ev(char="(", keysym="parenleft"))
        check("( 自动补 )", ed.text.get("1.0", "end-1c") == "()",
              repr(ed.text.get("1.0", "end-1c")))
        check("光标停在括号中间", ed.text.index("insert") == "1.1")
        ed.destroy()

        ed = make("")
        ed._on_key_press(Ev(char='"', keysym="quotedbl"))
        check('" 自动补 "', ed.text.get("1.0", "end-1c") == '""')
        ed.destroy()

        ed = make("()")
        ed.text.mark_set("insert", "1.1")
        ed._on_key_press(Ev(char=")", keysym="parenright"))
        check("已存在的闭括号被跳过", ed.text.get("1.0", "end-1c") == "()",
              repr(ed.text.get("1.0", "end-1c")))
        ed.destroy()

        ed = make("abc")
        ed.text.tag_add("sel", "1.0", "1.3")
        ed._on_key_press(Ev(char="(", keysym="parenleft"))
        check("选区被包裹", ed.text.get("1.0", "end-1c") == "(abc)",
              repr(ed.text.get("1.0", "end-1c")))
        ed.destroy()

        # --- Tab / Shift+Tab ---
        ed = make("let a = 1;\nlet b = 2;\n")
        ed.text.tag_add("sel", "1.0", "2.end")
        ed._ac_navigate(Ev(keysym="Tab"))
        body = ed.text.get("1.0", "end-1c").rstrip("\n")
        check("Tab 缩进两行", body == "    let a = 1;\n    let b = 2;", repr(body))
        ed._ac_navigate(Ev(keysym="ISO_Left_Tab"))
        body = ed.text.get("1.0", "end-1c").rstrip("\n")
        check("Shift+Tab 反缩进", body == "let a = 1;\nlet b = 2;", repr(body))
        ed.destroy()

        # --- Ctrl+/ 注释切换 ---
        ed = make("let x = 1;")
        ed.text.mark_set("insert", "1.0")
        ed._ac_navigate(Ev(keysym="slash", state=0x4))
        check("Ctrl+/ 加注释", ed.text.get("1.0", "end-1c") == "// let x = 1;",
              repr(ed.text.get("1.0", "end-1c")))
        ed._ac_navigate(Ev(keysym="slash", state=0x4))
        check("Ctrl+/ 取消注释", ed.text.get("1.0", "end-1c") == "let x = 1;",
              repr(ed.text.get("1.0", "end-1c")))
        ed.destroy()

        # --- 着色落点:标签必须对齐到字符(列号基准曾经差 1)---
        ed = make("type P { x: int; }\n")
        ed._apply_highlight()
        rs = ed.text.tag_ranges("decl")
        got = ed.text.get(rs[0], rs[1]) if rs else ""
        check("声明关键字着色对齐", got == "type", repr(got))
        rs2 = ed.text.tag_ranges("delim")
        check("标点着色存在", bool(rs2))
        ed.destroy()
    finally:
        try:
            root.destroy()
        except Exception:
            pass
        for f in (tmp,):
            if os.path.exists(f):
                os.remove(f)



def test_bracket_highlight():
    """括号配对高亮:光标处的括号与配对项;未配对标红;注释/字符串内不参与。"""
    print("[editor] 括号配对高亮")
    import tkinter as tk
    from dexide.editor import CodeEditor
    try:
        root = tk.Tk()
        root.withdraw()
    except Exception as e:
        print(f"  SKIP  (无 Tk: {e})")
        return
    from dexide.analyzer import ProjectAnalyzer
    an = ProjectAnalyzer(SAMPLES)
    an.reanalyze()
    tmp = os.path.join(SAMPLES, "_ide_bracket_tmp.dex")

    def check_at(text, line, col, label, want_match, want_unmatched):
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        ed = CodeEditor(root, tmp, an)
        ed.set_text(text)
        # mark_set 用 Tk 索引(1 基列 → Tk 列 col-1)
        ed.text.mark_set("insert", f"{line}.{col - 1}")
        ed.highlight_brackets()

        def chars(tag):
            rs = ed.text.tag_ranges(tag)
            return [ed.text.get(rs[i], rs[i + 1]) for i in range(0, len(rs), 2)]

        m, u = chars("bracket_match"), chars("bracket_unmatched")
        ed.destroy()
        check(label, m == want_match and u == want_unmatched,
              f"match={m} unmatched={u}")

    T = "func f(a, b) { return a; }\n"
    check_at(T, 1, 7, "光标在 ( 配 )", ["(", ")"], [])
    check_at(T, 1, 14, "光标在 { 配 }", ["{", "}"], [])
    # 配对结果按源码顺序返回(先是靠前的那个)
    check_at(T, 1, 26, "光标在 } 配 {", ["{", "}"], [])
    check_at(T, 1, 12, "光标在 ) 配 (", ["(", ")"], [])
    check_at(T, 1, 31, "行尾无括号则不高亮", [], [])
    check_at("func f(a, b {\n", 1, 13, "缺 ) 时 { 标红", [], ["{"])
    check_at("// note ( \nlet x = 1;\n", 1, 9, "注释里的括号不参与", [], [])
    check_at('let s = "a(b";\n', 1, 11, "字符串里的括号不参与", [], [])
    check_at("((a))\n", 1, 1, "嵌套:外层 ( 配最外层 )", ["(", ")"], [])
    check_at("((a))\n", 1, 2, "嵌套:内层 ( 配内层 )", ["(", ")"], [])

    try:
        if os.path.exists(tmp):
            os.remove(tmp)
    except OSError:
        pass
    try:
        root.destroy()
    except Exception:
        pass



def test_find_replace():
    """查找替换:大小写不敏感、多匹配、环绕导航、替换当前/全部、关闭后清理。"""
    print("[editor] 查找替换")
    import tkinter as tk
    from dexide.editor import CodeEditor
    try:
        root = tk.Tk()
        root.withdraw()
    except Exception as e:
        print(f"  SKIP  (无 Tk: {e})")
        return
    from dexide.analyzer import ProjectAnalyzer
    an = ProjectAnalyzer(SAMPLES)
    an.reanalyze()
    tmp = os.path.join(SAMPLES, "_ide_find_tmp.dex")
    SRC = "let alpha = 1;\nlet Beta = 2;\nprint alpha;\nprint Beta;\n"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(SRC)
    ed = CodeEditor(root, tmp, an)
    ed.set_text(SRC)
    ed.show_find()

    def nmatch():
        return len(ed.text.tag_ranges("find_all")) // 2

    ed._find_var.set("alpha")
    ed._refresh_find_marks()
    check("找到全部匹配", nmatch() == 2, nmatch())
    check("计数标签显示处数", ed._find_count.cget("text") == "2 处",
          ed._find_count.cget("text"))

    ed._find_var.set("beta")
    ed._refresh_find_marks()
    check("大小写不敏感匹配", nmatch() == 2, nmatch())

    ed._find_var.set("zzz")
    ed._refresh_find_marks()
    check("无匹配时计数提示", ed._find_count.cget("text") == "无匹配",
          ed._find_count.cget("text"))

    ed._find_var.set("alpha")
    ed._refresh_find_marks()
    check("默认选中第 0 个", ed._find_idx == 0, ed._find_idx)
    ed.find_next()
    check("下一个前进到第 1 个", ed._find_idx == 1, ed._find_idx)
    ed.find_next()
    check("下一个在末尾环绕", ed._find_idx == 0, ed._find_idx)
    ed.find_prev()
    check("上一个在开头环绕", ed._find_idx == 1, ed._find_idx)

    ed._find_var.set("alpha")
    ed._repl_var.set("ALPHA")
    ed._refresh_find_marks()
    ed.replace_all()
    body = ed.text.get("1.0", "end-1c")
    check("全部替换(大小写不敏感)",
          body == "let ALPHA = 1;\nlet Beta = 2;\nprint ALPHA;\nprint Beta;\n",
          repr(body))

    ed._find_var.set("print")
    ed._repl_var.set("show")
    ed._refresh_find_marks()
    ed.replace_current()
    body = ed.text.get("1.0", "end-1c")
    check("替换当前只改一处", body.count("show") == 1 and body.count("print") == 1,
          repr(body))

    ed.hide_find()
    check("关闭后清除高亮", nmatch() == 0, nmatch())
    check("关闭后面板隐藏", not ed._find_panel.winfo_ismapped())

    ed.destroy()
    try:
        root.destroy()
    except Exception:
        pass
    try:
        os.remove(tmp)
    except OSError:
        pass



def test_code_folding():
    """代码折叠:区域识别、折叠隐藏、展开还原、嵌套、注释/字符串不产生假块。"""
    print("[editor] 代码折叠")
    import tkinter as tk
    from dexide.editor import CodeEditor
    try:
        root = tk.Tk()
        root.withdraw()
    except Exception as e:
        print(f"  SKIP  (无 Tk: {e})")
        return
    from dexide.analyzer import ProjectAnalyzer
    an = ProjectAnalyzer(SAMPLES)
    an.reanalyze()
    tmp = os.path.join(SAMPLES, "_ide_fold_tmp.dex")
    SRC = ("func f() {\n"
           "    let a = 1;\n"
           "    if a {\n"
           "        print a;\n"
           "    }\n"
           "    return a;\n"
           "}\n"
           "let x = 1;\n")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(SRC)
    ed = CodeEditor(root, tmp, an)
    ed.set_text(SRC)
    ed.compute_folds()
    t = ed.text

    check("识别出两个折叠块", sorted(ed._fold_regions) == [1, 3],
          ed._fold_regions)
    check("折叠块行数正确", ed._fold_regions.get(1, (0, 0))[1] == 6,
          ed._fold_regions)

    ed.fold(1)
    rs = t.tag_ranges("fold_hidden")
    check("折叠后产生隐藏区间", len(rs) == 2, [str(x) for x in rs])
    check("隐藏区间覆盖块体", len(rs) == 2 and t.get(rs[0], rs[1]).lstrip().startswith("let a"),
          t.get(rs[0], rs[1]) if len(rs) == 2 else "")
    check("行尾给出折叠提示", "⋯" in t.get("1.0", "1.end"), t.get("1.0", "1.end"))
    check("文本内容未被改动(折叠只是显示层)", t.get("1.0", "end-1c") != SRC
          and SRC.replace("func f() {", "func f() {⋯ 6 行", 1) == t.get("1.0", "end-1c"),
          repr(t.get("1.0", "end-1c")))

    ed.fold(3)
    check("支持嵌套折叠", 3 in ed._folded and 1 in ed._folded, ed._folded)

    ed.unfold(3)
    check("展开内层不影响外层", ed._folded == {1}, ed._folded)
    ed.unfold(1)
    check("全部展开后无隐藏区间", not t.tag_ranges("fold_hidden"))
    check("展开后文本完全还原", t.get("1.0", "end-1c") == SRC,
          repr(t.get("1.0", "end-1c")))

    ed.fold(1)
    ed.fold(3)
    ed.unfold_all()
    check("unfold_all 还原", t.get("1.0", "end-1c") == SRC and not ed._folded)

    # 注释/字符串里的花括号不应产生折叠块
    tricky = '// 说明 {\nlet s = "a{b";\n'
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(tricky)
    ed.set_text(tricky)
    ed.compute_folds()
    check("注释/字符串里的花括号不产生假折叠块", not ed._fold_regions,
          ed._fold_regions)

    # 单行块 {} 不必折叠
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("func g() { return 1; }\n")
    ed.set_text("func g() { return 1; }\n")
    ed.compute_folds()
    check("单行块不产生折叠", not ed._fold_regions, ed._fold_regions)

    ed.destroy()
    try:
        root.destroy()
    except Exception:
        pass
    try:
        os.remove(tmp)
    except OSError:
        pass


def main():
    print("DEXIDE 测试")
    test_analyzer()
    test_gui()
    test_ime_safety()
    test_compile_tools()
    test_terminal_run()
    test_include_dirs_and_settings()
    test_editing()
    test_syntax_mask()
    test_highlight_categories()
    test_editor_indent_and_editing()
    test_bracket_highlight()
    test_find_replace()
    test_code_folding()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
