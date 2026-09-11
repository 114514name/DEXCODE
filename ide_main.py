#!/usr/bin/env python3
"""DEXIDE 启动入口。

用法:
  python ide_main.py [项目文件夹] [文件.dex]
"""

import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from dexide.ide import main  # noqa: E402

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    root = args[0] if len(args) >= 1 and os.path.isdir(args[0]) else ROOT
    file = None
    for a in args:
        if os.path.isfile(a):
            file = os.path.abspath(a)
            break
    main(root_dir=root, start_file=file)
