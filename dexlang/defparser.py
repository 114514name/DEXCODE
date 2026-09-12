"""定义文件(.dexdef)解析器。

定义文件描述一个 DLL/so 库的接口:

    # 指向实际库文件(相对本定义文件所在目录解析)
    refer "libdexmath.dll";

    extern func dex_add(a: int, b: int) -> int;
    extern func dex_fact(n: int) -> int;
    extern func dex_sqrt(x: float) -> float;
    extern func dex_len(s: string) -> int;
    extern func dex_hello() -> string;

类型:int / float / string / void。函数可返回 void(表示无返回值)。
"""

from dataclasses import dataclass, field
from typing import List, Optional

from .errors import DexError
from .lexer import Lexer
from .tokens import TokKind
from . import opcodes as O

# 定义文件中允许的类型名 → 类型码
_TYPE_NAMES = {
    "int": O.NAT_INT,
    "float": O.NAT_FLOAT,
    "string": O.NAT_STR,
    "void": O.NAT_VOID,
}


@dataclass
class NativeDecl:
    name: str
    param_types: List[int]      # NAT_INT/NAT_FLOAT/NAT_STR
    ret_type: int               # NAT_INT/NAT_FLOAT/NAT_STR/NAT_VOID

    @property
    def arity(self):
        return len(self.param_types)

    @property
    def sig(self):
        """形如 'ii:i' 的签名串(参数类型 + ':' + 返回类型)。"""
        params = "".join(O.CODE_TO_TYPE[c] for c in self.param_types)
        return f"{params}:{O.CODE_TO_TYPE[self.ret_type]}"


@dataclass
class DefFile:
    lib_path: Optional[str] = None    # refer 指向的 DLL/so 路径(相对路径)
    is_static: bool = False           # refer static:库字节将内嵌进字节码(静态链接)
    natives: List[NativeDecl] = field(default_factory=list)
    release: Optional[str] = None     # release <名>;原生返回字符串的释放函数(可选)
    abi: str = "direct"               # 调用约定:direct(默认) 或 value_array(见 SPEC 4.5)


def parse_sig(sig, max_arity=None):
    """'ii:i' → (param_types, ret_type)。

    max_arity 默认取直接 ABI 的上限;值数组 ABI 的调用方应传 opcodes.MAX_NATIVE_ARGS。
    """
    if ":" not in sig:
        raise DexError(f"invalid signature {sig!r} (expected e.g. 'ii:i')", phase="def")
    params, ret = sig.split(":", 1)
    pt = []
    for ch in params:
        code = O.TYPE_TO_CODE.get(ch)
        if code is None or code == O.NAT_VOID:
            raise DexError(f"invalid parameter type {ch!r} in signature {sig!r}", phase="def")
        pt.append(code)
    rc = O.TYPE_TO_CODE.get(ret)
    if rc is None:
        raise DexError(f"invalid return type {ret!r} in signature {sig!r}", phase="def")
    limit = O.MAX_NATIVE_ARITY if max_arity is None else max_arity
    if len(pt) > limit:
        raise DexError(
            f"native signature {sig!r} has {len(pt)} parameter(s); "
            f"at most {limit} are supported",
            phase="def")
    return pt, rc


