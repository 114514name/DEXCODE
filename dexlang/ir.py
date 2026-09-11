"""汇编 IR:中间表示(汇编形式)。

每条指令是一个 `Insn`,操作数可以是常量 / 局部槽 / 标签 / 函数。
标签用 `Insn(op=None, label=...)` 表示(伪指令,零字节)。
"""

from dataclasses import dataclass, field
from typing import List, Optional, Any


@dataclass
class ConstOperand:
    """常量操作数,value 为 Python 值(int/float/str)。"""
    value: Any


@dataclass
class LocalOperand:
    """局部变量槽编号。"""
    index: int


@dataclass
class LabelOperand:
    """符号标签(函数内唯一)。"""
    name: str


@dataclass
class FuncOperand:
    """DexLang 函数名。"""
    name: str


@dataclass
class NativeOperand:
    """原生(DLL/so)函数名。"""
    name: str


@dataclass
class TypeOperand:
    """结构体类型名(MAKE_OBJ)。"""
    name: str
    nfields: int = 0


@dataclass
class FieldOperand:
    """结构体字段索引(GET_FIELD / SET_FIELD)。"""
    index: int


@dataclass
class Insn:
    op: Optional[int] = None          # 操作码;None 表示标签定义
    operand: Any = None               # Const/Local/Label/Func/NativeOperand 之一
    label: Optional[str] = None       # op 为 None 时保存标签名
    line: int = 0                     # 来源源码行号(用于调试/断点)


@dataclass
class AsmFunc:
    name: str = ""
    arity: int = 0
    nlocals: int = 0
    insns: List[Insn] = field(default_factory=list)


@dataclass
class NativeFunc:
    """原生函数(来自定义文件):名字 + 所属库 + 签名。"""
    name: str
    lib: str                      # 库路径(与 AssemblyProgram.libs 中的元素一致)
    param_types: List[int]        # NAT_INT/NAT_FLOAT/NAT_STR
    ret_type: int = 1             # NAT_VOID/NAT_INT/NAT_FLOAT/NAT_STR

    @property
    def arity(self):
        return len(self.param_types)

    @property
    def sig(self):
        from . import opcodes as O
        params = "".join(O.CODE_TO_TYPE[c] for c in self.param_types)
        return f"{params}:{O.CODE_TO_TYPE[self.ret_type]}"


@dataclass
class LibInfo:
    """一个原生库(动态加载或静态内嵌)。"""
    path: str = ""
    is_static: bool = False       # True = 库字节内嵌在字节码中,运行时无需外部 DLL
    data: bytes = b""             # 内嵌的库文件字节(is_static 时有效)
    release_name: str = ""        # 本库的字符串释放函数名(约定见 SPEC 4.5),空=无


@dataclass
class AssemblyProgram:
    """完整汇编程序:普通函数 + 原生函数 + 库表。"""
    funcs: List[AsmFunc] = field(default_factory=list)
    natives: List[NativeFunc] = field(default_factory=list)
    libs: List[LibInfo] = field(default_factory=list)
