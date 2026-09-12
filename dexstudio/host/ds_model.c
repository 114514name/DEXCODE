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

const char *ds_version(void) { return DS_VERSION; }

struct DsModel {
    DsEngine eng;
    char exe_dir[1024];
    char root[1200];       /* 当前项目目录(空 = 没打开项目) */
    char scene[1200];      /* 当前场景文件绝对路径 */
    Dsj *project;          /* project.json 的 DOM */
    char **undo; int n_undo;
    char **redo; int n_redo;
    int cap_stack;         /* 两个栈共用容量(同时扩容) */
    int dirty;
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

static void stacks_clear(DsModel *m)
{
    int i;
    for (i = 0; i < m->n_undo; i++) free(m->undo[i]);
    for (i = 0; i < m->n_redo; i++) free(m->redo[i]);
    m->n_undo = m->n_redo = 0;
    m->dirty = 0;
}

static void redo_clear(DsModel *m)
{
    int i;
    for (i = 0; i < m->n_redo; i++) free(m->redo[i]);
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

static void undo_push(DsModel *m, char *snap)
{
    if (!snap) return;
    stack_ensure(m);
    if (m->n_undo == m->cap_stack) {
        if (m->cap_stack >= DS_STACK_MAX) {
            free(m->undo[0]);
            memmove(m->undo, m->undo + 1, (size_t)(m->n_undo - 1) * sizeof *m->undo);
            m->n_undo--;
        } else {
            stack_grow(m);
        }
    }
    m->undo[m->n_undo++] = snap;
    redo_clear(m);
}

/* 撤销/重做:把"当前场景"压到对面的栈,再从本栈弹出快照恢复 */
static int undo_apply(DsModel *m, int is_redo)
{
    char **from = is_redo ? m->redo : m->undo;
    int *nfrom = is_redo ? &m->n_redo : &m->n_undo;
    char ***to = is_redo ? &m->undo : &m->redo;
    int *nto = is_redo ? &m->n_undo : &m->n_redo;
    char *snap, *cur;
    DexValue a[1];

    if (*nfrom == 0) {
        seterr(m, is_redo ? "没有可重做的操作" : "没有可撤销的操作");
        return 0;
    }
    stack_ensure(m);
    if (*nto == m->cap_stack && m->cap_stack < DS_STACK_MAX) stack_grow(m);
    snap = from[*nfrom - 1];
    cur = scene_snapshot(m);
    if (cur) (*to)[(*nto)++] = cur;
    (*nfrom)--;
    a[0] = V_s(snap);
    if (m->eng.scene_load_json(a, 1) != 0) {
        seterr(m, "恢复场景失败:%s", ds_engine_last_error(&m->eng));
        return 0;
    }
    free(snap);
    m->dirty = 1;
    return 1;
}

/* 编辑包装:成功才把快照压栈 */
typedef struct { DsModel *m; char *snap; } Edit;

static Edit edit_begin(DsModel *m)
{
    Edit e;
    e.m = m;
    e.snap = scene_snapshot(m);
    return e;
}

static void edit_end(Edit *e, int changed)
{
    if (changed) {
        undo_push(e->m, e->snap);
        e->m->dirty = 1;
    } else if (e->snap) {
        free(e->snap);
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
    "# DexStudio 生成的入口脚本\n"
    "# 编译:tools/dexc/dexc.exe compile scripts/main.dex\n"
    "include \"dexgame\";\n"
    "include \"dexgame_fast\";\n"
    "\n"
    "func on_start() {\n"
    "    eng_set_clear_color(eng_rgba(30, 30, 46, 255));\n"
    "    eng_scene_load(\"scenes/main.json\");\n"
    "}\n"
    "\n"
    "func on_update() {\n"
    "}\n"
    "\n"
    "func on_draw() {\n"
    "    eng_draw_scene();\n"
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
static Dsj *cmd_comp_set(DsModel *m, Dsj *args)
{
    int64_t id = dsj_get_int(args, "id", 0);
    const char *comp = dsj_get_str(args, "comp", "");
    const char *field = dsj_get_str(args, "field", "");
    Dsj *value = dsj_get(args, "value");
    int ft = -1;
    Edit e;
    if (!m->eng.ok) { seterr(m, "引擎不可用:%s", m->eng.err); return NULL; }
    if (!id || !comp || !*comp || !field || !*field) {
        seterr(m, "comp.set 需要 args.id / args.comp / args.field");
        return NULL;
    }
    if (!value) { seterr(m, "comp.set 需要 args.value"); return NULL; }
    {
        int nf, f;
        DexValue a[2];
        a[0] = V_s(comp); a[1] = V_i(0);
        nf = (int)m->eng.field_count(a, 2);
        for (f = 0; f < nf; f++) {
            const char *fn;
            a[0] = V_s(comp); a[1] = V_i(f);
            fn = m->eng.field_name(a, 2);
            if (fn && !strcmp(fn, field)) { ft = (int)m->eng.field_type(a, 2); break; }
        }
        if (ft < 0) {
            seterr(m, "组件 '%s' 没有字段 '%s'", comp, field);
            return NULL;
        }
    }
    e = edit_begin(m);
    {
        DexValue a[4];
        int64_t rc;
        a[0] = V_i(id); a[1] = V_s(comp); a[2] = V_s(field);
        if (ft == 1) {
            double d = value->t == DSJ_NUM ? value->num : 0.0;
            a[3] = V_f(d);
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
            seterr(m, "设置 '%s.%s' 失败:%s", comp, field,
                   ds_engine_last_error(&m->eng));
            edit_end(&e, 0);
            return NULL;
        }
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
    else if (!strcmp(cmd, "undo")) ret = c_undo(m, args);
    else if (!strcmp(cmd, "redo")) ret = c_redo(m, args);
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
        a[0] = V_i(64);
        a[1] = V_i(64);
        if (m->eng.init_offscreen(a, 2) < 0) {
            snprintf(m->eng.err, sizeof m->eng.err, "引擎离屏初始化失败:%s",
                     ds_engine_last_error(&m->eng));
            m->eng.ok = 0;
        }
    }
    return m;
}

void ds_model_destroy(DsModel *m)
{
    int i;
    if (!m) return;
    for (i = 0; i < m->n_undo; i++) free(m->undo[i]);
    for (i = 0; i < m->n_redo; i++) free(m->redo[i]);
    free(m->undo);
    free(m->redo);
    if (m->project) dsj_free(m->project);
    if (m->eng.ok) ds_engine_unload(&m->eng);
    free(m->resp);
    free(m);
}

int ds_model_engine_ok(const DsModel *m) { return m && m->eng.ok; }
const char *ds_model_engine_error(const DsModel *m) { return m ? m->eng.err : ""; }
