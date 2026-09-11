"""galide.sceneio — .galscene 场景文件读写。

格式与 C 版 tools/galedit.cpp 的 scene_save/scene_load 完全一致(UTF-8, INI-like),
并支持旧格式([shot] 多镜头)迁移。
"""

import os

from .model import (Project, Scene, Character, Block, block_defaults,
                    scene_defaults, project_defaults,
                    MAX_SCENES, MAX_CHARS, MAX_STATES, MAX_SPR, MAX_CHOICES,
                    MAX_BLOCKS, BL_SPEAK, BL_BG, BL_SPRITE, BL_AUDIO,
                    BL_WAIT, BL_CLEAR, BL_CHOICE, BL_OPTION, BL_IF, BL_CODE,
                    BL_END)


# ---------------- 序列化(保存) ----------------

def _clean_newlines(s: str) -> str:
    """block_save 中把 speaker/text/option_text 的换行替换为空格。"""
    return s.replace("\n", " ").replace("\r", " ")


def _block_save(b: Block) -> list:
    lines = ["[block]", f"type={b.type}", f"depth={b.depth}"]
    if b.type == BL_SPEAK:
        lines.append(f"speaker={b.speaker}")
        lines.append(f"text={_clean_newlines(b.text)}")
    elif b.type == BL_CHOICE:
        # C 版把选项文本存到 text 字段;这里从 choice_text 写出以兼容
        lines.append(f"speaker={b.speaker}")
        lines.append(f"text={_clean_newlines(b.choice_text)}")
    if b.type == BL_SPEAK:
        lines.append(f"avatar={b.style_avatar}")
        lines.append(f"name={b.style_name}")
        lines.append(f"textpos={b.text_pos}")
        lines.append(f"autofit={b.auto_fit}")
    if b.type == BL_BG:
        lines.append(f"bg={b.bg}")
        lines.append(f"usebgimg={b.useBgImg}")
        lines.append(f"bgcolor={b.bgColor}")
    if b.type == BL_SPRITE:
        lines.append(f"charidx={b.charIdx}")
        lines.append(f"stateidx={b.stateIdx}")
        lines.append(f"sprpath={b.sprPath}")
        lines.append(f"sprlayer={b.spr_layer}")
        lines.append(f"sprshow={b.spr_show}")
        lines.append(f"sprpos={b.spr_pos}")
        lines.append(f"spr_autofit={b.spr_autofit}")
        lines.append(f"spranim={b.spr_anim}")
    if b.type == BL_AUDIO:
        lines.append(f"audio_type={b.audio_type}")
        lines.append(f"volume={b.volume}")
        lines.append(f"audio_path={b.audio_path}")
    if b.type == BL_WAIT:
        lines.append(f"wait_type={b.wait_type}")
        lines.append(f"wait_ms={b.wait_ms}")
    if b.type == BL_OPTION:
        lines.append(f"option_text={_clean_newlines(b.option_text)}")
    if b.type == BL_IF:
        lines.append(f"cond={b.cond}")
    if b.type == BL_CODE:
        lines.append(f"codefile={b.code_file}")
        lines.append(f"codecond={b.code_cond}")
    lines.append("[/block]")
    return lines


def save_scene(path: str, p: Project):
    """保存项目到 .galscene(UTF-8)。"""
    out = []
    out.append(f"title={p.title}")
    out.append(f"res={p.resDir}")
    for ch in p.chars[:MAX_CHARS]:
        out.append("[char]")
        out.append(f"cname={ch.name}")
        out.append(f"nstates={ch.nStates}")
        for s in range(ch.nStates):
            out.append(f"stname={ch.stateNames[s]}")
            out.append(f"stimg={ch.states[s]}")
        out.append("[/char]")
    for sc in p.scenes[:MAX_SCENES]:
        out.append("[scene]")
        out.append(f"stitle={sc.title}")
        out.append(f"bgdir={sc.bgDir}")
        out.append(f"bg={sc.bg}")
        out.append(f"bgcolor={sc.bgColor}")
        out.append(f"usebgimg={sc.useBgImg}")
        for k in range(MAX_SPR):
            out.append(f"spr{k}={sc.spr[k]}")
        for b in sc.blocks[:MAX_BLOCKS]:
            out.extend(_block_save(b))
        out.append("[/scene]")
    text = "\n".join(out) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# ---------------- 解析(加载) ----------------

def _split_kv(line: str):
    if "=" not in line:
        return None, None
    k, v = line.split("=", 1)
    return k.strip(), v


def _int(v, default=0):
    try:
        return int(v)
    except Exception:
        return default


