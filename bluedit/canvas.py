"""bluedit.canvas — UE 蓝图式画布(tkinter Canvas)。

渲染节点(标题条+输入/输出引脚)、连线(exec 金色/ value 蓝色贝塞尔)、
支持:选中节点、拖拽移动、从引脚拖线连接(正/反向)、右键菜单、平移。
"""

import tkinter as tk
import tkinter.font as tkfont
import time
import math

from . import theme as T
from .model import (NODE_NAMES, NODE_COLORS, NODE_ICONS, PALETTE,
                    N_ENTRY, N_EXIT, N_SPEAK, N_LONG, N_SPRITE, N_BG,
                    N_CHOICE, N_CODE, N_DEFVAR, N_SETVAR, N_IF, N_GETVAR,
                    N_LOGIC, N_MATH, N_FLOW, N_FX_ADD, N_FX_OFF, N_FX_CLEAR,
                    N_MENU, N_TEXT, N_SAY, N_INPUT, N_RANDOM, N_WAIT,
                    N_DOUT, N_DREF, N_TEXTLIT, N_NUMLIT, N_BOLLIT,
                    N_BGM, N_SE, N_SOUND_STOP, N_SOUND_SWAP,
                    N_BG_TRANS, N_SCENE_TRANS, N_TOAST,
                    N_SAVE, N_LOAD, N_FILE_WRITE, N_FILE_READ,
                    N_CHECKPOINT, N_JUMP,
                    BG_FIT_NAMES, FX_NAMES, Link)

NODE_W = 230
TITLE_H = 34
PIN_H = 26
PIN_R = 6
PAD = 8


