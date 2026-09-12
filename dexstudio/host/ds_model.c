/* ============================================================================
 * ds_model.c — DexStudio 模型层(见 ds_model.h)。
 *
 * 结构:
 *   - 场景 = **引擎的场景**(eng_init_offscreen 起的离屏实例)。IDE 不另造一套
 *     实体/组件模型:属性面板的字段表来自 `eng_comp_*` 与 `eng_field_*` 自省,
 *     场景文件就是 eng_scene_json() / eng_scene_load()。
 *   - 项目 = project.json + scenes/ + scripts/ + res/(决策 #11 的固定约定)。
 *   - 撤销 = 场景 JSON 的**快照栈**。场景只有几百个实体,快照足够便宜,而且天然
 *     覆盖所有编辑动作 —— 不必给每个命令手写一遍逆操作。命令成功才压栈,
 *     失败丢弃,所以失败操作不会污染撤销历史。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "ds_model.h"
#include "ds_engine.h"
#include "ds_json.h"
#include "ds_graph.h"
#include "ds_blocks.h"
#include "ds_run.h"
#include "ds_res.h"
#include "ds_utf8.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#define DS_VERSION "0.1.0"
#define DS_STACK_MAX 128
/* 生成的 DexLang 模块:把"游戏启动时加载哪个场景"从模板里挪出来(见 ds_write_project_info) */
#define DS_PROJECT_INFO_REL "scripts/project_info.dex"
/* 编辑器视口的离屏渲染尺寸。引擎的离屏目标不可 resize(见 ds_model_create),
 * 所以定得比一般窗口大一些:前端按 CSS 缩放显示。 */
#define DS_VIEW_W 1024
#define DS_VIEW_H 640

const char *ds_version(void) { return DS_VERSION; }

/* ------------------------------------------------------------ 撤销条目 */

/* 一条撤销记录 = 场景 JSON + 若干**不在场景里的文本文件**(路径 → 内容)。
 * 为什么要带文件:瓦片数据在外部 CSV(tilemap 的 cols/rows/texture 都是 persist=0),
 * 逻辑图在 scripts/logic.json —— 只快照场景 JSON 的话,刷完瓦片/改完节点图再撤销会
 * "看着撤销了、东西还在"。文件按**路径**记,恢复时重写文件再让引擎重新加载。 */
typedef struct {
    char *scene;     /* scene JSON 文本 */
    Dsj *files;      /* {"<绝对路径>": "<文本>", …} */
} UndoEntry;

struct DsModel {
    DsEngine eng;
    char exe_dir[1024];
    char root[1200];       /* 当前项目目录(空 = 没打开项目) */
    char scene[1200];      /* 当前场景文件绝对路径 */
    Dsj *project;          /* project.json 的 DOM */
    UndoEntry *undo; int n_undo;
    UndoEntry *redo; int n_redo;
    int cap_stack;         /* 两个栈共用容量(同时扩容) */
    int dirty;
    /* 打开/切换场景时顺手修掉的"旧模板 32×32 裁切"个数(见 fix_legacy_crops)。
     * 只改内存、不算脏、不改用户的文件 —— 但要说给用户听。 */
    int legacy_crops;
    /* 编辑器视图(视口平移/缩放)。on=0 表示"用场景里的相机" */
    int view_on;
    double view_x, view_y, view_zoom;
    int render_seq;        /* 每次渲染 +1,前端用它做 ?t= 破缓存 */
    Dsj *clipboard;        /* 实体剪贴板(entity.copy/paste),数组 */
    Dsj *graph;            /* 逻辑图的 DOM(scripts/logic.json,B4) */
    Dsj *blocks;           /* 积木脚本的 DOM(scripts/blocks.json,B11) */
    void *proc;            /* 正在独立窗口运行的游戏进程(B5) */
    unsigned long proc_pid;
    int autosave_seq;       /* 自动保存次数(B6;前端显示"已自动保存 N 次") */
    /* 本次运行的会话标识(B7 修):自动保存文件里记下"是谁写的"。
     * 只有**上一次运行**留下的自动保存才值得提示恢复 —— 不记的话,当前这次
     * 运行自己写的自动保存(比场景新)会让界面一直喊"上次好像没有正常退出"。 */
    char session[64];
    char *resp;
    size_t resp_cap;
    int has_id;            /* 请求里带了 id → 响应原样带回(前端靠它配对 Promise) */
    long long resp_id;
    char ui_error[512];    /* 前端报回来的 JS 错误(诊断用,app.info 里能看到) */
    char err[512];
};

/* ------------------------------------------------------------ 小工具 */

static void *xm(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "dexstudio: out of memory\n"); exit(2); }
    return p;
}

static void *xr(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "dexstudio: out of memory\n"); exit(2); }
    return q;
}

static char *xs(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = xm(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static void seterr(DsModel *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->err, sizeof m->err, fmt, ap);
    va_end(ap);
}

static char *pjoin(const char *a, const char *b)
{
    size_t la = strlen(a);
    char *out;
    if (!la) return xs(b);
    out = xm(la + strlen(b) + 2);
#if defined(_WIN32)
    sprintf(out, "%s\\%s", a, b);
#else
    sprintf(out, "%s/%s", a, b);
#endif
    return out;
}

static int path_is_abs(const char *p)
{
#if defined(_WIN32)
    return (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/';
#else
    return p[0] == '/';
#endif
}

static int path_exists(const char *p)
{
    return dsu_exists(p);
}

static int ensure_dir(const char *path)
{
    return dsu_mkdir(path);
}

static char *read_text(const char *path, size_t *out_len)
{
    FILE *f = dsu_fopen(path, "rb");
    long sz;
    char *buf;
    size_t got;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = xm((size_t)sz + 1);
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB
        && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 3 + 1);
        got -= 3;
    }
    if (out_len) *out_len = got;
    return buf;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = dsu_fopen(path, "wb");
    if (!f) return 0;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 1;
}

/* 文件小工具的实现(ds_graph.c 也用同一份,避免两份行为不一致) */
int ds_mkdir(const char *path) { return ensure_dir(path); }
int ds_write_text(const char *path, const char *text) { return write_text(path, text); }
char *ds_read_text(const char *path, size_t *out_len) { return read_text(path, out_len); }
char *ds_strdup(const char *s) { return xs(s ? s : ""); }
char *ds_path_join(const char *a, const char *b) { return pjoin(a ? a : ".", b ? b : ""); }
char *ds_file_read_text(const char *path, size_t *out_len) { return read_text(path, out_len); }

/* "x.dex" + ".dexbc" → "x.dexbc"(把最后一个扩展名换掉) */
char *ds_path_replace_ext(const char *path, const char *newext)
{
    const char *dot, *slash, *slash2;
    size_t n;
    char *out;
    if (!path) return NULL;
    slash = strrchr(path, '\\');
    slash2 = strrchr(path, '/');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    dot = strrchr(path, '.');
    if (dot && slash && dot < slash) dot = NULL;   /* 点在目录里,不算扩展名 */
    n = dot ? (size_t)(dot - path) : strlen(path);
    out = xm(n + strlen(newext ? newext : "") + 1);
    memcpy(out, path, n);
    out[n] = 0;
    strcat(out, newext ? newext : "");
    return out;
}

char *ds_model_errbuf(DsModel *m) { return m->err; }
size_t ds_model_errbuf_size(void) { return sizeof(((DsModel *)0)->err); }
const char *ds_model_exe_dir(DsModel *m) { return m->exe_dir; }
void **ds_model_proc_slot(DsModel *m) { return &m->proc; }
unsigned long *ds_model_pid_slot(DsModel *m) { return &m->proc_pid; }

/* 文件存在(不是目录)*/
static int file_exists(const char *path) { return path_exists(path); }

/* 值数组 ABI 的实参构造 */
static DexValue V_i(int64_t x) { DexValue v; v.tag = DEXV_INT; v.as.i = x; return v; }
static DexValue V_f(double x) { DexValue v; v.tag = DEXV_FLOAT; v.as.f = x; return v; }
static DexValue V_s(const char *x) { DexValue v; v.tag = DEXV_STR; v.as.s = x ? x : ""; return v; }

static const char *obj_name(DsModel *m, int64_t id)
{
    DexValue a[1];
    const char *s;
    if (!m->eng.ok) return "";
    a[0] = V_i(id);
    s = m->eng.name(a, 1);
    return s ? s : "";
}

static double obj_world(DsModel *m, int64_t id, DsFnF fn)
{
    DexValue a[1];
    (void)m;
    a[0] = V_i(id);
    return fn(a, 1);
}

/* ------------------------------------------------------------ 响应构造 */

static const char *resp_ok(DsModel *m, Dsj *result)
{
    Dsj *r = dsj_obj();
    char *txt;
    if (m->has_id) dsj_set_int(r, "id", m->resp_id);
    dsj_set_bool(r, "ok", 1);
    if (result) dsj_set(r, "result", result);
    txt = dsj_dump(r);
    if (strlen(txt) + 1 > m->resp_cap) {
        m->resp_cap = strlen(txt) + 256;
        m->resp = xr(m->resp, m->resp_cap);
    }
    memcpy(m->resp, txt, strlen(txt) + 1);
    free(txt);
    dsj_free(r);
    return m->resp;
}

static const char *resp_err(DsModel *m, const char *msg)
{
    Dsj *r = dsj_obj();
    char *txt;
    if (m->has_id) dsj_set_int(r, "id", m->resp_id);
    dsj_set_bool(r, "ok", 0);
    dsj_set_str(r, "error", msg);
    txt = dsj_dump(r);
    if (strlen(txt) + 1 > m->resp_cap) {
        m->resp_cap = strlen(txt) + 256;
        m->resp = xr(m->resp, m->resp_cap);
    }
    memcpy(m->resp, txt, strlen(txt) + 1);
    free(txt);
    dsj_free(r);
    return m->resp;
}

/* ------------------------------------------------------------ 撤销栈 */

static char *scene_snapshot(DsModel *m)
{
    DexValue none[1];
    const char *s;
    if (!m->eng.ok) return NULL;
    s = m->eng.scene_json(none, 0);
    return s ? xs(s) : NULL;
}

/* 记录当前各瓦片地图的 CSV 文本(按文件路径)。读不到就跳过(例如 path 为空)。 */
static Dsj *tilemap_snapshot(DsModel *m)
{
    Dsj *o = dsj_obj();
    int n, i;
    if (!m->eng.ok) return o;
    n = (int)m->eng.object_count(NULL, 0);
    for (i = 0; i < n; i++) {
        DexValue a[4];
        int64_t id;
        char path[1024];
        a[0] = V_i(i);
        id = m->eng.object_id_at(a, 1);
        if (!id) continue;
        a[0] = V_i(id); a[1] = V_s("tilemap");
        if (m->eng.has(a, 2) != 1) continue;
        a[0] = V_i(id); a[1] = V_s("tilemap"); a[2] = V_s("path");
        snprintf(path, sizeof path, "%s", m->eng.get_s(a, 3));
        if (!path[0]) continue;
        if (!dsj_get(o, path)) {
            size_t len = 0;
            char *txt = read_text(path, &len);
            if (txt) {
                dsj_set_str(o, path, txt);
                free(txt);
            }
        }
    }
    return o;
}

/* 逻辑图(节点图)文件也是"不在场景 JSON 里的编辑状态",同样要进撤销快照。
 * 它只有一个固定路径,所以直接按约定拼出来(没有项目时为空)。 */
char *ds_graph_path(DsModel *m)
{
    if (!m->root[0]) return NULL;
    return pjoin(m->root, DS_GRAPH_REL);
}

/* 快照里要额外记的**文本文件**(路径 → 内容):瓦片 CSV + 逻辑图 JSON。
 * 逻辑图取**内存里的那一份**而不是读文件:编辑器改图之后可能还没存盘,
 * 从磁盘读会把"上一次存盘"当快照,撤销就退过头了。 */
static Dsj *files_snapshot(DsModel *m)
{
    Dsj *o = tilemap_snapshot(m);
    char *g = ds_graph_path(m);
    if (g) {
        const char *txt = ds_graph_json(m);
        if (txt) dsj_set_str(o, g, txt);
        free(g);
    }
    /* 积木也是"不在场景 JSON 里的编辑状态",同样要进快照(取内存里的那一份) */
    if (m->root[0]) {
        char *bp = pjoin(m->root, DS_BLOCKS_REL);
        const char *btxt = ds_blocks_json(m);
        if (btxt) dsj_set_str(o, bp, btxt);
        free(bp);
    }
    return o;
}

static UndoEntry *undo_new_entry(DsModel *m)
{
    UndoEntry *e = xm(sizeof *e);
    e->scene = scene_snapshot(m);
    e->files = files_snapshot(m);
    return e;
}

/* 只释放内容。栈里的条目是**数组内元素**,不是单独分配的 —— 对它们调 free()
 * 就是 free 一个内指针(实测:堆损坏 0xC0000374,而且崩在销毁时,离病因很远)。 */
static void undo_free_contents(UndoEntry *e)
{
    if (!e) return;
    free(e->scene);
    e->scene = NULL;
    if (e->files) dsj_free(e->files);
    e->files = NULL;
}

/* 释放一条**堆上单独分配**的条目(undo_new_entry 的返回值/临时快照) */
static void undo_free_entry(UndoEntry *e)
{
    if (!e) return;
    undo_free_contents(e);
    free(e);
}

static void stacks_clear(DsModel *m)
{
    int i;
    for (i = 0; i < m->n_undo; i++) undo_free_contents(&m->undo[i]);
    for (i = 0; i < m->n_redo; i++) undo_free_contents(&m->redo[i]);
    m->n_undo = m->n_redo = 0;
    m->dirty = 0;
}

static void redo_clear(DsModel *m)
{
    int i;
    for (i = 0; i < m->n_redo; i++) undo_free_contents(&m->redo[i]);
    m->n_redo = 0;
}

static void stack_ensure(DsModel *m)
{
    if (m->cap_stack == 0) {
        m->cap_stack = 32;
        m->undo = xr(m->undo, (size_t)m->cap_stack * sizeof *m->undo);
        m->redo = xr(m->redo, (size_t)m->cap_stack * sizeof *m->redo);
    }
}

static void stack_grow(DsModel *m)
{
    int newcap = m->cap_stack * 2;
    if (newcap > DS_STACK_MAX) newcap = DS_STACK_MAX;
    stack_ensure(m);
    if (newcap == m->cap_stack) return;
    m->undo = xr(m->undo, (size_t)newcap * sizeof *m->undo);
    m->redo = xr(m->redo, (size_t)newcap * sizeof *m->redo);
    m->cap_stack = newcap;
}

static void undo_push(DsModel *m, UndoEntry *e)
{
    if (!e) return;
    stack_ensure(m);
    if (m->n_undo == m->cap_stack) {
        if (m->cap_stack >= DS_STACK_MAX) {
            undo_free_contents(&m->undo[0]);
            memmove(m->undo, m->undo + 1, (size_t)(m->n_undo - 1) * sizeof *m->undo);
            m->n_undo--;
        } else {
            stack_grow(m);
        }
    }
    m->undo[m->n_undo++] = *e;
    free(e);
    redo_clear(m);
}

/* 把"场景 JSON + 外部文本文件"写回:撤销恢复与自动保存恢复共用这一份。
 * 外部文件(瓦片 CSV)写完要让引擎重新加载,逻辑图写完要让内存里的图重新读。 */
int ds_scene_restore(DsModel *m, const char *scene_json, Dsj *files)
{
    DexValue a[1];
    int i;
    a[0] = V_s(scene_json ? scene_json : "{}");
    if (m->eng.scene_load_json(a, 1) != 0) {
        seterr(m, "恢复场景失败:%s", ds_engine_last_error(&m->eng));
        return 0;
    }
    for (i = 0; i < dsj_len(files); i++) {
        const char *path = files->keys[i];
        const char *text = dsj_at(files, i)->str;
        int64_t id;
        DexValue b[2];
        write_text(path, text);
        /* 场景是刚恢复的,实体 id 变了 → 按路径找回那个 tilemap 实体 */
        {
            int n = (int)m->eng.object_count(NULL, 0), k;
            for (k = 0; k < n; k++) {
                char cur[1024];
                b[0] = V_i(k);
                id = m->eng.object_id_at(b, 1);
                if (!id) continue;
                b[0] = V_i(id); b[1] = V_s("tilemap");
                if (m->eng.has(b, 2) != 1) continue;
                {
                    DexValue c[3];
                    c[0] = V_i(id); c[1] = V_s("tilemap"); c[2] = V_s("path");
                    snprintf(cur, sizeof cur, "%s", m->eng.get_s(c, 3));
                }
                if (!strcmp(cur, path)) {
                    DexValue d[2];
                    d[0] = V_i(id); d[1] = V_s(path);
                    m->eng.tilemap_load_file(d, 2);
                }
            }
        }
    }
    /* 逻辑图文件写回之后,内存里的图也要跟着回到那一版 */
    ds_graph_reload(m);
    ds_blocks_reload(m);
    m->dirty = 1;
    return 1;
}

/* 把一份撤销快照写回 */
static int restore_entry(DsModel *m, UndoEntry *e)
{
    return ds_scene_restore(m, e->scene, e->files);
}

/* 撤销/重做:把"当前"压到对面的栈,再从本栈弹出快照恢复 */
static int undo_apply(DsModel *m, int is_redo)
{
    UndoEntry *from = is_redo ? m->redo : m->undo;
    int *nfrom = is_redo ? &m->n_redo : &m->n_undo;
    UndoEntry **to = is_redo ? &m->undo : &m->redo;
    int *nto = is_redo ? &m->n_undo : &m->n_redo;
    UndoEntry *snap, *cur;
    int ok;

    if (*nfrom == 0) {
        seterr(m, is_redo ? "没有可重做的操作" : "没有可撤销的操作");
        return 0;
    }
    stack_ensure(m);
    if (*nto == m->cap_stack && m->cap_stack < DS_STACK_MAX) stack_grow(m);
    snap = xm(sizeof *snap);
    *snap = from[*nfrom - 1];
    cur = undo_new_entry(m);
    (*to)[(*nto)++] = *cur;
    free(cur);
    (*nfrom)--;
    ok = restore_entry(m, snap);
    undo_free_entry(snap);
    return ok;
}

/* 编辑包装:成功才把快照压栈 */
typedef struct { DsModel *m; UndoEntry *snap; } Edit;

static Edit edit_begin(DsModel *m)
{
    Edit e;
    e.m = m;
    e.snap = undo_new_entry(m);
    return e;
}

static void edit_end(Edit *e, int changed)
{
    if (changed) {
        undo_push(e->m, e->snap);
        e->m->dirty = 1;
    } else {
        undo_free_entry(e->snap);
    }
    e->snap = NULL;
}

