# AGENTS.md — 项目状态与协作须知

> 本文件是**给 AI 协作者(以及人类)的项目交接说明**:当前状态、必须遵守的约定、
> 已知陷阱、待办。改动项目后请顺手更新本文件的「变更记录」与「当前状态」两节。
>
> 其他文档的分工(避免重复,请交叉引用而不是复制):
> - `README.md` — 面向使用者:怎么装、怎么跑、有哪些库与示例
> - `docs/SPEC.md` — 语言与字节码的**权威规格**(改格式必须同步这里)
> - `docs/MEMORY_DESIGN.md` — 内存模型的方案与**已实施/已撤销**结论
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
| 语言前端 + 汇编器 | `dexlang/`(14 文件 ~2900 行) | 词法/语法/编译/汇编/反汇编/`.dexdef` 解析 |
| 调试用 Python VM | `dexlang/pyvm.py` | 与 C VM 语义一致的参照实现,IDE 断点/单步靠它 |
| C 字节码解释器 | `vm/vm.c`(单文件 ~1460 行) | 含原生 FFI(P0–P3 的内存模型改动都在这里) |
| 原生库(6 个) | `libs/*/` | 纯 C 实现:`std` `math` `img` `ui` `egui` `gal` |
| 集成开发环境 | `dexide/`(8 文件 ~3700 行) | tkinter,零第三方依赖 |
| GAL 蓝图编辑器 | `bluedit/`(~6400 行) | UE 风格节点连线 → 生成 DexLang 代码 |
| 旧版 GAL 编辑器 | `galide/`(~2500 行) | **已停用**,仅为兼容保留 |
| 测试 | `tests/`(21 文件 ~5500 行) | 全部是**手写 check() 脚本**,不用 pytest |

**零第三方 Python 依赖**是刻意设计(只用标准库)。唯一可选外部依赖是
`ziglang`(pip 包,提供 C 编译器)。

---

## 2. 当前状态(请以本节为权威)

- 分支 `main`,工作树干净。
- **测试:483 项通过 / 0 失败**(见下方基线)。1 项已知失败是环境问题,见「已知陷阱」。
- 跟踪 205 个文件、约 34 MB(含 `vm/*.exe` 与 `libs/*/lib*.dll`,刻意入库以便 clone 即用)。
- 版本控制**刚建立**(2026-09),此前项目无 git;历史提交从「初始导入」开始。

### 测试基线(改动后请对照)

```bash
# 逐个跑(每个都是独立脚本,退出码 0/1)
python tests/test_toolchain.py   # 29  核心工具链 + C VM + 往返
python tests/test_native.py      # 32  include/refer + DLL 调用
python tests/test_static.py      # 16  静态链接
python tests/test_stdlib.py      # 36  标准库
python tests/test_struct.py      # 18  type/结构体 + 双 VM 一致性
python tests/test_call.py        # 12  call() 动态调用
python tests/test_module.py      # 10  .dex 语言模块
python tests/test_img.py         # 13  图片渲染库
python tests/test_ui.py          # 27  WinAPI UI 库
python tests/test_egui.py        # 12  EGUI 库
python tests/test_gal.py         # 30  GAL 引擎(含 BMP 像素断言)
python tests/test_galide_py.py   # 46  旧版编辑器模型
python tests/test_pyvm.py        # 14  Python 调试 VM
python tests/test_ide.py         # 123 DEXIDE 分析器 + 编辑器
python tests/test_designer.py    # 16  EGUI Designer
python tests/test_bluedit.py     # 185 GAL 蓝图模型/导出
python tests/test_nopydep.py     # 7   脱离 Python 运行
python tests/test_vm_versions.py # 13  三个 VM 变体 + release 打包
python tests/test_robust.py      # 25  健壮性(畸形字节码/循环对象/整型边界)
python tests/test_memmodel.py    # 44  内存模型(P0–P3 + FFI 契约)
python tests/test_editor.py      # 0/2 永远 skip(见「已知陷阱」)
```

合计 **701 处 `check()` 调用**;实际执行数随平台与是否构建 `vm.exe` 而变,
本机实测 **483 项通过**。本文件不逐条维护各项数字,以实际运行为准。

### 构建

