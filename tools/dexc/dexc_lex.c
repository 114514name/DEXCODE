/* ============================================================================
 * dexc_lex.c — 词法分析器:源码 → Token 流(dexlang/lexer.py 的逐行对照移植)。
 *
 * 唯一的实质差别是字符分类:Python 的 str.isalpha()/isalnum()/isdigit() 是
 * Unicode 语义,所以中文标识符(`let 玩家 = 1;`)在 Python 版里是合法的。
 * C 版按 UTF-8 解码后用一个范围表近似同样的判断(见 cp_is_alpha/cp_is_alnum),
 * 覆盖 CJK/假名/谚文/全角字母与拉丁扩展;全角数字这类病态输入会有差异,
 * 但两边都会报错(DXDIFF,记录在 docs/DEXGAME_DESIGN.md)。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dexc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *src;
    size_t len, pos;
    int line, col;
    const char *filename;
} Lex;

/* ------------------------------------------------------------ 字符分类 */

/* UTF-8:取 pos 处的码点(0 = EOF),并给出该码点占用的字节数 */
static int cp_at(const char *s, size_t len, size_t pos, size_t *cp_len)
{
    unsigned char c;
    int need, cp, k;
    if (pos >= len) { *cp_len = 0; return 0; }
    c = (unsigned char)s[pos];
    if (c < 0x80) { *cp_len = 1; return c; }
    if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
    else { *cp_len = 1; return 0xFFFD; }
    if (pos + (size_t)need >= len) { *cp_len = 1; return 0xFFFD; }
    for (k = 1; k <= need; k++) {
        unsigned char cc = (unsigned char)s[pos + (size_t)k];
        if ((cc & 0xC0) != 0x80) { *cp_len = 1; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *cp_len = (size_t)need + 1;
    return cp;
}

static int is_ascii_digit(int c) { return c >= '0' && c <= '9'; }
static int is_ascii_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* 近似 Python 的 isalpha();覆盖现实中会出现在标识符里的非 ASCII 字母。 */
static int cp_is_alpha(int c)
{
    if (c < 0x80) return is_ascii_alpha(c);
    if (c >= 0x00C0 && c <= 0x024F) return 1;   /* 拉丁扩展 */
    if (c >= 0x0370 && c <= 0x03FF) return 1;   /* 希腊 */
    if (c >= 0x0400 && c <= 0x04FF) return 1;   /* 西里尔 */
    if (c >= 0x0530 && c <= 0x058F) return 1;   /* 亚美尼亚 */
    if (c >= 0x05D0 && c <= 0x05EA) return 1;   /* 希伯来 */
    if (c >= 0x0620 && c <= 0x064A) return 1;   /* 阿拉伯 */
    if (c >= 0x3040 && c <= 0x30FF) return 1;   /* 平假名/片假名 */
    if (c >= 0x3105 && c <= 0x312F) return 1;   /* 注音 */
    if (c >= 0x3131 && c <= 0x318E) return 1;   /* 谚文字母 */
    if (c >= 0x3400 && c <= 0x4DBF) return 1;   /* CJK 扩展 A */
    if (c >= 0x4E00 && c <= 0x9FFF) return 1;   /* CJK 基本区 */
    if (c >= 0xAC00 && c <= 0xD7A3) return 1;   /* 谚文音节 */
    if (c >= 0xF900 && c <= 0xFAFF) return 1;   /* CJK 兼容 */
    return 0;
}

/* 近似 Python 的 isdigit()/isalnum()。全角数字在 Python 里 isdigit() 为真,
 * 这里按"字母数字"处理(见文件头 DXDIFF 说明)。 */
static int cp_is_alnum(int c)
{
    if (c < 0x80) return is_ascii_digit(c) || is_ascii_alpha(c);
    if (c >= 0xFF10 && c <= 0xFF19) return 1;   /* 全角数字 */
    if (c >= 0xFF21 && c <= 0xFF3A) return 1;   /* 全角大写 */
    if (c >= 0xFF41 && c <= 0xFF5A) return 1;   /* 全角小写 */
    return cp_is_alpha(c);
}

/* ------------------------------------------------------------ 词法器状态 */

static int lx_peek(Lex *lx, int n)
{
    size_t p = lx->pos;
    size_t cl = 0;
    int i, cp;
    for (i = 0; i < n; i++) {
        cp = cp_at(lx->src, lx->len, p, &cl);
        if (cl == 0) return 0;
        p += cl;
    }
    cp = cp_at(lx->src, lx->len, p, &cl);
    return cl == 0 ? 0 : cp;
}

static int lx_advance(Lex *lx)
{
    size_t cl = 0;
    int cp = cp_at(lx->src, lx->len, lx->pos, &cl);
    if (cl == 0) return 0;
    lx->pos += cl;
    if (cp == '\n') { lx->line++; lx->col = 1; }
    else lx->col++;
    return cp;
}

static void lx_error(Lex *lx, int line, int col, const char *msg)
{
    dx_throw("lexer", line ? line : lx->line, col ? col : lx->col, "%s", msg);
}

static Token mk_token(int kind, char *lexeme, int line, int col)
{
    Token t;
    memset(&t, 0, sizeof t);
    t.kind = kind;
    t.lexeme = lexeme;
    t.line = line;
    t.col = col;
    return t;
}

/* ------------------------------------------------------------ 字面量与标识符 */

static void skip_block_comment(Lex *lx)
{
    int sl = lx->line, sc = lx->col;
    lx_advance(lx);   /* / */
    lx_advance(lx);   /* * */
    for (;;) {
        if (lx->pos >= lx->len) lx_error(lx, sl, sc, "unterminated block comment");
        if (lx_peek(lx, 0) == '*' && lx_peek(lx, 1) == '/') {
            lx_advance(lx);
            lx_advance(lx);
            return;
        }
        lx_advance(lx);
    }
}

static Token read_number(Lex *lx)
{
    int sl = lx->line, sc = lx->col;
    char *buf = dx_malloc(64);
    size_t cap = 64, n = 0;
    int is_float = 0;
#define PUSHCH(ch)                                                              \
    do {                                                                        \
        if (n + 2 > cap) { cap *= 2; buf = dx_realloc(buf, cap); }               \
        buf[n++] = (char)(ch);                                                  \
    } while (0)

    if (lx_peek(lx, 0) == '0' && (lx_peek(lx, 1) == 'x' || lx_peek(lx, 1) == 'X')) {
        char *text;
        long long v;
        lx_advance(lx);   /* 0 */
        lx_advance(lx);   /* x */
        while (lx_peek(lx, 0)
               && (is_ascii_digit(lx_peek(lx, 0))
                   || (lx_peek(lx, 0) >= 'a' && lx_peek(lx, 0) <= 'f')
                   || (lx_peek(lx, 0) >= 'A' && lx_peek(lx, 0) <= 'F'))) {
            PUSHCH(lx_advance(lx));
        }
        buf[n] = 0;
        text = dx_aprintf("0x%s", buf);
        if (n == 0) {
            lx_error(lx, sl, sc, dx_aprintf("invalid hex literal '%s'", text));
        }
        /* Python: int(text, 16) 是任意精度;超出 int64 时后续 struct.pack 会崩。
         * DXDIFF:这里直接按 int64 解析,溢出报错而不是崩溃。 */
        errno = 0;
        v = strtoll(text, NULL, 16);
        if (errno == ERANGE) {
            lx_error(lx, sl, sc,
                     dx_aprintf("integer literal out of int64 range: '%s'", text));
        }
        {
            Token t = mk_token(TK_INT, text, sl, sc);
            t.ival = (int64_t)v;
            return t;
        }
    }
    while (is_ascii_digit(lx_peek(lx, 0))) PUSHCH(lx_advance(lx));
    if (lx_peek(lx, 0) == '.' && is_ascii_digit(lx_peek(lx, 1))) {
        is_float = 1;
        PUSHCH(lx_advance(lx));
        while (is_ascii_digit(lx_peek(lx, 0))) PUSHCH(lx_advance(lx));
    }
    if (lx_peek(lx, 0) == 'e' || lx_peek(lx, 0) == 'E') {
        int nxt = lx_peek(lx, 1);
        if (is_ascii_digit(nxt)
            || ((nxt == '+' || nxt == '-') && is_ascii_digit(lx_peek(lx, 2)))) {
            is_float = 1;
            PUSHCH(lx_advance(lx));
            if (lx_peek(lx, 0) == '+' || lx_peek(lx, 0) == '-') PUSHCH(lx_advance(lx));
            while (is_ascii_digit(lx_peek(lx, 0))) PUSHCH(lx_advance(lx));
        }
    }
    buf[n] = 0;
    if (is_float) {
        double d = strtod(buf, NULL);   /* 与 Python float() 一样允许 1e999 → inf */
        Token t = mk_token(TK_FLOAT, dx_strdup(buf), sl, sc);
        t.fval = d;
        return t;
    }
    {
        long long v;
        errno = 0;
        v = strtoll(buf, NULL, 10);
        if (errno == ERANGE) {
            lx_error(lx, sl, sc,
                     dx_aprintf("integer literal out of int64 range: '%s'", buf));
        }
        {
            Token t = mk_token(TK_INT, dx_strdup(buf), sl, sc);
            t.ival = (int64_t)v;
            return t;
        }
    }
#undef PUSHCH
}

static int escape_char(int c)
{
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '\\': return '\\';
    case '"': return '"';
    case '\'': return '\'';
    case '0': return '\0';
    default: return c;
    }
}

static void buf_put_cp(char **buf, size_t *cap, size_t *n, int ch)
{
    if (*n + 6 > *cap) { *cap *= 2; *buf = dx_realloc(*buf, *cap); }
    if (ch < 0x80) (*buf)[(*n)++] = (char)ch;
    else if (ch < 0x800) {
        (*buf)[(*n)++] = (char)(0xC0 | (ch >> 6));
        (*buf)[(*n)++] = (char)(0x80 | (ch & 0x3F));
    } else if (ch < 0x10000) {
        (*buf)[(*n)++] = (char)(0xE0 | (ch >> 12));
        (*buf)[(*n)++] = (char)(0x80 | ((ch >> 6) & 0x3F));
        (*buf)[(*n)++] = (char)(0x80 | (ch & 0x3F));
    } else {
        (*buf)[(*n)++] = (char)(0xF0 | (ch >> 18));
        (*buf)[(*n)++] = (char)(0x80 | ((ch >> 12) & 0x3F));
        (*buf)[(*n)++] = (char)(0x80 | ((ch >> 6) & 0x3F));
        (*buf)[(*n)++] = (char)(0x80 | (ch & 0x3F));
    }
}

static Token read_string(Lex *lx)
{
    int sl = lx->line, sc = lx->col;
    char *buf = dx_malloc(64);
    size_t cap = 64, n = 0;
    char *lex;
    Token t;
    lx_advance(lx);   /* 开引号 */
    for (;;) {
        int ch;
        if (lx->pos >= lx->len) lx_error(lx, sl, sc, "unterminated string literal");
        ch = lx_advance(lx);
        if (ch == '"') break;
        if (ch == '\\') {
            int esc;
            if (lx->pos >= lx->len) lx_error(lx, sl, sc, "unterminated string literal");
            esc = lx_advance(lx);
            ch = escape_char(esc);
        }
        buf_put_cp(&buf, &cap, &n, ch);
    }
    buf[n] = 0;
    lex = dx_aprintf("\"%s\"", buf);
    t = mk_token(TK_STRING, lex, sl, sc);
    t.sval = dx_strdup(buf);
    return t;
}

static int keyword_kind(const char *text)
{
    static const struct { const char *name; int kind; } tbl[] = {
        { "let", TK_LET }, { "if", TK_IF }, { "else", TK_ELSE },
        { "while", TK_WHILE }, { "func", TK_FUNC }, { "return", TK_RETURN },
        { "print", TK_PRINT }, { "true", TK_TRUE }, { "false", TK_FALSE },
        { "include", TK_INCLUDE }, { "refer", TK_REFER }, { "extern", TK_EXTERN },
        { "release", TK_RELEASE }, { "type", TK_TYPE }
    };
    size_t i;
    for (i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
        if (!strcmp(tbl[i].name, text)) return tbl[i].kind;
    return TK_IDENT;
}

static Token read_ident(Lex *lx)
{
    int sl = lx->line, sc = lx->col;
    size_t start = lx->pos;
    while (cp_is_alnum(lx_peek(lx, 0)) || lx_peek(lx, 0) == '_') lx_advance(lx);
    {
        char *text = dx_strndup(lx->src + start, lx->pos - start);
        return mk_token(keyword_kind(text), text, sl, sc);
    }
}

static Token next_token(Lex *lx)
{
    int line = lx->line, col = lx->col;
    int ch = lx_peek(lx, 0);
    size_t cl = 0;

    if (is_ascii_digit(ch)) return read_number(lx);
    if (cp_is_alpha(ch) || ch == '_') return read_ident(lx);
    if (ch == '"') return read_string(lx);

    cp_at(lx->src, lx->len, lx->pos, &cl);
    lx->pos += cl;
    lx->col++;
    {
        int two = lx_peek(lx, 0);
        if (ch < 0x80 && two < 0x80) {
            char *pair = dx_aprintf("%c%c", ch, two);
            if (!strcmp(pair, "==")) { lx_advance(lx); return mk_token(TK_EQEQ, pair, line, col); }
            if (!strcmp(pair, "!=")) { lx_advance(lx); return mk_token(TK_NE, pair, line, col); }
            if (!strcmp(pair, "<=")) { lx_advance(lx); return mk_token(TK_LE, pair, line, col); }
            if (!strcmp(pair, ">=")) { lx_advance(lx); return mk_token(TK_GE, pair, line, col); }
            if (!strcmp(pair, "&&")) { lx_advance(lx); return mk_token(TK_AND, pair, line, col); }
            if (!strcmp(pair, "||")) { lx_advance(lx); return mk_token(TK_OR, pair, line, col); }
            if (!strcmp(pair, "->")) { lx_advance(lx); return mk_token(TK_ARROW, pair, line, col); }
        }
    }
    switch (ch) {
    case '(': return mk_token(TK_LPAREN, dx_strdup("("), line, col);
    case ')': return mk_token(TK_RPAREN, dx_strdup(")"), line, col);
    case '{': return mk_token(TK_LBRACE, dx_strdup("{"), line, col);
    case '}': return mk_token(TK_RBRACE, dx_strdup("}"), line, col);
    case ',': return mk_token(TK_COMMA, dx_strdup(","), line, col);
    case ';': return mk_token(TK_SEMI, dx_strdup(";"), line, col);
    case ':': return mk_token(TK_COLON, dx_strdup(":"), line, col);
    case '.': return mk_token(TK_DOT, dx_strdup("."), line, col);
    case '+': return mk_token(TK_PLUS, dx_strdup("+"), line, col);
    case '-': return mk_token(TK_MINUS, dx_strdup("-"), line, col);
    case '*': return mk_token(TK_STAR, dx_strdup("*"), line, col);
    case '/': return mk_token(TK_SLASH, dx_strdup("/"), line, col);
    case '%': return mk_token(TK_PERCENT, dx_strdup("%"), line, col);
    case '=': return mk_token(TK_EQ, dx_strdup("="), line, col);
    case '<': return mk_token(TK_LT, dx_strdup("<"), line, col);
    case '>': return mk_token(TK_GT, dx_strdup(">"), line, col);
    case '!': return mk_token(TK_BANG, dx_strdup("!"), line, col);
    default: break;
    }
    {
        /* 报错信息里的 {ch!r}:把码点重新编码成 UTF-8 再取 repr */
        char tmp[8];
        size_t tn = 0;
        if (ch < 0x80) tmp[tn++] = (char)ch;
        else if (ch < 0x800) {
            tmp[tn++] = (char)(0xC0 | (ch >> 6));
            tmp[tn++] = (char)(0x80 | (ch & 0x3F));
        } else if (ch < 0x10000) {
            tmp[tn++] = (char)(0xE0 | (ch >> 12));
            tmp[tn++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            tmp[tn++] = (char)(0x80 | (ch & 0x3F));
        } else {
            tmp[tn++] = (char)(0xF0 | (ch >> 18));
            tmp[tn++] = (char)(0x80 | ((ch >> 12) & 0x3F));
            tmp[tn++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            tmp[tn++] = (char)(0x80 | (ch & 0x3F));
        }
        tmp[tn] = 0;
        lx_error(lx, line, col,
                 dx_aprintf("unexpected character %s", dx_py_repr_str(tmp)));
    }
    return mk_token(TK_EOF, dx_strdup(""), line, col);   /* 不会到达 */
}

Token *dx_lex(const char *src, const char *filename, int *out_n)
{
    Lex lx;
    Token *toks = NULL;
    int n = 0, cap = 0;

    lx.src = src;
    lx.len = strlen(src);
    lx.pos = 0;
    lx.line = 1;
    lx.col = 1;
    lx.filename = filename;

    while (lx.pos < lx.len) {
        int ch = lx_peek(&lx, 0);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { lx_advance(&lx); continue; }
        if (ch == '#') {
            while (lx_peek(&lx, 0) && lx_peek(&lx, 0) != '\n') lx_advance(&lx);
            continue;
        }
        if (ch == '/' && lx_peek(&lx, 1) == '/') {
            while (lx_peek(&lx, 0) && lx_peek(&lx, 0) != '\n') lx_advance(&lx);
            continue;
        }
        if (ch == '/' && lx_peek(&lx, 1) == '*') { skip_block_comment(&lx); continue; }
        {
            Token t = next_token(&lx);
            DXV_PUSH(toks, n, cap, t);
        }
    }
    {
        Token t = mk_token(TK_EOF, dx_strdup(""), lx.line, lx.col);
        DXV_PUSH(toks, n, cap, t);
    }
    *out_n = n;
    return toks;
}
