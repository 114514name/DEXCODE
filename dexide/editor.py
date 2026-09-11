"""DEXIDE 代码编辑器:行号 + 断点槽 + 语法着色 + 悬停提示 + 自动补全。"""

import re
import tkinter as tk

from dexlang import Lexer, DexError
from dexlang import opcodes as O

from .theme import C, HL, TAG_FOR_KEYWORD
from . import syntax as S

# token 种类名 → 高亮标签。
# 词法器为每个关键字给出独立种类(不是 IDENT),所以必须在这里逐个归类 ——
# 旧实现只判断 IDENT,导致 print/include/refer/extern/type 全部落到
# `return "operator"` 兜底分支而被当作运算符着色。
_KIND_TAG = {
    # 声明与流程关键字
    "LET": "decl", "FUNC": "decl", "TYPE": "decl",
    "IF": "keyword", "ELSE": "keyword", "WHILE": "keyword",
    "RETURN": "keyword", "TRUE": "keyword", "FALSE": "keyword",
    # 库引入与原生声明
    "INCLUDE": "builtin", "REFER": "builtin",
    "EXTERN": "extern", "RELEASE": "extern",
    "PRINT": "builtin",
    # 字面量与标识符
    "INT": "number", "FLOAT": "number", "STRING": "string",
    # 运算符
    "PLUS": "operator", "MINUS": "operator", "STAR": "operator",
    "SLASH": "operator", "PERCENT": "operator",
    "EQ": "operator", "EQEQ": "operator", "NE": "operator",
    "LT": "operator", "LE": "operator", "GT": "operator", "GE": "operator",
    "AND": "operator", "OR": "operator", "BANG": "operator",
    "ARROW": "operator",
    # 标点单独一类,视觉上弱于运算符
    "SEMI": "delim", "COMMA": "delim", "LPAREN": "delim", "RPAREN": "delim",
    "LBRACE": "delim", "RBRACE": "delim", "COLON": "delim", "DOT": "operator",
}

# 类型名(参数/字段/返回类型标注)
_TYPE_WORDS = frozenset({"int", "float", "string", "void"})