```bash
python main.py build-vm     # 构建 vm.exe / vmnc.exe / galrun.exe
build_libs.bat              # 构建 6 个原生库 DLL(需要 ziglang)
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

### 3.4 其他

- **提交前**:工作树干净 + 全量测试通过。
- **生成产物不入库**:`*.dxasm`、`*.dexbc`、`*.do`、`scene_out.dex` 等
  (可由 `.dex` 重建),见 `.gitignore` 注释。
- 二进制(`vm/*.exe`、`libs/*/lib*.dll`)刻意入库:仓库缺少部分构建脚本时,
  不入库会导致 clone 后无法运行示例与测试。

---

## 4. 已知陷阱(踩过并记录)

| 陷阱 | 现象 | 应对 |
|------|------|------|
| zig 缓存复用失败的链接 | 修好编译参数后仍报旧的 `undefined symbol` | 构建前清空 `_zigcache/`;`build_libs.bat` 已内置 |
| sandbox/受限环境写 zig 缓存被拒 | `failed to create output directory ...AccessDenied` | 把 `ZIG_GLOBAL_CACHE_DIR`/`TMP` 指向工作区内 |
| 测试临时文件重名 | 4 个测试文件曾共用 `_tmp_test.dexbc`,互相覆盖导致 VM 读到截断文件而随机崩溃 | 各测试文件使用带唯一后缀的路径(已修) |
| `str.endswith("([{")` | 参数是**后缀串**不是字符集合,该判断恒为假(曾让"行尾 `{` 自动缩进"从未生效) | 用「末字符 ∈ 集合」判断 |
| `tag_configure` 放在 `tag_add` 之后 | 配置不作用于已打区间(折叠的 `elide` 曾完全失效) | 先 `tag_configure` 再 `tag_add` |
| Tk 全局 mark 多用途复用 | 多个折叠块共用一个 mark,后者覆盖前者,导致展开时删不掉提示 | 按行使用唯一 tag 定位 |
| `Text.get("1.0","end-1c")` 与 `elide` | 折叠后仍返回全文(这是 Tk 设计,便于保存) | 判断是否隐藏要看 `tag_ranges`/`dlineinfo`,不是 `get` |
| `tests/test_editor.py` 永远 skip | 它驱动 `tools/galedit.exe`,该文件不存在(实际在 `tools/legacy_galedit_c/`) | 已知缺陷,未修;`check(..., True)` 是恒真断言 |
| `test_bluedit.py` 有 1 项失败 | 该测试向 `%TEMP%` 写临时目录,受限环境下被拒 | **环境问题,非代码缺陷**;本地正常环境可过 |
| `%TEMP%` 不可写的连锁反应 | `test_designer`/`test_module`/`test_vm_versions` 退出码为 1 但 FAIL=0 | 同上,都是 temp 目录清理时的 `PermissionError` |

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

| 提交 | 内容 |
|------|------|
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

按价值排序,括号内是我的评估:

1. **`tests/test_editor.py` 的恒真断言**(低风险,易修):它测的是不存在的
   `tools/galedit.exe`,两个用例永远 skip,`check(..., True)` 无意义。
   要么修路径、要么删除该测试文件。
2. **`.dexdef` 的 arity≤3 不在编译期校验**:编译/汇编都能过,直到运行时第一次
   `NCALL` 才报错。建议在 `defparser`/`compiler` 加检查。
3. **`galide/` 停用但仍在仓库**:与 `bluedit/` 有 13 个同名函数、
   `settings.py` 与 `theme.py` 几乎重复,且**两者写同一个 `~/.galide.json`**。
   建议删除或抽出共享的 `galcore`。
4. **数据目录里的散件**:根目录曾有大量以 `_` 开头的调试脚本(已 gitignore,
   未删除);`我的项目/`、`我的界面设计/`、`完整导出/`、`mycode/` 是用户数据,归属待确认。
5. **IDE 未实现**:多光标、`Ctrl+D` 选同词、正则/全词查找、查找历史、
   自动换行、代码折叠跨多行注释。
6. **文档漂移风险**:`README`/`SPEC` 里的测试项数会随测试增加而过时;
   本文件刻意不逐条维护那些数字,以实际运行为准。

### 明确不建议做(除非条件变化)

- **不要重新引入 P4/P5(引用计数或 GC)**:除非出现真实的字符串累积需求。
  详细理由见 `docs/MEMORY_DESIGN.md` 6.1。
- **不要给 `.dexdef` 加"必须释放"的约定**:现有 6 个库都返回静态缓冲区,
  默认契约(借用 + VM 复制)已经覆盖;`(string)->void` 是给需要的库的可选升级。
