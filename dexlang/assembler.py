"""汇编器:汇编 IR → 字节码二进制(.dexbc)。

- 构建常量池(库路径→原生名→函数名→代码中 PUSH 常量,固定顺序保证往返一致)
- 解析函数内标签为偏移(函数内相对偏移)
- 编码普通函数指令与原生调用(NCALL)
"""

import struct

from . import opcodes as O
from .errors import DexError
from .ir import (
    ConstOperand, LocalOperand, LabelOperand, FuncOperand, NativeOperand,
    TypeOperand, FieldOperand,
)


def _encode_const(v):
    if isinstance(v, bool):
        v = int(v)
    if isinstance(v, int):
        return bytes([O.TAG_INT]) + struct.pack("<q", v)
    if isinstance(v, float):
        return bytes([O.TAG_FLOAT]) + struct.pack("<d", v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        return bytes([O.TAG_STRING]) + struct.pack("<H", len(b)) + b
    raise DexError(f"unsupported constant type {type(v).__name__}", phase="assembler")


def _compute_nlocals(f):
    if f.nlocals:
        return f.nlocals
    max_local = -1
    for insn in f.insns:
        if insn.op in (O.LOAD, O.STORE):
            max_local = max(max_local, insn.operand.index)
    return max(f.arity, max_local + 1)


def _operand_value(insn, offsets, cidx, func_index, native_index, nlocals):
    op = insn.op
    operand = insn.operand
    if op == O.PUSH:
        return cidx(operand.value)
    if op in (O.LOAD, O.STORE):
        idx = operand.index
        if idx < 0 or idx >= nlocals:
            raise DexError(
                f"local index %{idx} out of range (nlocals={nlocals}) in function",
                phase="assembler",
            )
        return idx
    if op in (O.JMP, O.JZ, O.JNZ):
        if operand.name not in offsets:
            raise DexError(f"undefined label '{operand.name}'", phase="assembler")
        return offsets[operand.name]
    if op == O.CALL:
        fi = func_index.get(operand.name)
        if fi is None:
            raise DexError(f"undefined function '{operand.name}'", phase="assembler")
        return fi
    if op == O.NCALL:
        ni = native_index.get(operand.name)
        if ni is None:
            raise DexError(f"undefined native function '{operand.name}'", phase="assembler")
        return ni
    if op == O.MAKE_OBJ:
        # 操作数 = 类型名常量索引<<16 | 字段数
        tc = cidx(operand.name)
        return (tc << 16) | operand.nfields
    if op in (O.GET_FIELD, O.SET_FIELD):
        return operand.index
    raise DexError(f"unexpected operand on opcode 0x{op:02X}", phase="assembler")


def assemble(prog) -> bytes:
    """AssemblyProgram → .dexbc 二进制字节。"""
    asm_funcs = prog.funcs
    natives = prog.natives
    libs = prog.libs
    if not asm_funcs:
        raise DexError("no functions to assemble", phase="assembler")

    # ---------- 常量池注册(固定顺序:库路径 → 原生名 → 函数名 → PUSH 常量) ----------
    consts = []
    const_index = {}

    def cidx(value):
        idx = const_index.get(value)
        if idx is None:
            idx = len(consts)
            if idx >= 0x10000:
                raise DexError("too many constants (>65535)", phase="assembler")
            const_index[value] = idx
            consts.append(value)
        return idx

    if len(libs) >= 0x10000 or len(natives) >= 0x10000 or len(asm_funcs) >= 0x10000:
        raise DexError("too many libs/natives/functions (>65535)", phase="assembler")
    for lib in libs:
        cidx(lib.path)
    for nf in natives:
        cidx(nf.name)
    func_index = {f.name: i for i, f in enumerate(asm_funcs)}
    for f in asm_funcs:
        cidx(f.name)
    native_index = {n.name: i for i, n in enumerate(natives)}
    for f in asm_funcs:
        for insn in f.insns:
            if insn.op == O.PUSH and isinstance(insn.operand, ConstOperand):
                cidx(insn.operand.value)
            elif insn.op == O.MAKE_OBJ:
                cidx(insn.operand.name)   # 类型名入常量池(字符串常量)

    # ---------- 编码代码段与函数表 ----------
    code = bytearray()
    func_table = []

    for f in asm_funcs:
        # pass 1:标签 → 函数内偏移
        offsets = {}
        off = 0
        for insn in f.insns:
            if insn.op is None:
                if insn.label in offsets:
                    raise DexError(f"duplicate label '{insn.label}'", phase="assembler")
                offsets[insn.label] = off
            else:
                off += O.instruction_size(insn.op)

        nlocals = _compute_nlocals(f)

        # pass 2:编码
        start = len(code)
        for insn in f.insns:
            if insn.op is None:
                continue
            code.append(insn.op)
            if insn.op in O.HAS_OPERAND:
                code += struct.pack(
                    "<I", _operand_value(insn, offsets, cidx, func_index, native_index, nlocals)
                )
        code_len = len(code) - start

        name_idx = cidx(f.name)
        func_table.append(
            struct.pack("<H", name_idx)
            + bytes([f.arity])
            + struct.pack("<H", nlocals)
            + struct.pack("<I", start)
            + struct.pack("<I", code_len)
        )

    # ---------- 库表(含静态内嵌数据)与原生函数表 ----------
    lib_table = []
    for lib in libs:
        flags = O.LIB_STATIC if lib.is_static else 0
        entry = (struct.pack("<H", cidx(lib.path))
                 + bytes([flags])
                 + struct.pack("<I", len(lib.data)))
        if lib.data:
            entry += lib.data
        lib_table.append(entry)
    native_table = []
    for nf in natives:
        lib_idx = next(i for i, l in enumerate(libs) if l.path == nf.lib)
        native_table.append(
            struct.pack("<H", cidx(nf.name))
            + struct.pack("<H", lib_idx)
            + bytes([nf.arity, nf.ret_type])
            + bytes(nf.param_types)
        )

    # ---------- 组装文件 ----------
    header = (
        O.MAGIC
        + bytes([O.VERSION, 0])
        + struct.pack("<H", len(consts))
        + struct.pack("<H", len(asm_funcs))
        + struct.pack("<H", len(libs))
        + struct.pack("<H", len(natives))
        + struct.pack("<I", len(code))
    )
    pool = b"".join(_encode_const(v) for v in consts)
    body = (header + pool + b"".join(lib_table) + b"".join(native_table)
            + b"".join(func_table) + bytes(code))

    # 可选 trailer:DXRL + 条目数 + (lib_idx, release_native_idx) 对。
    # 放在文件末尾,因而对不认识本节的旧版 VM 完全无害(它们只读 code_size 之前的内容);
    # 也避免了改动函数表/库表的定长布局,保持旧字节码的原样可读。
    trailer = b""
    if any(lib.release_name for lib in libs):
        entries = []
        for i, lib in enumerate(libs):
            if not lib.release_name:
                continue
            ri = native_index.get(lib.release_name)
            if ri is None:
                raise DexError(
                    f"library '{lib.path}' declares release function "
                    f"'{lib.release_name}' which is not among its natives",
                    phase="assembler")
            entries.append(struct.pack("<HH", i, ri))
        trailer = (b"DXRL" + struct.pack("<H", len(entries)) + b"".join(entries))

    return body + trailer