def load_scene(path: str) -> Project:
    """加载 .galscene,返回 Project;失败返回 None。"""
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8-sig") as f:
        raw = f.read()
    lines = raw.splitlines()

    # 检测旧格式
    old_format = any(ln.strip() == "[shot]" for ln in lines)

    np = project_defaults()
    np.title = ""
    np.scenes = []
    np.nScenes = 0
    np.chars = []
    np.nChars = 0

    sc = None
    b = None
    ch = None
    btype_override = None

    for line in lines:
        line = line.strip()
        if line == "[char]":
            ch = Character()
            continue
        if line == "[/char]":
            if ch is not None and np.nChars < MAX_CHARS:
                np.chars.append(ch)
                np.nChars = len(np.chars)
            ch = None
            continue
        if line == "[scene]":
            sc = Scene()
            sc.bgColor = 0x101018
            sc.useBgImg = 1
            b = None
            continue
        if line == "[/scene]":
            if sc is not None:
                _finalize_scene(sc)
                np.scenes.append(sc)
                np.nScenes = len(np.scenes)
            sc = None
            b = None
            continue
        if line == "[block]":
            b = block_defaults(BL_SPEAK, 0)
            continue
        if line == "[/block]":
            if sc is not None and b is not None:
                sc.blocks.append(b)
            b = None
            continue

        k, v = _split_kv(line)
        if k is None:
            continue

        if k == "title" and sc is None and b is None:
            np.title = v
        elif k == "res":
            np.resDir = v
        elif ch is not None:
            if k == "cname":
                ch.name = v
            elif k == "stname":
                if ch.nStates < MAX_STATES:
                    ch.stateNames[ch.nStates] = v
            elif k == "stimg":
                if ch.nStates < MAX_STATES:
                    ch.states[ch.nStates] = v
                    ch.nStates += 1
        elif sc is not None and b is None:
            if k == "stitle":
                sc.title = v
            elif k == "bgdir":
                sc.bgDir = v
            elif k == "bg":
                sc.bg = v
            elif k == "bgcolor":
                sc.bgColor = _int(v, sc.bgColor)
            elif k == "usebgimg":
                sc.useBgImg = _int(v, sc.useBgImg)
            elif k.startswith("spr") and len(k) > 3:
                idx = _int(k[3:])
                if 0 <= idx < MAX_SPR:
                    sc.spr[idx] = v
        elif sc is not None and b is not None:
            if k == "type":
                t = _int(v)
                if t < BL_SPEAK or t > BL_END:
                    t = BL_SPEAK
                b.type = t
            elif k == "depth":
                d = _int(v)
                b.depth = max(0, min(12, d))
            elif k == "speaker":
                b.speaker = v
            elif k == "text":
                b.text = v
                if b.type == BL_CHOICE:
                    b.choice_text = v
            elif k == "avatar":
                b.style_avatar = _int(v)
            elif k == "name":
                b.style_name = _int(v)
            elif k == "textpos":
                b.text_pos = _int(v)
            elif k == "autofit":
                b.auto_fit = _int(v)
            elif k == "charidx":
                b.charIdx = _int(v, -1)
            elif k == "stateidx":
                b.stateIdx = _int(v)
            elif k == "sprpath":
                b.sprPath = v
            elif k == "sprlayer":
                b.spr_layer = _int(v)
            elif k == "sprshow":
                b.spr_show = _int(v)
            elif k == "sprpos":
                b.spr_pos = _int(v)
            elif k == "spr_autofit":
                b.spr_autofit = _int(v)
            elif k == "spranim":
                b.spr_anim = _int(v)
            elif k == "audio_type":
                b.audio_type = _int(v)
            elif k == "volume":
                b.volume = _int(v, 80)
            elif k == "audio_path":
                b.audio_path = v
            elif k == "wait_type":
                b.wait_type = _int(v)
            elif k == "wait_ms":
                b.wait_ms = _int(v)
            elif k == "option_text":
                b.option_text = v
            elif k == "cond":
                b.cond = v
            elif k == "codefile":
                b.code_file = v
            elif k == "codecond":
                b.code_cond = v

    # 收尾:未闭合的块/场景
    if sc is not None and b is not None:
        sc.blocks.append(b)
    if sc is not None:
        _finalize_scene(sc)
        np.scenes.append(sc)
        np.nScenes = len(np.scenes)
    if ch is not None and np.nChars < MAX_CHARS:
        np.chars.append(ch)
        np.nChars = len(np.chars)

    # 旧格式迁移
    if old_format and np.nScenes == 0:
        np.nScenes = 1
        mig = Scene()
        mig.bgColor = 0x101018
        mig.useBgImg = 1
        _migrate_old(lines, mig)
        _finalize_scene(mig)
        np.scenes = [mig]
        np.nScenes = 1

    if not np.scenes:
        np.scenes = [scene_defaults("镜头 1")]
        np.nScenes = 1
    if not np.title:
        np.title = os.path.splitext(os.path.basename(path))[0]
    if np.cur < 0 or np.cur >= np.nScenes:
        np.cur = 0
    return np


