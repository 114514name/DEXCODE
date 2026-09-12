/* ============================================================================
 * ds_json.c — 极小 JSON DOM 的实现(见 ds_json.h 的说明)。
 *
 * 数字写回用的是"最短且能往返的十进制 + Python 的指数规则",与
 * tools/dexc/dexc_util.c 的 dx_py_repr_float 同一套规则 —— 这样 IDE 写出来的
 * project.json / 场景文件与 Python 工具链的 json.dump 看起来一致(3 而不是 3.0、
 * 0.1 而不是 0.10000000000000001)。
 * ==========================================================================*/
#include "ds_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xj_malloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "dexstudio: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return p;
}

static void *xj_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "dexstudio: out of memory (%zu bytes)\n", n);
        exit(2);
    }
    return q;
}

static char *xj_strdup(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = xj_malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* ------------------------------------------------------------ 构造 */

static Dsj *node_new(DsjType t)
{
    Dsj *v = xj_malloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->t = t;
    return v;
}

Dsj *dsj_null(void) { return node_new(DSJ_NULL); }
Dsj *dsj_bool(int b) { Dsj *v = node_new(DSJ_BOOL); v->b = b ? 1 : 0; return v; }
Dsj *dsj_int(long long x) { Dsj *v = node_new(DSJ_NUM); v->num = (double)x; v->is_int = 1; return v; }
Dsj *dsj_num(double x) { Dsj *v = node_new(DSJ_NUM); v->num = x; v->is_int = 0; return v; }
Dsj *dsj_str(const char *s) { Dsj *v = node_new(DSJ_STR); v->str = xj_strdup(s ? s : ""); return v; }
Dsj *dsj_arr(void) { return node_new(DSJ_ARR); }
Dsj *dsj_obj(void) { return node_new(DSJ_OBJ); }

static void push_raw(Dsj *v, char *key, Dsj *child)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xj_realloc(v->items, (size_t)v->cap * sizeof *v->items);
        if (v->t == DSJ_OBJ)
            v->keys = xj_realloc(v->keys, (size_t)v->cap * sizeof *v->keys);
    }
    if (v->t == DSJ_OBJ) v->keys[v->n] = key;
    v->items[v->n] = child;
    v->n++;
}

void dsj_push(Dsj *arr, Dsj *v)
{
    if (!arr || arr->t != DSJ_ARR) { dsj_free(v); return; }
    push_raw(arr, NULL, v);
}

void dsj_set(Dsj *obj, const char *key, Dsj *v)
{
    int i;
    if (!obj || obj->t != DSJ_OBJ || !key) { dsj_free(v); return; }
    for (i = 0; i < obj->n; i++) {
        if (!strcmp(obj->keys[i], key)) {
            dsj_free(obj->items[i]);
            obj->items[i] = v;
            return;
        }
    }
    push_raw(obj, xj_strdup(key), v);
}

void dsj_set_str(Dsj *o, const char *k, const char *v) { dsj_set(o, k, dsj_str(v)); }
void dsj_set_int(Dsj *o, const char *k, long long v) { dsj_set(o, k, dsj_int(v)); }
void dsj_set_num(Dsj *o, const char *k, double v) { dsj_set(o, k, dsj_num(v)); }
void dsj_set_bool(Dsj *o, const char *k, int v) { dsj_set(o, k, dsj_bool(v)); }

/* ------------------------------------------------------------ 读取 */