/* ------------------------------------------------------------ 自省数据 */

static const char *field_type_name_of(int t)
{
    switch (t) {
    case 0: return "int";
    case 1: return "float";
    case 2: return "bool";
    case 3: return "string";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------ 字段元数据
 *
 * 自省只给"名字 + 类型码",而 IDE 需要知道**人类语义**:这个数字是枚举还是颜色、
 * 这个字符串是图片还是声音、这个字段改了到底有没有用。所以在这里补一张表 ——
 * 引擎加组件/加字段时只在这里补一行,前端不在任何地方硬编码字段名单
 * (以前"哪些 int 当布尔"的名单是写死在 JS 里的,引擎加字段就会漏)。
 *
 * kind 决定前端用哪种控件:
 *   number  数字框(带上下键/滑杆)   bool   复选
 *   enum    下拉(值域由 items 给)    flags  多选位(复选组合)
 *   color   取色器                   image  图片资源下拉
 *   audio   声音资源下拉             entity 实体下拉
 *   text    文本框                   readonly 只读(运行期字段/引擎未实现的字段)
 * items / min / max / step / unit / hint 都是给 UI 的提示,不参与逻辑。
 */
typedef struct { int value; const char *label; } DsEnumItem;

static const DsEnumItem ENUM_COLLIDER_KIND[] = {
    {0, "矩形(AABB)"}, {1, "圆"}, {2, "胶囊"}, {0, NULL}
};
static const DsEnumItem ENUM_BODY_MOTION[] = {
    {0, "静态(不动)"}, {1, "运动学(自己移动)"}, {2, "动态(受物理影响)"}, {0, NULL}
};
/* flip 是**位掩码**:1 横向、2 纵向,可以同时选 */
static const DsEnumItem ENUM_FLIP[] = {
    {1, "横向翻转"}, {2, "纵向翻转"}, {0, NULL}
};

typedef struct {
    const char *comp;
    const char *field;
    const char *label;     /* 中文标签;空 = 用字段名 */
    const char *kind;      /* NULL = 按类型码推断 */
    const char *unit;      /* 单位提示(像素/秒/倍率…) */
    const char *hint;      /* 补充说明(tooltip) */
    const DsEnumItem *items;
    double min, max, step; /* step<=0 = 用默认 */
    int readonly;          /* 改了没有效果,或不该手改 */
} DsFieldMeta;

static const DsFieldMeta FIELD_META[] = {
    /* --- transform --- */
    {.comp="transform", .field="x", .label="X 位置", .kind="number", .unit="像素", .step=1},
    {.comp="transform", .field="y", .label="Y 位置", .kind="number", .unit="像素", .step=1},
    {.comp="transform", .field="rot", .label="旋转", .readonly=1,
     .hint="引擎目前不读这个字段(2D 一期没做旋转),改了什么都不会变"},
    {.comp="transform", .field="sx", .label="缩放 X", .step=0.1, .min=0.01, .max=64,
     .hint="1 = 原大小;0.5 = 一半;2 = 两倍。图太大就用它缩小"},
    {.comp="transform", .field="sy", .label="缩放 Y", .step=0.1, .min=0.01, .max=64,
     .hint="1 = 原大小;要等比缩放就把 X 和 Y 填一样的数"},
    {.comp="transform", .field="parent", .label="父实体", .kind="entity", .step=1,
     .hint="跟着父实体一起移动(只继承位置)"},
    /* --- sprite --- */
    {.comp="sprite", .field="tex_path", .label="贴图", .kind="image",
     .hint="项目相对路径(点左侧资源缩略图也能设)"},
    {.comp="sprite", .field="texture", .label="纹理句柄", .readonly=1,
     .hint="引擎加载后给的编号;失败时是 -1"},
    {.comp="sprite", .field="sx", .label="裁切起点 X", .unit="像素",
     .hint="从贴图里截取的起点(整张图就保持 0)"},
    {.comp="sprite", .field="sy", .label="裁切起点 Y", .unit="像素",
     .hint="从贴图里截取的起点(整张图就保持 0)"},
    {.comp="sprite", .field="sw", .label="裁切宽", .unit="像素",
     .hint="0 = 画整张贴图(推荐);只有用图集切图时才填"},
    {.comp="sprite", .field="sh", .label="裁切高", .unit="像素",
     .hint="0 = 画整张贴图(推荐);只有用图集切图时才填"},
    {.comp="sprite", .field="px", .label="轴心 X", .hint="0~1,0.5 = 居中"},
    {.comp="sprite", .field="py", .label="轴心 Y", .hint="0~1,0.5 = 居中"},
    {.comp="sprite", .field="tint", .label="颜色", .kind="color",
     .hint="整体染色(白 = 不染色)"},
    {.comp="sprite", .field="flip", .label="翻转", .kind="flags", .items=ENUM_FLIP},
    {.comp="sprite", .field="layer", .label="图层", .step=1,
     .hint="小的先画(在下面)"},
    {.comp="sprite", .field="order", .label="同层顺序", .step=1},
    /* --- camera --- */
    {.comp="camera", .field="x", .label="X", .kind="number", .unit="像素", .step=1,
     .hint="相机 (x,y) = 显示在屏幕左上角的世界坐标"},
    {.comp="camera", .field="y", .label="Y", .kind="number", .unit="像素", .step=1},
    {.comp="camera", .field="zoom", .label="缩放", .hint="每世界单位画几个像素"},
    {.comp="camera", .field="rot", .label="旋转", .readonly=1,
     .hint="引擎目前不读这个字段"},
    {.comp="camera", .field="active", .label="启用", .kind="bool",
     .hint="场景里同时只有一个相机生效(第一个启用的)"},
    /* --- animation --- */
    {.comp="animation", .field="first", .label="起始图块", .step=1},
    {.comp="animation", .field="count", .label="帧数", .step=1},
    {.comp="animation", .field="fps", .label="帧率"},
    {.comp="animation", .field="loop", .label="循环", .kind="bool"},
    {.comp="animation", .field="time", .label="已播放", .readonly=1},
    /* --- collider --- */
    {.comp="collider", .field="kind", .label="形状", .kind="enum",
     .items=ENUM_COLLIDER_KIND, .step=1},
    {.comp="collider", .field="hw", .label="半宽", .unit="像素"},
    {.comp="collider", .field="hh", .label="半高", .unit="像素"},
    {.comp="collider", .field="ox", .label="偏移 X", .unit="像素"},
    {.comp="collider", .field="oy", .label="偏移 Y", .unit="像素"},
    {.comp="collider", .field="is_trigger", .label="只是触发器", .kind="bool",
     .hint="不挡路,但查询/重叠事件仍能看到它"},
    {.comp="collider", .field="layer", .label="碰撞层", .step=1},
    {.comp="collider", .field="mask", .label="可碰的层", .step=1,
     .hint="位掩码;0 = 不做层筛选"},
    /* --- body --- */
    {.comp="body", .field="motion", .label="运动模式", .kind="enum",
     .items=ENUM_BODY_MOTION, .step=1},
    {.comp="body", .field="vx", .label="速度 X", .unit="像素/秒"},
    {.comp="body", .field="vy", .label="速度 Y", .unit="像素/秒"},
    {.comp="body", .field="gravity_scale", .label="重力倍率", .step=0.1},
    {.comp="body", .field="friction", .label="摩擦", .min=0, .max=1, .step=0.05},
    {.comp="body", .field="restitution", .label="弹性", .min=0, .max=1, .step=0.05},
    {.comp="body", .field="sleeping", .label="休眠中", .kind="bool", .readonly=1},
    {.comp="body", .field="px", .label="上一帧 X", .readonly=1},
    {.comp="body", .field="py", .label="上一帧 Y", .readonly=1},
    {.comp="body", .field="stepped", .label="已积分", .readonly=1},
    {.comp="body", .field="flags", .label="状态位", .readonly=1},
    /* --- tilemap --- */
    {.comp="tilemap", .field="path", .label="瓦片数据(CSV)", .readonly=1,
     .hint="用右侧「新建瓦片地图」生成,不用手填"},
    {.comp="tilemap", .field="tex_path", .label="图集", .kind="image"},
    {.comp="tilemap", .field="texture", .label="纹理句柄", .readonly=1},
    {.comp="tilemap", .field="cols", .label="列数", .readonly=1},
    {.comp="tilemap", .field="rows", .label="行数", .readonly=1},
    {.comp="tilemap", .field="tw", .label="格子宽", .unit="像素"},
    {.comp="tilemap", .field="th", .label="格子高", .unit="像素"},
    {.comp="tilemap", .field="atlas_tile", .label="首块编号", .step=1},
    {.comp="tilemap", .field="atlas_cols", .label="图集列数", .step=1,
     .hint="图集里一行有几个格子(决定每块的 UV)"},
    {.comp="tilemap", .field="layer", .label="图层", .step=1},
    {.comp="tilemap", .field="order", .label="同层顺序", .step=1},
    {.comp="tilemap", .field="visible", .label="可见", .kind="bool"},
    /* --- audio --- */
    {.comp="audio", .field="path", .label="声音", .kind="audio"},
    {.comp="audio", .field="volume", .label="音量", .min=0, .max=1, .step=0.05},
    {.comp="audio", .field="loop", .label="循环", .kind="bool"},
    {.comp="audio", .field="play_on_start", .label="进场就播", .kind="bool"},
    {.comp="audio", .field="handle", .label="声音句柄", .readonly=1},
    {.comp="audio", .field="voice", .label="播放实例", .readonly=1},
    {.comp="audio", .field="playing", .label="正在播放", .readonly=1},
};

static const DsFieldMeta *field_meta(const char *comp, const char *field)
{
    int i;
    if (!comp || !field) return NULL;
    for (i = 0; i < (int)(sizeof FIELD_META / sizeof FIELD_META[0]); i++) {
        if (!strcmp(FIELD_META[i].comp, comp) && !strcmp(FIELD_META[i].field, field))
            return &FIELD_META[i];
    }
    return NULL;
}

static void meta_put_enum(Dsj *o, const DsEnumItem *items)
{
    Dsj *a = dsj_arr();
    int i;
    for (i = 0; items && items[i].label; i++) {
        Dsj *it = dsj_obj();
        dsj_set_int(it, "value", items[i].value);
        dsj_set_str(it, "label", items[i].label);
        dsj_push(a, it);
    }
    dsj_set(o, "enum", a);
}

/* eng_comp_name_at 的索引是 **1 基**(0 会报越界)——引擎只在这里用 1 基,
 * 其余(字段名/字段类型)都是 0 基。这个不一致踩过一次:漏掉了最后一个组件(audio)。 */
static int comp_total(DsModel *m)
{
    return m->eng.ok ? (int)m->eng.comp_count(NULL, 0) : 0;
}

static Dsj *comp_names(DsModel *m)
{
    Dsj *a = dsj_arr();
    int n, i;
    if (!m->eng.ok) return a;
    n = comp_total(m);
    for (i = 1; i <= n; i++) {
        DexValue v[1];
        const char *nm;
        v[0] = V_i(i);
        nm = m->eng.comp_name_at(v, 1);
        if (nm && *nm) dsj_push(a, dsj_str(nm));
    }
    return a;
}

static Dsj *entity_comps(DsModel *m, int64_t id)
{
    Dsj *o = dsj_obj();
    int n, c;
    if (!m->eng.ok) return o;
    n = comp_total(m);
    for (c = 1; c <= n; c++) {
        DexValue a[3];
        const char *cname;
        Dsj *fields;
        int nf, f;
        a[0] = V_i(c);
        cname = m->eng.comp_name_at(a, 1);
        if (!cname || !*cname) continue;
        a[0] = V_i(id); a[1] = V_s(cname);
        if (m->eng.has(a, 2) != 1) continue;
        fields = dsj_obj();
        a[0] = V_s(cname); a[1] = V_i(0);
        nf = (int)m->eng.field_count(a, 2);
        for (f = 0; f < nf; f++) {
            const char *fname;
            int ft;
            a[0] = V_s(cname); a[1] = V_i(f);
            fname = m->eng.field_name(a, 2);
            if (!fname || !*fname) continue;
            ft = (int)m->eng.field_type(a, 2);
            {
                DexValue g[3];
                g[0] = V_i(id); g[1] = V_s(cname); g[2] = V_s(fname);
                if (ft == 2) dsj_set_int(fields, fname, (long long)m->eng.get_i(g, 3));
                else if (ft == 1) dsj_set_num(fields, fname, m->eng.get_f(g, 3));
                else if (ft == 3) dsj_set_str(fields, fname, m->eng.get_s(g, 3));
                else dsj_set_int(fields, fname, (long long)m->eng.get_i(g, 3));
            }
        }
        dsj_set(o, cname, fields);
    }
    return o;
}

/* ------------------------------------------------------------ 项目 */

static const char *MAIN_DEX_TEMPLATE =
    "# DexStudio 生成的入口脚本 —— 这个文件归你,可以随便改\n"
    "#   · 逻辑图/积木生成的代码在 scripts/logic.dex(别手改那个,改积木)\n"
    "#   · 起始场景与窗口标题在 scripts/project_info.dex(由 IDE 生成),\n"
    "#     所以下面不用写死 scenes/xxx.json —— 换场景不必改这个文件\n"
    "#   · 编译:tools/dexc/dexc.exe compile scripts/main.dex\n"
    "include \"dexgame\";\n"
    "include \"dexgame_fast\";\n"
    "include \"logic\";\n"
    "include \"project_info\";\n"
    "\n"
    "func on_start() {\n"
    "    eng_set_clear_color(eng_rgba(30, 30, 46, 255));\n"
    "    eng_bind_default_actions();   # left/right/up/down/jump/action/back\n"
    "    eng_scene_load(dexstudio_start_scene());\n"
    "    logic_start(0.0);\n"
    "}\n"
    "\n"
    "func on_update(dt: float) {\n"
    "    logic_update(dt);\n"
    "}\n"
    "\n"
    "func on_draw() {\n"
    "    eng_draw_scene();\n"
    "    logic_draw(0.0);\n"
    "}\n"
    "\n"
    "# 开窗口必须**在 on_start 之前**:on_start 要加载场景(带贴图的场景需要 D3D 设备),\n"
    "# 而且 eng_run 只在窗口活着的时候循环 —— 少了这一句,游戏进程 60ms 就退出了。\n"
    "if eng_init(dexstudio_game_title(), 960, 540, 1) != 0 {\n"
    "    print \"启动游戏窗口失败: \" + eng_last_error();\n"
    "}\n"
    "eng_run(\"on_start\", \"on_update\", \"on_draw\");\n";

static Dsj *project_default(const char *name)
{
    Dsj *p = dsj_obj();
    Dsj *scripts = dsj_arr();
    Dsj *run = dsj_obj();
    dsj_set_str(p, "name", name);
    dsj_set_int(p, "format", 1);
    dsj_set_str(p, "start_scene", "scenes/main.json");
    /* 新项目默认用**积木**(零基础用户的入口;节点图是"进阶"那一档)。
     * 老项目(没有这个键)一律按 graph 处理,见 ds_model_logic_mode。 */
    dsj_set_str(p, "logic_mode", "blocks");
    dsj_push(scripts, dsj_str("scripts/main.dex"));
    dsj_set(p, "scripts", scripts);
    dsj_set_str(p, "res", "res");
    dsj_set_str(p, "engine_dll", "");
    dsj_set_str(run, "vm", "vm/vm.exe");
    dsj_set_str(run, "dexc", "tools/dexc/dexc.exe");
    dsj_set(p, "run", run);
    return p;
}

static int scene_save_to(DsModel *m, const char *full)
{
    DexValue a[1];
    a[0] = V_s(full);
    if (m->eng.scene_save(a, 1) != 0) {
        seterr(m, "保存场景失败:%s", ds_engine_last_error(&m->eng));
        return 0;
    }
    snprintf(m->scene, sizeof m->scene, "%s", full);
    m->dirty = 0;
    return 1;
}

static int project_save(DsModel *m)
{
    char *path, *txt;
    int ok;
    if (!m->root[0] || !m->project) {
        seterr(m, "还没有打开项目");
        return 0;
    }
    path = pjoin(m->root, "project.json");
    txt = dsj_dump(m->project);
    ok = write_text(path, txt);
    if (ok) {
        FILE *f = dsu_fopen(path, "ab");
        if (f) { fputc('\n', f); fclose(f); }
    }
    free(txt);
    free(path);
    if (!ok) { seterr(m, "无法写入 project.json"); return 0; }
    if (m->scene[0] && !scene_save_to(m, m->scene)) return 0;
    /* 逻辑图也是项目的一部分:以前只有「保存图」/编译/自动保存才写它,
     * 于是 Ctrl+S 在逻辑图模式下"看着保存了"其实没有(实测漏过)。 */
    if (m->root[0] && !ds_graph_save(m)) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", m->err[0] ? m->err : "未知原因");
        seterr(m, "保存逻辑图失败:%s", tmp);
        return 0;
    }
    if (m->root[0] && !ds_blocks_save(m)) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", m->err[0] ? m->err : "未知原因");
        seterr(m, "保存积木脚本失败:%s", tmp);
        return 0;
    }
    ds_write_project_info(m);
    m->dirty = 0;
    return 1;
}

/* 把当前场景记成项目的"起始场景"(项目相对路径,正斜杠),这样下次打开项目 * 就回到你上次在编辑的那个场景。不记的话:新建/切换场景 → 存盘 → 重开,
 * 又回到 scenes/main.json —— 摆好的东西其实还在那个场景文件里,但用户会以为丢了。 */
static void project_set_start_scene(DsModel *m, const char *full)
{
    const char *root = ds_project_dir(m);
    char *norm;
    size_t i, rl;
    if (!m->project || !full || !*full) return;
    if (root && *root && !strncmp(full, root, (rl = strlen(root)))
        && (full[rl] == '\\' || full[rl] == '/')) {
        norm = ds_strdup(full + rl + 1);
    } else {
        norm = ds_strdup(full);
    }
    for (i = 0; norm[i]; i++) if (norm[i] == '\\') norm[i] = '/';
    dsj_set_str(m->project, "start_scene", norm);
    free(norm);
}

/* 切换当前场景到 <root>/<rel>;rel 空则用 project.start_scene */
/* 旧模板留下的 32×32 裁切修正(实现在 cmd_comp_set 旁边,scene_switch 也要用) */
static int fix_legacy_crop(DsModel *m, int64_t id);
static int fix_legacy_crops(DsModel *m);

