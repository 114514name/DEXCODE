# DEXCODE 字节码解释器 · 教学文档

> 从零开始理解"一门语言是怎么被造出来的"。
> 本文档配合 `docs/SPEC.md`(格式规格)与 `README.md`(使用手册)阅读。
> 所有示例均可在本仓库中真实运行验证。

---

## 第 0 章 预备知识:解释器 / 编译器 / 字节码

写代码时,CPU 只认识机器指令,不认识 `print "hi"`。于是需要**把高级语言翻译成 CPU 能执行的东西**,这就是"语言实现"。大体有两种路线:

| 路线 | 做法 | 例子 |
|------|------|------|
| **编译器** | 先把整份源码翻译成目标代码,再运行目标代码 | C、Go、Rust |
| **解释器** | 边翻译边执行,不预先产出完整目标文件 | Python、Ruby、早期的 JS |

**字节码(Bytecode)** 是一种"中间产物":它比高级语言更接近机器(指令短、结构规整),但比机器码更抽象、可移植。一个常见架构是:

```
源码 → 前端(词法+语法)→ 中间表示 → 后端(代码生成)→ 字节码 → 虚拟机执行
```

**虚拟机上跑字节码**,正是本项目做的事。Java 的 JVM、Python 的 CPython 都是这个思路。本项目用 **Python 写编译器**(前端 + 中端 + 汇编),用 **C 写虚拟机**(后端执行),恰好还原了工业界最常见的分工。

---

## 第 1 章 项目总览:一趟完整旅程

```
          ┌───────────── Python 工具链 ─────────────┐          ┌─────────┐
源码 .dex │ 词法 → 语法 → 降级 → 汇编器             │ 字节码     │ C 虚拟机 │
─────────►│ Lexer  Parser  Compiler Assembler      │─.dexbc───►│   VM     │──► 输出
          │  (Token)  (AST)   (.dxasm 汇编 IR)      │           └─────────┘
          └─────────────────────────────────────────┘
                                     ▲
                         反汇编器 Disassembler
                         (.dexbc 重新变回 .dxasm)
```

四个关键角色:

| 组件 | 语言 | 输入 → 输出 | 职责 |
|------|------|-------------|------|
| 词法分析器 `lexer.py` | Python | 源码 → Token 流 | 把字符切成"词" |
| 语法分析器 `parser.py` | Python | Token 流 → AST | 把词组成"句子" |
| 编译器 `compiler.py` | Python | AST → **汇编 IR** | **降级**:把高级结构拆成低级指令 |
| 汇编器 `assembler.py` | Python | 汇编 IR → **字节码** | 把指令编码成二进制 |
| 反汇编器 `disassembler.py` | Python | 字节码 → 汇编 | 逆过程,用于调试/理解 |
| 虚拟机 `vm/vm.c` | C | 字节码 → 执行结果 | 解释执行 |

**为什么中间要先过一遭"汇编"?** 这是本项目最核心的设计思想:
1. **汇编是一种人类可读的低级表示**,方便调试——你能亲眼看到 `if` 变成了几条 `JZ` 跳转;
2. **编译分两步更易实现与测试**:先解决"语义"(把 `if/while/函数` 变成指令序列),再解决"编码"(把指令序列压成二进制);
3. **汇编可再汇编、字节码可反汇编**,形成一个可验证的闭环(见第 8 章)。

---

## 第 2 章 DexLang 语言入门

DexLang 是一个"小而全"的命令式语言,第 1 版包含:

- 三种类型:`int64`、`float64`、`string`
- 语句:`let`(声明)、赋值、`print`、`if/else`、`while`、`func`(函数)、`return`
- 表达式:算术 `+ - * / %`、比较 `== != < <= > >=`、逻辑 `&& ||`(短路)、一元 `! -`、函数调用

```c
// 递归斐波那契 —— 本项目的"标准测试程序"
func fib(n) {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

let i = 0;
while i <= 10 {
    print fib(i);
    i = i + 1;
}
```

几条约定(SPEC 里都有):
- 变量是**函数作用域**,`let` 需在使用前声明;
- `/` 永远产生浮点结果(`7 / 2` 是 `3.5`),`%` 只支持整数;
- `print` 每个值单独输出一行;
- `true` / `false` 只是 `1` / `0` 的语法糖。

> 它是"Turing 完全"的:有变量、分支、循环、函数、递归——理论上能算任何可计算函数。

---

## 第 3 章 前端一:词法分析(Lexer)

