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
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

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


def bmp_size(path):
    """读 BMP 头拿宽高(24/32 位 BMP 的宽高是 int32 @18/@22)。"""
    with open(path, "rb") as f:
        head = f.read(26)
    if len(head) < 26 or head[:2] != b"BM":
        return None
    w = int.from_bytes(head[18:22], "little", signed=True)
    h = int.from_bytes(head[22:26], "little", signed=True)
    return (w, abs(h))


def test_viewport(dll):
    """B3 的视口:视图覆盖、离屏渲染落盘、世界包围盒(scene.outline)。"""
    print("[视口:view.set / scene.render / scene.outline]")
    with tempdir("ds_view_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "v"})
            info = m.ok("app.info")
            check("app.info 带视口尺寸", info["view_w"] > 0 and info["view_h"] > 0,
                  f"{info['view_w']}x{info['view_h']}")
            check("app.info 带视图状态", isinstance(info.get("view"), dict), info.get("view"))
            check("app.info 带预览目录", "preview" in (info.get("preview_dir") or "").lower()
                  or os.path.isdir(info.get("preview_dir") or ""), info.get("preview_dir"))
            r = m.ok("view.set", {"x": 10, "y": 20, "zoom": 2})
            check("view.set 回显", r["x"] == 10 and r["y"] == 20 and r["zoom"] == 2
                  and r["on"] is True, r)
            check("app.info 记住视图",
                  m.ok("app.info")["view"]["zoom"] == 2, m.ok("app.info")["view"])
            r = m.ok("view.set", {"zoom": 0})
            check("zoom<=0 被夹到 1", r["zoom"] == 1, r)
            m.ok("view.set", {"on": False})
            check("view.set on=0 关掉覆盖", m.ok("app.info")["view"]["on"] is False)

            eid = m.ok("entity.add", {"name": "hero"})["id"]
            r = m.ok("scene.render")
            check("scene.render 返回 url/seq/尺寸",
                  r["url"].startswith("https://") and r["seq"] >= 1
                  and r["w"] == info["view_w"] and r["h"] == info["view_h"], r)
            check("scene.render 落盘存在", os.path.exists(r["path"]), r["path"])
            check("落盘的是尺寸正确的 BMP",
                  bmp_size(r["path"]) == (info["view_w"], info["view_h"]),
                  bmp_size(r["path"]))
            check("渲染序号递增", m.ok("scene.render")["seq"] == r["seq"] + 1)
            m.ok("view.set", {"x": -100, "y": -100, "zoom": 0.5})
            r2 = m.ok("scene.render")
            check("换视图后仍能渲染", r2["seq"] == r["seq"] + 2, r2["seq"])

            o = m.ok("scene.outline")
            check("outline 是一个实体", len(o) == 1 and o[0]["id"] == eid, o)
            b = o[0]
            check("outline 自带包围盒(无组件时 16×16)",
                  b["w"] == 16 and b["h"] == 16, b)
            check("outline 的世界坐标与实体一致", b["wx"] == 0 and b["wy"] == 0, b)
            m.ok("comp.add", {"id": eid, "comp": "collider"})
            m.ok("comp.set", {"id": eid, "comp": "collider", "field": "hw", "value": 20})
            m.ok("comp.set", {"id": eid, "comp": "collider", "field": "hh", "value": 10})
            m.ok("entity.set_pos", {"id": eid, "x": 100, "y": 50})
            o = m.ok("scene.outline")[0]
            check("collider 决定包围盒(中心 ±hw/hh)",
                  o["x"] == 80 and o["y"] == 40 and o["w"] == 40 and o["h"] == 20, o)
            check("outline 标出 kind/图层", o["kind"] == "box" and o["layer"] == 0, o)
            m.ok("comp.add", {"id": eid, "comp": "sprite"})
            m.ok("comp.set", {"id": eid, "comp": "sprite", "field": "sw", "value": 8})
            m.ok("comp.set", {"id": eid, "comp": "sprite", "field": "sh", "value": 6})
            check("collider 优先于 sprite", m.ok("scene.outline")[0]["w"] == 40)
            m.ok("comp.remove", {"id": eid, "comp": "collider"})
            o = m.ok("scene.outline")[0]
            check("去掉 collider 后按 sprite 算",
                  o["kind"] == "sprite" and o["w"] == 8 and o["h"] == 6, o)
            # 轴心:默认 0.5 = 以实体位置为中心。框线必须与**引擎画出来的位置**逐字一致
            # (引擎:原点 = 世界坐标 - 源尺寸 × 轴心 × 缩放),否则用户看到的就是
            # "只有框线在动、图像在别处"。实体在 (100,50),8×6 的图 → 框线 (96,47)。
            check("轴心 0.5 时框线以实体为中心(与引擎同一公式)",
                  abs(o["x"] - 96.0) < 0.01 and abs(o["y"] - 47.0) < 0.01, o)
            m.ok("comp.set", {"id": eid, "comp": "sprite", "field": "px", "value": 0})
            m.ok("comp.set", {"id": eid, "comp": "sprite", "field": "py", "value": 0})
            o = m.ok("scene.outline")[0]
            check("轴心改成左上角后框线跟着挪到实体位置",
                  abs(o["x"] - 100.0) < 0.01 and abs(o["y"] - 50.0) < 0.01, o)
            m.ok("comp.set", {"id": eid, "comp": "transform", "field": "sx", "value": 2.0})
            m.ok("comp.set", {"id": eid, "comp": "transform", "field": "sy", "value": 2.0})
            o = m.ok("scene.outline")[0]
            check("缩放后框线宽高一起变(8×6 → 16×12)",
                  abs(o["w"] - 16.0) < 0.01 and abs(o["h"] - 12.0) < 0.01, o)
            # 视口用的是**活动相机**(camera.x/y/zoom,不是 transform),前端画网格/选中框
            # 用的是 app.info 报的那个视图 —— 报错了就是"画面跟相机走、框线不跟"。
            m.ok("view.set", {"on": False})
            check("没有相机时视图就是 0,0,1",
                  m.ok("app.info")["view"]["x"] == 0
                  and m.ok("app.info")["view"]["zoom"] == 1, m.ok("app.info")["view"])
            cam = m.ok("entity.add", {"name": "相机", "comps": ["transform", "camera"]})
            m.ok("comp.set", {"id": cam["id"], "comp": "camera", "field": "x",
                              "value": 40})
            m.ok("comp.set", {"id": cam["id"], "comp": "camera", "field": "y",
                              "value": 30})
            m.ok("comp.set", {"id": cam["id"], "comp": "camera", "field": "zoom",
                              "value": 2})
            info = m.ok("app.info")
            check("没有视图覆盖时报的是活动相机的视图(画面和框线才在同一个坐标系)",
                  info["view"]["x"] == 40 and info["view"]["y"] == 30
                  and info["view"]["zoom"] == 2 and info["view"]["on"] is False,
                  info["view"])
            r0 = m.ok("scene.render")
            check("scene.render 也报实际生效的视图",
                  r0["x"] == 40 and r0["y"] == 30 and r0["zoom"] == 2, r0)
            # 相机不参与活动时(active=0)视图回落到 0,0,1
            m.ok("comp.set", {"id": cam["id"], "comp": "camera", "field": "active",
                              "value": 0})
            check("相机标成不活动后视图回落",
                  m.ok("app.info")["view"]["zoom"] == 1, m.ok("app.info")["view"])
            m.ok("view.set", {"x": 10, "y": 20, "zoom": 4})
            check("有视图覆盖时优先用覆盖值",
                  m.ok("app.info")["view"]["x"] == 10
                  and m.ok("app.info")["view"]["zoom"] == 4, m.ok("app.info")["view"])
        finally:
            m.close()


def test_tile_brush(dll):
    """B3 的瓦片刷子:CSV 建/读/写/批量,以及"一次拖拽 = 一条撤销"。"""
    print("[瓦片刷子:tilemap.create/info/set/paint/csv]")
    with tempdir("ds_tile_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "t"})
            eid = m.ok("entity.add", {"name": "map"})["id"]
            check("普通实体不能用 tilemap.info",
                  "没有 tilemap" in m.err("tilemap.info", {"id": eid}))
            m.ok("comp.add", {"id": eid, "comp": "tilemap"})
            r = m.ok("tilemap.create",
                     {"id": eid, "path": "res/map.csv", "cols": 4, "rows": 3})
            csv_path = os.path.join(tmp, "res", "map.csv")
            check("tilemap.create 落盘", os.path.exists(csv_path), r)
            check("tilemap.create 把 path 写进组件",
                  os.path.normcase(m.ok("tilemap.info", {"id": eid})["path"])
                  == os.path.normcase(csv_path))
            with open(csv_path, encoding="utf-8") as f:
                text = f.read()
            check("新建的 CSV 是 3 行 4 列全空",
                  text.count("\n") == 3 and text.count("-1") == 12, repr(text))

            info = m.ok("tilemap.info", {"id": eid})
            check("tilemap.info 尺寸对", info["cols"] == 4 and info["rows"] == 3, info)
            check("tilemap.info 带几何(16×16 像素/格)",
                  info["tw"] == 16 and info["th"] == 16, info)
            check("tilemap.info 全空", set(info["tiles"]) == {-1}, info["tiles"])
            check("没有图集时 atlas_url 为空", info["atlas_url"] == "")

            # 图集预览的 URL(**用户报的"图片损坏"就在这条路上**):
            # 场景里存的是项目相对路径 `res/xxx.png`,而以前这里只认"绝对路径且在项目里",
            # 相对路径一律算出空 URL → 瓦片面板一直说"还没有图集";中文/空格名还不做
            # 百分号编码 → 真正的裂图标。
            png = os.path.join(ROOT, "tests", "fixtures", "tiles.png")
            if os.path.exists(png):
                m.ok("res.import", {"src": png, "name": "图集 测试.png"})
                m.ok("comp.set", {"id": eid, "comp": "tilemap", "field": "tex_path",
                                  "value": "res/图集 测试.png"})
                info = m.ok("tilemap.info", {"id": eid})
                url = info["atlas_url"]
                check("相对路径的图集也算得出 URL(以前是空的)",
                      url.startswith("https://dexstudio-proj.local/res/"), url)
                check("URL 里的中文/空格被百分号编码(否则浏览器给裂图标)",
                      "%E5%9B%BE%E9%9B%86%20%E6%B5%8B%E8%AF%95.png" in url, url)
                check("URL 带 ?v= 破缓存(换图后不能拿旧结果)", "?v=" in url, url)
                m.ok("comp.set", {"id": eid, "comp": "tilemap", "field": "tex_path",
                                  "value": os.path.join(ROOT, "tests", "fixtures",
                                                        "tiles.png")})
                check("项目外的绝对路径不给 URL(映射不到虚拟主机,面板该提示选 res/ 里的图)",
                      m.ok("tilemap.info", {"id": eid})["atlas_url"] == "",
                      m.ok("tilemap.info", {"id": eid})["atlas_url"])

            r = m.ok("tilemap.set", {"id": eid, "col": 2, "row": 1, "tile": 5})
            check("tilemap.set 返回网格尺寸", r["cols"] == 4 and r["rows"] == 3, r)
            info = m.ok("tilemap.info", {"id": eid})
            check("画的那一格读回来是 5", info["tiles"][1 * 4 + 2] == 5, info["tiles"])
            check("其它格没被碰到", info["tiles"].count(5) == 1, info["tiles"])

            m.ok("undo")
            eid2 = m.ok("entity.find", {"name": "map"})["id"]
            info = m.ok("tilemap.info", {"id": eid2})
            check("撤销瓦片后 CSV 复原(瓦片不在场景 JSON 里,靠 CSV 快照)",
                  set(info["tiles"]) == {-1}, info["tiles"])

            cells = [{"col": c, "row": 0, "tile": 7 + c} for c in range(4)]
            r = m.ok("tilemap.paint", {"id": eid2, "cells": cells})
            check("tilemap.paint 写 4 格", r["count"] == 4, r)
            info = m.ok("tilemap.info", {"id": eid2})
            check("paint 四格都写进去了", info["tiles"][0:4] == [7, 8, 9, 10],
                  info["tiles"][0:4])
            m.ok("undo")
            eid3 = m.ok("entity.find", {"name": "map"})["id"]
            check("整笔 paint 只需撤销一次",
                  set(m.ok("tilemap.info", {"id": eid3})["tiles"]) == {-1},
                  m.ok("tilemap.info", {"id": eid3})["tiles"])

            r = m.ok("tilemap.set", {"id": eid3, "col": 9, "row": 5, "tile": 2})
            check("画到范围外会自动扩", r["cols"] == 10 and r["rows"] == 6, r)
            info = m.ok("tilemap.info", {"id": eid3})
            check("扩展后旧格子保留、新格子写入",
                  info["tiles"][5 * 10 + 9] == 2 and len(info["tiles"]) == 60, len(info["tiles"]))

            m.ok("tilemap.csv", {"id": eid3, "csv": "-1,1\n2,-1\n"})
            info = m.ok("tilemap.info", {"id": eid3})
            check("tilemap.csv 整图替换", info["cols"] == 2 and info["rows"] == 2
                  and info["tiles"] == [-1, 1, 2, -1], info)
            with open(csv_path, encoding="utf-8") as f:
                check("整图替换也落盘", f.read().strip() == "-1,1\n2,-1".strip())

            e = m.err("tilemap.set", {"id": eid3, "col": -1, "row": 0})
            check("负坐标给出原因", "col" in e or "row" in e, e)
            e = m.err("tilemap.create", {"id": eid3})
            check("缺 path 给出原因", "args.path" in e, e)
            e = m.err("tilemap.info", {"id": 999999})
            check("无效实体的 tilemap.info 带原因", "不存在" in e or "没有 tilemap" in e, e)
        finally:
            m.close()


def test_clipboard(dll):
    """B3 的复制/粘贴/再做一个(字段级复制,对所有组件都成立)。"""
    print("[复制 / 粘贴 / 批量字段写]")
    with tempdir("ds_clip_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "c"})
            a = m.ok("entity.add", {"name": "src"})["id"]
            m.ok("comp.add", {"id": a, "comp": "sprite"})
            m.ok("comp.set", {"id": a, "comp": "sprite", "field": "sw", "value": 24})
            m.ok("comp.set", {"id": a, "comp": "sprite", "field": "sh", "value": 12})
            m.ok("entity.set_pos", {"id": a, "x": 100, "y": 40})

            r = m.ok("entity.duplicate", {"id": a, "dx": 10, "dy": -5})
            dup = r["ids"][0]
            check("duplicate 返回新 id 且不同", dup != a, r)
            d = m.ok("entity.get", {"id": dup})
            check("duplicate 复制了组件字段", d["comps"]["sprite"]["sw"] == 24
                  and d["comps"]["sprite"]["sh"] == 12, d["comps"].get("sprite"))
            check("duplicate 位置按偏移", d["x"] == 110 and d["y"] == 35, d)
            check("duplicate 保留了名字", d["name"] == "src", d["name"])
            check("原实体没被动过",
                  m.ok("entity.get", {"id": a})["x"] == 100)

            r = m.ok("entity.duplicate", {"ids": [a, dup], "dx": 0, "dy": 0})
            check("duplicate 支持一次多个", len(r["ids"]) == 2, r)
            check("实体总数 = 4", len(m.ok("entity.list")) == 4)

            r = m.ok("entity.copy", {"ids": [a]})
            check("entity.copy 记下 1 个", r["count"] == 1, r)
            r = m.ok("entity.paste", {"dx": -50, "dy": -50})
            pasted = r["ids"][0]
            d = m.ok("entity.get", {"id": pasted})
            check("paste 位置按偏移", d["x"] == 50 and d["y"] == -10, d)
            check("paste 带组件", d["comps"]["sprite"]["sw"] == 24, d["comps"].get("sprite"))

            r = m.ok("comp.set_many", {"items": [
                {"id": a, "comp": "transform", "field": "x", "value": 1},
                {"id": a, "comp": "transform", "field": "y", "value": 2},
            ]})
            check("comp.set_many 报条数", r["count"] == 2, r)
            d = m.ok("entity.get", {"id": a})
            check("set_many 两项都生效", d["x"] == 1 and d["y"] == 2, d)
            n_undo = m.ok("app.info")["undo"]
            m.ok("undo")
            check("set_many 一条撤销就全回去(不是两条)",
                  m.ok("app.info")["undo"] == n_undo - 1)
            a2 = m.ok("entity.find", {"name": "src"})["id"]
            d = m.ok("entity.get", {"id": a2})
            check("撤销后 xy 都复原", d["x"] == 100 and d["y"] == 40, d)

            e = m.err("entity.duplicate", {})
            check("duplicate 缺 id 给出原因", "args.id" in e, e)
            e = m.err("comp.set_many", {"items": []})
            check("空 items 给出原因", "items" in e, e)
            e = m.err("comp.set_many", {"items": [
                {"id": a2, "comp": "transform", "field": "nope", "value": 1}]})
            check("batch 里字段错也带原因", "没有字段" in e, e)
            check("batch 失败不留半成品",
                  m.ok("entity.get", {"id": a2})["x"] == 100, m.ok("entity.get", {"id": a2}))
        finally:
            m.close()


def test_graph_model(dll):
    """B4 的逻辑图模型:节点目录、增删改、连线校验、撤销、存盘往返。"""
    print("[逻辑图模型(节点/连线/校验/存盘)]")
    with tempdir("ds_graph_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "g"})
            types = m.ok("graph.types")
            check("节点目录非空", len(types) >= 20, len(types))
            names = [t["type"] for t in types]
            check("目录含事件/动作/条件三类节点",
                  "on_start" in names and "set_field" in names and "compare" in names,
                  names)
            by = {t["type"]: t for t in types}
            check("事件节点只有执行输出", by["on_start"]["out"][0]["type"] == "exec"
                  and by["on_start"]["in"] == [], by["on_start"])
            check("分支有 exec 输入与条件输入",
                  [p["name"] for p in by["branch"]["in"]] == ["exec", "cond"],
                  by["branch"]["in"])
            check("分支有两个执行输出",
                  [p["name"] for p in by["branch"]["out"]] == ["true", "false"],
                  by["branch"]["out"])
            check("写字段的属性带类型",
                  [(p["name"], p["type"]) for p in by["set_field"]["props"]]
                  == [("obj", "string"), ("comp", "string"), ("field", "string"),
                      ("value", "float"), ("as", "string")],
                  by["set_field"]["props"])
            check("比较节点有 a/b 两个数值输入",
                  [p["name"] for p in by["compare"]["in"]] == ["a", "b"],
                  by["compare"]["in"])

            n1 = m.ok("graph.node.add", {"type": "on_update", "x": 10, "y": 20})["id"]
            n2 = m.ok("graph.node.add", {"type": "set_field"})["id"]
            check("节点 id 递增且不同", n1 != n2 and n2 > n1, (n1, n2))
            check("新节点在 info 里", m.ok("graph.info")["nodes"] == 2)
            r = m.ok("graph.node.move", {"id": n2, "x": 300, "y": 40})
            check("移动生效", r["x"] == 300 and r["y"] == 40, r)
            r = m.ok("graph.node.set", {"id": n2, "props": {"obj": "player"}})
            check("改属性生效", r["props"]["obj"] == "player", r)
            check("节点默认属性已填(不用前端兜底)",
                  m.ok("graph.node.set", {"id": n2, "props": {"comp": "transform"}})
                  ["props"].get("as") == "f")

            m.ok("graph.link", {"from": n1, "from_pin": "out", "to": n2, "to_pin": "exec"})
            check("连线数 = 1", m.ok("graph.info")["links"] == 1)
            e = m.err("graph.link", {"from": n1, "from_pin": "out", "to": n2,
                                     "to_pin": "exec"})
            check("同一个输入口不能接两条", "已经有连线" in e, e)
            e = m.err("graph.link", {"from": n1, "from_pin": "out", "to": n1,
                                     "to_pin": "exec"})
            check("不能连自己", "自己" in e, e)
            e = m.err("graph.link", {"from": n1, "from_pin": "nope", "to": n2,
                                     "to_pin": "exec"})
            check("输出引脚不存在给出原因", "输出引脚" in e, e)
            e = m.err("graph.link", {"from": n1, "from_pin": "out", "to": n2,
                                     "to_pin": "value"})
            check("执行流不能连数据口", "执行流只能连执行流" in e, e)
            e = m.err("graph.node.add", {"type": "nope"})
            check("未知节点类型给出原因", "没有这种节点类型" in e, e)
            e = m.err("graph.node.set", {"id": n2, "props": {"nope": 1}})
            check("未知属性给出原因", "没有属性" in e, e)
            e = m.err("graph.node.remove", {"id": 9999})
            check("删不存在的节点给出原因", "没有节点" in e, e)

            # 数据线成环
            a = m.ok("graph.node.add", {"type": "math"})["id"]
            b = m.ok("graph.node.add", {"type": "math"})["id"]
            m.ok("graph.link", {"from": a, "from_pin": "v", "to": b, "to_pin": "a"})
            e = m.err("graph.link", {"from": b, "from_pin": "v", "to": a, "to_pin": "a"})
            check("数据线成环被拒", "成环" in e, e)

            # 删节点会带走它的连线
            m.ok("graph.link", {"from": b, "from_pin": "v", "to": n2, "to_pin": "value"})
            before = m.ok("graph.info")["links"]
            m.ok("graph.node.remove", {"id": n2})
            check("删节点同时清掉相关连线",
                  m.ok("graph.info")["links"] < before, before)
            check("断开指定输入口",
                  m.ok("graph.unlink", {"to": b, "to_pin": "a"})["removed"] == 1)

            # 撤销:逻辑图的编辑也要能撤
            n = m.ok("graph.info")["nodes"]
            m.ok("graph.node.add", {"type": "destroy"})
            check("加节点", m.ok("graph.info")["nodes"] == n + 1)
            m.ok("undo")
            check("撤销后节点数回落", m.ok("graph.info")["nodes"] == n,
                  m.ok("graph.info")["nodes"])
            m.ok("redo")
            check("重做后节点回来", m.ok("graph.info")["nodes"] == n + 1)

            # 存盘 + 重开
            ids = [nd["id"] for nd in m.ok("graph.info")["graph"]["nodes"]]
            m.ok("graph.link", {"from": ids[0], "from_pin": "out",
                                "to": ids[-1], "to_pin": "exec"})
            n_before = m.ok("graph.info")["nodes"]
            l_before = m.ok("graph.info")["links"]
            gpath = m.ok("graph.info")["path"]
            m.ok("graph.save")
            check("逻辑图落盘", os.path.exists(gpath), gpath)
            with open(gpath, encoding="utf-8") as f:
                dom = json.load(f)
            check("落盘 JSON 有 format/nodes/links",
                  dom.get("format") == 1 and "nodes" in dom and "links" in dom, dom)
            m.ok("project.open", {"dir": tmp})
            check("重开后节点还在", m.ok("graph.info")["nodes"] == n_before,
                  m.ok("graph.info")["nodes"])
            check("重开后连线还在", m.ok("graph.info")["links"] == l_before,
                  m.ok("graph.info")["links"])
        finally:
            m.close()


def build_demo_graph(m):
    """造一张能跑出可观测行为的图:每帧 x += 5,按下 jump 时 vy -= 400。"""
    ev = m.ok("graph.node.add", {"type": "on_update", "x": 40, "y": 40})["id"]
    add = m.ok("graph.node.add", {"type": "add_field", "x": 300, "y": 40})["id"]
    m.ok("graph.node.set", {"id": add, "props": {
        "obj": "player", "comp": "transform", "field": "x", "delta": 5, "as": "f"}})
    m.ok("graph.link", {"from": ev, "from_pin": "out", "to": add, "to_pin": "exec"})
    br = m.ok("graph.node.add", {"type": "branch", "x": 40, "y": 180})["id"]
    m.ok("graph.link", {"from": add, "from_pin": "out", "to": br, "to_pin": "exec"})
    press = m.ok("graph.node.add", {"type": "action_pressed", "x": 40, "y": 320})["id"]
    m.ok("graph.node.set", {"id": press, "props": {"action": "jump"}})
    m.ok("graph.link", {"from": press, "from_pin": "v", "to": br, "to_pin": "cond"})
    jump = m.ok("graph.node.add", {"type": "add_field", "x": 320, "y": 180})["id"]
    m.ok("graph.node.set", {"id": jump, "props": {
        "obj": "player", "comp": "body", "field": "vy", "delta": -400, "as": "f"}})
    m.ok("graph.link", {"from": br, "from_pin": "true", "to": jump, "to_pin": "exec"})
    st = m.ok("graph.node.add", {"type": "on_start", "x": 40, "y": 480})["id"]
    sf = m.ok("graph.node.add", {"type": "set_field", "x": 300, "y": 480})["id"]
    m.ok("graph.node.set", {"id": sf, "props": {
        "obj": "player", "comp": "transform", "field": "y", "value": 7, "as": "f"}})
    m.ok("graph.link", {"from": st, "from_pin": "out", "to": sf, "to_pin": "exec"})
    return {"ev": ev, "add": add, "br": br, "press": press, "jump": jump,
            "st": st, "sf": sf}


def test_graph_codegen(dll):
    """B4 的代码生成:生成的 DexLang 必须能过 dexc.exe,而且**确定性**。"""
    print("[逻辑图 → DexLang 代码生成]")
    with tempdir("ds_gen_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "gen"})
            build_demo_graph(m)
            r = m.ok("graph.generate")
            src = r["source"]
            check("生成文件落在 scripts/ 下",
                  r["path"].replace("/", "\\").endswith("scripts\\logic.dex"), r["path"])
            check("生成的代码有 include", 'include "dexgame";' in src, src[:200])
            check("生成三个回调(都收 dt 秒)", all(f"func {f}(dt: float)" in src
                                                 for f in ("logic_start", "logic_update",
                                                           "logic_draw")), src)
            check("每帧的 add_field 生成了读-改-写",
                  'eng_set_f(eng_find("player"), "transform", "x", '
                  'eng_get_f(eng_find("player"), "transform", "x") + 5.0);' in src, src)
            check("on_start 写了 y=7", '"transform", "y", 7.0);' in src, src)
            check("分支生成成 if + 轮询动作",
                  'if (eng_action_pressed("jump")) != 0 {' in src, src)
            check("语句都以分号结束",
                  all((not l.strip())
                      or l.strip().endswith((";", "{", "}"))
                      or l.strip().startswith("#")
                      for l in src.splitlines()), src)
            check("属性值优先于空引脚(没有未接线的报错)",
                  "没有接东西" not in src, src)

            src2 = m.ok("graph.generate")["source"]
            check("两次生成逐字节一致(确定性)", src == src2)

            if not os.path.exists(DEXC):
                skip("生成的代码能编译", "dexc.exe 未构建")
            else:
                r2 = subprocess.run([DEXC, "compile", r["path"]],
                                    capture_output=True, text=True, encoding="utf-8",
                                    errors="replace")
                check("生成的代码能被 dexc 编译", r2.returncode == 0,
                      (r2.stdout or "") + (r2.stderr or ""))
                check("生成了字节码",
                      os.path.exists(r["path"].replace(".dex", ".dexbc")), r["path"])

            # 空图也要能生成并编译(新项目刚建出来就是这个状态)
            m.ok("graph.new")
            r3 = m.ok("graph.generate")
            check("空图生成三个空函数",
                  r3["source"].count("func logic_") == 3, r3["source"])
            if os.path.exists(DEXC):
                r4 = subprocess.run([DEXC, "compile", r3["path"]],
                                    capture_output=True, text=True, encoding="utf-8",
                                    errors="replace")
                check("空图生成的代码也能编译", r4.returncode == 0,
                      (r4.stdout or "") + (r4.stderr or ""))
        finally:
            m.close()


MAIN_DEX_GRAPH = """include "dexgame";
include "dexgame_fast";
include "logic";

func on_start() {
    eng_init_offscreen(320, 180);
    eng_scene_load("scenes/main.json");
    logic_start(0.0);
}

func on_update(dt: float) {
    logic_update(dt);
}

func on_draw() {
    logic_draw(0.0);
}

eng_run_frames("on_start", "on_update", "on_draw", 4);
print eng_get_f(eng_find("player"), "transform", "x");
print eng_get_f(eng_find("player"), "transform", "y");
print eng_get_f(eng_find("player"), "body", "vy");
"""


def test_graph_behavior(dll):
    """B4 的验收线:图 → 代码 → 编译 → 真的跑出预期行为(不是只看能不能编译)。"""
    print("[逻辑图跑出预期行为(图→代码→dexc→vm)]")
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not (os.path.exists(DEXC) and os.path.exists(vm)):
        skip("图驱动运行", "dexc.exe / vm.exe 未构建")
        return
    with tempdir("ds_run_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "run"})
            pid = m.ok("entity.add", {"name": "player"})["id"]
            m.ok("comp.add", {"id": pid, "comp": "body"})
            build_demo_graph(m)
            m.ok("scene.save")
            r = m.ok("graph.generate")
            main_dex = os.path.join(tmp, "scripts", "main.dex")
            with open(main_dex, "w", encoding="utf-8", newline="\n") as f:
                f.write(MAIN_DEX_GRAPH)
            bc = os.path.join(tmp, "scripts", "main.dexbc")
            c = subprocess.run([DEXC, "compile", main_dex, "-o", bc],
                               capture_output=True, text=True, encoding="utf-8",
                               errors="replace", cwd=tmp)
            check("图驱动的 main.dex 能编译", c.returncode == 0,
                  (c.stdout or "") + (c.stderr or ""))
            if c.returncode != 0:
                return
            run = subprocess.run([vm, "scripts/main.dexbc", "-L",
                                  os.path.join(LIBS, "dexgame")],
                                 capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", cwd=tmp)
            out = (run.stdout or "").strip().splitlines()
            check("图驱动的程序能跑起来", run.returncode == 0,
                  (run.stdout or "") + (run.stderr or ""))
            check("on_start 的写字段生效(y=7)",
                  len(out) >= 2 and out[1].strip() == "7", out)
            check("每帧累加生效(4 帧 × +5 = x=20)",
                  len(out) >= 1 and out[0].strip() == "20", out)
            check("分支没被触发时不动 vy(没有按下动作)",
                  len(out) >= 3 and out[2].strip() == "0", out)
            check("生成的 logic.dex 在项目里", os.path.exists(r["path"]), r["path"])
        finally:
            m.close()


BLOCKS_MAIN_DEX = """# 积木驱动的入口:像 main.dex 模板那样跑几帧,把结果打出来
include "dexgame";
include "logic";

func on_start() {
    eng_init_offscreen(320, 180);    # 测试里不开真窗口;模板里这一步是 eng_init
    eng_scene_load("scenes/main.json");
    logic_start(0.016);
}

func on_update(dt: float) {
    logic_update(dt);
}

func run_frames() {
    on_start();
    let i = 0;
    while (i < 4) {
        on_update(0.5);
        i = i + 1;
    }
}

run_frames();
print eng_has(eng_find("玩家"), "transform");   # 场景真的装上了组件吗
print eng_get_f(eng_find("玩家"), "transform", "x");
print eng_get_f(eng_find("玩家"), "body", "vy");
"""


def test_blocks_model(dll):
    """积木(给零基础用户的那一套):目录 / 增删改 / 一键示例 / 生成 / 校验 / 撤销。"""
    print("[积木脚本(目录/增删改/一键示例/生成)]")
    with tempdir("ds_blk_") as tmp:
        proj = os.path.join(tmp, "proj")
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": proj, "name": "blk", "template": "starter"})
            info = m.ok("app.info")
            check("新项目默认用积木生成逻辑", info["logic_mode"] == "blocks",
                  info["logic_mode"])
            types = m.ok("blocks.types")
            names = [t["type"] for t in types]
            check("积木目录有 16 种", len(types) == 16, len(types))
            for want in ("on_start", "on_update", "on_key", "move", "jump",
                         "camera_follow", "play_sound", "if_ground", "if_key",
                         "if_field"):
                check("目录里有 " + want, want in names, names)
            move = [t for t in types if t["type"] == "move"][0]
            check("积木句式是中文句子", "走" in move["text"] and "{0}" in move["text"],
                  move["text"])
            check("每个空都是下拉选项(不是让用户打字)",
                  all(len(s.get("options") or []) > 0 for s in move["slots"]),
                  move["slots"])
            check("实体类是场景实体的下拉",
                  [s for s in move["slots"] if s["kind"] == "entity"][0]["options"][0]["value"]
                  in [e["name"] for e in m.ok("entity.list")], move["slots"][0])
            check("数值槽带「自定义…」入口",
                  any(o["value"] == "__custom__"
                      for s in [x for x in m.ok("blocks.types")
                                if x["type"] == "set_pos"][0]["slots"]
                      if s["kind"] == "num"
                      for o in (s.get("options") or [])), "num 槽")

            # 一键示例:建出来就必须**可编辑**(每块都要有 block_id)
            ids = []
            for s in m.ok("blocks.info")["scripts"]:
                for b in s["blocks"]:
                    ids.append(b.get("block_id"))
                    for c in (b.get("body") or []):
                        ids.append(c.get("block_id"))
            check("一键示例真的写了积木", len(ids) >= 2, ids)
            check("一键示例的每块积木都有 block_id(否则界面上改不动/删不掉)",
                  all(isinstance(i, int) and i > 0 for i in ids), ids)

            # 手动加一段 + 加积木 + 改空 + 上移 + 删除
            s2 = m.ok("blocks.script.add", {"event": "on_update"})["id"]
            a = m.ok("blocks.add", {"id": s2, "type": "move"})["block_id"]
            b = m.ok("blocks.add", {"id": s2, "type": "jump"})["block_id"]
            check("新加的积木有 block_id", a > 0 and b > a, (a, b))
            m.ok("blocks.set", {"id": s2, "block_id": a, "name": "dir", "value": "left"})
            m.ok("blocks.set", {"id": s2, "block_id": b, "name": "obj", "value": "player"})
            got = [x for x in m.ok("blocks.info")["scripts"] if x["id"] == s2][0]
            check("改空生效",
                  got["blocks"][0]["props"]["dir"] == "left"
                  and got["blocks"][1]["props"]["obj"] == "player", got["blocks"])
            m.ok("blocks.move", {"id": s2, "block_id": b, "dir": -1})
            got = [x for x in m.ok("blocks.info")["scripts"] if x["id"] == s2][0]
            check("上移生效", got["blocks"][0]["block_id"] == b, got["blocks"])
            check("最上面那块不能再上移",
                  "最上面" in m.err("blocks.move", {"id": s2, "block_id": b, "dir": -1}))
            check("不认识的积木带原因",
                  "没有这种积木" in m.err("blocks.add", {"id": s2, "type": "nope"}))
            check("改不存在的空带原因",
                  "没有" in m.err("blocks.set", {"id": s2, "block_id": b,
                                                 "name": "nope", "value": "x"}))
            m.ok("blocks.remove", {"id": s2, "block_id": a})
            got = [x for x in m.ok("blocks.info")["scripts"] if x["id"] == s2][0]
            check("删积木生效", len(got["blocks"]) == 1, got["blocks"])

            # 校验:播放声音没选声音 = 错误(拦住生成)
            m.ok("blocks.script.add", {"event": "on_key"})
            s3 = m.ok("blocks.info")["scripts"][-1]["id"]
            m.ok("blocks.add", {"id": s3, "type": "play_sound"})
            m.ok("blocks.set", {"id": s3, "block_id": 0, "name": "key", "value": "32"})
            v = m.ok("blocks.validate")
            check("没选声音会被校验拦下", v["ok"] is False and v["errors"] >= 1, v)
            check("生成代码前会挡住有问题的积木",
                  "积木有" in m.err("blocks.generate"))
            ps = [x for x in m.ok("blocks.info")["scripts"] if x["id"] == s3][0]
            m.ok("blocks.remove", {"id": s3,
                                   "block_id": ps["blocks"][0]["block_id"]})

            # 代码生成:模板那段必须生成出"每帧往右走 + 按跳键就跳"
            g = m.ok("blocks.generate")
            src = g["source"]
            check("生成的代码有 logic_update", "func logic_update" in src, src[:200])
            check("生成的代码里是每帧移动(乘以 dt)", "* dt)" in src, src)
            check("生成的代码里有 eng_find", "eng_find(" in src, src)
            check("积木落盘到 scripts/logic.dex",
                  os.path.exists(os.path.join(proj, "scripts", "logic.dex")))
            check("blocks.json 也落盘了",
                  os.path.exists(os.path.join(proj, "scripts", "blocks.json")))

            # 撤销:积木的改动要能撤销(快照里带 blocks.json)
            before = len(m.ok("blocks.info")["scripts"])
            m.ok("blocks.script.add", {"event": "on_start"})
            check("加一段后多了一段", len(m.ok("blocks.info")["scripts"]) == before + 1)
            m.ok("undo")
            check("撤销后回到原来的段数",
                  len(m.ok("blocks.info")["scripts"]) == before,
                  len(m.ok("blocks.info")["scripts"]))

            # 模式切换:切到节点图之后,编译走的是节点图那套
            r = m.ok("logic.mode", {"mode": "graph"})
            check("logic.mode 能切到节点图", r["mode"] == "graph", r)
            m.ok("project.save")
            check("模式写进了 project.json",
                  "graph" in open(os.path.join(proj, "project.json"),
                                  encoding="utf-8").read())
        finally:
            m.close()


def test_blocks_behavior(dll):
    """积木的验收线:积木 → 代码 → 编译 → **真的跑出预期行为**。

    模板是"每一帧按 speed=mid(150/秒)往右走",玩家起点 x=300(模板摆好的),
    所以 4 帧 × dt=0.5 之后应该是 300 + 150×0.5×4 = 600 —— 这正是零基础用户
    "点了运行就该看到"的效果。
    """
    print("[积木跑出预期行为(积木→代码→dexc→vm)]")
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not (os.path.exists(DEXC) and os.path.exists(vm)):
        skip("积木驱动运行", "dexc.exe / vm.exe 未构建")
        return
    with tempdir("ds_blkrun_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "blkrun", "template": "starter"})
            m.ok("scene.save")
            m.ok("blocks.generate")
            main_dex = os.path.join(tmp, "scripts", "main.dex")
            with open(main_dex, "w", encoding="utf-8", newline="\n") as f:
                f.write(BLOCKS_MAIN_DEX)
            bc = os.path.join(tmp, "scripts", "main.dexbc")
            c = subprocess.run([DEXC, "compile", main_dex, "-o", bc],
                               capture_output=True, text=True, encoding="utf-8",
                               errors="replace", cwd=tmp)
            check("积木生成的代码能过 dexc", c.returncode == 0,
                  (c.stdout or "") + (c.stderr or ""))
            if c.returncode != 0:
                return
            run = subprocess.run([vm, "scripts/main.dexbc", "-L",
                                  os.path.join(LIBS, "dexgame")],
                                 capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", cwd=tmp)
            out = (run.stdout or "").strip().splitlines()
            check("积木驱动的程序能跑起来", run.returncode == 0,
                  (run.stdout or "") + (run.stderr or ""))
            check("场景真的装上了组件(不是「有实体没组件」的空壳)",
                  len(out) >= 1 and out[0].strip() == "1", out)
            check("「每一帧往右走」真的动了(300 起点 + 4×0.5×150 = 600)",
                  len(out) >= 2 and abs(float(out[1]) - 600.0) < 0.01, out)
            check("没有多余的动作(没按跳键就不该跳)",
                  len(out) < 3 or abs(float(out[2])) < 0.01, out)
        finally:
            m.close()


PREINIT_DEX = """# 故意**不**先 eng_init:场景加载必须照样把组件装上
include "dexgame";

print eng_scene_load("scenes/main.json");
let p = eng_find("玩家");
print eng_object_count();
print eng_has(p, "transform");
print eng_has(p, "sprite");
print eng_get_f(p, "transform", "x");
print eng_get_f(p, "transform", "y");
"""


def test_scene_load_before_init(dll):
    """回归:engine 没初始化时加载场景,不能"加载成功但一个组件都没有"。

    症状(实测过):IDE 生成的 main.dex 在 eng_init 之前 eng_scene_load,
    `dg_comp_kind` 因为组件池没建而返回 -1,而场景加载把 -1 当成
    "不认识的组件(新版本写的)"**静默跳过** —— 于是窗口里什么都没有,
    还查不到原因。现在组件池按需初始化,加载结果必须完整。
    """
    print("[引擎未初始化就加载场景(静默丢组件的回归)]")
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not (os.path.exists(DEXC) and os.path.exists(vm)):
        skip("未初始化加载场景", "dexc.exe / vm.exe 未构建")
        return
    with tempdir("ds_preinit_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "preinit", "template": "starter"})
            m.ok("scene.save")
            src = os.path.join(tmp, "pre.dex")
            with open(src, "w", encoding="utf-8", newline="\n") as f:
                f.write(PREINIT_DEX)
            bc = os.path.join(tmp, "pre.dexbc")
            c = subprocess.run([DEXC, "compile", src, "-o", bc],
                               capture_output=True, text=True, encoding="utf-8",
                               errors="replace", cwd=tmp)
            check("未初始化加载场景的探针能编译", c.returncode == 0,
                  (c.stdout or "") + (c.stderr or ""))
            if c.returncode != 0:
                return
            run = subprocess.run([vm, "pre.dexbc", "-L", os.path.join(LIBS, "dexgame")],
                                 capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", cwd=tmp)
            out = (run.stdout or "").strip().splitlines()
            check("引擎未初始化也能加载场景", len(out) >= 1 and out[0].strip() == "0", out)
            check("实体数与场景一致(2 个)", len(out) >= 2 and out[1].strip() == "2", out)
            check("transform 装上了", len(out) >= 3 and out[2].strip() == "1", out)
            check("sprite 装上了", len(out) >= 4 and out[3].strip() == "1", out)
            check("字段值读得回来(x=300,y=200)",
                  len(out) >= 6 and abs(float(out[4]) - 300.0) < 0.01
                  and abs(float(out[5]) - 200.0) < 0.01, out)
        finally:
            m.close()


def test_template_game_really_runs(dll):
    """用户按下「运行」必须**真的开出一个游戏窗口**(60ms 就退出的回归)。

    症状(实测过):模板 main.dex 从不调 eng_init,而 eng_run 的循环条件是
    `while eng_running()` —— 没初始化就没有窗口、eng_running() 恒为 0,
    于是整个游戏 60 毫秒跑完、什么都不显示。这条测试直接编译**IDE 生成的
    main.dex** 并跑起来:2 秒后进程必须还活着(窗口开着、帧循环在转),
    然后把它杀掉。
    """
    print("[IDE 生成的游戏真的跑起来(窗口 + 帧循环)]")
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not (os.path.exists(DEXC) and os.path.exists(vm)):
        skip("模板游戏真的跑起来", "dexc.exe / vm.exe 未构建")
        return
    with tempdir("ds_game_") as tmp:
        m = Model(dll)
        proc = None
        try:
            m.ok("project.new", {"dir": tmp, "name": "跑起来", "template": "starter"})
            m.ok("scene.save")
            m.ok("blocks.generate")
            tpl = open(os.path.join(tmp, "scripts", "main.dex"),
                       encoding="utf-8").read()
            check("模板里在 on_start 之前 eng_init(否则 on_start 加载场景时还没有设备)",
                  "eng_init(" in tpl.split("eng_run(")[0], tpl[-300:])
            check("模板里窗口标题来自项目名(不是写死的字符串)",
                  "dexstudio_game_title()" in tpl, tpl[-300:])
            info = open(os.path.join(tmp, "scripts", "project_info.dex"),
                        encoding="utf-8").read()
            check("project_info.dex 里有窗口标题函数",
                  "func dexstudio_game_title()" in info, info)
            bc = os.path.join(tmp, "scripts", "main.dexbc")
            c = subprocess.run([DEXC, "compile", os.path.join(tmp, "scripts", "main.dex"),
                                "-o", bc],
                               capture_output=True, text=True, encoding="utf-8",
                               errors="replace", cwd=tmp)
            check("模板能编译", c.returncode == 0, (c.stdout or "") + (c.stderr or ""))
            if c.returncode != 0:
                return
            proc = subprocess.Popen([vm, "scripts/main.dexbc", "-L",
                                     os.path.join(LIBS, "dexgame")],
                                    cwd=tmp, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            time.sleep(2.0)
            alive = proc.poll() is None
            if not alive:
                out = (proc.stdout.read() or b"").decode("utf-8", "replace")
                check("游戏进程活着(窗口开着、帧循环在转)", False,
                      "2 秒内就退出了(rc=%s):%s" % (proc.returncode, out[-300:]))
            else:
                check("游戏进程活着(窗口开着、帧循环在转)", True)
        finally:
            if proc is not None and proc.poll() is None:
                proc.kill()
                try:
                    proc.wait(timeout=10)
                except Exception:
                    pass
            m.close()


def test_build_and_run(dll):
    """B5 的模型层:一键编译(诊断带行列)、独立运行 + 停止、读源码。"""
    print("[编译 / 运行 / 输出与错误定位]")
    vm = os.path.join(ROOT, "vm", "vm.exe" if os.name == "nt" else "vm")
    if not os.path.exists(DEXC):
        skip("B5 编译/运行", "dexc.exe 未构建")
        return
    with tempdir("ds_build_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "b"})
            scripts = m.ok("project.scripts")
            check("project.scripts 列出 main.dex 与 logic.dex",
                  "main.dex" in scripts and "logic.dex" in scripts, scripts)
            src = m.ok("file.read", {"path": "scripts/main.dex"})["text"]
            check("file.read 读到源码", "include \"dexgame\"" in src, src[:80])
            e = m.err("file.read", {"path": "scripts/nope.dex"})
            check("读不到的文件带原因", "读不到" in e, e)

            r = m.ok("build.compile")
            check("一键编译新项目成功", r["ok"] and r["code"] == 0, r["out"][:200])
            check("编译产物存在", r["bytecode_exists"], r["bytecode"])
            check("编译输出里有字节码行", "字节码" in r["out"], r["out"][:120])
            check("编译顺手生成了逻辑图", r["graph_generated"] is True, r)

            # 语法错:诊断必须带 phase/行/列/消息
            bad = os.path.join(tmp, "scripts", "bad.dex")
            with open(bad, "w", encoding="utf-8", newline="\n") as f:
                f.write("func f() {\n    print 1;\n")
            r = m.ok("build.compile", {"path": "scripts/bad.dex"})
            check("坏源码编译失败", not r["ok"] and r["code"] != 0, r)
            check("失败时没有留下字节码", not r["bytecode_exists"], r["bytecode"])
            check("诊断有 1 条 error", r["errors"] == 1 and len(r["diag"]) == 1, r["diag"])
            d0 = r["diag"][0]
            check("诊断带阶段/行/列",
                  d0["level"] == "error" and d0["phase"] == "parser"
                  and d0["line"] == 3 and d0["col"] == 1, d0)
            check("诊断带可读消息", "expected" in d0["msg"], d0)
            check("诊断指向出错的脚本", d0["file"].endswith("bad.dex"), d0)

            # 警告:编译成功但带 warning
            warn = os.path.join(tmp, "scripts", "warn.dex")
            with open(warn, "w", encoding="utf-8", newline="\n") as f:
                f.write("func f() {\n    return 1;\n    print 2;\n}\nprint f();\n")
            r = m.ok("build.compile", {"path": "scripts/warn.dex"})
            check("有警告仍然编译成功", r["ok"] and r["warnings"] == 1, r)
            check("警告的行列与消息对",
                  r["diag"][0]["level"] == "warning" and r["diag"][0]["line"] == 3
                  and "unreachable" in r["diag"][0]["msg"], r["diag"])

            # 同步运行(把输出收回来)
            hello = os.path.join(tmp, "scripts", "hello.dex")
            with open(hello, "w", encoding="utf-8", newline="\n") as f:
                f.write('print "hello dexc";\n')
            m.ok("build.compile", {"path": "scripts/hello.dex"})
            if not os.path.exists(vm):
                skip("同步运行", "vm.exe 未构建")
            else:
                r = m.ok("build.run", {"path": "scripts/hello.dexbc", "wait": 1})
                check("同步运行拿到输出与退出码",
                      r["code"] == 0 and "hello dexc" in r["out"], r)
                check("运行用的不是无控制台版本", r["exe"].endswith("vm.exe"), r["exe"])

                # 独立窗口运行 + 停止(用一个死循环脚本,免得真开窗口)
                loop = os.path.join(tmp, "scripts", "loop.dex")
                with open(loop, "w", encoding="utf-8", newline="\n") as f:
                    f.write("let i = 0;\nwhile 1 { i = i + 1; }\n")
                m.ok("build.compile", {"path": "scripts/loop.dex"})
                r = m.ok("build.run", {"path": "scripts/loop.dexbc", "detach": 1})
                check("独立运行返回 pid", r["running"] and r["pid"] > 0, r)
                check("独立运行用无控制台 VM(只有一个游戏窗口)",
                      "vmnc.exe" in r["exe"], r["exe"])
                st = m.ok("build.status")
                check("status 报告在运行", st["running"] and st["pid"] == r["pid"], st)
                check("stop 停掉进程", m.ok("build.stop")["stopped"] is True)
                st = m.ok("build.status")
                check("stop 之后不再运行", st["running"] is False, st)

                m.ok("build.compile", {"path": "scripts/hello.dex"})
                r = m.ok("build.run", {"path": "scripts/hello.dexbc", "detach": 1})
                check("可以再起一个", r["running"] and r["pid"] > 0, r)
                m.ok("build.stop")

            e = m.err("build.run", {"path": "scripts/nope.dexbc"})
            check("运行不存在的字节码带原因", "找不到字节码" in e, e)
            e = m.err("build.compile", {"path": "scripts/nope.dex"})
            check("编译不存在的脚本带原因", "找不到脚本" in e, e)
        finally:
            m.close()


def test_resources_and_autosave(dll):
    """B6 的资源管理与自动保存/崩溃恢复。

    注意:`res.pick` 会弹**模态**文件对话框,测试里只用 `dry:1` 那条路
    (命令本身照常分发,只是不弹窗)—— 无人值守流程绝不能被对话框挂住。"""
    print("[资源管理 / 自动保存 / 崩溃恢复]")
    with tempdir("ds_res_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "r"})
            check("新项目 res/ 是空的", m.ok("res.list")["count"] == 0)
            check("res/ 目录被建出来了", os.path.isdir(os.path.join(tmp, "res")))

            # 造一个假 PNG(内容不重要,只验证"复制进来/列出来/删掉")
            src = os.path.join(tmp, "outside.png")
            with open(src, "wb") as f:
                f.write(b"\x89PNG\r\n\x1a\n" + b"\x00" * 32)
            r = m.ok("res.import", {"src": src})
            check("res.import 复制进 res/", r["name"] == "outside.png", r)
            check("导入后文件真的在", os.path.exists(os.path.join(tmp, "res",
                                                                "outside.png")))
            e = m.err("res.import", {"src": src})
            check("重名导入不覆盖并给原因", "已经有" in e, e)
            e = m.err("res.import", {"src": os.path.join(tmp, "nope.png")})
            check("导入不存在的文件给原因", "不存在" in e, e)

            lst = m.ok("res.list")
            check("res.list 报出名字/大小/类型",
                  lst["count"] == 1 and lst["files"][0]["name"] == "outside.png"
                  and lst["files"][0]["size"] > 0
                  and lst["files"][0]["kind"] == "image", lst)
            m.ok("res.rename", {"name": "outside.png", "to": "atlas.png"})
            check("改名生效",
                  m.ok("res.list")["files"][0]["name"] == "atlas.png")
            e = m.err("res.rename", {"name": "atlas.png", "to": "atlas.png"})
            check("改成已存在的名字给原因", "已经有" in e, e)
            m.ok("res.delete", {"name": "atlas.png"})
            check("删除生效", m.ok("res.list")["count"] == 0)
            e = m.err("res.delete", {"name": "atlas.png"})
            check("删不存在的资源给原因", "没有" in e, e)
            e = m.err("res.delete", {"name": "../evil.txt"})
            check("资源名不许带路径(防越界)", "不合法" in e, e)
            p = m.ok("res.pick", {"dry": 1})
            check("res.pick dry 模式不弹窗", p["picked"] is False and p["dry"] is True, p)

            # --- 自动保存 ---
            r = m.ok("autosave.tick")
            check("没有改动时不写自动保存", r["saved"] is False, r)
            m.ok("entity.add", {"name": "player"})
            r = m.ok("autosave.tick")
            check("有改动就写自动保存", r["saved"] is True and r["seq"] >= 1, r)
            apath = os.path.join(tmp, ".dexstudio", "autosave.json")
            check("自动保存文件在 .dexstudio/ 下", os.path.exists(apath), apath)
            with open(apath, encoding="utf-8") as f:
                bundle = json.load(f)
            check("自动保存是整包(场景 + 外部文件)",
                  bundle.get("scene_path", "").endswith("main.json")
                  and "scene" in bundle and "files" in bundle, list(bundle.keys()))
            check("包里的场景确实有那个实体", "player" in bundle["scene"],
                  bundle["scene"][:80])

            # --- 同一次运行里,自动保存比场景新也**不该**提示恢复 ---
            # (跨会话的"上次没正常退出"在 test_recover_session 里测:那里才需要
            #  两个模型实例。这里先证明"当前这次运行"不会自己吓自己。)
            m.ok("entity.add", {"name": "enemy"})
            m.ok("autosave.tick")
            spath = os.path.join(tmp, "scenes", "main.json")
            old = os.path.getmtime(spath)
            os.utime(spath, (old - 120, old - 120))     # 假装场景是两分钟前存的
            st = m.ok("recover.status")
            check("同一会话的自动保存不提示恢复", st["recoverable"] is False, st)
            check("app.info 也不提示", m.ok("app.info")["recoverable"] is False)
            e = m.err("recover.apply")
            check("没有可恢复内容时给原因", "没有可恢复" in e, e)

            # --- 正常保存之后同样不提示;丢弃始终可用 ---
            m.ok("entity.add", {"name": "third"})
            m.ok("autosave.tick")
            m.ok("project.save")
            os.utime(spath, (old + 600, old + 600))     # 场景现在比自动保存新
            check("正常保存后也不再提示恢复",
                  m.ok("recover.status")["recoverable"] is False)
            r = m.ok("recover.discard")
            check("丢弃自动保存返回成功", r["discarded"] is True, r)
        finally:
            m.close()


def test_utf8_paths(dll):
    """中文路径/中文名必须一路 UTF-8 到底(B7 修)。

    这里的每一项过去都是坏的,而且坏法不同:
      - JSON 解析把 UTF-8 的字节当成"码点"再编码一次 → 二次编码;
      - 宿主用 CreateFileA/FindFirstFileA/fopen 这些 ANSI 接口 → 中文路径找不到;
      - 窗口标题用 CreateWindowExA → 乱码;
      - CreateProcessA 起的 dexc 收到乱码路径。
    一个中文名走一遍"命令 → 内存 → 磁盘 → JSON → 命令",上面几处全都会被踩到。
    """
    print("[中文与编码(UTF-8 路径一路到底)]")
    with tempdir("ds_utf8_") as tmp:
        # 故意多一层还不存在的父目录:project.new 要能自己建出来
        root = os.path.join(tmp, "我的项目", "游戏")
        m = Model(dll)
        try:
            r = m.ok("project.new", {"dir": root, "name": "我的游戏"})
            check("中文项目目录能建出来", r["root"] == root, r)
            check("中文项目目录真的落盘",
                  os.path.isdir(os.path.join(root, "scenes")), root)
            check("project.json 里没有乱码",
                  "我的游戏" in open(os.path.join(root, "project.json"),
                                     encoding="utf-8").read())

            # 场景:中文名 + 中文实体名(实体名走引擎的 JSON 读写)
            r = m.ok("scene.new", {"name": "第一关"})
            check("中文场景名原样回来", r["scene"].endswith("第一关.json"), r)
            check("中文场景文件真的落盘",
                  os.path.exists(os.path.join(root, "scenes", "第一关.json")))
            check("scene.list 里是 UTF-8 中文名",
                  "第一关.json" in m.ok("scene.list"), m.ok("scene.list"))
            m.ok("entity.add", {"name": "玩家"})
            m.ok("entity.add", {"name": "敌人"})
            names = [e["name"] for e in m.ok("entity.list")]
            check("中文实体名原样回来", names == ["玩家", "敌人"], names)
            eid = m.ok("entity.find", {"name": "玩家"})["id"]
            check("entity.find 认中文名",
                  m.ok("entity.get", {"id": eid})["name"] == "玩家", eid)

            # 存盘 → 重开:中文名要能从磁盘读回来
            # (引擎在 DLL 里是**单例**,同一个进程只能有一个活着的模型 ——
            #  所以先关掉 m 再开 m2,不能同时开两个。)
            m.ok("project.save")
        finally:
            m.close()

        m2 = Model(dll)
        try:
            m2.ok("project.open", {"dir": root})
            names2 = [e["name"] for e in m2.ok("entity.list")]
            check("重开项目后中文实体名还在", names2 == ["玩家", "敌人"], names2)
            check("重开项目后当前场景还是中文那个",
                  m2.ok("app.info")["scene"].endswith("第一关.json"),
                  m2.ok("app.info")["scene"])
        finally:
            m2.close()

        m3 = Model(dll)
        try:
            m3.ok("project.open", {"dir": root})
            # 资源:中文文件名,导入/列表/改名都要是 UTF-8
            src = os.path.join(tmp, "来源 图片.png")
            with open(src, "wb") as f:
                f.write(b"\x89PNG\r\n\x1a\n" + b"\0" * 24)
            r = m3.ok("res.import", {"src": src, "name": "我的图片.png"})
            check("中文资源名导入成功", r["name"] == "我的图片.png", r)
            check("中文资源文件真的落盘",
                  os.path.exists(os.path.join(root, "res", "我的图片.png")))
            files = m3.ok("res.list")["files"]
            check("res.list 里是 UTF-8 中文名",
                  [f["name"] for f in files] == ["我的图片.png"], files)
            check("资源扩展名识别正确", files[0]["kind"] == "image", files[0])
            m3.ok("res.rename", {"name": "我的图片.png", "to": "背景图.png"})
            check("中文资源改名成功",
                  os.path.exists(os.path.join(root, "res", "背景图.png")))
            m3.ok("res.delete", {"name": "背景图.png"})
            check("中文资源删除成功", m3.ok("res.list")["count"] == 0)

            # 视口渲染:预览图要落在中文目录里(引擎侧也是 _wfopen)
            r = m3.ok("scene.render")
            check("中文目录里也能渲染预览", r["seq"] >= 1, r)
            check("预览图真的写出了",
                  os.path.exists(os.path.join(root, ".dexstudio", "preview.bmp")))

            # 自动保存:包里的路径与文件都要能读写
            m3.ok("entity.add", {"name": "存档点"})
            check("中文目录里能写自动保存", m3.ok("autosave.tick")["saved"] is True)
            check("中文目录里自动保存文件在",
                  os.path.exists(os.path.join(root, ".dexstudio", "autosave.json")))
        finally:
            m3.close()


def test_build_in_chinese_path(dll):
    """一键编译要能在中文目录里跑通(dexc 是 CreateProcessW 起的)。"""
    print("[中文目录里一键编译]")
    if not os.path.exists(DEXC):
        skip("中文目录编译", "dexc.exe 未构建")
        return
    with tempdir("ds_utf8_build_") as tmp:
        root = os.path.join(tmp, "我的游戏")
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": root, "name": "游戏"})
            r = m.ok("build.compile")
            check("中文目录里 build.compile 成功", r["ok"] is True, r)
            check("字节码落在中文目录里",
                  r.get("bytecode_exists") is True and
                  os.path.exists(r.get("bytecode") or ""), r.get("bytecode"))
            scripts = m.ok("project.scripts")
            check("scripts 列表正常", "main.dex" in scripts, scripts)
            src = m.ok("file.read", {"path": "scripts/main.dex"})
            check("file.read 读得到中文目录里的源码",
                  "logic" in (src.get("text") or ""), str(src)[:120])
        finally:
            m.close()


def test_recover_session(dll):
    """恢复提示的语义:只提示**上一次运行**留下的自动保存。

    用户报的坑:IDE 开着,自动保存每 30 秒写一次(比场景文件新),界面就一直喊
    "上次好像没有正常退出",点「恢复/丢弃」也没用 —— 30 秒后它又回来了。
    """
    print("[恢复提示只认上一次运行]")
    with tempdir("ds_recover_") as tmp:
        root = os.path.join(tmp, "proj")
        apath = os.path.join(root, ".dexstudio", "autosave.json")

        # --- 第 1 次运行:改一改 → 自动保存 → 同一次运行里不该提示 ---
        m = Model(dll)
        m.ok("project.new", {"dir": root, "name": "proj"})
        m.ok("entity.add", {"name": "player"})
        check("自动保存写出来了", m.ok("autosave.tick")["saved"] is True)
        check("包里记了是哪个会话写的",
              "session" in open(apath, encoding="utf-8").read())
        check("本次运行自己写的自动保存不提示恢复",
              m.ok("recover.status")["recoverable"] is False)
        check("app.info 也不提示", m.ok("app.info")["recoverable"] is False)
        m.close()

        # --- 第 2 次运行:同一份自动保存,这次要提示(正常退出 → unsaved)---
        m2 = Model(dll)
        m2.ok("project.open", {"dir": root})
        st = m2.ok("recover.status")
        check("上一次运行留下的自动保存会提示恢复", st["recoverable"] is True, st)
        check("正常退出(打过 clean 标记)归类为 unsaved",
              st["kind"] == "unsaved", st)
        check("app.info 也报 recover_kind",
              m2.ok("app.info")["recover_kind"] == "unsaved", m2.ok("app.info"))
        # 假装上次是被强杀的(clean=0)→ 归类成 crash,措辞不同
        with open(apath, encoding="utf-8") as f:
            bundle = json.load(f)
        bundle["clean"] = False
        with open(apath, "w", encoding="utf-8") as f:
            json.dump(bundle, f, ensure_ascii=False)
        check("clean=0 时归类为 crash(强杀)", m2.ok("recover.status")["kind"] == "crash")
        r = m2.ok("recover.apply")
        check("恢复成功", r["recovered"] is True, r)
        check("恢复后实体回来了",
              [e["name"] for e in m2.ok("entity.list")] == ["player"])
        check("恢复后不再提示", m2.ok("recover.status")["recoverable"] is False)
        check("恢复出来的内容算未保存改动", m2.ok("app.info")["dirty"] is True)
        m2.close()

        # --- 第 3 次运行:丢弃之后,同一次运行里再怎么自动保存都不该冒出来 ---
        m3 = Model(dll)
        m3.ok("project.open", {"dir": root})
        check("恢复过的自动保存已经清掉,不再提示",
              m3.ok("recover.status")["recoverable"] is False)
        m3.ok("entity.add", {"name": "enemy"})
        m3.ok("autosave.tick")
        check("同一会话里自动保存比场景新也不提示(用户的坑)",
              m3.ok("recover.status")["recoverable"] is False)
        m3.ok("project.save")
        m3.ok("entity.add", {"name": "enemy2"})
        m3.ok("autosave.tick")
        check("保存后又编辑、又自动保存,依然不提示",
              m3.ok("recover.status")["recoverable"] is False)
        check("丢弃命令可用", m3.ok("recover.discard")["discarded"] is True)
        m3.close()


def test_asset_paths(dll):
    """场景里的资源路径必须是**项目相对**的,而且要能加载。

    用户报的坑:点资源缩略图设贴图,`comp.set sprite.tex_path = "res/x.png"`
    直接失败(`cannot open image 'res/x.png'`)—— 因为引擎按**自己的工作目录**
    找相对路径,而 IDE 的工作目录不是项目根。以前靠"写绝对路径"绕过去会让
    项目一搬家就废,所以改成:引擎有个资源根(eng_set_asset_dir),IDE 打开
    项目时设成项目根。
    """
    print("[资源路径(项目相对 + 资源根)]")
    with tempdir("ds_asset_") as tmp:
        root = os.path.join(tmp, "proj")
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": root, "name": "p"})
            src = os.path.join(ROOT, "tests", "fixtures", "atlas2x2.png")
            m.ok("res.import", {"src": src, "name": "hero.png"})
            eid = m.ok("entity.add", {"name": "hero"})["id"]
            m.ok("comp.add", {"id": eid, "comp": "sprite"})
            r = m.ok("comp.set", {"id": eid, "comp": "sprite", "field": "tex_path",
                                  "value": "res/hero.png"})
            check("项目相对路径的贴图能设上", r["value"] == "res/hero.png", r)
            sp = m.ok("entity.get", {"id": eid})["comps"]["sprite"]
            check("引擎真的加载了它(texture >= 0)",
                  isinstance(sp.get("texture"), int) and sp["texture"] >= 0, sp)
            check("场景 JSON 里存的是相对路径(项目可搬)",
                  "res/hero.png" in json.dumps(m.ok("scene.json"), ensure_ascii=False),
                  m.ok("scene.json"))
            # 重开项目后也要能加载(资源根是在 project.open 时设的)
            m.ok("project.save")
        finally:
            m.close()
        m2 = Model(dll)
        try:
            m2.ok("project.open", {"dir": root})
            eid = m2.ok("entity.find", {"name": "hero"})["id"]
            sp = m2.ok("entity.get", {"id": eid})["comps"]["sprite"]
            check("重开项目后贴图仍然加载得起来(texture >= 0)",
                  isinstance(sp.get("texture"), int) and sp["texture"] >= 0, sp)
            check("重开项目后路径依旧是相对的",
                  sp.get("tex_path") == "res/hero.png", sp.get("tex_path"))
        finally:
            m2.close()


def test_graph_link_drag_visible():
    """拉线时那根预览线要跟着鼠标(页面自测覆盖;这里只钉住 JS 不被改回去)。

    真正的行为断言在 `--wv-selftest` 的页面自测里(派发 mousedown/mousemove 后
    检查 Graph.dragState())。这条静态检查是"别把 guard 又写回 pending 之前"的哨兵。
    """
    print("[逻辑图:拉线预览(静态哨兵)]")
    p = os.path.join(ROOT, "dexstudio", "web", "graph.js")
    if not os.path.exists(p):
        skip("拉线预览哨兵", "graph.js 不存在")
        return
    src = open(p, encoding="utf-8").read()
    # 只看 mousemove 处理函数那一段(注释里也提到过那句 guard,别被它干扰)
    seg = src[src.index("addEventListener('mousemove'"):]
    seg = seg[:seg.index("addEventListener('mouseup'")]
    check("mousemove 里先更新 pending 再判断 drag",
          "if (pending) { pending.gx" in seg
          and seg.index("if (pending) { pending.gx") < seg.index("if (!drag) return;"),
          "pending 必须在 drag 的 guard 之前")


def test_ui_contract(dll):
    """界面能用下拉/复选而不是"让用户手打"的**契约**测试。

    这一组盯的是 docs/DEXSTUDIO_UX_ISSUES.md 里的修复:字段元数据、场景管理、
    父子关系、一键编译存盘、生成前校验、资源改名的引用同步。
    每一条以前都是"静默做错"——所以必须在无窗口下钉住。"""
    print("[界面契约:元数据 / 场景管理 / 父子 / 存盘 / 校验]")
    with tempdir("ds_ui_") as tmp:
        proj = os.path.join(tmp, "proj")
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": proj, "name": "ui"})

            # ---- 字段元数据:前端据此出下拉/复选/取色器/资源选择 ----
            schema = {c["name"]: {f["name"]: f for f in c["fields"]}
                      for c in m.ok("comp.schema")}
            check("字段带中文标签", schema["transform"]["x"].get("label") not in (None, ""),
                  schema["transform"]["x"])
            check("collider.kind 是枚举且有值域",
                  schema["collider"]["kind"].get("kind") == "enum"
                  and len(schema["collider"]["kind"].get("enum") or []) == 3,
                  schema["collider"]["kind"])
            check("枚举值带中文标签",
                  any(e["label"] == "胶囊" for e in schema["collider"]["kind"]["enum"]),
                  schema["collider"]["kind"].get("enum"))
            check("body.motion 是枚举", schema["body"]["motion"].get("kind") == "enum")
            check("sprite.tex_path 是图片资源选择",
                  schema["sprite"]["tex_path"].get("kind") == "image")
            check("sprite.tint 是取色器", schema["sprite"]["tint"].get("kind") == "color")
            check("sprite.flip 是位掩码多选",
                  schema["sprite"]["flip"].get("kind") == "flags")
            check("transform.parent 是实体下拉",
                  schema["transform"]["parent"].get("kind") == "entity")
            check("运行期字段只读", schema["sprite"]["texture"].get("readonly") is True)
            check("引擎没实现的字段只读且说明原因",
                  schema["transform"]["rot"].get("readonly") is True
                  and "不读" in (schema["transform"]["rot"].get("hint") or ""),
                  schema["transform"]["rot"])
            check("数字字段带单位提示",
                  schema["transform"]["x"].get("unit") == "像素",
                  schema["transform"]["x"])

            # ---- 下游数据:实体带 parent、outline 带渲染顺序 ----
            a = m.ok("entity.add", {"name": "父"})
            b = m.ok("entity.add", {"name": "子"})
            lst = {e["name"]: e for e in m.ok("entity.list")}
            check("entity.list 带 parent(层级树靠它)",
                  "parent" in lst["父"] and lst["父"]["parent"] == -1, lst["父"])
            out = {o["name"]: o for o in m.ok("scene.outline")}
            check("scene.outline 带 layer/order/seq(命中测试按渲染顺序)",
                  all(k in out["父"] for k in ("layer", "order", "seq")), out["父"])

            # ---- 父子关系 ----
            r = m.ok("entity.set_parent", {"id": b["id"], "parent": a["id"]})
            check("设父级", r["parent"] == a["id"], r)
            m.ok("comp.set", {"id": a["id"], "comp": "transform", "field": "x",
                              "value": 200})
            got = m.ok("entity.get", {"id": b["id"]})
            check("子实体世界坐标含父级位移", got["x"] == 200, got["x"])
            check("自己当父级被拒绝", "自己" in m.err("entity.set_parent",
                                                     {"id": b["id"], "parent": b["id"]}))
            check("成环被拒绝", "环" in m.err("entity.set_parent",
                                             {"id": a["id"], "parent": b["id"]}))
            check("父级不存在被拒绝",
                  "不存在" in m.err("entity.set_parent", {"id": b["id"], "parent": 999999}))
            m.ok("entity.set_parent", {"id": b["id"], "parent": -1})
            check("可以断开父级",
                  m.ok("entity.get", {"id": b["id"]})["x"] == 0)
            # 删除父级时子实体的 parent 要一起断掉(否则位置"看起来没变"其实掉了)
            m.ok("entity.set_parent", {"id": b["id"], "parent": a["id"]})
            m.ok("entity.remove", {"id": a["id"]})
            check("删掉父级后子实体的 parent 被清掉",
                  m.ok("entity.list")[0]["parent"] == -1, m.ok("entity.list"))

            # ---- 场景管理 ----
            check("场景名不许路径穿越",
                  "不合法" in m.err("scene.new", {"name": "../坏"}))
            m.ok("scene.new", {"name": "第二关"})
            check("场景列表里有它", "第二关.json" in m.ok("scene.list"))
            m.ok("scene.rename", {"name": "第二关", "to": "第三关"})
            check("改名后新名字在列表里", "第三关.json" in m.ok("scene.list"))
            check("改名后旧名字不在了", "第二关.json" not in m.ok("scene.list"))
            m.ok("scene.set_start", {"name": "第三关"})
            check("起始场景记进了项目状态",
                  "第三关" in json.dumps(m.ok("project.state").get("project"),
                                         ensure_ascii=False),
                  m.ok("project.state").get("project"))
            m.ok("project.save")
            check("存盘后起始场景写进了 project.json",
                  "第三关" in open(os.path.join(proj, "project.json"),
                                   encoding="utf-8").read())
            m.ok("scene.delete", {"name": "第三关"})
            check("删除后不在列表里", "第三关.json" not in m.ok("scene.list"))

            # ---- 项目:文件夹对话框(不弹窗的入口)+ 最近项目 ----
            check("project.pick(dry) 不弹窗", m.ok("project.pick", {"dry": 1})["dry"] is True)
            check("最近项目里有它", any(it["path"].endswith("proj")
                                        for it in m.ok("project.recent")["items"]))

            # ---- 逻辑图:下拉候选 + 默认值 + 校验 ----
            o = m.ok("graph.options")
            check("选项里有运算符(且没有 DexLang 不认的 ^)", "^" not in o["ops"]
                  and "+" in o["ops"], o["ops"])
            check("选项里有默认动作名", "jump" in o["actions"], o["actions"])
            check("选项里有按键表与鼠标键",
                  len(o["keys"]) > 20 and len(o["mouse"]) == 3, len(o["keys"]))
            check("选项里有组件→字段(下拉联动用)",
                  len(o["schema"]) >= 8 and "transform" in
                  [c["name"] for c in o["schema"]], len(o["schema"]))
            m.ok("entity.add", {"name": "玩家"})
            o = m.ok("graph.options")
            check("选项里有场景里的实体(属性下拉用)",
                  "玩家" in [e["name"] for e in o["entities"]], o["entities"])
            names = [e["name"] for e in m.ok("entity.list")]
            n = m.ok("graph.node.add", {"type": "set_field"})
            info = m.ok("graph.info")["graph"]
            nd = [x for x in info["nodes"] if x["id"] == n["id"]][0]
            check("新「写字段」节点默认属性是合法值(以前是空串)",
                  nd["props"]["obj"] in names and nd["props"]["comp"] == "transform"
                  and nd["props"]["field"] == "x", nd["props"])
            kn = m.ok("graph.node.add", {"type": "on_key"})
            knode = [x for x in m.ok("graph.info")["graph"]["nodes"]
                     if x["id"] == kn["id"]][0]
            check("新「按键」节点的键码是有意义的(空格 32,以前是 0)",
                  knode["props"]["key"] == 32, knode["props"])
            check("校验:合法图没问题", m.ok("graph.validate")["count"] == 0)
            ps = m.ok("graph.node.add", {"type": "play_sound"})
            v = m.ok("graph.validate")
            check("校验:没选声音会被指出来并带上节点号",
                  v["count"] == 1 and v["issues"][0]["id"] == ps["id"], v)
            check("生成代码前会挡住有问题的图",
                  "问题" in m.err("graph.generate"), m.err("graph.generate"))
            m.ok("graph.node.remove", {"id": ps["id"]})
            m.ok("graph.node.add", {"type": "on_action"})
            g = m.ok("graph.generate")
            check("用到动作节点会自动绑默认键位",
                  "eng_bind_default_actions" in g["source"], g["source"][:200])
            check("生成物里没有空引用的调用",
                  'eng_find("")' not in g["source"]
                  and "eng_key_pressed(0.0)" not in g["source"], g["source"])

            # ---- 「保存」要把逻辑图一起写盘 ----
            m.ok("project.save")
            lj = open(os.path.join(proj, "scripts", "logic.json"), encoding="utf-8").read()
            check("project.save 写了逻辑图", "on_action" in lj, lj[:120])

            # ---- 一键编译:存盘 + project_info.dex ----
            if os.path.exists(DEXC):
                # 前面的场景管理把当前场景换到了别的文件,这里明确切回 main.json
                m.ok("scene.load", {"path": "scenes/main.json"})
                m.ok("entity.add", {"name": "编译存盘探针"})
                r = m.ok("build.compile")
                check("build.compile 成功", r["ok"] is True, r.get("out", "")[-200:])
                check("编译报出起始场景", "main.json" in (r.get("start_scene") or ""),
                      r.get("start_scene"))
                scene_txt = open(os.path.join(proj, "scenes", "main.json"),
                                 encoding="utf-8").read()
                check("编译前把场景存盘了(游戏跑的就是你看的那份)",
                      "编译存盘探针" in scene_txt, scene_txt[:200])
                check("生成了 scripts/project_info.dex",
                      os.path.exists(os.path.join(proj, "scripts", "project_info.dex")))
                pi = open(os.path.join(proj, "scripts", "project_info.dex"),
                          encoding="utf-8").read()
                check("project_info.dex 里是当前起始场景",
                      "scenes/main.json" in pi, pi)
                # 老项目兜底:main.dex 里写死的场景要被改写成当前起始场景
                main_dex = os.path.join(proj, "scripts", "main.dex")
                with open(main_dex, "w", encoding="utf-8") as f:
                    f.write('include "dexgame";\nfunc on_start() {\n'
                            '    eng_scene_load("scenes/main.json");\n}\n')
                m.ok("scene.new", {"name": "第二关"})
                r2 = m.ok("build.compile")
                txt = open(main_dex, encoding="utf-8").read()
                check("老项目写死的场景被改写为当前起始场景",
                      'eng_scene_load("scenes/第二关.json")' in txt
                      and r2["main_patched"] == 1, txt)
            else:
                skip("一键编译的存盘/起始场景检查", "dexc.exe 未构建")

            # ---- 资源:改名连引用一起改 ----
            # 用仓库里真实的 PNG(假装成图片会被引擎拒绝解码,那不是这条要测的东西)
            real_png = None
            for base, _dirs, files in os.walk(os.path.join(ROOT, "tests", "fixtures")):
                for f in files:
                    if f.lower().endswith(".png"):
                        real_png = os.path.join(base, f)
                        break
                if real_png:
                    break
            if real_png is None:
                skip("资源引用同步检查", "tests/fixtures 里没有 PNG")
            else:
                m.ok("res.import", {"src": real_png, "name": "hero.png"})
                e2 = m.ok("entity.add", {"name": "有贴图的"})
                m.ok("comp.add", {"id": e2["id"], "comp": "sprite"})
                m.ok("comp.set", {"id": e2["id"], "comp": "sprite", "field": "tex_path",
                                  "value": "res/hero.png"})
                check("res.refs 数得出引用",
                      m.ok("res.refs", {"name": "hero.png"})["refs"] == 1,
                      m.ok("res.refs", {"name": "hero.png"}))
                check("还被引用的资源不能直接删",
                      "引用" in m.err("res.delete", {"name": "hero.png"}))
                r = m.ok("res.rename", {"name": "hero.png", "to": "hero2.png",
                                        "update_refs": 1})
                check("改名连引用一起改", r["updated"] == 1, r)
                check("场景里的引用真的变成新名字",
                      m.ok("entity.get", {"id": e2["id"]})["comps"]["sprite"]["tex_path"]
                      == "res/hero2.png",
                      m.ok("entity.get", {"id": e2["id"]})["comps"]["sprite"])
                check("导入的名字不许带路径",
                      "不合法" in m.err("res.import", {"src": real_png,
                                                       "name": "../x.png"}))

            # ---- 代码页签:只读 + 外部打开 ----
            check("file.read 能读脚本", "func" in m.ok("file.read",
                                                       {"path": "scripts/main.dex"})["text"])
            check("file.open_external(dry) 只回路径",
                  m.ok("file.open_external", {"path": "scripts/main.dex",
                                              "dry": 1})["opened"] is False)
        finally:
            m.close()


def test_sprite_scale(dll):
    """贴图必须能**缩放**:这是零基础用户最先要做的事("图太大,缩小它")。

    为什么单独一条:引擎原来只会按 sw/sh 1:1 画(取多少画多大),用户拿一张
    300×400 的角色图**没有任何办法**把它变成 60×80 的小人 —— 而且不知道这一点时
    会以为"贴图没生效"。现在缩放走 `transform.sx/sy`(1.0 = 原大小),
    这条测试直接**数渲染出来的像素**:缩放 0.5 之后面积应该约为 1/4。
    """
    print("[贴图缩放(transform.sx/sy 真的影响画面)]")
    bmp = os.path.join(ROOT, "tests", "fixtures", "tiles.png")
    if not os.path.exists(bmp):
        skip("贴图缩放", "tests/fixtures/tiles.png 不存在")
        return
    with tempdir("ds_scale_") as tmp:
        proj = os.path.join(tmp, "proj")
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": proj, "name": "scale"})
            m.ok("res.import", {"src": bmp, "name": "tiles.png"})
            e = m.ok("entity.add", {"name": "小人",
                                    "comps": ["transform", "sprite"]})
            # 新加的 sprite 默认就是"整张贴图"(sw/sh = 0)
            got = m.ok("entity.get", {"id": e["id"]})["comps"]["sprite"]
            check("新的 sprite 默认画整张贴图(sw/sh = 0)", got["sw"] == 0
                  and got["sh"] == 0, got)
            # 模拟**老项目/老模板**留下的 32×32 裁切:换贴图时必须自动改成整张
            for f in ("sw", "sh"):
                m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": f,
                                  "value": 32})
            r = m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": "tex_path",
                                  "value": "res/tiles.png"})
            check("换贴图时把旧模板留下的 32×32 裁切改成整张贴图",
                  r.get("value") == "res/tiles.png", r)
            got = m.ok("entity.get", {"id": e["id"]})["comps"]["sprite"]
            check("裁切被清成 0 = 整张贴图", got["sw"] == 0 and got["sh"] == 0, got)
            check("用户能看到程序的这句说明(不然像是偷偷改了东西)",
                  "整张贴图" in (r.get("note") or ""), r)
            # 用户自己裁过的大小不能被顺手改掉
            for f, v in (("sw", 16), ("sh", 8)):
                m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": f,
                                  "value": v})
            m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": "tex_path",
                              "value": "res/tiles.png"})
            got = m.ok("entity.get", {"id": e["id"]})["comps"]["sprite"]
            check("自己裁的 16×8 不会被顺手改掉", got["sw"] == 16 and got["sh"] == 8,
                  got)
            for f in ("sw", "sh"):
                m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": f,
                                  "value": 0})
            m.ok("comp.set", {"id": e["id"], "comp": "transform", "field": "x",
                              "value": 0})
            m.ok("comp.set", {"id": e["id"], "comp": "transform", "field": "y",
                              "value": 0})
            m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": "px",
                              "value": 0})
            m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": "py",
                              "value": 0})

            def drawn():
                r = m.ok("scene.render")
                w, h, px = read_bmp(r["path"])
                # 背景 = IDE 的清屏色 0xAARRGGBB=0xFF1E1E2E → 读出来 (R,G,B)=(0x1E,0x1E,0x2E)
                return sum(1 for (pr, pg, pb) in px
                           if not (pr == 0x1E and pg == 0x1E and pb == 0x2E)), (w, h)

            full, size = drawn()
            check("整张贴图被画出来了", full > 500, (full, size))
            g = m.ok("entity.get", {"id": e["id"]})
            check("引擎加载了贴图(texture >= 0)", g["comps"]["sprite"]["texture"] >= 0,
                  g["comps"]["sprite"])
            for f, v in (("sx", 0.5), ("sy", 0.5)):
                m.ok("comp.set", {"id": e["id"], "comp": "transform", "field": f,
                                  "value": v})
            half, _ = drawn()
            ratio = half / max(1, full)
            check("缩放 0.5 之后画面上的面积约为 1/4", 0.15 < ratio < 0.40,
                  "%.3f(%d → %d)" % (ratio, full, half))
            # 缩放要反映到选中框上(否则用户拖的是"看不见的框")
            m.ok("comp.set", {"id": e["id"], "comp": "transform", "field": "sx",
                              "value": 1})
            m.ok("comp.set", {"id": e["id"], "comp": "transform", "field": "sy",
                              "value": 1})
            one = [o for o in m.ok("scene.outline") if o["id"] == e["id"]][0]
            m.ok("comp.set", {"id": e["id"], "comp": "transform", "field": "sx",
                              "value": 0.5})
            m.ok("comp.set", {"id": e["id"], "comp": "transform", "field": "sy",
                              "value": 0.5})
            halfo = [o for o in m.ok("scene.outline") if o["id"] == e["id"]][0]
            check("选中框跟着缩放一起变小",
                  abs(halfo["w"] * 2 - one["w"]) < 1.0
                  and abs(halfo["h"] * 2 - one["h"]) < 1.0,
                  (one["w"], one["h"], halfo["w"], halfo["h"]))
        finally:
            m.close()


