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
    /* 编辑器视图(视口平移/缩放)。on=0 表示"用场景里的相机" */
    int view_on;
    double view_x, view_y, view_zoom;
    int render_seq;        /* 每次渲染 +1,前端用它做 ?t= 破缓存 */
    Dsj *clipboard;        /* 实体剪贴板(entity.copy/paste),数组 */
    Dsj *graph;            /* 逻辑图的 DOM(scripts/logic.json,B4) */
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
#if defined(_WIN32)
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    return access(p, 0) == 0;
#endif
}

static int ensure_dir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path) == 0 || path_exists(path);
#else
    return mkdir(path, 0777) == 0 || path_exists(path);
#endif
}

static char *read_text(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
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
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 1;
}

/* 文件小工具的实现(ds_graph.c 也用同一份,避免两份行为不一致) */
int ds_mkdir(const char *path) { return ensure_dir(path); }
int ds_write_text(const char *path, const char *text) { return write_text(path, text); }
char *ds_read_text(const char *path, size_t *out_len) { return read_text(path, out_len); }

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

/* 把一份快照写回:场景 JSON + 各外部文本文件(瓦片 CSV 写完让引擎重新加载) */
static int restore_entry(DsModel *m, UndoEntry *e)
{
    DexValue a[1];
    int i;
    a[0] = V_s(e->scene ? e->scene : "{}");
    if (m->eng.scene_load_json(a, 1) != 0) {
        seterr(m, "恢复场景失败:%s", ds_engine_last_error(&m->eng));
        return 0;
    }
    for (i = 0; i < dsj_len(e->files); i++) {
        const char *path = e->files->keys[i];
        const char *text = dsj_at(e->files, i)->str;
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
    m->dirty = 1;
    return 1;
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
    "#   · 逻辑图生成的代码在 scripts/logic.dex(别手改那个,改图)\n"
    "#   · 编译:tools/dexc/dexc.exe compile scripts/main.dex\n"
    "include \"dexgame\";\n"
    "include \"dexgame_fast\";\n"
    "include \"logic\";\n"
    "\n"
    "func on_start() {\n"
    "    eng_set_clear_color(eng_rgba(30, 30, 46, 255));\n"
    "    eng_scene_load(\"scenes/main.json\");\n"
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
    "eng_run(\"on_start\", \"on_update\", \"on_draw\");\n";

static Dsj *project_default(const char *name)
{
    Dsj *p = dsj_obj();
    Dsj *scripts = dsj_arr();
    Dsj *run = dsj_obj();
    dsj_set_str(p, "name", name);
    dsj_set_int(p, "format", 1);
    dsj_set_str(p, "start_scene", "scenes/main.json");
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
        FILE *f = fopen(path, "ab");
        if (f) { fputc('\n', f); fclose(f); }
    }
    free(txt);
    free(path);
    if (!ok) { seterr(m, "无法写入 project.json"); return 0; }
    if (m->scene[0] && !scene_save_to(m, m->scene)) return 0;
    m->dirty = 0;
    return 1;
}

/* 切换当前场景到 <root>/<rel>;rel 空则用 project.start_scene */
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
    free(full);
    stacks_clear(m);
    return 1;
}

/* ------------------------------------------------------------ 命令实现 */

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
    /* 编辑器视口状态(前端刷新后据此恢复) */
    {
        Dsj *v = dsj_obj();
        dsj_set_bool(v, "on", m->view_on);
        dsj_set_num(v, "x", m->view_x);
        dsj_set_num(v, "y", m->view_y);
        dsj_set_num(v, "zoom", m->view_zoom);
        dsj_set(r, "view", v);
    }
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
    /* 立刻落盘:新建完的项目必须**马上就能重新打开**(否则"新建"只是个半成品)。
     * 顺带写出空的起始场景。 */
    if (!project_save(m)) return NULL;
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
    if (m->project) dsj_free(m->project);
    m->project = dom;
    m->scene[0] = 0;
    ds_graph_reload(m);
    if (!scene_switch(m, NULL, 1)) return NULL;
    r = dsj_obj();
    dsj_set_str(r, "root", m->root);
    dsj_set_str(r, "scene", m->scene);
    return r;
}

