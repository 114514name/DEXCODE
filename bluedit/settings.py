"""bluedit.settings — 用户设置持久化(~/.galide.json)。"""

import json
import os

from . import paths


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
    p = d.get("engine_dll", "")
    return p if p and os.path.isfile(p) else (paths.engine_dll_path() if os.path.isfile(paths.engine_dll_path()) else "")


def set_engine_dll(path: str):
    d = load()
    d["engine_dll"] = path
    save(d)


def get_vm_path() -> str:
    d = load()
    p = d.get("vm", "")
    if p and os.path.isfile(p):
        return p
    # 默认:发布版 exe 旁的 vm/vm.exe
    cand = os.path.join(paths.vm_dir(), "vm.exe")
    return cand if os.path.isfile(cand) else ""


def set_vm_path(path: str):
    """保存 VM 路径。

    app.py 的设置对话框一直调用本函数,但此前它并不存在,导致
    AttributeError 中断保存闭包 —— VM 路径、字节码导出、链接模式、
    自动保存全都静默丢失。补上后这些设置才能真正持久化。
    """
    d = load()
    d["vm"] = path
    save(d)


def get_compiler() -> str:
    """编译器(Python 解释器)路径;留空用当前解释器跑 main.py compile。"""
    d = load()
    return d.get("compiler", "")


def set_compiler(path: str):
    d = load()
    d["compiler"] = path
    save(d)


def get_export_bytecode() -> bool:
    """完整导出时是否同时输出字节码(.dexbc)。默认关闭。"""
    return bool(load().get("export_bytecode", False))


def set_export_bytecode(v: bool):
    d = load()
    d["export_bytecode"] = bool(v)
    save(d)


def get_gal_link_mode() -> str:
    """库链接模式:static(引擎内嵌进字节码,默认)/ portable(文件夹自包含,带 DLL+相对路径)。"""
    return load().get("gal_link_mode", "static")


def set_gal_link_mode(mode: str):
    d = load()
    d["gal_link_mode"] = mode if mode in ("static", "portable") else "static"
    save(d)


# ---- 外观风格(整体主题:对话框/选项/菜单颜色与特效) ----
GAL_THEMES = ["默认", "中世纪", "史诗", "简约", "科幻", "樱花", "风雅", "压抑", "奇特"]


def get_gal_theme() -> str:
    """当前外观风格(默认"默认")。导出时按此调用引擎 gal_theme_apply。"""
    t = load().get("gal_theme", "默认")
    return t if t in GAL_THEMES else "默认"


def set_gal_theme(name: str):
    d = load()
    d["gal_theme"] = name if name in GAL_THEMES else "默认"
    save(d)


def get_trail_enabled() -> bool:
    """鼠标拖尾开关(默认开)。运行时引擎也可用 gal_trail_enable 动态开关。"""
    return bool(load().get("gal_trail", True))


def set_trail_enabled(v: bool):
    d = load()
    d["gal_trail"] = bool(v)
    save(d)


def get_autosave() -> dict:
    """间隔自动保存:{enabled, interval(分钟)}。默认关闭。"""
    a = load().get("autosave", {})
    if not isinstance(a, dict):
        a = {}
    return {"enabled": bool(a.get("enabled", False)),
            "interval": int(a.get("interval", 5) or 5)}


def set_autosave(enabled: bool, interval: int):
    d = load()
    d["autosave"] = {"enabled": bool(enabled),
                     "interval": max(1, int(interval))}
    save(d)


# ---- 块默认属性(全局设置,新创建的块遵循;单个块仍可单独改) ----
DEFAULT_BLOCK_DEFAULTS = {
    # 文本(说话/长对话/选项框共用)
    "text_mode": 0,          # 0逐字 1直接全部 2先空框点击
    "text_speed": 60,        # 逐字间隔毫秒/字(0=引擎默认40)
    # 说话
    "text_pos": 0,           # 文字位置 0中间 1左 2右 3偏左 4偏右
    "show_name": 1,          # 显示人名
    "use_char": 0,           # 使用人物集
    # 立绘
    "spr_pos_mode": 0,
    "spr_layer": 0,
    "spr_scale": 100,
    "spr_alpha": 255,
    "spr_anim": 0,           # 0无 1呼吸 2淡入 3上浮 4抖动 5脉冲 6消失
    "spr_fit": 0,            # 立绘适配:0原大小 1自动适配(裁剪+高78%) 2完整显示 3填满
    # 背景
    "bg_mode": 0,            # 0图片 1纯色
    "bg_color": 0x000000,
    "bg_fit": 0,             # 0拉伸 1原大小 2保持比例完整 3保持比例填满
}


def get_block_defaults() -> dict:
    """读取全局块默认属性(缺省用内置默认,未改动项不写入配置文件)。"""
    d = load()
    bd = d.get("block_defaults", {})
    if not isinstance(bd, dict):
        bd = {}
    merged = dict(DEFAULT_BLOCK_DEFAULTS)
    merged.update(bd)
    return merged


def set_block_defaults(bd: dict):
    d = load()
    d["block_defaults"] = bd
    save(d)
