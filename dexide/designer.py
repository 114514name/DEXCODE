"""
dexide/designer.py — EGUI Designer:图形化制作界面(类似 QT Designer)

流程:
  1) 在设计窗体上添加控件,拖拽定位,右侧设置编号/文本/尺寸
  2) 为控件连接信号(信号类型 + 回调函数名)
  3) 生成 DexLang 代码文件(.dex),编译后即可运行(EGUI)

该工具仅用于"设计期"生成代码,运行期不依赖 Python —— 生成的
.dex 由 DEXCODE 编译器编译成 .dexbc,再用 vm.exe 独立运行。

核心逻辑(数据模型 + 代码生成)不依赖 tkinter,可被测试直接调用。
"""

import json
import os

# 控件类型及其可选信号
KINDS = [
    "label", "button", "checkbox", "radio", "edit", "edit_multi",
    "combo", "list", "slider", "progress", "group",
]

KIND_SIGNALS = {
    "label": [],
    "button": ["clicked"],
    "checkbox": ["changed"],
    "radio": ["changed"],
    "edit": ["changed"],
    "edit_multi": ["changed"],
    "combo": ["selected"],
    "list": ["selected"],
    "slider": ["changed"],
    "progress": [],
    "group": [],
}

# 默认控件尺寸
KIND_SIZE = {
    "label": (120, 24), "button": (100, 30), "checkbox": (120, 26),
    "radio": (100, 26), "edit": (160, 26), "edit_multi": (160, 60),
    "combo": (140, 26), "list": (140, 60), "slider": (180, 26),
    "progress": (180, 26), "group": (200, 120),
}

DEFAULT_TITLE = "我的程序"


# ---------- 数据模型 ----------
class Widget:
    def __init__(self, kind, wid, text="", x=20, y=20, w=None, h=None, signals=None):
        self.kind = kind
        self.wid = wid
        self.text = text if text != "" else _default_text(kind)
        dw, dh = KIND_SIZE.get(kind, (120, 24))
        self.x, self.y = x, y
        self.w = w if w is not None else dw
        self.h = h if h is not None else dh
        self.signals = signals if signals is not None else []   # [(signal, cb)]


def _default_text(kind):
    return {
        "button": "按钮", "checkbox": "选项", "radio": "单选",
        "group": "分组", "label": "标签", "edit": "",
    }.get(kind, "")


# ---------- 序列化 ----------
def widget_to_dict(w):
    return {
        "kind": w.kind, "id": w.wid, "text": w.text,
        "x": w.x, "y": w.y, "w": w.w, "h": w.h,
        "signals": [list(s) for s in w.signals],
    }


def widget_from_dict(d):
    w = Widget(d["kind"], d.get("id", 1), d.get("text", ""),
               d.get("x", 20), d.get("y", 20), d.get("w"), d.get("h"))
    w.signals = [tuple(s) for s in d.get("signals", [])]
    return w


def design_to_dict(title, width, height, widgets):
    return {
        "title": title, "width": width, "height": height,
        "widgets": [widget_to_dict(w) for w in widgets],
    }


def design_from_dict(d):
    return {
        "title": d.get("title", DEFAULT_TITLE),
        "width": int(d.get("width", 520)),
        "height": int(d.get("height", 400)),
        "widgets": [widget_from_dict(x) for x in d.get("widgets", [])],
    }


# ---------- 代码生成 ----------
def _cb_name(wid, signal):
    return f"on_{wid}_{signal}"


