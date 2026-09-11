"""bluedit.app — 蓝图式 GAL 编辑器主窗口。"""

import copy
import os
import shutil
import subprocess
import sys
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

from . import theme as T
from . import settings as settings_mod
from .model import (Project, NODE_NAMES, NODE_ICONS, NODE_COLORS, PALETTE,
                    N_ENTRY, N_EXIT, N_SPEAK, N_LONG, N_SPRITE, N_BG,
                    N_CHOICE, N_CODE, N_DEFVAR, N_SETVAR, N_IF, N_GETVAR,
                    N_LOGIC, N_MATH, N_FLOW, N_FX_ADD, N_FX_OFF, N_FX_CLEAR,
                    N_MENU, N_TEXT, N_SAY, N_INPUT, N_RANDOM, N_WAIT,
                    N_DOUT, N_DREF, N_TEXTLIT, N_NUMLIT, N_BOLLIT,
                    N_BGM, N_SE, N_SOUND_STOP, N_SOUND_SWAP,
                    N_BG_TRANS, N_SCENE_TRANS, N_TOAST,
                    N_SAVE, N_LOAD, N_FILE_WRITE, N_FILE_READ,
                    N_CHECKPOINT, N_JUMP,
                    POS_NAMES, LAYER_NAMES, TEXT_MODE_NAMES,
                    Link,
                    BG_FIT_NAMES, SPRITE_FIT_NAMES, project_defaults,
                    scene_defaults, Character, char_state_path,
                    project_to_dict, project_from_dict)
from . import sceneio
from . import export as export_mod
from . import paths
from . import compile as compile_mod
from .canvas import BlueCanvas
from .inspector import Inspector
from .model import menu_defaults, MENU_CTRL_TYPE_NAMES, M_CTRL, M_START
from .menueditor import MenuEditor

# 代码块功能简介(左栏"块"页悬停提示)
BLOCK_DESC = {
    N_SPEAK: "显示一句台词:逐字打字机,点击继续",
    N_LONG: "连续播放多句对话,自动高亮说话人",
    N_SPRITE: "显示/更新立绘:位置/缩放/透明度/动画",
    N_BG: "切换场景背景(图片或纯色,可选适配)",
    N_CHOICE: "显示选项,每个选项一个输出口",
    N_CODE: "插入一段 DexLang 代码(内容内联进导出)",
    N_DEFVAR: "定义全局/局部变量",
    N_SETVAR: "修改变量的值(可来自 value 输入)",
    N_IF: "条件分支:真/假两个输出口",
    N_GETVAR: "读取变量值(输出 value 供使用)",
    N_LOGIC: "逻辑比较:== != > < >= <=",
    N_MATH: "数学运算:+ - * / % ^",
    N_FLOW: "镜头控制:下一镜头/指定镜头/结束",
    N_FX_ADD: "添加场景特效(老电影/故障等,可叠加)",
    N_FX_OFF: "取消指定场景特效",
    N_FX_CLEAR: "取消当前全部场景特效",
    N_MENU: "运行/控制 MENU 菜单:独占运行、叠加显示/隐藏、发送信号",
    N_TEXT: "文本拼接:三段 A+B+C 拼成字符串(可留空),输出供动态文本等使用",
    N_SAY: "动态文本:显示 value 输入或字面文本,等待点击后清除",
    N_INPUT: "读取输入框:取指定菜单里输入框控件的当前内容",
    N_RANDOM: "随机数:生成 0..max-1 的随机整数(输出 value)",
    N_WAIT: "等待:暂停执行指定毫秒后继续",
    N_DOUT: "数据输出点:流程执行到这里时,把「数据」值送出镜头(供其它镜头读取)",
    N_DREF: "场景数据:读取其它镜头「数据输出点」送出的值",
    N_TEXTLIT: "纯文本:输出一段文字字面值(块上显示内容,长文本自动换行加宽)",
    N_NUMLIT: "纯数字:输出一个数字字面值",
    N_BOLLIT: "布尔:输出 true/false 字面值",
    N_BGM: "BGM 播放:循环播放背景音乐(已有则自动切换)",
    N_SE: "SE 播放:播放一次音效",
    N_SOUND_STOP: "停止声音:停止 BGM / SE / 全部",
    N_SOUND_SWAP: "切换声音:停止当前 BGM 并播放新的",
    N_BG_TRANS: "柔和换背景:新旧背景交叉淡化",
    N_SCENE_TRANS: "柔和切镜头:淡出黑场 → 切换 → 淡入",
    N_TOAST: "消息提示:在屏幕角落快速显示一条消息(可设四角/时长),淡入淡出后消失",
    N_SAVE: "存档:把所有变量写入文件(做存档用)",
    N_LOAD: "读档:从文件恢复所有变量",
    N_FILE_WRITE: "写入文件:把 value 内容写入文本文件(覆盖)",
    N_FILE_READ: "读取文件:读取文本文件内容(输出 value)",
    N_CHECKPOINT: "存档点:记录当前位置(存档数据包),可选把全部变量写入文件;跳转从这里之后继续",
    N_JUMP: "跳转到存档点:读档恢复变量并从存档点之后继续(适合读档页)",
}


