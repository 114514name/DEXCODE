/* ============================================================================
 * dexc_compile.c — 编译器:AST → 汇编 IR(dexlang/compiler.py 的移植)。
 *
 * 这里是整条链最容易出偏差的地方,因为字节码的**常量池顺序**、函数顺序、
 * 标签编号顺序都直接由遍历顺序决定,而"输出逐字节一致"是硬要求:
 *
 *   - 函数顺序:main 恒为 0 号,其余按源码声明顺序;语言模块(.dex)的函数
 *     追加在最后(按 include 顺序)。
 *   - 标签编号:每遇到一个 if/while/&&/|| 就 new_label() 两次,顺序即生成顺序。
 *   - 常量池顺序在汇编阶段(先库路径 → 原生名 → 函数名 → 各函数的 PUSH 常量,
 *     函数仍按 main 在前)。
 *   - `else { }` 的空块在 Python 里是**假值**,所以不生成 JMP end;这里必须用
 *     els.len > 0 而不是"有没有 else"。
 *   - 变量静态类型表要区分"键不存在"与"键存在但值为 None"(present 标志),
 *     否则 `let s = x; s = 1;` 这类序列的类型推断会与 Python 分叉,进而让
 *     `+` 走 ADD 还是 CONCAT 判断不同。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dexc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)
#  include <direct.h>
#else
#  include <unistd.h>
#endif

typedef struct {
    CompileUnit *unit;
    AsmFunc *func;
    int is_main;
    Map locals;          /* 变量名 → ival(槽号) */
    Map var_types;       /* 变量名 → sval(静态类型,可为 NULL;present 有意义) */
    int next_local;
    int label_count;
    int line;
    Map infer_stack;     /* 正在推断返回类型的函数名(防递归) */
} FuncCompiler;

/* ------------------------------------------------------------ 小工具 */

static int streq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return !strcmp(a, b);
}

static int ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcmp(s + ls - lf, suf);
}

static const char *type_name_of_code(int code)
{
    switch (code) {
    case NAT_INT: return "int";
    case NAT_FLOAT: return "float";
    case NAT_STR: return "string";
    case NAT_VOID: return "void";
    default: return "void";
    }
}

static void unit_warn(CompileUnit *u, char *msg)
{
    DXV_PUSH(u->warnings, u->n_warn, u->cap_warn, msg);
}

/* ------------------------------------------------- 文件搜索 / include 解析 */

static void push_unique(char ***arr, int *n, int *cap, char *s)
{
    int i;
    for (i = 0; i < *n; i++) if (!strcmp((*arr)[i], s)) return;
    DXV_PUSH(*arr, *n, *cap, s);
}

/* 在源目录与 include 目录(及其直接子目录)里找 <target><ext>;找不到返回 NULL。
 * 子目录必须按名字排序遍历 —— 决定命中哪一个候选,影响最终写进字节码的库路径。 */
static char *search_lang_file(const char *target, const char *source_path,
                              char **include_dirs, int n_include_dirs, const char *ext)
{
    char **cands = NULL;
    int nc = 0, capc = 0;
    char *found = NULL;
    int i;

    if (source_path) {
        char *sd = dx_path_dirname(dx_path_abspath(source_path));
        char *c = dx_path_join(sd, dx_aprintf("%s%s", target, ext));
        DXV_PUSH(cands, nc, capc, c);
    }
    for (i = 0; i < n_include_dirs; i++) {
        char *base = dx_path_abspath(include_dirs[i]);
        int nn = 0, j;
        char **names;
        push_unique(&cands, &nc, &capc,
                    dx_path_join(base, dx_aprintf("%s%s", target, ext)));
        names = dx_listdir(base, &nn);
        for (j = 0; j < nn; j++) {
            char *sub = dx_path_join(base, names[j]);
            if (dx_path_isdir(sub))
                push_unique(&cands, &nc, &capc,
                            dx_path_join(sub, dx_aprintf("%s%s", target, ext)));
        }
    }
    for (i = 0; i < nc; i++) {
        if (dx_path_exists(cands[i])) { found = dx_path_abspath(cands[i]); break; }
    }
    return found;
}

static char *resolve_def(const char *target, const char *kind, const char *source_path,
                         char **include_dirs, int n_include_dirs)
{
    if (!strcmp(kind, "refer")) {
        char *sd;
        char *path;
        if (source_path) sd = dx_path_dirname(dx_path_abspath(source_path));
        else {
            char cwd[4096];
#if defined(_WIN32)
            if (!_getcwd(cwd, (int)sizeof cwd)) cwd[0] = 0;
#else
            if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
#endif
            sd = dx_strdup(cwd);
        }
        path = dx_path_isabs(target) ? dx_strdup(target) : dx_path_join(sd, target);
        if (!ends_with(path, ".dexdef")) path = dx_aprintf("%s.dexdef", path);
        if (!dx_path_exists(path))
            dx_throw("compiler", 0, 0, "definition file not found: %s", path);
        return dx_path_abspath(path);
    }
    {
        char *p = search_lang_file(target, source_path, include_dirs, n_include_dirs,
                                   ".dexdef");
        if (p) return p;
        p = search_lang_file(target, source_path, include_dirs, n_include_dirs, ".dex");
        if (p) return p;
    }
    dx_throw("compiler", 0, 0,
             "cannot find library '%s' (searched *.dexdef / *.dex in source dir "
             "and include dirs)", target);
    return NULL;
}

/* ------------------------------------------------------------ 类型表 */

