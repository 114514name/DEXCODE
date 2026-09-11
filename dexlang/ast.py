"""AST 节点定义。"""

from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class Node:
    line: int = 0
    col: int = 0


@dataclass
class Program(Node):
    stmts: List[Node] = field(default_factory=list)


@dataclass
class Let(Node):
    name: str = ""
    value: Node = None


@dataclass
class Assign(Node):
    name: str = ""
    value: Node = None


@dataclass
class Print(Node):
    exprs: List[Node] = field(default_factory=list)


@dataclass
class If(Node):
    cond: Node = None
    then: List[Node] = field(default_factory=list)
    els: Optional[List[Node]] = None


@dataclass
class While(Node):
    cond: Node = None
    body: List[Node] = field(default_factory=list)


@dataclass
class Func(Node):
    name: str = ""
    params: List[str] = field(default_factory=list)
    param_types: List[str] = field(default_factory=list)  # 与 params 对齐;空串=未知
    body: List[Node] = field(default_factory=list)
    ret_type: str = ""   # 返回类型标注(func f() -> Type;空表示未知)


@dataclass
class Return(Node):
    value: Optional[Node] = None


@dataclass
class ExprStmt(Node):
    expr: Node = None


@dataclass
class Literal(Node):
    value: object = None


@dataclass
class Name(Node):
    name: str = ""


@dataclass
class BinOp(Node):
    op: str = ""
    left: Node = None
    right: Node = None


@dataclass
class UnaryOp(Node):
    op: str = ""
    operand: Node = None


@dataclass
class Call(Node):
    name: str = ""
    args: List[Node] = field(default_factory=list)


@dataclass
class LibRef(Node):
    """include/refer 库引入语句(仅允许出现在顶层)。"""
    kind: str = "include"     # "include" | "refer"
    target: str = ""          # 库名(include)或定义文件路径(refer)


@dataclass
class TypeDef(Node):
    """自定义数据类型(struct):type Name { field: type; ... }"""
    name: str = ""
    fields: List = field(default_factory=list)   # [(字段名, 类型名)]


@dataclass
class StructLit(Node):
    """结构体字面量:Name{ field: expr, ... }"""
    type_name: str = ""
    fields: List = field(default_factory=list)   # [(字段名, 值节点)]


@dataclass
class GetField(Node):
    """成员读取:p.x(可链式 p.a.b)"""
    obj: Node = None
    field: str = ""


@dataclass
class SetField(Node):
    """成员赋值:p.x = expr"""
    obj: Node = None
    field: str = ""
    value: Node = None
