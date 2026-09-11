"""bluedit.sceneio — 蓝图场景文件读写(.bluescene, JSON)。"""

import json
import os

from .model import (project_to_dict, project_from_dict, project_defaults,
                    menu_to_dict, menu_from_dict)


def save_menu(path, menu):
    """把单个 MENU 场景保存为 .menubluescene 文件。"""
    try:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(menu_to_dict(menu), f, ensure_ascii=False, indent=1)
        return True
    except Exception:
        return False


def load_menu(path):
    """从 .menubluescene 文件读取 MENU 场景;失败返回 None。"""
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            d = json.load(f)
        if not isinstance(d, dict):
            return None
        return menu_from_dict(d)
    except Exception:
        return None


def save_project(path, project):
    d = project_to_dict(project)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(d, f, ensure_ascii=False, indent=1)


def load_project(path):
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8-sig") as f:
        d = json.load(f)
    if not isinstance(d, dict):
        return None
    return project_from_dict(d)


def is_blueprint_file(path):
    """判断是否蓝图格式(JSON 开头)。"""
    if not os.path.isfile(path):
        return False
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            head = f.read(256).lstrip()
        return head.startswith("{") or head.startswith("[")
    except Exception:
        return False