**任务**:把源代码字符串拆成一个个 **Token**(带类型的"词"),并顺带跳过空白和注释、报告非法字符。

以 `print square(a) + 1;` 的一部分为例,`lexer.py` 会产出:

```
FUNC     'func'
IDENT    'square'
LPAREN   '('
...
RETURN   'return'
IDENT    'x'
STAR     '*'
IDENT    'x'
SEMI     ';'
INT      '3'      ← 值 3 被解析为 int 存进 token.value
EOF      ''       ← 结束标记
```

每个 Token 记录四样东西:`kind`(类别)、`lexeme`(原文)、`line` / `col`(行号列号,用于报错)。

**动手看**:在 `main.py` 里临时执行
```python
from dexlang import Lexer
for t in Lexer('let a = 3;', '<t>').tokenize():
    print(t)
```

**练习**:想支持 `+=` 或 `//` 整除,应该改哪里?(提示:先查 `tokens.py` 的 `TokKind` 和 `KEYWORDS`,再在 `lexer.py` 的双字符操作符 `pairs` 字典里加。)

---

## 第 4 章 前端二:语法分析(Parser)与 AST

**任务**:把 Token 流组织成 **AST**(抽象语法树)——一种描述"程序结构"的树。

`parser.py` 用的是**递归下降 + 优先级爬升**:
- 每种语法结构对应一个函数:`parse_statement`、`parse_expression`、`_parse_term`……
- 优先级靠"层层包裹"实现:`a + b * c` 中,`*` 所在的 `_parse_factor` 比 `+` 所在的 `_parse_term` 更内层,所以 `b * c` 先被解析成一个整体。

以 `func square(x) { return x * x; } let a = 3; print square(a) + 1;` 为例,AST(简化)是:

```
Program
├── Func(name=square, params=[x])
│   └── body: [ Return( BinOp(*, Name(x), Name(x)) ) ]
├── Let(name=a, value=Literal(3))
└── Print( exprs=[ BinOp(+, Call(square, [Name(a)]), Literal(1)) ] )
```

AST 节点定义在 `ast.py`。注意它们都继承自带 `line/col` 的 `Node`,这是报错定位的基础。

> 💡 经验教训:Python `dataclass` 继承时,**父类字段排在子类字段前面**,所以构造节点必须用关键字参数(如 `Literal(value=3)`),否则会把值错塞进 `line`。本项目曾因此踩坑,`parser.py` 里现在全部使用关键字构造。

---

## 第 5 章 中端:编译与降级(Compiler,AST → 汇编)

这是"**把高级语言低级化**"的关键一步。`compiler.py` 把 `if / while / && / 函数调用` 这些"高层结构"展开成一张**扁平的指令表**。

### 5.1 栈式指令集

DexLang 的汇编是一个**栈式(stack-based)指令集**:指令直接在"值栈"上运算,不命名操作数寄存器。核心规则:

- 计算表达式 = 往栈上**压值** + 用指令**弹出并运算再压回**
- 变量 = 当前函数的**局部槽位** `%0, %1, %2...`(参数占 0..arity-1)

```
表达式 x * x + 1        →   LOAD %0     ; 把 x 压栈
                            LOAD %0
                            MUL         ; 弹出两个,相乘,压回
                            PUSH 1
                            ADD
```

常用指令(完整列表见 SPEC 第 4 节):

| 指令 | 作用 |
|------|------|
| `PUSH 常量` | 压入字面量 |
| `LOAD %N` / `STORE %N` | 压入 / 弹出存回局部变量 |
| `ADD SUB MUL DIV MOD NEG` | 算术(弹出 b,a,压入 a op b) |
| `EQ NE LT LE GT GE` | 比较,压入 0/1 |
| `JMP / JZ / JNZ 标签` | 跳转;`JZ` 弹出值,若"假"则跳 |
| `CALL @函数` / `RET` | 函数调用 / 返回 |
| `PRINT / POP / DUP / HALT` | 打印 / 丢弃 / 复制栈顶 / 停机 |

### 5.2 高层结构如何"塌缩"成跳转

**`if`** 变成"条件求值 + 条件跳转 + 两个代码块":

```
    <条件表达式>        ; 栈顶留下 0/1
    JZ  else_label
    <then 代码块>
    JMP end_label
else_label:
    <else 代码块>
end_label:
```

**`while`** 变成"回跳循环":

```
loop_label:
    <条件表达式>
    JZ  end_label
    <循环体>
    JMP loop_label
end_label:
```