static void unit_add_type(CompileUnit *u, const char *name, Node *node)
{
    if (dx_map_get(&u->types, name))
        dx_throw("compiler", node->line, node->col, "duplicate type '%s'", name);
    dx_map_set(&u->types, name)->node = node;
    dx_map_set(&u->type_order, name);
}

static Node *type_node(CompileUnit *u, const char *name)
{
    MapEntry *e = dx_map_get(&u->types, name);
    return e ? e->node : NULL;
}

static void check_recursive_visit(CompileUnit *u, const char *tname, Map *state,
                                  const char **path, int path_len)
{
    MapEntry *st = dx_map_get(state, tname);
    int s = st ? st->ival : 0;
    Node *tn;
    int i;
    if (s == 2) return;
    if (s == 1) {
        char *chain = dx_strdup("");
        int k;
        for (k = 0; k < path_len; k++) chain = dx_aprintf("%s%s -> ", chain, path[k]);
        chain = dx_aprintf("%s%s", chain, tname);
        tn = type_node(u, tname);
        dx_throw("compiler", tn ? tn->line : 0, tn ? tn->col : 0,
                 "recursive type is not allowed: %s\n"
                 "    a value type cannot contain itself (it would have infinite size); "
                 "hold the field as a separate variable instead", chain);
    }
    dx_map_set(state, tname)->ival = 1;
    tn = type_node(u, tname);
    if (tn) {
        for (i = 0; i < tn->nfields; i++) {
            const char *ftype = tn->fields[i].type_name;
            if (type_node(u, ftype)) {
                const char **np = dx_malloc(((size_t)path_len + 1) * sizeof *np);
                int k;
                for (k = 0; k < path_len; k++) np[k] = path[k];
                np[path_len] = tname;
                check_recursive_visit(u, ftype, state, np, path_len + 1);
            }
        }
    }
    dx_map_set(state, tname)->ival = 2;
}

static void unit_check_recursive_types(CompileUnit *u)
{
    Map state;
    int i;
    memset(&state, 0, sizeof state);
    for (i = 0; i < u->type_order.n; i++)
        check_recursive_visit(u, u->type_order.e[i].key, &state, NULL, 0);
}

static int unit_field_index(CompileUnit *u, const char *tname, const char *fname,
                            Node *node)
{
    Node *tn = type_node(u, tname);
    int line = node ? node->line : 0, col = node ? node->col : 0;
    int i;
    if (!tn) dx_throw("compiler", line, col, "unknown type '%s'", tname);
    for (i = 0; i < tn->nfields; i++)
        if (!strcmp(tn->fields[i].name, fname)) return i;
    dx_throw("compiler", line, col, "type '%s' has no field '%s'", tname, fname);
    return -1;
}

/* ------------------------------------------------------------ 原生函数注册 */

static int unit_add_native(CompileUnit *u, const char *name, const char *lib_path,
                           int lib_is_static, int *param_types, int nparams,
                           int ret_type, Node *node, int abi)
{
    int idx;
    if (dx_map_get(&u->func_index, name) || dx_map_get(&u->native_index, name))
        dx_throw("compiler", node->line, node->col,
                 "duplicate or conflicting function '%s'", name);
    if (!dx_map_get(&u->lib_index, lib_path)) {
        LibInfo li;
        memset(&li, 0, sizeof li);
        li.path = dx_strdup(lib_path);
        li.is_static = lib_is_static;
        dx_map_set(&u->lib_index, lib_path)->ival = u->prog.nlibs;
        DXV_PUSH(u->prog.libs, u->prog.nlibs, u->prog.caplibs, li);
    }
    {
        NativeFunc nf;
        memset(&nf, 0, sizeof nf);
        nf.name = dx_strdup(name);
        nf.lib = dx_strdup(lib_path);
        memcpy(nf.param_types, param_types, (size_t)nparams * sizeof(int));
        nf.nparams = nparams;
        nf.ret_type = ret_type;
        nf.abi = abi;
        DXV_PUSH(u->prog.natives, u->prog.nnatives, u->prog.capnatives, nf);
    }
    idx = u->prog.nnatives - 1;
    dx_map_set(&u->native_index, name)->ival = idx;
    return idx;
}

static void load_lang_module(CompileUnit *u, const char *path, Node *node,
                             char **include_dirs, int n_include_dirs, int rel_lib);

