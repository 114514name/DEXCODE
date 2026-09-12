# AGENTS.md — 项目状态与协作须知

> 本文件是**给 AI 协作者(以及人类)的项目交接说明**:当前状态、必须遵守的约定、
> 已知陷阱、待办。改动项目后请顺手更新本文件的「变更记录」与「当前状态」两节。
>
> 其他文档的分工(避免重复,请交叉引用而不是复制):
> - `README.md` — 面向使用者:怎么装、怎么跑、有哪些库与示例
> - `docs/SPEC.md` — 语言与字节码的**权威规格**(改格式必须同步这里)
> - `docs/MEMORY_DESIGN.md` — 内存模型的方案与**已实施/已撤销**结论
> - `docs/DEXGAME_DESIGN.md` — **dexgame 游戏引擎 + 可视化 IDE 的设计决策与里程碑**
> - `docs/TUTORIAL.md`、`docs/*_guide.html` — 教学材料
> - `docs/PITFALLS.md` — **踩过的坑全表**(本文件 §4 只留最近几条)
> - 本文件 — 状态、约定、陷阱、待办

---

## 1. 这是什么

一门自制语言 **DexLang** 的完整工具链:

```
.dex 源码 → Lexer → Parser(AST) → Compiler(栈式汇编 IR .dxasm) → Assembler(.dexbc)
                                                                      ↓
                          Disassembler(反汇编) ←→ C 字节码解释器 vm.exe → NCALL → DLL(FFI)
```

| 组件 | 位置 | 说明 |
|------|------|------|
| 语言前端 + 汇编器 | `dexlang/`(14 文件 ~3320 行) | 词法/语法/编译/汇编/反汇编/`.dexdef` 解析 |
| 调试用 Python VM | `dexlang/pyvm.py`(~580 行) | 与 C VM 语义一致的参照实现,IDE 断点/单步靠它 |
| C 字节码解释器 | `vm/vm.c`(单文件 ~1650 行) | 含原生 FFI(P0–P3 的内存模型改动与值数组 ABI 都在这里) |
| 原生库(7 个) | `libs/*/` | 纯 C 实现:`std` `math` `img` `ui` `egui` `gal` `dexgame` |
| **游戏引擎** | `libs/dexgame/`(~6800 行,9 个 .c + 4 个 .h + 1 个语言模块) | 模块化 2D 引擎,**M0.5–M4 全部完成**:D3D11 批渲染 + 实体/组件/场景(JSON)+ 物理/瓦片地图 + 输入/主循环 + 音频(XAudio2)+ 文字(DirectWrite)+ 静态内嵌变体;`eng_*` 共 **169** 个函数(含为 IDE 加的 `eng_object_id_at`/`eng_set_view`/`eng_set_asset_dir`)。见 `docs/DEXGAME_DESIGN.md` |
| **纯 C 工具链(产品 B 地基)** | `tools/dexc/`(7 个 .c + 1 个 .h,~3700 行) | **dexc.exe**:lexer/parser/compiler/assembler/disassembler/asmtext/.dexdef 全部从 Python 移植到 C,与 Python 前端**逐字节一致**(`tests/test_dexc.py` 478 项,拿全仓库 34 个 .dex + 一批故意写错的源码对照字节码/汇编文本/错误/警告)。编译这一步从此不需要 Python —— `dexc.exe` + `vm.exe` 即可完成"源码 → 可运行游戏" |
| **可视化 IDE(产品 B)** | `dexstudio/`(`host/` 11 个 C 文件 ~6300 行 + `web/` 11 个文件) | **DexStudio**:C 宿主 + 内嵌 **WebView2**(单窗口,HTML/CSS/JS 前端)。**模型层在 C**(`libdexstudio.dll`:项目/场景/撤销/视图/瓦片/积木/命令通道),场景数据直接复用引擎的 `eng_scene_json` 与 `eng_comp_*`/`eng_field_*` 自省 → 引擎加组件不用改 IDE。**B3 场景 + B4 节点图 + B5 编译/运行 + B6 资源/自动保存 + B7 打包 + B8 中文编码 + B9 资源根 + B10 体验大修 + B11 Scratch 式积木模式** 已完成。前端三档:`积木`(零基础,句子积木 + 全下拉,不连线不打字;`scripts/blocks.json`)→ `节点`(UE 蓝图式) → `代码`;字段元数据驱动的控件(枚举下拉/取色器/资源与实体下拉/只读标注)、父子层级树、场景管理、一键编译**先存盘**、生成前校验、资源改名同步引用、关窗未保存确认、单实例。测试:ctypes 调模型 + 无窗口 CLI + 离屏 WebView2 自测 + `node --check` + **页面自测(不带项目 95 / 带项目 120)** + 发布形态 + 中文路径 + 真实项目像素断言(`tests/test_dexstudio.py` **479 项**;`--selftest` **51 项**)。细节/未做项见 `docs/DEXSTUDIO_UX_ISSUES.md`,见 `docs/DEXGAME_DESIGN.md` §9 |
| 集成开发环境 | `dexide/`(8 文件 ~4120 行) | tkinter,零第三方依赖 |
| GAL 蓝图编辑器 | `bluedit/`(15 文件 ~7180 行) | UE 风格节点连线 → 生成 DexLang 代码 |
| 测试 | `tests/`(30 个测试文件 + `_tmpdir.py`,~12900 行) | 全部是**手写 check() 脚本**,不用 pytest |

**零第三方 Python 依赖**是刻意设计(只用标准库)。唯一可选外部依赖是
`ziglang`(pip 包,提供 C 编译器)。

> 已删除:`galide/`(Python 旧版 GAL 编辑器,Scratch 式指令块)。它与 `bluedit/`
> 有 **104 个同名定义**(`engine.py` 近乎整文件级重复),且两者曾共用同一个
> `~/.galide.json`。详见「变更记录」。旧 `.galscene` 工作流现由
> `tools/legacy_galedit_c/`(C 版)与 `bluedit/` 承接。

---

## 2. 当前状态(请以本节为权威)

- 分支 `main`,工作树干净。
- **测试:全量 30 个脚本通过 / 0 失败**(逐文件数字见下方基线,以实际运行为准)。
- 跟踪约 210 个文件、约 38 MB。**体积构成容易被误判**:`examples/**/res/` 的示例媒体
  占 **28 MB**(单张 jpg 2–3 MB,单个 mp3 3–4.5 MB),vendor 的 **WebView2 SDK 头文件**
  占 **3 MB**(`dexstudio/host/third_party/`),而「刻意入库以便 clone 即用」的二进制
  (`vm/*.exe` + `libs/*/lib*.dll` + `tools/dexc/dexc.exe` + `dexstudio/host/*.exe|dll`)
  只有约 **3.4 MB**。要让仓库瘦身,该动的是示例媒体,不是二进制。
- 版本控制**刚建立**(2026-09),此前项目无 git;历史提交从「初始导入」开始。
- 远端 `origin` = `git@github.com:114514name/DEXCODE.git`。**本机到 `github.com:22`
  的连接会被对端直接关闭**(TCP 其实通,但握手被切断),必须走 GitHub 官方备用通道
  `ssh.github.com:443` —— 已写入 `~/.ssh/config` 的 `Host github.com` 块(见 `docs/PITFALLS.md` 的对应两条)。

### 测试基线(改动后请对照)

```bash
# 逐个跑(每个都是独立脚本,退出码 0/1);也可直接跑 python _status_run2.py 之类的批处理
python tests/test_toolchain.py   # 29  核心工具链 + C VM + 往返
python tests/test_native.py      # 41  include/refer + .dexdef + arity 上限 + DLL 调用
python tests/test_static.py      # 24  静态链接(含 dexgame 静态内嵌,M4-d)
python tests/test_stdlib.py      # 36  标准库
python tests/test_struct.py      # 18  type/结构体 + 双 VM 一致性
python tests/test_call.py        # 12  call() 动态调用
python tests/test_module.py      # 7   .dex 语言模块
python tests/test_img.py         # 13  图片渲染库
python tests/test_ui.py          # 27  WinAPI UI 库
python tests/test_egui.py        # 12  EGUI 库
python tests/test_gal.py         # 30  GAL 引擎(含 BMP 像素断言)
python tests/test_pyvm.py        # 14  Python 调试 VM
python tests/test_ide.py         # 123 DEXIDE 分析器 + 编辑器
python tests/test_designer.py    # 22  EGUI Designer
python tests/test_bluedit.py     # 180 GAL 蓝图模型/导出
python tests/test_nopydep.py     # 7   脱离 Python 运行
python tests/test_vm_versions.py # 13  三个 VM 变体 + release 打包
python tests/test_robust.py      # 24  健壮性(畸形字节码/循环对象/整型边界)
python tests/test_memmodel.py    # 44  内存模型(P0–P3 + FFI 契约)
python tests/test_editor.py      # 18  旧版 C 编辑器 .galscene 往返 + 导出链路
python tests/test_abi.py         # 37  原生调用「值数组 ABI」(M0.5)
python tests/test_dexgame.py     # 59  dexgame 引擎 D3D11 渲染核心(M1,含像素断言)
python tests/test_scene.py       # 145 dexgame 实体/组件/JSON 场景(M2,含像素断言)
python tests/test_phys.py        # 193 dexgame 碰撞/查询/运动学/瓦片地图(M3,含像素断言)
python tests/test_input.py       # 78  dexgame 输入/动作映射/主循环(M4-a)
python tests/test_audio.py       # 81  dexgame 音频(XAudio2 + 手写 WAV 解析,M4-b)
python tests/test_text.py        # 46  dexgame 文字(DirectWrite + 字形图集,M4-c)
python tests/test_examples.py    # 30  examples/dexgame/*.dex 编译守卫(防示例悄悄烂掉)
python tests/test_dexc.py        # 478 纯 C 工具链 dexc 与 Python 前端**逐字节一致**
python tests/test_dexstudio.py   # 479 DexStudio 模型/积木与逻辑图/编译运行/资源自动保存 + 前端资源/接线哨兵 + CLI + WebView2 + 发布形态 + **中文路径/恢复语义/资源根/字段元数据/父子/一键编译存盘/未初始化加载场景/生成的游戏真的跑起来** + 页面自测(不带项目 95 项、带项目 120 项)
```

合计 **30 个脚本 / 236 个 `def test_*` 函数**(`check()` 常在循环里被多次调用,所以
**实测执行数 > 静态调用数**);实际执行数随平台与是否构建 `vm.exe` 而变。
本文件不逐条维护各项数字,以实际运行为准(改动后请把上面这行数字顺手改掉)。