**`&&` / `||` 的短路求值**也靠跳转实现(右侧只在需要时才求值)。

**函数** 被编译成独立指令块;调用前把实参依次压栈,然后 `CALL @name`,`CALL` 时 VM 会弹出 arity 个实参塞进新函数帧的局部槽。

### 5.3 一个完整例子

把 `func square(x) { return x * x; } let a = 3; print square(a) + 1;` 编译后得到:

```
.func main 0
    PUSH 3          ; a = 3
    STORE %0
    LOAD %0         ; 压入 a
    CALL @square     ; 调用 square(a),栈上留下结果
    PUSH 1
    ADD
    PRINT
    HALT

.func square 1
    LOAD %0         ; x
    LOAD %0         ; x
    MUL
    RET
```

可以看到:每个 `func` 一行头部(`.func 名字 参数个数`),指令缩进排列,跳转目标用符号标签。这就是**汇编形式(.dxasm)**——人类可以直接读懂的低级代码。

---

## 第 6 章 后端一:汇编器(Assembler,汇编 → 字节码)

**任务**:把符号化的汇编(有名字、有标签)翻译成**紧凑的二进制**——只有数字,没有名字。

`assembler.py` 做四件事:
1. **建常量池**:把所有字面量(`3`、`"hi"`、函数名)去重,编号;
2. **解析标签**:把 `JMP end_label` 里的名字换算成"函数内偏移"(字节数);
3. **分配局部槽**:把 `LOAD %N` 直接映射为槽号;
4. **编码**:每条指令 = `1 字节操作码 [+ 4 字节小端操作数]`。

### 6.1 字节码二进制格式(.dexbc)

以上面例子为例,114 字节的完整二进制布局:

```
偏移  内容                                说明
─────────────────────────────────────────────────────────────
0    44 45 58 43                         魔数 "DEXC"
4    01                                  版本
5    00                                  保留
6    04 00                               n_consts = 4
8    02 00                               n_funcs  = 2
10   28 00 00 00                         code_size = 40
─────────────────────────── 常量池 ─────────────────────────
14   02 04 00 6d 61 69 6e               STRING "main"
21   02 06 00 73 71 75 61 72 65         STRING "square"
30   00 03 00 00 00 00 00 00 00         INT 3
39   00 01 00 00 00 00 00 00 00         INT 1
─────────────────────────── 函数表(每项 13 字节) ───────────
48   00 00 | 00 | 01 00 | 00 00 00 00 | 1c 00 00 00
     name=0 arity=0 nlocals=1 code_off=0  code_len=28   (main)
61   01 00 | 01 | 01 00 | 1c 00 00 00 | 0c 00 00 00
     name=1 arity=1 nlocals=1 code_off=28 code_len=12   (square)
─────────────────────────── 代码段(偏移 74 起,40 字节) ────
main(0..27):
    01 02 00 00 00    PUSH const[2]=3
    03 00 00 00 00    STORE %0
    02 00 00 00 00    LOAD %0
    16 01 00 00 00    CALL func[1]=square
    01 03 00 00 00    PUSH const[3]=1
    04                ADD
    18                PRINT
    1b                HALT
square(28..39):
    02 00 00 00 00    LOAD %0
    02 00 00 00 00    LOAD %0
    06                MUL
    17                RET
```

要点:
- **操作数一律 4 字节小端**:如 `PUSH const[2]` 编码为操作码 `01` + `02 00 00 00`;
- **跳转操作数是"函数内偏移"**:`main` 内第 1 条指令偏移为 0,`square` 内第 1 条指令偏移为 0(它们共享一份大代码段,靠 `code_off/code_len` 划分区域);
- **常量可去重**:同一个 `3` 只存一份,`PUSH` 引用其编号。

> 💡 经验教训:跳转偏移是**函数内相对偏移**,虚拟机执行跳转时必须加上当前函数的 `code_off`;反汇编器收集跳转目标时也要统一换算成绝对偏移。主函数 `code_off=0` 时"碰巧"正确,容易掩盖 bug——本项目曾因此出现"非主函数标签全部丢失"的往返失败。

---

## 第 7 章 C 虚拟机:字节码怎么被"跑"起来

`vm/vm.c` 是一个约 400 行的单文件栈式虚拟机。它只做三件事:**加载 → 校验 → 循环执行**。

### 7.1 两个核心数据结构

