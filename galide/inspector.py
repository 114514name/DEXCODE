"""galide.inspector — 右侧属性面板:按选中块类型动态生成表单。

修改即时写回块,并通过 app.on_scene_changed(keep_sel=True, refresh_inspector=False)
刷新画布摘要与预览,避免打断正在输入的控件。
"""

import tkinter as tk
from tkinter import ttk, filedialog, colorchooser

from . import theme as T
from .model import BLOCK_NAMES, MAX_SPR

IMG_TYPES = [("图片", "*.png *.jpg *.jpeg *.bmp *.webp"), ("全部", "*.*")]
AUD_TYPES = [("音频", "*.mp3 *.wav *.ogg *.flac"), ("全部", "*.*")]
DEX_TYPES = [("DEX 代码", "*.dex *.dxasm *.txt"), ("全部", "*.*")]

POS_NAMES = ["中间", "左", "右", "偏左", "偏右"]
LAYER_NAMES = [f"图层 {i}" for i in range(MAX_SPR)]
ANIM_NAMES = ["无", "呼吸", "淡入", "上浮", "抖动", "脉冲", "消失"]
AUD_NAMES = ["BGM", "SE", "停止 BGM", "音量"]


class Inspector(tk.Frame):
    def __init__(self, master, app, **kw):
        super().__init__(master, bg=T.BG2, **kw)
        self.app = app
        self._vars = {}
        self._flushers = []
        self._b = None
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
        self._vars = {}
        self._flushers = []
        sc = self.app.scene()
        if sc is None:
            return
        self._b = None
        if 0 <= sc.cur < sc.nBlocks:
            self._b = sc.blocks[sc.cur]
            self.title_lbl.config(text=f"属性　·　{BLOCK_NAMES.get(self._b.type, '?')}")
            self._build(self._b)
        else:
            self.title_lbl.config(text="属性　·　（未选中块）")

    def flush(self):
        """强制把当前控件值写回 block(解决焦点未离开导致的输入丢失)。"""
        for f in self._flushers:
            try:
                f()
            except Exception:
                pass

    def _row(self, label, widget):
        # 把标签放在上面，控件放在下面，形成竖排布局
        tk.Label(self.body, text=label, bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 anchor="w").pack(fill="x", pady=(6, 2))
        widget.pack(fill="x", pady=(0, 6))
        return None

    def _entry(self, key, init, commit):
        v = tk.StringVar(value=init)
        e = tk.Entry(self.body, textvariable=v, bg=T.BG3, fg=T.FG,
                     insertbackground=T.FG, relief="flat", font=T.FONT)
        e.bind("<FocusOut>", lambda _e: commit(v.get()))
        e.bind("<Return>", lambda _e: (commit(v.get()), self.focus_set()))
        self._flushers.append(lambda: commit(v.get()))
        self._vars[key] = v
        return e

    def _text(self, key, init, commit, height=4):
        t = tk.Text(self.body, bg=T.BG3, fg=T.FG, insertbackground=T.FG,
                    relief="flat", font=T.FONT, height=height, wrap="word",
                    undo=True)
        t.insert("1.0", init)
        t.bind("<FocusOut>", lambda _e: commit(t.get("1.0", "end-1c")))
        self._flushers.append(lambda: commit(t.get("1.0", "end-1c")))
        self._vars[key] = t
        return t

    def _check(self, key, init, commit, label):
        v = tk.BooleanVar(value=bool(init))
        c = tk.Checkbutton(self.body, text=label, variable=v, bg=T.BG2,
                           fg=T.FG, activebackground=T.BG2, activeforeground=T.FG,
                           selectcolor=T.BG3, font=T.FONT_SM,
                           command=lambda: commit(1 if v.get() else 0))
        self._flushers.append(lambda: commit(1 if v.get() else 0))
        self._vars[key] = v
        return c

    def _combo(self, key, init, items, commit):
        v = tk.StringVar(value=items[init] if 0 <= init < len(items) else items[0])
        cb = ttk.Combobox(self.body, textvariable=v, values=items,
                          state="readonly", font=T.FONT_SM)
        cb.bind("<<ComboboxSelected>>",
                lambda _e: commit(items.index(v.get())))
        self._flushers.append(lambda: commit(items.index(v.get())))
        self._vars[key] = v
        return cb

    def _browse_row(self, label, key, init, commit, types, root):
        # 标签在上，下面一行为输入框 + 按钮（水平）
        tk.Label(self.body, text=label, bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                 anchor="w").pack(fill="x", pady=(6, 2))
        v = tk.StringVar(value=init)
        row = tk.Frame(self.body, bg=T.BG2)
        row.pack(fill="x", pady=3)
        e = tk.Entry(row, textvariable=v, bg=T.BG3, fg=T.FG,
                     insertbackground=T.FG, relief="flat", font=T.FONT_SM)
        e.pack(side="left", fill="x", expand=True)
        e.bind("<FocusOut>", lambda _e: commit(v.get()))
        self._flushers.append(lambda: commit(v.get()))
        self._vars[key] = v

        def browse():
            p = filedialog.askopenfilename(initialdir=root or ".",
                                           filetypes=types, parent=self)
            if p:
                v.set(p)
                commit(p)

        tk.Button(row, text="…", command=browse, bg=T.BG3, fg=T.FG,
                  relief="flat", font=T.FONT_SM, width=3).pack(side="right")
        return row

    # ---------------- 各类型表单 ----------------
    def _build(self, b):
        app = self.app

        def changed():
            app.on_scene_changed(keep_sel=True, refresh_inspector=False)

        t = b.type
        if t == 0:  # SPEAK
            self._row("说话人", self._entry("speaker", b.speaker,
                        lambda v: (setattr(b, "speaker", v), changed())))
            self._row("台词", self._text("text", b.text,
                        lambda v: (setattr(b, "text", v), changed()), height=5))
            self._check("avatar", b.style_avatar,
                        lambda v: (setattr(b, "style_avatar", v), changed()),
                        "有头像对话框").pack(fill="x", padx=12, pady=2)
            self._check("name", b.style_name,
                        lambda v: (setattr(b, "style_name", v), changed()),
                        "显示人名").pack(fill="x", padx=12, pady=2)
            self._check("autofit", b.auto_fit,
                        lambda v: (setattr(b, "auto_fit", v), changed()),
                        "自适应").pack(fill="x", padx=12, pady=2)
            self._row("文字位置", self._combo("textpos", b.text_pos, POS_NAMES,
                        lambda v: (setattr(b, "text_pos", v), changed())))
        elif t == 1:  # BG
            self._browse_row("背景图", "bg", b.bg,
                             lambda v: (setattr(b, "bg", v), changed()),
                             IMG_TYPES, app.project_root or ".")
            self._check("usebgimg", b.useBgImg,
                        lambda v: (setattr(b, "useBgImg", v), changed()),
                        "使用图片").pack(fill="x", padx=12, pady=2)

            def pick_color():
                col = colorchooser.askcolor(color=f"#{b.bgColor:06X}", parent=self)
                if col and col[1]:
                    b.bgColor = int(col[1].lstrip("#"), 16)
                    changed()

            def _apply_color(s):
                try:
                    b.bgColor = int(s.lstrip("#"), 16)
                except Exception:
                    pass
                changed()
            # 背景色行(单独 frame,避免先 pack 到 self.body 再 reparent)
            cr = tk.Frame(self.body, bg=T.BG2); cr.pack(fill="x", pady=3)
            tk.Label(cr, text="背景色", bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                     width=9, anchor="w").pack(side="left")
            cv = tk.StringVar(value=f"#{b.bgColor:06X}")
            e = tk.Entry(cr, textvariable=cv, bg=T.BG3, fg=T.FG,
                         insertbackground=T.FG, relief="flat", width=10,
                         font=T.FONT_SM)
            e.pack(side="left", fill="x", expand=True)
            e.bind("<FocusOut>", lambda _e: _apply_color(cv.get()))
            tk.Button(cr, text="选色", command=pick_color, bg=T.BG3,
                      fg=T.FG, relief="flat", font=T.FONT_SM,
                      width=6).pack(side="right", padx=2)
        elif t == 2:  # SPRITE
            chars = app.project.chars if app.project else []
            char_names = [c.name or f"人物{i}" for i, c in enumerate(chars)]
            if not char_names:
                char_names = ["（无人物集）"]
            # 来源选择
            src = tk.StringVar(value="人物集" if b.charIdx >= 0 else "直接路径")
            tk.Radiobutton(self.body, text="⏺ 人物集", value="人物集", variable=src,
                           bg=T.BG2, fg=T.FG, selectcolor=T.BG3,
                           activebackground=T.BG2, font=T.FONT_SM,
                           command=lambda: (setattr(b, "charIdx", 0 if b.charIdx < 0 else b.charIdx), changed())
                           ).pack(fill="x", padx=12, pady=1)
            tk.Radiobutton(self.body, text="⏺ 直接路径", value="直接路径", variable=src,
                           bg=T.BG2, fg=T.FG, selectcolor=T.BG3,
                           activebackground=T.BG2, font=T.FONT_SM,
                           command=lambda: (setattr(b, "charIdx", -1), changed())
                           ).pack(fill="x", padx=12, pady=1)
            if chars:
                self._row("人物", self._combo("charidx", max(0, b.charIdx),
                            char_names,
                            lambda v: (setattr(b, "charIdx", v), changed())))
            st = ""
            states = []
            if 0 <= b.charIdx < len(chars):
                ch = chars[b.charIdx]
                states = [ch.stateNames[i] or f"状态{i}" for i in range(ch.nStates)] or ["状态 0"]
                self._row("状态", self._combo("stateidx",
                            min(max(0, b.stateIdx), len(states) - 1), states,
                            lambda v: (setattr(b, "stateIdx", v), changed())))
            self._browse_row("路径", "sprpath", b.sprPath,
                             lambda v: (setattr(b, "sprPath", v), changed()),
                             IMG_TYPES, app.project_root or ".")
            self._row("图层", self._combo("sprlayer", b.spr_layer, LAYER_NAMES,
                        lambda v: (setattr(b, "spr_layer", v), changed())))
            self._row("位置", self._combo("sprpos", b.spr_pos, POS_NAMES,
                        lambda v: (setattr(b, "spr_pos", v), changed())))
            self._row("动画", self._combo("spranim", b.spr_anim, ANIM_NAMES,
                        lambda v: (setattr(b, "spr_anim", v), changed())))
            self._check("sprshow", b.spr_show,
                        lambda v: (setattr(b, "spr_show", v), changed()),
                        "显示").pack(fill="x", padx=12, pady=2)
            self._check("spr_autofit", b.spr_autofit,
                        lambda v: (setattr(b, "spr_autofit", v), changed()),
                        "自适应").pack(fill="x", padx=12, pady=2)
        elif t == 3:  # AUDIO
            self._row("类型", self._combo("audio_type", b.audio_type, AUD_NAMES,
                        lambda v: (setattr(b, "audio_type", v), changed())))
            self._browse_row("文件", "audio_path", b.audio_path,
                             lambda v: (setattr(b, "audio_path", v), changed()),
                             AUD_TYPES, app.project_root or ".")
            # 音量行
            ar = tk.Frame(self.body, bg=T.BG2); ar.pack(fill="x", pady=3)
            tk.Label(ar, text="音量", bg=T.BG2, fg=T.FG2, font=T.FONT_SM,
                     width=9, anchor="w").pack(side="left")
            sv = tk.IntVar(value=b.volume)
            sc = tk.Scale(ar, from_=0, to=100, orient="horizontal",
                          variable=sv, bg=T.BG2, fg=T.FG, highlightthickness=0,
                          command=lambda _v: (setattr(b, "volume", sv.get()), changed()))
            sc.pack(side="left", fill="x", expand=True)
            tk.Button(self.body, text="试听", command=lambda: app.preview_audio(),
                      bg=T.BG3, fg=T.FG, relief="flat", font=T.FONT_SM
                      ).pack(anchor="w", pady=4)
        elif t == 4:  # WAIT
            self._row("方式", self._combo("wait_type", b.wait_type,
                        ["点击继续", "等待毫秒"],
                        lambda v: (setattr(b, "wait_type", v), changed())))
            self._row("毫秒", self._entry("wait_ms", str(b.wait_ms),
                        lambda v: (setattr(b, "wait_ms", _int(v)), changed())))
        elif t == 5:  # CLEAR
            tk.Label(self.body, text="清空当前对话文本。", bg=T.BG2, fg=T.FG2,
                     font=T.FONT_SM, justify="left", wraplength=220).pack(anchor="w", pady=6)
        elif t == 6:  # CHOICE
            self._row("说话人", self._entry("speaker", b.speaker,
                        lambda v: (setattr(b, "speaker", v), changed())))
            self._row("问题文本", self._text("choice_text", b.choice_text,
                        lambda v: (setattr(b, "choice_text", v), changed()), height=4))
            tk.Label(self.body,
                     text="在其下方添加「选项项」块作为分支；分支内容作为选项项的子块。",
                     bg=T.BG2, fg=T.FG2, font=T.FONT_SM, justify="left",
                     wraplength=220).pack(anchor="w", pady=6)
        elif t == 7:  # OPTION
            self._row("选项文字", self._entry("option_text", b.option_text,
                        lambda v: (setattr(b, "option_text", v), changed())))
            tk.Label(self.body,
                     text="此选项选中后执行的内容:在其下方添加子块(拖入并缩进)。",
                     bg=T.BG2, fg=T.FG2, font=T.FONT_SM, justify="left",
                     wraplength=220).pack(anchor="w", pady=6)
        elif t == 8:  # IF
            self._row("条件", self._entry("cond", b.cond,
                        lambda v: (setattr(b, "cond", v), changed())))
            tk.Label(self.body, text="条件成立时执行其子块。", bg=T.BG2,
                     fg=T.FG2, font=T.FONT_SM, justify="left").pack(anchor="w", pady=4)
        elif t == 9:  # CODE
            self._browse_row("代码文件", "code_file", b.code_file,
                             lambda v: (setattr(b, "code_file", v), changed()),
                             DEX_TYPES, app.project_root or ".")
            self._row("条件", self._entry("code_cond", b.code_cond,
                        lambda v: (setattr(b, "code_cond", v), changed())))
        elif t == 10:  # END
            tk.Label(self.body, text="镜头结束标记。", bg=T.BG2, fg=T.FG2,
                     font=T.FONT_SM, justify="left").pack(anchor="w", pady=6)


def _int(v, d=0):
    try:
        return int(v)
    except Exception:
        return d
