"""bluedit.model — 蓝图数据模型。

镜头 = 入口节点 + 出口节点 + 中间节点。节点有 exec 引脚(执行流)和 value 引脚(数据)。
连线把输出引脚连到输入引脚。
"""

MAX_CHARS = 32
MAX_STATES = 10
MAX_SCENES = 32
MAX_SPR = 4

# ---- 节点类型 ----
N_ENTRY = 0
N_EXIT = 1
N_SPEAK = 2
N_LONG = 3
N_SPRITE = 4
N_CHOICE = 5
N_CODE = 6
N_DEFVAR = 7
N_SETVAR = 8
N_IF = 9
N_GETVAR = 10
N_LOGIC = 11
N_MATH = 12
N_FLOW = 13
N_BG = 14
N_FX_ADD = 15
N_FX_OFF = 16
N_FX_CLEAR = 17
N_MENU = 18
N_TEXT = 19       # 文本拼接(A+B+C)
N_SAY = 20        # 动态文本(显示 value/字面文本)
N_INPUT = 21      # 读取菜单输入框内容
N_RANDOM = 22     # 随机数
N_WAIT = 23       # 等待(毫秒)
N_DOUT = 24       # 数据输出点(场景级,把数据送出场景)
N_DREF = 25       # 场景数据(引用其它场景的数据输出点)
N_TEXTLIT = 26    # 纯文本(字面值,仅 value 输出)
N_NUMLIT = 27     # 纯数字(字面值,仅 value 输出)
N_BOLLIT = 28     # 布尔(字面值 true/false,仅 value 输出)
N_BGM = 29        # BGM 播放
N_SE = 30         # SE 播放
N_SOUND_STOP = 31 # 停止声音
N_SOUND_SWAP = 32 # 切换声音(BGM 换歌)
N_BG_TRANS = 33   # 特效背景切换(交叉淡化)
N_SCENE_TRANS = 34  # 特效镜头切换(淡入淡出)
N_TOAST = 35      # 快速消息提示(角落 Toast)
N_SAVE = 36       # 存档(所有变量 → 文件)
N_LOAD = 37       # 读档(文件 → 恢复变量)
N_FILE_WRITE = 38 # 写入文件(内容 → 文件)
N_FILE_READ = 39  # 读取文件(文件 → 字符串输出)
N_CHECKPOINT = 40 # 存档点(记录位置+可选存档)
N_JUMP = 41       # 跳转到存档点(读档+按位置继续)

NODE_NAMES = {
    N_ENTRY: "镜头入口", N_EXIT: "镜头出口",
    N_SPEAK: "说话", N_LONG: "长对话", N_SPRITE: "显示立绘",
    N_BG: "显示背景",
    N_FX_ADD: "添加场景特效", N_FX_OFF: "取消场景特效",
    N_FX_CLEAR: "取消全部特效", N_MENU: "菜单",
    N_TEXT: "文本拼接", N_SAY: "动态文本", N_INPUT: "读取输入框",
    N_RANDOM: "随机数", N_WAIT: "等待",
    N_DOUT: "数据输出点", N_DREF: "场景数据",
    N_TEXTLIT: "纯文本", N_NUMLIT: "纯数字", N_BOLLIT: "布尔",
    N_BGM: "BGM 播放", N_SE: "SE 播放", N_SOUND_STOP: "停止声音",
    N_SOUND_SWAP: "切换声音", N_BG_TRANS: "柔和换背景", N_SCENE_TRANS: "柔和切镜头",
    N_TOAST: "消息提示",
    N_SAVE: "存档", N_LOAD: "读档", N_FILE_WRITE: "写入文件", N_FILE_READ: "读取文件",
    N_CHECKPOINT: "存档点", N_JUMP: "跳转到存档点",
    N_CHOICE: "选项框", N_CODE: "代码框", N_DEFVAR: "定义变量",
    N_SETVAR: "改变变量", N_IF: "如果", N_GETVAR: "变量取值",
    N_LOGIC: "逻辑运算", N_MATH: "运算", N_FLOW: "镜头控制",
}

# 节点标题色(用于画布)
NODE_COLORS = {
    N_ENTRY: "#2d7a4a", N_EXIT: "#7a2d3a",
    N_SPEAK: "#2d4a7a", N_LONG: "#4a3a7a", N_SPRITE: "#1f6a5c",
    N_BG: "#2a5f7a",
    N_FX_ADD: "#7a2d5a", N_FX_OFF: "#5a2d3a", N_FX_CLEAR: "#3a2d5a",
    N_MENU: "#5a4a7a",
    N_TEXT: "#2d5a7a", N_SAY: "#1f6a5c", N_INPUT: "#4a3a7a",
    N_RANDOM: "#7a5a1f", N_WAIT: "#3a4a5a",
    N_DOUT: "#0f6a5a", N_DREF: "#0f5a7a",
    N_TEXTLIT: "#2d4a7a", N_NUMLIT: "#7a5a1f", N_BOLLIT: "#5a3a7a",
    N_BGM: "#4a3a7a", N_SE: "#7a2d5a", N_SOUND_STOP: "#5a2d3a",
    N_SOUND_SWAP: "#6a4a1f", N_BG_TRANS: "#2a5f7a", N_SCENE_TRANS: "#2d7a4a",
    N_TOAST: "#5a4a1f",
    N_SAVE: "#1f5a3a", N_LOAD: "#3a5a1f", N_FILE_WRITE: "#2a4a6a", N_FILE_READ: "#4a2a6a",
    N_CHECKPOINT: "#1f7a4a", N_JUMP: "#6a2a3a",
    N_CHOICE: "#6a2436", N_CODE: "#1f3a5a", N_DEFVAR: "#5a4a1f",
    N_SETVAR: "#7a4a1f", N_IF: "#1f4a2a", N_GETVAR: "#3a5a1f",
    N_LOGIC: "#4a3a7a", N_MATH: "#3a4a5a", N_FLOW: "#5a2a4a",
}

