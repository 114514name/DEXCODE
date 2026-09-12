"""DEXIDE 语法辅助:注释/字符串掩码、缩进计算、括号配对。

为什么需要本模块:
    词法器(`dexlang.lexer`)是**面向编译**的 —— 它跳过空白、不产出 COMMENT
    token、也不报告注释与字符串的字符区间。于是编辑器此前只能靠文本启发式
    做两件事,两件都不完整:

      * 高亮:`print`/`include`/`refer`/`extern`/`type` 在词法器里是独立 token
        而非 IDENT,旧代码只判断 IDENT,它们便落到最后一行被判成"运算符";
        注释更是完全没有被着色。
      * 缩进:旧代码只看"上一行 rstrip 是否以 `{` 结尾",于是注释里的
        `// 说明 {` 会多缩一层,`}` 也不会自动回退。

    本模块在**纯文本层面**扫描一遍,给出注释区间、字符串区间与括号深度。
    它不替代词法器(高亮仍以 token 为准),而是补上词法器有意省略的信息。
    无第三方依赖,纯函数便于测试。
"""

from dataclasses import dataclass
from typing import List, Optional, Tuple

INDENT_UNIT = 4          # 一级缩进的空格数(与 Text 的 tabs 设置一致)
_OPENERS = "([{"
_CLOSERS = ")]}"


@dataclass
class Span:
    """源码中一段字符区间(1 基行,0 基列)。"""
    line: int
    col: int
    end_line: int
    end_col: int

    def is_empty(self) -> bool:
        return self.end_line < self.line or (
            self.end_line == self.line and self.end_col <= self.col)


def scan(source: str) -> Tuple[List[Span], List[Span]]:
    """扫描源码,返回 (注释区间, 字符串区间)。

    逐字符状态机,认得:
      - `//` 行注释、`#` 行注释(至行尾)
      - `/* ... */` 块注释(可跨行)
      - `"..."` 字符串(支持 \\ 转义;不跨行)
    未闭合的块注释/字符串延伸到文件末尾,便于边打边着色。
    """
    comments: List[Span] = []
    strings: List[Span] = []
    lines = source.split("\n")
    in_block = False
    block_start: Optional[Tuple[int, int]] = None

    for li, text in enumerate(lines, 1):
        n = len(text)
        i = 0
        if in_block:
            end = text.find("*/")
            if end < 0:
                continue
            comments.append(Span(block_start[0], block_start[1], li, end + 2))
            in_block = False
            i = end + 2
        # 用 str.find(纯 C 实现)定位下一个待处理字符,避免逐字符在 Python 层循环 ——
        # 该函数在每次光标移动/输入时都会跑,大文件下逐字符循环会成为主要开销。
        while i < n:
            cand = []
            for ch in ('"', "#", "/"):
                p = text.find(ch, i)
                if p >= 0:
                    cand.append(p)
            if not cand:
                break
            i = min(cand)
            ch = text[i]
            # 行注释:两种写法
            if ch == "#" or (ch == "/" and i + 1 < n and text[i + 1] == "/"):
                comments.append(Span(li, i, li, n))
                break
            if ch == "/" and i + 1 < n and text[i + 1] == "*":
                end = text.find("*/", i + 2)
                if end < 0:
                    in_block = True
                    block_start = (li, i)
                    break
                comments.append(Span(li, i, li, end + 2))
                i = end + 2
                continue
            if ch == "/":
                # 单独一个 / 是运算符,继续往后找
                i += 1
                continue
            # 字符串:用 find 找收尾(先处理转义)
            j = text.find('"', i + 1)
            if j >= 0:
                # 回退检查反斜杠转义:若结尾前有奇数个反斜杠则不是真正的收尾
                k = j - 1
                bs = 0
                while k > i and text[k] == "\\":
                    bs += 1
                    k -= 1
                if bs % 2 == 1:
                    j = text.find('"', j + 1)
                    while j >= 0:
                        k = j - 1
                        bs = 0
                        while k > i and text[k] == "\\":
                            bs += 1
                            k -= 1
                        if bs % 2 == 0:
                            break
                        j = text.find('"', j + 1)
            # 未闭合则延伸到行尾(不跨行)
            end_col = (j + 1) if j >= 0 else n
            strings.append(Span(li, i, li, end_col))
            i = end_col

    if in_block and block_start is not None:
        last = len(lines)
        comments.append(Span(block_start[0], block_start[1], last, len(lines[-1]) if lines else 0))
    return comments, strings


