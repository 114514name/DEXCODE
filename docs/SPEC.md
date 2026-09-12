# DEXCODE 字节码解释器项目规格说明

## 1. 项目目标

构建一个完整的"自制语言工具链":

- 一门自制的命令式小语言 **DexLang**(源码后缀 `.dex`)
- **Python 工具链**:将 DexLang 源码经词法/语法分析后,**先降级为汇编形式**(栈式汇编 IR,`.dxasm`),再**汇编成字节码**(`.dexbc`)
- **C 字节码解释器(VM)**:加载并执行 `.dexbc`
- **反汇编器**:将 `.dexbc` **反编译回汇编形式**,支持汇编↔字节码往返

## 2. 处理管线

```
DexLang 源码  --Lexer-->  Token 流  --Parser-->  AST
AST  --Compiler(降级)-->  汇编 IR(.dxasm 文本)
汇编 IR  --Assembler-->  字节码二进制(.dexbc)
字节码  --Disassembler--> 汇编文本(.dxasm)
字节码  --C VM-->  执行结果
源码 --include/refer--> .dexdef 定义文件 --Compiler--> 注册 DLL 原生函数(NCALL)
```

## 3. DexLang 语言(第 1 版)

- 类型:整数(int64)、浮点(float64)、字符串
- 语句:`let` 声明、赋值、`print`、`if/else`、`while`、`func` 函数、`return`、
  `include "库"`、`refer "定义文件"`、表达式语句
- 表达式:字面量、变量、二元运算(`+ - * / %`)、比较(`== != < <= > >=`)、
  逻辑(`&& ||`,短路)、一元(`! -`)、函数调用、括号
- 关键字:`let if else while func return print true false include refer`
- 注释:`//` 行注释、`/* */` 块注释、`#` 行注释
- 变量为**函数作用域**,`let` 需在使用前声明
- `print` 输出单个值并换行;`/` 始终产生浮点结果;`%` 仅支持整数
- **字符串拼接**:`+` 的一侧为字符串时自动拼接,数字自动转成字符串
  (编译为 `CONCAT` 指令;标准库亦提供 `dex_str_concat`)
- 函数需在使用前或后声明均可(编译器先收集全部函数),`main` 由顶层语句构成
- `include`/`refer` 仅允许出现在顶层,用于引入 DLL 原生库(见第 4.5 节)
- **自定义类型(`type`)是值语义**:`let b = a;` 深拷贝一份,改 `b.x` 不影响 `a.x`;
  传参同理,形参是实参的副本。赋值时对嵌套结构体一并深拷贝。
  **递归类型被拒绝**(`type N { self: N; }` 报错,`A→B→A` 亦然),因为值类型
  不能包含自身(大小无限)。这一约束使**环在类型层面不可表达**,因此运行期无需环收集器
- 字段赋值会校验类型:把 int 值赋给 string 字段、或把对象赋给基础类型字段都会在编译期报错

## 4. 汇编 IR(.dxasm 文本格式)