# 文本形式的关键字集合(用于悬停/补全等按词判断的场合;
# 语法着色本身按 token 种类进行,不依赖这个集合)
_KEYWORDS_TEXT = frozenset({
    "let", "if", "else", "while", "func", "return", "print",
    "true", "false", "include", "refer", "extern", "type",
})


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
        self.text.bind("<KeyPress>", self._on_key_press)
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
        self.text.bind("<Shift-Tab>", self._ac_navigate)
        self.text.bind("<ISO_Left_Tab>", self._ac_navigate)
        self.text.bind("<Control-slash>", self._ac_navigate)
        self.text.bind("<Control-Key-slash>", self._ac_navigate)
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

    # 可自动配对的字符
    _AUTO_PAIRS = {"(": ")", "[": "]", "{": "}"}
    _AUTO_QUOTES = ('"', "'")

    def _on_key_press(self, ev):
        """KeyPress:处理自动配对与"跳过已存在的闭括号"。

        注意只在 KeyPress 里做这些,KeyRelease 仍保留原有逻辑
        (自动补全与光标回调),两者职责不重叠。
        """
        if ev.state & 0x4:      # Ctrl 组合交给其它绑定
            return None
        # 回车/退格等交给 KeyRelease 与 _ac_navigate 处理。
        # 必须显式拦截:否则 Tk 会先插入一个默认换行,紧接着 _ac_navigate
        # 的智能换行又插一个,结果多出一个空行。
        if ev.keysym in ("Return", "KP_Enter"):
            return "break"
        ch = ev.char or ""
        if not ch:
            return None
        try:
            idx = self.text.index("insert")
            line, col = (int(x) for x in idx.split("."))
            # 有选区时,输入配对字符应把选区包裹起来
            if self.text.tag_ranges("sel"):
                if ch in self._AUTO_PAIRS or ch in self._AUTO_QUOTES:
                    return self._wrap_selection(ch)
                return None

            comments, strings = S.scan(self.get_text())
            # 在注释里:不配对(注释里写括号很常见)
            if S.span_contains_any(comments, line, col + 1):
                return None

            closer = self._AUTO_PAIRS.get(ch)
            if closer:
                self.text.insert("insert", ch + closer)
                self.text.mark_set("insert", f"insert - 1c")
                return "break"

            if ch in self._AUTO_QUOTES:
                # 字符串内再打引号通常是收尾,跳过已存在的
                nxt = self._char_at(line, col + 1)
                if nxt == ch:
                    self.text.mark_set("insert", f"insert + 1c")
                    return "break"
                if S.span_contains_any(strings, line, col + 1):
                    prev = self._char_at(line, col)
                    if prev != "\\":        # 转义引号不配对
                        return None
                self.text.insert("insert", ch + ch)
                self.text.mark_set("insert", f"insert - 1c")
                return "break"

            if ch in ")]}":
                # 紧邻相同闭括号时直接越过,避免出现 )))
                if self._char_at(line, col + 1) == ch:
                    self.text.mark_set("insert", f"insert + 1c")
                    return "break"
                return None
        except tk.TclError:
            return None
        return None

    def _wrap_selection(self, ch):
        """用配对字符包裹当前选区(与 VS Code 一致)。"""
        try:
            s = self.text.index("sel.first")
            e = self.text.index("sel.last")
            closer = self._AUTO_PAIRS.get(ch, ch)
            body = self.text.get(s, e)
            self.text.delete(s, e)
            self.text.insert(s, ch + body + closer)
            self.text.tag_remove("sel", "1.0", "end")
            return "break"
        except tk.TclError:
            return None

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

    # ---------- 缩进(语法感知) ----------
    def _code_source(self):
        """返回 (原文, 掩码后的代码行, 注释区间, 字符串区间)。

        掩码把注释与字符串内容换成等长空格 —— 缩进判断必须基于它,
        否则 `// 说明 {` 里的花括号会被当成真的块开始。
        """
        src = self.get_text()
        comments, strings = S.scan(src)
        return src, S.code_lines(src, comments, strings), comments, strings

    def _indent_at(self, code_lines_list, line):
        """第 line 行(1 基)应有的缩进空格数。"""
        if 1 <= line <= len(code_lines_list):
            return S.indent_for_line(code_lines_list, line)
        return 0

    def _smart_newline(self):
        """回车:按**括号深度**给出新行缩进(而非只看上一行是否以 { 结尾)。

        处理三种情形:
          1) 在行中间回车 —— 切分,后半段继承前半段的缩进;
             若前半段以开括号结尾,后半段再加一级;若后半段以闭括号开头,减一级。
          2) 在行尾回车 —— 新行缩进 = 当前行缩进 (行尾 `{` ? +1 : 0)。
          3) 光标前只有空白 —— 与情形 2 相同,但基于该行的代码内容判断。
        """
        try:
            idx = self.text.index("insert")
            line, col = (int(x) for x in idx.split("."))
            src, code, comments, strings = self._code_source()
            cur_text = self.text.get(f"{line}.0", f"{line}.0 lineend")

            # 光标之前是否只有空白?是则"整行重排",否则按切分处理
            before = cur_text[:col]
            at_line_end = (col >= len(cur_text))
            only_ws_before = before.strip() == ""

            if at_line_end or only_ws_before:
                # 情形 2/3:整行重排 —— 由该行(掩码后)的括号深度决定新行缩进
                base = self._indent_at(code, line)
                code_line = code[line - 1] if line - 1 < len(code) else ""
                # 该行以开括号结尾 → 新行再进一级。
                # 注意用"末字符属于集合"判断:str.endswith 的参数是**后缀串**,
                # 写成 endswith("([{") 是在找整个 "([{" 结尾,永远为假。
                stripped = code_line.rstrip()
                if stripped and stripped[-1] in S._OPENERS:
                    base += S.INDENT_UNIT
                self.text.insert("insert", "\n" + " " * base)
            else:
                # 情形 1:切分 —— 后半段按自己的代码内容重新缩进,
                # 因此要先删掉它原有的前导空白,避免新旧缩进叠加。
                tail = cur_text[col:]
                tail_ws = len(tail) - len(tail.lstrip())
                if tail_ws:
                    self.text.delete("insert", f"insert + {tail_ws}c")
                new_indent = self._indent_at(code, line)
                self.text.insert("insert", "\n" + " " * new_indent)
            self.text.tag_remove("sel", "1.0", "end")
            return "break"
        except tk.TclError:
            return "break"

    # ---------- 选中行缩进 / 注释切换 ----------
    def _selected_line_range(self):
        """返回受影响的 (首行, 末行)。无选区时为光标所在行。"""
        try:
            if self.text.tag_ranges("sel"):
                s = self.text.index("sel.first")
                e = self.text.index("sel.last")
                l1 = int(s.split(".")[0])
                l2 = int(e.split(".")[0])
                # 选区结束正好在行首时不把该行算进来
                if e.split(".")[1] == "0" and l2 > l1:
                    l2 -= 1
                return l1, l2
            ln = self._cursor_line()
            return ln, ln
        except tk.TclError:
            ln = self._cursor_line()
            return ln, ln

    def indent_lines(self, dedent=False):
        """对选中行(或当前行)整体缩进/反缩进。"""
        l1, l2 = self._selected_line_range()
        unit = S.INDENT_UNIT
        self.text.edit_separator()
        # 从下往上改,避免行号变动
        for ln in range(l2, l1 - 1, -1):
            text = self.text.get(f"{ln}.0", f"{ln}.0 lineend")
            if dedent:
                if text.startswith("\t"):
                    self.text.delete(f"{ln}.0", f"{ln}.1")
                else:
                    strip = 0
                    for ch in text:
                        if ch == " " and strip < unit:
                            strip += 1
                        else:
                            break
                    if strip:
                        self.text.delete(f"{ln}.0", f"{ln}.{strip}")
            else:
                if text.strip() == "":
                    continue
                self.text.insert(f"{ln}.0", " " * unit)
        self.text.edit_separator()
        return "break"

    def toggle_comment(self):
        """Ctrl+/ :注释或取消注释选中行(或当前行)。"""
        l1, l2 = self._selected_line_range()
        lines = []
        for ln in range(l1, l2 + 1):
            lines.append(self.text.get(f"{ln}.0", f"{ln}.0 lineend"))
        # 以"非空行是否都已被注释"决定是加还是去
        meaningful = [t for t in lines if t.strip()]
        all_commented = bool(meaningful) and all(t.lstrip().startswith("//") for t in meaningful)
        self.text.edit_separator()
        for ln in range(l2, l1 - 1, -1):
            text = self.text.get(f"{ln}.0", f"{ln}.0 lineend")
            if text.strip() == "":
                continue
            stripped = text.lstrip()
            pad = len(text) - len(stripped)
            if all_commented:
                if stripped.startswith("// "):
                    self.text.delete(f"{ln}.{pad}", f"{ln}.{pad + 3}")
                elif stripped.startswith("//"):
                    self.text.delete(f"{ln}.{pad}", f"{ln}.{pad + 2}")
            else:
                self.text.insert(f"{ln}.{pad}", "// ")
        self.text.edit_separator()
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
        # 直接按主题里定义的标签清理,避免手工维护的列表漏项
        for tag in list(HL.keys()) + ["err", "warn"]:
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
            self._apply_problem_marks()
            self._highlight_debug_line()
            return

        # 注释与字符串在词法器里被跳过/无 COMMENT token,故先按文本扫描着色;
        # 词法错误(边打边写时很常见)不影响这一步,注释与字符串仍能稳定着色。
        comments, strings = S.scan(src)
        self._apply_span_tags(comments, "comment")
        self._apply_span_tags(strings, "string")

        # 其余部分以真实 token 为准(语义着色:函数调用、原生函数、变量、类型)
        try:
            tokens = Lexer(src, self.path).tokenize()
        except DexError:
            tokens = []
        prev = None
        for tok in tokens:
            tag = self._token_tag(tok, prev)
            if tag:
                self._apply_tag(tok.line, tok.col, len(tok.lexeme), tag)
            prev = tok

        self._apply_problem_marks()
        self._highlight_debug_line()

    def _apply_span_tags(self, spans, tag):
        """把若干区间整段着色(用于注释与字符串,支持跨行)。"""
        # syntax.scan 给的是 0 基列;这里 +1 转成 1 基,与 token 坐标统一
        by_line = S.comment_spans_by_line(spans)
        for line, ranges in by_line.items():
            for c0, c1 in ranges:
                try:
                    if c1 >= 10 ** 9:
                        self.text.tag_add(tag, self._idx(line, c0 + 1), f"{line}.end")
                    else:
                        self.text.tag_add(tag, self._idx(line, c0 + 1),
                                          self._idx(line, c1 + 1))
                except tk.TclError:
                    pass



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
        """汇编文本着色。

        注意:本函数内部全部使用**1 基列**(与词法器 token 坐标一致),
        因此正则给出的 0 基偏移都要 +1;`_apply_tag_abs` 也按 1 基解释。
        """
        mnems = O.MNEMONIC_TO_OP  # 助记符名 → 操作码
        lines = src.split("\n")
        for i, line in enumerate(lines, 1):
            work = line
            # 注释 #:整段到行尾(用 _idx 构造索引,避免 "L.0 + Nc" 失效)
            hp = work.find("#")
            if hp >= 0:
                try:
                    self.text.tag_add("comment", self._idx(i, hp + 1), f"{i}.0 lineend")
                except tk.TclError:
                    pass
                work = work[:hp]
            stripped = work.strip()
            if not stripped:
                continue
            # 标签 NAME:
            m = re.match(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*):\s*$", work)
            if m:
                col = len(m.group(1)) + 1
                name = m.group(2)
                self._apply_tag_abs(i, col, len(name), "label")
                continue
            # 指令 / 指令头
            m = re.match(r"^(\s*)([A-Za-z.][\w]*)(.*)$", work)
            if not m:
                continue
            col = len(m.group(1)) + 1
            head = m.group(2)
            rest = m.group(3)
            if head in (".func", ".lib", ".native"):
                self._apply_tag_abs(i, col, len(head), "keyword")
                # .lib 的路径字符串
                sm = re.search(r'"([^"]*)"', rest)
                if sm:
                    # sm.start() 指向开引号;内容从 +1 开始
                    c2 = col + len(head) + sm.start() + 1
                    self._apply_tag_abs(i, c2, len(sm.group(1)), "library")
            elif head == "NCALL" or head in mnems:
                self._apply_tag_abs(i, col, len(head), "keyword")
            # 操作数:字符串 / @函数 / %局部 / 数字
            base = col + len(head)
            # 字符串字面量:只给内容着色(不含引号)
            for m2 in re.finditer(r'"([^"]*)"', rest):
                self._apply_tag_abs(i, base + m2.start() + 1, len(m2.group(1)), "string")
            # @函数 / %局部 / 数字:整体着色
            for m2 in re.finditer(r"@[A-Za-z_]\w*", rest):
                self._apply_tag_abs(i, base + m2.start(), len(m2.group(0)), "func")
            for m2 in re.finditer(r"%\d+", rest):
                self._apply_tag_abs(i, base + m2.start(), len(m2.group(0)), "number")
            for m2 in re.finditer(r"\b\d+(\.\d+)?\b", rest):
                self._apply_tag_abs(i, base + m2.start(), len(m2.group(0)), "number")

    # 列号一律沿用**词法器的 1 基列**:
    #   - 词法器的 col 是 1 基(type 在 col=1,对应文本首字符);
    #   - Tk 的 "L.1" 恰好也是该行第一个字符。
    # 注意不要用 "L.0 + Nc" 形式 —— Tk 只解析一次 "+",
    # 带运算的索引不能再次拼接相对偏移(会静默退化为位置 0)。
    @staticmethod
    def _idx(line, col):
        # 1 基列 → Tk 索引:"1.1" 指该行**第 2** 个字符,故首字符是 "1.0"。
        return f"{line}.{max(0, col - 1)}"

    def _char_at(self, line, col):
        """取 1 基列 col 处的字符。"""
        try:
            return self.text.get(self._idx(line, col), self._idx(line, col + 1))
        except tk.TclError:
            return ""

    def _apply_tag_abs(self, line, col, length, tag):
        """按 (行, 1 基列) 应用标签。"""
        if length <= 0:
            return
        try:
            self.text.tag_add(tag, self._idx(line, col), self._idx(line, col + length))
        except tk.TclError:
            pass

    def _token_tag(self, tok, prev=None):
        """token → 高亮标签。

        `prev` 是前一个 token:用它区分「声明出来的名字」与「引用」——
        `func foo(...)` 里的 foo 是声明(着色为函数),而 `foo()` 里的 foo 是调用。
        """
        name = tok.kind.name
        if name == "EOF":
            return None

        # 1) 由 token 种类直接决定的标签(覆盖全部关键字种类)
        tag = _KIND_TAG.get(name)
        if tag is not None:
            # 声明关键字后面紧跟的标识符要特殊处理,见下
            if name != "IDENT":
                return tag

        # 2) 标识符需要语义判断
        if name == "IDENT":
            w = tok.lexeme
            prev_name = prev.kind.name if prev is not None else None

            # 紧随 func / type / extern func 的名字 → 声明
            if prev_name == "FUNC":
                return "func"
            if prev_name == "TYPE":
                return "decl"
            # 形参名:`func f(a: int, b: string)` —— 前一个 token 是 LPAREN 或 COMMA,
            # 且后面跟着 COLON
            if prev_name in ("LPAREN", "COMMA") and self._next_char(tok) == ":":
                return "param"

            if w in _TYPE_WORDS:
                return "type"
            if w in _KEYWORDS_TEXT:
                return TAG_FOR_KEYWORD(w) or "keyword"

            if self._next_char(tok) == "(":
                # 调用:原生函数优先(名字可能重名,原生更具体)
                if self.analyzer and w in self.analyzer.native_names():
                    return "native"
                if self.analyzer and w in self.analyzer.all_func_names():
                    return "func"
                return "func"
            if self._is_var(w):
                return "variable"
            return None
        return None

    def _next_char(self, tok):
        """取 token 结束位置的下一个字符(即时读文本,无需重新分析)。"""
        return self._char_at(tok.line, tok.col + len(tok.lexeme))

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
        """按 (行, 1 基列) 应用标签 —— 与词法器 token 的坐标一致。"""
        self._apply_tag_abs(line, col, length, tag)

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
        if keysym == "Tab":
            return self.indent_lines(dedent=False)
        if keysym in ("Shift-Tab", "ISO_Left_Tab"):
            return self.indent_lines(dedent=True)
        if keysym == "slash" and (ev.state & 0x4):
            return self.toggle_comment()
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
