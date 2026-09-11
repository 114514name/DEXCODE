# -*- coding: utf-8 -*-
"""构建全功能测试蓝图 examples/test/test_all.bluescene(资源来自 JASONBASEENG)。"""
import json
import sys

sys.path.insert(0, r"c:\Users\ASUS\Desktop\Code\DEXCODE")

from bluedit.model import (project_defaults, scene_defaults, Character, Link,
                           N_ENTRY, N_EXIT, N_SPEAK, N_LONG, N_SPRITE, N_BG,
                           N_DEFVAR, N_SETVAR, N_IF, N_GETVAR, N_LOGIC,
                           N_MATH, N_FLOW, N_FX_ADD, N_FX_CLEAR, N_MENU,
                           N_TEXT, N_SAY, N_RANDOM, N_WAIT, N_DOUT, N_DREF,
                           N_TEXTLIT, N_NUMLIT, N_BOLLIT,
                           N_BGM, N_SE, N_SOUND_STOP, N_SOUND_SWAP,
                           N_BG_TRANS, N_SCENE_TRANS,
                           M_START, M_CTRL, M_CLICK, M_EXIT, MENUScene,
                           project_to_dict)
from bluedit import settings as settings_mod

# 资源路径约定:相对项目根(project_root)。运行时 cwd=project_root,资源在 <root>/res/ 下。
R = "res/"

p = project_defaults()
p.title = "全功能测试蓝图"
p.description = "尽可能覆盖引擎功能的测试:剧情/菜单/输入框/声音/过渡/数据流/特效/主题"
p.win_w = 960
p.win_h = 540

# 人物:小羽(立绘用猫)
c = Character()
c.name = "小羽"
c.states = [R + "ch_cat_normal.png", R + "ch_cat_happy.png"]
c.stateNames = ["平常", "开心"]
c.nStates = 2
c.cur = 0
p.chars.append(c)

# 菜单(镜头里的菜单块要引用,先建空壳,最后再填充)
p.menus.append(MENUScene("主菜单"))

sc1 = p.scenes[0]
sc1.title = "清晨的海边"
sc1.bg = R + "bg_beach_day.jpg"
sc1.useBgImg = 1
sc1.bg_fit = 2
sc2 = scene_defaults("黄昏的公园")
sc2.bg = R + "bg_park_day.jpg"
sc2.useBgImg = 1
sc2.bg_fit = 2
p.scenes.append(sc2)


def link(sc, fn, fp, tn, tp):
    sc.links.append(Link(sc.lid, fn, fp, tn, tp))
    sc.lid += 1


# ============ 镜头1 ============
e1 = sc1.node(sc1.entry_id)
ex1 = sc1.node(sc1.exit_id)

bgm = sc1.new_node(N_BGM, 0, 0)
bgm.sound_path = R + "bgm_sea.mp3"
bgm.sound_vol = 70
spk1 = sc1.new_node(N_SPEAK, 0, 0)
spk1.text = "欢迎来到全功能测试蓝图！我会带你逛遍每一个角落。"
spk1.name = "小羽"
spk1.use_char = 1
spk1.char_idx = 0
spk1.state_idx = 0
long1 = sc1.new_node(N_LONG, 0, 0)
long1.lines = [[0, "这是一段长对话,用于测试自动连续播放。"],
               [0, "第二句:对话会自动推进,不用点击。"]]
defvar = sc1.new_node(N_DEFVAR, 0, 0)
defvar.var_name = "hp"
defvar.var_type = 0
defvar.var_value = "100"
rand = sc1.new_node(N_RANDOM, 0, 0)
rand.max = 100
setvar = sc1.new_node(N_SETVAR, 0, 0)
setvar.var_name = "hp"
setvar.var_type = 0
getvar = sc1.new_node(N_GETVAR, 0, 0)
getvar.var_name = "hp"
num50 = sc1.new_node(N_NUMLIT, 0, 0)
num50.lit_num = "50"
logic = sc1.new_node(N_LOGIC, 0, 0)
logic.op = ">"
ifn = sc1.new_node(N_IF, 0, 0)
spk_t = sc1.new_node(N_SPEAK, 0, 0)
spk_t.text = "随机后 HP 大于 50！运气不错。"
spk_t.name = "小羽"
spk_t.use_char = 1
spk_t.char_idx = 0
spk_t.state_idx = 0
spk_f = sc1.new_node(N_SPEAK, 0, 0)
spk_f.text = "随机后 HP 不大于 50,再接再厉。"
spk_f.name = "小羽"
spk_f.use_char = 1
spk_f.char_idx = 0
spk_f.state_idx = 0
bgtr = sc1.new_node(N_BG_TRANS, 0, 0)
bgtr.bg_path = R + "bg_beach_dusk.jpg"
bgtr.bg_trans_ms = 600
se = sc1.new_node(N_SE, 0, 0)
se.sound_path = R + "se_hit.ogg"
menu = sc1.new_node(N_MENU, 0, 0)
menu.menu_idx = 0
menu.action = 0
# 菜单块数据口自动同步(在保存后由 from_dict 重建;这里手动同步)
menu.sync_menu_outputs(p.menus[0])

