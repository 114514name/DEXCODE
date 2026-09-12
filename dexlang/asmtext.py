"""汇编文本格式(.dxasm)的渲染与解析。

文本示例:
    .lib "C:/libs/libdexmath.dll"
    .native dex_add 0 2 ii:i
    .func main 0
        PUSH 10
        STORE %0
        JMP Lmain_1
    Lmain_1:
        HALT
    .func fib 1
        ...
    NCALL dex_add
"""

import base64
import re

from . import opcodes as O
from .errors import DexError
from .ir import (
    AsmFunc, Insn, ConstOperand, LocalOperand, LabelOperand,
    FuncOperand, NativeOperand, NativeFunc, LibInfo, AssemblyProgram,
    TypeOperand, FieldOperand,
)

_LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):$")


def _split_line(line):
    """按空白切分,但保持带引号的字符串为一个整体(支持 \\ 转义)。"""
    parts = []
    i, n = 0, len(line)
    cur = []
    in_str = False
    while i < n:
        ch = line[i]
        if in_str:
            cur.append(ch)
            if ch == "\\" and i + 1 < n:
                cur.append(line[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
        elif ch.isspace():
            if cur:
                parts.append("".join(cur))
                cur = []
            i += 1
        elif ch == '"':
            in_str = True
            cur.append(ch)
            i += 1
        else:
            cur.append(ch)
            i += 1
    if cur:
        parts.append("".join(cur))
    return parts

# ---------- 渲染(IR → 文本) ----------


def _render_const(v):
    if isinstance(v, bool):
        return str(int(v))
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, str):
        esc = (
            v.replace("\\", "\\\\")
            .replace('"', '\\"')
            .replace("\n", "\\n")
            .replace("\t", "\\t")
            .replace("\r", "\\r")
        )
        return '"' + esc + '"'
    raise DexError(f"cannot render constant {v!r}", phase="asm-text")


def render(prog):
    """AssemblyProgram → 汇编文本。"""
    lines = []
    for lib in prog.libs:
        line = f".lib {_render_const(lib.path)}"
        if lib.is_static:
            b64 = base64.b64encode(lib.data).decode("ascii")
            line += f" static {b64}"
        lines.append(line)
    lib_index = {lib.path: i for i, lib in enumerate(prog.libs)}
    cur_abi = "direct"
    for nf in prog.natives:
        li = lib_index.get(nf.lib)
        if li is None:
            raise DexError(f"native '{nf.name}' references unknown library '{nf.lib}'", phase="asm-text")
        # 调用约定变化时才输出一行 .abi(现有全 direct 的库因此不产生任何额外输出,
        # 汇编文本保持原样,往返仍逐字节一致)
        abi = getattr(nf, "abi", "direct")
        if abi != cur_abi:
            lines.append(f".abi {abi}")
            cur_abi = abi
        lines.append(f".native {nf.name} {li} {nf.sig}")
    nat_index = {nf.name: i for i, nf in enumerate(prog.natives)}
    for lib in prog.libs:
        if lib.release_name:
            ri = nat_index.get(lib.release_name)
            if ri is None:
                raise DexError(
                    f"library '{lib.path}' release function '{lib.release_name}' "
                    f"is not a declared native", phase="asm-text")
            lines.append(f".release {lib_index[lib.path]} {ri}")
    for f in prog.funcs:
        lines.append(f".func {f.name} {f.arity}")
        for insn in f.insns:
            if insn.op is None:
                lines.append(f"{insn.label}:")
                continue
            mnem = O.MNEMONICS.get(insn.op, f"0x{insn.op:02X}")
            line = "    " + mnem
            operand = insn.operand
            if operand is not None:
                if isinstance(operand, ConstOperand):
                    line += " " + _render_const(operand.value)
                elif isinstance(operand, LocalOperand):
                    line += f" %{operand.index}"
                elif isinstance(operand, LabelOperand):
                    line += " " + operand.name
                elif isinstance(operand, FuncOperand):
                    line += f" @{operand.name}"
                elif isinstance(operand, NativeOperand):
                    line += f" {operand.name}"
                elif isinstance(operand, TypeOperand):
                    line += f" {operand.name}({operand.nfields})"
                elif isinstance(operand, FieldOperand):
                    line += f" {operand.index}"
                else:
                    raise DexError(f"unknown operand type {type(operand).__name__}", phase="asm-text")
            lines.append(line)
    return "\n".join(lines) + "\n"


# ---------- 解析(文本 → IR) ----------

_ESCAPE_MAP = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "0": "\0"}


def _parse_const(text, lineno):
    if text.startswith('"'):
        if not text.endswith('"') or len(text) < 2:
            raise DexError("invalid string constant", lineno, 1, "asm-text")
        body = text[1:-1]
        out = []
        i = 0
        while i < len(body):
            ch = body[i]
            if ch == "\\" and i + 1 < len(body):
                out.append(_ESCAPE_MAP.get(body[i + 1], body[i + 1]))
                i += 2
            else:
                out.append(ch)
                i += 1
        return "".join(out)
    try:
        if any(c in text for c in ".eE"):
            return float(text)
        return int(text, 10)
    except ValueError:
        raise DexError(f"invalid constant {text!r}", lineno, 1, "asm-text")