NODE_ICONS = {
    N_ENTRY: "▶", N_EXIT: "■",
    N_SPEAK: "💬", N_LONG: "💬", N_SPRITE: "👤",
    N_BG: "🖼",
    N_FX_ADD: "✨", N_FX_OFF: "🚫", N_FX_CLEAR: "🧹",
    N_MENU: "☰",
    N_TEXT: "⧉", N_SAY: "📣", N_INPUT: "✎", N_RANDOM: "🎲", N_WAIT: "⏳",
    N_DOUT: "📤", N_DREF: "📥",
    N_TEXTLIT: "🅃", N_NUMLIT: "#", N_BOLLIT: "⚑",
    N_BGM: "🎵", N_SE: "🔔", N_SOUND_STOP: "🔇",
    N_SOUND_SWAP: "🔁", N_BG_TRANS: "🖼", N_SCENE_TRANS: "🎬",
    N_TOAST: "💬",
    N_SAVE: "📁", N_LOAD: "📂", N_FILE_WRITE: "📝", N_FILE_READ: "📄",
    N_CHECKPOINT: "🚩", N_JUMP: "🎯",
    N_CHOICE: "❓", N_CODE: "⌨", N_DEFVAR: "=",
    N_SETVAR: "⇐", N_IF: "🔀", N_GETVAR: "☰",
    N_LOGIC: "⚖", N_MATH: "∑", N_FLOW: "⇨",
}

# 位置预设名
POS_NAMES = ["中间", "左", "右", "偏左", "偏右"]
LAYER_NAMES = ["图层 0", "图层 1", "图层 2", "图层 3"]
TYPE_NAMES = ["整数 int", "小数 float", "字符串 string", "布尔 bool"]
TEXT_MODE_NAMES = ["逐字显示", "直接全部", "先空框点击"]
BG_FIT_NAMES = ["拉伸填满", "原大小", "保持比例完整", "保持比例填满"]
SPRITE_FIT_NAMES = ["原大小", "自动适配", "完整显示", "填满"]
# 场景特效(索引顺序对应 FX_BITS 位掩码,与引擎 gal.fx 一致)
FX_NAMES = ["老电影", "回忆", "朦胧", "震动", "马赛克", "反色",
            "模糊", "镜像", "灰度", "暗角", "扫描线", "夜视",
            "老电视", "故障"]
FX_BITS = [1 << i for i in range(len(FX_NAMES))]
LOGIC_OPS = ["==", "!=", ">", "<", ">=", "<="]
MATH_OPS = ["+", "-", "*", "/", "%", "^"]

# 哪些节点可加入"块面板"(可创建)
PALETTE = [N_SPEAK, N_LONG, N_SPRITE, N_BG, N_CHOICE, N_CODE, N_DEFVAR,
           N_SETVAR, N_IF, N_GETVAR, N_LOGIC, N_MATH, N_FLOW,
           N_FX_ADD, N_FX_OFF, N_FX_CLEAR, N_MENU,
           N_TEXT, N_SAY, N_INPUT, N_RANDOM, N_WAIT,
           N_DOUT, N_DREF, N_TEXTLIT, N_NUMLIT, N_BOLLIT,
           N_BGM, N_SE, N_SOUND_STOP, N_SOUND_SWAP, N_BG_TRANS, N_SCENE_TRANS,
           N_TOAST, N_SAVE, N_LOAD, N_FILE_WRITE, N_FILE_READ,
           N_CHECKPOINT, N_JUMP]


class Pin:
    def __init__(self, kind, name="", pin_id=0):
        self.id = pin_id
        self.kind = kind          # 'exec' | 'value'
        self.name = name          # 显示名(如 "条件"、"数值")
        self.source = "literal"   # value 引脚: 'literal' | 'variable'
        self.literal = ""         # 字面值
        self.variable = ""        # 变量名
        self.var_type = 0         # 字面值类型

    def to_dict(self):
        return {"kind": self.kind, "name": self.name, "source": self.source,
                "literal": self.literal, "variable": self.variable,
                "var_type": self.var_type}

    @classmethod
    def from_dict(cls, d):
        p = cls(d.get("kind", "exec"), d.get("name", ""))
        p.source = d.get("source", "literal")
        p.literal = d.get("literal", "")
        p.variable = d.get("variable", "")
        p.var_type = d.get("var_type", 0)
        return p


class Link:
    def __init__(self, link_id, from_node, from_pin, to_node, to_pin):
        self.id = link_id
        self.from_node = from_node
        self.from_pin = from_pin
        self.to_node = to_node
        self.to_pin = to_pin