```c
typedef struct {            // 值:带类型标签的联合体
    VType type;             // V_INT / V_FLOAT / V_STR
    union { int64_t i; double f; char *p; } as;
} Value;

typedef struct {            // 调用帧
    Value *locals;          // 局部变量数组(长度 = nlocals)
    size_t base;            // 本帧在值栈中的基址
    uint32_t ret_pc;        // 返回地址(返回到调用者的下一条指令)
    uint16_t func_idx;      // 当前函数编号(用于报错显示函数名)
} Frame;
```

程序启动时:
- 把 `funcs[0]`(main)做成一个**主帧**,`pc` 指向它的 `code_off`;
- 循环 `取操作码 → switch 分发 → 改 pc/栈/帧`,直到 `HALT` 或 `RET` 出 main。

### 7.2 表达式怎么执行

`ADD` 就是"弹出两个数,相加,压回":

```c
case OP_ADD: {
    if (sp < 2) die(prog, fr, pc, "stack underflow");
    Value b = stack[--sp], a = stack[--sp];
    Value r = value_binop(prog, fr, pc, op, a, b);   // 处理 int/float 混算、除零、类型错误
    stack[sp++] = r;
    pc += 1;
    break;
}
```

整型 + 整型得整型;只要有一个浮点就提升为浮点;`DIV` 恒为浮点;字符串支持比较、打印与 `+` 拼接(数字自动转字符串);遇到除零、类型不匹配就报运行期错误(带函数名和 pc)。

### 7.3 函数调用:帧的"压栈"与"出栈"(最重要!)

`CALL @square`(arity=1)的步骤:

```
1. 记下实参起始位置:  base = sp - arity      ; 实参已由调用者压栈
2. 新帧:             nframes++
                     新帧.locals = calloc(nlocals)
                     把栈上 base..base+arity-1 的实参拷入 locals
                     新帧.base = base
                     新帧.ret_pc = pc + 5     ; 返回地址 = CALL 之后
                     sp = base                ; 值栈"回退"到本帧起点
                     pc = 被调函数.code_off
```

`RET` 的步骤(**两处极易写错**):

```
1. 弹出返回值 rv
2. 返回地址取"被调帧自己"的 ret_pc   ← 不是调用方的!
3. sp 恢复到"被调帧自己"的 base       ← 也不是调用方的!
   把 rv 压到该位置
4. 弹出帧,pc = 返回地址,继续执行调用者
```

为什么?因为调用者在 `CALL` 之前可能已经压了**前一个子表达式的中间结果**。比如 `fib(n-1) + fib(n-2)`,先算完 `fib(n-1)` 得到一个结果压在栈上,再算 `fib(n-2)` 时它的实参压在该结果**之上**。返回时结果必须放回"实参的起始位置",才能和前面那个结果并排,供最后的 `ADD` 一次弹出两个数。

> 💡 经验教训(本项目实测):曾把返回地址写成 `prev->ret_pc`(调用方的返回地址),导致嵌套调用后 pc 错乱、递归死循环;曾把栈恢复到 `prev->base`,导致第二个子表达式的结果覆盖第一个。修正为"取被调帧自己的 ret_pc 与 base"后,`fib(10)=55` 立即通过。

### 7.4 用 `DEX_TRACE` 观察每一步

VM 内置指令级跟踪:设置环境变量 `DEX_TRACE=1` 后运行,会打印每条指令、所在函数、pc、栈深:

```
$env:DEX_TRACE=1; ./vm/vm.exe examples/fib.dexbc
[main] pc=0 sp=0 PUSH c2
[main] pc=5 sp=1 CALL f1
[fib]  pc=12 sp=0 LOAD %0
[fib]  pc=17 sp=1 PUSH c3
[fib]  pc=22 sp=2 LT
...
```

这是理解"栈是怎么涨落"的最好工具(注意:递归很深时输出会很大,建议用 `fib(4)` 之类的小输入)。

---

## 第 8 章 反汇编与往返:字节码 ↔ 汇编

`disassembler.py` 是汇编器的**逆过程**:读二进制 → 还原出操作码/操作数 → 按跳转目标生成符号标签 → 输出人类可读的汇编文本。

本项目最自豪的验证是**往返一致性**:对同一份字节码,

```
字节码 → 反汇编(文本)→ 重新汇编 → 字节码'
```

要求 **`字节码'` 与 `字节码` 逐字节相等**。命令行一键校验:

```bash
python main.py roundtrip examples/fib.dexbc
# 往返校验通过:字节码与反汇编后重新汇编一致
```