static Dsj *cmd_scene_list(DsModel *m)
{
    Dsj *a = dsj_arr();
    char *dir;
    if (!m->root[0]) {
        seterr(m, "还没有打开项目");
        return NULL;
    }
    dir = pjoin(m->root, "scenes");
#if defined(_WIN32)
    {
        char pat[1400];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*.json", dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                dsj_push(a, dsj_str(fd.cFileName));
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#endif
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
    edit_end(&e, 1);
    {
        Dsj *r = dsj_obj();
        dsj_set_bool(r, "removed", 1);
        return r;
    }
}

static Dsj *cmd_entity_rename(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *name = dsj_get_str(args, "name", "");
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id || !name) { seterr(m, "entity.rename 需要 args.id 与 args.name"); return NULL; }
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
    edit_end(&e, 1);
    {
        Dsj *r = dsj_obj();
        dsj_set_str(r, "name", obj_name(m, id));
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

static Dsj *cmd_comp_set(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *comp = dsj_get_str(args, "comp", "");
    const char *field = dsj_get_str(args, "field", "");
    Dsj *value = dsj_get(args, "value");
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

static Dsj *cmd_scene_new(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "scene");
    char rel[512];
    DexValue none[1];
    Dsj *r;
    if (!m->root[0]) { seterr(m, "还没有打开项目"); return NULL; }
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
    else if (m->exe_dir[0])
        snprintf(dir, sizeof dir, "%s\\..\\..\\_zigtmp\\ds_preview", m->exe_dir);
    else
        snprintf(dir, sizeof dir, ".dexstudio");
    ensure_dir(dir);
    return dir;
}

const char *ds_project_dir(DsModel *m) { return m->root; }

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
        a[0] = V_i(i);
        id = m->eng.object_id_at(a, 1);
        if (!id) { dsj_free(o); continue; }
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
            if (sw <= 0) sw = (double)m->eng.tex_width((DexValue[]){V_i(DSI("sprite", "texture"))}, 1);
            if (sh <= 0) sh = (double)m->eng.tex_height((DexValue[]){V_i(DSI("sprite", "texture"))}, 1);
            if (sw <= 0) sw = 32;
            if (sh <= 0) sh = 32;
            bx = wx + px; by = wy + py; bw = sw; bh = sh;
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
        dsj_set_num(o, "x", bx);
        dsj_set_num(o, "y", by);
        dsj_set_num(o, "w", bw);
        dsj_set_num(o, "h", bh);
        dsj_set_num(o, "wx", wx);
        dsj_set_num(o, "wy", wy);
        dsj_set_str(o, "kind", kind);
        dsj_set_int(o, "layer", layer);
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
    char fx[32], fy[32], fz[32];
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
    snprintf(fx, sizeof fx, "%.4f", m->view_x);
    snprintf(fy, sizeof fy, "%.4f", m->view_y);
    snprintf(fz, sizeof fz, "%.4f", m->view_zoom);
    r = dsj_obj();
    dsj_set_str(r, "url", "https://dexstudio-preview.local/preview.bmp");
    dsj_set_int(r, "seq", m->render_seq);
    dsj_set_str(r, "path", path);
    dsj_set_bool(r, "view_on", m->view_on);
    dsj_set_num(r, "x", m->view_x);
    dsj_set_num(r, "y", m->view_y);
    dsj_set_num(r, "zoom", m->view_zoom);
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
    else if (!strcmp(cmd, "project.state")) {
        Dsj *r = dsj_obj();
        dsj_set_str(r, "root", m->root);
        dsj_set_str(r, "scene", m->scene);
        dsj_set_bool(r, "dirty", m->dirty);
        dsj_set_bool(r, "has_project", m->project != NULL);
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
                    a1[0] = V_s(cn); a1[1] = V_i(f);
                    fn = m->eng.field_name(a1, 2);
                    if (!fn || !*fn) continue;
                    fo = dsj_obj();
                    dsj_set_str(fo, "name", fn);
                    dsj_set_str(fo, "type",
                                field_type_name_of((int)m->eng.field_type(a1, 2)));
                    dsj_set_bool(fo, "persist", m->eng.field_persist(a1, 2) != 0);
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
    if (m->eng.ok) ds_engine_unload(&m->eng);
    free(m->resp);
    free(m);
}

int ds_model_engine_ok(const DsModel *m) { return m && m->eng.ok; }
const char *ds_model_engine_error(const DsModel *m) { return m ? m->eng.err : ""; }
