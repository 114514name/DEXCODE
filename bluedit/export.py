"""bluedit.export — 蓝图节点图 → DexLang 代码。

每个镜头一个函数;从入口沿 exec 连线遍历,分支(选项/if)展开为嵌套代码。
value 流(GETVAR/LOGIC/MATH)以内联表达式生成。变量用 gal 引擎存储(全局/局部)。
"""

import os

from .model import (N_ENTRY, N_EXIT, N_SPEAK, N_LONG, N_SPRITE, N_BG,
                    N_CHOICE, N_CODE, N_DEFVAR, N_SETVAR, N_IF, N_GETVAR,
                    N_LOGIC, N_MATH, N_FLOW, N_FX_ADD, N_FX_OFF, N_FX_CLEAR,
                    N_MENU, N_TEXT, N_SAY, N_INPUT, N_RANDOM, N_WAIT,
                    N_DOUT, N_DREF, N_TEXTLIT, N_NUMLIT, N_BOLLIT,
                    N_BGM, N_SE, N_SOUND_STOP, N_SOUND_SWAP,
                    N_BG_TRANS, N_SCENE_TRANS, N_TOAST,
                    N_SAVE, N_LOAD, N_FILE_WRITE, N_FILE_READ,
                    N_CHECKPOINT, N_JUMP,
                    FX_BITS, MAX_SPR,
                    M_START, M_CTRL, M_CLICK, M_SIGNAL, M_EXIT, M_IN, M_JUMP)
from . import vars as vars_mod

_indent = 0
_choice_seq = 0

# 最近一次导出失败的原因(None 表示上次导出成功)。
# 原先 write_scene_dex/export_project 用裸 `except Exception` 把异常吞成 False/"",
# 调用方只能弹「导出失败。」,开发者与用户都拿不到任何诊断信息。
last_error = None


def _esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _text_speed(node):
    """逐字间隔:优先节点单独设置,否则用全局默认(0 表示不输出)。"""
    sp = getattr(node, "text_speed", 0)
    if sp:
        return sp
    from . import settings as settings_mod
    return settings_mod.get_block_defaults().get("text_speed", 0)


def _line(out, s=""):
    out.append("    " * _indent + s)


def _var_type(name, project, scene_idx, default=0):
    for sc in project.scenes:
        for n in sc.nodes:
            if n.type == N_DEFVAR and n.var_name == name:
                return n.var_type
    for sc in project.scenes:
        for n in sc.nodes:
            if n.type == N_SETVAR and n.var_name == name:
                return n.var_type
    # 菜单场景里的变量节点(菜单内操作用全局变量)
    for mn in getattr(project, "menus", []) or []:
        for n in mn.nodes:
            if n.type in (N_DEFVAR, N_SETVAR) and n.var_name == name:
                return n.var_type
    return default


def _var_ref(name, project, scene_idx, scope=0):
    """生成读取变量的 DexLang 表达式(按类型)。"""
    vt = _var_type(name, project, scene_idx)
    sname = vars_mod.storage_name(name, scene_idx, scope)
    if vt == 1:
        return f'gal_atof(gal_var_get("{sname}"))'
    if vt == 2:
        return f'gal_var_get("{sname}")'
    return f'gal_var_num("{sname}")'


def _literal(pin):
    lit = pin.literal
    if lit in ("true", "false"):
        return "1" if lit == "true" else "0"
    try:
        float(lit)
        return lit
    except Exception:
        return f'"{_esc(lit)}"'


def _value_expr(scene, node, pin_idx, project, scene_idx):
    """生成 value 引脚表达式(优先取连线来源,否则取引脚配置)。"""
    src, src_pin = scene.find_value_source_with_pin(node, pin_idx)
    if src is not None:
        if src.type == N_GETVAR:
            return _var_ref(src.var_name, project, scene_idx)
        if src.type in (N_LOGIC, N_MATH):
            return _math_expr(scene, src, project, scene_idx)
        if src.type == N_TEXT:
            return _text_cat_expr(scene, src, project, scene_idx)
        if src.type == N_INPUT:
            return _menu_input_expr(src)
        if src.type == N_RANDOM:
            return f"gal_rand({src.max or 100})"
        if src.type == N_DREF:
            return _scene_data_expr(project, src)
        if src.type == N_MENU:
            return _menu_data_expr(project, src, src_pin)
        if src.type == N_FILE_READ:
            return f'gal_file_read("{_esc(src.file_path)}")'
        if src.type == N_TEXTLIT:
            return f'"{_esc(src.lit_text)}"'
        if src.type == N_NUMLIT:
            return src.lit_num or "0"
        if src.type == N_BOLLIT:
            return "1" if src.lit_bool else "0"
        return "0"
    pin = node.inputs[pin_idx]
    if pin.source == "variable":
        vt = _var_type(pin.variable, project, scene_idx)
        sname = vars_mod.storage_name(pin.variable, scene_idx, 0)
        if vt == 1:
            return f'gal_atof(gal_var_get("{sname}"))'
        if vt == 2:
            return f'gal_var_get("{sname}")'
        return f'gal_var_num("{sname}")'
    return _literal(pin)