static void load_definition(CompileUnit *u, const char *def_path, Node *node,
                            int rel_lib)
{
    char *text;
    DefFile *df;
    char *lib_dir, *abs_lib, *record_path;
    int lib_idx, i;

    if (dx_map_get(&u->loaded_defs, def_path)) return;
    dx_map_set(&u->loaded_defs, def_path);

    text = dx_read_file(def_path, NULL);
    if (!text)
        dx_throw("compiler", node->line, node->col, "cannot read definition file: %s",
                 def_path);
    df = dx_parse_def(text, def_path);
    if (df->lib_path == NULL) {
        dx_throw("compiler", node->line, node->col,
                 "definition file must contain 'refer \"...dll\"' to point to the "
                 "native library");
    }
    lib_dir = dx_path_dirname(def_path);
    abs_lib = dx_path_isabs(df->lib_path)
        ? dx_strdup(df->lib_path)
        : dx_path_normpath(dx_path_join(lib_dir, df->lib_path));
    record_path = (rel_lib && !df->is_static) ? dx_strdup(df->lib_path)
                                              : dx_path_abspath(abs_lib);
    {
        LibInfo li;
        memset(&li, 0, sizeof li);
        li.path = dx_strdup(record_path);
        li.is_static = df->is_static;
        if (df->is_static) {
            size_t len = 0;
            char *bytes = dx_read_file(dx_path_abspath(abs_lib), &len);
            if (!bytes) {
                /* DXDIFF:Python 会把 OSError 的文本(errno 文案)拼进消息,这里只给路径 */
                dx_throw("compiler", node->line, node->col,
                         "cannot read library '%s' for static linking: "
                         "No such file or directory", dx_path_abspath(abs_lib));
            }
            li.data = (unsigned char *)bytes;
            li.data_len = len;
        }
        dx_map_set(&u->lib_index, record_path)->ival = u->prog.nlibs;
        DXV_PUSH(u->prog.libs, u->prog.nlibs, u->prog.caplibs, li);
    }
    lib_idx = dx_map_get(&u->lib_index, record_path)->ival;
    for (i = 0; i < df->nnatives; i++) {
        unit_add_native(u, df->natives[i].name, record_path, df->is_static,
                        df->natives[i].param_types, df->natives[i].nparams,
                        df->natives[i].ret_type, node, df->abi);
    }
    /* 约定式释放函数:`extern func f(s: string) -> void;` 视为该库的字符串释放器。
     * 记到库表上,使其进入字节码的 DXRL trailer。 */
    for (i = 0; i < df->nnatives; i++) {
        if (df->natives[i].ret_type == NAT_VOID && df->natives[i].nparams == 1
            && df->natives[i].param_types[0] == NAT_STR) {
            LibInfo *lib = &u->prog.libs[lib_idx];
            dx_map_set(&u->lib_release, record_path)->sval = df->natives[i].name;
            lib->release_name = dx_strdup(df->natives[i].name);
            break;
        }
    }
}

static void load_lang_module(CompileUnit *u, const char *path, Node *node,
                             char **include_dirs, int n_include_dirs, int rel_lib)
{
    char *text;
    int ntoks = 0;
    Token *toks;
    Node *mod;
    int i;

    (void)node;
    if (dx_map_get(&u->loaded_defs, path)) return;
    dx_map_set(&u->loaded_defs, path);

    text = dx_read_file(path, NULL);
    if (!text) dx_throw("compiler", 0, 0, "cannot read library file: %s", path);
    toks = dx_lex(text, path, &ntoks);
    mod = dx_parse(toks, ntoks, path);

    for (i = 0; i < mod->stmts.len; i++) {
        Node *st = mod->stmts.items[i];
        if (st->kind == N_LIBREF) {
            char *dep = resolve_def(st->sval, st->name, path, include_dirs,
                                    n_include_dirs);
            if (ends_with(dep, ".dex"))
                load_lang_module(u, dep, st, include_dirs, n_include_dirs, rel_lib);
            else
                load_definition(u, dep, st, rel_lib);
        } else if (st->kind == N_TYPEDEF) {
            unit_add_type(u, st->name, st);
        } else if (st->kind == N_FUNC) {
            int k;
            for (k = 0; k < u->n_modf; k++)
                if (!strcmp(u->module_funcs[k]->name, st->name))
                    dx_throw("compiler", st->line, st->col, "duplicate function '%s'",
                             st->name);
            DXV_PUSH(u->module_funcs, u->n_modf, u->cap_modf, st);
            dx_map_set(&u->func_nodes, st->name)->node = st;
            if (st->sval && st->sval[0])
                dx_map_set(&u->func_ret_types, st->name)->sval = dx_strdup(st->sval);
        } else {
            dx_throw("compiler", st->line, st->col,
                     "library file may only contain declarations "
                     "(include/refer/type/func)");
        }
    }
}

/* ============================================================ FuncCompiler */

static void fc_emit(FuncCompiler *fc, int op, const Operand *operand)
{
    Insn in;
    memset(&in, 0, sizeof in);
    in.op = op;
    if (operand) in.operand = *operand;
    in.line = fc->line;
    DXV_PUSH(fc->func->insns, fc->func->ninsns, fc->func->capinsns, in);
}

static void fc_emit0(FuncCompiler *fc, int op) { fc_emit(fc, op, NULL); }

static void fc_emit_ci(FuncCompiler *fc, int op, int64_t v)
{
    Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OA_CONST_I;
    o.ival = v;
    fc_emit(fc, op, &o);
}

static void fc_emit_cf(FuncCompiler *fc, int op, double v)
{
    Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OA_CONST_F;
    o.fval = v;
    fc_emit(fc, op, &o);
}

static void fc_emit_cs(FuncCompiler *fc, int op, const char *v)
{
    Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OA_CONST_S;
    o.s = dx_strdup(v);
    fc_emit(fc, op, &o);
}

static void fc_emit_local(FuncCompiler *fc, int op, int idx)
{
    Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OA_LOCAL;
    o.index = idx;
    fc_emit(fc, op, &o);
}

static void fc_emit_sym(FuncCompiler *fc, int op, int kind, const char *name)
{
    Operand o;
    memset(&o, 0, sizeof o);
    o.kind = kind;
    o.s = dx_strdup(name);
    fc_emit(fc, op, &o);
}

static void fc_emit_field(FuncCompiler *fc, int op, int idx)
{
    Operand o;
    memset(&o, 0, sizeof o);
    o.kind = OA_FIELD;
    o.index = idx;
    fc_emit(fc, op, &o);
}

