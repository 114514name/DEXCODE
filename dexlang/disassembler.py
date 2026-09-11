"""反汇编器:字节码二进制(.dexbc) → 汇编 IR / 文本。

- 校验头部/魔数/版本
- 解码常量池、函数表、代码段
- 按跳转目标在函数内生成符号标签
- 与 assemble() 组合可实现汇编 ↔ 字节码往返
"""

import struct

from . import opcodes as O
from .errors import DexError
from .ir import (
    AsmFunc, Insn, ConstOperand, LocalOperand, LabelOperand,
    FuncOperand, NativeOperand, NativeFunc, LibInfo, AssemblyProgram,
    TypeOperand, FieldOperand,
)


def _rd32(code, off):
    return struct.unpack_from("<I", code, off)[0]


def decode(data) -> AssemblyProgram:
    """解析 .dexbc 字节 → AssemblyProgram。"""
    if len(data) < O.HEADER_SIZE:
        raise DexError("file too small / not a bytecode file", phase="disassembler")
    if data[0:4] != O.MAGIC:
        raise DexError("bad magic (not a .dexbc file)", phase="disassembler")
    if data[4] != O.VERSION:
        raise DexError(f"unsupported version {data[4]}", phase="disassembler")

    off = 6
    n_consts, n_funcs, n_libs, n_natives, code_size = struct.unpack_from("<HHHHI", data, off)
    off += 12

    # ---------- 常量池 ----------
    consts = []
    for _ in range(n_consts):
        if off >= len(data):
            raise DexError("truncated constant pool", phase="disassembler")
        tag = data[off]
        off += 1
        if tag == O.TAG_INT:
            consts.append(struct.unpack_from("<q", data, off)[0])
            off += 8
        elif tag == O.TAG_FLOAT:
            consts.append(struct.unpack_from("<d", data, off)[0])
            off += 8
        elif tag == O.TAG_STRING:
            n = struct.unpack_from("<H", data, off)[0]
            off += 2
            if off + n > len(data):
                raise DexError("truncated string constant", phase="disassembler")
            consts.append(data[off:off + n].decode("utf-8", "replace"))
            off += n
        else:
            raise DexError(f"bad constant tag {tag}", phase="disassembler")

    def _need_str(idx, what):
        if idx >= len(consts) or not isinstance(consts[idx], str):
            raise DexError(f"bad {what} index {idx}", phase="disassembler")
        return consts[idx]

    # ---------- 库表(含静态内嵌数据) ----------
    libs = []
    for i in range(n_libs):
        if off + O.LIB_ENTRY_SIZE > len(data):
            raise DexError("truncated library table", phase="disassembler")
        path_idx, flags, data_len = struct.unpack_from("<HBI", data, off)
        off += O.LIB_ENTRY_SIZE
        if off + data_len > len(data):
            raise DexError("truncated static library data", phase="disassembler")
        payload = data[off:off + data_len]
        off += data_len
        libs.append(LibInfo(
            path=_need_str(path_idx, f"library {i} path"),
            is_static=bool(flags & O.LIB_STATIC),
            data=payload,
        ))

    # ---------- 原生函数表 ----------
    natives = []
    for i in range(n_natives):
        if off + O.NATIVE_FIXED_SIZE > len(data):
            raise DexError("truncated native function table", phase="disassembler")
        name_idx, lib_idx, arity, ret_type = struct.unpack_from("<HHBB", data, off)
        off += O.NATIVE_FIXED_SIZE
        if off + arity > len(data):
            raise DexError("truncated native parameter types", phase="disassembler")
        param_types = list(data[off:off + arity])
        off += arity
        name = _need_str(name_idx, f"native {i} name")
        if lib_idx >= len(libs):
            raise DexError(f"native '{name}' references unknown library {lib_idx}", phase="disassembler")
        if ret_type not in O.CODE_TO_TYPE:
            raise DexError(f"native '{name}' has bad return type {ret_type}", phase="disassembler")
        for c in param_types:
            if c not in O.CODE_TO_TYPE or c == O.NAT_VOID:
                raise DexError(f"native '{name}' has bad parameter type {c}", phase="disassembler")
        natives.append(NativeFunc(name=name, lib=libs[lib_idx].path,
                                  param_types=param_types, ret_type=ret_type))

    # ---------- 函数表 ----------
    funcs = []
    for _ in range(n_funcs):
        if off + O.FUNC_ENTRY_SIZE > len(data):
            raise DexError("truncated function table", phase="disassembler")
        name_idx, arity, nlocals, code_off, code_len = struct.unpack_from(
            "<HBHII", data, off
        )
        off += O.FUNC_ENTRY_SIZE
        funcs.append({
            "name": _need_str(name_idx, "function name"),
            "arity": arity,
            "nlocals": nlocals,
            "code_off": code_off,
            "code_len": code_len,
        })

    # ---------- 代码段 ----------
    code = data[off:off + code_size]
    if len(code) < code_size:
        raise DexError("truncated code section", phase="disassembler")
    off += code_size          # 代码段之后可能还有可选 trailer(见文件末尾)

    asm_funcs = []
    for f in funcs:
        start, end = f["code_off"], f["code_off"] + f["code_len"]
        if end > len(code):
            raise DexError(f"function '{f['name']}' code out of range", phase="disassembler")

        # 收集跳转目标(统一转换为绝对偏移)
        targets = set()
        p = start
        while p < end:
            op = code[p]
            if op not in O.MNEMONICS:
                raise DexError(f"unknown opcode 0x{op:02X}", phase="disassembler")
            if op in O.HAS_OPERAND:
                operand = _rd32(code, p + 1)
                if op in (O.JMP, O.JZ, O.JNZ):
                    targets.add(start + operand)
                p += 5
            else:
                p += 1

        labels = {t: f"L{t}" for t in sorted(targets)}

        insns = []
        p = start
        while p < end:
            if p in labels:
                insns.append(Insn(op=None, label=labels[p]))
            op = code[p]
            if op in O.HAS_OPERAND:
                operand = _rd32(code, p + 1)
                if op == O.PUSH:
                    if operand >= len(consts):
                        raise DexError("PUSH index out of range", phase="disassembler")
                    operand = ConstOperand(consts[operand])
                elif op in (O.LOAD, O.STORE):
                    operand = LocalOperand(operand)
                elif op in (O.JMP, O.JZ, O.JNZ):
                    target_abs = start + operand
                    operand = LabelOperand(labels.get(target_abs, f"L{target_abs}"))
                elif op == O.CALL:
                    if operand >= len(funcs):
                        raise DexError("CALL index out of range", phase="disassembler")
                    operand = FuncOperand(funcs[operand]["name"])
                elif op == O.NCALL:
                    if operand >= len(natives):
                        raise DexError("NCALL index out of range", phase="disassembler")
                    operand = NativeOperand(natives[operand].name)
                elif op == O.MAKE_OBJ:
                    type_cidx = operand >> 16
                    nfields = operand & 0xFFFF
                    tname = _need_str(type_cidx, "type name")
                    operand = TypeOperand(name=tname, nfields=nfields)
                elif op in (O.GET_FIELD, O.SET_FIELD):
                    operand = FieldOperand(operand)
                insns.append(Insn(op=op, operand=operand))
                p += 5
            else:
                insns.append(Insn(op=op))
                p += 1

        if end in labels:  # 跳到函数末尾的兜底标签
            insns.append(Insn(op=None, label=labels[end]))

        asm_funcs.append(AsmFunc(
            name=f["name"], arity=f["arity"], nlocals=f["nlocals"], insns=insns,
        ))

    # ---------- 可选 trailer:DXRL(库的字符串释放函数) ----------
    # 位于代码段之后;旧字节码没有这一节,所以必须按"剩余字节 + 标记"来探测。
    if off + 6 <= len(data) and data[off:off + 4] == b"DXRL":
        n_rel = struct.unpack_from("<H", data, off + 4)[0]
        p = off + 6
        for _ in range(n_rel):
            if p + 4 > len(data):
                raise DexError("truncated release table", phase="disassembler")
            lib_idx, rel_idx = struct.unpack_from("<HH", data, p)
            p += 4
            if lib_idx >= len(libs) or rel_idx >= len(natives):
                raise DexError("bad release table entry", phase="disassembler")
            libs[lib_idx].release_name = natives[rel_idx].name

    return AssemblyProgram(funcs=asm_funcs, natives=natives, libs=libs)


def disassemble(data) -> str:
    """解析 .dexbc 字节 → 汇编文本。"""
    from .asmtext import render
    return render(decode(data))