> `tests/_tmpdir.py` 不是测试文件,是测试共用的「可写临时目录」工具。**新增需要临时
> 目录的测试请用它,不要用 `tempfile.mkdtemp`** —— 原因见 `docs/PITFALLS.md` 的「mkdtemp 的 0o700 ACL」。

### 构建

```bash
python main.py build-vm        # 构建 vm.exe / vmnc.exe / galrun.exe
build_libs.bat                 # 构建 7 个原生库 DLL(需要 ziglang)
python main.py build-dexc      # 构建纯 C 工具链 tools/dexc/dexc.exe
python main.py build-dexstudio # 构建 DexStudio(含把 web/ 内嵌成 ds_embed.c)
python main.py package-dexstudio  # 打包发布目录 dist/DexStudio(exe + 两个 DLL + README)
```

**可复现性:VM 的二进制度可复现,库的 DLL 不可复现。** 实测:
- `main.py build-vm` 重编出的三个 exe 与仓库里已提交的**逐字节一致** ——
  所以 VM 的二进制 diff 只在 `vm.c` 真改动时才出现。
- `build_libs.bat` 重编出的 DLL **每次都不同**(PE `TimeDateStamp` 每次变,
  加 `SOURCE_DATE_EPOCH` 也无效),所以重建库文件必然产生二进制 diff;
  这与改动是否相关无关,不要去追。

### 当前内存行为(不读 MEMORY_DESIGN 容易误判)

- **不回收**运行期产生的字符串(拼接结果、原生字符串返回)→ 长驻程序内存只增不减。
  这是**刻意选择**,不是遗漏:P4(引用计数)实现过并撤销,理由见
  `docs/MEMORY_DESIGN.md` 6.1。
- **结构体是值语义**(P3):`let b = a;` 深拷贝,改 `b.x` 不影响 `a.x`;传参也是副本。
- **递归类型被拒绝**:`type N { self: N; }` 编译报错 —— 这让环在**类型层面不可表达**,
  因此不需要环收集器。
- **内存预算**:环境变量 `DEXCODE_MAX_MEM_MB`(0=不限,**默认不限**);
  超限时明确报错并指出常见原因。
- **字符串累加是 O(n²) 时间**(与内存无关):循环内 `s = s + "x"` 20,000 次约 0.5 s。
- **UTF-8 是唯一的字符串编码**(B8 起):引擎、宿主、dexc 的字符串全是 UTF-8,
  交给 Windows 之前一律转宽字符(`ds_utf8.c` / `dg_utf8.c` / `dx_utf8.c`)。
  **不要**再直接用 `fopen`/`GetFileAttributesA`/`FindFirstFileA`/`CreateProcessA`/
  `LoadLibraryA`/`GetModuleFileNameA` —— 中文系统(ACP=936)下它们会把 UTF-8 当 GBK 解。

---

## 3. 必须遵守的约定

### 3.1 编码:UTF-8 无 BOM(**这条踩过大坑**)

**不要用 PowerShell 的 `Set-Content` / `Out-File` 重写源码文本文件。**
PowerShell 5.1 会把 UTF-8 内容按 GBK 解读后再按 UTF-8 写出,导致中文注释变成乱码
并加上 BOM。**`vm.c` 曾因此损坏 118 行**,当时只能靠打包目录里的旧快照恢复
(那个快照现已删除,恢复手段改为 git)。

- 用 `edit`/`write` 工具或支持 UTF-8 的编辑器;
- 若必须用脚本批量改写,用 Python 并显式指定:
  `open(path, 'w', encoding='utf-8', newline='')`
- **`.bat` 文件必须是纯 ASCII**:cmd.exe 按 OEM 代码页解析批处理文件,
  UTF-8 中文会变成乱码命令。

### 3.2 换行:不要整体转换

仓库原本是**混合**换行(既有 `.py`/`.md` 为 CRLF,`.c` 为 LF)。
改写既有文件时**不要把它整体变成另一种换行** —— 这会让 `git diff` 显示全文件重写,
掩盖真实改动。

仓库**刻意不使用 `.gitattributes`**:任何 `text=auto` 之类的规则都会让 git 持续
重新规范化,使工作树长期显示大量"已修改"。相关提交历史见
`revert: 移除 .gitattributes` 与 `docs: 开发约定`。

### 3.3 编码列号的基准(容易搞错)

- **词法器的 `col` 是 1 基**(`type` 在 col=1 对应行首字符);
- **Tk 的 `"L.1"` 是第 2 个字符**(首字符是 `"L.0"`)。

因此编辑器里「1 基列 → Tk 索引」的换算见 `dexide/editor.py` 的 `_idx()`,
**不要**写成 `"L.0 + Nc"` —— Tk 只解析一次 `+`,那种表达式会静默退化为行首。

### 3.4 临时目录:测试里用 `tests/_tmpdir.py`,不要用 `tempfile.mkdtemp`

`tempfile.mkdtemp()` 内部是 `os.mkdir(path, 0o700)`;`0o700` 在 Windows 上会落成
「仅属主」ACL,受限沙箱下当前进程写不进也删不掉该目录(详见 `docs/PITFALLS.md`)。测试统一改用
`tests/_tmpdir.py` 的 `mktempdir()` / `tempdir()`:同样的接口,但目录以 `0o777` 创建,
且优先放在系统临时目录(正常机器行为与 `tempfile` 等价,只在系统临时目录不可用时
回退到仓库内 `_tmptest/`)。

### 3.5 其他

- **提交前**:工作树干净 + 全量测试通过。
- **生成产物不入库**:`*.dxasm`、`*.dexbc`、`*.do`、`scene_out.dex` 等
  (可由 `.dex` 重建),见 `.gitignore` 注释。
- 二进制(`vm/*.exe`、`libs/*/lib*.dll`、`tools/legacy_galedit_c/galedit.exe`、
  `tools/dexc/dexc.exe`)刻意入库:仓库缺少部分构建脚本时,不入库会导致 clone 后
  无法运行示例与测试。
- **原生 arity 上限有两个来源,别搞混**(见 SPEC 4.5):`dexlang/opcodes.py` 的
  `MAX_NATIVE_ARITY = 3`(直接 ABI)与 `MAX_NATIVE_ARGS = 16`(值数组 ABI)。
  要功能上放宽到 3 参以上,得让 `.dexdef` 声明 `abi value_array;`,不是改常量。
  `vm/vm.c` 的 `MAX_NATIVE_ARGS` 是同一件事的 C 侧副本,两边必须一致。
  **后者只是存储常量,不是格式约束**(字节码里 arity 本就是 u8、表条目本就是变长的
  `6+arity`),所以调大它不需要改格式、不影响旧字节码 —— M1 就把它从 8 提到了 16,
  因为 `eng_draw_uv` 有 10 个参数。
- **`.dexdef` 的 `abi` 是文件级指令**:一行影响该文件全部 `extern`,可出现在任意位置
  (解析完再统一校验),同一文件只能出现一次。
- **DexLang 没有位移运算**(词法器里没有 `<<`/`>>`),所以从 r/g/b/a 拼颜色必须借
  原生辅助(`eng_rgba`)。写引擎/库 API 时别假设用户能自己位运算。

---

## 4. 已知陷阱(踩过并记录)

**完整表格见 `docs/PITFALLS.md`**(M0.5 → B11,79 条)。
这里只留最近一阶段的几条,免得本文件超出工作区指令预算被截断。

| 陷阱 | 现象 | 应对 |
|------|------|------|
| **`hidden` 属性被作者样式的 `display` 盖掉**(B10,用户报的) | `style.css` 里 `.recover{display:flex}` / `.row{display:flex}` 这类规则**作者样式优先于浏览器默认样式**,于是 `[hidden]{display:none}` 失效:恢复提示条、橡皮选项、改名行在"该藏"的时候照样显示。JS 里 `el.hidden = true` 看起来生效了,写测试也只断言 `.hidden` 属性,所以长期没人发现 | `style.css` 顶部加 `[hidden] { display: none !important; }`;**断言要断 `getComputedStyle(el).display === 'none'`,不要断 `.hidden` 属性** —— 这两句话合起来才算把这类问题钉死(页面自测里 4 个元素逐个断 computed display) |
| **按钮引用了不存在的元素 id = 整块接线停摆**(B10) | `el('btn-x').onclick = …` 在 `init()` 里跑,id 写错/漏在 HTML 外就抛 TypeError,**后面所有按钮的接线都不会执行** —— 现象是"好几个按钮一起点了没反应",很难看出源头 | `tools/check_web_ids.py`:把 `$('…')` / `el('…')` / `getElementById('…')` 里的 id 与 `index.html` 的 id 集合对照,任何缺失直接退出码 1(已进 `tests/test_dexstudio.py`)。判据:**HTML 是前端的"接口",接口名要对得上,而且要有自动检查** |
| **引擎没初始化就干活:游戏 60ms 跑完 / 场景"加载成功"却是空壳**(B11,实测) | 两个连着的坑:① IDE 生成的 `main.dex` 模板从不 `eng_init`,而 `eng_run` 的循环条件是 `while eng_running()` —— 没窗口它恒为 0,于是 on_start 跑一遍就退出(**60ms、exit 0、零输出**,用户看到的是"点运行什么都没发生");② `dg_comp_kind()` 在组件池没建时返回 -1,而场景加载把 -1 当成"不认识的组件"**静默跳过** → 实体/名字都在,`eng_find` 找得到,但 transform/sprite 一个都没挂上 | 模板在 `eng_run` 前显式 `eng_init(...)`;`eng_run`/`eng_run_frames` 自己兜底自动开窗;`dg_comp_kind()`/`dg_scene_parse()` 按需 `dg_scene_init()`。判据:**"未知"和"还没准备好"必须能区分**;`test_template_game_really_runs`(跑起来 2 秒后进程必须还活着)+ `test_scene_load_before_init`(未初始化也要装上组件)钉住 |
| **同一个坐标公式在引擎和宿主里各写一遍**(B11,用户报的) | 引擎画精灵是 `原点 = 世界坐标 - 源尺寸 × 轴心 × 缩放`,宿主 `scene.outline` 写的是 `世界坐标 + 轴心` → 框线偏到图片外面,用户看到的就是**"改了贴图只有框线在动、图像没出现"**。同类:视口用**活动相机的 camera.x/y/zoom**,而 `app.info` 报的是编辑器视图覆盖值 → 相机一挪,画面走、框线不走 | 宿主照抄引擎公式并注明"必须与引擎逐字相同";没有视图覆盖时 `app.info`/`scene.render` 返回**实际生效**的活动相机视图。判据:**凡是"引擎怎么画"的公式,宿主只能有一份** —— 靠 `test_outline_matches_pixels`(比框线与像素包围盒)锁住 |