class BlueCanvas(tk.Canvas):
    def __init__(self, master, app, **kw):
        super().__init__(master, bg=T.BG, highlightthickness=0, bd=0, **kw)
        self.app = app
        self.vx = 0
        self.vy = 0
        self.zoom = 1.0
        self.selected = None
        self._drag = None       # (node_id, sx, sy)
        self._pan = None
        self._wire = None       # (from_node, from_pin, kind, mx, my) 正向
        self._wire_rev = None   # (to_node, to_pin, kind, mx, my) 反向
        self._ghost = None      # (ntype, mx, my)
        self._last_render = 0.0  # 用于拖拽时节流重绘
        self._clipboard = None   # 复制的节点(剪贴板)

        self.bind("<Configure>", lambda e: self.render())
        self.bind("<ButtonPress-1>", self._on_press)
        self.bind("<B1-Motion>", self._on_motion)
        self.bind("<ButtonRelease-1>", self._on_release)
        self.bind("<ButtonPress-2>", self._on_pan_press)
        self.bind("<B2-Motion>", self._on_pan_motion)
        self.bind("<ButtonPress-3>", self._on_right)
        self.bind("<MouseWheel>", self._on_wheel)
        self.bind("<Motion>", self._on_move)
        self.bind("<Key>", self._on_key)

    # ---------------- 几何(支持缩放) ----------------
    def node_rect(self, n):
        z = self.zoom
        x0 = n.x * z + self.vx
        y0 = n.y * z + self.vy
        return x0, y0, x0 + self.node_width(n) * z, y0 + self.node_height(n) * z

    def node_width(self, n):
        """节点宽度:纯文本/长对话按内容自适应(最长 2 倍默认宽),其余固定。"""
        if n.type == N_TEXTLIT:
            w, _ = self._lit_layout(n)
            return w
        if n.type == N_LONG:
            w, _ = self._long_layout(n)
            return w
        return NODE_W

    def _long_layout(self, n):
        """长对话块排版:返回 (块宽, 总显示行数)。按所有行中最长宽度定宽,再按换行算总行数。"""
        chars = self.app.project.chars
        f = tkfont.Font(family=T.FONT[0], size=8)
        full = []
        for ci, txt in n.lines:
            who = ""
            if 0 <= ci < len(chars) and chars[ci].name:
                who = chars[ci].name + "："
            full.append((who + (txt or "")).replace("\n", " "))
        if not full:
            return NODE_W, 1
        maxw = max(f.measure(ln) for ln in full)
        inner_base = NODE_W - 24
        width = NODE_W
        if maxw > inner_base:
            width = min(max(maxw + 24, NODE_W), NODE_W * 2)
        inner = width - 24
        total = 0
        for ln in full:
            if not ln.strip():
                total += 1
            else:
                total += max(1, math.ceil(f.measure(ln) / inner))
        return width, total

    def _lit_layout(self, n):
        """纯文本块排版:返回 (块宽, 内容行数)。文本过长先加宽,再换行加高。"""
        text = n.lit_text or ""
        if not text:
            return NODE_W, 1
        f = tkfont.Font(family=T.FONT[0], size=9)
        w = f.measure(text)
        inner_base = NODE_W - 24
        width = NODE_W
        if w > inner_base:
            width = min(max(w + 24, NODE_W), NODE_W * 2)
        inner = width - 24
        lines = max(1, math.ceil(w / inner))
        return width, lines

    def node_height(self, n):
        rows = max(len(n.inputs), len(n.outputs), 1)
        h = TITLE_H + rows * PIN_H + PAD
        if n.type == N_LONG:
            _, tl = self._long_layout(n)
            h += tl * 16
        elif n.type == N_CHOICE:
            h += max(0, len(n.options) - 1) * 6
        elif n.type == N_TEXTLIT:
            _, lines = self._lit_layout(n)
            if lines > 1:
                h += (lines - 1) * 15
        return h

    def pin_pos(self, n, pin_idx, is_out):
        z = self.zoom
        rows = max(len(n.inputs), len(n.outputs), 1)
        if is_out:
            row = pin_idx + (rows - len(n.outputs))
            cx = n.x * z + self.vx + (self.node_width(n) - PIN_R - 2) * z
        else:
            row = pin_idx
            cx = n.x * z + self.vx + (PIN_R + 2) * z
        cy = n.y * z + self.vy + (TITLE_H + row * PIN_H + PIN_H // 2) * z
        return cx, cy

    def _font(self, size, bold=False):
        s = max(6, int(size * self.zoom))
        fam = T.FONT[0]
        return (fam, s, "bold") if bold else (fam, s)

    def s2w(self, mx, my):
        """画布像素坐标 → 场景世界坐标(含平移与缩放)。"""
        return (mx - self.vx) / self.zoom, (my - self.vy) / self.zoom

    # ---- 节点类型外观(子类可覆写用于菜单画布) ----
    def _colors(self):
        return NODE_COLORS

    def _icons(self):
        return NODE_ICONS

    def _names(self):
        return NODE_NAMES

    def _palette(self):
        return PALETTE

    def _undeletable(self):
        return (N_ENTRY, N_EXIT)

    def _zoom_at(self, factor, mx, my):
        old = self.zoom
        new = max(0.35, min(2.0, old * factor))
        cx = (mx - self.vx) / old
        cy = (my - self.vy) / old
        self.zoom = new
        self.vx = mx - cx * new
        self.vy = my - cy * new
        self.render()

    def pin_kind(self, n, pin_idx, is_out):
        pins = n.outputs if is_out else n.inputs
        if 0 <= pin_idx < len(pins):
            return pins[pin_idx].kind
        return 'exec'

    def hit_node(self, mx, my):
        sc = self.app.scene()
        if not sc:
            return None
        for n in reversed(sc.nodes):
            x0, y0, x1, y1 = self.node_rect(n)
            if x0 <= mx <= x1 and y0 <= my <= y1:
                return n
        return None

    def hit_pin(self, mx, my):
        sc = self.app.scene()
        if not sc:
            return None
        for n in sc.nodes:
            for i in range(len(n.outputs)):
                cx, cy = self.pin_pos(n, i, True)
                if abs(mx - cx) < 13 and abs(my - cy) < 13:
                    return (n, i, True)
            for i in range(len(n.inputs)):
                cx, cy = self.pin_pos(n, i, False)
                if abs(mx - cx) < 13 and abs(my - cy) < 13:
                    return (n, i, False)
        return None

    # ---------------- 渲染 ----------------
    def render(self):
        self.delete("all")
        self._draw_grid()
        sc = self.app.scene()
        if not sc:
            return
        for l in sc.links:
            self._draw_link(l)
        if self._wire:
            self._draw_wire_drag()
        if self._wire_rev:
            self._draw_wire_rev()
        for n in sc.nodes:
            self._draw_node(n, n.id == self.selected)
        if self._ghost:
            self._draw_ghost()
        self.create_text(8, 8, anchor="nw", fill=T.FG2, font=T.FONT_SM,
                         text="拖引脚连线 · 拖节点移动 · 滚轮缩放 · 中键拖平移 · 右键菜单")

    def _draw_grid(self):
        step = 24
        ox = self.vx % step
        oy = self.vy % step
        w, h = self.winfo_width(), self.winfo_height()
        for x in range(int(-ox), w, step):
            self.create_line(x, 0, x, h, fill=T.GRID)
        for y in range(int(-oy), h, step):
            self.create_line(0, y, w, y, fill=T.GRID)

    def _draw_node(self, n, selected):
        z = self.zoom
        x0, y0, x1, y1 = self.node_rect(n)
        color = self._colors().get(n.type, "#555")
        self.create_rectangle(x0 + 3 * z, y0 + 3 * z, x1 + 3 * z, y1 + 3 * z,
                              fill="#111", outline="")
        self.create_rectangle(x0, y0, x1, y1, fill="#2a2d37",
                              outline=T.ACCENT if selected else "#3a3e4c",
                              width=2 if selected else 1)
        th = TITLE_H * z
        self.create_rectangle(x0, y0, x1, y0 + th, fill=color, outline=color)
        icon = self._icons().get(n.type, "·")
        name = self._names().get(n.type, "?")
        self.create_text(x0 + 10 * z, y0 + th / 2, anchor="w", fill="#fff",
                         font=self._font(10, True), text=f"{icon} {name}")
        for i, pin in enumerate(n.inputs):
            cx, cy = self.pin_pos(n, i, False)
            pc = T.PIN_EXEC if pin.kind == 'exec' else T.PIN_VALUE
            if n.type == N_EXIT and i == 0 and pin.kind == 'exec':
                pc = "#ff5a6e"     # 出口输入:红色
            if n.type == N_DOUT and pin.kind == 'value':
                pc = "#2ecc8f"     # 数据输出点输入:青色
            r = PIN_R * z
            self.create_oval(cx - r, cy - r, cx + r, cy + r, fill=pc, outline="#fff")
            if pin.name:
                self.create_text(cx + 10 * z, cy, anchor="w", fill=T.FG,
                                 font=self._font(9), text=pin.name)
        for i, pin in enumerate(n.outputs):
            cx, cy = self.pin_pos(n, i, True)
            pc = T.PIN_EXEC if pin.kind == 'exec' else T.PIN_VALUE
            if n.type == N_ENTRY and pin.kind == 'exec':
                pc = "#3ddc84"     # 入口输出:绿色
            if n.type == N_DREF and pin.kind == 'value':
                pc = "#2ecc8f"     # 场景数据输出:青色
            if n.type == N_MENU and pin.kind == 'value':
                pc = "#2ecc8f"     # 菜单数据输出口(输入框内容):青色
            r = PIN_R * z
            self.create_oval(cx - r, cy - r, cx + r, cy + r, fill=pc, outline="#fff")
            if pin.name:
                self.create_text(cx - 10 * z, cy, anchor="e", fill=T.FG,
                                 font=self._font(9), text=pin.name)
        # 长对话:逐行显示新增对话
        if n.type == N_LONG:
            self._draw_long_lines(n, x0, y0, th)
        # 字面量块:块上直接显示数值(醒目)
        if n.type == N_TEXTLIT:
            self.create_text(x0 + 12 * z, y0 + th + 8 * z, anchor="nw",
                             width=(self.node_width(n) - 24) * z,
                             text=n.lit_text or "…",
                             fill="#e8e8f0", font=self._font(9), justify="left")
        elif n.type == N_NUMLIT:
            self.create_text(x0 + 12 * z, y0 + th + 8 * z, anchor="nw",
                             text=n.lit_num or "0",
                             fill="#ffd76e", font=self._font(12, True))
        elif n.type == N_BOLLIT:
            self.create_text(x0 + 12 * z, y0 + th + 8 * z, anchor="nw",
                             text="true" if n.lit_bool else "false",
                             fill="#7ee787" if n.lit_bool else "#ff7b72",
                             font=self._font(11, True))
        summary = self._node_summary(n)
        # 拖拽时跳过绘制节点摘要以减少重绘开销
        if summary and n.type != N_LONG and not self._drag:
            self.create_text(x0 + 10 * z, y0 + th + 4 * z, anchor="nw", fill=T.FG2,
                             font=self._font(9), text=summary, width=x1 - x0 - 20 * z)

    def _draw_long_lines(self, n, x0, y0, th):
        z = self.zoom
        chars = self.app.project.chars
        content_w = self.node_width(n) - 24
        f = tkfont.Font(family=T.FONT[0], size=8)
        y = y0 + th + 6 * z
        for ci, txt in n.lines:
            who = ""
            if 0 <= ci < len(chars) and chars[ci].name:
                who = chars[ci].name + "："
            line = (who + (txt or "")).replace("\n", " ")
            if not line.strip():
                line = "　"
            self.create_text(x0 + 12 * z, y, anchor="nw", width=content_w * z,
                             fill=T.FG, font=self._font(8), text=line, justify="left")
            w = f.measure(line)
            rows = max(1, math.ceil(w / content_w)) if line.strip() else 1
            y += rows * 16 * z

    def _node_summary(self, n):
        t = n.type
        if t == N_SPEAK:
            s = (n.name + "：") if n.show_name and n.name else ""
            return (s + n.text) or "点击右侧面板编辑"
        if t == N_LONG:
            return f"{len(n.lines)} 行对话"
        if t == N_SPRITE:
            src = f"人物[{n.char_idx}]" if n.use_char and n.char_idx >= 0 else (n.spr_path or "—")
            pos = f"({n.spr_x},{n.spr_y})" if n.use_xy else ['中间', '左', '右', '偏左', '偏右'][n.spr_pos_mode]
            return f"{src}  {pos}"
        if t == N_CHOICE:
            return (n.choice_text or "选项") + f"  [{len(n.options)} 个选项]"
        if t == N_CODE:
            return n.code_file or "未选代码文件"
        if t == N_DEFVAR:
            return f"{'全局' if n.scope == 0 else '局部'} {n.var_name} = {n.var_value}"
        if t == N_SETVAR:
            return f"{n.var_name} = {n.value if n.set_mode == 0 else '← value'}"
        if t == N_IF:
            return n.cond if n.mode == 0 else "← value 条件"
        if t == N_GETVAR:
            return f"→ {n.var_name}"
        if t == N_LOGIC:
            return f"{self._pin_expr(n, 0)} {n.op} {self._pin_expr(n, 1)}"
        if t == N_MATH:
            return f"{self._pin_expr(n, 0)} {n.op} {self._pin_expr(n, 1)}"
        if t == N_FLOW:
            return {0: "跳转到下一镜头", 1: f"跳转到镜头 {n.flow_scene + 1}", 2: "结束"}.get(n.flow_target, "")
        if t == N_MENU:
            return {0: "运行独占菜单", 1: "显示叠加菜单", 2: "隐藏叠加菜单",
                    3: "发送信号"}.get(n.action, "")
        if t == N_TEXT:
            return f"拼接 {len(n.inputs)} 段 (value 输入)"
        if t in (N_TEXTLIT, N_NUMLIT, N_BOLLIT):
            return ""   # 数值已直接显示在块上
        if t == N_SAY:
            s = (n.name + "：") if n.name else ""
            return s + (n.text or "← value 文本")
        if t == N_INPUT:
            return f"输入框: {n.ctrl or '?'}"
        if t == N_RANDOM:
            return f"随机 0..{n.max}"
        if t == N_WAIT:
            return f"等待 {n.ms} ms"
        if t == N_DOUT:
            return f"输出「{n.out_name or '?'}」← {self._pin_expr(n, 1)}"
        if t == N_DREF:
            if 0 <= n.out_scene < len(self.app.project.scenes):
                return f"读 {self.app.project.scenes[n.out_scene].title}.{n.out_name}"
            return "读场景数据 (未选)"
        if t == N_BGM:
            return f"BGM: {n.sound_path or '未选音频'}"
        if t == N_SE:
            return f"SE: {n.sound_path or '未选音频'}"
        if t == N_SOUND_STOP:
            return {0: "停止 BGM", 1: "停止 SE", 2: "停止全部声音"}.get(n.stop_target, "停止声音")
        if t == N_SOUND_SWAP:
            return f"切换BGM: {n.sound_path or '未选音频'}"
        if t == N_BG_TRANS:
            return f"柔和背景: {n.bg_path or '未选'} · {n.bg_trans_ms}ms"
        if t == N_SCENE_TRANS:
            dst = {0: "下一镜头", 1: f"镜头 {n.flow_scene + 1}", 2: "结束"}.get(n.flow_target, "")
            return f"淡出切 {dst} · {n.scene_trans_ms}ms"
        if t == N_TOAST:
            return f"消息: {n.toast_text or '?'} · {['左上','右上','左下','右下'][n.toast_corner if 0 <= n.toast_corner < 4 else 3]} · {n.toast_ms}ms"
        if t == N_SAVE:
            return f"存档 → {n.file_path or '未选路径'}"
        if t == N_LOAD:
            return f"读档 ← {n.file_path or '未选路径'}"
        if t == N_FILE_WRITE:
            return f"写文件 {n.file_path or '未选路径'}"
        if t == N_FILE_READ:
            return f"读文件 {n.file_path or '未选路径'}"
        if t == N_CHECKPOINT:
            s = f"存档点「{n.ck_name or '?'}」"
            if n.file_path:
                s += f" → {n.file_path}"
            return s
        if t == N_JUMP:
            return f"跳转到存档点 ← {n.file_path or '未选路径'}"
        if t == N_BG:
            if n.bg_mode == 1:
                return f"纯色 0x{n.bg_color:06X}"
            fit = BG_FIT_NAMES[n.bg_fit] if 0 <= n.bg_fit < len(BG_FIT_NAMES) else "?"
            return f"背景 {n.bg_path} · {fit}"
        if t == N_FX_ADD:
            fx = FX_NAMES[n.fx] if 0 <= n.fx < len(FX_NAMES) else "?"
            return f"特效: {fx} 强度{n.fx_strength}"
        if t == N_FX_OFF:
            fx = FX_NAMES[n.fx] if 0 <= n.fx < len(FX_NAMES) else "?"
            return f"取消: {fx}"
        if t == N_FX_CLEAR:
            return "清除全部特效"
        return ""

    def _pin_expr(self, n, idx):
        if idx < len(n.inputs):
            p = n.inputs[idx]
            return (p.variable or "?") if p.source == 'variable' else (p.literal or "?")
        return "?"

    def _draw_link(self, l):
        sc = self.app.scene()
        sn = sc.node(l.from_node)
        tn = sc.node(l.to_node)
        if not sn or not tn:
            return
        x0, y0 = self.pin_pos(sn, l.from_pin, True)
        x1, y1 = self.pin_pos(tn, l.to_pin, False)
        kind = self.pin_kind(sn, l.from_pin, True)
        # 若目标输入存在多条 exec 链接, 使用替代颜色标记为"多路汇入"
        if kind == 'exec':
            dup_count = sum(1 for ll in sc.links if ll.to_node == l.to_node and ll.to_pin == l.to_pin)
            color = T.EXEC_LINE_ALT if dup_count > 1 else T.EXEC_LINE
        else:
            color = T.VALUE_LINE
        dx = max(40, abs(x1 - x0) / 2)
        self.create_line(x0, y0, x0 + dx, y0, x1 - dx, y1, x1, y1,
                         fill=color, width=2, smooth=True)
        if kind == 'value':
            self.create_line(x0, y0, x0 + dx, y0, x1 - dx, y1, x1, y1,
                             fill=color, width=1, dash=(4, 3), smooth=True)

    def _draw_wire_drag(self):
        fnode, fpin, kind, mx, my = self._wire
        sc = self.app.scene()
        sn = sc.node(fnode) if sc else None
        if not sn:
            return
        x0, y0 = self.pin_pos(sn, fpin, True)
        color = T.EXEC_LINE if kind == 'exec' else T.VALUE_LINE
        dx = max(40, abs(mx - x0) / 2)
        self.create_line(x0, y0, x0 + dx, y0, mx - dx, my, mx, my,
                         fill=color, width=2, smooth=True, dash=(6, 3))

    def _draw_wire_rev(self):
        tnode, tpin, kind, mx, my = self._wire_rev
        sc = self.app.scene()
        tn = sc.node(tnode) if sc else None
        if not tn:
            return
        x1, y1 = self.pin_pos(tn, tpin, False)
        color = T.EXEC_LINE if kind == 'exec' else T.VALUE_LINE
        dx = max(40, abs(x1 - mx) / 2)
        self.create_line(mx, my, mx + dx, my, x1 - dx, y1, x1, y1,
                         fill=color, width=2, smooth=True, dash=(6, 3))

    def _draw_ghost(self):
        ntype, mx, my = self._ghost
        color = self._colors().get(ntype, "#555")
        name = self._names().get(ntype, "?")
        self.create_rectangle(mx - NODE_W // 2, my - 30, mx + NODE_W // 2, my + 30,
                              fill="#2a2d37", outline=color, width=2)
        self.create_text(mx, my, fill="#fff", font=T.FONT_BOLD,
                         text=f"＋ {self._icons().get(ntype, '')} {name}")

    def hit_link(self, mx, my):
        """命中连线:返回连线对象或 None。"""
        sc = self.app.scene()
        if not sc:
            return None
        for l in sc.links:
            sn = sc.node(l.from_node)
            tn = sc.node(l.to_node)
            if not sn or not tn:
                continue
            x0, y0 = self.pin_pos(sn, l.from_pin, True)
            x1, y1 = self.pin_pos(tn, l.to_pin, False)
            if _dist_seg(mx, my, x0, y0, x1, y1) < 9:
                return l
        return None

    # ---------------- 事件 ----------------
    def _on_press(self, ev):
        self.focus_set()          # 让画布获得键盘焦点(快捷键生效)
        sc = self.app.scene()
        if not sc:
            return
        if self.app.drag_source is not None:
            self._ghost = (self.app.drag_source, ev.x, ev.y)
            self.render()
            return
        hit = self.hit_pin(ev.x, ev.y)
        if hit:
            n, pidx, is_out = hit
            kind = self.pin_kind(n, pidx, is_out)
            if is_out:
                self._wire = (n.id, pidx, kind, ev.x, ev.y)
                self.selected = n.id
                self.app.on_select()
            else:
                sc.links = [l for l in sc.links
                            if not (l.to_node == n.id and l.to_pin == pidx)]
                self._wire_rev = (n.id, pidx, kind, ev.x, ev.y)
            self.render()
            return
        n = self.hit_node(ev.x, ev.y)
        self.selected = n.id if n else None
        if n:
            self._drag = (n.id, ev.x, ev.y)
        self.app.on_select()
        self.render()

    def _on_motion(self, ev):
        sc = self.app.scene()
        if not sc:
            return
        if self._ghost:
            self._ghost = (self._ghost[0], ev.x, ev.y)
            self.render()
            return
        if self._wire:
            self._wire = (self._wire[0], self._wire[1], self._wire[2], ev.x, ev.y)
            self.render()
            return
        if self._wire_rev:
            self._wire_rev = (self._wire_rev[0], self._wire_rev[1],
                             self._wire_rev[2], ev.x, ev.y)
            self.render()
            return
        if self._drag:
            nid, sx, sy = self._drag
            n = sc.node(nid)
            if n:
               n.x += ev.x - sx
               n.y += ev.y - sy
               self._drag = (nid, ev.x, ev.y)
               # 拖拽时节流重绘，减少卡顿
               now = time.time()
               if now - self._last_render > 0.03:  # 约 30ms
                   self._last_render = now
                   self.render()
            return
        if self._wire:
            self._wire = (self._wire[0], self._wire[1], self._wire[2], ev.x, ev.y)
            self.render()
            return
        if self._wire_rev:
            self._wire_rev = (self._wire_rev[0], self._wire_rev[1],
                              self._wire_rev[2], ev.x, ev.y)
            self.render()
            return
        if self._drag:
            nid, sx, sy = self._drag
            n = sc.node(nid)
            if n:
                n.x += ev.x - sx
                n.y += ev.y - sy
                self._drag = (nid, ev.x, ev.y)
                self.render()

    def _on_release(self, ev):
        sc = self.app.scene()
        if not sc:
            return
        if self._ghost:
            ntype, mx, my = self._ghost
            self._ghost = None
            wx, wy = self.s2w(mx, my)
            self.app.add_node_at(ntype, wx, wy)
            self.render()
            return
        if self._wire:
            fnode, fpin, kind, _, _ = self._wire
            self._wire = None
            target = self.hit_pin(ev.x, ev.y)
            if target and not target[2]:
                tn, tpin, _ = target
                if self._try_connect(sc, fnode, fpin, tn.id, tpin):
                    self.app.on_scene_changed(keep_selection=True)
            self.render()
            return
        if self._wire_rev:
            tnode, tpin, kind, _, _ = self._wire_rev
            self._wire_rev = None
            target = self.hit_pin(ev.x, ev.y)
            if target and target[2]:
                sn, spin, _ = target
                if self._try_connect(sc, sn.id, spin, tnode, tpin):
                    self.app.on_scene_changed(keep_selection=True)
            self.render()
            return
        if self._drag:
            self._drag = None
            self.app.on_scene_changed(keep_selection=True)

    def _try_connect(self, sc, fnode, fpin, tnode, tpin):
        sn = sc.node(fnode)
        tn = sc.node(tnode)
        if not sn or not tn or sn.id == tn.id:
            return False
        src_kind = self.pin_kind(sn, fpin, True)
        dst_kind = self.pin_kind(tn, tpin, False)
        if src_kind != dst_kind:
            return False
        # 允许多路连接到同一输入（多条 exec 线汇入同一目标）
        sc.links = [l for l in sc.links
                    if not (l.from_node == sn.id and l.from_pin == fpin)]
        sc.links.append(Link(sc.lid, sn.id, fpin, tn.id, tpin))
        sc.lid += 1
        return True

    def _on_right(self, ev):
        sc = self.app.scene()
        if not sc:
            return
        n = self.hit_node(ev.x, ev.y)
        if n is not None:
            self._node_menu(ev, n)
            return
        l = self.hit_link(ev.x, ev.y)
        if l is not None:
            self._link_menu(ev, l)
            return
        self._blank_menu(ev)

    def _node_menu(self, ev, n):
        self.selected = n.id
        self.app.on_select()
        self.render()
        m = tk.Menu(self, tearoff=0)
        if n.type not in self._undeletable():
            m.add_command(label="删除节点", command=lambda: self._delete(n.id))
        if n.type == N_CHOICE:
            m.add_command(label="添加选项", command=lambda: self._add_choice(n.id))
        if n.type == N_LONG:
            m.add_command(label="添加对话行", command=lambda: self._add_long(n.id))
        m.add_command(label="复制节点", command=lambda: self._copy_node(n.id))
        m.add_separator()
        m.add_command(label="断开所有输入连线", command=lambda: self._disconnect_inputs(n.id))
        m.add_command(label="断开所有输出连线", command=lambda: self._disconnect_outputs(n.id))
        m.add_command(label="断开全部连线", command=lambda: self._disconnect_all(n.id))
        try:
            m.tk_popup(ev.x_root, ev.y_root)
        finally:
            m.grab_release()

    def _link_menu(self, ev, l):
        m = tk.Menu(self, tearoff=0)
        m.add_command(label="删除连线", command=lambda: self._delete_link(l.id))
        try:
            m.tk_popup(ev.x_root, ev.y_root)
        finally:
            m.grab_release()

    def _blank_menu(self, ev):
        m = tk.Menu(self, tearoff=0)
        add = tk.Menu(m, tearoff=0)
        for t in self._palette():
            add.add_command(label=f"{self._icons().get(t, '')} {self._names().get(t)}",
                            command=lambda tt=t, e=ev: self.app.add_node_at(
                                tt, *self.s2w(e.x, e.y)))
        m.add_cascade(label="添加节点", menu=add)
        m.add_separator()
        m.add_command(label="🧹 自动整理", command=self.app.auto_layout)
        m.add_command(label="重置视图", command=self._reset_view)
        try:
            m.tk_popup(ev.x_root, ev.y_root)
        finally:
            m.grab_release()

    def _reset_view(self):
        self.vx, self.vy = 0, 0
        self.render()

    def _delete_link(self, lid):
        sc = self.app.scene()
        if sc:
            sc.links = [x for x in sc.links if x.id != lid]
            self.app.on_scene_changed()

    def _disconnect_inputs(self, nid):
        sc = self.app.scene()
        if sc:
            sc.links = [l for l in sc.links if l.to_node != nid]
            self.app.on_scene_changed()

    def _disconnect_outputs(self, nid):
        sc = self.app.scene()
        if sc:
            sc.links = [l for l in sc.links if l.from_node != nid]
            self.app.on_scene_changed()

    def _disconnect_all(self, nid):
        sc = self.app.scene()
        if sc:
            sc.links = [l for l in sc.links
                        if l.to_node != nid and l.from_node != nid]
            self.app.on_scene_changed()

    def _delete(self, nid):
        sc = self.app.scene()
        if not sc:
            return
        n = sc.node(nid)
        if n is None or n.type in self._undeletable():
            return   # 起点/出口不可删除
        sc.delete_node(nid)
        if self.selected == nid:
            self.selected = None
        self.app.on_scene_changed()

    def _copy_node(self, nid):
        import copy
        sc = self.app.scene()
        n = sc.node(nid)
        if n:
            c = copy.deepcopy(n)
            c.id = sc.nid
            sc.nid += 1
            c.x += 30
            c.y += 30
            sc.nodes.append(c)
            self.selected = c.id
            self.app.on_scene_changed(keep_selection=True)

    def _add_choice(self, nid):
        sc = self.app.scene()
        n = sc.node(nid)
        if n:
            n.options.append("新选项")
            n.sync_choice_outputs()
            self.app.on_scene_changed()

    def _add_long(self, nid):
        sc = self.app.scene()
        n = sc.node(nid)
        if n:
            n.lines.append([-1, ""])
            self.app.on_scene_changed()

    def _on_pan_press(self, ev):
        self._pan = (self.vx, self.vy, ev.x, ev.y)

    def _on_pan_motion(self, ev):
        if self._pan:
            vx, vy, sx, sy = self._pan
            self.vx = vx + ev.x - sx
            self.vy = vy + ev.y - sy
            self.render()

    def _on_wheel(self, ev):
        d = ev.delta
        if d:
            factor = 1.12 if d > 0 else 1 / 1.12
            self._zoom_at(factor, ev.x, ev.y)

    def _on_move(self, ev):
        pass

    # ---------------- 快捷键 ----------------
    def _on_key(self, ev):
        ctrl = (ev.state & 0x4) != 0
        k = ev.keysym
        if k == "Delete":
            self.app.delete_selected()
            return "break"
        if ctrl and k.lower() == "c":
            self.copy_selected()
            return "break"
        if ctrl and k.lower() == "v":
            self.paste_selected()
            return "break"
        if ctrl and k.lower() == "z":
            self.app.undo() if not (ev.state & 0x1) else self.app.redo()
            return "break"
        if ctrl and k.lower() == "y":
            self.app.redo()
            return "break"
        if k in ("Up", "Down", "Left", "Right"):
            self._nudge(k)
            return "break"
        return None

    def copy_selected(self):
        """复制选中节点到剪贴板(Ctrl+C)。"""
        import copy
        sc = self.app.scene()
        n = sc.node(self.selected) if self.selected is not None else None
        if n is not None:
            self._clipboard = copy.deepcopy(n)

    def paste_selected(self):
        """粘贴剪贴板中的节点(Ctrl+V),位置偏移。"""
        import copy
        if self._clipboard is None:
            return
        sc = self.app.scene()
        c = copy.deepcopy(self._clipboard)
        c.id = sc.nid; sc.nid += 1
        c.x += 40; c.y += 40
        sc.nodes.append(c)
        self.selected = c.id
        self.app.on_scene_changed(keep_selection=True)

    def _nudge(self, k):
        """方向键微移选中节点。"""
        sc = self.app.scene()
        n = sc.node(self.selected) if self.selected is not None else None
        if n is None:
            return
        d = 10
        if k == "Up": n.y -= d
        elif k == "Down": n.y += d
        elif k == "Left": n.x -= d
        elif k == "Right": n.x += d
        self.app.on_scene_changed(keep_selection=True)


def _dist_seg(px, py, x0, y0, x1, y1):
    """点到线段距离。"""
    import math
    dx, dy = x1 - x0, y1 - y0
    if dx == 0 and dy == 0:
        return math.hypot(px - x0, py - y0)
    t = max(0, min(1, ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy)))
    cx, cy = x0 + t * dx, y0 + t * dy
    return math.hypot(px - cx, py - cy)