栈式指令集。每函数以 `.func NAME ARITY` 开始。标签 `NAME:` 单独一行。
库表以 `.lib "路径"` 声明;原生函数以 `.native NAME LIBIDX 签名` 声明。

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH` | 常量(int/float/字符串) | 压入常量 |
| `LOAD` | `%N` 局部槽 | 压入局部变量 |
| `STORE` | `%N` 局部槽 | 弹出并存到局部变量 |
| `ADD SUB MUL DIV MOD` | 无 | 弹出 b,a,压入 a op b |
| `CONCAT` | 无 | 弹出 b,a,压入 a+b(字符串拼接;数字自动转字符串) |
| `NEG` | 无 | 弹出并压入取负 |
| `EQ NE LT LE GT GE` | 无 | 比较,压入 0/1 |
| `AND OR NOT` | 无 | 逻辑运算,压入 0/1 |
| `JMP` | 标签 | 无条件跳转(函数内偏移) |
| `JZ` | 标签 | 弹出,若假则跳转 |
| `JNZ` | 标签 | 弹出,若真则跳转 |
| `CALL` | `@NAME` | 调用 DexLang 函数 |
| `NCALL` | `NAME` | 调用 DLL/so 原生函数 |
| `RET` | 无 | 弹出返回值,恢复调用帧 |
| `PRINT` | 无 | 弹出并打印值(换行) |
| `POP DUP NOP HALT` | 无 | 栈操作/停机 |

`&&`/`||` 由编译器展开为短路跳转序列,不直接生成 `AND/OR`(后者保留供手工汇编/完整性)。

### 4.5 定义文件(.dexdef)与原生函数

定义文件描述 DLL/so 库接口,供 `include`/`refer` 引入:

```
# math.dexdef
refer "libdexmath.dll";                       # 指向实际库(相对定义文件解析)
extern func dex_add(a: int, b: int) -> int;   # 类型:int/float/string/void
extern func dex_hello() -> string;
```

- `include "name"`:`name.dexdef` 在源码目录与 `-L` 目录中搜索
- `refer "path"`:显式指向定义文件(相对源码目录)
- `refer static "path.dll"`:静态链接。编译器把 DLL 字节内嵌进字节码,运行时从字节码中
  提取 DLL 到临时文件再加载,程序自带库,无需外部 DLL 文件(见第 5 节库表)
- 编译器据此注册原生函数(名字、arity、参数/返回类型)并可做静态检查
- 签名串约定:`ii:i` = 参数类型串 + `:` + 返回类型(`i`=int `f`=float `s`=string `v`=void)
- **FFI 限制**:参数类型 `int/float/string`,参数个数 ≤ 3(**编译期强制**,见下);返回
  `int/float/string/void`;支持的签名组合见 `vm/vm.c` 的 `native_sig_supported`
  (不支持的组合在运行时明确报错)。除常见组合外,还支持 `(string,string)`、
  `(string,int)`、`(string,int,int)` 与 `(int/float→string)`(标准库所需)。
- **arity 上限的强制点**:`dexlang/opcodes.py` 的 `MAX_NATIVE_ARITY = 3` 是唯一上限来源。
  `defparser.parse_def()`(`.dexdef`)与 `parse_sig()`(汇编文本的 `.native` 行)都在解析期
  拒绝超过 3 个参数的签名,报 `at most 3 are supported`。
  手写或篡改的 `.dexbc` 绕过前端,故 VM 仍保留两道运行期兜底:加载期拒绝
  `arity > MAX_NATIVE_ARGS`(=8,`param_types` 数组容量),调用期拒绝 arity 4..8,
  报 `unsupported native arity`。
- **标准库**:`libs/std/` 提供纯 C 实现的 `libdexstd.dll`(math/string/io/time/
  random/system 共 33 个函数,见 `std.dexdef`)。使用 `refer static` 可静态内嵌进
  `.dexbc`,运行时只依赖 C 虚拟机,无需 Python 或外部 DLL

### 4.6 字符串所有权(FFI 契约)

**默认契约:原生函数返回的字符串是「借用」。**

| 方向 | 规则 |
|------|------|
| 传入原生函数的 `string` | **借用**。原生侧只读,不得保存指针、不得释放。VM 保证在本次调用期间有效 |
| 原生函数返回的 `const char*` | **借用**。缓冲区归原生库所有;VM 立即复制一份进自己的堆并使用副本,从不释放原生指针 |
| 调用方(VM) | 对**未声明释放函数**的库,假定其返回的是静态/常驻缓冲区,不做任何释放 |

因此库作者可以安全地返回静态缓冲区、线程局部缓冲区,或库内自管理的常驻缓冲,
**不需要**为返回值的内存负责,也**不应该**期望 VM 去释放它。

**可选升级:需要返回堆分配字符串的库。**

若库确实需要每次调用都 `malloc` 一份返回值,可在自己的定义文件里额外声明一个
释放函数,签名为 `(string) -> void`:

```
refer "libfoo.dll";