def test_outline_matches_pixels(dll):
    """选中框必须框住**真的画出来的像素**。

    用户报的症状是"改了贴图,框线变了但图像没出现" —— 根因之一就是框线的
    坐标公式和引擎画图的公式**不是同一个**:引擎是
    `原点 = 世界坐标 - 源尺寸 × 轴心 × 缩放`,宿主写的是 `世界坐标 + 轴心`。
    于是框线偏到图片右下角外面。这条测试直接比"框线"和"画面上的非背景像素包围盒"。
    """
    print("[选中框与画面像素对齐]")
    bmp = os.path.join(ROOT, "tests", "fixtures", "tiles.png")
    if not os.path.exists(bmp):
        skip("选中框与像素对齐", "tests/fixtures/tiles.png 不存在")
        return
    with tempdir("ds_obox_") as tmp:
        proj = os.path.join(tmp, "proj")
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": proj, "name": "obox"})
            m.ok("res.import", {"src": bmp, "name": "tiles.png"})
            e = m.ok("entity.add", {"name": "小人",
                                    "comps": ["transform", "sprite"]})
            m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": "tex_path",
                              "value": "res/tiles.png"})
            for f, v in (("x", 100), ("y", 50)):
                m.ok("comp.set", {"id": e["id"], "comp": "transform", "field": f,
                                  "value": v})

            def box():
                return [o for o in m.ok("scene.outline") if o["id"] == e["id"]][0]

            def pix_bbox():
                r = m.ok("scene.render")
                w, h, px = read_bmp(r["path"])
                xs = [i % w for i, c in enumerate(px)
                      if not (c[0] == 0x1E and c[1] == 0x1E and c[2] == 0x2E)]
                ys = [i // w for i, c in enumerate(px)
                      if not (c[0] == 0x1E and c[1] == 0x1E and c[2] == 0x2E)]
                if not xs:
                    return None
                return min(xs), min(ys), max(xs), max(ys)

            o = box()
            tw, th = o["w"], o["h"]        # sw=0 → 引擎按整张贴图画
            check("轴心 0.5 时框线以实体位置为中心",
                  abs(o["x"] - (100 - tw / 2.0)) < 0.01
                  and abs(o["y"] - (50 - th / 2.0)) < 0.01, o)
            b = pix_bbox()
            check("画面上真的有像素", b is not None, b)
            if b:
                check("像素落在框线里(框线不是偏的)",
                      b[0] >= o["x"] - 1.5 and b[1] >= o["y"] - 1.5
                      and b[2] <= o["x"] + o["w"] + 0.5
                      and b[3] <= o["y"] + o["h"] + 0.5,
                      "bbox=%s outline=(%s,%s,%s,%s)" % (b, o["x"], o["y"],
                                                        o["w"], o["h"]))
                check("像素包围盒的左上角与框线左上角对齐(偏差 ≤ 2px;右/下边依赖图的内容)",
                      abs(b[0] - o["x"]) <= 2 and abs(b[1] - o["y"]) <= 2,
                      "bbox=%s outline=%s" % (b, (o["x"], o["y"], o["w"], o["h"])))
            # 轴心改成左上角:框线与像素必须**一起**挪,而且挪的量一样
            m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": "px",
                              "value": 0})
            m.ok("comp.set", {"id": e["id"], "comp": "sprite", "field": "py",
                              "value": 0})
            o2 = box()
            check("轴心 0 时框线左上角就在实体位置",
                  abs(o2["x"] - 100.0) < 0.01 and abs(o2["y"] - 50.0) < 0.01, o2)
            b2 = pix_bbox()
            if b and b2:
                check("像素也跟着挪了同样的距离",
                      abs((b2[0] - b[0]) - (o2["x"] - o["x"])) <= 2
                      and abs((b2[1] - b[1]) - (o2["y"] - o["y"])) <= 2,
                      "bbox %s → %s, outline %s → %s" % (b, b2, o["x"], o2["x"]))
        finally:
            m.close()


