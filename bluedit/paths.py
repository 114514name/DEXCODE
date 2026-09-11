"""bluedit.paths — 应用根目录定位(兼容 nuitka 打包后的 frozen 环境)。

开发时:包在项目根下一层 → 根 = dirname(dirname(__file__))。
打包后(exe):根 = exe 所在目录(数据文件 vm/libs/docs/examples 放在 exe 旁)。
"""

import os
import sys


def frozen() -> bool:
    """是否运行在打包后的 exe 中(nuitka 设 sys.frozen)。"""
    return bool(getattr(sys, "frozen", False))


def project_root() -> str:
    """应用根目录:发布版 = exe 目录;开发版 = 项目根。"""
    if frozen():
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def libs_dir() -> str:
    return os.path.join(project_root(), "libs")


def vm_dir() -> str:
    return os.path.join(project_root(), "vm")


def engine_dll_path() -> str:
    """libdexxgal.dll 默认位置(exe 旁 libs/gal/libdexxgal.dll)。"""
    return os.path.join(project_root(), "libs", "gal", "libdexxgal.dll")
