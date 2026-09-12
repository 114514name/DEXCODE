/*
 * vm.c — DEXC 字节码解释器
 *
 * 读取 .dexbc 文件(格式见 docs/SPEC.md),在栈式虚拟机中执行。
 * 支持通过 include/refer 加载 DLL/so 原生函数(NCALL 指令)。
 *
 * 用法: vm <file.dexbc>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>   /* offsetof:值数组 ABI 的 DexValue 布局静态断言用 */
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define GETPID _getpid
#else
#include <dlfcn.h>
#include <unistd.h>
#define GETPID getpid
#endif

/* ============================================================
 * 统一堆入口(P0)与内存预算(P1)
 *
 * 背景:此前 21 处堆分配全部直接调 malloc/calloc。
 *  - 没有单一入口,任何回收策略(引用计数/清扫)都无从插入;
 *  - 也没有任何"用了多少内存"的可观测性,累加器模式的 O(n²) 增长
 *    只能等进程被系统杀掉才发现。
 *
 * 本层做两件事:
 *  1. 所有 VM 堆分配改走 vm_alloc/vm_calloc/vm_free,字节数进入计数器;
 *  2. 超过预算时以明确错误退出,并指出最常见的原因。
 *     预算由环境变量 DEXCODE_MAX_MEM_MB 指定,0=不限(默认)。
 *
 * 计数器只统计经过本层的分配,因此开销是一次整数加法,与 GC 无关。
 * ============================================================ */
#define VM_ALLOC_MAGIC 0x564D414Cu   /* 'VMAL' */

static size_t g_mem_used = 0;        /* 当前存活的 VM 堆字节数 */
static size_t g_mem_peak = 0;        /* 峰值(诊断用)       */
static int64_t g_mem_budget = -1;    /* 字节;0 = 不限; -1 = 尚未解析 */
static int g_mem_over = 0;           /* 是否已就超限报错(防重复) */

static void pump_mem_budget(void) {
    if (g_mem_budget >= 0) return;
    g_mem_budget = 0;                /* 默认不限,保证不干扰既有程序 */
    const char *s = getenv("DEXCODE_MAX_MEM_MB");
    if (s && *s) {
        long long mb = atoll(s);
        if (mb > 0) {
            /* 上限保护:避免 MB 换算在 size_t 上溢出 */
            if (mb > (long long)(SIZE_MAX / (1024 * 1024)))
                g_mem_budget = (int64_t)SIZE_MAX;
            else
                g_mem_budget = (int64_t)((unsigned long long)mb * 1024ULL * 1024ULL);
        }
    }
}

static void mem_note_oom(void) {
    if (g_mem_over) return;
    g_mem_over = 1;
    fflush(stdout);
    fprintf(stderr,
        "\nruntime error: 内存用量超过预算上限 (%lld MB,当前已用 %lld MB)\n"
        "\n"
        "  最常见的两种原因:\n"
        "    - 循环内字符串累加,例如  while ... { s = s + \"x\"; }\n"
        "      每轮都会产生新字符串而旧值不会被回收,是 O(n^2) 增长;\n"
        "    - 大循环里反复构造临时字符串用于 print / 拼接。\n"
        "\n"
        "  说明:当前 VM 不回收运行期产生的字符串与结构体(见 README「已知限制」),\n"
        "        因此长时间运行的程序应避免在循环里无限累积字符串。\n"
        "  调整上限:设置环境变量 DEXCODE_MAX_MEM_MB(0 = 不限制).\n",
        (long long)(g_mem_budget / (1024 * 1024)),
        (long long)(g_mem_used / (1024 * 1024)));
    exit(1);
}

static void *vm_alloc(size_t n) {
    pump_mem_budget();
    if (n == 0) n = 1;
    if (g_mem_budget > 0 && g_mem_used + n > (size_t)g_mem_budget) mem_note_oom();
    unsigned char *raw = (unsigned char *)malloc(sizeof(size_t) + n);
    if (!raw) {
        fprintf(stderr, "runtime error: out of memory (requested %llu bytes)\n",
                (unsigned long long)n);
        exit(1);
    }
    *(size_t *)raw = n;
    g_mem_used += n;
    if (g_mem_used > g_mem_peak) g_mem_peak = g_mem_used;
    return raw + sizeof(size_t);
}

static void *vm_calloc(size_t count, size_t size) {
    size_t n = count * size;               /* 调用方保证不溢出 */
    void *p = vm_alloc(n);
    memset(p, 0, n);
    return p;
}

static void vm_free(void *p) {
    if (!p) return;
    unsigned char *raw = (unsigned char *)p - sizeof(size_t);
    size_t n = *(size_t *)raw;
    if (n <= g_mem_used) g_mem_used -= n; else g_mem_used = 0;
    free(raw);
}

/* ---------- 字节码格式常量(与 dexlang/opcodes.py 一致) ---------- */
#define MAGIC0 'D'
#define MAGIC1 'E'
#define MAGIC2 'X'
#define MAGIC3 'C'
#define VERSION 3

#define HEADER_SIZE 18
#define FUNC_ENTRY_SIZE 13
#define LIB_ENTRY_SIZE 7   /* path_idx(2) + flags(1) + data_len(4) */
#define NATIVE_FIXED_SIZE 6

/* 库表 flags 位 */
#define LIB_STATIC 0x01    /* 库字节内嵌在字节码中(静态链接) */

#define TAG_INT    0
#define TAG_FLOAT  1
#define TAG_STRING 2

/* 原生函数类型码 */
#define NAT_VOID  0
#define NAT_INT   1
#define NAT_FLOAT 2
#define NAT_STR   3

enum {
    OP_NOP = 0x00, OP_PUSH = 0x01, OP_LOAD = 0x02, OP_STORE = 0x03,
    OP_ADD = 0x04, OP_SUB = 0x05, OP_MUL = 0x06, OP_DIV = 0x07,
    OP_MOD = 0x08, OP_NEG = 0x09, OP_EQ = 0x0A, OP_NE = 0x0B,
    OP_LT = 0x0C, OP_LE = 0x0D, OP_GT = 0x0E, OP_GE = 0x0F,
    OP_AND = 0x10, OP_OR = 0x11, OP_NOT = 0x12, OP_JMP = 0x13,
    OP_JZ = 0x14, OP_JNZ = 0x15, OP_CALL = 0x16, OP_RET = 0x17,
    OP_PRINT = 0x18, OP_POP = 0x19, OP_DUP = 0x1A, OP_HALT = 0x1B,
    OP_NCALL = 0x1C,
    OP_CONCAT = 0x1D,
    OP_MAKE_OBJ = 0x1E, OP_GET_FIELD = 0x1F, OP_SET_FIELD = 0x20,
    OP_CALL_NAME = 0x21
};

#define MAX_STACK  (1u << 20)
#define MAX_FRAMES (1u << 16)
#define MAX_NATIVE_ARGS 16

/* 原生函数表里 ret_type 字节的高位标记调用约定(见 docs/SPEC.md 4.5)。
   ret_type 的有效值只有 0..3,高位是空的;旧字节码该位恒为 0,故新旧 VM 对
   老字节码的解析结果一致 —— 不需要改格式、不需要升版号(本文件的版本检查是
   严格相等,升版号会让所有现存 .dexbc 立刻失效)。 */
#define NATIVE_ABI_MASK  0x80
#define NATIVE_ABI_ARRAY 0x80

/* 值数组 ABI 里每个元素的标签。**必须与下面的 VType 枚举同值**。 */
#define DEXV_INT   0
#define DEXV_FLOAT 1
#define DEXV_STR   2
#define DEXV_OBJ   3   /* 预留:结构体/引用(尚未实现) */

/* ---------- 运行时值 ---------- */
typedef enum { V_INT, V_FLOAT, V_STR, V_OBJ } VType;

typedef struct {
    VType type;
    union {
        int64_t i;
        double f;
        char *p; /* 字符串值直接指向常量池字符串 */
        void *o; /* V_OBJ:指向 Obj */
    } as;
} Value;

/* 结构体实例(MAKE_OBJ 创建;字段数组在堆上,引用语义) */
typedef struct {
    const char *type_name;  /* 类型名(指向常量池字符串) */
    uint32_t nfields;
    Value *fields;
} Obj;

/* ---------- 值数组 ABI 的公开元素类型(见 docs/SPEC.md 4.5) ----------
   声明了 `abi value_array;` 的原生库,其导出函数签名为:

       <ret> name(const DexValue *args, int argc);

   VM 把操作数栈上的实参原样传过去,不做任何编组(栈上的 Value 与 DexValue
   布局相同)。原生侧只应依赖本类型,不应依赖 VM 内部的 Value。
   返回字符串仍按既有契约:视为**借用**,VM 立即复制一份自己的副本。 */
