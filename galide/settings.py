"""galide.settings — 用户设置持久化(~/.galide.json)。"""

import json
import os


def _cfg_path():
    return os.path.join(os.path.expanduser("~"), ".galide.json")


def load() -> dict:
    try:
        with open(_cfg_path(), "r", encoding="utf-8") as f:
            d = json.load(f)
        if isinstance(d, dict):
            return d
    except Exception:
        pass
    return {}


def save(d: dict):
    try:
        with open(_cfg_path(), "w", encoding="utf-8") as f:
            json.dump(d, f, ensure_ascii=False, indent=2)
    except Exception:
        pass


def get_project_root() -> str:
    d = load()
    r = d.get("project_root", "")
    return r if r and os.path.isdir(r) else ""


def set_project_root(path: str):
    d = load()
    d["project_root"] = path
    save(d)


def get_engine_dll() -> str:
    d = load()
    return d.get("engine_dll", "")


def set_engine_dll(path: str):
    d = load()
    d["engine_dll"] = path
    save(d)