def span_contains_any(spans: List[Span], line: int, col: int) -> bool:
    """(line, col) 是否落在任一区间内(列按左闭右开)。"""
    for sp in spans:
        if sp.line <= line <= sp.end_line:
            if line == sp.line and col < sp.col:
                continue
            if line == sp.end_line and col >= sp.end_col:
                continue
            return True
    return False


def comment_spans_by_line(comments: List[Span]) -> dict:
    """把注释区间按行展开 → {行号: [(起列, 止列), ...]},便于整段着色。"""
    out: dict = {}
    for sp in comments:
        for ln in range(sp.line, sp.end_line + 1):
            c0 = sp.col if ln == sp.line else 0
            c1 = sp.end_col if ln == sp.end_line else 10 ** 9
            out.setdefault(ln, []).append((c0, c1))
    return out


def code_lines(source: str, comments: List[Span], strings: List[Span]) -> List[str]:
    """把注释与字符串内容替换为等长占位,得到"只剩代码"的行。

    缩进判断必须基于它 —— 否则注释/字符串里的 `{` `}` 会污染括号深度。
    等长替换保证列号不变。
    """
    lines = source.split("\n")
    masked = list(lines)
    for sp in list(comments) + list(strings):
        for ln in range(sp.line, sp.end_line + 1):
            if ln - 1 >= len(masked):
                break
            cur = masked[ln - 1]
            c0 = sp.col if ln == sp.line else 0
            c1 = sp.end_col if ln == sp.end_line else len(cur)
            c0 = max(0, min(c0, len(cur)))
            c1 = max(c0, min(c1, len(cur)))
            # 行注释可能延伸到行尾,补足等长
            fill = " " * (c1 - c0)
            masked[ln - 1] = cur[:c0] + fill + cur[c1:]
    return masked


def brace_deltas(code: str) -> Tuple[int, int]:
    """一行的括号效应:返回 (本行闭合数, 本行开启数)。

    只统计同一行内成对的与剩余的括号;对 `{}` 同现的行(如 `} else {`),
    两者都计,由调用方据此决定缩进。
    """
    closes = 0
    opens = 0
    for ch in code:
        if ch == "{":
            opens += 1
        elif ch == "}":
            closes += 1
    return closes, opens


def indent_for_line(code_lines_list: List[str], line: int) -> int:
    """返回第 line 行(1 基)应有的缩进空格数。

    规则与 VS Code 一致:
      - 该行以 `}` 开头(前导空白后第一个非空字符)时,先退一级;
      - 缩进 = 前面所有行的开启括号累计深度 × 单位缩进。
    """
    idx = line - 1
    if idx < 0 or idx >= len(code_lines_list):
        return 0
    depth = 0
    for prev in code_lines_list[:idx]:
        closes, opens = brace_deltas(prev)
        depth += opens - closes
        if depth < 0:
            depth = 0
    cur = code_lines_list[idx].lstrip()
    if cur.startswith("}"):
        depth -= 1
    return max(0, depth) * INDENT_UNIT


def leading_width(text: str) -> int:
    """前导空白宽度,制表符按 INDENT_UNIT 计。"""
    w = 0
    for ch in text:
        if ch == " ":
            w += 1
        elif ch == "\t":
            w += INDENT_UNIT
        elif ch == "\r":
            continue
        else:
            break
    return w


def is_blank(line_text: str) -> bool:
    return line_text.strip() == ""


# ---------- 括号/引号配对(自动闭合与跳过) ----------

PAIRS = {"(": ")", "[": "]", "{": "}"}
CLOSERS = {v: k for k, v in PAIRS.items()}
QUOTES = ('"', "'")


def matching_closer(ch: str) -> Optional[str]:
    return PAIRS.get(ch)


def is_closer(ch: str) -> bool:
    return ch in CLOSERS


def inside_span(comments: List[Span], strings: List[Span], line: int, col: int,
                include_strings: bool = True) -> bool:
    """该位置是否处于注释(或字符串)之中 —— 自动闭合与智能换行都要避开。"""
    if span_contains_any(comments, line, col):
        return True
    if include_strings and span_contains_any(strings, line, col):
        return True
    return False