static char *fc_new_label(FuncCompiler *fc)
{
    char *name = dx_aprintf("L%s_%d", fc->func->name, fc->label_count);
    fc->label_count++;
    return name;
}

static void fc_place_label(FuncCompiler *fc, const char *name)
{
    Insn in;
    memset(&in, 0, sizeof in);
    in.op = -1;
    in.label = dx_strdup(name);
    in.line = fc->line;
    DXV_PUSH(fc->func->insns, fc->func->ninsns, fc->func->capinsns, in);
}

static void fc_declare(FuncCompiler *fc, const char *name, Node *node)
{
    if (dx_map_get(&fc->locals, name))
        dx_throw("compiler", node->line, node->col, "duplicate variable '%s'", name);
    dx_map_set(&fc->locals, name)->ival = fc->next_local++;
}

static int fc_local(FuncCompiler *fc, const char *name, Node *node)
{
    MapEntry *e = dx_map_get(&fc->locals, name);
    if (!e) dx_throw("compiler", node->line, node->col, "undefined variable '%s'", name);
    return e->ival;
}

static void fc_init(FuncCompiler *fc, CompileUnit *u, AsmFunc *func, StrList *params,
                    Node *func_node, int is_main)
{
    int i;
    memset(fc, 0, sizeof *fc);
    fc->unit = u;
    fc->func = func;
    fc->is_main = is_main;
    for (i = 0; params && i < params->len; i++)
        fc_declare(fc, params->items[i], func_node);
    func->nlocals = fc->next_local;
}

/* ------------------------------------------------------------ 类型推断 */

static const char *fc_infer(FuncCompiler *fc, Node *node);

static void collect_var_types(FuncCompiler *fc, NodeList *nodes)
{
    int i;
    for (i = 0; i < nodes->len; i++) {
        Node *st = nodes->items[i];
        if (st->kind == N_LET) {
            dx_map_set(&fc->var_types, st->name)->sval = (char *)fc_infer(fc, st->value);
        } else if (st->kind == N_ASSIGN) {
            const char *t = fc_infer(fc, st->value);
            MapEntry *e = dx_map_get(&fc->var_types, st->name);
            if (e && !streq(e->sval, t)) e->sval = NULL;
            else dx_map_set(&fc->var_types, st->name)->sval = (char *)t;
        }
        if (st->kind == N_IF) {
            collect_var_types(fc, &st->stmts);
            collect_var_types(fc, &st->els);
        } else if (st->kind == N_WHILE) {
            collect_var_types(fc, &st->stmts);
        }
    }
}

static void infer_return_walk(FuncCompiler *fc, NodeList *nodes, StrList *types)
{
    int i;
    for (i = 0; i < nodes->len; i++) {
        Node *st = nodes->items[i];
        if (st->kind == N_RETURN) {
            const char *t = st->value ? fc_infer(fc, st->value) : "int";
            DXV_PUSH(types->items, types->len, types->cap, (char *)t);
        }
        if (st->kind == N_IF) {
            infer_return_walk(fc, &st->stmts, types);
            infer_return_walk(fc, &st->els, types);
        } else if (st->kind == N_WHILE) {
            infer_return_walk(fc, &st->stmts, types);
        }
    }
}

static const char *fc_infer_return_type(FuncCompiler *fc, Node *fnode)
{
    const char *name = fnode->name;
    StrList types;
    const char *known = NULL;
    int i, n_known = 0, has_none = 0;
    memset(&types, 0, sizeof types);
    if (name && dx_map_get(&fc->infer_stack, name)) return NULL;
    if (name) dx_map_set(&fc->infer_stack, name);
    infer_return_walk(fc, &fnode->stmts, &types);
    for (i = 0; i < types.len; i++) {
        const char *t = types.items[i];
        if (!t) { has_none = 1; continue; }
        if (n_known == 0) { known = t; n_known = 1; }
        else if (!streq(known, t)) n_known++;
    }
    if (n_known == 1 && !has_none) return known;
    return NULL;
}

static const char *fc_infer(FuncCompiler *fc, Node *node)
{
    switch (node->kind) {
    case N_LITERAL:
        if (node->lit_kind == LIT_STR) return "string";
        if (node->lit_kind == LIT_FLOAT) return "float";
        return "int";
    case N_NAME: {
        MapEntry *e = dx_map_get(&fc->var_types, node->name);
        return e ? e->sval : NULL;
    }
    case N_STRUCTLIT:
        return node->name;
    case N_GETFIELD: {
        const char *tname = fc_infer(fc, node->obj);
        Node *tn = tname ? type_node(fc->unit, tname) : NULL;
        int i;
        if (!tn) return NULL;
        for (i = 0; i < tn->nfields; i++)
            if (!strcmp(tn->fields[i].name, node->name)) return tn->fields[i].type_name;
        return NULL;
    }
    case N_UNARYOP:
        return fc_infer(fc, node->value);
    case N_BINOP:
        if (!strcmp(node->op, "+")) {
            const char *lt = fc_infer(fc, node->left);
            const char *rt = fc_infer(fc, node->right);
            if (streq(lt, "string") || streq(rt, "string")) return "string";
            if (streq(lt, "float") || streq(rt, "float")) return "float";
            if (streq(lt, "int") && streq(rt, "int")) return "int";
            return NULL;
        }
        return NULL;
    case N_CALL: {
        MapEntry *rt = dx_map_get(&fc->unit->func_ret_types, node->name);
        MapEntry *ni;
        if (rt && rt->sval && rt->sval[0]) return rt->sval;
        ni = dx_map_get(&fc->unit->native_index, node->name);
        if (ni) return type_name_of_code(fc->unit->prog.natives[ni->ival].ret_type);
        return NULL;
    }
    default:
        return NULL;
    }
}