static int scene_switch(DsModel *m, const char *rel, int load)
{
    char *full;
    const char *r = rel;
    if (!r || !*r) r = dsj_get_str(m->project, "start_scene", "scenes/main.json");
    full = path_is_abs(r) ? xs(r) : pjoin(m->root, r);
    if (load && m->eng.ok) {
        DexValue a[1];
        a[0] = V_s(full);
        if (m->eng.scene_load(a, 1) != 0) {
            if (path_exists(full)) {
                seterr(m, "加载场景失败:%s", ds_engine_last_error(&m->eng));
                free(full);
                return 0;
            }
            /* 文件还不存在:新场景,清空即可 */
            {
                DexValue none[1];
                m->eng.scene_clear(none, 0);
            }
        }
    }
    snprintf(m->scene, sizeof m->scene, "%s", full);
    project_set_start_scene(m, full);
    /* 旧模板/旧版本留下的 32×32 裁切:在内存里改成"整张贴图"。
     * 这样用户打开老项目**立刻**能看到整张图,而不是"贴图设了却只有一角"。
     * 刻意不算脏:用户什么都没动,不该在关窗时被问"要保存吗"。 */
    m->legacy_crops = (load && m->eng.ok) ? fix_legacy_crops(m) : 0;
    /* 起始场景变了 → 同步生成的 project_info.dex(游戏启动时读的就是它) */
    if (m->root[0]) ds_write_project_info(m);
    free(full);
    stacks_clear(m);
    return 1;
}

/* ------------------------------------------------------------ 命令实现 */

/* 把项目根设成引擎的"资源根"。为什么必须有这一步:
 * 场景里存的是**项目相对**路径(`res/hero.png` 这种,才可搬),而 IDE 自己那个
 * 引擎实例的工作目录是 IDE 的目录 —— 不告诉引擎"相对谁",贴图/声音/瓦片 CSV
 * 全部加载失败("设置 sprite.tex_path 失败: cannot open image 'res/hero.png'")。
 * 游戏那边由 ds_run.c 以**项目根为工作目录**启动,所以天然一致。 */
static void apply_asset_dir(DsModel *m)
{
    DexValue a[1];
    if (!m->eng.ok || !m->eng.set_asset_dir) return;
    a[0] = V_s(m->root);
    m->eng.set_asset_dir(a, 1);
}


/* 没有视图覆盖时,视口用的是**场景里的活动相机**(第一个 active 的 camera 组件,
 * 字段是 camera.x/y/zoom,不是 transform)。前端画网格/选中框用的是同一套坐标变换,
 * 所以 app.info 里必须报**实际生效**的视图 —— 否则用户把相机一挪,画面跟着挪、
 * 框线却不挪,看起来就是"框线和图像对不上"。 */
static void effective_view(DsModel *m, double *x, double *y, double *zoom)
{
    DexValue none[1];
    int n, i;
    *x = 0; *y = 0; *zoom = 1;
    if (!m->eng.ok) return;
    n = (int)m->eng.object_count(none, 0);
    for (i = 0; i < n; i++) {
        DexValue a[3];
        int64_t id;
        a[0] = V_i(i);
        id = m->eng.object_id_at(a, 1);
        if (!id) continue;
        a[0] = V_i(id); a[1] = V_s("camera");
        if (m->eng.has(a, 2) != 1) continue;
        a[2] = V_s("active");
        if (m->eng.get_i(a, 3) != 1) continue;
        a[2] = V_s("x");    *x = m->eng.get_f(a, 3);
        a[2] = V_s("y");    *y = m->eng.get_f(a, 3);
        a[2] = V_s("zoom"); *zoom = m->eng.get_f(a, 3);
        if (*zoom <= 0.0) *zoom = 1.0;
        return;
    }
}

static Dsj *cmd_app_info(DsModel *m)
{
    Dsj *r = dsj_obj();
    DexValue none[1];
    dsj_set_str(r, "version", DS_VERSION);
    dsj_set_bool(r, "engine", m->eng.ok);
    dsj_set_str(r, "engine_error", m->eng.ok ? "" : m->eng.err);
    dsj_set_str(r, "root", m->root);
    dsj_set_str(r, "project_name", m->project ? dsj_get_str(m->project, "name", "") : "");
    dsj_set_str(r, "scene", m->scene);
    dsj_set_bool(r, "dirty", m->dirty);
    dsj_set_str(r, "ui_error", m->ui_error);
    dsj_set_int(r, "objects", m->eng.ok ? (long long)m->eng.object_count(none, 0) : 0);
    dsj_set_int(r, "undo", m->n_undo);
    dsj_set_int(r, "redo", m->n_redo);
    dsj_set(r, "components", comp_names(m));
    dsj_set_str(r, "preview_dir", ds_preview_dir(m));
    dsj_set_int(r, "preview_seq", m->render_seq);
    /* 编辑器视口状态(前端刷新后据此恢复)。
     * 没有视图覆盖时给的是**活动相机**的值 —— 前端的坐标变换靠它,必须与实际画面一致。 */
    {
        Dsj *v = dsj_obj();
        double vx = m->view_x, vy = m->view_y, vz = m->view_zoom;
        if (!m->view_on) effective_view(m, &vx, &vy, &vz);
        dsj_set_bool(v, "on", m->view_on);
        dsj_set_num(v, "x", vx);
        dsj_set_num(v, "y", vy);
        dsj_set_num(v, "zoom", vz);
        dsj_set(r, "view", v);
    }
    dsj_set_bool(r, "recoverable", ds_res_recoverable(m));
    dsj_set_str(r, "recover_kind", ds_res_recover_kind(m));
    dsj_set_int(r, "autosave_seq", m->autosave_seq);
    dsj_set_str(r, "logic_mode", ds_model_logic_mode(m));
    dsj_set_int(r, "view_w", m->eng.ok ? (long long)m->eng.width(none, 0) : 0);
    dsj_set_int(r, "view_h", m->eng.ok ? (long long)m->eng.height(none, 0) : 0);
    return r;
}

static Dsj *cmd_project_new(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "game");
    const char *dir = dsj_get_str(args, "dir", "");
    char *full, *d1, *d2, *d3, *maindex;
    Dsj *r;
    if (!dir || !*dir) {
        seterr(m, "project.new 需要 args.dir(项目目录)");
        return NULL;
    }
    full = path_is_abs(dir) ? xs(dir)
                            : pjoin(m->exe_dir[0] ? m->exe_dir : ".", dir);
    if (!ensure_dir(full)) {
        seterr(m, "无法创建目录:%s", full);
        free(full);
        return NULL;
    }
    d1 = pjoin(full, "scenes");
    d2 = pjoin(full, "scripts");
    d3 = pjoin(full, "res");
    ensure_dir(d1);
    ensure_dir(d2);
    ensure_dir(d3);
    maindex = pjoin(d2, "main.dex");
    if (!path_exists(maindex)) write_text(maindex, MAIN_DEX_TEMPLATE);
    free(d1); free(d2); free(d3); free(maindex);

    snprintf(m->root, sizeof m->root, "%s", full);
    free(full);
    apply_asset_dir(m);
    if (m->project) dsj_free(m->project);
    m->project = project_default(name);
    m->scene[0] = 0;
    if (m->eng.ok) {
        DexValue none[1];
        m->eng.scene_clear(none, 0);
    }
    stacks_clear(m);
    ds_graph_reload(m);
    /* 空逻辑图立刻落盘,并把对应的 scripts/logic.dex 也生成出来 ——
     * 模板 main.dex 里 include "logic",少了它新建的项目直接编译不过。
     * 顺带:逻辑图文件从第一天就存在(撤销快照按路径记它)。 */
    {
        char gerr[256];
        if (!ds_graph_generate_to_file(m, NULL, 0, gerr, sizeof gerr)) {
            seterr(m, "初始化逻辑图失败:%s", gerr);
            return NULL;
        }
    }
    if (!scene_switch(m, NULL, 0)) return NULL;
    ds_blocks_reload(m);
    /* "能跑的最小项目":相机 + 玩家 + 一段已经能动的积木(让用户一运行就看到效果,
     * 而不是对着空场景发呆)。模板的选择在向导里,这里只负责把它建出来。 */
    {
        const char *tpl = dsj_get_str(args, "template", "empty");
        if (tpl && !strcmp(tpl, "starter")) {
            DexValue none[1];
            int64_t cam_id, pl_id;
            char berr2[256];
            cam_id = m->eng.ok ? m->eng.object_new(none, 0) : 0;
            pl_id = m->eng.ok ? m->eng.object_new(none, 0) : 0;
            if (cam_id) {
                DexValue a[2];
                a[0] = V_i(cam_id); a[1] = V_s("相机");
                m->eng.set_name(a, 2);
                a[0] = V_i(cam_id); a[1] = V_s("transform");
                m->eng.attach(a, 2);
                a[0] = V_i(cam_id); a[1] = V_s("camera");
                m->eng.attach(a, 2);
            }
            if (pl_id) {
                DexValue a[4];
                a[0] = V_i(pl_id); a[1] = V_s("玩家");
                m->eng.set_name(a, 2);
                a[0] = V_i(pl_id); a[1] = V_s("transform");
                m->eng.attach(a, 2);
                a[0] = V_i(pl_id); a[1] = V_s("sprite");
                m->eng.attach(a, 2);
                a[0] = V_i(pl_id); a[1] = V_s("collider");
                m->eng.attach(a, 2);
                a[0] = V_i(pl_id); a[1] = V_s("body");
                m->eng.attach(a, 2);
                /* 位置:放在相机看得见的地方 */
                a[0] = V_i(pl_id); a[1] = V_s("transform"); a[2] = V_s("x"); a[3] = V_f(300);
                m->eng.set_f(a, 4);
                a[0] = V_i(pl_id); a[1] = V_s("transform"); a[2] = V_s("y"); a[3] = V_f(200);
                m->eng.set_f(a, 4);
                a[0] = V_i(pl_id); a[1] = V_s("collider"); a[2] = V_s("hw"); a[3] = V_f(16);
                m->eng.set_i(a, 4);
                a[0] = V_i(pl_id); a[1] = V_s("collider"); a[2] = V_s("hh"); a[3] = V_f(16);
                m->eng.set_i(a, 4);
                a[0] = V_i(pl_id); a[1] = V_s("body"); a[2] = V_s("motion"); a[3] = V_f(2);
                m->eng.set_i(a, 4);
            }
            if (!ds_blocks_apply_template(m, "move_jump", berr2, sizeof berr2)) {
                seterr(m, "建示例积木失败:%s", berr2);
                return NULL;
            }
        }
    }
    /* 立刻落盘:新建完的项目必须**马上就能重新打开**(否则"新建"只是个半成品)。
     * 顺带写出空的起始场景。 */
    if (!project_save(m)) return NULL;
    ds_model_recent_push(m, m->root);
    r = dsj_obj();
    dsj_set_str(r, "root", m->root);
    dsj_set_str(r, "scene", m->scene);
    return r;
}

static Dsj *cmd_project_open(DsModel *m, Dsj *args)
{
    const char *dir = dsj_get_str(args, "dir", "");
    char *full, *pj, *txt, perr[256];
    Dsj *dom, *r;
    if (!dir || !*dir) {
        seterr(m, "project.open 需要 args.dir");
        return NULL;
    }
    full = path_is_abs(dir) ? xs(dir)
                            : pjoin(m->exe_dir[0] ? m->exe_dir : ".", dir);
    pj = pjoin(full, "project.json");
    txt = read_text(pj, NULL);
    if (!txt) {
        seterr(m, "不是项目目录(缺 project.json):%s", full);
        free(full); free(pj);
        return NULL;
    }
    dom = dsj_parse(txt, perr, sizeof perr);
    free(txt);
    free(pj);
    if (!dom || dom->t != DSJ_OBJ) {
        seterr(m, "project.json 解析失败:%s", perr);
        free(full);
        if (dom) dsj_free(dom);
        return NULL;
    }
    snprintf(m->root, sizeof m->root, "%s", full);
    free(full);
    apply_asset_dir(m);
    if (m->project) dsj_free(m->project);
    m->project = dom;
    m->scene[0] = 0;
    ds_graph_reload(m);
    ds_blocks_reload(m);
    if (!scene_switch(m, NULL, 1)) return NULL;
    ds_model_recent_push(m, m->root);
    r = dsj_obj();
    dsj_set_str(r, "root", m->root);
    dsj_set_str(r, "scene", m->scene);
    if (m->legacy_crops > 0)
        dsj_set_str(r, "note", "场景里有贴图带着旧模板的 32×32 裁切,"
                               "已经改成画整张贴图(保存后会写回场景文件)");
    return r;
}

/* dsu_list 的回调:把名字推进数组(名字已经是 UTF-8,可以直接进 JSON) */
static void push_name_cb(const char *name, long long size, unsigned long attrs,
                         void *ud)
{
    (void)size;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return;
    dsj_push((Dsj *)ud, dsj_str(name));
}

static Dsj *cmd_scene_list(DsModel *m)
{
    Dsj *a = dsj_arr();
    char *dir, *pat;
    if (!m->root[0]) {
        seterr(m, "还没有打开项目");
        return NULL;
    }
    dir = pjoin(m->root, "scenes");
    pat = pjoin(dir, "*.json");
    dsu_list(pat, push_name_cb, a);
    free(pat);
    free(dir);
    return a;
}

static Dsj *cmd_entity_add(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    Dsj *comps = dsj_get(args, "comps");
    int64_t id;
    Dsj *r;
    Edit e;
    DexValue none[1];
    if (!m->eng.ok) {
        seterr(m, "引擎不可用:%s", m->eng.err);
        return NULL;
    }
    e = edit_begin(m);
    id = m->eng.object_new(none, 0);
    if (!id) {
        seterr(m, "创建实体失败:%s", ds_engine_last_error(&m->eng));
        edit_end(&e, 0);
        return NULL;
    }
    if (name && *name) {
        DexValue a[2];
        a[0] = V_i(id); a[1] = V_s(name);
        if (m->eng.set_name(a, 2) != 0) {
            seterr(m, "设置名字失败:%s", ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return NULL;
        }
    }
    {
        int i, n = dsj_len(comps);
        if (n == 0) {
            DexValue a[2];
            a[0] = V_i(id); a[1] = V_s("transform");
            if (m->eng.attach(a, 2) != 0) {
                seterr(m, "挂载 transform 失败:%s", ds_engine_last_error(&m->eng));
                edit_end(&e, 0);
                return NULL;
            }
        } else {
            for (i = 0; i < n; i++) {
                Dsj *cv = dsj_at(comps, i);
                DexValue a[2];
                if (!cv || cv->t != DSJ_STR) continue;
                a[0] = V_i(id); a[1] = V_s(cv->str);
                if (m->eng.attach(a, 2) != 0) {
                    seterr(m, "挂载组件 '%s' 失败:%s", cv->str,
                           ds_engine_last_error(&m->eng));
                    edit_end(&e, 0);
                    return NULL;
                }
            }
        }
    }
    edit_end(&e, 1);
    r = dsj_obj();
    dsj_set_int(r, "id", (long long)id);
    dsj_set_str(r, "name", obj_name(m, id));
    return r;
}

/* 复制/粘贴与"再做一个":把实体的**全部运行时字段**抄成 JSON,再按它建新实体。
 * 走字段自省而不是场景 JSON 文本,是因为场景 JSON 用的是场景内索引(见陷阱表),
 * 单独抠一个实体出来重编号很容易错。字段级复制对所有组件都成立(含将来新增的)。 */

static int apply_comp_set(DsModel *m, int64_t id, const char *comp, const char *field,
                          Dsj *value);

static Dsj *entity_to_json(DsModel *m, int64_t id)
{
    Dsj *o = dsj_obj();
    dsj_set_str(o, "name", obj_name(m, id));
    dsj_set(o, "comps", entity_comps(m, id));
    return o;
}

/* 按 ej 建一个新实体;dx/dy 只作用于 transform。失败返回 0 并写 err。 */
static int64_t entity_from_json(DsModel *m, Dsj *ej, double dx, double dy)
{
    DexValue none[1];
    DexValue a[2];
    int64_t id = m->eng.object_new(none, 0);
    Dsj *comps;
    int i;
    if (!id) {
        seterr(m, "创建实体失败:%s", ds_engine_last_error(&m->eng));
        return 0;
    }
    {
        const char *nm = dsj_get_str(ej, "name", "");
        if (nm && *nm) {
            a[0] = V_i(id); a[1] = V_s(nm);
            if (m->eng.set_name(a, 2) != 0) {
                seterr(m, "设置名字失败:%s", ds_engine_last_error(&m->eng));
                return 0;
            }
        }
    }
    comps = dsj_get(ej, "comps");
    if (!comps || comps->t != DSJ_OBJ) comps = dsj_obj();
    for (i = 0; i < dsj_len(comps); i++) {
        const char *cname = comps->keys[i];
        Dsj *fields = dsj_at(comps, i);
        int f;
        a[0] = V_i(id); a[1] = V_s(cname);
        if (m->eng.attach(a, 2) != 0) {
            seterr(m, "挂载组件 '%s' 失败:%s", cname, ds_engine_last_error(&m->eng));
            return 0;
        }
        if (!fields || fields->t != DSJ_OBJ) continue;
        for (f = 0; f < dsj_len(fields); f++) {
            const char *fname = fields->keys[f];
            Dsj *v = dsj_at(fields, f);
            if (!strcmp(cname, "transform") && (!strcmp(fname, "x") || !strcmp(fname, "y"))) {
                Dsj nv;
                memset(&nv, 0, sizeof nv);
                nv.t = DSJ_NUM;
                nv.num = (!strcmp(fname, "x") ? dx : dy)
                       + ((v && v->t == DSJ_NUM) ? v->num : 0.0);
                if (!apply_comp_set(m, id, cname, fname, &nv)) return 0;
                continue;
            }
            if (!apply_comp_set(m, id, cname, fname, v)) return 0;
        }
    }
    return id;
}

/* 把一堆实体写进剪贴板(模型持有,跨场景/跨项目有效) */
static Dsj *cmd_entity_copy(DsModel *m, Dsj *args)
{
    Dsj *ids = dsj_get(args, "ids");
    Dsj *arr = dsj_arr();
    int i;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (ids && ids->t == DSJ_ARR) {
        for (i = 0; i < dsj_len(ids); i++) {
            int64_t id = (int64_t)dsj_at(ids, i)->num;
            if (!id) continue;
            dsj_push(arr, entity_to_json(m, id));
        }
    } else {
        int64_t id = dsj_get_int(args, "id", 0);
        if (!id) { seterr(m, "entity.copy 需要 args.ids 或 args.id"); return NULL; }
        dsj_push(arr, entity_to_json(m, id));
    }
    if (m->clipboard) dsj_free(m->clipboard);
    m->clipboard = arr;
    {
        Dsj *r = dsj_obj();
        dsj_set_int(r, "count", dsj_len(arr));
        return r;
    }
}

static Dsj *cmd_entity_paste(DsModel *m, Dsj *args)
{
    double dx = dsj_get_num(args, "dx", 8.0);
    double dy = dsj_get_num(args, "dy", 8.0);
    Dsj *made = dsj_arr();
    Dsj *r;
    Edit e;
    int i;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!m->clipboard || dsj_len(m->clipboard) == 0) {
        seterr(m, "剪贴板是空的(先 Ctrl+C 复制实体)");
        return NULL;
    }
    e = edit_begin(m);
    for (i = 0; i < dsj_len(m->clipboard); i++) {
        int64_t nid = entity_from_json(m, dsj_at(m->clipboard, i), dx, dy);
        if (!nid) { edit_end(&e, 0); dsj_free(made); return NULL; }
        dsj_push(made, dsj_int((long long)nid));
    }
    edit_end(&e, 1);
    r = dsj_obj();
    dsj_set(r, "ids", made);
    return r;
}

