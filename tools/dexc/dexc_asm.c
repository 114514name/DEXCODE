/* ============================================================================
 * dexc_asm.c — 汇编器(IR → .dexbc)+ 汇编文本(渲染/解析)+ 反汇编器。
 *              对应 dexlang/{assembler,asmtext,disassembler}.py。
 *
 * 逐字节一致的两个关键点:
 *   1) 常量池的**注册顺序**是固定的:库路径 → 原生名 → 函数名 → 各函数的
 *      PUSH 常量(函数顺序 = main 在前)。顺序变了,字节码就整体不同。
 *   2) 常量池的键**必须带类型**:int 0 与 float 0.0 是两条不同的常量。
 *      Python 版最早只按值做键,踩中了 `0 == 0.0`,这个 bug 在值数组 ABI 下
 *      才暴露出来(见 AGENTS.md 的陷阱表)。移植时不能再犯。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dexc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ 字节缓冲 */

typedef struct {
    unsigned char *p;
    size_t len, cap;
} Buf;

static void buf_bytes(Buf *b, const void *src, size_t n)
{
    size_t need = b->len + n;
    if (need < b->len) {
        fprintf(stderr, "dexc: internal buffer size overflow\n");
        exit(2);
    }
    if (need > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < need) {
            if (nc > ((size_t)-1) / 2) { nc = need; break; }
            nc *= 2;
        }
        b->cap = nc;
        b->p = dx_realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void buf_u8(Buf *b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    buf_bytes(b, &c, 1);
}

static void buf_u16(Buf *b, unsigned v)
{
    unsigned char c[2];
    c[0] = (unsigned char)(v & 0xFF);
    c[1] = (unsigned char)((v >> 8) & 0xFF);
    buf_bytes(b, c, 2);
}

static void buf_u32(Buf *b, uint32_t v)
{
    unsigned char c[4];
    c[0] = (unsigned char)(v & 0xFF);
    c[1] = (unsigned char)((v >> 8) & 0xFF);
    c[2] = (unsigned char)((v >> 16) & 0xFF);
    c[3] = (unsigned char)((v >> 24) & 0xFF);
    buf_bytes(b, c, 4);
}

/* ------------------------------------------------------------ 常量池 */

typedef struct {
    int kind;          /* DEXC_TAG_* */
    int64_t ival;
    double fval;
    char *sval;
} Const;

typedef struct {
    Const *items;
    int n, cap;
} ConstPool;

static int pool_find(ConstPool *cp, int kind, int64_t iv, double fv, const char *sv)
{
    int i;
    for (i = 0; i < cp->n; i++) {
        Const *c = &cp->items[i];
        if (c->kind != kind) continue;
        if (kind == DEXC_TAG_INT) { if (c->ival == iv) return i; }
        else if (kind == DEXC_TAG_FLOAT) { if (c->fval == fv) return i; }
        else if (!strcmp(c->sval, sv)) return i;
    }
    return -1;
}

static int pool_add(ConstPool *cp, int kind, int64_t iv, double fv, const char *sv)
{
    int idx = pool_find(cp, kind, iv, fv, sv);
    Const c;
    if (idx >= 0) return idx;
    if (cp->n >= 0x10000) dx_throw("assembler", 0, 0, "too many constants (>65535)");
    memset(&c, 0, sizeof c);
    c.kind = kind;
    c.ival = iv;
    c.fval = fv;
    c.sval = sv ? dx_strdup(sv) : NULL;
    DXV_PUSH(cp->items, cp->n, cp->cap, c);
    return cp->n - 1;
}

static int cidx_i(ConstPool *cp, int64_t v) { return pool_add(cp, DEXC_TAG_INT, v, 0, NULL); }
static int cidx_f(ConstPool *cp, double v) { return pool_add(cp, DEXC_TAG_FLOAT, 0, v, NULL); }
static int cidx_s(ConstPool *cp, const char *v) { return pool_add(cp, DEXC_TAG_STRING, 0, 0, v); }

static int cidx_operand(ConstPool *cp, const Operand *o)
{
    if (o->kind == OA_CONST_I) return cidx_i(cp, o->ival);
    if (o->kind == OA_CONST_F) return cidx_f(cp, o->fval);
    if (o->kind == OA_CONST_S) return cidx_s(cp, o->s);
    dx_throw("assembler", 0, 0, "unsupported constant type");
    return -1;
}

/* ------------------------------------------------------------ 汇编 */

static int compute_nlocals(AsmFunc *f)
{
    int max_local = -1, i;
    if (f->nlocals) return f->nlocals;
    for (i = 0; i < f->ninsns; i++) {
        Insn *in = &f->insns[i];
        if (in->op == OP_LOAD || in->op == OP_STORE)
            if (in->operand.index > max_local) max_local = in->operand.index;
    }
    return (f->arity > max_local + 1) ? f->arity : max_local + 1;
}

static Map *g_func_index;
static Map *g_native_index;

static uint32_t operand_value(Insn *in, Map *offsets, ConstPool *cp, int nlocals)
{
    Operand *o = &in->operand;
    if (in->op == OP_PUSH) return (uint32_t)cidx_operand(cp, o);
    if (in->op == OP_LOAD || in->op == OP_STORE) {
        if (o->index < 0 || o->index >= nlocals) {
            dx_throw("assembler", 0, 0,
                     "local index %%%d out of range (nlocals=%d) in function",
                     o->index, nlocals);
        }
        return (uint32_t)o->index;
    }
    if (in->op == OP_JMP || in->op == OP_JZ || in->op == OP_JNZ) {
        MapEntry *e = dx_map_get(offsets, o->s);
        if (!e) dx_throw("assembler", 0, 0, "undefined label '%s'", o->s);
        return (uint32_t)e->ival;
    }
    if (in->op == OP_CALL) {
        MapEntry *e = dx_map_get(g_func_index, o->s);
        if (!e) dx_throw("assembler", 0, 0, "undefined function '%s'", o->s);
        return (uint32_t)e->ival;
    }
    if (in->op == OP_NCALL) {
        MapEntry *e = dx_map_get(g_native_index, o->s);
        if (!e) dx_throw("assembler", 0, 0, "undefined native function '%s'", o->s);
        return (uint32_t)e->ival;
    }
    if (in->op == OP_MAKE_OBJ) {
        int tc = cidx_s(cp, o->s);
        return (uint32_t)(((unsigned)tc << 16) | (unsigned)o->nfields);
    }
    if (in->op == OP_GET_FIELD || in->op == OP_SETFIELD) return (uint32_t)o->index;
    dx_throw("assembler", 0, 0, "unexpected operand on opcode 0x%02X", in->op);
    return 0;
}

unsigned char *dx_assemble(CompileUnit *u, size_t *out_len)
{
    AssemblyProgram *prog = &u->prog;
    ConstPool cp;
    Buf code, lib_table, native_table, func_table, pool, header, body, trailer;
    Map func_index, native_index, offsets;
    int i, k;

    memset(&cp, 0, sizeof cp);
    memset(&code, 0, sizeof code);
    memset(&lib_table, 0, sizeof lib_table);
    memset(&native_table, 0, sizeof native_table);
    memset(&func_table, 0, sizeof func_table);
    memset(&pool, 0, sizeof pool);
    memset(&header, 0, sizeof header);
    memset(&body, 0, sizeof body);
    memset(&trailer, 0, sizeof trailer);

    if (prog->nfuncs == 0) dx_throw("assembler", 0, 0, "no functions to assemble");

    /* ---------- 常量池注册(固定顺序) ---------- */
    if (prog->nlibs >= 0x10000 || prog->nnatives >= 0x10000 || prog->nfuncs >= 0x10000)
        dx_throw("assembler", 0, 0, "too many libs/natives/functions (>65535)");
    for (i = 0; i < prog->nlibs; i++) cidx_s(&cp, prog->libs[i].path);
    for (i = 0; i < prog->nnatives; i++) cidx_s(&cp, prog->natives[i].name);
    memset(&func_index, 0, sizeof func_index);
    for (i = 0; i < prog->nfuncs; i++) {
        cidx_s(&cp, prog->funcs[i].name);
        dx_map_set(&func_index, prog->funcs[i].name)->ival = i;
    }
    memset(&native_index, 0, sizeof native_index);
    for (i = 0; i < prog->nnatives; i++)
        dx_map_set(&native_index, prog->natives[i].name)->ival = i;
    for (i = 0; i < prog->nfuncs; i++) {
        AsmFunc *f = &prog->funcs[i];
        for (k = 0; k < f->ninsns; k++) {
            Insn *in = &f->insns[k];
            if (in->op == OP_PUSH && in->operand.kind != OA_NONE)
                cidx_operand(&cp, &in->operand);
            else if (in->op == OP_MAKE_OBJ)
                cidx_s(&cp, in->operand.s);
        }
    }

    /* ---------- 编码代码段与函数表 ---------- */
    g_func_index = &func_index;
    g_native_index = &native_index;
    for (i = 0; i < prog->nfuncs; i++) {
        AsmFunc *f = &prog->funcs[i];
        uint32_t start, code_len;
        int nlocals;

        memset(&offsets, 0, sizeof offsets);
        {
            int off = 0;
            for (k = 0; k < f->ninsns; k++) {
                Insn *in = &f->insns[k];
                if (in->op < 0) {
                    if (dx_map_get(&offsets, in->label))
                        dx_throw("assembler", 0, 0, "duplicate label '%s'", in->label);
                    dx_map_set(&offsets, in->label)->ival = off;
                } else {
                    off += dx_instruction_size(in->op);
                }
            }
        }
        nlocals = compute_nlocals(f);
        start = (uint32_t)code.len;
        for (k = 0; k < f->ninsns; k++) {
            Insn *in = &f->insns[k];
            if (in->op < 0) continue;
            buf_u8(&code, (unsigned)in->op);
            if (dx_has_operand(in->op))
                buf_u32(&code, operand_value(in, &offsets, &cp, nlocals));
        }
        code_len = (uint32_t)(code.len - start);
        buf_u16(&func_table, (unsigned)cidx_s(&cp, f->name));
        buf_u8(&func_table, (unsigned)f->arity);
        buf_u16(&func_table, (unsigned)nlocals);
        buf_u32(&func_table, start);
        buf_u32(&func_table, code_len);
    }

    /* ---------- 库表(含静态内嵌数据)与原生函数表 ---------- */
    for (i = 0; i < prog->nlibs; i++) {
        LibInfo *lib = &prog->libs[i];
        buf_u16(&lib_table, (unsigned)cidx_s(&cp, lib->path));
        buf_u8(&lib_table, lib->is_static ? DEXC_LIB_STATIC : 0);
        buf_u32(&lib_table, (uint32_t)lib->data_len);
        if (lib->data_len) buf_bytes(&lib_table, lib->data, lib->data_len);
    }
    for (i = 0; i < prog->nnatives; i++) {
        NativeFunc *nf = &prog->natives[i];
        int lib_idx = -1, limit, abi_flag;
        const char *abi_name;
        for (k = 0; k < prog->nlibs; k++)
            if (!strcmp(prog->libs[k].path, nf->lib)) { lib_idx = k; break; }
        abi_flag = (nf->abi == ABI_VALUE_ARRAY) ? DEXC_NATIVE_ABI_MASK : 0;
        abi_name = (nf->abi == ABI_VALUE_ARRAY) ? "value_array" : "direct";
        limit = (nf->abi == ABI_VALUE_ARRAY) ? DX_MAX_NATIVE_ARGS : DX_MAX_NATIVE_ARITY;
        if (nf->nparams > limit) {
            dx_throw("assembler", 0, 0,
                     "native '%s' has %d parameter(s); at most %d are supported with "
                     "the '%s' ABI", nf->name, nf->nparams, limit, abi_name);
        }
        buf_u16(&native_table, (unsigned)cidx_s(&cp, nf->name));
        buf_u16(&native_table, (unsigned)lib_idx);
        buf_u8(&native_table, (unsigned)nf->nparams);
        buf_u8(&native_table, (unsigned)(nf->ret_type | abi_flag));
        for (k = 0; k < nf->nparams; k++)
            buf_u8(&native_table, (unsigned)nf->param_types[k]);
    }

    /* ---------- 组装文件 ---------- */
    buf_bytes(&header, DEXC_MAGIC, 4);
    buf_u8(&header, DEXC_VERSION);
    buf_u8(&header, 0);
    buf_u16(&header, (unsigned)cp.n);
    buf_u16(&header, (unsigned)prog->nfuncs);
    buf_u16(&header, (unsigned)prog->nlibs);
    buf_u16(&header, (unsigned)prog->nnatives);
    buf_u32(&header, (uint32_t)code.len);

    for (i = 0; i < cp.n; i++) {
        Const *c = &cp.items[i];
        buf_u8(&pool, (unsigned)c->kind);
        if (c->kind == DEXC_TAG_INT) {
            unsigned char b[8];
            uint64_t v = (uint64_t)c->ival;
            for (k = 0; k < 8; k++) b[k] = (unsigned char)((v >> (8 * k)) & 0xFF);
            buf_bytes(&pool, b, 8);
        } else if (c->kind == DEXC_TAG_FLOAT) {
            unsigned char b[8];
            uint64_t v;
            memcpy(&v, &c->fval, 8);
            for (k = 0; k < 8; k++) b[k] = (unsigned char)((v >> (8 * k)) & 0xFF);
            buf_bytes(&pool, b, 8);
        } else {
            size_t n = strlen(c->sval);
            buf_u16(&pool, (unsigned)n);
            buf_bytes(&pool, c->sval, n);
        }
    }

    /* 可选 trailer:DXRL(库的字符串释放函数)。放在文件末尾,旧 VM 只读 code_size
     * 之前的内容,所以完全无影响。 */
    {
        int any = 0;
        for (i = 0; i < prog->nlibs; i++) if (prog->libs[i].release_name) any = 1;
        if (any) {
            Buf entries;
            int n_entries = 0;
            memset(&entries, 0, sizeof entries);
            for (i = 0; i < prog->nlibs; i++) {
                MapEntry *ri;
                if (!prog->libs[i].release_name) continue;
                ri = dx_map_get(&native_index, prog->libs[i].release_name);
                if (!ri) {
                    dx_throw("assembler", 0, 0,
                             "library '%s' declares release function '%s' which is not "
                             "among its natives", prog->libs[i].path,
                             prog->libs[i].release_name);
                }
                buf_u16(&entries, (unsigned)i);
                buf_u16(&entries, (unsigned)ri->ival);
                n_entries++;
            }
            buf_bytes(&trailer, "DXRL", 4);
            buf_u16(&trailer, (unsigned)n_entries);
            buf_bytes(&trailer, entries.p, entries.len);
        }
    }

    buf_bytes(&body, header.p, header.len);
    buf_bytes(&body, pool.p, pool.len);
    buf_bytes(&body, lib_table.p, lib_table.len);
    buf_bytes(&body, native_table.p, native_table.len);
    buf_bytes(&body, func_table.p, func_table.len);
    buf_bytes(&body, code.p, code.len);
    buf_bytes(&body, trailer.p, trailer.len);
    *out_len = body.len;
    return body.p;
}

/* ================================================== 汇编文本:渲染与解析 */

static const char *code_to_type(int code)
{
    switch (code) {
    case NAT_VOID: return "v";
    case NAT_INT: return "i";
    case NAT_FLOAT: return "f";
    case NAT_STR: return "s";
    default: return "?";
    }
}

/* 汇编文本里的字符串常量:Python 版 _render_const 固定用**双引号**并转义
 * \\ " \n \t \r(注意不是 repr:repr 默认用单引号,这里不能直接用)。 */
static char *render_str_dq(const char *s)
{
    size_t i, n = strlen(s);
    char *out = dx_malloc(n * 4 + 3);
    size_t p = 0;
    out[p++] = '"';
    for (i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '\\') { out[p++] = '\\'; out[p++] = '\\'; }
        else if (ch == '"') { out[p++] = '\\'; out[p++] = '"'; }
        else if (ch == '\n') { out[p++] = '\\'; out[p++] = 'n'; }
        else if (ch == '\t') { out[p++] = '\\'; out[p++] = 't'; }
        else if (ch == '\r') { out[p++] = '\\'; out[p++] = 'r'; }
        else out[p++] = ch;
    }
    out[p++] = '"';
    out[p] = 0;
    return out;
}

