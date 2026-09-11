#!/usr/bin/env python3
"""GAL 编辑器(Python + tkinter 版)启动入口。

用法:
  python gal_main.py                 # 新建项目
  python gal_main.py 作品.galscene   # 打开现有场景
"""

import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from bluedit.app import main  # noqa: E402

if __name__ == "__main__":
    start_file = None
    for a in sys.argv[1:]:
        if os.path.isfile(a):
            start_file = os.path.abspath(a)
            break
    main(start_file=start_file)
