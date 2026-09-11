"""DEXCODE 工具链包。

管线:源码 → Lexer → Parser(AST) → Compiler(汇编 IR) → Assembler(字节码)
以及 Disassembler(字节码 → 汇编);支持 include/refer 引入 DLL 原生函数。
"""

from .lexer import Lexer
from .parser import Parser
from .compiler import compile_program, CompileUnit
from .assembler import assemble
from .disassembler import decode, disassemble
from .asmtext import render, parse as parse_asm_text
from .defparser import parse_def, parse_sig, NativeDecl, DefFile
from .errors import DexError
from .ir import AssemblyProgram, NativeFunc

__all__ = [
    "Lexer",
    "Parser",
    "compile_program",
    "CompileUnit",
    "assemble",
    "decode",
    "disassemble",
    "render",
    "parse_asm_text",
    "parse_def",
    "parse_sig",
    "NativeDecl",
    "DefFile",
    "AssemblyProgram",
    "NativeFunc",
    "DexError",
]


def compile_string(source, filename="<source>", include_dirs=None):
    """源码字符串 → (汇编文本, 字节码 bytes)。

    filename 用于解析 include/refer 的相对路径;include_dirs 为附加搜索目录。
    """
    tokens = Lexer(source, filename).tokenize()
    ast = Parser(tokens, filename).parse_program()
    unit = compile_program(ast, source_path=filename, include_dirs=include_dirs)
    prog = unit.to_program()
    return render(prog), assemble(prog)