def _text_cat_expr(scene, node, project, scene_idx):
    """文本拼接:遍历全部输入段(value 连线或旧的字面/变量配置),空段跳过。"""
    parts = []
    for i in range(len(node.inputs)):
        src = scene.find_value_source(node, i)
        if src is not None:
            parts.append(_value_expr(scene, node, i, project, scene_idx))
            continue
        pin = node.inputs[i]
        lit = (pin.literal or "").strip()
        if pin.source == "variable" and pin.variable:
            vt = _var_type(pin.variable, project, scene_idx)
            sname = vars_mod.storage_name(pin.variable, scene_idx, 0)
            parts.append(f'gal_var_get("{sname}")' if vt == 2
                         else f'gal_itoa(gal_var_num("{sname}"))')
        elif lit:
            parts.append(_literal(pin))
    if not parts:
        return '""'
    return "(" + " + ".join(parts) + ")"


def _menu_input_expr(node):
    return f'gal_menu_get_text("{_esc(node.ctrl or "")}")'


def _menu_data_expr(project, node, src_pin):
    """菜单块的数据输出口(src_pin 是第几个输出口)→ 对应输入框控件内容。
    输出口顺序:先 exec(退出点们),再 value(输入框们)。"""
    mi = node.menu_idx
    if not (0 <= mi < len(project.menus)):
        return '""'
    m = project.menus[mi]
    exs = m.exits()
    exec_n = len(exs) if exs else 1
    ctrls = [c.ctrl for c in m.input_ctrls()]
    value_idx = src_pin - exec_n
    if 0 <= value_idx < len(ctrls):
        return f'gal_menu_get_text("{_esc(ctrls[value_idx])}")'
    return '""'


def _scene_data_key(scene_idx, name):
    """数据输出点存储名(引擎全局变量命名空间,与流程/用户变量分开)。"""
    return f"_scout_{scene_idx}_{name}"


def _scene_data_expr(project, node):
    si = node.out_scene
    if not (0 <= si < len(project.scenes)):
        return '""'
    name = node.out_name or "?"
    return f'gal_var_get("{_esc(_scene_data_key(si, name))}")'


def _value_is_string(scene, node, pin_idx, project, scene_idx):
    """判断 value 引脚表达式是否为字符串(否则是数字,需转字符串存储)。"""
    src = scene.find_value_source(node, pin_idx)
    if src is not None:
        if src.type in (N_TEXT, N_INPUT, N_DREF, N_MENU, N_TEXTLIT):
            return True
        if src.type in (N_NUMLIT, N_BOLLIT):
            return False
        if src.type == N_GETVAR:
            return _var_type(src.var_name, project, scene_idx) == 2
        return False   # RANDOM/LOGIC/MATH → 数字
    if pin_idx < len(node.inputs):
        pin = node.inputs[pin_idx]
        if pin.source == "variable":
            return _var_type(pin.variable, project, scene_idx) == 2
        lit = (pin.literal or "").strip()
        if lit in ("true", "false"):
            return False
        try:
            float(lit)
            return False
        except Exception:
            return True
    return False


def _math_expr(scene, node, project, scene_idx):
    a = _value_expr(scene, node, 0, project, scene_idx)
    b = _value_expr(scene, node, 1, project, scene_idx)
    if node.type == N_MATH and node.op == "^":
        return f"({a} ** {b})" if False else f"({a} ^ {b})"
    return f"({a} {node.op} {b})"


# ---------------- exec 链导出 ----------------
def _emit_exec(scene, node, project, scene_idx, out, visited, all_visited, stop=None):
    """从节点沿 exec 连线递归导出代码。visited:当前分支已访问(防环)。
    stop:若指定,遇到该节点时不输出(用于 if 分支汇合去重)。"""
    if node is None:
        return
    if stop is not None and node.id == stop:
        return
    if node.id in all_visited and node.type not in (N_ENTRY,):
        pass
    all_visited.add(node.id)
    if node.id in visited:
        _line(out, "// 循环回边已截断")
        return
    visited = visited | {node.id}

    t = node.type
    if t == N_EXIT:
        _emit_flow(scene, node, project, scene_idx, out, 0)  # EXIT = 跳下一镜头
        return
    if t == N_FLOW:
        _emit_flow(scene, node, project, scene_idx, out, node.flow_target)
        return
    if t == N_SCENE_TRANS:
        _emit_scene_trans(scene, node, project, scene_idx, out)
        return
    if t == N_JUMP:
        _emit_jump(project, node, out)
        return
    if t == N_CHOICE:
        _emit_choice(scene, node, project, scene_idx, out, visited, all_visited)
        return
    if t == N_IF:
        _emit_if(scene, node, project, scene_idx, out, visited, all_visited)
        return
    if t == N_MENU and node.action == 0:
        # 独占菜单的输出口已在 _emit_menu 内部按 r 分支遍历,不再沿第 0 口重复
        _emit_menu(scene, node, project, scene_idx, out)
        return
    # 普通顺序节点
    _emit_node(scene, node, project, scene_idx, out)
    nxt = scene.find_exec_target(node, 0)
    if nxt is not None:
        _emit_exec(scene, nxt, project, scene_idx, out, visited, all_visited)
    else:
        _line(out, "// 链尾")


