"""bluedit.inspector — 节点属性面板(右侧,竖排)。

按选中节点类型动态生成表单。修改即时写回节点并刷新画布。
提供 flush():强制提交未提交输入(保存/预览/切换前)。
"""

import tkinter as tk
from tkinter import ttk, filedialog, colorchooser

from . import theme as T
from .model import (NODE_NAMES, N_ENTRY, N_EXIT, N_SPEAK, N_LONG, N_SPRITE,
                    N_BG, N_CHOICE, N_CODE, N_DEFVAR, N_SETVAR, N_IF,
                    N_GETVAR, N_LOGIC, N_MATH, N_FLOW, N_FX_ADD, N_FX_OFF,
                    N_FX_CLEAR, N_MENU, N_TEXT, N_SAY, N_INPUT, N_RANDOM,
                    N_WAIT, N_DOUT, N_DREF, N_TEXTLIT, N_NUMLIT, N_BOLLIT,
                    N_BGM, N_SE, N_SOUND_STOP, N_SOUND_SWAP,
                    N_BG_TRANS, N_SCENE_TRANS, N_TOAST,
                    N_SAVE, N_LOAD, N_FILE_WRITE, N_FILE_READ,
                    N_CHECKPOINT, N_JUMP,
                    POS_NAMES, LAYER_NAMES, TYPE_NAMES,
                    TEXT_MODE_NAMES, BG_FIT_NAMES, SPRITE_FIT_NAMES,
                    FX_NAMES, LOGIC_OPS, MATH_OPS)
from . import vars as vars_mod

IMG_TYPES = [("图片", "*.png *.jpg *.jpeg *.bmp *.webp"), ("全部", "*.*")]
AUDIO_TYPES = [("音频", "*.mp3 *.wav *.ogg"), ("全部", "*.*")]
DEX_TYPES = [("DEX 代码", "*.dex *.dxasm *.txt"), ("全部", "*.*")]