实现能往返的关键:
1. 常量池去重顺序固定(函数名先、PUSH 常量按代码顺序),重汇编能重建出完全相同的池;
2. 跳转偏移统一为"函数内相对",汇编与反汇编各做一次正确的换算;
3. 汇编文本解析能正确处理**含空格的字符串**与注释。

---

## 第 9 章 动手实践

### 9.1 常用命令

```bash
# 构建 C 虚拟机(本机无 gcc 时自动用 ziglang 包的 zig)
python main.py build-vm

# ① 源码 → 汇编 + 字节码
python main.py compile examples/fib.dex
#    生成 examples/fib.dxasm(汇编)与 examples/fib.dexbc(字节码)

# ② 用 C 虚拟机执行字节码
python main.py run examples/fib.dexbc

# ③ 汇编文本 → 字节码(独立路径,可手写汇编!)
python main.py asm examples/fib.dxasm -o manual.dexbc

# ④ 字节码 → 汇编(反编译)
python main.py disasm examples/fib.dexbc -o fib_back.dxasm

# ⑤ 往返一致性校验
python main.py roundtrip examples/fib.dexbc

# ⑥ 全套测试
python tests/test_toolchain.py
```

### 9.2 建议的上手步骤

1. 先跑 `run` 和 `roundtrip`,建立"一切正常"的感觉;
2. 用 `compile` 生成 `.dxasm`,**亲手读一读** `if`、`while`、`&&` 被展开成了哪些跳转;
3. 用 `disasm` 看同一份 `.dxasm` 能否被还原;再 `roundtrip` 验证逐字节一致;
4. 打开 `DEX_TRACE` 跑一个 `fib(4)`,亲眼观察栈深随递归涨落;
5. 尝试**手写一份汇编**文件,用 `asm` 命令汇编、`run` 运行——此时你就是"汇编程序员"。

### 9.3 想改语言?改哪里?

| 想加的功能 | 修改位置 |
|-----------|----------|
| 新关键字(如 `for`) | `tokens.py` 加 `TokKind`+`KEYWORDS` → `lexer.py` 无需改 → `parser.py` 加语法 → `compiler.py` 降级 |
| 新运算符(如 `^`) | `lexer.py` 加 token → `parser.py` 加优先级 → `compiler.py` 映射 opcode → `opcodes.py` 定义 → `vm.c` 实现 |
| 新类型(如 bool/数组) | `Value` 联合体 + 常量池 tag + 运算规则 |
| 新指令 | `opcodes.py` 定义 → `assembler.py`/`disassembler.py` 编解码 → `vm.c` switch 实现 |

---

## 第 10 章 练习题

**基础**
1. 写出 `while` 求 `1+2+...+100` 的 DexLang 程序并运行,验证 `5050`。
2. 解释下面汇编每行的作用,并手算最终输出:
   ```
   .func main 0
       PUSH 5
       PUSH 2
       MUL
       PUSH 1
       ADD
       PRINT
       HALT
   ```
3. 用 `DEX_TRACE` 观察 `fib(4)`,数一数 `fib` 被调用了几次。

**进阶**
4. 为什么 `JZ` 跳转目标用"函数内偏移"而不是整个代码段的绝对偏移?如果把偏移改成绝对,汇编器和 VM 要各自改哪里?
5. `a && b || c` 会被降级成怎样的跳转序列?写出汇编。
6. 反汇编为什么能还原函数名,却不能还原局部变量名?(提示:字节码里只存了槽号 `%N`,没存名字。)
7. 为语言增加 `for i in a..b { ... }` 语法,要求编译成 `while` 的等价指令序列。

**挑战**
8. 给 VM 增加"调用深度上限"的报错(而不是直接栈溢出崩溃),想想该在 `CALL` 的哪里检查。
9. 尝试把 `%` 改为支持浮点(`fmod`),需要改 `compiler.py`、`vm.c` 的哪些判断?
10. 给字节码增加一个"校验和"字段,加载时校验文件是否被篡改。

---

## 第 11 章 引入原生库(include / refer)

很多语言都支持"调用 C 库"。本项目的做法分三层:

### 11.1 定义文件(.dexdef)——声明 DLL 接口

用 `extern func` 描述 DLL 导出的每个函数:名字、参数类型、返回类型。

```
# math.dexdef
refer "libdexmath.dll";                          # 指向实际库文件
extern func dex_add(a: int, b: int) -> int;
extern func dex_fact(n: int) -> int;
extern func dex_sqrt(x: float) -> float;
extern func dex_hello() -> string;
```

类型只有四种:`int`(int64)、`float`(float64)、`string`(char*)、`void`(无返回值)。

