#!/usr/bin/env python3
"""检查前端"引用了不存在的元素 id"。

为什么单独一个脚本:这类错误的表现是**某个按钮永远点了没反应**(init 里
`el('btn-x').onclick = …` 抛 TypeError,后面的接线全部没执行),而肉眼审代码
很难发现。实测踩过一次:`btn-code-reload` 在重写 index.html 时被漏掉,
整块代码页签的按钮全都没接线。

用法:python tools/check_web_ids.py   (退出码 0 = 全部存在)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "dexstudio", "web")


def main():
    index = os.path.join(WEB, "index.html")
    if not os.path.exists(index):
        print("找不到 dexstudio/web/index.html", file=sys.stderr)
        return 1
    with open(index, encoding="utf-8") as f:
        html = f.read()
    have = set(re.findall(r'id="([A-Za-z0-9_\-]+)"', html))
    # 前端里所有"取元素"的写法:$( ) / el( ) / getElementById( )
    pats = [
        re.compile(r"\$\(\s*'([A-Za-z0-9_\-]+)'\s*\)"),
        re.compile(r"\bel\(\s*'([A-Za-z0-9_\-]+)'\s*\)"),
        re.compile(r"getElementById\(\s*'([A-Za-z0-9_\-]+)'\s*\)"),
    ]
    missing = {}
    for name in sorted(os.listdir(WEB)):
        if not name.endswith(".js"):
            continue
        with open(os.path.join(WEB, name), encoding="utf-8") as f:
            js = f.read()
        for pat in pats:
            for m in pat.finditer(js):
                el = m.group(1)
                if el not in have:
                    missing.setdefault(el, set()).add(name)
    if missing:
        print("以下元素 id 被 JS 引用但 index.html 里没有(按钮会点不动):")
        for el in sorted(missing):
            print("  %-22s ← %s" % (el, ", ".join(sorted(missing[el]))))
        return 1
    print("前端元素 id 检查通过(index.html 覆盖了所有 JS 引用)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
