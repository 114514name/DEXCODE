/* ============================================================================
 * dexc_main.c — 命令行入口。
 *
 * 用法(与 main.py 的对应子命令行为一致,便于测试逐字节对照):
 *   dexc compile <src.dex> [-o out.dexbc] [-L dir]... [--no-asm] [--rel-lib]
 *   dexc asm     <in.dxasm> [-o out.dexbc]
 *   dexc disasm  <in.dexbc> [-o out.dxasm]
 *   dexc run     <in.dexbc|in.dex> [-L dir]... [--vm path]
 *   dexc dump-tokens <src.dex>        (调试:打印 token 流)
 *   dexc version
 *
 * 出错时的输出格式与 dexlang/errors.py 的 DexError 完全一致:
 *   [phase]error at line:col: message
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dexc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <process.h>
#else
#  include <unistd.h>
#endif

jmp_buf dx_jmp_buf;

/* ------------------------------------------------------------ 小工具 */

static int has_ext(const char *p, const char *ext)
{
    size_t lp = strlen(p), le = strlen(ext);
    return lp >= le && !strcmp(p + lp - le, ext);
}

static char *derive_out(const char *path, const char *out, const char *def_ext)
{
    if (out && *out) return dx_strdup(out);
    return dx_aprintf("%s%s", dx_path_abspath_noext(path), def_ext);
}

static void write_bytes(const char *path, const unsigned char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "dexc: cannot write %s\n", path);
        exit(1);
    }
    if (n) fwrite(data, 1, n, f);
    fclose(f);
}

static void write_text(const char *path, const char *text)
{
    write_bytes(path, (const unsigned char *)text, strlen(text));
}

static char *exe_dir(void)
{
#if defined(_WIN32)
    char buf[4096];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (n == 0) return dx_path_abspath(".");
    buf[sizeof buf - 1] = 0;
    return dx_path_dirname(dx_path_abspath(buf));
#else
    return dx_path_abspath(".");
#endif
}

/* 默认 include 目录:仓库根下的 libs/。
 * dexc.exe 通常在 <root>/tools/dexc/ 下,所以先试 <root>/libs,再试 exe 同级的 libs/。 */
static char *default_lib_dir(void)
{
    char *dir = exe_dir();
    char *up2 = dx_path_normpath(dx_path_join(dx_path_join(dir, ".."), ".."));
    char *cand1 = dx_path_join(up2, "libs");
    char *cand2 = dx_path_join(dir, "libs");
    if (dx_path_isdir(cand1)) return cand1;
    if (dx_path_isdir(cand2)) return cand2;
    return NULL;
}

/* ------------------------------------------------------------ 编译 */

static int cmd_compile(int argc, char **argv)
{
    const char *src = NULL, *out = NULL;
    char **libs = NULL;
    int nlibs = 0, caplibs = 0;
    int no_asm = 0, rel_lib = 0, i;
    char *text, *asm_text, *bc_path, *asm_path;
    int ntoks = 0;
    Token *toks;
    Node *program;
    CompileUnit *unit;
    unsigned char *bc;
    size_t bc_len = 0;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if ((!strcmp(argv[i], "-L") || !strcmp(argv[i], "--lib-dir"))
                 && i + 1 < argc)
            DXV_PUSH(libs, nlibs, caplibs, dx_strdup(argv[++i]));
        else if (!strcmp(argv[i], "--no-asm")) no_asm = 1;
        else if (!strcmp(argv[i], "--rel-lib")) rel_lib = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "dexc: unknown option %s\n", argv[i]);
            return 2;
        } else src = argv[i];
    }
    if (!src) {
        fprintf(stderr, "dexc compile: missing source file\n");
        return 2;
    }
    if (nlibs == 0) {
        char *d = default_lib_dir();
        if (d) DXV_PUSH(libs, nlibs, caplibs, d);
    }

    text = dx_read_file(src, NULL);
    if (!text) {
        fprintf(stderr, "dexc: cannot read %s\n", src);
        return 1;
    }
    toks = dx_lex(text, src, &ntoks);
    program = dx_parse(toks, ntoks, src);
    unit = dx_compile(program, src, libs, nlibs, rel_lib);

    for (i = 0; i < unit->n_warn; i++)
        fprintf(stderr, "warning: %s\n", unit->warnings[i]);

    asm_text = dx_render_asm(&unit->prog);
    bc_path = derive_out(src, out, ".dexbc");
    asm_path = derive_out(src, NULL, ".dxasm");

    bc = dx_assemble(unit, &bc_len);
    write_bytes(bc_path, bc, bc_len);
    if (!no_asm) {
        write_text(asm_path, asm_text);
        printf("汇编(IR): %s\n", asm_path);
    }
    printf("字节码:   %s (%u bytes)\n", bc_path, (unsigned)bc_len);
    if (unit->prog.nnatives) {
        char *info = dx_strdup("");
        for (i = 0; i < unit->prog.nlibs; i++) {
            info = dx_aprintf("%s%s%s%s", info, i ? ", " : "",
                              dx_path_basename(unit->prog.libs[i].path),
                              unit->prog.libs[i].is_static ? "[静态]" : "");
        }
        printf("原生函数: %d 个,库: [%s]\n", unit->prog.nnatives, info);
    }
    return 0;
}