/* ------------------------------------------------------------ 语句编译 */

static void fc_stmts(FuncCompiler *fc, NodeList *nodes);
static void fc_stmt(FuncCompiler *fc, Node *node);
static void fc_expr(FuncCompiler *fc, Node *node);

static void fc_stmts(FuncCompiler *fc, NodeList *nodes)
{
    int i;
    for (i = 0; i < nodes->len; i++) {
        Node *s = nodes->items[i];
        fc_stmt(fc, s);
        if (s->kind == N_RETURN && i + 1 < nodes->len) {
            Node *nxt = nodes->items[i + 1];
            unit_warn(fc->unit, dx_aprintf(
                "unreachable statement at %d:%d (after 'return')", nxt->line, nxt->col));
        }
    }
}

static void fc_default_value(FuncCompiler *fc, const char *ftype, Node *node)
{
    if (!strcmp(ftype, "int") || !strcmp(ftype, "bool")) { fc_emit_ci(fc, OP_PUSH, 0); return; }
    if (!strcmp(ftype, "float")) { fc_emit_cf(fc, OP_PUSH, 0.0); return; }
    if (!strcmp(ftype, "string")) { fc_emit_cs(fc, OP_PUSH, ""); return; }
    dx_throw("compiler", node->line, node->col,
             "field of custom type '%s' must be provided explicitly", ftype);
}

static void fc_check_field_assign(FuncCompiler *fc, const char *fname, const char *ftype,
                                  Node *value_node, Node *node)
{
    const char *vtype = fc_infer(fc, value_node);
    if (!vtype) return;
    if (streq(vtype, ftype)) return;
    if (!strcmp(ftype, "float") && !strcmp(vtype, "int")) return;
    if ((!strcmp(ftype, "int") || !strcmp(ftype, "bool"))
        && (!strcmp(vtype, "int") || !strcmp(vtype, "bool"))) return;
    if (type_node(fc->unit, vtype) || type_node(fc->unit, ftype)) {
        dx_throw("compiler", node->line, node->col,
                 "cannot assign a value of type '%s' to field '%s' of type '%s'",
                 vtype, fname, ftype);
    }
    if ((!strcmp(ftype, "int") || !strcmp(ftype, "float") || !strcmp(ftype, "string"))
        && (!strcmp(vtype, "int") || !strcmp(vtype, "float")
            || !strcmp(vtype, "string"))) {
        dx_throw("compiler", node->line, node->col,
                 "cannot assign a value of type '%s' to field '%s' of type '%s'",
                 vtype, fname, ftype);
    }
}

static void fc_check_native_literal(FuncCompiler *fc, const char *fname, NativeFunc *native,
                                    int idx, Node *lit)
{
    int ptype = native->param_types[idx];
    int argno = idx + 1;
    (void)fc;
    if (ptype == NAT_STR) {
        if (lit->lit_kind != LIT_STR) {
            const char *vt = (lit->lit_kind == LIT_INT) ? "int" : "float";
            dx_throw("compiler", lit->line, lit->col,
                     "argument %d of native '%s' expects a string, got a %s literal",
                     argno, fname, vt);
        }
    } else if (ptype == NAT_INT) {
        if (lit->lit_kind == LIT_STR) {
            dx_throw("compiler", lit->line, lit->col,
                     "argument %d of native '%s' expects an int, got a string literal",
                     argno, fname);
        }
        if (lit->lit_kind == LIT_FLOAT) {
            double v = lit->fval;
            int non_integral = 0;
            if (isinf(v) || isnan(v)) non_integral = 1;
            else if (v != (double)(int64_t)v) non_integral = 1;
            if (non_integral) {
                dx_throw("compiler", lit->line, lit->col,
                         "argument %d of native '%s' expects an int, got non-integral "
                         "float literal %s", argno, fname, dx_py_repr_float(v));
            }
        }
    } else if (ptype == NAT_FLOAT) {
        if (lit->lit_kind == LIT_STR) {
            dx_throw("compiler", lit->line, lit->col,
                     "argument %d of native '%s' expects a number, got a string literal",
                     argno, fname);
        }
    }
}

static void fc_call_builtin(FuncCompiler *fc, Node *node)
{
    int i, nargs;
    if (node->args.len < 1) {
        dx_throw("compiler", node->line, node->col,
                 "call() expects at least a function name as its first argument");
    }
    nargs = node->args.len - 1;
    for (i = 1; i < node->args.len; i++) fc_expr(fc, node->args.items[i]);
    fc_expr(fc, node->args.items[0]);
    fc_emit_ci(fc, OP_PUSH, nargs);
    fc_emit0(fc, OP_CALL_NAME);
}

