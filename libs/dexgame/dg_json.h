/* dg_json.h — 极简 JSON 读写(dexgame 内部用)
 *
 * 为什么自己写而不是 vendor jsmn/cJSON:
 *   项目一贯手写(唯一 vendored 的是 stb_image,那是 283KB 的解码器)。
 *   这里只需 JSON 的一个子集(对象/数组/数/串/布尔),而且**读写两端都由我们
 *   自己控制**,所以自写更小、无授权负担。设计文档 §7 提到"vendor 一个小解析器",
 *   这是对它的偏离,理由是子集足够小(~250 行)。
 *
 * 写入端是流式的(带键名,自动处理逗号与缩进);读取端是游标式的(无 DOM),
 * 正好匹配"按描述符表逐字段读写"的用法。
 *
 * 读取端刻意提供 djr_skip_value():遇到不认识的字段就跳过 —— 这是 JSON 相对
 * 二进制格式的关键优势(可缺可多、向前兼容),设计文档 §7.4 正是靠它。
 */
#ifndef DG_JSON_H
#define DG_JSON_H

#include <stddef.h>
#include <stdint.h>

/* ================= 写入 ================= */
typedef struct {
    char  *buf;
    size_t len, cap;
    int    depth;                 /* 当前嵌套深度 */
    int    need_comma[32];        /* 每层是否已写过元素 */
    int    oom;                   /* 一旦失败就置位,失败要能带得出原因 */
} DgJsonW;

void djw_init(DgJsonW *w);
void djw_free(DgJsonW *w);
/* 取出结果(以 '\0' 结尾);w->oom 为真时返回 NULL */
const char *djw_text(DgJsonW *w);
/* 供增量写入(如把多个场景拼起来);需自行保证格式 */
void djw_raw(DgJsonW *w, const char *s);

void djw_obj_begin(DgJsonW *w, const char *key);
void djw_obj_end(DgJsonW *w);
void djw_arr_begin(DgJsonW *w, const char *key);
void djw_arr_end(DgJsonW *w);
void djw_int(DgJsonW *w, const char *key, long long v);
void djw_float(DgJsonW *w, const char *key, double v);
void djw_str(DgJsonW *w, const char *key, const char *v);
void djw_bool(DgJsonW *w, const char *key, int v);
/* 数组元素(没有键名) */
void djw_elem_int(DgJsonW *w, long long v);
void djw_elem_float(DgJsonW *w, double v);
void djw_elem_str(DgJsonW *w, const char *v);

/* ================= 读取 ================= */
typedef struct {
    const char *p, *end;
    int         line;
    char        err[160];
} DgJsonR;

void djr_init(DgJsonR *r, const char *text, size_t len);
const char *djr_error(DgJsonR *r);   /* 空串 = 尚未出错 */

/* 期待 '{' / '[' */
int djr_obj_begin(DgJsonR *r);
int djr_arr_begin(DgJsonR *r);
/* 对象里取下一项的键名。返回 1 = 对象到头了, 0 = 拿到键, -1 = 错误。
   拿到键之后必须消费掉该项的值(用下面的 djr_* 或 djr_skip_value)。
   **返回 1 时不消费 '}'** —— 必须再调 djr_obj_end() 收尾。*/
int djr_key(DgJsonR *r, char *key, size_t cap);
/* 数组里是否还有下一项:1 = 有, 0 = 到头了, -1 = 错误。有的话需自行消费该值。
   会顺带消费元素之间的分隔逗号(无状态迭代器,所以看到的 ',' 只可能来自上一元素之后)。
   **返回 0 时不消费 ']'** —— 必须再调 djr_arr_end() 收尾。*/
int djr_arr_more(DgJsonR *r);
int djr_obj_end(DgJsonR *r);
int djr_arr_end(DgJsonR *r);

int djr_int(DgJsonR *r, long long *out);
int djr_float(DgJsonR *r, double *out);
int djr_str(DgJsonR *r, char *out, size_t cap);
int djr_bool(DgJsonR *r, int *out);
/* 跳过任意一个值(未知字段、或不关心的字段)—— 向前兼容的关键 */
int djr_skip_value(DgJsonR *r);
/* 记录错误(只在第一次出错时写入)并返回 -1。读取端所有失败都经它,保证
   调用方永远能拿到"哪一行、期待什么、实际看到什么"。 */
int djr_fail(DgJsonR *r, const char *fmt, ...);

#endif /* DG_JSON_H */
