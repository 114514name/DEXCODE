/* ============================================================================
 * dexc_util.c — 基础工具:分配、错误抛出、Python 兼容的 repr、路径层、操作码表。
 *
 * 注意(踩过的坑):**不要**用 `open(f,'w').write(open(f).read()...)` 这种一行式
 * 去改文件 —— Python 先算 `open(f,'w')`(立即截断),再算 `open(f).read()`(读到空),
 * 结果把文件清空。7 个 .c 文件就是这样被清过一次。批量改写一律走 read → 改 → write。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dexc.h"
#include "dx_utf8.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/stat.h>
#endif

/* ---------------------------------------------------------------- 基础工具 */

void *dx_malloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "dexc: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return p;
}

void *dx_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "dexc: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return q;
}

char *dx_strdup(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = dx_malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *dx_strndup(const char *s, size_t n)
{
    char *p = dx_malloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *dx_aprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    int n;
    char *buf;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    buf = dx_malloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

/* ---------------------------------------------------------------- 错误抛出 */

char dx_err_msg[4096];
int dx_err_line = 0, dx_err_col = 0;
const char *dx_err_phase = NULL;

void dx_throw(const char *phase, int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dx_err_msg, sizeof dx_err_msg, fmt, ap);
    va_end(ap);
    dx_err_line = line;
    dx_err_col = col;
    dx_err_phase = phase;
    longjmp(*dx_jmp_target(), 1);
}

/* ------------------------------------------------- Python 兼容的文本格式化 */

/* UTF-8 解码:返回码点并推进 *i。非法序列返回 0xFFFD 并按"最大子部分"跳字节。 */
static int utf8_next(const char *s, size_t *i, size_t n)
{
    unsigned char c = (unsigned char)s[*i];
    int need, cp, k;
    if (c < 0x80) { (*i)++; return c; }
    if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
    else { (*i)++; return 0xFFFD; }
    if (*i + (size_t)need >= n) {
        (*i)++;
        return 0xFFFD;
    }
    for (k = 1; k <= need; k++) {
        unsigned char cc = (unsigned char)s[*i + (size_t)k];
        if ((cc & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i += (size_t)need + 1;
    return cp;
}

/* Python str.isprintable() 的近似(判据:不是控制/格式/分隔/未分配字符)。
 * 只影响 repr 里非 ASCII 字符是原样输出还是 \uXXXX,够用即可。 */
static int cp_is_printable(int cp)
{
    if (cp == 0x20) return 1;
    if (cp < 0x20 || cp == 0x7F) return 0;
    if (cp < 0x7F) return 1;
    if (cp >= 0x80 && cp <= 0x9F) return 0;         /* C1 控制 */
    if (cp == 0xA0) return 0;                       /* NBSP(Zs) */
    if (cp >= 0x2000 && cp <= 0x200F) return 0;     /* 各种空格/方向标记 */
    if (cp >= 0x2028 && cp <= 0x202F) return 0;
    if (cp >= 0x205F && cp <= 0x206F) return 0;
    if (cp == 0x3000) return 0;                     /* 全角空格 */
    if (cp == 0xFEFF) return 0;
    if (cp >= 0xFFF9 && cp <= 0xFFFB) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;     /* 代理区 */
    if (cp >= 0xE000 && cp <= 0xF8FF) return 0;     /* 私用区(Co) */
    if (cp >= 0xF0000) return 0;
    return 1;
}

char *dx_py_repr_str(const char *s)
{
    size_t i = 0, n = strlen(s);
    int has_sq = strchr(s, '\'') != NULL;
    int has_dq = strchr(s, '"') != NULL;
    char q = (has_sq && !has_dq) ? '"' : '\'';
    char *out = dx_malloc(n * 6 + 8);
    size_t o = 0;
    out[o++] = q;
    while (i < n) {
        int cp = utf8_next(s, &i, n);
        if (cp == (unsigned char)q || cp == '\\') {
            out[o++] = '\\';
            out[o++] = (char)cp;
        } else if (cp == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (cp == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (cp == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (cp_is_printable(cp)) {
            /* 原样:重新编码 UTF-8(1..4 字节) */
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
        } else if (cp < 0x100) {
            o += (size_t)sprintf(out + o, "\\x%02x", cp);
        } else if (cp < 0x10000) {
            o += (size_t)sprintf(out + o, "\\u%04x", cp);
        } else {
            o += (size_t)sprintf(out + o, "\\U%08x", cp);
        }
    }
    out[o++] = q;
    out[o] = 0;
    return out;
}

char *dx_py_repr_float(double v)
{
    char buf[64];
    char digits[32];
    int prec, exp10 = 0, nd = 0, i, neg = 0;
    const char *e;
    char *out;
    size_t o = 0;

    if (isnan(v)) return dx_strdup("nan");
    if (isinf(v)) return dx_strdup(v < 0 ? "-inf" : "inf");
    if (v == 0.0) return dx_strdup(signbit(v) ? "-0.0" : "0.0");

    /* 最短且能往返的十进制位数(Python 的 repr 用的就是这个定义) */
    for (prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        if (strtod(buf, NULL) == v) break;
    }
    if (prec > 17) snprintf(buf, sizeof buf, "%.17e", v);

    /* 拆出符号、有效数字、十进制指数 */
    {
        const char *p = buf;
        if (*p == '-') { neg = 1; p++; }
        for (; *p && *p != 'e' && *p != 'E'; p++) {
            if (*p >= '0' && *p <= '9') {
                if (nd == 0 && *p == '0' && p[1] != '.') continue;   /* 前导 0 */
                if (nd < (int)sizeof digits - 1) digits[nd++] = *p;
            }
        }
        e = strchr(buf, 'e');
        if (!e) e = strchr(buf, 'E');
        exp10 = e ? atoi(e + 1) : 0;
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;   /* 去掉尾部 0 */
    digits[nd] = 0;

    out = dx_malloc(64);
    if (neg) out[o++] = '-';
    if (exp10 < -4 || exp10 >= 16) {
        /* 科学计数法:d.dddde±XX(指数至少两位) */
        out[o++] = digits[0];
        if (nd > 1) {
            out[o++] = '.';
            for (i = 1; i < nd; i++) out[o++] = digits[i];
        }
        o += (size_t)sprintf(out + o, "e%c%02d", exp10 < 0 ? '-' : '+',
                             exp10 < 0 ? -exp10 : exp10);
    } else if (exp10 >= 0) {
        for (i = 0; i <= exp10; i++) out[o++] = (i < nd) ? digits[i] : '0';
        out[o++] = '.';
        if (nd > exp10 + 1) { for (i = exp10 + 1; i < nd; i++) out[o++] = digits[i]; }
        else out[o++] = '0';
    } else {
        out[o++] = '0';
        out[o++] = '.';
        for (i = 0; i < -exp10 - 1; i++) out[o++] = '0';
        for (i = 0; i < nd; i++) out[o++] = digits[i];
    }
    out[o] = 0;
    return out;
}

/* ------------------------------------------------------------------ 路径层 */

static int is_sep(char c) { return c == '/' || c == '\\'; }

int dx_path_isabs(const char *p)
{
    if (!p || !*p) return 0;
#if defined(_WIN32)
    if (is_sep(p[0]) && is_sep(p[1])) return 1;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        return 1;
    return is_sep(p[0]);
#else
    return p[0] == '/';
#endif
}

char *dx_path_basename(const char *p)
{
    const char *s = p + strlen(p);
    while (s > p && !is_sep(s[-1])) s--;
    return dx_strdup(s);
}

char *dx_path_dirname(const char *p)
{
    const char *s = p + strlen(p);
    const char *start = p;
#if defined(_WIN32)
    if (p[0] && p[1] == ':') start = p + 2;
#endif
    while (s > start && !is_sep(s[-1])) s--;
    if (s == start) {
#if defined(_WIN32)
        if (p[0] && p[1] == ':') return dx_strndup(p, 2);
#endif
        return dx_strdup("");
    }
    while (s > start + 1 && is_sep(s[-1])) s--;
    return dx_strndup(p, (size_t)(s - p));
}

char *dx_path_join(const char *a, const char *b)
{
    size_t la;
    if (!a || !*a) return dx_strdup(b);
    if (dx_path_isabs(b)) return dx_strdup(b);
    la = strlen(a);
#if defined(_WIN32)
    if (la && a[la - 1] == ':') return dx_aprintf("%s\\%s", a, b);
#endif
    if (la && is_sep(a[la - 1])) return dx_aprintf("%s%s", a, b);
#if defined(_WIN32)
    return dx_aprintf("%s\\%s", a, b);
#else
    return dx_aprintf("%s/%s", a, b);
#endif
}

char *dx_path_normpath(const char *p)
{
    char *tmp = dx_strdup(p);
    size_t i, n = strlen(tmp);
    char sep =
#if defined(_WIN32)
        '\\';
#else
        '/';
#endif
    char *out;
    size_t o = 0;

    for (i = 0; i < n; i++) if (is_sep(tmp[i])) tmp[i] = sep;
    {
        size_t drv = 0;
        size_t r, w;
        int seps = 0;
#if defined(_WIN32)
        if (n >= 2 && tmp[1] == ':') drv = 2;
#endif
        r = drv;
        w = drv;
        while (r < n) {
            if (tmp[r] == sep) {
                seps++;
                if (seps > 1 && !(drv == 0 && w == 1 && seps == 2)) { r++; continue; }
                tmp[w++] = sep;
                r++;
            } else {
                seps = 0;
                tmp[w++] = tmp[r++];
            }
        }
        tmp[w] = 0;
        n = w;
    }
    out = dx_malloc(n + 4);
    {
        size_t drv_len = 0;
        size_t pos, base;
        int has_root = 0;
#if defined(_WIN32)
        if (n >= 2 && tmp[1] == ':') drv_len = 2;
#endif
        for (i = 0; i < drv_len; i++) out[o++] = tmp[i];
        pos = drv_len;
        /* 根分隔符:UNC 前缀保留两个 */
        if (pos < n && tmp[pos] == sep) {
            has_root = 1;
            out[o++] = sep;
            pos++;
#if defined(_WIN32)
            if (drv_len == 0 && pos < n && tmp[pos] == sep) { out[o++] = sep; pos++; }
#endif
        }
        while (pos < n && tmp[pos] == sep) pos++;
        base = o;                       /* 根之后写过的内容起点 */
        while (pos < n) {
            size_t seg_start = pos, seg_len;
            while (pos < n && tmp[pos] != sep) pos++;
            seg_len = pos - seg_start;
            while (pos < n && tmp[pos] == sep) pos++;
            if (seg_len == 1 && tmp[seg_start] == '.') continue;
            if (seg_len == 2 && tmp[seg_start] == '.' && tmp[seg_start + 1] == '.') {
                if (o > base) {
                    while (o > base && out[o - 1] != sep) o--;
                    if (o > base) o--;              /* 去掉分隔符 */
                } else if (!has_root && drv_len == 0) {
                    if (o > 0) out[o++] = sep;
                    out[o++] = '.';
                    out[o++] = '.';
                }
                continue;
            }
            if (o > base && out[o - 1] != sep) out[o++] = sep;
            memcpy(out + o, tmp + seg_start, seg_len);
            o += seg_len;
        }
        if (o == 0) out[o++] = '.';
    }
    out[o] = 0;
    return out;
}

char *dx_path_abspath(const char *p)
{
    if (dx_path_isabs(p)) return dx_path_normpath(p);
    {
        char cwd[4096];
#if defined(_WIN32)
        char *w = dxu_getcwd();
        if (w) { snprintf(cwd, sizeof cwd, "%s", w); free(w); }
        else cwd[0] = 0;
#else
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
#endif
        return dx_path_normpath(dx_path_join(cwd, p));
    }
}

int dx_path_exists(const char *p)
{
#if defined(_WIN32)
    return dxu_exists(p);
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

int dx_path_isdir(const char *p)
{
#if defined(_WIN32)
    return dxu_isdir(p);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

char *dx_path_strip_ext(const char *p)
{
    const char *base = p + strlen(p);
    const char *dot;
    while (base > p && !is_sep(base[-1])) base--;
    dot = strrchr(base, '.');
    if (dot && dot != base) return dx_strndup(p, (size_t)(dot - p));
    return dx_strdup(p);
}

char *dx_path_abspath_noext(const char *p)
{
    return dx_path_strip_ext(dx_path_abspath(p));
}

char **dx_listdir(const char *dir, int *out_n)
{
    /* 实现搬到 dx_utf8.c 了:目录名/文件名都按 UTF-8 处理(宽字符 API + 转码),
     * 否则中文目录下的 libs/ 与 .dexdef 扫描会一无所获。 */
    return dxu_listdir(dir, out_n);
}

char *dx_read_file(const char *path, size_t *out_len)
{
    FILE *f = dxu_fopen(path, "rb");
    char *buf;
    long sz;
    size_t got;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = dx_malloc((size_t)sz + 1);
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    /* utf-8-sig:丢掉可能存在的 BOM */
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB
        && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 3 + 1);
        got -= 3;
    }
    if (out_len) *out_len = got;
    return buf;
}

/* ------------------------------------------------------------- 操作码表 */

static const char *const mnem[0x22] = {
    "NOP", "PUSH", "LOAD", "STORE", "ADD", "SUB", "MUL", "DIV", "MOD", "NEG",
    "EQ", "NE", "LT", "LE", "GT", "GE", "AND", "OR", "NOT", "JMP", "JZ", "JNZ",
    "CALL", "RET", "PRINT", "POP", "DUP", "HALT", "NCALL", "CONCAT", "MAKE_OBJ",
    "GET_FIELD", "SET_FIELD", "CALL_NAME"
};

const char *dx_mnemonic(int op)
{
    if (op >= 0 && op <= 0x21) return mnem[op];
    return NULL;
}

int dx_has_operand(int op)
{
    switch (op) {
    case OP_PUSH: case OP_LOAD: case OP_STORE: case OP_JMP: case OP_JZ:
    case OP_JNZ: case OP_CALL: case OP_NCALL: case OP_MAKE_OBJ:
    case OP_GET_FIELD: case OP_SETFIELD:
        return 1;
    default:
        return 0;
    }
}

int dx_instruction_size(int op)
{
    return dx_has_operand(op) ? 5 : 1;
}
