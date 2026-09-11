"""galide.export — 导出可编译的 DexLang 场景代码(scene_out.dex)。

逻辑与 C 版 tools/galedit.cpp 的 write_scene_dex / emit_blocks / emit_normal 对齐:
生成 include "gal" 的 DEX 源码,可直接用 `python main.py compile scene_out.dex` 编译。
"""

import os

from .model import (MAX_SPR, MAX_CHOICES,
                    BL_SPEAK, BL_BG, BL_SPRITE, BL_AUDIO, BL_WAIT, BL_CLEAR,
                    BL_CHOICE, BL_OPTION, BL_IF, BL_CODE, BL_END,
                    block_end)


def _str_esc(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _block_spr_path(p, sc, b) -> str:
    """解析立绘实际路径:优先人物集状态图,否则直接路径(与 C 版 block_spr_path 对齐)。"""
    if b.charIdx >= 0 and b.charIdx < len(p.chars):
        ch = p.chars[b.charIdx]
        if 0 <= b.stateIdx < ch.nStates and ch.states[b.stateIdx]:
            return ch.states[b.stateIdx]
    return b.sprPath


def _emit_normal(out: list, p, sc, b):
    if b.type == BL_SPEAK:
        if b.speaker:
            out.append(f'    gal_speaker("{_str_esc(b.speaker)}");')
        out.append(f'    gal_box_style({b.style_avatar}, {b.style_name});')
        out.append(f'    gal_box_autofit({b.auto_fit});')
        out.append(f'    gal_text_pos({b.text_pos});')
        if b.text:
            out.append(f'    gal_text("{_str_esc(b.text)}");')
            out.append('    wait_click();')
            out.append('    gal_text_clear();')
    elif b.type == BL_BG:
        if b.useBgImg and b.bg:
            out.append(f'    gal_bg("{_str_esc(b.bg)}");')
        else:
            out.append(f'    gal_bg_color(0x{b.bgColor:06X});')
    elif b.type == BL_SPRITE:
        k = b.spr_layer
        if k < 0 or k >= MAX_SPR:
            k = 0
        wp = _block_spr_path(p, sc, b)
        if wp:
            out.append(f'    gal_sprite({k}, "{_str_esc(wp)}");')
            out.append(f'    gal_sprite_pos_mode({k}, {b.spr_pos});')
            out.append(f'    gal_sprite_autofit({k}, {b.spr_autofit});')
            out.append(f'    gal_sprite_anim({k}, {b.spr_anim});')
            out.append(f'    gal_sprite_show({k}, {b.spr_show});')
    elif b.type == BL_AUDIO:
        if b.audio_type == 0 and b.audio_path:
            out.append(f'    gal_play_bgm("{_str_esc(b.audio_path)}");')
        elif b.audio_type == 1 and b.audio_path:
            out.append(f'    gal_play_se("{_str_esc(b.audio_path)}");')
        elif b.audio_type == 2:
            out.append('    gal_stop_bgm();')
        elif b.audio_type == 3:
            out.append(f'    gal_volume({b.volume});')
    elif b.type == BL_WAIT:
        if b.wait_type == 1 and b.wait_ms > 0:
            out.append(f'    gal_wait({b.wait_ms});')
        else:
            out.append('    while gal_running() { gal_poll(); if gal_clicked() { break; } gal_wait(16); }')
    elif b.type == BL_CLEAR:
        out.append('    gal_text_clear();')
    elif b.type == BL_CODE:
        cf = b.code_file
        cond = b.cond if b.code_cond else ""
        body = ""
        if cf and os.path.isfile(cf):
            try:
                with open(cf, "r", encoding="utf-8-sig") as f:
                    body = f.read()
            except Exception:
                body = ""
        else:
            out.append(f'    // 代码文件未找到: {cf}')
            return
        if b.code_cond:
            out.append(f'    if {_str_esc(b.code_cond)} {{')
        for ln in body.splitlines():
            out.append(f'      {ln}')
        if b.code_cond:
            out.append('    }')
    # BL_END / default:无输出


def _emit_blocks(out: list, p, sc, start, parent_depth):
    """递归导出块序列(从 start 开始,处理 depth > parent_depth 的块)。"""
    i = start
    while i < sc.nBlocks and sc.blocks[i].depth > parent_depth:
        b = sc.blocks[i]
        d = b.depth
        if b.type == BL_CHOICE:
            if b.speaker:
                out.append(f'    gal_speaker("{_str_esc(b.speaker)}");')
            if b.choice_text:
                out.append(f'    gal_text("{_str_esc(b.choice_text)}");')
            nopt = 0
            j = i + 1
            while j < sc.nBlocks and sc.blocks[j].depth > d:
                if sc.blocks[j].type == BL_OPTION and sc.blocks[j].depth == d + 1:
                    nopt += 1
                j = block_end(sc, j)
            if nopt < 1:
                nopt = 1
            opt_idx = 0
            j = i + 1
            while j < sc.nBlocks and sc.blocks[j].depth > d:
                if sc.blocks[j].type == BL_OPTION and sc.blocks[j].depth == d + 1:
                    if opt_idx >= nopt or opt_idx >= MAX_CHOICES:
                        break
                    txt = sc.blocks[j].option_text if sc.blocks[j].option_text else "…"
                    out.append(f'    gal_set_choice({opt_idx}, "{_str_esc(txt)}");')
                    opt_idx += 1
                j = block_end(sc, j)
            out.append('    gal_show_choices();')
            out.append('    wait_pick();')
            out.append('    let p = gal_picked();')
            out.append('    gal_hide_choices();')
            out.append('    gal_text_clear();')
            sel = 0
            j2 = i + 1
            while j2 < sc.nBlocks and sc.blocks[j2].depth > d:
                if sc.blocks[j2].type == BL_OPTION and sc.blocks[j2].depth == d + 1:
                    out.append(f'    {"else " if sel else ""}if p == {sel} {{')
                    _emit_blocks(out, p, sc, j2 + 1, d + 1)
                    out.append('    }')
                    sel += 1
                j2 = block_end(sc, j2)
            if sel == 0:
                out.append('    if p >= 0 {}')
            i = j2
        elif b.type == BL_OPTION:
            i = block_end(sc, i)
        elif b.type == BL_IF:
            cond = b.cond if b.cond else "1"
            out.append(f'    if {_str_esc(cond)} {{')
            _emit_blocks(out, p, sc, i + 1, d)
            out.append('    }')
            i = block_end(sc, i)
        else:
            _emit_normal(out, p, sc, b)
            i += 1


def write_scene_dex(p, out_path: str) -> bool:
    """导出整个项目的 DEX 代码到 out_path。返回是否成功。"""
    if not p.scenes:
        return False
    out = []
    out.append('include "gal";')
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
    out.append("gal_init(960, 540);")
    out.append(f'gal_set_title("{_str_esc(p.title)}");')
    out.append("gal_box(1);")
    out.append("")

    n = len(p.scenes)
    for s, sc in enumerate(p.scenes):
        out.append(f"// ===== 镜头 {s + 1}: {sc.title} =====")
        out.append(f"func scene_{s}() {{")
        if sc.useBgImg and sc.bg:
            out.append(f'  gal_bg("{_str_esc(sc.bg)}");')
        else:
            out.append(f'  gal_bg_color(0x{sc.bgColor:06X});')
        for k in range(MAX_SPR):
            if sc.spr[k]:
                out.append(f'  gal_sprite({k}, "{_str_esc(sc.spr[k])}");')
                out.append(f'  gal_sprite_pos_mode({k}, 0);')
                out.append(f'  gal_sprite_scale({k}, 100);')
                out.append(f'  gal_sprite_show({k}, 0);')
        _emit_blocks(out, p, sc, 0, -1)
        if s + 1 < n:
            out.append(f'  scene_{s + 1}();')
        else:
            out.append('  gal_close();')
            out.append('  return;')
        out.append('}')
        out.append("")

    out.append("scene_0();")
    out.append('print "end";')
    text = "\n".join(out) + "\n"
    try:
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return True
    except Exception:
        return False


def export_project(p, root_dir: str) -> str:
    """导出到 root_dir/scene_out.dex,返回输出路径;失败返回空串。"""
    if not p.scenes:
        return ""
    if not root_dir:
        return ""
    try:
        os.makedirs(root_dir, exist_ok=True)
    except Exception:
        pass
    dex_path = os.path.join(root_dir, "scene_out.dex")
    if write_scene_dex(p, dex_path):
        return dex_path
    return ""
