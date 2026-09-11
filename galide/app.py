"""galide.app — GAL 编辑器主窗口(tkinter)。

布局:菜单 + 工具栏 / 左栏(镜头·人物·块面板)| 块画布 | 属性面板 / 预览条 / 状态栏。
"""

import os
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

from . import theme as T
from . import settings as settings_mod
from .model import (Project, project_defaults, block_defaults, block_end,
                    MAX_SCENES, MAX_CHARS, MAX_STATES, MAX_BLOCKS,
                    BLOCK_NAMES, BL_SPEAK, BL_CHOICE, BL_IF, BL_OPTION)
from . import sceneio
from . import export as export_mod
from .engine import Engine
from .preview import Preview
from .blocks import BlockCanvas, BLOCK_H
from .inspector import Inspector

PANEL_BLOCK_TYPES = [0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 7]


class App(tk.Tk):
    def __init__(self, start_file=None):
        super().__init__()
        self.title("GAL 编辑器")
        self.geometry("1440x900")
        self.minsize(1000, 640)
        self.configure(bg=T.BG)

        self.project = project_defaults()
        self.file_path = ""
        self.project_root = ""
        self.drag_source = None        # 从块面板拖出的块类型(跨 widget)
        self.preview_mode = False
        self._status_timer = None
        self.dirty = False

        # 设置
        self.project_root = settings_mod.get_project_root()
        self._init_engine()
        self._build_menu()
        self._build_toolbar()
        self._build_body()
        self._build_statusbar()
        if not self._engine_ready:
            self.set_status("⚠ DLL 引擎未就绪 — 预览不可用。检查 编辑→项目设置", sticky=False)
        self._preview_poll()

        if start_file and os.path.isfile(start_file):
            self.open_file(start_file)
        self._refresh_title()
        self.bind("<Delete>", lambda e: self._del_selected())
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------------- 引擎 ----------------
    def _init_engine(self):
        dll = settings_mod.get_engine_dll()
        self.engine = Engine(dll if dll else None)
        self._engine_ready = self.engine.load()  # 启动时预加载,提前暴露 DLL 问题

    # ---------------- 访问器 ----------------
    def scene(self):
        if not self.project.scenes:
            return None
        return self.project.scenes[self.project.cur]

    # ---------------- 菜单 / 工具栏 ----------------
    def _build_menu(self):
        m = tk.Menu(self)
        fm = tk.Menu(m, tearoff=0)
        fm.add_command(label="新建", accelerator="Ctrl+N", command=self.new_project)
        fm.add_command(label="打开…", accelerator="Ctrl+O", command=self.open_file_dialog)
        fm.add_command(label="保存", accelerator="Ctrl+S", command=self.save_file)
        fm.add_command(label="另存为…", accelerator="Ctrl+Shift+S", command=self.save_as)
        fm.add_separator()
        fm.add_command(label="导出代码 (scene_out.dex)", command=self.export)
        fm.add_separator()
        fm.add_command(label="退出", command=self._on_close)
        m.add_cascade(label="文件", menu=fm)

        vm = tk.Menu(m, tearoff=0)
        vm.add_command(label="▶ 预览当前镜头", command=self.start_preview)
        vm.add_command(label="■ 停止预览", command=self.stop_preview)
        m.add_cascade(label="预览", menu=vm)

        em = tk.Menu(m, tearoff=0)
        em.add_command(label="项目设置…", command=self.settings_dialog)
        m.add_cascade(label="编辑", menu=em)

        hm = tk.Menu(m, tearoff=0)
        hm.add_command(label="关于", command=self._about)
        m.add_cascade(label="帮助", menu=hm)
        self.config(menu=m)

    def _build_toolbar(self):
        tb = tk.Frame(self, bg=T.BG2, height=40)
        tb.pack(fill="x", side="top")
        btns = [
            ("新建", self.new_project), ("打开…", self.open_file_dialog),
            ("保存", self.save_file), ("导出代码", self.export),
        ]
        for txt, cmd in btns:
            tk.Button(tb, text=txt, command=cmd, bg=T.BG3, fg=T.FG,
                      activebackground=T.ACCENT_DARK, activeforeground="#fff",
                      relief="flat", font=T.FONT_SM, padx=12, pady=3,
                      cursor="hand2").pack(side="left", padx=(4, 0), pady=5)
        self.preview_btn = tk.Button(tb, text="▶ 预览", command=self.toggle_preview,
                                     bg=T.OK, fg="#102", activebackground="#2fae6a",
                                     relief="flat", font=T.FONT_SM, padx=14, pady=3,
                                     cursor="hand2")
        self.preview_btn.pack(side="left", padx=(12, 0), pady=5)

    # ---------------- 主体布局 ----------------
    def _build_body(self):
        body = tk.Frame(self, bg=T.BG)
        body.pack(fill="both", expand=True, side="top")

        # 左栏
        self.left = tk.Frame(body, bg=T.BG2, width=210)
        self.left.pack(side="left", fill="y")
        self.left.pack_propagate(False)
        self._build_left_panel()

        # 中央画布
        center = tk.Frame(body, bg=T.BG)
        center.pack(side="left", fill="both", expand=True)
        self.canvas = BlockCanvas(center, self)
        self.canvas.pack(fill="both", expand=True)

        # 右栏属性
        self.right = tk.Frame(body, bg=T.BG2, width=280)
        self.right.pack(side="right", fill="y")
        self.right.pack_propagate(False)
        self.inspector = Inspector(self.right, self)
        self.inspector.pack(fill="both", expand=True)

        # 底部预览条
        self.preview_frame = tk.Frame(self, bg="#000", height=300)
        self.preview_frame.pack(fill="x", side="bottom")
        self.preview_frame.pack_propagate(False)
        self.preview = Preview(self.preview_frame, self.engine)
        self.preview_label = tk.Label(self.preview_frame, text="预览未启动（点击工具栏 ▶ 预览）",
                                      bg="#000", fg="#888", font=T.FONT_SM)
        self.preview_label.place(relx=0.5, rely=0.5, anchor="center")
        self.preview_frame.bind("<Configure>", self._layout_preview)

    def _layout_preview(self, _e=None):
        if self.preview.active:
            self.preview_label.place_forget()
            pf = self.preview_frame
            self.preview.resize(4, 4, max(pf.winfo_width() - 8, 0),
                                max(pf.winfo_height() - 8, 0))
        else:
            self.preview_label.place(relx=0.5, rely=0.5, anchor="center")

    def _build_left_panel(self):
        nb = ttk.Notebook(self.left)
        nb.pack(fill="both", expand=True)

        # 镜头页
        sc_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(sc_pg, text="镜头")
        self.scene_list = tk.Listbox(sc_pg, bg=T.BG3, fg=T.FG, relief="flat",
                                     selectbackground=T.ACCENT_DARK, font=T.FONT_SM)
        self.scene_list.pack(fill="both", expand=True, padx=6, pady=6)
        self.scene_list.bind("<<ListboxSelect>>", self._on_scene_pick)
        sr = tk.Frame(sc_pg, bg=T.BG2); sr.pack(fill="x", padx=6, pady=(0, 6))
        tk.Button(sr, text="＋镜头", command=self.add_scene, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True)
        tk.Button(sr, text="－删除", command=self.del_scene, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))

        # 人物页
        ch_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(ch_pg, text="人物")
        self.char_list = tk.Listbox(ch_pg, bg=T.BG3, fg=T.FG, relief="flat",
                                    selectbackground=T.ACCENT_DARK, font=T.FONT_SM)
        self.char_list.pack(fill="both", expand=True, padx=6, pady=(6, 2))
        self.char_list.bind("<<ListboxSelect>>", self._on_char_pick)
        self.state_list = tk.Listbox(ch_pg, bg=T.BG3, fg=T.FG, relief="flat",
                                     selectbackground=T.ACCENT_DARK,
                                     font=T.FONT_SM, height=5)
        self.state_list.pack(fill="both", expand=True, padx=6, pady=2)
        cr = tk.Frame(ch_pg, bg=T.BG2); cr.pack(fill="x", padx=6, pady=(2, 2))
        tk.Button(cr, text="＋人物", command=self.add_char, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True)
        tk.Button(cr, text="重命名", command=self.rename_char, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))
        sr2 = tk.Frame(ch_pg, bg=T.BG2); sr2.pack(fill="x", padx=6, pady=2)
        tk.Button(sr2, text="＋状态图", command=self.add_char_state, bg=T.BG3,
                  fg=T.FG, relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True)
        tk.Button(sr2, text="－状态", command=self.del_char_state, bg=T.BG3,
                  fg=T.FG, relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))

        # 块面板页
        bp_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(bp_pg, text="块")
        tk.Label(bp_pg, text="点击添加，或按住拖到画布", bg=T.BG2, fg=T.FG2,
                 font=T.FONT_SM, anchor="w").pack(fill="x", padx=8, pady=(6, 2))
        bf = tk.Frame(bp_pg, bg=T.BG2)
        bf.pack(fill="both", expand=True, padx=6, pady=4)
        for t in PANEL_BLOCK_TYPES:
            self._panel_block_btn(bf, t)

    def _panel_block_btn(self, parent, t):
        fill, edge, fg = T.BLOCK_COLORS.get(t, ("#333", "#555", "#eee"))
        name = BLOCK_NAMES.get(t, "?")
        b = tk.Button(parent, text=f"{T.BLOCK_ICONS.get(t, '')} {name}",
                      bg=fill, fg=fg, activebackground=edge,
                      relief="flat", font=T.FONT_SM, anchor="w", pady=4,
                      cursor="hand2")
        b.pack(fill="x", pady=2)
        b.bind("<ButtonPress-1>", lambda _e, tt=t: self._panel_drag_start(tt))
        b.bind("<ButtonRelease-1>", lambda _e: self._panel_drag_end())
        b.bind("<Double-Button-1>", lambda _e, tt=t: self._panel_click_add(tt))
        return b

    def _panel_drag_start(self, t):
        self.drag_source = t
        self.canvas._ghost = t

    def _panel_drag_end(self):
        self.drag_source = None
        self.canvas._ghost = None
        self.canvas._drop_line = -1

    def _panel_click_add(self, t):
        sc = self.scene()
        if sc is None:
            return
        if sc.nBlocks >= MAX_BLOCKS:
            return
        # 在选中块后插入(选项项特殊处理为选中块的子块)
        if t == BL_OPTION:
            if 0 <= sc.cur < sc.nBlocks and sc.blocks[sc.cur].type == BL_CHOICE:
                b = block_defaults(BL_OPTION, sc.blocks[sc.cur].depth + 1)
                pos = block_end(sc, sc.cur)
                sc.blocks.insert(pos, b)
                sc.cur = pos
            else:
                b = block_defaults(BL_OPTION, 1)
                sc.blocks.insert(sc.cur + 1, b)
                sc.cur = sc.cur + 1
        else:
            from .model import insert_block
            d = 0
            if 0 <= sc.cur < sc.nBlocks:
                cb = sc.blocks[sc.cur]
                if cb.type in (BL_CHOICE, BL_IF):
                    d = cb.depth + 1
                else:
                    d = cb.depth
            insert_block(sc, t, d)
        sc.nBlocks = len(sc.blocks)
        self.on_scene_changed()

    # ---------------- 状态栏 ----------------
    def _build_statusbar(self):
        self.status = tk.Label(self, text="就绪", bg=T.BG2, fg=T.FG2,
                               font=T.FONT_SM, anchor="w", padx=8)
        self.status.pack(fill="x", side="bottom")

    def set_status(self, msg, sticky=True):
        self.status.config(text=msg)
        if sticky and self._status_timer is None:
            self._status_timer = self.after(6000, self._clear_status)

    def _clear_status(self):
        self._refresh_title()
        self._status_timer = None

    def _refresh_title(self):
        name = os.path.basename(self.file_path) if self.file_path else "未命名"
        star = " *" if self.dirty else ""
        self.title(f"{name}{star} — GAL 编辑器")

    # ---------------- 镜头 / 人物 ----------------
    def _on_scene_pick(self, _e=None):
        sel = self.scene_list.curselection()
        if sel:
            self.project.cur = sel[0]
            self.on_select()
            self.refresh_left()

    def refresh_left(self):
        # 镜头列表
        self.scene_list.delete(0, "end")
        for i, s in enumerate(self.project.scenes):
            tag = "▸ " if i == self.project.cur else "  "
            self.scene_list.insert("end", f"{tag}{s.title}")
        # 人物列表
        self.char_list.delete(0, "end")
        for i, c in enumerate(self.project.chars):
            self.char_list.insert("end", c.name or f"人物{i}")
        self._refresh_states()

    def _refresh_states(self):
        self.state_list.delete(0, "end")
        sel = self.char_list.curselection()
        if sel and sel[0] < len(self.project.chars):
            ch = self.project.chars[sel[0]]
            for i in range(ch.nStates):
                nm = ch.stateNames[i] or f"状态{i}"
                self.state_list.insert("end", f"{nm}  ·  {os.path.basename(ch.states[i])}")

    def add_scene(self):
        if self.project.nScenes >= MAX_SCENES:
            return
        from .model import scene_defaults
        self.project.scenes.append(scene_defaults(f"镜头 {self.project.nScenes + 1}"))
        self.project.nScenes = len(self.project.scenes)
        self.project.cur = len(self.project.scenes) - 1
        self.dirty = True
        self.on_scene_changed()

    def del_scene(self):
        if self.project.nScenes <= 1:
            return
        if not messagebox.askyesno("删除镜头", "确定删除当前镜头及其全部指令块？"):
            return
        del self.project.scenes[self.project.cur]
        self.project.nScenes = len(self.project.scenes)
        self.project.cur = min(self.project.cur, self.project.nScenes - 1)
        self.dirty = True
        self.on_scene_changed()

    def add_char(self):
        if self.project.nChars >= MAX_CHARS:
            return
        name = simpledialog.askstring("添加人物", "人物名称：", parent=self)
        if name is None:
            return
        from .model import Character
        c = Character()
        c.name = name
        self.project.chars.append(c)
        self.project.nChars = len(self.project.chars)
        self.char_list.selection_clear(0, "end")
        self.char_list.selection_set(len(self.project.chars) - 1)
        self.dirty = True
        self.refresh_left()
        self.inspector.rebuild()

    def rename_char(self):
        sel = self.char_list.curselection()
        if not sel or sel[0] >= len(self.project.chars):
            return
        ch = self.project.chars[sel[0]]
        name = simpledialog.askstring("重命名", "人物名称：", parent=self,
                                      initialvalue=ch.name)
        if name:
            ch.name = name
            self.dirty = True
            self.refresh_left()

    def add_char_state(self):
        sel = self.char_list.curselection()
        if not sel or sel[0] >= len(self.project.chars):
            messagebox.showinfo("提示", "先在人物列表选中一个角色。", parent=self)
            return
        ch = self.project.chars[sel[0]]
        if ch.nStates >= MAX_STATES:
            return
        p = filedialog.askopenfilename(title="选择状态图",
                                       filetypes=[("图片", "*.png *.jpg *.jpeg *.bmp *.webp")],
                                       parent=self)
        if not p:
            return
        stname = simpledialog.askstring("状态名", "状态名称（如：微笑/生气）：", parent=self)
        ch.states[ch.nStates] = p
        ch.stateNames[ch.nStates] = stname or f"状态{ch.nStates}"
        ch.nStates += 1
        self.dirty = True
        self.refresh_left()

    def del_char_state(self):
        sel = self.char_list.curselection()
        if not sel or sel[0] >= len(self.project.chars):
            return
        ch = self.project.chars[sel[0]]
        ssel = self.state_list.curselection()
        if not ssel:
            return
        i = ssel[0]
        if i < ch.nStates:
            del ch.states[i]
            del ch.stateNames[i]
            ch.nStates -= 1
            self.dirty = True
            self.refresh_left()

    def _on_char_pick(self, _e=None):
        self._refresh_states()
        self.inspector.rebuild()

    # ---------------- 块操作 ----------------
    def on_select(self):
        """选中块变化:刷新属性面板 + 画布 + 预览到当前块。"""
        self.inspector.flush()  # 保存未提交的输入
        self.canvas.render()
        self.inspector.rebuild()
        self._apply_preview_block()

    def on_scene_changed(self, keep_sel=False, refresh_inspector=True):
        """场景内容变化(添加/删除/移动/改属性)。"""
        sc = self.scene()
        if sc is None:
            return
        self.dirty = True
        if not keep_sel:
            pass
        if refresh_inspector:
            self.inspector.flush()  # 保存未提交的输入再重建
        self.canvas.render()
        self.refresh_left()
        if refresh_inspector:
            self.inspector.rebuild()
        self._apply_preview_block()
        self._refresh_title()

    def _del_selected(self):
        sc = self.scene()
        if sc is None:
            return
        if 0 <= sc.cur < sc.nBlocks:
            from .model import delete_block
            delete_block(sc, sc.cur)
            self.on_scene_changed()

    # ---------------- 预览 ----------------
    def toggle_preview(self):
        if self.preview.active or self.preview_mode:
            self.stop_preview()
        else:
            self.start_preview()

    def start_preview(self):
        self.inspector.flush()  # 保存未提交输入再预览
        if not self.preview.start():
            err = getattr(self.preview, '_last_err', '') or "未知错误"
            messagebox.showwarning("预览",
                f"预览启动失败。\n{err}\n\n请确认 libs/gal/libdexxgal.dll 存在且未被其他程序占用。",
                parent=self)
            return
        self.preview_mode = True
        self.preview_btn.config(text="■ 停止", bg=T.WARN, fg="#102")
        self.preview_label.place_forget()
        self._layout_preview()
        self._apply_preview_scene()
        # 如果没有嵌入而是使用独立窗口，提示并在状态栏显示区别
        if getattr(self.preview, '_embedded', False):
            self.set_status("预览已启动", sticky=False)
        else:
            self.set_status("预览已启动（使用独立引擎窗口，未嵌入）", sticky=False)

    def stop_preview(self):
        self.preview.close()
        self.preview_mode = False
        self.preview_btn.config(text="▶ 预览", bg=T.OK, fg="#102")
        self.preview_label.place(relx=0.5, rely=0.5, anchor="center")

    def _apply_preview_scene(self):
        sc = self.scene()
        if sc is not None:
            self.preview.apply_scene(self.project, sc)
            self._apply_preview_block()

    def _apply_preview_block(self):
        sc = self.scene()
        if sc is None or not self.preview.active:
            return
        if 0 <= sc.cur < sc.nBlocks:
            self.preview.apply_block(self.project, sc, sc.blocks[sc.cur])

    def preview_audio(self):
        sc = self.scene()
        if sc is None:
            return
        if 0 <= sc.cur < sc.nBlocks:
            self.preview.play_block_audio(sc.blocks[sc.cur])

    def _preview_poll(self):
        if self.preview.active:
            self.preview.poll()
        self.after(16, self._preview_poll)

    # ---------------- 文件 ----------------
    def new_project(self):
        if self.dirty and not messagebox.askyesno("新建", "当前项目未保存，确定丢弃？", parent=self):
            return
        self.project = project_defaults()
        self.file_path = ""
        self.project_root = settings_mod.get_project_root()
        self.dirty = False
        self.on_scene_changed()

    def open_file_dialog(self):
        p = filedialog.askopenfilename(title="打开场景",
                                       filetypes=[("GAL 项目", "*.galscene"), ("全部", "*.*")],
                                       parent=self)
        if p:
            self.open_file(p)

    def open_file(self, path):
        self.inspector.flush()
        proj = sceneio.load_scene(path)
        if proj is None:
            messagebox.showwarning("打开", f"无法读取：{path}", parent=self)
            return
        self.project = proj
        self.file_path = path
        self.project_root = os.path.dirname(os.path.abspath(path))
        self.dirty = False
        self.on_scene_changed()
        self.set_status(f"已打开：{path}", sticky=False)

    def save_file(self):
        if not self.file_path:
            return self.save_as()
        self.inspector.flush()  # 保存未提交输入
        sceneio.save_scene(self.file_path, self.project)
        self.dirty = False
        self.set_status(f"已保存：{self.file_path}", sticky=False)
        self._refresh_title()
        return True

    def save_as(self):
        p = filedialog.asksaveasfilename(title="保存场景",
                                         defaultextension=".galscene",
                                         filetypes=[("GAL 项目", "*.galscene")],
                                         parent=self)
        if p:
            self.file_path = p
            self.project_root = os.path.dirname(os.path.abspath(p))
            return self.save_file()
        return False

    def export(self):
        self.inspector.flush()  # 保存未提交输入
        root = self.project_root or os.getcwd()
        path = export_mod.export_project(self.project, root)
        if path:
            self.set_status(f"✅ 已导出：{path}\n编译：python main.py compile {os.path.basename(path)}", sticky=False)
            messagebox.showinfo("导出", f"已导出可编译代码：\n{path}\n\n编译：python main.py compile {os.path.basename(path)}", parent=self)
        else:
            messagebox.showwarning("导出", "导出失败。", parent=self)

    # ---------------- 设置 / 其他 ----------------
    def settings_dialog(self):
        dlg = tk.Toplevel(self)
        dlg.title("项目设置")
        dlg.geometry("460x220")
        dlg.configure(bg=T.BG2)
        dlg.transient(self)
        dlg.grab_set()
        tk.Label(dlg, text="项目根目录（导出代码写入此处）", bg=T.BG2, fg=T.FG2,
                 font=T.FONT_SM, anchor="w").pack(fill="x", padx=14, pady=(14, 4))
        root = tk.StringVar(value=self.project_root)
        r1 = tk.Frame(dlg, bg=T.BG2); r1.pack(fill="x", padx=14)
        e = tk.Entry(r1, textvariable=root, bg=T.BG3, fg=T.FG, relief="flat",
                     font=T.FONT_SM)
        e.pack(side="left", fill="x", expand=True)

        def browse_root():
            p = filedialog.askdirectory(title="选择项目根目录", parent=dlg)
            if p:
                root.set(p)

        tk.Button(r1, text="…", command=browse_root, bg=T.BG3, fg=T.FG,
                  relief="flat").pack(side="right")
        tk.Label(dlg, text="GAL 引擎 DLL（留空用默认 libs/gal/libdexxgal.dll）",
                 bg=T.BG2, fg=T.FG2, font=T.FONT_SM, anchor="w").pack(fill="x", padx=14, pady=(10, 4))
        dll = tk.StringVar(value=settings_mod.get_engine_dll())
        r2 = tk.Frame(dlg, bg=T.BG2); r2.pack(fill="x", padx=14)
        e2 = tk.Entry(r2, textvariable=dll, bg=T.BG3, fg=T.FG, relief="flat",
                      font=T.FONT_SM)
        e2.pack(side="left", fill="x", expand=True)

        def browse_dll():
            p = filedialog.askopenfilename(title="选择引擎 DLL", parent=dlg)
            if p:
                dll.set(p)

        tk.Button(r2, text="…", command=browse_dll, bg=T.BG3, fg=T.FG,
                  relief="flat").pack(side="right")
        br = tk.Frame(dlg, bg=T.BG2); br.pack(fill="x", padx=14, pady=14)

        def save():
            self.project_root = root.get()
            settings_mod.set_project_root(self.project_root)
            if dll.get():
                settings_mod.set_engine_dll(dll.get())
            dlg.destroy()
            self.set_status("设置已保存", sticky=False)

        tk.Button(br, text="保存", command=save, bg=T.ACCENT_DARK, fg="#fff",
                  relief="flat", padx=20, pady=4).pack(side="left")
        tk.Button(br, text="取消", command=dlg.destroy, bg=T.BG3, fg=T.FG,
                  relief="flat", padx=20, pady=4).pack(side="left", padx=8)

    def _about(self):
        messagebox.showinfo("关于", "GAL 编辑器（Python + tkinter 版）\n\n"
                            "· Scratch 式指令块编辑\n"
                            "· 人物集（多状态立绘）\n"
                            "· 复用 libdexxgal.dll 引擎实时预览\n"
                            "· 导出可编译 DexLang 代码\n\n"
                            "场景文件 .galscene 兼容旧版", parent=self)

    def _on_close(self):
        if self.dirty and not messagebox.askyesno("退出", "项目未保存，确定退出？", parent=self):
            return
        self.preview.destroy()  # 彻底关闭引擎窗口
        self.destroy()


def main(root_dir=None, start_file=None):
    app = App(start_file=start_file)
    app.mainloop()
