"""bluedit.compile — 进程内编译 DexLang(替代 subprocess 调 main.py)。

打包后的 exe 无独立 Python 解释器与 main.py 脚本,编译必须走进程内 dexlang。
"""

import os

from . import paths
from dexlang import Lexer, Parser, compile_program, assemble, DexError


def compile_dex_file(dex_path, bc_path, include_dirs=None, rel_lib=False):
    """编译 .dex 源码 → .dexbc 字节码(进程内)。

    成功返回 AssemblyProgram;失败抛 DexError(带行号信息)。
    """
    with open(dex_path, "r", encoding="utf-8-sig") as f:
        source = f.read()
    inc = include_dirs if include_dirs is not None else [paths.libs_dir()]
    tokens = Lexer(source, dex_path).tokenize()
    ast = Parser(tokens, dex_path).parse_program()
    unit = compile_program(ast, source_path=dex_path,
                           include_dirs=inc, rel_lib=rel_lib)
    prog = unit.to_program()
    os.makedirs(os.path.dirname(os.path.abspath(bc_path)), exist_ok=True)
    with open(bc_path, "wb") as f:
        f.write(assemble(prog))
    return prog


def compile_error_text(e):
    """把 DexError 转成可展示的文本(含行号/列号)。"""
    if isinstance(e, DexError):
        msg = e.message
        if getattr(e, "line", 0):
            msg = f"[{e.phase or 'compiler'}] {getattr(e, 'line', 0)}:{getattr(e, 'col', 0)} {msg}"
        return msg
    return str(e)