def parse_def(text, filename="<def>"):
    """解析定义文件文本 → DefFile。"""
    toks = Lexer(text, filename).tokenize()
    pos = 0

    def peek(n=0):
        i = pos + n
        return toks[i] if i < len(toks) else toks[-1]

    def advance():
        nonlocal pos
        t = toks[pos]
        if t.kind != TokKind.EOF:
            pos += 1
        return t

    def expect(kind, what):
        t = peek()
        if t.kind != kind:
            raise DexError(f"expected {what}, got {t.lexeme!r}", t.line, t.col, "def")
        return advance()

    def parse_type():
        t = peek()
        if t.kind != TokKind.IDENT or t.lexeme not in _TYPE_NAMES:
            raise DexError(f"expected a type (int/float/string/void), got {t.lexeme!r}",
                           t.line, t.col, "def")
        advance()
        return _TYPE_NAMES[t.lexeme]

    def parse_native():
        # 形如: extern func name(a: int, b: float) -> string;
        expect(TokKind.EXTERN, "'extern'")
        expect(TokKind.FUNC, "'func'")
        name_tok = expect(TokKind.IDENT, "function name")
        name = name_tok.lexeme
        expect(TokKind.LPAREN, "'('")
        param_types = []

        def skip_opt_param_name():
            # 跳过可选的 `参数名:`
            if peek().kind == TokKind.IDENT and peek(1).kind == TokKind.COLON:
                advance()
                advance()

        if peek().kind != TokKind.RPAREN:
            skip_opt_param_name()
            param_types.append(parse_type())
            while peek().kind == TokKind.COMMA:
                advance()
                skip_opt_param_name()
                param_types.append(parse_type())
        expect(TokKind.RPAREN, "')'")
        expect(TokKind.ARROW, "'->'")
        ret_type = parse_type()
        expect(TokKind.SEMI, "';'")
        # 这里只做「绝对上限」的粗筛(不区分调用约定);ABI 相关上限在 parse_def
        # 末尾统一校验 —— 因为 `abi` 是文件级指令,可以出现在 extern 之后。
        if len(param_types) > O.MAX_NATIVE_ARGS:
            raise DexError(
                f"native function '{name}' has {len(param_types)} parameter(s); "
                f"at most {O.MAX_NATIVE_ARGS} are supported",
                name_tok.line, name_tok.col, "def")
        return NativeDecl(name=name, param_types=param_types, ret_type=ret_type)

    def_file = DefFile()
    abi_seen = False
    while peek().kind != TokKind.EOF:
        t = peek()
        if t.kind == TokKind.REFER:
            advance()
            # 可选 static:refer static "路径"; —— 库字节内嵌进字节码
            if peek().kind == TokKind.IDENT and peek().lexeme == "static":
                advance()
                def_file.is_static = True
            lib = expect(TokKind.STRING, "library path string").value
            expect(TokKind.SEMI, "';'")
            if def_file.lib_path is not None:
                raise DexError("duplicate 'refer' in definition file", t.line, t.col, "def")
            def_file.lib_path = lib
        elif t.kind == TokKind.EXTERN:
            def_file.natives.append(parse_native())
        elif t.kind == TokKind.IDENT and t.lexeme == "abi":
            # 可选:`abi value_array;` —— 文件级调用约定声明(默认 direct)。
            # 见 opcodes.NATIVE_ABI_* 与 docs/SPEC.md 4.5。
            advance()
            name_tok = expect(TokKind.IDENT, "abi name (direct / value_array)")
            if name_tok.lexeme not in O.ABI_NAMES:
                raise DexError(
                    f"unknown abi {name_tok.lexeme!r} (expected "
                    f"{' or '.join(sorted(O.ABI_NAMES))})",
                    name_tok.line, name_tok.col, "def")
            if abi_seen:
                raise DexError("duplicate 'abi' in definition file",
                               name_tok.line, name_tok.col, "def")
            abi_seen = True
            def_file.abi = name_tok.lexeme
            expect(TokKind.SEMI, "';'")
        elif t.kind == TokKind.RELEASE:
            # 可选:release <函数名>; —— 声明释放「原生返回的字符串缓冲区」的函数。
            # 见本文件头部「字符串所有权」说明与 README「引入 DLL 原生库」。
            advance()
            name = expect(TokKind.IDENT, "release function name").lexeme
            expect(TokKind.SEMI, "';'")
            if def_file.release is not None:
                raise DexError("duplicate 'release' in definition file",
                               t.line, t.col, "def")
            def_file.release = name
        else:
            raise DexError(f"unexpected token {t.lexeme!r} in definition file",
                           t.line, t.col, "def")

    # ABI 相关的 arity 上限:等整个文件读完再校验,这样与 `abi` 声明的位置无关
    # (它可以出现在 extern 之后)。直接 ABI 上限 3;值数组 ABI 上限 8。
    limit = (O.MAX_NATIVE_ARGS if def_file.abi == "value_array"
             else O.MAX_NATIVE_ARITY)
    for nd in def_file.natives:
        if nd.arity > limit:
            hint = ("" if def_file.abi == "value_array" else
                    f" (declare `abi value_array;` to allow up to "
                    f"{O.MAX_NATIVE_ARGS})")
            raise DexError(
                f"native function '{nd.name}' has {nd.arity} parameter(s); "
                f"at most {limit} are supported with the '{def_file.abi}' ABI{hint}",
                phase="def")
    return def_file
