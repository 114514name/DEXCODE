"""galide.model — GAL 编辑器数据模型。

字段、块类型、默认值与 C 版 tools/galedit.cpp 完全一致,确保 .galscene 兼容。
"""

# ---- 常量(与 C 版 #define 对齐) ----
MAX_SPR = 4
MAX_CHOICES = 4
MAX_BLOCKS = 384
MAX_SCENES = 32
MAX_CHARS = 32
MAX_STATES = 10

# ---- 块类型(与 C 版 BlockType 对齐) ----
BL_SPEAK = 0
BL_BG = 1
BL_SPRITE = 2
BL_AUDIO = 3
BL_WAIT = 4
BL_CLEAR = 5
BL_CHOICE = 6
BL_OPTION = 7
BL_IF = 8
BL_CODE = 9
BL_END = 10

# 块类型显示名(用于 UI)
BLOCK_NAMES = {
    BL_SPEAK: "说话",
    BL_BG: "背景",
    BL_SPRITE: "立绘",
    BL_AUDIO: "音频",
    BL_WAIT: "等待",
    BL_CLEAR: "清空文本",
    BL_CHOICE: "选项",
    BL_OPTION: "选项项",
    BL_IF: "如果",
    BL_CODE: "代码",
    BL_END: "结束",
}


class Block:
    """指令块。块数组为深度优先序(树用 depth 表示):C 形块(选项/选项项/如果)
    的子块 = 其后 depth 更大的连续块。"""

    def __init__(self, type_=BL_SPEAK, depth=0):
        self.type = type_
        self.depth = depth
        # 说话
        self.speaker = ""
        self.text = ""
        self.style_avatar = 0      # 有头像对话框
        self.style_name = 1        # 显示人名
        self.text_pos = 0          # 0中 1左 2右 3偏左 4偏右
        self.auto_fit = 0          # 对话框自适应
        # 背景
        self.useBgImg = 1
        self.bgColor = 0x101018
        self.bg = ""
        # 立绘
        self.charIdx = -1          # 人物集索引;-1=直接路径
        self.stateIdx = 0
        self.sprPath = ""          # 直接路径
        self.spr_layer = 0
        self.spr_show = 1
        self.spr_pos = 0
        self.spr_autofit = 0
        self.spr_anim = 0
        # 音频
        self.audio_type = 0        # 0BGM 1SE 2停止BGM 3音量
        self.audio_path = ""
        self.volume = 80
        # 等待
        self.wait_type = 0         # 0点击 1毫秒
        self.wait_ms = 0
        # 选项(CHOICE)
        self.choice_text = ""
        # 选项项(OPTION)
        self.option_text = ""
        # 如果(IF)
        self.cond = ""
        # 代码(CODE)
        self.code_file = ""
        self.code_cond = ""


class Character:
    """人物集:名字 + 多张状态图 + 状态名。"""

    def __init__(self):
        self.name = ""
        self.states = [""] * MAX_STATES   # 状态图路径
        self.stateNames = [""] * MAX_STATES
        self.nStates = 0
        self.cur = 0


class Scene:
    def __init__(self):
        self.title = "镜头"
        self.bgDir = ""
        self.bg = ""
        self.useBgImg = 1
        self.bgColor = 0x101018
        self.spr = [""] * MAX_SPR
        self.blocks = []
        self.nBlocks = 0
        self.cur = 0


class Project:
    def __init__(self):
        self.title = "新项目"
        self.resDir = ""
        self.chars = []
        self.nChars = 0
        self.scenes = []
        self.nScenes = 0
        self.cur = 0


# ---- 默认值(与 C 版 block_defaults/scene_defaults/project_defaults 对齐) ----

def block_defaults(type_=BL_SPEAK, depth=0):
    b = Block(type_, depth)
    b.style_name = 1
    b.bgColor = 0x101018
    b.useBgImg = 1
    b.charIdx = -1
    b.stateIdx = 0
    b.spr_layer = 0
    b.spr_show = 1
    b.spr_pos = 0
    b.wait_type = 0
    b.volume = 80
    return b