class Node:
    """蓝图节点。x/y 为画布坐标(相对画布)。"""

    def __init__(self, ntype, node_id, x=0, y=0):
        self.id = node_id
        self.type = ntype
        self.x = x
        self.y = y
        self.inputs = []
        self.outputs = []
        self.setup_pins(ntype)
        self.reset_params(ntype)
        if ntype == N_CHOICE:
            self.sync_choice_outputs()

    def setup_pins(self, t):
        self.inputs, self.outputs = [], []
        if t == N_ENTRY:
            self.outputs = [Pin('exec', '')]
        elif t == N_EXIT:
            self.inputs = [Pin('exec', '')]
        elif t in (N_SPEAK, N_LONG, N_SPRITE, N_BG, N_CODE, N_DEFVAR, N_FLOW,
                   N_FX_ADD, N_FX_OFF, N_FX_CLEAR, N_MENU,
                   N_BGM, N_SE, N_SOUND_STOP, N_SOUND_SWAP,
                   N_BG_TRANS, N_SCENE_TRANS, N_TOAST,
                   N_SAVE, N_LOAD, N_CHECKPOINT, N_JUMP):
            self.inputs = [Pin('exec', '')]
            self.outputs = [Pin('exec', '')]
        elif t == N_FILE_WRITE:
            self.inputs = [Pin('exec', ''), Pin('value', '内容')]
            self.outputs = [Pin('exec', '')]
        elif t == N_FILE_READ:
            self.outputs = [Pin('value', '')]
        elif t == N_CHOICE:
            self.inputs = [Pin('exec', '')]
            self.outputs = []      # 选项输出在参数变化时重建
        elif t == N_SETVAR:
            self.inputs = [Pin('exec', ''), Pin('value', '新值')]
            self.outputs = [Pin('exec', '')]
        elif t == N_IF:
            self.inputs = [Pin('exec', ''), Pin('value', '条件')]
            self.outputs = [Pin('exec', '真'), Pin('exec', '假')]
        elif t == N_GETVAR:
            self.outputs = [Pin('value', '')]
        elif t == N_INPUT:
            self.outputs = [Pin('value', '')]
        elif t == N_RANDOM:
            self.outputs = [Pin('value', '')]
        elif t == N_TEXT:
            self.inputs = [Pin('value', 'A'), Pin('value', 'B'), Pin('value', 'C')]
            self.outputs = [Pin('value', '')]
        elif t == N_LOGIC:
            self.inputs = [Pin('value', 'A'), Pin('value', 'B')]
            self.outputs = [Pin('value', '')]
        elif t == N_SAY:
            self.inputs = [Pin('exec', ''), Pin('value', '文本')]
            self.outputs = [Pin('exec', '')]
        elif t == N_WAIT:
            self.inputs = [Pin('exec', '')]
            self.outputs = [Pin('exec', '')]
        elif t == N_DOUT:
            self.inputs = [Pin('exec', ''), Pin('value', '数据')]
            self.outputs = [Pin('exec', '')]
        elif t == N_DREF:
            self.outputs = [Pin('value', '')]
        elif t in (N_TEXTLIT, N_NUMLIT, N_BOLLIT):
            self.outputs = [Pin('value', '')]
        elif t == N_MATH:
            self.inputs = [Pin('value', 'A'), Pin('value', 'B')]
            self.outputs = [Pin('value', '')]

    def to_dict(self):
        d = {"id": self.id, "type": self.type, "x": self.x, "y": self.y,
             "inputs": [p.to_dict() for p in self.inputs],
             "outputs": [p.to_dict() for p in self.outputs]}
        for attr in ("text", "text_pos", "text_mode", "text_speed", "show_name", "name", "use_char",
                     "char_idx", "state_idx", "lines", "spr_path", "spr_pos_mode",
                     "use_xy", "spr_x", "spr_y", "spr_layer", "spr_scale",
                     "spr_alpha", "spr_anim", "spr_fit", "bg_path", "bg_mode",
                     "bg_color", "bg_fit", "fx", "fx_strength", "choice_text", "options",
                     "code_file", "code_cond", "scope", "var_name", "var_type",
                     "var_value", "set_mode", "op", "flow_target", "flow_scene",
                     "cond", "mode", "value", "menu_idx", "action",
                     "result_var", "signal", "signal_data",
                     "ctrl", "max", "ms",
                     "out_name", "out_scene",
                     "lit_text", "lit_num", "lit_bool",
                     "sound_path", "sound_vol", "stop_target",
                     "bg_trans_ms", "scene_trans_ms",
                     "toast_text", "toast_corner", "toast_ms",
                     "file_path", "ck_name",
                     "long_clear_start", "long_narration_hide", "long_dual"):
            if hasattr(self, attr):
                d[attr] = getattr(self, attr)
        return d

    @classmethod
    def from_dict(cls, d):
        n = cls(d["type"], d["id"], d.get("x", 0), d.get("y", 0))
        n.inputs = [Pin.from_dict(p) for p in d.get("inputs", [])]
        n.outputs = [Pin.from_dict(p) for p in d.get("outputs", [])]
        for attr in ("text", "text_pos", "text_mode", "text_speed", "show_name", "name", "use_char",
                     "char_idx", "state_idx", "lines", "spr_path", "spr_pos_mode",
                     "use_xy", "spr_x", "spr_y", "spr_layer", "spr_scale",
                     "spr_alpha", "spr_anim", "spr_fit", "bg_path", "bg_mode",
                     "bg_color", "bg_fit", "fx", "fx_strength", "choice_text", "options",
                     "code_file", "code_cond", "scope", "var_name", "var_type",
                     "var_value", "set_mode", "op", "flow_target", "flow_scene",
                     "cond", "mode", "value", "menu_idx", "action",
                     "result_var", "signal", "signal_data",
                     "ctrl", "max", "ms",
                     "out_name", "out_scene",
                     "lit_text", "lit_num", "lit_bool",
                     "sound_path", "sound_vol", "stop_target",
                     "bg_trans_ms", "scene_trans_ms",
                     "toast_text", "toast_corner", "toast_ms",
                     "file_path", "ck_name",
                     "long_clear_start", "long_narration_hide", "long_dual"):
            if attr in d:
                setattr(n, attr, d[attr])
        if not n.lines:
            n.lines = []
        if not n.options:
            n.options = []
        if n.type == N_CHOICE:
            n.sync_choice_outputs()
        return n

    def reset_params(self, t):
        self.text = ""
        self.text_pos = 0
        self.text_mode = 0        # 0逐字 1直接全部 2先空框点击
        self.text_speed = 0       # 逐字间隔 ms,0=用全局默认
        self.show_name = 1
        self.name = ""
        self.use_char = 0
        self.char_idx = -1
        self.state_idx = 0
        # 长对话
        self.lines = []
        # 长对话立绘模式(可详细设置)
        self.long_clear_start = 1    # 块开始先清理所有立绘层(防跨块残留)
        self.long_narration_hide = 1 # 旁白行隐藏所有立绘(旁白空白)
        self.long_dual = 1           # 同屏双人:切换说话人时保留其他人(高亮当前)
        # 立绘
        self.spr_path = ""
        self.spr_pos_mode = 0
        self.use_xy = 0
        self.spr_x = 0
        self.spr_y = 0
        self.spr_layer = 0
        self.spr_scale = 100
        self.spr_alpha = 255
        self.spr_anim = 0
        self.spr_fit = 0          # 立绘适配:0原大小 1自动适配(裁剪+高78%) 2完整显示 3填满
        # 背景
        self.bg_path = ""
        self.bg_mode = 0          # 0 图片 1 纯色
        self.bg_color = 0x000000
        self.bg_fit = 0           # 背景适配:0拉伸 1原大小 2保持比例完整 3保持比例填满
        # 场景特效
        self.fx = 0               # 特效索引(对应 FX_NAMES)
        self.fx_strength = 50     # 特效强度 0-100
        # 选项
        self.choice_text = ""
        self.options = []
        # 代码
        self.code_file = ""
        self.code_cond = ""
        # 变量
        self.scope = 0
        self.var_name = ""
        self.var_type = 0
        self.var_value = ""
        self.set_mode = 0          # 0 字面值 1 来自value输入
        self.value = ""
        # 逻辑/运算
        self.op = "=="
        # if
        self.mode = 0              # 0 字面条件 1 来自value输入
        self.cond = ""
        # 镜头控制
        self.flow_target = 0       # 0 下一镜头 1 指定镜头 2 结束
        self.flow_scene = -1
        # 菜单块
        self.menu_idx = -1         # 引用的菜单索引
        self.action = 0            # 0 运行独占菜单 1 显示叠加 2 隐藏叠加 3 发送信号
        self.result_var = ""       # 独占菜单结果存到的变量名
        self.signal = ""           # 发送的信号名
        self.signal_data = ""      # 信号数据
        # 读取输入框/菜单控件
        self.ctrl = ""             # 控件名
        # 随机数
        self.max = 100             # 随机上限
        # 等待
        self.ms = 1000             # 毫秒
        # 数据输出点 / 场景数据
        self.out_name = ""         # 数据输出点名(场景内唯一)
        self.out_scene = -1        # 引用的源场景索引(N_DREF)
        # 字面量块
        self.lit_text = ""         # N_TEXTLIT 纯文本内容
        self.lit_num = "0"         # N_NUMLIT 纯数字
        self.lit_bool = 0          # N_BOLLIT 布尔 0/1
        # 声音
        self.sound_path = ""       # BGM/SE/切换 音频文件
        self.sound_vol = 80        # 音量 0-100
        self.stop_target = 0       # 停止:0 BGM 1 SE 2 全部
        # 柔和过渡
        self.bg_trans_ms = 400     # 柔和换背景时长 ms
        self.scene_trans_ms = 300  # 柔和切镜头时长 ms
        # 消息提示
        self.toast_text = ""       # 提示文本
        self.toast_corner = 3      # 0左上 1右上 2左下 3右下
        self.toast_ms = 1500       # 显示时长 ms
        # 文件 IO
        self.file_path = ""        # 存档/读档/文件读写 路径(相对项目根)
        # 存档点
        self.ck_name = ""          # 存档点名(跳转目标)

    # ---- 便捷 ----
    def title(self):
        return NODE_NAMES.get(self.type, "?")

    def sync_choice_outputs(self):
        """选项框:每个选项一个 exec 输出。"""
        n = max(1, len(self.options))
        if len(self.outputs) != n:
            self.outputs = [Pin('exec', f"选项 {i + 1}") for i in range(n)]

    # ---- 文本拼接动态输入段 ----
    def text_input_names(self):
        """文本拼接各输入段显示名 A/B/C/..."""
        names = []
        for i in range(len(self.inputs)):
            if i < 26:
                names.append(chr(ord('A') + i))
            else:
                names.append(f"S{i}")
        return names

    def text_add_input(self):
        """文本拼接增加一个输入段(上限 12)。"""
        if self.type != N_TEXT or len(self.inputs) >= 12:
            return False
        i = len(self.inputs)
        name = chr(ord('A') + i) if i < 26 else f"S{i}"
        self.inputs.append(Pin('value', name))
        return True

    def text_remove_input(self):
        """文本拼接删除最后一个输入段(至少保留 2 段)。"""
        if self.type != N_TEXT or len(self.inputs) <= 2:
            return False
        self.inputs.pop()
        return True

    def sync_menu_outputs(self, menu):
        """N_MENU:按引用菜单自动重建输入/输出口。
        输入口 = [exec] + 数据入口参数(value, 名字=参数名);
        输出口 = 菜单「退出」节点(exec)+ 输入框控件(value)。"""
        if self.type != N_MENU:
            return
        # 输入口:exec + 数据入口参数(value)
        ins = [Pin('exec', '')]
        if menu is not None:
            for p in menu.param_names():
                ins.append(Pin('value', p))
        self.inputs = ins
        # 输出口
        if self.action == 0 and menu is not None:
            outs = []
            exs = [n.exit_name for n in menu.exits()]
            if exs:
                for name in exs:
                    outs.append(Pin('exec', name))
            else:
                outs.append(Pin('exec', ''))
            for c in menu.input_ctrls():
                outs.append(Pin('value', c.ctrl))
            self.outputs = outs
            return
        self.outputs = [Pin('exec', '')]