static Dsj *cmd_entity_duplicate(DsModel *m, Dsj *args)
{
    Dsj *ids = dsj_get(args, "ids");
    double dx = dsj_get_num(args, "dx", 8.0);
    double dy = dsj_get_num(args, "dy", 8.0);
    Dsj *made = dsj_arr();
    Dsj *r;
    Edit e;
    int i, n, single = 0;
    int64_t single_id = dsj_get_int(args, "id", 0);
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (ids && ids->t == DSJ_ARR && dsj_len(ids) > 0) n = dsj_len(ids);
    else if (single_id) { single = 1; n = 1; }
    else {
        seterr(m, "entity.duplicate 需要 args.id 或 args.ids");
        dsj_free(made);
        return NULL;
    }
    /* 先把要复制的实体全抄下来,再建新的 —— 建的过程中会 push 到实体池,
     * 边遍历边建会漏/重(索引在变)。 */
    {
        Dsj *snap = dsj_arr();
        e = edit_begin(m);
        for (i = 0; i < n; i++) {
            int64_t id = single_id;
            if (!single) {
                Dsj *it = dsj_at(ids, i);
                id = (it && it->t == DSJ_NUM) ? (int64_t)it->num : 0;
            }
            if (!id) continue;
            dsj_push(snap, entity_to_json(m, id));
        }
        for (i = 0; i < dsj_len(snap); i++) {
            int64_t nid = entity_from_json(m, dsj_at(snap, i), dx, dy);
            if (!nid) {
                edit_end(&e, 0);
                dsj_free(snap); dsj_free(made);
                return NULL;
            }
            dsj_push(made, dsj_int((long long)nid));
        }
        dsj_free(snap);
    }
    edit_end(&e, 1);
    r = dsj_obj();
    dsj_set(r, "ids", made);
    return r;
}

/* 设/断父子关系。为什么要有专门命令:parent 字段是个 int,以前只能手填实体 id ——
 * 而 id 在撤销后会变,填错了就是"位置莫名其妙"。这里做完整校验(存在/自环/成环),
 * 前端只需要给一个实体下拉。parent = -1(或省略)表示断开。 */
static Dsj *cmd_entity_set_parent(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    int64_t parent = dsj_get_int(args, "parent", -1);
    Edit e;
    Dsj *r;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id) { seterr(m, "entity.set_parent 需要 args.id"); return NULL; }
    {
        DexValue a[1];
        a[0] = V_i(id);
        if (m->eng.object_alive(a, 1) != 1) {
            seterr(m, "实体 %lld 不存在", (long long)id);
            return NULL;
        }
    }
    if (parent == id) { seterr(m, "不能让实体做自己的父级"); return NULL; }
    if (parent >= 0) {
        /* 父级必须存在,而且不能是自己的子孙(否则世界坐标会绕圈) */
        int64_t cur = parent;
        int guard = 0;
        DexValue a[1];
        a[0] = V_i(parent);
        if (m->eng.object_alive(a, 1) != 1) {
            seterr(m, "父实体 %lld 不存在", (long long)parent);
            return NULL;
        }
        while (cur >= 0 && guard++ < 64) {
            DexValue b[4];
            if (cur == id) { seterr(m, "这样会成环(父级是它自己的子孙)"); return NULL; }
            b[0] = V_i(cur); b[1] = V_s("transform"); b[2] = V_s("parent");
            cur = (int64_t)m->eng.get_i(b, 3);
        }
    }
    e = edit_begin(m);
    {
        DexValue a[4];
        a[0] = V_i(id); a[1] = V_s("transform"); a[2] = V_s("parent"); a[3] = V_i(parent);
        if (m->eng.set_i(a, 4) != 0) {
            seterr(m, "设置父级失败:%s", ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return NULL;
        }
    }
    edit_end(&e, 1);
    r = dsj_obj();
    dsj_set_int(r, "id", (long long)id);
    dsj_set_int(r, "parent", (long long)parent);
    return r;
}

static Dsj *cmd_entity_remove(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    Edit e;
    if (!m->eng.ok) {
        seterr(m, "引擎不可用:%s", m->eng.err);
        return NULL;
    }
    if (!id) {
        seterr(m, "entity.remove 需要 args.id");
        return NULL;
    }
    e = edit_begin(m);
    {
        DexValue a[1];
        a[0] = V_i(id);
        if (m->eng.object_free(a, 1) != 0) {
            seterr(m, "删除实体 %lld 失败:%s", (long long)id,
                   ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return NULL;
        }
    }
    /* 子实体的 parent 悬空了(引擎只在算世界坐标时兜底)→ 顺手断干净:
     * 否则用户删掉父级之后,子实体的位置看起来"没变",其实已经掉到根上了。 */
    {
        int n = (int)m->eng.object_count(NULL, 0), i, detached = 0;
        for (i = 0; i < n; i++) {
            DexValue b[4];
            int64_t cid;
            b[0] = V_i(i);
            cid = m->eng.object_id_at(b, 1);
            if (!cid) continue;
            b[0] = V_i(cid); b[1] = V_s("transform"); b[2] = V_s("parent");
            if ((int64_t)m->eng.get_i(b, 3) != id) continue;
            {
                DexValue s[4];
                s[0] = V_i(cid); s[1] = V_s("transform"); s[2] = V_s("parent");
                s[3] = V_i(-1);
                if (m->eng.set_i(s, 4) == 0) detached++;
            }
        }
        (void)detached;
    }
    edit_end(&e, 1);
    {
        Dsj *r = dsj_obj();
        dsj_set_bool(r, "removed", 1);
        return r;
    }
}

/* 逻辑图按**实体名**引用实体(生成的代码是 eng_find("player")):所以改名之后
 * 图里的引用必须跟着改,否则用户一改名逻辑就静默失效(校验只会报"场景里没有
 * 叫 xxx 的实体",而用户不明白为什么)。 */
static int graph_rename_refs(Dsj *v, const char *from, const char *to)
{
    int n = 0, i;
    if (!v) return 0;
    if (v->t == DSJ_STR) {
        if (v->str && !strcmp(v->str, from)) {
            free(v->str);
            v->str = ds_strdup(to);
            n = 1;
        }
        return n;
    }
    if (v->t == DSJ_OBJ || v->t == DSJ_ARR) {
        for (i = 0; i < v->n; i++) n += graph_rename_refs(v->items[i], from, to);
    }
    return n;
}

static Dsj *cmd_entity_rename(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *name = dsj_get_str(args, "name", "");
    char old[256];
    int refs = 0;
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id || !name || !*name) { seterr(m, "entity.rename 需要 args.id 与 args.name"); return NULL; }
    snprintf(old, sizeof old, "%s", obj_name(m, id));
    e = edit_begin(m);
    {
        DexValue a[2];
        a[0] = V_i(id); a[1] = V_s(name);
        if (m->eng.set_name(a, 2) != 0) {
            seterr(m, "改名失败:%s", ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return NULL;
        }
    }
    if (old[0] && strcmp(old, name)) refs = graph_rename_refs(ds_model_graph(m), old, name);
    edit_end(&e, 1);
    {
        Dsj *r = dsj_obj();
        dsj_set_str(r, "name", obj_name(m, id));
        dsj_set_int(r, "refs", refs);
        return r;
    }
}

static Dsj *cmd_entity_set_pos(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    double x = dsj_get_num(args, "x", 0), y = dsj_get_num(args, "y", 0);
    int has_x = dsj_get(args, "x") != NULL, has_y = dsj_get(args, "y") != NULL;
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id) { seterr(m, "entity.set_pos 需要 args.id"); return NULL; }
    e = edit_begin(m);
    {
        DexValue a[4];
        if (has_x) {
            a[0] = V_i(id); a[1] = V_s("transform"); a[2] = V_s("x"); a[3] = V_f(x);
            if (m->eng.set_f(a, 4) != 0) {
                seterr(m, "设置 x 失败:%s", ds_engine_last_error(&m->eng));
                edit_end(&e, 0);
                return NULL;
            }
        }
        if (has_y) {
            a[0] = V_i(id); a[1] = V_s("transform"); a[2] = V_s("y"); a[3] = V_f(y);
            if (m->eng.set_f(a, 4) != 0) {
                seterr(m, "设置 y 失败:%s", ds_engine_last_error(&m->eng));
                edit_end(&e, 0);
                return NULL;
            }
        }
    }
    edit_end(&e, has_x || has_y);
    {
        Dsj *r = dsj_obj();
        dsj_set_num(r, "x", obj_world(m, id, m->eng.world_x));
        dsj_set_num(r, "y", obj_world(m, id, m->eng.world_y));
        return r;
    }
}

static Dsj *cmd_comp_add(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *comp = dsj_get_str(args, "comp", "");
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id || !comp || !*comp) { seterr(m, "comp.add 需要 args.id 与 args.comp"); return NULL; }
    e = edit_begin(m);
    {
        DexValue a[2];
        int64_t rc;
        a[0] = V_i(id); a[1] = V_s(comp);
        rc = m->eng.attach(a, 2);
        if (rc != 0) {
            seterr(m, "挂载组件 '%s' 失败:%s", comp, ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return NULL;
        }
    }
    edit_end(&e, 1);
    {
        Dsj *r = dsj_obj();
        Dsj *all = entity_comps(m, id);
        Dsj *c = all ? dsj_get(all, comp) : NULL;
        dsj_set_str(r, "comp", comp);
        if (c) dsj_set(r, "fields", dsj_clone(c));   /* 必须克隆:见 dsj_clone 注释 */
        dsj_free(all);
        return r;
    }
}

static Dsj *cmd_comp_remove(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *comp = dsj_get_str(args, "comp", "");
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id || !comp || !*comp) { seterr(m, "comp.remove 需要 args.id 与 args.comp"); return NULL; }
    e = edit_begin(m);
    {
        DexValue a[2];
        a[0] = V_i(id); a[1] = V_s(comp);
        if (m->eng.detach(a, 2) != 0) {
            seterr(m, "卸载组件 '%s' 失败:%s", comp, ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return NULL;
        }
    }
    edit_end(&e, 1);
    {
        Dsj *r = dsj_obj();
        dsj_set_bool(r, "removed", 1);
        return r;
    }
}

/* 组件字段写入:按字段类型分派。value 从 JSON 取(数字/布尔/字符串)。 */
/* 找字段的类型码(1=float 2=bool/int 3=string);找不到返回 -1 */
static int field_type_of(DsModel *m, const char *comp, const char *field)
{
    int nf, f;
    DexValue a[2];
    if (!comp || !*comp || !field || !*field) return -1;
    a[0] = V_s(comp); a[1] = V_i(0);
    nf = (int)m->eng.field_count(a, 2);
    for (f = 0; f < nf; f++) {
        const char *fn;
        a[0] = V_s(comp); a[1] = V_i(f);
        fn = m->eng.field_name(a, 2);
        if (fn && !strcmp(fn, field)) return (int)m->eng.field_type(a, 2);
    }
    return -1;
}

/* 真正落地一次字段写入(不含撤销包装)。成功返回 1,失败写 err 并返回 0。 */
static int apply_comp_set(DsModel *m, int64_t id, const char *comp, const char *field,
                          Dsj *value)
{
    int ft = field_type_of(m, comp, field);
    DexValue a[4];
    int64_t rc;
    if (ft < 0) {
        seterr(m, "组件 '%s' 没有字段 '%s'", comp, field);
        return 0;
    }
    if (!value) { seterr(m, "缺少 value"); return 0; }
    a[0] = V_i(id); a[1] = V_s(comp); a[2] = V_s(field);
    if (ft == 1) {
        a[3] = V_f(value->t == DSJ_NUM ? value->num : 0.0);
        rc = m->eng.set_f(a, 4);
    } else if (ft == 3) {
        a[3] = V_s(value->t == DSJ_STR ? value->str : "");
        rc = m->eng.set_s(a, 4);
    } else {
        int64_t iv = value->t == DSJ_NUM ? (int64_t)value->num
                   : value->t == DSJ_BOOL ? value->b : 0;
        a[3] = V_i(iv);
        rc = m->eng.set_i(a, 4);
    }
    if (rc != 0) {
        seterr(m, "设置 '%s.%s' 失败:%s", comp, field, ds_engine_last_error(&m->eng));
        return 0;
    }
    return 1;
}

/* 给 sprite 换了贴图之后,顺手把"旧模板/引擎默认留下的 32×32 裁切"改成整张图。
 *
 * 为什么必须做:引擎的 sprite 默认 sw=sh=32(当年的瓦片尺寸),而 sw/sh 是
 * **从贴图里截取多大**(1:1,不是缩放)。于是用户把一张 300×400 的角色图设上去,
 * 屏幕上只出现左上角 32×32 的一小块 —— 表现就是"贴图设了,可图像没显示出来"
 * (用户实测报的正是这个)。sw=0 是引擎的"整张贴图"约定。
 *
 * 只动**恰好是 32×32** 的那种情况(旧默认值);用户自己裁过的大小一概不碰。
 * 返回 1 = 改过(调用方可以把这句话说给用户听)。 */
static int fix_legacy_crop(DsModel *m, int64_t id)
{
    DexValue a[4];
    double sw, sh, tw, th;
    int64_t tex;
    if (!m->eng.ok) return 0;
    a[0] = V_i(id); a[1] = V_s("sprite"); a[2] = V_s("sw");
    sw = m->eng.get_f(a, 3);
    a[2] = V_s("sh");
    sh = m->eng.get_f(a, 3);
    if (sw != 32.0 || sh != 32.0) return 0;
    a[2] = V_s("texture");
    tex = m->eng.get_i(a, 3);
    if (tex < 0) return 0;
    a[0] = V_i(tex);
    tw = m->eng.tex_width(a, 1);
    th = m->eng.tex_height(a, 1);
    if (tw <= 0 || th <= 0) return 0;
    if (tw == 32.0 && th == 32.0) return 0;      /* 图本身就是 32×32:裁切=整张 */
    a[0] = V_i(id); a[1] = V_s("sprite"); a[2] = V_s("sw"); a[3] = V_f(0.0);
    if (m->eng.set_f(a, 4) != 0) return 0;
    a[2] = V_s("sh");
    if (m->eng.set_f(a, 4) != 0) return 0;
    return 1;
}

/* 场景里所有 sprite 都过一遍(见 fix_legacy_crop);返回改了几个。 */
static int fix_legacy_crops(DsModel *m)
{
    DexValue none[1];
    int n, i, fixed = 0;
    if (!m->eng.ok) return 0;
    n = (int)m->eng.object_count(none, 0);
    for (i = 0; i < n; i++) {
        DexValue a[2];
        int64_t id;
        a[0] = V_i(i);
        id = m->eng.object_id_at(a, 1);
        if (!id) continue;
        a[0] = V_i(id);
        a[1] = V_s("sprite");
        if (m->eng.has(a, 2) != 1) continue;
        if (fix_legacy_crop(m, id)) fixed++;
    }
    return fixed;
}

static Dsj *cmd_comp_set(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *comp = dsj_get_str(args, "comp", "");
    const char *field = dsj_get_str(args, "field", "");
    Dsj *value = dsj_get(args, "value");
    int fixed_crop = 0;
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id || !comp || !*comp || !field || !*field) {
        seterr(m, "comp.set 需要 args.id / args.comp / args.field");
        return NULL;
    }
    if (!value) { seterr(m, "comp.set 需要 args.value"); return NULL; }
    e = edit_begin(m);
    if (!apply_comp_set(m, id, comp, field, value)) {
        edit_end(&e, 0);
        return NULL;
    }
    if (!strcmp(comp, "sprite") && !strcmp(field, "tex_path")
        && value->t == DSJ_STR && value->str && value->str[0])
        fixed_crop = fix_legacy_crop(m, id);
    edit_end(&e, 1);
    {
        Dsj *r = dsj_obj();
        Dsj *all = entity_comps(m, id);
        Dsj *c = dsj_get(all, comp);
        dsj_set_str(r, "comp", comp);
        dsj_set_str(r, "field", field);
        if (c) {
            Dsj *v = dsj_get(c, field);
            dsj_set(r, "value", v ? dsj_clone(v) : dsj_null());
        }
        dsj_free(all);
        if (fixed_crop)
            dsj_set_str(r, "note", "贴图换好了,已改成画整张贴图"
                                   "(以前是旧模板留下的 32×32 裁切,所以只看得到一角)");
        return r;
    }
}

/* 批量写字段,**只产生一条撤销记录**。为什么需要它:拖着一组实体移动如果按
 * 每个实体一条 comp.set,撤销要按十几次才退回去 —— 用户视角是"撤销坏了"。 */
static Dsj *cmd_comp_set_many(DsModel *m, Dsj *args)
{
    Dsj *items = dsj_get(args, "items");
    Edit e;
    int i, n;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!items || items->t != DSJ_ARR || dsj_len(items) == 0) {
        seterr(m, "comp.set_many 需要 args.items 数组");
        return NULL;
    }
    n = dsj_len(items);
    e = edit_begin(m);
    for (i = 0; i < n; i++) {
        Dsj *it = dsj_at(items, i);
        if (!it || it->t != DSJ_OBJ) {
            seterr(m, "items[%d] 不是对象", i);
            edit_end(&e, 0);
            return NULL;
        }
        if (!apply_comp_set(m, dsj_get_int(it, "id", 0), dsj_get_str(it, "comp", ""),
                            dsj_get_str(it, "field", ""), dsj_get(it, "value"))) {
            edit_end(&e, 0);
            return NULL;
        }
    }
    edit_end(&e, 1);
    {
        Dsj *r = dsj_obj();
        dsj_set_int(r, "count", n);
        return r;
    }
}

static Dsj *c_undo(DsModel *m, Dsj *args)
{
    Dsj *r;
    (void)args;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!undo_apply(m, 0)) return NULL;
    r = dsj_obj();
    dsj_set_int(r, "undo", m->n_undo);
    dsj_set_int(r, "redo", m->n_redo);
    return r;
}

