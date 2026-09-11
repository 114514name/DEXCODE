"""bluedit.menueditor — MENU 场景编辑器(独立窗口)。

控件放置区 + 代码区(UE 蓝图式画布)。左侧/底部放置节点,右侧属性面板编辑。
控件:0按钮 1文字 2图片 3面板 4输入框;节点:M_START/M_CTRL/M_CLICK/M_SIGNAL/M_EXIT。
菜单数据内嵌于项目(随 .bluescene 保存),编辑器直接修改内存对象并复用主窗口撤销栈。
"""

import os
import tkinter as tk
from tkinter import ttk, messagebox, simpledialog, filedialog

from . import theme as T
from .model import (MENU_NODE_NAMES, MENU_NODE_COLORS, MENU_NODE_ICONS,
                    MENU_CTRL_TYPE_NAMES, MENU_CTRL_FIT_NAMES, MENU_MODE_NAMES,
                    NODE_NAMES, NODE_COLORS, NODE_ICONS,
                    N_DEFVAR, N_SETVAR, N_GETVAR, N_SAVE, N_LOAD,
                    N_FILE_WRITE, N_FILE_READ, N_TOAST, N_WAIT,
                    N_TEXT, N_LOGIC, N_MATH, N_TEXTLIT, N_NUMLIT, N_BOLLIT,
                    N_RANDOM, N_INPUT,
                    M_START, M_CTRL, M_CLICK, M_SIGNAL, M_EXIT, M_IN, M_JUMP,
                    menu_defaults)
from .canvas import BlueCanvas
from . import vars as vars_mod


class MenuCanvas(BlueCanvas):
    """复用镜头画布交互;仅替换节点外观/摘要/删除保护。"""

    def _colors(self):
        d = dict(MENU_NODE_COLORS)
        d.update(NODE_COLORS)
        return d

    def _icons(self):
        d = dict(MENU_NODE_ICONS)
        d.update(NODE_ICONS)
        return d

    def _names(self):
        d = dict(MENU_NODE_NAMES)
        d.update(NODE_NAMES)
        return d

    def _palette(self):
        # 开场起点默认已存在,不加入可新建面板
        return [M_CTRL, M_CLICK, M_SIGNAL, M_EXIT, M_IN, M_JUMP,
                N_DEFVAR, N_SETVAR, N_GETVAR, N_SAVE, N_LOAD,
                N_FILE_WRITE, N_FILE_READ, N_TOAST, N_WAIT,
                N_TEXT, N_LOGIC, N_MATH, N_TEXTLIT, N_NUMLIT, N_BOLLIT,
                N_RANDOM, N_INPUT]

    def _undeletable(self):
        return (M_START,)

    def _node_summary(self, n):
        t = n.type
        if t == M_START:
            return "菜单初始化从这里开始"
        if t == M_CTRL:
            ty = MENU_CTRL_TYPE_NAMES[n.ctype] if 0 <= n.ctype < len(MENU_CTRL_TYPE_NAMES) else "?"
            pos = f"({n.px},{n.py} {n.pw}x{n.ph})"
            s = f"{ty} {n.ctrl or '?'}  {pos}"
            if n.text:
                s += f"  「{n.text[:14]}」"
            return s
        if t == M_CLICK:
            return f"被点击: {n.target_ctrl or '未选控件'}"
        if t == M_SIGNAL:
            return f"收到信号: {n.signal or '未填信号名'}"
        if t == M_EXIT:
            return f"退出结果: {n.exit_name or '退出'}"
        if t == M_IN:
            return f"数据入口: {n.param_name or '未命名'}"
        if t == M_JUMP:
            return f"跳转镜头 {n.flow_scene + 1 if n.flow_scene >= 0 else '?'}"
        if t == N_DEFVAR:
            return f"定义变量 {n.var_name or '?'}"
        if t == N_SETVAR:
            return f"设置变量 {n.var_name or '?'}"
        if t == N_GETVAR:
            return f"读取变量 {n.var_name or '?'}"
        if t == N_SAVE:
            return f"存档 → {n.file_path or '未选路径'}"
        if t == N_LOAD:
            return f"读档 ← {n.file_path or '未选路径'}"
        if t == N_FILE_WRITE:
            return f"写文件 {n.file_path or '未选路径'}"
        if t == N_FILE_READ:
            return f"读文件 {n.file_path or '未选路径'}"
        if t == N_TOAST:
            return f"消息「{n.toast_text[:12] or '?'}」"
        if t == N_WAIT:
            return f"等待 {n.ms}ms"
        if t == N_TEXT:
            return "文本拼接"
        if t == N_LOGIC:
            return f"逻辑 {n.op or '=='}"
        if t == N_MATH:
            return f"运算 {n.op or '+'}"
        if t == N_TEXTLIT:
            return f"字面文本「{n.lit_text[:12]}」"
        if t == N_NUMLIT:
            return f"字面数字 {n.lit_num}"
        if t == N_BOLLIT:
            return "true" if n.lit_bool else "false"
        if t == N_INPUT:
            return f"输入框内容 {n.ctrl or '?'}"
        if t == N_RANDOM:
            return f"随机 0..{max(n.max - 1, 0)}"
        return ""


