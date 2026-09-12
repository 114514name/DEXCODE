#!/usr/bin/env python3
"""DexStudio(产品 B)模型层 + 宿主的测试。

分三层,都不需要人看屏幕:
  1) **ctypes 直接调 libdexstudio.dll**:项目/场景/撤销/自省/错误路径。模型层
     不依赖窗口,所以 IDE 的核心逻辑可以像 libdexgame 一样被脚本断言。
  2) **宿主的无界面 CLI**:--version / --command / --selftest(见 ds_main.c)。
  3) **WebView2 整条链**:--wv-selftest 把窗口放到屏幕外,等前端发来 ui.ready,
     证明"窗口 → WebView2 → 本地页面 → JS→C→JS"真的通了(装了 WebView2 运行时才跑)。

为什么要测这一层:IDE 的 UI 很难自动化,但**模型与生成物**可以 —— 只要把
状态与命令通道放在 C 侧,端到端行为就能被钉住(见 docs/DEXGAME_DESIGN.md §9.1 决策 #10)。

运行: python tests/test_dexstudio.py
"""

import ctypes
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _tmpdir import tempdir  # noqa: E402

HOST = os.path.join(ROOT, "dexstudio", "host")
DLL = os.path.join(HOST, "libdexstudio.dll")
EXE = os.path.join(HOST, "dexstudio.exe" if os.name == "nt" else "dexstudio")
DEXC = os.path.join(ROOT, "tools", "dexc", "dexc.exe")
LIBS = os.path.join(ROOT, "libs")

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


# ------------------------------------------------------------------ ctypes 绑定

def load_model():
    if not os.path.exists(DLL):
        return None
    if os.name == "nt":
        try:
            os.add_dll_directory(HOST)          # 让 DLL 能找到引擎
        except (AttributeError, OSError):
            pass
    dll = ctypes.CDLL(DLL)
    dll.ds_version.restype = ctypes.c_char_p
    dll.ds_model_create.restype = ctypes.c_void_p
    dll.ds_model_create.argtypes = [ctypes.c_char_p]
    dll.ds_model_destroy.restype = None
    dll.ds_model_destroy.argtypes = [ctypes.c_void_p]
    dll.ds_command.restype = ctypes.c_char_p
    dll.ds_command.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    dll.ds_model_engine_ok.restype = ctypes.c_int
    dll.ds_model_engine_ok.argtypes = [ctypes.c_void_p]
    dll.ds_model_engine_error.restype = ctypes.c_char_p
    dll.ds_model_engine_error.argtypes = [ctypes.c_void_p]
    return dll


class Model:
    """ds_command 的 Python 包装:把 JSON 收回来并断言 ok/error。"""

    def __init__(self, dll, exe_dir=HOST):
        self.dll = dll
        self.h = dll.ds_model_create(exe_dir.encode("utf-8"))
        self.seq = 0

    def close(self):
        if self.h:
            self.dll.ds_model_destroy(self.h)
            self.h = None

    def raw(self, cmd, args=None):
        self.seq += 1
        req = {"id": self.seq, "cmd": cmd}
        if args is not None:
            req["args"] = args
        txt = json.dumps(req, ensure_ascii=False)
        out = self.dll.ds_command(self.h, txt.encode("utf-8"))
        return json.loads(out.decode("utf-8"))

    def ok(self, cmd, args=None):
        r = self.raw(cmd, args)
        if not r.get("ok"):
            raise AssertionError(f"{cmd} 失败:{r.get('error')}")
        return r.get("result")

    def err(self, cmd, args=None):
        r = self.raw(cmd, args)
        if r.get("ok"):
            raise AssertionError(f"{cmd} 本应失败,却成功了:{r.get('result')}")
        return r.get("error", "")


# ------------------------------------------------------------------ 测试

def test_version_and_engine(dll):
    print("[模型层:版本与引擎]")
    ver = dll.ds_version().decode()
    check("ds_version 非空", bool(ver), ver)
    m = Model(dll)
    try:
        check("引擎加载成功", dll.ds_model_engine_ok(m.h) == 1,
              dll.ds_model_engine_error(m.h).decode())
        info = m.ok("app.info")
        check("app.info 带版本", info.get("version") == ver, info)
        check("app.info 报告引擎可用", info.get("engine") is True, info)
        check("响应回带请求 id(前端靠它配对)", m.raw("app.info").get("id") == m.seq)
        comps = info.get("components") or []
        check("自省出 8 种组件", len(comps) == 8, comps)
        for want in ("transform", "sprite", "camera", "animation", "collider",
                     "body", "tilemap", "audio"):
            check(f"组件清单含 {want}", want in comps, comps)
    finally:
        m.close()