static Dsj *c_redo(DsModel *m, Dsj *args)
{
    Dsj *r;
    (void)args;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!undo_apply(m, 1)) return NULL;
    r = dsj_obj();
    dsj_set_int(r, "undo", m->n_undo);
    dsj_set_int(r, "redo", m->n_redo);
    return r;
}

/* 场景名必须是**纯名字**:别让前端顺手把 "../../x" 传进来(会写到项目外面)。
 * 与 ds_res.c 的资源名规则一致;顺便挡掉 Windows 文件名非法字符。 */
static int scene_name_ok(const char *name)
{
    size_t i;
    if (!name || !*name) return 0;
    if (strstr(name, "..")) return 0;
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) return 0;
    for (i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return 0;
    }
    return 1;
}

static Dsj *cmd_scene_new(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "scene");
    char rel[512];
    DexValue none[1];
    Dsj *r;
    if (!m->root[0]) { seterr(m, "还没有打开项目"); return NULL; }
    if (!scene_name_ok(name)) {
        seterr(m, "场景名不合法:'%s'(只能是名字,不能带 / \\ : 或 ..)",
               name ? name : "");
        return NULL;
    }
    snprintf(rel, sizeof rel, "scenes/%s.json", name);
    if (m->eng.ok) m->eng.scene_clear(none, 0);
    if (!scene_switch(m, rel, 0)) return NULL;
    /* 立刻写出空场景:这样"场景列表"里就有它,项目始终是可重新打开的完整状态 */
    if (!scene_save_to(m, m->scene)) return NULL;
    r = dsj_obj();
    dsj_set_str(r, "scene", m->scene);
    return r;
}

static Dsj *cmd_scene_load(DsModel *m, Dsj *args)
{
    const char *path = dsj_get_str(args, "path", "");
    Dsj *r;
    if (!m->root[0]) { seterr(m, "还没有打开项目"); return NULL; }
    if (!path || !*path) { seterr(m, "scene.load 需要 args.path"); return NULL; }
    if (!scene_switch(m, path, 1)) return NULL;
    r = dsj_obj();
    dsj_set_str(r, "scene", m->scene);
    if (m->legacy_crops > 0)
        dsj_set_str(r, "note", "这个场景里有贴图带着旧模板的 32×32 裁切,"
                               "已经改成画整张贴图(保存后会写回场景文件)");
    return r;
}

static Dsj *cmd_scene_save(DsModel *m, Dsj *args)
{
    const char *path = dsj_get_str(args, "path", "");
    char *full;
    Dsj *r;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (path && *path) full = path_is_abs(path) ? xs(path) : pjoin(m->root, path);
    else if (m->scene[0]) full = xs(m->scene);
    else { seterr(m, "还没有指定场景文件"); return NULL; }
    if (!scene_save_to(m, full)) { free(full); return NULL; }
    free(full);
    r = dsj_obj();
    dsj_set_str(r, "scene", m->scene);
    dsj_set_bool(r, "saved", 1);
    return r;
}

/* ------------------------------------------------------------ 场景管理(改名/删除/起始场景)
 * 以前只有 scene.new / scene.load:用户没法把"第一关"改个名字,也没法删掉建错的场景。 */

/* 场景名(可带 scenes/ 前缀、可带 .json)→ **项目相对**的正斜杠路径 */
static char *scene_rel_norm(const char *name)
{
    char buf[512];
    char *out;
    size_t i;
    if (!name || !*name) return NULL;
    if (strchr(name, '/') || strchr(name, '\\')) snprintf(buf, sizeof buf, "%s", name);
    else snprintf(buf, sizeof buf, "scenes/%s", name);
    {
        size_t n = strlen(buf);
        if (n < 5 || strcmp(buf + n - 5, ".json") != 0) {
            if (n + 5 >= sizeof buf) return NULL;
            strcat(buf, ".json");
        }
    }
    out = ds_strdup(buf);
    for (i = 0; out[i]; i++) if (out[i] == '\\') out[i] = '/';
    return out;
}

/* 项目里所有场景(**项目相对**路径,正斜杠,带 scenes/ 前缀);返回 malloc 的字符串数组。
 * 注意一定要带前缀:调用方会拿它直接 scene_switch(),而裸文件名会被当成
 * "项目根下的 main.json"(实测踩到:删掉当前场景后回退到了一个不存在的路径,
 * 起始场景也跟着变成了 main.json)。 */
static char **scene_list_rel(DsModel *m, int *out_n)
{
    Dsj *a = cmd_scene_list(m);
    char **out;
    int i, n = a ? dsj_len(a) : 0;
    *out_n = 0;
    if (n <= 0) { if (a) dsj_free(a); return NULL; }
    out = xm(sizeof(char *) * (size_t)n);
    for (i = 0; i < n; i++) {
        Dsj *it = dsj_at(a, i);
        char buf[600];
        snprintf(buf, sizeof buf, "scenes/%s", (it && it->str) ? it->str : "");
        out[i] = ds_strdup(buf);
    }
    dsj_free(a);
    *out_n = n;
    return out;
}

static Dsj *cmd_scene_rename(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    const char *to = dsj_get_str(args, "to", "");
    char *old_rel, *new_rel, *old_full, *new_full;
    Dsj *r;
    int i, n = 0;
    char **list;
    if (!m->root[0]) { seterr(m, "还没有打开项目"); return NULL; }
    if (!scene_name_ok(to)) {
        seterr(m, "新场景名不合法:'%s'", to ? to : "");
        return NULL;
    }
    old_rel = scene_rel_norm(name);
    new_rel = scene_rel_norm(to);
    if (!old_rel || !new_rel) { free(old_rel); free(new_rel); seterr(m, "场景名不合法"); return NULL; }
    old_full = pjoin(m->root, old_rel);
    new_full = pjoin(m->root, new_rel);
    if (!path_exists(old_full)) {
        seterr(m, "没有这个场景:%s", old_rel);
        free(old_rel); free(new_rel); free(old_full); free(new_full);
        return NULL;
    }
    if (path_exists(new_full)) {
        seterr(m, "已经有 '%s' 了", new_rel);
        free(old_rel); free(new_rel); free(old_full); free(new_full);
        return NULL;
    }
    if (!dsu_move_file(old_full, new_full)) {
        seterr(m, "改名失败(%lu):%s", (unsigned long)GetLastError(), old_rel);
        free(old_rel); free(new_rel); free(old_full); free(new_full);
        return NULL;
    }
    /* 当前场景 / 起始场景都要跟着改 —— 否则"改了名就回不到这个场景" */
    if (m->scene[0] && !strcmp(m->scene, old_full))
        snprintf(m->scene, sizeof m->scene, "%s", new_full);
    if (m->project) {
        const char *st = dsj_get_str(m->project, "start_scene", "");
        if (st && !strcmp(st, old_rel)) dsj_set_str(m->project, "start_scene", new_rel);
    }
    for (i = 0; i < m->n_undo; i++) { /* 撤销快照里记的是文件路径,一并换掉 */
        Dsj *f = m->undo[i].files;
        int k;
        for (k = 0; f && k < dsj_len(f); k++) {
            if (f->keys[k] && !strcmp(f->keys[k], old_full)) {
                char *txt = f->items[k] ? f->items[k]->str : NULL;
                dsj_set_str(f, new_full, txt ? txt : "");
            }
        }
    }
    list = scene_list_rel(m, &n);
    for (i = 0; i < n; i++) free(list[i]);
    free(list);
    ds_write_project_info(m);
    r = dsj_obj();
    dsj_set_str(r, "scene", new_rel);
    dsj_set_str(r, "path", new_full);
    free(old_rel); free(new_rel); free(old_full); free(new_full);
    return r;
}

static Dsj *cmd_scene_delete(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    char *rel, *full;
    Dsj *r;
    int i, n = 0;
    char **list;
    if (!m->root[0]) { seterr(m, "还没有打开项目"); return NULL; }
    rel = scene_rel_norm(name);
    if (!rel) { seterr(m, "场景名不合法"); return NULL; }
    full = pjoin(m->root, rel);
    if (!path_exists(full)) {
        seterr(m, "没有这个场景:%s", rel);
        free(rel); free(full);
        return NULL;
    }
    if (!dsu_remove(full)) {
        seterr(m, "删除失败(%lu):%s", (unsigned long)GetLastError(), rel);
        free(rel); free(full);
        return NULL;
    }
    /* 删掉的正好是当前场景 → 切到别的场景(没有别的就清空,保持可编辑) */
    if (m->scene[0] && !strcmp(m->scene, full)) {
        char *next = NULL;
        list = scene_list_rel(m, &n);
        for (i = 0; i < n; i++) {
            if (strcmp(list[i], rel) != 0) { next = ds_strdup(list[i]); break; }
        }
        for (i = 0; i < n; i++) free(list[i]);
        free(list);
        m->scene[0] = 0;
        if (next) {
            scene_switch(m, next, 1);
            free(next);
        } else {
            DexValue none[1];
            if (m->eng.ok) m->eng.scene_clear(none, 0);
            if (m->project) dsj_set_str(m->project, "start_scene", "");
            stacks_clear(m);
        }
    } else if (m->project) {
        const char *st = dsj_get_str(m->project, "start_scene", "");
        if (st && !strcmp(st, rel)) {
            list = scene_list_rel(m, &n);
            dsj_set_str(m->project, "start_scene", n > 0 ? list[0] : "");
            for (i = 0; i < n; i++) free(list[i]);
            free(list);
        }
    }
    ds_write_project_info(m);
    r = dsj_obj();
    dsj_set_bool(r, "deleted", 1);
    dsj_set_str(r, "scene", m->scene);
    free(rel); free(full);
    return r;
}

/* "把当前/指定场景设成游戏启动时加载的那个" */
static Dsj *cmd_scene_set_start(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    char *rel;
    Dsj *r;
    if (!m->root[0]) { seterr(m, "还没有打开项目"); return NULL; }
    if (name && *name) {
        char *full;
        rel = scene_rel_norm(name);
        if (!rel) { seterr(m, "场景名不合法"); return NULL; }
        full = pjoin(m->root, rel);
        if (!path_exists(full)) {
            seterr(m, "没有这个场景:%s", rel);
            free(rel); free(full);
            return NULL;
        }
        free(full);
        dsj_set_str(m->project, "start_scene", rel);
    } else {
        rel = ds_strdup(dsj_get_str(m->project, "start_scene", ""));
    }
    ds_write_project_info(m);
    m->dirty = 1;
    r = dsj_obj();
    dsj_set_str(r, "start_scene", rel ? rel : "");
    free(rel);
    return r;
}

/* ------------------------------------------------------------ 生成文件:起始场景 + 窗口
 *
 * 游戏启动加载哪个场景,以前是**写死在 main.dex 模板里**的 `eng_scene_load("scenes/main.json")`
 * —— 用户在 IDE 里切到"第一关",运行起来还是 main.json(实测就是这个症状)。
 * 现在:project.json 的 start_scene 是唯一真相,这里把它生成成 DexLang 形式
 * (`scripts/project_info.dex`),模板的 on_start 调 `dexstudio_start_scene()`。
 * 老项目(模板里还是字面量)由 ds_patch_main_scene() 兜底改写那一行。
 * 窗口标题同样来自 project.json(name),让用户一眼看出"这是我那个游戏"。 */
static int write_all(const char *path, const char *text) { return write_text(path, text); }

/* DexLang 字符串字面量转义(两个生成函数共用) */
static void ds_escape_dex(const char *in, char *out, size_t cap)
{
    size_t i, o = 0;
    if (!in) in = "";
    for (i = 0; in[i] && o + 2 < cap; i++) {
        if (in[i] == '"' || in[i] == '\\') out[o++] = '\\';
        out[o++] = in[i];
    }
    out[o] = 0;
}

int ds_write_project_info(DsModel *m)
{
    char *path, *src;
    const char *start, *name;
    char esc[2048], esc_name[512];
    size_t o = 0, need;
    if (!m || !m->root[0]) return 0;
    start = (m->project ? dsj_get_str(m->project, "start_scene", "") : "");
    if (!start || !*start) start = "scenes/main.json";
    name = (m->project ? dsj_get_str(m->project, "name", "") : "");
    if (!name || !*name) name = "我的游戏";
    ds_escape_dex(start, esc, sizeof esc);
    ds_escape_dex(name, esc_name, sizeof esc_name);
    need = strlen(esc) + strlen(esc_name) + 700;
    src = xm(need);
    snprintf(src, need,
             "# 由 DexStudio 生成 —— 不要手改(改 IDE 里的「起始场景」/项目名即可)\n"
             "# 游戏启动时加载哪个场景由 project.json 的 start_scene 决定,窗口标题用项目名,\n"
             "# 这个文件只是它们的 DexLang 形式;main.dex include 它并调用下面的函数。\n"
             "func dexstudio_start_scene() -> string {\n"
             "    return \"%s\";\n"
             "}\n"
             "func dexstudio_game_title() -> string {\n"
             "    return \"%s\";\n"
             "}\n", esc, esc_name);
    path = pjoin(m->root, DS_PROJECT_INFO_REL);
    o = (size_t)write_all(path, src);
    free(path);
    free(src);
    return (int)o;
}

/* 老项目兜底:main.dex 里若还是 `eng_scene_load("字面量")`,把那个字面量改成
 * 当前的起始场景。返回 1 = 改了,0 = 不需要改(已经是函数调用/找不到),-1 = 写失败。 */
int ds_patch_main_scene(DsModel *m)
{
    char *path, *txt, *pat, *close, *second, *out;
    const char *start;
    size_t head, tail, need;
    int rc = 0;
    if (!m || !m->root[0]) return 0;
    path = pjoin(m->root, "scripts/main.dex");
    txt = read_text(path, NULL);
    if (!txt) { free(path); return 0; }
    if (strstr(txt, "dexstudio_start_scene")) { free(txt); free(path); return 0; }
    start = m->project ? dsj_get_str(m->project, "start_scene", "") : "";
    if (!start || !*start) { free(txt); free(path); return 0; }
    pat = strstr(txt, "eng_scene_load(\"");
    if (!pat) { free(txt); free(path); return 0; }
    close = strstr(pat + 16, "\")");
    if (!close) { free(txt); free(path); return 0; }
    second = strstr(close + 2, "eng_scene_load(\"");
    if (second) { free(txt); free(path); return 0; }   /* 多处出现:不猜 */
    head = (size_t)(pat + 16 - txt);
    tail = (size_t)(close - txt);
    need = strlen(txt) + strlen(start) + 8;
    out = xm(need);
    snprintf(out, need, "%.*s%s%s", (int)head, txt, start, txt + tail);
    rc = write_text(path, out) ? 1 : -1;
    free(out);
    free(txt);
    free(path);
    return rc;
}

/* ------------------------------------------------------------ 逻辑图的下拉候选
 * 逻辑图里 obj/comp/field/path 全是手打的字符串 —— 打错一个字就静默无效。
 * 这个命令把"程序知道的那些集合"一次性交给前端(与节点目录同一套路:
 * 选项由 C 给,前端只负责画下拉)。 */
static int ext_is_image(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    return !strcmp(d, ".png") || !strcmp(d, ".jpg") || !strcmp(d, ".jpeg")
        || !strcmp(d, ".bmp") || !strcmp(d, ".gif");
}
static int ext_is_audio(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    return !strcmp(d, ".wav") || !strcmp(d, ".mp3") || !strcmp(d, ".ogg");
}

static void opt_res_cb(const char *name, long long size, unsigned long attrs, void *ud)
{
    Dsj **lists = (Dsj **)ud;
    char rel[1024];
    (void)size;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return;
    if (name[0] == '.' && name[1] == 0) return;
    snprintf(rel, sizeof rel, "res/%s", name);
    if (ext_is_image(name)) dsj_push(lists[0], dsj_str(rel));
    else if (ext_is_audio(name)) dsj_push(lists[1], dsj_str(rel));
}

Dsj *ds_model_scene_options(DsModel *m)
{
    Dsj *o = dsj_obj();
    Dsj *ents = dsj_arr();
    int n, i;
    if (m->eng.ok) {
        n = (int)m->eng.object_count(NULL, 0);
        for (i = 0; i < n; i++) {
            DexValue a[3];
            int64_t id;
            Dsj *e = dsj_obj();
            Dsj *comps = dsj_obj();
            int c, nc = comp_total(m);
            a[0] = V_i(i);
            id = m->eng.object_id_at(a, 1);
            if (!id) { dsj_free(e); continue; }
            a[0] = V_i(id);
            dsj_set_int(e, "id", (long long)id);
            dsj_set_str(e, "name", m->eng.name(a, 1));
            for (c = 1; c <= nc; c++) {
                DexValue b[3];
                const char *cn;
                int nf, f;
                Dsj *fields = dsj_obj();
                b[0] = V_i(c);
                cn = m->eng.comp_name_at(b, 1);
                if (!cn || !*cn) continue;
                b[0] = V_i(id); b[1] = V_s(cn);
                if (m->eng.has(b, 2) != 1) continue;
                b[0] = V_s(cn); b[1] = V_i(0);
                nf = (int)m->eng.field_count(b, 2);
                for (f = 0; f < nf; f++) {
                    const char *fn;
                    b[0] = V_s(cn); b[1] = V_i(f);
                    fn = m->eng.field_name(b, 2);
                    if (!fn || !*fn) continue;
                    dsj_set_str(fields, fn, field_type_name_of((int)m->eng.field_type(b, 2)));
                }
                dsj_set(comps, cn, fields);
            }
            dsj_set(e, "comps", comps);
            dsj_push(ents, e);
        }
    }
    dsj_set(o, "entities", ents);
    {
        Dsj *lists[2];
        lists[0] = dsj_arr();
        lists[1] = dsj_arr();
        if (m->root[0]) {
            char *dir = pjoin(m->root, "res");
            char *pat = pjoin(dir, "*");
            dsu_list(pat, opt_res_cb, lists);
            free(pat);
            free(dir);
        }
        dsj_set(o, "images", lists[0]);
        dsj_set(o, "audios", lists[1]);
    }
    /* 组件→字段的完整清单(前端做"选组件 → 字段联动"用) */
    {
        Dsj *a = dsj_arr();
        int c, nc = comp_total(m);
        for (c = 1; c <= nc; c++) {
            DexValue b[2];
            const char *cn;
            int nf, f;
            Dsj *co = dsj_obj();
            Dsj *fields = dsj_arr();
            b[0] = V_i(c);
            cn = m->eng.comp_name_at(b, 1);
            if (!cn || !*cn) continue;
            b[0] = V_s(cn); b[1] = V_i(0);
            nf = (int)m->eng.field_count(b, 2);
            for (f = 0; f < nf; f++) {
                const char *fn;
                b[0] = V_s(cn); b[1] = V_i(f);
                fn = m->eng.field_name(b, 2);
                if (!fn || !*fn) continue;
                dsj_push(fields, dsj_str(fn));
            }
            dsj_set_str(co, "name", cn);
            dsj_set(co, "fields", fields);
            dsj_push(a, co);
        }
        dsj_set(o, "schema", a);
    }
    return o;
}

