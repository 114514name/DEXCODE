#!/usr/bin/env python3
"""DEXCODE 工具链 CLI。

用法:
  python main.py compile <src.dex> [-o out.dexbc]   # 源码 → 汇编 + 字节码
  python main.py asm     <in.dxasm> [-o out.dexbc]  # 汇编 → 字节码
  python main.py disasm  <in.dexbc> [-o out.dxasm]  # 字节码 → 汇编
  python main.py run     <in.dexbc>                 # 用 C VM 执行
  python main.py build-vm                           # 编译 C VM
"""

import argparse
import os
import shutil
import subprocess
import sys

from dexlang import (
    Lexer, Parser, compile_program, assemble, decode, disassemble,
    render, parse_asm_text, DexError,
)

ROOT = os.path.dirname(os.path.abspath(__file__))

# 默认 include 库定义搜索目录:项目根下的 libs/(include "math" → libs/math/math.dexdef)
DEFAULT_LIBS = os.path.join(ROOT, "libs")


def vm_exe_name():
    return "vm.exe" if os.name == "nt" else "vm"


def vm_path():
    p = os.path.join(ROOT, "vm", vm_exe_name())
    return p if os.path.exists(p) else None


def write_bytes(path, data):
    with open(path, "wb") as f:
        f.write(data)


