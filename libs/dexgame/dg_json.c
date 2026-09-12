/* dg_json.c — 极简 JSON 读写实现(见 dg_json.h 的设计说明)
 *
 * 写入:自动缩进 2 空格、自动逗号;浮点用 %.9g(float32 往返无损所需的有效位)。
 * 读取:游标式;错误一次性记录在 r->err 里(不 abort),调用方据此给可读信息。
 */
#include "dg_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================ 写入 ============================ */

static int djw_reserve(DgJsonW *w, size_t extra) {
    if (w->oom) return -1;
    if (w->len + extra + 1 <= w->cap) return 0;
    size_t cap = w->cap ? w->cap : 1024;
    while (cap < w->len + extra + 1) cap *= 2;
    char *nb = (char *)realloc(w->buf, cap);
    if (!nb) { w->oom = 1; return -1; }
    w->buf = nb;
    w->cap = cap;
    return 0;
}

static void djw_put(DgJsonW *w, const char *s, size_t n) {
    if (djw_reserve(w, n)) return;
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void djw_indent(DgJsonW *w) {
    for (int i = 0; i < w->depth; i++) djw_put(w, "  ", 2);
}

/* 元素前置:换行 + 缩进 + (逗号) + 可选键名 */
static void djw_pre(DgJsonW *w, const char *key) {
    if (w->depth > 0 && w->depth < 32 && w->need_comma[w->depth - 1]) djw_put(w, ",", 1);
    djw_put(w, "\n", 1);
    djw_indent(w);
    if (w->depth > 0 && w->depth < 32) w->need_comma[w->depth - 1] = 1;
    if (key) {
        djw_put(w, "\"", 1);
        djw_put(w, key, strlen(key));
        djw_put(w, "\": ", 3);
    }
}

static void djw_push(DgJsonW *w) {
    if (w->depth < 31) w->need_comma[w->depth] = 0;
    w->depth++;
}

static void djw_pop(DgJsonW *w) {
    w->depth--;
    if (w->depth > 0) djw_put(w, "\n", 1);
    if (w->depth > 0) djw_indent(w);
}

static void djw_escaped(DgJsonW *w, const char *s) {
    djw_put(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  djw_put(w, "\\\"", 2); break;
        case '\\': djw_put(w, "\\\\", 2); break;
        case '\n': djw_put(w, "\\n", 2); break;
        case '\r': djw_put(w, "\\r", 2); break;
        case '\t': djw_put(w, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", *p);
                djw_put(w, tmp, 6);
            } else {
                djw_put(w, (const char *)p, 1);   /* UTF-8 字节直接透传 */
            }
        }
    }
    djw_put(w, "\"", 1);
}

void djw_init(DgJsonW *w) { memset(w, 0, sizeof *w); }
void djw_free(DgJsonW *w) { free(w->buf); memset(w, 0, sizeof *w); }
const char *djw_text(DgJsonW *w) { return w->oom ? NULL : (w->buf ? w->buf : ""); }
void djw_raw(DgJsonW *w, const char *s) { djw_put(w, s, strlen(s)); }

void djw_obj_begin(DgJsonW *w, const char *key) { djw_pre(w, key); djw_put(w, "{", 1); djw_push(w); }
void djw_obj_end(DgJsonW *w)   { djw_pop(w); djw_put(w, "}", 1); }
void djw_arr_begin(DgJsonW *w, const char *key) { djw_pre(w, key); djw_put(w, "[", 1); djw_push(w); }
void djw_arr_end(DgJsonW *w)   { djw_pop(w); djw_put(w, "]", 1); }

void djw_int(DgJsonW *w, const char *key, long long v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v);
    djw_pre(w, key);
    djw_put(w, tmp, strlen(tmp));
}

void djw_float(DgJsonW *w, const char *key, double v) {
    char tmp[40];
    /* %.9g 足以让 float32 往返无损;整数值写成 "1" 而不是 "1.0",
       读回来仍是 1.0 —— JSON 的数字本来就不区分 int/float,类型由描述符表决定。 */
    snprintf(tmp, sizeof tmp, "%.9g", v);
    djw_pre(w, key);
    djw_put(w, tmp, strlen(tmp));
}

void djw_str(DgJsonW *w, const char *key, const char *v) { djw_pre(w, key); djw_escaped(w, v ? v : ""); }
void djw_bool(DgJsonW *w, const char *key, int v) { djw_pre(w, key); djw_put(w, v ? "true" : "false", v ? 4 : 5); }