class Character:
    def __init__(self):
        self.name = ""
        self.states = [""] * MAX_STATES
        self.stateNames = [""] * MAX_STATES
        self.nStates = 0
        self.cur = 0


class Var:
    """编辑器统计出的变量(用于下拉/实时列表)。"""
    def __init__(self, name="", type_=0, scope=0, value=""):
        self.name = name
        self.type = type_       # 0 int 1 float 2 string 3 bool
        self.scope = scope      # 0 全局 1 局部
        self.value = value


class Scene:
    def __init__(self, title=None):
        self.title = title or "镜头"
        self.bg = ""
        self.bgColor = 0x101018
        self.useBgImg = 1
        self.bg_fit = 0           # 背景适配:0拉伸 1原大小 2保持比例完整 3保持比例填满
        self.nodes = []
        self.links = []
        self.entry_id = -1
        self.exit_id = -1
        self.nid = 1
        self.pid = 1
        self.lid = 1

    # ---- 节点/引脚/连线 ----
    def node(self, nid):
        for n in self.nodes:
            if n.id == nid:
                return n
        return None

    def new_node(self, ntype, x=0, y=0):
        n = Node(ntype, self.nid, x, y)
        self.nid += 1
        self.nodes.append(n)
        return n

    def delete_node(self, nid):
        # 入口/出口节点不可删除
        n = self.node(nid)
        if n is not None and n.type in (N_ENTRY, N_EXIT):
            return
        self.links = [l for l in self.links
                      if l.from_node != nid and l.to_node != nid]
        self.nodes = [n for n in self.nodes if n.id != nid]

    def find_exec_target(self, node, out_idx=0):
        """从节点第 out_idx 个 exec 输出找连到的下一节点。"""
        for l in self.links:
            if l.from_node == node.id and l.from_pin == out_idx:
                return self.node(l.to_node)
        return None

    def find_value_source(self, node, in_idx):
        """找连到节点第 in_idx 个输入 value 的来源节点(输出 value 的节点)。"""
        for l in self.links:
            if l.to_node == node.id and l.to_pin == in_idx:
                return self.node(l.from_node)
        return None

    def find_value_source_with_pin(self, node, in_idx):
        """同 find_value_source,但额外返回来源的输出引脚序号(供多输出口节点定位)。"""
        for l in self.links:
            if l.to_node == node.id and l.to_pin == in_idx:
                return self.node(l.from_node), l.from_pin
        return None, None

    def find_value_link(self, node, in_idx):
        for l in self.links:
            if l.to_node == node.id and l.to_pin == in_idx:
                return l
        return None

    def node_has_incoming(self, node):
        """节点是否已有 exec 输入连接。"""
        for l in self.links:
            if l.to_node == node.id and l.to_pin == 0:
                return True
        return False

    def node_has_outgoing(self, node, out_idx=0):
        for l in self.links:
            if l.from_node == node.id and l.from_pin == out_idx:
                return True
        return False

    def pin_count(self):
        """统计所有节点的引脚总数(供连线段检查)。"""
        return sum(len(n.inputs) + len(n.outputs) for n in self.nodes)

    # ---- 数据输出点 ----
    def data_outputs(self):
        """本镜头所有数据输出点节点。"""
        return [n for n in self.nodes if n.type == N_DOUT]

    def data_names(self):
        """数据输出点名(去重保序)。"""
        names, seen = [], set()
        for n in self.data_outputs():
            if n.out_name and n.out_name not in seen:
                seen.add(n.out_name)
                names.append(n.out_name)
        return names

    def duplicate_data_names(self):
        """重名的数据输出点(去重后仍有多个)。"""
        from collections import Counter
        c = Counter(n.out_name for n in self.data_outputs() if n.out_name)
        return [k for k, v in c.items() if v > 1]