> 加新陷阱时:**写进 `docs/PITFALLS.md` 的表尾**,再把本节的最后一条挤出去。

---

## 5. 架构要点(改代码前先读)

### 5.1 字节码格式与权威定义

- 格式常量在 `dexlang/opcodes.py`(版本 3);`vm/vm.c` 里有一份对应的 `#define`/枚举,
  **两边必须一致**。
- 改格式:同步 `docs/SPEC.md` 第 5 节 + `opcodes.py` + `vm.c` + `disassembler.py`
  + `assembler.py`,并跑 `python main.py roundtrip <bc>` 验证往返逐字节一致。
- **已有一次向后兼容的格式扩展**:代码段之后的可选 `DXRL` 尾节(记录各库的字符串
  释放函数)。它不改版本号、不改定长表布局,旧 VM 只读 `code_size` 之前的内容。
  见 `docs/SPEC.md` 5.1。
- **第二次向后兼容扩展(M0.5)**:原生函数表 `ret_type` 字节的**位 7** 作为调用约定
  标记(`0` 直接 ABI / `1` 值数组 ABI)。该字节有效值只有 0..3,高位本来是空的,故
  **旧字节码该位恒为 0**,新旧 VM 解析结果逐字节一致。见 `docs/SPEC.md` 4.5.3。
  两次扩展的共同思路都是**借用未使用的位/尾部空间**,而不是升版号 ——
  因为 `vm.c` 的版本检查是严格相等(`data[4] != VERSION`),升版号会让所有现存
  `.dexbc` 立刻失效。
- **M2/M3 完全不碰格式**:实体/组件/物理/瓦片全是 `libs/dexgame` 内部实现,新增的只是
  `eng_*` 导出(值数组 ABI)。所以这两轮的二进制 diff 只出现在 DLL 里,`vm/*.exe` 不变。
- **推论:M1 把 `MAX_NATIVE_ARGS` 从 8 提到 16 也没动格式**。它只是 `param_types`
  数组的容量。这条可作为以后判断"某个改动要不要动格式"的判据:**看字节码里有没有
  承载它的字段**(arity 是 u8、表条目本就是变长的 `6+arity`),有就不用动。
- **C 工具链(dexc)必须与 Python 前端逐字节一致**:`tools/dexc/` 是 `dexlang/` 的
  **移植**,不是"第二个实现"—— 两边对同一份源码必须给出相同的字节码、汇编文本、
  警告与错误信息。改任何一侧都要跑 `python tests/test_dexc.py`(478 项:全仓库
  34 个 .dex 四项对照 + ~90 个语言特性用例 + ~35 个错误路径 + .dexdef + 反汇编往返)。
  这与 §5.2「两个 VM 必须语义一致」是同一条规律:**双实现的一致性靠测试锁住,
  不靠"我记得两边都改了"**。

### 5.2 两个 VM 必须语义一致

`vm/vm.c`(执行)与 `dexlang/pyvm.py`(IDE 调试)是独立实现。
**改一个必须同步改另一个**,否则 IDE 调试结果会误导用户
(`test_call.py`/`test_struct.py`/`test_memmodel.py` 都有双 VM 一致性断言)。
历史上就出现过对象比较语义不一致。

### 5.3 编辑器的两个基础件

`dexide/editor.py` 的着色与缩进都建立在 `dexide/syntax.py` 之上:

- `syntax.scan(src)` → 注释区间、字符串区间。**词法器不产出 COMMENT token、
  也不报告注释区间**(它面向编译),所以这部分信息靠文本扫描补;
- `syntax.code_lines(...)` → 把注释/字符串内容替换为**等长空格**的"掩码代码"。
  括号深度、缩进、折叠块判定都必须基于它,否则注释里的 `{` 会造成假缩进/假折叠;
- 性能:这些函数在**每次光标移动/输入**后都会跑。已做三处优化
  (`str.find` 代替逐字符、正则挑括号、按内容缓存扫描结果);
  往这条路径加东西时请注意复杂度。

### 5.4 DexStudio 的分层(产品 B:模型在 C,视图在 JS)

```
dexstudio/web/           HTML/CSS/JS:只做视图与交互(视口、树、属性面板…)
      ↑↓  WebView2 的 postMessage(唯一通道,请求/响应用 id 配对)
dexstudio/host/ds_webview.c   WebView2 封装(运行时加载 Loader、虚拟主机映射、导航、eval)
dexstudio/host/ds_main.c      窗口宿主 + 无窗口 CLI(--command / --selftest / --wv-selftest)
dexstudio/host/ds_model.c     模型层:项目 / 场景 / 撤销 / **命令分发** ← 全部逻辑在这
dexstudio/host/ds_engine.c    对 libdexgame.dll 的动态绑定(值数组 ABI)
dexstudio/host/ds_json.c      极小 JSON DOM(编辑器要"取字段、改字段、写回去")
      ↑
libdexgame.dll          场景的权威表示:eng_scene_json / eng_scene_load_json / eng_comp_* 自省
```

三条约定:

1. **逻辑写在 C,JS 只画**:所有状态改动都经 `ds_command(json) → json`,JS 不做业务
   判断。好处是"UI 能做的"与"测试能断言的"是同一件事 —— `tests/test_dexstudio.py`
   用 ctypes 调同一批命令,`--command`/`--selftest` 再用命令行跑一遍。
2. **场景不另造模型**:实体/组件/字段全部落在引擎里,属性面板的字段表来自
   `eng_comp_*`/`eng_field_*` 自省 —— 引擎加组件时 IDE 不用改一行。
3. **失败必须带得出原因**:`ds_command` 永远返回 `{"ok":true,…}` 或
   `{"ok":false,"error":"…"}`,错误串要具体到"哪个实体的哪个字段为什么不行"
   (来自 `eng_last_error()`)。前端的输出面板直接显示它。

测试入口(C 侧逻辑全部可无窗口验证):
```bash
python main.py build-dexstudio                 # 构建
dexstudio/host/dexstudio.exe --selftest        # 模型自测(51 项:中文/编码、下拉候选、父子、编译存盘、生成前校验…)
dexstudio/host/dexstudio.exe --command '{"cmd":"app.info"}'
dexstudio/host/dexstudio.exe --wv-selftest     # 窗口放屏幕外;等 ui.ready 后再让页面自测(加 --project <绝对路径> 会多跑 25 项资源/贴图检查)
python tests/test_dexstudio.py                 # 479 项(ctypes + 积木/逻辑图 + 编译/运行 + 资源/自动保存 + 前端资源/接线哨兵 + CLI + WebView2 + 发布形态 + 中文路径/恢复语义/资源根/字段元数据/父子/一键编译存盘 + 未初始化加载场景 + 模板游戏真的跑起来 + 页面自测 95/120 项)
```

**命令一览**(全部走 `ds_command`,前端与测试用的是同一批):
`app.info` / `app.components` / `ui.boot|ready|error` /
`project.new|open|pick|recent|save|state` / `scene.new|load|save|save_as|list|json|render|outline|options|rename|delete|set_start` /
`entity.list|get|add|remove|rename|set_pos|set_parent|find|duplicate|copy|paste` /
`comp.schema|add|remove|set` / `comp.set_many` /
`view.set` / `tilemap.create|info|set|paint|csv` / `undo` / `redo` /
`graph.types|options|validate|new|info|save|generate` / `graph.node.add|remove|duplicate|set|move|move_many` / `graph.link|unlink`
(逻辑图在 `scripts/logic.json`,生成 `scripts/logic.dex`)/
`build.compile|run|stop|status` / `project.scripts` / `file.read|open_external`
(一键编译 = 存盘 + 重生成逻辑图 + dexc;诊断带 `{level,phase,line,col,msg}`;`run detach:1` 用 vmnc 开独立窗口)/
`res.list|import|pick(multi)|delete(force)|rename(update_refs)|refs` / `autosave.tick|clear` / `recover.status|apply|discard`
(自动保存是**整包**:场景 + 瓦片 CSV + 逻辑图 → `.dexstudio/autosave.json`;`recoverable` 判据是"自动保存比场景新")。

**界面自测**:`dexstudio/web/app.js` 里的 `window.__ds_selftest()` 由页面自己跑
(渲染图真的解码成 1024×640 了吗 / 画布上真有非背景像素吗 / 世界↔屏幕往返自洽吗 /
层级树行数对不对 / 属性面板生成字段了吗 / 拖动真的写回位置了吗 / 瓦片真的落进 CSV
且能一次撤销吗 / 逻辑图模式切过去后画布真有尺寸吗 / 加节点后画布像素里有节点与连线吗),
结果用 `ui.selftest` 消息回传,`--wv-selftest` 断言“0 项失败”并
作为退出码。**这是“不看屏幕也能证明 UI 没坏”的那条路。**

---

## 6. 变更记录(新→旧)

> 每完成一项改动,在这里加一行;细节请写在提交信息里。
>
> **哈希基准**:下表哈希来自「首次推送前把占位身份 `DEXCODE Dev <dev@localhost>`
> 统一改写为 `114514name <feifan477@gmail.com>`」之后的历史。改写会改变**所有**
> 提交的哈希,故此刻之前的任何旧哈希记录均已失效。此后本仓库提交身份为
> `114514name <feifan477@gmail.com>`(本地 `git config`,未动全局配置)。