# 开始分支:纯文本 + 菜单输入框昵称 → 拼接 → 动态文本 → 特效 → 数据输出点 → 柔和切镜头
tl1 = sc1.new_node(N_TEXTLIT, 0, 0)
tl1.lit_text = "你好, "
tl3 = sc1.new_node(N_TEXTLIT, 0, 0)
tl3.lit_text = "！欢迎开始冒险。"
txt1 = sc1.new_node(N_TEXT, 0, 0)
say1 = sc1.new_node(N_SAY, 0, 0)
say1.name = "小羽"
fx1 = sc1.new_node(N_FX_ADD, 0, 0)
fx1.fx = 2            # 朦胧
fx1.fx_strength = 40
dout = sc1.new_node(N_DOUT, 0, 0)
dout.out_name = "hp_final"
sctr = sc1.new_node(N_SCENE_TRANS, 0, 0)
sctr.scene_trans_ms = 400
# 退出分支:结束游戏
exit_flow = sc1.new_node(N_FLOW, 0, 0)
exit_flow.flow_target = 2   # 结束

link(sc1, e1.id, 0, bgm.id, 0)
link(sc1, bgm.id, 0, spk1.id, 0)
link(sc1, spk1.id, 0, long1.id, 0)
link(sc1, long1.id, 0, defvar.id, 0)
link(sc1, defvar.id, 0, setvar.id, 0)
link(sc1, setvar.id, 0, ifn.id, 0)
link(sc1, ifn.id, 0, spk_t.id, 0)
link(sc1, ifn.id, 1, spk_f.id, 0)
link(sc1, spk_t.id, 0, bgtr.id, 0)
# 假分支独立走:不汇合,直接柔和切镜头到镜头2(避免导出器分支汇合重复)
sctr_f = sc1.new_node(N_SCENE_TRANS, 0, 0)
sctr_f.scene_trans_ms = 400
link(sc1, spk_f.id, 0, sctr_f.id, 0)
link(sc1, bgtr.id, 0, se.id, 0)
link(sc1, se.id, 0, menu.id, 0)
# 菜单"开始"(exec 口 0) → 拼接链
link(sc1, menu.id, 0, say1.id, 0)
link(sc1, say1.id, 0, fx1.id, 0)
link(sc1, fx1.id, 0, dout.id, 0)
link(sc1, dout.id, 0, sctr.id, 0)
# 菜单"退出"(exec 口 1) → 结束
link(sc1, menu.id, 1, exit_flow.id, 0)
# value 网
link(sc1, rand.id, 0, setvar.id, 1)
link(sc1, getvar.id, 0, logic.id, 0)
link(sc1, num50.id, 0, logic.id, 1)
link(sc1, logic.id, 0, ifn.id, 1)
link(sc1, tl1.id, 0, txt1.id, 0)
link(sc1, menu.id, 2, txt1.id, 1)     # 菜单块 nickname 数据口 → 拼接 B
link(sc1, tl3.id, 0, txt1.id, 2)
link(sc1, txt1.id, 0, say1.id, 1)
link(sc1, getvar.id, 0, dout.id, 1)

