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
    # 页面那一层(渲染图/层级树/属性面板/瓦片刷子)由页面自己验,结果回传给宿主断言
    check("界面自测跑起来了", "界面自测" in (r.stdout or ""), (r.stdout or "")[-600:])
    check("界面自测全过", "0 项失败" in (r.stdout or ""), (r.stdout or "")[-600:])


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
    test_cli()
    test_webview_chain()
    print(f"\n结果: {PASS} 通过, {FAIL} 失败" + (f", {SKIP} 跳过" if SKIP else ""))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