void djw_elem_int(DgJsonW *w, long long v) { djw_int(w, NULL, v); }
void djw_elem_float(DgJsonW *w, double v) { djw_float(w, NULL, v); }
void djw_elem_str(DgJsonW *w, const char *v) { djw_str(w, NULL, v); }

/* ============================ 读取 ============================ */

int djr_fail(DgJsonR *r, const char *fmt, ...) {
    if (!r->err[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r->err, sizeof r->err, fmt, ap);
        va_end(ap);
    }
    return -1;
}
const char *djr_error(DgJsonR *r) { return r->err; }

void djr_init(DgJsonR *r, const char *text, size_t len) {
    memset(r, 0, sizeof *r);
    r->p = text;
    r->end = text + len;
    r->line = 1;
}

static void djr_ws(DgJsonR *r) {
    while (r->p < r->end) {
        char c = *r->p;
        if (c == '\n') { r->line++; r->p++; }
        else if (c == ' ' || c == '\t' || c == '\r') r->p++;
        else break;
    }
}

static int djr_lit(DgJsonR *r, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, lit, n) != 0)
        return djr_fail(r, "line %d: expected '%s'", r->line, lit);
    r->p += n;
    return 0;
}

/* 把「下一个字符」渲染成可读文本,便于报错时直接看出解析卡在哪 */
static const char *djr_peek_desc(DgJsonR *r) {
    static char buf[16];
    if (r->p >= r->end) return "end of text";
    const unsigned char c = (unsigned char)*r->p;
    switch (c) {
    case '\n': return "newline";
    case '\r': return "carriage return";
    case '\t': return "tab";
    case '\0': return "NUL";
    }
    if (c < 0x20) { snprintf(buf, sizeof buf, "byte 0x%02x", c); return buf; }
    snprintf(buf, sizeof buf, "'%c'", c);
    return buf;
}

static int djr_expect(DgJsonR *r, char c) {
    djr_ws(r);
    if (r->p >= r->end || *r->p != c)
        return djr_fail(r, "line %d: expected '%c', found %s", r->line, c, djr_peek_desc(r));
    r->p++;
    return 0;
}

int djr_obj_begin(DgJsonR *r) { return djr_expect(r, '{'); }
int djr_arr_begin(DgJsonR *r) { return djr_expect(r, '['); }
int djr_obj_end(DgJsonR *r)   { return djr_expect(r, '}'); }
int djr_arr_end(DgJsonR *r)   { return djr_expect(r, ']'); }

