/* ============================================================================
 * ds_blocks.c — 积木脚本的模型 + 校验 + 代码生成(见 ds_blocks.h)。
 *
 * 设计原则(与 ds_graph.c 一致,但**刻意更简单**):
 *   1. 积木目录在 C 里定义一次,`blocks.types` 交给前端 —— 前端不写死任何句式;
 *   2. 每个空都是**下拉**:能用预设表达的绝不让用户打字(数值也给预设 + "自定义");
 *   3. 生成前校验:引用不存在的实体只**警告**,缺必需项(比如没选声音)是**错误**并拦住;
 *   4. 同一份积木永远生成同一份代码(脚本按 id、积木按顺序)。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "ds_blocks.h"
#include "ds_json.h"
#include "ds_utf8.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ 目录 */

typedef struct { const char *value; const char *label; } BlkOpt;

static const BlkOpt OPT_KEY[] = {
    {"32", "空格"}, {"38", "↑ 上"}, {"40", "↓ 下"}, {"37", "← 左"}, {"39", "→ 右"},
    {"13", "回车"}, {"27", "Esc"}, {"65", "A"}, {"68", "D"}, {"87", "W"}, {"83", "S"},
    {"256", "鼠标左键"}, {NULL, NULL}
};
static const BlkOpt OPT_ACTION[] = {
    {"left", "向左"}, {"right", "向右"}, {"up", "向上"}, {"down", "向下"},
    {"jump", "跳"}, {"action", "动作键"}, {"back", "返回"}, {NULL, NULL}
};
static const BlkOpt OPT_MOUSE[] = {
    {"256", "左键"}, {"257", "右键"}, {"258", "中键"}, {NULL, NULL}
};
static const BlkOpt OPT_DIR[] = {
    {"right", "右"}, {"left", "左"}, {"up", "上"}, {"down", "下"}, {NULL, NULL}
};
static const BlkOpt OPT_SPEED[] = {
    {"slow", "慢(每秒 60)"}, {"mid", "中(每秒 150)"}, {"fast", "快(每秒 300)"},
    {NULL, NULL}
};
static const BlkOpt OPT_FORCE[] = {
    {"small", "小"}, {"mid", "中"}, {"big", "大"}, {NULL, NULL}
};
static const BlkOpt OPT_SIZE[] = {
    {"0.25", "1/4 大小"}, {"0.5", "1/2 大小"}, {"1", "原大小"}, {"2", "2 倍"},
    {"4", "4 倍"}, {NULL, NULL}
};
static const BlkOpt OPT_FIELD[] = {
    {"transform.x", "左右位置"}, {"transform.y", "上下位置"},
    {"transform.sx", "大小"}, {"body.vx", "横向速度"}, {"body.vy", "纵向速度"},
    {"collider.hw", "碰撞半宽"}, {NULL, NULL}
};
static const BlkOpt OPT_CMP[] = {
    {"gt", "大于"}, {"lt", "小于"}, {"eq", "等于"}, {NULL, NULL}
};
static const BlkOpt OPT_NUM[] = {
    {"0", "0"}, {"10", "10"}, {"30", "30"}, {"50", "50"}, {"100", "100"},
    {"150", "150"}, {"200", "200"}, {"300", "300"}, {"500", "500"}, {"1000", "1000"},
    {"-50", "-50"}, {"-100", "-100"}, {NULL, NULL}
};

typedef struct {
    const char *name;    /* JSON 里的属性名 */
    const char *kind;    /* entity/key/action/mouse/dir/speed/force/size/field/cmp/num/audio */
    const BlkOpt *opts;  /* 下拉候选(kind=entity/audio 时由运行时给) */
    const char *dflt;    /* 默认值 */
} BlkSlot;

typedef struct {
    const char *type;
    const char *cat;     /* 事件 / 动作 / 如果 */
    const char *text;    /* 句式:{0} {1} … 对应 slots 的顺序 */
    int body;            /* 1 = C 形积木(有身子,里面能再放动作) */
    const BlkSlot *slots;
} BlkType;