def gen_dex(design) -> str:
    """把设计数据(design_from_dict 的输出)生成 DexLang 源码字符串。"""
    title = design["title"]
    width = design["width"]
    height = design["height"]
    widgets = design["widgets"]
    L = []
    L.append('include "egui_fast";')
    L.append("")
    L.append("// ---- 信号回调(自动生成,补充你的逻辑) ----")
    seen_cbs = set()
    for w in widgets:
        for signal, cb in w.signals:
            if cb in seen_cbs:
                continue
            seen_cbs.add(cb)
            L.append(f"func {cb}(id) {{")
            L.append(f"    // TODO: 处理 {w.kind} 控件 {w.wid} 的 '{signal}' 信号")
            L.append("}")
    L.append("")
    L.append(f'let win = eg_window("{_escape(title)}", {width}, {height});')
    L.append('eg_connect(win, "closed", "on_win_closed");')
    L.append("")
    L.append("func on_win_closed(id) {")
    L.append("    eg_quit();")
    L.append("}")
    L.append("")
    for w in widgets:
        L.append(f'eg_add("{w.kind}", win, {w.wid});')
        L.append(f"eg_set_pos({w.wid}, {w.x}, {w.y});")
        L.append(f"eg_set_size({w.wid}, {w.w}, {w.h});")
        if w.text != "":
            L.append(f'eg_set_text({w.wid}, "{_escape(w.text)}");')
        for signal, cb in w.signals:
            L.append(f'eg_connect({w.wid}, "{signal}", "{cb}");')
        L.append("")
    L.append("eg_app_run();")
    L.append('print "退出";')
    return "\n".join(L) + "\n"


def _escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def save_design(path, design):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(design_to_dict(design["title"], design["width"],
                                 design["height"], design["widgets"]),
                  f, ensure_ascii=False, indent=2)