def _emit_flow(scene, node, project, scene_idx, out, target):
    n = len(project.scenes)
    if target == 2:                        # 结束游戏
        _line(out, "gal_close();")
        _line(out, "return;")
    elif target == 1 and 0 <= node.flow_scene < n:
        _line(out, f"scene_{node.flow_scene}();")
    elif scene_idx + 1 < n:
        _line(out, f"scene_{scene_idx + 1}();")
    else:
        _line(out, "gal_close();")
        _line(out, "return;")


def _emit_scene_trans(scene, node, project, scene_idx, out):
    """特效镜头切换:淡出到黑 → 切到目标镜头(新镜头开头自动淡入) → 结束本函数。"""
    n = len(project.scenes)
    _line(out, f"gal_fade_out({node.scene_trans_ms or 300});")
    if node.flow_target == 1 and 0 <= node.flow_scene < n:
        _line(out, f"scene_{node.flow_scene}();")
        _line(out, "return;")
    elif scene_idx + 1 < n:
        _line(out, f"scene_{scene_idx + 1}();")
        _line(out, "return;")
    else:
        _line(out, "gal_close();")
        _line(out, "return;")


def _emit_jump(project, node, out):
    """跳转到存档点:读档恢复全部变量(含 _ckpt 位置)→ 按 _ckpt_scene 分派到对应场景,
    该场景开头再按 _ckpt 分支进入对应存档点之后继续。"""
    _line(out, f'gal_load_vars("{_esc(node.file_path)}");')
    _line(out, 'let _s = gal_var_num("_ckpt_scene");')
    n = len(project.scenes)
    for i in range(n):
        _line(out, f'{"else " if i else ""}if _s == {i} {{')
        global _indent
        _indent += 1
        _line(out, f"scene_{i}();")
        _indent -= 1
        _line(out, "}")
    _line(out, "return;")


def _emit_choice(scene, node, project, scene_idx, out, visited, all_visited):
    if node.choice_text:
        _line(out, f'gal_text_mode({node.text_mode});')
        sp = _text_speed(node)
        if sp:
            _line(out, f'gal_text_speed({sp});')
        _line(out, f'gal_text("{_esc(node.choice_text)}");')
    opts = node.options or [""]
    for i, o in enumerate(opts[:4]):
        _line(out, f'gal_set_choice({i}, "{_esc(o)}");')
    _line(out, "gal_show_choices();")
    _line(out, "wait_pick();")
    global _choice_seq
    _choice_seq += 1
    pv = f"_p{_choice_seq}"          # 唯一临时变量,避免同场景多个选项块重复声明
    _line(out, f"let {pv} = gal_picked();")
    _line(out, "gal_hide_choices();")
    _line(out, "gal_text_clear();")
    for i in range(len(opts[:4])):
        _line(out, f'{"else " if i else ""}if {pv} == {i} {{')
        global _indent
        _indent += 1
        nxt = scene.find_exec_target(node, i)
        if nxt is not None:
            _emit_exec(scene, nxt, project, scene_idx, out, visited, all_visited)
        _indent -= 1
        _line(out, "}")


def _emit_if(scene, node, project, scene_idx, out, visited, all_visited):
    if node.mode == 1 or scene.find_value_source(node, 1) is not None:
        cond = _value_expr(scene, node, 1, project, scene_idx)
    else:
        cond = node.cond if node.cond.strip() else "1"
    tn = scene.find_exec_target(node, 0)
    fn = scene.find_exec_target(node, 1)
    # 分支汇合(diamond):真假分支都到同一节点 → 各分支只输出到汇合点前,汇合点放到 if 后只一次
    common = tn.id if (tn is not None and fn is not None and tn.id == fn.id) else None
    _line(out, f"if {cond} {{")
    global _indent
    _indent += 1
    if tn is not None:
        _emit_exec(scene, tn, project, scene_idx, out, visited, all_visited, common)
    _indent -= 1
    _line(out, "} else {")
    _indent += 1
    if fn is not None:
        _emit_exec(scene, fn, project, scene_idx, out, visited, all_visited, common)
    _indent -= 1
    _line(out, "}")
    if common is not None:
        _emit_exec(scene, scene.node(common), project, scene_idx, out, set(), all_visited)