/* ------------------------------------------------------------ 资源引用
 * 资源改名(或删除)之后,场景里那些 tex_path/path 还指着旧名字 —— 用户视角就是
 * "改个名贴图全裂了"。这里把引用一并数出来/改掉(实体字符串字段 + 逻辑图里的路径)。 */

static char *replace_all(const char *src, const char *needle, const char *repl)
{
    size_t nl = strlen(needle), rl = strlen(repl);
    size_t cap, o = 0;
    const char *p = src;
    char *out;
    if (!nl) return NULL;
    cap = strlen(src) + 32;
    out = xm(cap);
    while (*p) {
        const char *hit = strstr(p, needle);
        size_t chunk;
        if (!hit) { strcpy(out + o, p); break; }
        chunk = (size_t)(hit - p);
        if (o + chunk + rl + 1 > cap) {
            cap = (o + chunk + rl + 1) * 2;
            out = xr(out, cap);
        }
        memcpy(out + o, p, chunk);
        o += chunk;
        memcpy(out + o, repl, rl);
        o += rl;
        p = hit + nl;
    }
    out[o] = 0;
    return out;
}

static int json_refs_walk(Dsj *v, const char *needle, const char *repl)
{
    int n = 0, i;
    if (!v) return 0;
    if (v->t == DSJ_STR) {
        if (v->str && strstr(v->str, needle)) {
            n = 1;
            if (repl) {
                char *nw = replace_all(v->str, needle, repl);
                if (nw) { free(v->str); v->str = nw; }
            }
        }
        return n;
    }
    if (v->t == DSJ_OBJ || v->t == DSJ_ARR) {
        for (i = 0; i < v->n; i++) n += json_refs_walk(v->items[i], needle, repl);
    }
    return n;
}

/* needle 形如 "res/hero.png"。apply=0 只数;apply=1 顺手改掉(一条撤销记录)。 */
int ds_model_asset_refs(DsModel *m, const char *name, const char *new_name, int apply)
{
    char needle[1024], repl[1024];
    int count = 0, n, i, c;
    Edit e;
    if (!m || !name || !*name) return 0;
    snprintf(needle, sizeof needle, "res/%s", name);
    if (new_name && *new_name) snprintf(repl, sizeof repl, "res/%s", new_name);
    else repl[0] = 0;
    e = edit_begin(m);
    n = m->eng.ok ? (int)m->eng.object_count(NULL, 0) : 0;
    for (i = 0; i < n; i++) {
        DexValue a[3];
        int64_t id;
        int nc = comp_total(m);
        a[0] = V_i(i);
        id = m->eng.object_id_at(a, 1);
        if (!id) continue;
        for (c = 1; c <= nc; c++) {
            DexValue b[3];
            const char *cn;
            int nf, f;
            b[0] = V_i(c);
            cn = m->eng.comp_name_at(b, 1);
            if (!cn || !*cn) continue;
            b[0] = V_i(id); b[1] = V_s(cn);
            if (m->eng.has(b, 2) != 1) continue;
            b[0] = V_s(cn); b[1] = V_i(0);
            nf = (int)m->eng.field_count(b, 2);
            for (f = 0; f < nf; f++) {
                const char *fn, *cur;
                b[0] = V_s(cn); b[1] = V_i(f);
                fn = m->eng.field_name(b, 2);
                if (!fn || !*fn) continue;
                if ((int)m->eng.field_type(b, 2) != 3) continue;
                {
                    DexValue g[3];
                    g[0] = V_i(id); g[1] = V_s(cn); g[2] = V_s(fn);
                    cur = m->eng.get_s(g, 3);
                }
                if (cur && strstr(cur, needle)) {
                    count++;
                    if (apply && repl[0]) {
                        DexValue s[4];
                        char *nw = replace_all(cur, needle, repl);
                        s[0] = V_i(id); s[1] = V_s(cn); s[2] = V_s(fn);
                        s[3] = V_s(nw ? nw : cur);
                        m->eng.set_s(s, 4);
                        free(nw);
                    }
                }
            }
        }
    }
    /* 逻辑图里的 play_sound.path 等也一起改 */
    count += json_refs_walk(ds_model_graph(m), needle, (apply && repl[0]) ? repl : NULL);
    edit_end(&e, (apply && count > 0) ? 1 : 0);
    return count;
}

/* ------------------------------------------------------------ 项目选择与"最近打开"
 * 以前"新建/打开项目"靠前端 prompt() 手打路径 —— 键盘负担最大的一处。 */
static char *recent_path(void)
{
    char buf[MAX_PATH * 2];
    snprintf(buf, sizeof buf, "%s\\recent.json", ds_temp_dir());
    return ds_strdup(buf);
}

static Dsj *recent_load(void)
{
    char *p = recent_path();
    char *txt = read_text(p, NULL);
    Dsj *d = NULL;
    free(p);
    if (txt) {
        char perr[128];
        d = dsj_parse(txt, perr, sizeof perr);
        free(txt);
    }
    if (!d || d->t != DSJ_ARR) {
        if (d) dsj_free(d);
        d = dsj_arr();
    }
    return d;
}

static void recent_save(Dsj *a)
{
    char *p = recent_path();
    char *txt = dsj_dump(a);
    if (txt) { write_text(p, txt); free(txt); }
    free(p);
}

/* 把 path 提到最近列表最前面(去重、最多 12 条) */
void ds_model_recent_push(DsModel *m, const char *path)
{
    Dsj *a, *out;
    int i;
    (void)m;
    if (!path || !*path) return;
    a = recent_load();
    out = dsj_arr();
    dsj_push(out, dsj_str(path));
    for (i = 0; i < dsj_len(a) && dsj_len(out) < 12; i++) {
        Dsj *it = dsj_at(a, i);
        if (it && it->t == DSJ_STR && it->str && strcmp(it->str, path)) {
            dsj_push(out, dsj_str(it->str));
        }
    }
    dsj_free(a);
    recent_save(out);
    dsj_free(out);
}

static Dsj *cmd_project_recent(DsModel *m)
{
    Dsj *a = recent_load();
    Dsj *out = dsj_arr();
    Dsj *r = dsj_obj();
    int i;
    (void)m;
    for (i = 0; i < dsj_len(a) && i < 12; i++) {
        Dsj *it = dsj_at(a, i);
        const char *p;
        char *pj;
        Dsj *o;
        if (!it || it->t != DSJ_STR || !it->str) continue;
        p = it->str;
        pj = pjoin(p, "project.json");
        o = dsj_obj();
        dsj_set_str(o, "path", p);
        {
            const char *slash = strrchr(p, '\\');
            const char *slash2 = strrchr(p, '/');
            if (slash2 && (!slash || slash2 > slash)) slash = slash2;
            dsj_set_str(o, "name", slash ? slash + 1 : p);
        }
        dsj_set_bool(o, "exists", path_exists(pj));
        free(pj);
        dsj_push(out, o);
    }
    dsj_free(a);
    dsj_set(r, "items", out);
    return r;
}

static Dsj *cmd_project_pick(DsModel *m, Dsj *args)
{
    Dsj *r = dsj_obj();
    (void)m;
    /* dry=1:只回"能弹但不弹"(测试/离屏自测绝不能卡在模态对话框上) */
    if (dsj_get_bool(args, "dry", 0)) {
        dsj_set_bool(r, "picked", 0);
        dsj_set_bool(r, "dry", 1);
        dsj_set_str(r, "path", "");
        return r;
    }
    {
        char buf[MAX_PATH * 4];
        buf[0] = 0;
        if (ds_pick_folder_utf8(buf, sizeof buf) && buf[0]) {
            dsj_set_bool(r, "picked", 1);
            dsj_set_str(r, "path", buf);
        } else {
            dsj_set_bool(r, "picked", 0);
            dsj_set_str(r, "path", "");
        }
    }
    return r;
}

/* ------------------------------------------------------------ 视口预览与瓦片刷子 */

/* CSV 文本 → tiles[row][col]。返回 malloc 的 int 数组(行优先)+ 行列数。
 * 容忍空行与前后空格;非数字一律当 -1(空)。 */
static int *csv_parse(const char *text, int *out_cols, int *out_rows)
{
    int *tiles = NULL, n = 0, cap = 0, cols = 0, rows = 0;
    const char *p = text ? text : "";
    int *row = NULL;
    int rowcap = 0, rown = 0;
    while (*p) {
        const char *line_end = p;
        while (*line_end && *line_end != '\n' && *line_end != '\r') line_end++;
        {
            const char *q = p;
            rown = 0;
            while (q < line_end) {
                const char *cell = q;
                int v = -1;
                while (q < line_end && *q != ',') q++;
                {
                    char tmp[32];
                    size_t len = (size_t)(q - cell);
                    while (len && (*cell == ' ' || *cell == '\t')) { cell++; len--; }
                    while (len && (cell[len - 1] == ' ' || cell[len - 1] == '\t')) len--;
                    if (len && len < sizeof tmp) {
                        char *endp = NULL;
                        memcpy(tmp, cell, len);
                        tmp[len] = 0;
                        v = (int)strtol(tmp, &endp, 10);
                        if (endp == tmp) v = -1;   /* 不是数字 = 空格子 */
                    }
                }
                if (rown == rowcap) {
                    rowcap = rowcap ? rowcap * 2 : 16;
                    row = xr(row, (size_t)rowcap * sizeof *row);
                }
                row[rown++] = v;
                if (q < line_end) q++;   /* 跳过逗号 */
            }
            if (rown > 0) {
                int c;
                if (cols == 0) cols = rown;
                if (n + cols > cap) {
                    while (n + cols > cap) cap = cap ? cap * 2 : 256;
                    tiles = xr(tiles, (size_t)cap * sizeof *tiles);
                }
                for (c = 0; c < cols; c++) tiles[n++] = (c < rown) ? row[c] : -1;
                rows++;
            }
        }
        p = line_end;
        while (*p == '\r' || *p == '\n') p++;
    }
    free(row);
    *out_cols = cols;
    *out_rows = rows;
    if (!tiles) tiles = xm(sizeof *tiles);
    (void)n;
    return tiles;
}

static char *csv_format(const int *tiles, int cols, int rows)
{
    size_t cap = (size_t)cols * (size_t)rows * 5 + (size_t)rows * 2 + 16;
    char *out = xm(cap);
    size_t o = 0;
    int r, c;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            o += (size_t)snprintf(out + o, cap - o, "%s%d", c ? "," : "",
                                  tiles[r * cols + c]);
        }
        out[o++] = '\n';
    }
    out[o] = 0;
    return out;
}

/* 预览目录:项目内优先(.dexstudio/),没项目时用 exe 附近的 gitignored 目录。
 * 都保证已创建 —— 前端会用虚拟主机把它当图片源。 */
const char *ds_preview_dir(DsModel *m)
{
    static char dir[1400];
    if (m->root[0]) snprintf(dir, sizeof dir, "%s\\.dexstudio", m->root);
    else snprintf(dir, sizeof dir, "%s\\preview", ds_temp_dir());
    ensure_dir(dir);
    return dir;
}

const char *ds_project_dir(DsModel *m) { return m->root; }
const char *ds_model_scene_path(DsModel *m) { return m->scene; }

/* 可写的缓存目录(见 ds_model.h)。进程内缓存一次。 */
const char *ds_temp_dir(void)
{
    static char dir[1400];
    static int done = 0;
    if (!done) {
        /* 注意:窄的 getenv 在**中文用户名**下拿到的就已经是乱码路径了
         * (%LOCALAPPDATA% = C:\Users\<用户名>\AppData\Local),所以要问宽字符那套。 */
        char *base = dsu_env("LOCALAPPDATA");
        if (!base || !*base) { free(base); base = dsu_env("TEMP"); }
        if (!base || !*base) { free(base); base = dsu_env("TMP"); }
        snprintf(dir, sizeof dir, "%s\\DexStudio", (base && *base) ? base : ".");
        free(base);
        ensure_dir(dir);
        done = 1;
    }
    return dir;
}
void ds_model_mark_dirty(DsModel *m) { m->dirty = 1; }
/* 给 ds_run.c 用:编译前必须存盘(项目 + 场景 + 逻辑图) */
int ds_model_project_save(DsModel *m) { return project_save(m); }
const char *ds_model_start_scene(DsModel *m)
{
    return (m && m->project) ? dsj_get_str(m->project, "start_scene", "") : "";
}
int ds_model_autosave_seq(DsModel *m) { return ++m->autosave_seq; }
int ds_model_dirty(DsModel *m) { return m->dirty; }
const char *ds_model_session(DsModel *m) { return m ? m->session : ""; }
/* 给 ds_res.c 用的公开包装(内部那两份是 static) */
char *ds_scene_snapshot(DsModel *m) { return scene_snapshot(m); }
Dsj *ds_files_snapshot(DsModel *m) { return files_snapshot(m); }

/* ------------------------------------------------ 给 ds_graph.c 的最小访问面 */

Dsj *ds_model_graph(DsModel *m)
{
    if (!m->graph) m->graph = dsj_obj();
    return m->graph;
}

void ds_model_set_graph(DsModel *m, Dsj *g)
{
    if (m->graph) dsj_free(m->graph);
    m->graph = g ? g : dsj_obj();
}

/* 积木脚本(与图并列;前端 / ds_blocks.c 用) */
Dsj *ds_model_blocks(DsModel *m)
{
    if (!m->blocks) m->blocks = dsj_obj();
    return m->blocks;
}

void ds_model_set_blocks(DsModel *m, Dsj *g)
{
    if (m->blocks) dsj_free(m->blocks);
    m->blocks = g ? g : dsj_obj();
}

/* 用哪一套生成 logic.dex:
 *   · 新项目默认 "blocks"(零基础用户先看到积木);
 *   · 老项目(project.json 里没有这个键)**保持 "graph"** —— 它们的节点图里
 *     已经有内容,静默换一套生成器会让既有玩法失效。 */
const char *ds_model_logic_mode(DsModel *m)
{
    const char *v;
    if (!m || !m->project) return "graph";
    v = dsj_get_str(m->project, "logic_mode", "");
    if (v && (!strcmp(v, "blocks") || !strcmp(v, "graph"))) return v;
    return "graph";
}

void ds_model_set_logic_mode(DsModel *m, const char *mode)
{
    if (!m || !m->project) return;
    if (!mode || (strcmp(mode, "blocks") && strcmp(mode, "graph"))) return;
    dsj_set_str(m->project, "logic_mode", mode);
    m->dirty = 1;
}

void ds_model_error(DsModel *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->err, sizeof m->err, fmt, ap);
    va_end(ap);
}

void *ds_model_edit_open(DsModel *m)
{
    Edit *e = xm(sizeof *e);
    *e = edit_begin(m);
    return e;
}

void ds_model_edit_close(DsModel *m, void *tok, int changed)
{
    Edit *e = (Edit *)tok;
    if (!e) return;
    edit_end(e, changed);
    free(e);
    if (changed) m->dirty = 1;
}

/* 每个实体的**世界包围盒** + 图层信息,一次调用给全。
 * 视口要画选中框、图层面板要按 layer 分组,都需要它;若让前端逐个 entity.get,
 * 一百个实体就是一百次往返。这里的算法与引擎渲染一致:
 *   有 collider → 碰撞盒;否则有 sprite → 精灵框;否则有 tilemap → 整图;再否则 8×8。
 * 坐标用 eng_world_x/y(层级只继承位置,与引擎同一份实现)。 */
static Dsj *cmd_scene_outline(DsModel *m, Dsj *args)
{
    Dsj *arr = dsj_arr();
    int n, i;
    (void)args;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    n = (int)m->eng.object_count(NULL, 0);
    for (i = 0; i < n; i++) {
        DexValue a[3];
        int64_t id;
        Dsj *o = dsj_obj();
        double wx = 0, wy = 0, bx = 0, by = 0, bw = 0, bh = 0;
        const char *kind = "empty";
        int layer = 0;
        int order = 0;
        int seq = 0;
        a[0] = V_i(i);
        id = m->eng.object_id_at(a, 1);
        if (!id) { dsj_free(o); continue; }
        seq = i;
        {
            DexValue w[1];
            const char *nm;
            w[0] = V_i(id);
            wx = m->eng.world_x(w, 1);
            wy = m->eng.world_y(w, 1);
            nm = m->eng.name(w, 1);
            dsj_set_str(o, "name", nm ? nm : "");
        }
        dsj_set_int(o, "id", (long long)id);
#define DSI(c, f) (m->eng.get_i((DexValue[]){V_i(id), V_s(c), V_s(f)}, 3))
#define DSF(c, f) (m->eng.get_f((DexValue[]){V_i(id), V_s(c), V_s(f)}, 3))
#define HASC(c) (m->eng.has((DexValue[]){V_i(id), V_s(c)}, 2) == 1)
        if (HASC("collider")) {
            double ox = DSF("collider", "ox"), oy = DSF("collider", "oy");
            double hw = DSF("collider", "hw"), hh = DSF("collider", "hh");
            bx = wx + ox - hw; by = wy + oy - hh; bw = hw * 2; bh = hh * 2;
            kind = (DSI("collider", "kind") == 0) ? "box" : "shape";
        } else if (HASC("sprite")) {
            double px = DSF("sprite", "px"), py = DSF("sprite", "py");
            double sw = DSF("sprite", "sw"), sh = DSF("sprite", "sh");
            double kx = 1, ky = 1;
            if (sw <= 0) sw = (double)m->eng.tex_width((DexValue[]){V_i(DSI("sprite", "texture"))}, 1);
            if (sh <= 0) sh = (double)m->eng.tex_height((DexValue[]){V_i(DSI("sprite", "texture"))}, 1);
            if (sw <= 0) sw = 32;
            if (sh <= 0) sh = 32;
            /* 选中框要跟**画出来的样子**一致 —— 公式必须与引擎
             * (dg_scene.c 的 sprite 绘制)逐字相同:
             *     原点 = 世界坐标 - 源尺寸 × 轴心 × 缩放;宽高 = 源尺寸 × 缩放
             * 以前这里写的是 `wx + px`,而引擎是 `wx - sw*px` —— 于是"框线跑到了
             * 图片右下角外面",用户看到的就是"改了贴图只有框线在动,图像没出现"。 */
            if (HASC("transform")) {
                double a = DSF("transform", "sx"), b = DSF("transform", "sy");
                if (a != 0) kx = a;
                if (b != 0) ky = b;
            }
            bx = wx - sw * px * kx; by = wy - sh * py * ky;
            bw = sw * kx; bh = sh * ky;
            kind = "sprite";
        } else if (HASC("tilemap")) {
            double tw = DSF("tilemap", "tw"), th = DSF("tilemap", "th");
            int cols = (int)DSI("tilemap", "cols"), rows = (int)DSI("tilemap", "rows");
            bx = wx; by = wy; bw = tw * cols; bh = th * rows;
            kind = "tilemap";
        } else {
            bx = wx - 8; by = wy - 8; bw = 16; bh = 16;
        }
        if (HASC("sprite")) layer = (int)DSI("sprite", "layer");
        else if (HASC("tilemap")) layer = (int)DSI("tilemap", "layer");
        else if (HASC("camera")) kind = "camera";
        /* 同层顺序也给出来:视口命中测试要按**渲染顺序**取最上面那个,
         * 只按创建顺序取会和用户看到的画面不一致。 */
        if (HASC("sprite")) order = (int)DSI("sprite", "order");
        else if (HASC("tilemap")) order = (int)DSI("tilemap", "order");
        dsj_set_num(o, "x", bx);
        dsj_set_num(o, "y", by);
        dsj_set_num(o, "w", bw);
        dsj_set_num(o, "h", bh);
        dsj_set_num(o, "wx", wx);
        dsj_set_num(o, "wy", wy);
        dsj_set_str(o, "kind", kind);
        dsj_set_int(o, "layer", layer);
        dsj_set_int(o, "order", order);
        dsj_set_int(o, "seq", seq);
        dsj_push(arr, o);
#undef DSI
#undef DSF
#undef HASC
    }
    return arr;
}