def load_design(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return design_from_dict(json.load(f))


# ---------- tkinter UI ----------
def launch(design_path=None):
    """启动 Designer 主窗口(独立运行: python main.py designer)。"""
    import tkinter as tk
    from tkinter import messagebox, simpledialog, ttk

    app = _DesignerApp(tk.Tk())
    if design_path:
        try:
            app.load(design_path)
        except Exception as e:
            messagebox.showerror("打开失败", str(e))
    app.root.mainloop()


class _DesignerApp:
    """Designer 主界面:画布(窗体) + 控件工具栏 + 属性/信号面板。"""

    def __init__(self, root):
        self.root = root
        root.title("EGUI Designer — 界面设计器")
        root.geometry("1080x700")
        root.configure(bg="#2b2d42")

        self.design = {"title": DEFAULT_TITLE, "width": 520, "height": 400,
                       "widgets": []}
        self.widgets = self.design["widgets"]     # List[Widget]
        self.selected = None                       # 当前选中 wid
        self.next_id = 1

        self._build_menu()
        self._build_toolbar()
        self._build_canvas()
        self._build_side()
        self._build_status()
        self._draw_all()

    # ---- 界面搭建 ----
    def _build_menu(self):
        from tkinter import Menu
        mb = Menu(self.root)
        fm = Menu(mb, tearoff=0)
        fm.add_command(label="新建", command=self.new)
        fm.add_command(label="打开设计...", command=self.open_dialog)
        fm.add_command(label="保存设计...", command=self.save_dialog)
        fm.add_separator()
        fm.add_command(label="生成 .dex 代码...", command=self.export)
        fm.add_separator()
        fm.add_command(label="退出", command=self.root.destroy)
        mb.add_cascade(label="文件", menu=fm)
        hm = Menu(mb, tearoff=0)
        hm.add_command(label="帮助", command=self.help_dialog)
        mb.add_cascade(label="帮助", menu=hm)
        self.root.config(menu=mb)

    def _build_toolbar(self):
        import tkinter as tk
        from tkinter import ttk
        bar = ttk.Frame(self.root, padding=4)
        bar.pack(side="top", fill="x")
        ttk.Label(bar, text="窗体:").pack(side="left")
        self.title_var = tk.StringVar(value=DEFAULT_TITLE)
        ttk.Entry(bar, textvariable=self.title_var, width=16).pack(side="left", padx=4)
        ttk.Label(bar, text="宽").pack(side="left")
        self.w_var = tk.StringVar(value="520")
        ttk.Spinbox(bar, from_=200, to=2000, width=6,
                    textvariable=self.w_var).pack(side="left", padx=2)
        ttk.Label(bar, text="高").pack(side="left")
        self.h_var = tk.StringVar(value="400")
        ttk.Spinbox(bar, from_=200, to=2000, width=6,
                    textvariable=self.h_var).pack(side="left", padx=2)
        ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=8)
        ttk.Label(bar, text="添加控件:").pack(side="left")
        for kind in KINDS:
            ttk.Button(bar, text=kind, width=9,
                       command=lambda k=kind: self.add_widget(k)).pack(side="left", padx=1)

    def _build_canvas(self):
        import tkinter as tk
        wrap = tk.Frame(self.root, bg="#2b2d42")
        wrap.pack(side="left", fill="both", expand=True, padx=6, pady=6)
        self.canvas = tk.Canvas(wrap, bg="#1e1e2e", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        # 窗体背景矩形 + 标题
        self.form_rect = None
        self.canvas.bind("<Button-1>", self._on_click)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        self.canvas.bind("<Delete>", lambda e: self.delete_selected())

    def _build_side(self):
        import tkinter as tk
        from tkinter import ttk
        side = ttk.Frame(self.root, padding=8)
        side.pack(side="right", fill="y")
        ttk.Label(side, text="属性", font=("", 11, "bold")).pack(anchor="w")
        grid = ttk.Frame(side)
        grid.pack(fill="x", pady=4)
        self.prop_vars = {}
        for i, (key, label) in enumerate(
                [("id", "编号(id)"), ("text", "文本"), ("x", "x"), ("y", "y"),
                 ("w", "宽"), ("h", "高")]):
            ttk.Label(grid, text=label).grid(row=i, column=0, sticky="w", padx=2, pady=1)
            var = tk.StringVar()
            ttk.Entry(grid, textvariable=var, width=12).grid(row=i, column=1, padx=2, pady=1)
            var.trace_add("write", lambda *a, k=key: self._prop_edited(k))
            self.prop_vars[key] = var
        ttk.Separator(side, orient="horizontal").pack(fill="x", pady=8)
        ttk.Label(side, text="信号连接", font=("", 11, "bold")).pack(anchor="w")
        sigrow = ttk.Frame(side)
        sigrow.pack(fill="x", pady=4)
        self.sig_var = tk.StringVar()
        self.sig_combo = ttk.Combobox(sigrow, textvariable=self.sig_var,
                                      values=[], width=10, state="readonly")
        self.sig_combo.pack(side="left")
        self.cb_var = tk.StringVar()
        ttk.Entry(sigrow, textvariable=self.cb_var, width=14).pack(side="left", padx=3)
        ttk.Button(sigrow, text="连接", command=self.connect_signal).pack(side="left")
        self.sig_list = tk.Listbox(side, height=8, width=34)
        self.sig_list.pack(fill="x", pady=4)
        ttk.Button(side, text="删除选中信号", command=self.remove_signal).pack(fill="x")
        ttk.Button(side, text="删除控件", command=self.delete_selected).pack(fill="x", pady=4)

    def _build_status(self):
        import tkinter as tk
        self.status = tk.Label(self.root, text="就绪:拖拽移动控件,右侧编辑属性",
                               anchor="w", bg="#313244", fg="#cdd6f4")
        self.status.pack(side="bottom", fill="x")

    # ---- 数据操作 ----
    def new(self):
        self.widgets.clear()
        self.selected = None
        self.design["title"] = DEFAULT_TITLE
        self.title_var.set(DEFAULT_TITLE)
        self._draw_all()

    def add_widget(self, kind):
        wid = self.next_id
        self.next_id += 1
        w = Widget(kind, wid, x=30 + 8 * len(self.widgets) % 40,
                   y=30 + 8 * len(self.widgets) % 30)
        self.widgets.append(w)
        self.select(wid)
        self._draw_all()

    def widget(self, wid):
        for w in self.widgets:
            if w.wid == wid:
                return w
        return None

    def select(self, wid):
        self.selected = wid
        self._update_props()
        self._update_signal_combo()
        self._draw_all()

    def delete_selected(self):
        if self.selected is None:
            return
        self.widgets[:] = [w for w in self.widgets if w.wid != self.selected]
        self.selected = None
        self._draw_all()

    def _prop_edited(self, key):
        w = self.widget(self.selected) if self.selected is not None else None
        if w is None:
            return
        try:
            if key == "id":
                val = int(self.prop_vars["id"].get() or 0)
                if val != w.wid and self.widget(val) is None:
                    w.wid = val
                    self._update_signal_combo()
            elif key == "text":
                w.text = self.prop_vars["text"].get()
            elif key in ("x", "y", "w", "h"):
                setattr(w, key, int(self.prop_vars[key].get() or 0))
            self._draw_all()
        except ValueError:
            pass

    def connect_signal(self):
        w = self.widget(self.selected) if self.selected is not None else None
        if w is None:
            return
        signal = self.sig_var.get()
        cb = self.cb_var.get().strip()
        if not signal or not cb:
            return
        w.signals = [s for s in w.signals if s[0] != signal]
        w.signals.append((signal, cb))
        self._update_signal_list()
        self._draw_all()

    def remove_signal(self):
        w = self.widget(self.selected) if self.selected is not None else None
        if w is None:
            return
        sel = self.sig_list.curselection()
        if sel:
            w.signals.pop(sel[0])
            self._update_signal_list()
            self._draw_all()

    # ---- 画布绘制 / 交互 ----
    def _draw_all(self):
        c = self.canvas
        c.delete("all")
        try:
            ww = int(self.w_var.get() or 520)
            wh = int(self.h_var.get() or 400)
        except ValueError:
            ww, wh = 520, 400
        self.design["width"], self.design["height"] = ww, wh
        self.form_rect = c.create_rectangle(20, 20, 20 + ww, 20 + wh,
                                            fill="#313244", outline="#89b4fa", width=2)
        c.create_text(20 + ww // 2, 34, text=self.title_var.get(),
                      fill="#cdd6f4", font=("", 10, "bold"))
        for w in self.widgets:
            x, y = 20 + w.x, 20 + w.y
            fill = "#89b4fa" if w.wid == self.selected else "#585b70"
            c.create_rectangle(x, y, x + w.w, y + w.h, fill=fill,
                               outline="#f9e2af" if w.wid == self.selected else "#45475a",
                               width=2 if w.wid == self.selected else 1,
                               tags=("ctl", str(w.wid)))
            c.create_text(x + 4, y + 3, anchor="nw", text=f"{w.wid}",
                          fill="#1e1e2e", font=("", 8, "bold"))
            c.create_text(x + 4, y + 17, anchor="nw", text=f"{w.kind}",
                          fill="#11111b", font=("", 8))
            if w.text:
                c.create_text(x + w.w // 2, y + w.h // 2, text=w.text,
                              fill="#11111b", font=("", 9))

    def _canvas_widget_at(self, cx, cy):
        # 命中测试:取控件列表里第一个覆盖该点的
        for w in reversed(self.widgets):
            if w.x <= cx - 20 <= w.x + w.w and w.y <= cy - 20 <= w.y + w.h:
                return w
        return None

    def _on_click(self, e):
        w = self._canvas_widget_at(e.x, e.y)
        if w is None:
            self.selected = None
            self._update_props()
            self._update_signal_combo()
            self._draw_all()
            return
        self._drag_off = (e.x - w.x - 20, e.y - w.y - 20)
        self.select(w.wid)

    def _on_drag(self, e):
        if self.selected is None or self._drag_off is None:
            return
        w = self.widget(self.selected)
        if w is None:
            return
        ox, oy = self._drag_off
        w.x = max(0, e.x - 20 - ox)
        w.y = max(0, e.y - 20 - oy)
        self._update_props()
        self._draw_all()

    def _on_release(self, e):
        self._drag_off = None

    # ---- 属性面板同步 ----
    def _update_props(self):
        w = self.widget(self.selected) if self.selected is not None else None
        if w is None:
            for k in self.prop_vars:
                self.prop_vars[k].set("")
            self._update_signal_list()
            return
        self.prop_vars["id"].set(w.wid)
        self.prop_vars["text"].set(w.text)
        self.prop_vars["x"].set(w.x)
        self.prop_vars["y"].set(w.y)
        self.prop_vars["w"].set(w.w)
        self.prop_vars["h"].set(w.h)
        self._update_signal_list()

    def _update_signal_combo(self):
        w = self.widget(self.selected) if self.selected is not None else None
        if w is None:
            self.sig_combo.configure(values=[])
            self.sig_var.set("")
            return
        self.sig_combo.configure(values=KIND_SIGNALS.get(w.kind, []) or [""])
        if KIND_SIGNALS.get(w.kind):
            self.sig_var.set(KIND_SIGNALS[w.kind][0])
        else:
            self.sig_var.set("")

    def _update_signal_list(self):
        self.sig_list.delete(0, "end")
        w = self.widget(self.selected) if self.selected is not None else None
        if w is None:
            return
        for signal, cb in w.signals:
            self.sig_list.insert("end", f"{signal} -> {cb}")

    # ---- 文件操作 ----
    def _current_design(self):
        return design_from_dict(design_to_dict(
            self.title_var.get(), int(self.w_var.get() or 520),
            int(self.h_var.get() or 400), self.widgets))

    def save_dialog(self):
        from tkinter import filedialog
        path = filedialog.asksaveasfilename(
            title="保存设计", defaultextension=".egui",
            filetypes=[("EGUI 设计", "*.egui"), ("所有文件", "*.*")])
        if not path:
            return
        d = self._current_design()
        with open(path, "w", encoding="utf-8") as f:
            json.dump(design_to_dict(d["title"], d["width"], d["height"], d["widgets"]),
                      f, ensure_ascii=False, indent=2)
        self.status.config(text=f"已保存: {path}")

    def open_dialog(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(
            title="打开设计", filetypes=[("EGUI 设计", "*.egui"), ("所有文件", "*.*")])
        if not path:
            return
        try:
            self.load(path)
        except Exception as e:
            from tkinter import messagebox
            messagebox.showerror("打开失败", str(e))

    def load(self, path):
        d = load_design(path)
        self.design = d
        self.widgets = d["widgets"]
        self.title_var.set(d["title"])
        self.w_var.set(d["width"])
        self.h_var.set(d["height"])
        self.next_id = (max((w.wid for w in self.widgets), default=0) + 1)
        self.selected = None
        self._draw_all()
        self.status.config(text=f"已打开: {path}")

    def export(self):
        from tkinter import filedialog, messagebox
        path = filedialog.asksaveasfilename(
            title="生成 DexLang 代码", defaultextension=".dex",
            filetypes=[("DexLang 源码", "*.dex"), ("所有文件", "*.*")])
        if not path:
            return
        code = gen_dex(self._current_design())
        with open(path, "w", encoding="utf-8") as f:
            f.write(code)
        messagebox.showinfo("生成完成",
                            f"已生成 {path}\n\n编译运行:\n  python main.py run \"{path}\"")
        self.status.config(text=f"已生成代码: {path}")

    def help_dialog(self):
        from tkinter import messagebox
        messagebox.showinfo(
            "EGUI Designer",
            "1. 顶部工具栏添加控件\n"
            "2. 在窗体上拖拽移动,点击选中\n"
            "3. 右侧设置编号/文本/尺寸\n"
            "4. 选择信号并填写回调函数名,点“连接”\n"
            "5. 文件→生成 .dex 代码\n\n"
            "回调函数在生成的 .dex 中自动创建,补充逻辑后编译运行。\n"
            "运行时仅需 vm.exe,不依赖 Python。")


if __name__ == "__main__":
    import tkinter as tk
    tkinter = tk
    launch()