| 提交 | 内容 |
|------|------|
| (本次 B11) | **DexStudio:Scratch 式积木模式 + 用户报的四个"看不到效果"**(用户原话:"导入的预览图还是裂图标""改贴图了能看到框线改变,但并没有实际的图像被显示""声音预览也没有效果""这代码块设计是认真的吗…完全就是把代码换了一种形式!必须修改")。**① 积木模式(新)**:`dexstudio/host/ds_blocks.c`(~1370 行)+ `web/blocks.js`,16 种**句子积木**("让 [玩家] 往 [右] 走,速度 [中(每秒 150)]"),每个空都是下拉(实体/方向/速度/按键/动作/字段…),点一下加进脚本、▲▼ 调序、✕ 删、`如果…就` 有肚子(条件积木把动作装进去),全程不连线不打字;`scripts/blocks.json` 落盘,`logic_mode` 选 `blocks`/`graph`(新项目默认积木,老项目按 graph);一键示例"移动+跳跃 / 相机跟随 / 放音效"。**② 模板游戏 60ms 就跑完(实测)**:IDE 生成的 `main.dex` 从不 `eng_init`,而 `eng_run` 的循环条件是 `while eng_running()`(没窗口恒为 0)→ 用户按「运行」什么都不发生;修成模板显式 `eng_init(dexstudio_game_title(), 960, 540, 1)` + `eng_run` 自己兜底开窗。**③ 场景"加载成功"却是空壳**:`dg_comp_kind()` 在组件池没建时返回 -1,场景加载把 -1 当成"不认识的组件"静默跳过 → 组件池按需 `dg_scene_init()`。**④ 框线公式与引擎不一致**(引擎 `原点 = 世界坐标 - 源尺寸 × 轴心 × 缩放`,宿主写成 `世界坐标 + 轴心`)→ 这就是"只有框线在动、图像在别处"的根因;顺带视口改用**实际生效的活动相机**视图(`app.info`/`scene.render`)。**⑤ 旧模板的 32×32 裁切**(`sprite.sw/sh` 是"截取多大")→ 换贴图时自动改成整张(带 `note` 说明),打开老项目时内存里同样修正(不动用户文件、不算脏)。**⑥ 页面自测带项目时资源那段被整段 SKIP**(`DS.info` 还是 refresh 之前的)→ 自测自己记 `app.info`,并新增 `test_page_with_project` 钉住"缩略图真的解码 / 声音真的能播 / 换贴图后选中框 = 整张贴图"。**测**:`tests/test_dexstudio.py` 397 → **479 项**(新增 `test_blocks_model` / `test_blocks_behavior` / `test_scene_load_before_init` / `test_template_game_really_runs` / `test_outline_matches_pixels` / `test_page_with_project`),`--selftest` **51 项**,页面自测 **95(不带项目)/ 120(带项目)**,全量 30 脚本 0 失败。**零字节码格式改动**(引擎只改了 `dg_comp_kind`/`dg_scene_parse` 的按需初始化) |
| (本次 B10) | **DexStudio 体验/逻辑大修(用户报"用起来十分别扭"——逐条查证后一次性修)**。先用实证定位:`tools/check_web_ids.py`(前端 id 覆盖)、ctypes 直连模型、headless Edge 读 computed style、离屏 WebView2 自测、`dexc` 编译探针。**修掉的硬伤**:① 「挂组件」「改名」两个按钮**从来没有接线**(点了没反应,于是用户根本挂不上任何组件);② `[hidden]` 被 `.recover{display:flex}` 等作者样式盖掉 → 恢复提示条/橡皮选项/改名行**永远显示**(用户报的"一直提示没正常退出"根因在这里,B8 只修了判定逻辑);③ 「编译/运行」**不存盘** → 游戏跑的是磁盘旧场景;④ 运行的游戏**写死 `scenes/main.json`** → 改成生成 `scripts/project_info.dex` + `dexstudio_start_scene()`,老项目编译时兜底改写;⑤ 「保存」**不写逻辑图**;⑥ 新节点属性是空值却照样生成 `eng_find("")` 这种静默无效代码 → 默认值给成合法值 + **生成前校验**(缺属性拦住、实体不在当前场景只警告);⑦ `on_action` 类节点永不触发(谁都没绑默认键位)→ 生成 `logic_start` 时 `eng_bind_default_actions()`;⑧ 瓦片地图在界面上无法新建;⑨ 删掉当前场景后回退到 `<根>\main.json`(裸文件名当相对路径用)。**门槛改造**:字段元数据表(`label/kind/enum/min/max/step/unit/readonly`)→ 枚举下拉/布尔复选/位掩码/**取色器**/**资源与实体下拉**(带导入入口);逻辑图属性全部改下拉(实体/组件/**联动字段**/动作/**按键名**/鼠标键/音频);项目改用**文件夹对话框 + 最近项目 + 新建向导(模板单选)**;实体「+」先问模板;场景**下拉切换**+ 改名/删除/「游戏从这里开始」;图层整层 ↑↓;网格预设;父子层级树 + 拖拽设父 + `entity.set_parent`(带环检测);资源改名**同步引用**;关窗未保存确认;单实例互斥量;`web-<hash>` 旧目录清理;帮助面板;逻辑图框选多选/整组拖动(`graph.node.move_many`)/复制;输出面板默认只显示人话。**测**:`tests/test_dexstudio.py` 329 → **397 项**(新增 `test_ui_contract` / `test_web_wiring`),`--selftest` 22 → **51 项**,页面自测 68 → **102 项**(新增"computed display 真的隐藏"、"真的点按钮"、多选/整组拖动、图层整层移动);全量 30 个脚本 0 失败。**零字节码格式改动**(引擎一行没动,只有宿主/前端/工具链)。细节与未做项见 `docs/DEXSTUDIO_UX_ISSUES.md` |
| (本次 B9) | **DexStudio:资源路径(资源根)+ 逻辑图拉线预览 + 资源面板两处修复(用户真机报的两个症状)**。① **逻辑图拉线不跟鼠标**:`mousemove` 开头先判断 `drag` 就 return,而拉线用的是 `pending` → 预览线一动不动,松手才出现;把 pending 的更新挪到 guard 之前,并在页面自测里派发 mousedown/mousemove 后读 `Graph.dragState()` 断言"跟着走"。② **贴图设不上 / 缩略图裂图标**:根因是场景里存的是**项目相对**路径(`res/hero.png`),而引擎按自己的工作目录解析 → `comp.set sprite.tex_path` 直接失败(`texture` 停在 -1);缩略图的裂图标经排查是 B8 之前那版的乱码资源名导致的 URL 404(本版已好,新增"每张缩略图都真的解码一次"的断言钉住)。修法:引擎新增 `eng_set_asset_dir(dir)` + `dg_fopen_asset()`(相对路径拼资源根、绝对路径原样),IDE 在 `project.new/open` 时设成项目根 —— 场景里继续存相对路径,项目仍可搬(`eng_*` 168 → **169**)。顺带:`applyRes` 不再拿本地缓存当存在性依据(缓存过期就静默 return = "点了没反应"),改为现查 `entity.get`;`--wv-selftest` 的页面自测 62 → **68 项**(拉线预览 3 项 + 缩略图逐张解码 + 点缩略图设贴图 2 项 + 自测收尾会删掉自己建的 CSV)。**测**:`tests/test_dexstudio.py` 323 → **329 项**(新增 `test_asset_paths` / `test_graph_link_drag_visible`)。**零字节码格式改动** | 
| (本次 B8) | **DexStudio 中文/编码修复 + 恢复提示语义(用户报的三个症状)**。① **窗口标题乱码**:`CreateWindowExA` 把 UTF-8 标题按 ANSI(936)解 → 改 `SetWindowTextW`,并在 `--wv-selftest` 里 `GetWindowTextW` **回读断言**(不看屏幕也能证明)。② **恢复提示一直显示、按钮没用**:自动保存每 30 秒写一次(总比场景新),而 `recoverable` 不区分"谁写的" → 自动保存包里记 `session`,只提示**上一次运行**留下的;正常退出盖 `clean=1`(强杀盖不上)⇒ 前端能区分"崩溃"与"正常退出但没存盘";`file_mtime` 读不出来不再当 0(假可恢复)。③ **中文名全是乱码/找不到资源**:根因是 `ds_json.c` 的解析器把 UTF-8 的**字节**当码点又编了一遍(二次编码),叠加 Windows 窄字符 API 按 ANSI 解路径。于是新增三层 UTF-8 路径层(`dexstudio/host/ds_utf8.c`、`libs/dexgame/dg_utf8.c`、`tools/dexc/dx_utf8.c`),把 `fopen`/`GetFileAttributesA`/`FindFirstFileA`/`CopyFileA`/`MoveFileA`/`DeleteFileA`/`CreateProcessA`/`LoadLibraryA`/`GetOpenFileNameA`/`GetModuleFileNameA`/`argv` 全换成宽字符版本;`dsu_utf8_clean()` 保证"响应永远是合法 UTF-8"。顺带修两处真问题:`scene.new` 没把当前场景记进 `start_scene`(重开项目回到空场景);`project.new` 不能建多级目录。**测**:`test_dexstudio.py` 284 → **323 项**(新增 `test_utf8_paths` / `test_build_in_chinese_path` / `test_recover_session`),`--selftest` 14 → **22 项**(含"中文与编码"一节),页面自测新增 2 项措辞断言;**dexc 仍与 Python 前端逐字节一致(478 项)**。**零字节码格式改动** |
| (本次 B7) | **DexStudio B7:打包(前端资源内嵌 + 发布形态)**。新增 `tools/embed_web.py`(把 `dexstudio/web/*` 生成成 `dexstudio/host/ds_embed.c`:字节数组 + 表 + 内容哈希)与 `main.py package-dexstudio`(产出 `dist/DexStudio/`:exe + WebView2Loader.dll + libdexgame.dll + README.txt)。宿主找前端目录的顺序改成 `--web` → exe 同目录 `web/` → `exe/../../dexstudio/web` → **内嵌资源**(解包到 `%LOCALAPPDATA%\DexStudio\web-<哈希>` 再映射),于是发布出去**不需要 `web/` 目录**,而开发期仍然优先用磁盘上的那份。顺带修两个只在发布形态暴露的真问题:`--selftest` 的临时项目与「没有项目时的预览图目录」原本都写成 `exe_dir/../..\_zigtmp\...`,干净目录里不可写(视口渲染图解码成 `0×0`)—— 统一走新的 `ds_temp_dir()`。**测**:`tests/test_dexstudio.py` 279 → **284 项**,新增 `test_packaged_exe()`(把三个文件拷到**没有 `web/`** 的干净临时目录,跑 `--selftest` 与 `--wv-selftest`,后者含页面自测 58 项)。**零字节码格式改动** |
| (本次 B6) | **DexStudio B6:资源管理 + 自动保存/崩溃恢复**。新增 `dexstudio/host/ds_res.c`(~470 行)。**资源**:`res.list/import/pick/delete/rename` —— 导入是**复制**且同名报错不覆盖,资源名不许带路径(挡 `../`),`res.pick {dry:1}` 让自动测试不必弹模态对话框;缩略图/试听走虚拟主机 `dexstudio-proj.local`(与视口预览同一机制)。**自动保存**:`autosave.tick` 只在脏时写,存的是**整包**(场景 JSON + 场景引用的外部文件 = 瓦片 CSV / 逻辑图)到 `<root>/.dexstudio/autosave.json`,复用撤销那套快照原语;`recover.status/apply/discard` 的判据是"自动保存比场景文件新"(上次没正常收尾),`app.info` 多报 `recoverable`/`autosave_seq`,正常存盘后提示自然消失。前端:左栏资源面板(点图片=设选中实体的 `sprite.tex_path`、点声音=试听、右键=改名/删除)、顶栏下的恢复提示条(恢复/丢弃,不擅自恢复)、`startAutosave()` 每 30 秒敲一次(**策略在 C,节拍由 UI 带**)。**测**:`tests/test_dexstudio.py` 251 → **279 项**(B6 28 项),页面自测 48 → **58 项**(含 `fetch('https://dexstudio-proj.local/project.json')` 真拿到 200)。**零字节码格式改动** |
| (本次 B5) | **DexStudio B5:一键编译 + 独立窗口运行 + 输出面板 + 代码页签**。新增 `dexstudio/host/ds_run.c`(~580 行)与前端 `code.js`/`highlight.js`。`build.compile` = 存盘 + **重生成逻辑图** + `dexc.exe`,把 stdout/stderr 解析成结构化诊断(`{level,phase,line,col,msg}`,两种格式都认:`[parser]error at L:C:` 与 `warning: … at L:C`),失败不留字节码;`build.run detach:1` 用 **vmnc.exe**(无控制台)开独立游戏窗口,`wait:1` 用 vm.exe 收输出(无人值守);`build.stop`/`build.status` + **销毁模型自动收进程**(不留孤儿);`project.scripts`/`file.read` 供代码页签。工具路径按 绝对 → exe 同目录 → 仓库根 → cwd 解析。前端:代码页签(文件下拉/源码/编译/运行/停止/输出面板,诊断可点跳行并闪一下)、**自写的 DexLang 词法高亮**(决策 #9,零第三方 JS)、顶栏 F3/Ctrl+B/F5。**测**:`tests/test_dexstudio.py` 201 → **251 项**(B5 25 项 + 前端资源 25 项,后者对每个 `.js` 跑 `node --check` —— 注释里 `*/` 那类语法错的守卫),页面自测 34 → **48 项**(含「点诊断真的切到代码模式」)。**零字节码格式改动** |
| (本次 B4) | **DexStudio B4:逻辑编辑器(节点图 → DexLang 代码)**。新增 `dexstudio/host/ds_graph.c`(~1200 行)与 `dexstudio/web/graph.js`(~600 行)。**图 = JSON DOM**(`scripts/logic.json`),节点目录在 C 里定义一次、`graph.types` 交给前端自省(与属性面板靠 `comp.schema` 同一套路);**22 种面向 dexgame 的节点**(事件/流程/动作/条件),不是 bluedit 那套 GAL 词汇 —— 复用它的架构(引脚分 exec/value、输出扇出、按 exec 链展开、界面自省),但在三处刻意更严:**输入口独占**(不靠连线顺序决定语义)、**禁环**(exec 与数据都查)、**失败必带原因**(bluedit 几乎全是静默 return)。命令 `graph.types/new/info/save/generate/node.add/node.remove/node.set/node.move/link/unlink`;生成的 `logic_start/logic_update/logic_draw` **都收 `dt: float`**,`on_action`/`on_key` 生成成 update 里的 `eng_action_pressed` 轮询,事件按 `(y,x,id)` 排序 → **同一份图永远生成同一份代码**。撤销快照推广成"场景 JSON + 不在场景里的文本文件(路径→内容)",逻辑图与瓦片 CSV 共用(取内存里的图,不是磁盘旧文件)。`project.new` 顺带写出空的 `logic.json`/`logic.dex`,模板 `main.dex` include "logic"。**测**:`tests/test_dexstudio.py` 152 → **201 项**(逻辑图模型 24 + 代码生成 13 + **行为 6**:造图→生成→dexc 编译→vm 跑 4 帧→断言 `x=20/y=7/vy=0`);页面自测 21 → **34 项**,且**不破坏用户已有的图**。**零字节码格式改动** |
| (本次 B3) | **DexStudio B3:可视化场景编辑器**。`dexstudio/web/` 从 B2 的“状态面板”变成真编辑器:`app.js`(桥/RPC/状态/顶栏/快捷键/自测)、`scene.js`(层级树/图层/自省生成的属性面板/瓦片调色板)、`viewport.js`(视口:平移缩放/网格吸附/框选/拖动移动/瓦片刷子)。**视口画面 = 引擎离屏渲染的一帧**(1024×640)→ BMP → 虚拟主机 `dexstudio-preview.local` → `<canvas>`,网格/选中框/瓦片格与画面同坐标系。**模型层新增**:`view.set`(编辑器视图覆盖,不动用户的相机实体)、`scene.render`(渲染落盘,返回 `seq` 破缓存)、`scene.outline`(每实体世界包围盒,一次调用给全)、`entity.duplicate/copy/paste`(字段级复制,跨场景剪贴板)、`comp.set_many`(批量写字段 = **一条**撤销记录)、`tilemap.create/info/set/paint/csv`(刷子;`paint` 一次画多格 = 一条撤销)。**撤销快照现在含各瓦片地图的 CSV 文本** —— 瓦片数据不在场景 JSON 里(`cols/rows/texture` 都是 `persist=0`),只快照场景会出现“撤销了但瓦片还在”。引擎侧顺带:新增 `eng_set_view`(视图覆盖,`dg_scene_active_camera` 优先返回它)、修 `dg_set_s` 把“清空 tex_path”误判为失败(`eng_*` 167 → **168**)。**测**:`tests/test_dexstudio.py` 91 → **152 项**(新增视口/瓦片刷子/复制粘贴三组),`--wv-selftest` 多跑一层**页面自测 21 项**。**零字节码格式改动** |
| (本次 docs) | **B2 完成文档化**:设计文档 §9.2 把 B2 标为 ✅ 并列证据;本文件 §1 加 DexStudio 行、§2 同步测试基线(30 脚本 / 1932 项)与构建命令、§4 新增 **6 条 B2 陷阱**(`chrome.webview` 全小写 / 注释里的 `*/` / `dsj_set` 所有权 / 组件名 1 基 / WebView2 异步就绪顺序 / 撤销后实体 id 变化)、新增 **§5.4 DexStudio 分层**、§6/§7 更新;README 补 DexStudio 一节 |
| (本次 B2) | **DexStudio 宿主骨架(产品 B 的可视化地基)**。新增 `dexstudio/`:`host/` 7 个 C 文件(~1900 行)+ `web/` 前端 3 个文件。**模型层在 C**(`libdexstudio.dll`):项目(project.json + scenes/ + scripts/ + res/ 固定约定,新建即落盘)/ 场景(直接复用引擎的 `eng_scene_json`、`eng_scene_load_json`,**不另造一套实体模型**)/ **撤销重做**(场景 JSON 快照栈,天然覆盖全部编辑动作)/ 命令通道 `ds_command(json)→json`(`app.info`、`project.*`、`scene.*`、`entity.*`、`comp.schema/add/remove/set`、`undo/redo`,失败一律带原因)。**宿主**:Win32 窗口 + 内嵌 **WebView2**(运行时 LoadLibrary + 虚拟主机映射;SDK 头与 x64 加载器 vendor 进 `third_party/`,共 3 MB)。**可验证性**:`--command`/`--selftest` 无窗口跑模型,`--wv-selftest` 把窗口放屏幕外等前端发 `ui.ready`,证明"窗口 → WebView2 → 本地页面 → JS↔C"整条链通;`tests/test_dexstudio.py` **91 项**(ctypes 调模型 + CLI + WebView2 链)。引擎顺带新增 `eng_object_id_at(index)`(IDE 场景树要按序号枚举实体,`eng_*` 166 → **167**)。**零字节码格式改动** |
| (本次 docs) | **产品 B 开工**:`docs/DEXGAME_DESIGN.md` §9 从占位改成**已锁定决策 + B1–B7 里程碑**(14 项决策逐条来自用户确认),§10 里程碑表补产品 B 行;本文件 §1/§2 接入 dexc、§4 新增 **6 条 B1 陷阱**(含"一行式改写清空 7 个源文件")、§5.1 补"双实现一致性"、§7 更新为产品 B 进行中;README 补 `build-dexc` |
| (本次 B1) | **纯 C 工具链 `dexc.exe`(产品 B 的地基)**。`tools/dexc/`(7 个 .c + 1 个 .h,~3700 行):词法/语法/编译/汇编/反汇编/汇编文本/.dexdef 解析全部从 `dexlang/` 移植到 C,`python main.py build-dexc` 构建(285 KB exe 入库)。**与 Python 前端逐字节一致**:新增 `tests/test_dexc.py` **478 项** —— 全仓库 34 个 .dex 的「字节码/汇编文本/警告/错误」四项对照、约 90 个语言特性用例、约 35 个错误路径、13 个 .dexdef 用例、反汇编↔汇编往返、16 个浮点边界。子命令:`compile` / `asm` / `disasm` / `run`(编译后交给 vm.exe)/ `dump-tokens` / `version`。**编译这一步从此不需要 Python** —— `dexc.exe` + `vm.exe` 就能完成"源码 → 可运行游戏" |
| (本次 docs) | **M4-e 完成 = 产品 A(M0.5–M4)收官**:设计文档里程碑表把 M4 标为完成并列证据、验收表 M4 行打勾;本文件 §1/§2/§7 更新为"引擎完成"、测试基线加示例编译守卫;顺带校正测试基线口径(`test_scene.py` 133→**135**、测试函数数按 `^def test_` 实测为 **199**、并说明「实测执行数 1353 > 静态 `check()` 调用数 1319」的原因) |
| (本次 M4-e) | **dexgame 完整可玩示例(平台跳跃)**。新增 `examples/dexgame/platformer.dex`(~380 行)与 `tests/fixtures/make_tiles.py`(+`tiles.png` 7 格图集)、`make_wav.py` 补 jump/coin 音效。示例用 `eng_run_frames` 的 **on_start/on_update/on_draw 回调**组织,把引擎各层串起来:D3D11 批渲染 + 相机跟随、动态玩家(重力/跳跃/土狼时间/松手跳得矮)、move-and-slide 与瓦片碰撞、**射线探坑**、**矩形查询**找金币/敌人、动作映射、**合成输入的自动演示**(不碰键鼠时自己玩,遇墙/坑/敌人/金币就跳)、文字 HUD、XAudio2 音效、`eng_find` + 组件字段当跨帧状态(语言没有全局变量)。实测:自动演示 **1050 帧 / 6.4 秒通关**(金币 1/3、0 死亡),`python main.py run examples/dexgame/platformer.dex` 可复现。新增 `tests/test_examples.py`(30 项)编译守卫 —— 钉住示例引用的接口没被改名 |
| (本次 docs) | M4-c/M4-d 完成:设计文档 §8 补上文字实现(DirectWrite 字形 → 覆盖率位图 → 图集 → 四边形;整数像素定位;两层缓存;一期不支持换行/对齐/子像素)与静态内嵌变体,里程碑表勾掉两项;本文件同步测试基线、新增 3 条 M4 陷阱、§1/§2/§7 更新 |
| (本次 M4-c/M4-d) | **dexgame M4-c 文字 + M4-d 静态变体**。新增 `dg_text.c`(~430 行):DirectWrite 工厂/字体面,`IDWriteGlyphRunAnalysis` 把**单个字形**光栅化成覆盖率位图(ClearType 三通道取最大值当 alpha),货架式打进 512×512 图集(`dg_tex_update_rgba` 局部上传),画字 = 白色四边形 + 顶点色 tint(**整数像素定位**);字体按(家族,字号)、字形按(字体,码点)**两层缓存**(度量不重排),UTF-8 与 **CJK** 都能显示;`eng_font_*`/`eng_text_*` 12 个函数(`eng_*` 154 → **166**)。**M4-d**:`test_static.py` 加 dexgame 覆盖 —— 400KB+ 的 DLL 内嵌进字节码,外部 DLL 改名后引擎仍能跑且离屏像素断言成立(16 → 24 项)。顺带修 `dg_audio_init` 在 offscreen 路径漏调的问题(见陷阱表) |
| (本次 docs) | M4-b 完成:设计文档 §8 补上音频实现(XAudio2 选型、声音资源/播放实例两层模型、WAV 支持范围、无声卡时的降级、audio 组件);本文件同步测试基线、新增 3 条 M4 陷阱、§1/§2/§7 更新 |
| (本次 M4-b) | **dexgame M4-b:音频**。新增 `dg_audio.c`(~430 行):XAudio2(运行时 LoadLibrary,COM 初始化)**声音资源按路径缓存** + **播放实例**(同一声音可同时多次播放)、手写 RIFF/WAVE 解析(8/16 位 PCM,单/双声道,拒绝非 PCM/24 位/多声道并给原因)、主音量/实例音量/停止/播放计数、`eng_audio_ok()` 设备状态(无声卡时播放**带原因失败**而不是崩)。新增 `audio` 组件(场景里能带声音,path 一设立刻解码,JSON 只存持久字段)。`eng_*` 132 → **154** 个函数。新增 `tests/test_audio.py`(81 项,含解析拒绝路径/播放状态/组件往返/双 VM)与 `tests/fixtures/make_wav.py`(+3 个 WAV 输入数据) |
| (本次 docs) | M4-a 完成:设计文档 §8 补上**已实现**的帧循环/回调/输入/动作映射(eng_run 是语言模块而非 C 导出、按键码统一编号、合成输入),§5 的风险表补"回调拿不到局部变量";本文件同步测试基线、新增 3 条 M4 陷阱、§1/§2/§7 更新 |
| (本次 M4-a) | **dexgame M4-a:输入 + 动作映射 + 主循环 + 实体名**。新增 `dg_input.c`(键盘/鼠标/XInput 手柄、统一按键码、**两层接口**:轮询 + 动作映射、边沿检测、`eng_input_feed*` 合成输入供无人值守测试)、`dexgame_fast.dex`(语言模块提供 `eng_run`/`eng_run_frames`/`eng_bind_default_actions`,零 VM 改动)、**实体名**(`eng_set_name`/`eng_name`/`eng_find`,随场景 JSON 存取 —— 因为语言没有全局变量,回调只能靠名字找状态实体);`eng_*` 105 → **132** 个函数;`eng_dt()` 改为帧间隔(与物理模式无关)。新增 `tests/test_input.py`(78 项,含双 VM)。修掉一个隐蔽 bug:边沿检测的 now→prev 拷贝放在帧开始会把 pressed 变成永远为假 |
| (本次 docs) | M3 完成:设计文档 §6 重写为**已实现的样子**(固定步长/子步上限/轴分离解算/宽相/插值/瓦片),新增 §6.4 本轮锁定的 7 个决定与 §6.5 已知简化,里程碑表与验收表同步;本文件同步测试基线、新增 8 条 M3 陷阱、§1/§2/§5.1/§7 更新 |
| (本次 M3) | **dexgame 引擎 M3:碰撞 / 查询 / 运动学 / 瓦片地图**。新增 `dg_phys.c`(~1000 行):AABB/圆/竖直胶囊的精确重叠、**均匀网格空间哈希**宽相(脏标记重建)、**游标式**范围查询(语言没有数组)、射线/扫掠 + **结果槽**(带 `ignore` 参数)、**固定步长 120Hz + 累加器**、轴分离 move-and-slide(子步上限 4px)、重力/速度/摩擦/弹性/休眠、**渲染插值**(alpha,默认开);`tilemap` 组件(CSV 加载/保存、`solid` 表、网格碰撞、视口裁剪批量渲染);`eng_*` 58 → **105** 个函数。新增 `tests/test_phys.py`(193 项,含确定性/像素/双 VM)与 `examples/dexgame/demo_m3.dex`。顺带修 `main.py run` 现在也接受 `.dex` |
| (本次 docs) | M2 完成:设计文档新增 §4.3 纹理采样规则、§5.4 M2 落地实况、§7.5 手写 JSON 的实现与代价,里程碑表同步;本文件同步测试基线、新增 6 条 M2 陷阱、§7 待办推进 |
| (本次 M2) | **dexgame 引擎 M2:实体与组件**。`dg_scene.c`(实体池 + 代际句柄 + 6 种内置组件 + 描述符表 + 层级世界坐标 + 场景 JSON 读写 + 从组件渲染)与 `dg_json.c/h`(手写流式 JSON 读写,含未知值跳过);`eng_*` 33 → **58** 个函数;新增 `tests/test_scene.py`(133 项,含像素断言/往返/向前兼容/双 VM)。**零字节码格式改动**(纯库改动) |
| (本次 docs) | M1 完成:SPEC 的 arity 上限 8→16 与"存储常量而非格式约束"说明;本文件同步测试基线、新增 5 条 M1 陷阱、§3.5/§5.1 更新、待办推进 |
| (本次 M1) | **dexgame 引擎 M1:D3D11 渲染核心**。新增 `libs/dexgame/`(dg_gfx/dg_draw/dg_api + dexvalue.h/dg_guids.h,~1500 行):flip 交换链、PER_MONITOR_AWARE_V2、2D 精灵批渲染、stb_image、代际句柄、`eng_last_error` 失败带原因、离屏渲染 + 像素回读/BMP 落盘(测试通道)。`MAX_NATIVE_ARGS` 8→16(eng_draw_uv 有 10 参)。新增 `tests/test_dexgame.py`(59 项)与 `examples/dexgame/demo_m1.dex`(实测 164.6fps@165Hz) |
| (本次 docs) | M0.5 完成:SPEC 4.5 重写为两套调用约定 + 新增 4.5.1–4.5.3;本文件同步测试基线、新增两条陷阱、§3.5 与 §5.1 更新、待办推进 |
| (本次 M0.5) | **原生调用「值数组 ABI」**:`.dexdef` 加 `abi value_array;` 文件级指令,原生侧收 `(const DexValue*, int)`,arity 上限 3→8;借 `ret_type` 的 bit7 标记,**零格式改动、零版号改动**;两个 VM 同步;顺带修 **常量池把 int N 与 float N.0 合并**的既有 bug。新增 `tests/test_abi.py`(36 项)与 `tests/fixtures/libabiarr.{c,dll,dexdef}` |
| (本次 docs) | 新增 `docs/DEXGAME_DESIGN.md`(dexgame 决策记录 / ABI 方案 / 里程碑);本文件接入该文档、更新 zig 缓存陷阱行与待办 |
| `e1c0a83` | `main.py build-vm` 在受限环境下失败:把 zig 缓存/临时目录收进工作区(`_compiler_env()`),并给出可操作的失败诊断。重编产物与已提交二进制**逐字节一致** |
| `41c1777` | `tests/_tmpdir.py` 的回退分支原先**恒不触发**:`tempfile.gettempdir()` 在所有候选都不可用时按 Python 既定行为退化成 `os.getcwd()`,于是临时目录被撒在仓库根而不是 `_tmptest/`。现显式排除 cwd |
| `6471b34` | 修正本文件与 README 的失实项:测试计数、34 MB 体积归因(示例媒体 28 MB 而非二进制)、galide/bluedit 重复规模、待办重排 |
| `4b6836a` | 删除停用的 `galide/`(与 `bluedit/` 重复 **104 个同名定义**)+ 其测试 |
| `7f50657` | `test_editor.py` 指向真实存在的 `galedit.exe` 并入库该二进制(0 → 18 项有效断言) |
| `2a48704` | 测试临时目录改用 `tests/_tmpdir.py`:修 `mkdtemp` 的 `0o700` ACL 导致 4 脚本失败;`test_bluedit` 导出失败改为带出 `last_error` |
| `0828d9f` | `.dexdef` 原生 arity 上限改为**编译期强制**(`MAX_NATIVE_ARITY`,SPEC 4.5 同步) |
| `b032a14` | IDE:括号配对高亮 + 查找替换(Ctrl+F/H/F3) + 代码折叠(边栏点击) |
| `802b04d` | IDE:着色按 token 种类完整归类(修 1 基列偏移)+ 语法感知缩进 + 自动配对/Tab/Ctrl+/ |
| Revert | **撤销 P4 字符串引用计数**(理由见 MEMORY_DESIGN 6.1) |
| `fec59c3` | (已撤销)P4 字符串引用计数 |
| `15eb0de` | P3 结构体值语义 + 拒绝递归类型(环在类型层面不可表达) |
| `6e2f9ec` | P0/P1/P2 统一堆入口 + 内存预算 + FFI 字符串所有权契约(`DXRL`) |
| `f56bff9` | 提交重建的二进制 |
| `19ab40a` | 文档:开发约定(编码规则、不要改换行) |
| `ce10e33` / `fc2e554` | 引入后撤销 `.gitattributes`(换行规范化 churn) |
| `b92bce3` | `build_libs.bat` 库构建脚本 + 编码约定 |
| `8911f96` | 初始导入(建立版本控制) |

### 更早的无版本控制改动(仅在文档/代码里留痕)

- **P0–P2 之前的修复**(都已进 P0 提交或有独立提交):加载期校验
  `nlocals>=arity`、函数 code 区间、指令对齐(恶意字节码防护);
  `INT64_MIN % -1` 陷阱;静态库临时文件改为原子唯一命名(消除可预测路径与 TOCTOU);
  `SetConsoleCP` 修 UTF-8 输入;对象比较保序;循环对象打印/比较的深度兜底;
  `pyvm` 释放临时文件;`bluedit` 补 `set_vm_path`;导出错误不再静默吞掉。
- 删除 `build/`、`build312/`、`DEXCODE-发布/`(共 484 MB 打包产物)。
- 删除 `password.txt`(内容是一条无关命令行)。

---

## 7. 待办 / 已知未做

**✅ 已完成:dexgame 游戏引擎(产品 A,设计见 `docs/DEXGAME_DESIGN.md`)** — 决策全部锁定并落地,
按 `M0.5 ABI 扩展 → M1 D3D11 渲染核心 → M2 场景与实体 → M3 物理 → M4 接口层` 全部走完;
下面是每个阶段的证据,以及下一步(产品 B:可视化 IDE)。

- ✅ **M0.5 已完成**:原生调用「值数组 ABI」落地(零格式/版号改动、两个 VM 同步)。
- ✅ **M1 已完成**(渲染核心):`libs/dexgame/` 可用 —— D3D11 flip 交换链、
  PER_MONITOR_AWARE_V2、2D 精灵批渲染、stb_image、代际句柄、失败带原因、
  离屏渲染+像素断言通道。`eng_*` 33 个函数(值数组 ABI)。
  实测示例 `examples/dexgame/demo_m1.dex` 跑 **164.6 fps @165Hz 垂直同步**
  (240 帧墙钟 1.82s,与 240/165=1.45s 吻合)—— 即**没有被锁死在 60Hz**。
- ✅ **M2 已完成**(实体与组件):实体池 + **代际句柄** + 6 种内置组件
  (transform/sprite/camera/animation/collider/body)+ 描述符表自省 +
  位置层级世界坐标 + **JSON 场景读写**(含"未知组件/字段被跳过"的向前兼容)+
  从组件渲染(相机、layer/order 稳定排序)。`eng_*` 共 **58** 个函数,
  `tests/test_scene.py` 135 项(像素断言 + 往返 + 双 VM 一致),**零字节码格式改动**。
  - 已知简化:层级只做**位置**继承(`eng_world_x/y`),旋转/缩放继承与缓存待有需求再做;
    相机约定是"(x,y) = 屏幕左上角的世界坐标"(见设计文档 §4.3/§5.4)。
  - 顺带修掉两条自己踩的坑:单纹素精灵的 uv 必须取纹素中心;场景加载失败不留半成品。
- ✅ **M3 已完成**(物理与瓦片):`dg_phys.c` 提供 L1 查询原语(AABB/圆/胶囊的
  重叠、point/rect/circle 查询、raycast、sweep)、L2 均匀网格空间哈希宽相、
  L3 固定步长(120Hz)+ 轴分离 **move-and-slide** + 重力/弹性/休眠 + 渲染插值,
  以及 L5 瓦片地图(CSV + 网格碰撞 + 视口裁剪渲染)。`eng_*` 共 **105** 个函数,
  `tests/test_phys.py` 193 项(固定步长下**逐帧可比**、像素断言、双 VM 一致),
  示例 `examples/dexgame/demo_m3.dex` 可跑(瓦片地图 + 巡逻跳跃 + 弹球 + 查询/射线)。
  - 已知简化:圆/胶囊作为**阻挡物**时轴向解算按包围盒近似(重叠判定仍精确);
    旋转不参与碰撞;`transform` 层级只继承位置;渲染插值让画面**晚一个物理步**
    (这是插值的固有代价)。详见设计文档 §6.5。
- ✅ **M4 已完成(接口层)**:
  - **主循环与回调**:`libs/dexgame/dexgame_fast.dex`(语言模块)提供
    `eng_run("on_start","on_update","on_draw")` / `eng_run_frames(...,N)`;
    帧生命周期仍在 C(`eng_frame_begin/end`),调度用 `call("名字")` —— **零 VM 改动**
    (与 egui_fast 的 eg_app_run 同一套路)。自动物理在 on_update 之前跑,
    所以那一帧设的速度下一步生效(1 帧输入延迟,165Hz 下约 6ms)。
  - **输入**:`dg_input.c` —— 键盘/鼠标/XInput 手柄,**统一按键码**(0..255 VK、
    256..258 鼠标、300..313 手柄、400+i/500+i 摇杆轴),**两层接口**(轮询
    `eng_key_*`/`eng_mouse_*`/`eng_pad_*` 与动作映射 `eng_action_*`,
    `eng_bind_default_actions()` 给一套 WASD+方向键+空格+鼠标+手柄的默认键位),
    边沿检测,以及 `eng_input_feed*` **合成输入**(无人值守测试与 IDE 脚本化试玩)。
  - **实体名**:`eng_set_name/eng_name/eng_find`,随场景 JSON 存取。原因是语言
    **没有全局变量** —— 回调函数看不到主程序的 `let`,跨帧状态只能放组件里靠名字找。
  - **音频(M4-b 已完成)**:`dg_audio.c` —— XAudio2(运行时 LoadLibrary + COM 初始化),
    **声音资源(按路径缓存)+ 播放实例(同一声音可同时多次响)**两层模型;
    手写 RIFF/WAVE 解析(8/16 位 PCM,单/双声道);`eng_sound_*`/`eng_voice_*`/`eng_audio_*`;
    **没有声卡时不崩**(`eng_audio_ok()` 暴露状态,播放带原因失败,测试据此 SKIP);
    `audio` 组件让场景能带声音(JSON 只存 path/volume/loop/play_on_start)。
  - **文字(M4-c 已完成)**:`dg_text.c` —— DirectWrite 字形经 `IDWriteGlyphRunAnalysis`
    光栅化成覆盖率位图,货架式打进 512×512 图集,画字 = 四边形 + tint(整数像素定位);
    `eng_font_load/line_height/ascent` + `eng_text_width/height/text`;字体与字形都缓存
    (度量不重排文字),UTF-8 与**中文**都显示正常;一期不做换行/对齐/富文本/子像素定位。
  - **静态内嵌变体(M4-d 已完成)**:`include "dexgame_static"` 把整个引擎 DLL 内嵌进
    `.dexbc`,外部 DLL 移走也能跑(`test_static.py` 24 项,含 dexgame 离屏像素断言)。
  - **完整可玩示例(M4-e 已完成)**:`examples/dexgame/platformer.dex` —— 平台跳跃,
    用 `eng_run_frames` 的回调组织,串起渲染/相机/物理/瓦片/射线/查询/输入/音频/文字;
    不碰键鼠时**自动演示**(合成输入走同一套代码路径),实测 1050 帧通关。
    `tests/test_examples.py` 钉住示例仍能编译(示例最容易悄悄烂)。
- ✅ **产品 A(dexgame 引擎)到此完成**:M0.5 ABI → M1 渲染 → M2 场景 → M3 物理 →
  M4 接口层全部落地,`eng_*` 169 个函数(含 B9 为 IDE 加的 `eng_set_asset_dir`),全量测试通过(见 §2)。
  引擎可只靠 `libdexgame.dll` + `vm.exe` 独立发布(或用 `dexgame_static` 把 DLL
  内嵌进字节码,发布产物不需要单独的 DLL 文件)。
- 🟢 **产品 B(DexStudio 可视化 IDE)进行中** —— 设计见 `docs/DEXGAME_DESIGN.md` §9。
  - ✅ **B1 已完成(纯 C 工具链 dexc.exe)**:见上方「纯 C 工具链」一行与变更记录。
    验收证据:478 项双实现对照全过;`dexc run examples/fib.dex` 直接出结果。
  - ✅ **B2 已完成(WebView2 宿主骨架 + 可测模型层)**:见上方「可视化 IDE」一行。
    验收证据:`tests/test_dexstudio.py` 91 项全过(ctypes 调模型 + 无窗口 CLI +
    **窗口放屏幕外的 WebView2 整条链自测** `--wv-selftest`);`--selftest` 14 项全过。
  - 已锁定决策(14 项,用户逐条确认):① 编译链移植成 C 的 `dexc.exe`;② C 宿主 +
    **内嵌 WebView2**;③ WebView2 SDK 下载并 vendor 入库;④ 逻辑编辑用 **UE 蓝图式节点图**
    (复用 `bluedit/` 模型);⑤ **完整可视化场景编辑器**(视口/层级树/属性面板/图层);
    ⑥ 要**瓦片地图刷子**;⑦ 一键编译 + 独立窗口运行 + 输出面板 + 错误定位
    (**不做** PIE 与断点);⑧ 编辑器基础功能全要(撤销重做/复制粘贴/框选多选/网格吸附/
    中文界面/自动保存);⑨ 自写轻量代码高亮页签(**不引第三方 JS**);⑩ 模型层在 **C 宿主**
    并导出可测接口(顺带得到无界面命令行构建);⑪ 固定项目结构
    (`project.json` + `scenes/` + `scripts/` + `res/`)且支持新建/打开项目;
    ⑫ 目录 **`dexstudio/`**;⑬ **地基先行**(先 dexc,再宿主,再场景,再逻辑);
    ⑭ 全权自主:自动提交 + 推送 origin,细节按"最简可行 + 与现有风格一致"自行决定并记入文档。
  - ✅ **B3 已完成(可视化场景编辑器)**:视口(引擎离屏渲染 → 虚拟主机 → canvas,
    平移/缩放/网格/吸附/框选/拖动移动)、层级树(过滤/双击居中)、自省生成的属性面板、
    图层分组、瓦片刷子(图集调色板/画笔/橡皮/整图清空/越界自动扩)、复制粘贴与再做一个、
    快照式撤销(含瓦片 CSV)、批量字段写一条撤销、快捷键。验收证据:
    `python tests/test_dexstudio.py` **152 项**全过;`dexstudio.exe --wv-selftest` 报
    **界面自测 21 项通过、0 项失败**(渲染图解码尺寸/画布像素/世界↔屏幕换算/层级树行数/
    属性面板字段/选中框/拖动写回/瓦片落 CSV 与一次撤销),不再需要人看屏幕。
    细节与已知简化见 `docs/DEXGAME_DESIGN.md` §9.3。
  - ✅ **B4 已完成(逻辑编辑器)**:UE 蓝图式节点图 → DexLang 源码。验收证据:
    `tests/test_dexstudio.py` **201 项**全过,其中行为那 6 项把"图 → 代码 → dexc 编译
    → vm 跑 4 帧"整条链跑通并断言了数值;`--wv-selftest` 的页面自测 **34 项**全过。
    细节与已知简化见 `docs/DEXGAME_DESIGN.md` §9.4。
  - ✅ **B5 已完成(编译/运行/输出面板/代码页签)**:一键 = 存盘 + 重生成逻辑图 + `dexc.exe`;
    诊断带行列号、输出面板点一下跳行;「运行」用无控制台 VM 开**独立游戏窗口**并可「停止」;
    代码页签用自写的词法高亮。验收证据:`tests/test_dexstudio.py` **251 项**全过(含前端资源
    的 `node --check` 守卫),页面自测 **48 项**全过。细节见 `docs/DEXGAME_DESIGN.md` §9.5。
  - ✅ **B6 已完成(资源管理 / 自动保存 / 崩溃恢复)**:`res.*`(导入=复制、同名不覆盖、
    名字不许带路径)+ 缩略图/试听走虚拟主机;自动保存是**整包**(场景 + 瓦片 CSV + 逻辑图),
    恢复判据用"自动保存比场景新"。验收证据:`tests/test_dexstudio.py` **279 项**全过,
    页面自测 **58 项**全过。细节与已知简化见 `docs/DEXGAME_DESIGN.md` §9.6。
  - ✅ **B7 已完成(打包)**:前端资源内嵌进 exe + `package-dexstudio`;
    发布形态 = `dexstudio.exe` + `WebView2Loader.dll` + `libdexgame.dll` + `README.txt`。
    验收证据:`test_packaged_exe()` 在**没有 `web/`** 的干净目录里跑通 `--selftest`
    与 `--wv-selftest`(**页面自测全过**)。细节见 `docs/DEXGAME_DESIGN.md` §9.7。
  - ✅ **B8 已完成(中文/编码修复 + 恢复提示语义)**:用户在真机上开 IDE 报的三个症状
    —— 窗口标题乱码 / 一直提示"上次没有正常退出"且按钮没用 / 导入资源说路径下没有那个资源
    且名字乱码 —— 全部定位并修掉:① `CreateWindowExA` 的标题 → `SetWindowTextW`
    (并在 `--wv-selftest` 里 `GetWindowTextW` 回读断言);② 自动保存包记 `session`,
    只提示**上一次运行**留下的(不会每 30 秒又冒出来),正常退出盖 `clean=1` 以区分
    "崩溃"与"正常退出但没存盘";③ 根因是 `ds_json.c` 把 UTF-8 的**字节**当码点又编了一遍
    (二次编码)+ Windows 窄字符 API 按 ANSI 解路径 → 新增三层 UTF-8 路径层
    (`ds_utf8.c`/`dg_utf8.c`/`dx_utf8.c`),`CopyFileA`/`FindFirstFileA`/`CreateProcessA`/`LoadLibraryA`/`GetOpenFileNameA`
    之类全部换成宽字符版本,`dexc` 也改成 UTF-8 原生(仍与 Python 前端逐字节一致)。
    顺带修:`scene.new` 没记 `start_scene`(重开项目回空场景)、`project.new` 不能建多级目录。
    验收证据:`tests/test_dexstudio.py` **323 项**全过(新增 `test_utf8_paths` /
    `test_build_in_chinese_path` / `test_recover_session`);`--selftest` **22 项**;
    发布形态测试把三个文件拷进**中文目录**再跑,标题也断言。
  - ✅ **B9 已完成(资源根 + 拉线预览 + 资源面板)**:用户报「流程图拉线不跟鼠标」「图片能导入但显示裂图标、设不上贴图」两件事 —— 前者是 `mousemove` 的 guard 看错了状态(`drag` vs `pending`),后者是**相对资源路径按引擎自己的工作目录解析**(引擎新增 `eng_set_asset_dir` + `dg_fopen_asset`,IDE 打开项目时设成项目根,场景里继续存可搬的相对路径);顺带让 `applyRes` 不再拿本地缓存当存在性依据。验收证据:`tests/test_dexstudio.py` **329 项**、页面自测 **68 项**(含「预览线跟着鼠标走」与「点缩略图真的设上贴图且 texture>=0」)。细节见 `docs/DEXGAME_DESIGN.md` §9.9。
  - ✅ **B10 已完成(体验/逻辑大修)**:用户报"用起来十分别扭"之后逐条查证并修复,清单/验收/未做项见 `docs/DEXSTUDIO_UX_ISSUES.md`。验收证据:`tests/test_dexstudio.py` **397 项**、`--selftest` **51 项**、页面自测 **102 项**,全量 30 脚本 0 失败。
  - ✅ **B11 已完成(Scratch 式积木模式 + 四个"看不到效果")**:用户报「预览图裂图标 / 改贴图只有框线动、图像不显示 / 声音试听没反应 / 这代码块设计根本没法让零基础用户上手」。做成三档前端:**积木**(句子积木 + 全下拉,不连线不打字,点一下就加,`如果…就` 有肚子;16 种积木;`scripts/blocks.json`)/ 节点图(进阶)/ 代码。同时挖出并修掉四个**真**问题:① IDE 生成的 `main.dex` 从不 `eng_init`,而 `eng_run` 的循环条件是 `while eng_running()` → **游戏 60ms 就退出、什么都不显示**(实测);② 组件池没初始化时场景加载把每个组件当成"不认识的组件"静默跳过 → **有实体没组件**;③ 宿主 `scene.outline` 的框线公式与引擎画图**不是同一个**(差一个 `源尺寸 × 轴心 × 缩放`)→ 这就是"只有框线在动、图像在别处";④ 旧模板留下的 `sprite.sw/sh = 32` 让 300×400 的图只画左上角一块。验收证据:`tests/test_dexstudio.py` **479 项**(含"未初始化也能装组件""模板游戏 2 秒后还活着""框线与像素包围盒对齐""带项目跑页面自测:缩略图真的解码 / 声音真的能播"),`--selftest` **51 项**,页面自测 **95 / 120 项**,全量 30 脚本 0 失败。页面自测 95/120 的差别就是这一轮新开的资源/试听/换贴图那 25 项。细节见 `docs/DEXGAME_DESIGN.md` §9.10 与 `docs/PITFALLS.md` 的 B11 段。
  - ✅ **产品 B(B1–B7)全部完成** —— 14 项决策逐条落地:纯 C 工具链 / WebView2 宿主 /
    蓝图式逻辑图 / 完整场景编辑器 + 瓦片刷子 / 一键编译 + 独立窗口运行 + 错误定位 /
    编辑器基础功能(撤销重做、复制粘贴、框选、网格吸附、中文界面、自动保存与崩溃恢复)/
    自写代码高亮 / 模型层在 C 并可测 / 固定项目结构 / `dexstudio/` / 地基先行 / 自动提交推送。
    全量测试见 §2(其中 DexStudio **397 项**;B10 之后页面自测 102 项)。
- 开工前已验证的前提:① `zig cc` 能编译并链接 D3D11(本机硬件设备 S_OK、特性级别 11_1、
  RTX 4060;WARP 兜底也可用);② `python main.py build-vm` 可重编且产物与已提交二进制
  **逐字节一致**(所以改 vm.c 的二进制 diff 只在真改动时出现)。
- 两个已实测并已落实的约束:显示器 **2560×1600 @165Hz**(已用 flip + Present(1,0) 控帧,
  **没有**用 Sleep(16))、系统 **150% DPI 缩放**(已设 PER_MONITOR_AWARE_V2)。

以下是既有项目的零散待办,按价值排序:

1. **示例媒体占仓库体积的 80%**:`examples/**/res/` 28 MB(单个 mp3 4.5 MB、单张
   jpg 3 MB)。若要瘦身,方向是压缩/换格式/移出仓库(用下载脚本或 LFS),而不是动二进制。
2. **`bluedit/settings.py` 仍把配置写到 `~/.galide.json`**:`galide/` 已删除,这个文件名
   成了遗留物。直接改名会静默丢用户设置,故**未改**;如要改名需带迁移(旧文件存在且
   新文件不存在时沿用旧文件)。
3. **环境相关的散件**(已 gitignore,未删除):根目录 20+ 个 `_*` 调试脚本与
   `_sm2.pdb`/`_smoke.pdb`(各 1.1 MB)、`nuitka-crash-report.xml`(0.6 MB)、
   `_zigcache/`(103 MB)、`.venv/`(353 MB)。另外 `我的项目/`、`我的界面设计/`、
   `完整导出/`、`mycode/` 是用户数据,归属待确认。
4. **IDE 未实现**:多光标、`Ctrl+D` 选同词、正则/全词查找、查找历史、
   自动换行、代码折叠跨多行注释。
5. **`vm.c` 的 arity 兜底可以更早**:手写/篡改的 `.dexbc` 目前要到**调用期**才报
   `unsupported native arity`;加载期只拒绝 `arity > 8`。改成加载期直接拒绝 `> 3`
   能让不可调用的签名更早暴露。**未做**,因为要重编 `vm.exe`/`vmnc.exe`/`galrun.exe`
   并重新入库三个二进制(改动收益小、二进制 diff 噪声大)。
6. **文档漂移风险**:`README`/本文件里的测试项数会随测试增加而过期。
   本文件已改为只维护「本机实测总数」,逐文件数字以实际运行为准。

### 明确不建议做(除非条件变化)

- **不要重新引入 P4/P5(引用计数或 GC)**:除非出现真实的字符串累积需求。
  详细理由见 `docs/MEMORY_DESIGN.md` 6.1。
- **不要给 `.dexdef` 加"必须释放"的约定**:现有 6 个库都返回静态缓冲区,
  默认契约(借用 + VM 复制)已经覆盖;`(string)->void` 是给需要的库的可选升级。