static char *render_operand(const Operand *o)
{
    switch (o->kind) {
    case OA_CONST_I: return dx_aprintf("%lld", (long long)o->ival);
    case OA_CONST_F: return dx_py_repr_float(o->fval);
    case OA_CONST_S: return render_str_dq(o->s);
    case OA_LOCAL: return dx_aprintf("%%%d", o->index);
    case OA_LABEL: return dx_strdup(o->s);
    case OA_FUNC: return dx_aprintf("@%s", o->s);
    case OA_NATIVE: return dx_strdup(o->s);
    case OA_TYPE: return dx_aprintf("%s(%d)", o->s, o->nfields);
    case OA_FIELD: return dx_aprintf("%d", o->index);
    default: return dx_strdup("");
    }
}

static const char *b64chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *data, size_t n)
{
    size_t i, o = 0;
    char *out = dx_malloc((n + 2) / 3 * 4 + 1);
    for (i = 0; i < n; i += 3) {
        size_t rem = n - i;
        unsigned v = (unsigned)data[i] << 16;
        if (rem > 1) v |= (unsigned)data[i + 1] << 8;
        if (rem > 2) v |= (unsigned)data[i + 2];
        out[o++] = b64chars[(v >> 18) & 0x3F];
        out[o++] = b64chars[(v >> 12) & 0x3F];
        out[o++] = (rem > 1) ? b64chars[(v >> 6) & 0x3F] : '=';
        out[o++] = (rem > 2) ? b64chars[v & 0x3F] : '=';
    }
    out[o] = 0;
    return out;
}

