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
| **游戏引擎** | `libs/dexgame/`(~1500 行,3 个 .c) | 模块化 2D 引擎:**D3D11 批渲染**,见 `docs/DEXGAME_DESIGN.md`。当前 M1(渲染核心) |
| 集成开发环境 | `dexide/`(8 文件 ~4120 行) | tkinter,零第三方依赖 |
| GAL 蓝图编辑器 | `bluedit/`(15 文件 ~7180 行) | UE 风格节点连线 → 生成 DexLang 代码 |
| 测试 | `tests/`(22 个测试文件 + `_tmpdir.py`,~6900 行) | 全部是**手写 check() 脚本**,不用 pytest |

**零第三方 Python 依赖**是刻意设计(只用标准库)。唯一可选外部依赖是
`ziglang`(pip 包,提供 C 编译器)。

> 已删除:`galide/`(Python 旧版 GAL 编辑器,Scratch 式指令块)。它与 `bluedit/`
> 有 **104 个同名定义**(`engine.py` 近乎整文件级重复),且两者曾共用同一个
> `~/.galide.json`。详见「变更记录」。旧 `.galscene` 工作流现由
> `tools/legacy_galedit_c/`(C 版)与 `bluedit/` 承接。

---

## 2. 当前状态(请以本节为权威)

- 分支 `main`,工作树干净。
- **测试:782 项通过 / 0 失败** —— 22 个测试脚本**全部退出码 0**,无 skip(见下方基线)。
- 跟踪 196 个文件、约 34 MB。**体积构成容易被误判**:`examples/**/res/` 的示例媒体
  占 **28 MB**(单张 jpg 2–3 MB,单个 mp3 3–4.5 MB),而「刻意入库以便 clone 即用」的
  二进制(`vm/*.exe` + `libs/*/lib*.dll` + `tools/legacy_galedit_c/galedit.exe`)只有
  **2.4 MB**。要让仓库瘦身,该动的是示例媒体,不是二进制。
- 版本控制**刚建立**(2026-09),此前项目无 git;历史提交从「初始导入」开始。
- 远端 `origin` = `git@github.com:114514name/DEXCODE.git`。**本机到 `github.com:22`
  的连接会被对端直接关闭**(TCP 其实通,但握手被切断),必须走 GitHub 官方备用通道
  `ssh.github.com:443` —— 已写入 `~/.ssh/config` 的 `Host github.com` 块。见 §4。

### 测试基线(改动后请对照)

```bash
# 逐个跑(每个都是独立脚本,退出码 0/1);也可直接跑 python _status_run2.py 之类的批处理
python tests/test_toolchain.py   # 29  核心工具链 + C VM + 往返
python tests/test_native.py      # 41  include/refer + .dexdef + arity 上限 + DLL 调用
python tests/test_static.py      # 16  静态链接
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
```

合计 **153 个测试函数 / 763 处 `check()` 调用**;实际执行数随平台与是否构建
`vm.exe` 而变,本机实测 **782 项通过**。本文件不逐条维护各项数字,以实际运行为准。

> `tests/_tmpdir.py` 不是测试文件,是测试共用的「可写临时目录」工具。**新增需要临时
> 目录的测试请用它,不要用 `tempfile.mkdtemp`** —— 原因见 §4「mkdtemp 的 0o700 ACL」。

### 构建

```bash
python main.py build-vm     # 构建 vm.exe / vmnc.exe / galrun.exe
build_libs.bat              # 构建 7 个原生库 DLL(需要 ziglang)
```

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
「仅属主」ACL,受限沙箱下当前进程写不进也删不掉该目录(详见 §4)。测试统一改用
`tests/_tmpdir.py` 的 `mktempdir()` / `tempdir()`:同样的接口,但目录以 `0o777` 创建,
且优先放在系统临时目录(正常机器行为与 `tempfile` 等价,只在系统临时目录不可用时
回退到仓库内 `_tmptest/`)。

### 3.5 其他

- **提交前**:工作树干净 + 全量测试通过。
- **生成产物不入库**:`*.dxasm`、`*.dexbc`、`*.do`、`scene_out.dex` 等
  (可由 `.dex` 重建),见 `.gitignore` 注释。
- 二进制(`vm/*.exe`、`libs/*/lib*.dll`、`tools/legacy_galedit_c/galedit.exe`)
  刻意入库:仓库缺少部分构建脚本时,不入库会导致 clone 后无法运行示例与测试。
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

