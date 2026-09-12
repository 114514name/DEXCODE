"""DEXCODE 工具链 — 操作码与字节码格式的共享定义。

字节码格式(.dexbc):
  header (18B): magic "DEXC" | version(2) | reserved(1)
                | n_consts(u16) | n_funcs(u16) | n_libs(u16) | n_natives(u16)
                | code_size(u32)
  常量池: 每个常量 = tag(1B) + 负载
          tag=0 INT(8B int64) | tag=1 FLOAT(8B f64) | tag=2 STRING(u16 len + bytes)
  库表:   每个库 = 库路径字符串常量索引(u16)
  原生函数表: 每个 = name_idx(u16) | lib_idx(u16) | arity(u8) | ret_type(u8)
                    | param_types(arity 字节)          (固定 6 字节 + arity)
  函数表: 每个函数 = name_idx(u16) | arity(u8) | nlocals(u16)
                    | code_off(u32) | code_len(u32)        (共 13 字节)
  代码段: 连续指令。指令 = opcode(1B) [+ operand(u32 LE) 若有]
"""

MAGIC = b"DEXC"
VERSION = 3

HEADER_SIZE = 18
FUNC_ENTRY_SIZE = 13
LIB_ENTRY_SIZE = 7   # path_idx(2) + flags(1) + data_len(4);内嵌数据另加 data_len 字节

# 库表 flags 位
LIB_STATIC = 0x01    # 库字节内嵌在字节码中(静态链接)
NATIVE_FIXED_SIZE = 6

# 原生函数「参数个数」的编译期上限。有两套调用约定(见 docs/SPEC.md 4.5):
#
#   直接 ABI(老库,默认):实参逐个展开成 C 参数。VM 的分发是按「arity × 类型组合」
#     手写的分支表,组合数按 3^N 增长,所以只覆盖 arity 0..3。
#   值数组 ABI(新库,`.dexdef` 里写 `abi value_array;`):原生函数收
#     (const DexValue *args, int argc),与类型组合无关,故上限只受 param_types
#     的存储限制约束。
#
# 两个上限分开命名,避免把「老 ABI 的 3」和「数组 ABI 的 8」搞混。
MAX_NATIVE_ARITY = 3          # 直接 ABI 的上限
MAX_NATIVE_ARGS = 8           # 值数组 ABI 的上限(必须与 vm.c 的 MAX_NATIVE_ARGS 一致)

# 原生函数表里 ret_type 字节的高位用来标记调用约定。
# 之所以能这么用:ret_type 的有效值只有 0..3(NAT_VOID/INT/FLOAT/STR),高位是空的。
# 这样旧字节码该位恒为 0 → 新旧 VM 解析结果一致;**不需要改格式、不需要升版号**
# (vm.c 的版本检查是严格相等,升版号会让所有现存 .dexbc 立刻失效)。
NATIVE_ABI_MASK = 0x80
NATIVE_ABI_DIRECT = 0x00      # 0 = 直接 ABI(现有 6 个库)
NATIVE_ABI_ARRAY = 0x80       # 1 = 值数组 ABI(abstraction: DexValue[])

# 值数组 ABI 里每个元素的标签。**必须与 vm.c 的 VType 枚举顺序一致**
# (V_INT, V_FLOAT, V_STR, V_OBJ)。前三个已实现;DEXV_OBJ 是给未来「结构体/数组
# 引用」预留的占位,tag 值域 4..255 同样保留。
DEXV_INT = 0
DEXV_FLOAT = 1
DEXV_STR = 2
DEXV_OBJ = 3

ABI_NAMES = {"direct": NATIVE_ABI_DIRECT, "value_array": NATIVE_ABI_ARRAY}
ABI_FROM_FLAG = {NATIVE_ABI_DIRECT: "direct", NATIVE_ABI_ARRAY: "value_array"}

# 常量池标签
TAG_INT = 0
TAG_FLOAT = 1
TAG_STRING = 2

# 原生函数类型码(参数/返回值)
NAT_VOID = 0
NAT_INT = 1
NAT_FLOAT = 2
NAT_STR = 3

# 类型名 ↔ 类型码(定义文件与汇编文本使用)
TYPE_TO_CODE = {"v": NAT_VOID, "i": NAT_INT, "f": NAT_FLOAT, "s": NAT_STR}
CODE_TO_TYPE = {code: name for name, code in TYPE_TO_CODE.items()}

# 操作码
NOP = 0x00
PUSH = 0x01
LOAD = 0x02
STORE = 0x03
ADD = 0x04
SUB = 0x05
MUL = 0x06
DIV = 0x07
MOD = 0x08
NEG = 0x09
EQ = 0x0A
NE = 0x0B
LT = 0x0C
LE = 0x0D
GT = 0x0E
GE = 0x0F
AND = 0x10
OR = 0x11
NOT = 0x12
JMP = 0x13
JZ = 0x14
JNZ = 0x15
CALL = 0x16
RET = 0x17
PRINT = 0x18
POP = 0x19
DUP = 0x1A
HALT = 0x1B
NCALL = 0x1C  # 调用 DLL/so 原生导出函数(操作数 = 原生函数表索引)
CONCAT = 0x1D  # 字符串拼接(编译器在 + 的操作数为字符串时生成;数字自动转字符串)
MAKE_OBJ = 0x1E  # 创建结构体实例(操作数 = 类型名常量索引<<16 | 字段数)
GET_FIELD = 0x1F  # 读字段(操作数 = 字段索引)
SET_FIELD = 0x20  # 写字段(操作数 = 字段索引;栈 [obj, value] -> 弹两者写字段)
CALL_NAME = 0x21  # 动态按名调用(无操作数;栈 [args..., name, argc])

# 助记符(同时作为汇编文本格式中的助记符)
MNEMONICS = {
    NOP: "NOP", PUSH: "PUSH", LOAD: "LOAD", STORE: "STORE",
    ADD: "ADD", SUB: "SUB", MUL: "MUL", DIV: "DIV", MOD: "MOD",
    NEG: "NEG", EQ: "EQ", NE: "NE", LT: "LT", LE: "LE",
    GT: "GT", GE: "GE", AND: "AND", OR: "OR", NOT: "NOT",
    JMP: "JMP", JZ: "JZ", JNZ: "JNZ", CALL: "CALL", RET: "RET",
    PRINT: "PRINT", POP: "POP", DUP: "DUP", HALT: "HALT",
    NCALL: "NCALL", CONCAT: "CONCAT",
    MAKE_OBJ: "MAKE_OBJ", GET_FIELD: "GET_FIELD", SET_FIELD: "SET_FIELD",
    CALL_NAME: "CALL_NAME",
}
MNEMONIC_TO_OP = {name: op for op, name in MNEMONICS.items()}

# 哪些操作码携带一个 u32 操作数
HAS_OPERAND = frozenset({PUSH, LOAD, STORE, JMP, JZ, JNZ, CALL, NCALL,
                         MAKE_OBJ, GET_FIELD, SET_FIELD})


def instruction_size(op: int) -> int:
    """返回某操作码的指令字节数(1 或 5)。"""
    return 5 if op in HAS_OPERAND else 1
