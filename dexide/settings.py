"""DEXIDE 用户设置:vm.exe 位置、库目录。

持久化到用户目录 ~/.dexide.json(JSON),供 IDE 的「设置」对话框读写。
"""

import json
import os

CONFIG_FILE = os.path.join(os.path.expanduser("~"), ".dexide.json")


def load(path=None):
    """加载设置;文件不存在/损坏时返回空设置。"""
    path = path or CONFIG_FILE
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            data = {}
    except (OSError, ValueError):
        data = {}
    data.setdefault("vm_exe", "")      # 自定义 C 虚拟机路径,空 = 用项目默认
    data.setdefault("lib_dirs", [])    # 自定义库目录列表(include 搜索)
    return data


def save(data, path=None):
    """保存设置;成功返回 True。"""
    path = path or CONFIG_FILE
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        return True
    except OSError:
        return False