class Project:
    def __init__(self):
        self.title = "新项目"
        self.resDir = ""
        self.author = ""
        self.version = "1.0"
        self.description = ""
        self.win_w = 960
        self.win_h = 540
        self.chars = []
        self.scenes = []
        self.menus = []          # MENU 场景列表(MENUScene)
        self.cur = 0
        self.cur_menu = 0


def char_by_index(p, idx):
    if 0 <= idx < len(p.chars):
        return p.chars[idx]
    return None


def char_state_path(p, idx, state):
    ch = char_by_index(p, idx)
    if ch and 0 <= state < ch.nStates and ch.states[state]:
        return ch.states[state]
    return ""


def link_to_dict(l):
    return {"id": l.id, "fn": l.from_node, "fp": l.from_pin,
            "tn": l.to_node, "tp": l.to_pin}


def link_from_dict(d):
    return Link(d["id"], d["fn"], d["fp"], d["tn"], d["tp"])


def scene_to_dict(sc):
    return {"title": sc.title, "bg": sc.bg, "bgColor": sc.bgColor,
            "useBgImg": sc.useBgImg, "bg_fit": sc.bg_fit,
            "entry": sc.entry_id, "exit": sc.exit_id,
            "nid": sc.nid, "pid": sc.pid, "lid": sc.lid,
            "nodes": [n.to_dict() for n in sc.nodes],
            "links": [link_to_dict(l) for l in sc.links]}


