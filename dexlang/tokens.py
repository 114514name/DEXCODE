"""Token 与词法类别定义。"""

from dataclasses import dataclass
from enum import Enum, auto


class TokKind(Enum):
    # 字面量 / 标识符
    INT = auto()
    FLOAT = auto()
    STRING = auto()
    IDENT = auto()
    # 标点
    LPAREN = auto()   # (
    RPAREN = auto()   # )
    LBRACE = auto()   # {
    RBRACE = auto()   # }
    COMMA = auto()    # ,
    SEMI = auto()     # ;
    # 运算符
    PLUS = auto()     # +
    MINUS = auto()    # -
    STAR = auto()     # *
    SLASH = auto()    # /
    PERCENT = auto()  # %
    EQ = auto()       # =
    EQEQ = auto()     # ==
    NE = auto()       # !=
    LT = auto()       # <
    LE = auto()       # <=
    GT = auto()       # >
    GE = auto()       # >=
    AND = auto()      # &&
    OR = auto()       # ||
    BANG = auto()     # !
    ARROW = auto()    # ->

    COLON = auto()    # :(用于定义文件的参数类型标注)
    DOT = auto()      # .(结构体字段访问)
    # 关键字
    LET = auto()
    IF = auto()
    ELSE = auto()
    WHILE = auto()
    FUNC = auto()
    RETURN = auto()
    PRINT = auto()
    TRUE = auto()
    FALSE = auto()
    INCLUDE = auto()  # include "库名";
    REFER = auto()    # refer "定义文件路径"; / refer "dll路径";
    EXTERN = auto()   # 定义文件:extern func ...
    RELEASE = auto()  # 定义文件:release <释放函数>;
    TYPE = auto()     # type 自定义数据类型(struct)
    # 结束
    EOF = auto()


KEYWORDS = {
    "let": TokKind.LET,
    "if": TokKind.IF,
    "else": TokKind.ELSE,
    "while": TokKind.WHILE,
    "func": TokKind.FUNC,
    "return": TokKind.RETURN,
    "print": TokKind.PRINT,
    "true": TokKind.TRUE,
    "false": TokKind.FALSE,
    "include": TokKind.INCLUDE,
    "refer": TokKind.REFER,
    "extern": TokKind.EXTERN,
    "release": TokKind.RELEASE,
    "type": TokKind.TYPE,
}

# 两字符运算符
DOUBLE_OPERATORS = {
    "==": TokKind.EQEQ, "!=": TokKind.NE, "<=": TokKind.LE,
    ">=": TokKind.GE, "&&": TokKind.AND, "||": TokKind.OR,
    "->": TokKind.ARROW,
}


@dataclass
class Token:
    kind: TokKind
    lexeme: str
    line: int
    col: int
    value: object = None

    def __repr__(self):
        return f"Token({self.kind.name}, {self.lexeme!r}, {self.line}:{self.col})"
