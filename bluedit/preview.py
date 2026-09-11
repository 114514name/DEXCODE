"""bluedit.preview — 引擎预览(嵌入 + 从入口按连线运行模拟器)。"""

import ctypes

from .model import (N_ENTRY, N_EXIT, N_SPEAK, N_LONG, N_SPRITE, N_CHOICE,
                    N_CODE, N_DEFVAR, N_SETVAR, N_IF, N_GETVAR, N_LOGIC,
                    N_MATH, N_FLOW, char_state_path)
from . import vars as vars_mod

user32 = ctypes.windll.user32
GWL_STYLE = -16
WS_CHILD = 0x40000000
WS_POPUP = 0x80000000
WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000
SWP_NOZORDER = 0x0004
SW_SHOW = 5
HWND_TOP = 0


class Preview:
    def __init__(self, parent_widget, engine):
        self.parent = parent_widget
        self.engine = engine
        self.hwnd = None
        self.active = False
        self._embedded = False
        self._last_err = ""

    @staticmethod
    def _find_gal():
        result = []
        WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

        def _enum(h, _):
            buf = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(h, buf, 64)
            if buf.value == "DexGALWindow":
                result.append(h)
                return False
            return True
        user32.EnumWindows(WNDENUMPROC(_enum), 0)
        return result[0] if result else None

    @property
    def parent_hwnd(self):
        try:
            return int(self.parent.winfo_id())
        except Exception:
            return 0

    def start(self):
        if self.active:
            return True
        if not self.engine.loaded and not self.engine.load():
            self._last_err = "DLL 加载失败: " + self.engine.dll_path
            return False
        stale = self._find_gal()
        if stale:
            user32.DestroyWindow(ctypes.c_void_p(stale))
        rc = self.engine.init(960, 540)
        if rc == 0:
            self._last_err = "引擎窗口初始化失败"
            return False
        self.hwnd = self._find_gal()
        if not self.hwnd:
            self._last_err = "引擎窗口未找到"
            self.engine.close()
            return False
        if not self._embedded:
            self.engine.set_title("GAL 预览")
            user32.SetParent(ctypes.c_void_p(self.hwnd),
                             ctypes.c_void_p(self.parent_hwnd))
            style = user32.GetWindowLongPtrW(ctypes.c_void_p(self.hwnd), GWL_STYLE)
            newstyle = (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME)) | WS_CHILD
            user32.SetWindowLongPtrW(ctypes.c_void_p(self.hwnd), GWL_STYLE, newstyle)
            self._embedded = True
        self._last_err = ""
        user32.ShowWindow(ctypes.c_void_p(self.hwnd), SW_SHOW)
        self.active = True
        return True

    def resize(self, x, y, w, h):
        if not self.active or not self.hwnd:
            return
        user32.SetWindowPos(ctypes.c_void_p(self.hwnd), HWND_TOP, x, y, w, h,
                            SWP_NOZORDER)

    def poll(self):
        if self.active:
            self.engine.poll()

    def close(self):
        if self.active and self.hwnd:
            user32.ShowWindow(ctypes.c_void_p(self.hwnd), 0)
            self.active = False

    def destroy(self):
        if self.hwnd:
            try:
                self.engine.close()
            except Exception:
                pass
            self.active = False
            self._embedded = False
            self.hwnd = None

    def apply_scene(self, project, sc):
        if not self.active:
            return
        eng = self.engine
        if sc.useBgImg and sc.bg:
            if eng.bg(sc.bg) != 0:
                eng.bg_color(sc.bgColor)
        else:
            eng.bg_color(sc.bgColor)
        self.poll()

    def reset_vars(self, project, scene_idx):
        """预览开始时把 DEFVAR 变量设初值(可被模拟器覆盖)。"""
        for sc in project.scenes:
            for n in sc.nodes:
                if n.type == N_DEFVAR:
                    sname = vars_mod.storage_name(n.var_name, scene_idx, n.scope)
                    self.engine.var_set(sname, n.var_value or "")