static Dsj *cmd_view_set(DsModel *m, Dsj *args)
{
    Dsj *r = dsj_obj();
    if (dsj_get(args, "on") == NULL && dsj_get(args, "x") == NULL
        && dsj_get(args, "y") == NULL && dsj_get(args, "zoom") == NULL) {
        /* 无参数 = 回到"用场景相机" */
        m->view_on = 0;
    } else {
        if (dsj_get(args, "on") != NULL) m->view_on = dsj_get_bool(args, "on", 1);
        else m->view_on = 1;
        m->view_x = dsj_get_num(args, "x", m->view_x);
        m->view_y = dsj_get_num(args, "y", m->view_y);
        m->view_zoom = dsj_get_num(args, "zoom", m->view_zoom);
        if (m->view_zoom <= 0.0) m->view_zoom = 1.0;
    }
    dsj_set_bool(r, "on", m->view_on);
    dsj_set_num(r, "x", m->view_x);
    dsj_set_num(r, "y", m->view_y);
    dsj_set_num(r, "zoom", m->view_zoom);
    return r;
}

/* 离屏渲染一帧并落成 BMP。前端用 <img src="https://dexstudio-preview.local/preview.bmp?t=N">
 * 显示 —— 走虚拟主机让浏览器直接读文件,比把像素塞进 JSON 便宜几个数量级。 */
static Dsj *cmd_scene_render(DsModel *m, Dsj *args)
{
    char *dir = xs(ds_preview_dir(m));
    char *path = pjoin(dir, "preview.bmp");
    Dsj *r;
    if (!m->eng.ok) {
        seterr(m, "引擎不可用:%s", m->eng.err);
        free(dir); free(path);
        return NULL;
    }
    if (dsj_get(args, "x") || dsj_get(args, "y") || dsj_get(args, "zoom")) {
        m->view_on = 1;
        m->view_x = dsj_get_num(args, "x", m->view_x);
        m->view_y = dsj_get_num(args, "y", m->view_y);
        m->view_zoom = dsj_get_num(args, "zoom", m->view_zoom);
        if (m->view_zoom <= 0.0) m->view_zoom = 1.0;
    }
    {
        DexValue a[4];
        DexValue c[1];
        DexValue p[1];
        a[0] = V_i(m->view_on);
        a[1] = V_f(m->view_x);
        a[2] = V_f(m->view_y);
        a[3] = V_f(m->view_zoom);
        m->eng.set_view(a, 4);
        c[0] = V_i(0xFF1E1E2E);      /* 深色背景,和 IDE 一致(ABGR 与引擎约定一致) */
        m->eng.set_clear_color(c, 1);
        m->eng.frame_begin(NULL, 0);
        m->eng.draw_scene(NULL, 0);
        m->eng.frame_end(NULL, 0);
        p[0] = V_s(path);
        if (m->eng.save_bmp(p, 1) != 0) {
            seterr(m, "渲染失败:%s", ds_engine_last_error(&m->eng));
            free(dir); free(path);
            return NULL;
        }
    }
    m->render_seq++;
    {
        /* 报**实际生效**的视图(没有覆盖时就是活动相机的值) */
        double vx = m->view_x, vy = m->view_y, vz = m->view_zoom;
        if (!m->view_on) effective_view(m, &vx, &vy, &vz);
        r = dsj_obj();
        dsj_set_str(r, "url", "https://dexstudio-preview.local/preview.bmp");
        dsj_set_int(r, "seq", m->render_seq);
        dsj_set_str(r, "path", path);
        dsj_set_bool(r, "view_on", m->view_on);
        dsj_set_num(r, "x", vx);
        dsj_set_num(r, "y", vy);
        dsj_set_num(r, "zoom", vz);
    }
    dsj_set_int(r, "w", (long long)m->eng.width(NULL, 0));
    dsj_set_int(r, "h", (long long)m->eng.height(NULL, 0));
    free(dir);
    free(path);
    return r;
}

static Dsj *cmd_tilemap_info(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    char path[1024], tex[1024];
    int cols, rows;
    char *text;
    int *tiles;
    Dsj *r, *arr;
    int i;
    DexValue a[3];
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id) { seterr(m, "tilemap.info 需要 args.id"); return NULL; }
    a[0] = V_i(id); a[1] = V_s("tilemap");
    if (m->eng.has(a, 2) != 1) { seterr(m, "实体 %lld 没有 tilemap 组件", (long long)id); return NULL; }
    a[0] = V_i(id); a[1] = V_s("tilemap"); a[2] = V_s("path");
    snprintf(path, sizeof path, "%s", m->eng.get_s(a, 3));
    a[2] = V_s("tex_path");
    snprintf(tex, sizeof tex, "%s", m->eng.get_s(a, 3));
    a[2] = V_s("cols");
    cols = (int)m->eng.get_i(a, 3);
    a[2] = V_s("rows");
    rows = (int)m->eng.get_i(a, 3);
    text = path[0] ? read_text(path, NULL) : NULL;    if (text) {
        tiles = csv_parse(text, &cols, &rows);
        free(text);
    } else {
        int k;
        tiles = xm((size_t)(cols > 0 ? cols : 1) * (size_t)(rows > 0 ? rows : 1) * sizeof *tiles);
        for (k = 0; k < (cols > 0 ? cols : 0) * (rows > 0 ? rows : 0); k++) tiles[k] = -1;
    }
    r = dsj_obj();
    dsj_set_int(r, "id", (long long)id);
    dsj_set_str(r, "path", path);
    dsj_set_str(r, "tex_path", tex);
    dsj_set_int(r, "cols", cols);
    dsj_set_int(r, "rows", rows);
    a[2] = V_s("tw");
    dsj_set_num(r, "tw", m->eng.get_f(a, 3));
    a[2] = V_s("th");
    dsj_set_num(r, "th", m->eng.get_f(a, 3));
    a[2] = V_s("atlas_tile");
    dsj_set_int(r, "atlas_tile", (long long)m->eng.get_i(a, 3));
    a[2] = V_s("atlas_cols");
    dsj_set_int(r, "atlas_cols", (long long)m->eng.get_i(a, 3));
    a[2] = V_s("visible");
    dsj_set_bool(r, "visible", m->eng.get_i(a, 3) != 0);
    if (tex[0] && m->root[0] && !strncmp(tex, m->root, strlen(m->root))) {
        char url[1400];
        snprintf(url, sizeof url, "https://dexstudio-proj.local/%s",
                 tex + strlen(m->root) + 1);
        { size_t k; for (k = 0; url[k]; k++) if (url[k] == '\\') url[k] = '/'; }
        dsj_set_str(r, "atlas_url", url);
    } else {
        dsj_set_str(r, "atlas_url", "");
    }
    arr = dsj_arr();
    for (i = 0; i < cols * rows && i < 65536; i++) dsj_push(arr, dsj_int(tiles[i]));
    dsj_set(r, "tiles", arr);
    free(tiles);
    return r;
}

/* 给实体的 tilemap 组件**建**一份 CSV(默认 16×16 全空),然后再设 path。
 * 为什么要单独一个命令:引擎的 comp.set(tilemap.path) 会**立刻加载**该文件,
 * 文件不存在就报错(而且字段已经被写进去了)—— 前端会看到"失败"却又是"生效"的。
 * 这里先把文件写出来再设路径,于是整步要么全成、要么带原因失败。 */
static Dsj *cmd_tilemap_create(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *rel = dsj_get_str(args, "path", "");
    const char *tex = dsj_get_str(args, "tex_path", "");
    int cols = (int)dsj_get_int(args, "cols", 16);
    int rows = (int)dsj_get_int(args, "rows", 16);
    char *full;
    int *tiles;
    char *text;
    Dsj *r;
    DexValue a[3];
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id) { seterr(m, "tilemap.create 需要 args.id"); return NULL; }
    if (!rel || !*rel) { seterr(m, "tilemap.create 需要 args.path(CSV 文件)"); return NULL; }
    if (cols <= 0) cols = 16;
    if (rows <= 0) rows = 16;
    if ((long long)cols * rows > 65536) {
        seterr(m, "瓦片地图太大(%d×%d,上限 65536 格)", cols, rows);
        return NULL;
    }
    a[0] = V_i(id); a[1] = V_s("tilemap");
    if (m->eng.has(a, 2) != 1) { seterr(m, "实体 %lld 没有 tilemap 组件", (long long)id); return NULL; }
    full = path_is_abs(rel) ? xs(rel)
         : pjoin(m->root[0] ? m->root : ds_preview_dir(m), rel);
    {
        /* 建父目录 */
        char *slash = strrchr(full, '\\');
        char *slash2 = strrchr(full, '/');
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        if (slash) {
            char *dir = xm((size_t)(slash - full) + 1);
            memcpy(dir, full, (size_t)(slash - full));
            dir[slash - full] = 0;
            ensure_dir(dir);
            free(dir);
        }
    }
    e = edit_begin(m);
    if (!file_exists(full)) {
        int k;
        tiles = xm((size_t)cols * (size_t)rows * sizeof *tiles);
        for (k = 0; k < cols * rows; k++) tiles[k] = -1;
        text = csv_format(tiles, cols, rows);
        free(tiles);
        if (!write_text(full, text)) {
            seterr(m, "无法写入 %s", full);
            free(text); free(full);
            edit_end(&e, 0);
            return NULL;
        }
        free(text);
    }
    /* 已经存在就沿用它(不要覆盖用户的数据) */
    {
        DexValue s[4];
        s[0] = V_i(id); s[1] = V_s("tilemap"); s[2] = V_s("path"); s[3] = V_s(full);
        if (m->eng.set_s(s, 4) != 0) {
            seterr(m, "设置 tilemap.path 失败:%s", ds_engine_last_error(&m->eng));
            free(full);
            edit_end(&e, 0);
            return NULL;
        }
    }
    if (tex && *tex) {
        DexValue s[4];
        char *tfull = path_is_abs(tex) ? xs(tex)
                     : pjoin(m->root[0] ? m->root : ".", tex);
        s[0] = V_i(id); s[1] = V_s("tilemap"); s[2] = V_s("tex_path"); s[3] = V_s(tfull);
        if (m->eng.set_s(s, 4) != 0) {
            seterr(m, "设置 tilemap.tex_path 失败:%s", ds_engine_last_error(&m->eng));
            free(tfull); free(full);
            edit_end(&e, 0);
            return NULL;
        }
        free(tfull);
    }
    edit_end(&e, 1);
    r = dsj_obj();
    dsj_set_str(r, "path", full);
    dsj_set_int(r, "cols", cols);
    dsj_set_int(r, "rows", rows);
    free(full);
    return r;
}

/* 把若干格写进瓦片地图的 CSV,再让引擎重新加载。**一次调用 = 一条撤销记录**:
 * 刷子拖一下就改几十格,按格入栈会让"撤销"要点几十次。
 * cells 形如 [{"col":c,"row":r,"tile":t}, …]。失败返回 0(带原因)。 */
static int tilemap_apply_cells(DsModel *m, int64_t id, Dsj *cells, int *out_cols,
                               int *out_rows)
{
    char path[1024];
    int cols = 0, rows = 0;
    char *text, *out;
    int *tiles;
    int i, maxc = -1, maxr = -1;
    DexValue a[3];
    Edit e;
    a[0] = V_i(id); a[1] = V_s("tilemap");
    if (m->eng.has(a, 2) != 1) {
        seterr(m, "实体 %lld 没有 tilemap 组件", (long long)id);
        return 0;
    }
    if (!cells || cells->t != DSJ_ARR || dsj_len(cells) == 0) {
        seterr(m, "没有要写的格子");
        return 0;
    }
    a[0] = V_i(id); a[1] = V_s("tilemap"); a[2] = V_s("path");
    snprintf(path, sizeof path, "%s", m->eng.get_s(a, 3));
    if (!path[0]) {
        seterr(m, "这个瓦片地图还没有 CSV 路径(先用「新建瓦片地图」或 tilemap.create)");
        return 0;
    }
    text = read_text(path, NULL);
    if (text) {
        tiles = csv_parse(text, &cols, &rows);
        free(text);
    } else {
        int k;
        a[2] = V_s("cols");
        cols = (int)m->eng.get_i(a, 3);
        a[2] = V_s("rows");
        rows = (int)m->eng.get_i(a, 3);
        if (cols < 0) cols = 0;
        if (rows < 0) rows = 0;
        tiles = xm((size_t)(cols > 0 ? cols : 1) * (size_t)(rows > 0 ? rows : 1)
                   * sizeof *tiles);
        for (k = 0; k < cols * rows; k++) tiles[k] = -1;
    }
    for (i = 0; i < dsj_len(cells); i++) {
        Dsj *c = dsj_at(cells, i);
        int col, row;
        if (!c || c->t != DSJ_OBJ) continue;
        col = (int)dsj_get_int(c, "col", -1);
        row = (int)dsj_get_int(c, "row", -1);
        if (col < 0 || row < 0) continue;
        if (col > maxc) maxc = col;
        if (row > maxr) maxr = row;
    }
    if (maxc < 0) { seterr(m, "格子坐标无效"); free(tiles); return 0; }
    if (maxc >= cols || maxr >= rows) {  /* 越界就扩,不报错(画到边缘外是常态) */
        int nc = maxc + 1 > cols ? maxc + 1 : cols;
        int nr = maxr + 1 > rows ? maxr + 1 : rows;
        int *nt = xm((size_t)nc * (size_t)nr * sizeof *nt);
        int rr, cc;
        for (rr = 0; rr < nr; rr++)
            for (cc = 0; cc < nc; cc++)
                nt[rr * nc + cc] = (rr < rows && cc < cols) ? tiles[rr * cols + cc] : -1;
        free(tiles);
        tiles = nt;
        cols = nc;
        rows = nr;
    }
    for (i = 0; i < dsj_len(cells); i++) {
        Dsj *c = dsj_at(cells, i);
        int col, row;
        if (!c || c->t != DSJ_OBJ) continue;
        col = (int)dsj_get_int(c, "col", -1);
        row = (int)dsj_get_int(c, "row", -1);
        if (col < 0 || row < 0 || col >= cols || row >= rows) continue;
        tiles[row * cols + col] = (int)dsj_get_int(c, "tile", -1);
    }
    out = csv_format(tiles, cols, rows);
    free(tiles);
    e = edit_begin(m);
    if (!write_text(path, out)) {
        seterr(m, "无法写入 %s", path);
        free(out);
        edit_end(&e, 0);
        return 0;
    }
    free(out);
    {
        DexValue d[2];
        d[0] = V_i(id); d[1] = V_s(path);
        if (m->eng.tilemap_load_file(d, 2) != 0) {
            seterr(m, "重新加载瓦片地图失败:%s", ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return 0;
        }
    }
    edit_end(&e, 1);
    if (out_cols) *out_cols = cols;
    if (out_rows) *out_rows = rows;
    return 1;
}

static Dsj *cmd_tilemap_set(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    int col = (int)dsj_get_int(args, "col", -1);
    int row = (int)dsj_get_int(args, "row", -1);
    int tile = (int)dsj_get_int(args, "tile", -1);
    int cols = 0, rows = 0;
    Dsj *cells, *r;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id || col < 0 || row < 0) {
        seterr(m, "tilemap.set 需要 args.id / col / row");
        return NULL;
    }
    cells = dsj_arr();
    {
        Dsj *c = dsj_obj();
        dsj_set_int(c, "col", col);
        dsj_set_int(c, "row", row);
        dsj_set_int(c, "tile", tile);
        dsj_push(cells, c);
    }
    if (!tilemap_apply_cells(m, id, cells, &cols, &rows)) {
        dsj_free(cells);
        return NULL;
    }
    dsj_free(cells);
    r = dsj_obj();
    dsj_set_int(r, "col", col);
    dsj_set_int(r, "row", row);
    dsj_set_int(r, "tile", tile);
    dsj_set_int(r, "cols", cols);
    dsj_set_int(r, "rows", rows);
    return r;
}

/* 一次刷一片(拖动刷子用)。args.cells = [{col,row,tile}, …] */
static Dsj *cmd_tilemap_paint(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    int cols = 0, rows = 0, n;
    Dsj *cells = dsj_get(args, "cells");
    Dsj *r;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id) { seterr(m, "tilemap.paint 需要 args.id"); return NULL; }
    n = cells ? dsj_len(cells) : 0;
    if (!tilemap_apply_cells(m, id, cells, &cols, &rows)) return NULL;
    r = dsj_obj();
    dsj_set_int(r, "count", n);
    dsj_set_int(r, "cols", cols);
    dsj_set_int(r, "rows", rows);
    return r;
}

/* 把瓦片地图的 CSV 换成一段文本(IDE 的"整图导入/清空"用) */
static Dsj *cmd_tilemap_csv(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *csv = dsj_get_str(args, "csv", "");
    char path[1024];
    Dsj *r;
    DexValue a[3];
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id) { seterr(m, "tilemap.csv 需要 args.id"); return NULL; }
    a[0] = V_i(id); a[1] = V_s("tilemap");
    if (m->eng.has(a, 2) != 1) { seterr(m, "实体 %lld 没有 tilemap 组件", (long long)id); return NULL; }
    e = edit_begin(m);
    {
        DexValue d[2];
        d[0] = V_i(id); d[1] = V_s(csv);
        if (m->eng.tilemap_load_csv(d, 2) != 0) {
            seterr(m, "加载 CSV 失败:%s", ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return NULL;
        }
    }
    a[0] = V_i(id); a[1] = V_s("tilemap"); a[2] = V_s("path");
    snprintf(path, sizeof path, "%s", m->eng.get_s(a, 3));
    if (path[0]) write_text(path, csv);
    edit_end(&e, 1);
    r = dsj_obj();
    dsj_set_str(r, "path", path);
    return r;
}