typedef struct {
    uint32_t tag;                /* DEXV_INT / DEXV_FLOAT / DEXV_STR */
    union {
        int64_t i;
        double f;
        const char *s;
    } as;
} DexValue;

typedef int64_t (*DexNativeArrayI)(const DexValue *args, int argc);
typedef double (*DexNativeArrayF)(const DexValue *args, int argc);
typedef const char *(*DexNativeArrayS)(const DexValue *args, int argc);
typedef void (*DexNativeArrayV)(const DexValue *args, int argc);

/* 守住宿主:标签顺序与结构布局一旦漂移,这里立刻编译失败。 */
_Static_assert(V_INT == DEXV_INT && V_FLOAT == DEXV_FLOAT && V_STR == DEXV_STR,
               "DexValue tags must match the VM VType enum order");
_Static_assert(sizeof(DexValue) == sizeof(Value),
               "DexValue must have the same size as the VM Value");
_Static_assert(offsetof(DexValue, as) == offsetof(Value, as),
               "DexValue.as must be at the same offset as Value.as");

/* ---------- 程序结构 ---------- */
typedef struct {
    uint16_t name_idx;
    uint8_t  arity;
    uint16_t nlocals;
    uint32_t code_off;
    uint32_t code_len;
    const char *name;
} Func;

typedef struct {
    const char *path;      /* 库路径(指向常量池字符串) */
    int is_static;         /* 1 = 静态内嵌,运行时从临时文件加载 */
    char *temp_path;       /* 静态库解包出的临时文件路径(非静态为 NULL) */
    void *handle;          /* 已加载的库句柄,未加载为 NULL */
    void *release_fn;      /* 本库的字符串释放函数 (string)->void,未声明为 NULL */
} Lib;

typedef struct {
    uint16_t name_idx;
    uint16_t lib_idx;
    uint8_t  arity;
    uint8_t  ret_type;
    int      is_array_abi;  /* 1 = 值数组 ABI(收 DexValue[]);0 = 直接 ABI */
    uint8_t  param_types[MAX_NATIVE_ARGS];
    void *fn;              /* 已解析的函数指针,未解析为 NULL */
    const char *name;
} Native;

typedef struct {
    Value *consts;
    size_t nconsts;
    const char **names;  /* 第 i 个常量为字符串时为其内容,否则 NULL */
    Func *funcs;
    size_t nfuncs;
    Lib *libs;
    size_t nlibs;
    Native *natives;
    size_t nnatives;
    uint8_t *code;
    size_t code_size;
} Program;

static void free_program(Program *prog);  /* 前向声明(die 也用它清理临时文件) */

/* 调用帧:局部变量数组 + 栈基址 + 返回地址 + 函数信息 */
typedef struct {
    Value *locals;
    uint32_t nlocals;
    size_t base;
    uint32_t ret_pc;
    uint16_t func_idx;
} Frame;

static void die(const Program *prog, const Frame *fr, uint32_t pc,
                const char *fmt, ...);

