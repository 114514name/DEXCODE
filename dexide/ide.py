"""DEXIDE — VS Code 风格 DexLang 集成开发环境(Python + tkinter,零依赖)。

布局:
  菜单 / 工具栏
  左侧栏: 资源管理器 | 大纲 | 库(原生函数)
  中央:    标签页(可关闭)+ 代码编辑器(断点槽/行号/着色/悬停/补全)
  底部:    问题 | 控制台 | 调试(调用栈/变量/值栈/监视)
  状态栏
"""

import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

from dexlang import Lexer, Parser, compile_program, assemble, render, \
    parse_asm_text, DexError
from dexlang.pyvm import PyVM

from .theme import C
from .analyzer import ProjectAnalyzer
from .editor import CodeEditor, os_abspath
from . import settings as settings_mod

SUPPORTED = (".dex", ".dexdef", ".dxasm")


# ============================================================
# 调试控制器(后台线程驱动 PyVM)
# ============================================================
class DebugController:
    def __init__(self, vm, event_queue):
        """event_queue: 线程安全的 queue.Queue,事件由 UI 线程轮询。"""
        self.vm = vm
        self._queue = event_queue
        self._evt = threading.Event()
        self._cmd = None
        self._out_index = 0
        self.alive = True
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self, initial="run"):
        self._cmd = initial
        self.thread.start()

    def send(self, cmd):
        self._cmd = cmd
        self._evt.set()

    def stop(self):
        self._cmd = "stop"
        self._evt.set()

    def _emit(self, ev):
        try:
            self._queue.put(ev)
        except Exception:
            pass

    def _flush_output(self):
        out = self.vm.output
        while self._out_index < len(out):
            self._emit({"type": "output", "text": out[self._out_index]})
            self._out_index += 1

    def _run(self):
        try:
            while self.alive:
                if self._cmd is None:
                    self._evt.wait()
                    self._evt.clear()
                    if not self.alive:
                        break
                    continue
                cmd = self._cmd
                self._cmd = None
                if cmd == "stop":
                    break
                if cmd == "step_into":
                    self.vm.step_into()
                elif cmd == "step_over":
                    self.vm.step_over()
                elif cmd == "step_out":
                    self.vm.step_out()
                elif cmd == "run":
                    self.vm.run()
                self._flush_output()
                self.vm.run_until_stop()
                self._flush_output()
                if self.vm.error or self.vm.done:
                    self._emit({"type": "ended",
                                "error": self.vm.error,
                                "output": list(self.vm.output)})
                    self.alive = False
                    break
                self._emit({"type": "paused", "reason": self.vm._stop_reason})
        finally:
            # 调试线程退出时释放 ctypes 库句柄并删除静态库临时文件,
            # 否则反复调试会不断累积 temp 残留。
            try:
                self.vm.dispose()
            except Exception:
                pass