class Inspector(tk.Frame):
    def __init__(self, master, app, **kw):
        super().__init__(master, bg=T.BG2, **kw)
        self.app = app
        self._flushers = []
        self._node = None
        self._mode = "node"      # "node" 节点属性 / "char" 人物属性
        self._char_idx = -1
        self.title_lbl = tk.Label(self, text="属性", bg=T.BG2, fg=T.FG,
                                  font=T.FONT_BOLD, anchor="w")
        self.title_lbl.pack(fill="x", padx=10, pady=8)
        self.body = tk.Frame(self, bg=T.BG2)
        self.body.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.rebuild()

    # ---------------- 重建 ----------------
    def rebuild(self):
        for w in self.body.winfo_children():
            w.destroy()
        self._flushers = []
        self._mode = "node"
        sc = self.app.scene()
        self._node = None
        if sc and self.app.canvas.selected:
            n = sc.node(self.app.canvas.selected)
            if n:
                self._node = n
                self.title_lbl.config(text=f"属性　·　{NODE_NAMES.get(n.type, '?')}")
                self._build(n)
                return
        self.title_lbl.config(text="属性　·　（未选中节点）")

    def flush(self):
        for f in self._flushers:
            try:
                f()
            except Exception:
                pass

    # ---------------- 基础控件 ----------------
    def _row(self, label, widget):
        # 标签在上方，控件占据整行（竖向排列）
        r = tk.Frame(self.body, bg=T.BG2)
        r.pack(fill="x", pady=6)
        lbl = tk.Label(r, text=label, bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                       anchor="w")
        lbl.pack(fill="x")
        widget.pack(fill="x", pady=(4, 0))
        return r

    def _entry(self, key, init, commit, width=None):
        v = tk.StringVar(value=init)
        e = tk.Entry(self.body, textvariable=v, bg=T.BG3, fg=T.FG,
                     insertbackground=T.FG, relief="flat", font=T.FONT_SM,
                     width=width)
        e.bind("<FocusOut>", lambda _e: commit(v.get()))
        e.bind("<Return>", lambda _e: (commit(v.get()), self.focus_set()))
        self._flushers.append(lambda: commit(v.get()))
        return e

    def _text(self, key, init, commit, height=4):
        t = tk.Text(self.body, bg=T.BG3, fg=T.FG, insertbackground=T.FG,
                    relief="flat", font=T.FONT_SM, height=height, wrap="word")
        t.insert("1.0", init)
        t.bind("<FocusOut>", lambda _e: commit(t.get("1.0", "end-1c")))
        self._flushers.append(lambda: commit(t.get("1.0", "end-1c")))
        return t

    def _check(self, key, init, commit, label):
        v = tk.BooleanVar(value=bool(init))
        c = tk.Checkbutton(self.body, text=label, variable=v, bg=T.BG2,
                           fg=T.FG, activebackground=T.BG2, activeforeground=T.FG,
                           selectcolor=T.BG3, font=T.FONT_SM,
                           command=lambda: commit(1 if v.get() else 0))
        c.pack(fill="x", padx=2, pady=2, anchor="w")
        self._flushers.append(lambda: commit(1 if v.get() else 0))
        return c

    def _combo(self, key, init, items, commit):
        init = max(0, init) if isinstance(init, int) else init
        v = tk.StringVar(value=items[init] if isinstance(init, int) and init < len(items) else (init if init in items else items[0]))
        cb = ttk.Combobox(self.body, textvariable=v, values=items,
                          state="readonly", font=T.FONT_SM)
        # readonly 下拉选中即提交,无需注册 flusher(注册了反而可能在 flush 时
        # 重入 commit→struct_changed→on_scene_changed→flush… 无限递归)
        cb.bind("<<ComboboxSelected>>", lambda _e: commit(items.index(v.get())))
        return cb

    def _btn(self, text, cmd, color=T.ACCENT_DARK):
        return tk.Button(self.body, text=text, command=cmd, bg=color, fg="#fff",
                         activebackground=color, relief="flat", font=T.FONT_SM)

    def _hint(self, text):
        tk.Label(self.body, text=text, bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 justify="left", wraplength=240).pack(anchor="w", pady=4)

    # ---------------- 各类型表单 ----------------
    def _build(self, n):
        app = self.app

        def changed():
            # 属性修改:只刷新画布摘要,不重建面板(避免丢焦点/递归)
            app.on_node_edited()

        def struct_changed():
            # 结构变化(加减行/选项):重建面板并刷新画布
            app.on_scene_changed()

        t = n.type
        if t == N_MENU:
            menus = app.project.menus
            names = [m.title for m in menus]
            if menus:
                mi = n.menu_idx if 0 <= n.menu_idx < len(menus) else 0
                self._row("菜单", self._combo("menu_idx", mi, names,
                          lambda v: (setattr(n, "menu_idx", v), struct_changed())))
            else:
                self._hint("项目还没有菜单。请在左栏「菜单」页新建。")
            self._row("动作", self._combo(
                "action", n.action,
                ["运行独占菜单", "显示叠加菜单", "隐藏叠加菜单", "发送信号"],
                lambda v: (setattr(n, "action", v), struct_changed())))
            if n.action == 0:
                if 0 <= n.menu_idx < len(menus):
                    m = menus[n.menu_idx]
                    exs = m.exits()
                    if exs:
                        self._hint("菜单「%s」的退出节点自动生成输出口:\n  %s" %
                                   (m.title, " / ".join(e.exit_name for e in exs)))
                        self._hint("把每个输出口连到后续流程即可(如「退出」连到告别、再结束)。")
                    else:
                        self._hint("此菜单没有「退出」节点,运行后无法正常返回(可按 ESC 强制退出)。")
                    ins = m.input_ctrls()
                    if ins:
                        self._hint("菜单里的输入框自动生成数据输出口(青色,名字=控件名):\n  %s\n"
                                   "连线到「文本拼接」等即可读取玩家输入,支持多个输入框。" %
                                   " / ".join(c.ctrl for c in ins))
                self._row("结果存入变量(可选)", self._entry(
                    "result_var", n.result_var,
                    lambda v: (setattr(n, "result_var", v), changed())))
                self._hint("不填则只按输出口分支;填入变量名可同时把结果存进变量。")
            elif n.action == 3:
                self._row("信号名", self._entry(
                    "signal", n.signal,
                    lambda v: (setattr(n, "signal", v), changed())))
                self._row("信号数据", self._entry(
                    "signal_data", n.signal_data,
                    lambda v: (setattr(n, "signal_data", v), changed())))
                self._hint("发送信号给叠加菜单,并立即驱动它的处理流程(同步更新界面)。")
            elif n.action == 1:
                self._hint("显示叠加菜单(控件浮在镜头画面上,不阻塞剧情)。")
            else:
                self._hint("隐藏当前叠加菜单。")
        elif t == N_SPEAK:
            self._row("内容", self._text("text", n.text,
                      lambda v: (setattr(n, "text", v), changed())))
            self._check("use_char", n.use_char,
                        lambda v: (setattr(n, "use_char", v), changed()),
                        "☑ 选择说话人（按位置自动显示立绘）")
            if n.use_char:
                ch = self._char_combo(n, "char_idx", changed)
                self._row("人物", ch)
                self._char_state_combo(n, changed)
            self._check("show_name", n.show_name,
                        lambda v: (setattr(n, "show_name", v), changed()),
                        "☑ 显示人名")
            self._row("人名", self._entry("name", n.name,
                      lambda v: (setattr(n, "name", v), changed())))
            self._row("文字位置", self._combo("text_pos", n.text_pos, POS_NAMES,
                      lambda v: (setattr(n, "text_pos", v), changed())))
            self._row("显示模式", self._combo("text_mode", n.text_mode, TEXT_MODE_NAMES,
                      lambda v: (setattr(n, "text_mode", v), changed())))
            self._row("逐字速度 ms (0=默认)", self._entry("text_speed", str(n.text_speed),
                      lambda v: (setattr(n, "text_speed", _int(v, 0)), changed())))
        elif t == N_LONG:
            self._hint("自动连续播放的长对话。可配置说话人立绘模式。")
            self._row("显示模式", self._combo("text_mode", n.text_mode, TEXT_MODE_NAMES,
                      lambda v: (setattr(n, "text_mode", v), changed())))
            self._row("逐字速度 ms (0=默认)", self._entry("text_speed", str(n.text_speed),
                      lambda v: (setattr(n, "text_speed", _int(v, 0)), changed())))
            self._row("立绘位置", self._combo(
                "spr_pos_mode", n.spr_pos_mode,
                ["自动(第一左/第二右)", "中间", "左", "右", "偏左", "偏右"],
                lambda v: (setattr(n, "spr_pos_mode", v), changed())))
            self._row("立绘适配", self._combo("spr_fit", n.spr_fit, SPRITE_FIT_NAMES,
                      lambda v: (setattr(n, "spr_fit", v), changed())))
            self._check("long_dual", n.long_dual,
                        lambda v: (setattr(n, "long_dual", v), changed()),
                        "☑ 同屏双人：切换说话人保留其他人(高亮当前)")
            self._check("long_narration_hide", n.long_narration_hide,
                        lambda v: (setattr(n, "long_narration_hide", v), changed()),
                        "☑ 旁白行隐藏立绘(空白)")
            self._check("long_clear_start", n.long_clear_start,
                        lambda v: (setattr(n, "long_clear_start", v), changed()),
                        "☑ 块开始清理立绘层(防残留)")
            self._build_long_lines(n, changed, struct_changed)
        elif t == N_SPRITE:
            self._check("use_char", n.use_char,
                        lambda v: (setattr(n, "use_char", v), changed()),
                        "☑ 使用人物集")
            if n.use_char:
                self._row("人物", self._char_combo(n, "char_idx", changed))
                self._char_state_combo(n, changed)
            else:
                self._row("图片路径", self._browse_entry("spr_path", n.spr_path,
                          lambda v: (setattr(n, "spr_path", v), changed()),
                          IMG_TYPES, app.project_root or "."))
            self._check("use_xy", n.use_xy,
                        lambda v: (setattr(n, "use_xy", v), changed()),
                        "☑ 使用坐标")
            if n.use_xy:
                self._row("X", self._entry("x", str(n.spr_x),
                          lambda v: (setattr(n, "spr_x", _int(v)), changed())))
                self._row("Y", self._entry("y", str(n.spr_y),
                          lambda v: (setattr(n, "spr_y", _int(v)), changed())))
            else:
                self._row("预设位置", self._combo("spr_pos_mode", n.spr_pos_mode,
                          POS_NAMES, lambda v: (setattr(n, "spr_pos_mode", v), changed())))
            self._row("图层", self._combo("spr_layer", n.spr_layer, LAYER_NAMES,
                      lambda v: (setattr(n, "spr_layer", v), changed())))
            self._row("缩放 %", self._entry("scale", str(n.spr_scale),
                      lambda v: (setattr(n, "spr_scale", _int(v, 100)), changed())))
            self._row("适配", self._combo("spr_fit", n.spr_fit, SPRITE_FIT_NAMES,
                      lambda v: (setattr(n, "spr_fit", v), changed())))
            self._row("透明度", self._entry("alpha", str(n.spr_alpha),
                      lambda v: (setattr(n, "spr_alpha", _int(v, 255)), changed())))
            self._row("动画", self._combo("spr_anim", n.spr_anim,
                      ["无", "呼吸", "淡入", "上浮", "抖动", "脉冲", "消失"],
                      lambda v: (setattr(n, "spr_anim", v), changed())))
        elif t == N_BG:
            self._row("图片路径", self._browse_entry("bg_path", n.bg_path,
                      lambda v: (setattr(n, "bg_path", v), changed()),
                      IMG_TYPES, app.project_root or "."))
            self._hint("留空则不切换背景。路径相对项目根,如 res/bg_day.png。")
            self._check("bg_mode", n.bg_mode,
                        lambda v: (setattr(n, "bg_mode", v), changed()),
                        "☑ 使用纯色背景")
            if n.bg_mode:
                self._row("颜色 #RRGGBB", self._entry("bg_color",
                          f"0x{n.bg_color:06X}",
                          lambda v: (setattr(n, "bg_color", _hex(v, n.bg_color)), changed())))
            else:
                self._row("适配模式", self._combo("bg_fit", n.bg_fit, BG_FIT_NAMES,
                          lambda v: (setattr(n, "bg_fit", v), changed())))
        elif t == N_CHOICE:
            self._row("问题文本", self._text("choice_text", n.choice_text,
                      lambda v: (setattr(n, "choice_text", v), changed()), height=3))
            self._row("显示模式", self._combo("text_mode", n.text_mode, TEXT_MODE_NAMES,
                      lambda v: (setattr(n, "text_mode", v), changed())))
            self._row("逐字速度 ms (0=默认)", self._entry("text_speed", str(n.text_speed),
                      lambda v: (setattr(n, "text_speed", _int(v, 0)), changed())))
            self._hint("每个选项有一个输出口,选中后激活。")
            self._build_choice_options(n, changed, struct_changed)
        elif t == N_CODE:
            self._row("代码文件", self._browse_entry("code_file", n.code_file,
                      lambda v: (setattr(n, "code_file", v), changed()),
                      DEX_TYPES, app.project_root or "."))
            self._row("条件", self._entry("code_cond", n.code_cond,
                      lambda v: (setattr(n, "code_cond", v), changed())))
        elif t == N_DEFVAR:
            self._row("作用域", self._combo("scope", n.scope,
                      ["全局", "局部（本镜头）"],
                      lambda v: (setattr(n, "scope", v), changed())))
            self._row("变量名", self._entry("var_name", n.var_name,
                      lambda v: (setattr(n, "var_name", v), changed())))
            self._row("类型", self._combo("var_type", n.var_type, TYPE_NAMES,
                      lambda v: (setattr(n, "var_type", v), changed())))
            self._row("初始值", self._entry("var_value", n.var_value,
                      lambda v: (setattr(n, "var_value", v), changed())))
            self._hint("变量由编辑器实时统计,可被其它节点引用。")
        elif t == N_SETVAR:
            self._row("变量", self._var_combo(n, "var_name", changed))
            self._row("类型", self._combo("var_type", n.var_type, TYPE_NAMES,
                      lambda v: (setattr(n, "var_type", v), changed())))
            self._hint("新值来自 value 输入:连接「纯数字/纯文本/变量取值/运算」等\n"
                       "的输出到本节点的新值输入(蓝色线)。")
        elif t == N_IF:
            self._hint("条件来自 value 输入:连接「逻辑运算/变量取值/布尔」等\n"
                       "的输出到条件输入(蓝色线)。")
            self._hint("真/假两个输出口。")
        elif t == N_GETVAR:
            self._row("变量", self._var_combo(n, "var_name", changed))
        elif t in (N_LOGIC, N_MATH):
            ops = LOGIC_OPS if t == N_LOGIC else MATH_OPS
            self._row("运算符", self._combo("op", ops.index(n.op) if n.op in ops else 0, ops,
                      lambda v: (setattr(n, "op", ops[v]), changed())))
            self._hint("A、B 从上方 value 输出连入(蓝色线)。\n"
                       "字面数字用「纯数字」块,布尔比较用「布尔」块。")
        elif t == N_FLOW:
            self._row("跳转", self._combo("flow_target", n.flow_target,
                      ["下一镜头", "指定镜头", "结束游戏"],
                      lambda v: (setattr(n, "flow_target", v), changed())))
            if n.flow_target == 1:
                scenes = [s.title for s in app.project.scenes] or ["（无）"]
                idx = n.flow_scene if 0 <= n.flow_scene < len(scenes) else 0
                self._row("目标镜头", self._combo("flow_scene", idx, scenes,
                          lambda v: (setattr(n, "flow_scene", v), changed())))
        elif t in (N_BGM, N_SE, N_SOUND_SWAP):
            if t == N_BGM:
                self._row("背景音乐", self._browse_entry(
                    "sound_path", n.sound_path,
                    lambda v: (setattr(n, "sound_path", v), changed()),
                    AUDIO_TYPES, app.project_root or "."))
                self._hint("循环播放 BGM。若已有 BGM 会自动切换。")
            elif t == N_SE:
                self._row("音效", self._browse_entry(
                    "sound_path", n.sound_path,
                    lambda v: (setattr(n, "sound_path", v), changed()),
                    AUDIO_TYPES, app.project_root or "."))
                self._hint("播放一次音效(SE),可叠加在 BGM 上。")
            else:
                self._row("新 BGM", self._browse_entry(
                    "sound_path", n.sound_path,
                    lambda v: (setattr(n, "sound_path", v), changed()),
                    AUDIO_TYPES, app.project_root or "."))
                self._hint("切换背景音乐:停止当前并播放新的(循环)。")
            self._row("音量 0-100 (80=默认)", self._entry(
                "sound_vol", str(n.sound_vol),
                lambda v: (setattr(n, "sound_vol", _int(v, 80)), changed())))
        elif t == N_SOUND_STOP:
            self._row("停止", self._combo("stop_target", n.stop_target,
                      ["BGM", "SE", "全部声音"],
                      lambda v: (setattr(n, "stop_target", v), changed())))
            self._hint("停止指定声音。")
        elif t == N_BG_TRANS:
            self._row("新背景", self._browse_entry(
                "bg_path", n.bg_path,
                lambda v: (setattr(n, "bg_path", v), changed()),
                IMG_TYPES, app.project_root or "."))
            self._row("过渡时长 ms", self._entry(
                "bg_trans_ms", str(n.bg_trans_ms),
                lambda v: (setattr(n, "bg_trans_ms", _int(v, 400)), changed())))
            self._row("适配", self._combo("bg_fit", n.bg_fit, BG_FIT_NAMES,
                      lambda v: (setattr(n, "bg_fit", v), changed())))
            self._hint("柔和更换背景:新旧背景交叉淡化(不打断剧情)。")
        elif t == N_SCENE_TRANS:
            self._row("跳转", self._combo("flow_target", n.flow_target,
                      ["下一镜头", "指定镜头", "结束游戏"],
                      lambda v: (setattr(n, "flow_target", v), changed())))
            if n.flow_target == 1:
                scenes = [s.title for s in app.project.scenes] or ["（无）"]
                idx = n.flow_scene if 0 <= n.flow_scene < len(scenes) else 0
                self._row("目标镜头", self._combo("flow_scene", idx, scenes,
                          lambda v: (setattr(n, "flow_scene", v), changed())))
            self._row("过渡时长 ms", self._entry(
                "scene_trans_ms", str(n.scene_trans_ms),
                lambda v: (setattr(n, "scene_trans_ms", _int(v, 300)), changed())))
            self._hint("柔和切换镜头:先淡出到黑,再切入目标镜头并淡入。")
        elif t == N_TOAST:
            self._row("提示文字", self._entry(
                "toast_text", n.toast_text,
                lambda v: (setattr(n, "toast_text", v), changed())))
            self._row("位置", self._combo(
                "toast_corner", n.toast_corner if 0 <= n.toast_corner < 4 else 3,
                ["左上", "右上", "左下", "右下"],
                lambda v: (setattr(n, "toast_corner", v), changed())))
            self._row("显示时长 ms", self._entry(
                "toast_ms", str(n.toast_ms),
                lambda v: (setattr(n, "toast_ms", _int(v, 1500)), changed())))
            self._hint("在屏幕角落快速显示一条消息,淡入淡出后自动消失。\n"
                       "适合存档提示、获得道具、系统通知等。")
        elif t in (N_SAVE, N_LOAD, N_FILE_WRITE, N_FILE_READ):
            if t == N_SAVE:
                self._row("存档路径", self._entry(
                    "file_path", n.file_path,
                    lambda v: (setattr(n, "file_path", v), changed())))
                self._hint("把所有变量(gal_var)写入文件(每行 变量=值)。\n"
                           "适合做存档;多槽位可用文本拼接生成 save1/save2。\n"
                           "路径相对项目根,如 save/档1.dat。")
            elif t == N_LOAD:
                self._row("读档路径", self._entry(
                    "file_path", n.file_path,
                    lambda v: (setattr(n, "file_path", v), changed())))
                self._hint("从文件恢复所有变量(覆盖同名变量)。\n"
                           "与「存档」块配套使用。")
            elif t == N_FILE_WRITE:
                self._row("文件路径", self._entry(
                    "file_path", n.file_path,
                    lambda v: (setattr(n, "file_path", v), changed())))
                self._hint("把「内容」输入(value 线)的字符串写入文件(覆盖)。\n"
                           "字面文字用「纯文本」块连入。")
            else:
                self._row("文件路径", self._entry(
                    "file_path", n.file_path,
                    lambda v: (setattr(n, "file_path", v), changed())))
                self._hint("读取文件内容(字符串),输出到 value。\n"
                           "可配合文本拼接/动态文本显示,或写回变量。")
        elif t == N_CHECKPOINT:
            self._row("存档点名", self._entry(
                "ck_name", n.ck_name,
                lambda v: (setattr(n, "ck_name", v), changed())))
            self._row("存档文件(可选)", self._entry(
                "file_path", n.file_path,
                lambda v: (setattr(n, "file_path", v), changed())))
            self._hint("存档点:流程经过时记录当前位置(存档数据包),\n"
                       "填入存档文件则同时把全部变量写入文件。\n"
                       "跳转到存档点会从这里之后继续。\n"
                       "存档点名在本镜头内应唯一。")
        elif t == N_JUMP:
            self._row("存档文件", self._entry(
                "file_path", n.file_path,
                lambda v: (setattr(n, "file_path", v), changed())))
            self._hint("跳转到存档点:读取该存档文件,恢复全部变量,\n"
                       "并从存档点之后继续运行(自动载入位置与数据)。\n"
                       "适合放在读档页(点击某存档→跳转)。")
        elif t == N_FX_ADD:
            self._row("特效", self._combo("fx", n.fx, FX_NAMES,
                      lambda v: (setattr(n, "fx", v), changed())))
            self._row("强度 0-100", self._entry("fx_strength", str(n.fx_strength),
                      lambda v: (setattr(n, "fx_strength", _int(v, 50)), changed())))
            self._hint("对整个场景生效,可与其它特效叠加。")
        elif t == N_FX_OFF:
            self._row("取消特效", self._combo("fx", n.fx, FX_NAMES,
                      lambda v: (setattr(n, "fx", v), changed())))
            self._hint("取消指定场景特效(不影响其它特效)。")
        elif t == N_FX_CLEAR:
            self._hint("取消当前全部场景特效。")
        elif t == N_TEXT:
            self._hint("把各段拼成一个字符串。每段用 value 线连入:\n"
                       "字面文字用「纯文本」块,数字用「纯数字」块,变量用「变量取值」。")
            seg_names = n.text_input_names()
            for i, pin in enumerate(n.inputs):
                seg = seg_names[i] if i < len(seg_names) else "?"
                r = tk.Frame(self.body, bg=T.BG2); r.pack(fill="x", pady=2)
                tk.Label(r, text=f"{seg}", bg=T.BG2, fg=T.FG2, font=T.FONT_BOLD,
                         width=3, anchor="w").pack(side="left")
                tk.Label(r, text="← 从上方 value 输出连入", bg=T.BG2, fg=T.FG2,
                         font=T.FONT_SM, anchor="w").pack(side="left", fill="x", expand=True)
                if len(n.inputs) > 2:
                    tk.Button(r, text="×", bg=T.BG3, fg=T.FG, relief="flat",
                              font=T.FONT_SM, width=2,
                              command=lambda: self._text_del(n, struct_changed)
                              ).pack(side="right")
            tk.Button(self.body, text="＋ 增加输入点", bg=T.BG3, fg=T.FG,
                      relief="flat", font=T.FONT_SM,
                      command=lambda: self._text_add(n, struct_changed)).pack(fill="x", pady=4)
            self._hint("空段会自动忽略(不参与拼接)。")
        elif t == N_SAY:
            self._hint("显示一段文本,等待点击后清除。\n"
                       "文本来自「文本」输入的 value 线;字面文字用「纯文本」块连入。")
            self._row("人名(可选)", self._entry(
                "name", n.name, lambda v: (setattr(n, "name", v), changed())))
            self._row("文字位置", self._combo("text_pos", n.text_pos, POS_NAMES,
                      lambda v: (setattr(n, "text_pos", v), changed())))
            self._row("显示模式", self._combo("text_mode", n.text_mode, TEXT_MODE_NAMES,
                      lambda v: (setattr(n, "text_mode", v), changed())))
            self._row("逐字速度 ms (0=默认)", self._entry(
                "text_speed", str(n.text_speed),
                lambda v: (setattr(n, "text_speed", _int(v, 0)), changed())))
            self._hint("从「文本」输入(蓝色线)连入要显示的内容。")
        elif t == N_INPUT:
            menus = app.project.menus
            names = [m.title for m in menus]
            if menus:
                mi = n.menu_idx if 0 <= n.menu_idx < len(menus) else 0
                self._row("菜单", self._combo("menu_idx", mi, names,
                          lambda v: (setattr(n, "menu_idx", v), struct_changed())))
                m = menus[mi]
                inputs = [c.ctrl for c in m.ctrl_nodes()
                          if getattr(c, "ctype", 0) == 4 and c.ctrl]
                if inputs:
                    cur = n.ctrl if n.ctrl in inputs else inputs[0]
                    self._row("输入框控件", self._combo(
                        "ctrl", cur, inputs,
                        lambda v: (setattr(n, "ctrl", v), changed())))
                    self._hint("读取该输入框的当前内容(字符串)。\n"
                               "需先运行独占菜单让玩家输入,再读取。")
                else:
                    self._hint("该菜单还没有「输入框」控件。\n"
                               "请在菜单编辑器添加输入框(M_CTRL 类型=输入框)。")
            else:
                self._hint("项目还没有菜单。")
        elif t == N_RANDOM:
            self._row("随机上限 (0..max)", self._entry(
                "max", str(n.max),
                lambda v: (setattr(n, "max", _int(v, 100)), changed())))
            self._hint("生成 0..max-1 的随机整数。\nmax=100 即 0~99。")
        elif t == N_WAIT:
            self._row("毫秒", self._entry(
                "ms", str(n.ms),
                lambda v: (setattr(n, "ms", _int(v, 1000)), changed())))
            self._hint("暂停执行指定毫秒后继续。\n适合打字机停顿、特效节奏等。")
        elif t == N_DOUT:
            self._row("输出点名", self._entry(
                "out_name", n.out_name,
                lambda v: (setattr(n, "out_name", v), changed())))
            if n.out_name and n.out_name in app.scene().duplicate_data_names():
                self._hint("⚠ 输出点名重复,引用时可能取到同名值。")
            self._hint("把「数据」输入(value 线)的值送出镜头。\n"
                       "其它镜头用「场景数据」块引用 本镜头.输出点名 即可读取。\n"
                       "输出点名在本镜头内应唯一(类似菜单退出名)。")
        elif t in (N_TEXTLIT, N_NUMLIT, N_BOLLIT):
            if t == N_TEXTLIT:
                self._row("文本", self._text(
                    "lit_text", n.lit_text,
                    lambda v: (setattr(n, "lit_text", v), changed()), height=5))
                self._hint("输出一段文本字面值。块上会直接显示内容,\n长文本自动换行并加宽。")
            elif t == N_NUMLIT:
                self._row("数值", self._entry(
                    "lit_num", str(n.lit_num),
                    lambda v: (setattr(n, "lit_num", v), changed())))
                self._hint("输出一个数字字面值。请填数字(可带小数点)。")
            else:
                self._row("值", self._combo(
                    "lit_bool", 1 if n.lit_bool else 0, ["false", "true"],
                    lambda v: (setattr(n, "lit_bool", v), changed())))
                self._hint("输出布尔字面值 true / false。")
        elif t == N_DREF:
            scenes = app.project.scenes
            if scenes:
                names = [s.title for s in scenes]
                si = n.out_scene if 0 <= n.out_scene < len(scenes) else 0
                self._row("源镜头", self._combo(
                    "out_scene", si, names,
                    lambda v: (setattr(n, "out_scene", v),
                               setattr(n, "out_name", ""),
                               struct_changed())))
                src = scenes[si]
                outs = src.data_names()
                if outs:
                    cur = n.out_name if n.out_name in outs else outs[0]
                    self._row("输出点", self._combo(
                        "out_name", cur, outs,
                        lambda v: (setattr(n, "out_name", v), changed())))
                    self._hint("读取该镜头执行后送出的数据值。\n"
                               "需等源镜头先执行到对应「数据输出点」才有值。")
                else:
                    self._hint("该镜头还没有「数据输出点」。\n"
                               "去源镜头放置一个数据输出点块并命名。")
            else:
                self._hint("项目还没有镜头。")
        elif t == N_ENTRY:
            self._hint("镜头入口。执行从这里开始。不可删除。")
        elif t == N_EXIT:
            self._hint("镜头出口。执行到这里跳转到其它镜头。不可删除。")

    # ---------------- 组合控件 ----------------
    def _char_combo(self, n, attr, changed):
        chars = self.app.project.chars
        names = [c.name or f"人物{i}" for i, c in enumerate(chars)] or ["（无人物集）"]
        idx = n.char_idx if 0 <= n.char_idx < len(chars) else 0
        v = tk.StringVar(value=names[idx] if names else "（无人物集）")
        cb = ttk.Combobox(self.body, textvariable=v, values=names,
                          state="readonly", font=T.FONT_SM)

        def on_sel(_e):
            try:
                setattr(n, attr, names.index(v.get()))
            except ValueError:
                setattr(n, attr, -1)
            changed()
        cb.bind("<<ComboboxSelected>>", on_sel)
        return cb

    def _char_state_combo(self, n, changed):
        ch = None
        if 0 <= n.char_idx < len(self.app.project.chars):
            ch = self.app.project.chars[n.char_idx]
        states = []
        if ch and ch.nStates:
            states = [ch.stateNames[i] or f"状态{i}" for i in range(ch.nStates)]
        if not states:
            states = ["状态 0"]
        idx = n.state_idx if 0 <= n.state_idx < len(states) else 0
        self._row("状态", self._combo("state_idx", idx, states,
                  lambda v: (setattr(n, "state_idx", v), changed())))

    def _var_combo(self, n, attr, changed):
        names = vars_mod.var_names(self.app.project, self.app.project.cur) or ["（无变量）"]
        v = tk.StringVar(value=getattr(n, attr) if getattr(n, attr) in names else (names[0] if names else ""))
        cb = ttk.Combobox(self.body, textvariable=v, values=names,
                          state="readonly", font=T.FONT_SM)

        def on_sel(_e):
            raw = v.get().split("  (")[0]
            setattr(n, attr, raw)
            vv = vars_mod.var_value(raw, self.app.project)
            if vv is not None:
                n.var_type = vv.type
                n.scope = vv.scope
            changed()
        cb.bind("<<ComboboxSelected>>", on_sel)
        return cb

    def _browse_entry(self, key, init, commit, types, root):
        r = tk.Frame(self.body, bg=T.BG2)
        r.pack(fill="x", pady=3)
        tk.Label(r, text="路径", bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 width=10, anchor="w").pack(side="left")
        v = tk.StringVar(value=init)
        e = tk.Entry(r, textvariable=v, bg=T.BG3, fg=T.FG,
                     insertbackground=T.FG, relief="flat", font=T.FONT_SM)
        e.pack(side="left", fill="x", expand=True)
        e.bind("<FocusOut>", lambda _e: commit(v.get()))
        self._flushers.append(lambda: commit(v.get()))

        def browse():
            p = filedialog.askopenfilename(initialdir=root or ".",
                                           filetypes=types, parent=self)
            if p:
                v.set(p)
                commit(p)
        tk.Button(r, text="…", command=browse, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM, width=3).pack(side="right")
        return r

    # ---------------- 人物面板 ----------------
    def show_char(self, idx):
        """右侧面板显示人物信息(名称/立绘状态)。"""
        self.flush()               # 先提交旧面板未提交的编辑(如状态名)
        self._mode = "char"
        self._char_idx = idx
        for w in self.body.winfo_children():
            w.destroy()
        self._flushers = []
        c = self.app.project.chars[idx] if 0 <= idx < len(self.app.project.chars) else None
        if c is None:
            self.title_lbl.config(text="属性　·　人物")
            return
        self.title_lbl.config(text=f"人物　·　{c.name or '人物'}")
        self._row("名称", self._entry(
            "name", c.name, lambda v: self._char_commit_name(c, v)))
        tk.Label(self.body, text=f"立绘状态（{c.nStates} 个）", bg=T.BG2, fg=T.FG2,
                 font=T.FONT_SM, anchor="w").pack(fill="x", pady=(10, 0))
        for i in range(c.nStates):
            self._build_char_state_row(c, i)
        tk.Button(self.body, text="＋ 添加状态", bg=T.BG3, fg=T.FG, relief="flat",
                  font=T.FONT_SM,
                  command=lambda: self._char_add_state(c)).pack(fill="x", pady=4)
        self._hint("人物状态 = 一张立绘。在「说话/长对话/立绘」块里选 人物+状态,\n"
                   "就会自动显示对应立绘。可设多张(默认/开心/生气…)。")

    def _build_char_state_row(self, c, i):
        r = tk.Frame(self.body, bg=T.BG2)
        r.pack(fill="x", pady=2)
        tk.Label(r, text=f"#{i}", bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 width=3, anchor="w").pack(side="left")
        name_v = tk.StringVar(value=c.stateNames[i] or f"状态{i}")
        e = tk.Entry(r, textvariable=name_v, bg=T.BG3, fg=T.FG,
                     insertbackground=T.FG, relief="flat", font=T.FONT_SM)
        e.pack(side="left", fill="x", expand=True, padx=(0, 4))
        e.bind("<FocusOut>", lambda _e, i=i: self._char_state_name(c, i, name_v.get()))
        e.bind("<Return>", lambda _e, i=i: (self._char_state_name(c, i, name_v.get()),
                                             self.focus_set()))
        self._flushers.append(lambda i=i: self._char_state_name(c, i, name_v.get()))
        tk.Button(r, text="立绘…", bg=T.BG3, fg=T.FG, relief="flat", font=T.FONT_SM,
                  command=lambda i=i: self._char_state_pick(c, i)).pack(side="left")
        if c.nStates > 1:
            tk.Button(r, text="×", bg=T.BG3, fg=T.FG, relief="flat", font=T.FONT_SM,
                      width=2, command=lambda i=i: self._char_del_state(c, i)
                      ).pack(side="left", padx=(2, 0))
        tk.Label(self.body, text=("  " + c.states[i]) if c.states[i] else "  （未设置立绘）",
                 bg=T.BG2, fg=T.FG2, font=T.FONT_SM, anchor="w",
                 width=1).pack(fill="x", padx=(18, 4))

    def _char_commit_name(self, c, v):
        c.name = v
        self.title_lbl.config(text=f"人物　·　{v or '人物'}")
        self.app.dirty = True
        self.app.refresh_left()          # 左栏名字更新(选中已由 _sel_char 恢复)

    def _char_state_name(self, c, i, v):
        c.stateNames[i] = v
        self.app.dirty = True

    def _char_state_pick(self, c, i):
        f = filedialog.askopenfilename(title="选择立绘", parent=self.app,
                                       filetypes=IMG_TYPES,
                                       initialdir=self.app.project_root or ".")
        if f:
            c.states[i] = f
            self.app.dirty = True
            self.show_char(self._char_idx)

    def _char_add_state(self, c):
        if c.nStates >= 10:
            return
        c.states[c.nStates] = ""
        c.stateNames[c.nStates] = f"状态{c.nStates}"
        c.nStates += 1
        self.app.dirty = True
        self.show_char(self._char_idx)

    def _char_del_state(self, c, i):
        if c.nStates <= 1:
            return
        for j in range(i, c.nStates - 1):
            c.states[j] = c.states[j + 1]
            c.stateNames[j] = c.stateNames[j + 1]
        c.nStates -= 1
        self.app.dirty = True
        self.show_char(self._char_idx)

    def _text_add(self, n, changed):
        """文本拼接:增加一个输入段。"""
        if n.text_add_input():
            changed()

    def _text_del(self, n, changed):
        """文本拼接:删除最后一个输入段(至少保留 2 段)。"""
        if n.text_remove_input():
            changed()

    def _build_pin_source(self, n, idx, label, changed):
        """value 引脚来源:字面值 或 变量。"""
        pin = n.inputs[idx]
        r = tk.Frame(self.body, bg=T.BG2); r.pack(fill="x", pady=2)
        tk.Label(r, text=label, bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 width=4, anchor="w").pack(side="left")
        src = tk.StringVar(value="变量" if pin.source == "variable" else "字面值")
        tk.Radiobutton(r, text="字面值", value="字面值", variable=src,
                       bg=T.BG2, fg=T.FG, selectcolor=T.BG3,
                       activebackground=T.BG2, font=T.FONT_SM,
                       command=lambda: (setattr(pin, "source", "literal"), changed())
                       ).pack(side="left")
        tk.Radiobutton(r, text="变量", value="变量", variable=src,
                       bg=T.BG2, fg=T.FG, selectcolor=T.BG3,
                       activebackground=T.BG2, font=T.FONT_SM,
                       command=lambda: (setattr(pin, "source", "variable"), changed())
                       ).pack(side="left")
        if pin.source == "variable":
            self._row("", self._var_combo_for_pin(pin, changed))
        else:
            self._row("", self._entry("pin", pin.literal,
                      lambda v: (setattr(pin, "literal", v), changed())))

    def _var_combo_for_pin(self, pin, changed):
        names = vars_mod.var_names(self.app.project, self.app.project.cur) or ["（无变量）"]
        cur = pin.variable if pin.variable in names else (names[0] if names else "")
        v = tk.StringVar(value=cur)
        cb = ttk.Combobox(self.body, textvariable=v, values=names,
                          state="readonly", font=T.FONT_SM)

        def on_sel(_e):
            raw = v.get().split("  (")[0]
            pin.variable = raw
            changed()
        cb.bind("<<ComboboxSelected>>", on_sel)
        return cb

    # ---------------- 长对话行 ----------------
    def _build_long_lines(self, n, changed, struct_changed):
        chars = self.app.project.chars
        names = [c.name or f"人物{i}" for i, c in enumerate(chars)] or ["（无人物）"]
        for i, (ci, txt) in enumerate(list(n.lines)):
            r = tk.Frame(self.body, bg=T.BG3)
            r.pack(fill="x", pady=2)
            vc = tk.StringVar(value=names[ci] if 0 <= ci < len(names) else names[0])
            cbo = ttk.Combobox(r, textvariable=vc, values=names,
                               state="readonly", font=T.FONT_SM, width=8)
            cbo.pack(side="left", padx=2)
            vt = tk.StringVar(value=txt)
            en = tk.Entry(r, textvariable=vt, bg=T.BG3, fg=T.FG,
                          insertbackground=T.FG, relief="flat", font=T.FONT_SM)
            en.pack(side="left", fill="x", expand=True, padx=2)
            cbo.bind("<<ComboboxSelected>>", lambda _e, i=i, vc=vc: self._set_long_char(n, i, vc, names, changed))
            en.bind("<FocusOut>", lambda _e, i=i, vt=vt: self._set_long_text(n, i, vt, changed))
            en.bind("<Return>", lambda _e, i=i, vt=vt: (self._set_long_text(n, i, vt, changed), self.focus_set()))
            # 注册 flusher:切换/导出/保存前强制提交未失焦的修改(否则刚输入就丢)
            self._flushers.append(lambda i=i, vt=vt: self._set_long_text(n, i, vt, changed))
            tk.Button(r, text="－", command=lambda i=i: self._del_long(n, i, struct_changed),
                      bg=T.BG2, fg=T.WARN, relief="flat", font=T.FONT_SM, width=3
                      ).pack(side="right")
        b = self._btn("＋ 添加对话行", lambda: self._add_long(n, struct_changed))
        b.pack(fill="x", pady=4)

    def _set_long_char(self, n, i, vc, names, changed):
        try:
            n.lines[i][0] = names.index(vc.get())
        except ValueError:
            n.lines[i][0] = -1
        changed()

    def _set_long_text(self, n, i, vt, changed):
        n.lines[i][1] = vt.get()
        changed()

    def _add_long(self, n, changed):
        if len(n.lines) < 12:
            n.lines.append([-1, ""])
            changed()

    def _del_long(self, n, i, changed):
        if len(n.lines) > 1:
            del n.lines[i]
            changed()

    # ---------------- 选项列表 ----------------
    def _build_choice_options(self, n, changed, struct_changed):
        for i, opt in enumerate(list(n.options)):
            r = tk.Frame(self.body, bg=T.BG3)
            r.pack(fill="x", pady=2)
            vt = tk.StringVar(value=opt)
            en = tk.Entry(r, textvariable=vt, bg=T.BG3, fg=T.FG,
                          insertbackground=T.FG, relief="flat", font=T.FONT_SM)
            en.pack(side="left", fill="x", expand=True, padx=2)
            # 立即在失焦时保存
            en.bind("<FocusOut>", lambda _e, i=i, vt=vt: self._set_opt(n, i, vt, changed))
            # 回车也保存并移出焦点
            en.bind("<Return>", lambda _e, i=i, vt=vt: (self._set_opt(n, i, vt, changed), self.focus_set()))
            # 同步到 flushers: 在切换/导出前强制提交未失焦修改
            self._flushers.append(lambda i=i, vt=vt: self._set_opt(n, i, vt, changed))
            tk.Button(r, text="－", command=lambda i=i: self._del_opt(n, i, struct_changed),
                      bg=T.BG2, fg=T.WARN, relief="flat", font=T.FONT_SM, width=3
                      ).pack(side="right")
        b = self._btn("＋ 添加选项", lambda: self._add_opt(n, struct_changed))
        b.pack(fill="x", pady=4)

    def _set_opt(self, n, i, vt, changed):
        n.options[i] = vt.get()
        changed()

    def _add_opt(self, n, changed):
        n.options.append("新选项")
        n.sync_choice_outputs()
        changed()

    def _del_opt(self, n, i, changed):
        if len(n.options) > 1:
            del n.options[i]
            n.sync_choice_outputs()
            changed()


def _int(v, d=0):
    try:
        return int(v)
    except Exception:
        return d


def _hex(v, d=0):
    s = (v or "").strip().lstrip("#")
    try:
        return int(s, 16) & 0xFFFFFF
    except Exception:
        return d