# ============ 镜头2 ============
e2 = sc2.node(sc2.entry_id)
ex2 = sc2.node(sc2.exit_id)
spk2 = sc2.new_node(N_SPEAK, 0, 0)
spk2.text = "镜头2:黄昏的公园。先看看上一幕留下的数据……"
spk2.name = "小羽"
spk2.use_char = 1
spk2.char_idx = 0
spk2.state_idx = 1          # 开心立绘
dref = sc2.new_node(N_DREF, 0, 0)
dref.out_scene = 0
dref.out_name = "hp_final"
tl_a = sc2.new_node(N_TEXTLIT, 0, 0)
tl_a.lit_text = "你最终的 HP 是 "
txt2 = sc2.new_node(N_TEXT, 0, 0)
say2 = sc2.new_node(N_SAY, 0, 0)
say2.name = "小羽"
swap = sc2.new_node(N_SOUND_SWAP, 0, 0)
swap.sound_path = R + "bgm_dusk.mp3"
swap.sound_vol = 60
fxc = sc2.new_node(N_FX_CLEAR, 0, 0)
stop = sc2.new_node(N_SOUND_STOP, 0, 0)
stop.stop_target = 2
wait = sc2.new_node(N_WAIT, 0, 0)
wait.ms = 300
say_end = sc2.new_node(N_SAY, 0, 0)
say_end.name = "小羽"
say_end.text_mode = 1
say_end.text = "这就是全部功能啦——感谢游玩本测试蓝图！我们下次再见。"
link(sc2, e2.id, 0, spk2.id, 0)
link(sc2, spk2.id, 0, say2.id, 0)
link(sc2, say2.id, 0, swap.id, 0)
link(sc2, swap.id, 0, fxc.id, 0)
link(sc2, fxc.id, 0, stop.id, 0)
link(sc2, stop.id, 0, wait.id, 0)
link(sc2, wait.id, 0, say_end.id, 0)
link(sc2, say_end.id, 0, ex2.id, 0)
link(sc2, dref.id, 0, txt2.id, 1)
link(sc2, tl_a.id, 0, txt2.id, 0)
link(sc2, txt2.id, 0, say2.id, 1)

# ============ 菜单:主菜单 ============
m = p.menus[0]
m.title = "主菜单"
m.bg = R + "bg_park_day.jpg"
st = m.new_node(M_START, 0, 0)
t1 = m.new_node(M_CTRL, 0, 0); t1.ctrl = "title"; t1.ctype = 1
t1.text = "★ 全功能测试 ★"; t1.px = 330; t1.py = 80; t1.pw = 300; t1.ph = 60
t1.font = 26; t1.bold = 1
inp = m.new_node(M_CTRL, 0, 0); inp.ctrl = "nickname"; inp.ctype = 4
inp.text = "玩家"; inp.px = 330; inp.py = 170; inp.pw = 300; inp.ph = 50
btn_s = m.new_node(M_CTRL, 0, 0); btn_s.ctrl = "btn_start"; btn_s.ctype = 0
btn_s.text = "开始"; btn_s.px = 330; btn_s.py = 260; btn_s.pw = 200; btn_s.ph = 60
btn_s.bg = "#3377dd"
btn_q = m.new_node(M_CTRL, 0, 0); btn_q.ctrl = "btn_quit"; btn_q.ctype = 0
btn_q.text = "退出"; btn_q.px = 330; btn_q.py = 340; btn_q.pw = 200; btn_q.ph = 60
btn_q.bg = "#aa4444"
ck_s = m.new_node(M_CLICK, 0, 0); ck_s.target_ctrl = "btn_start"
ex_s = m.new_node(M_EXIT, 0, 0); ex_s.exit_name = "开始"
ck_q = m.new_node(M_CLICK, 0, 0); ck_q.target_ctrl = "btn_quit"
ex_q = m.new_node(M_EXIT, 0, 0); ex_q.exit_name = "退出"
for n in (t1, inp, btn_s, btn_q):
    link(m, st.id, 0, n.id, 0)
link(m, ck_s.id, 0, ex_s.id, 0)
link(m, ck_q.id, 0, ex_q.id, 0)

# 菜单块数据口再同步一次(菜单已建完)
menu.sync_menu_outputs(p.menus[0])
print("菜单块输出口:", [(q.kind, q.name) for q in menu.outputs])

out = r"c:\Users\ASUS\Desktop\Code\DEXCODE\examples\test\test_all.bluescene"
with open(out, "w", encoding="utf-8", newline="\n") as f:
    json.dump(project_to_dict(p), f, ensure_ascii=False, indent=1)
print("已保存:", out)
print("场景数:", len(p.scenes), "菜单数:", len(p.menus))