class MenuInspector(tk.Frame):
    """菜单节点属性面板。"""

    def __init__(self, master, editor, **kw):
        super().__init__(master, bg=T.BG2, **kw)
        self.editor = editor
        self._flushers = []
        self.title_lbl = tk.Label(self, text="属性", bg=T.BG2, fg=T.FG,
                                  font=T.FONT_BOLD, anchor="w")
        self.title_lbl.pack(fill="x", padx=10, pady=8)
        # 可滚动内容区(属性项多时用滚动条,避免挤在一起)
        self._cv = tk.Canvas(self, bg=T.BG2, highlightthickness=0, bd=0)
        sb = tk.Scrollbar(self, orient="vertical", command=self._cv.yview)
        self.body = tk.Frame(self._cv, bg=T.BG2)
        self._win = self._cv.create_window((0, 0), window=self.body, anchor="nw")
        self.body.bind("<Configure>",
                       lambda e: self._cv.configure(scrollregion=self._cv.bbox("all")))
        self._cv.bind("<Configure>",
                      lambda e: self._cv.itemconfig(self._win, width=e.width))
        self._cv.configure(yscrollcommand=sb.set)
        self._cv.pack(side="left", fill="both", expand=True, padx=(10, 0), pady=(0, 10))
        sb.pack(side="right", fill="y", padx=(0, 4), pady=(0, 10))
        self._cv.bind("<Enter>", lambda e: self._wheel_bind(True))
        self._cv.bind("<Leave>", lambda e: self._wheel_bind(False))
        self.rebuild()

    def _wheel_bind(self, on):
        try:
            if on:
                self._cv.bind_all("<MouseWheel>", self._on_wheel)
            else:
                self._cv.unbind_all("<MouseWheel>")
        except Exception:
            pass

    def _on_wheel(self, e):
        try:
            self._cv.yview_scroll(int(-e.delta / 120), "units")
        except Exception:
            pass

    # ---------------- 重建 ----------------
    def rebuild(self):
        for w in self.body.winfo_children():
            w.destroy()
        self._flushers = []
        try:
            self._cv.yview_moveto(0)   # 切节点时滚动回到顶部
        except Exception:
            pass
        m = self.editor.scene()
        n = None
        if m and self.editor.canvas.selected:
            n = m.node(self.editor.canvas.selected)
        if n is not None:
            self.title_lbl.config(text=f"属性　·　{MENU_NODE_NAMES.get(n.type, '?')}")
            self._build(n)
        else:
            self.title_lbl.config(text="属性　·　（未选中节点）")
            self._menu_props(m)

    def flush(self):
        for f in self._flushers:
            try:
                f()
            except Exception:
                pass

    # ---------------- 基础控件 ----------------
    def _row(self, label, widget):
        r = tk.Frame(self.body, bg=T.BG2)
        r.pack(fill="x", pady=6)
        tk.Label(r, text=label, bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 anchor="w").pack(fill="x")
        widget.pack(fill="x", pady=(4, 0))
        return r

    def _entry(self, init, commit, width=None):
        v = tk.StringVar(value=init)
        e = tk.Entry(self.body, textvariable=v, bg=T.BG3, fg=T.FG,
                     insertbackground=T.FG, relief="flat", font=T.FONT_SM,
                     width=width)
        e.bind("<FocusOut>", lambda _e: commit(v.get()))
        e.bind("<Return>", lambda _e: (commit(v.get()), self.focus_set()))
        self._flushers.append(lambda: commit(v.get()))
        return e

    def _check(self, init, commit, label):
        v = tk.BooleanVar(value=bool(init))
        c = tk.Checkbutton(self.body, text=label, variable=v, bg=T.BG2,
                           fg=T.FG, activebackground=T.BG2,
                           activeforeground=T.FG, selectcolor=T.BG3,
                           font=T.FONT_SM,
                           command=lambda: commit(1 if v.get() else 0))
        c.pack(fill="x", padx=2, pady=2, anchor="w")
        self._flushers.append(lambda: commit(1 if v.get() else 0))
        return c

    def _combo(self, init, items, commit, editable=False):
        # init 可能是整数索引(如 ctype=1)或字符串(如控件名);整数按索引取显示名
        if isinstance(init, int) and 0 <= init < len(items):
            val = items[init]
        elif init in items:
            val = init
        else:
            val = items[0] if items else ""
        v = tk.StringVar(value=val)
        cb = ttk.Combobox(self.body, textvariable=v, values=items,
                          state="normal" if editable else "readonly",
                          font=T.FONT_SM)
        if editable:
            cb.bind("<FocusOut>", lambda _e: commit(v.get()))
            cb.bind("<Return>", lambda _e: (commit(v.get()), self.focus_set()))
            self._flushers.append(lambda: commit(v.get()))
        else:
            # readonly 下拉选中即提交,不注册 flusher(避免 flush 重入)
            cb.bind("<<ComboboxSelected>>",
                    lambda _e: commit(items.index(v.get()) if v.get() in items else 0))
        return cb

    def _btn(self, text, cmd, color=T.ACCENT_DARK):
        return tk.Button(self.body, text=text, command=cmd, bg=color, fg="#fff",
                         activebackground=color, relief="flat", font=T.FONT_SM)

    def _file_btn(self, label, init, commit, browse_title="选择图片", types=None):
        """带「浏览…」按钮的路径输入(选文件自动转相对项目根路径)。"""
        r = tk.Frame(self.body, bg=T.BG2)
        r.pack(fill="x", pady=6)
        tk.Label(r, text=label, bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 anchor="w").pack(fill="x")
        row = tk.Frame(self.body, bg=T.BG2)
        row.pack(fill="x", pady=(4, 0))
        v = tk.StringVar(value=init)
        e = tk.Entry(row, textvariable=v, bg=T.BG3, fg=T.FG,
                     insertbackground=T.FG, relief="flat", font=T.FONT_SM)
        e.pack(side="left", fill="x", expand=True)
        types = types or [("图片", "*.png *.jpg *.jpeg *.bmp *.webp"), ("全部", "*.*")]

        def browse():
            f = filedialog.askopenfilename(title=browse_title, filetypes=types,
                                           parent=self.winfo_toplevel())
            if f:
                root = self.editor.app.project_root or os.getcwd()
                try:
                    rel = os.path.relpath(f, root)
                    if rel.startswith("..") or os.path.isabs(rel):
                        rel = f
                except Exception:
                    rel = f
                v.set(rel)
                commit(rel)

        tk.Button(row, text="浏览…", command=browse, bg=T.BG3, fg=T.FG,
                  activebackground=T.ACCENT_DARK, activeforeground="#fff",
                  relief="flat", font=T.FONT_SM, cursor="hand2"
                  ).pack(side="right", padx=(4, 0))
        e.bind("<FocusOut>", lambda _e: commit(v.get()))
        e.bind("<Return>", lambda _e: (commit(v.get()), self.focus_set()))
        self._flushers.append(lambda: commit(v.get()))
        return r

    def _hint(self, text):
        tk.Label(self.body, text=text, bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 justify="left", wraplength=240).pack(anchor="w", pady=4)

    # ---------------- 菜单全局属性 ----------------
    def _menu_props(self, m):
        ed = self.editor

        def changed():
            ed.on_node_edited()

        if m is None:
            self._hint("（无菜单）")
            return
        self._row("菜单标题", self._entry(m.title, lambda v: (setattr(m, "title", v), changed())))
        self._row("显示模式",
                  self._combo(MENU_MODE_NAMES[m.mode], MENU_MODE_NAMES,
                              lambda v: (setattr(m, "mode", v), changed())))
        self._file_btn("独占菜单背景图(相对项目根,可选)", m.bg,
                       lambda v: (setattr(m, "bg", v), changed()))
        self._hint("独占菜单：阻塞镜头、独立画面(可用背景图),退出后返回结果。\n"
                   "叠加菜单：叠加在镜头画面上,不阻塞;镜头发信号驱动。")

    # ---------------- 各节点表单 ----------------
    def _build(self, n):
        ed = self.editor

        def changed():
            ed.on_node_edited()

        t = n.type
        if t == M_START:
            self._hint("开场起点：菜单初始化从这里开始。\n"
                       "独占菜单启动时、或叠加菜单每次收到信号时都会从这里跑一遍\n"
                       "(控件同名则不会重复创建,直接更新,因此可重复调用)。")
        elif t == M_CTRL:
            self._row("控件名", self._entry(n.ctrl, lambda v: (setattr(n, "ctrl", v), changed())))
            self._check(n.create, lambda v: (setattr(n, "create", v), changed()),
                        "☑ 不存在则创建")
            self._row("控件类型",
                      self._combo(n.ctype, MENU_CTRL_TYPE_NAMES,
                                  lambda v: (setattr(n, "ctype", v), changed())))
            self._row("适配(仅图片控件生效)",
                      self._combo(n.fit, MENU_CTRL_FIT_NAMES,
                                  lambda v: (setattr(n, "fit", v), changed())))
            self._row("位置 X", self._entry(str(n.px),
                      lambda v: (setattr(n, "px", _int(v, n.px)), changed())))
            self._row("位置 Y", self._entry(str(n.py),
                      lambda v: (setattr(n, "py", _int(v, n.py)), changed())))
            self._row("宽度", self._entry(str(n.pw),
                      lambda v: (setattr(n, "pw", _int(v, n.pw)), changed())))
            self._row("高度", self._entry(str(n.ph),
                      lambda v: (setattr(n, "ph", _int(v, n.ph)), changed())))
            self._row("文字", self._entry(n.text,
                      lambda v: (setattr(n, "text", v), changed())))
            self._file_btn("背景(颜色直接手输 #RRGGBB;图片点浏览)", n.bg,
                           lambda v: (setattr(n, "bg", v), changed()))
            self._check(n.use_signal_data,
                        lambda v: (setattr(n, "use_signal_data", v), changed()),
                        "☑ 文字来自信号数据")
            self._check(n.show, lambda v: (setattr(n, "show", v), changed()),
                        "☑ 显示")
            self._hint("控件位置/大小是相对菜单画面的像素坐标。\n"
                       "输入框：点击后直接键盘输入,回车确认(触发该控件的点击起点),\n"
                       "内容可用 gal_menu_get_text(控件名) 读取(如在镜头代码块里)。\n"
                       "「文字」输入口(蓝色)可连「数据入口」,用镜头传入的值动态显示文字。")
        elif t == M_CLICK:
            names = ed.scene().ctrl_names() if ed.scene() else []
            self._row("被点击的控件",
                      self._combo(n.target_ctrl, names,
                                  lambda v: (setattr(n, "target_ctrl", v), changed()),
                                  editable=True))
            self._hint("当该控件被点击时,从这里开始执行。\n"
                       "此起点无需输入连接,直接输出处理流程。")
        elif t == M_SIGNAL:
            self._row("信号名", self._entry(n.signal,
                      lambda v: (setattr(n, "signal", v), changed())))
            self._hint("当镜头发送同名信号时,从这里开始执行(仅叠加菜单)。")
        elif t == M_EXIT:
            self._row("退出结果名", self._entry(n.exit_name,
                      lambda v: (setattr(n, "exit_name", v), changed())))
            self._hint("独占菜单：结束菜单并返回此结果名,镜头按返回值分支。\n"
                       "结果名不可与其他退出节点重名。\n"
                       "⚠ 退出节点请从「点击起点」/「信号起点」连入；\n"
                       "  不要直接接在「开场起点」链上，否则菜单一打开就立即退出。")
        elif t == M_IN:
            self._row("参数名", self._entry(n.param_name,
                      lambda v: (setattr(n, "param_name", v), ed.on_scene_changed())))
            self._hint("数据入口：定义本菜单的一个输入参数。\n"
                       "镜头里的「菜单」块会自动生成同名的 value 输入口,连线即可把值传入。\n"
                       "菜单内把本节点的 value 输出连到控件的「文字」输入,即可动态显示传入值。")
        elif t == M_JUMP:
            scenes = ed.app.project.scenes
            names = [f"镜头 {i + 1}: {s.title}" for i, s in enumerate(scenes)]
            si = n.flow_scene if 0 <= n.flow_scene < len(scenes) else 0
            self._row("目标镜头", self._combo(si, names,
                      lambda v: (setattr(n, "flow_scene", v), changed())))
            self._hint("结束菜单并跳转到指定镜头(返回后镜头直接切过去)。\n"
                       "适合做「去读档页 / 返回标题」等菜单按钮。")
        elif t == N_DEFVAR:
            vnames = sorted(v.name for v in vars_mod.collect_vars(ed.app.project))
            self._row("变量名", self._combo(n.var_name, vnames,
                      lambda v: (setattr(n, "var_name", v), changed()), editable=True))
            self._row("类型", self._combo(n.var_type, ["int", "float", "str", "bool"],
                      lambda v: (setattr(n, "var_type", v), changed())))
            self._row("初始值", self._entry(n.var_value,
                      lambda v: (setattr(n, "var_value", v), changed())))
            self._hint("定义全局变量并设初值(菜单内统一用全局,镜头可读取)。")
        elif t == N_SETVAR:
            vnames = sorted(v.name for v in vars_mod.collect_vars(ed.app.project))
            self._row("变量名", self._combo(n.var_name, vnames,
                      lambda v: (setattr(n, "var_name", v), changed()), editable=True))
            self._row("类型", self._combo(n.var_type, ["int", "float", "str", "bool"],
                      lambda v: (setattr(n, "var_type", v), changed())))
            self._row("取值方式", self._combo(n.set_mode, ["字面值", "来自 value 输入"],
                      lambda v: (setattr(n, "set_mode", v), changed())))
            if n.set_mode == 0:
                self._row("值", self._entry(n.value,
                          lambda v: (setattr(n, "value", v), changed())))
            self._hint("把值写入全局变量(如把输入框内容存进变量)。\n"
                       "「来自 value 输入」：从 A 口(蓝色)连线数据源。")
        elif t == N_GETVAR:
            vnames = sorted(v.name for v in vars_mod.collect_vars(ed.app.project))
            self._row("变量名", self._combo(n.var_name, vnames,
                      lambda v: (setattr(n, "var_name", v), changed()), editable=True))
            self._row("类型", self._combo(n.var_type, ["int", "float", "str", "bool"],
                      lambda v: (setattr(n, "var_type", v), changed())))
            self._hint("读取全局变量值,作为数据源输出(连到控件文字/拼接/设置变量等)。")
        elif t in (N_SAVE, N_LOAD, N_FILE_WRITE, N_FILE_READ):
            self._row("文件路径", self._entry(n.file_path,
                      lambda v: (setattr(n, "file_path", v), changed())))
            if t == N_SAVE:
                self._hint("把所有全局变量写入存档文件(相对项目根)。")
            elif t == N_LOAD:
                self._hint("从存档文件恢复全部全局变量。")
            elif t == N_FILE_WRITE:
                self._hint("把 A 口(value 输入)的内容写入文本文件(覆盖)。")
            else:
                self._hint("读取文本文件内容,作为数据源输出(连到拼接/控件文字等)。")
        elif t == N_TOAST:
            self._row("文本", self._entry(n.toast_text,
                      lambda v: (setattr(n, "toast_text", v), changed())))
            self._row("角落", self._combo(n.toast_corner, ["左上", "右上", "左下", "右下"],
                      lambda v: (setattr(n, "toast_corner", v), changed())))
            self._row("时长 ms", self._entry(str(n.toast_ms),
                      lambda v: (setattr(n, "toast_ms", _int(v, n.toast_ms)), changed())))
            self._hint("快速消息提示(角落弹出,不阻塞)。适合「已保存」「欢迎回来」等。")
        elif t == N_WAIT:
            self._row("毫秒", self._entry(str(n.ms),
                      lambda v: (setattr(n, "ms", _int(v, n.ms)), changed())))
            self._hint("暂停指定毫秒后继续。")
        elif t == N_TEXT:
            self._hint("文本拼接：把 A/B/C 三个 value 输入的值拼成一个字符串输出。\n"
                       "空段自动忽略。可连到控件文字/设置变量等。")
        elif t in (N_LOGIC, N_MATH):
            ops = (["==", "!=", ">", "<", ">=", "<=", "&&", "||"] if t == N_LOGIC
                   else ["+", "-", "*", "/", "%"])
            self._row("运算", self._combo(n.op, ops,
                      lambda v: (setattr(n, "op", v), changed()), editable=True))
            self._hint(("对 A、B 两个输入做逻辑运算,输出布尔(数据源)。"
                        if t == N_LOGIC else "对 A、B 两个输入做算术运算,输出数值(数据源)。"))
        elif t == N_TEXTLIT:
            self._row("文本", self._entry(n.lit_text,
                      lambda v: (setattr(n, "lit_text", v), changed())))
            self._hint("输出一段文本字面值(数据源)。")
        elif t == N_NUMLIT:
            self._row("数值", self._entry(str(n.lit_num),
                      lambda v: (setattr(n, "lit_num", v), changed())))
            self._hint("输出一个数字字面值(数据源)。")
        elif t == N_BOLLIT:
            self._row("值", self._combo(1 if n.lit_bool else 0, ["false", "true"],
                      lambda v: (setattr(n, "lit_bool", v), changed())))
            self._hint("输出布尔字面值(数据源)。")
        elif t == N_INPUT:
            inputs = [c.ctrl for c in ed.scene().ctrl_nodes()
                      if getattr(c, "ctype", 0) == 4 and c.ctrl] if ed.scene() else []
            if inputs:
                cur = n.ctrl if n.ctrl in inputs else inputs[0]
                self._row("输入框控件", self._combo(cur, inputs,
                          lambda v: (setattr(n, "ctrl", v), changed()), editable=True))
            else:
                self._hint("菜单还没有「输入框」控件(先加一个类型=输入框的控件)。")
            self._hint("读取该输入框的当前内容,作为数据源输出。\n"
                       "配合「设置变量」即可把玩家输入存进全局变量。")
        elif t == N_RANDOM:
            self._row("随机上限 (0..max)", self._entry(str(n.max),
                      lambda v: (setattr(n, "max", _int(v, n.max)), changed())))
            self._hint("生成 0..max-1 的随机整数(数据源)。")