def _emit_node(scene, node, project, scene_idx, out):
    t = node.type
    if t == N_SPEAK:
        _emit_speak(scene, node, project, scene_idx, out)
    elif t == N_LONG:
        _emit_long(scene, node, project, scene_idx, out)
    elif t == N_SPRITE:
        _emit_sprite(scene, node, project, scene_idx, out)
    elif t == N_BG:
        _emit_bg(node, out)
    elif t == N_FX_ADD:
        _line(out, f"gal_fx_add({FX_BITS[node.fx]});")
        _line(out, f"gal_fx_strength({node.fx_strength});")
    elif t == N_FX_OFF:
        _line(out, f"gal_fx_remove({FX_BITS[node.fx]});")
    elif t == N_FX_CLEAR:
        _line(out, "gal_fx_clear();")
    elif t == N_SAY:
        _emit_say(scene, node, project, scene_idx, out)
    elif t == N_WAIT:
        _line(out, f"gal_wait({node.ms});")
    elif t == N_DOUT:
        key = _scene_data_key(scene_idx, node.out_name or "?")
        val = _value_expr(scene, node, 1, project, scene_idx)
        if _value_is_string(scene, node, 1, project, scene_idx):
            _line(out, f'gal_var_set("{_esc(key)}", {val});')
        else:
            _line(out, f'gal_var_set("{_esc(key)}", gal_itoa({val}));')
    elif t == N_MENU:
        _emit_menu(scene, node, project, scene_idx, out)
    elif t == N_CODE:
        _emit_code(node, out)
    elif t == N_BGM:
        if node.sound_vol != 80:
            _line(out, f"gal_volume({node.sound_vol});")
        _line(out, f'gal_play_bgm("{_esc(node.sound_path)}");')
    elif t == N_SE:
        if node.sound_vol != 80:
            _line(out, f"gal_volume({node.sound_vol});")
        _line(out, f'gal_play_se("{_esc(node.sound_path)}");')
    elif t == N_SOUND_STOP:
        if node.stop_target == 1:
            _line(out, "gal_stop_se();")
        elif node.stop_target == 2:
            _line(out, "gal_stop_sound();")
        else:
            _line(out, "gal_stop_bgm();")
    elif t == N_SOUND_SWAP:
        if node.sound_vol != 80:
            _line(out, f"gal_volume({node.sound_vol});")
        _line(out, f'gal_play_bgm("{_esc(node.sound_path)}");')
    elif t == N_BG_TRANS:
        _line(out, f"gal_bg_fit({node.bg_fit});")
        _line(out, f'gal_bg_trans("{_esc(node.bg_path)}", {node.bg_trans_ms or 400});')
    elif t == N_TOAST:
        _line(out, f'gal_toast("{_esc(node.toast_text)}", {getattr(node, "toast_corner", 3)}, {node.toast_ms or 1500});')
    elif t == N_SAVE:
        _line(out, f'gal_save_vars("{_esc(node.file_path)}");')
    elif t == N_LOAD:
        _line(out, f'gal_load_vars("{_esc(node.file_path)}");')
    elif t == N_FILE_WRITE:
        val = _value_expr(scene, node, 1, project, scene_idx)
        _line(out, f'gal_file_write("{_esc(node.file_path)}", {val});')
    elif t == N_CHECKPOINT:
        _line(out, f'gal_var_set("_ckpt", "{_esc(node.ck_name)}");')
        _line(out, f'gal_var_set("_ckpt_scene", gal_itoa({scene_idx}));')
        if node.file_path:
            _line(out, f'gal_save_vars("{_esc(node.file_path)}");')
    elif t == N_DEFVAR:
        sname = vars_mod.storage_name(node.var_name, scene_idx, node.scope)
        val = node.var_value
        if node.var_type == 2:
            _line(out, f'gal_var_set("{sname}", "{_esc(val)}");')
        else:
            _line(out, f'gal_var_set("{sname}", gal_itoa({val or 0}));' if _is_num(val)
                  else f'gal_var_set("{sname}", "{_esc(val)}");')
    elif t == N_SETVAR:
        sname = vars_mod.storage_name(node.var_name, scene_idx, node.scope)
        if node.set_mode == 1 or scene.find_value_source(node, 1) is not None:
            val = _value_expr(scene, node, 1, project, scene_idx)
        else:
            val = node.value if _is_num(node.value) else f'"{_esc(node.value)}"'
        # 按类型存储:数值转字符串
        if node.var_type == 1:
            _line(out, f'gal_var_set("{sname}", gal_ftoa({val}));')
        elif node.var_type == 0:
            _line(out, f'gal_var_set("{sname}", gal_itoa({val}));')
        else:
            _line(out, f'gal_var_set("{sname}", {val});')


def _is_num(s):
    try:
        float(s)
        return True
    except Exception:
        return False


def _emit_speak(scene, node, project, scene_idx, out):
    if node.use_char and node.char_idx >= 0:
        from .model import char_state_path
        p = char_state_path(project, node.char_idx, node.state_idx)
        if p:
            layer = 0
            _line(out, f'gal_sprite({layer}, "{_esc(p)}");')
            _line(out, f'gal_sprite_pos_mode({layer}, {node.text_pos});')
            _line(out, f'gal_sprite_show({layer}, 1);')
    if node.show_name and node.name:
        _line(out, f'gal_speaker("{_esc(node.name)}");')
    _line(out, f'gal_text_pos({node.text_pos});')
    if node.text:
        _line(out, f'gal_text_mode({node.text_mode});')
        sp = _text_speed(node)
        if sp:
            _line(out, f'gal_text_speed({sp});')
        _line(out, f'gal_text("{_esc(node.text)}");')
        _line(out, "wait_click();")
        _line(out, "gal_text_clear();")