def scene_from_dict(d):
    sc = Scene(d.get("title", "镜头"))
    sc.bg = d.get("bg", "")
    sc.bgColor = d.get("bgColor", 0x101018)
    sc.useBgImg = d.get("useBgImg", 1)
    sc.bg_fit = d.get("bg_fit", 0)
    sc.entry_id = d.get("entry", -1)
    sc.exit_id = d.get("exit", -1)
    sc.nid = d.get("nid", 1)
    sc.pid = d.get("pid", 1)
    sc.lid = d.get("lid", 1)
    sc.nodes = [Node.from_dict(n) for n in d.get("nodes", [])]
    sc.links = [link_from_dict(l) for l in d.get("links", [])]
    if not sc.nodes:
        entry = sc.new_node(N_ENTRY, 60, 200)
        sc.entry_id = entry.id
        exitn = sc.new_node(N_EXIT, 700, 200)
        sc.exit_id = exitn.id
    return sc


def char_to_dict(c):
    return {"name": c.name, "states": c.states, "stateNames": c.stateNames,
            "nStates": c.nStates, "cur": c.cur}


def char_from_dict(d):
    c = Character()
    c.name = d.get("name", "")
    c.states = (d.get("states") or [""] * MAX_STATES)[:MAX_STATES]
    while len(c.states) < MAX_STATES:
        c.states.append("")
    c.stateNames = (d.get("stateNames") or [""] * MAX_STATES)[:MAX_STATES]
    while len(c.stateNames) < MAX_STATES:
        c.stateNames.append("")
    c.nStates = d.get("nStates", 0)
    c.cur = d.get("cur", 0)
    return c


def project_to_dict(p):
    return {"format": "bluescene", "title": p.title, "res": p.resDir,
            "author": p.author, "version": p.version,
            "description": p.description, "win_w": p.win_w, "win_h": p.win_h,
            "chars": [char_to_dict(c) for c in p.chars],
            "scenes": [scene_to_dict(s) for s in p.scenes],
            "menus": [menu_to_dict(m) for m in p.menus]}


def project_from_dict(d):
    p = Project()
    p.title = d.get("title", "新项目")
    p.resDir = d.get("res", "")
    p.author = d.get("author", "")
    p.version = d.get("version", "1.0")
    p.description = d.get("description", "")
    p.win_w = d.get("win_w", 960)
    p.win_h = d.get("win_h", 540)
    p.chars = [char_from_dict(c) for c in d.get("chars", [])]
    p.scenes = [scene_from_dict(s) for s in d.get("scenes", [])]
    p.menus = [menu_from_dict(m) for m in d.get("menus", [])]
    if not p.scenes:
        p.scenes = [scene_defaults()]
    p.cur = 0
    p.cur_menu = 0
    return p


def scene_defaults(title=None):
    sc = Scene(title or "镜头")
    sc.bgColor = 0x101018
    sc.useBgImg = 1
    entry = sc.new_node(N_ENTRY, 60, 200)
    exitn = sc.new_node(N_EXIT, 700, 200)
    sc.entry_id = entry.id
    sc.exit_id = exitn.id
    return sc


def project_defaults():
    p = Project()
    p.title = "新项目"
    p.scenes = [scene_defaults("镜头 1")]
    p.menus = []
    return p


# =====================================================================
# MENU 场景系统
#   独立于镜头体系的 GUI 菜单蓝图。存储于项目内(随 .bluescene 保存)。
#   控件:0按钮 1文字 2图片 3面板 4输入框
#   节点:M_START 开场起点 / M_CTRL 控件 / M_CLICK 点击起点 /
#         M_SIGNAL 信号起点 / M_EXIT 退出节点
# =====================================================================

M_START = 200
M_CTRL = 201
M_CLICK = 202
M_SIGNAL = 203
M_EXIT = 204
M_IN = 205                # 数据入口(镜头菜单块传入的参数)
M_JUMP = 206             # 菜单跳转(结束菜单并跳转指定镜头)

MENU_NODE_NAMES = {
    M_START: "开场起点", M_CTRL: "控件", M_CLICK: "点击起点",
    M_SIGNAL: "信号起点", M_EXIT: "退出", M_IN: "数据入口",
    M_JUMP: "跳转镜头",
}
MENU_NODE_COLORS = {
    M_START: "#2d7a4a", M_CTRL: "#2d4a7a", M_CLICK: "#7a2d3a",
    M_SIGNAL: "#4a3a7a", M_EXIT: "#7a2d3a", M_IN: "#1f7a6a",
    M_JUMP: "#7a5a2d",
}
MENU_NODE_ICONS = {
    M_START: "▶", M_CTRL: "▣", M_CLICK: "🖱", M_SIGNAL: "📡", M_EXIT: "■",
    M_IN: "⇥", M_JUMP: "➜",
}
# 控件类型名(与引擎 MenuCtrl.type 一致)
MENU_CTRL_TYPE_NAMES = ["按钮", "文字", "图片", "面板", "输入框"]
# 图片控件适配(相对控件矩形;与背景/立绘语义一致)
MENU_CTRL_FIT_NAMES = ["拉伸填满", "原大小", "保持比例完整", "保持比例填满"]
MENU_MODE_NAMES = ["独占菜单 (modal)", "叠加菜单 (overlay)"]