def _int(s, default):
    try:
        return int(s.strip())
    except Exception:
        return default


class MenuEditor(tk.Toplevel):
    """MENU 场景编辑器窗口。"""

    def __init__(self, app, index):
        super().__init__(app)
        self.app = app
        self.index = index
        self.title("MENU 菜单编辑器")
        self.geometry("1180x700")
        self.minsize(900, 560)
        self.configure(bg=T.BG)
        self.transient(app)
        self.drag_source = None     # BlueCanvas 面板拖拽依赖;菜单编辑器用底部按钮,置空即可

        # 顶部工具条
        top = tk.Frame(self, bg=T.BG2, height=46)
        top.pack(fill="x", side="top")
        tk.Label(top, text="菜单：", bg=T.BG2, fg=T.FG2,
                 font=T.FONT_SM).pack(side="left", padx=(10, 4), pady=10)
        self.menu_combo = ttk.Combobox(top, state="readonly", width=22, font=T.FONT_SM)
        self.menu_combo.pack(side="left", pady=10)
        self.menu_combo.bind("<<ComboboxSelected>>", self._on_pick_menu)
        for txt, cmd in (("＋新建", self._add_menu), ("改名", self._rename_menu),
                         ("－删", self._del_menu)):
            tk.Button(top, text=txt, command=cmd, bg=T.BG3, fg=T.FG,
                      activebackground=T.ACCENT_DARK, activeforeground="#fff",
                      relief="flat", font=T.FONT_SM, padx=10, pady=2,
                      cursor="hand2").pack(side="left", padx=(6, 0), pady=8)
        tk.Button(top, text="▶ 预览", command=self._preview, bg=T.ACCENT,
                  fg="#fff", activebackground=T.ACCENT_DARK, relief="flat",
                  font=T.FONT_SM, padx=12, pady=2, cursor="hand2"
                  ).pack(side="right", padx=10, pady=8)

        # 主体:画布 + 右侧(预览 + 属性面板)
        body = tk.Frame(self, bg=T.BG)
        body.pack(fill="both", expand=True)
        self.canvas = MenuCanvas(body, self)
        self.canvas.pack(side="left", fill="both", expand=True)
        right = tk.Frame(body, bg=T.BG2, width=300)
        right.pack(side="right", fill="y")
        right.pack_propagate(False)
        # 实时预览(纯布局展示,不处理点击/信号)
        pv = tk.LabelFrame(right, text=" 实时预览 · 按比例 ", bg=T.BG2, fg=T.ACCENT,
                           font=T.FONT_SM, relief="flat")
        pv.pack(fill="x", padx=6, pady=(6, 2))
        self._pv = tk.Canvas(pv, bg="#101218", height=172,
                             highlightthickness=1, highlightbackground="#3a3e4c")
        self._pv.pack(fill="x", padx=4, pady=4)
        self._pv_imgs = []
        self._pv.bind("<Configure>", lambda _e: self._draw_preview())
        self.inspector = MenuInspector(right, self)
        self.inspector.pack(fill="both", expand=True)

        # 底部:块添加条(可横向滚动)
        bar = tk.Frame(self, bg=T.BG2, height=56)
        bar.pack(fill="x", side="bottom")
        bc = tk.Canvas(bar, bg=T.BG2, height=44, highlightthickness=0)
        hs = tk.Scrollbar(bar, orient="horizontal", command=bc.xview)
        bc.configure(xscrollcommand=hs.set)
        inner = tk.Frame(bc, bg=T.BG2)
        inner_id = bc.create_window((0, 0), window=inner, anchor="nw")
        inner.bind("<Configure>",
                   lambda e: (bc.configure(scrollregion=bc.bbox("all")),
                              bc.itemconfig(inner_id, height=44)))
        tk.Label(inner, text="添加节点：", bg=T.BG2, fg=T.FG2,
                 font=T.FONT_SM).pack(side="left", padx=(10, 6), pady=8)
        for t, name in ((M_CTRL, "控件"), (M_CLICK, "点击起点"),
                        (M_SIGNAL, "信号起点"), (M_EXIT, "退出"), (M_IN, "数据入口"),
                        (M_JUMP, "跳转镜头"),
                        (N_DEFVAR, "定义变量"), (N_SETVAR, "设置变量"),
                        (N_GETVAR, "读变量"), (N_SAVE, "存档"), (N_LOAD, "读档"),
                        (N_FILE_WRITE, "写文件"), (N_FILE_READ, "读文件"),
                        (N_TOAST, "消息"), (N_WAIT, "等待"),
                        (N_TEXT, "拼接"), (N_LOGIC, "逻辑"), (N_MATH, "运算"),
                        (N_TEXTLIT, "字面文本"), (N_NUMLIT, "字面数字"),
                        (N_BOLLIT, "布尔"), (N_INPUT, "输入框值"), (N_RANDOM, "随机")):
            tk.Button(inner, text=name, command=lambda tt=t: self._add_node_center(tt),
                      bg=T.BG3, fg=T.FG, activebackground=T.ACCENT_DARK,
                      activeforeground="#fff", relief="flat", font=T.FONT_SM,
                      padx=8, pady=2, cursor="hand2").pack(side="left", padx=(4, 0), pady=6)
        bc.pack(side="top", fill="x")
        hs.pack(side="bottom", fill="x")

        self.protocol("WM_DELETE_WINDOW", self._close)
        self._refresh_top()
        self._draw_preview()

    # ---------------- 实时预览 ----------------
    def _color_of(self, s):
        if not s:
            return None
        s = s.strip()
        if s.startswith("#") and len(s) >= 7:
            try:
                return "#%02x%02x%02x" % (int(s[1:3], 16), int(s[3:5], 16), int(s[5:7], 16))
            except Exception:
                return None
        return None

    def _load_thumb(self, path, tw, th):
        """加载图片并缩放到预览尺寸;失败返回 None。"""
        if not path:
            return None
        root = self.app.project_root or os.getcwd()
        ap = path if os.path.isabs(path) else os.path.join(root, path)
        if not os.path.isfile(ap):
            return None
        try:
            from PIL import Image, ImageTk
            im = Image.open(ap).convert("RGBA")
            im.thumbnail((max(1, int(tw)), max(1, int(th))))
            return ImageTk.PhotoImage(im)
        except Exception:
            return None

    def _draw_preview(self):
        """绘制控件布局预览(按项目窗口 960x540 等比缩放,纯展示)。"""
        cv = getattr(self, "_pv", None)
        if cv is None:
            return
        try:
            cv.delete("all")
        except Exception:
            return
        m = self.scene()
        if not m:
            return
        try:
            w = cv.winfo_width() or 280
            h = cv.winfo_height() or 172
        except Exception:
            w, h = 280, 172
        scale = min(w / 960.0, h / 540.0)
        ow, oh = 960 * scale, 540 * scale
        ox, oy = (w - ow) / 2, (h - oh) / 2
        cv.create_rectangle(ox, oy, ox + ow, oy + oh, fill="#14141e", outline="#3a3e4c")
        self._pv_imgs = []
        # 菜单背景图
        if m.bg:
            img = self._load_thumb(m.bg, ow, oh)
            if img:
                self._pv_imgs.append(img)
                cv.create_image(ox, oy, anchor="nw", image=img)
        font = (T.FONT[0], max(7, int(10 * scale)))
        small = (T.FONT[0], max(6, int(8 * scale)))
        for n in m.ctrl_nodes():
            if not getattr(n, "show", 1):
                continue
            x = ox + n.px * scale
            y = oy + n.py * scale
            pw, ph = n.pw * scale, n.ph * scale
            fill = self._color_of(n.bg)
            t = n.ctype
            if t == 0:      # 按钮
                cv.create_rectangle(x, y, x + pw, y + ph,
                                    fill=fill or "#3377dd", outline="#ffffff")
                cv.create_text(x + pw / 2, y + ph / 2, text=n.text or "按钮",
                               fill="#fff", font=font)
            elif t == 1:    # 文字
                cv.create_text(x + 2, y + 2, anchor="nw", text=n.text or "文字",
                               fill="#fff", font=font)
            elif t == 2:    # 图片
                img = self._load_thumb(n.bg, pw, ph) if n.bg else None
                if img:
                    self._pv_imgs.append(img)
                    cv.create_image(x, y, anchor="nw", image=img)
                else:
                    cv.create_rectangle(x, y, x + pw, y + ph, fill="#2a2d37", outline="#666")
                    cv.create_text(x + pw / 2, y + ph / 2, text="图", fill="#999", font=small)
            elif t == 3:    # 面板
                cv.create_rectangle(x, y, x + pw, y + ph,
                                    fill=fill or "#223344", outline="#445566")
                if n.text:
                    cv.create_text(x + 6, y + 4, anchor="nw", text=n.text, fill="#dde", font=small)
            elif t == 4:    # 输入框
                cv.create_rectangle(x, y, x + pw, y + ph, fill="#1a1f2a", outline="#5577aa")
                cv.create_text(x + 6, y + ph / 2, anchor="w", text=n.text or "输入…",
                               fill="#ccc", font=small)

    # ---------------- 访问器(供 MenuCanvas 调用) ----------------
    def scene(self):
        if 0 <= self.index < len(self.app.project.menus):
            return self.app.project.menus[self.index]
        return None

    def on_select(self):
        self.inspector.flush()
        self.canvas.render()
        self._draw_preview()
        self.inspector.rebuild()

    def on_node_edited(self):
        """属性修改:只刷新画布摘要,不重建面板(避免 flush 递归/丢焦点)。"""
        self.app._push_snapshot()
        self.app.dirty = True
        self.canvas.render()
        self._draw_preview()
        self.app._refresh_title()

    def on_scene_changed(self, keep_selection=True):
        """结构变化(增删节点/连线):提交输入、刷新画布并重建属性面板。"""
        depth = getattr(self, "_scene_change_depth", 0) + 1
        self._scene_change_depth = depth
        try:
            if depth > 8:
                return
            self.app._push_snapshot()
            self.app.dirty = True
            self.inspector.flush()
            self.canvas.render()
            self._draw_preview()
            self.inspector.rebuild()
            self.app.refresh_left()
            self.app._refresh_title()
            # 菜单退出节点可能变化:同步镜头里的菜单块输出口并刷新主画布
            self.app.sync_menu_blocks()
            self.app.canvas.render()
        finally:
            self._scene_change_depth = depth - 1

    def add_node_at(self, ntype, x, y):
        m = self.scene()
        if not m:
            return
        n = m.new_node(ntype, x, y)
        if ntype == M_CTRL:
            n.ctrl = self._auto_ctrl_name()
        self.canvas.selected = n.id
        self.on_scene_changed()

    def delete_selected(self):
        if self.canvas.selected is not None:
            self.canvas._delete(self.canvas.selected)

    def undo(self):
        self.inspector.flush()
        self.app.undo()
        self.on_project_restored()

    def redo(self):
        self.inspector.flush()
        self.app.redo()
        self.on_project_restored()

    # ---------------- 撤销/重做后同步 ----------------
    def on_project_restored(self):
        if not (0 <= self.index < len(self.app.project.menus)):
            self._close()
            return
        self.canvas.selected = None
        self.canvas.render()
        self.inspector.rebuild()
        self._refresh_top()

    # ---------------- 菜单管理 ----------------
    def _refresh_top(self):
        names = [m.title for m in self.app.project.menus]
        self.menu_combo["values"] = names
        if 0 <= self.index < len(names):
            self.menu_combo.set(names[self.index])

    def _on_pick_menu(self, _e=None):
        i = self.menu_combo.current()
        if i >= 0 and i != self.index:
            self.index = i
            self.canvas.selected = None
            self.canvas.render()
            self.inspector.rebuild()
            self.app.refresh_left()

    def _add_menu(self):
        m = menu_defaults(f"菜单 {len(self.app.project.menus) + 1}")
        self.app.project.menus.append(m)
        self.index = len(self.app.project.menus) - 1
        self.app._push_snapshot()
        self.app.dirty = True
        self.canvas.selected = None
        self.canvas.render()
        self.inspector.rebuild()
        self._refresh_top()
        self.app.refresh_left()
        self.app._refresh_title()

    def _rename_menu(self):
        m = self.scene()
        if not m:
            return
        name = simpledialog.askstring("重命名菜单", "菜单名称：", parent=self,
                                      initialvalue=m.title)
        if name:
            m.title = name
            self.app.dirty = True
            self._refresh_top()
            self.app.refresh_left()

    def _del_menu(self):
        m = self.scene()
        if not m:
            return
        if not messagebox.askyesno("删除菜单", f"确定删除菜单「{m.title}」？", parent=self):
            return
        del self.app.project.menus[self.index]
        if not self.app.project.menus:
            self.index = -1
            self.canvas.render()
            self.inspector.rebuild()
        else:
            self.index = min(self.index, len(self.app.project.menus) - 1)
            self.canvas.selected = None
            self.canvas.render()
            self.inspector.rebuild()
        self.app._push_snapshot()
        self.app.dirty = True
        self._refresh_top()
        self.app.refresh_left()
        self.app._refresh_title()

    def _auto_ctrl_name(self):
        m = self.scene()
        used = set(m.ctrl_names()) if m else set()
        i = 1
        while f"控件 {i}" in used:
            i += 1
        return f"控件 {i}"

    def _add_node_center(self, ntype):
        x = (self.canvas.winfo_width() // 2) - self.canvas.vx - 60
        y = (self.canvas.winfo_height() // 2) - self.canvas.vy - 40
        self.add_node_at(ntype, x, y)

    def _preview(self):
        # 预览:只预览当前菜单(临时项目,VM 独立窗口)
        self.inspector.flush()
        m = self.scene()
        if not m:
            return
        if m.mode == 0 and not m.has_exit_path():
            if not messagebox.askyesno(
                    "预览提示",
                    "该菜单没有可达的「退出」节点,运行时无法正常退出\n"
                    "(会卡在菜单画面;可按 ESC 键强制退出)。\n\n仍要预览吗？",
                    parent=self):
                return
        self.app.preview_menu(self.index)

    def _close(self):
        try:
            self.inspector.flush()
        except Exception:
            pass
        if self in getattr(self.app, "menu_windows", []):
            self.app.menu_windows.remove(self)
        self.destroy()