def _emit_long(scene, node, project, scene_idx, out):
    """长对话:逐行自动播放,说话人自动立绘高亮。
    立绘模式可配置:long_clear_start(块开始清理)/ long_narration_hide(旁白隐藏)/
    long_dual(同屏双人:切换说话人保留其他人,高亮当前)。"""
    from .model import char_state_path
    clear_start = getattr(node, "long_clear_start", 1)
    narr_hide = getattr(node, "long_narration_hide", 1)
    dual = getattr(node, "long_dual", 1)
    if clear_start:
        # 先隐藏所有立绘层:不同长对话块/立绘间层分配独立,不清除旧层会导致
        # 同一个人物在不同层残留 → 屏幕出现两个相同立绘。
        for li in range(MAX_SPR):
            _line(out, f'gal_sprite_show({li}, 0);')
    # 收集涉及的人物集索引(去重)
    used = []
    for ci, _ in node.lines:
        if ci >= 0 and ci not in used:
            used.append(ci)
    layers = {}
    for k, ci in enumerate(used[:MAX_SPR]):
        layers[ci] = k
    shown = []                       # 本块内已显示过的人物(同屏时保留)
    for ci, txt in node.lines:
        if not txt:
            continue
        if ci >= 0 and ci < len(project.chars):
            ch = project.chars[ci]
            if ch.name:
                _line(out, f'gal_speaker("{_esc(ch.name)}");')
            # 该人物状态图:用人物当前状态(取第一个有效)
            st = ""
            for s in range(ch.nStates):
                if ch.states[s]:
                    st = ch.states[s]
                    break
            if st:
                layer = layers.get(ci, 0)
                if not dual:
                    # 单立绘模式:切换说话人时只留当前(先隐藏全部层)
                    for li in range(MAX_SPR):
                        _line(out, f'gal_sprite_show({li}, 0);')
                # 位置:默认第一个左/第二个右;可用节点的 spr_pos_mode 覆盖
                pos = 1 if layers.get(ci, 0) % 2 == 0 else 2
                pm = getattr(node, "spr_pos_mode", 0)
                if pm == 1:
                    pos = 0
                elif pm == 2:
                    pos = 1
                elif pm == 3:
                    pos = 2
                elif pm == 4:
                    pos = 3
                elif pm == 5:
                    pos = 4
                _line(out, f'gal_sprite({layer}, "{_esc(st)}");')
                _line(out, f'gal_sprite_pos_mode({layer}, {pos});')
                if getattr(node, "spr_fit", 0):
                    _line(out, f'gal_sprite_fit({layer}, {node.spr_fit});')
                _line(out, f'gal_sprite_show({layer}, 1);')
                _line(out, f'gal_sprite_alpha({layer}, 255);')
                if dual:
                    # 同屏:当前高亮,本块内已出现的其他人压暗(保留在画面上)
                    for k, oci in enumerate(used[:MAX_SPR]):
                        if oci != ci and oci in shown:
                            _line(out, f'gal_sprite_alpha({layers.get(oci, k)}, 90);')
                shown.append(ci)
            else:
                # 该人物无立绘状态(如"旁白"角色) → 总隐藏所有立绘层(空白场景)
                for li in range(MAX_SPR):
                    _line(out, f'gal_sprite_show({li}, 0);')
        elif narr_hide and not dual:
            # 旁白(无说话人):仅非「同屏双人」模式时隐藏立绘;
            # 同屏模式下旁白不打断画面上的双人立绘。
            for li in range(MAX_SPR):
                _line(out, f'gal_sprite_show({li}, 0);')
        _line(out, f'gal_text_mode({node.text_mode});')
        sp = _text_speed(node)
        if sp:
            _line(out, f'gal_text_speed({sp});')
        _line(out, f'gal_text("{_esc(txt)}");')
        _line(out, "wait_click();")
        _line(out, "gal_text_clear();")


def _emit_sprite(scene, node, project, scene_idx, out):
    from .model import char_state_path
    layer = node.spr_layer
    if node.use_char and node.char_idx >= 0:
        p = char_state_path(project, node.char_idx, node.state_idx)
    else:
        p = node.spr_path
    if not p:
        return
    _line(out, f'gal_sprite({layer}, "{_esc(p)}");')
    if node.use_xy:
        _line(out, f'gal_sprite_xy({layer}, {node.spr_x}, {node.spr_y});')
    else:
        _line(out, f'gal_sprite_pos_mode({layer}, {node.spr_pos_mode});')
    _line(out, f'gal_sprite_scale({layer}, {node.spr_scale});')
    if node.spr_fit:
        _line(out, f'gal_sprite_fit({layer}, {node.spr_fit});')
    _line(out, f'gal_sprite_alpha({layer}, {node.spr_alpha});')
    if node.spr_anim:
        _line(out, f'gal_sprite_anim({layer}, {node.spr_anim});')
    _line(out, f'gal_sprite_show({layer}, 1);')


def _emit_bg(node, out):
    """显示背景:图片或纯色。"""
    if node.bg_mode == 1:
        _line(out, f"gal_bg_color(0x{node.bg_color:06X});")
    elif node.bg_path:
        _line(out, f'gal_bg("{_esc(node.bg_path)}");')
        _line(out, f"gal_bg_fit({node.bg_fit});")