/* ---------- 运行时错误 ---------- */
static void die(const Program *prog, const Frame *fr, uint32_t pc,
                const char *fmt, ...) {
    fflush(stdout);
    const char *fname = (prog && fr && fr->func_idx < prog->nfuncs)
                            ? prog->funcs[fr->func_idx].name
                            : "?";
    fprintf(stderr, "runtime error in function '%s' at pc=%u: ",
            fname ? fname : "?", pc);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    /* 运行期错误退出前也释放程序资源(含静态库临时文件,避免残留) */
    if (prog) free_program((Program *)prog);
    exit(1);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------- 程序加载 ---------- */

/* 该操作码是否携带一个 u32 操作数(与 dexlang/opcodes.py 的 HAS_OPERAND 一致)。 */
static int opcode_has_operand(uint8_t op) {
    switch (op) {
    case OP_PUSH: case OP_LOAD: case OP_STORE:
    case OP_JMP: case OP_JZ: case OP_JNZ:
    case OP_CALL: case OP_NCALL:
    case OP_MAKE_OBJ: case OP_GET_FIELD: case OP_SET_FIELD:
        return 1;
    default:
        return 0;
    }
}

/* 生成一个不可预测的临时库文件路径并安全创建(空)文件。
 *
 * 旧实现用固定的 "<temp>/dexvm_<pid>_<idx>.dll" 加普通 fopen("wb") 写入,
 * 在共享 /tmp 上可被预先放置的符号链接劫持(任意文件覆盖),
 * 并在"写入"与"dlopen 加载"之间存在 TOCTOU 替换窗口。
 * 现在改用系统提供的原子唯一命名(O_EXCL / GetTempFileName),
 * 调用方通过已建立的句柄写入,攻击者无法预置该路径。
 * 返回 malloc 的路径(成功)或 NULL(失败)。 */
static char *create_temp_lib(unsigned idx) {
    (void)idx;   /* 唯一性由系统 API 保证,不使用索引 */
#ifdef _WIN32
    char tmpdir[MAX_PATH], tmplib[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmpdir) == 0) return NULL;
    /* GetTempFileNameA 原子地创建唯一文件,不会覆盖已存在的路径。
       uUnique=0 时由系统按当前时间+计数器生成,不使用 idx。 */
    if (GetTempFileNameA(tmpdir, "dxv", 0, tmplib) == 0) return NULL;
    size_t n = strlen(tmplib) + 1;
    char *path = malloc(n);
    if (!path) { DeleteFileA(tmplib); return NULL; }
    memcpy(path, tmplib, n);
    return path;
#else
    /* /tmp 优先,其次 TMPDIR;mkstemp 以 O_CREAT|O_EXCL 打开,天然防止符号链接攻击 */
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    size_t n = strlen(dir) + 32;
    char *path = malloc(n);
    if (!path) return NULL;
    snprintf(path, n, "%s/dexvm_XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    close(fd);
    return path;
#endif
}

/* 把静态库字节写入"已安全创建"的临时文件,返回 0 成功 / 1 失败。 */
static int write_temp_lib(const char *path, const uint8_t *bytes, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    if (fwrite(bytes, 1, len, f) != len) { fclose(f); remove(path); return 1; }
    if (fclose(f) != 0) { remove(path); return 1; }
    return 0;
}

static void free_program(Program *prog) {
    if (!prog) return;
    if (prog->consts) {
        for (size_t i = 0; i < prog->nconsts; i++) {
            if (prog->consts[i].type == V_STR) vm_free(prog->consts[i].as.p);
        }
        vm_free(prog->consts);
    }
    vm_free(prog->names);
    vm_free(prog->funcs);
    if (prog->libs) {
        for (size_t i = 0; i < prog->nlibs; i++) {
            Lib *lib = &prog->libs[i];
            if (lib->is_static) {
                if (lib->handle) {
#ifdef _WIN32
                    FreeLibrary((HMODULE)lib->handle);
#else
                    dlclose(lib->handle);
#endif
                }
                if (lib->temp_path) {
                    remove(lib->temp_path);   /* 清理临时文件 */
                    free(lib->temp_path);
                }
            }
        }
        vm_free(prog->libs);
    }
    vm_free(prog->natives);
    vm_free(prog->code);
    vm_free(prog);
}

static Program *load_program(const uint8_t *data, size_t n) {
    Program *prog = vm_calloc(1, sizeof(Program));
    if (!prog) return NULL;

    if (n < HEADER_SIZE ||
        data[0] != MAGIC0 || data[1] != MAGIC1 ||
        data[2] != MAGIC2 || data[3] != MAGIC3) {
        fprintf(stderr, "error: not a valid DEXC bytecode file\n");
        free_program(prog);
        return NULL;
    }
    if (data[4] != VERSION) {
        fprintf(stderr, "error: unsupported bytecode version %d\n", data[4]);
        free_program(prog);
        return NULL;
    }

    size_t off = 6;
    uint16_t n_consts = (uint16_t)(data[off] | (data[off + 1] << 8)); off += 2;
    uint16_t n_funcs = (uint16_t)(data[off] | (data[off + 1] << 8)); off += 2;
    uint16_t n_libs = (uint16_t)(data[off] | (data[off + 1] << 8)); off += 2;
    uint16_t n_natives = (uint16_t)(data[off] | (data[off + 1] << 8)); off += 2;
    uint32_t code_size = rd32(data + off); off += 4;

    /* 常量池 */
    prog->nconsts = n_consts;
    prog->consts = vm_calloc(n_consts ? n_consts : 1, sizeof(Value));
    prog->names = vm_calloc(n_consts ? n_consts : 1, sizeof(char *));
    if (!prog->consts || !prog->names) { free_program(prog); return NULL; }

    for (uint16_t i = 0; i < n_consts; i++) {
        if (off >= n) { fprintf(stderr, "error: truncated constant pool\n"); free_program(prog); return NULL; }
        uint8_t tag = data[off++];
        if (tag == TAG_INT) {
            if (off + 8 > n) { free_program(prog); return NULL; }
            int64_t v;
            memcpy(&v, data + off, 8); off += 8;
            prog->consts[i].type = V_INT;
            prog->consts[i].as.i = v;
        } else if (tag == TAG_FLOAT) {
            if (off + 8 > n) { free_program(prog); return NULL; }
            double v;
            memcpy(&v, data + off, 8); off += 8;
            prog->consts[i].type = V_FLOAT;
            prog->consts[i].as.f = v;
        } else if (tag == TAG_STRING) {
            if (off + 2 > n) { free_program(prog); return NULL; }
            uint16_t len = (uint16_t)(data[off] | (data[off + 1] << 8)); off += 2;
            if (off + len > n) { free_program(prog); return NULL; }
            char *s = vm_alloc((size_t)len + 1);
            if (!s) { free_program(prog); return NULL; }
            memcpy(s, data + off, len);
            s[len] = '\0';
            off += len;
            prog->consts[i].type = V_STR;
            prog->consts[i].as.p = s;
            prog->names[i] = s;
        } else {
            fprintf(stderr, "error: bad constant tag %d\n", tag);
            free_program(prog);
            return NULL;
        }
    }

    /* 库表 */
    prog->nlibs = n_libs;
    prog->libs = vm_calloc(n_libs ? n_libs : 1, sizeof(Lib));
    if (!prog->libs) { free_program(prog); return NULL; }
    for (uint16_t i = 0; i < n_libs; i++) {
        if (off + LIB_ENTRY_SIZE > n) {
            fprintf(stderr, "error: truncated library table\n");
            free_program(prog);
            return NULL;
        }
        uint16_t path_idx = (uint16_t)(data[off] | (data[off + 1] << 8)); off += 2;
        uint8_t flags = data[off++];
        uint32_t data_len = rd32(data + off); off += 4;
        if (off + data_len > n) {
            fprintf(stderr, "error: truncated static library data\n");
            free_program(prog);
            return NULL;
        }
        if (path_idx >= n_consts || prog->names[path_idx] == NULL) {
            fprintf(stderr, "error: bad library path index %u\n", path_idx);
            free_program(prog);
            return NULL;
        }
        prog->libs[i].path = prog->names[path_idx];
        prog->libs[i].is_static = (flags & LIB_STATIC) != 0;
        prog->libs[i].temp_path = NULL;
        prog->libs[i].handle = NULL;   /* 惰性加载 */
        if (prog->libs[i].is_static) {
            if (data_len == 0) {
                fprintf(stderr, "error: static library %u has no embedded data\n", i);
                free_program(prog);
                return NULL;
            }
            prog->libs[i].temp_path = create_temp_lib(i);
            if (!prog->libs[i].temp_path ||
                write_temp_lib(prog->libs[i].temp_path, data + off, data_len) != 0) {
                fprintf(stderr, "error: cannot extract static library %u to temp file\n", i);
                free_program(prog);
                return NULL;
            }
        }
        off += data_len;
    }

    /* 原生函数表 */
    prog->nnatives = n_natives;
    prog->natives = vm_calloc(n_natives ? n_natives : 1, sizeof(Native));
    if (!prog->natives) { free_program(prog); return NULL; }
    for (uint16_t i = 0; i < n_natives; i++) {
        if (off + NATIVE_FIXED_SIZE > n) {
            fprintf(stderr, "error: truncated native function table\n");
            free_program(prog);
            return NULL;
        }
        uint16_t name_idx = (uint16_t)(data[off] | (data[off + 1] << 8));
        uint16_t lib_idx = (uint16_t)(data[off + 2] | (data[off + 3] << 8));
        uint8_t arity = data[off + 4];
        uint8_t ret_raw = data[off + 5];
        /* ret_type 的高位是调用约定标记,低 7 位才是返回类型 */
        int is_array_abi = (ret_raw & NATIVE_ABI_MASK) != 0;
        uint8_t ret_type = (uint8_t)(ret_raw & ~NATIVE_ABI_MASK);
        off += NATIVE_FIXED_SIZE;
        if (off + arity > n) {
            fprintf(stderr, "error: truncated native parameter types\n");
            free_program(prog);
            return NULL;
        }
        if (arity > MAX_NATIVE_ARGS) {
            fprintf(stderr, "error: native arity %u too large (max %d)\n", arity, MAX_NATIVE_ARGS);
            free_program(prog);
            return NULL;
        }
        if (name_idx >= n_consts || prog->names[name_idx] == NULL) {
            fprintf(stderr, "error: bad native name index %u\n", name_idx);
            free_program(prog);
            return NULL;
        }
        if (lib_idx >= n_libs) {
            fprintf(stderr, "error: native references unknown library %u\n", lib_idx);
            free_program(prog);
            return NULL;
        }
        prog->natives[i].name_idx = name_idx;
        prog->natives[i].lib_idx = lib_idx;
        prog->natives[i].arity = arity;
        prog->natives[i].ret_type = ret_type;
        prog->natives[i].is_array_abi = is_array_abi;
        for (uint8_t k = 0; k < arity; k++) {
            prog->natives[i].param_types[k] = data[off++];
        }
        prog->natives[i].fn = NULL;   /* 惰性解析符号 */
        prog->natives[i].name = prog->names[name_idx];
    }

    /* 函数表 */
    prog->nfuncs = n_funcs;
    prog->funcs = vm_calloc(n_funcs ? n_funcs : 1, sizeof(Func));
    if (!prog->funcs) { free_program(prog); return NULL; }

    for (uint16_t i = 0; i < n_funcs; i++) {
        if (off + FUNC_ENTRY_SIZE > n) {
            fprintf(stderr, "error: truncated function table\n");
            free_program(prog);
            return NULL;
        }
        uint16_t name_idx = (uint16_t)(data[off] | (data[off + 1] << 8));
        uint8_t arity = data[off + 2];
        uint16_t nlocals = (uint16_t)(data[off + 3] | (data[off + 4] << 8));
        uint32_t code_off = rd32(data + off + 5);
        uint32_t code_len = rd32(data + off + 9);
        off += FUNC_ENTRY_SIZE;

        if (name_idx >= n_consts || prog->names[name_idx] == NULL) {
            fprintf(stderr, "error: bad function name index %u\n", name_idx);
            free_program(prog);
            return NULL;
        }
        /* 实参复制进局部变量槽,故 nlocals 必须容得下 arity:
           否则 calloc(nlocals) 之后会按 arity 越界写堆(恶意字节码可触发堆破坏)。
           编译器生成的字节码恒满足 nlocals >= arity(compiler.py 的 declare 分配)。 */
        if (nlocals < arity) {
            fprintf(stderr,
                    "error: function '%s' has nlocals=%u < arity=%u (would overflow locals)\n",
                    prog->names[name_idx], nlocals, arity);
            free_program(prog);
            return NULL;
        }
        prog->funcs[i].name_idx = name_idx;
        prog->funcs[i].arity = arity;
        prog->funcs[i].nlocals = nlocals;
        prog->funcs[i].code_off = code_off;
        prog->funcs[i].code_len = code_len;
        prog->funcs[i].name = prog->names[name_idx];
    }

    /* 代码段 */
    if (off + code_size > n) {
        fprintf(stderr, "error: truncated code section\n");
        free_program(prog);
        return NULL;
    }
    prog->code = vm_alloc(code_size ? code_size : 1);
    if (!prog->code) { free_program(prog); return NULL; }
    memcpy(prog->code, data + off, code_size);
    prog->code_size = code_size;

    /* ---------- 可选 trailer:DXRL(库的字符串释放函数) ----------
       位于代码段之后,布局: "DXRL" | count(u16) | (lib_idx(u16), native_idx(u16)) * count
       旧字节码没有这一节,故以标记探测;不认识本节的旧 VM 只读 code_size 之前的内容,
       因此这是向后兼容的格式扩展,无需改动版本号或定长表布局。

       为什么要放进字节码:原生函数返回的 const char* 原始缓冲区指针只存在于
       VM 的值栈上,编译器无法用字节码表达"释放它"(DUP 复制的是 VM 已 strdup 的副本)。
       约定与动机见 docs/SPEC.md 4.5。 */
    {
        size_t rel_off = off + code_size;
        if (rel_off + 6 <= n &&
            data[rel_off] == 'D' && data[rel_off + 1] == 'X' &&
            data[rel_off + 2] == 'R' && data[rel_off + 3] == 'L') {
            uint16_t n_rel = (uint16_t)(data[rel_off + 4] | (data[rel_off + 5] << 8));
            size_t p = rel_off + 6;
            for (uint16_t k = 0; k < n_rel; k++) {
                if (p + 4 > n) {
                    fprintf(stderr, "error: truncated release table\n");
                    free_program(prog);
                    return NULL;
                }
                uint16_t lib_idx = (uint16_t)(data[p] | (data[p + 1] << 8));
                uint16_t nat_idx = (uint16_t)(data[p + 2] | (data[p + 3] << 8));
                p += 4;
                if (lib_idx >= n_libs || nat_idx >= n_natives) {
                    fprintf(stderr, "error: bad release table entry\n");
                    free_program(prog);
                    return NULL;
                }
                prog->libs[lib_idx].release_fn = &prog->natives[nat_idx];
            }
        }
    }

    /* 校验每个函数的代码区间与指令对齐。
       执行期的 pc 检查只看全局 code_size,若某个函数的 code_off/code_len 越界,
       或函数内最后一条带操作数的指令只写了部分操作数,执行时就会读到代码段之外
       (或执行到邻接函数的指令体)。这里在加载期一次性拒绝。 */
    for (uint16_t i = 0; i < n_funcs; i++) {
        const Func *f = &prog->funcs[i];
        if ((size_t)f->code_off > (size_t)code_size ||
            (size_t)f->code_len > (size_t)code_size - (size_t)f->code_off) {
            fprintf(stderr,
                    "error: function '%s' code range [%u,+%u) exceeds code section (%u bytes)\n",
                    f->name, f->code_off, f->code_len, code_size);
            free_program(prog);
            return NULL;
        }
        uint32_t p = f->code_off, end = f->code_off + f->code_len;
        while (p < end) {
            uint8_t op = prog->code[p];
            if (!opcode_has_operand(op)) {
                p += 1;
            } else if (end - p < 5) {
                fprintf(stderr,
                        "error: truncated instruction (opcode 0x%02X) at end of function '%s'\n",
                        op, f->name);
                free_program(prog);
                return NULL;
            } else {
                p += 5;
            }
        }
    }
    return prog;
}

/* ---------- 值运算辅助 ---------- */
static int value_truthy(Value v) {
    switch (v.type) {
        case V_INT:   return v.as.i != 0;
        case V_FLOAT: return v.as.f != 0.0;
        case V_STR:   return v.as.p[0] != '\0';
        case V_OBJ:   return 1;   /* 对象恒为真 */
    }
    return 0;
}

/* 对象图的最大递归深度,用于打印/比较/值拷贝三处的防御。
   环已由两道措施排除:
     1) 编译期拒绝递归类型(见 compiler.py 的 check_recursive_types),因此
        `a.v = a` 这类构造在类型层面就不成立;
     2) 编译期校验字段赋值类型。
   这里仍设上限,是因为字节码也可能来自其它工具或被手工构造。 */
#define MAX_OBJ_DEPTH 64

/* ---------------- 结构体值语义(P3) ----------------
 *
 * 源码表面本来就把结构体写成值(`Box{...}`、`b.f`,无取地址、无 null),
 * 但 VM 原先按**引用**实现:let b = a 之后 b 与 a 共享同一对象,改 b.x 会改到 a.x。
 * 那正是此前禁用回收的根因 —— 字段与局部槽可能指向同一块堆内存,按槽释放必然悬垂。
 *
 * 现在对齐为值语义:赋值(OP_STORE)时把结构体深拷贝一份。
 * 配合「拒绝递归类型」,环在类型层面不可表达,因此不需要环收集器,
 * 引用计数(P4)即可完备。
 *
 * 只有 OP_STORE 需要拷贝:
 *   - 表达式求值产生的都是栈上的临时值,不构成别名;
 *   - SET_FIELD 修改的是局部槽对象自身(用户期望就地修改)。 */
static Value copy_value(const Program *prog, Value v, int depth);

static Value copy_obj(const Program *prog, const Obj *src, int depth) {
    Obj *o = (Obj *)vm_alloc(sizeof(Obj));
    o->type_name = src->type_name;
    o->nfields = src->nfields;
    o->fields = src->nfields
                    ? (Value *)vm_alloc(src->nfields * sizeof(Value))
                    : NULL;
    for (uint32_t i = 0; i < src->nfields; i++)
        o->fields[i] = copy_value(prog, src->fields[i], depth + 1);
    Value r = { V_OBJ, {0} };
    r.as.o = o;
    return r;
}

static Value copy_value(const Program *prog, Value v, int depth) {
    /* 类型层面已排除递归,这里的上限只是防御手写字节码;
       触顶时退化为共享(对象本身不释放,故不会悬垂)。 */
    if (v.type != V_OBJ || depth >= MAX_OBJ_DEPTH) return v;
    (void)prog;
    return copy_obj(prog, (Obj *)v.as.o, depth);
}

/* 打印单个值(不带换行;对象字段递归打印) */
static void print_value_d(const Program *prog, Value v, int depth) {
    switch (v.type) {
        case V_INT:   printf("%lld", (long long)v.as.i); break;
        case V_FLOAT: printf("%g", v.as.f); break;
        case V_STR:   printf("%s", v.as.p); break;
        case V_OBJ: {
            Obj *o = (Obj *)v.as.o;
            if (depth >= MAX_OBJ_DEPTH) { printf("..."); break; }
            printf("%s(", o->type_name ? o->type_name : "?");
            for (uint32_t i = 0; i < o->nfields; i++) {
                if (i) printf(", ");
                Value f = o->fields[i];
                switch (f.type) {
                    case V_INT:   printf("%lld", (long long)f.as.i); break;
                    case V_FLOAT: printf("%g", f.as.f); break;
                    case V_STR:   printf("\"%s\"", f.as.p); break;
                    case V_OBJ:   print_value_d(prog, f, depth + 1); break;
                }
            }
            printf(")");
            break;
        }
    }
}

/* 打印单个值(不带换行;对象字段递归打印) */
static void print_value(const Program *prog, Value v) {
    print_value_d(prog, v, 0);
}

static void value_print(const Program *prog, Value v) {
    print_value(prog, v);
    printf("\n");
}

/* 比较:返回 -1/0/1;不可比较返回 INT_MIN */
static int value_cmp_d(const Program *prog, Value a, Value b, int depth) {
    if (a.type == V_OBJ || b.type == V_OBJ) {
        /* 对象仅支持 ==/!=:按类型名 + 字段递归比较 */
        if (a.type != b.type) return INT_MIN;
        Obj *oa = (Obj *)a.as.o, *ob = (Obj *)b.as.o;
        if (oa == ob) return 0;                 /* 同一实例 */
        if (depth >= MAX_OBJ_DEPTH) return INT_MIN;   /* 环:视为不可比较 */
        if (oa->nfields != ob->nfields) return INT_MIN;
        const char *ta = oa->type_name ? oa->type_name : "";
        const char *tb = ob->type_name ? ob->type_name : "";
        if (strcmp(ta, tb) != 0) return INT_MIN;
        for (uint32_t i = 0; i < oa->nfields; i++) {
            int c = value_cmp_d(prog, oa->fields[i], ob->fields[i], depth + 1);
            if (c == INT_MIN) return INT_MIN;
            /* 逐字段按序比较:必须原样传递 -1/0/1,
               否则 a < b 与 a > b 会同时为真(旧实现恒返回 1)。 */
            if (c != 0) return c;
        }
        return 0;
    }
    if (a.type == V_INT && b.type == V_INT)
        return (a.as.i < b.as.i) ? -1 : (a.as.i > b.as.i ? 1 : 0);
    if (a.type == V_STR && b.type == V_STR) {
        int c = strcmp(a.as.p, b.as.p);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (a.type == V_FLOAT || b.type == V_FLOAT) {
        double x = (a.type == V_INT) ? (double)a.as.i : a.as.f;
        double y = (b.type == V_INT) ? (double)b.as.i : b.as.f;
        return (x < y) ? -1 : (x > y ? 1 : 0);
    }
    (void)prog;
    return INT_MIN;
}

static int value_cmp(const Program *prog, Value a, Value b) {
    return value_cmp_d(prog, a, b, 0);
}

/* 字符串拼接(+ 被编译为 CONCAT):任一操作数为字符串则拼接,
   数字自动转成字符串;两者皆数值则不应发生。 */
static Value value_concat(const Program *prog, const Frame *fr, uint32_t pc,
                          Value a, Value b) {
    char abuf[64], bbuf[64];
    const char *as = NULL, *bs = NULL;
    Value r;
    memset(&r, 0, sizeof(r));
    if (a.type == V_STR) as = a.as.p;
    else if (a.type == V_INT) {
        snprintf(abuf, sizeof abuf, "%lld", (long long)a.as.i); as = abuf;
    } else if (a.type == V_FLOAT) {
        snprintf(abuf, sizeof abuf, "%g", a.as.f); as = abuf;
    } else die(prog, fr, pc, "cannot concatenate this value");

    if (b.type == V_STR) bs = b.as.p;
    else if (b.type == V_INT) {
        snprintf(bbuf, sizeof bbuf, "%lld", (long long)b.as.i); bs = bbuf;
    } else if (b.type == V_FLOAT) {
        snprintf(bbuf, sizeof bbuf, "%g", b.as.f); bs = bbuf;
    } else die(prog, fr, pc, "cannot concatenate this value");

    size_t na = strlen(as), nb = strlen(bs);
    char *out = (char *)vm_alloc(na + nb + 1);
    if (!out) die(prog, fr, pc, "out of memory");
    memcpy(out, as, na);
    memcpy(out + na, bs, nb);
    out[na + nb] = '\0';
    r.type = V_STR;
    r.as.p = out;
    return r;
}

static Value value_binop(const Program *prog, const Frame *fr, uint32_t pc,
                         int op, Value a, Value b) {
    Value r;
    memset(&r, 0, sizeof(r));
    r.type = V_INT;

    if (op == OP_ADD || op == OP_SUB || op == OP_MUL ||
        op == OP_DIV || op == OP_MOD) {
        if (a.type == V_INT && b.type == V_INT) {
            if (op == OP_DIV) {
                if (b.as.i == 0) die(prog, fr, pc, "division by zero");
                r.type = V_FLOAT;
                r.as.f = (double)a.as.i / (double)b.as.i;
                return r;
            }
            if (op == OP_MOD) {
                if (b.as.i == 0) die(prog, fr, pc, "division by zero");
                /* INT64_MIN % -1 在 C 中是未定义行为(x86-64 上触发 SIGFPE
                   整数溢出陷阱),数学结果为 0,这里直接给出 */
                if (b.as.i == -1) { r.as.i = 0; return r; }
                r.as.i = a.as.i % b.as.i;
                return r;
            }
            switch (op) {
                case OP_ADD: r.as.i = a.as.i + b.as.i; break;
                case OP_SUB: r.as.i = a.as.i - b.as.i; break;
                case OP_MUL: r.as.i = a.as.i * b.as.i; break;
            }
            return r;
        }
        if (a.type == V_FLOAT || b.type == V_FLOAT) {
            if (a.type == V_STR || b.type == V_STR)
                die(prog, fr, pc, "unsupported operand types for arithmetic");
            double x = (a.type == V_INT) ? (double)a.as.i : a.as.f;
            double y = (b.type == V_INT) ? (double)b.as.i : b.as.f;
            switch (op) {
                case OP_ADD: r.type = V_FLOAT; r.as.f = x + y; break;
                case OP_SUB: r.type = V_FLOAT; r.as.f = x - y; break;
                case OP_MUL: r.type = V_FLOAT; r.as.f = x * y; break;
                case OP_DIV:
                    if (y == 0.0) die(prog, fr, pc, "division by zero");
                    r.type = V_FLOAT; r.as.f = x / y; break;
                case OP_MOD: die(prog, fr, pc, "MOD requires integer operands"); break;
            }
            return r;
        }
        die(prog, fr, pc, "unsupported operand types for arithmetic");
    }

    if (op == OP_EQ || op == OP_NE || op == OP_LT ||
        op == OP_LE || op == OP_GT || op == OP_GE) {
        int c = value_cmp(prog, a, b);
        if (c == INT_MIN) {
            if (op == OP_EQ) { r.as.i = 0; return r; }
            if (op == OP_NE) { r.as.i = 1; return r; }
            die(prog, fr, pc, "cannot order values of these types");
        }
        switch (op) {
            case OP_EQ: r.as.i = (c == 0); break;
            case OP_NE: r.as.i = (c != 0); break;
            case OP_LT: r.as.i = (c < 0); break;
            case OP_LE: r.as.i = (c <= 0); break;
            case OP_GT: r.as.i = (c > 0); break;
            case OP_GE: r.as.i = (c >= 0); break;
        }
        return r;
    }

    die(prog, fr, pc, "unsupported binary opcode %d", op);
    return r;
}

/* ---------- 原生函数调用(FFI) ---------- */
static char *xstrdup(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    char *p = vm_alloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len + 1);
    return p;
}

/* 惰性加载库并解析符号;成功返回 0,失败返回 1(已向 stderr 报错)。 */
static int native_resolve(Program *prog, Native *na) {
    if (na->fn) return 0;
    Lib *lib = &prog->libs[na->lib_idx];
    if (!lib->handle) {
        const char *load_path = lib->is_static ? lib->temp_path : lib->path;
#ifdef _WIN32
        lib->handle = (void *)LoadLibraryA(load_path);
#else
        lib->handle = dlopen(load_path, RTLD_LAZY);
#endif
        if (!lib->handle) {
            fprintf(stderr, "runtime error: cannot load library '%s'\n", load_path);
            return 1;
        }
#ifdef _WIN32
        /* Windows:模块文件在加载期间被映像映射,此处删除会失败;
           交由 free_program() 在 FreeLibrary 之后再删。 */
#else
        /* POSIX:dlopen 后 inode 已引用,立即 unlink。这样临时文件在运行期
           极短存在,也避免进程被 kill 时残留(旧实现在 SIGKILL 下会留垃圾)。 */
        if (lib->is_static && lib->temp_path) {
            remove(lib->temp_path);
        }
#endif
    }
#ifdef _WIN32
    na->fn = (void *)GetProcAddress((HMODULE)lib->handle, na->name);
#else
    na->fn = dlsym(lib->handle, na->name);
#endif
    if (!na->fn) {
        fprintf(stderr, "runtime error: symbol '%s' not found in '%s'\n", na->name, lib->path);
        return 1;
    }
    return 0;
}

/* 当前 FFI 支持的签名集合(参数组合 + 返回类型)。 */
static int native_sig_supported(const Native *na) {
    /* 值数组 ABI:实参不展开成 C 参数,类型组合在运行时由标签决定,
       所以只要求 arity 与返回类型合法 —— 与 arity 0..3 的类型组合表无关。 */
    if (na->is_array_abi) {
        return na->arity <= MAX_NATIVE_ARGS &&
               (na->ret_type == NAT_VOID || na->ret_type == NAT_INT ||
                na->ret_type == NAT_FLOAT || na->ret_type == NAT_STR);
    }
    switch (na->arity) {
    case 0:
        return na->ret_type == NAT_VOID || na->ret_type == NAT_INT ||
               na->ret_type == NAT_FLOAT || na->ret_type == NAT_STR;
    case 1:
        if (na->param_types[0] == NAT_INT || na->param_types[0] == NAT_FLOAT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_FLOAT
                   || na->ret_type == NAT_STR;
        if (na->param_types[0] == NAT_STR)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_STR
                   || na->ret_type == NAT_FLOAT;
        return 0;
    case 2:
        if (na->param_types[0] == NAT_INT && na->param_types[1] == NAT_INT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_FLOAT;
        if (na->param_types[0] == NAT_INT && na->param_types[1] == NAT_FLOAT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_FLOAT;
        if (na->param_types[0] == NAT_INT && na->param_types[1] == NAT_STR)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT;
        if (na->param_types[0] == NAT_FLOAT && na->param_types[1] == NAT_INT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_FLOAT;
        if (na->param_types[0] == NAT_FLOAT && na->param_types[1] == NAT_FLOAT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_FLOAT;
        if (na->param_types[0] == NAT_STR && na->param_types[1] == NAT_STR)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_STR;
        if (na->param_types[0] == NAT_STR && na->param_types[1] == NAT_INT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_STR;
        return 0;
    case 3:
        if (na->param_types[0] == NAT_INT && na->param_types[1] == NAT_INT &&
            na->param_types[2] == NAT_INT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_FLOAT;
        if (na->param_types[0] == NAT_STR && na->param_types[1] == NAT_INT &&
            na->param_types[2] == NAT_INT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_STR;
        if (na->param_types[0] == NAT_INT && na->param_types[1] == NAT_STR &&
            na->param_types[2] == NAT_STR)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT;
        if (na->param_types[0] == NAT_STR && na->param_types[1] == NAT_STR &&
            na->param_types[2] == NAT_STR)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_STR;
        if (na->param_types[0] == NAT_STR && na->param_types[1] == NAT_STR &&
            na->param_types[2] == NAT_INT)
            return na->ret_type == NAT_VOID || na->ret_type == NAT_INT || na->ret_type == NAT_STR;
        return 0;
    default:
        return 0;
    }
}

/* ---------- 执行主循环 ---------- */
static int run(Program *prog) {
    if (prog->nfuncs == 0) {
        fprintf(stderr, "error: program has no functions\n");
        return 1;
    }
    Func *mainf = &prog->funcs[0];

    static Value stack[MAX_STACK];
    static Frame frames[MAX_FRAMES];
    size_t sp = 0;
    size_t nframes = 0; /* main 之上的活动帧数 */
    int trace = getenv("DEX_TRACE") != NULL;

    /* 主帧 */
    frames[0].locals = vm_calloc(mainf->nlocals ? mainf->nlocals : 1, sizeof(Value));
    frames[0].nlocals = mainf->nlocals;
    frames[0].base = 0;
    frames[0].ret_pc = 0;
    frames[0].func_idx = 0;
    Frame *fr = &frames[0];

    uint32_t pc = mainf->code_off;
    if (pc >= prog->code_size)
        die(prog, fr, pc, "main code offset out of range");

    for (;;) {
        if (pc >= prog->code_size)
            die(prog, fr, pc, "pc out of code section");
        uint8_t op = prog->code[pc];

        if (trace) {
            const char *fn = (fr->func_idx < prog->nfuncs)
                                 ? prog->funcs[fr->func_idx].name : "?";
            fprintf(stderr, "[%s] pc=%u sp=%zu ", fn ? fn : "?", pc, sp);
            switch (op) {
                case OP_PUSH: fprintf(stderr, "PUSH c%u", rd32(prog->code + pc + 1)); break;
                case OP_LOAD: fprintf(stderr, "LOAD %%%u", rd32(prog->code + pc + 1)); break;
                case OP_STORE: fprintf(stderr, "STORE %%%u", rd32(prog->code + pc + 1)); break;
                case OP_JMP: case OP_JZ: case OP_JNZ:
                    fprintf(stderr, "%s @%u", op == OP_JMP ? "JMP" : (op == OP_JZ ? "JZ" : "JNZ"),
                            rd32(prog->code + pc + 1));
                    break;
                case OP_CALL: fprintf(stderr, "CALL f%u", rd32(prog->code + pc + 1)); break;
                case OP_NCALL: fprintf(stderr, "NCALL n%u", rd32(prog->code + pc + 1)); break;
                default:
                    fprintf(stderr, "opcode 0x%02X", op);
                    break;
            }
            fprintf(stderr, "\n");
        }

        switch (op) {
        case OP_NOP:
            pc += 1;
            break;

        case OP_PUSH: {
            uint32_t ci = rd32(prog->code + pc + 1);
            if (ci >= prog->nconsts) die(prog, fr, pc, "constant index out of range");
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow");
            stack[sp++] = prog->consts[ci];
            pc += 5;
            break;
        }

        case OP_LOAD: {
            uint32_t li = rd32(prog->code + pc + 1);
            if (li >= fr->nlocals) die(prog, fr, pc, "local index out of range");
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow");
            stack[sp++] = fr->locals[li];
            pc += 5;
            break;
        }

        case OP_STORE: {
            uint32_t li = rd32(prog->code + pc + 1);
            if (li >= fr->nlocals) die(prog, fr, pc, "local index out of range");
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            /* 结构体按值语义存储:拷贝一份,使局部槽不与他人共享对象 */
            fr->locals[li] = copy_value(prog, stack[--sp], 0);
            pc += 5;
            break;
        }

        case OP_CONCAT: {
            if (sp < 2) die(prog, fr, pc, "stack underflow");
            Value b = stack[--sp];
            Value a = stack[--sp];
            Value r = value_concat(prog, fr, pc, a, b);
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow");
            stack[sp++] = r;
            pc += 1;
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            if (sp < 2) die(prog, fr, pc, "stack underflow");
            Value b = stack[--sp];
            Value a = stack[--sp];
            Value r = value_binop(prog, fr, pc, op, a, b);
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow");
            stack[sp++] = r;
            pc += 1;
            break;
        }

        case OP_NEG: {
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            Value v = stack[--sp];
            if (v.type == V_INT) v.as.i = -v.as.i;
            else if (v.type == V_FLOAT) v.as.f = -v.as.f;
            else die(prog, fr, pc, "cannot negate a string");
            stack[sp++] = v;
            pc += 1;
            break;
        }

        case OP_NOT: {
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            Value v = stack[--sp];
            Value r = { V_INT, {0} };
            r.as.i = value_truthy(v) ? 0 : 1;
            stack[sp++] = r;
            pc += 1;
            break;
        }

        case OP_AND: case OP_OR: {
            if (sp < 2) die(prog, fr, pc, "stack underflow");
            Value b = stack[--sp];
            Value a = stack[--sp];
            int x = value_truthy(a);
            int y = value_truthy(b);
            Value r = { V_INT, {0} };
            r.as.i = (op == OP_AND) ? (x && y) : (x || y);
            stack[sp++] = r;
            pc += 1;
            break;
        }

        case OP_JMP: {
            /* 跳转操作数为函数内偏移,需加上当前函数的 code_off */
            uint32_t t = rd32(prog->code + pc + 1);
            uint32_t target = prog->funcs[fr->func_idx].code_off + t;
            if (target >= prog->code_size)
                die(prog, fr, pc, "jump target out of code");
            pc = target;
            break;
        }

        case OP_JZ: case OP_JNZ: {
            uint32_t t = rd32(prog->code + pc + 1);
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            Value v = stack[--sp];
            int truth = value_truthy(v);
            int take = (op == OP_JZ) ? !truth : truth;
            if (take) {
                uint32_t target = prog->funcs[fr->func_idx].code_off + t;
                if (target >= prog->code_size)
                    die(prog, fr, pc, "jump target out of code");
                pc = target;
            } else {
                pc += 5;
            }
            break;
        }

        case OP_CALL_NAME: {
            /* 栈 [args..., name, argc];弹 argc,再弹 name,剩余 argc 个实参 */
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            int64_t argc = stack[--sp].as.i;
            if (sp == 0) die(prog, fr, pc, "stack underflow (call name)");
            Value nv = stack[--sp];
            if (nv.type != V_STR) die(prog, fr, pc, "call() name must be a string");
            const char *name = nv.as.p;
            Func *callee = NULL;
            for (size_t i = 0; i < prog->nfuncs; i++) {
                if (prog->funcs[i].name && strcmp(prog->funcs[i].name, name) == 0) {
                    callee = &prog->funcs[i];
                    break;
                }
            }
            if (!callee) die(prog, fr, pc, "unknown function '%s' (call)", name);
            if (argc < 0 || (uint64_t)argc != callee->arity)
                die(prog, fr, pc, "function '%s' expects %u argument(s), got %lld",
                    name, callee->arity, (long long)argc);
            if (sp < (size_t)argc) die(prog, fr, pc, "stack underflow (call arguments)");
            if (nframes + 1 >= MAX_FRAMES) die(prog, fr, pc, "call stack overflow");

            size_t base = sp - (size_t)argc;
            nframes++;
            Frame *nf = &frames[nframes];
            nf->locals = vm_calloc(callee->nlocals ? callee->nlocals : 1, sizeof(Value));
            /* 结构体按值语义传参:形参拿到的是副本,函数内修改不影响调用方 */
            for (uint8_t i = 0; i < callee->arity; i++)
                nf->locals[i] = copy_value(prog, stack[base + i], 0);
            nf->nlocals = callee->nlocals;
            nf->base = base;
            nf->ret_pc = pc + 1;
            nf->func_idx = (uint16_t)(callee - prog->funcs);

            sp = base;
            fr = nf;
            pc = callee->code_off;
            if (pc >= prog->code_size)
                die(prog, fr, pc, "callee code offset out of range");
            break;
        }

        case OP_CALL: {
            uint32_t fi = rd32(prog->code + pc + 1);
            if (fi >= prog->nfuncs) die(prog, fr, pc, "function index out of range");
            Func *callee = &prog->funcs[fi];
            if (sp < callee->arity)
                die(prog, fr, pc, "stack underflow (missing arguments)");
            if (nframes + 1 >= MAX_FRAMES) die(prog, fr, pc, "call stack overflow");

            size_t base = sp - callee->arity;
            nframes++;
            Frame *nf = &frames[nframes];
            nf->locals = vm_calloc(callee->nlocals ? callee->nlocals : 1, sizeof(Value));
            /* 结构体按值语义传参:形参拿到的是副本,函数内修改不影响调用方 */
            for (uint8_t i = 0; i < callee->arity; i++)
                nf->locals[i] = copy_value(prog, stack[base + i], 0);
            nf->nlocals = callee->nlocals;
            nf->base = base;
            nf->ret_pc = pc + 5;
            nf->func_idx = (uint16_t)fi;

            sp = base;
            fr = nf;
            pc = callee->code_off;
            if (pc >= prog->code_size)
                die(prog, fr, pc, "callee code offset out of range");
            break;
        }

        case OP_NCALL: {
            uint32_t ni = rd32(prog->code + pc + 1);
            if (ni >= prog->nnatives)
                die(prog, fr, pc, "native function index out of range");
            Native *na = &prog->natives[ni];
            if (sp < na->arity)
                die(prog, fr, pc, "stack underflow (native arguments)");
            if (native_resolve(prog, na))
                die(prog, fr, pc, "cannot resolve native function '%s'", na->name);
            if (!native_sig_supported(na))
                die(prog, fr, pc, "unsupported native signature for '%s' (arity %u)",
                    na->name, na->arity);

            size_t base = sp - na->arity;

            int64_t ri = 0;
            double rf = 0;
            const char *rs = NULL;

            if (na->is_array_abi) {
                /* 值数组 ABI(见 docs/SPEC.md 4.5):操作数栈上的 Value 与
                   DexValue 布局一致,所以零编组直接把这段栈原样传过去。
                   调用期间不改 sp,返回后由下面的公共尾部统一消耗实参。
                   走 goto 而不是把直接 ABI 那一大段包进 else,是为了让现有
                   6 个库的代码路径保持一字不动(只加分支,不改老代码)。 */
                const DexValue *argv = (const DexValue *)(const void *)&stack[base];
                int argc = (int)na->arity;
                switch (na->ret_type) {
                case NAT_INT:   ri = ((DexNativeArrayI)na->fn)(argv, argc); break;
                case NAT_FLOAT: rf = ((DexNativeArrayF)na->fn)(argv, argc); break;
                case NAT_STR:   rs = ((DexNativeArrayS)na->fn)(argv, argc); break;
                default:        ((DexNativeArrayV)na->fn)(argv, argc); break;
                }
                goto ncall_done;
            }

            /* ---------- 直接 ABI(现有 6 个库):实参逐个展开成 C 参数 ---------- */
            {
            int64_t ai[MAX_NATIVE_ARGS];
            double af[MAX_NATIVE_ARGS];
            const char *as[MAX_NATIVE_ARGS];
            for (uint8_t k = 0; k < na->arity; k++) {
                Value v = stack[base + k];
                switch (na->param_types[k]) {
                case NAT_INT:
                    if (v.type == V_INT) ai[k] = v.as.i;
                    else if (v.type == V_FLOAT) ai[k] = (int64_t)v.as.f;
                    else die(prog, fr, pc, "native '%s' arg %u expects int", na->name, k + 1);
                    break;
                case NAT_FLOAT:
                    if (v.type == V_FLOAT) af[k] = v.as.f;
                    else if (v.type == V_INT) af[k] = (double)v.as.i;
                    else die(prog, fr, pc, "native '%s' arg %u expects float", na->name, k + 1);
                    break;
                case NAT_STR:
                    if (v.type != V_STR)
                        die(prog, fr, pc, "native '%s' arg %u expects string", na->name, k + 1);
                    as[k] = v.as.p;
                    break;
                default:
                    die(prog, fr, pc, "native '%s' has unknown argument type", na->name);
                }
            }

            if (na->arity == 0) {
                if (na->ret_type == NAT_INT) ri = ((int64_t (*)(void))na->fn)();
                else if (na->ret_type == NAT_FLOAT) rf = ((double (*)(void))na->fn)();
                else if (na->ret_type == NAT_STR) rs = ((const char *(*)(void))na->fn)();
                else ((void (*)(void))na->fn)();
            } else if (na->arity == 1) {
                switch (na->param_types[0]) {
                case NAT_INT:
                    if (na->ret_type == NAT_INT) ri = ((int64_t (*)(int64_t))na->fn)(ai[0]);
                    else if (na->ret_type == NAT_FLOAT) rf = ((double (*)(int64_t))na->fn)(ai[0]);
                    else if (na->ret_type == NAT_STR) rs = ((const char *(*)(int64_t))na->fn)(ai[0]);
                    else ((void (*)(int64_t))na->fn)(ai[0]);
                    break;
                case NAT_FLOAT:
                    if (na->ret_type == NAT_INT) ri = ((int64_t (*)(double))na->fn)(af[0]);
                    else if (na->ret_type == NAT_FLOAT) rf = ((double (*)(double))na->fn)(af[0]);
                    else if (na->ret_type == NAT_STR) rs = ((const char *(*)(double))na->fn)(af[0]);
                    else ((void (*)(double))na->fn)(af[0]);
                    break;
                case NAT_STR:
                    if (na->ret_type == NAT_INT) ri = ((int64_t (*)(const char *))na->fn)(as[0]);
                    else if (na->ret_type == NAT_FLOAT) rf = ((double (*)(const char *))na->fn)(as[0]);
                    else if (na->ret_type == NAT_STR) rs = ((const char *(*)(const char *))na->fn)(as[0]);
                    else ((void (*)(const char *))na->fn)(as[0]);
                    break;
                }
            } else if (na->arity == 2) {
                switch (na->param_types[0]) {
                case NAT_INT:
                    switch (na->param_types[1]) {
                    case NAT_INT:
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(int64_t, int64_t))na->fn)(ai[0], ai[1]);
                        else if (na->ret_type == NAT_FLOAT)
                            rf = ((double (*)(int64_t, int64_t))na->fn)(ai[0], ai[1]);
                        else ((void (*)(int64_t, int64_t))na->fn)(ai[0], ai[1]);
                        break;
                    case NAT_FLOAT:
                        if (na->ret_type == NAT_FLOAT)
                            rf = ((double (*)(int64_t, double))na->fn)(ai[0], af[1]);
                        else ((void (*)(int64_t, double))na->fn)(ai[0], af[1]);
                        break;
                    case NAT_STR:
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(int64_t, const char *))na->fn)(ai[0], as[1]);
                        else ((void (*)(int64_t, const char *))na->fn)(ai[0], as[1]);
                        break;
                    }
                    break;
                case NAT_FLOAT:
                    switch (na->param_types[1]) {
                    case NAT_INT:
                        if (na->ret_type == NAT_FLOAT)
                            rf = ((double (*)(double, int64_t))na->fn)(af[0], ai[1]);
                        else ((void (*)(double, int64_t))na->fn)(af[0], ai[1]);
                        break;
                    case NAT_FLOAT:
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(double, double))na->fn)(af[0], af[1]);
                        else if (na->ret_type == NAT_FLOAT)
                            rf = ((double (*)(double, double))na->fn)(af[0], af[1]);
                        else ((void (*)(double, double))na->fn)(af[0], af[1]);
                        break;
                    }
                    break;
                case NAT_STR:
                    switch (na->param_types[1]) {
                    case NAT_STR:
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(const char *, const char *))na->fn)(as[0], as[1]);
                        else if (na->ret_type == NAT_STR)
                            rs = ((const char *(*)(const char *, const char *))na->fn)(as[0], as[1]);
                        else ((void (*)(const char *, const char *))na->fn)(as[0], as[1]);
                        break;
                    case NAT_INT:
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(const char *, int64_t))na->fn)(as[0], ai[1]);
                        else if (na->ret_type == NAT_STR)
                            rs = ((const char *(*)(const char *, int64_t))na->fn)(as[0], ai[1]);
                        else ((void (*)(const char *, int64_t))na->fn)(as[0], ai[1]);
                        break;
                    }
                    break;
                }
            } else if (na->arity == 3) {
                if (na->param_types[0] == NAT_INT) {
                    if (na->param_types[1] == NAT_INT) {
                        /* (int,int,int) */
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(int64_t, int64_t, int64_t))na->fn)(ai[0], ai[1], ai[2]);
                        else if (na->ret_type == NAT_FLOAT)
                            rf = ((double (*)(int64_t, int64_t, int64_t))na->fn)(ai[0], ai[1], ai[2]);
                        else ((void (*)(int64_t, int64_t, int64_t))na->fn)(ai[0], ai[1], ai[2]);
                    } else {
                        /* (int,str,str) */
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(int64_t, const char *, const char *))na->fn)
                                    (ai[0], as[1], as[2]);
                        else ((void (*)(int64_t, const char *, const char *))na->fn)
                                    (ai[0], as[1], as[2]);
                    }
                } else if (na->param_types[0] == NAT_STR) {
                    if (na->param_types[1] == NAT_INT) {
                        /* (str,int,int) */
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(const char *, int64_t, int64_t))na->fn)
                                    (as[0], ai[1], ai[2]);
                        else if (na->ret_type == NAT_STR)
                            rs = ((const char *(*)(const char *, int64_t, int64_t))na->fn)
                                    (as[0], ai[1], ai[2]);
                        else ((void (*)(const char *, int64_t, int64_t))na->fn)
                                    (as[0], ai[1], ai[2]);
                    } else if (na->param_types[2] == NAT_INT) {
                        /* (str,str,int) */
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(const char *, const char *, int64_t))na->fn)
                                    (as[0], as[1], ai[2]);
                        else if (na->ret_type == NAT_STR)
                            rs = ((const char *(*)(const char *, const char *, int64_t))na->fn)
                                    (as[0], as[1], ai[2]);
                        else ((void (*)(const char *, const char *, int64_t))na->fn)
                                    (as[0], as[1], ai[2]);
                    } else {
                        /* (str,str,str) */
                        if (na->ret_type == NAT_INT)
                            ri = ((int64_t (*)(const char *, const char *, const char *))na->fn)
                                    (as[0], as[1], as[2]);
                        else if (na->ret_type == NAT_STR)
                            rs = ((const char *(*)(const char *, const char *, const char *))na->fn)
                                    (as[0], as[1], as[2]);
                        else ((void (*)(const char *, const char *, const char *))na->fn)
                                    (as[0], as[1], as[2]);
                    }
                }
            } else {
                die(prog, fr, pc, "unsupported native arity %u (max 3)", na->arity);
            }
            }  /* 直接 ABI 作用域结束 */

        ncall_done:
            sp = base;  /* 消耗实参 */

            Value r = { V_INT, {0} };
            if (na->ret_type == NAT_FLOAT) { r.type = V_FLOAT; r.as.f = rf; }
            else if (na->ret_type == NAT_STR) {
                r.type = V_STR;
                r.as.p = xstrdup(rs);        /* VM 自己持有一份副本 */
                /* 按 DXRL trailer 的约定释放原生侧缓冲区。
                   传给它的是 rs(原生库的原始指针),不是 r.as.p,因此不会双重释放。 */
                Native *rel_nat = (Native *)prog->libs[na->lib_idx].release_fn;
                if (rel_nat) {
                    if (native_resolve(prog, rel_nat)) {
                        die(prog, fr, pc, "cannot resolve release function '%s'", rel_nat->name);
                    }
                    ((void (*)(const char *))rel_nat->fn)(rs);
                }
            }
            else { r.type = V_INT; r.as.i = ri; }
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow");
            stack[sp++] = r;
            pc += 5;
            break;
        }

        case OP_MAKE_OBJ: {
            uint32_t operand = rd32(prog->code + pc + 1);
            uint32_t type_cidx = operand >> 16;
            uint32_t nfields = operand & 0xFFFF;
            if (type_cidx >= prog->nconsts)
                die(prog, fr, pc, "type constant index out of range");
            const char *tname = prog->names[type_cidx];
            if (!tname) die(prog, fr, pc, "type constant is not a string");
            if (sp < nfields) die(prog, fr, pc, "stack underflow (object fields)");
            Obj *o = (Obj *)vm_alloc(sizeof(Obj));
            if (!o) die(prog, fr, pc, "out of memory");
            o->type_name = tname;
            o->nfields = nfields;
            o->fields = nfields ? (Value *)vm_alloc(nfields * sizeof(Value)) : NULL;
            if (nfields && !o->fields) die(prog, fr, pc, "out of memory");
            sp -= nfields;
            for (uint32_t i = 0; i < nfields; i++)
                o->fields[i] = stack[sp + i];
            Value r = { V_OBJ, {0} };
            r.as.o = o;
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow");
            stack[sp++] = r;
            pc += 5;
            break;
        }

        case OP_GET_FIELD: {
            uint32_t idx = rd32(prog->code + pc + 1);
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            Value v = stack[--sp];
            if (v.type != V_OBJ) die(prog, fr, pc, "GET_FIELD on non-object");
            Obj *o = (Obj *)v.as.o;
            if (idx >= o->nfields) die(prog, fr, pc, "field index out of range");
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow");
            stack[sp++] = o->fields[idx];
            pc += 5;
            break;
        }

        case OP_SET_FIELD: {
            uint32_t idx = rd32(prog->code + pc + 1);
            if (sp < 2) die(prog, fr, pc, "stack underflow");
            Value val = stack[--sp];
            Value objv = stack[--sp];
            if (objv.type != V_OBJ) die(prog, fr, pc, "SET_FIELD on non-object");
            Obj *o = (Obj *)objv.as.o;
            if (idx >= o->nfields) die(prog, fr, pc, "field index out of range");
            o->fields[idx] = val;
            pc += 5;
            break;
        }

        case OP_RET: {
            if (sp == 0) die(prog, fr, pc, "stack underflow on RET");
            Value rv = stack[--sp];
            if (nframes == 0) { /* 从 main 返回 → 停机 */
                vm_free(fr->locals);
                goto done;
            }
            Frame *prev = &frames[nframes - 1];
            /* 返回地址在"当前返回帧"的 ret_pc 中(CALL 时写入),而非调用方 */
            uint32_t ret_pc = fr->ret_pc;
            /* 结果放回被调帧的参数起始位置(其 base),而非调用方的 base */
            size_t res_base = fr->base;
            vm_free(fr->locals);
            sp = res_base;
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow on RET");
            stack[sp++] = rv;
            pc = ret_pc;
            fr = prev;
            nframes--;
            break;
        }

        case OP_PRINT: {
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            value_print(prog, stack[--sp]);
            pc += 1;
            break;
        }

        case OP_POP: {
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            sp--;
            pc += 1;
            break;
        }

        case OP_DUP: {
            if (sp == 0) die(prog, fr, pc, "stack underflow");
            if (sp >= MAX_STACK) die(prog, fr, pc, "stack overflow");
            stack[sp] = stack[sp - 1];
            sp++;
            pc += 1;
            break;
        }

        case OP_HALT:
            vm_free(fr->locals);
            goto done;

        default:
            die(prog, fr, pc, "unknown opcode 0x%02X", op);
        }
    }