def test_project_files(dll):
    print("[项目:新建 / 目录约定 / 存盘 / 重开]")
    with tempdir("ds_proj_") as tmp:
        m = Model(dll)
        try:
            r = m.ok("project.new", {"dir": tmp, "name": "demo"})
            check("project.new 返回场景路径", r.get("scene", "").endswith("main.json"), r)
            check("写下了 project.json", os.path.exists(os.path.join(tmp, "project.json")))
            for sub in ("scenes", "scripts", "res"):
                check(f"建了 {sub}/ 目录", os.path.isdir(os.path.join(tmp, sub)))
            check("生成了 scripts/main.dex",
                  os.path.exists(os.path.join(tmp, "scripts", "main.dex")))
            pj = json.load(open(os.path.join(tmp, "project.json"), encoding="utf-8"))
            check("project.json 名字对", pj.get("name") == "demo", pj)
            check("project.json 起始场景对", pj.get("start_scene") == "scenes/main.json", pj)
            check("project.json 记录运行器",
                  pj.get("run", {}).get("dexc", "").endswith("dexc.exe"), pj)
            # 生成的入口脚本必须**真能编译**(不然"新建项目"就是半成品)
            if os.path.exists(DEXC):
                r2 = subprocess.run([DEXC, "compile", os.path.join(tmp, "scripts", "main.dex"),
                                     "-L", LIBS, "--no-asm"],
                                    capture_output=True, text=True, encoding="utf-8",
                                    errors="replace")
                check("模板 main.dex 能编译", r2.returncode == 0, (r2.stderr or "")[:300])
                for q in ("main.dexbc",):
                    p = os.path.join(tmp, "scripts", q)
                    if os.path.exists(p):
                        os.remove(p)
            else:
                skip("模板 main.dex 能编译", "dexc.exe 未构建")
            # 加个实体再存盘,重开后应该还在
            eid = m.ok("entity.add", {"name": "player"})["id"]
            m.ok("entity.set_pos", {"id": eid, "x": 12, "y": 34})
            m.ok("project.save")
            check("存盘后场景文件存在",
                  os.path.exists(os.path.join(tmp, "scenes", "main.json")))
            m.ok("project.open", {"dir": tmp})
            lst = m.ok("entity.list")
            check("重开后实体还在", len(lst) == 1 and lst[0]["name"] == "player", lst)
            check("重开后位置对", lst[0]["x"] == 12 and lst[0]["y"] == 34, lst)
            check("project.state 列出场景", "main.json" in (m.ok("project.state").get("scenes") or []),
                  m.ok("project.state"))
        finally:
            m.close()