def _emit_code(node, out):
    cf = node.code_file
    body = ""
    if cf and os.path.isfile(cf):
        try:
            with open(cf, "r", encoding="utf-8-sig") as f:
                body = f.read()
        except Exception:
            body = ""
    else:
        _line(out, f"// 代码文件未找到: {cf}")
        return
    if node.code_cond:
        _line(out, f"if {node.code_cond} {{")
        global _indent
        _indent += 1
    for ln in body.splitlines():
        out.append("      " + ln)
    if node.code_cond:
        _indent -= 1
        _line(out, "}")


def _emit_say(scene, node, project, scene_idx, out):
    """动态文本:显示 value 输入或字面文本,等点击后清除(用于变量/拼接内容)。"""
    if node.name:
        _line(out, f'gal_speaker("{_esc(node.name)}");')
    _line(out, f"gal_text_pos({node.text_pos});")
    if node.text_mode:
        _line(out, f"gal_text_mode({node.text_mode});")
    sp = _text_speed(node)
    if sp:
        _line(out, f"gal_text_speed({sp});")
    src = scene.find_value_source(node, 1)
    if src is not None:
        expr = _value_expr(scene, node, 1, project, scene_idx)
        _line(out, f"gal_text({expr});")
    else:
        _line(out, f'gal_text("{_esc(node.text)}");')
    _line(out, "wait_click();")
    _line(out, "gal_text_clear();")


# ---------------- 菜单(MENU 场景)导出 ----------------
def _menu_func_name(title, idx):
    s = "".join(c if (c.isalnum() or c == "_") else "_" for c in title)
    s = s or f"m{idx}"
    return f"menu_{idx}_{s}"


def _menu_param_decl(m):
    """菜单数据入口 → 形参声明 [(形参变量名, 参数名), ...]。"""
    return [(f"_mi{i}", n.param_name) for i, n in enumerate(m.param_nodes())]


def _menu_param_map(m):
    """菜单数据入口节点 id → 形参变量名(供控件文字取值)。"""
    return {n.id: f"_mi{i}" for i, n in enumerate(m.param_nodes())}


# 菜单 exec 链中可执行的动作节点(复用镜头导出,变量统一全局作用域)
MENU_ACTION_TYPES = (N_DEFVAR, N_SETVAR, N_SAVE, N_LOAD,
                     N_FILE_WRITE, N_TOAST, N_WAIT)


def _emit_menu_ctrl(node, out, menu, params, project):
    """控件节点 → gal_menu_new + gal_menu_set 序列。
    params: {M_IN 节点 id: 形参变量名};控件「文字」value 输入连到
    数据入口/任意数据节点时用对应表达式。"""
    name = _esc(node.ctrl or "")
    if node.create:
        _line(out, f'gal_menu_new("{name}", {node.ctype});')
    _line(out, f'gal_menu_set("{name}", "x", "{node.px}");')
    _line(out, f'gal_menu_set("{name}", "y", "{node.py}");')
    _line(out, f'gal_menu_set("{name}", "w", "{node.pw}");')
    _line(out, f'gal_menu_set("{name}", "h", "{node.ph}");')
    _line(out, f'gal_menu_set("{name}", "show", "{1 if node.show else 0}");')
    if node.fit:
        _line(out, f'gal_menu_set("{name}", "fit", "{node.fit}");')
    src = menu.find_value_source(node, 1) if menu else None
    if node.use_signal_data:
        _line(out, f'gal_menu_set("{name}", "text", gal_signal_data());')
    elif src is not None and src.type == M_IN and src.id in params:
        # 文字来自数据入口参数(镜头菜单块传值)
        _line(out, f'gal_menu_set("{name}", "text", {params[src.id]});')
    elif src is not None:
        # 文字来自任意数据节点(变量/拼接/运算/读文件…)
        _line(out, f'gal_menu_set("{name}", "text", {_value_expr(menu, node, 1, project, 0)});')
    elif node.text:
        _line(out, f'gal_menu_set("{name}", "text", "{_esc(node.text)}");')
    if node.bg:
        if node.bg.lstrip().startswith("#"):
            try:
                color = int(node.bg.lstrip().lstrip("#"), 16) & 0xFFFFFF
                _line(out, f'gal_menu_set("{name}", "bg", "{color}");')
            except Exception:
                pass
        else:
            _line(out, f'gal_menu_set("{name}", "img", "{_esc(node.bg)}");')


def _menu_emit_exec(m, node, out, visited, ret_expr=None, params=None, project=None):
    """沿菜单 exec 链遍历(控件/退出/动作节点)。
    ret_expr: modal 菜单函数返回结果值;None 表示 overlay(void 函数,裸返回)。
    params: {M_IN 节点 id: 形参变量名}(数据入口值)。"""
    if node is None or node.id in visited:
        return
    visited = visited | {node.id}
    params = params or {}
    if node.type == M_CTRL:
        _emit_menu_ctrl(node, out, m, params, project)
    elif node.type == M_EXIT:
        _line(out, f'gal_menu_exit("{_esc(node.exit_name or "退出")}");')
        if ret_expr:
            _line(out, f"return {ret_expr};")
        else:
            _line(out, "return;")
        return
    elif node.type == M_JUMP:
        # 结束菜单并跳转指定镜头(返回特殊结果,镜头菜单块检测后切镜头)
        _line(out, f'gal_menu_exit("@scene:{node.flow_scene}");')
        if ret_expr:
            _line(out, f"return {ret_expr};")
        else:
            _line(out, "return;")
        return
    elif node.type in MENU_ACTION_TYPES:
        # 动作节点:输出副作用语句,继续沿 exec 链(变量统一全局 scene_idx=0)
        _emit_node(m, node, project, 0, out)
    for c in m.exec_children(node):
        _menu_emit_exec(m, c, out, visited, ret_expr, params, project)


