# -*- coding: utf-8 -*-
"""从 dexgame.dexdef 生成 dexgame_static.dexdef(只改 refer 行与头部注释)。

两份文件必须保持一致,手工维护容易漂移 —— 但仓库惯例是两份都入库
(gal.dexdef / gal_static.dexdef 也是如此,便于用户直接 include)。
故用本脚本生成,并在改动接口后重新运行:
    python _mkstatic.py
"""
import os

SRC = os.path.join("libs", "dexgame", "dexgame.dexdef")
DST = os.path.join("libs", "dexgame", "dexgame_static.dexdef")

s = open(SRC, encoding="utf-8").read()

old_head = ("# dexgame.dexdef — dexgame 游戏引擎接口(动态链接版)\n"
            "#\n")
new_head = ("# dexgame_static.dexdef — dexgame 游戏引擎接口(静态内嵌版)\n"
            "#\n"
            "# 由 dexgame.dexdef 生成,唯一区别是下面的 `refer static`:编译时把\n"
            "# libdexgame.dll 的字节内嵌进 .dexbc,运行时无需外部 DLL 文件。\n"
            "# 改接口后请重跑生成脚本,不要手工编辑本文件。\n"
            "#\n")
if old_head not in s:
    raise SystemExit("头部不匹配,请检查 dexgame.dexdef")
s = s.replace(old_head, new_head, 1)

old_refer = 'refer "libdexgame.dll";'
new_refer = 'refer static "libdexgame.dll";'
if old_refer not in s:
    raise SystemExit("refer 行不匹配")
s = s.replace(old_refer, new_refer, 1)

open(DST, "w", encoding="utf-8", newline="").write(s)
print("生成", DST, len(s), "字节,", s.count("\n") + 1, "行")