static void fc_call(FuncCompiler *fc, Node *node)
{
    MapEntry *fi;
    MapEntry *ni;
    if (!strcmp(node->name, "call")) { fc_call_builtin(fc, node); return; }

    fi = dx_map_get(&fc->unit->func_index, node->name);
    if (fi) {
        AsmFunc *callee = &fc->unit->prog.funcs[fi->ival];
        int i;
        if (node->args.len != callee->arity) {
            dx_throw("compiler", node->line, node->col,
                     "function '%s' expects %d argument(s), got %d",
                     node->name, callee->arity, node->args.len);
        }
        for (i = 0; i < node->args.len; i++) fc_expr(fc, node->args.items[i]);
        fc_emit_sym(fc, OP_CALL, OA_FUNC, node->name);
        return;
    }
    ni = dx_map_get(&fc->unit->native_index, node->name);
    if (ni) {
        NativeFunc *native = &fc->unit->prog.natives[ni->ival];
        int i;
        if (node->args.len != native->nparams) {
            dx_throw("compiler", node->line, node->col,
                     "native function '%s' expects %d argument(s), got %d",
                     node->name, native->nparams, node->args.len);
        }
        for (i = 0; i < node->args.len; i++)
            if (node->args.items[i]->kind == N_LITERAL)
                fc_check_native_literal(fc, node->name, native, i, node->args.items[i]);
        for (i = 0; i < node->args.len; i++) fc_expr(fc, node->args.items[i]);
        fc_emit_sym(fc, OP_NCALL, OA_NATIVE, node->name);
        return;
    }
    dx_throw("compiler", node->line, node->col, "undefined function '%s'", node->name);
}

static void fc_binop(FuncCompiler *fc, Node *node)
{
    const char *op = node->op;
    if (!strcmp(op, "&&")) {
        char *false_l = fc_new_label(fc), *end_l = fc_new_label(fc);
        fc_expr(fc, node->left);
        fc_emit_sym(fc, OP_JZ, OA_LABEL, false_l);
        fc_expr(fc, node->right);
        fc_emit_sym(fc, OP_JZ, OA_LABEL, false_l);
        fc_emit_ci(fc, OP_PUSH, 1);
        fc_emit_sym(fc, OP_JMP, OA_LABEL, end_l);
        fc_place_label(fc, false_l);
        fc_emit_ci(fc, OP_PUSH, 0);
        fc_place_label(fc, end_l);
    } else if (!strcmp(op, "||")) {
        char *true_l = fc_new_label(fc), *end_l = fc_new_label(fc);
        fc_expr(fc, node->left);
        fc_emit_sym(fc, OP_JNZ, OA_LABEL, true_l);
        fc_expr(fc, node->right);
        fc_emit_sym(fc, OP_JNZ, OA_LABEL, true_l);
        fc_emit_ci(fc, OP_PUSH, 0);
        fc_emit_sym(fc, OP_JMP, OA_LABEL, end_l);
        fc_place_label(fc, true_l);
        fc_emit_ci(fc, OP_PUSH, 1);
        fc_place_label(fc, end_l);
    } else {
        int opcode = -1;
        if (!strcmp(op, "+")) {
            if (streq(fc_infer(fc, node->left), "string")
                || streq(fc_infer(fc, node->right), "string")) {
                fc_expr(fc, node->left);
                fc_expr(fc, node->right);
                fc_emit0(fc, OP_CONCAT);
                return;
            }
            opcode = OP_ADD;
        } else if (!strcmp(op, "-")) opcode = OP_SUB;
        else if (!strcmp(op, "*")) opcode = OP_MUL;
        else if (!strcmp(op, "/")) opcode = OP_DIV;
        else if (!strcmp(op, "%")) opcode = OP_MOD;
        else if (!strcmp(op, "==")) opcode = OP_EQ;
        else if (!strcmp(op, "!=")) opcode = OP_NE;
        else if (!strcmp(op, "<")) opcode = OP_LT;
        else if (!strcmp(op, "<=")) opcode = OP_LE;
        else if (!strcmp(op, ">")) opcode = OP_GT;
        else if (!strcmp(op, ">=")) opcode = OP_GE;
        if (opcode < 0)
            dx_throw("compiler", node->line, node->col,
                     "unsupported binary operator '%s'", op);
        if ((!strcmp(op, "/") || !strcmp(op, "%"))
            && node->right->kind == N_LITERAL
            && ((node->right->lit_kind == LIT_INT && node->right->ival == 0)
                || (node->right->lit_kind == LIT_FLOAT && node->right->fval == 0.0))) {
            dx_throw("compiler", node->line, node->col,
                     "division/modulo by literal zero (%s 0)", op);
        }
        fc_expr(fc, node->left);
        fc_expr(fc, node->right);
        fc_emit0(fc, opcode);
    }
}

