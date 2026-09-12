# -*- coding: utf-8 -*-
"""测试用「可写」临时目录工具。

为什么不用 `tempfile` 的目录 API
--------------------------------
`tempfile.mkdtemp()` 内部是 `os.mkdir(path, 0o700)`。在 Windows 上 `0o700` 会落成
「仅属主」的 ACL;于是在受限沙箱中(例如只放行工作区写入的环境)当前进程并非该 ACL
的属主,后果是 —— **mkdtemp 返回的目录里既不能建子目录也不能写文件**,连
`shutil.rmtree` 都会被拒。症状是最外层只看到
`PermissionError: [WinError 5] 拒绝访问` / `[Errno 13] Permission denied`,
看不出是谁造成的。

实测(Windows + 受限沙箱,同一父目录):

    os.mkdir(p, 0o700) -> 内部写入 FAIL / 清理 FAIL
    os.mkdir(p, 0o777) -> 内部写入 OK   / 清理 OK
    os.mkdir(p)        -> 内部写入 OK   / 清理 OK

`tempfile.TemporaryDirectory` 同样走 mkdtemp,故一并受影响。

本模块的做法
------------
一律用 `0o777` 自建临时目录,根目录优先用系统临时目录;若系统临时目录不可用
(受限环境),回退到仓库内的 `_tmptest/`(`.gitignore` 的 `/_*` 已覆盖)。
**在正常机器上行为与 `tempfile` 等价**,因此对现有测试是纯替换。

用法:

    from _tmpdir import mktempdir, tempdir

    d = mktempdir("dexc_")              # 进程退出时自动清理
    with tempdir("dexc_") as d:         # 退出块时立即清理
        ...
"""

import atexit
import os
import shutil
import tempfile
import uuid
from contextlib import contextmanager

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_FALLBACK_ROOT = os.path.join(_REPO, "_tmptest")
_created = []


def _writable_root(root):
    """能否在该根目录下建立 0o777 目录并写入?(照着真实用法探测)"""
    probe = os.path.join(root, "w_" + uuid.uuid4().hex[:8])
    try:
        os.makedirs(root, exist_ok=True)
        os.mkdir(probe, 0o777)
    except OSError:
        return False
    try:
        with open(os.path.join(probe, "probe"), "w", encoding="utf-8") as f:
            f.write("x")
        return True
    except OSError:
        return False
    finally:
        shutil.rmtree(probe, ignore_errors=True)


def _pick_root():
    sys_root = tempfile.gettempdir()
    # 注意:tempfile.gettempdir() 在所有候选都不可用时会**退化成 os.getcwd()**
    # (Python 的既定行为)。那种情况下它虽然「可写」,但会把临时目录撒在仓库根目录,
    # 所以这里显式排除 cwd,改走 _FALLBACK_ROOT(仓库内 _tmptest/,已被 gitignore)。
    is_cwd = os.path.normcase(os.path.abspath(sys_root)) == os.path.normcase(os.getcwd())
    if not is_cwd and _writable_root(sys_root):
        return sys_root
    if _writable_root(_FALLBACK_ROOT):
        return _FALLBACK_ROOT
    # 两处都不可用:交回系统临时目录,让失败信息保持原样而不是被这里掩盖
    return sys_root


ROOT = _pick_root()
USING_FALLBACK = os.path.normcase(ROOT) == os.path.normcase(_FALLBACK_ROOT)


def mktempdir(prefix="dex_"):
    """建一个可写临时目录,返回其路径(进程退出时自动清理)。"""
    os.makedirs(ROOT, exist_ok=True)
    path = os.path.join(ROOT, prefix + uuid.uuid4().hex[:10])
    os.mkdir(path, 0o777)
    _created.append(path)
    return path


def _cleanup_one(path):
    try:
        _created.remove(path)
    except ValueError:
        pass
    shutil.rmtree(path, ignore_errors=True)


@contextmanager
def tempdir(prefix="dex_"):
    """`with tempdir() as d:` —— 离开代码块时立即清理。"""
    path = mktempdir(prefix)
    try:
        yield path
    finally:
        _cleanup_one(path)


@atexit.register
def _cleanup_all():
    for path in list(_created):
        _cleanup_one(path)
    if USING_FALLBACK:
        try:
            os.rmdir(_FALLBACK_ROOT)
        except OSError:
            pass
