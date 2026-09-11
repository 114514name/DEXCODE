#!/usr/bin/env python3
"""DEXCODE 发布版 VM / 成品打包测试。

运行: python tests/test_vm_versions.py
覆盖:release 打包(core.do + galrun.exe + res + 启动脚本)、
发布版 VM 无参数自动读取 core.do、普通 VM 无参数读取 core.do、
无控制台 VM 运行、三版本文件齐全。

注意:GAL 成品运行会短暂创建窗口,需要交互桌面会话。
"""

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import main as dexmain  # noqa: E402

PASS = 0
FAIL = 0
SKIP = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def skip(name, why):
    global SKIP
    SKIP += 1
    print(f"  SKIP  {name}  ({why})")


VM = os.path.join(ROOT, "vm", "vm.exe")
VMNC = os.path.join(ROOT, "vm", "vmnc.exe")
GALRUN = os.path.join(ROOT, "vm", "galrun.exe")

GAME_SRC = """
include "gal";
gal_init(400, 300);
gal_set_title("发布测试");
gal_bg_color(0x2050A0);
gal_text("发布测试文本");
let n = 0;
while n < 8 { gal_poll(); n = n + 1; }
gal_close();
print "done";
"""


def test_files_exist():
    print("三版本 VM 存在")
    check("vm.exe(普通)", os.path.exists(VM))
    check("vmnc.exe(无控制台)", os.path.exists(VMNC))
    check("galrun.exe(发布版)", os.path.exists(GALRUN))


def test_release_package():
    print("release 打包")
    if not os.path.exists(GALRUN):
        skip("release 打包", "galrun.exe 未构建")
        return
    with tempfile.TemporaryDirectory(prefix="dexrel_") as tmp:
        src = os.path.join(tmp, "game.dex")
        with open(src, "w", encoding="utf-8") as f:
            f.write(GAME_SRC)
        res = os.path.join(tmp, "res_src")
        os.makedirs(res)
        with open(os.path.join(res, "bg.png"), "wb") as f:
            f.write(b"\x89PNG test")
        with open(os.path.join(res, "bgm.mp3"), "wb") as f:
            f.write(b"ID3 fake")

        out = os.path.join(tmp, "release")
        rc = dexmain.main(["release", src, "-o", out, "-r", res, "--title", "测试游戏"])
        check("release 命令退出码 0", rc == 0, f"rc={rc}")
        check("core.do 生成", os.path.exists(os.path.join(out, "core.do")))
        check("galrun.exe 复制", os.path.exists(os.path.join(out, "galrun.exe")))
        check("启动脚本生成", os.path.exists(os.path.join(out, "启动游戏.bat")))
        check("说明生成", os.path.exists(os.path.join(out, "游戏说明.txt")))
        check("资源复制", os.path.exists(os.path.join(out, "res", "bg.png"))
              and os.path.exists(os.path.join(out, "res", "bgm.mp3")))

        # 运行发布版:无参数自动读 core.do(FreeConsole 隐藏控制台窗口,
        # 双击运行时不可见;此处用管道模式验证能正常运行即可)
        r = subprocess.run([os.path.join(out, "galrun.exe")], cwd=out,
                           capture_output=True, timeout=40,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        check("发布版运行退出码 0", r.returncode == 0, f"code={r.returncode}")
        check("发布版正常输出 done", b"done" in r.stdout, f"stdout={r.stdout!r}")


def test_vm_reads_core_do():
    print("普通 VM 无参数读取同目录 core.do")
    if not os.path.exists(VM):
        skip("core.do 读取", "vm.exe 未构建")
        return
    with tempfile.TemporaryDirectory(prefix="dexvm_") as tmp:
        shutil.copy(VM, os.path.join(tmp, "vm.exe"))
        # 用非 GUI 程序(避免窗口),验证 core.do 机制
        src = os.path.join(tmp, "t.dex")
        with open(src, "w", encoding="utf-8") as f:
            f.write('include "math";\nprint dex_add(20, 22);\n')
        r = subprocess.run(
            [sys.executable, os.path.join(ROOT, "main.py"), "compile", src,
             "-o", os.path.join(tmp, "core.do")],
            capture_output=True, timeout=30)
        check("编译 core.do 成功", os.path.exists(os.path.join(tmp, "core.do")))
        r = subprocess.run([os.path.join(tmp, "vm.exe")], cwd=tmp,
                           capture_output=True, text=True, timeout=30,
                           encoding="utf-8", errors="replace")
        check("普通 VM 自动读 core.do 输出 42", r.returncode == 0 and "42" in r.stdout,
              f"code={r.returncode} out={r.stdout!r}")


if __name__ == "__main__":
    test_files_exist()
    test_release_package()
    test_vm_reads_core_do()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败, {SKIP} 跳过")
    sys.exit(1 if FAIL else 0)