extern func foo_name(id: int) -> string;   # 返回堆分配的字符串
extern func foo_free(p: string) -> void;   # (string)->void —— 会被认作本库的释放函数
```

编译器发现定义文件中存在 `(string) -> void` 的函数后,会把它登记为本库的释放函数,
写进字节码的库表(`DXRL` 小节,见第 5 节)。运行时流程:

```
NCALL foo_name        → 原生返回 p(malloc 的缓冲区)
VM: r.as.p = strdup(p)   ← VM 持有自己的副本
VM: foo_free(p)          ← 用原生原始指针释放原生缓冲区
结果:栈上留下 VM 的副本,原生缓冲区已归还,无泄漏
```

要点:

- VM 传给释放函数的是**原生的原始指针**,不是 VM 的副本,因此不会双重释放。
- 释放只发生在「返回类型为 `string`」的原生调用之后;其它返回类型不涉及。
- 一个库只认第一个 `(string) -> void` 函数作为释放函数。
- 释放**必须由 VM 执行**:原始指针只存在于 VM 的值栈上,编译器无法用字节码表达它
  (字节码里的 `DUP` 复制的是 VM 已复制的副本,交给释放器会导致双重释放)。
- 未声明释放函数时**不产生任何警告** —— 那正是默认契约的常态,给每个项目都报警
  只会污染 IDE 的问题面板。

回归测试见 `tests/test_memmodel.py`,配套库 `tests/fixtures/libleaky.c`。

## 5. 字节码格式(.dexbc)

二进制,小端序。**头部(18 字节)**:

| 偏移 | 大小 | 字段 |
|------|------|------|
| 0 | 4 | 魔数 `"DEXC"` |
| 4 | 1 | 版本 `3` |
| 5 | 1 | 保留(0) |
| 6 | 2 | 常量个数 `n_consts`(u16) |
| 8 | 2 | 函数个数 `n_funcs`(u16) |
| 10 | 2 | 库个数 `n_libs`(u16) |
| 12 | 2 | 原生函数个数 `n_natives`(u16) |
| 14 | 4 | 代码段字节数 `code_size`(u32) |

**常量池**:每个常量 1 字节标签 + 负载
- `0` INT:8 字节 int64
- `1` FLOAT:8 字节 float64
- `2` STRING:u16 长度 + UTF-8 字节

**库表**:每库 7 字节固定 + 可选内嵌数据
- `path_idx` u16(库路径字符串常量索引)
- `flags` u8(位 0 `LIB_STATIC`:库字节内嵌在字节码中,静态链接)
- `data_len` u32(内嵌数据字节数,非静态为 0)
- 若 `flags & LIB_STATIC`,其后紧跟 `data_len` 字节的库文件内容(如 PE DLL 或 ELF .so)

静态库加载:VM/pyvm 运行时把内嵌字节写到临时文件(`<temp>/dexvm_<pid>_<i>.dll`),
`LoadLibrary`/`dlopen` 该临时文件;进程结束释放句柄并删除临时文件。
因此静态链接的 .dexbc 是自包含的,分发时无需附带外部 DLL。

**原生函数表**:每个 = 6 字节固定 + arity 字节参数类型
- `name_idx` u16(函数名常量)
- `lib_idx` u16(库表索引)
- `arity` u8
- `ret_type` u8(`0`void `1`int `2`float `3`string)
- `param_types` arity 字节(同上类型码)

**函数表**:每函数 13 字节
- `name_idx` u16(指向字符串常量)
- `arity` u8
- `nlocals` u16
- `code_off` u32(代码段内偏移)
- `code_len` u32

**代码段**:连续指令。指令 = 1 字节操作码;若该操作码带操作数则再跟 4 字节 u32 小端操作数。
带操作数的指令:`PUSH LOAD STORE JMP JZ JNZ CALL NCALL`(5 字节),其余 1 字节。

跳转操作数为**函数内代码偏移**(相对于该函数 `code_off`);`NCALL` 操作数为原生函数表索引。

### 5.1 可选尾节:字符串释放表(`DXRL`)

**位于代码段之后**,用于记录各库的字符串释放函数(见 4.6)。布局:

| 偏移 | 大小 | 字段 |
|------|------|------|
| +0 | 4 | 标记 `"DXRL"` |
| +4 | 2 | 条目数 `n`(u16) |
| +6 | 4×n | 每条 = `lib_idx`(u16) + `release_native_idx`(u16) |

- **可选且向后兼容**:只有当存在声明了释放函数的库时才写入。旧字节码没有这一节,
  加载器按「代码段之后是否还有字节 + 标记是否为 `DXRL`」探测。
- 旧版 VM 只读取 `code_size` 之前的内容,因此完全不认识本节的实现也能正常运行这些文件。
- 因此**无需变更版本号**,也无需改动函数表(13 字节)与库表(7 字节)的定长布局。
- 尾节由汇编器从 `LibInfo.release_name` 生成;反汇编器读回该字段,故 `.dexbc → .dxasm → .dexbc`
  的往返仍逐字节一致(`tests/test_memmodel.py` 覆盖)。

## 6. C VM 语义

- 值栈(定长数组)+ 帧栈(定长数组)
- 每帧:局部变量数组、栈基址、返回 PC、函数索引、nlocals
- `CALL`:弹出 arity 个实参 → 复制入新帧局部变量,sp 回退到基址,压帧,push 新帧
- `RET`:弹出返回值,弹出帧,sp=帧基址,压入返回值,pc=返回地址;`main` 返回即停机
- `NCALL`:弹出 arity 个实参,按定义文件签名编组为 C 类型,通过 LoadLibrary/dlsym
  解析的函数指针调用 DLL/so 导出函数,结果压栈;库与符号惰性加载。
  对静态库(`LIB_STATIC`),先解包内嵌字节到临时文件再加载,不依赖外部 DLL
- 运算规则:整型运算得整型;任一浮点则提升为浮点;`/` 恒为浮点;`%` 仅整型;
  字符串仅支持比较与打印
- 运行期错误:带函数名与 pc 输出到 stderr 并退出(含 DLL 加载失败、符号缺失、
  签名不支持、参数类型不匹配等)

## 7. 目录结构

```
DEXCODE/
  main.py                  # CLI: compile/asm/disasm/run/roundtrip/build-vm
  dexlang/                 # Python 工具链包
    __init__.py opcodes.py errors.py tokens.py lexer.py ast.py
    parser.py ir.py asmtext.py defparser.py
    compiler.py assembler.py disassembler.py
  vm/vm.c                  # C 字节码解释器(单文件,含原生 FFI)
  libs/<库名>/             # 纯 C 库:源码 + DLL + .dexdef(math/std/img/ui)
  examples/*.dex           # 示例程序(hello/fib 等 + 各库的子目录示例)
  tests/                   # test_toolchain.py + test_native.py 等(21 个文件 / 639 处断言)
  docs/SPEC.md             # 本规格
  docs/TUTORIAL.md         # 教学文档
```

## 8. 阶段划分(PHASE)

1. 需求分析与架构设计(本文档)
2. 共享定义:opcodes / errors / tokens
3. 词法分析器 Lexer
4. AST 与语法分析器 Parser
5. 汇编 IR 与编译器(源码 → 汇编,降级)
6. 汇编文本格式 + 汇编器(汇编 → 字节码)
7. 反汇编器(字节码 → 汇编)
8. C 字节码解释器 VM
9. CLI 集成 + 构建脚本
10. 示例程序 + 测试 + 端到端验证
11. **扩展:include/refer 引入 DLL 原生库(定义文件、NCALL、FFI)**
12. **扩展:编译期错误检查增强(原生 arity/类型、顶层 return、除零、重复引入、不可达警告)**