class MenuNode:
    """MENU 场景节点。x/y 为画布坐标;控件几何用 px/py/pw/ph。"""

    def __init__(self, ntype, nid, x=0, y=0):
        self.id = nid
        self.type = ntype
        self.x = x
        self.y = y
        self.reset_params(ntype)
        self.setup_pins(ntype)

    def setup_pins(self, t):
        self.inputs, self.outputs = [], []
        if t == M_START:
            self.outputs = [Pin('exec', '')]
        elif t == M_CTRL:
            self.inputs = [Pin('exec', ''), Pin('value', '文字')]
            self.outputs = [Pin('exec', '')]
        elif t in (M_CLICK, M_SIGNAL):
            self.outputs = [Pin('exec', '')]
        elif t == M_EXIT:
            self.inputs = [Pin('exec', '')]
        elif t == M_IN:
            self.outputs = [Pin('value', '')]
        elif t == M_JUMP:
            self.inputs = [Pin('exec', '')]
            self.outputs = [Pin('exec', '')]
        # ---- 复用镜头节点(变量/逻辑/运算/拼接/字面量/文件/消息/等待/读输入框) ----
        elif t in (N_DEFVAR, N_SAVE, N_LOAD, N_TOAST, N_WAIT):
            self.inputs = [Pin('exec', '')]
            self.outputs = [Pin('exec', '')]
        elif t in (N_SETVAR, N_FILE_WRITE):
            self.inputs = [Pin('exec', ''),
                           Pin('value', '内容' if t == N_FILE_WRITE else '新值')]
            self.outputs = [Pin('exec', '')]
        elif t in (N_GETVAR, N_FILE_READ, N_RANDOM, N_INPUT):
            self.outputs = [Pin('value', '')]
        elif t == N_TEXT:
            self.inputs = [Pin('value', 'A'), Pin('value', 'B'), Pin('value', 'C')]
            self.outputs = [Pin('value', '')]
        elif t in (N_LOGIC, N_MATH):
            self.inputs = [Pin('value', 'A'), Pin('value', 'B')]
            self.outputs = [Pin('value', '')]
        elif t in (N_TEXTLIT, N_NUMLIT, N_BOLLIT):
            self.outputs = [Pin('value', '')]

    def reset_params(self, t):
        # 控件(M_CTRL)
        self.ctrl = ""          # 控件名
        self.create = 1         # 1 不存在则创建;0 仅更新已存在
        self.ctype = 0          # 控件类型
        self.px = 0             # 控件几何(相对菜单画面)
        self.py = 0
        self.pw = 200
        self.ph = 50
        self.text = ""
        self.bg = ""           # 颜色(#rrggbb)或图片路径;空=默认
        self.font = 24
        self.show = 1
        self.bold = 1           # 按钮文字加粗
        self.use_signal_data = 0  # 文字来自信号数据(叠加菜单动态更新)
        self.fit = 0            # 图片控件适配:0拉伸 1原大小 2完整 3填满(相对控件)
        # 点击起点(M_CLICK)
        self.target_ctrl = ""   # 绑定控件名
        # 信号起点(M_SIGNAL)
        self.signal = ""        # 信号名
        # 退出节点(M_EXIT)
        self.exit_name = "退出"  # 退出结果名
        # 数据入口(M_IN)
        self.param_name = ""     # 参数名(镜头菜单块对应输入口名)
        # 菜单跳转(M_JUMP)
        self.flow_target = 1      # 1 指定镜头
        self.flow_scene = -1      # 目标镜头索引
        # ---- 复用镜头节点字段 ----
        # 变量
        self.var_name = ""
        self.var_type = 0         # 0 int 1 float 2 string 3 bool
        self.var_value = ""
        self.set_mode = 0         # 0 字面值 1 来自value输入
        self.value = ""
        self.scope = 0            # 菜单内统一用全局
        # 逻辑/运算
        self.op = "=="
        # 字面量
        self.lit_text = ""
        self.lit_num = "0"
        self.lit_bool = 0
        # 文件/存档
        self.file_path = ""
        # 消息
        self.toast_text = ""
        self.toast_corner = 3
        self.toast_ms = 1500
        # 随机/等待
        self.max = 100
        self.ms = 1000

    def title(self):
        return MENU_NODE_NAMES.get(self.type, "?")

    def to_dict(self):
        d = {"id": self.id, "type": self.type, "x": self.x, "y": self.y}
        for a in ("ctrl", "create", "ctype", "px", "py", "pw", "ph", "text",
                  "bg", "font", "show", "bold", "use_signal_data", "fit",
                  "target_ctrl", "signal", "exit_name", "param_name",
                  "flow_target", "flow_scene",
                  "var_name", "var_type", "var_value", "set_mode", "value",
                  "scope", "op", "lit_text", "lit_num", "lit_bool",
                  "file_path", "toast_text", "toast_corner", "toast_ms",
                  "max", "ms"):
            d[a] = getattr(self, a)
        return d

    @classmethod
    def from_dict(cls, d):
        n = cls(d["type"], d["id"], d.get("x", 0), d.get("y", 0))
        for a in ("ctrl", "create", "ctype", "px", "py", "pw", "ph", "text",
                  "bg", "font", "show", "bold", "use_signal_data", "fit",
                  "target_ctrl", "signal", "exit_name", "param_name",
                  "flow_target", "flow_scene",
                  "var_name", "var_type", "var_value", "set_mode", "value",
                  "scope", "op", "lit_text", "lit_num", "lit_bool",
                  "file_path", "toast_text", "toast_corner", "toast_ms",
                  "max", "ms"):
            if a in d:
                setattr(n, a, d[a])
        if n.type == M_CTRL:
            n.sync_ctrl_inputs()
        return n

    def sync_ctrl_inputs(self):
        """控件:确保 [exec, value(文字)] 两个输入口(旧蓝图只有 exec 时补齐)。"""
        if self.type != M_CTRL:
            return
        if len(self.inputs) < 2 or self.inputs[1].kind != 'value':
            self.inputs = [Pin('exec', ''), Pin('value', '文字')]