Dsj *dsj_get(const Dsj *obj, const char *key)
{
    int i;
    if (!obj || obj->t != DSJ_OBJ || !key) return NULL;
    for (i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

const char *dsj_get_str(const Dsj *o, const char *k, const char *dflt)
{
    Dsj *v = dsj_get(o, k);
    if (!v) return dflt;
    if (v->t == DSJ_STR) return v->str;
    if (v->t == DSJ_NULL) return dflt;
    return dflt;
}

long long dsj_get_int(const Dsj *o, const char *k, long long dflt)
{
    Dsj *v = dsj_get(o, k);
    if (!v || v->t != DSJ_NUM) return dflt;
    return (long long)v->num;
}

double dsj_get_num(const Dsj *o, const char *k, double dflt)
{
    Dsj *v = dsj_get(o, k);
    if (!v || v->t != DSJ_NUM) return dflt;
    return v->num;
}

int dsj_get_bool(const Dsj *o, const char *k, int dflt)
{
    Dsj *v = dsj_get(o, k);
    if (!v) return dflt;
    if (v->t == DSJ_BOOL) return v->b;
    if (v->t == DSJ_NUM) return v->num != 0.0;
    return dflt;
}

int dsj_len(const Dsj *v) { return (v && (v->t == DSJ_ARR || v->t == DSJ_OBJ)) ? v->n : 0; }

Dsj *dsj_at(const Dsj *v, int i)
{
    if (!v || i < 0 || i >= v->n) return NULL;
    return v->items[i];
}

Dsj *dsj_clone(const Dsj *v)
{
    int i;
    Dsj *c;
    if (!v) return NULL;
    switch (v->t) {
    case DSJ_NULL: return dsj_null();
    case DSJ_BOOL: return dsj_bool(v->b);
    case DSJ_NUM:
        c = dsj_num(v->num);
        c->is_int = v->is_int;
        return c;
    case DSJ_STR: return dsj_str(v->str);
    case DSJ_ARR:
        c = dsj_arr();
        for (i = 0; i < v->n; i++) dsj_push(c, dsj_clone(v->items[i]));
        return c;
    case DSJ_OBJ:
        c = dsj_obj();
        for (i = 0; i < v->n; i++) dsj_set(c, v->keys[i], dsj_clone(v->items[i]));
        return c;
    default:
        return dsj_null();
    }
}

void dsj_free(Dsj *v)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->n; i++) {
        if (v->t == DSJ_OBJ && v->keys) free(v->keys[i]);
        dsj_free(v->items[i]);
    }
    free(v->keys);
    free(v->items);
    free(v->str);
    free(v);
}

/* ------------------------------------------------------------ 解析 */

typedef struct {
    const char *s;
    size_t i, n;
    char *err;
    size_t errsz;
    int depth;
} P;

static void perr(P *p, const char *msg)
{
    if (p->err && p->errsz)
        snprintf(p->err, p->errsz, "%s at byte %zu", msg, p->i);
}

