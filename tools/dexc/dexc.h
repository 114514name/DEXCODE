/* ============================================================================
 * dexc.h — DEXCODE 纯 C 工具链(dexc.exe)的内部接口。
 *
 * 为什么存在:语言前端一直是 Python(dexlang 包的 14 个模块),于是「编译 .dex」
 * 这件事必须依赖 Python。游戏运行端早已是 vm.exe + libdexgame.dll(零 Python),
 * 只有编译这一步还拖着 Python。dexc 把 lexer/parser/compiler/assembler/
 * disassembler/asmtext/.dexdef 解析全部搬到 C,让整条链彻底无 Python。
 *
 * 与 dexlang 包的关系:**行为必须逐字节一致**。判定标准不是"看起来对",
 * 而是 tests/test_dexc.py 用同一份语料分别跑两边,比较
 *   ① .dexbc 字节 ② .dxasm 文本 ③ 错误信息 ④ 警告
 * 逐字节相同。因此本文件里凡是"顺手改好一点"的地方都必须显式标注 DXDIFF。
 *
 * 内存策略:全进程只分配、不释放(编译一次就跑完退出)。错误用 longjmp 抛到
 * main 统一打印 —— 对应 Python 的 DexError 异常。
 * ==========================================================================*/
#ifndef DEXC_H
#define DEXC_H

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

/* ---------------------------------------------------------------- 基础工具 */

void *dx_malloc(size_t n);
void *dx_realloc(void *p, size_t n);
char *dx_strdup(const char *s);
char *dx_strndup(const char *s, size_t n);
char *dx_aprintf(const char *fmt, ...);

/* 动态数组:push 时按 2 倍扩容。用宏而不是泛型,和仓库里其他 C 代码的朴素风格一致。 */
#define DXV_PUSH(arr, len, cap, val)                                            \
    do {                                                                        \
        if ((len) == (cap)) {                                                   \
            (cap) = (cap) ? (cap) * 2 : 8;                                      \
            (arr) = dx_realloc((arr), (size_t)(cap) * sizeof(*(arr)));          \
        }                                                                       \
        (arr)[(len)++] = (val);                                                 \
    } while (0)

/* ---------------------------------------------------------------- 错误抛出 */

/* 对应 dexlang/errors.py:DexError(message, line, col, phase)。
 * 打印格式也一致:"[phase]error at line:col: message"(line 为 0/未设时省略定位)。*/
void dx_throw(const char *phase, int line, int col, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5), noreturn))
#endif
    ;

extern char dx_err_msg[4096];
extern int dx_err_line, dx_err_col;
extern const char *dx_err_phase;

/* main 里 setjmp 一次,dx_throw 负责 longjmp 回来(对应 Python 的 DexError) */
extern jmp_buf dx_jmp_buf;
static inline jmp_buf *dx_jmp_target(void) { return &dx_jmp_buf; }

/* ------------------------------------------------- Python 兼容的文本格式化 */

/* 错误信息里到处是 Python 的 {x!r}(repr),必须逐字节一致:引号选择、控制字符
 * 转义、非 ASCII 原样输出。 */
char *dx_py_repr_str(const char *s);
/* repr(float):最短往返十进制 + Python 的"何时用科学计数法"规则
 * (指数 < -4 或 >= 16 用科学计数法)。.dxasm 里的浮点常量文本也靠它。 */
char *dx_py_repr_float(double v);

/* ------------------------------------------------------------------ 路径层 */

/* ntpath/posixpath 语义(Windows 上按 ntpath:反斜杠分隔、大小写不敏感的比较由
 * 调用方处理)。字节码常量池里存的是绝对路径,必须与 Python 拼出来的一模一样。 */
char *dx_path_dirname(const char *p);
char *dx_path_join(const char *a, const char *b);
char *dx_path_abspath(const char *p);
char *dx_path_normpath(const char *p);
int dx_path_isabs(const char *p);
int dx_path_exists(const char *p);
int dx_path_isdir(const char *p);
char *dx_path_strip_ext(const char *p);          /* os.path.splitext(p)[0] */
char *dx_path_abspath_noext(const char *p);      /* splitext(abspath(p))[0] */
char *dx_path_basename(const char *p);
char **dx_listdir(const char *dir, int *out_n);  /* 已排序(等价 sorted(os.listdir)) */
char *dx_read_file(const char *path, size_t *out_len);   /* utf-8-sig:去 BOM */

/* ============================================================== 词法 / Token */

enum {
    TK_INT = 0, TK_FLOAT, TK_STRING, TK_IDENT,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_COMMA, TK_SEMI,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_EQEQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_AND, TK_OR, TK_BANG,
    TK_ARROW, TK_COLON, TK_DOT,
    TK_LET, TK_IF, TK_ELSE, TK_WHILE, TK_FUNC, TK_RETURN, TK_PRINT,
    TK_TRUE, TK_FALSE, TK_INCLUDE, TK_REFER, TK_EXTERN, TK_RELEASE, TK_TYPE,
    TK_EOF
};