static Dsj *cmd_entity_find(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    DexValue a[1];
    Dsj *r;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    a[0] = V_s(name);
    r = dsj_obj();
    dsj_set_int(r, "id", (long long)m->eng.find(a, 1));
    return r;
}

/* ------------------------------------------------------------ dispatch */

const char *ds_command(DsModel *m, const char *request)
{
    char perr[256];
    Dsj *req, *args, *ret = NULL;
    Dsj empty;
    const char *cmd;

    if (!m) return "{\"ok\":false,\"error\":\"no model\"}";
    m->err[0] = 0;
    m->has_id = 0;
    if (!request || !*request) return resp_ok(m, cmd_app_info(m));
    req = dsj_parse(request, perr, sizeof perr);
    if (!req || req->t != DSJ_OBJ) {
        char msg[400];
        if (req) dsj_free(req);
        snprintf(msg, sizeof msg, "请求不是 JSON 对象:%s", perr);
        return resp_err(m, msg);
    }
    if (dsj_get(req, "id")) {
        m->has_id = 1;
        m->resp_id = dsj_get_int(req, "id", 0);
    }
    cmd = dsj_get_str(req, "cmd", "");
    args = dsj_get(req, "args");
    if (!args || args->t != DSJ_OBJ) {
        memset(&empty, 0, sizeof empty);
        empty.t = DSJ_OBJ;
        args = &empty;
    }

    if (!strcmp(cmd, "app.info") || !cmd[0]) ret = cmd_app_info(m);
    else if (!strcmp(cmd, "ui.ready")) {
        /* 前端宣布"界面已就绪"。宿主用它做 --wv-selftest 的判据(不需要人看屏幕),
         * 以后也方便在就绪后推初始状态。 */
        Dsj *r = dsj_obj();
        DexValue none[1];
        dsj_set_bool(r, "ready", 1);
        dsj_set_int(r, "objects",
                    m->eng.ok ? (long long)m->eng.object_count(none, 0) : 0);
        ret = r;
    } else if (!strcmp(cmd, "ui.boot")) {
        /* 前端第一件事:确认桥可用(见 web/index.html 顶部那段内联脚本)。
         * 宿主 --wv-selftest 用它区分"桥不通"和"页面里 JS 出错"。 */
        Dsj *r = dsj_obj();
        dsj_set_bool(r, "boot", 1);
        dsj_set_str(r, "href", dsj_get_str(args, "href", ""));
        ret = r;
    } else if (!strcmp(cmd, "ui.error")) {
        Dsj *r = dsj_obj();
        snprintf(m->ui_error, sizeof m->ui_error, "%s (%s:%lld:%lld)",
                 dsj_get_str(args, "msg", ""), dsj_get_str(args, "src", ""),
                 dsj_get_int(args, "line", 0), dsj_get_int(args, "col", 0));
        dsj_set_bool(r, "noted", 1);
        ret = r;
    } else if (!strcmp(cmd, "app.components")) {
        Dsj *r = dsj_obj();
        dsj_set(r, "components", comp_names(m));
        ret = r;
    } else if (!strcmp(cmd, "project.new")) ret = cmd_project_new(m, args);
    else if (!strcmp(cmd, "project.open")) ret = cmd_project_open(m, args);
    else if (!strcmp(cmd, "project.pick")) ret = cmd_project_pick(m, args);
    else if (!strcmp(cmd, "project.recent")) ret = cmd_project_recent(m);
    else if (!strcmp(cmd, "scene.options")) ret = ds_model_scene_options(m);
    else if (!strcmp(cmd, "logic.mode")) {
        /* 切"用积木还是用节点图生成逻辑"(project.json 的 logic_mode) */
        Dsj *r = dsj_obj();
        const char *mode = dsj_get_str(args, "mode", "");
        if (mode && *mode) {
            if (strcmp(mode, "blocks") && strcmp(mode, "graph")) {
                seterr(m, "logic.mode 只认 blocks / graph");
            } else {
                ds_model_set_logic_mode(m, mode);
                dsj_set_str(r, "mode", mode);
                ret = r;
            }
        } else {
            dsj_set_str(r, "mode", ds_model_logic_mode(m));
            ret = r;
        }
    }
    else if (!strcmp(cmd, "scene.set_start")) ret = cmd_scene_set_start(m, args);
    else if (!strcmp(cmd, "scene.rename")) ret = cmd_scene_rename(m, args);
    else if (!strcmp(cmd, "scene.delete")) ret = cmd_scene_delete(m, args);
    else if (!strcmp(cmd, "project.state")) {
        Dsj *r = dsj_obj();
        dsj_set_str(r, "root", m->root);
        dsj_set_str(r, "scene", m->scene);
        dsj_set_bool(r, "dirty", m->dirty);
        dsj_set_bool(r, "has_project", m->project != NULL);
        dsj_set_str(r, "logic_mode", ds_model_logic_mode(m));
        if (m->project) {
            char *txt = dsj_dump(m->project);
            Dsj *p = dsj_parse(txt, NULL, 0);
            free(txt);
            if (p) dsj_set(r, "project", p);
        }
        if (m->root[0]) {
            Dsj *scenes = cmd_scene_list(m);
            dsj_set(r, "scenes", scenes ? scenes : dsj_arr());
        } else {
            dsj_set(r, "scenes", dsj_arr());
        }
        ret = r;
    } else if (!strcmp(cmd, "project.save")) {
        if (project_save(m)) {
            Dsj *r = dsj_obj();
            dsj_set_bool(r, "saved", 1);
            dsj_set_str(r, "root", m->root);
            ret = r;
        }
    } else if (!strcmp(cmd, "scene.new")) ret = cmd_scene_new(m, args);
    else if (!strcmp(cmd, "scene.load")) ret = cmd_scene_load(m, args);
    else if (!strcmp(cmd, "scene.save") || !strcmp(cmd, "scene.save_as"))
        ret = cmd_scene_save(m, args);
    else if (!strcmp(cmd, "scene.list")) ret = cmd_scene_list(m);
    else if (!strcmp(cmd, "scene.json")) {
        const char *txt;
        DexValue none[1];
        if (!m->eng.ok) seterr(m, "引擎不可用:%s", m->eng.err);
        else {
            txt = m->eng.scene_json(none, 0);
            ret = dsj_parse(txt ? txt : "{}", perr, sizeof perr);
            if (!ret) seterr(m, "场景 JSON 解析失败:%s", perr);
        }
    } else if (!strcmp(cmd, "entity.list")) {
        Dsj *a = dsj_arr();
        int n, i;
        if (!m->eng.ok) seterr(m, "引擎不可用:%s", m->eng.err);
        else {
            n = (int)m->eng.object_count(NULL, 0);
            for (i = 0; i < n; i++) {
                DexValue a1[1];
                int64_t id;
                Dsj *e;
                a1[0] = V_i(i);
                id = m->eng.object_id_at(a1, 1);
                if (!id) continue;
                e = dsj_obj();
                dsj_set_int(e, "id", (long long)id);
                dsj_set_str(e, "name", obj_name(m, id));
                dsj_set_num(e, "x", obj_world(m, id, m->eng.world_x));
                dsj_set_num(e, "y", obj_world(m, id, m->eng.world_y));
                /* 父实体:id 之外还要给 parent —— 层级树靠它缩进/拖拽,
                 * 前端不该为了画一棵树再逐个 entity.get。 */
                if (m->eng.has((DexValue[]){V_i(id), V_s("transform")}, 2) == 1) {
                    dsj_set_int(e, "parent",
                                (long long)m->eng.get_i((DexValue[]){V_i(id),
                                    V_s("transform"), V_s("parent")}, 3));
                } else {
                    dsj_set_int(e, "parent", -1);
                }
                {
                    Dsj *comps = dsj_arr();
                    int c, nc = comp_total(m);
                    for (c = 1; c <= nc; c++) {
                        DexValue a2[2];
                        const char *cn;
                        a2[0] = V_i(c);
                        cn = m->eng.comp_name_at(a2, 1);
                        if (!cn || !*cn) continue;
                        a2[0] = V_i(id); a2[1] = V_s(cn);
                        if (m->eng.has(a2, 2) == 1) dsj_push(comps, dsj_str(cn));
                    }
                    dsj_set(e, "comps", comps);
                }
                dsj_push(a, e);
            }
            ret = a;
        }
    } else if (!strcmp(cmd, "entity.get")) {
        int64_t id = dsj_get_int(args, "id", 0);
        if (!m->eng.ok) seterr(m, "引擎不可用:%s", m->eng.err);
        else if (!id) seterr(m, "entity.get 需要 args.id");
        else {
            DexValue a1[1];
            a1[0] = V_i(id);
            if (m->eng.object_alive(a1, 1) != 1)
                seterr(m, "实体 %lld 不存在(可能已被删除)", (long long)id);
            else {
                Dsj *e = dsj_obj();
                dsj_set_int(e, "id", (long long)id);
                dsj_set_str(e, "name", obj_name(m, id));
                dsj_set_num(e, "x", obj_world(m, id, m->eng.world_x));
                dsj_set_num(e, "y", obj_world(m, id, m->eng.world_y));
                dsj_set(e, "comps", entity_comps(m, id));
                ret = e;
            }
        }
    } else if (!strcmp(cmd, "entity.add")) ret = cmd_entity_add(m, args);
    else if (!strcmp(cmd, "entity.remove")) ret = cmd_entity_remove(m, args);
    else if (!strcmp(cmd, "entity.set_parent")) ret = cmd_entity_set_parent(m, args);
    else if (!strcmp(cmd, "entity.rename")) ret = cmd_entity_rename(m, args);
    else if (!strcmp(cmd, "entity.set_pos")) ret = cmd_entity_set_pos(m, args);
    else if (!strcmp(cmd, "entity.find")) ret = cmd_entity_find(m, args);
    else if (!strcmp(cmd, "comp.schema")) {
        Dsj *a = dsj_arr();
        int n, c;
        if (!m->eng.ok) seterr(m, "引擎不可用:%s", m->eng.err);
        else {
            n = comp_total(m);
            for (c = 1; c <= n; c++) {
                DexValue a1[2];
                const char *cn;
                Dsj *co, *fields;
                int nf, f;
                a1[0] = V_i(c);
                cn = m->eng.comp_name_at(a1, 1);
                if (!cn || !*cn) continue;
                co = dsj_obj();
                dsj_set_str(co, "name", cn);
                fields = dsj_arr();
                a1[0] = V_s(cn); a1[1] = V_i(0);
                nf = (int)m->eng.field_count(a1, 2);
                for (f = 0; f < nf; f++) {
                    const char *fn;
                    Dsj *fo;
                    int ft;
                    const DsFieldMeta *mt;
                    a1[0] = V_s(cn); a1[1] = V_i(f);
                    fn = m->eng.field_name(a1, 2);
                    if (!fn || !*fn) continue;
                    ft = (int)m->eng.field_type(a1, 2);
                    mt = field_meta(cn, fn);
                    fo = dsj_obj();
                    dsj_set_str(fo, "name", fn);
                    dsj_set_str(fo, "type", field_type_name_of(ft));
                    dsj_set_bool(fo, "persist", m->eng.field_persist(a1, 2) != 0);
                    /* 人类语义(前端按 kind 选控件;没登记的字段按类型码兜底) */
                    dsj_set_str(fo, "label", (mt && mt->label) ? mt->label : fn);
                    dsj_set_str(fo, "kind", (mt && mt->kind) ? mt->kind
                                             : (ft == 3 ? "text" : "number"));
                    if (mt && mt->unit) dsj_set_str(fo, "unit", mt->unit);
                    if (mt && mt->hint) dsj_set_str(fo, "hint", mt->hint);
                    if (mt && mt->items) meta_put_enum(fo, mt->items);
                    if (mt && mt->step > 0) dsj_set_num(fo, "step", mt->step);
                    if (mt && (mt->min || mt->max)) {
                        dsj_set_num(fo, "min", mt->min);
                        dsj_set_num(fo, "max", mt->max);
                    }
                    /* 只读:运行期字段,或引擎暂未实现的字段(面板要禁用并说明) */
                    if ((mt && mt->readonly)
                        || (ft == 0 && m->eng.field_persist(a1, 2) == 0)) {
                        dsj_set_bool(fo, "readonly", 1);
                    }
                    dsj_push(fields, fo);
                }
                dsj_set(co, "fields", fields);
                dsj_push(a, co);
            }
            ret = a;
        }
    } else if (!strcmp(cmd, "comp.add")) ret = cmd_comp_add(m, args);
    else if (!strcmp(cmd, "comp.remove")) ret = cmd_comp_remove(m, args);
    else if (!strcmp(cmd, "comp.set")) ret = cmd_comp_set(m, args);
    else if (!strcmp(cmd, "comp.set_many")) ret = cmd_comp_set_many(m, args);
    else if (!strcmp(cmd, "entity.duplicate")) ret = cmd_entity_duplicate(m, args);
    else if (!strcmp(cmd, "entity.copy")) ret = cmd_entity_copy(m, args);
    else if (!strcmp(cmd, "entity.paste")) ret = cmd_entity_paste(m, args);
    else if (!strcmp(cmd, "view.set")) ret = cmd_view_set(m, args);
    else if (!strcmp(cmd, "scene.render")) ret = cmd_scene_render(m, args);
    else if (!strcmp(cmd, "scene.outline")) ret = cmd_scene_outline(m, args);
    else if (!strcmp(cmd, "tilemap.info")) ret = cmd_tilemap_info(m, args);
    else if (!strcmp(cmd, "tilemap.create")) ret = cmd_tilemap_create(m, args);
    else if (!strcmp(cmd, "tilemap.set")) ret = cmd_tilemap_set(m, args);
    else if (!strcmp(cmd, "tilemap.paint")) ret = cmd_tilemap_paint(m, args);
    else if (!strcmp(cmd, "tilemap.csv")) ret = cmd_tilemap_csv(m, args);
    else if (!strcmp(cmd, "undo")) ret = c_undo(m, args);
    else if (!strcmp(cmd, "redo")) ret = c_redo(m, args);
    else if (!strncmp(cmd, "graph.", 6)) ret = ds_graph_command(m, cmd, args);
    else if (!strncmp(cmd, "blocks.", 7)) ret = ds_blocks_command(m, cmd, args);
    else if (!strncmp(cmd, "res.", 4) || !strncmp(cmd, "autosave.", 9)
             || !strncmp(cmd, "recover.", 8)) ret = ds_res_command(m, cmd, args);
    else if (!strncmp(cmd, "build.", 6) || !strcmp(cmd, "project.scripts") || !strncmp(cmd, "file.", 5)) ret = ds_run_command(m, cmd, args);
    else {
        char msg[256];
        snprintf(msg, sizeof msg, "未知命令 '%s'", cmd);
        seterr(m, msg);
    }

    dsj_free(req);
    if (!ret) return resp_err(m, m->err[0] ? m->err : "命令失败");
    return resp_ok(m, ret);
}

const char *ds_state(DsModel *m) { return ds_command(m, "{\"cmd\":\"app.info\"}"); }

const char *ds_components_json(DsModel *m)
{
    return ds_command(m, "{\"cmd\":\"app.components\"}");
}

/* ------------------------------------------------------------ 生命周期 */

DsModel *ds_model_create(const char *exe_dir)
{
    DsModel *m = xm(sizeof *m);
    DexValue a[2];
    memset(m, 0, sizeof *m);
    if (exe_dir) snprintf(m->exe_dir, sizeof m->exe_dir, "%s", exe_dir);
    ds_engine_load(&m->eng, NULL, m->exe_dir);
    if (m->eng.ok) {
        /* 视口离屏尺寸:引擎的离屏目标不可 resize,所以一次定够大 —— 前端把这张
         * 图按 CSS 缩放显示(平移/缩放改的是编辑器视图覆盖,不是目标尺寸)。 */
        a[0] = V_i(DS_VIEW_W);
        a[1] = V_i(DS_VIEW_H);
        if (m->eng.init_offscreen(a, 2) < 0) {
            snprintf(m->eng.err, sizeof m->eng.err, "引擎离屏初始化失败:%s",
                     ds_engine_last_error(&m->eng));
            m->eng.ok = 0;
        }
    }
    m->view_zoom = 1.0;
    m->view_on = 1;
    /* **编辑器预览不跑物理**。为什么必须显式关掉:预览每刷新一次就 begin/end 一帧,
     * 而引擎默认在 frame_begin 里自动推进物理 —— 于是带 body(motion=dynamic)的实体
     * 在我们没在编辑它们的时候也一直按重力往下掉:实测一个 300 秒不到的项目里
     * 玩家从 y=0 掉到 y=3486 并把那个位置**存进了场景文件**,而视口(相机在 0,0)
     * 什么都看不到 —— 用户视角就是"贴图设了但画面里没有图"。
     * 游戏那边照旧自动推进(真正的物理只在运行的游戏里发生)。 */
    if (m->eng.ok && m->eng.physics_set_auto) {
        DexValue a[1];
        a[0] = V_i(0);
        m->eng.physics_set_auto(a, 1);
    }
    /* 会话标识:进程号 + 启动时刻 + 一个计数器。要求只是"同一次运行里稳定、
     * 换一次运行必然不同",不做密码学意义上的唯一性。 */
    {
        static int seq = 0;
        snprintf(m->session, sizeof m->session, "%lu-%llu-%d",
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long long)GetTickCount64(), ++seq);
    }
    return m;
}

void ds_model_destroy(DsModel *m)
{
    int i;
    if (!m) return;
    for (i = 0; i < m->n_undo; i++) undo_free_contents(&m->undo[i]);
    for (i = 0; i < m->n_redo; i++) undo_free_contents(&m->redo[i]);
    free(m->undo);
    free(m->redo);
    if (m->project) dsj_free(m->project);
    if (m->clipboard) dsj_free(m->clipboard);
    if (m->graph) dsj_free(m->graph);
    if (m->blocks) dsj_free(m->blocks);
    ds_run_kill(m);
    /* 走到这儿说明是正常退出:给本次运行的自动保存盖个 clean 章(崩了就走不到) */
    ds_res_autosave_mark_clean(m);
    if (m->eng.ok) ds_engine_unload(&m->eng);
    free(m->resp);
    free(m);
}

int ds_model_engine_ok(const DsModel *m) { return m && m->eng.ok; }
const char *ds_model_engine_error(const DsModel *m) { return m ? m->eng.err : ""; }
