"""DEXIDE 代码编辑器:行号 + 断点槽 + 语法着色 + 悬停提示 + 自动补全。"""

import re
import tkinter as tk

from dexlang import Lexer, DexError
from dexlang import opcodes as O

from .theme import C, HL

_KEYWORDS = {"let", "if", "else", "while", "func", "return", "print",
             "true", "false", "include", "refer", "extern"}

_TWO_CHAR = {"==", "!=", "<=", ">=", "&&", "||", "->"}


class CodeEditor(tk.Frame):
    FONT_FAMILY = "Cascadia Code"

    def __init__(self, master, path, analyzer, on_modify=None,
                 on_breakpoints=None, on_cursor=None):
        super().__init__(master, bg=C["crust"])
        self.path = path
        self.analyzer = analyzer
        self.on_modify = on_modify
        self.on_breakpoints_change = on_breakpoints
        self.on_cursor_move = on_cursor
        self.breakpoints = set()
        self._debug_line = None
        self._font_size = 12
        # 自动补全:默认开启。弹窗为"被动"形式(不抢焦点),与 JASON IDE 一致,
        # 因此输入中文(IME 组合)不会被终止;可在 IDE「视图」菜单关闭。
        self.auto_complete = True
        self._hl_after = None
        self._hover_after = None
        self._hover_popup = None
        self._ac_popup = None
        self._ac_after = None
        self._ac_items = []
        self._ac_sel = 0
        self._ac_auto = False
        self._ac_listbox = None
        self._ignore_modify = False

        self.bp = tk.Text(self, width=2, padx=2, pady=6, takefocus=0, border=0,
                          bg=C["mantle"], fg=C["red"], font=(self.FONT_FAMILY, self._font_size),
                          state="disabled", relief="flat", cursor="hand2")
        self.ln = tk.Text(self, width=4, padx=6, pady=6, takefocus=0, border=0,
                          bg=C["mantle"], fg=C["surface2"], font=(self.FONT_FAMILY, self._font_size),
                          state="disabled", relief="flat")
        self.text = tk.Text(self, wrap="none", undo=True,
                            bg=C["base"], fg=C["text"], insertbackground=C["text"],
                            selectbackground=C["surface1"], font=(self.FONT_FAMILY, self._font_size),
                            relief="flat", border=0, padx=10, pady=6, tabs=("1c", 4))
        self.vbar = tk.Scrollbar(self, orient="vertical", command=self._scroll,
                                 bg=C["surface0"], troughcolor=C["mantle"],
                                 activebackground=C["surface1"], relief="flat", borderwidth=0)
        self.text.configure(yscrollcommand=self._set_scroll)

        self.bp.pack(side="left", fill="y")
        self.ln.pack(side="left", fill="y")
        self.text.pack(side="left", fill="both", expand=True)
        self.vbar.pack(side="right", fill="y")

        # 语法标签
        for tag, color in HL.items():
            self.text.tag_configure(tag, foreground=color)
        self.text.tag_configure("comment", foreground=HL["comment"], font=(self.FONT_FAMILY, self._font_size, "italic"))
        self.text.tag_configure("err", underline=True, foreground=C["red"])
        self.text.tag_configure("warn", underline=True, foreground=C["yellow"])
        self.text.tag_configure("debug_line", background=C["surface0"])
        self.bp.tag_configure("bp", foreground=C["red"])
        self.bp.tag_configure("bpcur", foreground=C["green"])
        self.ln.tag_configure("cur", foreground=C["peach"])

        # 事件
        self.text.bind("<KeyRelease>", self._on_key)
        self.text.bind("<<Modified>>", self._on_modified)
        self.text.bind("<ButtonRelease-1>", self._on_click)
        self.text.bind("<Motion>", self._on_motion)
        self.text.bind("<Leave>", self._on_leave)
        self.text.bind("<MouseWheel>", self._on_wheel)
        self.text.bind("<Control-MouseWheel>", self._on_zoom)
        self.text.bind("<Control-space>", self._show_ac)
        self.text.bind("<Alt-slash>", self._show_ac)
        self.text.bind("<Control-s>", lambda e: "break")
        # 补全弹窗打开时的编辑器内导航(不抢焦点,避免打断 IME)
        self.text.bind("<Up>", self._ac_navigate)
        self.text.bind("<Down>", self._ac_navigate)
        self.text.bind("<Return>", self._ac_navigate)
        self.text.bind("<KP_Enter>", self._ac_navigate)
        self.text.bind("<Tab>", self._ac_navigate)
        self.text.bind("<Escape>", self._ac_navigate)
        self.text.bind("<FocusOut>", lambda e: self._close_ac())
        self.bp.bind("<Button-1>", self._toggle_bp)
        self.bp.bind("<MouseWheel>", lambda e: "break")
        self.ln.bind("<MouseWheel>", lambda e: "break")
        self.ln.bind("<Control-MouseWheel>", self._on_zoom)
        self.ln.bind("<Button-1>", self._set_cur_line_from_ln)
        self.bp.bind("<Control-MouseWheel>", self._on_zoom)

        self._init_content()
        self._sync_all()

    # ---------- 内容 ----------
    def _init_content(self):
        self._ignore_modify = True
        self.text.edit_reset()
        self.text.edit_modified(False)
        self._ignore_modify = False

    def set_text(self, text, reset_undo=True):
        self._ignore_modify = True
        self.text.delete("1.0", "end")
        self.text.insert("1.0", text)
        if reset_undo:
            self.text.edit_reset()
        self.text.edit_modified(False)
        self._ignore_modify = False
        self._sync_all()
        self._apply_highlight()

    def get_text(self):
        return self.text.get("1.0", "end-1c")

    # ---------- 断点 ----------
    def toggle_breakpoint(self, line):
        if line in self.breakpoints:
            self.breakpoints.remove(line)
        else:
            self.breakpoints.add(line)
        self._render_bp()
        if self.on_breakpoints_change:
            self.on_breakpoints_change(self.breakpoints)

    def set_breakpoints(self, lines):
        self.breakpoints = set(lines)
        self._render_bp()

    def _toggle_bp(self, ev):
        idx = self.bp.index(f"@{ev.x},{ev.y}")
        line = int(float(idx))
        if 1 <= line <= self._n_lines():
            self.toggle_breakpoint(line)
        return "break"

    def _render_bp(self):
        self.bp.configure(state="normal")
        self.bp.delete("1.0", "end")
        n = self._n_lines()
        for i in range(1, n + 1):
            if i in self.breakpoints:
                mark = "●" if i != self._debug_line else "▶"
                tag = "bpcur" if i == self._debug_line else "bp"
                self.bp.insert("end", mark, tag)
            else:
                self.bp.insert("end", " ")
            self.bp.insert("end", "\n")
        self.bp.configure(state="disabled")

    # ---------- 调试行 ----------
    def set_debug_line(self, line):
        self._debug_line = line
        self._render_bp()
        self._highlight_debug_line()

    def clear_debug_line(self):
        self._debug_line = None
        self._render_bp()
        self._highlight_debug_line()

    def _highlight_debug_line(self):
        self.text.tag_remove("debug_line", "1.0", "end")
        if self._debug_line:
            self.text.tag_add("debug_line", f"{self._debug_line}.0", f"{self._debug_line}.0 lineend")

    # ---------- 行号/滚动 ----------
    def _n_lines(self):
        return int(self.text.index("end-1c").split(".")[0])

    def _set_scroll(self, first, last):
        self.vbar.set(first, last)
        self.bp.yview_moveto(first)
        self.ln.yview_moveto(first)

    def _scroll(self, *args):
        self.text.yview(*args)
        self.bp.yview(*args)
        self.ln.yview(*args)

    def _on_wheel(self, ev):
        self.text.yview_scroll(int(-ev.delta / 120), "units")
        f, l = self.text.yview()
        self.bp.yview_moveto(f)
        self.ln.yview_moveto(f)
        return "break"

    def _on_zoom(self, ev):
        d = int(-ev.delta / 120)
        ns = min(36, max(8, self._font_size + d))
        if ns == self._font_size:
            return "break"
        self._font_size = ns
        f = (self.FONT_FAMILY, ns)
        self.text.config(font=f)
        self.ln.config(font=f)
        self.bp.config(font=f)
        self.text.tag_configure("comment", font=(self.FONT_FAMILY, ns, "italic"))
        return "break"

    def _sync_line_numbers(self):
        self.ln.configure(state="normal")
        self.ln.delete("1.0", "end")
        n = self._n_lines()
        cur = self._cursor_line()
        for i in range(1, n + 1):
            self.ln.insert("end", str(i) + ("\n" if i < n else ""), "cur" if i == cur else None)
        self.ln.configure(state="disabled")

    def _sync_all(self):
        self._render_bp()
        self._sync_line_numbers()
        self._apply_highlight()

    def _cursor_line(self):
        try:
            return int(float(self.text.index("insert")))
        except tk.TclError:
            return 1

    def _set_cur_line_from_ln(self, ev):
        idx = self.ln.index(f"@{ev.x},{ev.y}")
        line = int(float(idx))
        self.text.mark_set("insert", f"{line}.0")
        return "break"

    # ---------- 修改 ----------
    def _on_modified(self, _ev=None):
        if self._ignore_modify:
            return
        if self.text.edit_modified():
            self.text.edit_modified(False)
            self._schedule_highlight()
            if self.on_modify:
                self.on_modify()

    # KeyRelease 时应忽略的纯导航/编辑键(不触发补全、不关闭弹窗、不做智能换行)
    _AC_KEYS = frozenset(("Return", "KP_Enter", "Up", "Down", "Tab", "Escape",
                          "BackSpace", "Delete", "Left", "Right", "Home",
                          "End", "Prior", "Next"))

    def _on_key(self, ev):
        # KeyRelease:回车/导航等纯按键不做补全逻辑。
        # 智能换行已在 KeyPress(_ac_navigate)完成,这里若再插入会多一个空行。
        if ev.keysym in self._AC_KEYS:
            return
        ch = ev.char or ""
        if (ch.isalpha() and ch.isascii()) or ch == "_":
            # 纯 ASCII 标识符:自动补全开启时自动触发;弹窗已开时刷新候选
            if self.auto_complete or self._ac_popup is not None:
                self._schedule_ac()
        else:
            # 关闭补全弹窗,但不关闭 Ctrl+Space 松开时的弹窗
            if self._ac_popup is not None and not (ev.keysym == "space" and (ev.state & 0x4)):
                self._close_ac()
        if self.on_cursor_move:
            self.on_cursor_move()

    def _smart_newline(self):
        """智能换行(KeyPress 触发):继承上一行缩进,行尾 { 多加一层。"""
        try:
            line = self._cursor_line()
            prev = self.text.get(f"{line - 1}.0", f"{line - 1}.0 lineend")
            indent = len(prev) - len(prev.lstrip())
            if prev.rstrip().endswith("{"):
                indent += 4
            self.text.insert("insert", "\n" + " " * indent)
            return "break"
        except tk.TclError:
            return "break"

    def _on_click(self, _ev=None):
        if self.on_cursor_move:
            self.on_cursor_move()
        self._sync_line_numbers()

    # ---------- 高亮 ----------
    def _schedule_highlight(self):
        if self._hl_after:
            self.after_cancel(self._hl_after)
        self._hl_after = self.after(250, self._apply_highlight)

    def _clear_hl_tags(self):
        for tag in ("keyword", "type", "number", "string", "comment", "operator",
                    "func", "native", "builtin", "variable", "library", "label",
                    "extern", "err", "warn"):
            try:
                self.text.tag_remove(tag, "1.0", "end")
            except tk.TclError:
                pass

    def _apply_highlight(self):
        self._hl_after = None
        self._clear_hl_tags()
        src = self.get_text()

        if self.path.endswith(".dxasm"):
            self._apply_asm_highlight(src)
        else:
            try:
                tokens = Lexer(src, self.path).tokenize()
            except DexError:
                return
            prev_ident = None   # 上一个标识符(判断是否为调用)
            for tok in tokens:
                tag = self._token_tag(tok, prev_ident)
                if tag:
                    self._apply_tag(tok.line, tok.col, len(tok.lexeme), tag)
                if tok.kind.name == "IDENT":
                    prev_ident = tok.lexeme
                else:
                    prev_ident = None

        self._apply_problem_marks()
        self._highlight_debug_line()

    def _apply_problem_marks(self):
        # 错误/警告下划线
        fa = self.analyzer.files.get(os_abspath(self.path))
        if fa:
            for p in fa.problems:
                if p.line <= 0:
                    continue
                tag = "err" if p.severity == "error" else "warn"
                try:
                    self.text.tag_add(tag, f"{p.line}.0", f"{p.line}.0 lineend")
                except tk.TclError:
                    pass

    # ---------- 汇编(.dxasm)着色 ----------
    def _apply_asm_highlight(self, src):
        mnems = O.MNEMONIC_TO_OP  # 助记符名 → 操作码
        lines = src.split("\n")
        for i, line in enumerate(lines, 1):
            ln = f"{i}.0"
            work = line
            # 注释 #
            hp = work.find("#")
            if hp >= 0:
                try:
                    self.text.tag_add("comment", f"{ln}+{hp}c", f"{i}.0 lineend")
                except tk.TclError:
                    pass
                work = work[:hp]
            stripped = work.strip()
            if not stripped:
                continue
            # 标签 NAME:
            m = re.match(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*):\s*$", work)
            if m:
                col = len(m.group(1))
                name = m.group(2)
                self._apply_tag_abs(i, col, len(name), "label")
                continue
            # 指令 / 指令头
            m = re.match(r"^(\s*)([A-Za-z.][\w]*)(.*)$", work)
            if not m:
                continue
            col = len(m.group(1))
            head = m.group(2)
            rest = m.group(3)
            if head in (".func", ".lib", ".native"):
                self._apply_tag_abs(i, col, len(head), "keyword")
                # .lib 的路径字符串
                sm = re.search(r'"([^"]*)"', rest)
                if sm:
                    c2 = col + len(head) + sm.start() + 1
                    self._apply_tag_abs(i, c2, len(sm.group(1)), "library")
            elif head == "NCALL" or head in mnems:
                self._apply_tag_abs(i, col, len(head), "keyword")
            # 操作数:字符串 / @函数 / %局部 / 数字
            for m2 in re.finditer(r'"([^"]*)"', rest):
                c2 = col + len(head) + m2.start() + 1
                self._apply_tag_abs(i, c2, len(m2.group(1)), "string")
            for m2 in re.finditer(r"@([A-Za-z_]\w*)", rest):
                c2 = col + len(head) + m2.start()
                self._apply_tag_abs(i, c2, len(m2.group(0)), "func")
            for m2 in re.finditer(r"%(\d+)", rest):
                c2 = col + len(head) + m2.start()
                self._apply_tag_abs(i, c2, len(m2.group(0)), "number")
            for m2 in re.finditer(r"\b\d+(\.\d+)?\b", rest):
                c2 = col + len(head) + m2.start()
                self._apply_tag_abs(i, c2, len(m2.group(0)), "number")

    def _apply_tag_abs(self, line, col0, length, tag):
        """按 (行, 0 基列) 应用标签。"""
        if length <= 0:
            return
        start = f"{line}.{col0}"
        end = f"{line}.{col0 + length}"
        try:
            self.text.tag_add(tag, start, end)
        except tk.TclError:
            pass

    def _token_tag(self, tok, prev_ident):
        kind = tok.kind
        name = kind.name
        if name in ("EOF",):
            return None
        if name == "INT" or name == "FLOAT":
            return "number"
        if name == "STRING":
            return "string"
        if kind.name in ("SEMI", "COMMA", "LPAREN", "RPAREN", "LBRACE", "RBRACE",
                         "COLON"):
            return "operator"
        if name == "IDENT":
            w = tok.lexeme
            if w in ("print",):
                return "builtin"
            if w in ("include", "refer"):
                return "builtin"
            if w == "extern":
                return "extern"
            if w in _KEYWORDS:
                return "keyword"
            # 调用?
            is_call = False
            try:
                nxt = self.text.get(f"{tok.line}.{tok.col + len(w)}", f"{tok.line}.{tok.col + len(w) + 1}")
                is_call = (nxt == "(")
            except tk.TclError:
                is_call = False
            if is_call and self.analyzer and w in self.analyzer.native_names():
                return "native"
            if is_call and w in self.analyzer.all_func_names():
                return "func"
            if w in ("int", "float", "string", "void"):
                return "type"
            # 变量
            if self._is_var(w):
                return "variable"
            return None
        # 运算符
        return "operator"

    def _is_var(self, word):
        fa = self.analyzer.files.get(os_abspath(self.path))
        if not fa:
            return False
        if word in fa.top_vars:
            return True
        func = self._func_at_cursor()
        if func and func in fa.funcs and word in fa.funcs[func].var_names:
            return True
        return False

    def _func_at_cursor(self):
        line = self._cursor_line()
        fa = self.analyzer.files.get(os_abspath(self.path))
        if not fa:
            return None
        best = None
        for name, fs in fa.funcs.items():
            if fs.line <= line and (best is None or fs.line > fa.funcs[best].line):
                best = name
        return best

    def _apply_tag(self, line, col, length, tag):
        if length <= 0:
            return
        start = f"{line}.{col}"
        end = f"{line}.{col + length}"
        try:
            self.text.tag_add(tag, start, end)
        except tk.TclError:
            pass

    # ---------- 悬停提示 ----------
    def _on_motion(self, ev):
        if self._hover_after:
            self.after_cancel(self._hover_after)
        self._hover_after = self.after(350, lambda: self._show_hover(ev))

    def _on_leave(self, _ev=None):
        if self._hover_after:
            self.after_cancel(self._hover_after)
            self._hover_after = None
        self._close_hover()

    def _show_hover(self, ev):
        self._hover_after = None
        try:
            idx = self.text.index(f"@{ev.x},{ev.y}")
            line = int(float(idx))
            col = int(str(idx).split(".")[1])
        except tk.TclError:
            return
        word = self._word_at(line, col)
        if not word:
            self._close_hover()
            return
        info = self.analyzer.hover(os_abspath(self.path), word)
        if not info:
            self._close_hover()
            return
        title, detail = info
        self._close_hover()
        pop = tk.Toplevel(self)
        pop.overrideredirect(True)
        pop.configure(bg=C["surface1"])
        x = self.winfo_rootx() + ev.x_root - self.winfo_rootx() + 12
        y = self.winfo_rooty() + ev.y_root - self.winfo_rooty() + 20
        frm = tk.Frame(pop, bg=C["surface1"])
        frm.pack(fill="both", expand=True)
        tk.Label(frm, text=title, bg=C["surface1"], fg=C["yellow"],
                 font=(self.FONT_FAMILY, 11, "bold"), justify="left").pack(anchor="w", padx=8, pady=(6, 0))
        tk.Label(frm, text=detail, bg=C["surface1"], fg=C["text"],
                 font=(self.FONT_FAMILY, 10), justify="left").pack(anchor="w", padx=8, pady=(2, 6))
        pop.wm_geometry(f"+{x}+{y}")
        self._hover_popup = pop

    def _close_hover(self):
        if self._hover_popup:
            try:
                self._hover_popup.destroy()
            except tk.TclError:
                pass
            self._hover_popup = None

    def _word_at(self, line, col):
        try:
            text = self.text.get(f"{line}.0", f"{line}.0 lineend")
        except tk.TclError:
            return ""
        i = max(0, col - 1)
        start = i
        end = i
        while start > 0 and (text[start - 1].isalnum() or text[start - 1] == "_"):
            start -= 1
        while end < len(text) and (text[end].isalnum() or text[end] == "_"):
            end += 1
        return text[start:end]

    # ---------- 自动补全 ----------
    def set_auto_complete(self, enabled):
        """开启/关闭输入时自动补全(默认关闭,避免打断中文输入法)。"""
        self.auto_complete = bool(enabled)
        if not enabled:
            self._close_ac()

    def _schedule_ac(self):
        if self._ac_after:
            self.after_cancel(self._ac_after)
        self._ac_after = self.after(260, self._show_ac)

    def _prefix(self):
        try:
            idx = self.text.index("insert")
            line = int(float(idx))
            col = int(str(idx).split(".")[1])
        except tk.TclError:
            return ""
        text = self.text.get(f"{line}.0", "insert")
        i = len(text)
        while i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_"):
            i -= 1
        return text[i:]

    def _show_ac(self, _ev=None):
        self._ac_after = None
        prefix = self._prefix()
        func = self._func_at_cursor()
        items = self.analyzer.autocomplete(os_abspath(self.path), func, prefix)
        if not items:
            self._close_ac()
            return
        self._ac_items = items
        self._ac_sel = 0
        if self._ac_popup is None:
            self._create_ac_popup()
        self._ac_update_list()
        self._ac_position()

    def _create_ac_popup(self):
        pop = tk.Toplevel(self)
        pop.overrideredirect(True)
        pop.configure(bg=C["surface1"])
        try:
            pop.attributes("-topmost", True)
        except tk.TclError:
            pass
        frame = tk.Frame(pop, bg=C["surface1"])
        frame.pack(fill="both", expand=True)
        lb = tk.Listbox(frame, bg=C["mantle"], fg=C["text"], selectbackground=C["surface0"],
                        selectforeground=C["yellow"], activestyle="none",
                        font=(self.FONT_FAMILY, 11), borderwidth=0, highlightthickness=0,
                        exportselection=False)
        lb.pack(side="left", fill="both", expand=True)
        sb = tk.Scrollbar(frame, orient="vertical", command=lb.yview,
                          bg=C["surface0"], troughcolor=C["mantle"], relief="flat", borderwidth=0)
        sb.pack(side="right", fill="y")
        lb.configure(yscrollcommand=sb.set)
        lb.bind("<ButtonRelease-1>", lambda e: self._ac_commit())
        lb.bind("<Double-Button-1>", lambda e: self._ac_commit())
        self._ac_popup = pop
        self._ac_listbox = lb
        # 关键:弹窗不抢焦点(不调用 focus_set);部分平台新建 Toplevel 会隐式抢焦点,
        # 这里强制把焦点还给编辑器,输入法组合不会被终止
        try:
            self.text.focus_force()
        except tk.TclError:
            pass

    def _ac_update_list(self):
        lb = self._ac_listbox
        lb.delete(0, "end")
        for word, kind, detail in self._ac_items:
            lb.insert("end", f"{word}   · {detail}")
        lb.selection_clear(0, "end")
        lb.selection_set(self._ac_sel)
        lb.see(self._ac_sel)
        lb.configure(height=min(12, max(1, len(self._ac_items))))

    def _ac_position(self):
        try:
            x, y = self.text.bbox("insert")[0:2]
        except (tk.TclError, TypeError):
            x, y = 20, 20
        self._ac_popup.wm_geometry(f"+{self.winfo_rootx() + x}+{self.winfo_rooty() + y + 24}")

    def _ac_navigate(self, ev):
        """补全弹窗打开时的编辑器内导航(不抢焦点,IME 安全)。
        弹窗未打开时,回车执行智能换行(自动缩进)。"""
        keysym = ev.keysym
        if self._ac_popup is not None and self._ac_items:
            if keysym == "Down":
                self._ac_sel = min(len(self._ac_items) - 1, self._ac_sel + 1)
                self._ac_update_list()
                return "break"
            if keysym == "Up":
                self._ac_sel = max(0, self._ac_sel - 1)
                self._ac_update_list()
                return "break"
            if keysym in ("Return", "KP_Enter", "Tab"):
                self._ac_commit()
                return "break"
            if keysym == "Escape":
                self._close_ac()
                return "break"
            return None
        if keysym in ("Return", "KP_Enter"):
            return self._smart_newline()
        return None

    def _ac_commit(self):
        if self._ac_popup is None or not self._ac_items:
            self._close_ac()
            return
        word = self._ac_items[self._ac_sel][0]
        self._insert_ac(word)

    def _insert_ac(self, word):
        prefix = self._prefix()
        if prefix:
            try:
                self.text.delete(f"insert-{len(prefix)}c", "insert")
            except tk.TclError:
                pass
        self.text.insert("insert", word)
        self._close_ac()
        self._apply_highlight()

    def _close_ac(self):
        if self._ac_popup:
            try:
                self._ac_popup.destroy()
            except tk.TclError:
                pass
            self._ac_popup = None
            self._ac_listbox = None
            self._ac_items = []


def os_abspath(p):
    import os
    return os.path.abspath(p)