class MENUScene:
    def __init__(self, title=None):
        self.title = title or "菜单"
        self.mode = 0             # 0 modal(独占) 1 overlay(叠加)
        self.bg = ""             # 独占菜单背景图(相对项目根)
        self.nodes = []
        self.links = []
        self.nid = 1
        self.lid = 1

    # ---- 节点/连线 ----------------
    def node(self, nid):
        for n in self.nodes:
            if n.id == nid:
                return n
        return None

    def new_node(self, ntype, x=0, y=0):
        n = MenuNode(ntype, self.nid, x, y)
        self.nid += 1
        self.nodes.append(n)
        return n

    def delete_node(self, nid):
        n = self.node(nid)
        if n is not None and n.type == M_START:
            return                    # 开场起点不可删除
        self.links = [l for l in self.links
                      if l.from_node != nid and l.to_node != nid]
        self.nodes = [n for n in self.nodes if n.id != nid]

    def exec_children(self, node):
        """从节点 exec 输出可达的下游节点(按连线顺序)。"""
        out = []
        for l in self.links:
            if l.from_node == node.id and l.from_pin == 0:
                c = self.node(l.to_node)
                if c is not None:
                    out.append(c)
        return out

    def exec_parents(self, node):
        """连到节点 exec 输入的父节点。"""
        out = []
        for l in self.links:
            if l.to_node == node.id and l.to_pin == 0:
                p = self.node(l.from_node)
                if p is not None:
                    out.append(p)
        return out

    # ---- 汇总 ----------------
    def starts(self):
        return [n for n in self.nodes if n.type == M_START]

    def clicks(self):
        return [n for n in self.nodes if n.type == M_CLICK]

    def signals(self):
        return [n for n in self.nodes if n.type == M_SIGNAL]

    def exits(self):
        return [n for n in self.nodes if n.type == M_EXIT]

    def ctrl_nodes(self):
        return [n for n in self.nodes if n.type == M_CTRL]

    def input_ctrls(self):
        """输入框控件(ctype==4)且已命名。按场景内顺序排列。"""
        return [n for n in self.ctrl_nodes() if n.ctype == 4 and n.ctrl]

    def param_nodes(self):
        """数据入口节点(有参数名),按添加顺序去重保序。"""
        out, seen = [], set()
        for n in self.nodes:
            if n.type == M_IN and n.param_name and n.param_name not in seen:
                seen.add(n.param_name)
                out.append(n)
        return out

    def param_names(self):
        return [n.param_name for n in self.param_nodes()]

    def find_value_source(self, node, in_idx):
        """连到节点第 in_idx 个 value 输入的来源节点(数据源)。"""
        for l in self.links:
            if l.to_node == node.id and l.to_pin == in_idx:
                return self.node(l.from_node)
        return None

    def find_value_source_with_pin(self, node, in_idx):
        """同 find_value_source,额外返回来源的输出引脚序号(供多输出口节点定位)。"""
        for l in self.links:
            if l.to_node == node.id and l.to_pin == in_idx:
                return self.node(l.from_node), l.from_pin
        return None, None

    def find_value_link(self, node, in_idx):
        for l in self.links:
            if l.to_node == node.id and l.to_pin == in_idx:
                return l
        return None

    def ctrl_names(self):
        """场景内所有控件名(去重保序)。"""
        names, seen = [], set()
        for n in self.ctrl_nodes():
            if n.ctrl and n.ctrl not in seen:
                seen.add(n.ctrl)
                names.append(n.ctrl)
        return names

    def exit_names(self):
        return [n.exit_name for n in self.exits()]

    def duplicate_exit_names(self):
        """重名的退出节点名(去重后仍有多个)。"""
        from collections import Counter
        c = Counter(n.exit_name for n in self.exits())
        return [k for k, v in c.items() if v > 1]

    def reachable_to(self, start_nodes, target_type):
        """从起点节点沿 exec 链能否到达指定类型节点。"""
        seen = set()
        stack = list(start_nodes)
        while stack:
            n = stack.pop()
            if n is None or n.id in seen:
                continue
            seen.add(n.id)
            if n.type == target_type:
                return True
            stack.extend(self.exec_children(n))
        return False

    def has_exit_path(self):
        """独占菜单是否有可达退出路径(开场链或点击起点链能到达 M_EXIT)。"""
        return self.reachable_to(self.starts() + self.clicks(), M_EXIT)


def menu_to_dict(m):
    return {"title": m.title, "mode": m.mode, "bg": m.bg,
            "nid": m.nid, "lid": m.lid,
            "nodes": [n.to_dict() for n in m.nodes],
            "links": [link_to_dict(l) for l in m.links]}


def menu_from_dict(d):
    m = MENUScene(d.get("title", "菜单"))
    m.mode = d.get("mode", 0)
    m.bg = d.get("bg", "")
    m.nid = d.get("nid", 1)
    m.lid = d.get("lid", 1)
    m.nodes = [MenuNode.from_dict(n) for n in d.get("nodes", [])]
    m.links = [link_from_dict(l) for l in d.get("links", [])]
    return m


def menu_defaults(title=None):
    """新建默认菜单:开场起点 → 一个按钮控件 → 退出。"""
    m = MENUScene(title or "菜单")
    start = m.new_node(M_START, 60, 200)
    btn = m.new_node(M_CTRL, 320, 200)
    btn.ctrl = "按钮 1"
    btn.ctype = 0
    btn.px, btn.py = 380, 240
    btn.pw, btn.ph = 200, 60
    btn.text = "开始"
    btn.bg = "#3377dd"
    m.links.append(Link(m.lid, start.id, 0, btn.id, 0))
    m.lid += 1
    return m