/* 读一个字符串字面量到 out(含 \uXXXX → UTF-8 解码,中文场景要它)*/
static int djr_string(DgJsonR *r, char *out, size_t cap) {
    if (djr_expect(r, '"')) return -1;
    size_t n = 0;
    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p++;
        if (c == '\\') {
            if (r->p >= r->end) return djr_fail(r, "line %d: truncated escape", r->line);
            char e = *r->p++;
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'u': {
                /* 只处理 BMP(不含代理对) —— 对场景文本足够 */
                if (r->end - r->p < 4) return djr_fail(r, "line %d: bad \\u", r->line);
                char hex[5] = { r->p[0], r->p[1], r->p[2], r->p[3], 0 };
                r->p += 4;
                unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                if (cp < 0x80) {
                    c = (unsigned char)cp;
                } else if (cp < 0x800) {
                    if (n + 2 >= cap) return djr_fail(r, "string too long");
                    out[n++] = (char)(0xC0 | (cp >> 6));
                    c = (unsigned char)(0x80 | (cp & 0x3F));
                } else {
                    if (n + 3 >= cap) return djr_fail(r, "string too long");
                    out[n++] = (char)(0xE0 | (cp >> 12));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    c = (unsigned char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default:
                return djr_fail(r, "line %d: unknown escape '\\%c'", r->line, e);
            }
        }
        if (n + 1 >= cap) return djr_fail(r, "string too long (cap %u)", (unsigned)cap);
        out[n++] = (char)c;
    }
    if (r->p >= r->end) return djr_fail(r, "line %d: unterminated string", r->line);
    r->p++;                       /* 收尾的 '"' */
    out[n] = '\0';
    return 0;
}

int djr_key(DgJsonR *r, char *key, size_t cap) {
    djr_ws(r);
    /* 注意:**不消费** '}' —— 交给调用方的 djr_obj_end。
       否则调用方再调 djr_obj_end 就会找同一个 '}' 而报 "expected '}'"。
       (这个契约不对称是刻意的:迭代器只报告"到头了",不代劳收尾。) */
    if (r->p < r->end && *r->p == '}') return 1;
    if (!key[0]) {                /* 第一次进来:不需要逗号 */
        if (djr_string(r, key, cap)) return -1;
    } else {
        if (djr_expect(r, ',')) return -1;
        djr_ws(r);
        if (r->p < r->end && *r->p == '}') return 1;   /* 容忍尾逗号 */
        if (djr_string(r, key, cap)) return -1;
    }
    return djr_expect(r, ':') ? -1 : 0;
}

int djr_arr_more(DgJsonR *r) {
    djr_ws(r);
    /* 消费元素之间的分隔逗号(数组迭代器无状态,所以只可能是"上一个元素之后")。
       注意:**不消费** ']',交给 djr_arr_end。 */
    if (r->p < r->end && *r->p == ',') { r->p++; djr_ws(r); }
    if (r->p < r->end && *r->p == ']') return 0;
    if (r->p >= r->end) return djr_fail(r, "line %d: unterminated array", r->line);
    return 1;
}

static int djr_number(DgJsonR *r, double *out) {
    djr_ws(r);
    const char *start = r->p;
    if (r->p < r->end && (*r->p == '-' || *r->p == '+')) r->p++;
    int digits = 0;
    while (r->p < r->end && ((*r->p >= '0' && *r->p <= '9') || *r->p == '.' ||
                             *r->p == 'e' || *r->p == 'E' || *r->p == '-' || *r->p == '+')) {
        r->p++;
        digits++;
    }
    if (!digits) return djr_fail(r, "line %d: expected a number", r->line);
    char tmp[64];
    size_t n = (size_t)(r->p - start);
    if (n >= sizeof tmp) return djr_fail(r, "line %d: number too long", r->line);
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    *out = strtod(tmp, NULL);
    return 0;
}

int djr_int(DgJsonR *r, long long *out) {
    double d;
    if (djr_number(r, &d)) return -1;
    *out = (long long)d;
    return 0;
}

int djr_float(DgJsonR *r, double *out) { return djr_number(r, out); }

int djr_bool(DgJsonR *r, int *out) {
    djr_ws(r);
    if (r->p < r->end && *r->p == 't') { if (djr_lit(r, "true")) return -1; *out = 1; return 0; }
    if (r->p < r->end && *r->p == 'f') { if (djr_lit(r, "false")) return -1; *out = 0; return 0; }
    return djr_fail(r, "line %d: expected true/false", r->line);
}

int djr_str(DgJsonR *r, char *out, size_t cap) { return djr_string(r, out, cap); }

/* 跳过一个字符串字面量(**不复制**内容)。
   必须独立实现:`djr_skip_value` 要能跳过任意长的字符串,而 djr_string 需要缓冲。 */
static int djr_skip_string(DgJsonR *r) {
    if (djr_expect(r, '"')) return -1;
    while (r->p < r->end && *r->p != '"') {
        if (*r->p == '\n') r->line++;
        if (*r->p == '\\') {
            r->p++;
            if (r->p >= r->end) return djr_fail(r, "line %d: truncated escape", r->line);
        }
        r->p++;
    }
    if (r->p >= r->end) return djr_fail(r, "line %d: unterminated string", r->line);
    r->p++;                       /* 收尾的 '"' */
    return 0;
}

/* 跳过任意一个值。这是"未知字段也能安全读旧存档/新存档"的关键。 */
int djr_skip_value(DgJsonR *r) {
    djr_ws(r);
    if (r->p >= r->end) return djr_fail(r, "line %d: expected a value", r->line);
    switch (*r->p) {
    case '{':
    case '[': {
        char open = *r->p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (r->p < r->end) {
            char c = *r->p;
            if (c == '"') {                    /* 跳过字符串,避免把里面的括号当结构 */
                if (djr_skip_string(r)) return -1;
                continue;
            }
            if (c == '\n') r->line++;
            if (c == open) depth++;
            else if (c == close) { depth--; r->p++; if (!depth) return 0; continue; }
            r->p++;
        }
        return djr_fail(r, "line %d: unterminated value", r->line);
    }
    case 't': return djr_lit(r, "true");
    case 'f': return djr_lit(r, "false");
    case 'n': return djr_lit(r, "null");
    case '"': return djr_skip_string(r);
    default: { double d; return djr_number(r, &d); }
    }
}