static void fc_expr(FuncCompiler *fc, Node *node)
{
    fc->line = node->line;
    switch (node->kind) {
    case N_LITERAL:
        if (node->lit_kind == LIT_STR) fc_emit_cs(fc, OP_PUSH, node->sval);
        else if (node->lit_kind == LIT_FLOAT) fc_emit_cf(fc, OP_PUSH, node->fval);
        else fc_emit_ci(fc, OP_PUSH, node->ival);
        return;
    case N_NAME:
        fc_emit_local(fc, OP_LOAD, fc_local(fc, node->name, node));
        return;
    case N_STRUCTLIT: {
        Node *tn = type_node(fc->unit, node->name);
        int i;
        if (!tn)
            dx_throw("compiler", node->line, node->col, "unknown type '%s'", node->name);
        for (i = 0; i < tn->nfields; i++) {
            const char *fname = tn->fields[i].name;
            Node *given = NULL;
            int k;
            for (k = 0; k < node->nfields; k++)
                if (!strcmp(node->fields[k].name, fname)) {
                    given = node->fields[k].value;
                    break;
                }
            if (given) fc_expr(fc, given);
            else fc_default_value(fc, tn->fields[i].type_name, node);
        }
        {
            Operand o;
            memset(&o, 0, sizeof o);
            o.kind = OA_TYPE;
            o.s = dx_strdup(node->name);
            o.nfields = tn->nfields;
            fc_emit(fc, OP_MAKE_OBJ, &o);
        }
        return;
    }
    case N_GETFIELD: {
        const char *tname;
        int idx;
        fc_expr(fc, node->obj);
        tname = fc_infer(fc, node->obj);
        if (!tname || !type_node(fc->unit, tname)) {
            dx_throw("compiler", node->line, node->col,
                     "cannot determine type of field '%s'", node->name);
        }
        idx = unit_field_index(fc->unit, tname, node->name, node);
        fc_emit_field(fc, OP_GET_FIELD, idx);
        return;
    }
    case N_BINOP:
        fc_binop(fc, node);
        return;
    case N_UNARYOP:
        if (!strcmp(node->op, "!")) {
            fc_expr(fc, node->value);
            fc_emit0(fc, OP_NOT);
        } else if (!strcmp(node->op, "-")) {
            fc_expr(fc, node->value);
            fc_emit0(fc, OP_NEG);
        } else {
            dx_throw("compiler", node->line, node->col,
                     "unsupported unary operator '%s'", node->op);
        }
        return;
    case N_CALL:
        fc_call(fc, node);
        return;
    default:
        dx_throw("compiler", node->line, node->col, "unsupported expression");
    }
}

static void fc_stmt(FuncCompiler *fc, Node *node)
{
    fc->line = node->line;
    switch (node->kind) {
    case N_LET: {
        MapEntry *e;
        fc_expr(fc, node->value);
        fc_declare(fc, node->name, node);
        e = dx_map_set(&fc->var_types, node->name);
        e->sval = (char *)fc_infer(fc, node->value);
        fc_emit_local(fc, OP_STORE, fc_local(fc, node->name, node));
        return;
    }
    case N_ASSIGN: {
        int slot = fc_local(fc, node->name, node);
        MapEntry *e;
        fc_expr(fc, node->value);
        e = dx_map_set(&fc->var_types, node->name);
        e->sval = (char *)fc_infer(fc, node->value);
        fc_emit_local(fc, OP_STORE, slot);
        return;
    }
    case N_SETFIELD: {
        const char *tname = fc_infer(fc, node->obj);
        Node *tn = tname ? type_node(fc->unit, tname) : NULL;
        int idx;
        if (!tn) {
            dx_throw("compiler", node->line, node->col,
                     "cannot determine type of field '%s'", node->name);
        }
        idx = unit_field_index(fc->unit, tname, node->name, node);
        fc_check_field_assign(fc, node->name, tn->fields[idx].type_name, node->value, node);
        fc_expr(fc, node->obj);
        fc_expr(fc, node->value);
        fc_emit_field(fc, OP_SETFIELD, idx);
        return;
    }
    case N_PRINT: {
        int i;
        for (i = 0; i < node->exprs.len; i++) {
            fc_expr(fc, node->exprs.items[i]);
            fc_emit0(fc, OP_PRINT);
        }
        return;
    }
    case N_IF: {
        char *else_label = fc_new_label(fc);
        char *end_label = fc_new_label(fc);
        fc_expr(fc, node->value);
        fc_emit_sym(fc, OP_JZ, OA_LABEL, else_label);
        fc_stmts(fc, &node->stmts);
        if (node->els.len > 0) {          /* Python: els 为空列表是假值 */
            fc_emit_sym(fc, OP_JMP, OA_LABEL, end_label);
            fc_place_label(fc, else_label);
            fc_stmts(fc, &node->els);
            fc_place_label(fc, end_label);
        } else {
            fc_place_label(fc, else_label);
        }
        return;
    }
    case N_WHILE: {
        char *loop_label = fc_new_label(fc);
        char *end_label = fc_new_label(fc);
        fc_place_label(fc, loop_label);
        fc_expr(fc, node->value);
        fc_emit_sym(fc, OP_JZ, OA_LABEL, end_label);
        fc_stmts(fc, &node->stmts);
        fc_emit_sym(fc, OP_JMP, OA_LABEL, loop_label);
        fc_place_label(fc, end_label);
        return;
    }
    case N_RETURN:
        if (fc->is_main) {
            dx_throw("compiler", node->line, node->col,
                     "'return' is only allowed inside a function");
        }
        if (node->value) fc_expr(fc, node->value);
        else fc_emit_ci(fc, OP_PUSH, 0);
        fc_emit0(fc, OP_RET);
        return;
    case N_EXPRSTMT:
        fc_expr(fc, node->value);
        fc_emit0(fc, OP_POP);
        return;
    case N_LIBREF:
        dx_throw("compiler", node->line, node->col,
                 "'include'/'refer' are only allowed at the top level");
        return;
    case N_TYPEDEF:
        dx_throw("compiler", node->line, node->col,
                 "'type' definitions are only allowed at the top level");
        return;
    default:
        dx_throw("compiler", node->line, node->col, "unsupported statement");
    }
}

static void fc_finish_function(FuncCompiler *fc)
{
    Insn *last;
    fc->func->nlocals = fc->next_local;
    last = fc->func->ninsns ? &fc->func->insns[fc->func->ninsns - 1] : NULL;
    if (last && last->op == OP_RET) return;
    fc_emit_ci(fc, OP_PUSH, 0);
    fc_emit0(fc, OP_RET);
}

static void fc_finish_main(FuncCompiler *fc)
{
    fc->func->nlocals = fc->next_local;
    fc_emit0(fc, OP_HALT);
}

