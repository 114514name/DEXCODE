/* ============================================================================
 * dexc_def.c — .dexdef 定义文件解析(dexlang/defparser.py 的移植)+ Map 工具。
 *
 * `abi` 是**文件级**指令(可以出现在 extern 之后),所以 ABI 相关的 arity
 * 上限必须在整个文件读完后统一校验 —— 否则 `extern` 在 `abi value_array;`
 * 之前就会误报。这条在 Python 版里有注释,移植时容易漏掉。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dexc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ Map 工具 */

MapEntry *dx_map_get(Map *m, const char *key)
{
    int i;
    for (i = 0; i < m->n; i++)
        if (!strcmp(m->e[i].key, key)) return &m->e[i];
    return NULL;
}

MapEntry *dx_map_set(Map *m, const char *key)
{
    MapEntry *e = dx_map_get(m, key);
    MapEntry ne;
    if (e) return e;
    memset(&ne, 0, sizeof ne);
    ne.key = dx_strdup(key);
    ne.present = 1;
    DXV_PUSH(m->e, m->n, m->cap, ne);
    return &m->e[m->n - 1];
}

/* ------------------------------------------------------------ .dexdef */

static int type_code(const char *name)
{
    if (!strcmp(name, "int")) return NAT_INT;
    if (!strcmp(name, "float")) return NAT_FLOAT;
    if (!strcmp(name, "string")) return NAT_STR;
    if (!strcmp(name, "void")) return NAT_VOID;
    return -1;
}

/* 签名串('ii:i')里的单字母类型码 —— 与 type_code 是**两套**写法:
 * 前者用于 .dexdef 的 `a: int`,后者用于 `.native NAME IDX SIG` 与汇编文本。
 * 第一版把它们混成一个函数,于是 `.native ... siii:i` 被判成非法参数类型 's',
 * 症状是 dexc disasm 出来的文本 dexc asm 自己不认(往返在 arity>=3 处断裂)。 */
static int sig_type_code(int ch)
{
    switch (ch) {
    case 'v': return NAT_VOID;
    case 'i': return NAT_INT;
    case 'f': return NAT_FLOAT;
    case 's': return NAT_STR;
    default: return -1;
    }
}

void dx_parse_sig(const char *sig, int max_arity, int *param_types, int *nparams,
                  int *ret_type)
{
    const char *colon = strchr(sig, ':');
    const char *p;
    int n = 0, limit;
    if (!colon) {
        dx_throw("def", 0, 0, "invalid signature %s (expected e.g. 'ii:i')",
                 dx_py_repr_str(sig));
    }
    for (p = sig; p < colon; p++) {
        char ch[2];
        int code;
        ch[0] = *p;
        ch[1] = 0;
        code = sig_type_code((unsigned char)*p);
        if (code < 0 || code == NAT_VOID) {
            dx_throw("def", 0, 0, "invalid parameter type %s in signature %s",
                     dx_py_repr_str(ch), dx_py_repr_str(sig));
        }
        if (n >= DX_MAX_NATIVE_ARGS) {
            dx_throw("def", 0, 0, "invalid parameter type %s in signature %s",
                     dx_py_repr_str(ch), dx_py_repr_str(sig));
        }
        param_types[n++] = code;
    }
    {
        int rc = sig_type_code((unsigned char)colon[1]);
        if (rc < 0 || colon[1] == 0 || colon[2] != 0) {
            dx_throw("def", 0, 0, "invalid return type %s in signature %s",
                     dx_py_repr_str(colon + 1), dx_py_repr_str(sig));
        }
        *ret_type = rc;
    }
    limit = (max_arity < 0) ? DX_MAX_NATIVE_ARITY : max_arity;
    if (n > limit) {
        dx_throw("def", 0, 0,
                 "native signature %s has %d parameter(s); at most %d are supported",
                 dx_py_repr_str(sig), n, limit);
    }
    *nparams = n;
}

static Token *pk(Token *toks, int ntoks, int n)
{
    int i = n;
    return (i < ntoks) ? &toks[i] : &toks[ntoks - 1];
}