/* ------------------------------------------------------------ 汇编 / 反汇编 */

static int cmd_asm(int argc, char **argv)
{
    const char *src = NULL, *out = NULL;
    char *text, *bc_path;
    AssemblyProgram *prog;
    CompileUnit unit;
    unsigned char *bc;
    size_t bc_len = 0;
    int i;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else src = argv[i];
    }
    if (!src) {
        fprintf(stderr, "dexc asm: missing source file\n");
        return 2;
    }
    text = dx_read_file(src, NULL);
    if (!text) {
        fprintf(stderr, "dexc: cannot read %s\n", src);
        return 1;
    }
    prog = dx_parse_asm_text(text);
    memset(&unit, 0, sizeof unit);
    unit.prog = *prog;
    bc = dx_assemble(&unit, &bc_len);
    bc_path = derive_out(src, out, ".dexbc");
    write_bytes(bc_path, bc, bc_len);
    printf("字节码: %s (%u bytes)\n", bc_path, (unsigned)bc_len);
    return 0;
}

static int cmd_disasm(int argc, char **argv)
{
    const char *src = NULL, *out = NULL;
    char *asm_path;
    unsigned char *data;
    size_t len = 0;
    char *text;
    int i;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else src = argv[i];
    }
    if (!src) {
        fprintf(stderr, "dexc disasm: missing source file\n");
        return 2;
    }
    data = (unsigned char *)dx_read_file(src, &len);
    if (!data) {
        fprintf(stderr, "dexc: cannot read %s\n", src);
        return 1;
    }
    text = dx_disassemble(data, len);
    asm_path = derive_out(src, out, ".dxasm");
    write_text(asm_path, text);
    printf("汇编(IR): %s\n", asm_path);
    return 0;
}

/* ------------------------------------------------------------ 运行 */

static int cmd_run(int argc, char **argv)
{
    const char *target = NULL, *vm = NULL;
    char **libs = NULL;
    int nlibs = 0, caplibs = 0;
    char *bc_path;
    int i, rc;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--vm") && i + 1 < argc) vm = argv[++i];
        else if ((!strcmp(argv[i], "-L") || !strcmp(argv[i], "--lib-dir"))
                 && i + 1 < argc)
            DXV_PUSH(libs, nlibs, caplibs, dx_strdup(argv[++i]));
        else if (argv[i][0] == '-') {
            fprintf(stderr, "dexc: unknown option %s\n", argv[i]);
            return 2;
        } else target = argv[i];
    }
    if (!target) {
        fprintf(stderr, "dexc run: missing bytecode/source file\n");
        return 2;
    }
    if (nlibs == 0) {
        char *d = default_lib_dir();
        if (d) DXV_PUSH(libs, nlibs, caplibs, d);
    }

    bc_path = dx_path_abspath(target);
    if (has_ext(bc_path, ".dex")) {
        char *text;
        int ntoks = 0;
        Token *toks;
        Node *program;
        CompileUnit *unit;
        unsigned char *bc;
        size_t bc_len = 0;
        text = dx_read_file(bc_path, NULL);
        if (!text) {
            fprintf(stderr, "dexc: cannot read %s\n", bc_path);
            return 1;
        }
        toks = dx_lex(text, bc_path, &ntoks);
        program = dx_parse(toks, ntoks, bc_path);
        unit = dx_compile(program, bc_path, libs, nlibs, 0);
        for (i = 0; i < unit->n_warn; i++)
            fprintf(stderr, "warning: %s\n", unit->warnings[i]);
        bc = dx_assemble(unit, &bc_len);
        bc_path = dx_aprintf("%s.dexbc", dx_path_strip_ext(bc_path));
        write_bytes(bc_path, bc, bc_len);
    }

    if (!vm) {
        char *dir = exe_dir();
        char *cand1 = dx_path_join(dir, "vm.exe");
        char *cand2 = dx_path_join(
            dx_path_normpath(dx_path_join(dx_path_join(dir, ".."), "..")),
            "vm\\vm.exe");
        if (dx_path_exists(cand1)) vm = cand1;
        else if (dx_path_exists(cand2)) vm = cand2;
    }
    if (!vm) {
        fprintf(stderr, "dexc: vm.exe not found; pass --vm <path>\n");
        return 1;
    }
#if defined(_WIN32)
    {
        const char *args[3];
        args[0] = vm;
        args[1] = bc_path;
        args[2] = NULL;
        rc = (int)_spawnv(_P_WAIT, vm, args);
    }
#else
    {
        char *cmd = dx_aprintf("'%s' '%s'", vm, bc_path);
        rc = system(cmd);
    }
#endif
    return rc;
}

/* ------------------------------------------------------------ 调试命令 */