def _emit_menu_modal(m, fn, out, params, param_map, project):
    """独占菜单:begin → 初始化 → while active 事件循环 → 返回结果。"""
    global _indent
    decl = ", ".join(f"{v}: string" for v, _ in params)
    out.append(f"func {fn}({decl}) -> string {{")
    _indent = 1
    _line(out, "gal_menu_begin();")
    if m.bg:
        _line(out, f'gal_menu_bg("{_esc(m.bg)}");')
    for st in m.starts():
        _menu_emit_exec(m, st, out, set(), "gal_menu_result()", param_map, project)
    _line(out, "while gal_menu_active() {")
    _indent += 1
    _line(out, "gal_poll();")
    _line(out, "let e = gal_menu_event();")
    for ck in m.clicks():
        cond = (ck.target_ctrl or "").strip()
        if not cond:
            continue
        _line(out, f'if e == "{_esc(cond)}" {{')
        _indent += 1
        _menu_emit_exec(m, ck, out, set(), "gal_menu_result()", param_map, project)
        _indent -= 1
        _line(out, "}")
    _line(out, "gal_wait(16);")
    _indent -= 1
    _line(out, "}")
    _line(out, "return gal_menu_result();")
    _indent = 0
    out.append("}")


def _emit_menu_overlay(m, fn, out, params, param_map, project):
    """叠加菜单:幂等初始化 → 处理积压点击 → 分发信号。"""
    global _indent
    decl = ", ".join(f"{v}: string" for v, _ in params)
    out.append(f"func {fn}({decl}) {{")
    _indent = 1
    for st in m.starts():
        _menu_emit_exec(m, st, out, set(), None, param_map, project)
    _line(out, "let e = gal_menu_event();")
    for ck in m.clicks():
        cond = (ck.target_ctrl or "").strip()
        if not cond:
            continue
        _line(out, f'if e == "{_esc(cond)}" {{')
        _indent += 1
        _menu_emit_exec(m, ck, out, set(), None, param_map, project)
        _indent -= 1
        _line(out, "}")
    _line(out, "let n = gal_signal_name();")
    for sg in m.signals():
        cond = (sg.signal or "").strip()
        if not cond:
            continue
        _line(out, f'if n == "{_esc(cond)}" {{')
        _indent += 1
        _menu_emit_exec(m, sg, out, set(), None, param_map, project)
        _indent -= 1
        _line(out, "}")
    _indent = 0
    out.append("}")


def _emit_menu(scene, node, project, scene_idx, out):
    """镜头 N_MENU 块:运行独占菜单 / 显示叠加 / 隐藏叠加 / 发送信号。
    菜单数据入口参数:从本块的 value 输入口(索引 1..)取实参传入菜单函数。"""
    mi = node.menu_idx
    if not (0 <= mi < len(project.menus)):
        _line(out, "// 未选择菜单")
        return
    m = project.menus[mi]
    fn = _menu_func_name(m.title, mi)
    params = _menu_param_decl(m)
    args = ", ".join(_value_expr(scene, node, 1 + i, project, scene_idx)
                     for i in range(len(params)))
    if node.action == 0:
        exits = [n.exit_name for n in m.exits()]
        _line(out, f"let r = {fn}({args});")
        if node.result_var:
            sname = vars_mod.storage_name(node.result_var, scene_idx, 0)
            _line(out, f'gal_var_set("{sname}", r);')
        if exits:
            global _indent
            for i, en in enumerate(exits):
                _line(out, f'{"else " if i else ""}if r == "{_esc(en)}" {{')
                _indent += 1
                nxt = scene.find_exec_target(node, i)
                if nxt is not None:
                    _emit_exec(scene, nxt, project, scene_idx, out, set(), set())
                _indent -= 1
                _line(out, "}")
        else:
            # 无退出节点:运行后直接继续(第 0 输出)
            nxt = scene.find_exec_target(node, 0)
            if nxt is not None:
                _emit_exec(scene, nxt, project, scene_idx, out, set(), set())
        # 菜单内「跳转镜头」节点(M_JUMP):返回特殊结果 @scene:i → 切镜头
        jumps = sorted({n.flow_scene for n in m.nodes
                        if n.type == M_JUMP and 0 <= n.flow_scene < len(project.scenes)})
        for k, j in enumerate(jumps):
            pre = "else " if (exits or k) else ""
            _line(out, f'{pre}if r == "@scene:{j}" {{')
            _indent += 1
            _line(out, f"scene_{j}();")
            _line(out, "return;")
            _indent -= 1
            _line(out, "}")
    elif node.action == 1:
        _line(out, "gal_menu_overlay_show();")
        _line(out, f"{fn}({args});")
    elif node.action == 2:
        _line(out, "gal_menu_overlay_hide();")
    elif node.action == 3:
        _line(out, f'gal_signal_emit("{_esc(node.signal)}", "{_esc(node.signal_data)}");')
        _line(out, f"{fn}({args});")