def _parse_operand(op, text, lineno):
    try:
        if op == O.PUSH:
            return ConstOperand(_parse_const(text, lineno))
        if op in (O.LOAD, O.STORE):
            if not text.startswith("%"):
                raise ValueError
            return LocalOperand(int(text[1:], 10))
        if op in (O.JMP, O.JZ, O.JNZ):
            return LabelOperand(text)
        if op == O.CALL:
            if not text.startswith("@"):
                raise ValueError
            return FuncOperand(text[1:])
        if op == O.NCALL:
            return NativeOperand(text)
        if op == O.MAKE_OBJ:
            if text.endswith(")") and "(" in text:
                name, _, nf = text[:-1].partition("(")
                return TypeOperand(name=name, nfields=int(nf, 10))
            return TypeOperand(name=text, nfields=0)
        if op in (O.GET_FIELD, O.SET_FIELD):
            return FieldOperand(int(text, 10))
    except ValueError:
        raise DexError(f"invalid operand {text!r} for '{O.MNEMONICS[op]}'", lineno, 1, "asm-text")
    raise DexError(f"instruction '{O.MNEMONICS[op]}' takes no operand", lineno, 1, "asm-text")


def parse(text):
    """解析汇编文本 → AssemblyProgram。"""
    prog = AssemblyProgram()
    current = None
    cur_abi = "direct"      # 被 .abi 指令修改,影响其后所有 .native 行
    labels = set()

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue

        m = _LABEL_RE.match(line)
        if m:
            if current is None:
                raise DexError("label outside function", lineno, 1, "asm-text")
            lbl = m.group(1)
            if lbl in labels:
                raise DexError(f"duplicate label '{lbl}'", lineno, 1, "asm-text")
            labels.add(lbl)
            current.insns.append(Insn(op=None, label=lbl))
            continue

        parts = _split_line(line)
        head = parts[0]

        if head == ".lib":
            # .lib "path" 或 .lib "path" static <base64>
            if len(parts) == 2:
                prog.libs.append(LibInfo(path=_parse_const(parts[1], lineno),
                                         is_static=False, data=b""))
            elif len(parts) == 4 and parts[2] == "static":
                try:
                    data = base64.b64decode(parts[3], validate=True)
                except Exception:
                    raise DexError("invalid base64 in static .lib", lineno, 1, "asm-text")
                prog.libs.append(LibInfo(path=_parse_const(parts[1], lineno),
                                         is_static=True, data=data))
            else:
                raise DexError("expected '.lib \"path\"' or '.lib \"path\" static <base64>'",
                               lineno, 1, "asm-text")
            continue

        if head == ".native":
            # .native NAME LIBIDX SIGNATURE
            if len(parts) != 4:
                raise DexError("expected '.native NAME LIBIDX SIGNATURE'", lineno, 1, "asm-text")
            name = parts[1]
            try:
                li = int(parts[2])
            except ValueError:
                raise DexError(f"invalid library index {parts[2]!r}", lineno, 1, "asm-text")
            if li >= len(prog.libs):
                raise DexError(f".native '{name}' references unknown library index {li}", lineno, 1, "asm-text")
            if any(n.name == name for n in prog.natives):
                raise DexError(f"duplicate native '{name}'", lineno, 1, "asm-text")
            from .defparser import parse_sig
            limit = (O.MAX_NATIVE_ARGS if cur_abi == "value_array"
                     else O.MAX_NATIVE_ARITY)
            param_types, ret_type = parse_sig(parts[3], max_arity=limit)
            prog.natives.append(NativeFunc(
                name=name, lib=prog.libs[li].path, param_types=param_types,
                ret_type=ret_type, abi=cur_abi))
            continue

        if head == ".abi":
            # .abi direct | value_array —— 影响其后所有 .native 行的调用约定
            if len(parts) != 2 or parts[1] not in O.ABI_NAMES:
                raise DexError("expected '.abi direct' or '.abi value_array'",
                               lineno, 1, "asm-text")
            cur_abi = parts[1]
            continue

        if head == ".release":
            # .release LIBIDX NATIVEIDX —— 该库的字符串释放函数(见 SPEC 4.6)
            if len(parts) != 3:
                raise DexError("expected '.release LIBIDX NATIVEIDX'", lineno, 1, "asm-text")
            try:
                li = int(parts[1]); ri = int(parts[2])
            except ValueError:
                raise DexError("invalid index in .release", lineno, 1, "asm-text")
            if li >= len(prog.libs):
                raise DexError(f".release references unknown library index {li}", lineno, 1, "asm-text")
            if ri >= len(prog.natives):
                raise DexError(f".release references unknown native index {ri}", lineno, 1, "asm-text")
            rel_name = prog.natives[ri].name
            if rel_name == "":
                raise DexError(".release native has no name", lineno, 1, "asm-text")
            prog.libs[li].release_name = rel_name
            continue

        if head == ".func":
            if len(parts) != 3:
                raise DexError("expected '.func NAME ARITY'", lineno, 1, "asm-text")
            name = parts[1]
            try:
                arity = int(parts[2])
            except ValueError:
                raise DexError(f"invalid arity {parts[2]!r}", lineno, 1, "asm-text")
            if any(f.name == name for f in prog.funcs):
                raise DexError(f"duplicate function '{name}'", lineno, 1, "asm-text")
            current = AsmFunc(name=name, arity=arity)
            prog.funcs.append(current)
            labels = set()
            continue

        if current is None:
            raise DexError("instruction outside function", lineno, 1, "asm-text")

        op = O.MNEMONIC_TO_OP.get(head)
        if op is None:
            raise DexError(f"unknown mnemonic '{head}'", lineno, 1, "asm-text")

        if op in O.HAS_OPERAND:
            if len(parts) != 2:
                raise DexError(f"'{head}' requires exactly one operand", lineno, 1, "asm-text")
            operand = _parse_operand(op, parts[1], lineno)
        else:
            if len(parts) != 1:
                raise DexError(f"'{head}' takes no operand", lineno, 1, "asm-text")
            operand = None

        current.insns.append(Insn(op=op, operand=operand))

    return prog