def read_text(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return f.read()


def derive_out(path, out, default_ext):
    if out:
        return out
    base = os.path.splitext(os.path.abspath(path))[0]
    return base + default_ext


# ---------- 各子命令 ----------

def cmd_compile(args):
    source = read_text(args.src)
    tokens = Lexer(source, args.src).tokenize()
    ast = Parser(tokens, args.src).parse_program()
    unit = compile_program(ast, source_path=args.src, include_dirs=args.include_dirs,
                           rel_lib=getattr(args, "rel_lib", False))
    prog = unit.to_program()

    for w in unit.warnings:
        print(f"warning: {w}", file=sys.stderr)

    asm_text = render(prog)
    bc_path = derive_out(args.src, args.o, ".dexbc")
    asm_path = derive_out(args.src, None, ".dxasm")

    write_bytes(bc_path, assemble(prog))
    if not args.no_asm:
        with open(asm_path, "w", encoding="utf-8") as f:
            f.write(asm_text)
        print(f"汇编(IR): {asm_path}")
    print(f"字节码:   {bc_path} ({os.path.getsize(bc_path)} bytes)")
    if prog.natives:
        info = ", ".join(f"{os.path.basename(l.path)}"
                         + ("[静态]" if l.is_static else "") for l in prog.libs)
        print(f"原生函数: {len(prog.natives)} 个,库: [{info}]")
    return 0


def cmd_asm(args):
    prog = parse_asm_text(read_text(args.src))
    bc_path = derive_out(args.src, args.o, ".dexbc")
    write_bytes(bc_path, assemble(prog))
    print(f"字节码: {bc_path} ({os.path.getsize(bc_path)} bytes)")
    return 0


def cmd_disasm(args):
    with open(args.src, "rb") as f:
        data = f.read()
    text = disassemble(data)
    asm_path = derive_out(args.src, args.o, ".dxasm")
    with open(asm_path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"汇编(IR): {asm_path}")
    return 0


def cmd_run(args):
    vm = vm_path()
    if vm is None:
        print("未找到 C VM,请先运行: python main.py build-vm", file=sys.stderr)
        return 1
    try:
        rc = subprocess.run([vm, os.path.abspath(args.bc)], check=False)
        return rc.returncode
    except OSError as e:
        print(f"运行 VM 失败: {e}", file=sys.stderr)
        return 1


def cmd_release(args):
    """把 GAL/DexLang 程序打包成可独立发布的成品目录。

    成品 = 发布版 VM(galrun.exe)+ core.do(字节码)+ 资源 + 启动脚本。
    运行时无需 Python,双击"启动游戏.bat"即可。
    """
    import shutil

    # 1) 编译源码 -> 字节码
    source = read_text(args.src)
    tokens = Lexer(source, args.src).tokenize()
    ast = Parser(tokens, args.src).parse_program()
    unit = compile_program(ast, source_path=args.src, include_dirs=args.include_dirs)
    for w in unit.warnings:
        print(f"warning: {w}", file=sys.stderr)
    bc = assemble(unit.to_program())

    # 2) 组装目录
    out = os.path.abspath(args.out or "release")
    os.makedirs(out, exist_ok=True)
    galrun = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vm", "galrun.exe")
    if not os.path.exists(galrun):
        print("未找到发布版 VM(vm/galrun.exe),请先运行: python main.py build-vm", file=sys.stderr)
        return 1
    shutil.copy(galrun, os.path.join(out, "galrun.exe"))
    with open(os.path.join(out, "core.do"), "wb") as f:
        f.write(bc)

    # 3) 复制资源目录到 res/
    res_out = os.path.join(out, "res")
    copied = 0
    for d in args.res or []:
        src_dir = os.path.abspath(d)
        if not os.path.isdir(src_dir):
            print(f"warning: 资源目录不存在,跳过: {src_dir}", file=sys.stderr)
            continue
        os.makedirs(res_out, exist_ok=True)
        for name in os.listdir(src_dir):
            s = os.path.join(src_dir, name)
            if os.path.isfile(s):
                shutil.copy(s, os.path.join(res_out, name))
                copied += 1

    # 4) 启动脚本 + 说明
    title = args.title or os.path.splitext(os.path.basename(args.src))[0]
    with open(os.path.join(out, "启动游戏.bat"), "w", encoding="utf-8") as f:
        f.write('@echo off\r\nstart "" "%~dp0galrun.exe"\r\n')
    with open(os.path.join(out, "游戏说明.txt"), "w", encoding="utf-8") as f:
        f.write(f"{title}\n\n"
                "这是一个由 DEXCODE 生成的 GAL 游戏。\n"
                "运行时只需要本目录,无需安装 Python 或任何库。\n"
                "双击 \"启动游戏.bat\" 即可开始。\n\n"
                "目录结构:\n"
                "  galrun.exe   发布版虚拟机(自动读取 core.do)\n"
                "  core.do      游戏字节码\n"
                "  res/         游戏资源(图片/音频)\n")

    print(f"成品已生成: {out}")
    print(f"  galrun.exe  (发布版 VM,无控制台,自动读取 core.do)")
    print(f"  core.do     ({len(bc)} bytes)")
    if copied:
        print(f"  res/        (已复制 {copied} 个资源文件)")
    print(f"  启动游戏.bat")
    print("运行时无需 Python;双击 启动游戏.bat 即可运行。")
    return 0


def _find_zig():
    """在 ziglang pip 包中查找 zig 可执行文件(便携 C 编译器)。"""
    try:
        import ziglang
        import pathlib
        p = pathlib.Path(ziglang.__file__).parent / ("zig.exe" if os.name == "nt" else "zig")
        return str(p) if p.exists() else None
    except Exception:
        return None


def _compiler_env():
    """把 C 编译器的缓存与临时目录收进工作区内。

    为什么需要:zig 默认用 %LOCALAPPDATA%\\zig\\tmp 与 %TEMP%。在受限/沙箱环境里
    这些位置在工作区之外、写不进去,构建会以

        error: failed to create output directory '...': AccessDenied

    失败 —— 报错看起来像编译器坏了,其实只是缓存目录不可写。
    `build_libs.bat` 一直在做这件事,但 `main.py build-vm` 之前没做,
    于是同一个环境下「库能构建、VM 不能构建」。
    """
    env = dict(os.environ)
    cache = os.path.join(ROOT, "_zigcache")
    tmp = os.path.join(ROOT, "_zigtmp")
    for d in (cache, tmp):
        try:
            os.makedirs(d, exist_ok=True)
        except OSError:
            pass
    env["ZIG_GLOBAL_CACHE_DIR"] = cache
    env["ZIG_LOCAL_CACHE_DIR"] = cache
    env["TMP"] = tmp
    env["TEMP"] = tmp
    return env


def cmd_build(args):
    src = os.path.join(ROOT, "vm", "vm.c")
    compiler = shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
    zig = None
    if compiler is None:
        zig = _find_zig()
        if zig is None:
            print("未找到 C 编译器(gcc/clang/zig),可先: pip install ziglang", file=sys.stderr)
            return 1

    def build_one(out_name, extra=None):
        out = os.path.join(ROOT, "vm", out_name)
        if zig:
            cmd = [zig, "cc", "-target", "x86_64-windows-gnu",
                   "-O2", "-Wall", "-Wextra", "-std=c11"]
            if extra:
                cmd += extra
            cmd += ["-o", out, src]
        else:
            cmd = [compiler, "-O2", "-Wall", "-Wextra", "-std=c11"]
            if extra:
                cmd += extra
            cmd += ["-o", out, src]
        print(" ".join(cmd))
        try:
            subprocess.run(cmd, check=True, env=_compiler_env())
        except subprocess.CalledProcessError as e:
            print(f"构建 {out_name} 失败(编译器退出码 {e.returncode})。", file=sys.stderr)
            print("若报 'failed to create output directory ... AccessDenied',"
                  "说明编译器的缓存/临时目录不可写:", file=sys.stderr)
            print("  本命令已把 ZIG_GLOBAL_CACHE_DIR/TMP 指向工作区内的 "
                  "_zigcache/ 与 _zigtmp/,仍在报错请检查这两个目录的权限。",
                  file=sys.stderr)
            print("  另一个已知原因:改过编译参数后 zig 复用了旧的失败缓存 —— "
                  "删掉 _zigcache/ 再试。", file=sys.stderr)
            return False
        print(f"已构建: {out}")
        return True

    # 三个版本:普通 / 无控制台 / 发布版(读 core.do)
    if not build_one(vm_exe_name()):            # vm.exe
        return 1
    if zig:
        if not build_one("vmnc.exe", ["-DPUBLISH_BUILD"]):
            return 1
        if not build_one("galrun.exe", ["-DPUBLISH_BUILD"]):
            return 1
    else:
        print("提示: 使用非 zig 编译器,仅构建普通 vm.exe(无控制台/发布版请用 zig)")
    return 0


def cmd_roundtrip(args):
    """校验:汇编 → 字节码 → 汇编 → 字节码 往返一致。"""
    with open(args.bc, "rb") as f:
        data = f.read()
    text1 = disassemble(data)
    funcs = parse_asm_text(text1)
    data2 = assemble(funcs)
    if data == data2:
        print("往返校验通过:字节码与反汇编后重新汇编一致")
    else:
        print("往返校验失败:字节码不一致!", file=sys.stderr)
        return 1
    return 0


def cmd_ide(args):
    """启动 DEXIDE 集成开发环境。"""
    from dexide.ide import main as ide_main
    import os as _os
    root = _os.getcwd()
    start = None
    if args.path:
        if _os.path.isdir(args.path):
            root = _os.path.abspath(args.path)
        elif _os.path.isfile(args.path):
            start = _os.path.abspath(args.path)
            root = _os.path.dirname(start)
    ide_main(root_dir=root, start_file=start)
    return 0


def cmd_designer(args):
    """启动 EGUI Designer(图形化界面设计器,生成 .dex 代码)。"""
    from dexide.designer import launch
    launch(design_path=args.path)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="dexcode",
        description="DEXCODE 工具链:源码 → 汇编(IR) → 字节码 → C VM 执行 / 反汇编",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("compile", help="源码 → 汇编 + 字节码")
    p.add_argument("src", help="DexLang 源码文件 (.dex)")
    p.add_argument("-o", help="输出字节码路径")
    p.add_argument("-L", "--lib-dir", action="append", default=[DEFAULT_LIBS],
                   dest="include_dirs", metavar="DIR",
                   help="include 库定义搜索目录(默认 libs/,可多次指定)")
    p.add_argument("--no-asm", action="store_true", help="不生成汇编文件")
    p.add_argument("--rel-lib", action="store_true",
                   help="动态库路径保留相对原样(发布自包含,VM 从运行目录解析 DLL)")
    p.set_defaults(func=cmd_compile)

    p = sub.add_parser("asm", help="汇编文本 → 字节码")
    p.add_argument("src", help="汇编文件 (.dxasm)")
    p.add_argument("-o", help="输出字节码路径")
    p.set_defaults(func=cmd_asm)

    p = sub.add_parser("disasm", help="字节码 → 汇编文本")
    p.add_argument("src", help="字节码文件 (.dexbc)")
    p.add_argument("-o", help="输出汇编路径")
    p.set_defaults(func=cmd_disasm)

    p = sub.add_parser("run", help="用 C VM 执行字节码")
    p.add_argument("bc", help="字节码文件 (.dexbc)")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("build-vm", help="编译 C 字节码解释器")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("roundtrip", help="字节码 → 汇编 → 字节码 往返校验")
    p.add_argument("bc", help="字节码文件 (.dexbc)")
    p.set_defaults(func=cmd_roundtrip)

    p = sub.add_parser("ide", help="启动 DEXIDE 集成开发环境")
    p.add_argument("path", nargs="?", default=None, help="项目文件夹或要打开的文件")
    p.add_argument("-L", "--lib-dir", action="append", default=[DEFAULT_LIBS],
                   dest="include_dirs", metavar="DIR", help="include 搜索目录")
    p.set_defaults(func=cmd_ide)

    p = sub.add_parser("designer", help="启动 EGUI Designer(图形化界面设计器)")
    p.add_argument("path", nargs="?", default=None, help="要打开的设计文件 (.egui)")
    p.set_defaults(func=cmd_designer)

    p = sub.add_parser("release", help="打包 GAL/程序为可独立发布的成品目录")
    p.add_argument("src", help="DexLang 源码文件 (.dex)")
    p.add_argument("-o", "--out", default=None, help="输出成品目录(默认 ./release)")
    p.add_argument("-r", "--res", action="append", default=None,
                   dest="res", metavar="DIR", help="要随成品分发的资源目录(可多次)")
    p.add_argument("--title", default=None, help="游戏标题(用于说明文件)")
    p.add_argument("-L", "--lib-dir", action="append", default=[DEFAULT_LIBS],
                   dest="include_dirs", metavar="DIR", help="include 搜索目录")
    p.set_defaults(func=cmd_release)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except DexError as e:
        print(e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