def write_scene_dex(project, out_path):
    """导出整个项目的蓝图 → DexLang。"""
    if not project.scenes:
        return False
    out = []
    from . import settings as settings_mod
    include_lib = "gal_static" if settings_mod.get_gal_link_mode() == "static" else "gal"
    out.append(f'include "{include_lib}";')
    out.append("")
    out.append("func wait_click() {")
    out.append("  while gal_running() {")
    out.append("    gal_poll();")
    out.append("    if gal_text_done() && gal_clicked() { return; }")
    out.append("    gal_wait(16);")
    out.append("  }")
    out.append("}")
    out.append("func wait_pick() {")
    out.append("  while gal_running() {")
    out.append("    gal_poll();")
    out.append("    if gal_picked() >= 0 { return; }")
    out.append("    gal_wait(16);")
    out.append("  }")
    out.append("}")
    out.append("")
    out.append(f"gal_init({project.win_w or 960}, {project.win_h or 540});")
    out.append(f'gal_set_title("{_esc(project.title)}");')
    out.append("gal_box(1);")
    theme = settings_mod.get_gal_theme()
    if theme and theme != "默认":
        out.append(f'gal_theme_apply("{theme}");')
    out.append("gal_trail_enable(%d);" % (1 if settings_mod.get_trail_enabled() else 0))
    out.append("")

    n = len(project.scenes)
    for si, sc in enumerate(project.scenes):
        out.append(f"// ===== 镜头 {si + 1}: {sc.title} =====")
        out.append(f"func scene_{si}() {{")
        if sc.useBgImg and sc.bg:
            out.append(f'  gal_bg("{_esc(sc.bg)}");')
            out.append(f"  gal_bg_fit({sc.bg_fit});")
        else:
            out.append(f"  gal_bg_color(0x{sc.bgColor:06X});")
        out.append("  gal_fade_in(0);  // 若由「柔和切镜头」淡出切入,这里淡入;否则无效果")
        cks = [ck for ck in sc.nodes if ck.type == N_CHECKPOINT]
        global _indent
        _indent = 1
        if cks:
            # 进入点分支:由「跳转到存档点」跳入时,按 _ckpt 从对应存档点之后继续
            out.append('  let _enter = gal_var_get("_ckpt");')
            for k, ck in enumerate(cks):
                pre = "  else " if k else "  "
                out.append(f'{pre}if _enter == "{_esc(ck.ck_name)}" {{')
                _indent = 2
                nxt = sc.find_exec_target(ck, 0)
                if nxt is not None:
                    _emit_exec(sc, nxt, project, si, out, set(), set())
                else:
                    out.append("    // 存档点无后续")
                _indent = 1
                out.append("  }")
            out.append("  else {")
            _indent = 2
            entry = sc.node(sc.entry_id)
            if entry:
                nxt = sc.find_exec_target(entry, 0)
                if nxt is not None:
                    _emit_exec(sc, nxt, project, si, out, set(), set())
                else:
                    out.append("    // 入口未连接")
            _indent = 1
            out.append("  }")
        else:
            entry = sc.node(sc.entry_id)
            if entry:
                nxt = sc.find_exec_target(entry, 0)
                if nxt is not None:
                    _emit_exec(sc, nxt, project, si, out, set(), set())
                else:
                    out.append("  // 入口未连接")
        out.append("}")
        out.append("")

    # 菜单处理函数(在 scene_0 调用之前定义)
    for mi, m in enumerate(project.menus):
        fn = _menu_func_name(m.title, mi)
        out.append(f"// ===== 菜单 {mi + 1}: {m.title} ({'独占' if m.mode == 0 else '叠加'}) =====")
        if m.mode == 0:
            _emit_menu_modal(m, fn, out, _menu_param_decl(m), _menu_param_map(m), project)
        else:
            _emit_menu_overlay(m, fn, out, _menu_param_decl(m), _menu_param_map(m), project)
        out.append("")

    out.append("scene_0();")
    out.append('print "end";')
    text = "\n".join(out) + "\n"
    global last_error
    last_error = None
    try:
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return True
    except Exception as e:
        # 记录原因而不是静默吞掉(写权限/路径非法/磁盘满等)
        last_error = f"无法写入 {out_path}: {e}"
        return False


def export_project(project, root_dir):
    global last_error
    last_error = None
    if not project.scenes:
        last_error = "项目没有任何镜头,无可导出内容"
        return ""
    try:
        os.makedirs(root_dir, exist_ok=True)
    except Exception as e:
        last_error = f"无法创建输出目录 {root_dir}: {e}"
        return ""
    path = os.path.join(root_dir, "scene_out.dex")
    return path if write_scene_dex(project, path) else ""