char *dx_render_asm(const AssemblyProgram *prog)
{
    char **lines = NULL;
    int n = 0, cap = 0;
    Map lib_index, nat_index;
    const char *cur_abi = "direct";
    int i, k;
    char *out;
    size_t total = 0, o = 0;

    memset(&lib_index, 0, sizeof lib_index);
    memset(&nat_index, 0, sizeof nat_index);

    for (i = 0; i < prog->nlibs; i++) {
        char *line = dx_aprintf(".lib %s", render_str_dq(prog->libs[i].path));
        if (prog->libs[i].is_static) {
            char *b64 = b64_encode(prog->libs[i].data, prog->libs[i].data_len);
            line = dx_aprintf("%s static %s", line, b64);
        }
        DXV_PUSH(lines, n, cap, line);
        dx_map_set(&lib_index, prog->libs[i].path)->ival = i;
    }
    for (i = 0; i < prog->nnatives; i++) {
        NativeFunc *nf = &prog->natives[i];
        MapEntry *li = dx_map_get(&lib_index, nf->lib);
        const char *abi = (nf->abi == ABI_VALUE_ARRAY) ? "value_array" : "direct";
        char *sig;
        if (!li) {
            dx_throw("asm-text", 0, 0, "native '%s' references unknown library '%s'",
                     nf->name, nf->lib);
        }
        if (strcmp(abi, cur_abi)) {
            DXV_PUSH(lines, n, cap, dx_aprintf(".abi %s", abi));
            cur_abi = abi;
        }
        {
            char *params = dx_strdup("");
            for (k = 0; k < nf->nparams; k++)
                params = dx_aprintf("%s%s", params, code_to_type(nf->param_types[k]));
            sig = dx_aprintf("%s:%s", params, code_to_type(nf->ret_type));
        }
        DXV_PUSH(lines, n, cap,
                 dx_aprintf(".native %s %d %s", nf->name, li->ival, sig));
    }
    for (i = 0; i < prog->nnatives; i++)
        dx_map_set(&nat_index, prog->natives[i].name)->ival = i;
    for (i = 0; i < prog->nlibs; i++) {
        if (!prog->libs[i].release_name) continue;
        {
            MapEntry *ri = dx_map_get(&nat_index, prog->libs[i].release_name);
            MapEntry *li = dx_map_get(&lib_index, prog->libs[i].path);
            if (!ri) {
                dx_throw("asm-text", 0, 0,
                         "library '%s' release function '%s' is not a declared native",
                         prog->libs[i].path, prog->libs[i].release_name);
            }
            DXV_PUSH(lines, n, cap, dx_aprintf(".release %d %d", li->ival, ri->ival));
        }
    }
    for (i = 0; i < prog->nfuncs; i++) {
        AsmFunc *f = &prog->funcs[i];
        DXV_PUSH(lines, n, cap, dx_aprintf(".func %s %d", f->name, f->arity));
        for (k = 0; k < f->ninsns; k++) {
            Insn *in = &f->insns[k];
            if (in->op < 0) {
                DXV_PUSH(lines, n, cap, dx_aprintf("%s:", in->label));
                continue;
            }
            {
                const char *mn = dx_mnemonic(in->op);
                char *line = dx_aprintf("    %s", mn ? mn : "???");
                if (in->operand.kind != OA_NONE)
                    line = dx_aprintf("%s %s", line, render_operand(&in->operand));
                DXV_PUSH(lines, n, cap, line);
            }
        }
    }
    for (i = 0; i < n; i++) total += strlen(lines[i]) + 1;
    out = dx_malloc(total + 1);
    for (i = 0; i < n; i++) {
        size_t l = strlen(lines[i]);
        memcpy(out + o, lines[i], l);
        o += l;
        out[o++] = '\n';
    }
    out[o] = 0;
    return out;
}

