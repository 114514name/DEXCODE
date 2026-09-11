"""galide.engine — ctypes 封装 libdexxgal.dll(GAL 引擎)。

引擎导出函数均为 C 的 int64_t 返回、int64_t/const char* 参数(见 gal.dexdef),
因此 ctypes 用 c_longlong / c_char_p。
"""

import ctypes
import os
import sys


class Engine:
    def __init__(self, dll_path=None):
        if dll_path is None:
            # 默认:项目根 libs/gal/libdexxgal.dll
            root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
            dll_path = os.path.join(root, "libs", "gal", "libdexxgal.dll")
        self.dll_path = dll_path
        self.lib = None
        self.loaded = False

    def load(self) -> bool:
        if not os.path.isfile(self.dll_path):
            return False
        try:
            self.lib = ctypes.CDLL(os.path.abspath(self.dll_path))
        except Exception:
            return False
        ll = ctypes.c_longlong
        cp = ctypes.c_char_p
        L = self.lib
        L.gal_init.restype = ll; L.gal_init.argtypes = [ll, ll]
        L.gal_poll.restype = ll; L.gal_poll.argtypes = []
        L.gal_close.restype = ll; L.gal_close.argtypes = []
        L.gal_wait.restype = ll; L.gal_wait.argtypes = [ll]
        L.gal_running.restype = ll; L.gal_running.argtypes = []
        for name in ("gal_bg", "gal_set_title", "gal_text", "gal_speaker",
                     "gal_play_bgm", "gal_play_se", "gal_screenshot"):
            f = getattr(L, name, None)
            if f is not None:
                f.restype = ll; f.argtypes = [cp]
        for name, nargs in (("gal_bg_color", 1), ("gal_text_color", 1),
                            ("gal_text_size", 1), ("gal_text_pos", 1),
                            ("gal_box", 1), ("gal_box_autofit", 1),
                            ("gal_skip", 1), ("gal_volume", 1),
                            ("gal_sprite_show", 2), ("gal_sprite_pos", 2),
                            ("gal_sprite_scale", 2), ("gal_sprite_pos_mode", 2),
                            ("gal_sprite_autofit", 2), ("gal_sprite_anim", 2),
                            ("gal_box_style", 2)):
            f = getattr(L, name, None)
            if f is not None:
                f.restype = ll; f.argtypes = [ll] * nargs
        L.gal_sprite.restype = ll; L.gal_sprite.argtypes = [ll, cp]
        L.gal_set_choice.restype = ll; L.gal_set_choice.argtypes = [ll, cp]
        L.gal_text_done.restype = ll; L.gal_text_done.argtypes = []
        L.gal_text_clear.restype = ll; L.gal_text_clear.argtypes = []
        L.gal_clicked.restype = ll; L.gal_clicked.argtypes = []
        L.gal_picked.restype = ll; L.gal_picked.argtypes = []
        L.gal_show_choices.restype = ll; L.gal_show_choices.argtypes = []
        L.gal_hide_choices.restype = ll; L.gal_hide_choices.argtypes = []
        L.gal_stop_bgm.restype = ll; L.gal_stop_bgm.argtypes = []
        self.loaded = True
        return True

    # ---- 基础 ----
    def init(self, w=960, h=540):
        if self.loaded:
            return self.lib.gal_init(w, h)
        return 0

    def poll(self):
        if self.loaded:
            self.lib.gal_poll()

    def close(self):
        if self.loaded:
            self.lib.gal_close()

    def wait(self, ms):
        if self.loaded:
            self.lib.gal_wait(ms)

    def running(self):
        return self.loaded and self.lib.gal_running() != 0

    # ---- 背景 / 立绘 / 文本 / 选项 / 音频 ----
    def _s(self, v):
        return v.encode("utf-8") if isinstance(v, str) else v

    def bg(self, path):
        if self.loaded:
            return self.lib.gal_bg(self._s(path))
        return 0

    def bg_color(self, color):
        if self.loaded:
            self.lib.gal_bg_color(color)

    def sprite(self, k, path):
        if self.loaded:
            return self.lib.gal_sprite(k, self._s(path))
        return 0

    def sprite_show(self, k, on):
        if self.loaded:
            self.lib.gal_sprite_show(k, on)

    def sprite_pos(self, k, x):
        if self.loaded:
            self.lib.gal_sprite_pos(k, x)

    def sprite_scale(self, k, pct):
        if self.loaded:
            self.lib.gal_sprite_scale(k, pct)

    def sprite_pos_mode(self, k, mode):
        if self.loaded:
            self.lib.gal_sprite_pos_mode(k, mode)

    def sprite_autofit(self, k, on):
        if self.loaded:
            self.lib.gal_sprite_autofit(k, on)

    def sprite_anim(self, k, anim):
        if self.loaded:
            self.lib.gal_sprite_anim(k, anim)

    def text(self, t):
        if self.loaded:
            self.lib.gal_text(self._s(t))

    def text_done(self):
        return self.loaded and self.lib.gal_text_done() != 0

    def text_clear(self):
        if self.loaded:
            self.lib.gal_text_clear()

    def speaker(self, name):
        if self.loaded:
            self.lib.gal_speaker(self._s(name))

    def text_pos(self, pos):
        if self.loaded:
            self.lib.gal_text_pos(pos)

    def box(self, on):
        if self.loaded:
            self.lib.gal_box(on)

    def box_style(self, avatar, name):
        if self.loaded:
            self.lib.gal_box_style(avatar, name)

    def box_autofit(self, on):
        if self.loaded:
            self.lib.gal_box_autofit(on)

    def set_choice(self, i, text):
        if self.loaded:
            self.lib.gal_set_choice(i, self._s(text))

    def show_choices(self):
        if self.loaded:
            self.lib.gal_show_choices()

    def hide_choices(self):
        if self.loaded:
            self.lib.gal_hide_choices()

    def picked(self):
        return self.lib.gal_picked() if self.loaded else -1

    def clicked(self):
        return self.loaded and self.lib.gal_clicked() != 0

    def play_bgm(self, path):
        if self.loaded:
            self.lib.gal_play_bgm(self._s(path))

    def play_se(self, path):
        if self.loaded:
            self.lib.gal_play_se(self._s(path))

    def stop_bgm(self):
        if self.loaded:
            self.lib.gal_stop_bgm()

    def volume(self, v):
        if self.loaded:
            self.lib.gal_volume(v)

    def set_title(self, title):
        if self.loaded:
            self.lib.gal_set_title(self._s(title))
