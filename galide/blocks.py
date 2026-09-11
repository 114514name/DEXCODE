"""galide.blocks — Scratch 式指令块画布(tkinter Canvas)。

以深度优先线性序渲染当前镜头的块树,depth 表示缩进(C 形块的子块缩进更深)。
支持:点击选中、块内拖拽排序/嵌套(插入线指示)、从块面板拖出新建、滚轮滚动。
"""

import tkinter as tk

from . import theme as T
from .model import (BLOCK_NAMES, BL_CHOICE, BL_IF, BL_OPTION, block_end,
                    block_move, insert_block, delete_block, prev_sibling,
                    next_sibling)

BLOCK_H = 58
INDENT = 26
PAD = 8
HEAD_H = 34


def _summary(b) -> str:
    t = b.type
    if t == 0:  # SPEAK
        s = b.speaker + "：" if b.speaker else ""
        return s + (b.text or "（空白台词）")
    if t == 1:  # BG
        return b.bg if b.useBgImg and b.bg else f"纯色 #{b.bgColor:06X}"
    if t == 2:  # SPRITE
        src = f"人物[{b.charIdx}].状态[{b.stateIdx}]" if b.charIdx >= 0 else (b.sprPath or "—")
        return f"{src}  图层{b.spr_layer} {'显示' if b.spr_show else '隐藏'}"
    if t == 3:  # AUDIO
        kinds = {0: "BGM", 1: "SE", 2: "停止BGM", 3: "音量"}
        k = kinds.get(b.audio_type, "?")
        return f"{k} {b.audio_path}" if b.audio_path else k
    if t == 4:  # WAIT
        return f"点击继续" if b.wait_type == 0 else f"等待 {b.wait_ms} ms"
    if t == 6:  # CHOICE
        return b.choice_text or "（选项）"
    if t == 7:  # OPTION
        return b.option_text or "（选项项）"
    if t == 8:  # IF
        return b.cond or "1"
    if t == 9:  # CODE
        return b.code_file or "（代码文件）"
    return ""