class App(tk.Tk):
    def __init__(self, start_file=None):
        super().__init__()
        self.title("GAL 蓝图编辑器")
        self.geometry("1500x920")
        self.minsize(1100, 700)
        self.configure(bg=T.BG)

        self.project = project_defaults()
        self.file_path = ""
        self.project_root = ""
        self.drag_source = None
        self.dirty = False
        self.vm_path = settings_mod.get_vm_path()
        self.compiler = settings_mod.get_compiler()
        self._run_proc = None
        self._copied_res = []
        self.menu_windows = []          # 打开的菜单编辑器窗口
        self._menu_sel = -1             # 左栏菜单列表当前选中索引
        self._sel_char = -1             # 左栏人物列表当前选中索引
        # 撤销/重做(项目状态快照栈)
        self._undo_stack = []
        self._redo_stack = []
        self._restoring = False
        self._tip = None

        self.project_root = settings_mod.get_project_root()
        self._build_menu()
        self._build_toolbar()
        self._build_body()
        self._build_statusbar()

        if start_file and os.path.isfile(start_file):
            self.open_file(start_file)
        self._refresh_title()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self._autosave_loop()

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
        fm.add_command(label="完整资源导出…", command=self.export_full)
        fm.add_separator()
        fm.add_command(label="退出", command=self._on_close)
        m.add_cascade(label="文件", menu=fm)

        vm = tk.Menu(m, tearoff=0)
        vm.add_command(label="▶ 编译并运行 (VM)", command=self.run_preview)
        m.add_cascade(label="运行", menu=vm)

        sm = tk.Menu(m, tearoff=0)
        sm.add_command(label="项目设置…", command=self.settings_dialog)
        m.add_cascade(label="设置", menu=sm)

        hm = tk.Menu(m, tearoff=0)
        hm.add_command(label="关于", command=self._about)
        m.add_cascade(label="帮助", menu=hm)
        self.config(menu=m)

    def _build_toolbar(self):
        tb = tk.Frame(self, bg=T.BG2, height=42)
        tb.pack(fill="x", side="top")
        for txt, cmd in (("新建", self.new_project), ("打开…", self.open_file_dialog),
                         ("保存", self.save_file), ("导出代码", self.export),
                         ("完整导出", self.export_full)):
            tk.Button(tb, text=txt, command=cmd, bg=T.BG3, fg=T.FG,
                      activebackground=T.ACCENT_DARK, activeforeground="#fff",
                      relief="flat", font=T.FONT_SM, padx=12, pady=3,
                      cursor="hand2").pack(side="left", padx=(4, 0), pady=5)
        tk.Button(tb, text="🧹 自动整理", command=self.auto_layout,
                  bg=T.BG3, fg=T.FG, activebackground=T.ACCENT_DARK,
                  activeforeground="#fff", relief="flat", font=T.FONT_SM,
                  padx=12, pady=3, cursor="hand2").pack(side="left", padx=(4, 0), pady=5)
        self.run_btn = tk.Button(tb, text="▶ 运行", command=self.run_preview,
                                 bg=T.ACCENT, fg="#fff", activebackground=T.ACCENT_DARK,
                                 relief="flat", font=T.FONT_SM, padx=14, pady=3,
                                 cursor="hand2")
        self.run_btn.pack(side="left", padx=(10, 0), pady=5)
        self.stop_btn = tk.Button(tb, text="■ 停止", command=self.kill_vm,
                                  bg=T.BG3, fg="#666", activebackground="#ff8080",
                                  state="disabled", relief="flat", font=T.FONT_SM,
                                  padx=12, pady=3, cursor="hand2")
        self.stop_btn.pack(side="left", padx=(6, 0), pady=5)

    # ---------------- 主体 ----------------
    def _build_body(self):
        body = tk.Frame(self, bg=T.BG)
        body.pack(fill="both", expand=True, side="top")

        self.left = tk.Frame(body, bg=T.BG2, width=230)
        self.left.pack(side="left", fill="y")
        self.left.pack_propagate(False)
        self._build_left_panel()

        center = tk.Frame(body, bg=T.BG)
        center.pack(side="left", fill="both", expand=True)
        self.canvas = BlueCanvas(center, self)
        self.canvas.pack(fill="both", expand=True)

        self.right = tk.Frame(body, bg=T.BG2, width=300)
        self.right.pack(side="right", fill="y")
        self.right.pack_propagate(False)
        self.inspector = Inspector(self.right, self)
        self.inspector.pack(fill="both", expand=True)
        # 初始时填充左侧列表（镜头 / 人物）
        self.refresh_left()

    def _build_left_panel(self):
        nb = ttk.Notebook(self.left)
        nb.pack(fill="both", expand=True)

        # 镜头
        sc_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(sc_pg, text="镜头")
        self.scene_list = tk.Listbox(sc_pg, bg=T.BG3, fg=T.FG, relief="flat",
                                     selectbackground=T.ACCENT_DARK, font=T.FONT_SM)
        self.scene_list.pack(fill="both", expand=True, padx=6, pady=6)
        self.scene_list.bind("<<ListboxSelect>>", self._on_scene_pick)
        sr = tk.Frame(sc_pg, bg=T.BG2); sr.pack(fill="x", padx=6, pady=(0, 6))
        tk.Button(sr, text="＋镜头", command=self.add_scene, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True)
        tk.Button(sr, text="改名", command=self.rename_scene, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))
        tk.Button(sr, text="－删", command=self.del_scene, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))

        # 块
        bp_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(bp_pg, text="块")
        tk.Label(bp_pg, text="点击添加，或按住拖到画布", bg=T.BG2, fg=T.FG2,
                 font=T.FONT_SM, anchor="w").pack(fill="x", padx=8, pady=(6, 2))
        # 可滚动块列表(块多了放不下时用滚动条)
        bfw = tk.Canvas(bp_pg, bg=T.BG2, highlightthickness=0, bd=0)
        bsb = tk.Scrollbar(bp_pg, orient="vertical", command=bfw.yview)
        bf = tk.Frame(bfw, bg=T.BG2)
        _bf_win = bfw.create_window((0, 0), window=bf, anchor="nw")
        bf.bind("<Configure>",
                lambda e: bfw.configure(scrollregion=bfw.bbox("all")))
        bfw.bind("<Configure>",
                  lambda e: bfw.itemconfig(_bf_win, width=e.width))
        bfw.configure(yscrollcommand=bsb.set)
        bfw.pack(side="left", fill="both", expand=True, padx=(6, 0), pady=4)
        bsb.pack(side="right", fill="y", pady=4)
        self._block_canvas = bfw
        bfw.bind("<Enter>", lambda e: self._block_wheel_bind(True))
        bfw.bind("<Leave>", lambda e: self._block_wheel_bind(False))
        for t in PALETTE:
            self._panel_btn(bf, t)

        # 菜单(MENU 场景)
        mn_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(mn_pg, text="菜单")
        self.menu_list = tk.Listbox(mn_pg, bg=T.BG3, fg=T.FG, relief="flat",
                                    selectbackground=T.ACCENT_DARK, font=T.FONT_SM)
        self.menu_list.pack(fill="both", expand=True, padx=6, pady=6)
        self.menu_list.bind("<<ListboxSelect>>", self._on_menu_pick)
        self.menu_list.bind("<Double-Button-1>", lambda _e: self.edit_menu())
        mr = tk.Frame(mn_pg, bg=T.BG2); mr.pack(fill="x", padx=6, pady=(0, 6))
        tk.Button(mr, text="＋菜单", command=self.add_menu, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True)
        tk.Button(mr, text="编辑", command=self.edit_menu, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))
        tk.Button(mr, text="－删", command=self.del_menu, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))
        mr2 = tk.Frame(mn_pg, bg=T.BG2); mr2.pack(fill="x", padx=6, pady=(0, 6))
        tk.Button(mr2, text="导入…", command=self.import_menu, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True)
        tk.Button(mr2, text="导出…", command=self.export_menu, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))
        tk.Label(mn_pg, text="双击菜单名打开编辑器；导入/导出用 .menubluescene 文件", bg=T.BG2, fg=T.FG2,
                 font=T.FONT_SM, anchor="w").pack(fill="x", padx=8, pady=(0, 6))

        # 人物
        ch_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(ch_pg, text="人物")
        self.char_list = tk.Listbox(ch_pg, bg=T.BG3, fg=T.FG, relief="flat",
                                    selectbackground=T.ACCENT_DARK, font=T.FONT_SM)
        self.char_list.pack(fill="both", expand=True, padx=6, pady=6)
        self.char_list.bind("<<ListboxSelect>>", self._on_char_pick)
        cr = tk.Frame(ch_pg, bg=T.BG2); cr.pack(fill="x", padx=6, pady=(0, 6))
        tk.Button(cr, text="＋人物", command=self.add_char, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True)
        tk.Button(cr, text="＋状态", command=self.add_char_state, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM).pack(side="left", fill="x", expand=True, padx=(4, 0))

        # 项目信息
        pj_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(pj_pg, text="项目")
        self._build_project_panel(pj_pg)

        # 全局设置(块默认属性)
        gs_pg = tk.Frame(nb, bg=T.BG2)
        nb.add(gs_pg, text="全局设置")
        self._build_global_settings_panel(gs_pg)

    def _panel_btn(self, parent, t):
        fill = NODE_COLORS.get(t, "#555")
        name = NODE_NAMES.get(t, "?")
        b = tk.Button(parent, text=f"{NODE_ICONS.get(t, '')} {name}",
                      bg=fill, fg="#fff", activebackground=T.ACCENT_DARK,
                      relief="flat", font=T.FONT_SM, anchor="w", pady=4,
                      cursor="hand2")
        b.pack(fill="x", pady=2)
        b.bind("<ButtonPress-1>", lambda e, tt=t: self._panel_drag(tt, e))
        b.bind("<B1-Motion>", lambda e: self._panel_motion(e))
        b.bind("<ButtonRelease-1>", lambda e: self._panel_release(e))
        desc = BLOCK_DESC.get(t, "")
        if desc:
            b.bind("<Enter>", lambda e, d=desc: self._show_tip(d, e))
            b.bind("<Leave>", lambda e: self._hide_tip())
        return b

    def _show_tip(self, text, ev):
        """块面板悬停功能提示。"""
        try:
            if self._tip is not None:
                self._tip.destroy()
        except Exception:
            pass
        t = tk.Toplevel(self)
        t.wm_overrideredirect(True)
        t.wm_geometry(f"+{ev.x_root + 14}+{ev.y_root + 14}")
        tk.Label(t, text=text, bg="#20242e", fg="#e8e8f0", font=T.FONT_SM,
                 justify="left", padx=10, pady=6, relief="solid", bd=1).pack()
        self._tip = t

    def _hide_tip(self):
        try:
            if self._tip is not None:
                self._tip.destroy()
                self._tip = None
        except Exception:
            pass

    def _panel_drag(self, t, ev):
        """按住块按钮:进入拖拽/点击模式,画布显示幽灵块。"""
        self.drag_source = t
        self._panel_press = (ev.x_root, ev.y_root)
        self.canvas._ghost = (t, 0, 0)

    def _panel_motion(self, ev):
        """按住拖动:让幽灵块跟随鼠标(用全局坐标换算画布位置)。"""
        if self.drag_source is None:
            return
        cv = self.canvas
        gx, gy = cv.winfo_pointerxy()
        lx = gx - cv.winfo_rootx()
        ly = gy - cv.winfo_rooty()
        cv._ghost = (self.drag_source, lx, ly)
        cv.render()

    def _panel_release(self, ev):
        """松开块按钮:指针在画布上 → 在该处创建;否则当作点击,在画布中央创建。"""
        t = getattr(self, "drag_source", None)
        self.drag_source = None
        self.canvas._ghost = None
        if t is None:
            return
        cv = self.canvas
        gx, gy = cv.winfo_pointerxy()
        w = cv.winfo_containing(gx, gy)
        if w is cv:
            wx, wy = cv.s2w(gx - cv.winfo_rootx(), gy - cv.winfo_rooty())
            self.add_node_at(t, wx, wy)
        else:
            self._panel_add(t)

    def _panel_add(self, t):
        sc = self.scene()
        if not sc:
            return
        cv = self.canvas
        wx, wy = cv.s2w(cv.winfo_width() // 2, cv.winfo_height() // 2)
        self.add_node_at(t, wx, wy)

    def _block_wheel_bind(self, on):
        """进入块面板时劫持滚轮用于滚动块列表,离开恢复。"""
        try:
            if on:
                self.bind_all("<MouseWheel>", self._block_wheel)
            else:
                self.unbind_all("<MouseWheel>")
        except Exception:
            pass

    def _block_wheel(self, e):
        try:
            cv = getattr(self, "_block_canvas", None)
            if cv is not None:
                cv.yview_scroll(int(-e.delta / 120), "units")
        except Exception:
            pass

    # ---------------- 项目信息 ----------------
    def _build_project_panel(self, parent):
        self._pj = {}

        def label(txt):
            tk.Label(parent, text=txt, bg=T.BG2, fg=T.FG2,
                     font=T.FONT_SM, anchor="w").pack(fill="x", padx=10, pady=(10, 2))

        def entry(key):
            var = tk.StringVar()
            e = tk.Entry(parent, textvariable=var, bg=T.BG3, fg=T.FG,
                         relief="flat", font=T.FONT_SM)
            e.pack(fill="x", padx=10)
            e.bind("<KeyRelease>", lambda _ev, k=key: self._pj_commit(k))
            self._pj[key] = var

        label("标题名称")
        entry("title")
        label("窗口宽 / 高 (px)")
        wr = tk.Frame(parent, bg=T.BG2); wr.pack(fill="x", padx=10)
        for k in ("win_w", "win_h"):
            self._pj[k] = tk.StringVar()
            e = tk.Entry(wr, textvariable=self._pj[k], bg=T.BG3, fg=T.FG,
                         relief="flat", font=T.FONT_SM)
            e.pack(side="left", fill="x", expand=True, padx=(0, 6))
            e.bind("<KeyRelease>", lambda _ev, kk=k: self._pj_commit(kk))
        label("资源目录 (相对项目根, 如 res)")
        entry("resDir")
        label("作者")
        entry("author")
        label("版本")
        entry("version")
        label("简介")
        self._pj["description"] = tk.Text(parent, bg=T.BG3, fg=T.FG,
                                           relief="flat", font=T.FONT_SM,
                                           height=5, wrap="word")
        self._pj["description"].pack(fill="x", padx=10, pady=(0, 10))
        self._pj["description"].bind("<KeyRelease>",
                                      lambda _ev: self._pj_commit("description"))

    def _pj_commit(self, key):
        p = self.project
        try:
            if key == "win_w":
                p.win_w = max(320, min(3840, int(self._pj["win_w"].get())))
            elif key == "win_h":
                p.win_h = max(240, min(2160, int(self._pj["win_h"].get())))
            elif key == "description":
                p.description = self._pj["description"].get("1.0", "end-1c")
            else:
                setattr(p, key, self._pj[key].get())
        except Exception:
            pass
        self.dirty = True
        self._refresh_title()

    def refresh_project_panel(self):
        if not hasattr(self, "_pj"):
            return
        p = self.project
        for key in ("title", "resDir", "author", "version"):
            if key in self._pj:
                self._pj[key].set(getattr(p, key, "") or "")
        self._pj["win_w"].set(str(getattr(p, "win_w", 960)))
        self._pj["win_h"].set(str(getattr(p, "win_h", 540)))
        if "description" in self._pj:
            self._pj["description"].delete("1.0", "end")
            self._pj["description"].insert("1.0", p.description or "")

    # ---------------- 全局设置(块默认属性) ----------------
    def _build_global_settings_panel(self, parent):
        """全局块默认属性:新建块时遵循,单个块仍可在右侧属性栏单独改。"""
        cv = tk.Canvas(parent, bg=T.BG2, highlightthickness=0)
        sb = tk.Scrollbar(parent, orient="vertical", command=cv.yview)
        body = tk.Frame(cv, bg=T.BG2)
        body.bind("<Configure>", lambda e: cv.configure(scrollregion=cv.bbox("all")))
        cv.create_window((0, 0), window=body, anchor="nw")
        cv.configure(yscrollcommand=sb.set)
        cv.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        def _wheel(e):
            cv.yview_scroll(int(-e.delta / 120), "units")
        cv.bind("<Enter>", lambda e: cv.bind_all("<MouseWheel>", _wheel))
        cv.bind("<Leave>", lambda e: cv.unbind_all("<MouseWheel>"))

        self._gs = {}
        d = settings_mod.get_block_defaults()

        def sec(txt):
            tk.Label(body, text=txt, bg=T.BG2, fg=T.ACCENT,
                     font=T.FONT_BOLD, anchor="w").pack(fill="x", padx=10, pady=(12, 2))

        def glabel(txt):
            tk.Label(body, text=txt, bg=T.BG2, fg=T.FG2,
                     font=T.FONT_SM, anchor="w").pack(fill="x", padx=10, pady=(8, 2))

        def combo(key, items, init):
            idx = init if 0 <= init < len(items) else 0
            var = tk.StringVar(value=items[idx])
            cb = ttk.Combobox(body, textvariable=var, values=items,
                              state="readonly", font=T.FONT_SM)
            cb.pack(fill="x", padx=10)
            cb.bind("<<ComboboxSelected>>", lambda _e, k=key: self._gs_commit(k))
            self._gs[key] = var

        def check(key, label, init):
            var = tk.BooleanVar(value=bool(init))
            tk.Checkbutton(body, text=label, variable=var, bg=T.BG2, fg=T.FG,
                           activebackground=T.BG2, selectcolor=T.BG3,
                           font=T.FONT_SM, anchor="w",
                           command=lambda k=key: self._gs_commit(k)).pack(fill="x", padx=6, pady=1)
            self._gs[key] = var

        def entry(key, init):
            var = tk.StringVar(value=str(init))
            e = tk.Entry(body, textvariable=var, bg=T.BG3, fg=T.FG,
                         relief="flat", font=T.FONT_SM)
            e.pack(fill="x", padx=10)
            e.bind("<KeyRelease>", lambda _ev, k=key: self._gs_commit(k))
            self._gs[key] = var

        sec("外观风格")
        glabel("整体风格(对话框/选项/菜单颜色与特效,含鼠标拖尾)")
        combo("gal_theme", settings_mod.GAL_THEMES,
              settings_mod.GAL_THEMES.index(settings_mod.get_gal_theme())
              if settings_mod.get_gal_theme() in settings_mod.GAL_THEMES else 0)
        trail = tk.BooleanVar(value=settings_mod.get_trail_enabled())
        tk.Checkbutton(body, text="☑ 鼠标拖尾(风格自带,可关闭)", variable=trail,
                       bg=T.BG2, fg=T.FG, selectcolor=T.BG3, activebackground=T.BG2,
                       font=T.FONT_SM, anchor="w",
                       command=lambda: settings_mod.set_trail_enabled(trail.get())
                       ).pack(fill="x", padx=10, pady=(2, 1))
        tk.Label(body, text="风格作用于最终运行画面;「默认」= 经典样式。",
                 bg=T.BG2, fg=T.FG2, font=T.FONT_SM, justify="left"
                 ).pack(fill="x", padx=10, pady=(8, 4))
        sec("文本 (说话/长对话/选项框)")
        glabel("显示模式")
        combo("text_mode", TEXT_MODE_NAMES, d.get("text_mode", 0))
        glabel("逐字速度 (毫秒/字, 0=引擎默认40)")
        entry("text_speed", d.get("text_speed", 60))
        sec("说话")
        glabel("文字位置")
        combo("text_pos", POS_NAMES, d.get("text_pos", 0))
        check("show_name", "☑ 显示人名", d.get("show_name", 1))
        check("use_char", "☑ 使用人物集", d.get("use_char", 0))
        sec("显示立绘")
        glabel("预设位置")
        combo("spr_pos_mode", POS_NAMES, d.get("spr_pos_mode", 0))
        glabel("图层")
        combo("spr_layer", LAYER_NAMES, d.get("spr_layer", 0))
        glabel("缩放 %")
        entry("spr_scale", d.get("spr_scale", 100))
        glabel("透明度 0-255")
        entry("spr_alpha", d.get("spr_alpha", 255))
        glabel("动画")
        combo("spr_anim", ["无", "呼吸", "淡入", "上浮", "抖动", "脉冲", "消失"],
              d.get("spr_anim", 0))
        glabel("适配")
        combo("spr_fit", SPRITE_FIT_NAMES, d.get("spr_fit", 0))
        sec("背景")
        check("bg_mode", "☑ 使用纯色背景", d.get("bg_mode", 0))
        glabel("颜色 #RRGGBB")
        entry("bg_color", f"0x{d.get('bg_color', 0):06X}")
        glabel("适配模式")
        combo("bg_fit", BG_FIT_NAMES, d.get("bg_fit", 0))
        tk.Label(body, text="新建块时自动采用以上默认值;\n每个块仍可在右侧属性栏单独修改。",
                 bg=T.BG2, fg=T.FG2, font=T.FONT_SM, justify="left"
                 ).pack(fill="x", padx=10, pady=(12, 8))

    def _gs_commit(self, key):
        if key == "gal_theme":
            settings_mod.set_gal_theme(self._gs[key].get())
            return
        d = settings_mod.get_block_defaults()
        try:
            if key in ("text_mode", "text_pos", "spr_pos_mode", "spr_layer", "spr_anim", "bg_fit", "spr_fit"):
                items = {"text_mode": TEXT_MODE_NAMES, "text_pos": POS_NAMES,
                         "spr_pos_mode": POS_NAMES, "spr_layer": LAYER_NAMES,
                         "bg_fit": BG_FIT_NAMES, "spr_fit": SPRITE_FIT_NAMES,
                         "spr_anim": ["无", "呼吸", "淡入", "上浮", "抖动", "脉冲", "消失"]}[key]
                d[key] = items.index(self._gs[key].get())
            elif key in ("show_name", "use_char", "bg_mode"):
                d[key] = 1 if self._gs[key].get() else 0
            elif key in ("spr_scale", "spr_alpha", "text_speed"):
                d[key] = max(0, min(2000, int(self._gs[key].get())))
            elif key == "bg_color":
                s = self._gs[key].get().strip().lstrip("#")
                d[key] = int(s, 16) & 0xFFFFFF
        except Exception:
            return
        settings_mod.set_block_defaults(d)

    def _apply_block_defaults(self, n):
        """新建块时套用全局默认属性(之后仍可单独修改)。"""
        d = settings_mod.get_block_defaults()
        if n.type == N_SPEAK:
            for k in ("text_pos", "show_name", "use_char"):
                if k in d:
                    setattr(n, k, d[k])
        if n.type in (N_SPEAK, N_LONG, N_CHOICE) and "text_mode" in d:
            n.text_mode = d["text_mode"]
        if n.type in (N_SPEAK, N_LONG, N_CHOICE) and "text_speed" in d:
            n.text_speed = d["text_speed"]
        elif n.type == N_SPRITE:
            for k in ("spr_pos_mode", "spr_layer", "spr_scale", "spr_alpha", "spr_anim", "spr_fit"):
                if k in d:
                    setattr(n, k, d[k])
        elif n.type == N_BG:
            for k in ("bg_mode", "bg_color", "bg_fit"):
                if k in d:
                    setattr(n, k, d[k])

    # ---------------- 状态栏 ----------------
    def _build_statusbar(self):
        self.status = tk.Label(self, text="就绪", bg=T.BG2, fg=T.FG2,
                               font=T.FONT_SM, anchor="w", padx=8)
        self.status.pack(fill="x", side="bottom")

    def set_status(self, msg, sticky=True):
        self.status.config(text=msg)

    def _refresh_title(self):
        name = os.path.basename(self.file_path) if self.file_path else "未命名"
        star = " *" if self.dirty else ""
        self.title(f"{name}{star} — GAL 蓝图编辑器")

    # ---------------- 镜头 ----------------
    def refresh_left(self):
        self.scene_list.delete(0, "end")
        for i, s in enumerate(self.project.scenes):
            tag = "▸ " if i == self.project.cur else "  "
            self.scene_list.insert("end", f"{tag}{s.title}")
        self.char_list.delete(0, "end")
        for i, c in enumerate(self.project.chars):
            self.char_list.insert("end", c.name or f"人物{i}")
        if 0 <= self._sel_char < len(self.project.chars):
            self.char_list.selection_set(self._sel_char)
            self.char_list.activate(self._sel_char)
        self.refresh_menu_list()

    def _on_scene_pick(self, _e=None):
        sel = self.scene_list.curselection()
        if sel:
            self.project.cur = sel[0]
            self.canvas.selected = None
            self.canvas.render()
            self.inspector.rebuild()
            self.refresh_left()

    def _on_char_pick(self, _e=None):
        """选中左栏人物:右侧属性面板显示人物信息。"""
        sel = self.char_list.curselection()
        if sel:
            self._sel_char = sel[0]
            self.inspector.show_char(sel[0])

    # ---------------- 菜单 (MENU 场景) ----------------
    def _on_menu_pick(self, _e=None):
        sel = self.menu_list.curselection()
        if sel:
            self._menu_sel = sel[0]

    def _menu_sel_index(self):
        sel = self.menu_list.curselection()
        if sel:
            self._menu_sel = sel[0]
            return sel[0]
        return self._menu_sel if 0 <= self._menu_sel < len(self.project.menus) else -1

    def add_menu(self):
        if len(self.project.menus) >= 32:
            messagebox.showinfo("菜单", "最多 32 个菜单。", parent=self)
            return
        self.project.menus.append(menu_defaults(f"菜单 {len(self.project.menus) + 1}"))
        self._push_snapshot()
        self.dirty = True
        self.refresh_left()
        self._refresh_title()
        w = MenuEditor(self, len(self.project.menus) - 1)
        self.menu_windows.append(w)

    def edit_menu(self):
        i = self._menu_sel_index()
        if i < 0:
            if not self.project.menus:
                self.add_menu()
                return
            i = len(self.project.menus) - 1
        # 若已打开则聚焦
        for w in self.menu_windows:
            if w.index == i:
                try:
                    w.lift(); w.focus_set()
                except Exception:
                    pass
                return
        w = MenuEditor(self, i)
        self.menu_windows.append(w)

    def del_menu(self):
        if not self.project.menus:
            return
        i = self._menu_sel_index()
        if i < 0:
            messagebox.showinfo("菜单", "先在左侧选中一个菜单。", parent=self)
            return
        if not messagebox.askyesno("删除菜单",
                                   f"确定删除菜单「{self.project.menus[i].title}」？", parent=self):
            return
        del self.project.menus[i]
        # 关闭引用该菜单的编辑器
        for w in list(self.menu_windows):
            if w.index == i:
                try:
                    w._close()
                except Exception:
                    pass
        for w in list(self.menu_windows):
            if w.index > i:
                w.index -= 1
                w.on_project_restored()
        self._push_snapshot()
        self.dirty = True
        self.refresh_left()
        self._refresh_title()

    def refresh_menu_list(self):
        if not hasattr(self, "menu_list"):
            return
        self.menu_list.delete(0, "end")
        for i, m in enumerate(self.project.menus):
            mode = "独" if m.mode == 0 else "叠"
            warn = " !" if (m.mode == 0 and not m.has_exit_path()) else ""
            self.menu_list.insert("end", f"[{mode}] {m.title}{warn}")
        # 重建后恢复选中(否则列表内容变化会丢高亮/选中)
        if self._menu_sel >= len(self.project.menus):
            self._menu_sel = len(self.project.menus) - 1
        if 0 <= self._menu_sel < len(self.project.menus):
            self.menu_list.selection_clear(0, "end")
            self.menu_list.selection_set(self._menu_sel)
            self.menu_list.see(self._menu_sel)

    def import_menu(self):
        """从 .menubluescene 文件导入一个菜单到当前项目。"""
        p = filedialog.askopenfilename(
            title="导入菜单",
            filetypes=[("MENU 场景", "*.menubluescene"), ("全部", "*.*")],
            parent=self)
        if not p:
            return
        m = sceneio.load_menu(p)
        if m is None:
            messagebox.showwarning("导入", "无法读取菜单文件。", parent=self)
            return
        # 重名自动加序号
        base = m.title
        names = {x.title for x in self.project.menus}
        i = 1
        while m.title in names:
            m.title = f"{base} {i}"
            i += 1
        self.project.menus.append(m)
        self._menu_sel = len(self.project.menus) - 1
        self._push_snapshot()
        self.dirty = True
        self.refresh_left()
        self._refresh_title()
        self.set_status(f"✅ 已导入菜单：{m.title}")
        w = MenuEditor(self, len(self.project.menus) - 1)
        self.menu_windows.append(w)

    def export_menu(self):
        """把选中的菜单导出为 .menubluescene 文件(可换项目复用)。"""
        i = self._menu_sel_index()
        if i < 0:
            messagebox.showinfo("导出", "先在左侧选中一个菜单。", parent=self)
            return
        m = self.project.menus[i]
        root = self.project_root or os.getcwd()
        p = filedialog.asksaveasfilename(
            title="导出菜单",
            defaultextension=".menubluescene",
            initialdir=root,
            initialfile=f"{m.title}.menubluescene",
            filetypes=[("MENU 场景", "*.menubluescene")],
            parent=self)
        if p:
            if sceneio.save_menu(p, m):
                self.set_status(f"✅ 已导出菜单：{p}")
            else:
                self._warn_export("导出")

    def _warn_export(self, title):
        """导出失败的统一提示:附上 export_mod.last_error 的具体原因。"""
        reason = getattr(export_mod, "last_error", None)
        msg = "导出失败。" if not reason else f"导出失败。\n\n{reason}"
        messagebox.showwarning(title, msg, parent=self)

    def add_scene(self):
        if len(self.project.scenes) >= 32:
            return
        self.project.scenes.append(scene_defaults(f"镜头 {len(self.project.scenes) + 1}"))
        self.project.cur = len(self.project.scenes) - 1
        self.canvas.selected = None
        self.dirty = True
        self.on_scene_changed()

    def del_scene(self):
        if len(self.project.scenes) <= 1:
            return
        if not messagebox.askyesno("删除镜头", "确定删除当前镜头？"):
            return
        del self.project.scenes[self.project.cur]
        self.project.cur = min(self.project.cur, len(self.project.scenes) - 1)
        self.dirty = True
        self.on_scene_changed()

    def rename_scene(self):
        sc = self.scene()
        if sc:
            name = simpledialog.askstring("重命名镜头", "镜头名称：", parent=self,
                                          initialvalue=sc.title)
            if name:
                sc.title = name
                self.dirty = True
                self.refresh_left()
                self.canvas.render()

    # ---------------- 人物 ----------------
    def add_char(self):
        name = simpledialog.askstring("添加人物", "人物名称：", parent=self)
        if not name:
            return
        c = Character()
        c.name = name
        self.project.chars.append(c)
        self.dirty = True
        self.refresh_left()
        self.inspector.rebuild()

    def add_char_state(self):
        sel = self.char_list.curselection()
        if not sel or sel[0] >= len(self.project.chars):
            messagebox.showinfo("提示", "先选中一个人物。", parent=self)
            return
        ch = self.project.chars[sel[0]]
        p = filedialog.askopenfilename(title="选择状态图",
                                       filetypes=[("图片", "*.png *.jpg *.jpeg *.bmp *.webp")],
                                       parent=self)
        if not p:
            return
        stname = simpledialog.askstring("状态名", "状态名称（如：微笑/生气）：", parent=self)
        if ch.nStates >= 10:
            return
        ch.states[ch.nStates] = p
        ch.stateNames[ch.nStates] = stname or f"状态{ch.nStates}"
        ch.nStates += 1
        self.dirty = True
        self.refresh_left()

    # ---------------- 画布 / 节点 ----------------
    def add_node_at(self, ntype, x, y):
        sc = self.scene()
        if not sc:
            return
        n = sc.new_node(ntype, x, y)
        self._apply_block_defaults(n)
        self.canvas.selected = n.id
        self.dirty = True
        self.on_scene_changed()

    def auto_layout(self):
        """自动整理当前镜头:从前到后分层、层内排序减少交叉、块不重叠。"""
        sc = self.scene()
        if not sc:
            return
        from . import layout as layout_mod
        starts = [sc.entry_id] if sc.entry_id >= 0 else []
        layout_mod.auto_layout(sc, starts,
                               width_fn=self.canvas.node_width,
                               height_fn=self.canvas.node_height)
        self.on_scene_changed()
        self.set_status("🧹 已自动整理当前镜头")

    def on_select(self):
        """选中节点变化。"""
        self.inspector.flush()
        self.canvas.render()
        self.inspector.rebuild()

    def on_node_edited(self):
        """节点属性被修改:只刷新画布摘要,不重建属性面板(避免丢焦点/递归)。"""
        self._push_snapshot()
        self.dirty = True
        self.canvas.render()
        self._refresh_title()

    def on_scene_changed(self, keep_selection=True):
        """结构变化(添加/删除/移动/连线/选项增删):刷新画布并重建属性面板。"""
        # 递归深度保护:防止 flush→commit→on_scene_changed 意外重入导致卡死
        depth = getattr(self, "_scene_change_depth", 0) + 1
        self._scene_change_depth = depth
        try:
            if depth > 8:
                return
            self._push_snapshot()
            self.dirty = True
            self.inspector.flush()
            self.sync_menu_blocks()
            self.canvas.render()
            self.inspector.rebuild()
            # 刷新左侧镜头/人物列表，保证 UI 状态一致
            self.refresh_left()
            self._refresh_title()
        finally:
            self._scene_change_depth = depth - 1

    def sync_menu_blocks(self):
        """所有镜头里的菜单块:按引用菜单的退出节点同步输出口(菜单退出增删/改名后调用)。"""
        for sc in self.project.scenes:
            for n in sc.nodes:
                if n.type == N_MENU:
                    m = self.project.menus[n.menu_idx] \
                        if 0 <= n.menu_idx < len(self.project.menus) else None
                    n.sync_menu_outputs(m)

    # ---------------- 撤销 / 重做 ----------------
    def _push_snapshot(self):
        """把当前项目状态压入撤销栈(与栈顶相同则跳过)。"""
        if self._restoring:
            return
        snap = copy.deepcopy(project_to_dict(self.project))
        if self._undo_stack and self._undo_stack[-1] == snap:
            return
        self._undo_stack.append(snap)
        if len(self._undo_stack) > 60:
            self._undo_stack.pop(0)
        self._redo_stack.clear()

    def undo(self):
        """撤销上一步操作(Ctrl+Z)。"""
        if not self._undo_stack:
            return
        self._restoring = True
        try:
            self._redo_stack.append(copy.deepcopy(project_to_dict(self.project)))
            snap = self._undo_stack.pop()
            self.project = project_from_dict(copy.deepcopy(snap))
            self.dirty = True
            self.canvas.selected = None
            self.on_scene_changed()
        finally:
            self._restoring = False
        self._refresh_title()
        self.set_status("↩ 已撤销")

    def redo(self):
        """重做(Ctrl+Y / Ctrl+Shift+Z)。"""
        if not self._redo_stack:
            return
        self._restoring = True
        try:
            self._undo_stack.append(copy.deepcopy(project_to_dict(self.project)))
            snap = self._redo_stack.pop()
            self.project = project_from_dict(copy.deepcopy(snap))
            self.dirty = True
            self.canvas.selected = None
            self.on_scene_changed()
        finally:
            self._restoring = False
        self._refresh_title()
        self.set_status("↪ 已重做")

    def delete_selected(self):
        """删除选中的节点(Delete 键)。"""
        nid = self.canvas.selected
        if nid is not None:
            self.canvas._delete(nid)

    # ---------------- 间隔自动保存 ----------------
    def _autosave_loop(self):
        try:
            a = settings_mod.get_autosave()
            if a["enabled"] and self.file_path and self.dirty:
                self.inspector.flush()
                sceneio.save_project(self.file_path, self.project)
                self.dirty = False
                self._refresh_title()
                self.set_status("🕒 已自动保存")
        except Exception:
            pass
        interval = max(1, a["interval"]) * 60000
        self.after(interval, self._autosave_loop)

    # ---------------- 菜单独立预览 ----------------
    def preview_menu(self, index):
        """只预览单个菜单:生成临时项目(镜头只运行该菜单),编译并用 VM 打开。"""
        self.inspector.flush()
        if not (0 <= index < len(self.project.menus)):
            return
        m = self.project.menus[index]
        vm = self.vm_path
        if not vm or not os.path.isfile(vm):
            messagebox.showwarning("预览",
                                   "请先在「设置」中配置 VM 位置。", parent=self)
            self.settings_dialog()
            return
        if self._run_proc is not None and self._run_proc.poll() is None:
            messagebox.showinfo("预览", "已有 VM 正在运行。", parent=self)
            return
        import tempfile
        tmp = tempfile.mkdtemp(prefix="dexmenu_")
        p = project_defaults()
        p.title = m.title
        p.win_w = self.project.win_w
        p.win_h = self.project.win_h
        sc = scene_defaults(m.title)
        entry = sc.node(sc.entry_id)
        exitn = sc.node(sc.exit_id)
        mn = sc.new_node(N_MENU, 300, 200)
        mn.menu_idx = 0
        mn.action = 0 if m.mode == 0 else 1
        sc.links.append(Link(sc.lid, entry.id, 0, mn.id, 0)); sc.lid += 1
        sc.links.append(Link(sc.lid, mn.id, 0, exitn.id, 0)); sc.lid += 1
        p.scenes = [sc]
        p.menus = [m]
        dex = export_mod.export_project(p, tmp)
        if not dex:
            self._warn_export("预览")
            return
        bc = os.path.join(tmp, "scene_out.dexbc")
        self.set_status("菜单预览编译中…")
        self.update_idletasks()
        try:
            compile_mod.compile_dex_file(dex, bc)
        except Exception as e:
            messagebox.showwarning("预览",
                                   f"编译失败：\n{compile_mod.compile_error_text(e)}",
                                   parent=self)
            return
        self._copied_res = self._copy_resources_to(os.path.dirname(os.path.abspath(vm)))
        self.set_status("▶ 菜单预览运行中…（关闭游戏窗口即结束,可继续编辑）")
        self.run_btn.config(text="● 运行中", bg=T.WARN, fg="#102")
        self._run_proc = subprocess.Popen([vm, bc], cwd=tmp)
        self._set_stop_btn(True)
        self._poll_run()

    # ---------------- 运行 (外部 VM) ----------------
    def _set_stop_btn(self, active):
        """停止按钮:平时灰色禁用,VM 运行时激活显示鲜艳颜色。"""
        if not hasattr(self, "stop_btn"):
            return
        if active:
            self.stop_btn.config(state="normal", bg=T.WARN, fg="#102")
        else:
            self.stop_btn.config(state="disabled", bg=T.BG3, fg="#666")

    def kill_vm(self):
        """主动停止/杀死正在运行的 VM 进程(窗口关了但进程残留时也用它)。"""
        proc = self._run_proc
        if proc is None:
            self.set_status("没有正在运行的 VM。")
            return
        pid = proc.pid
        try:
            proc.kill()
        except Exception:
            pass
        self._run_proc = None
        self.run_btn.config(text="▶ 运行", bg=T.ACCENT, fg="#fff")
        self._set_stop_btn(False)
        self._cleanup_resources()
        # 清理该 VM 解包的临时 DLL(进程被杀时不会自行删除)
        if pid:
            try:
                tmp = os.environ.get("TEMP", os.environ.get("TMP", "/tmp"))
                for n in os.listdir(tmp):
                    if n.startswith("dexvm_%d_" % pid) and n.endswith(".dll"):
                        try:
                            os.remove(os.path.join(tmp, n))
                        except Exception:
                            pass
            except Exception:
                pass
        self.set_status("⛔ 已停止 VM")

    def run_preview(self):
        """外部运行:导出 → 编译 → 用 VM 打开独立窗口(所见即所得)。"""
        self.inspector.flush()
        # 需要 VM 与编译器配置
        vm = self.vm_path
        if not vm or not os.path.isfile(vm):
            messagebox.showwarning("运行",
                                   "请先在「设置」中配置 VM 位置(如 vm/vm.exe 或 vm/galrun.exe)。",
                                   parent=self)
            self.settings_dialog()
            return
        if self._run_proc is not None and self._run_proc.poll() is None:
            if messagebox.askyesno("运行", "已有 VM 正在运行。\n要先停止它再运行吗？", parent=self):
                self.kill_vm()
            else:
                return
        # 1) 导出
        root = self.project_root or os.getcwd()
        dex = export_mod.export_project(self.project, root)
        if not dex:
            self._warn_export("运行")
            return
        # 2) 编译(进程内 dexlang,无需外部解释器)
        bc = os.path.join(root, "scene_out.dexbc")
        self.set_status("编译中…")
        self.update_idletasks()
        try:
            compile_mod.compile_dex_file(dex, bc)
        except Exception as e:
            messagebox.showwarning("运行",
                                   f"编译失败：\n{compile_mod.compile_error_text(e)}",
                                   parent=self)
            return
        # 3) 复制项目图片资源到 VM 的 res 目录(防止资源引用问题;运行完删除)
        self._copied_res = self._copy_resources()
        # 4) 运行 VM(独立进程;cwd 用项目根,资源相对路径与 DLL 相对加载都正确)
        self.set_status("▶ 运行中…（关闭游戏窗口即结束,可继续编辑）")
        self.run_btn.config(text="● 运行中", bg=T.WARN, fg="#102")
        self._run_proc = subprocess.Popen([vm, bc], cwd=root)
        self._set_stop_btn(True)
        self._poll_run()

    def _poll_run(self):
        if self._run_proc is not None:
            rc = self._run_proc.poll()
            if rc is not None:
                self._run_proc = None
                self.run_btn.config(text="▶ 运行", bg=T.ACCENT, fg="#fff")
                self._set_stop_btn(False)
                self._cleanup_resources()
                self.set_status("✅ 运行结束,已清理复制到 VM 的临时资源,可继续编辑")
                return
        self.after(500, self._poll_run)

    # ---------------- 运行资源复制(临时复制到 VM 的 res 目录) ----------------
    def _collect_resources(self):
        """收集项目引用的所有图片资源(场景背景 + 背景块 + 立绘)。返回绝对路径列表。"""
        root = self.project_root or os.getcwd()
        res = set()

        def add(p):
            if not p:
                return
            ap = p if os.path.isabs(p) else os.path.join(root, p)
            if os.path.isfile(ap):
                res.add(os.path.abspath(ap))

        for sc in self.project.scenes:
            if sc.useBgImg and sc.bg:
                add(sc.bg)
            for n in sc.nodes:
                if n.type == N_BG:
                    add(n.bg_path)
                elif n.type == N_SPRITE:
                    if n.use_char:
                        add(char_state_path(self.project, n.char_idx, n.state_idx))
                    else:
                        add(n.spr_path)
                elif n.type == N_SPEAK:
                    if n.use_char:
                        add(char_state_path(self.project, n.char_idx, n.state_idx))
                elif n.type == N_LONG:
                    for ci, _ in n.lines:
                        if ci >= 0:
                            add(char_state_path(self.project, ci, 0))
        # 菜单:独占菜单背景图 + 控件背景可能是图片
        for m in self.project.menus:
            if m.bg:
                add(m.bg)
            for n in m.nodes:
                if n.type == M_CTRL and n.bg and not n.bg.startswith("#"):
                    add(n.bg)
        return list(res)

    def _copy_resources_to(self, dest_dir):
        """按项目相对路径结构,把引用的图片资源复制到 dest_dir。返回复制目标列表。
        项目内资源保持相对结构(如 res/bg.png → dest/res/bg.png);
        项目外资源(绝对路径/跨目录)放到 dest/res/ 下按文件名,避免 ..\\ 逃逸出输出目录。"""
        root = self.project_root or os.getcwd()
        copied = []
        for abs_src in self._collect_resources():
            try:
                rel = os.path.relpath(abs_src, root)
            except ValueError:
                rel = None
            if rel and not rel.startswith("..") and not os.path.isabs(rel):
                dst = os.path.join(dest_dir, rel)
            else:
                dst = os.path.join(dest_dir, "res", os.path.basename(abs_src))
            try:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(abs_src, dst)
                copied.append(dst)
            except Exception:
                pass
        return copied

    def _copy_resources(self):
        """把项目图片资源复制到 VM 的 res 目录(自动创建),返回复制目标列表。"""
        if not self.vm_path:
            self._copied_res = []
            return []
        vm_dir = os.path.dirname(os.path.abspath(self.vm_path))
        self._copied_res = self._copy_resources_to(vm_dir)
        return self._copied_res

    def _cleanup_resources(self):
        """删除本次复制到 VM 的资源文件及其产生的空目录。"""
        copied = getattr(self, "_copied_res", [])
        self._copied_res = []
        vm_dir = os.path.dirname(os.path.abspath(self.vm_path)) if self.vm_path else ""
        for dst in copied:
            try:
                if os.path.isfile(dst):
                    os.remove(dst)
            except Exception:
                pass
        dirs = sorted({os.path.dirname(d) for d in copied if d}, key=len, reverse=True)
        for d in dirs:
            if not vm_dir or not os.path.abspath(d).startswith(vm_dir):
                continue
            try:
                if os.path.isdir(d) and not os.listdir(d):
                    os.rmdir(d)
            except Exception:
                pass

    # ---------------- 文件 ----------------
    def new_project(self):
        if self.dirty and not messagebox.askyesno("新建", "当前项目未保存，确定丢弃？", parent=self):
            return
        self.project = project_defaults()
        self._undo_stack = []
        self._redo_stack = []
        self.file_path = ""
        self.project_root = settings_mod.get_project_root()
        self.dirty = False
        self.canvas.selected = None
        self.refresh_project_panel()
        self.on_scene_changed()

    def open_file_dialog(self):
        p = filedialog.askopenfilename(title="打开蓝图",
                                       filetypes=[("GAL 蓝图", "*.bluescene *.galscene"), ("全部", "*.*")],
                                       parent=self)
        if p:
            self.open_file(p)

    def open_file(self, path):
        proj = sceneio.load_project(path)
        if proj is None:
            messagebox.showwarning("打开", f"无法读取蓝图：{path}", parent=self)
            return
        self.project = proj
        self._undo_stack = []
        self._redo_stack = []
        self.file_path = path
        self.project_root = os.path.dirname(os.path.abspath(path))
        self.dirty = False
        self.canvas.selected = None
        self.refresh_project_panel()
        self.on_scene_changed()
        self.set_status(f"已打开：{path}")

    def save_file(self):
        if not self.file_path:
            return self.save_as()
        self.inspector.flush()
        sceneio.save_project(self.file_path, self.project)
        self.dirty = False
        self._refresh_title()
        self.set_status(f"已保存：{self.file_path}")
        return True

    def save_as(self):
        p = filedialog.asksaveasfilename(title="保存蓝图",
                                         defaultextension=".bluescene",
                                         filetypes=[("GAL 蓝图", "*.bluescene")],
                                         parent=self)
        if p:
            self.file_path = p
            self.project_root = os.path.dirname(os.path.abspath(p))
            return self.save_file()
        return False

    def export(self):
        """导出单文件代码:让用户选择保存位置。"""
        self.inspector.flush()
        root = self.project_root or os.getcwd()
        path = filedialog.asksaveasfilename(
            title="导出代码",
            defaultextension=".dex",
            initialdir=root,
            initialfile="scene_out.dex",
            filetypes=[("DexLang 代码", "*.dex"), ("全部", "*.*")],
            parent=self)
        if not path:
            return
        if export_mod.write_scene_dex(self.project, path):
            self.set_status(f"✅ 已导出：{path}")
            messagebox.showinfo("导出",
                                f"已导出 DexLang 代码：\n{path}\n\n在编辑器中点「▶ 运行」即可编译运行。",
                                parent=self)
        else:
            self._warn_export("导出")

    def export_full(self):
        """完整资源导出:选一个输出目录,导出代码 + 按用户目录结构复制引用的图片;可选输出字节码。"""
        self.inspector.flush()
        root = self.project_root or os.getcwd()
        out_dir = filedialog.askdirectory(title="选择导出目录", parent=self, initialdir=root)
        if not out_dir:
            return
        # 1) 导出代码到输出目录
        dex = export_mod.export_project(self.project, out_dir)
        if not dex:
            self._warn_export("完整导出")
            return
        # 2) 按项目目录结构复制引用的图片(如 res/bg.png → 输出/res/bg.png)
        n = len(self._copy_resources_to(out_dir))
        msg = f"✅ 已完整导出到：\n{out_dir}\n\n代码：scene_out.dex\n图片资源：{n} 个"
        portable = settings_mod.get_gal_link_mode() == "portable"
        # 自包含模式:复制 libdexxgal.dll 到输出目录(动态引用,文件夹换机可用)
        if portable:
            dll = settings_mod.get_engine_dll()
            if not dll or not os.path.isfile(dll):
                dll = paths.engine_dll_path()
            if os.path.isfile(dll):
                try:
                    shutil.copy2(dll, os.path.join(out_dir, os.path.basename(dll)))
                    msg += "\n引擎：libdexxgal.dll"
                except Exception:
                    pass
        # 3) 可选:同时输出字节码(默认关闭,设置里开启;进程内编译)
        if settings_mod.get_export_bytecode():
            bc = os.path.join(out_dir, "scene_out.dexbc")
            self.set_status("编译字节码中…")
            self.update_idletasks()
            try:
                compile_mod.compile_dex_file(dex, bc, rel_lib=portable)
                msg += "\n字节码：scene_out.dexbc"
            except Exception as e:
                msg += f"\n字节码编译失败：\n{compile_mod.compile_error_text(e)}"
        self.set_status(f"✅ 已完整导出：{out_dir}")
        messagebox.showinfo("完整导出", msg, parent=self)

    # ---------------- 设置 / 其它 ----------------
    def settings_dialog(self):
        dlg = tk.Toplevel(self)
        dlg.title("项目设置")
        dlg.geometry("520x320")
        dlg.configure(bg=T.BG2)
        dlg.transient(self)
        dlg.grab_set()

        def row(label, var, kind="file"):
            tk.Label(dlg, text=label, bg=T.BG2, fg=T.FG2,
                     font=T.FONT_SM, anchor="w").pack(fill="x", padx=14, pady=(10, 2))
            r = tk.Frame(dlg, bg=T.BG2); r.pack(fill="x", padx=14)
            e = tk.Entry(r, textvariable=var, bg=T.BG3, fg=T.FG, relief="flat",
                         font=T.FONT_SM)
            e.pack(side="left", fill="x", expand=True)

            def browse():
                p = filedialog.askdirectory(title=label, parent=dlg) if kind == "dir" \
                    else filedialog.askopenfilename(title=label, parent=dlg)
                if p:
                    var.set(p)
            tk.Button(r, text="…", command=browse, bg=T.BG3, fg=T.FG,
                      relief="flat").pack(side="right")
            return r

        root = tk.StringVar(value=self.project_root)
        row("项目根目录（导出代码写入此处）", root, "dir")
        vm = tk.StringVar(value=self.vm_path)
        row("VM 位置（vm/vm.exe 或 vm/galrun.exe）", vm, "file")
        tk.Label(dlg, text="编译已内置,无需配置 Python 解释器。",
                 bg=T.BG2, fg=T.FG2, font=T.FONT_SM, anchor="w"
                 ).pack(fill="x", padx=14, pady=(10, 2))

        bc = tk.BooleanVar(value=settings_mod.get_export_bytecode())
        tk.Checkbutton(dlg, text="完整导出时同时输出字节码 (.dexbc)", variable=bc,
                       bg=T.BG2, fg=T.FG, selectcolor=T.BG3, activebackground=T.BG2,
                       font=T.FONT_SM, anchor="w").pack(fill="x", padx=14, pady=(10, 0))

        mode = tk.StringVar(value=settings_mod.get_gal_link_mode())
        tk.Label(dlg, text="库链接模式", bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 anchor="w").pack(fill="x", padx=14, pady=(10, 2))
        ttk.Combobox(dlg, textvariable=mode, values=["static", "portable"],
                     state="readonly", font=T.FONT_SM).pack(fill="x", padx=14)
        tk.Label(dlg, text="static=引擎内嵌进字节码(单文件,换机免 DLL,默认);\n"
                           "portable=文件夹自包含(带 libdexxgal.dll,相对路径引用)",
                 bg=T.BG2, fg=T.FG2, font=T.FONT_SM, justify="left"
                 ).pack(fill="x", padx=14, pady=(6, 0))

        as_cfg = settings_mod.get_autosave()
        as_on = tk.BooleanVar(value=as_cfg["enabled"])
        tk.Checkbutton(dlg, text="间隔自动保存", variable=as_on, bg=T.BG2, fg=T.FG,
                       selectcolor=T.BG3, activebackground=T.BG2,
                       font=T.FONT_SM, anchor="w").pack(fill="x", padx=14, pady=(10, 2))
        tk.Label(dlg, text="间隔(分钟)", bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 anchor="w").pack(fill="x", padx=14)
        as_int = tk.StringVar(value=str(as_cfg["interval"]))
        tk.Entry(dlg, textvariable=as_int, bg=T.BG3, fg=T.FG, relief="flat",
                 font=T.FONT_SM).pack(fill="x", padx=14)

        tk.Label(dlg, text="VM 运行:导出 scene_out.dex → 编译 → VM 打开游戏窗口;\n关闭游戏窗口即结束,可继续编辑。",
                 bg=T.BG2, fg=T.FG2, font=T.FONT_SM, justify="left"
                 ).pack(fill="x", padx=14, pady=(12, 2))

        br = tk.Frame(dlg, bg=T.BG2); br.pack(fill="x", padx=14, pady=12)

        def save():
            self.project_root = root.get()
            self.vm_path = vm.get()
            settings_mod.set_project_root(self.project_root)
            settings_mod.set_vm_path(self.vm_path)
            settings_mod.set_export_bytecode(bc.get())
            settings_mod.set_gal_link_mode(mode.get())
            settings_mod.set_autosave(as_on.get(), int(as_int.get() or 5))
            dlg.destroy()
            self.set_status("设置已保存")
        tk.Button(br, text="保存", command=save, bg=T.ACCENT_DARK, fg="#fff",
                  relief="flat", padx=20, pady=4).pack(side="left")
        tk.Button(br, text="取消", command=dlg.destroy, bg=T.BG3, fg=T.FG,
                  relief="flat", padx=20, pady=4).pack(side="left", padx=8)

    def _about(self):
        messagebox.showinfo("关于", "GAL 蓝图编辑器（Python + tkinter）\n\n"
                            "UE 风格节点连线：入口/出口 + 说话/长对话/立绘/选项/\n"
                            "代码/变量/if/逻辑/运算/镜头控制。\n"
                            "滚轮缩放 · 拖引脚连线 · 拖节点移动 · 右键菜单\n"
                            "导出 DexLang 代码,设置 VM 后可一键编译运行。\n\n"
                            "文件：.bluescene", parent=self)

    def _on_close(self):
        if self.dirty and not messagebox.askyesno("退出", "项目未保存，确定退出？", parent=self):
            return
        self._cleanup_resources()
        self.destroy()


def main(start_file=None):
    app = App(start_file=start_file)
    app.mainloop()