### 11.2 include / refer ——引入

```c
include "math";      // 在 libs/ 目录搜索 math.dexdef
// 或
refer "libs/math/math.dexdef";   // 显式路径(相对项目根)

print dex_add(2, 3);        // 像普通函数一样调用 DLL 函数
```

### 11.3 编译器如何"帮忙检查错误"

引入定义文件后,编译器就知道每个原生函数的**参数个数与参数类型**,于是能静态拦截错误:

```
print dex_add(1, 2, 3);     // [compiler]error: native 'dex_add' expects 2 argument(s), got 3
print dex_fact("abc");      // [compiler]error: argument 1 ... expects an int, got a string literal
print dex_len(42);          // [compiler]error: ... expects a string
```

### 11.4 字节码与 VM 如何调用

编译器把 `dex_add(2,3)` 降级为一条 **`NCALL dex_add`** 指令,并在字节码里附加**库表**与**原生函数表**(含签名)。VM 执行 `NCALL` 时:

1. 惰性 `LoadLibrary("...libdexmath.dll")` + `GetProcAddress("dex_add")`;
2. 按签名把栈上的值编组成 C 类型(整型/浮点/字符串指针);
3. 通过函数指针调用;
4. 把返回值重新包装成带标签的 `Value` 压栈。

反汇编能看到完整的库/签名信息:

```
.lib "C:\...\libdexmath.dll"
.native dex_add 0 ii:i
    ...
    NCALL dex_add
```

> ⚠️ **FFI 边界**:这是"教学级 FFI",目前支持参数 `int/float/string`、参数个数 ≤ 3、
> 返回 `int/float/string/void`,且只支持一组常见签名组合(见 `vm/vm.c` 的
> `native_sig_supported`)。遇到不支持的组合,VM 会明确报错而不是悄悄出错——这正是
> 定义文件的意义:**把"接口约束"显式化,让错误在编译期/运行期都能被准确报告**。

### 11.5 动手实验

```bash
# 构建示例 DLL(需要 zig)
zig cc -shared -target x86_64-windows-gnu -O2 \
    -o libs/math/libdexmath.dll libs/math/libdexmath.c

# 编译并运行
python main.py compile examples/math/use_math.dex
python main.py run examples/math/use_math.dexbc

# 看反汇编里的 .lib/.native/NCALL
python main.py disasm examples/math/use_math.dexbc
```

**练习**:给 `libdexmath.c` 增加一个 `dex_pow(base, exp)` 并同步到 `math.dexdef`,
然后在 `use_math.dex` 里调用;验证编译期检查与运行结果。

### 11.6 静态链接——程序自带 DLL

默认(`refer "..."`)是**动态链接**:运行 `.dexbc` 时仍需旁边有 `libdexmath.dll`。
而 `refer static` 会把 DLL 的**字节内嵌进字节码**,运行时就无需外部 DLL 了:

```
# math_static.dexdef
refer static "libdexmath.dll";          # 关键区别:static

extern func dex_add(a: int, b: int) -> int;
extern func dex_fact(n: int) -> int;
extern func dex_hello() -> string;
```

```c
include "native/math_static";
print dex_add(2, 3);      // 正常调用
```

编译时,编译器**读取 DLL 的二进制字节**放进库表(反汇编能看到 base64):

```
.lib "C:\...\libdexmath.dll" static a2Vwd...（Base64 编码的 DLL 字节）
.native dex_add 0 ii:i
```

运行时(VM 或 pyvm):
1. 把库表里的内嵌字节**解包到临时文件**(`<temp>/dexvm_<pid>_<i>.dll`);
2. `LoadLibrary` 这个临时文件并正常 `GetProcAddress` 调用;
3. 程序结束**释放句柄并删除临时文件**。

于是 `.dexbc` 是**自包含**的——哪怕你把原 `libdexmath.dll` 删掉/改名,程序照样运行:

```bash
python main.py compile examples/math/use_math_static.dex
python main.py run examples/math/use_math_static.dexbc   # 即使 DLL 已删除也能运行
```

> 💡 想验证?把 `libs/math/libdexmath.dll` 临时改名再 `run`,你会发现程序照常输出——
> 因为库字节已经躺在 `.dexbc` 里了。`tests/test_static.py` 正是这样自动验证的。
>
> **什么时候用静态**:发布单文件程序、目标机器不确定有没有那个 DLL。代价是 `.dexbc`
> 变大(等于多存了一份 DLL)。教学项目里动态/静态都支持,顺便演示了“链接”这个经典概念。