class BlockCanvas(tk.Canvas):
    def __init__(self, master, app, **kw):
        super().__init__(master, bg=T.BG, highlightthickness=0,
                         bd=0, **kw)
        self.app = app
        self.scroll = 0          # 像素滚动偏移
        self._drag_block = -1
        self._drag_y = 0
        self._drop_line = -1
        self._drop_where = 0
        self._ghost = None       # 从面板拖出的幽灵块 type
        self._ghost_y = 0
        self._rows = []          # [(index, y, h, x0, x1)]

        self.bind("<Configure>", lambda e: self.render())
        self.bind("<ButtonPress-1>", self._on_press)
        self.bind("<B1-Motion>", self._on_motion)
        self.bind("<ButtonRelease-1>", self._on_release)
        self.bind("<MouseWheel>", self._on_wheel)
        self.bind("<Button-3>", self._on_right)

    # ---------------- 布局 ----------------
    def _compute_rows(self):
        sc = self.app.scene()
        self._rows = []
        y = HEAD_H
        for i, b in enumerate(sc.blocks):
            x0 = PAD + b.depth * INDENT
            x1 = self.winfo_width() - PAD
            if x1 < x0 + 40:
                x1 = x0 + 40
            self._rows.append((i, y, BLOCK_H, x0, x1))
            y += BLOCK_H
        return y

    # ---------------- 渲染 ----------------
    def render(self):
        self.delete("all")
        sc = self.app.scene()
        if sc is None:
            return
        w = self.winfo_width()
        self._compute_rows()
        cy = HEAD_H - self.scroll
        for i, b in enumerate(sc.blocks):
            y = cy
            if y + BLOCK_H > 0 and y < self.winfo_height() + 60:
                self._draw_block(i, b, y, sc.cur == i, w)
            cy += BLOCK_H
        # 顶部说明
        self.create_text(PAD + 4, 12, anchor="nw", fill=T.FG2,
                         font=T.FONT_SM,
                         text=f"场景：{sc.title}　·　{sc.nBlocks} 块　·　滚轮滚动 / 拖拽排序")
        # 插入线
        if self._drop_line >= 0 and self._drop_line < len(self._rows):
            self._draw_drop(self._drop_line, self._drop_where)
        # 从面板拖出的幽灵
        if self._ghost is not None:
            self._draw_ghost()

    def _draw_block(self, i, b, y, selected, w):
        _, _, _, x0, x1 = self._rows[i]
        bw = x1 - x0
        fill, edge, fg = T.BLOCK_COLORS.get(b.type, ("#333", "#555", "#eee"))
        # 圆角矩形
        self._round(x0, y, x0 + bw, y + BLOCK_H, 8, fill=fill, outline=edge,
                    width=3 if selected else 1)
        # 左侧类型色条
        self.create_rectangle(x0 + 2, y + 6, x0 + 5, y + BLOCK_H - 6,
                              fill=edge, outline="")
        # 图标 + 标题
        icon = T.BLOCK_ICONS.get(b.type, "·")
        name = BLOCK_NAMES.get(b.type, "?")
        self.create_text(x0 + 14, y + 8, anchor="nw", fill=fg,
                         font=T.FONT_BOLD,
                         text=f"{icon} {name}")
        # 摘要
        self.create_text(x0 + 14, y + 30, anchor="nw", fill=T.FG,
                         font=T.FONT_SM,
                         text=_summary(b)[:bw // 7], width=bw - 24)
        # 子块指示
        if b.type in (BL_CHOICE, BL_IF):
            sc = self.app.scene()
            e = block_end(sc, i)
            nchild = e - i - 1
            if nchild > 0:
                self.create_text(x1 - 10, y + BLOCK_H // 2, anchor="e",
                                 fill=T.FG2, font=T.FONT_SM,
                                 text=f"▾ {nchild}")

    def _draw_ghost(self):
        if self._ghost is None:
            return
        y = self._ghost_y - BLOCK_H // 2
        fill, edge, fg = T.BLOCK_COLORS.get(self._ghost, ("#333", "#555", "#eee"))
        self._round(PAD, y, self.winfo_width() - PAD, y + BLOCK_H, 8,
                    fill=fill, outline=edge, width=2, stipple="gray50")
        name = BLOCK_NAMES.get(self._ghost, "?")
        self.create_text(PAD + 14, y + 8, anchor="nw", fill=fg, font=T.FONT_BOLD,
                         text=f"＋ {T.BLOCK_ICONS.get(self._ghost, '')} {name}")
        self.create_text(PAD + 14, y + 30, anchor="nw", fill=fg, font=T.FONT_SM,
                         text="松开以插入到此处")

    def _draw_drop(self, idx, where):
        if idx < 0 or idx >= len(self._rows):
            return
        _, ry, rh, rx0, rx1 = self._rows[idx]
        sc = self.app.scene()
        is_c = sc.blocks[idx].type in (BL_CHOICE, BL_IF)
        if where == 2 and is_c:
            # 作为子块:高亮目标块内部
            self.create_rectangle(rx0, ry, rx1, ry + rh, outline=T.ACCENT,
                                  width=3)
            self.create_text(rx1 - 12, ry + 8, anchor="ne", fill=T.ACCENT,
                             font=T.FONT_SM, text="作为子块")
        else:
            yy = ry if where == 0 else ry + rh
            self.create_rectangle(rx0 - 2, yy - 2, rx1 + 2, yy + 2,
                                  fill=T.ACCENT, outline="")

    def _round(self, x0, y0, x1, y1, r, **kw):
        pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r,
               x1, y1, x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r,
               x0, y0 + r, x0, y0]
        self.create_polygon(pts, smooth=True, **kw)

    # ---------------- 事件 ----------------
    def _on_press(self, ev):
        self.focus_set()
        sc = self.app.scene()
        if sc is None:
            return
        y = ev.y + self.scroll
        for idx, ry, rh, x0, x1 in self._rows:
            if ry <= y < ry + rh and x0 <= ev.x <= x1:
                sc.cur = idx
                self._drag_block = idx
                self._drag_y = ev.y
                self.app.on_select()
                self.render()
                return
        # 空白处:取消选中
        sc.cur = -1
        self.app.on_select()
        self.render()

    def _on_motion(self, ev):
        sc = self.app.scene()
        if sc is None:
            return
        # 从面板拖出
        if self.app.drag_source is not None:
            self._ghost = self.app.drag_source
            self._ghost_y = ev.y
            self._drop_line = self._hit_drop(ev.y)
            self._drop_where = self._where_at(ev.y)
            self.render()
            return
        if self._drag_block < 0:
            return
        # 块内拖拽:计算插入目标
        self._drop_line = self._hit_drop(ev.y)
        self._drop_where = self._where_at(ev.y)
        # 滚动边缘
        if ev.y < 40:
            self.scroll = max(0, self.scroll - 14)
        elif ev.y > self.winfo_height() - 40:
            self.scroll += 14
        self.render()

    def _hit_drop(self, y):
        yy = y + self.scroll
        for idx, ry, rh, _, _ in self._rows:
            if ry <= yy < ry + rh:
                return idx
        # 列表外:末尾/开头
        if self._rows:
            if yy < self._rows[0][1]:
                return 0
            if yy >= self._rows[-1][1] + self._rows[-1][2]:
                return len(self._rows) - 1
        return -1

    def _where_at(self, y):
        sc = self.app.scene()
        if sc is None:
            return 0
        yy = y + self.scroll
        for idx, ry, rh, _, _ in self._rows:
            if ry <= yy < ry + rh:
                b = sc.blocks[idx]
                # C 形块(选项/如果)→ 拖入内部即作为子块
                if b.type in (BL_CHOICE, BL_IF):
                    return 2
                return 0 if yy < ry + rh / 2 else 1
        return 0

    def _on_release(self, ev):
        sc = self.app.scene()
        if sc is None:
            return
        # 从面板拖出 → 插入新块
        if self.app.drag_source is not None:
            t = self.app.drag_source
            self.app.drag_source = None
            self._ghost = None
            idx = self._hit_drop(ev.y)
            where = self._where_at(ev.y)
            self._insert_from_panel(t, idx, where)
            self._drop_line = -1
            self.render()
            return
        if self._drag_block >= 0:
            src = self._drag_block
            dst = self._hit_drop(ev.y)
            where = self._where_at(ev.y)
            if dst >= 0 and dst != src:
                # 避免把块拖进自己子树
                end = block_end(sc, src)
                if not (src < dst < end):
                    block_move(sc, src, dst, where)
                    sc.cur = min(dst, sc.nBlocks - 1)
                    self.app.on_scene_changed(keep_sel=True)
            self._drag_block = -1
            self._drop_line = -1
            self.render()

    def _insert_from_panel(self, t, idx, where):
        sc = self.app.scene()
        if sc is None:
            return
        if sc.nBlocks >= 384:
            return
        from .model import block_defaults
        b = block_defaults(t, 0)
        if idx >= 0 and idx < sc.nBlocks:
            target = sc.blocks[idx]
            if t in (BL_OPTION,):
                # 选项项必须是 CHOICE 的子块
                if target.type == BL_CHOICE:
                    pos = idx + 1
                    b.depth = target.depth + 1
                    sc.blocks.insert(pos, b)
                    sc.cur = pos
                elif where == 2 and target.type in (BL_CHOICE, BL_IF):
                    b.depth = target.depth + 1
                    sc.blocks.insert(idx + 1, b)
                    sc.cur = idx + 1
                else:
                    b.depth = 1
                    # 插到 target 后(作为可能的子块)
                    sc.blocks.insert(idx + 1, b)
                    sc.cur = idx + 1
            else:
                if where == 2 and target.type in (BL_CHOICE, BL_IF):
                    b.depth = target.depth + 1
                    sc.blocks.insert(idx + 1, b)
                    sc.cur = idx + 1
                else:
                    pos = idx if where == 0 else idx + 1
                    b.depth = target.depth
                    sc.blocks.insert(pos, b)
                    sc.cur = pos
        else:
            sc.blocks.append(b)
            sc.cur = len(sc.blocks) - 1
        sc.nBlocks = len(sc.blocks)
        self.app.on_scene_changed()

    def _on_wheel(self, ev):
        self.scroll = max(0, self.scroll - ev.delta // 2)
        self.render()

    def _on_right(self, ev):
        sc = self.app.scene()
        if sc is None:
            return
        y = ev.y + self.scroll
        for idx, ry, rh, x0, x1 in self._rows:
            if ry <= y < ry + rh and x0 <= ev.x <= x1:
                sc.cur = idx
                self.app.on_select()
                self.render()
                m = tk.Menu(self, tearoff=0)
                m.add_command(label="删除此块（含子块）",
                              command=lambda i=idx: self._del(i))
                m.add_command(label="向上移动", command=lambda: self._move(-1))
                m.add_command(label="向下移动", command=lambda: self._move(1))
                try:
                    m.tk_popup(ev.x_root, ev.y_root)
                finally:
                    m.grab_release()
                return

    def _del(self, i):
        sc = self.app.scene()
        if sc:
            delete_block(sc, i)
            self.app.on_scene_changed()

    def _move(self, d):
        sc = self.app.scene()
        if sc is None:
            return
        i = sc.cur
        n = next_sibling(sc, i) if d > 0 else prev_sibling(sc, i)
        if n >= 0:
            block_move(sc, i, n, 1 if d > 0 else 0)
            sc.cur = n
            self.app.on_scene_changed(keep_sel=True)