static const char *tok_kind_name(int kind)
{
    switch (kind) {
    case TK_INT: return "INT";
    case TK_FLOAT: return "FLOAT";
    case TK_STRING: return "STRING";
    case TK_IDENT: return "IDENT";
    case TK_LPAREN: return "LPAREN";
    case TK_RPAREN: return "RPAREN";
    case TK_LBRACE: return "LBRACE";
    case TK_RBRACE: return "RBRACE";
    case TK_COMMA: return "COMMA";
    case TK_SEMI: return "SEMI";
    case TK_PLUS: return "PLUS";
    case TK_MINUS: return "MINUS";
    case TK_STAR: return "STAR";
    case TK_SLASH: return "SLASH";
    case TK_PERCENT: return "PERCENT";
    case TK_EQ: return "EQ";
    case TK_EQEQ: return "EQEQ";
    case TK_NE: return "NE";
    case TK_LT: return "LT";
    case TK_LE: return "LE";
    case TK_GT: return "GT";
    case TK_GE: return "GE";
    case TK_AND: return "AND";
    case TK_OR: return "OR";
    case TK_BANG: return "BANG";
    case TK_ARROW: return "ARROW";
    case TK_COLON: return "COLON";
    case TK_DOT: return "DOT";
    case TK_LET: return "LET";
    case TK_IF: return "IF";
    case TK_ELSE: return "ELSE";
    case TK_WHILE: return "WHILE";
    case TK_FUNC: return "FUNC";
    case TK_RETURN: return "RETURN";
    case TK_PRINT: return "PRINT";
    case TK_TRUE: return "TRUE";
    case TK_FALSE: return "FALSE";
    case TK_INCLUDE: return "INCLUDE";
    case TK_REFER: return "REFER";
    case TK_EXTERN: return "EXTERN";
    case TK_RELEASE: return "RELEASE";
    case TK_TYPE: return "TYPE";
    case TK_EOF: return "EOF";
    default: return "?";
    }
}

static int cmd_dump_tokens(int argc, char **argv)
{
    const char *src;
    char *text;
    int ntoks = 0, i;
    Token *toks;
    if (argc < 1) {
        fprintf(stderr, "dexc dump-tokens: missing source file\n");
        return 2;
    }
    src = argv[0];
    text = dx_read_file(src, NULL);
    if (!text) {
        fprintf(stderr, "dexc: cannot read %s\n", src);
        return 1;
    }
    toks = dx_lex(text, src, &ntoks);
    for (i = 0; i < ntoks; i++) {
        Token *t = &toks[i];
        switch (t->kind) {
        case TK_INT:
            printf("%s %s %d:%d %lld\n", tok_kind_name(t->kind),
                   dx_py_repr_str(t->lexeme), t->line, t->col, (long long)t->ival);
            break;
        case TK_FLOAT:
            printf("%s %s %d:%d %s\n", tok_kind_name(t->kind),
                   dx_py_repr_str(t->lexeme), t->line, t->col,
                   dx_py_repr_float(t->fval));
            break;
        case TK_STRING:
            printf("%s %s %d:%d %s\n", tok_kind_name(t->kind),
                   dx_py_repr_str(t->lexeme), t->line, t->col,
                   dx_py_repr_str(t->sval));
            break;
        default:
            printf("%s %s %d:%d\n", tok_kind_name(t->kind),
                   dx_py_repr_str(t->lexeme), t->line, t->col);
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ main */

static void print_error(void)
{
    const char *phase = dx_err_phase ? dx_err_phase : "";
    if (dx_err_line > 0)
        fprintf(stderr, "[%s]error at %d:%d: %s\n", phase, dx_err_line,
                dx_err_col ? dx_err_col : 1, dx_err_msg);
    else
        fprintf(stderr, "[%s]error: %s\n", phase, dx_err_msg);
}

int main(int argc, char **argv)
{
    const char *cmd;
#if defined(_WIN32)
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    if (setjmp(dx_jmp_buf)) {
        print_error();
        return 1;
    }
    if (argc < 2) {
        fprintf(stderr,
                "dexc — DEXCODE 纯 C 工具链\n"
                "用法:\n"
                "  dexc compile <src.dex> [-o out.dexbc] [-L dir]... [--no-asm] [--rel-lib]\n"
                "  dexc asm     <in.dxasm> [-o out.dexbc]\n"
                "  dexc disasm  <in.dexbc> [-o out.dxasm]\n"
                "  dexc run     <in.dexbc|in.dex> [-L dir]... [--vm path]\n"
                "  dexc dump-tokens <src.dex>\n"
                "  dexc version\n");
        return 2;
    }
    cmd = argv[1];
    if (!strcmp(cmd, "compile")) return cmd_compile(argc - 2, argv + 2);
    if (!strcmp(cmd, "asm")) return cmd_asm(argc - 2, argv + 2);
    if (!strcmp(cmd, "disasm")) return cmd_disasm(argc - 2, argv + 2);
    if (!strcmp(cmd, "run")) return cmd_run(argc - 2, argv + 2);
    if (!strcmp(cmd, "dump-tokens")) return cmd_dump_tokens(argc - 2, argv + 2);
    if (!strcmp(cmd, "version")) {
        printf("dexc 1.0 (DEXCODE C 工具链,字节码版本 %d)\n", DEXC_VERSION);
        return 0;
    }
    fprintf(stderr, "dexc: unknown command '%s'\n", cmd);
    return 2;
}
