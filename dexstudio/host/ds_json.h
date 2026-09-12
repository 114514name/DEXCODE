/* ============================================================================
 * ds_json.h — DexStudio 内部用的极小 JSON DOM。
 *
 * 为什么自己写:IDE 到处都要 JSON(project.json、场景文件、JS↔C 协议、B4 的节点图
 * 文件),而引擎那份手写 JSON(dg_json.c)是 dexgame 的内部实现,没导出。这里做一个
 * **DOM 式**(而不是游标式)的小实现:编辑器更需要"取字段、改字段、再写回去"。
 *
 * 只支持标准 JSON:对象/数组/字符串/数字/true/false/null,转义按规范处理
 * (\uXXXX 也认)。不做流式、不做注释、不做单引号 —— 与 Python json 模块一致。
 * ==========================================================================*/
#ifndef DS_JSON_H
#define DS_JSON_H

#include <stddef.h>

typedef enum { DSJ_NULL, DSJ_BOOL, DSJ_NUM, DSJ_STR, DSJ_ARR, DSJ_OBJ } DsjType;

typedef struct Dsj Dsj;

struct Dsj {
    DsjType t;
    int b;              /* BOOL */
    double num;         /* NUM */
    int is_int;         /* NUM:是否按整数写回(3 而不是 3.0) */
    char *str;          /* STR */
    Dsj **items;        /* ARR:元素;OBJ:值 */
    char **keys;        /* OBJ:键(与 items 对齐) */
    int n, cap;
};

/* ---------- 构造 ---------- */
Dsj *dsj_null(void);
Dsj *dsj_bool(int b);
Dsj *dsj_int(long long v);
Dsj *dsj_num(double v);
Dsj *dsj_str(const char *s);
Dsj *dsj_arr(void);
Dsj *dsj_obj(void);

/* ---------- 容器操作(接管 v 的所有权) ---------- */
void dsj_push(Dsj *arr, Dsj *v);
void dsj_set(Dsj *obj, const char *key, Dsj *v);      /* 已存在则替换 */
void dsj_set_str(Dsj *obj, const char *key, const char *v);
void dsj_set_int(Dsj *obj, const char *key, long long v);
void dsj_set_num(Dsj *obj, const char *key, double v);
void dsj_set_bool(Dsj *obj, const char *key, int v);

/* ---------- 读取(类型不符时返回默认值) ---------- */
Dsj *dsj_get(const Dsj *obj, const char *key);
const char *dsj_get_str(const Dsj *obj, const char *key, const char *dflt);
long long dsj_get_int(const Dsj *obj, const char *key, long long dflt);
double dsj_get_num(const Dsj *obj, const char *key, double dflt);
int dsj_get_bool(const Dsj *obj, const char *key, int dflt);
int dsj_len(const Dsj *v);                            /* ARR/OBJ 的元素数 */
Dsj *dsj_at(const Dsj *v, int i);                     /* ARR/OBJ 的第 i 个值 */

/* ---------- 解析 / 序列化 ---------- */
/* 失败返回 NULL 并往 err 写原因(带字节偏移)。 */
Dsj *dsj_parse(const char *text, char *err, size_t errsz);
/* 返回 malloc 的字符串(调用方负责 free)。紧凑格式,数字按 Python json 的风格。 */
char *dsj_dump(const Dsj *v);
/* 深拷贝。**需要它是因为所有权**:`dsj_set(o,k,v)` 会把 v 收进 o 并在 o 释放时
 * 一起释放 —— 若 v 还是另一棵树里的节点(比如 entity_comps 的返回值),
 * 那棵树一释放就会留下悬垂指针(实测:堆损坏 0xC0000374)。 */
Dsj *dsj_clone(const Dsj *v);
void dsj_free(Dsj *v);

#endif /* DS_JSON_H */