def test_entities_and_components(dll):
    print("[实体与组件]")
    with tempdir("ds_ent_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "ent"})
            a = m.ok("entity.add", {"name": "player"})["id"]
            b = m.ok("entity.add", {"name": "coin"})["id"]
            check("两个实体 id 不同", a != b, (a, b))
            lst = m.ok("entity.list")
            check("按创建顺序枚举", [e["name"] for e in lst] == ["player", "coin"], lst)
            check("entity.find 找得到", m.ok("entity.find", {"name": "coin"})["id"] == b)
            check("entity.find 找不到返回 0", m.ok("entity.find", {"name": "nope"})["id"] == 0)
            d = m.ok("entity.get", {"id": a})
            check("新实体自带 transform", "transform" in d["comps"], d)
            check("transform 有 x/y 字段", "x" in d["comps"]["transform"], d)
            m.ok("comp.add", {"id": a, "comp": "sprite"})
            check("挂上 sprite", "sprite" in m.ok("entity.get", {"id": a})["comps"])
            m.ok("comp.set", {"id": a, "comp": "sprite", "field": "sx", "value": 3.5})
            check("comp.set 生效(float)",
                  m.ok("entity.get", {"id": a})["comps"]["sprite"]["sx"] == 3.5)
            ATLAS = os.path.join(ROOT, "tests", "fixtures", "atlas2x2.png")
            m.ok("comp.set", {"id": a, "comp": "sprite", "field": "tex_path",
                              "value": ATLAS})
            check("comp.set 生效(string)",
                  m.ok("entity.get", {"id": a})["comps"]["sprite"]["tex_path"] == ATLAS)
            # 贴图不存在时引擎会**带原因失败**(IDE 的属性面板要把它显示出来)
            e = m.err("comp.set", {"id": a, "comp": "sprite", "field": "tex_path",
                                   "value": os.path.join(ROOT, "no_such.png")})
            check("贴图路径错时带原因", "cannot open image" in e, e)
            m.ok("comp.add", {"id": a, "comp": "camera"})
            m.ok("comp.remove", {"id": a, "comp": "camera"})
            check("comp.remove 生效", "camera" not in m.ok("entity.get", {"id": a})["comps"])
            m.ok("entity.rename", {"id": b, "name": "金币"})
            check("改名支持非 ASCII", m.ok("entity.find", {"name": "金币"})["id"] == b)
            m.ok("entity.remove", {"id": a})
            lst = m.ok("entity.list")
            check("删除后只剩一个", len(lst) == 1 and lst[0]["id"] == b, lst)
            check("删掉的实体查不到", m.raw("entity.get", {"id": a})["ok"] is False)
        finally:
            m.close()


def test_schema(dll):
    print("[组件自省(属性面板的数据源)]")
    m = Model(dll)
    try:
        schema = m.ok("comp.schema")
        by_name = {c["name"]: c for c in schema}
        check("transform 字段齐全", [f["name"] for f in by_name["transform"]["fields"]]
              == ["x", "y", "rot", "sx", "sy", "parent"], by_name["transform"])
        tf = {f["name"]: f for f in by_name["transform"]["fields"]}
        check("x 是 float 且要存盘", tf["x"]["type"] == "float" and tf["x"]["persist"] is True)
        check("parent 是 int", tf["parent"]["type"] == "int")
        sf = {f["name"]: f for f in by_name["sprite"]["fields"]}
        check("texture 运行期字段不存盘", sf["texture"]["persist"] is False, sf)
        check("tilemap 组件在(8 种全列出)", "tilemap" in by_name and "audio" in by_name,
              list(by_name))
        for c in schema:
            check(f"{c['name']} 有字段", len(c["fields"]) > 0, c)
    finally:
        m.close()


def test_undo_redo(dll):
    print("[撤销 / 重做]")
    with tempdir("ds_undo_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "u"})
            info = m.ok("app.info")
            check("初始撤销栈为空", info["undo"] == 0 and info["redo"] == 0, info)
            eid = m.ok("entity.add", {"name": "a"})["id"]
            check("一次编辑后 undo=1", m.ok("app.info")["undo"] == 1)
            m.ok("entity.set_pos", {"id": eid, "x": 5})
            m.ok("comp.add", {"id": eid, "comp": "sprite"})
            check("三次编辑后 undo=3", m.ok("app.info")["undo"] == 3)

            # 注意:撤销是"把整份场景 JSON 恢复回去",实体因此会被**重新创建**,
            # 运行时 id 会变(引擎的 id 带世代号)。所以每次撤销后要按名字重新查 ——
            # 这一点对 IDE 同样成立:撤销后必须刷新实体列表,不能抱着旧 id。
            m.ok("undo")
            eid = m.ok("entity.find", {"name": "a"})["id"]
            check("undo 撤掉 comp.add(且 id 已更新)",
                  "sprite" not in m.ok("entity.get", {"id": eid})["comps"])
            m.ok("undo")
            eid = m.ok("entity.find", {"name": "a"})["id"]
            check("再 undo 撤掉 set_pos",
                  m.ok("entity.get", {"id": eid})["comps"]["transform"]["x"] == 0)
            m.ok("redo")
            eid = m.ok("entity.find", {"name": "a"})["id"]
            check("redo 回到 x=5",
                  m.ok("entity.get", {"id": eid})["comps"]["transform"]["x"] == 5)
            m.ok("redo")
            eid = m.ok("entity.find", {"name": "a"})["id"]
            check("redo 回到有 sprite",
                  "sprite" in m.ok("entity.get", {"id": eid})["comps"])
            check("新编辑清空 redo 栈",
                  (m.ok("entity.add", {"name": "b"}) is not None)
                  and m.ok("app.info")["redo"] == 0)
            for _ in range(12):
                m.raw("undo")
            check("撤到底后 undo=0", m.ok("app.info")["undo"] == 0)
            check("撤到底后场景是空的", len(m.ok("entity.list")) == 0, m.ok("entity.list"))
            e = m.err("undo")
            check("没得撤时给出原因", "没有可撤销" in e, e)
            for _ in range(12):
                m.raw("redo")
            check("重做到头后 redo=0", m.ok("app.info")["redo"] == 0)
            e = m.err("redo")
            check("没得重做时给出原因", "没有可重做" in e, e)
        finally:
            m.close()


def test_errors(dll):
    print("[错误路径必须带得出原因]")
    m = Model(dll)
    try:
        e = m.err("entity.get", {"id": 999999})
        check("无效实体说'不存在'", "不存在" in e, e)
        e = m.err("nosuchcmd")
        check("未知命令回显命令名", "未知命令" in e and "nosuchcmd" in e, e)
        e = m.err("comp.set", {"id": 1, "comp": "transform", "field": "nope", "value": 1})
        check("未知字段说清楚", "没有字段" in e, e)
        e = m.err("entity.set_pos")
        check("缺参数说清楚", "args.id" in e, e)
        # 坏 JSON(绕过 json.dumps,直接发字符串)
        out = m.dll.ds_command(m.h, b"{not json}")
        r = json.loads(out.decode())
        check("坏 JSON 不崩且带原因", r.get("ok") is False and r.get("error"), r)
        out = m.dll.ds_command(m.h, b'{"cmd":"entity.add","args":{"id":"bad"}}')
        check("参数类型不对也不崩", json.loads(out.decode()) is not None)
    finally:
        m.close()


def test_scene_json(dll):
    print("[场景 JSON 就是引擎的格式]")
    with tempdir("ds_json_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "j"})
            eid = m.ok("entity.add", {"name": "e1"})["id"]
            m.ok("comp.add", {"id": eid, "comp": "sprite"})
            m.ok("entity.set_pos", {"id": eid, "x": 7, "y": 8})
            dom = m.ok("scene.json")
            check("是对象且有 objects/format",
                  isinstance(dom, dict) and "objects" in dom and "format" in dom, dom)
            ents = dom.get("objects") or []
            check("实体写进了 JSON", len(ents) == 1 and ents[0].get("name") == "e1", ents)
            check("count 与 objects 一致", dom.get("count") == len(ents), dom.get("count"))
            check("组件按名字分组", "transform" in ents[0] and "sprite" in ents[0], ents[0])
            m.ok("scene.save")
            with open(os.path.join(tmp, "scenes", "main.json"), encoding="utf-8") as f:
                dom2 = json.load(f)
            check("落盘文件与内存一致", dom2.get("objects") == ents, dom2)
            m.ok("scene.new", {"name": "second"})
            check("新场景是空的", len(m.ok("scene.json").get("objects")) == 0)
            check("场景列表有第二个", "second.json" in m.ok("project.state")["scenes"])
            m.ok("scene.load", {"path": "scenes/main.json"})
            check("切回老场景实体回来", len(m.ok("scene.json").get("objects")) == 1)
        finally:
            m.close()


def test_cli():
    print("[宿主 CLI(不开窗口)]")
    if not os.path.exists(EXE):
        skip("CLI", "dexstudio.exe 未构建")
        return
    r = subprocess.run([EXE, "--version"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    check("--version", r.returncode == 0 and "DexStudio" in (r.stdout or ""), r.stdout)
    r = subprocess.run([EXE, "--command", '{"cmd":"app.info"}'], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    check("--command app.info", r.returncode == 0 and '"ok":true' in r.stdout, r.stdout)
    r = subprocess.run([EXE, "--command", '{"cmd":"nosuch"}'], capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    check("--command 未知命令仍返回 JSON", '"ok":false' in r.stdout, r.stdout)
    r = subprocess.run([EXE, "--help"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    check("--help", r.returncode == 0 and "selftest" in r.stdout, r.stdout)
    r = subprocess.run([EXE, "--selftest"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    check("--selftest 全过", r.returncode == 0 and "0 失败" in r.stdout,
          (r.stdout or "")[-400:])
    check("--selftest 至少 14 项", "14 通过" in r.stdout, r.stdout)


def test_webview_chain():
    print("[WebView2 整条链(窗口在屏幕外)]")
    if not os.path.exists(EXE):
        skip("WebView2 链", "dexstudio.exe 未构建")
        return
    r = subprocess.run([EXE, "--webview-version"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        skip("WebView2 链", (r.stdout or r.stderr).strip()[:80])
        return
    check("WebView2 运行时版本可查", r.stdout.strip().count(".") >= 2, r.stdout)
    r = subprocess.run([EXE, "--wv-selftest"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=120)
    check("窗口 + 本地页面 + JS↔C 往返通", r.returncode == 0, (r.stdout or "")[-400:])
    check("自测报告 PASS", "PASS" in (r.stdout or ""), r.stdout)


def main():
    print("DexStudio(产品 B)模型层与宿主测试")
    dll = load_model()
    if dll is None:
        skip("全部模型层测试", "libdexstudio.dll 未构建,先跑 python main.py build-dexstudio")
        test_cli()
        test_webview_chain()
        print(f"\n结果: {PASS} 通过, {FAIL} 失败" + (f", {SKIP} 跳过" if SKIP else ""))
        return 1 if FAIL else 0
    test_version_and_engine(dll)
    test_project_files(dll)
    test_entities_and_components(dll)
    test_schema(dll)
    test_undo_redo(dll)
    test_errors(dll)
    test_scene_json(dll)
    test_cli()
    test_webview_chain()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败" + (f", {SKIP} 跳过" if SKIP else ""))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