### 11.7 标准库——纯 C 实现,脱离 Python

前面的库都是“例子”。现在给语言配一套**标准库**,放在 `libs/std/`(示例在 `examples/std/`):

- `libdexstd.c` —— **纯 C 编写**(零第三方依赖、**不含任何 Python**),编译成 `libdexstd.dll`;
- `std.dexdef` / `std_static.dexdef` —— 声明 33 个函数,按功能分组;
- `demo_std.dex` / `demo_std_static.dex` —— 演示程序(含用 DexLang 写的高层封装)。

分组一览:

| 分组 | 函数 |
|------|------|
| math 数学 | `dex_abs/fabs/pow/fmin/fmax/imin/imax/floor/ceil/round` |
| string 字符串 | `dex_strlen/upper/lower/trim/concat/repeat/sub/contains/itoa/ftoa` |
| io 文件/输入 | `dex_read_file/write_file/append_file/file_exists/remove_file/input/print/out` |
| time 时间 | `dex_now_ms/sleep_ms` |
| random 随机 | `dex_rand/srand` |
| system 系统 | `dex_system` |

用法与普通 include 完全一样:

```c
include "std";                    // 或静态版 include "std_static";
print dex_abs(-5);                 // 5
print dex_str_upper("hi");         // HI
print dex_str_concat("foo", "bar"); // foobar
print dex_write_file("a.txt", "x"); // 0
print dex_now_ms() > 0;            // 1
```

为了让 `(string,string)`、`(string,int)`、`(string,int,int)` 这些签名可用,我们扩展了
`vm/vm.c` 的 FFI 签名集合(`native_sig_supported` 与调用分发),所以标准库函数都能被 C VM 调用。

**“脱离 Python”是本项目的重要目标**:运行时只需要两样东西——

```text
vm.exe(纯 C 字节码解释器) + 你的 .dexbc(含静态内嵌的标准库)
```

不需要 Python、不需要外部 DLL。验证方式(也是 `tests/test_stdlib.py` 自动做的):

```bash
python main.py compile examples/std/demo_std_static.dex   # 编译(需要 Python 工具链)
# 之后完全脱离 Python:
./vm/vm.exe examples/std/demo_std_static.dexbc            # 直接运行!("done")
```

> 📦 若要用动态标准库,把 `libdexstd.dll` 和 `.dexbc` 一起分发即可;
> 用静态版则 `.dexbc` 自带整个标准库(191 KB 左右)。

---

## 第 12 章 DEXIDE 集成开发环境

IDE 不是"花架子":它把第 2~11 章讲的所有工具链环节**真实地**用了起来。

### 12.1 启动

```bash
python ide_main.py examples/fib.dex   # 或 python main.py ide examples/fib.dex
```

### 12.2 它怎么"分析你的代码"

IDE 的每个能力都对应真实的工具链步骤:

| IDE 能力 | 背后用的真实分析 |
|----------|------------------|
| 语法着色 | `Lexer` 逐个 token 分类(关键字/数字/字符串/注释…),再用分析器把标识符细分:函数名、原生名、变量 |
| 语法提示/补全 | `analyzer.autocomplete`:关键字 + 全项目函数 + 聚合原生库 + 当前函数作用域变量;`Ctrl+Space`/`Alt+/` 触发;弹窗不抢焦点,兼容中文输入法(默认不在输入时自动弹出,可在「视图」菜单开启) |
| 悬停文档 | `analyzer.hover`:函数给出 arity/参数/调用者/被调用者;原生函数给出签名与来源库 |
| 大纲/结构 | `analyzer` 遍历 AST:函数(参数、调用点)、`include`、顶层变量 |
| 函数关系 | AST 收集 `Call` 节点构建调用图(调用者 ↔ 被调用者) |
| 自动分析库 | 扫描 `include`/`refer` → 解析 `.dexdef` → 聚合原生函数签名 |
| 问题面板 | 真正执行"词法→语法→编译",把 `DexError`/警告放进面板 |
| 系统终端运行 | 「运行」菜单/工具栏的「终端」按钮:把当前 `.dex` 编译成 `.dexbc`,在**系统默认终端**的新窗口中运行 `vm.exe`——支持 ANSI 真彩色(图片渲染)与 stdin 交互,`cmd /k` 保持窗口以便查看 |

### 12.3 调试器是怎么工作的

IDE 的"调试"不调用 C VM,而是运行 **`dexlang/pyvm.py`** —— 一个与 C VM 语义一致的
**Python 版虚拟机**:

- 断点:点击行号槽,把行号发给 VM;VM 每条指令带源码行号,命中即暂停
- 单步:step-into(执行一条指令)/ step-over(执行完当前调用)/ step-out(返回调用者)
- 调用栈:每个 `Frame` 记录函数、局部变量、栈基址、返回地址
- 监视:在当前帧的局部变量上下文中用 `eval` 求值表达式

因为 pyvm 也支持 `NCALL`(通过 ctypes 加载 DLL),所以带原生库的程序也能调试。
"运行"按钮则直接调用 C VM,两者输出一致——你可以对比验证 pyvm 的正确性
(`tests/test_pyvm.py` 就断言了 pyvm 与 C VM 输出一致)。

### 12.4 动手实验

1. 启动 IDE 打开 `examples/math/use_math.dex`,在"库"面板看 `math.dexdef` 自动聚合的 6 个原生函数;
2. 把鼠标悬停在 `dex_add` 上,看签名与来源库提示;输入 `de` 触发自动补全;
3. 在 `print fib(i)` 那行点断点,点"调试",观察暂停、调用栈、变量、值栈;
4. 添加一个监视表达式 `i`,点"继续"观察其变化;
5. 故意写错(如 `print nope(1);`)并保存,看"问题"面板即时报错、编辑器红色波浪线;
6. 点工具栏「汇编」把当前 `.dex` **仅编译成汇编**并在新标签查看(自带汇编高亮);
   再点「→字节码」把该 `.dxasm` 汇编成 `.dexbc`,并可直接「运行」验证。

---

## 附录 A 常见问题

- **`build-vm` 报"未找到 C 编译器"**:本机装 `gcc/clang`,或 `pip install ziglang` 后重试(`build-vm` 会自动发现 zig)。
- **运行 `run` 报"未找到 C VM"**:先执行 `python main.py build-vm`。
- **`DEX_TRACE` 输出爆炸**:改用小输入,或重定向到文件再 `tail`。
- **编译报 `[lexer]error ... unexpected character`**:源码里可能有 BOM,本项目的 Lexer 与文件读取已兼容 UTF-8 BOM。
- **想知道某条指令怎么编解码**:查 `opcodes.py` 的 `HAS_OPERAND`,汇编器/反汇编器都依赖它决定指令长度。

## 附录 B 各文件速查

| 文件 | 一句话职责 |
|------|-----------|
| `dexlang/lexer.py` | 源码 → Token |
| `dexlang/parser.py` | Token → AST |
| `dexlang/compiler.py` | AST → 汇编 IR(降级)+ include/refer + 编译期检查 |
| `dexlang/defparser.py` | .dexdef 定义文件解析(extern func 声明) |
| `dexlang/asmtext.py` | 汇编 IR ↔ 文本(.dxasm,.lib/.native/.func) |
| `dexlang/assembler.py` | 汇编 IR → 字节码二进制 |
| `dexlang/disassembler.py` | 字节码 → 汇编 IR/文本 |
| `dexlang/opcodes.py` | 操作码与字节码格式的唯一事实来源 |
| `vm/vm.c` | C 虚拟机(加载 + 执行 + 原生 FFI + 运行期错误) |
| `dexlang/pyvm.py` | Python 调试虚拟机(断点/单步/调用栈/监视 + ctypes 原生调用) |
| `dexide/ide.py` | DEXIDE 主窗口(侧边栏/标签页/底部面板/调试联动) |
| `dexide/editor.py` | 代码编辑器(断点槽/行号/着色/悬停/自动补全) |
| `dexide/analyzer.py` | 项目分析(符号/函数/调用图/原生库/问题) |
| `tests/test_toolchain.py` | 26 项核心工具链测试(含往返一致性) |
| `tests/test_native.py` | 32 项 include/refer + 定义文件 + 错误检查 + DLL 调用测试 |
| `tests/test_static.py` | 16 项静态链接:static 解析、字节内嵌、v3 往返、无外部 DLL 运行 |
| `tests/test_img.py` | 13 项图片渲染库(BMP → ANSI 真彩色半块字符) |
| `tests/test_stdlib.py` | 36 项标准库(纯 C 实现 + 脱离 Python 运行) |
| `tests/test_pyvm.py` | 14 项 Python 调试 VM 测试 |
| `tests/test_ide.py` | 31 项 DEXIDE 分析器 + GUI + IME + 编译工具 + 终端运行测试 |