typedef struct {
    int kind;
    char *lexeme;
    int line, col;
    /* 字面量负载(按 kind 取用) */
    int64_t ival;
    double fval;
    char *sval;          /* STRING:已解转义的正文 */
} Token;

Token *dx_lex(const char *src, const char *filename, int *out_n);

/* ===================================================================== AST */

enum {
    N_LET = 0, N_ASSIGN, N_PRINT, N_IF, N_WHILE, N_FUNC, N_RETURN, N_EXPRSTMT,
    N_LITERAL, N_NAME, N_BINOP, N_UNARYOP, N_CALL, N_LIBREF, N_TYPEDEF,
    N_STRUCTLIT, N_GETFIELD, N_SETFIELD, N_PROGRAM
};

enum { LIT_INT = 0, LIT_FLOAT, LIT_STR };

typedef struct Node Node;

typedef struct {
    char *name;
    Node *value;          /* StructLit 用 */
    char *type_name;      /* TypeDef 用 */
} NameVal;

typedef struct {
    char **items;
    int len, cap;
} StrList;

typedef struct {
    Node **items;
    int len, cap;
} NodeList;

struct Node {
    int kind;
    int line, col;
    char *name;           /* Let/Assign/Name/Func/Call/LibRef/TypeDef/StructLit/GetField/SetField */
    char *sval;           /* LibRef.target、Func.ret_type、Literal 字符串 */
    int64_t ival;         /* Literal int */
    double fval;          /* Literal float */
    int lit_kind;         /* Literal */
    Node *value;          /* Let/Assign/Return/SetField.value、UnaryOp.operand、If.cond、While.cond */
    Node *obj;            /* GetField/SetField */
    Node *left, *right;   /* BinOp */
    char *op;             /* BinOp/UnaryOp:'+','&&','!'... */
    NodeList stmts;       /* Program、If.then、While.body、Func.body */
    NodeList els;         /* If.els(Python 里空列表是假值,所以判空要判 len) */
    NodeList exprs;       /* Print */
    NodeList args;        /* Call */
    StrList params;       /* Func */
    StrList param_types;  /* Func(与 params 对齐,空串 = 未标注) */
    NameVal *fields;      /* TypeDef/StructLit */
    int nfields, capfields;
};

typedef struct {
    NodeList stmts;
} Program;

Node *dx_parse(const Token *toks, int ntoks, const char *filename);

/* ============================================================== IR(汇编) */

enum {
    OA_NONE = 0, OA_CONST_I, OA_CONST_F, OA_CONST_S,
    OA_LOCAL, OA_LABEL, OA_FUNC, OA_NATIVE, OA_TYPE, OA_FIELD
};

typedef struct {
    int kind;
    int64_t ival;
    double fval;
    char *s;          /* 字符串常量 / 标签名 / 函数名 / 类型名 */
    int index;        /* LOCAL 槽号 / FIELD 字段号 */
    int nfields;      /* TYPE */
} Operand;

typedef struct {
    int op;           /* -1 = 标签定义(伪指令,零字节) */
    Operand operand;
    char *label;      /* op == -1 时的标签名 */
    int line;
} Insn;

typedef struct {
    char *name;
    int arity;
    int nlocals;
    Insn *insns;
    int ninsns, capinsns;
} AsmFunc;

enum { ABI_DIRECT = 0, ABI_VALUE_ARRAY = 1 };
#define DX_MAX_NATIVE_ARITY 3
#define DX_MAX_NATIVE_ARGS  16

typedef struct {
    char *name;
    char *lib;                        /* 与 libs[].path 相同的字符串 */
    int param_types[DX_MAX_NATIVE_ARGS];
    int nparams;
    int ret_type;
    int abi;                          /* ABI_DIRECT / ABI_VALUE_ARRAY */
} NativeFunc;

typedef struct {
    char *path;
    int is_static;
    unsigned char *data;
    size_t data_len;
    char *release_name;               /* 空 = 无 */
} LibInfo;

typedef struct {
    AsmFunc *funcs;
    int nfuncs, capfuncs;
    NativeFunc *natives;
    int nnatives, capnatives;
    LibInfo *libs;
    int nlibs, caplibs;
} AssemblyProgram;

