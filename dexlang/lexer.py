"""词法分析器:源码 → Token 流。"""

from .errors import DexError
from .tokens import Token, TokKind, KEYWORDS, DOUBLE_OPERATORS


class Lexer:
    def __init__(self, source, filename="<source>"):
        if source.startswith("\ufeff"):  # 兼容带 BOM 的 UTF-8 文件
            source = source[1:]
        self.src = source
        self.filename = filename
        self.pos = 0
        self.line = 1
        self.col = 1

    # ---------- 基础工具 ----------
    def _peek(self, n=0):
        i = self.pos + n
        return self.src[i] if i < len(self.src) else ""

    def _advance(self):
        ch = self.src[self.pos]
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def _error(self, msg, line=None, col=None):
        raise DexError(msg, line or self.line, col or self.col, "lexer")

    # ---------- 各类字面量 ----------
    def _skip_block_comment(self):
        start_line, start_col = self.line, self.col
        self._advance()  # '/'
        self._advance()  # '*'
        while True:
            if self.pos >= len(self.src):
                self._error("unterminated block comment", start_line, start_col)
            if self._peek() == "*" and self._peek(1) == "/":
                self._advance()
                self._advance()
                return
            self._advance()

    def _read_number(self):
        start_line, start_col = self.line, self.col
        chars = []
        is_float = False
        # 十六进制:0x / 0X 前缀(GAL 颜色 0xRRGGBB 等)
        if self._peek() == "0" and self._peek(1) in ("x", "X"):
            self._advance()  # 0
            self._advance()  # x / X
            digits = []
            while self._peek() and (self._peek().isdigit() or self._peek().lower() in "abcdef"):
                digits.append(self._advance())
            text = "0x" + "".join(digits)
            try:
                return Token(TokKind.INT, text, start_line, start_col, int(text, 16))
            except ValueError:
                self._error(f"invalid hex literal '{text}'", start_line, start_col)
        while self._peek().isdigit():
            chars.append(self._advance())
        if self._peek() == "." and self._peek(1).isdigit():
            is_float = True
            chars.append(self._advance())
            while self._peek().isdigit():
                chars.append(self._advance())
        if self._peek() in ("e", "E"):
            nxt = self._peek(1)
            if nxt.isdigit() or (nxt in "+-" and self._peek(2).isdigit()):
                is_float = True
                chars.append(self._advance())  # e / E
                if self._peek() in "+-":
                    chars.append(self._advance())
                while self._peek().isdigit():
                    chars.append(self._advance())
        text = "".join(chars)
        try:
            if is_float:
                return Token(TokKind.FLOAT, text, start_line, start_col, float(text))
            return Token(TokKind.INT, text, start_line, start_col, int(text))
        except ValueError:
            self._error(f"invalid number literal '{text}'", start_line, start_col)

    _ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0"}

    def _read_string(self):
        start_line, start_col = self.line, self.col
        self._advance()  # 开引号
        chars = []
        while True:
            if self.pos >= len(self.src):
                self._error("unterminated string literal", start_line, start_col)
            ch = self._advance()
            if ch == '"':
                break
            if ch == "\\":
                if self.pos >= len(self.src):
                    self._error("unterminated string literal", start_line, start_col)
                esc = self._advance()
                chars.append(self._ESCAPES.get(esc, esc))
            else:
                chars.append(ch)
        return Token(TokKind.STRING, '"' + "".join(chars) + '"', start_line, start_col, "".join(chars))

    def _read_ident(self):
        start_line, start_col = self.line, self.col
        chars = []
        while self._peek().isalnum() or self._peek() == "_":
            chars.append(self._advance())
        text = "".join(chars)
        kind = KEYWORDS.get(text, TokKind.IDENT)
        return Token(kind, text, start_line, start_col)

    def _next_token(self):
        line, col = self.line, self.col
        ch = self._peek()
        if ch.isdigit():
            return self._read_number()
        if ch.isalpha() or ch == "_":
            return self._read_ident()
        if ch == '"':
            return self._read_string()

        self._advance()
        two = ch + self._peek()
        if two in DOUBLE_OPERATORS:
            self._advance()
            return Token(DOUBLE_OPERATORS[two], two, line, col)

        single = {
            "(": TokKind.LPAREN, ")": TokKind.RPAREN,
            "{": TokKind.LBRACE, "}": TokKind.RBRACE,
            ",": TokKind.COMMA, ";": TokKind.SEMI,
            ":": TokKind.COLON, ".": TokKind.DOT,
            "+": TokKind.PLUS, "-": TokKind.MINUS,
            "*": TokKind.STAR, "/": TokKind.SLASH,
            "%": TokKind.PERCENT,
            "=": TokKind.EQ, "<": TokKind.LT, ">": TokKind.GT,
            "!": TokKind.BANG,
        }
        kind = single.get(ch)
        if kind is None:
            self._error(f"unexpected character {ch!r}", line, col)
        return Token(kind, ch, line, col)

    # ---------- 主入口 ----------
    def tokenize(self):
        tokens = []
        while self.pos < len(self.src):
            ch = self._peek()
            if ch in " \t\r\n":
                self._advance()
                continue
            if ch == "#":
                while self._peek() and self._peek() != "\n":
                    self._advance()
                continue
            if ch == "/" and self._peek(1) == "/":
                while self._peek() and self._peek() != "\n":
                    self._advance()
                continue
            if ch == "/" and self._peek(1) == "*":
                self._skip_block_comment()
                continue
            tokens.append(self._next_token())
        tokens.append(Token(TokKind.EOF, "", self.line, self.col))
        return tokens