| 陷阱 | 现象 | 应对 |
|------|------|------|
| zig 缓存复用失败的链接 | 修好编译参数后仍报旧的 `undefined symbol` | 构建前清空 `_zigcache/`;`build_libs.bat` 已内置 |
| sandbox/受限环境写 zig 缓存被拒 | `failed to create output directory ...AccessDenied` —— 报错**像编译器坏了或源码有问题**,其实只是 zig 的默认缓存(`%LOCALAPPDATA%\zig\tmp`)与 `%TEMP%` 在工作区外、写不进去 | `build_libs.bat` 与 `main.py build-vm`(内部 `_compiler_env()`)都已把 `ZIG_GLOBAL_CACHE_DIR`/`TMP` 指向工作区内的 `_zigcache/` 与 `_zigtmp/`;只有**手写** zig 命令时才需自己设。注意 `main.py` 直到 `e1c0a83` 才补上,此前同一环境下「库能构建、VM 不能构建」 |
| 测试临时文件重名 | 4 个测试文件曾共用 `_tmp_test.dexbc`,互相覆盖导致 VM 读到截断文件而随机崩溃 | 各测试文件使用带唯一后缀的路径(已修) |
| `str.endswith("([{")` | 参数是**后缀串**不是字符集合,该判断恒为假(曾让"行尾 `{` 自动缩进"从未生效) | 用「末字符 ∈ 集合」判断 |
| `tag_configure` 放在 `tag_add` 之后 | 配置不作用于已打区间(折叠的 `elide` 曾完全失效) | 先 `tag_configure` 再 `tag_add` |
| Tk 全局 mark 多用途复用 | 多个折叠块共用一个 mark,后者覆盖前者,导致展开时删不掉提示 | 按行使用唯一 tag 定位 |
| `Text.get("1.0","end-1c")` 与 `elide` | 折叠后仍返回全文(这是 Tk 设计,便于保存) | 判断是否隐藏要看 `tag_ranges`/`dlineinfo`,不是 `get` |
| **`tempfile.mkdtemp` 的 `0o700` ACL**(最坑的一条) | 受限沙箱(只放行工作区写入)下,`mkdtemp`/`TemporaryDirectory` 建出的目录**内部既不能建子目录也不能写文件**,连 `shutil.rmtree` 都被拒;最外层只看到 `PermissionError: [WinError 5] 拒绝访问` / `[Errno 13] Permission denied`,看不出病因。曾让 `test_bluedit`/`test_designer`/`test_module`/`test_vm_versions` 四个脚本 rc=1 | `mkdtemp` 内部是 `os.mkdir(p, 0o700)`。实测同一父目录下:`0o700` FAIL / `0o777` OK / 默认 OK。测试统一走 `tests/_tmpdir.py`(以 `0o777` 建目录)。见 §3.4 |
| 库函数失败只返回空值 | `bluedit.export.export_project` 失败时返回 `""`,裸调用后 `open(path)` 抛 `FileNotFoundError: ''`,把真实原因(「无法创建输出目录 …」)彻底吞掉 —— 排查时完全不知从何下手 | 测试改用 `_export()` 包装,失败时抛出 `export.last_error` 的内容。**写同类 API 时:失败要带得出原因,不要只返回空值**(库侧早已把原因记在 `last_error`,是调用方丢了它) |
| `tests/test_editor.py` 驱动的是哪份 exe | 该测试曾指向**不存在**的 `tools/galedit.exe`(实际在 `tools/legacy_galedit_c/`),两个用例永远 skip,`check(..., True)` 是死代码 | 已修:路径更正 + `.gitignore` 放行该 exe + exe 入库(否则 clone 后仍缺文件)。现为 18 项有效断言 |
| **`github.com:22` 被对端切断** | `git push` 报 `Connection closed by <ip> port 22`。注意这**不是** `Permission denied (publickey)` —— 用 `Test-NetConnection` 测 TCP 是通的,极容易被误判成「缺密钥/仓库不存在」而白折腾 | 走 GitHub 官方备用通道:在 `~/.ssh/config` 写 `Host github.com` / `HostName ssh.github.com` / `Port 443` / `User git`。验证:`ssh -T git@github.com` 应回 `Hi <用户名>! You've successfully authenticated` |
| 受限沙箱阻断 Git 出站 | 沙箱(仅工作区写)下 `git ls-remote`/`push` 报 `sh.exe: couldn't create signal pipe, Win32 error 5`,HTTPS 则报 `schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS` —— 看起来像 Git 坏了或没凭据,其实只是沙箱不放行出站 | 这两条都是**沙箱**造成,不是 Git 或凭据问题;提权后同一命令即恢复正常。诊断时先用 `git ls-remote <url>` 判定「通道是否通」「仓库是否存在」 |
| **常量池把 int `N` 与 float `N.0` 合并**(M0.5 时发现并修复) | `assembler.cidx()` 原先**只按值**当键,踩中 Python 的 `0 == 0.0` 且 `hash(0) == hash(0.0)`,于是 `print 0;` 与 `print 0.0;` 共用同一条常量,后出现的那一个拿到**错误的类型标签**。潜伏很久没暴露:直接 ABI 会按声明的参数类型做 int/float 互转,VM 的算术也对两者很宽容 | 键改成 `("int"|"float"|"str", value)`。值数组 ABI 让**类型标签变得可观测**,这个 bug 才浮出来 —— 是新 ABI 的**前置修复**。回归测试:`test_abi.py::test_const_pool_types` 与 `abi_tags` 断言 |
| 新 ABI 只在「类型标签可观测」时才暴露老 bug | 双 VM 一致性断言把上面那条抓了出来(`arr_greet("", 0, 0.0)` 的 `0.0` 在 C VM 成了 int) | 加 ABI/内存布局类改动时,**必须**同时加双 VM 逐行对比断言,否则 pyvm 与 C VM 会静默分叉 |
| **`-ld3dcompiler` 链不上**(M1) | zig 只提供 d3d11/dxgi/dwrite 等少数 Windows 导入库,`d3dcompiler` **没有**:`error: unable to find dynamic system library 'd3dcompiler'` | 运行时 `LoadLibraryA("d3dcompiler_47.dll")` + `GetProcAddress("D3DCompile")`(系统自带该 DLL)。反而更自包含 —— 不依赖任何导入库。见 `dg_draw.c` 的 `dg_d3dcompile()` |
| **`IID_xxx` 未定义**(M1) | 只 include 头文件时 `lld-link: error: undefined symbol: IID_IDWriteFactory` —— 因为 GUID 符号本应由导入库提供,而 zig 不提供 dwrite/dxgi 的导入库 | 在 `libs/dexgame/dg_guids.h` 里**显式写死**用到的那几个 GUID(比 `#include <initguid.h>` 更可预测,只需付出实际用到的) |
| **库往 stdout 打印**(M1) | 引擎初始化时 `printf` 一行 GPU 信息,结果把游戏的输出污染了 —— `test_dexgame.py` 的按行断言全部错位(5 项失败),而 DLL 本身完全正常 | **库不写 stdout**。改成按需查询(`eng_gpu_name()`/`eng_vram_mb()`)。写任何库时都该这样:输出是调用方的地盘 |
| **图集边缘渗漏**(M1) | 单纹素源用 `u0..u1` 覆盖整个纹素时,线性过滤会掺进邻居(实测纯绿变成 `0xff04ff04`) | 半纹素内缩:单纹素源直接用**纹素中心**(退化区间 `u0==u1`),即 `u=(x+0.5)/W`。`test_dexgame.py::test_atlas_png` 锁住这个约定 —— 将来做图集助手时要在库里自动完成内缩 |
| 引擎宿主进程的 DPI 只能设一次 | `SetProcessDpiAwarenessContext` 必须在**创建任何窗口之前**调用,且清单设过就再设会失败 | `dg_gfx_init_window` 里尽早调用并**忽略失败**(E_ACCESSDENIED 不算错);客户区尺寸要用 `AdjustWindowRectExForDpi` 算,否则 150% 缩放下会偏小 |

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
- **推论:M1 把 `MAX_NATIVE_ARGS` 从 8 提到 16 也没动格式**。它只是 `param_types`
  数组的容量。这条可作为以后判断"某个改动要不要动格式"的判据:**看字节码里有没有
  承载它的字段**(arity 是 u8、表条目本就是变长的 `6+arity`),有就不用动。

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