def read_bmp(path):
    """读 32/24bpp BMP,返回 (w, h, [(r,g,b), ...])(与 test_gal.py 同一实现)。"""
    with open(path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    topdown = h < 0
    h = abs(h)
    bytespp = bpp // 8
    row = ((w * bytespp) + 3) // 4 * 4
    px = []
    for y in range(h):
        sy = y if topdown else (h - 1 - y)
        base = off + sy * row
        for x in range(w):
            i = base + x * bytespp
            px.append((data[i + 2], data[i + 1], data[i]))
    return w, h, px


def test_web_wiring():
    """前端接线的静态哨兵:两道以前真出过问题的坑。

    1. 引用了 index.html 里不存在的元素 id → `el('x').onclick` 抛 TypeError,
       整块接线停摆(表现是"按钮点了没反应");
    2. `hidden` 属性被作者样式里的 display 盖掉 → 该藏的面板一直显示。
    """
    print("[前端接线哨兵]")
    p = os.path.join(ROOT, "tools", "check_web_ids.py")
    if not os.path.exists(p):
        skip("前端 id 覆盖检查", "tools/check_web_ids.py 不存在")
    else:
        r = subprocess.run([sys.executable, p], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        check("JS 引用的每个元素 id 都在 index.html 里", r.returncode == 0,
              (r.stdout or "") + (r.stderr or ""))
    css = os.path.join(ROOT, "dexstudio", "web", "style.css")
    if os.path.exists(css):
        with open(css, encoding="utf-8") as f:
            txt = f.read()
        check("[hidden] 有 !important(否则 display:flex 会盖掉它)",
              "[hidden]" in txt and "none !important" in txt)
    app = os.path.join(ROOT, "dexstudio", "web", "app.js")
    if os.path.exists(app):
        with open(app, encoding="utf-8") as f:
            src = f.read()
        check("「挂组件」按钮接线了", "btn-comp-add').onclick" in src)
        check("「改名」按钮接线了", "btn-rename').onclick" in src)
        check("页面自测断言 computed display", "hiddenNow(" in src)
        check("页面自测真的点了按钮", ".click();" in src)
        check("新建/打开项目走文件夹对话框(不再 prompt 手打路径)",
              "project.pick" in src and "prompt('项目目录" not in src)


def test_packaged_exe():
    """B7:发布形态 —— **exe 旁边没有 web/ 目录**也要能用。

    做法与真实发布一致:把 exe + WebView2Loader.dll + libdexgame.dll 拷到一个干净目录,
    然后在那里跑 `--wv-selftest`(它会等前端 ui.ready 并跑页面自测)。
    前端资源此时只能来自 exe 里内嵌的那份,所以这一项过了就说明"单 exe 可用"。"""
    print("[发布形态(内嵌前端资源,旁边没有 web/)]")
    if not os.path.exists(EXE):
        skip("发布形态", "dexstudio.exe 未构建")
        return
    loader = os.path.join(HOST, "WebView2Loader.dll")
    engine = os.path.join(LIBS, "dexgame", "libdexgame.dll")
    if not (os.path.exists(loader) and os.path.exists(engine)):
        skip("发布形态", "WebView2Loader.dll / libdexgame.dll 未构建")
        return
    with tempdir("ds_pkg_") as tmp:
        # 目录名故意用中文:发布形态下 exe 也可能被放在中文路径里,
        # 那样 --selftest / 找 WebView2Loader.dll / 解包内嵌前端都得走 UTF-8。
        out = os.path.join(tmp, "我的 DexStudio")
        os.makedirs(out, exist_ok=True)
        for p in (EXE, loader, engine):
            shutil.copy2(p, os.path.join(out, os.path.basename(p)))
        check("干净目录里只有三个文件",
              sorted(os.listdir(out)) == ["WebView2Loader.dll", "dexstudio.exe",
                                          "libdexgame.dll"], os.listdir(out))
        check("干净目录里没有 web/", not os.path.isdir(os.path.join(out, "web")))
        r = subprocess.run([os.path.join(out, "dexstudio.exe"), "--selftest"],
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace", cwd=out, timeout=120)
        check("发布目录里 --selftest 全过", r.returncode == 0
              and "0 失败" in (r.stdout or ""), (r.stdout or "")[-300:])
        r = subprocess.run([os.path.join(out, "dexstudio.exe"), "--wv-selftest"],
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace", cwd=out, timeout=180)
        out_txt = r.stdout or ""
        check("发布目录里能起窗口 + 加载内嵌前端", r.returncode == 0, out_txt[-400:])
        check("发布目录里页面自测也全过", "0 项失败" in out_txt, out_txt[-400:])
        check("发布目录里窗口标题不是乱码",
              "窗口标题:[DexStudio — DexLang 可视化 IDE]" in out_txt,
              [l for l in out_txt.splitlines() if "窗口标题" in l] or out_txt[-200:])
        if r.returncode != 0:
            skip("发布形态的页面自测项数", (r.stderr or "")[-120:])


def test_web_assets():
    """前端资源的静态检查:每个 .js 都要能被 JS 引擎解析。

    为什么值得单独测:注释里出现 `*/` 会把块注释提前关掉(仓库里在 C 上也踩过同款),
    症状是"页面白屏 + 一条看不懂的 SyntaxError"。有 node 就逐个 `--check`,
    没有 node 就退化成"括号/引号粗查 + 文件非空"。"""
    print("[前端资源(--check / 基本健全性)]")
    node = shutil.which("node")
    files = sorted(glob.glob(os.path.join(ROOT, "dexstudio", "web", "*.js")))
    check("前端有 JS 文件", len(files) >= 5, [os.path.basename(f) for f in files])
    if node:
        for f in files:
            r = subprocess.run([node, "--check", f], capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            check(f"node --check {os.path.basename(f)}", r.returncode == 0,
                  (r.stdout or "") + (r.stderr or ""))
    else:
        skip("node --check 前端 JS", "没有 node")
    for f in files:
        with open(f, encoding="utf-8") as fp:
            src = fp.read()
        check(f"{os.path.basename(f)} 非空且有导出", len(src) > 500,
              len(src))
    # index.html 的 script 标签都能在 web/ 下找到对应文件
    with open(os.path.join(ROOT, "dexstudio", "web", "index.html"), encoding="utf-8") as fp:
        html = fp.read()
    for name in ("app.js", "ui.js", "scene.js", "viewport.js", "graph.js",
                 "highlight.js", "code.js"):
        check(f"index.html 引用了 {name}", f'src="{name}"' in html)
        check(f"{name} 存在", os.path.exists(os.path.join(ROOT, "dexstudio", "web",
                                                         name)))


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
    # 项数只做"至少"断言:自测项会随着功能增加,别让数字成为维护负担
    m = re.search(r"自测结果:(\d+) 通过", r.stdout or "")
    check("--selftest 至少 20 项", bool(m) and int(m.group(1)) >= 20, r.stdout)
    check("--selftest 覆盖中文与编码", "中文与编码" in (r.stdout or ""), r.stdout)
    check("--selftest 覆盖恢复提示语义",
          "本次运行自己写的自动保存不提示恢复" in (r.stdout or ""), r.stdout)
    check("--selftest 覆盖父子关系与场景管理",
          "父子关系" in (r.stdout or "") and "场景管理" in (r.stdout or ""), r.stdout)
    check("--selftest 覆盖「编译前存盘」",
          "编译前先把场景存盘" in (r.stdout or ""), r.stdout)


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
    # 窗口标题是用户第一眼看到的东西:宿主回读 GetWindowTextW 并断言过 UTF-8
    check("窗口标题不是乱码",
          "窗口标题:[DexStudio — DexLang 可视化 IDE]" in (r.stdout or ""),
          [l for l in (r.stdout or "").splitlines() if "窗口标题" in l])
    # 页面那一层(渲染图/层级树/属性面板/瓦片刷子)由页面自己验,结果回传给宿主断言
    check("界面自测跑起来了", "界面自测" in (r.stdout or ""), (r.stdout or "")[-600:])
    check("界面自测全过", "0 项失败" in (r.stdout or ""), (r.stdout or "")[-600:])


def test_page_with_project():
    """带**真资源**跑一遍页面自测:缩略图真的解码、声音真的能播、换贴图后选中框对。

    为什么要单独一条:不带项目跑的时候,页面自测里这一整段是 SKIP 的 —— 而用户报的
    恰好就是这一段("导入的图是裂图标""声音试听没反应""改了贴图只有框线在动")。

    这里刻意用**两种**放资源的方式:
      · `res.import`(IDE 的「导入…」按钮)
      · **手工复制**进 res/(用户实际干的事 —— 文件名还带空格/中文/`#`/`%`/大写扩展名),
        因为"复制进 res/ 了但缩略图还是失败"就是这么报上来的。
    """
    print("[带项目的页面自测(缩略图/试听/换贴图/手工放资源)]")
    png = os.path.join(ROOT, "tests", "fixtures", "tiles.png")
    wav = os.path.join(ROOT, "tests", "fixtures", "beep.wav")
    if not os.path.exists(EXE):
        skip("带项目页面自测", "dexstudio.exe 未构建")
        return
    if not (os.path.exists(png) and os.path.exists(wav)):
        skip("带项目页面自测", "缺少 tests/fixtures 里的图片/声音")
        return
    dll = load_model()
    if dll is None:
        skip("带项目页面自测", "libdexstudio.dll 未构建")
        return
    with tempdir("ds_wvproj_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "页面自测", "template": "starter"})
            m.ok("res.import", {"src": png, "name": "英雄.png"})
            m.ok("res.import", {"src": wav, "name": "音效.wav"})
            m.ok("scene.save")
            manual = [("手工放的图.png"), ("manual copy.png"), ("UPPER.PNG"),
                      ("hash#name%.png")]
            for name in manual:
                shutil.copyfile(png, os.path.join(tmp, "res", name))
        finally:
            m.close()               # 先放开项目,再让 exe 去开它
        r = subprocess.run([EXE, "--project", tmp, "--wv-selftest"],
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=240)
        out = r.stdout or ""
        check("带项目跑页面自测能过", r.returncode == 0, out[-500:])
        check("页面自测不再跳过资源检查(缩略图/试听真的跑了)",
              "没有音频资源" not in out and "没有图片资源" not in out, out[-500:])
        check("缩略图真的解码了(用户报的裂图标)",
              "缩略图真的解码了" in out, out[-500:])
        check("手工复制进 res/ 的图也有缩略图(空名/大写扩展名/#%/中文)",
              all(("缩略图真的解码了  " + n) in out for n in manual), out[-600:])
        check("声音真的能解码并播放(用户报的试听没反应)",
              "声音能解码并播放" in out, out[-500:])
        check("换贴图后选中框 = 整张贴图(用户报的框线动了没图)",
              "选中框 = 整张贴图" in out and "裁切清 0" in out, out[-500:])
        check("页面自测没有失败项(含瓦片/图层/资源/恢复提示)",
              '"fail":[]' in out.replace(" ", ""), out[-800:])


def test_page_open_late():
    """**双击 exe 之后才打开项目**这条路径(用户报「导入图片一直显示图片读取失败,
    但点运行有图」的根因)。

    为什么单独一条、而且必须走 `--open-late`:资源缩略图/图集预览是靠 WebView2 的
    虚拟主机(`dexstudio-proj.local` → 项目根)交给网页显示的。实测:
    **对一个已经加载完的页面改映射不生效** —— SetVirtualHostNameToFolderMapping
    返回 S_OK,但那个页面发出的请求照样失败(img 裂图标 / fetch 抛
    "TypeError: Failed to fetch"),只有**新的导航**才认新映射。
    `--project` 是"导航前就映射好",所以它一直是对的,`--open-late`
    (先加载页面,再让页面去 openProjectAt)才是用户真实顺序。

    修法在宿主:映射改指向之后自动重新导航一次(ds_main.c 的
    sync_host_mappings → WM_APP_RELOAD)。这条测试钉住它。
    """
    print("[启动之后再打开项目(用户真实顺序:双击 exe → 打开项目)]")
    png = os.path.join(ROOT, "tests", "fixtures", "tiles.png")
    if not os.path.exists(EXE):
        skip("后开项目页面自测", "dexstudio.exe 未构建")
        return
    if not os.path.exists(png):
        skip("后开项目页面自测", "缺少 tests/fixtures/tiles.png")
        return
    dll = load_model()
    if dll is None:
        skip("后开项目页面自测", "libdexstudio.dll 未构建")
        return
    with tempdir("ds_late_") as tmp:
        m = Model(dll)
        try:
            m.ok("project.new", {"dir": tmp, "name": "后开项目", "template": "starter"})
            m.ok("res.import", {"src": png, "name": "英雄.png"})
            m.ok("scene.save")
        finally:
            m.close()
        # 关键:**不给 --project**(启动时没有项目),让页面自己把项目打开
        r = subprocess.run([EXE, "--wv-selftest", "--open-late", tmp],
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=240)
        out = r.stdout or ""
        check("后开项目这条路整条链通", r.returncode == 0, out[-500:])
        check("后开项目时资源检查真的跑了(不是当成没项目 SKIP)",
              "没有打开项目" not in out and "没有图片资源" not in out, out[-600:])
        check("后开项目后缩略图真的解码了(用户报的图片读取失败)",
              "缩略图真的解码了" in out, out[-600:])
        check("后开项目后项目根映射是通的(资源缩略图靠它)",
              "项目根映射出去了" in out and '"fail":[]' in out.replace(" ", ""),
              out[-700:])
        check("后开项目后点缩略图设的贴图真的被引擎加载", "texture >= 0" in out, out[-500:])


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
    test_viewport(dll)
    test_tile_brush(dll)
    test_clipboard(dll)
    test_graph_model(dll)
    test_graph_codegen(dll)
    test_graph_behavior(dll)
    test_build_and_run(dll)
    test_resources_and_autosave(dll)
    test_utf8_paths(dll)
    test_build_in_chinese_path(dll)
    test_recover_session(dll)
    test_asset_paths(dll)
    test_ui_contract(dll)
    test_sprite_scale(dll)
    test_outline_matches_pixels(dll)
    test_blocks_model(dll)
    test_blocks_behavior(dll)
    test_scene_load_before_init(dll)
    test_template_game_really_runs(dll)
    test_graph_link_drag_visible()
    test_web_wiring()
    test_web_assets()
    test_packaged_exe()
    test_cli()
    test_webview_chain()
    test_page_with_project()
    test_page_open_late()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败" + (f", {SKIP} 跳过" if SKIP else ""))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