DefFile *dx_parse_def(const char *text, const char *filename)
{
    int ntoks = 0;
    Token *toks = dx_lex(text, filename, &ntoks);
    int pos = 0;
    DefFile *df = dx_malloc(sizeof *df);
    int abi_seen = 0;
    memset(df, 0, sizeof *df);
    df->abi = ABI_DIRECT;

    while (pk(toks, ntoks, pos)->kind != TK_EOF) {
        Token *t = pk(toks, ntoks, pos);
        if (t->kind == TK_REFER) {
            char *lib;
            pos++;
            if (pk(toks, ntoks, pos)->kind == TK_IDENT
                && !strcmp(pk(toks, ntoks, pos)->lexeme, "static")) {
                df->is_static = 1;
                pos++;
            }
            if (pk(toks, ntoks, pos)->kind != TK_STRING) {
                Token *b = pk(toks, ntoks, pos);
                dx_throw("def", b->line, b->col, "expected %s, got %s",
                         "library path string", dx_py_repr_str(b->lexeme));
            }
            lib = dx_strdup(pk(toks, ntoks, pos)->sval);
            pos++;
            if (pk(toks, ntoks, pos)->kind != TK_SEMI) {
                Token *b = pk(toks, ntoks, pos);
                dx_throw("def", b->line, b->col, "expected %s, got %s", "';'",
                         dx_py_repr_str(b->lexeme));
            }
            pos++;
            if (df->lib_path != NULL)
                dx_throw("def", t->line, t->col, "duplicate 'refer' in definition file");
            df->lib_path = lib;
        } else if (t->kind == TK_EXTERN) {
            NativeDecl nd;
            Token *name_tok;
            memset(&nd, 0, sizeof nd);
            pos++;   /* extern */
            if (pk(toks, ntoks, pos)->kind != TK_FUNC) {
                Token *b = pk(toks, ntoks, pos);
                dx_throw("def", b->line, b->col, "expected %s, got %s", "'func'",
                         dx_py_repr_str(b->lexeme));
            }
            pos++;
            name_tok = pk(toks, ntoks, pos);
            if (name_tok->kind != TK_IDENT) {
                dx_throw("def", name_tok->line, name_tok->col, "expected %s, got %s",
                         "function name", dx_py_repr_str(name_tok->lexeme));
            }
            nd.name = dx_strdup(name_tok->lexeme);
            pos++;
            if (pk(toks, ntoks, pos)->kind != TK_LPAREN) {
                Token *b = pk(toks, ntoks, pos);
                dx_throw("def", b->line, b->col, "expected %s, got %s", "'('",
                         dx_py_repr_str(b->lexeme));
            }
            pos++;
            if (pk(toks, ntoks, pos)->kind != TK_RPAREN) {
                /* 跳过可选的 `参数名:` */
                if (pk(toks, ntoks, pos)->kind == TK_IDENT
                    && pk(toks, ntoks, pos + 1)->kind == TK_COLON) pos += 2;
                for (;;) {
                    Token *tt = pk(toks, ntoks, pos);
                    int code;
                    if (tt->kind != TK_IDENT || type_code(tt->lexeme) < 0) {
                        dx_throw("def", tt->line, tt->col,
                                 "expected a type (int/float/string/void), got %s",
                                 dx_py_repr_str(tt->lexeme));
                    }
                    code = type_code(tt->lexeme);
                    pos++;
                    if (nd.nparams >= DX_MAX_NATIVE_ARGS) {
                        dx_throw("def", name_tok->line, name_tok->col,
                                 "native function '%s' has %d parameter(s); "
                                 "at most %d are supported",
                                 nd.name, nd.nparams + 1, DX_MAX_NATIVE_ARGS);
                    }
                    nd.param_types[nd.nparams++] = code;
                    if (pk(toks, ntoks, pos)->kind == TK_COMMA) {
                        pos++;
                        if (pk(toks, ntoks, pos)->kind == TK_IDENT
                            && pk(toks, ntoks, pos + 1)->kind == TK_COLON) pos += 2;
                        continue;
                    }
                    break;
                }
            }
            {
                Token *b = pk(toks, ntoks, pos);
                if (b->kind != TK_RPAREN) {
                    dx_throw("def", b->line, b->col, "expected %s, got %s", "')'",
                             dx_py_repr_str(b->lexeme));
                }
            }
            pos++;
            {
                Token *b = pk(toks, ntoks, pos);
                if (b->kind != TK_ARROW) {
                    dx_throw("def", b->line, b->col, "expected %s, got %s", "'->'",
                             dx_py_repr_str(b->lexeme));
                }
            }
            pos++;
            {
                Token *tt = pk(toks, ntoks, pos);
                if (tt->kind != TK_IDENT || type_code(tt->lexeme) < 0) {
                    dx_throw("def", tt->line, tt->col,
                             "expected a type (int/float/string/void), got %s",
                             dx_py_repr_str(tt->lexeme));
                }
                nd.ret_type = type_code(tt->lexeme);
                pos++;
            }
            {
                Token *b = pk(toks, ntoks, pos);
                if (b->kind != TK_SEMI) {
                    dx_throw("def", b->line, b->col, "expected %s, got %s", "';'",
                             dx_py_repr_str(b->lexeme));
                }
            }
            pos++;
            DXV_PUSH(df->natives, df->nnatives, df->capnatives, nd);
        } else if (t->kind == TK_IDENT && !strcmp(t->lexeme, "abi")) {
            Token *name_tok;
            pos++;
            name_tok = pk(toks, ntoks, pos);
            if (name_tok->kind != TK_IDENT) {
                dx_throw("def", name_tok->line, name_tok->col, "expected %s, got %s",
                         "abi name (direct / value_array)",
                         dx_py_repr_str(name_tok->lexeme));
            }
            if (strcmp(name_tok->lexeme, "direct")
                && strcmp(name_tok->lexeme, "value_array")) {
                dx_throw("def", name_tok->line, name_tok->col,
                         "unknown abi %s (expected direct or value_array)",
                         dx_py_repr_str(name_tok->lexeme));
            }
            if (abi_seen) {
                dx_throw("def", name_tok->line, name_tok->col,
                         "duplicate 'abi' in definition file");
            }
            abi_seen = 1;
            df->abi = strcmp(name_tok->lexeme, "value_array") ? ABI_DIRECT
                                                             : ABI_VALUE_ARRAY;
            pos++;
            {
                Token *b = pk(toks, ntoks, pos);
                if (b->kind != TK_SEMI) {
                    dx_throw("def", b->line, b->col, "expected %s, got %s", "';'",
                             dx_py_repr_str(b->lexeme));
                }
            }
            pos++;
        } else if (t->kind == TK_RELEASE) {
            Token *name_tok;
            pos++;
            name_tok = pk(toks, ntoks, pos);
            if (name_tok->kind != TK_IDENT) {
                dx_throw("def", name_tok->line, name_tok->col, "expected %s, got %s",
                         "release function name", dx_py_repr_str(name_tok->lexeme));
            }
            pos++;
            {
                Token *b = pk(toks, ntoks, pos);
                if (b->kind != TK_SEMI) {
                    dx_throw("def", b->line, b->col, "expected %s, got %s", "';'",
                             dx_py_repr_str(b->lexeme));
                }
            }
            pos++;
            if (df->release != NULL)
                dx_throw("def", t->line, t->col, "duplicate 'release' in definition file");
            df->release = dx_strdup(name_tok->lexeme);
        } else {
            dx_throw("def", t->line, t->col, "unexpected token %s in definition file",
                     dx_py_repr_str(t->lexeme));
        }
    }
    {
        int limit = (df->abi == ABI_VALUE_ARRAY) ? DX_MAX_NATIVE_ARGS : DX_MAX_NATIVE_ARITY;
        int i;
        for (i = 0; i < df->nnatives; i++) {
            if (df->natives[i].nparams > limit) {
                const char *hint = (df->abi == ABI_VALUE_ARRAY)
                    ? ""
                    : " (declare `abi value_array;` to allow up to 16)";
                dx_throw("def", 0, 0,
                         "native function '%s' has %d parameter(s); at most %d are "
                         "supported with the '%s' ABI%s",
                         df->natives[i].name, df->natives[i].nparams, limit,
                         df->abi == ABI_VALUE_ARRAY ? "value_array" : "direct", hint);
            }
        }
    }
    return df;
}