static void skip_ws(P *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static int hex4(P *p, int *out)
{
    int k, v = 0;
    for (k = 0; k < 4; k++) {
        int c;
        if (p->i >= p->n) return 0;
        c = (unsigned char)p->s[p->i++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0;
    }
    *out = v;
    return 1;
}

static void put_utf8(char **buf, size_t *len, size_t *cap, int cp)
{
    if (*len + 5 > *cap) { *cap = *cap ? *cap * 2 : 64; *buf = xj_realloc(*buf, *cap); }
    if (cp < 0x80) (*buf)[(*len)++] = (char)cp;
    else if (cp < 0x800) {
        (*buf)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        (*buf)[(*len)++] = (char)(0xE0 | (cp >> 12));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        (*buf)[(*len)++] = (char)(0xF0 | (cp >> 18));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static char *parse_string(P *p)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (p->i >= p->n || p->s[p->i] != '"') { perr(p, "expected '\"'"); return NULL; }
    p->i++;
    for (;;) {
        int c;
        if (p->i >= p->n) { perr(p, "unterminated string"); free(buf); return NULL; }
        c = (unsigned char)p->s[p->i++];
        if (c == '"') break;
        if (c == '\\') {
            int e;
            if (p->i >= p->n) { perr(p, "unterminated escape"); free(buf); return NULL; }
            e = (unsigned char)p->s[p->i++];
            switch (e) {
            case '"': put_utf8(&buf, &len, &cap, '"'); break;
            case '\\': put_utf8(&buf, &len, &cap, '\\'); break;
            case '/': put_utf8(&buf, &len, &cap, '/'); break;
            case 'b': put_utf8(&buf, &len, &cap, '\b'); break;
            case 'f': put_utf8(&buf, &len, &cap, '\f'); break;
            case 'n': put_utf8(&buf, &len, &cap, '\n'); break;
            case 'r': put_utf8(&buf, &len, &cap, '\r'); break;
            case 't': put_utf8(&buf, &len, &cap, '\t'); break;
            case 'u': {
                int cp;
                if (!hex4(p, &cp)) { perr(p, "bad \\u escape"); free(buf); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 1 < p->n
                    && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    int lo;
                    size_t save = p->i;
                    p->i += 2;
                    if (hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else p->i = save;
                }
                put_utf8(&buf, &len, &cap, cp);
                break;
            }
            default:
                perr(p, "unknown escape");
                free(buf);
                return NULL;
            }
        } else {
            put_utf8(&buf, &len, &cap, c);
        }
    }
    if (!buf) buf = xj_malloc(1);
    buf[len] = 0;
    return buf;
}

static Dsj *parse_value(P *p);

static Dsj *parse_object(P *p)
{
    Dsj *o = dsj_obj();
    p->i++;   /* { */
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') { p->i++; return o; }
    for (;;) {
        char *k;
        Dsj *v;
        skip_ws(p);
        k = parse_string(p);
        if (!k) { dsj_free(o); return NULL; }
        skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != ':') {
            perr(p, "expected ':'");
            free(k);
            dsj_free(o);
            return NULL;
        }
        p->i++;
        skip_ws(p);
        v = parse_value(p);
        if (!v) { free(k); dsj_free(o); return NULL; }
        push_raw(o, k, v);
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; return o; }
        perr(p, "expected ',' or '}'");
        dsj_free(o);
        return NULL;
    }
}

static Dsj *parse_array(P *p)
{
    Dsj *a = dsj_arr();
    p->i++;   /* [ */
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') { p->i++; return a; }
    for (;;) {
        Dsj *v;
        skip_ws(p);
        v = parse_value(p);
        if (!v) { dsj_free(a); return NULL; }
        push_raw(a, NULL, v);
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; return a; }
        perr(p, "expected ',' or ']'");
        dsj_free(a);
        return NULL;
    }
}

static Dsj *parse_number(P *p)
{
    size_t start = p->i;
    int is_int = 1;
    Dsj *v;
    while (p->i < p->n) {
        char c = p->s[p->i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+') { p->i++; continue; }
        if (c == '.' || c == 'e' || c == 'E') { is_int = 0; p->i++; continue; }
        break;
    }
    {
        char tmp[64];
        size_t len = p->i - start;
        if (len == 0 || len >= sizeof tmp) { perr(p, "bad number"); return NULL; }
        memcpy(tmp, p->s + start, len);
        tmp[len] = 0;
        v = node_new(DSJ_NUM);
        v->num = strtod(tmp, NULL);
        v->is_int = is_int;
    }
    return v;
}

static int lit(P *p, const char *word)
{
    size_t l = strlen(word);
    if (p->i + l <= p->n && !strncmp(p->s + p->i, word, l)) { p->i += l; return 1; }
    return 0;
}

static Dsj *parse_value(P *p)
{
    char c;
    if (p->depth > 200) { perr(p, "too deeply nested"); return NULL; }
    skip_ws(p);
    if (p->i >= p->n) { perr(p, "unexpected end of input"); return NULL; }
    c = p->s[p->i];
    if (c == '{') { Dsj *v; p->depth++; v = parse_object(p); p->depth--; return v; }
    if (c == '[') { Dsj *v; p->depth++; v = parse_array(p); p->depth--; return v; }
    if (c == '"') { char *s = parse_string(p); Dsj *v; if (!s) return NULL; v = dsj_str(s); free(s); return v; }
    if (c == 't') { if (lit(p, "true")) return dsj_bool(1); perr(p, "bad literal"); return NULL; }
    if (c == 'f') { if (lit(p, "false")) return dsj_bool(0); perr(p, "bad literal"); return NULL; }
    if (c == 'n') { if (lit(p, "null")) return dsj_null(); perr(p, "bad literal"); return NULL; }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    {
        char msg[64];
        snprintf(msg, sizeof msg, "unexpected character '%c'", c);
        perr(p, msg);
    }
    return NULL;
}

Dsj *dsj_parse(const char *text, char *err, size_t errsz)
{
    P p;
    Dsj *v;
    p.s = text ? text : "";
    p.n = strlen(p.s);
    p.i = 0;
    p.err = err;
    p.errsz = errsz;
    p.depth = 0;
    if (err && errsz) err[0] = 0;
    v = parse_value(&p);
    if (!v) return NULL;
    skip_ws(&p);
    if (p.i != p.n) {
        perr(&p, "trailing garbage");
        dsj_free(v);
        return NULL;
    }
    return v;
}

/* ------------------------------------------------------------ 序列化 */

/* 最短且能往返的十进制 + Python 的指数规则(与 tools/dexc/dexc_util.c 同规则)。
 * 整数(解析进来没带 . / e 的)直接按整数写,保持 "id": 3 而不是 3.0。 */
static void num_repr(double v, int is_int, char *out, size_t outsz)
{
    char buf[64], digits[32];
    int prec, exp10 = 0, nd = 0, i, neg = 0;
    size_t o = 0;
    const char *e;

    if (is_int && v >= -9.0e15 && v <= 9.0e15) {
        snprintf(out, outsz, "%lld", (long long)v);
        return;
    }
    if (isnan(v) || isinf(v)) { snprintf(out, outsz, "0"); return; }
    if (v == 0.0) { snprintf(out, outsz, signbit(v) ? "-0.0" : "0.0"); return; }
    for (prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        if (strtod(buf, NULL) == v) break;
    }
    if (prec > 17) snprintf(buf, sizeof buf, "%.17e", v);
    {
        const char *p = buf;
        if (*p == '-') { neg = 1; p++; }
        for (; *p && *p != 'e' && *p != 'E'; p++) {
            if (*p >= '0' && *p <= '9') {
                if (nd == 0 && *p == '0' && p[1] != '.') continue;
                if (nd < (int)sizeof digits - 1) digits[nd++] = *p;
            }
        }
        e = strchr(buf, 'e');
        if (!e) e = strchr(buf, 'E');
        exp10 = e ? atoi(e + 1) : 0;
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = 0;
    if (neg && o + 1 < outsz) out[o++] = '-';
    if (exp10 < -4 || exp10 >= 16) {
        if (o + 1 < outsz) out[o++] = digits[0];
        if (nd > 1) {
            if (o + 1 < outsz) out[o++] = '.';
            for (i = 1; i < nd && o + 1 < outsz; i++) out[o++] = digits[i];
        }
        o += (size_t)snprintf(out + o, outsz - o, "e%c%02d", exp10 < 0 ? '-' : '+',
                              exp10 < 0 ? -exp10 : exp10);
    } else if (exp10 >= 0) {
        for (i = 0; i <= exp10 && o + 1 < outsz; i++) out[o++] = (i < nd) ? digits[i] : '0';
        if (o + 1 < outsz) out[o++] = '.';
        if (nd > exp10 + 1) {
            for (i = exp10 + 1; i < nd && o + 1 < outsz; i++) out[o++] = digits[i];
        } else if (o + 1 < outsz) out[o++] = '0';
    } else {
        if (o + 2 < outsz) { out[o++] = '0'; out[o++] = '.'; }
        for (i = 0; i < -exp10 - 1 && o + 1 < outsz; i++) out[o++] = '0';
        for (i = 0; i < nd && o + 1 < outsz; i++) out[o++] = digits[i];
    }
    out[o] = 0;
}

typedef struct {
    char *p;
    size_t len, cap;
} Out;

static void out_raw(Out *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256;
        b->p = xj_realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void out_c(Out *b, char c) { out_raw(b, &c, 1); }

static void dump_str(Out *b, const char *s)
{
    out_c(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': out_raw(b, "\\\"", 2); break;
        case '\\': out_raw(b, "\\\\", 2); break;
        case '\n': out_raw(b, "\\n", 2); break;
        case '\r': out_raw(b, "\\r", 2); break;
        case '\t': out_raw(b, "\\t", 2); break;
        case '\b': out_raw(b, "\\b", 2); break;
        case '\f': out_raw(b, "\\f", 2); break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", c);
                out_raw(b, tmp, strlen(tmp));
            } else {
                out_c(b, (char)c);   /* UTF-8 原样输出(Java/JS 都认) */
            }
        }
    }
    out_c(b, '"');
}

static void dump_value(Out *b, const Dsj *v)
{
    int i;
    if (!v) { out_raw(b, "null", 4); return; }
    switch (v->t) {
    case DSJ_NULL: out_raw(b, "null", 4); break;
    case DSJ_BOOL: out_raw(b, v->b ? "true" : "false", v->b ? 4 : 5); break;
    case DSJ_NUM: {
        char tmp[80];
        num_repr(v->num, v->is_int, tmp, sizeof tmp);
        out_raw(b, tmp, strlen(tmp));
        break;
    }
    case DSJ_STR: dump_str(b, v->str); break;
    case DSJ_ARR:
        out_c(b, '[');
        for (i = 0; i < v->n; i++) {
            if (i) out_c(b, ',');
            dump_value(b, v->items[i]);
        }
        out_c(b, ']');
        break;
    case DSJ_OBJ:
        out_c(b, '{');
        for (i = 0; i < v->n; i++) {
            if (i) out_c(b, ',');
            dump_str(b, v->keys[i]);
            out_c(b, ':');
            dump_value(b, v->items[i]);
        }
        out_c(b, '}');
        break;
    }
}

char *dsj_dump(const Dsj *v)
{
    Out b;
    memset(&b, 0, sizeof b);
    dump_value(&b, v);
    if (!b.p) b.p = xj_strdup("");
    return b.p;
}