class Sim:
    """从入口沿 exec 连线运行的预览模拟器(状态机)。"""

    def __init__(self, preview, project, scene_idx):
        self.pv = preview
        self.project = project
        self.eng = preview.engine
        self.scene_idx = scene_idx
        self.node = None
        self.state = "idle"      # idle / wait_click / wait_pick / long_line
        self.done = False
        self._long_i = 0
        self._visited = set()

    def start(self):
        sc = self.project.scenes[self.scene_idx]
        self.pv.apply_scene(self.project, sc)
        entry = sc.node(sc.entry_id)
        self.node = sc.find_exec_target(entry, 0) if entry else None
        self.state = "idle"
        self.done = False
        self._visited = set()

    def poll(self):
        """每帧调用,推进状态机。"""
        if self.done:
            return
        if self.state == "wait_click":
            if self.eng.text_done() and self.eng.clicked():
                self.eng.text_clear()
                self.eng.clicked()  # 消费
                self.state = "idle"
                self.node = self._next()
            return
        if self.state == "wait_pick":
            p = self.eng.picked()
            if p >= 0:
                self.eng.hide_choices()
                self.eng.text_clear()
                self.state = "idle"
                self.node = self._next(p)
            return
        if self.state == "long_line":
            if self.eng.text_done() and self.eng.clicked():
                self.eng.text_clear()
                self.state = "idle"
                self._long_i += 1
            return
        if self.state == "idle":
            self._step()

    def _next(self, out_idx=0):
        sc = self.project.scenes[self.scene_idx]
        if self.node is None:
            return None
        return sc.find_exec_target(self.node, out_idx)

    def _step(self):
        if self.node is None:
            self._end()
            return
        if self.node.id in self._visited:
            self._end()
            return
        self._visited.add(self.node.id)
        sc = self.project.scenes[self.scene_idx]
        t = self.node.type
        eng = self.eng
        if t == N_EXIT:
            self._jump(0)
        elif t == N_FLOW:
            self._jump(self.node.flow_target)
        elif t == N_SPEAK:
            if self.node.use_char and self.node.char_idx >= 0:
                p = char_state_path(self.project, self.node.char_idx, self.node.state_idx)
                if p:
                    eng.sprite(0, p)
                    eng.sprite_pos_mode(0, self.node.text_pos)
                    eng.sprite_show(0, 1)
            if self.node.show_name and self.node.name:
                eng.speaker(self.node.name)
            eng.text_pos(self.node.text_pos)
            if self.node.text:
                eng.text(self.node.text)
                self.state = "wait_click"
            else:
                self.state = "idle"
                self.node = self._next()
        elif t == N_LONG:
            if self._long_i >= len(self.node.lines):
                self._long_i = 0
                self.state = "idle"
                self.node = self._next()
                return
            ci, txt = self.node.lines[self._long_i]
            if txt:
                ch = None
                if 0 <= ci < len(self.project.chars):
                    ch = self.project.chars[ci]
                    if ch.name:
                        eng.speaker(ch.name)
                    st = ""
                    for s in range(ch.nStates):
                        if ch.states[s]:
                            st = ch.states[s]
                            break
                    if st:
                        eng.sprite(0, st)
                        eng.sprite_pos_mode(0, 1)
                        eng.sprite_show(0, 1)
                eng.text(txt)
                self.state = "long_line"
            else:
                self._long_i += 1
        elif t == N_SPRITE:
            p = char_state_path(self.project, self.node.char_idx, self.node.state_idx) \
                if self.node.use_char else self.node.spr_path
            if p:
                eng.sprite(self.node.spr_layer, p)
                if self.node.use_xy:
                    eng.sprite_xy(self.node.spr_layer, self.node.spr_x, self.node.spr_y)
                else:
                    eng.sprite_pos_mode(self.node.spr_layer, self.node.spr_pos_mode)
                eng.sprite_scale(self.node.spr_layer, self.node.spr_scale)
                eng.sprite_alpha(self.node.spr_layer, self.node.spr_alpha)
                eng.sprite_show(self.node.spr_layer, 1)
            self.node = self._next()
        elif t == N_CHOICE:
            if self.node.choice_text:
                eng.text(self.node.choice_text)
            opts = self.node.options or [""]
            for i, o in enumerate(opts[:4]):
                eng.set_choice(i, o)
            eng.show_choices()
            self.state = "wait_pick"
        elif t == N_DEFVAR:
            sname = vars_mod.storage_name(self.node.var_name, self.scene_idx, self.node.scope)
            eng.var_set(sname, self.node.var_value or "")
            self.node = self._next()
        elif t == N_SETVAR:
            sname = vars_mod.storage_name(self.node.var_name, self.scene_idx, self.node.scope)
            if self.node.set_mode == 1:
                val = self._value(1)
            else:
                val = self.node.value or "0"
            eng.var_set(sname, str(val))
            self.node = self._next()
        elif t == N_IF:
            cond = self._value(1) if self.node.mode == 1 else self.node.cond
            ok = self._eval_cond(cond)
            self.node = self._next(0 if ok else 1)
        elif t == N_CODE:
            self.node = self._next()

    def _value(self, pin_idx):
        """求 value 输入的值(字符串)。"""
        sc = self.project.scenes[self.scene_idx]
        src = sc.find_value_source(self.node, pin_idx)
        if src is not None:
            return self._node_value(src)
        pin = self.node.inputs[pin_idx]
        if pin.source == "variable":
            return self.eng.var_get(vars_mod.storage_name(pin.variable, self.scene_idx, 0))
        return pin.literal

    def _node_value(self, n):
        eng = self.eng
        if n.type == N_GETVAR:
            return eng.var_get(vars_mod.storage_name(n.var_name, self.scene_idx, 0))
        if n.type in (N_LOGIC, N_MATH):
            a = self._node_value_expr(n, 0)
            b = self._node_value_expr(n, 1)
            try:
                fa, fb = float(a), float(b)
            except Exception:
                fa, fb = 0.0, 0.0
            op = n.op
            if n.type == N_LOGIC:
                r = {"==": fa == fb, "!=": fa != fb, ">": fa > fb,
                     "<": fa < fb, ">=": fa >= fb, "<=": fa <= fb}.get(op, False)
                return "1" if r else "0"
            r = {"+": fa + fb, "-": fa - fb, "*": fa * fb, "/": (fa / fb if fb else 0),
                 "%": fa % fb if fb else 0, "^": fa ** fb}.get(op, fa)
            return str(r)
        return "0"

    def _node_value_expr(self, n, idx):
        sc = self.project.scenes[self.scene_idx]
        src = sc.find_value_source(n, idx)
        if src is not None:
            return self._node_value(src)
        pin = n.inputs[idx]
        if pin.source == "variable":
            return self.eng.var_get(vars_mod.storage_name(pin.variable, self.scene_idx, 0))
        return pin.literal

    def _eval_cond(self, cond):
        if isinstance(cond, str) and cond.strip() in ("1", "true"):
            return True
        if isinstance(cond, str) and cond.strip() in ("0", "false"):
            return False
        try:
            return float(cond) != 0
        except Exception:
            return bool(cond)

    def _jump(self, target):
        n = len(self.project.scenes)
        if target == 1 and 0 <= self.node.flow_scene < n:
            self.scene_idx = self.node.flow_scene
        elif self.scene_idx + 1 < n:
            self.scene_idx += 1
        else:
            self.done = True
            return
        sc = self.project.scenes[self.scene_idx]
        self.pv.apply_scene(self.project, sc)
        entry = sc.node(sc.entry_id)
        self.node = sc.find_exec_target(entry, 0) if entry else None
        self._visited = set()

    def _end(self):
        self.done = True