done:
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* 让控制台以 UTF-8 解释输出,避免中文/Unicode 乱码(配合 IDE 按 UTF-8 捕获) */
    SetConsoleOutputCP(CP_UTF8);
    /* 输入侧同样切到 UTF-8。只设输出代码页时,stdin 仍按 OEM 代码页解码,
       导致标准库 dex_input() 读取中文输入必然乱码。 */
    SetConsoleCP(CP_UTF8);
#endif
#ifdef PUBLISH_BUILD
    /* 发布版/无控制台版:不显示控制台窗口(FreeConsole 分离当前控制台) */
    FreeConsole();
#endif
    /* 参数 <file.dexbc>;无参数时(发布版)自动读取可执行文件同目录的 core.do */
    const char *path = NULL;
    char defpath[1024];
    if (argc >= 2) {
        path = argv[1];
    } else {
#ifdef _WIN32
        GetModuleFileNameA(NULL, defpath, sizeof defpath);
        char *slash = strrchr(defpath, '\\');
        if (slash) *(slash + 1) = '\0';
        else defpath[0] = '\0';
#else
        strcpy(defpath, "./");
#endif
        strcat(defpath, "core.do");
        path = defpath;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        if (argc < 2)
            fprintf(stderr, "usage: %s <file.dexbc>  (或在该目录放置 core.do)\n", argv[0]);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }

    uint8_t *data = malloc(n > 0 ? (size_t)n : 1);
    if (!data) { fclose(f); return 1; }
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(data); return 1; }

    Program *prog = load_program(data, (size_t)n);
    free(data);
    if (!prog) return 1;

    int rc = run(prog);
    free_program(prog);
    return rc;
}