/* ------------------------------------------------------ 解析汇编文本 */

typedef struct {
    char **items;
    int n, cap;
} SplitList;

static SplitList split_line(const char *line)
{
    SplitList sl;
    size_t i = 0, n = strlen(line);
    char *cur = dx_malloc(n + 1);
    size_t cn = 0;
    int in_str = 0;
    memset(&sl, 0, sizeof sl);
    while (i < n) {
        char ch = line[i];
        if (in_str) {
            cur[cn++] = ch;
            if (ch == '\\' && i + 1 < n) { cur[cn++] = line[i + 1]; i += 2; continue; }
            if (ch == '"') in_str = 0;
            i++;
        } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'
                   || ch == '\v' || ch == '\f') {
            if (cn) {
                cur[cn] = 0;
                DXV_PUSH(sl.items, sl.n, sl.cap, dx_strdup(cur));
                cn = 0;
            }
            i++;
        } else if (ch == '"') {
            in_str = 1;
            cur[cn++] = ch;
            i++;
        } else {
            cur[cn++] = ch;
            i++;
        }
    }
    if (cn) {
        cur[cn] = 0;
        DXV_PUSH(sl.items, sl.n, sl.cap, dx_strdup(cur));
    }
    return sl;
}

static int is_label_line(const char *line, char **out_name)
{
    size_t i = 0, n = strlen(line);
    if (n < 2 || line[n - 1] != ':') return 0;
    if (!((line[0] >= 'A' && line[0] <= 'Z') || (line[0] >= 'a' && line[0] <= 'z')
          || line[0] == '_')) return 0;
    for (i = 1; i + 1 < n; i++) {
        char c = line[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    *out_name = dx_strndup(line, n - 1);
    return 1;
}

static Operand parse_const(const char *text, int lineno)
{
    Operand o;
    memset(&o, 0, sizeof o);
    if (text[0] == '"') {
        size_t n = strlen(text), i = 1;
        char *out;
        size_t p = 0;
        if (n < 2 || text[n - 1] != '"')
            dx_throw("asm-text", lineno, 1, "invalid string constant");
        out = dx_malloc(n + 1);
        while (i + 1 < n) {
            char ch = text[i];
            if (ch == '\\' && i + 1 < n - 1) {
                char e = text[i + 1];
                switch (e) {
                case 'n': out[p++] = '\n'; break;
                case 't': out[p++] = '\t'; break;
                case 'r': out[p++] = '\r'; break;
                case '\\': out[p++] = '\\'; break;
                case '"': out[p++] = '"'; break;
                case '0': out[p++] = '\0'; break;
                default: out[p++] = e; break;
                }
                i += 2;
            } else {
                out[p++] = ch;
                i++;
            }
        }
        out[p] = 0;
        o.kind = OA_CONST_S;
        o.s = out;
        return o;
    }
    if (strpbrk(text, ".eE")) {
        char *endp = NULL;
        double d = strtod(text, &endp);
        if (!endp || *endp)
            dx_throw("asm-text", lineno, 1, "invalid constant %s",
                     dx_py_repr_str(text));
        o.kind = OA_CONST_F;
        o.fval = d;
        return o;
    }
    {
        char *endp = NULL;
        long long v = strtoll(text, &endp, 10);
        if (!endp || *endp)
            dx_throw("asm-text", lineno, 1, "invalid constant %s",
                     dx_py_repr_str(text));
        o.kind = OA_CONST_I;
        o.ival = (int64_t)v;
        return o;
    }
}

static Operand parse_operand(int op, const char *text, int lineno)
{
    Operand o;
    const char *mn = dx_mnemonic(op);
    memset(&o, 0, sizeof o);
    if (op == OP_PUSH) return parse_const(text, lineno);
    if (op == OP_LOAD || op == OP_STORE) {
        char *endp = NULL;
        long v;
        if (text[0] != '%')
            dx_throw("asm-text", lineno, 1, "invalid operand %s for '%s'",
                     dx_py_repr_str(text), mn);
        v = strtol(text + 1, &endp, 10);
        if (!endp || *endp)
            dx_throw("asm-text", lineno, 1, "invalid operand %s for '%s'",
                     dx_py_repr_str(text), mn);
        o.kind = OA_LOCAL;
        o.index = (int)v;
        return o;
    }
    if (op == OP_JMP || op == OP_JZ || op == OP_JNZ) {
        o.kind = OA_LABEL;
        o.s = dx_strdup(text);
        return o;
    }
    if (op == OP_CALL) {
        if (text[0] != '@')
            dx_throw("asm-text", lineno, 1, "invalid operand %s for '%s'",
                     dx_py_repr_str(text), mn);
        o.kind = OA_FUNC;
        o.s = dx_strdup(text + 1);
        return o;
    }
    if (op == OP_NCALL) {
        o.kind = OA_NATIVE;
        o.s = dx_strdup(text);
        return o;
    }
    if (op == OP_MAKE_OBJ) {
        size_t n = strlen(text);
        if (n > 0 && text[n - 1] == ')' && strchr(text, '(')) {
            const char *lp = strchr(text, '(');
            o.kind = OA_TYPE;
            o.s = dx_strndup(text, (size_t)(lp - text));
            o.nfields = atoi(lp + 1);
        } else {
            o.kind = OA_TYPE;
            o.s = dx_strdup(text);
            o.nfields = 0;
        }
        return o;
    }
    if (op == OP_GET_FIELD || op == OP_SETFIELD) {
        char *endp = NULL;
        long v = strtol(text, &endp, 10);
        if (!endp || *endp)
            dx_throw("asm-text", lineno, 1, "invalid operand %s for '%s'",
                     dx_py_repr_str(text), mn);
        o.kind = OA_FIELD;
        o.index = (int)v;
        return o;
    }
    dx_throw("asm-text", lineno, 1, "instruction '%s' takes no operand", mn);
    return o;
}

static int mnemonic_to_op(const char *m)
{
    int i;
    for (i = 0; i <= 0x21; i++) {
        const char *mn = dx_mnemonic(i);
        if (mn && !strcmp(mn, m)) return i;
    }
    return -1;
}

AssemblyProgram *dx_parse_asm_text(const char *text)
{
    AssemblyProgram *prog = dx_malloc(sizeof *prog);
    AsmFunc *current = NULL;
    const char *cur_abi = "direct";
    Map labels;
    char **lines = NULL;
    int n_lines = 0, cap_lines = 0;
    int lineno = 0, i;
    size_t p = 0, n = strlen(text);

    memset(prog, 0, sizeof *prog);
    memset(&labels, 0, sizeof labels);

    /* 按行切开。注意循环条件必须是"p 还没到末尾就再切一行",不能写成
     * `while (p <= n)`:那样最后会反复切出空行 —— 死循环 + 无限吃内存
     * (曾经报成 "out of memory (18446744056529682432 bytes)")。 */
    for (;;) {
        size_t start = p;
        char *raw;
        while (p < n && text[p] != '\n') p++;
        raw = dx_strndup(text + start, p - start);
        DXV_PUSH(lines, n_lines, cap_lines, raw);
        if (p >= n) break;
        p++;   /* 跳过换行 */
    }

    for (i = 0; i < n_lines; i++) {
        char *raw = lines[i];
        char *line;
        char *label_name = NULL;
        SplitList parts;
        char *head;
        char *hash;
        lineno = i + 1;
        hash = strchr(raw, '#');
        if (hash) *hash = 0;
        line = raw;
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        {
            size_t l = strlen(line);
            while (l && (line[l - 1] == ' ' || line[l - 1] == '\t' || line[l - 1] == '\r'))
                line[--l] = 0;
        }
        if (!*line) continue;

        if (is_label_line(line, &label_name)) {
            if (!current) dx_throw("asm-text", lineno, 1, "label outside function");
            if (dx_map_get(&labels, label_name))
                dx_throw("asm-text", lineno, 1, "duplicate label '%s'", label_name);
            dx_map_set(&labels, label_name);
            {
                Insn in;
                memset(&in, 0, sizeof in);
                in.op = -1;
                in.label = label_name;
                DXV_PUSH(current->insns, current->ninsns, current->capinsns, in);
            }
            continue;
        }

        parts = split_line(line);
        if (parts.n == 0) continue;
        head = parts.items[0];

        if (!strcmp(head, ".lib")) {
            if (parts.n == 2) {
                Operand o = parse_const(parts.items[1], lineno);
                LibInfo li;
                memset(&li, 0, sizeof li);
                li.path = o.s;
                DXV_PUSH(prog->libs, prog->nlibs, prog->caplibs, li);
            } else if (parts.n == 4 && !strcmp(parts.items[2], "static")) {
                const char *b = parts.items[3];
                size_t bl = strlen(b), k, outn = 0;
                unsigned char *out;
                int quad[4], qi = 0;
                Operand o;
                if (bl % 4)
                    dx_throw("asm-text", lineno, 1, "invalid base64 in static .lib");
                out = dx_malloc(bl / 4 * 3 + 4);
                for (k = 0; k < bl; k++) {
                    char c = b[k];
                    const char *pp = strchr(b64chars, c);
                    if (c == '=') quad[qi++] = -1;
                    else if (pp) quad[qi++] = (int)(pp - b64chars);
                    else dx_throw("asm-text", lineno, 1, "invalid base64 in static .lib");
                    if (qi == 4) {
                        out[outn++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
                        if (quad[2] >= 0)
                            out[outn++] =
                                (unsigned char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
                        if (quad[3] >= 0)
                            out[outn++] =
                                (unsigned char)(((quad[2] & 0x3) << 6) | quad[3]);
                        qi = 0;
                    }
                }
                o = parse_const(parts.items[1], lineno);
                {
                    LibInfo li;
                    memset(&li, 0, sizeof li);
                    li.path = o.s;
                    li.is_static = 1;
                    li.data = out;
                    li.data_len = outn;
                    DXV_PUSH(prog->libs, prog->nlibs, prog->caplibs, li);
                }
            } else {
                dx_throw("asm-text", lineno, 1,
                         "expected '.lib \"path\"' or '.lib \"path\" static <base64>'");
            }
            continue;
        }
        if (!strcmp(head, ".native")) {
            const char *name;
            long lib_idx;
            char *endp = NULL;
            int param_types[DX_MAX_NATIVE_ARGS], nparams = 0, ret_type = 0;
            NativeFunc nf;
            long k2;
            if (parts.n != 4)
                dx_throw("asm-text", lineno, 1,
                         "expected '.native NAME LIBIDX SIGNATURE'");
            name = parts.items[1];
            lib_idx = strtol(parts.items[2], &endp, 10);
            if (!endp || *endp)
                dx_throw("asm-text", lineno, 1, "invalid library index %s",
                         dx_py_repr_str(parts.items[2]));
            if (lib_idx < 0 || lib_idx >= prog->nlibs)
                dx_throw("asm-text", lineno, 1,
                         ".native '%s' references unknown library index %ld", name,
                         lib_idx);
            for (k2 = 0; k2 < prog->nnatives; k2++)
                if (!strcmp(prog->natives[k2].name, name))
                    dx_throw("asm-text", lineno, 1, "duplicate native '%s'", name);
            dx_parse_sig(parts.items[3],
                         !strcmp(cur_abi, "value_array") ? DX_MAX_NATIVE_ARGS
                                                         : DX_MAX_NATIVE_ARITY,
                         param_types, &nparams, &ret_type);
            memset(&nf, 0, sizeof nf);
            nf.name = dx_strdup(name);
            nf.lib = prog->libs[lib_idx].path;
            memcpy(nf.param_types, param_types, sizeof param_types);
            nf.nparams = nparams;
            nf.ret_type = ret_type;
            nf.abi = !strcmp(cur_abi, "value_array") ? ABI_VALUE_ARRAY : ABI_DIRECT;
            DXV_PUSH(prog->natives, prog->nnatives, prog->capnatives, nf);
            continue;
        }
        if (!strcmp(head, ".abi")) {
            if (parts.n != 2 || (strcmp(parts.items[1], "direct")
                                 && strcmp(parts.items[1], "value_array")))
                dx_throw("asm-text", lineno, 1,
                         "expected '.abi direct' or '.abi value_array'");
            cur_abi = dx_strdup(parts.items[1]);
            continue;
        }
        if (!strcmp(head, ".release")) {
            long li2, ri;
            char *endp = NULL;
            if (parts.n != 3)
                dx_throw("asm-text", lineno, 1, "expected '.release LIBIDX NATIVEIDX'");
            li2 = strtol(parts.items[1], &endp, 10);
            if (!endp || *endp)
                dx_throw("asm-text", lineno, 1, "invalid index in .release");
            ri = strtol(parts.items[2], &endp, 10);
            if (!endp || *endp)
                dx_throw("asm-text", lineno, 1, "invalid index in .release");
            if (li2 < 0 || li2 >= prog->nlibs)
                dx_throw("asm-text", lineno, 1,
                         ".release references unknown library index %ld", li2);
            if (ri < 0 || ri >= prog->nnatives)
                dx_throw("asm-text", lineno, 1,
                         ".release references unknown native index %ld", ri);
            if (!prog->natives[ri].name || !prog->natives[ri].name[0])
                dx_throw("asm-text", lineno, 1, ".release native has no name");
            prog->libs[li2].release_name = dx_strdup(prog->natives[ri].name);
            continue;
        }
        if (!strcmp(head, ".func")) {
            long arity;
            int fi2;
            char *endp = NULL;
            AsmFunc af;
            if (parts.n != 3)
                dx_throw("asm-text", lineno, 1, "expected '.func NAME ARITY'");
            arity = strtol(parts.items[2], &endp, 10);
            if (!endp || *endp)
                dx_throw("asm-text", lineno, 1, "invalid arity %s",
                         dx_py_repr_str(parts.items[2]));
            for (fi2 = 0; fi2 < prog->nfuncs; fi2++)
                if (!strcmp(prog->funcs[fi2].name, parts.items[1]))
                    dx_throw("asm-text", lineno, 1, "duplicate function '%s'",
                             parts.items[1]);
            memset(&af, 0, sizeof af);
            af.name = dx_strdup(parts.items[1]);
            af.arity = (int)arity;
            DXV_PUSH(prog->funcs, prog->nfuncs, prog->capfuncs, af);
            current = &prog->funcs[prog->nfuncs - 1];
            memset(&labels, 0, sizeof labels);
            continue;
        }

        if (!current) dx_throw("asm-text", lineno, 1, "instruction outside function");
        {
            int op = mnemonic_to_op(head);
            Insn in;
            if (op < 0) dx_throw("asm-text", lineno, 1, "unknown mnemonic '%s'", head);
            memset(&in, 0, sizeof in);
            in.op = op;
            if (dx_has_operand(op)) {
                if (parts.n != 2)
                    dx_throw("asm-text", lineno, 1,
                             "'%s' requires exactly one operand", head);
                in.operand = parse_operand(op, parts.items[1], lineno);
            } else {
                if (parts.n != 1)
                    dx_throw("asm-text", lineno, 1, "'%s' takes no operand", head);
            }
            DXV_PUSH(current->insns, current->ninsns, current->capinsns, in);
        }
    }
    return prog;
}

/* ============================================================ 反汇编器 */

typedef struct {
    char *name;
    int arity, nlocals;
    uint32_t code_off, code_len;
} FuncEntry;

static uint32_t rd32(const unsigned char *c, size_t off)
{
    return (uint32_t)c[off] | ((uint32_t)c[off + 1] << 8)
         | ((uint32_t)c[off + 2] << 16) | ((uint32_t)c[off + 3] << 24);
}

static uint16_t rd16(const unsigned char *c, size_t off)
{
    return (uint16_t)((uint32_t)c[off] | ((uint32_t)c[off + 1] << 8));
}

/* UTF-8 解码;"replace" 语义:非法字节 → U+FFFD */
static char *utf8_replace(const unsigned char *s, size_t n)
{
    char *out = dx_malloc(n * 3 + 1);
    size_t o = 0, i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int cp, need, k, ok = 1;
        if (c < 0x80) { out[o++] = (char)c; i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else { need = 0; cp = 0xFFFD; ok = 0; }
        if (ok) {
            if (i + (size_t)need >= n) { ok = 0; cp = 0xFFFD; need = 0; }
            else {
                for (k = 1; k <= need; k++) {
                    unsigned char cc = s[i + (size_t)k];
                    if ((cc & 0xC0) != 0x80) { ok = 0; cp = 0xFFFD; need = 0; break; }
                    cp = (cp << 6) | (cc & 0x3F);
                }
            }
        }
        i += (size_t)need + 1;
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    return out;
}

static const char *need_str(Const *consts, int nc, int idx, const char *what)
{
    if (idx >= nc || consts[idx].kind != DEXC_TAG_STRING) {
        char buf[128];
        snprintf(buf, sizeof buf, "bad %s index %d", what, idx);
        dx_throw("disassembler", 0, 0, "%s", buf);
    }
    return consts[idx].sval;
}

AssemblyProgram *dx_decode(const unsigned char *data, size_t len)
{
    AssemblyProgram *prog;
    size_t off = 6;
    int n_consts, n_funcs, n_libs, n_natives;
    uint32_t code_size;
    Const *consts = NULL;
    int nc = 0, capc = 0;
    FuncEntry *funcs = NULL;
    int nf = 0, capf = 0;
    const unsigned char *code;
    int i, k;

    if (len < DEXC_HEADER_SIZE)
        dx_throw("disassembler", 0, 0, "file too small / not a bytecode file");
    if (memcmp(data, DEXC_MAGIC, 4))
        dx_throw("disassembler", 0, 0, "bad magic (not a .dexbc file)");
    if (data[4] != DEXC_VERSION)
        dx_throw("disassembler", 0, 0, "unsupported version %d", data[4]);

    n_consts = rd16(data, off);
    n_funcs = rd16(data, off + 2);
    n_libs = rd16(data, off + 4);
    n_natives = rd16(data, off + 6);
    code_size = rd32(data, off + 8);
    off += 12;

    prog = dx_malloc(sizeof *prog);
    memset(prog, 0, sizeof *prog);

    /* ---------- 常量池 ---------- */
    for (i = 0; i < n_consts; i++) {
        Const c;
        memset(&c, 0, sizeof c);
        if (off >= len) dx_throw("disassembler", 0, 0, "truncated constant pool");
        c.kind = data[off++];
        if (c.kind == DEXC_TAG_INT) {
            uint64_t v = 0;
            if (off + 8 > len) dx_throw("disassembler", 0, 0, "truncated constant pool");
            for (k = 0; k < 8; k++) v |= (uint64_t)data[off + (size_t)k] << (8 * k);
            c.ival = (int64_t)v;
            off += 8;
        } else if (c.kind == DEXC_TAG_FLOAT) {
            uint64_t v = 0;
            if (off + 8 > len) dx_throw("disassembler", 0, 0, "truncated constant pool");
            for (k = 0; k < 8; k++) v |= (uint64_t)data[off + (size_t)k] << (8 * k);
            memcpy(&c.fval, &v, 8);
            off += 8;
        } else if (c.kind == DEXC_TAG_STRING) {
            int n;
            if (off + 2 > len) dx_throw("disassembler", 0, 0, "truncated constant pool");
            n = rd16(data, off);
            off += 2;
            if (off + (size_t)n > len)
                dx_throw("disassembler", 0, 0, "truncated string constant");
            c.sval = utf8_replace(data + off, (size_t)n);
            off += (size_t)n;
        } else {
            dx_throw("disassembler", 0, 0, "bad constant tag %d", c.kind);
        }
        DXV_PUSH(consts, nc, capc, c);
    }

    /* ---------- 库表(含静态内嵌数据) ---------- */
    for (i = 0; i < n_libs; i++) {
        int path_idx, flags;
        uint32_t data_len;
        LibInfo li;
        if (off + DEXC_LIB_ENTRY_SIZE > len)
            dx_throw("disassembler", 0, 0, "truncated library table");
        path_idx = rd16(data, off);
        flags = data[off + 2];
        data_len = rd32(data, off + 3);
        off += DEXC_LIB_ENTRY_SIZE;
        if (off + data_len > len)
            dx_throw("disassembler", 0, 0, "truncated static library data");
        memset(&li, 0, sizeof li);
        li.path = dx_strdup(need_str(consts, nc, path_idx,
                                     dx_aprintf("library %d path", i)));
        li.is_static = (flags & DEXC_LIB_STATIC) ? 1 : 0;
        li.data = (unsigned char *)(data + off);
        li.data_len = data_len;
        off += data_len;
        DXV_PUSH(prog->libs, prog->nlibs, prog->caplibs, li);
    }

    /* ---------- 原生函数表 ---------- */
    for (i = 0; i < n_natives; i++) {
        int name_idx, lib_idx;
        int arity, ret_raw, abi_flag, ret_type;
        NativeFunc nfn;
        char *name;
        if (off + DEXC_NATIVE_FIXED_SIZE > len)
            dx_throw("disassembler", 0, 0, "truncated native function table");
        name_idx = rd16(data, off);
        lib_idx = rd16(data, off + 2);
        arity = data[off + 4];
        ret_raw = data[off + 5];
        off += DEXC_NATIVE_FIXED_SIZE;
        abi_flag = ret_raw & DEXC_NATIVE_ABI_MASK;
        ret_type = ret_raw & ~DEXC_NATIVE_ABI_MASK;
        if (abi_flag != 0 && abi_flag != DEXC_NATIVE_ABI_MASK)
            dx_throw("disassembler", 0, 0, "native %d has unknown abi flag 0x%02x", i,
                     abi_flag);
        {
            int abi_limit = (abi_flag == DEXC_NATIVE_ABI_MASK) ? DX_MAX_NATIVE_ARGS
                                                              : DX_MAX_NATIVE_ARITY;
            if (arity > abi_limit) {
                dx_throw("disassembler", 0, 0,
                         "native %d has arity %d > %d for abi '%s'", i, arity, abi_limit,
                         abi_flag == DEXC_NATIVE_ABI_MASK ? "value_array" : "direct");
            }
        }
        if (off + (size_t)arity > len)
            dx_throw("disassembler", 0, 0, "truncated native parameter types");
        name = dx_strdup(need_str(consts, nc, name_idx,
                                 dx_aprintf("native %d name", i)));
        if (lib_idx >= prog->nlibs) {
            dx_throw("disassembler", 0, 0,
                     "native '%s' references unknown library %d", name, lib_idx);
        }
        if (ret_type > NAT_STR)
            dx_throw("disassembler", 0, 0, "native '%s' has bad return type %d", name,
                     ret_type);
        memset(&nfn, 0, sizeof nfn);
        nfn.name = name;
        nfn.lib = prog->libs[lib_idx].path;
        nfn.nparams = arity;
        for (k = 0; k < arity; k++) {
            int c = data[off + (size_t)k];
            if (c < NAT_INT || c > NAT_STR)
                dx_throw("disassembler", 0, 0, "native '%s' has bad parameter type %d",
                         name, c);
            nfn.param_types[k] = c;
        }
        nfn.ret_type = ret_type;
        nfn.abi = (abi_flag == DEXC_NATIVE_ABI_MASK) ? ABI_VALUE_ARRAY : ABI_DIRECT;
        off += (size_t)arity;
        DXV_PUSH(prog->natives, prog->nnatives, prog->capnatives, nfn);
    }

    /* ---------- 函数表 ---------- */
    for (i = 0; i < n_funcs; i++) {
        FuncEntry fe;
        if (off + DEXC_FUNC_ENTRY_SIZE > len)
            dx_throw("disassembler", 0, 0, "truncated function table");
        memset(&fe, 0, sizeof fe);
        fe.name = dx_strdup(need_str(consts, nc, rd16(data, off), "function name"));
        fe.arity = data[off + 2];
        fe.nlocals = rd16(data, off + 3);
        fe.code_off = rd32(data, off + 5);
        fe.code_len = rd32(data, off + 9);
        off += DEXC_FUNC_ENTRY_SIZE;
        DXV_PUSH(funcs, nf, capf, fe);
    }

    /* ---------- 代码段 ---------- */
    if (off + code_size > len)
        dx_throw("disassembler", 0, 0, "truncated code section");
    code = data + off;
    off += code_size;

    for (i = 0; i < nf; i++) {
        FuncEntry *fe = &funcs[i];
        uint32_t start = fe->code_off, end = fe->code_off + fe->code_len;
        uint32_t *targets = NULL;
        int nt = 0, capt = 0;
        AsmFunc af;
        size_t p;
        if (end > code_size)
            dx_throw("disassembler", 0, 0, "function '%s' code out of range", fe->name);

        /* 收集跳转目标(统一转成绝对偏移),保持升序 */
        p = start;
        while (p < end) {
            int op = code[p];
            if (!dx_mnemonic(op))
                dx_throw("disassembler", 0, 0, "unknown opcode 0x%02X", op);
            if (dx_has_operand(op)) {
                uint32_t operand = rd32(code, p + 1);
                if (op == OP_JMP || op == OP_JZ || op == OP_JNZ) {
                    uint32_t abs_t = start + operand;
                    int dup = 0, j;
                    for (k = 0; k < nt; k++) if (targets[k] == abs_t) dup = 1;
                    if (!dup) {
                        DXV_PUSH(targets, nt, capt, abs_t);
                        for (j = nt - 1; j > 0 && targets[j - 1] > targets[j]; j--) {
                            uint32_t tmp = targets[j - 1];
                            targets[j - 1] = targets[j];
                            targets[j] = tmp;
                        }
                    }
                }
                p += 5;
            } else {
                p += 1;
            }
        }

        memset(&af, 0, sizeof af);
        af.name = fe->name;
        af.arity = fe->arity;
        af.nlocals = fe->nlocals;

        p = start;
        while (p < end) {
            int j, is_label = 0;
            for (j = 0; j < nt; j++) if (targets[j] == p) is_label = 1;
            if (is_label) {
                Insn in;
                memset(&in, 0, sizeof in);
                in.op = -1;
                in.label = dx_aprintf("L%u", (unsigned)p);
                DXV_PUSH(af.insns, af.ninsns, af.capinsns, in);
            }
            {
                int op = code[p];
                Insn in;
                memset(&in, 0, sizeof in);
                in.op = op;
                if (dx_has_operand(op)) {
                    uint32_t operand = rd32(code, p + 1);
                    if (op == OP_PUSH) {
                        if (operand >= (uint32_t)nc)
                            dx_throw("disassembler", 0, 0, "PUSH index out of range");
                        if (consts[operand].kind == DEXC_TAG_INT) {
                            in.operand.kind = OA_CONST_I;
                            in.operand.ival = consts[operand].ival;
                        } else if (consts[operand].kind == DEXC_TAG_FLOAT) {
                            in.operand.kind = OA_CONST_F;
                            in.operand.fval = consts[operand].fval;
                        } else {
                            in.operand.kind = OA_CONST_S;
                            in.operand.s = consts[operand].sval;
                        }
                    } else if (op == OP_LOAD || op == OP_STORE) {
                        in.operand.kind = OA_LOCAL;
                        in.operand.index = (int)operand;
                    } else if (op == OP_JMP || op == OP_JZ || op == OP_JNZ) {
                        in.operand.kind = OA_LABEL;
                        in.operand.s = dx_aprintf("L%u", (unsigned)(start + operand));
                    } else if (op == OP_CALL) {
                        if (operand >= (uint32_t)nf)
                            dx_throw("disassembler", 0, 0, "CALL index out of range");
                        in.operand.kind = OA_FUNC;
                        in.operand.s = funcs[operand].name;
                    } else if (op == OP_NCALL) {
                        if (operand >= (uint32_t)prog->nnatives)
                            dx_throw("disassembler", 0, 0, "NCALL index out of range");
                        in.operand.kind = OA_NATIVE;
                        in.operand.s = prog->natives[operand].name;
                    } else if (op == OP_MAKE_OBJ) {
                        int type_cidx = (int)(operand >> 16);
                        int nfields = (int)(operand & 0xFFFF);
                        in.operand.kind = OA_TYPE;
                        in.operand.s = dx_strdup(
                            need_str(consts, nc, type_cidx, "type name"));
                        in.operand.nfields = nfields;
                    } else if (op == OP_GET_FIELD || op == OP_SETFIELD) {
                        in.operand.kind = OA_FIELD;
                        in.operand.index = (int)operand;
                    }
                    p += 5;
                } else {
                    p += 1;
                }
                DXV_PUSH(af.insns, af.ninsns, af.capinsns, in);
            }
        }
        /* 跳到函数末尾的兜底标签 */
        for (k = 0; k < nt; k++) {
            if (targets[k] == end) {
                Insn in;
                memset(&in, 0, sizeof in);
                in.op = -1;
                in.label = dx_aprintf("L%u", (unsigned)end);
                DXV_PUSH(af.insns, af.ninsns, af.capinsns, in);
            }
        }
        DXV_PUSH(prog->funcs, prog->nfuncs, prog->capfuncs, af);
    }

    /* ---------- 可选 trailer:DXRL ---------- */
    if (off + 6 <= len && !memcmp(data + off, "DXRL", 4)) {
        int n_rel = rd16(data, off + 4);
        size_t q = off + 6;
        for (i = 0; i < n_rel; i++) {
            int lib_idx, rel_idx;
            if (q + 4 > len) dx_throw("disassembler", 0, 0, "truncated release table");
            lib_idx = rd16(data, q);
            rel_idx = rd16(data, q + 2);
            q += 4;
            if (lib_idx >= prog->nlibs || rel_idx >= prog->nnatives)
                dx_throw("disassembler", 0, 0, "bad release table entry");
            prog->libs[lib_idx].release_name = prog->natives[rel_idx].name;
        }
    }
    return prog;
}

char *dx_disassemble(const unsigned char *data, size_t len)
{
    return dx_render_asm(dx_decode(data, len));
}