# ============================================================
# 主窗口
# ============================================================
class DexIDE(tk.Tk):
    def __init__(self, root_dir=None, start_file=None):
        super().__init__()
        self.title("DEXIDE — DexLang 集成开发环境")
        self.geometry("1440x900")
        self.minsize(1000, 640)
        self.configure(bg=C["crust"])

        self.root_dir = os.path.abspath(root_dir or os.getcwd())
        self.settings = settings_mod.load()
        self.analyzer = ProjectAnalyzer(self.root_dir,
                                        include_dirs=self._include_dirs())
        try:
            self.analyzer.reanalyze()
        except Exception as e:
            print("analyzer init failed:", e)

        self.tabs = {}            # path -> CodeEditor
        self._tab_order = []
        self.active_path = None
        self.debug = None
        self._debug_path = None
        self._watch_items = {}    # 表达式 -> 值
        self._dbg_queue = queue.Queue()
        self.after(50, self._poll_debug)

        self._apply_style()
        self._build_menu()
        self._build_toolbar()
        self._build_layout()
        self._build_statusbar()
        self._update_debug_buttons(enabled=False)
        self.after(150, self._refresh_sidebar)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        if start_file:
            self.open_file(start_file)

    # ---------- 路径/设置解析 ----------
    def _custom_lib_dirs(self):
        """用户自定义的库目录(仅保留存在的目录)。"""
        dirs = []
        for d in self.settings.get("lib_dirs", []):
            if d and os.path.isdir(d):
                d = os.path.abspath(d)
                if d not in dirs:
                    dirs.append(d)
        return dirs

    def _include_dirs(self):
        """编译用的 include 搜索目录:项目 libs/ + 用户自定义库目录。"""
        dirs = []
        for base in (self.root_dir, os.path.dirname(self.root_dir)):
            lib = os.path.join(base, "libs")
            if os.path.isdir(lib) and lib not in dirs:
                dirs.append(lib)
        for d in self._custom_lib_dirs():
            if d not in dirs:
                dirs.append(d)
        return dirs

    def _vm_exe(self):
        """C VM 可执行文件:优先用户设置(若存在),否则项目默认。"""
        custom = self.settings.get("vm_exe", "")
        if custom:
            custom = custom.strip().strip('"').strip("'")
        if custom and os.path.exists(custom):
            return os.path.abspath(custom)
        if getattr(sys, "frozen", False):
            root = os.path.dirname(os.path.abspath(sys.executable))
        else:
            root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        return os.path.join(root, "vm", "vm.exe" if os.name == "nt" else "vm")

    def _open_settings(self):
        """设置对话框:自定义 vm.exe 位置与库目录。"""
        win = tk.Toplevel(self)
        win.title("DEXIDE 设置")
        win.configure(bg=C["base"])
        win.transient(self)
        win.grab_set()
        win.resizable(False, False)
        pad = {"padx": 10, "pady": 4}
        btn = {"bg": C["surface0"], "fg": C["text"],
               "activebackground": C["surface1"], "activeforeground": C["text"]}

        tk.Label(win, text="C 虚拟机 (vm.exe,留空 = 使用项目默认)",
                 bg=C["base"], fg=C["text"]).pack(anchor="w", **pad)
        vm_row = tk.Frame(win, bg=C["base"])
        vm_row.pack(fill="x", **pad)
        vm_var = tk.StringVar(value=self.settings.get("vm_exe", ""))
        tk.Entry(vm_row, textvariable=vm_var, width=46, bg=C["mantle"],
                 fg=C["text"], insertbackground=C["text"]).pack(side="left")

        def _pick_vm():
            p = filedialog.askopenfilename(
                title="选择 C 虚拟机", initialdir=os.path.dirname(self._vm_exe()),
                filetypes=[("可执行文件", "*.exe"), ("所有文件", "*.*")])
            if p:
                vm_var.set(p)
        tk.Button(vm_row, text="浏览…", command=_pick_vm, **btn).pack(side="left", padx=6)

        tk.Label(win, text="库目录 (include 搜索,每行一个)",
                 bg=C["base"], fg=C["text"]).pack(anchor="w", **pad)
        lib_frame = tk.Frame(win, bg=C["base"])
        lib_frame.pack(fill="x", **pad)
        lib_lb = tk.Listbox(lib_frame, width=58, height=5, bg=C["mantle"],
                            fg=C["text"], selectbackground=C["surface1"],
                            highlightthickness=0)
        lib_lb.pack(side="left", fill="y")
        for d in self.settings.get("lib_dirs", []):
            lib_lb.insert("end", d)
        lib_btns = tk.Frame(lib_frame, bg=C["base"])
        lib_btns.pack(side="left", padx=6)

        def _add_lib():
            p = filedialog.askdirectory(title="选择库目录")
            if p:
                lib_lb.insert("end", os.path.abspath(p))

        def _del_lib():
            sel = lib_lb.curselection()
            if sel:
                lib_lb.delete(sel[0])

        tk.Button(lib_btns, text="添加…", command=_add_lib, **btn).pack(fill="x", pady=2)
        tk.Button(lib_btns, text="移除", command=_del_lib, **btn).pack(fill="x", pady=2)

        def _save():
            self.settings["vm_exe"] = vm_var.get().strip().strip('"').strip("'")
            self.settings["lib_dirs"] = [lib_lb.get(i) for i in range(lib_lb.size())]
            settings_mod.save(self.settings)
            # 重建分析器使新的库目录生效
            self.analyzer = ProjectAnalyzer(self.root_dir,
                                            include_dirs=self._include_dirs())
            try:
                self.analyzer.reanalyze()
            except Exception:
                pass
            self._refresh_sidebar()
            self.set_status("设置已保存")
            win.destroy()

        btns = tk.Frame(win, bg=C["base"])
        btns.pack(fill="x", **pad)
        tk.Button(btns, text="保存", command=_save, **btn).pack(side="right")
        tk.Button(btns, text="取消", command=win.destroy, **btn).pack(side="right", padx=6)

    # ---------- 样式 ----------
    def _apply_style(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure(".", background=C["base"], foreground=C["text"],
                        bordercolor=C["surface0"], troughcolor=C["mantle"])
        style.configure("TFrame", background=C["base"])
        style.configure("TLabel", background=C["base"], foreground=C["text"])
        style.configure("TNotebook", background=C["crust"], borderwidth=0)
        style.configure("TNotebook.Tab", background=C["mantle"], foreground=C["subtext0"],
                        padding=(12, 6))
        style.map("TNotebook.Tab", background=[("selected", C["surface0"])],
                  foreground=[("selected", C["text"])])
        style.configure("Treeview", background=C["mantle"], foreground=C["text"],
                        fieldbackground=C["mantle"], borderwidth=0, rowheight=24)
        style.map("Treeview", background=[("selected", C["surface0"])],
                  foreground=[("selected", C["yellow"])])
        style.configure("Vertical.TScrollbar", background=C["surface0"],
                        troughcolor=C["mantle"], borderwidth=0, arrowsize=0)
        style.configure("Horizontal.TScrollbar", background=C["surface0"],
                        troughcolor=C["mantle"], borderwidth=0, arrowsize=0)
        style.configure("TPanedwindow", background=C["crust"])

    def _btn(self, parent, text, cmd, accent=False):
        fg = C["crust"] if accent else C["text"]
        bg = C["blue"] if accent else C["surface0"]
        ab = C["sky"] if accent else C["surface1"]
        return tk.Button(parent, text=text, command=cmd, bg=bg, fg=fg,
                         activebackground=ab, activeforeground=fg,
                         relief="flat", borderwidth=0, padx=8, pady=2,
                         font=("Segoe UI", 10))

    # ---------- 菜单 ----------
    def _build_menu(self):
        m = tk.Menu(self, bg=C["surface0"], fg=C["text"],
                    activebackground=C["surface1"], activeforeground=C["text"],
                    tearoff=0)
        fm = tk.Menu(m, tearoff=0, bg=C["surface0"], fg=C["text"],
                     activebackground=C["surface1"])
        fm.add_command(label="新建文件", command=self.new_file, accelerator="Ctrl+N")
        fm.add_command(label="打开文件…", command=self.open_dialog, accelerator="Ctrl+O")
        fm.add_command(label="打开文件夹…", command=self.open_folder_dialog)
        fm.add_separator()
        fm.add_command(label="保存", command=self.save_active, accelerator="Ctrl+S")
        fm.add_command(label="另存为…", command=self.save_as_active)
        fm.add_command(label="关闭标签", command=self.close_active)
        fm.add_separator()
        fm.add_command(label="退出", command=self._on_close)
        m.add_cascade(label="文件", menu=fm)

        em = tk.Menu(m, tearoff=0, bg=C["surface0"], fg=C["text"],
                     activebackground=C["surface1"])
        em.add_command(label="撤销", command=lambda: self._text_cmd("edit_undo"))
        em.add_command(label="重做", command=lambda: self._text_cmd("edit_redo"))
        em.add_separator()
        em.add_command(label="剪切", command=lambda: self._text_cmd("cut"))
        em.add_command(label="复制", command=lambda: self._text_cmd("copy"))
        em.add_command(label="粘贴", command=lambda: self._text_cmd("paste"))
        m.add_cascade(label="编辑", menu=em)

        rm = tk.Menu(m, tearoff=0, bg=C["surface0"], fg=C["text"],
                     activebackground=C["surface1"])
        rm.add_command(label="运行(编译 + C VM)", command=self.run_program)
        rm.add_command(label="在系统终端运行(新窗口,支持彩色/交互)", command=self.run_in_terminal)
        rm.add_command(label="调试", command=self.debug_start)
        rm.add_command(label="停止", command=self.debug_stop)
        rm.add_separator()
        rm.add_command(label="重新分析项目", command=self._reanalyze)
        m.add_cascade(label="运行", menu=rm)

        cm = tk.Menu(m, tearoff=0, bg=C["surface0"], fg=C["text"],
                     activebackground=C["surface1"])
        cm.add_command(label="仅编译为汇编 (.dex → .dxasm)", command=self.compile_to_asm)
        cm.add_command(label="汇编 → 字节码 (.dxasm → .dexbc)", command=self.assemble_to_bc)
        m.add_cascade(label="编译", menu=cm)

        vm = tk.Menu(m, tearoff=0, bg=C["surface0"], fg=C["text"],
                     activebackground=C["surface1"])
        vm.add_checkbutton(label="侧边栏", command=self._toggle_sidebar)
        vm.add_checkbutton(label="底部面板", command=self._toggle_bottom)
        self._auto_ac_var = tk.BooleanVar(value=True)
        vm.add_checkbutton(label="输入时自动补全(弹窗不抢焦点,兼容中文输入法)",
                           variable=self._auto_ac_var,
                           command=self._toggle_auto_complete)
        vm.add_separator()
        self._fold_var = tk.BooleanVar(value=True)
        vm.add_checkbutton(label="代码折叠(点击行号右侧 ▸ 折叠/展开)",
                           variable=self._fold_var,
                           command=self._toggle_fold)
        vm.add_command(label="全部展开", command=self._unfold_all)
        m.add_cascade(label="视图", menu=vm)

        sm = tk.Menu(m, tearoff=0, bg=C["surface0"], fg=C["text"],
                     activebackground=C["surface1"])
        sm.add_command(label="VM 位置 / 库目录…", command=self._open_settings)
        m.add_cascade(label="设置", menu=sm)

        hm = tk.Menu(m, tearoff=0, bg=C["surface0"], fg=C["text"],
                     activebackground=C["surface1"])
        hm.add_command(label="关于 DEXIDE", command=self._about)
        m.add_cascade(label="帮助", menu=hm)

        self.config(menu=m)
        self.bind("<Control-n>", lambda e: self.new_file())
        self.bind("<Control-o>", lambda e: self.open_dialog())
        self.bind("<Control-s>", lambda e: self.save_active())

    # ---------- 工具栏 ----------
    def _build_toolbar(self):
        tb = tk.Frame(self, bg=C["mantle"], height=42)
        tb.pack(side="top", fill="x")
        tb.pack_propagate(False)
        for t, c in (("打开", self.open_dialog), ("保存", self.save_active),
                     ("重新分析", self._reanalyze)):
            self._btn(tb, t, c).pack(side="left", padx=(6, 2), pady=6)
        tk.Frame(tb, bg=C["surface2"], width=1).pack(side="left", fill="y", padx=6, pady=4)
        self._btn(tb, "汇编", self.compile_to_asm).pack(side="left", padx=2, pady=6)
        self._btn(tb, "→字节码", self.assemble_to_bc).pack(side="left", padx=2, pady=6)
        tk.Frame(tb, bg=C["surface2"], width=1).pack(side="left", fill="y", padx=6, pady=4)
        self._btn(tb, "▶ 运行", self.run_program, accent=True).pack(side="left", padx=2, pady=6)
        self._btn(tb, "终端", self.run_in_terminal).pack(side="left", padx=2, pady=6)
        self._btn(tb, "● 调试", self.debug_start, accent=True).pack(side="left", padx=2, pady=6)
        self._stop_btn = self._btn(tb, "■ 停止", self.debug_stop)
        self._stop_btn.pack(side="left", padx=2, pady=6)
        tk.Frame(tb, bg=C["surface2"], width=1).pack(side="left", fill="y", padx=6, pady=4)
        self._dbg_btns = {}
        for label, key in (("继续", "run"), ("单步进入", "step_into"),
                           ("单步跳过", "step_over"), ("跳出", "step_out")):
            b = self._btn(tb, label, lambda k=key: self.debug_command(k))
            b.pack(side="left", padx=2, pady=6)
            self._dbg_btns[key] = b
        self._run_btn = self._btn(tb, "▶", self.run_program)

    # ---------- 布局 ----------
    def _build_layout(self):
        outer = ttk.Panedwindow(self, orient="horizontal")
        outer.pack(fill="both", expand=True, padx=4, pady=(2, 0))
        self._outer = outer

        # 左侧栏
        self._sidebar = ttk.Frame(outer, width=280)
        self._sidebar.pack_propagate(False)
        outer.add(self._sidebar, weight=0)
        self.side = ttk.Notebook(self._sidebar)
        self.side.pack(fill="both", expand=True)
        self._explorer = self._build_explorer()
        self._outline = self._build_outline()
        self._libs = self._build_libs()
        self.side.add(self._explorer, text="  资源管理器  ")
        self.side.add(self._outline, text="  大纲  ")
        self.side.add(self._libs, text="  库  ")

        # 右侧(编辑器 + 底部)
        right = ttk.Panedwindow(outer, orient="vertical")
        outer.add(right, weight=1)

        center = ttk.Frame(right)
        right.add(center, weight=3)
        self.tabbar = tk.Frame(center, bg=C["mantle"], height=34)
        self.tabbar.pack(side="top", fill="x")
        self.tabbar.pack_propagate(False)
        self.editor_host = tk.Frame(center, bg=C["crust"])
        self.editor_host.pack(fill="both", expand=True)
        self._empty_label = tk.Label(self.editor_host, text="打开或新建一个 .dex 文件开始",
                                     bg=C["crust"], fg=C["surface2"],
                                     font=("Segoe UI", 14))
        self._empty_label.pack(expand=True)

        # 底部面板
        self._bottom = ttk.Frame(right)
        right.add(self._bottom, weight=1)
        self.bottom_nb = ttk.Notebook(self._bottom)
        self.bottom_nb.pack(fill="both", expand=True)
        self._problems = self._build_problems()
        self._console = self._build_console()
        self._debug_panel = self._build_debug_panel()
        self.bottom_nb.add(self._problems, text="  问题  ")
        self.bottom_nb.add(self._console, text="  控制台  ")
        self.bottom_nb.add(self._debug_panel, text="  调试  ")

    def _build_explorer(self):
        f = ttk.Frame(self.side)
        bar = tk.Frame(f, bg=C["mantle"])
        bar.pack(fill="x")
        self._btn(bar, "↻ 刷新", self._reanalyze).pack(side="left", padx=4, pady=2)
        self._btn(bar, "打开…", self.open_dialog).pack(side="left", padx=4, pady=2)
        tr = ttk.Treeview(f, columns=("kind",), show="tree", selectmode="browse")
        tr.pack(fill="both", expand=True)
        sb = ttk.Scrollbar(f, orient="vertical", command=tr.yview, style="Vertical.TScrollbar")
        sb.pack(side="right", fill="y")
        tr.configure(yscrollcommand=sb.set)
        tr.bind("<Double-1>", self._explorer_open)
        self._explorer_tree = tr
        return f

    def _build_outline(self):
        f = ttk.Frame(self.side)
        tr = ttk.Treeview(f, show="tree", selectmode="browse")
        tr.pack(fill="both", expand=True)
        sb = ttk.Scrollbar(f, orient="vertical", command=tr.yview, style="Vertical.TScrollbar")
        sb.pack(side="right", fill="y")
        tr.configure(yscrollcommand=sb.set)
        tr.bind("<Double-1>", self._outline_jump)
        self._outline_tree = tr
        return f

    def _build_libs(self):
        f = ttk.Frame(self.side)
        tr = ttk.Treeview(f, columns=("sig",), show="tree", selectmode="browse")
        tr.pack(fill="both", expand=True)
        sb = ttk.Scrollbar(f, orient="vertical", command=tr.yview, style="Vertical.TScrollbar")
        sb.pack(side="right", fill="y")
        tr.configure(yscrollcommand=sb.set)
        tr.bind("<Double-1>", self._libs_jump)
        self._libs_tree = tr
        return f

    def _build_problems(self):
        f = ttk.Frame(self.bottom_nb)
        cols = ("sev", "file", "line", "msg")
        tr = ttk.Treeview(f, columns=cols, show="headings", selectmode="browse")
        for c, w, t in (("sev", 60, "级别"), ("file", 200, "文件"),
                        ("line", 60, "行"), ("msg", 700, "消息")):
            tr.heading(c, text=t)
            tr.column(c, width=w, anchor="w")
        tr.pack(fill="both", expand=True)
        tr.tag_configure("error", foreground=C["red"])
        tr.tag_configure("warning", foreground=C["yellow"])
        tr.bind("<Double-1>", self._problem_jump)
        self._problems_tree = tr
        return f

    def _build_console(self):
        f = ttk.Frame(self.bottom_nb)
        txt = tk.Text(f, bg=C["crust"], fg=C["text"], state="disabled", relief="flat",
                      font=("Cascadia Code", 11), padx=6, pady=6, wrap="none")
        txt.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(f, orient="vertical", command=txt.yview, style="Vertical.TScrollbar")
        sb.pack(side="right", fill="y")
        txt.configure(yscrollcommand=sb.set)
        txt.tag_configure("out", foreground=C["text"])
        txt.tag_configure("err", foreground=C["red"])
        txt.tag_configure("info", foreground=C["subtext0"])
        self._console_txt = txt
        return f

    def _build_debug_panel(self):
        f = ttk.Frame(self.bottom_nb)
        # 监视输入
        top = tk.Frame(f, bg=C["mantle"])
        top.pack(fill="x")
        tk.Label(top, text="监视表达式:", bg=C["mantle"], fg=C["subtext0"]).pack(side="left", padx=6)
        self._watch_entry = tk.Entry(top, bg=C["surface0"], fg=C["text"],
                                     insertbackground=C["text"], relief="flat")
        self._watch_entry.pack(side="left", fill="x", expand=True, padx=6, pady=4)
        self._btn(top, "添加", self._add_watch).pack(side="left", padx=(0, 6))

        mid = ttk.Panedwindow(f, orient="horizontal")
        mid.pack(fill="both", expand=True, pady=2)

        left = ttk.Frame(mid)
        right = ttk.Frame(mid)
        mid.add(left, weight=1)
        mid.add(right, weight=1)

        # 调用栈 + 变量
        tk.Label(left, text="调用栈", bg=C["base"], fg=C["subtext0"]).pack(anchor="w", padx=4)
        self._stack_tree = ttk.Treeview(left, columns=("line",), show="tree", height=6)
        self._stack_tree.pack(fill="x", padx=2)
        self._stack_tree.column("#0", width=180)
        self._stack_tree.column("line", width=50, anchor="w")
        tk.Label(left, text="变量(当前帧)", bg=C["base"], fg=C["subtext0"]).pack(anchor="w", padx=4, pady=(4, 0))
        self._var_tree = ttk.Treeview(left, columns=("val",), show="tree")
        self._var_tree.pack(fill="both", expand=True, padx=2)
        self._var_tree.column("#0", width=120)
        self._var_tree.column("val", width=160, anchor="w")

        # 值栈 + 监视
        tk.Label(right, text="值栈(自顶向下)", bg=C["base"], fg=C["subtext0"]).pack(anchor="w", padx=4)
        self._stack_lb = tk.Listbox(right, bg=C["mantle"], fg=C["text"], relief="flat",
                                    height=6, font=("Cascadia Code", 10))
        self._stack_lb.pack(fill="x", padx=2)
        tk.Label(right, text="监视", bg=C["base"], fg=C["subtext0"]).pack(anchor="w", padx=4, pady=(4, 0))
        self._watch_tree = ttk.Treeview(right, columns=("val",), show="tree")
        self._watch_tree.pack(fill="both", expand=True, padx=2)
        self._watch_tree.column("#0", width=160)
        self._watch_tree.column("val", width=160, anchor="w")
        return f

    # ---------- 状态栏 ----------
    def _build_statusbar(self):
        self.statusbar = tk.Frame(self, bg=C["mantle"], height=26)
        self.statusbar.pack(side="bottom", fill="x")
        self.statusbar.pack_propagate(False)
        self._status_msg = tk.Label(self.statusbar, text="就绪", bg=C["mantle"],
                                    fg=C["subtext0"], anchor="w")
        self._status_msg.pack(side="left", padx=8)
        self._status_pos = tk.Label(self.statusbar, text="Ln 1, Col 1 | UTF-8 | DexLang",
                                    bg=C["mantle"], fg=C["subtext0"])
        self._status_pos.pack(side="right", padx=8)

    def set_status(self, msg):
        self._status_msg.config(text=msg)

    # ---------- 文件操作 ----------
    def new_file(self):
        n = 1
        base = "untitled"
        while f"{base}_{n}.dex" in self.tabs:
            n += 1
        path = os.path.abspath(os.path.join(self.root_dir, f"{base}_{n}.dex"))
        self._create_editor(path, "")

    def open_dialog(self):
        p = filedialog.askopenfilename(
            title="打开 DexLang 文件",
            initialdir=self.root_dir,
            filetypes=[("DexLang", "*.dex"), ("定义文件", "*.dexdef"),
                       ("汇编", "*.dxasm"), ("所有文件", "*.*")])
        if p:
            self.open_file(p)

    def open_folder_dialog(self):
        p = filedialog.askdirectory(title="选择项目文件夹", initialdir=self.root_dir)
        if p:
            self.root_dir = os.path.abspath(p)
            self.analyzer = ProjectAnalyzer(self.root_dir,
                                            include_dirs=self._include_dirs())
            self._reanalyze()

    def open_file(self, path):
        path = os_abspath(path)
        if path in self.tabs:
            self._activate(path)
            return
        try:
            with open(path, "r", encoding="utf-8-sig") as f:
                text = f.read()
        except OSError as e:
            messagebox.showerror("打开失败", str(e))
            return
        self._create_editor(path, text, autosave=False)

    def _create_editor(self, path, text, autosave=True):
        ed = CodeEditor(self.editor_host, path, self.analyzer,
                        on_modify=lambda: self._on_editor_modified(path),
                        on_breakpoints=lambda bp: self.set_status(
                            f"断点: {sorted(bp) if bp else '无'}"),
                        on_cursor=self._on_cursor)
        ed.set_text(text)
        ed.set_auto_complete(self._auto_ac_var.get())
        if hasattr(self, "_fold_var"):
            ed.fold_enabled = self._fold_var.get()
            ed._sync_gutters()
        ed.pack(fill="both", expand=True)
        self.tabs[path] = ed
        self._tab_order.append(path)
        self._activate(path)
        self.analyzer.analyze_path(path)
        self._refresh_sidebar()
        return ed

    def _activate(self, path):
        if self.active_path == path:
            return
        if self.active_path and self.active_path in self.tabs:
            self.tabs[self.active_path].pack_forget()
        self.active_path = path
        if self._empty_label is not None:
            self._empty_label.pack_forget()
            self._empty_label = None
        ed = self.tabs[path]
        ed.pack(fill="both", expand=True)
        ed.focus_set()
        self._refresh_tabbar()
        self._refresh_outline()
        self._update_status_pos()

    def save_active(self):
        if not self.active_path:
            return
        ed = self.tabs[self.active_path]
        path = self.active_path
        if os.path.basename(path).startswith("untitled"):
            p = filedialog.asksaveasfilename(
                initialdir=self.root_dir, defaultextension=".dex",
                filetypes=[("DexLang", "*.dex")])
            if not p:
                return
            path = os_abspath(p)
            # 重命名标签
            self.tabs[path] = self.tabs.pop(self.active_path)
            self._tab_order[self._tab_order.index(self.active_path)] = path
            self.active_path = path
            ed.path = path
        with open(path, "w", encoding="utf-8") as f:
            f.write(ed.get_text())
        self.set_status(f"已保存 {os.path.basename(path)}")

    def save_as_active(self):
        if not self.active_path:
            return
        p = filedialog.asksaveasfilename(initialdir=self.root_dir, defaultextension=".dex")
        if not p:
            return
        ed = self.tabs[self.active_path]
        with open(p, "w", encoding="utf-8") as f:
            f.write(ed.get_text())
        self.open_file(p)

    def close_active(self):
        if not self.active_path:
            return
        path = self.active_path
        ed = self.tabs.pop(path)
        self._tab_order.remove(path)
        ed.destroy()
        if self._tab_order:
            self._activate(self._tab_order[-1])
        else:
            self.active_path = None
            self._refresh_tabbar()
            self._empty_label = tk.Label(self.editor_host, text="打开或新建一个 .dex 文件开始",
                                         bg=C["crust"], fg=C["surface2"],
                                         font=("Segoe UI", 14))
            self._empty_label.pack(expand=True)
        self._refresh_outline()
        self._refresh_problems()

    def _text_cmd(self, cmd):
        ed = self.tabs.get(self.active_path)
        if ed:
            getattr(ed.text, cmd)()

    # ---------- 标签栏 ----------
    def _refresh_tabbar(self):
        for w in self.tabbar.winfo_children():
            w.destroy()
        for path in self._tab_order:
            if path not in self.tabs:
                continue
            name = os.path.basename(path)
            active = path == self.active_path
            bg = C["surface0"] if active else C["mantle"]
            fg = C["text"] if active else C["subtext0"]
            b = tk.Button(self.tabbar, text=" " + name + " ", command=lambda p=path: self._activate(p),
                          bg=bg, fg=fg, relief="flat", borderwidth=0, padx=2, pady=4,
                          activebackground=C["surface1"], activeforeground=fg,
                          font=("Segoe UI", 10))
            b.pack(side="left", padx=(0, 0), pady=2)
            x = tk.Button(self.tabbar, text="✕", command=lambda p=path: self._close_tab(p),
                          bg=bg, fg=C["surface2"], relief="flat", borderwidth=0,
                          activebackground=C["surface1"], activeforeground=C["red"],
                          font=("Segoe UI", 9))
            x.pack(side="left", padx=(0, 4), pady=2)

    def _close_tab(self, path):
        if path == self.active_path:
            self.close_active()
        else:
            self.tabs.pop(path).destroy()
            self._tab_order.remove(path)
            self._refresh_tabbar()

    # ---------- 回调 ----------
    def _on_editor_modified(self, path):
        if self.debug and self._debug_path == path:
            self.set_status("调试中修改源码:请重新调试以生效")
        self.after(300, lambda: (self.analyzer.analyze_path(path),
                                 self._refresh_problems(),
                                 self._refresh_outline()))

    def _on_cursor(self):
        self._update_status_pos()

    def _update_status_pos(self):
        ed = self.tabs.get(self.active_path)
        if not ed:
            return
        try:
            idx = ed.text.index("insert")
            line, col = str(idx).split(".")
            self._status_pos.config(text=f"Ln {line}, Col {int(col) + 1} | UTF-8 | DexLang")
        except tk.TclError:
            pass

    # ---------- 侧边栏刷新 ----------
    def _refresh_sidebar(self):
        self._refresh_explorer()
        self._refresh_outline()
        self._refresh_libs()
        self._refresh_problems()

    def _refresh_explorer(self):
        tr = self._explorer_tree
        tr.delete(*tr.get_children())
        root_i = tr.insert("", "end", text=self.root_dir, open=True)
        for path in self.analyzer.scan():
            rel = os.path.relpath(path, self.root_dir)
            parts = rel.split(os.sep)
            parent = root_i
            for i, part in enumerate(parts[:-1]):
                pid = f"{parent}/{i}/{part}"
                if not tr.exists(pid):
                    parent = tr.insert(parent, "end", iid=pid, text=part)
                else:
                    parent = pid
            tr.insert(parent, "end", text=parts[-1], values=(os.path.splitext(parts[-1])[1],),
                      tags=(path,))

    def _explorer_open(self, ev):
        tr = self._explorer_tree
        iid = tr.focus()
        path = tr.item(iid, "tags")
        if path:
            self.open_file(path[0])

    def _refresh_outline(self):
        tr = self._outline_tree
        tr.delete(*tr.get_children())
        if not self.active_path:
            return
        fa = self.analyzer.files.get(self.active_path)
        if not fa:
            return
        tr.insert("", "end", text=os.path.basename(self.active_path), open=True, tags=("file", self.active_path))
        inc_i = tr.insert("", "end", text=f"库引入 ({len(fa.includes)})", open=True)
        for line, kind, target, def_path in fa.includes:
            ni = tr.insert(inc_i, "end", text=f"{kind} {target}  (行 {line})", tags=("inc",))
            for nname in fa.natives:
                if fa.natives[nname].def_file == def_path:
                    ns = fa.natives[nname]
                    tr.insert(ni, "end", text=f"{nname}  {ns.sig}", tags=("lib", self.active_path))
        fi = tr.insert("", "end", text=f"函数 ({len(fa.funcs)})", open=True)
        for name in sorted(fa.funcs):
            fs = fa.funcs[name]
            fi2 = tr.insert(fi, "end", text=f"{name}({', '.join(fs.params)})  arity={fs.arity}",
                            tags=("func", self.active_path, name, str(fs.line)))
            if fs.params:
                tr.insert(fi2, "end", text=f"参数: {', '.join(fs.params)}")
            if fs.callees:
                tr.insert(fi2, "end", text=f"调用: {', '.join(sorted(fs.callees)) or '无'}")
            if fs.callers:
                tr.insert(fi2, "end", text=f"被调用: {', '.join(sorted(fs.callers)) or '无'}")
            for callee, line, col in fs.calls:
                tr.insert(fi2, "end", text=f" 调用 {callee} @{line}", tags=("call", self.active_path))
        if fa.top_vars:
            vi = tr.insert("", "end", text=f"顶层变量 ({len(fa.top_vars)})")
            for v, line in sorted(fa.top_vars.items(), key=lambda x: x[1]):
                tr.insert(vi, "end", text=f"{v}  (行 {line})")

    def _outline_jump(self, ev):
        tr = self._outline_tree
        tags = tr.item(tr.focus(), "tags")
        if len(tags) >= 4 and tags[0] == "func":
            self._jump_to_line(self.active_path, int(tags[3]))
        elif len(tags) >= 2 and tags[0] == "call":
            self._jump_to_line(self.active_path, int(tags[2]))

    def _refresh_libs(self):
        tr = self._libs_tree
        tr.delete(*tr.get_children())
        natives = self.analyzer.native_names()
        if not natives:
            tr.insert("", "end", text="(未引入库)", tags=("none",))
            return
        by_lib = {}
        for nname in sorted(natives):
            ns = self.analyzer._all_natives.get(nname)
            if not ns:
                continue
            by_lib.setdefault(ns.lib, []).append((nname, ns))
        for lib, items in by_lib.items():
            li = tr.insert("", "end", text=os.path.basename(lib) or lib, open=True,
                           values=("",))
            for nname, ns in items:
                tr.insert(li, "end", text=f"{nname}  {ns.sig} -> {ns.ret_name}",
                          values=("",), tags=("nat",))

    def _libs_jump(self, ev):
        tr = self._libs_tree
        tags = tr.item(tr.focus(), "tags")
        if tags and tags[0] == "nat":
            name = tr.item(tr.focus(), "text").split("  ")[0]
            self._find_native_def(name)

    def _find_native_def(self, name):
        for fa in self.analyzer.files.values():
            if name in fa.natives:
                def_path = fa.natives[name].def_file
                if os.path.exists(def_path):
                    self.open_file(def_path)
                return

    # ---------- 问题 ----------
    def _refresh_problems(self):
        tr = self._problems_tree
        tr.delete(*tr.get_children())
        for path in sorted(self.analyzer.files):
            fa = self.analyzer.files[path]
            for p in fa.problems:
                iid = tr.insert("", "end",
                                values=(p.severity.upper(), os.path.basename(path),
                                        p.line if p.line else "", p.msg),
                                tags=(p.severity,))
                tr.item(iid, tags=(p.severity, path, str(p.line), str(p.col)))

    def _problem_jump(self, ev):
        tr = self._problems_tree
        iid = tr.focus()
        tags = tr.item(iid, "tags")
        if len(tags) >= 2 and tags[1]:
            self._jump_to_line(tags[1], int(tags[2] or 0))

    def _jump_to_line(self, path, line):
        if line <= 0:
            return
        self.open_file(path)
        ed = self.tabs.get(path)
        if ed:
            ed.text.mark_set("insert", f"{line}.0")
            ed.text.see(f"{line}.0")
            ed.focus_set()

    # ---------- 控制台 ----------
    def _console_write(self, text, tag="out"):
        txt = self._console_txt
        txt.configure(state="normal")
        txt.insert("end", text, tag)
        txt.see("end")
        txt.configure(state="disabled")

    # ---------- 编译工具 ----------
    def _active_source(self):
        """返回当前活动文件的源码文本;无活动文件或非 .dex 时返回 None。"""
        if not self.active_path or self.active_path not in self.tabs:
            self.set_status("请先打开文件")
            return None
        return self.tabs[self.active_path].get_text()

    def compile_to_asm(self):
        """仅编译为汇编:当前 .dex → 生成同目录 .dxasm 并打开查看。"""
        if not self.active_path or self.active_path not in self.tabs:
            self.set_status("请先打开一个 .dex 文件")
            return
        path = self.active_path
        if not path.endswith(".dex"):
            self.set_status("仅 .dex 源文件可编译为汇编")
            return
        src = self._active_source()
        self._console_write(f"$ 编译为汇编 {os.path.basename(path)}\n", "info")
        try:
            toks = Lexer(src, path).tokenize()
            ast = Parser(toks, path).parse_program()
            unit = compile_program(ast, source_path=path,
                                   include_dirs=self._include_dirs())
            prog = unit.to_program()
        except DexError as e:
            self._console_write(f"编译错误: {e}\n", "err")
            self.set_status("编译错误")
            return
        asm_text = render(prog)
        asm_path = os.path.splitext(path)[0] + ".dxasm"
        try:
            with open(asm_path, "w", encoding="utf-8") as f:
                f.write(asm_text)
        except OSError as e:
            self._console_write(f"写入失败: {e}\n", "err")
            return
        info = f"已生成汇编: {asm_path}"
        if prog.natives:
            info += f" (原生 {len(prog.natives)} 个)"
        self._console_write(info + "\n", "out")
        self.set_status(info)
        # 打开生成的汇编以便查看(自带汇编高亮)
        self.open_file(asm_path)

    def assemble_to_bc(self):
        """汇编 → 字节码:当前 .dxasm → 生成同目录 .dexbc。"""
        if not self.active_path or self.active_path not in self.tabs:
            self.set_status("请先打开一个 .dxasm 汇编文件")
            return
        path = self.active_path
        if not path.endswith(".dxasm"):
            self.set_status("仅 .dxasm 汇编文件可转换为字节码")
            return
        text = self._active_source()
        self._console_write(f"$ 汇编→字节码 {os.path.basename(path)}\n", "info")
        try:
            prog = parse_asm_text(text)
            data = assemble(prog)
        except DexError as e:
            self._console_write(f"汇编错误: {e}\n", "err")
            self.set_status("汇编错误")
            return
        bc_path = os.path.splitext(path)[0] + ".dexbc"
        try:
            with open(bc_path, "wb") as f:
                f.write(data)
        except OSError as e:
            self._console_write(f"写入失败: {e}\n", "err")
            return
        msg = f"已生成字节码: {bc_path} ({len(data)} bytes, 函数 {len(prog.funcs)} 个)"
        self._console_write(msg + "\n", "out")
        self.set_status(msg)

    # ---------- 运行(非调试) ----------
    def run_program(self):
        if not self.active_path or self.active_path not in self.tabs:
            return
        path = self.active_path
        src = self.tabs[path].get_text()
        # 程序需要键盘输入(dex_input)时,面板运行因无 stdin 会卡死 → 自动改用系统终端
        if "dex_input" in src:
            self._console_write("检测到 dex_input(需要键盘输入),已改用系统终端运行…\n", "info")
            self.run_in_terminal()
            return
        self._console_write(f"$ 运行 {os.path.basename(path)}\n", "info")
        try:
            toks = Lexer(src, path).tokenize()
            ast = Parser(toks, path).parse_program()
            unit = compile_program(ast, source_path=path,
                                   include_dirs=self._include_dirs())
            prog = unit.to_program()
        except DexError as e:
            self._console_write(f"编译错误: {e}\n", "err")
            self.set_status("编译错误")
            return
        # 优先使用 C VM
        vm_exe = self._vm_exe()
        import tempfile
        with tempfile.NamedTemporaryFile("wb", suffix=".dexbc", delete=False) as f:
            from dexlang import assemble
            f.write(assemble(prog))
            bc = f.name
        try:
            if os.path.exists(vm_exe):
                # 按 UTF-8 捕获 C VM 输出(否则中文/Unicode 会按系统 GBK 解码成乱码)
                r = subprocess.run([vm_exe, bc], capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=30)
                out, err = r.stdout, r.stderr
            else:
                from dexlang.pyvm import run_program as rp
                out_l, err_l = rp(prog)
                out, err = "\n".join(out_l) + "\n", (err_l or "")
        except Exception as e:
            out, err = "", f"运行失败: {e}"
        finally:
            try:
                os.remove(bc)
            except OSError:
                pass
        if out:
            self._console_write(out)
        if err:
            self._console_write(err, "err")
        self.set_status("运行结束" if not err else "运行出错")

    def run_in_terminal(self):
        """用系统默认终端(新控制台窗口)运行当前程序。

        与内部面板运行不同:输出直接进系统终端,支持 ANSI 真彩色
        (如图片渲染)、stdin 交互输入,并可在程序结束后保持窗口查看。
        字节码写到源码同目录的 .dexbc(与 CLI compile 行为一致)。
        """
        if not self.active_path or self.active_path not in self.tabs:
            return
        path = self.active_path
        src = self.tabs[path].get_text()
        self._console_write(f"$ 在系统终端运行 {os.path.basename(path)}\n", "info")
        try:
            toks = Lexer(src, path).tokenize()
            ast = Parser(toks, path).parse_program()
            unit = compile_program(ast, source_path=path,
                                   include_dirs=self._include_dirs())
            prog = unit.to_program()
        except DexError as e:
            self._console_write(f"编译错误: {e}\n", "err")
            self.set_status("编译错误")
            return
        bc = os.path.splitext(path)[0] + ".dexbc"
        from dexlang import assemble
        try:
            with open(bc, "wb") as f:
                f.write(assemble(prog))
        except OSError as e:
            self._console_write(f"写入字节码失败: {e}\n", "err")
            return
        vm_exe = self._vm_exe()
        if not os.path.exists(vm_exe):
            self._console_write(f"未找到 C VM: {vm_exe}\n", "err")
            self.set_status("未找到 C VM")
            return
        try:
            if os.name == "nt":
                # CREATE_NEW_CONSOLE:在系统默认终端(Windows Terminal)打开新窗口;
                # cmd /k 让程序结束后窗口保持。用"字符串命令行 + 双引号包裹整条命令"
                # 避免 subprocess 对含空格/引号参数二次转义,导致 cmd 报
                # "不是内部或外部命令"(cmd /c 与 /k 引号规则相同,已实测)。
                inner = f'"{vm_exe}" "{bc}"'
                subprocess.Popen(f'cmd /k "{inner}"', cwd=self.root_dir,
                                 creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))
            else:
                subprocess.Popen(["sh", "-c",
                                  f'"{vm_exe}" "{bc}"; echo "--- 结束,按回车关闭 ---"; read'],
                                 cwd=self.root_dir)
        except OSError as e:
            self._console_write(f"启动终端失败: {e}\n", "err")
            self.set_status("启动终端失败")
            return
        self.set_status(f"已在系统终端运行: {os.path.basename(bc)}")

    # ---------- 调试 ----------
    def debug_start(self):
        if self.debug:
            return
        if not self.active_path or self.active_path not in self.tabs:
            return
        path = self.active_path
        ed = self.tabs[path]
        src = ed.get_text()
        self._console_write(f"$ 调试 {os.path.basename(path)} (断点 {sorted(ed.breakpoints) or '无'})\n", "info")
        try:
            toks = Lexer(src, path).tokenize()
            ast = Parser(toks, path).parse_program()
            unit = compile_program(ast, source_path=path,
                                   include_dirs=self._include_dirs())
            prog = unit.to_program()
        except DexError as e:
            self._console_write(f"编译错误: {e}\n", "err")
            return
        vm = PyVM(prog)
        vm.breakpoints = set(ed.breakpoints)
        self._debug_path = path
        self.debug = DebugController(vm, self._dbg_queue)
        self.debug.start("run")
        self._update_debug_buttons(enabled=True)
        self.bottom_nb.select(self._debug_panel)
        self.set_status("调试中…")

    def _poll_debug(self):
        try:
            while True:
                ev = self._dbg_queue.get_nowait()
                self._handle_debug_event(ev)
        except queue.Empty:
            pass
        except Exception:
            pass
        if getattr(self, "_closing", False):
            return
        try:
            self._poll_id = self.after(50, self._poll_debug)
        except tk.TclError:
            pass

    def debug_command(self, cmd):
        if self.debug:
            self.debug.send(cmd)

    def debug_stop(self):
        if self.debug:
            self.debug.stop()
            self.debug.thread.join(timeout=1)
            self.debug = None
            ed = self.tabs.get(self._debug_path)
            if ed:
                ed.clear_debug_line()
            self._debug_path = None
            self._clear_debug_views()
            self._update_debug_buttons(enabled=False)
            self.set_status("调试已停止")

    def _debug_event(self, ev):
        self.after(0, lambda: self._handle_debug_event(ev))
    def _handle_debug_event(self, ev):
        t = ev["type"]
        if t == "output":
            self._console_write(ev["text"] + "\n")
        elif t == "paused":
            ed = self.tabs.get(self._debug_path)
            if ed:
                ed.set_debug_line(ev["reason"] and self.debug.vm.current_line())
            self._refresh_debug_views()
            self.set_status(f"暂停: {ev['reason']}")
        elif t == "ended":
            if ev["error"]:
                self._console_write(ev["error"] + "\n", "err")
                self.set_status("运行期错误")
            else:
                self.set_status("调试结束")
            ed = self.tabs.get(self._debug_path)
            if ed:
                ed.clear_debug_line()
            self.debug = None
            self._debug_path = None
            self._clear_debug_views()
            self._update_debug_buttons(enabled=False)

    def _update_debug_buttons(self, enabled):
        state = "normal" if enabled else "disabled"
        for b in self._dbg_btns.values():
            b.config(state=state)
        self._stop_btn.config(state=state)

    def _refresh_debug_views(self):
        if not self.debug:
            return
        vm = self.debug.vm
        # 调用栈
        st = self._stack_tree
        st.delete(*st.get_children())
        for i, fi in enumerate(vm.frame_info()):
            st.insert("", "end", text=fi["func"],
                      values=(fi["line"] if fi["line"] else "",))
        # 变量
        vt = self._var_tree
        vt.delete(*vt.get_children())
        if vm.frames:
            locals_ = vm.frame_info()[0]["locals"]
            for k, v in locals_.items():
                vt.insert("", "end", text=k, values=(self._fmt(v),))
        # 值栈
        self._stack_lb.delete(0, "end")
        for v in reversed(vm.stack):
            self._stack_lb.insert("end", self._fmt(v))
        # 监视
        self._refresh_watch()

    def _clear_debug_views(self):
        for w in (self._stack_tree, self._var_tree, self._watch_tree):
            w.delete(*w.get_children())
        self._stack_lb.delete(0, "end")

    def _add_watch(self):
        expr = self._watch_entry.get().strip()
        if expr and expr not in self._watch_items:
            self._watch_items[expr] = None
            self._refresh_watch()
        self._watch_entry.delete(0, "end")

    def _refresh_watch(self):
        wt = self._watch_tree
        wt.delete(*wt.get_children())
        if not self.debug:
            return
        vm = self.debug.vm
        for expr in self._watch_items:
            val = self._eval_watch(vm, expr)
            self._watch_items[expr] = val
            wt.insert("", "end", text=expr, values=(self._fmt(val),))

    def _eval_watch(self, vm, expr):
        """在当前帧上下文中求值监视表达式(支持变量名/数字/简单运算)。"""
        expr = expr.strip()
        if not vm.frames:
            return "<无帧>"
        locals_ = vm.frame_info()[0]["locals"]
        import ast as pyast
        try:
            tree = pyast.parse(expr, mode="eval")
        except SyntaxError:
            return "<表达式错误>"
        names = {}
        for k, v in locals_.items():
            names[k.lstrip("%")] = v
        try:
            code = compile(tree, "<watch>", "eval")
            return eval(code, {"__builtins__": {}}, names)
        except Exception as e:
            return f"<{type(e).__name__}>"

    @staticmethod
    def _fmt(v):
        if v is None:
            return "<未初始化>"
        if isinstance(v, bool):
            return "1" if v else "0"
        if isinstance(v, float):
            return "%g" % v
        if isinstance(v, str):
            return '"%s"' % v
        return str(v)

    # ---------- 其他 ----------
    def _reanalyze(self):
        self.set_status("正在分析项目…")
        self.analyzer.reanalyze()
        self._refresh_sidebar()
        for ed in self.tabs.values():
            ed._apply_highlight()
        self.set_status("分析完成")

    def _toggle_sidebar(self):
        self._sidebar.pack_forget()
        self._outer.add(self._sidebar, weight=0) if False else None
        # 简化:切换可见性
        if self._sidebar.winfo_ismapped():
            self._sidebar.pack_forget()
        else:
            self._sidebar.pack(side="left", fill="y")
        self._outer.add(self._sidebar, weight=0)

    def _toggle_bottom(self):
        if self._bottom.winfo_ismapped():
            self._bottom.pack_forget()
        else:
            pass

    def _toggle_auto_complete(self):
        """切换输入时自动补全(默认关闭,避免打断中文输入法 IME)。"""
        enabled = self._auto_ac_var.get()
        for ed in self.tabs.values():
            ed.set_auto_complete(enabled)
        self.set_status("输入时自动补全: " + ("开" if enabled else "关"))

    def _toggle_fold(self):
        """开关折叠标记显示。"""
        enabled = self._fold_var.get()
        for ed in self.tabs.values():
            ed.fold_enabled = enabled
            if not enabled:
                ed.unfold_all()
            ed._sync_gutters()
        self.set_status("代码折叠: " + ("开" if enabled else "关"))

    def _unfold_all(self):
        for ed in self.tabs.values():
            ed.unfold_all()
        self.set_status("已全部展开")

    def _about(self):
        messagebox.showinfo("DEXIDE", "DEXIDE — DexLang 集成开发环境\n\n"
                                      "Python + tkinter,零第三方依赖。\n"
                                      "复用 DEXCODE 工具链进行真实的分析与调试。")

    def _on_close(self):
        self._closing = True
        self.debug_stop()
        self.destroy()

    def destroy(self):
        self._closing = True
        pid = getattr(self, "_poll_id", None)
        if pid:
            try:
                self.after_cancel(pid)
            except tk.TclError:
                pass
        super().destroy()


def main(root_dir=None, start_file=None):
    app = DexIDE(root_dir=root_dir, start_file=start_file)
    app.mainloop()