/* 操作码(dexlang/opcodes.py 的 C 侧副本,两边必须一致) */
enum {
    OP_NOP = 0x00, OP_PUSH = 0x01, OP_LOAD = 0x02, OP_STORE = 0x03,
    OP_ADD = 0x04, OP_SUB = 0x05, OP_MUL = 0x06, OP_DIV = 0x07,
    OP_MOD = 0x08, OP_NEG = 0x09, OP_EQ = 0x0A, OP_NE = 0x0B,
    OP_LT = 0x0C, OP_LE = 0x0D, OP_GT = 0x0E, OP_GE = 0x0F,
    OP_AND = 0x10, OP_OR = 0x11, OP_NOT = 0x12, OP_JMP = 0x13,
    OP_JZ = 0x14, OP_JNZ = 0x15, OP_CALL = 0x16, OP_RET = 0x17,
    OP_PRINT = 0x18, OP_POP = 0x19, OP_DUP = 0x1A, OP_HALT = 0x1B,
    OP_NCALL = 0x1C, OP_CONCAT = 0x1D, OP_MAKE_OBJ = 0x1E,
    OP_GET_FIELD = 0x1F, OP_SETFIELD = 0x20, OP_CALL_NAME = 0x21
};

#define DEXC_MAGIC "DEXC"
#define DEXC_VERSION 3
#define DEXC_HEADER_SIZE 18
#define DEXC_FUNC_ENTRY_SIZE 13
#define DEXC_LIB_ENTRY_SIZE 7
#define DEXC_LIB_STATIC 0x01
#define DEXC_NATIVE_FIXED_SIZE 6
#define DEXC_NATIVE_ABI_MASK 0x80

#define NAT_VOID 0
#define NAT_INT 1
#define NAT_FLOAT 2
#define NAT_STR 3
#define DEXC_TAG_INT 0
#define DEXC_TAG_FLOAT 1
#define DEXC_TAG_STRING 2

const char *dx_mnemonic(int op);
int dx_has_operand(int op);
int dx_instruction_size(int op);

/* ======================================================= .dexdef 定义文件 */

typedef struct {
    char *name;
    int param_types[DX_MAX_NATIVE_ARGS];
    int nparams;
    int ret_type;
} NativeDecl;

typedef struct {
    char *lib_path;                   /* refer 指向的库路径(可能是相对路径) */
    int is_static;
    NativeDecl *natives;
    int nnatives, capnatives;
    char *release;
    int abi;                          /* ABI_DIRECT / ABI_VALUE_ARRAY */
} DefFile;

DefFile *dx_parse_def(const char *text, const char *filename);
/* 'ii:i' → param_types/ret_type。max_arity < 0 表示用直接 ABI 的上限。 */
void dx_parse_sig(const char *sig, int max_arity, int *param_types, int *nparams,
                  int *ret_type);

/* ================================================= 编译(含 include/refer) */

/* 一张朴素的键值表(键是字符串)。用它表达 Python 版里的一堆 dict:
 * func_index / native_index / lib_index / types / var_types / locals …
 * 查找是线性扫描 —— 编译器面对的量级(函数几十个、原生几百个、局部变量几十个)
 * 下完全够用,换来的是不必引哈希表。 */
typedef struct {
    char *key;
    char *sval;       /* 字符串值(类型名、释放函数名…) */
    Node *node;
    int ival;
    int present;      /* Python 里"键存在但值是 None"与"键不存在"不同,必须区分 */
} MapEntry;

typedef struct {
    MapEntry *e;
    int n, cap;
} Map;

MapEntry *dx_map_get(Map *m, const char *key);   /* 不存在返回 NULL */
MapEntry *dx_map_set(Map *m, const char *key);   /* 不存在则新建(present = 1) */

typedef struct {
    AssemblyProgram prog;        /* funcs / natives / libs(顺序即索引) */
    Map func_index;              /* 函数名 → ival */
    Map native_index;            /* 原生函数名 → ival */
    Map lib_index;               /* 库路径 → ival */
    Map loaded_defs;             /* 已加载过的定义文件路径(去重用) */
    Map types;                   /* 类型名 → node(该 TypeDef 节点,字段表在 node->fields) */
    Map type_order;              /* 类型名 → ival(声明顺序) */
    Map lib_release;             /* 库路径 → sval(本库的字符串释放函数名) */
    Map func_ret_types;          /* 函数名 → sval(返回类型,空串 = 未知) */
    Map func_nodes;              /* 函数名 → node(Func 节点) */
    Node **module_funcs;         /* 语言模块(.dex)里收集到的函数声明,按加载顺序 */
    int n_modf, cap_modf;
    char **warnings;
    int n_warn, cap_warn;
} CompileUnit;

CompileUnit *dx_compile(Node *program, const char *source_path,
                        char **include_dirs, int n_include_dirs, int rel_lib);

/* ============================================ 汇编 / 反汇编 / 汇编文本 */

unsigned char *dx_assemble(CompileUnit *unit, size_t *out_len);
char *dx_render_asm(const AssemblyProgram *prog);
AssemblyProgram *dx_parse_asm_text(const char *text);
AssemblyProgram *dx_decode(const unsigned char *data, size_t len);
char *dx_disassemble(const unsigned char *data, size_t len);

#endif /* DEXC_H */
