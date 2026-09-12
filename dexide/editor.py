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

# 括号(用于配对高亮的快速定位)
_BRACKET_RE = re.compile(r"[()\[\]{}]")

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
        self._bracket_tagged = False   # 当前是否已打上括号配对标签
        self._scan_cache = None        # (文本, (注释, 字符串)) 缓存,见 _scan_cached
        self._fold_regions = {}        # {首行: (末行, 折叠行数)}
        self._folded = set()           # 当前处于折叠状态的首行
        self.fold_enabled = True       # 可在「视图」菜单关闭
        self._bracket_marks = []       # 上次打标签的位置 [(起索引, 止索引), ...]
                                       # 只清这几个位置:Text.tag_remove 遍历整个
                                       # 文档,对大文件是主要开销(实测 800 行约 5 ms)

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

        # 查找/替换面板(默认隐藏,贴在底部)
        self._find_panel = tk.Frame(self, bg=C["mantle"])
        self._build_find_panel()

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
        # 查找高亮:全部匹配 + 当前匹配(与 VS Code 一致的两级区分)
        self.text.tag_configure("find_all", background=C["surface1"])
        self.text.tag_configure("find_cur", background=C["peach"], foreground=C["crust"])
        # 括号配对:用背景色区分,前景保持可读
        self.text.tag_configure("bracket_match",
                                background=C["surface2"], foreground=HL["bracket_match"])
        self.text.tag_configure("bracket_unmatched",
                                background=C["surface1"], underline=True,
                                foreground=HL["bracket_unmatched"])
        self.bp.tag_configure("bp", foreground=C["red"])
        self.bp.tag_configure("bpcur", foreground=C["green"])
        self.bp.tag_configure("fold", foreground=C["overlay0"])
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
        # 查找 / 替换
        self.text.bind("<Control-f>", lambda e: self.show_find(False))
        self.text.bind("<Control-h>", lambda e: self.show_find(True))
        self.text.bind("<F3>", lambda e: self.find_next())
        # 折叠:Ctrl+Shift+[ 折叠 / Ctrl+Shift+] 展开 / Ctrl+K 后 Ctrl+0 全部展开
        self.text.bind("<Control-Shift-bracketleft>", lambda e: self.toggle_fold())
        self.text.bind("<Control-Shift-bracketright>", lambda e: self.unfold_all())
        self.text.bind("<Shift-F3>", lambda e: self.find_prev())
        # 面板打开时,编辑器里的 F3 仍可用
        self.bind_all("<F3>", self._global_find_next, add="+")
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
            # 该行有折叠标记时:点击折叠/展开(与 VS Code 折叠槽一致)
            self.compute_folds()
            if line in self._fold_regions and self.fold_enabled:
                self.toggle_fold(line)
            else:
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
        # 折叠标记叠加在断点列右侧(与 VS Code 的折叠槽类似)
        for ln, mark in (self._fold_marks() if self.fold_enabled else {}).items():
            try:
                self.bp.insert(f"{ln}.end", " " + mark, "fold")
            except tk.TclError:
                pass
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
            self._scan_cache = None      # 文本已变,区间缓存失效
            self._schedule_highlight()
            # 查找面板可见时,匹配位置也要跟着更新
            if self._find_panel.winfo_ismapped():
                self._refresh_find_marks()
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
        # 但方向键/Home/End 会移动光标,括号配对高亮要跟着更新。
        if ev.keysym in self._AC_KEYS:
            if ev.keysym in ("Left", "Right", "Up", "Down", "Home", "End",
                             "Prior", "Next"):
                self.highlight_brackets()
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
        src, comments, strings = self._scan_cached()
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
        self.highlight_brackets()

    # ---------- 高亮 ----------
    def _schedule_highlight(self):
        if self._hl_after:
            self.after_cancel(self._hl_after)
        self._hl_after = self.after(250, self._apply_highlight)

    def _clear_hl_tags(self):
        self._bracket_marks = []
        self._bracket_tagged = False
        # 直接按主题里定义的标签清理,避免手工维护的列表漏项
        for tag in list(HL.keys()) + ["err", "warn",
                                      "bracket_match", "bracket_unmatched"]:
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

    def _global_find_next(self, _ev=None):
        """F3 全局可用(面板未打开时也能"查找下一个")。"""
        if self._find_panel.winfo_ismapped() or self._find_var.get():
            self.find_next()

    # ---------- 查找 / 替换 ----------
    def _build_find_panel(self):
        """构造查找替换面板(默认隐藏)。

        与自动补全弹窗不同,查找框**需要**获得焦点(用户要在这里输入),
        因此打开时主动 focus;关闭时把焦点还给编辑器。
        """
        f = self._find_panel
        pad = dict(padx=6)
        row1 = tk.Frame(f, bg=C["mantle"])
        row1.pack(fill="x", **pad)
        tk.Label(row1, text="查找", bg=C["mantle"], fg=C["subtext1"],
                 font=(self.FONT_FAMILY, 10)).pack(side="left")
        self._find_var = tk.StringVar()
        self._find_entry = tk.Entry(row1, textvariable=self._find_var, width=26,
                                    bg=C["surface0"], fg=C["text"], relief="flat",
                                    insertbackground=C["text"],
                                    font=(self.FONT_FAMILY, 11))
        self._find_entry.pack(side="left", padx=6)
        self._find_count = tk.Label(row1, text="", bg=C["mantle"], fg=C["overlay0"],
                                    font=(self.FONT_FAMILY, 10), width=12, anchor="w")
        self._find_count.pack(side="left")
        for label, cmd in (("上一个", self.find_prev), ("下一个", self.find_next),
                           ("关闭", self.hide_find)):
            tk.Button(row1, text=label, command=cmd, bg=C["surface0"], fg=C["text"],
                      relief="flat", padx=8, font=(self.FONT_FAMILY, 10)
                      ).pack(side="left", padx=2)

        row2 = tk.Frame(f, bg=C["mantle"])
        row2.pack(fill="x", padx=6, pady=(2, 4))
        tk.Label(row2, text="替换", bg=C["mantle"], fg=C["subtext1"],
                 font=(self.FONT_FAMILY, 10)).pack(side="left")
        self._repl_var = tk.StringVar()
        self._repl_entry = tk.Entry(row2, textvariable=self._repl_var, width=26,
                                    bg=C["surface0"], fg=C["text"], relief="flat",
                                    insertbackground=C["text"],
                                    font=(self.FONT_FAMILY, 11))
        self._repl_entry.pack(side="left", padx=6)
        tk.Button(row2, text="替换", command=self.replace_current,
                  bg=C["surface0"], fg=C["text"], relief="flat", padx=8,
                  font=(self.FONT_FAMILY, 10)).pack(side="left", padx=2)
        tk.Button(row2, text="全部替换", command=self.replace_all,
                  bg=C["surface0"], fg=C["text"], relief="flat", padx=8,
                  font=(self.FONT_FAMILY, 10)).pack(side="left", padx=2)

        self._find_matches = []      # 匹配(文本偏移 起, 止)
        self._find_offsets = []      # 匹配的 Tk 索引
        self._find_idx = -1
        for w in (self._find_entry, self._repl_entry):
            w.bind("<Return>", lambda e: self.find_next())
            w.bind("<Shift-Return>", lambda e: self.find_prev())
            w.bind("<Escape>", lambda e: self.hide_find())
            w.bind("<KeyRelease>", lambda e: self._refresh_find_marks())

    def show_find(self, with_replace=False):
        self._find_panel.pack(side="bottom", fill="x")
        # 有选中的单行文本时带入搜索框(与常见编辑器一致)
        try:
            sel = self.text.get("sel.first", "sel.last") if self.text.tag_ranges("sel") else ""
        except tk.TclError:
            sel = ""
        if sel and "\n" not in sel:
            self._find_var.set(sel)
        self._refresh_find_marks()
        target = self._repl_entry if with_replace else self._find_entry
        target.focus_set()
        target.select_range(0, "end")
        return "break"

    def hide_find(self):
        self._find_panel.pack_forget()
        self.text.tag_remove("find_all", "1.0", "end")
        self.text.tag_remove("find_cur", "1.0", "end")
        self._find_matches = []
        self._find_offsets = []
        self._find_idx = -1
        self.text.focus_set()
        return "break"

    def _compute_find_matches(self):
        """找出所有匹配。大小写不敏感;支持跨行(纯文本匹配,无正则)。"""
        needle = self._find_var.get()
        self._find_matches = []
        self._find_offsets = []
        self._find_idx = -1
        if not needle:
            return
        src = self.get_text()
        hay, ndl = src.lower(), needle.lower()
        starts = [0]
        for i, ch in enumerate(src):
            if ch == "\n":
                starts.append(i + 1)
        import bisect
        pos = 0
        while True:
            p = hay.find(ndl, pos)
            if p < 0:
                break
            self._find_matches.append((p, p + len(needle)))
            li = bisect.bisect_right(starts, p) - 1
            lj = bisect.bisect_right(starts, p + len(needle)) - 1
            self._find_offsets.append(
                (f"{li + 1}.{p - starts[li]}",
                 f"{lj + 1}.{p + len(needle) - starts[lj]}"))
            pos = p + max(1, len(ndl))

    def _refresh_find_marks(self):
        self.text.tag_remove("find_all", "1.0", "end")
        self.text.tag_remove("find_cur", "1.0", "end")
        self._compute_find_matches()
        for a, b in self._find_offsets:
            try:
                self.text.tag_add("find_all", a, b)
            except tk.TclError:
                pass
        n = len(self._find_matches)
        self._find_count.configure(
            text=("%d 处" % n) if n else ("无匹配" if self._find_var.get() else ""))
        if n:
            # 保持"当前匹配"的选中状态(替换后会重算,索引取原位置或末尾)
            i = self._find_idx if self._find_idx >= 0 else 0
            self._select_match(min(i, n - 1), scroll=False)

    def _select_match(self, i, scroll=True):
        """选中第 i 个匹配(取模环绕),并同步 _find_idx。

        导航**只用 _find_idx**推进,不用光标位置判断 —— 选中时会把光标移到
        匹配开头,若再拿光标去比较就会原地绕圈(实测 find_next 无法前进)。"""
        if not self._find_matches:
            return
        i %= len(self._find_matches)
        self._find_idx = i
        a, b = self._find_offsets[i]
        try:
            self.text.tag_remove("find_cur", "1.0", "end")
            self.text.tag_add("find_cur", a, b)
            if scroll:
                self.text.see(a)
            self.text.mark_set("insert", a)     # 光标落在匹配开头
        except tk.TclError:
            pass

    def find_next(self):
        if not self._find_matches:
            self._refresh_find_marks()
            if self._find_matches:
                self._select_match(0)
                return "break"
            return "break"
        self._select_match(self._find_idx + 1)
        return "break"

    def find_prev(self):
        if not self._find_matches:
            self._refresh_find_marks()
            if self._find_matches:
                self._select_match(len(self._find_matches) - 1)
                return "break"
            return "break"
        self._select_match(self._find_idx - 1)
        return "break"

    def _replace_range(self, a, b, repl):
        self.text.delete(a, b)
        if repl:
            self.text.insert(a, repl)

    def replace_current(self):
        """替换当前匹配(通过替换前用光标定位,避免索引错位)。"""
        if not self._find_matches:
            self._refresh_find_marks()
        if not self._find_matches:
            return "break"
        if self._find_idx < 0:
            self._select_match(0, scroll=False)
        a, b = self._find_offsets[self._find_idx]
        self._ignore_modify = True
        try:
            self._replace_range(a, b, self._repl_var.get())
        finally:
            self._ignore_modify = False
        self._scan_cache = None
        self._refresh_find_marks()
        # 替换后前进到下一个匹配(与常见编辑器一致)
        if self._find_matches:
            self._select_match(self._find_idx)
        return "break"

    def replace_all(self):
        """全部替换:按匹配位置从后往前替换,避免前面的替换影响后面的索引。"""
        needle = self._find_var.get()
        if not needle:
            return "break"
        self._refresh_find_marks()
        if not self._find_matches:
            return "break"
        repl = self._repl_var.get()
        n = len(self._find_matches)
        self._ignore_modify = True
        try:
            for a, b in reversed(self._find_offsets):
                self._replace_range(a, b, repl)
        finally:
            self._ignore_modify = False
        self._scan_cache = None
        self._refresh_find_marks()
        self._find_count.configure(text="已替换 %d 处" % n)
        return "break"

    # ---------- 代码折叠 ----------
    #
    # 实现方式:Tk Text 的 elide 属性可以真正"隐藏"一段文本(不占显示空间),
    # 折叠时把 [首行末尾+1 .. 末行末尾] 标记为 elide;并在首行末尾放一个提示标签,
    # 让用户知道这里折叠了多少行。
    def compute_folds(self):
        """扫描全文档,得出可折叠区域 {首行: (末行, 折叠行数)}。

        折叠块 = 一对配对的 { }。深度计数基于**掩码后**的代码,因此注释与字符串
        里的花括号不会产生假的折叠块(与缩进用的是同一套判断)。
        """
        self._fold_regions = {}
        try:
            src, code, _c, _s = self._code_source()
        except tk.TclError:
            return
        stack = []
        for li, line in enumerate(code, 1):
            for ch in line:
                if ch == "{":
                    stack.append(li)
                elif ch == "}":
                    if not stack:
                        continue
                    start = stack.pop()
                    if li > start:                    # 至少跨一行才有折叠意义
                        self._fold_regions[start] = (li, li - start)
        # 折叠状态里已不存在的区域要清掉
        for ln in list(self._folded):
            if ln not in self._fold_regions:
                self._folded.discard(ln)

    def toggle_fold(self, line=None):
        if line is None:
            line = self._cursor_line()
        # 光标在块内时,折叠光标所在的最内层块
        if line not in self._fold_regions:
            cands = [k for k in self._fold_regions if k <= line <= self._fold_regions[k][0]]
            if not cands:
                return "break"
            line = max(cands)
        if line in self._folded:
            self.unfold(line)
        else:
            self.fold(line)
        return "break"

    # 折叠标记文字的长度上限(用于展开时精确删除)
    _FOLD_MARKER_MAX = 16

    def fold(self, line):
        """折叠第 line 行所在的块:用 elide 隐藏 [首行末尾+1c .. 末行末尾),
        并在首行末尾插入 "⋯ N 行" 提示。

        实现要点(踩过的坑):
          - `text.tag_configure` 若放在 tag_add **之后**,配置不会作用于已打的区间,
            必须先把 elide 配置好再加标签;
          - 提示标签要在 elide 之后添加,否则它自己也会被隐藏;
          - 折叠是**显示层**的变化,不改变文本内容(所以 get_text 仍是全文)。"""
        end, removed = self._fold_regions.get(line, (None, 0))
        if end is None or line in self._folded:
            return "break"
        try:
            marker_idx = f"{line}.end"
            body_start = f"{marker_idx} + 1c"
            body_end = f"{end}.end"
            # 1) 先配置 elide,再加标签
            self.text.tag_configure("fold_hidden", elide=True)
            self.text.tag_add("fold_hidden", body_start, body_end)
            # 2) 提示文字(在 elide 区间之外,不会被隐藏)。
            #    用一对 mark 夹住它:删除时按 mark 取范围,避免索引算术 ——
            #    Tk 的 "line.end" 位置微妙(它指向换行符之前还是之后并不直观),
            #    用 "1.end + Nc" 这类相对偏移很容易跨到下一行而删不掉标记。
            # 标记文字用**按行的唯一标签**定位,展开时按该标签取范围删除。
            # 不能用全局 mark:多个块折叠时 mark 会被后来的折叠覆盖(实测展开
            # 第 3 行后再展开第 1 行,mark 已指向第 3 行,导致标记删不掉)。
            tag = "fold_mark_%d" % line
            self.text.insert(marker_idx, "⋯ %d 行" % removed,
                             ("fold_marker", tag))
            self._folded.add(line)
        except tk.TclError:
            pass
        self._sync_gutters()
        return "break"

    def unfold(self, line):
        if line not in self._folded:
            return "break"
        try:
            # 先恢复显示,再删提示文字(顺序反了会因区间被隐藏而删不掉)
            self.text.tag_remove("fold_hidden", "1.0", "end")
            tag = "fold_mark_%d" % line
            rs = self.text.tag_ranges(tag)
            if len(rs) >= 2 and self.text.get(rs[0], rs[1]).startswith("⋯"):
                self.text.delete(rs[0], rs[1])
            self.text.tag_remove(tag, "1.0", "end")
            self._folded.discard(line)
            # 其它仍折叠的块需要重新隐藏(上面做了全局 tag_remove)
            for other in sorted(self._folded):
                self._apply_fold_elide(other)
        except tk.TclError:
            pass
        self._sync_gutters()
        return "break"

    def _apply_fold_elide(self, line):
        end, _n = self._fold_regions.get(line, (None, 0))
        if end is None:
            return
        try:
            self.text.tag_configure("fold_hidden", elide=True)
            self.text.tag_add("fold_hidden", f"{line}.end + 1c", f"{end}.end")
        except tk.TclError:
            pass

    def unfold_all(self):
        for line in sorted(self._folded, reverse=True):
            self.unfold(line)
        self.text.tag_remove("fold_hidden", "1.0", "end")
        self._folded.clear()
        self._sync_gutters()
        return "break"

    def _fold_marks(self):
        """{行号: 标记字符},供边栏渲染使用。"""
        out = {}
        self.compute_folds()
        for ln in self._fold_regions:
            out[ln] = "▾" if ln in self._folded else "▸"
        return out

    def _sync_gutters(self):
        self._render_bp()
        self._sync_line_numbers()

    # ---------- 括号配对高亮 ----------
    _OPEN2CLOSE = {"(": ")", "[": "]", "{": "}"}

    def _scan_cached(self):
        """返回 (文本, 注释区间, 字符串区间),带一层缓存。

        scan 会遍历全文,而括号配对与缩进都要用它;同一份文本重复扫描没必要。
        注意:每次按键都会改文本,所以缓存以**文本内容**为键。
        """
        src = self.get_text()
        if self._scan_cache is not None and self._scan_cache[0] == src:
            spans = self._scan_cache[1]
            return src, spans[0], spans[1]
        comments, strings = S.scan(src)
        self._scan_cache = (src, (comments, strings))
        return src, comments, strings

    def _bracket_list(self):
        """把文本里所有括号摊平成 (字符, 行, 1基列, 是否在注释/字符串内)。

        括号配对必须在**代码**里做:注释或字符串中的括号不参与配对
        (否则 `// (说明` 会让后面的 ) 配错)。
        """
        src, comments, strings = self._scan_cached()
        # 区间按出现顺序排好,用双指针推进 —— 避免对每个括号都遍历全部区间
        spans = sorted(list(comments) + list(strings),
                       key=lambda s: (s.line, s.col))
        # 用正则只挑出括号(而非逐字符 if) —— 大文件下逐字符循环开销明显
        out = []
        si = 0
        for li, line in enumerate(src.split("\n"), 1):
            for m in _BRACKET_RE.finditer(line):
                col = m.start() + 1
                while si < len(spans) and (
                        spans[si].end_line < li or
                        (spans[si].end_line == li and spans[si].end_col <= col)):
                    si += 1
                in_lit = False
                if si < len(spans):
                    sp = spans[si]
                    if sp.line <= li <= sp.end_line:
                        start_ok = not (li == sp.line and col < sp.col)
                        end_ok = not (li == sp.end_line and col >= sp.end_col)
                        in_lit = start_ok and end_ok
                out.append((m.group(0), li, col, in_lit))
        return out

    @staticmethod
    def _find_bracket_at(chars, line, col):
        for i, c in enumerate(chars):
            if c[1] == line and c[2] == col:
                return i
        return None

    def _match_bracket(self, chars, pos):
        """在 chars[pos] 处找配对项,返回下标;找不到返回 None。

        用同类括号计数法(而非简单栈),因此跨层级也能正确配对。
        """
        ch, line, col, in_literal = chars[pos]
        if in_literal:
            return None
        closer = self._OPEN2CLOSE.get(ch)
        if closer:
            depth = 0
            for j in range(pos, len(chars)):
                c = chars[j][0]
                if chars[j][3]:
                    continue
                if c == ch:
                    depth += 1
                elif c == closer:
                    depth -= 1
                    if depth == 0:
                        return j
            return None
        # 闭括号:反向扫描
        opener = {v: k for k, v in self._OPEN2CLOSE.items()}.get(ch)
        if not opener:
            return None
        depth = 0
        for j in range(pos, -1, -1):
            c = chars[j][0]
            if chars[j][3]:
                continue
            if c == ch:
                depth += 1
            elif c == opener:
                depth -= 1
                if depth == 0:
                    return j
        return None

    def _clear_bracket_tags(self):
        """只清除上次打上的那几个位置(而不是全文档扫描)。"""
        if not self._bracket_marks:
            return
        for tag in ("bracket_match", "bracket_unmatched"):
            for a, b in self._bracket_marks:
                try:
                    self.text.tag_remove(tag, a, b)
                except tk.TclError:
                    pass
        self._bracket_marks = []
        self._bracket_tagged = False

    def highlight_brackets(self):
        """高亮光标处(或紧邻左侧)的括号及其配对项;找不到配对的标红。

        与 VS Code 行为一致:优先看光标右边的字符,没有则看左边。
        """
        try:
            # 先判断光标处(或左邻)是否真的是括号 —— 绝大多数按键都不是。
            # 这一步必须放在清理旧标签**之前**:Text.tag_remove 本身是 O(n),
            # 若无条件执行,每次按键都会有全文扫描的开销(实测 800 行时约 5 ms)。
            idx = self.text.index("insert")
            line, col0 = (int(x) for x in idx.split("."))
            cur = self._char_at(line, col0 + 1)                    # 光标右
            left = self._char_at(line, col0) if col0 > 0 else ""   # 光标左
            if cur in "()[]{}":
                target = (line, col0 + 1)
            elif left in "()[]{}":
                target = (line, col0)
            else:
                self._clear_bracket_tags()
                return

            self._clear_bracket_tags()

            chars = self._bracket_list()
            pos = self._find_bracket_at(chars, target[0], target[1])
            if pos is None:
                return
            # 注释与字符串里的括号不参与配对,也不应被标成"未匹配"
            # (否则 `// 说明 (行` 会一直显示红色下划线)
            if chars[pos][3]:
                return
            other = self._match_bracket(chars, pos)
            if other is None:
                # 未配对:标红提示(编辑中途很常见)
                a = self._idx(chars[pos][1], chars[pos][2])
                b = self._idx(chars[pos][1], chars[pos][2] + 1)
                self.text.tag_add("bracket_unmatched", a, b)
                self._bracket_marks = [(a, b)]
                self._bracket_tagged = True
                return
            for p in (pos, other):
                a = self._idx(chars[p][1], chars[p][2])
                b = self._idx(chars[p][1], chars[p][2] + 1)
                self.text.tag_add("bracket_match", a, b)
                self._bracket_marks.append((a, b))
            self._bracket_tagged = True
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