def scene_defaults(title=None):
    s = Scene()
    s.title = title if title is not None else "镜头"
    s.bgColor = 0x101018
    s.useBgImg = 1
    s.blocks = [block_defaults(BL_SPEAK, 0)]
    s.nBlocks = 1
    s.cur = 0
    return s


def project_defaults():
    p = Project()
    p.title = "新项目"
    p.scenes = [scene_defaults("镜头 1")]
    p.nScenes = 1
    p.cur = 0
    return p


# ---- 块树工具(与 C 版对齐) ----

def block_end(sc: Scene, i: int) -> int:
    """返回块 i 子树结束后的索引(第一个 depth <= blocks[i].depth 的位置)。"""
    if i < 0 or i >= sc.nBlocks:
        return i + 1
    d = sc.blocks[i].depth
    j = i + 1
    while j < sc.nBlocks and sc.blocks[j].depth > d:
        j += 1
    return j


def block_parent(sc: Scene, i: int) -> int:
    d = sc.blocks[i].depth
    for j in range(i - 1, -1, -1):
        if sc.blocks[j].depth < d:
            return j
    return -1


def prev_sibling(sc: Scene, i: int) -> int:
    d = sc.blocks[i].depth
    for j in range(i - 1, -1, -1):
        if sc.blocks[j].depth < d:
            return -1
        if sc.blocks[j].depth == d:
            return j
    return -1


def next_sibling(sc: Scene, i: int) -> int:
    d = sc.blocks[i].depth
    s = block_end(sc, i)
    for j in range(s, sc.nBlocks):
        if sc.blocks[j].depth < d:
            return -1
        if sc.blocks[j].depth == d:
            return j
    return -1


def _subtree(sc: Scene, i: int):
    """提取块 i 的整棵子树(含自身),返回 (块列表, 结束位置)。"""
    e = block_end(sc, i)
    return sc.blocks[i:e], e


def block_move(sc: Scene, i: int, target: int, where: int):
    """移动块 i 的整棵子树:0=插到 target 前(同父) 1=插到 target 后(同父) 2=作为 target 子块。"""
    if not (0 <= i < sc.nBlocks) or not (0 <= target < sc.nBlocks) or i == target:
        return
    sub, e = _subtree(sc, i)
    rest = sc.blocks[:i] + sc.blocks[e:]
    # 重新计算 rest 中 target 的新索引
    if target >= e:
        nt = target - (e - i)
    else:
        nt = target
    if where == 2:
        # 作为 target 子块:子树整体下移一层(depth 相对偏移保持不变)
        td = rest[nt].depth + 1
        base = sub[0].depth
        newb = []
        for b in sub:
            nb = _clone_block(b)
            nb.depth = td + (b.depth - base)
            newb.append(nb)
        insert_at = nt + 1
    else:
        newb = [_clone_block(b) for b in sub]
        insert_at = nt if where == 0 else nt + 1
    sc.blocks = rest[:insert_at] + newb + rest[insert_at:]
    sc.nBlocks = len(sc.blocks)
    # 修正选中
    sc.cur = min(max(sc.cur, 0), sc.nBlocks - 1)


def _clone_block(b: Block) -> Block:
    import copy
    return copy.deepcopy(b)


def insert_block(sc: Scene, type_: int, depth: int = 0) -> int:
    """在选中块之后插入新块(含子树占位),返回新块索引。"""
    if sc.nBlocks >= MAX_BLOCKS:
        return -1
    b = block_defaults(type_, depth)
    at = sc.cur + 1
    # 插入到当前块子树之后(避免插进 C 形块的子块区)
    if sc.blocks[sc.cur].type in (BL_CHOICE, BL_OPTION, BL_IF):
        at = block_end(sc, sc.cur)
    sc.blocks.insert(at, b)
    sc.nBlocks = len(sc.blocks)
    sc.cur = at
    return at


def delete_block(sc: Scene, i: int):
    """删除块 i 的整棵子树。"""
    if sc.nBlocks <= 0:
        return
    e = block_end(sc, i)
    del sc.blocks[i:e]
    sc.nBlocks = len(sc.blocks)
    if sc.nBlocks == 0:
        sc.blocks = [block_defaults(BL_SPEAK, 0)]
        sc.nBlocks = 1
    sc.cur = min(sc.cur, sc.nBlocks - 1)