/* --- 事件(帽子积木)--- */
static const BlkSlot SL_KEY[] = {{"key", "key", OPT_KEY, "32"}, {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_ACTION[] = {{"action", "action", OPT_ACTION, "jump"},
                                    {NULL, NULL, NULL, NULL}};
/* --- 动作 --- */
static const BlkSlot SL_MOVE[] = {{"obj", "entity", NULL, ""},
                                  {"dir", "dir", OPT_DIR, "right"},
                                  {"speed", "speed", OPT_SPEED, "mid"},
                                  {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_SETPOS[] = {{"obj", "entity", NULL, ""},
                                    {"field", "field", OPT_FIELD, "transform.x"},
                                    {"value", "num", OPT_NUM, "100"},
                                    {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_JUMP[] = {{"obj", "entity", NULL, ""},
                                  {"force", "force", OPT_FORCE, "mid"},
                                  {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_SIZE[] = {{"obj", "entity", NULL, ""},
                                  {"size", "size", OPT_SIZE, "1"},
                                  {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_CAM[] = {{"cam", "entity", NULL, ""},
                                 {"obj", "entity", NULL, ""},
                                 {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_SOUND[] = {{"path", "audio", NULL, ""}, {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_ONLY_OBJ[] = {{"obj", "entity", NULL, ""},
                                      {NULL, NULL, NULL, NULL}};
/* --- 如果(C 形)--- */
static const BlkSlot SL_IF_GROUND[] = {{"obj", "entity", NULL, ""},
                                       {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_IF_KEY[] = {{"key", "key", OPT_KEY, "32"},
                                    {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_IF_ACTION[] = {{"action", "action", OPT_ACTION, "jump"},
                                       {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_IF_MOUSE[] = {{"btn", "mouse", OPT_MOUSE, "256"},
                                      {NULL, NULL, NULL, NULL}};
static const BlkSlot SL_IF_CMP[] = {{"obj", "entity", NULL, ""},
                                    {"field", "field", OPT_FIELD, "transform.x"},
                                    {"cmp", "cmp", OPT_CMP, "gt"},
                                    {"value", "num", OPT_NUM, "100"},
                                    {NULL, NULL, NULL, NULL}};

static const BlkType BLK_TYPES[] = {
    /* 事件:一段脚本的第一块 */
    {"on_start", "事件", "当游戏开始", 0, NULL},
    {"on_update", "事件", "每一帧", 0, NULL},
    {"on_key", "事件", "当按下 {0}", 0, SL_KEY},
    {"on_action", "事件", "当按下动作 {0}", 0, SL_ACTION},
    /* 动作 */
    {"move", "动作", "让 {0} 往 {1} 走,速度 {2}", 0, SL_MOVE},
    {"set_pos", "动作", "把 {0} 的 {1} 设为 {2}", 0, SL_SETPOS},
    {"jump", "动作", "让 {0} 跳一下,力度 {1}", 0, SL_JUMP},
    {"set_size", "动作", "把 {0} 的大小设为 {1}", 0, SL_SIZE},
    {"camera_follow", "动作", "让相机 {0} 跟着 {1}", 0, SL_CAM},
    {"play_sound", "动作", "播放声音 {0}", 0, SL_SOUND},
    {"destroy", "动作", "删掉 {0}", 0, SL_ONLY_OBJ},
    /* 如果(C 形:身子里还能放动作) */
    {"if_ground", "如果", "如果 {0} 站在地面上", 1, SL_IF_GROUND},
    {"if_key", "如果", "如果按着 {0}", 1, SL_IF_KEY},
    {"if_action", "如果", "如果按着动作 {0}", 1, SL_IF_ACTION},
    {"if_mouse", "如果", "如果按着鼠标 {0}", 1, SL_IF_MOUSE},
    {"if_field", "如果", "如果 {0} 的 {1} {2} {3}", 1, SL_IF_CMP},
};
#define BLK_COUNT ((int)(sizeof BLK_TYPES / sizeof BLK_TYPES[0]))

static const BlkType *blk_def(const char *type)
{
    int i;
    if (!type) return NULL;
    for (i = 0; i < BLK_COUNT; i++)
        if (!strcmp(BLK_TYPES[i].type, type)) return &BLK_TYPES[i];
    return NULL;
}

static const BlkSlot *blk_slot(const BlkType *d, const char *name)
{
    int i;
    if (!d || !d->slots) return NULL;
    for (i = 0; d->slots[i].name; i++)
        if (!strcmp(d->slots[i].name, name)) return &d->slots[i];
    return NULL;
}

/* ------------------------------------------------------------ 小工具 */

static void berr(DsModel *m, const char *fmt, ...)
{
    va_list ap;
    char *buf = ds_model_errbuf(m);
    size_t n = ds_model_errbuf_size();
    va_start(ap, fmt);
    vsnprintf(buf, n, fmt, ap);
    va_end(ap);
}

static Dsj *blk_scripts(DsModel *m)
{
    Dsj *g = ds_model_blocks(m);
    Dsj *a = dsj_get(g, "scripts");
    if (!a || a->t != DSJ_ARR) {
        a = dsj_arr();
        dsj_set(g, "scripts", a);
    }
    return a;
}

static Dsj *script_by_id(DsModel *m, long long id)
{
    Dsj *a = blk_scripts(m);
    int i;
    for (i = 0; i < dsj_len(a); i++) {
        Dsj *s = dsj_at(a, i);
        if (s && dsj_get_int(s, "id", 0) == id) return s;
    }
    return NULL;
}

static Dsj *script_blocks(Dsj *s)
{
    Dsj *a = dsj_get(s, "blocks");
    if (!a || a->t != DSJ_ARR) {
        a = dsj_arr();
        dsj_set(s, "blocks", a);
    }
    return a;
}

static long long next_script_id(DsModel *m)
{
    Dsj *a = blk_scripts(m);
    long long mx = 0;
    int i;
    for (i = 0; i < dsj_len(a); i++) {
        long long id = dsj_get_int(dsj_at(a, i), "id", 0);
        if (id > mx) mx = id;
    }
    return mx + 1;
}

/* 场景里第一个实体名(给默认值用);没有就空串。
 * want_camera=1 时优先挑**带相机组件**的实体;=0 时**跳过相机**
 * —— 否则"让 {实体} 往右走"会默认指到相机上(实测踩到:模板给相机写了移动)。 */
static void first_entity(DsModel *m, char *out, size_t outsz, int want_camera)
{
    Dsj *opt = ds_model_scene_options(m);
    Dsj *ents = dsj_get(opt, "entities");
    int i;
    out[0] = 0;
    for (i = 0; i < dsj_len(ents); i++) {
        Dsj *e = dsj_at(ents, i);
        const char *nm = dsj_get_str(e, "name", "");
        Dsj *comps = dsj_get(e, "comps");
        int is_cam = (comps && dsj_get(comps, "camera")) ? 1 : 0;
        if (!nm || !*nm) continue;
        if (want_camera) {
            if (is_cam) { snprintf(out, outsz, "%s", nm); break; }
        } else {
            if (is_cam) continue;
            snprintf(out, outsz, "%s", nm);
            break;
        }
    }
    if (!out[0]) {                       /* 兜底:场景里只有相机(或什么都没有) */
        for (i = 0; i < dsj_len(ents); i++) {
            const char *nm = dsj_get_str(dsj_at(ents, i), "name", "");
            if (nm && *nm) { snprintf(out, outsz, "%s", nm); break; }
        }
    }
    dsj_free(opt);
}

static void first_audio(DsModel *m, char *out, size_t outsz)
{
    Dsj *opt = ds_model_scene_options(m);
    Dsj *a = dsj_get(opt, "audios");
    out[0] = 0;
    if (dsj_len(a) > 0 && dsj_at(a, 0) && dsj_at(a, 0)->str)
        snprintf(out, outsz, "%s", dsj_at(a, 0)->str);
    dsj_free(opt);
}

/* 给一个积木填默认值(前端不用管"缺字段") */
static void block_defaults(DsModel *m, Dsj *b)
{
    const BlkType *d = blk_def(dsj_get_str(b, "type", ""));
    Dsj *p = dsj_get(b, "props");
    int i;
    char obj[160], cam[160], snd[1024];
    if (!d) return;
    if (!p || p->t != DSJ_OBJ) { p = dsj_obj(); dsj_set(b, "props", p); }
    first_entity(m, obj, sizeof obj, 0);
    first_entity(m, cam, sizeof cam, 1);
    first_audio(m, snd, sizeof snd);
    for (i = 0; d->slots && d->slots[i].name; i++) {
        const BlkSlot *s = &d->slots[i];
        if (dsj_get(p, s->name)) continue;
        if (!strcmp(s->kind, "entity")) {
            int want_cam = !strcmp(s->name, "cam") || !strcmp(s->name, "obj")
                           ? (!strcmp(s->name, "cam")) : 0;
            dsj_set_str(p, s->name, want_cam ? cam : obj);
        } else if (!strcmp(s->kind, "audio")) {
            dsj_set_str(p, s->name, snd);
        } else {
            dsj_set_str(p, s->name, s->dflt ? s->dflt : "");
        }
    }
    if (d->body && !dsj_get(b, "body")) dsj_set(b, "body", dsj_arr());
}

/* ------------------------------------------------------------ 存取 */

static Dsj *empty_blocks(void)
{
    Dsj *g = dsj_obj();
    dsj_set_int(g, "format", 1);
    dsj_set(g, "scripts", dsj_arr());
    return g;
}

void ds_blocks_reload(DsModel *m)
{
    const char *root = ds_project_dir(m);
    char path[1400];
    char *txt;
    Dsj *dom = NULL;
    if (!root || !*root) { ds_model_set_blocks(m, NULL); return; }
    snprintf(path, sizeof path, "%s\\" DS_BLOCKS_REL, root);
    {
        int i;
        for (i = 0; path[i]; i++) if (path[i] == '/') path[i] = '\\';
    }
    txt = ds_read_text(path, NULL);
    if (txt) {
        char perr[256];
        dom = dsj_parse(txt, perr, sizeof perr);
        free(txt);
        if (dom && (dom->t != DSJ_OBJ || !dsj_get(dom, "scripts"))) {
            dsj_free(dom);          /* 坏数据当空,别把编辑器的状态带坏 */
            dom = NULL;
        }
    }
    ds_model_set_blocks(m, dom ? dom : empty_blocks());
}

const char *ds_blocks_json(DsModel *m)
{
    static char *cache = NULL;
    char *txt = dsj_dump(ds_model_blocks(m));
    free(cache);
    cache = txt;
    return cache ? cache : "{}";
}

static int blocks_write(DsModel *m, char *err, unsigned errsz)
{
    const char *root = ds_project_dir(m);
    char path[1400];
    char *txt;
    int ok;
    if (!root || !*root) {
        if (err) snprintf(err, errsz, "还没有打开项目,积木没有地方存");
        return 0;
    }
    snprintf(path, sizeof path, "%s\\" DS_BLOCKS_REL, root);
    {
        size_t i;
        for (i = 0; path[i]; i++) if (path[i] == '/') path[i] = '\\';
    }
    /* scripts/ 可能不存在(老项目) */
    {
        char dir[1400];
        snprintf(dir, sizeof dir, "%s", path);
        {
            char *slash = strrchr(dir, '\\');
            if (slash) { *slash = 0; ds_mkdir(dir); }
        }
    }
    txt = dsj_dump(ds_model_blocks(m));
    if (!txt) {
        if (err) snprintf(err, errsz, "序列化积木失败");
        return 0;
    }
    ok = ds_write_text(path, txt);
    if (!ok && err) snprintf(err, errsz, "无法写入 %s", path);
    free(txt);
    return ok;
}

int ds_blocks_save(DsModel *m) { return blocks_write(m, NULL, 0); }

/* ------------------------------------------------------------ 目录 → JSON */

static Dsj *opts_json(const BlkOpt *o)
{
    Dsj *a = dsj_arr();
    int i;
    for (i = 0; o && o[i].value; i++) {
        Dsj *it = dsj_obj();
        dsj_set_str(it, "value", o[i].value);
        dsj_set_str(it, "label", o[i].label);
        dsj_push(a, it);
    }
    return a;
}

static Dsj *slot_json(DsModel *m, const BlkSlot *s)
{
    Dsj *o = dsj_obj();
    dsj_set_str(o, "name", s->name);
    dsj_set_str(o, "kind", s->kind);
    dsj_set_str(o, "value", s->dflt ? s->dflt : "");
    if (!strcmp(s->kind, "entity")) {
        Dsj *opt = ds_model_scene_options(m);
        Dsj *ents = dsj_get(opt, "entities");
        Dsj *a = dsj_arr();
        int i;
        for (i = 0; i < dsj_len(ents); i++) {
            Dsj *it = dsj_obj();
            dsj_set_str(it, "value", dsj_get_str(dsj_at(ents, i), "name", ""));
            dsj_set_str(it, "label", dsj_get_str(dsj_at(ents, i), "name", ""));
            dsj_push(a, it);
        }
        dsj_set(o, "options", a);
        dsj_free(opt);
    } else if (!strcmp(s->kind, "audio")) {
        Dsj *opt = ds_model_scene_options(m);
        Dsj *auds = dsj_get(opt, "audios");
        Dsj *a = dsj_arr();
        int i;
        for (i = 0; i < dsj_len(auds); i++) {
            const char *p = dsj_at(auds, i) && dsj_at(auds, i)->str
                            ? dsj_at(auds, i)->str : "";
            Dsj *it = dsj_obj();
            dsj_set_str(it, "value", p);
            dsj_set_str(it, "label", p);
            dsj_push(a, it);
        }
        dsj_set(o, "options", a);
        dsj_free(opt);
    } else if (s->opts) {
        Dsj *a = opts_json(s->opts);
        /* 数值槽多给一个"自定义"入口(前端会换成数字输入框) */
        if (!strcmp(s->kind, "num")) {
            Dsj *it = dsj_obj();
            dsj_set_str(it, "value", "__custom__");
            dsj_set_str(it, "label", "自定义…");
            dsj_push(a, it);
        }
        dsj_set(o, "options", a);
    }
    return o;
}

static Dsj *cmd_types(DsModel *m)
{
    Dsj *a = dsj_arr();
    int i;
    for (i = 0; i < BLK_COUNT; i++) {
        const BlkType *d = &BLK_TYPES[i];
        Dsj *o = dsj_obj();
        Dsj *slots = dsj_arr();
        int k;
        dsj_set_str(o, "type", d->type);
        dsj_set_str(o, "cat", d->cat);
        dsj_set_str(o, "text", d->text);
        dsj_set_bool(o, "body", d->body);
        for (k = 0; d->slots && d->slots[k].name; k++)
            dsj_push(slots, slot_json(m, &d->slots[k]));
        dsj_set(o, "slots", slots);
        dsj_push(a, o);
    }
    return a;
}

/* ------------------------------------------------------------ 命令 */

static Dsj *cmd_info(DsModel *m)
{
    Dsj *r = dsj_obj();
    Dsj *g = ds_model_blocks(m);
    const char *root = ds_project_dir(m);
    char path[1400] = "";
    dsj_set_int(r, "format", (long long)dsj_get_int(g, "format", 1));
    dsj_set(r, "scripts", dsj_clone(blk_scripts(m)));
    dsj_set_int(r, "count", dsj_len(blk_scripts(m)));
    if (root && *root) {
        snprintf(path, sizeof path, "%s\\%s", root, DS_BLOCKS_REL);
        dsj_set_str(r, "path", path);
    } else {
        dsj_set_str(r, "path", "");
    }
    return r;
}

static Dsj *cmd_save(DsModel *m)
{
    char err[256];
    Dsj *r;
    if (!blocks_write(m, err, sizeof err)) { berr(m, "%s", err); return NULL; }
    r = dsj_obj();
    dsj_set_bool(r, "saved", 1);
    return r;
}

/* 新增一段脚本:blocks.script.add {event:"update"} */
static Dsj *cmd_script_add(DsModel *m, Dsj *args)
{
    const char *event = dsj_get_str(args, "event", "on_update");
    long long id;
    Dsj *s, *r;
    void *tok = ds_model_edit_open(m);
    if (!blk_def(event)) {
        ds_model_edit_close(m, tok, 0);
        berr(m, "没有这种事件:'%s'", event);
        return NULL;
    }
    id = next_script_id(m);
    s = dsj_obj();
    dsj_set_int(s, "id", id);
    dsj_set_str(s, "event", event);
    dsj_set(s, "blocks", dsj_arr());
    /* 事件自带的属性(按键/动作)也按目录填好 */
    {
        Dsj *ev = dsj_obj();
        const BlkType *d = blk_def(event);
        int i;
        for (i = 0; d->slots && d->slots[i].name; i++)
            dsj_set_str(ev, d->slots[i].name, d->slots[i].dflt ? d->slots[i].dflt : "");
        dsj_set(s, "props", ev);
    }
    dsj_push(blk_scripts(m), s);
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_int(r, "id", id);
    dsj_set_str(r, "event", event);
    return r;
}

static Dsj *cmd_script_remove(DsModel *m, Dsj *args)
{
    long long id = dsj_get_int(args, "id", 0);
    Dsj *a = blk_scripts(m), *r;
    void *tok;
    int i, removed = 0;
    for (i = 0; i < dsj_len(a); i++) {
        if (dsj_get_int(dsj_at(a, i), "id", 0) == id) {
            tok = ds_model_edit_open(m);
            dsj_free(dsj_at(a, i));
            memmove(a->items + i, a->items + i + 1,
                    (size_t)(dsj_len(a) - i - 1) * sizeof *a->items);
            a->n--;
            ds_model_edit_close(m, tok, 1);
            removed = 1;
            break;
        }
    }
    if (!removed) { berr(m, "没有这段脚本(#%lld)", (long long)id); return NULL; }
    r = dsj_obj();
    dsj_set_bool(r, "removed", 1);
    return r;
}

/* 在一段脚本(或某个 if 的身子里)插入一块积木 */
static Dsj *insert_target(DsModel *m, long long script_id, long long in_block,
                          Dsj **out_script)
{
    Dsj *s = script_by_id(m, script_id);
    if (!s) { berr(m, "没有这段脚本(#%lld)", (long long)script_id); return NULL; }
    if (out_script) *out_script = s;
    if (!in_block) return script_blocks(s);
    {
        Dsj *blocks = script_blocks(s);
        int i;
        for (i = 0; i < dsj_len(blocks); i++) {
            Dsj *b = dsj_at(blocks, i);
            if (dsj_get_int(b, "block_id", 0) == in_block) {
                Dsj *body = dsj_get(b, "body");
                if (!body || body->t != DSJ_ARR) { body = dsj_arr(); dsj_set(b, "body", body); }
                return body;
            }
        }
    }
    berr(m, "找不到那个「如果」积木(#%lld)", (long long)in_block);
    return NULL;
}

/* 全文档里最大的 block_id(要**递归进 if 的肚子**;而且要在 scripts 层下面找 ——
 * 早先直接扫 scripts 数组,结果每块新积木的 id 都是 1)。 */
static long long max_block_id(Dsj *blocks)
{
    long long mx = 0;
    int i;
    if (!blocks || blocks->t != DSJ_ARR) return 0;
    for (i = 0; i < dsj_len(blocks); i++) {
        Dsj *b = dsj_at(blocks, i);
        long long id;
        if (!b || b->t != DSJ_OBJ) continue;
        id = dsj_get_int(b, "block_id", 0);
        if (id > mx) mx = id;
        id = max_block_id(dsj_get(b, "body"));
        if (id > mx) mx = id;
    }
    return mx;
}

static long long next_block_id(DsModel *m)
{
    Dsj *scripts = blk_scripts(m);
    long long mx = 0;
    int i;
    for (i = 0; i < dsj_len(scripts); i++) {
        long long id = max_block_id(dsj_get(dsj_at(scripts, i), "blocks"));
        if (id > mx) mx = id;
    }
    return mx + 1;
}

static Dsj *cmd_block_add(DsModel *m, Dsj *args)
{
    long long sid = dsj_get_int(args, "id", 0);
    long long in_block = dsj_get_int(args, "in_block", 0);
    const char *type = dsj_get_str(args, "type", "");
    int index = (int)dsj_get_int(args, "index", -1);
    const BlkType *d = blk_def(type);
    Dsj *target, *b, *r;
    void *tok;
    if (!d) { berr(m, "没有这种积木:'%s'", type); return NULL; }
    if (!sid) { berr(m, "blocks.add 需要 args.id(哪一段脚本)"); return NULL; }
    tok = ds_model_edit_open(m);
    target = insert_target(m, sid, in_block, NULL);
    if (!target) { ds_model_edit_close(m, tok, 0); return NULL; }
    b = dsj_obj();
    dsj_set_int(b, "block_id", next_block_id(m));
    dsj_set_str(b, "type", type);
    dsj_set(b, "props", dsj_obj());
    if (d->body) dsj_set(b, "body", dsj_arr());
    block_defaults(m, b);
    if (index < 0 || index > dsj_len(target)) {
        dsj_push(target, b);
    } else {
        dsj_push(target, NULL);            /* 先占位再插 */
        dsj_free(dsj_at(target, dsj_len(target) - 1));
        target->n--;
        {
            int i;
            dsj_push(target, dsj_obj());
            for (i = dsj_len(target) - 1; i > index; i--)
                target->items[i] = target->items[i - 1];
            target->items[index] = b;
        }
    }
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_int(r, "block_id", dsj_get_int(b, "block_id", 0));
    dsj_set_str(r, "type", type);
    dsj_set(r, "props", dsj_clone(dsj_get(b, "props")));
    return r;
}

/* 找到一个积木(含 if 身子里的一层)以及它所在的数组 */
static Dsj *find_block(Dsj *blocks, long long bid, Dsj **out_arr, int *out_index)
{
    int i;
    for (i = 0; i < dsj_len(blocks); i++) {
        Dsj *b = dsj_at(blocks, i);
        Dsj *body;
        int k;
        if (dsj_get_int(b, "block_id", 0) == bid) {
            if (out_arr) *out_arr = blocks;
            if (out_index) *out_index = i;
            return b;
        }
        body = dsj_get(b, "body");
        for (k = 0; body && k < dsj_len(body); k++) {
            Dsj *c = dsj_at(body, k);
            if (dsj_get_int(c, "block_id", 0) == bid) {
                if (out_arr) *out_arr = body;
                if (out_index) *out_index = k;
                return c;
            }
        }
    }
    return NULL;
}

static Dsj *cmd_block_remove(DsModel *m, Dsj *args)
{
    long long sid = dsj_get_int(args, "id", 0);
    long long bid = dsj_get_int(args, "block_id", 0);
    Dsj *s = script_by_id(m, sid), *arr = NULL, *r;
    int idx = -1;
    void *tok;
    if (!s) { berr(m, "没有这段脚本(#%lld)", (long long)sid); return NULL; }
    if (!find_block(script_blocks(s), bid, &arr, &idx) || !arr) {
        berr(m, "找不到那块积木(#%lld)", (long long)bid);
        return NULL;
    }
    tok = ds_model_edit_open(m);
    dsj_free(dsj_at(arr, idx));
    memmove(arr->items + idx, arr->items + idx + 1,
            (size_t)(dsj_len(arr) - idx - 1) * sizeof *arr->items);
    arr->n--;
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_bool(r, "removed", 1);
    return r;
}

/* 上移/下移:dir = -1 / +1 */
static Dsj *cmd_block_move(DsModel *m, Dsj *args)
{
    long long sid = dsj_get_int(args, "id", 0);
    long long bid = dsj_get_int(args, "block_id", 0);
    int dir = (int)dsj_get_int(args, "dir", 0);
    Dsj *s = script_by_id(m, sid), *arr = NULL, *r;
    int idx = -1;
    void *tok;
    if (!s) { berr(m, "没有这段脚本(#%lld)", (long long)sid); return NULL; }
    if (!find_block(script_blocks(s), bid, &arr, &idx) || !arr) {
        berr(m, "找不到那块积木(#%lld)", (long long)bid);
        return NULL;
    }
    if (dir < 0 && idx == 0) { berr(m, "已经在最上面了"); return NULL; }
    if (dir > 0 && idx == dsj_len(arr) - 1) { berr(m, "已经在最下面了"); return NULL; }
    if (dir == 0) { berr(m, "blocks.move 需要 dir(-1 上移 / +1 下移)"); return NULL; }
    tok = ds_model_edit_open(m);
    {
        Dsj tmp = *dsj_at(arr, idx);
        *arr->items[idx] = *arr->items[idx + dir];
        *arr->items[idx + dir] = tmp;
    }
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_bool(r, "moved", 1);
    return r;
}

/* 改一个空(下拉选了什么就写什么);script 级 props 用 block_id=0 */
static Dsj *cmd_block_set(DsModel *m, Dsj *args)
{
    long long sid = dsj_get_int(args, "id", 0);
    long long bid = dsj_get_int(args, "block_id", -1);
    const char *name = dsj_get_str(args, "name", "");
    Dsj *val = dsj_get(args, "value");
    Dsj *s = script_by_id(m, sid), *b, *r;
    void *tok;
    if (!s) { berr(m, "没有这段脚本(#%lld)", (long long)sid); return NULL; }
    if (!name || !*name) { berr(m, "blocks.set 需要 args.name"); return NULL; }
    /* block_id <= 0 = 改**事件自己**的空(比如"当按下 [空格]")。
     * 前端对脚本级属性就传 0(积木 id 从 1 开始),所以 0 必须当脚本级 ——
     * 否则"当按下 X"的下拉永远改不动(报"找不到那块积木 #0")。 */
    if (bid <= 0) {
        b = s;
        if (!blk_slot(blk_def(dsj_get_str(s, "event", "")), name)) {
            berr(m, "这段事件没有 '%s' 这个空", name);
            return NULL;
        }
    } else {
        b = find_block(script_blocks(s), bid, NULL, NULL);
        if (!b) { berr(m, "找不到那块积木(#%lld)", (long long)bid); return NULL; }
        if (!blk_slot(blk_def(dsj_get_str(b, "type", "")), name)) {
            berr(m, "'%s' 这块积木没有 '%s' 这个空",
                 dsj_get_str(b, "type", ""), name);
            return NULL;
        }
    }
    tok = ds_model_edit_open(m);
    {
        Dsj *p = dsj_get(b, "props");
        if (!p || p->t != DSJ_OBJ) { p = dsj_obj(); dsj_set(b, "props", p); }
        if (val) dsj_set(p, name, dsj_clone(val));
        else dsj_set_str(p, name, "");
    }
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_bool(r, "set", 1);
    return r;
}

/* ------------------------------------------------------------ 代码生成 */

typedef struct { char *buf; size_t len, cap; int depth; int need_actions; } Gen;

static void gput(Gen *g, const char *s)
{
    size_t n = strlen(s);
    if (g->len + n + 1 > g->cap) {
        size_t nc = g->cap ? g->cap * 2 : 4096;
        while (nc < g->len + n + 1) nc *= 2;
        g->buf = realloc(g->buf, nc);
        g->cap = nc;
    }
    memcpy(g->buf + g->len, s, n + 1);
    g->len += n;
}

static void gline(Gen *g, const char *s)
{
    int i;
    for (i = 0; i < g->depth; i++) gput(g, "    ");
    gput(g, s);
    gput(g, "\n");
}

static void gindent(Gen *g)
{
    int i;
    for (i = 0; i < g->depth; i++) gput(g, "    ");
}

static void gstr(Gen *g, const char *s)
{
    gput(g, "\"");
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') gput(g, "\\");
        {
            char one[2] = {*s, 0};
            gput(g, one);
        }
    }
    gput(g, "\"");
}

static void gnum(Gen *g, double v)
{
    char tmp[48];
    if (v == (double)(long long)v && v < 1e15 && v > -1e15)
        snprintf(tmp, sizeof tmp, "%lld.0", (long long)v);
    else
        snprintf(tmp, sizeof tmp, "%.6g", v);
    gput(g, tmp);
}

/* eng_find("名字") */
static void gfind(Gen *g, const char *name)
{
    gput(g, "eng_find(");
    gstr(g, name ? name : "");
    gput(g, ")");
}

static double speed_of(const char *v)
{
    if (!v) return 150.0;
    if (!strcmp(v, "slow")) return 60.0;
    if (!strcmp(v, "fast")) return 300.0;
    return 150.0;
}
static double force_of(const char *v)
{
    if (!v) return -450.0;
    if (!strcmp(v, "small")) return -300.0;
    if (!strcmp(v, "big")) return -600.0;
    return -450.0;
}
/* 槽里的数值:自定义时直接写数字 */
static double num_of(Dsj *p, const char *name, double dflt)
{
    const char *v = dsj_get_str(p, name, "");
    if (!v || !*v) return dflt;
    return atof(v);
}

/* 条件表达式 */
static void gen_cond(Gen *g, const char *type, Dsj *p)
{
    if (!strcmp(type, "if_ground")) {
        gput(g, "eng_on_ground(");
        gfind(g, dsj_get_str(p, "obj", ""));
        gput(g, ")");
    } else if (!strcmp(type, "if_key")) {
        gput(g, "eng_key_down(");
        gnum(g, num_of(p, "key", 32));
        gput(g, ")");
    } else if (!strcmp(type, "if_action")) {
        g->need_actions = 1;
        gput(g, "eng_action_down(");
        gstr(g, dsj_get_str(p, "action", "jump"));
        gput(g, ")");
    } else if (!strcmp(type, "if_mouse")) {
        gput(g, "eng_mouse_down(");
        gnum(g, num_of(p, "btn", 256));
        gput(g, ")");
    } else {                              /* if_field:字段 比较 数值 */
        const char *f = dsj_get_str(p, "field", "transform.x");
        const char *cmp = dsj_get_str(p, "cmp", "gt");
        const char *op = !strcmp(cmp, "lt") ? " < " : (!strcmp(cmp, "eq") ? " == " : " > ");
        char comp[64] = "transform", field[64] = "x";
        const char *dot = strchr(f, '.');
        if (dot) {
            size_t n = (size_t)(dot - f);
            if (n >= sizeof comp) n = sizeof comp - 1;
            memcpy(comp, f, n);
            comp[n] = 0;
            snprintf(field, sizeof field, "%s", dot + 1);
        }
        gput(g, "eng_get_f(");
        gfind(g, dsj_get_str(p, "obj", ""));
        gput(g, ", ");
        gstr(g, comp);
        gput(g, ", ");
        gstr(g, field);
        gput(g, ")");
        gput(g, op);
        gnum(g, num_of(p, "value", 100));
    }
}

/* 一条动作积木 */
static void gen_action(Gen *g, const char *type, Dsj *p)
{
    Gen st;
    memset(&st, 0, sizeof st);
    if (!strcmp(type, "move")) {
        const char *dir = dsj_get_str(p, "dir", "right");
        const char *axis = (!strcmp(dir, "up") || !strcmp(dir, "down")) ? "y" : "x";
        int neg = (!strcmp(dir, "left") || !strcmp(dir, "up"));
        const char *comp = (!strcmp(axis, "y") && !strcmp(dsj_get_str(p, "obj2", ""), ""))
                           ? "transform" : "transform";
        gput(&st, "eng_set_f(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ", ");
        gstr(&st, comp);
        gput(&st, ", ");
        gstr(&st, axis);
        gput(&st, ", eng_get_f(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ", ");
        gstr(&st, comp);
        gput(&st, ", ");
        gstr(&st, axis);
        gput(&st, ") ");
        gput(&st, neg ? "- " : "+ ");
        gnum(&st, speed_of(dsj_get_str(p, "speed", "mid")));
        gput(&st, " * dt);");
    } else if (!strcmp(type, "set_pos")) {
        const char *f = dsj_get_str(p, "field", "transform.x");
        char comp[64] = "transform", field[64] = "x";
        const char *dot = strchr(f, '.');
        if (dot) {
            size_t n = (size_t)(dot - f);
            if (n >= sizeof comp) n = sizeof comp - 1;
            memcpy(comp, f, n);
            comp[n] = 0;
            snprintf(field, sizeof field, "%s", dot + 1);
        }
        gput(&st, "eng_set_f(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ", ");
        gstr(&st, comp);
        gput(&st, ", ");
        gstr(&st, field);
        gput(&st, ", ");
        gnum(&st, num_of(p, "value", 100));
        gput(&st, ");");
    } else if (!strcmp(type, "jump")) {
        gput(&st, "eng_set_f(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ", \"body\", \"vy\", ");
        gnum(&st, force_of(dsj_get_str(p, "force", "mid")));
        gput(&st, ");");
    } else if (!strcmp(type, "set_size")) {
        double k = num_of(p, "size", 1.0);
        gput(&st, "eng_set_f(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ", \"transform\", \"sx\", ");
        gnum(&st, k);
        gput(&st, "); eng_set_f(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ", \"transform\", \"sy\", ");
        gnum(&st, k);
        gput(&st, ");");
    } else if (!strcmp(type, "camera_follow")) {
        /* 相机约定:(x,y) = 屏幕左上角的世界坐标 → 居中跟随 = 目标 - 屏幕的一半 */
        gput(&st, "eng_set_f(");
        gfind(&st, dsj_get_str(p, "cam", ""));
        gput(&st, ", \"camera\", \"x\", eng_world_x(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ") - 512.0); eng_set_f(");
        gfind(&st, dsj_get_str(p, "cam", ""));
        gput(&st, ", \"camera\", \"y\", eng_world_y(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ") - 320.0);");
    } else if (!strcmp(type, "play_sound")) {
        gput(&st, "eng_sound_play(eng_sound_load(");
        gstr(&st, dsj_get_str(p, "path", ""));
        gput(&st, "), 0, 1.0);");
    } else if (!strcmp(type, "destroy")) {
        gput(&st, "eng_object_free(");
        gfind(&st, dsj_get_str(p, "obj", ""));
        gput(&st, ");");
    } else {
        gput(&st, "0;");
    }
    gline(g, st.buf ? st.buf : "0;");
    free(st.buf);
}

static void gen_blocks(Gen *g, Dsj *blocks)
{
    int i;
    for (i = 0; i < dsj_len(blocks); i++) {
        Dsj *b = dsj_at(blocks, i);
        const char *type = dsj_get_str(b, "type", "");
        Dsj *p = dsj_get(b, "props");
        const BlkType *d = blk_def(type);
        if (!d) continue;
        if (d->body) {
            gindent(g);
            gput(g, "if (");
            gen_cond(g, type, p);
            gput(g, ") != 0 {\n");
            g->depth++;
            gen_blocks(g, dsj_get(b, "body"));
            g->depth--;
            gline(g, "}");
        } else {
            gen_action(g, type, p);
        }
    }
}

static void gen_event_scripts(DsModel *m, Gen *g, const char *event, int in_update)
{
    Dsj *a = blk_scripts(m);
    int i, any = 0;
    for (i = 0; i < dsj_len(a); i++) {
        Dsj *s = dsj_at(a, i);
        Dsj *p;
        if (strcmp(dsj_get_str(s, "event", ""), event)) continue;
        any = 1;
        p = dsj_get(s, "props");
        if (in_update) {                  /* 当按下 X → 每帧里轮询 */
            gindent(g);
            gput(g, "if (");
            if (!strcmp(event, "on_key")) {
                gput(g, "eng_key_pressed(");
                gnum(g, num_of(p, "key", 32));
                gput(g, ")");
            } else {
                g->need_actions = 1;
                gput(g, "eng_action_pressed(");
                gstr(g, dsj_get_str(p, "action", "jump"));
                gput(g, ")");
            }
            gput(g, ") != 0 {\n");
            g->depth++;
            gen_blocks(g, script_blocks(s));
            g->depth--;
            gline(g, "}");
        } else {
            gen_blocks(g, script_blocks(s));
        }
    }
    if (!any && !in_update) gline(g, "# (这段还没有积木)");
}

char *ds_blocks_generate(DsModel *m, char *err, unsigned errsz)
{
    Gen g;
    memset(&g, 0, sizeof g);
    (void)errsz;                     /* 生成本身不报错(校验在 generate_to_file 里) */
    if (err) *err = 0;

    gput(&g, "# 由 DexStudio 的积木生成 —— 不要手改(改积木,或者改 main.dex)\n");
    gput(&g, "include \"dexgame\";\n\n");

    gline(&g, "func logic_start(dt: float) {");
    g.depth++;
    gen_event_scripts(m, &g, "on_start", 0);
    g.depth--;
    gline(&g, "}");

    gline(&g, "func logic_update(dt: float) {");
    g.depth++;
    gen_event_scripts(m, &g, "on_update", 0);
    gen_event_scripts(m, &g, "on_key", 1);
    gen_event_scripts(m, &g, "on_action", 1);
    g.depth--;
    gline(&g, "}");

    gline(&g, "func logic_draw(dt: float) {");
    g.depth++;
    gline(&g, "# (积木暂时没有绘制类积木)");
    g.depth--;
    gline(&g, "}");

    if (g.need_actions) {
        /* 用到"动作"键位:在 include 之后补一行(主模板里也有,重复 include 会被去重) */
        const char *anchor = "include \"dexgame\";\n";
        char *at = strstr(g.buf, anchor);
        if (at) {
            const char *tail = at + strlen(anchor);
            size_t head = (size_t)(tail - g.buf);
            size_t need = g.len + 32;
            char *out = malloc(need);
            if (out) {
                snprintf(out, need, "%.*sinclude \"dexgame_fast\";\n%s",
                         (int)head, g.buf, tail);
                free(g.buf);
                g.buf = out;
            }
        }
    }
    if (!g.buf) g.buf = calloc(1, 1);
    return g.buf;
}

/* ------------------------------------------------------------ 校验 */

static void blk_issue(Dsj *issues, long long sid, const char *where, const char *level,
                      const char *fmt, ...)
{
    Dsj *o = dsj_obj();
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    dsj_set_int(o, "script", sid);
    dsj_set_str(o, "where", where ? where : "");
    dsj_set_str(o, "level", level);
    dsj_set_str(o, "msg", msg);
    dsj_push(issues, o);
}

static void validate_blocks(DsModel *m, Dsj *issues)
{
    Dsj *a = blk_scripts(m);
    Dsj *opts = ds_model_scene_options(m);
    Dsj *ents = dsj_get(opts, "entities");
    int i, k;
    for (i = 0; i < dsj_len(a); i++) {
        Dsj *s = dsj_at(a, i);
        long long sid = dsj_get_int(s, "id", 0);
        Dsj *blocks = script_blocks(s);
        const char *event = dsj_get_str(s, "event", "");
        const BlkType *ed = blk_def(event);
        if (!ed) { blk_issue(issues, sid, "事件", "error", "这段的事件类型不认识"); continue; }
        /* 事件自己的属性也要检查 */
        for (k = 0; ed->slots && ed->slots[k].name; k++) {
            const BlkSlot *sl = &ed->slots[k];
            const char *v = dsj_get_str(dsj_get(s, "props"), sl->name, "");
            if (!v || !*v) {
                blk_issue(issues, sid, "事件", "error", "「%s」还没有选 %s",
                          ed->text, sl->name);
            }
        }
        for (k = 0; k < dsj_len(blocks); k++) {
            Dsj *b = dsj_at(blocks, k);
            const char *type = dsj_get_str(b, "type", "");
            const BlkType *d = blk_def(type);
            Dsj *p = dsj_get(b, "props");
            int j;
            if (!d) { blk_issue(issues, sid, type, "error", "不认识的积木"); continue; }
            for (j = 0; d->slots && d->slots[j].name; j++) {
                const BlkSlot *sl = &d->slots[j];
                const char *v = dsj_get_str(p, sl->name, "");
                if (!strcmp(sl->kind, "audio")) {
                    if (!v || !*v)
                        blk_issue(issues, sid, type, "error",
                                  "「%s」还没有选声音", d->text);
                } else if (!strcmp(sl->kind, "entity")) {
                    int found = 0, q;
                    if (!v || !*v) {
                        blk_issue(issues, sid, type, "error", "「%s」还没有选角色", d->text);
                        continue;
                    }
                    for (q = 0; q < dsj_len(ents); q++)
                        if (!strcmp(dsj_get_str(dsj_at(ents, q), "name", ""), v)) found = 1;
                    if (!found)
                        blk_issue(issues, sid, type, "warn",
                                  "当前场景里没有叫 '%s' 的实体", v);
                } else if (!v || !*v) {
                    blk_issue(issues, sid, type, "error", "「%s」还有一个空没选", d->text);
                }
            }
        }
    }
    dsj_free(opts);
}

static Dsj *cmd_validate(DsModel *m)
{
    Dsj *issues = dsj_arr();
    Dsj *r = dsj_obj();
    int i, errs = 0, warns = 0;
    validate_blocks(m, issues);
    for (i = 0; i < dsj_len(issues); i++) {
        if (!strcmp(dsj_get_str(dsj_at(issues, i), "level", "error"), "error")) errs++;
        else warns++;
    }
    dsj_set_bool(r, "ok", errs == 0);
    dsj_set_int(r, "errors", errs);
    dsj_set_int(r, "warnings", warns);
    dsj_set(r, "issues", issues);
    return r;
}

/* ------------------------------------------------------------ 生成到文件 */

int ds_blocks_generate_to_file(DsModel *m, char *out_path, unsigned pathsz,
                               char *err, unsigned errsz)
{
    const char *root = ds_project_dir(m);
    char path[1400];
    Dsj *issues;
    int errs = 0, i;
    char *src;
    if (!root || !*root) {
        if (err) snprintf(err, errsz, "还没有打开项目,生成的代码没有地方放");
        return 0;
    }
    issues = dsj_arr();
    validate_blocks(m, issues);
    for (i = 0; i < dsj_len(issues); i++) {
        if (!strcmp(dsj_get_str(dsj_at(issues, i), "level", "error"), "error")) errs++;
    }
    if (errs > 0) {
        size_t o = 0;
        o += (size_t)snprintf(err, errsz, "积木有 %d 处必须修的问题:", errs);
        for (i = 0; i < dsj_len(issues) && i < 3 && o + 8 < errsz; i++) {
            Dsj *it = dsj_at(issues, i);
            if (strcmp(dsj_get_str(it, "level", "error"), "error")) continue;
            o += (size_t)snprintf(err + o, errsz - o, "\n  · %s",
                                  dsj_get_str(it, "msg", ""));
        }
        dsj_free(issues);
        return 0;
    }
    dsj_free(issues);
    if (!blocks_write(m, err, errsz)) return 0;
    src = ds_blocks_generate(m, err, errsz);
    if (!src) return 0;
    snprintf(path, sizeof path, "%s\\%s", root, DS_BLOCKS_DEX_REL);
    {
        size_t k;
        for (k = 0; path[k]; k++) if (path[k] == '/') path[k] = '\\';
    }
    if (out_path && pathsz) snprintf(out_path, pathsz, "%s", path);
    if (!ds_write_text(path, src)) {
        if (err) snprintf(err, errsz, "无法写入 %s", path);
        free(src);
        return 0;
    }
    free(src);
    return 1;
}

static Dsj *cmd_generate(DsModel *m)
{
    char err[512];
    char path[1400];
    char *src;
    Dsj *r;
    if (!ds_blocks_generate_to_file(m, path, sizeof path, err, sizeof err)) {
        berr(m, "%s", err);
        return NULL;
    }
    src = ds_read_text(path, NULL);
    r = dsj_obj();
    dsj_set_str(r, "path", path);
    dsj_set_str(r, "source", src ? src : "");
    free(src);
    return r;
}

/* ------------------------------------------------------------ 一键模板 */

/* 给一摞积木(含 if 肚子里的)分配唯一 block_id。
 * 必须做:没有 id 的积木在界面上**改不了也删不掉**(UI 是按 block_id 定位的),
 * 而"一键示例"是直接拼 JSON 的 —— 实测漏了这一步,示例积木全成了死块。 */
static void assign_block_ids(Dsj *blocks, long long *seed)
{
    int i;
    if (!blocks || blocks->t != DSJ_ARR) return;
    for (i = 0; i < dsj_len(blocks); i++) {
        Dsj *b = dsj_at(blocks, i);
        if (b->t != DSJ_OBJ) continue;
        if (dsj_get_int(b, "block_id", 0) == 0) dsj_set_int(b, "block_id", ++(*seed));
        assign_block_ids(dsj_get(b, "body"), seed);
    }
}

/* 三个"一上来就能动"的示例:新用户点一下就有东西跑起来,再自己改。 */
int ds_blocks_apply_template(DsModel *m, const char *id, char *err, unsigned errsz)
{
    char hero[160], cam[160], snd[1024];
    first_entity(m, hero, sizeof hero, 0);
    first_entity(m, cam, sizeof cam, 1);
    first_audio(m, snd, sizeof snd);
    if (!hero[0]) {
        if (err) snprintf(err, errsz, "场景里还没有实体 —— 先去「场景」加一个(比如玩家)");
        return 0;
    }
    {
        Dsj *g = ds_model_blocks(m);
        Dsj *arr = blk_scripts(m);
        long long sid = next_script_id(m);
        Dsj *s = dsj_obj();
        Dsj *blocks = dsj_arr();
        dsj_set_int(s, "id", sid);
        dsj_set_int(g, "format", 1);
        if (!strcmp(id, "move_jump")) {
            /* 每一帧:左右移动 + 如果按着跳 → 跳一下 */
            Dsj *mv = dsj_obj();
            Dsj *iff = dsj_obj();
            Dsj *body = dsj_arr();
            Dsj *jp = dsj_obj();
            dsj_set_str(s, "event", "on_update");
            dsj_set_str(mv, "type", "move");
            dsj_set(mv, "props", dsj_obj());
            dsj_set_str(dsj_get(mv, "props"), "obj", hero);
            dsj_set_str(dsj_get(mv, "props"), "dir", "right");
            dsj_set_str(dsj_get(mv, "props"), "speed", "mid");
            dsj_push(blocks, mv);
            dsj_set_str(iff, "type", "if_action");
            dsj_set(iff, "props", dsj_obj());
            dsj_set_str(dsj_get(iff, "props"), "action", "jump");
            dsj_set_str(jp, "type", "jump");
            dsj_set(jp, "props", dsj_obj());
            dsj_set_str(dsj_get(jp, "props"), "obj", hero);
            dsj_set_str(dsj_get(jp, "props"), "force", "mid");
            dsj_push(body, jp);
            dsj_set(iff, "body", body);
            dsj_push(blocks, iff);
        } else if (!strcmp(id, "camera")) {
            Dsj *cf = dsj_obj();
            dsj_set_str(s, "event", "on_update");
            dsj_set_str(cf, "type", "camera_follow");
            dsj_set(cf, "props", dsj_obj());
            dsj_set_str(dsj_get(cf, "props"), "cam", cam);
            dsj_set_str(dsj_get(cf, "props"), "obj", hero);
            dsj_push(blocks, cf);
        } else {                       /* sound:按下空格播放声音 */
            Dsj *ps = dsj_obj();
            dsj_set_str(s, "event", "on_key");
            dsj_set(s, "props", dsj_obj());
            dsj_set_str(dsj_get(s, "props"), "key", "32");
            dsj_set_str(ps, "type", "play_sound");
            dsj_set(ps, "props", dsj_obj());
            dsj_set_str(dsj_get(ps, "props"), "path", snd);
            dsj_push(blocks, ps);
            if (!snd[0]) {
                if (err) snprintf(err, errsz,
                                  "res/ 里还没有音频 —— 先在左边「资源」里导入一个");
                return 0;
            }
        }
        dsj_set(s, "blocks", blocks);
        {
            long long seed = next_block_id(m) - 1;
            assign_block_ids(blocks, &seed);
        }
        dsj_push(arr, s);
    }
    return 1;
}

static Dsj *cmd_template(DsModel *m, Dsj *args)
{
    const char *id = dsj_get_str(args, "id", "move_jump");
    char err[256];
    Dsj *r;
    void *tok = ds_model_edit_open(m);
    if (!ds_blocks_apply_template(m, id, err, sizeof err)) {
        ds_model_edit_close(m, tok, 0);
        berr(m, "%s", err);
        return NULL;
    }
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_str(r, "template", id);
    dsj_set(r, "scripts", dsj_clone(blk_scripts(m)));
    return r;
}

/* ------------------------------------------------------------ 分发 */

Dsj *ds_blocks_command(DsModel *m, const char *cmd, Dsj *args)
{
    if (!strcmp(cmd, "blocks.types")) return cmd_types(m);
    if (!strcmp(cmd, "blocks.info")) return cmd_info(m);
    if (!strcmp(cmd, "blocks.save")) return cmd_save(m);
    if (!strcmp(cmd, "blocks.script.add")) return cmd_script_add(m, args);
    if (!strcmp(cmd, "blocks.script.remove")) return cmd_script_remove(m, args);
    if (!strcmp(cmd, "blocks.add")) return cmd_block_add(m, args);
    if (!strcmp(cmd, "blocks.remove")) return cmd_block_remove(m, args);
    if (!strcmp(cmd, "blocks.move")) return cmd_block_move(m, args);
    if (!strcmp(cmd, "blocks.set")) return cmd_block_set(m, args);
    if (!strcmp(cmd, "blocks.template")) return cmd_template(m, args);
    if (!strcmp(cmd, "blocks.validate")) return cmd_validate(m);
    if (!strcmp(cmd, "blocks.generate")) return cmd_generate(m);
    berr(m, "未知命令 '%s'", cmd);
    return NULL;
}