def _finalize_scene(sc: Scene):
    if not sc.blocks:
        sc.blocks = [block_defaults(BL_SPEAK, 0)]
    if len(sc.blocks) > MAX_BLOCKS:
        sc.blocks = sc.blocks[:MAX_BLOCKS]
    sc.nBlocks = len(sc.blocks)
    if sc.cur < 0 or sc.cur >= sc.nBlocks:
        sc.cur = 0


# ---------------- 旧格式迁移([shot]) ----------------

def _migrate_old(lines, sc: Scene):
    """逐 shot 转成 背景/立绘/说话/选项 指令块(与 C 版 migrate_old 对齐)。"""
    sc.blocks = []
    shot_no = 0
    in_shot = False

    tbg = ""
    tspr = [""] * MAX_SPR
    tspk = ""
    ttxt = ""
    tch = [""] * MAX_CHOICES
    tnc = 0
    tuse = 1
    tcol = 0x101018

    def reset():
        nonlocal tbg, tspr, tspk, ttxt, tch, tnc, tuse, tcol
        tbg = ""
        tspr = [""] * MAX_SPR
        tspk = ""
        ttxt = ""
        tch = [""] * MAX_CHOICES
        tnc = 0
        tuse = 1
        tcol = 0x101018

    def commit():
        nonlocal shot_no
        if shot_no == 0:
            sc.bg = tbg
            sc.useBgImg = tuse
            sc.bgColor = tcol
            sc.spr = list(tspr)
        elif tbg and tbg != sc.bg:
            b = block_defaults(BL_BG, 0)
            b.bg = tbg
            b.useBgImg = tuse
            b.bgColor = tcol
            sc.blocks.append(b)
        for i in range(MAX_SPR):
            if tspr[i]:
                b = block_defaults(BL_SPRITE, 0)
                b.spr_layer = i
                b.spr_show = 1
                b.spr_autofit = 1
                b.charIdx = -1
                b.sprPath = tspr[i]
                sc.blocks.append(b)
        if tnc > 0:
            if ttxt:
                b = block_defaults(BL_SPEAK, 0)
                b.speaker = tspk
                b.text = ttxt
                sc.blocks.append(b)
            ch = block_defaults(BL_CHOICE, 0)
            ch.choice_text = ttxt
            sc.blocks.append(ch)
            for c2 in range(min(tnc, MAX_CHOICES)):
                op = block_defaults(BL_OPTION, 1)
                op.option_text = tch[c2]
                sc.blocks.append(op)
        elif ttxt:
            b = block_defaults(BL_SPEAK, 0)
            b.speaker = tspk
            b.text = ttxt
            sc.blocks.append(b)
        shot_no += 1
        reset()

    reset()
    for line in lines:
        line = line.strip()
        if line == "[shot]":
            in_shot = True
            continue
        if line == "[/shot]":
            if in_shot:
                commit()
            in_shot = False
            continue
        if not in_shot:
            k, v = _split_kv(line)
            if k is None:
                continue
            if line.startswith("title="):
                sc.title = line.split("=", 1)[1]
            elif k == "bg":
                tbg = v          # 全局默认背景(首个 commit 用)
            elif k == "bgcolor":
                tcol = _int(v, tcol)
            elif k == "usebgimg":
                tuse = _int(v, tuse)
            continue
        k, v = _split_kv(line)
        if k is None:
            continue
        if k == "bg":
            tbg = v
        elif k == "bgcolor":
            tcol = _int(v, tcol)
        elif k == "usebgimg":
            tuse = _int(v, tuse)
        elif k.startswith("spr"):
            idx = _int(k[3:])
            if 0 <= idx < MAX_SPR:
                tspr[idx] = v
        elif k == "speaker":
            tspk = v
        elif k == "text":
            ttxt = v
        elif k.startswith("choice"):
            idx = _int(k[6:])
            if 0 <= idx < MAX_CHOICES:
                tch[idx] = v
                if idx + 1 > tnc:
                    tnc = idx + 1
    if in_shot:
        commit()
    if not sc.blocks:
        sc.blocks = [block_defaults(BL_SPEAK, 0)]
    sc.nBlocks = len(sc.blocks)
    sc.cur = 0