/* ============================================================ 顶层编译 */

CompileUnit *dx_compile(Node *program, const char *source_path,
                        char **include_dirs, int n_include_dirs, int rel_lib)
{
    CompileUnit *u = dx_malloc(sizeof *u);
    Map seen;
    Node **decls = NULL;
    int n_decls = 0, cap_decls = 0;
    int i;

    memset(u, 0, sizeof *u);
    memset(&seen, 0, sizeof seen);
    if (n_include_dirs < 0) n_include_dirs = 0;

    /* 0) 自定义类型定义(先全部收集,供全程序引用) */
    for (i = 0; i < program->stmts.len; i++) {
        Node *s = program->stmts.items[i];
        if (s->kind == N_TYPEDEF) unit_add_type(u, s->name, s);
    }
    unit_check_recursive_types(u);

    /* 1) 库引入 + 函数声明(main 恒为 0 号,支持前向引用/递归) */
    dx_map_set(&seen, "main");
    for (i = 0; i < program->stmts.len; i++) {
        Node *s = program->stmts.items[i];
        if (s->kind == N_LIBREF) {
            char *def_path = resolve_def(s->sval, s->name, source_path, include_dirs,
                                         n_include_dirs);
            if (ends_with(def_path, ".dex"))
                load_lang_module(u, def_path, s, include_dirs, n_include_dirs, rel_lib);
            else
                load_definition(u, def_path, s, rel_lib);
        } else if (s->kind == N_FUNC) {
            if (dx_map_get(&seen, s->name))
                dx_throw("compiler", s->line, s->col, "duplicate function '%s'", s->name);
            dx_map_set(&seen, s->name);
            DXV_PUSH(decls, n_decls, cap_decls, s);
            dx_map_set(&u->func_nodes, s->name)->node = s;
            if (s->sval && s->sval[0])
                dx_map_set(&u->func_ret_types, s->name)->sval = dx_strdup(s->sval);
        }
    }
    for (i = 0; i < u->n_modf; i++) {
        Node *mf = u->module_funcs[i];
        if (dx_map_get(&seen, mf->name))
            dx_throw("compiler", mf->line, mf->col, "duplicate function '%s'", mf->name);
        dx_map_set(&seen, mf->name);
        DXV_PUSH(decls, n_decls, cap_decls, mf);
    }

    /* 函数表:main 在前 */
    {
        AsmFunc mf;
        memset(&mf, 0, sizeof mf);
        mf.name = dx_strdup("main");
        mf.arity = 0;
        DXV_PUSH(u->prog.funcs, u->prog.nfuncs, u->prog.capfuncs, mf);
    }
    for (i = 0; i < n_decls; i++) {
        AsmFunc af;
        memset(&af, 0, sizeof af);
        af.name = dx_strdup(decls[i]->name);
        af.arity = decls[i]->params.len;
        DXV_PUSH(u->prog.funcs, u->prog.nfuncs, u->prog.capfuncs, af);
    }
    for (i = 0; i < u->prog.nfuncs; i++)
        dx_map_set(&u->func_index, u->prog.funcs[i].name)->ival = i;

    /* 2) 预跑:推断没有返回类型标注的函数的返回类型。必须在编译任何函数体之前
     *    完成 —— infer() 依赖 func_ret_types 决定 `+` 走 CONCAT 还是 ADD。 */
    for (i = 0; i < n_decls; i++) {
        Node *d = decls[i];
        FuncCompiler probe;
        AsmFunc tmp;
        const char *rt;
        int k;
        if (d->sval && d->sval[0]) continue;
        memset(&tmp, 0, sizeof tmp);
        tmp.name = d->name;
        tmp.arity = d->params.len;
        fc_init(&probe, u, &tmp, &d->params, d, 0);
        for (k = 0; k < d->params.len && k < d->param_types.len; k++)
            if (d->param_types.items[k][0])
                dx_map_set(&probe.var_types, d->params.items[k])->sval =
                    d->param_types.items[k];
        collect_var_types(&probe, &d->stmts);
        rt = fc_infer_return_type(&probe, d);
        if (rt) dx_map_set(&u->func_ret_types, d->name)->sval = (char *)rt;
    }

    /* 3) 编译各具名函数 */
    for (i = 0; i < n_decls; i++) {
        Node *d = decls[i];
        FuncCompiler fc;
        int k;
        fc_init(&fc, u, &u->prog.funcs[i + 1], &d->params, d, 0);
        for (k = 0; k < d->params.len && k < d->param_types.len; k++)
            if (d->param_types.items[k][0])
                dx_map_set(&fc.var_types, d->params.items[k])->sval =
                    d->param_types.items[k];
        fc_stmts(&fc, &d->stmts);
        fc_finish_function(&fc);
    }

    /* 4) 编译 main(顶层非函数语句;include/refer/type 已处理) */
    {
        FuncCompiler fc;
        NodeList top;
        memset(&top, 0, sizeof top);
        for (i = 0; i < program->stmts.len; i++) {
            Node *s = program->stmts.items[i];
            if (s->kind == N_FUNC || s->kind == N_LIBREF || s->kind == N_TYPEDEF) continue;
            DXV_PUSH(top.items, top.len, top.cap, s);
        }
        fc_init(&fc, u, &u->prog.funcs[0], NULL, NULL, 1);
        fc_stmts(&fc, &top);
        fc_finish_main(&fc);
    }
    return u;
}
