#!/usr/bin/env python3
"""把一个最小可用的 WebView2 SDK 取进 dexstudio/host/third_party/webview2/。

为什么要 vendor(而不是让使用者去装 SDK):决策 #3 —— clone 即可构建、离线可用。
为什么只取这几样:我们**不链接** WebView2Loader.lib(IID 由 WebView2.h 自己给出,
加载器 DLL 运行时 LoadLibrary),所以整包(~9 MB,含各架构原生库与 .NET 程序集)
里只需要:

    build/native/include/WebView2.h                    ← 全部 COM 接口声明
    build/native/include/WebView2EnvironmentOptions.h  ← 环境选项头(头文件里会 include)
    build/native/x64/WebView2Loader.dll                ← 运行时加载器(163 KB)

另外仓库里还有一个手写的 `include/EventToken.h`:Windows SDK 有、mingw/zig 没有,
WebView2.h 会 include 它(只为 EventRegistrationToken 一个类型)。

用法(升级 SDK 时跑一次,然后把改动提交):
    python dexstudio/host/fetch_webview2_sdk.py            # 取最新稳定版
    python dexstudio/host/fetch_webview2_sdk.py 1.0.2903.40  # 指定版本
"""

import io
import json
import os
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "third_party", "webview2")
IDX = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/index.json"
PKG = ("https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/"
       "{v}/microsoft.web.webview2.{v}.nupkg")

WANT = [
    ("build/native/include/WebView2.h", "include/WebView2.h"),
    ("build/native/include/WebView2EnvironmentOptions.h",
     "include/WebView2EnvironmentOptions.h"),
    ("build/native/x64/WebView2Loader.dll", "x64/WebView2Loader.dll"),
]


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "dexstudio-sdk-fetch/1.0"})
    with urllib.request.urlopen(req, timeout=180) as r:
        return r.read()


def main(argv):
    ver = argv[1] if len(argv) > 1 else None
    if not ver:
        idx = json.loads(fetch(IDX).decode("utf-8"))
        stable = [v for v in idx.get("versions", []) if "-" not in v]
        if not stable:
            print("拿不到版本列表", file=sys.stderr)
            return 1
        ver = stable[-1]
    print("WebView2 SDK 版本:", ver)
    blob = fetch(PKG.format(v=ver))
    print("nupkg 已下载 %.1f MB" % (len(blob) / 1048576.0))
    zf = zipfile.ZipFile(io.BytesIO(blob))
    names = set(zf.namelist())
    ok = True
    for src, rel in WANT:
        if src not in names:
            print("  !! 包里没有", src, file=sys.stderr)
            ok = False
            continue
        dst = os.path.join(OUT, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        data = zf.read(src)
        with open(dst, "wb") as f:
            f.write(data)
        print("  %-52s %8d bytes" % (rel, len(data)))
    with open(os.path.join(OUT, "VERSION.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("Microsoft.Web.WebView2 %s\n" % ver)
        f.write("来源:%s\n" % PKG.format(v=ver))
        f.write("只保留 build/native/include/ 的两个头 + x64 的 WebView2Loader.dll。\n")
        f.write("include/EventToken.h 是本仓库手写的补丁(mingw/zig 没有 Windows SDK 的该头)。\n")
        f.write("重新获取:python dexstudio/host/fetch_webview2_sdk.py\n")
    print("完成:", OUT)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