**进行中:dexgame 游戏引擎(设计见 `docs/DEXGAME_DESIGN.md`)** — 决策已全部锁定,
按 `M0.5 ABI 扩展 → M1 D3D11 渲染核心 → M2 场景与实体 → M3 物理 → M4 接口层` 推进;
可视化 IDE(产品 B)在引擎可独立发布之后再动。

- ✅ **M0.5 已完成**:原生调用「值数组 ABI」落地(零格式/版号改动、两个 VM 同步)。
- ✅ **M1 已完成**(渲染核心):`libs/dexgame/` 可用 —— D3D11 flip 交换链、
  PER_MONITOR_AWARE_V2、2D 精灵批渲染、stb_image、代际句柄、失败带原因、
  离屏渲染+像素断言通道。`eng_*` 共 33 个函数(值数组 ABI)。
  实测示例 `examples/dexgame/demo_m1.dex` 跑 **164.6 fps @165Hz 垂直同步**
  (240 帧墙钟 1.82s,与 240/165=1.45s 吻合)—— 即**没有被锁死在 60Hz**。
  **下一刀是 M2**:实体池 + Unity 式组件挂载 + 变换层级 + 描述符驱动的 JSON 场景读写;
  文字(DirectWrite + 字形图集)与相机/层级排序也归在 M1b,可与 M2 并行。
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
