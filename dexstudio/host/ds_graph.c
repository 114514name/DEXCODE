/* ============================================================================
 * ds_graph.c — DexStudio 的**逻辑图**模型(UE 蓝图式节点图 → DexLang 源码)。
 *
 * 设计要点(与 docs/DEXGAME_DESIGN.md §9.1 决策 #4 一致:复用 bluedit 的**模型架构**,
 * 但节点词汇表换成**面向 dexgame 引擎**的 —— bluedit 是 GAL(视觉小说)的节点集,
 * 背景/立绘/选项那一套对 2D 游戏逻辑没有用):
 *
 *   1. **图 = 一棵 JSON DOM**,存在 <项目>/scripts/logic.json。不用 C 结构体是因为
 *      编辑器真正需要的只有"取字段、改字段、连线、写回去",那正是 ds_json 的强项;
 *      节点语义(校验 + 代码生成)集中在本文件,前端不做任何判断。
 *   2. **自省**:节点目录(Node catalog)在 C 里定义一次,`graph.types` 交给前端,
 *      前端据此画节点与引脚 —— 与属性面板靠 `comp.schema` 自省是同一套路。
 *   3. **生成的代码必须能过编译器**:B4 的验收线是"生成的 .dex 能被 dexc.exe 编译
 *      并用合成输入跑出预期行为",所以生成器只用 DexLang 确定有的语法
 *      (func/if/else/let/表达式/字符串),不赌任何边角特性。
 *   4. **没有用户变量**(一期):DexLang 没有全局变量,回调之间无法共享局部状态 ——
 *      引擎的正解就是把跨帧状态放**组件字段**里。所以图直接读/写实体字段
 *      (`field` / `set_field`),不另造一套变量系统。见 AGENTS.md 的 M4 陷阱。
 *   5. **确定性**:事件按 (y, x, id) 排序后生成,同一份图永远生成同一份代码,
 *      否则"改了别的节点导致代码乱跳"会让 diff 没法看。
 * ==========================================================================*/
#include "ds_graph.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ 节点目录 */

/* 引脚写法:"名字:类型,名字:类型";类型 e=执行流 n=数值 s=字符串 */
typedef struct {
    const char *type;    /* 存进 JSON 的类型名 */
    const char *title;   /* 界面上显示的中文名 */
    const char *cat;     /* 分类 */
    const char *in;      /* 输入引脚 */
    const char *out;     /* 输出引脚 */
    const char *props;   /* 属性:名字:类型(f=浮点 i=整数 s=字符串) */
} DsgTypeDef;

static const DsgTypeDef DSG_TYPES[] = {
    /* --- 事件(每个回调里可以放多个,按位置从上到下生成) --- */
    {"on_start",     "开始时",     "事件", "",               "out:e",          ""},
    {"on_update",    "每帧",       "事件", "",               "out:e",          ""},
    {"on_draw",      "绘制",       "事件", "",               "out:e",          ""},
    {"on_action",    "动作刚按下", "事件", "",               "out:e",          "action:s"},
    {"on_key",       "按键刚按下", "事件", "",               "out:e",          "key:i"},
    /* --- 流程 --- */
    {"branch",       "分支",       "流程", "exec:e,cond:n",  "true:e,false:e", "cond:n"},
    /* --- 动作 --- */
    {"set_field",    "写字段",     "动作", "exec:e,value:n", "out:e",
     "obj:s,comp:s,field:s,value:n,as:s"},
    {"add_field",    "字段累加",   "动作", "exec:e,delta:n", "out:e",
     "obj:s,comp:s,field:s,delta:n,as:s"},
    {"destroy",      "销毁实体",   "动作", "exec:e",         "out:e",          "obj:s"},
    {"play_sound",   "播放声音",   "动作", "exec:e",         "out:e",
     "path:s,volume:n,loop:i"},
    {"camera_to",    "相机跟随",   "动作", "exec:e",         "out:e",
     "cam:s,obj:s,ox:n,oy:n"},
    /* --- 取值 / 条件(没有执行引脚,只接数据线) --- */
    {"num",          "数字",       "取值", "",               "v:n",            "value:n"},
    {"dt",           "帧间隔(秒)", "取值", "",              "v:n",            ""},
    {"field",        "读字段",     "取值", "",               "v:n",
     "obj:s,comp:s,field:s"},
    {"math",         "四则运算",   "取值", "a:n,b:n",        "v:n",            "op:s"},
    {"compare",      "比较",       "条件", "a:n,b:n",        "v:n",            "op:s"},
    {"action_down",  "动作按住",   "条件", "",               "v:n",            "action:s"},
    {"action_pressed","动作刚按下", "条件", "",              "v:n",            "action:s"},
    {"key_down",     "按键按住",   "条件", "",               "v:n",            "key:i"},
    {"mouse_down",   "鼠标按住",   "条件", "",               "v:n",            "btn:i"},
    {"on_ground",    "在地面",     "条件", "",               "v:n",            "obj:s"},
    {"mouse_world",  "鼠标世界坐标", "取值", "",             "v:n",            "axis:s"},
};

#define DSG_TYPE_COUNT ((int)(sizeof DSG_TYPES / sizeof DSG_TYPES[0]))

static const DsgTypeDef *type_def(const char *type)
{
    int i;
    if (!type) return NULL;
    for (i = 0; i < DSG_TYPE_COUNT; i++)
        if (!strcmp(DSG_TYPES[i].type, type)) return &DSG_TYPES[i];
    return NULL;
}

/* 在一个 "名字:类型,…" 串里找引脚;返回 1 找到。ec=1 表示执行流引脚。 */
static int pin_find(const char *def, const char *pin, int *is_exec)
{
    const char *p = def ? def : "";
    size_t n = pin ? strlen(pin) : 0;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        if (colon && (size_t)(colon - p) == n && !strncmp(p, pin, n)) {
            if (is_exec) *is_exec = (colon[1] == 'e');
            return 1;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* 属性表里找名字;返回类型字符(f/i/s)或 0 */
static char prop_type_of(const DsgTypeDef *d, const char *name)
{
    const char *p = d->props ? d->props : "";
    size_t n = name ? strlen(name) : 0;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        if (colon && (size_t)(colon - p) == n && !strncmp(p, name, n)) return colon[1];
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* ------------------------------------------------------------ 文档存取 */

static Dsj *graph_nodes(DsModel *m)
{
    Dsj *g = ds_model_graph(m);
    Dsj *a = dsj_get(g, "nodes");
    if (!a || a->t != DSJ_ARR) {
        a = dsj_arr();
        dsj_set(g, "nodes", a);
    }
    return a;
}

static Dsj *graph_links(DsModel *m)
{
    Dsj *g = ds_model_graph(m);
    Dsj *a = dsj_get(g, "links");
    if (!a || a->t != DSJ_ARR) {
        a = dsj_arr();
        dsj_set(g, "links", a);
    }
    return a;
}

static Dsj *node_by_id(DsModel *m, long long id)
{
    Dsj *nodes = graph_nodes(m);
    int i;
    for (i = 0; i < dsj_len(nodes); i++) {
        Dsj *n = dsj_at(nodes, i);
        if (n && dsj_get_int(n, "id", 0) == id) return n;
    }
    return NULL;
}

static long long node_next_id(DsModel *m)
{
    Dsj *nodes = graph_nodes(m);
    long long maxid = 0;
    int i;
    for (i = 0; i < dsj_len(nodes); i++) {
        long long id = dsj_get_int(dsj_at(nodes, i), "id", 0);
        if (id > maxid) maxid = id;
    }
    return maxid + 1;
}

static Dsj *empty_graph(void)
{
    Dsj *g = dsj_obj();
    dsj_set_int(g, "format", 1);
    dsj_set(g, "nodes", dsj_arr());
    dsj_set(g, "links", dsj_arr());
    return g;
}

static int graph_write(DsModel *m, char *err, unsigned errsz)
{
    const char *root = ds_project_dir(m);
    char *path, *dir;
    char *txt;
    int ok;
    if (!root || !*root) {
        if (err) snprintf(err, errsz, "还没有打开项目,逻辑图没有地方存");
        return 0;
    }
    path = malloc(strlen(root) + 64);
    dir = malloc(strlen(root) + 64);
    if (!path || !dir) {
        free(path); free(dir);
        if (err) snprintf(err, errsz, "内存不足");
        return 0;
    }
    sprintf(path, "%s\\scripts", root);
    sprintf(dir, "%s\\scripts\\logic.json", root);
    /* scripts/ 目录由 project.new 建过;老项目可能没有,补一下 */
    {
        extern int ds_mkdir(const char *);   /* 由 ds_model.c 提供 */
        ds_mkdir(path);
    }
    txt = dsj_dump(ds_model_graph(m));
    if (!txt) {
        free(path); free(dir);
        if (err) snprintf(err, errsz, "序列化逻辑图失败");
        return 0;
    }
    {
        extern int ds_write_text(const char *, const char *);   /* 由 ds_model.c 提供 */
        ok = ds_write_text(dir, txt);
    }
    if (!ok && err) snprintf(err, errsz, "无法写入 %s", dir);
    free(txt);
    free(path);
    free(dir);
    return ok;
}

void ds_graph_reload(DsModel *m)
{
    const char *root = ds_project_dir(m);
    char path[1400];
    char *txt;
    Dsj *dom = NULL;
    if (!root || !*root) {
        ds_model_set_graph(m, NULL);
        return;
    }
    snprintf(path, sizeof path, "%s\\scripts\\logic.json", root);
    {
        extern char *ds_read_text(const char *, size_t *);      /* 由 ds_model.c 提供 */
        txt = ds_read_text(path, NULL);
    }
    if (txt) {
        char perr[256];
        dom = dsj_parse(txt, perr, sizeof perr);
        free(txt);
        if (dom && (dom->t != DSJ_OBJ || !dsj_get(dom, "nodes"))) {
            /* 不像逻辑图(比如被人手改坏了):宁可当空图,也不要把坏数据带进编辑器 */
            dsj_free(dom);
            dom = NULL;
        }
    }
    ds_model_set_graph(m, dom ? dom : empty_graph());
}

const char *ds_graph_json(DsModel *m)
{
    static char *cache = NULL;
    char *txt = dsj_dump(ds_model_graph(m));
    free(cache);
    cache = txt;
    return cache ? cache : "{}";
}

int ds_graph_save(DsModel *m) { return graph_write(m, NULL, 0); }

/* ------------------------------------------------------------ 目录(graph.types) */

static Dsj *pins_json(const char *def)
{
    Dsj *a = dsj_arr();
    const char *p = def ? def : "";
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        Dsj *o = dsj_obj();
        if (colon) {
            char name[64];
            size_t nl = (size_t)(colon - p);
            if (nl >= sizeof name) nl = sizeof name - 1;
            memcpy(name, p, nl);
            name[nl] = 0;
            dsj_set_str(o, "name", name);
            dsj_set_str(o, "type",
                        colon[1] == 'e' ? "exec" : (colon[1] == 's' ? "string" : "num"));
        }
        dsj_push(a, o);
        if (!comma) break;
        p = comma + 1;
    }
    return a;
}

static Dsj *props_json(const char *def)
{
    Dsj *a = dsj_arr();
    const char *p = def ? def : "";
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        Dsj *o = dsj_obj();
        if (colon) {
            char name[64];
            size_t nl = (size_t)(colon - p);
            if (nl >= sizeof name) nl = sizeof name - 1;
            memcpy(name, p, nl);
            name[nl] = 0;
            dsj_set_str(o, "name", name);
            dsj_set_str(o, "type",
                        colon[1] == 's' ? "string" : (colon[1] == 'i' ? "int" : "float"));
        }
        dsj_push(a, o);
        if (!comma) break;
        p = comma + 1;
    }
    return a;
}

static Dsj *cmd_types(DsModel *m)
{
    Dsj *a = dsj_arr();
    int i;
    (void)m;
    for (i = 0; i < DSG_TYPE_COUNT; i++) {
        Dsj *o = dsj_obj();
        dsj_set_str(o, "type", DSG_TYPES[i].type);
        dsj_set_str(o, "title", DSG_TYPES[i].title);
        dsj_set_str(o, "cat", DSG_TYPES[i].cat);
        dsj_set(o, "in", pins_json(DSG_TYPES[i].in));
        dsj_set(o, "out", pins_json(DSG_TYPES[i].out));
        dsj_set(o, "props", props_json(DSG_TYPES[i].props));
        dsj_push(a, o);
    }
    return a;
}

/* ------------------------------------------------------------ 下拉候选(给前端)
 *
 * 逻辑图里的 obj/comp/field/action/key/path 以前全是手打的字符串或数字 ——
 * 打错一个字就静默无效。这些集合**程序全知道**,所以由 C 一次性交给前端:
 * 实体名、组件→字段、默认动作名、按键码表、运算符表。
 * (与节点目录同一套路:前端只负责画,不在 JS 里硬编码第二份真相。) */

static void keys_push(Dsj *a, int code, const char *label)
{
    Dsj *o = dsj_obj();
    dsj_set_int(o, "value", code);
    dsj_set_str(o, "label", label);
    dsj_push(a, o);
}

static Dsj *cmd_options(DsModel *m)
{
    Dsj *r = ds_model_scene_options(m);     /* entities / images / audios / schema */
    int i;
    /* 运算符:DexLang 真正有的那些。注意**没有** ^(词法器不认,选它编译必错)。 */
    {
        static const char *OPS[] = {"+", "-", "*", "/", "%",
                                    "<", "<=", "==", "!=", ">=", ">"};
        Dsj *a = dsj_arr();
        for (i = 0; i < (int)(sizeof OPS / sizeof OPS[0]); i++)
            dsj_push(a, dsj_str(OPS[i]));
        dsj_set(r, "ops", a);
    }
    /* 默认动作名(与 dexgame_fast.dex 的 eng_bind_default_actions 一致) */
    {
        static const char *ACTS[] = {"left", "right", "up", "down", "jump",
                                     "action", "back"};
        Dsj *a = dsj_arr();
        for (i = 0; i < (int)(sizeof ACTS / sizeof ACTS[0]); i++)
            dsj_push(a, dsj_str(ACTS[i]));
        dsj_set(r, "actions", a);
    }
    /* 按键码表(统一编号,见 libs/dexgame/dg_input.c 头部注释) */
    {
        Dsj *a = dsj_arr();
        char buf[64];
        static const struct { int code; const char *label; } SPEC[] = {
            {32, "空格"}, {13, "回车"}, {27, "Esc"}, {9, "Tab"}, {8, "退格"},
            {16, "Shift"}, {17, "Ctrl"}, {18, "Alt"},
            {37, "方向键 左"}, {38, "方向键 上"}, {39, "方向键 右"}, {40, "方向键 下"},
        };
        for (i = 0; i < (int)(sizeof SPEC / sizeof SPEC[0]); i++)
            keys_push(a, SPEC[i].code, SPEC[i].label);
        for (i = 0; i < 26; i++) {                     /* A..Z */
            snprintf(buf, sizeof buf, "字母 %c", 'A' + i);
            keys_push(a, 65 + i, buf);
        }
        for (i = 0; i < 10; i++) {                     /* 0..9 */
            snprintf(buf, sizeof buf, "数字 %d", i);
            keys_push(a, 48 + i, buf);
        }
        for (i = 0; i < 12; i++) {                     /* F1..F12 */
            snprintf(buf, sizeof buf, "F%d", i + 1);
            keys_push(a, 112 + i, buf);
        }
        keys_push(a, 300, "手柄 上"); keys_push(a, 301, "手柄 下");
        keys_push(a, 302, "手柄 左"); keys_push(a, 303, "手柄 右");
        keys_push(a, 304, "手柄 Start"); keys_push(a, 305, "手柄 Back");
        keys_push(a, 306, "手柄 左摇杆按下"); keys_push(a, 307, "手柄 右摇杆按下");
        keys_push(a, 308, "手柄 LB"); keys_push(a, 309, "手柄 RB");
        keys_push(a, 310, "手柄 A"); keys_push(a, 311, "手柄 B");
        keys_push(a, 312, "手柄 X"); keys_push(a, 313, "手柄 Y");
        {
            static const char *AX[6] = {"左摇杆 X", "左摇杆 Y", "右摇杆 X",
                                        "右摇杆 Y", "左扳机 LT", "右扳机 RT"};
            for (i = 0; i < 6; i++) {
                snprintf(buf, sizeof buf, "%s 正向", AX[i]);
                keys_push(a, 400 + i, buf);
                snprintf(buf, sizeof buf, "%s 负向", AX[i]);
                keys_push(a, 500 + i, buf);
            }
        }
        dsj_set(r, "keys", a);
    }
    /* 鼠标键(工程内部统一编号:256 左 / 257 右 / 258 中) */
    {
        Dsj *a = dsj_arr();
        keys_push(a, 256, "鼠标 左键");
        keys_push(a, 257, "鼠标 右键");
        keys_push(a, 258, "鼠标 中键");
        dsj_set(r, "mouse", a);
    }
    return r;
}

/* ------------------------------------------------------------ 命令:改图 */

static Dsj *cmd_graph_new(DsModel *m)
{
    void *tok = ds_model_edit_open(m);
    Dsj *r;
    ds_model_set_graph(m, empty_graph());
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_str(r, "graph", ds_graph_json(m));
    return r;
}

static Dsj *cmd_graph_info(DsModel *m)
{
    Dsj *r = dsj_obj();
    Dsj *g = ds_model_graph(m);
    const char *root = ds_project_dir(m);
    dsj_set_int(r, "format", (long long)dsj_get_int(g, "format", 1));
    dsj_set(r, "graph", dsj_clone(g));
    dsj_set_int(r, "nodes", dsj_len(graph_nodes(m)));
    dsj_set_int(r, "links", dsj_len(graph_links(m)));
    if (root && *root) {
        char path[1400];
        snprintf(path, sizeof path, "%s\\%s", root, DS_GRAPH_REL);
        dsj_set_str(r, "path", path);
        snprintf(path, sizeof path, "%s\\%s", root, DS_GRAPH_DEX_REL);
        dsj_set_str(r, "dex_path", path);
    } else {
        dsj_set_str(r, "path", "");
        dsj_set_str(r, "dex_path", "");
    }
    return r;
}

static Dsj *cmd_graph_save(DsModel *m)
{
    char err[256];
    Dsj *r;
    if (!graph_write(m, err, sizeof err)) {
        ds_model_error(m, "%s", err);
        return NULL;
    }
    r = dsj_obj();
    dsj_set_bool(r, "saved", 1);
    return r;
}

static Dsj *cmd_node_add(DsModel *m, Dsj *args)
{
    const char *type = dsj_get_str(args, "type", "");
    const DsgTypeDef *d = type_def(type);
    Dsj *n, *r, *props, *nodes = graph_nodes(m);
    long long id;
    void *tok;
    if (!d) {
        ds_model_error(m, "没有这种节点类型:'%s'(用 graph.types 看有哪些)", type);
        return NULL;
    }
    tok = ds_model_edit_open(m);
    id = node_next_id(m);
    n = dsj_obj();
    dsj_set_int(n, "id", id);
    dsj_set_str(n, "type", type);
    dsj_set_num(n, "x", dsj_get_num(args, "x", 40));
    dsj_set_num(n, "y", dsj_get_num(args, "y", 40));
    props = dsj_obj();
    /* 属性放进默认值,前端就不用管"缺字段" */
    {
        const char *p = d->props ? d->props : "";
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            const char *colon = memchr(p, ':', len);
            if (colon) {
                char name[64];
                size_t nl = (size_t)(colon - p);
                if (nl >= sizeof name) nl = sizeof name - 1;
                memcpy(name, p, nl);
                name[nl] = 0;
                if (colon[1] == 's') dsj_set_str(props, name, "");
                else dsj_set_num(props, name, 0);
            }
            if (!comma) break;
            p = comma + 1;
        }
    }
    if (d->props && strstr(d->props, "op:")) {
        /* 有序的枚举给个合理默认,免得新节点一编译就报错 */
        if (!strcmp(type, "compare")) dsj_set_str(props, "op", "<");
        else dsj_set_str(props, "op", "+");
    }
    /* 剩下的"枚举式字符串"属性也给个可用默认值(前端下拉框直接就有选中项) */
    if (prop_type_of(d, "as") == 's') dsj_set_str(props, "as", "f");
    if (prop_type_of(d, "axis") == 's') dsj_set_str(props, "axis", "x");
    /* 关键的一步:**引用型属性给成场景里真实存在的第一个实体/资源**。
     * 以前这里一律留空,于是刚加的「写字段」节点生成 eng_find("") —— 编译通过、
     * 运行期静默无效(用户只会觉得"图连好了却没反应")。 */
    {
        Dsj *opt = ds_model_scene_options(m);
        char obj[160] = "", cam[160] = "", snd[1024] = "";
        Dsj *ents = dsj_get(opt, "entities");
        Dsj *auds = dsj_get(opt, "audios");
        int i, ne = dsj_len(ents);
        for (i = 0; i < ne; i++) {
            Dsj *e = dsj_at(ents, i);
            const char *nm = dsj_get_str(e, "name", "");
            Dsj *comps;
            if (!nm || !*nm) continue;
            if (!obj[0]) snprintf(obj, sizeof obj, "%s", nm);
            comps = dsj_get(e, "comps");
            if (!cam[0] && comps && dsj_get(comps, "camera"))
                snprintf(cam, sizeof cam, "%s", nm);
        }
        if (dsj_len(auds) > 0) {
            Dsj *a0 = dsj_at(auds, 0);
            if (a0 && a0->str) snprintf(snd, sizeof snd, "%s", a0->str);
        }
        if (prop_type_of(d, "obj") == 's' && !dsj_get_str(props, "obj", "")[0])
            dsj_set_str(props, "obj", obj);
        if (prop_type_of(d, "cam") == 's' && !dsj_get_str(props, "cam", "")[0])
            dsj_set_str(props, "cam", cam[0] ? cam : obj);
        if (prop_type_of(d, "path") == 's' && !dsj_get_str(props, "path", "")[0])
            dsj_set_str(props, "path", snd);
        if (prop_type_of(d, "comp") == 's' && !dsj_get_str(props, "comp", "")[0])
            dsj_set_str(props, "comp", "transform");
        if (prop_type_of(d, "field") == 's' && !dsj_get_str(props, "field", "")[0])
            dsj_set_str(props, "field", "x");
        if (prop_type_of(d, "action") == 's' && !dsj_get_str(props, "action", "")[0])
            dsj_set_str(props, "action", "jump");
        if (prop_type_of(d, "key") == 'i' && dsj_get_num(props, "key", 0) == 0)
            dsj_set_num(props, "key", 32);            /* 空格 */
        if (prop_type_of(d, "btn") == 'i' && dsj_get_num(props, "btn", 0) == 0)
            dsj_set_num(props, "btn", 256);           /* 鼠标左键 */
        dsj_free(opt);
    }
    dsj_set(n, "props", props);
    dsj_push(nodes, n);
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_int(r, "id", id);
    dsj_set_str(r, "type", type);
    return r;
}

static void unlink_node(DsModel *m, long long id)
{
    Dsj *links = graph_links(m);
    int i = 0;
    while (i < dsj_len(links)) {
        Dsj *l = dsj_at(links, i);
        if (dsj_get_int(l, "from", 0) == id || dsj_get_int(l, "to", 0) == id) {
            dsj_free(l);
            memmove(links->items + i, links->items + i + 1,
                    (size_t)(dsj_len(links) - i - 1) * sizeof *links->items);
            links->n--;
        } else {
            i++;
        }
    }
}

/* 把一组 id(或单个 id)收集成数组,方便批量命令共用 */
static Dsj *ids_arg(Dsj *args)
{
    Dsj *ids = dsj_get(args, "ids");
    Dsj *out = dsj_arr();
    int i;
    if (ids && ids->t == DSJ_ARR) {
        for (i = 0; i < dsj_len(ids); i++) {
            Dsj *it = dsj_at(ids, i);
            if (it && it->t == DSJ_NUM) dsj_push(out, dsj_int((long long)it->num));
        }
    }
    if (!dsj_len(out)) {
        long long id = dsj_get_int(args, "id", 0);
        if (id) dsj_push(out, dsj_int(id));
    }
    return out;
}

static Dsj *cmd_node_remove(DsModel *m, Dsj *args)
{
    Dsj *ids = ids_arg(args);
    Dsj *nodes = graph_nodes(m), *r;
    void *tok;
    int k, removed = 0;
    if (!dsj_len(ids)) {
        dsj_free(ids);
        ds_model_error(m, "graph.node.remove 需要 args.id 或 args.ids");
        return NULL;
    }
    tok = ds_model_edit_open(m);
    for (k = 0; k < dsj_len(ids); k++) {
        long long id = (long long)dsj_at(ids, k)->num;
        int i;
        if (!node_by_id(m, id)) continue;
        unlink_node(m, id);
        for (i = 0; i < dsj_len(nodes); i++) {
            if (dsj_get_int(dsj_at(nodes, i), "id", 0) == id) {
                dsj_free(dsj_at(nodes, i));
                memmove(nodes->items + i, nodes->items + i + 1,
                        (size_t)(dsj_len(nodes) - i - 1) * sizeof *nodes->items);
                nodes->n--;
                removed++;
                break;
            }
        }
    }
    ds_model_edit_close(m, tok, removed ? 1 : 0);
    if (!removed) {
        /* 单个 id 时把 id 报出来(错误信息越具体越好);批量时只说"这些" */
        if (dsj_len(ids) == 1)
            ds_model_error(m, "图里没有节点 %lld", (long long)dsj_at(ids, 0)->num);
        else
            ds_model_error(m, "图里没有这些节点");
        dsj_free(ids);
        return NULL;
    }
    dsj_free(ids);
    r = dsj_obj();
    dsj_set_int(r, "removed", removed);
    return r;
}

/* 再做一个(复制节点 + 它的属性;连线不复制 —— 复制连线往往不是用户想要的)。
 * 支持一次复制多个(框选之后 Ctrl+D)。 */
static Dsj *cmd_node_duplicate(DsModel *m, Dsj *args)
{
    double dx = dsj_get_num(args, "dx", 30);
    double dy = dsj_get_num(args, "dy", 30);
    Dsj *ids = ids_arg(args);
    Dsj *made = dsj_arr(), *r;
    void *tok;
    int k;
    if (!dsj_len(ids)) {
        dsj_free(ids);
        ds_model_error(m, "graph.node.duplicate 需要 args.id 或 args.ids");
        return NULL;
    }
    tok = ds_model_edit_open(m);
    for (k = 0; k < dsj_len(ids); k++) {
        long long id = (long long)dsj_at(ids, k)->num;
        Dsj *n = node_by_id(m, id), *copy;
        long long nid;
        if (!n) continue;
        nid = node_next_id(m);
        copy = dsj_clone(n);
        dsj_set_int(copy, "id", nid);
        dsj_set_num(copy, "x", dsj_get_num(n, "x", 0) + dx);
        dsj_set_num(copy, "y", dsj_get_num(n, "y", 0) + dy);
        dsj_push(graph_nodes(m), copy);
        dsj_push(made, dsj_int(nid));
    }
    ds_model_edit_close(m, tok, dsj_len(made) > 0);
    dsj_free(ids);
    if (!dsj_len(made)) {
        dsj_free(made);
        ds_model_error(m, "图里没有这些节点");
        return NULL;
    }
    r = dsj_obj();
    dsj_set(r, "ids", made);
    dsj_set_int(r, "id", dsj_len(made) ? (long long)dsj_at(made, 0)->num : 0);
    return r;
}

/* 批量移动(拖动一组选中的节点):**一次编辑 = 一条撤销记录** */
static Dsj *cmd_node_move_many(DsModel *m, Dsj *args)
{
    Dsj *items = dsj_get(args, "items");
    Dsj *r = dsj_obj();
    void *tok;
    int i, n = 0;
    if (!items || items->t != DSJ_ARR || dsj_len(items) == 0) {
        ds_model_error(m, "graph.node.move_many 需要 args.items 数组");
        return NULL;
    }
    tok = ds_model_edit_open(m);
    for (i = 0; i < dsj_len(items); i++) {
        Dsj *it = dsj_at(items, i);
        Dsj *n2 = it ? node_by_id(m, dsj_get_int(it, "id", 0)) : NULL;
        if (!n2) continue;
        dsj_set_num(n2, "x", dsj_get_num(it, "x", dsj_get_num(n2, "x", 0)));
        dsj_set_num(n2, "y", dsj_get_num(it, "y", dsj_get_num(n2, "y", 0)));
        n++;
    }
    ds_model_edit_close(m, tok, n > 0);
    dsj_set_int(r, "moved", n);
    return r;
}

static Dsj *cmd_node_set(DsModel *m, Dsj *args)
{
    long long id = dsj_get_int(args, "id", 0);
    Dsj *n = node_by_id(m, id);
    Dsj *in = dsj_get(args, "props");
    const DsgTypeDef *d;
    Dsj *props, *r;
    int i;
    void *tok;
    if (!n) {
        ds_model_error(m, "图里没有节点 %lld", id);
        return NULL;
    }
    d = type_def(dsj_get_str(n, "type", ""));
    if (!d) {
        ds_model_error(m, "节点 %lld 的类型不认识", id);
        return NULL;
    }
    if (!in || in->t != DSJ_OBJ) {
        ds_model_error(m, "graph.node.set 需要 args.props 对象");
        return NULL;
    }
    for (i = 0; i < dsj_len(in); i++) {
        const char *key = in->keys[i];
        if (!prop_type_of(d, key)) {
            ds_model_error(m, "节点类型 '%s' 没有属性 '%s'", d->type, key);
            return NULL;
        }
    }
    tok = ds_model_edit_open(m);
    props = dsj_get(n, "props");
    if (!props || props->t != DSJ_OBJ) {
        props = dsj_obj();
        dsj_set(n, "props", props);
    }
    for (i = 0; i < dsj_len(in); i++) {
        dsj_set(props, in->keys[i], dsj_clone(dsj_at(in, i)));
    }
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_int(r, "id", id);
    dsj_set(r, "props", dsj_clone(props));
    return r;
}

static Dsj *cmd_node_move(DsModel *m, Dsj *args)
{
    long long id = dsj_get_int(args, "id", 0);
    Dsj *n = node_by_id(m, id), *r;
    void *tok;
    if (!n) {
        ds_model_error(m, "图里没有节点 %lld", id);
        return NULL;
    }
    tok = ds_model_edit_open(m);
    dsj_set_num(n, "x", dsj_get_num(args, "x", dsj_get_num(n, "x", 0)));
    dsj_set_num(n, "y", dsj_get_num(args, "y", dsj_get_num(n, "y", 0)));
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_int(r, "id", id);
    dsj_set_num(r, "x", dsj_get_num(n, "x", 0));
    dsj_set_num(r, "y", dsj_get_num(n, "y", 0));
    return r;
}

/* 数据流会不会成环。执行流在连线时就查过了,这里只看数据线(kind="data")。 */
static int data_reaches(DsModel *m, long long from, long long target, int depth)
{
    Dsj *links = graph_links(m);
    int i;
    if (depth > 64) return 1;
    if (from == target) return 1;
    for (i = 0; i < dsj_len(links); i++) {
        Dsj *l = dsj_at(links, i);
        if (dsj_get_int(l, "from", 0) != from) continue;
        if (strcmp(dsj_get_str(l, "kind", ""), "data")) continue;
        if (data_reaches(m, dsj_get_int(l, "to", 0), target, depth + 1)) return 1;
    }
    return 0;
}

static Dsj *cmd_link(DsModel *m, Dsj *args)
{
    long long from = dsj_get_int(args, "from", 0);
    long long to = dsj_get_int(args, "to", 0);
    const char *from_pin = dsj_get_str(args, "from_pin", "");
    const char *to_pin = dsj_get_str(args, "to_pin", "");
    Dsj *a = node_by_id(m, from), *b = node_by_id(m, to);
    const DsgTypeDef *da, *db;
    int a_exec = 0, b_exec = 0;
    Dsj *links, *r;
    int i;
    void *tok;
    if (!a || !b) {
        ds_model_error(m, "连线两端的节点必须都存在");
        return NULL;
    }
    if (from == to) {
        ds_model_error(m, "不能把节点连到自己");
        return NULL;
    }
    da = type_def(dsj_get_str(a, "type", ""));
    db = type_def(dsj_get_str(b, "type", ""));
    if (!da || !db) {
        ds_model_error(m, "节点类型不认识");
        return NULL;
    }
    if (!pin_find(da->out, from_pin, &a_exec)) {
        ds_model_error(m, "节点 '%s' 没有输出引脚 '%s'", da->type, from_pin);
        return NULL;
    }
    if (!pin_find(db->in, to_pin, &b_exec)) {
        ds_model_error(m, "节点 '%s' 没有输入引脚 '%s'", db->type, to_pin);
        return NULL;
    }
    if (a_exec != b_exec) {
        ds_model_error(m, "执行流只能连执行流,数据只能连数据('%s.%s' → '%s.%s')",
                       da->type, from_pin, db->type, to_pin);
        return NULL;
    }
    /* 一个输入引脚只能来一条线(输出可以扇出);执行流尤其如此 */
    links = graph_links(m);
    for (i = 0; i < dsj_len(links); i++) {
        Dsj *l = dsj_at(links, i);
        if (dsj_get_int(l, "to", 0) == to
            && !strcmp(dsj_get_str(l, "to_pin", ""), to_pin)) {
            ds_model_error(m, "'%s.%s' 已经有连线了(先断开)", db->type, to_pin);
            return NULL;
        }
    }
    if (!a_exec && data_reaches(m, to, from, 0)) {
        ds_model_error(m, "这条数据线会成环");
        return NULL;
    }
    if (a_exec) {
        /* 执行流不能成环:生成代码时会无限展开 */
        Dsj *nodes = graph_nodes(m);
        long long cur = to;
        int guard = 0, k;
        (void)nodes;
        while (cur && guard++ < 512) {
            long long next = 0;
            for (k = 0; k < dsj_len(links); k++) {
                Dsj *l = dsj_at(links, k);
                if (dsj_get_int(l, "from", 0) != cur) continue;
                if (!strcmp(dsj_get_str(l, "from_pin", ""), "out")
                    || !strcmp(dsj_get_str(l, "from_pin", ""), "true")
                    || !strcmp(dsj_get_str(l, "from_pin", ""), "false")) {
                    next = dsj_get_int(l, "to", 0);
                }
            }
            if (next == from) {
                ds_model_error(m, "执行流不能成环");
                return NULL;
            }
            cur = next;
        }
    }
    tok = ds_model_edit_open(m);
    {
        Dsj *l = dsj_obj();
        dsj_set_int(l, "from", from);
        dsj_set_str(l, "from_pin", from_pin);
        dsj_set_int(l, "to", to);
        dsj_set_str(l, "to_pin", to_pin);
        dsj_set(l, "kind", dsj_str(a_exec ? "exec" : "data"));
        dsj_push(graph_links(m), l);
    }
    ds_model_edit_close(m, tok, 1);
    r = dsj_obj();
    dsj_set_int(r, "from", from);
    dsj_set_str(r, "from_pin", from_pin);
    dsj_set_int(r, "to", to);
    dsj_set_str(r, "to_pin", to_pin);
    return r;
}

static Dsj *cmd_unlink(DsModel *m, Dsj *args)
{
    long long to = dsj_get_int(args, "to", 0);
    const char *to_pin = dsj_get_str(args, "to_pin", "");
    Dsj *links = graph_links(m);
    Dsj *r;
    int i, removed = 0;
    void *tok = ds_model_edit_open(m);
    for (i = 0; i < dsj_len(links);) {
        Dsj *l = dsj_at(links, i);
        int match = dsj_get_int(l, "to", 0) == to;
        if (match && to_pin && *to_pin) match = !strcmp(dsj_get_str(l, "to_pin", ""), to_pin);
        if (match) {
            dsj_free(l);
            memmove(links->items + i, links->items + i + 1,
                    (size_t)(dsj_len(links) - i - 1) * sizeof *links->items);
            links->n--;
            removed++;
        } else {
            i++;
        }
    }
    ds_model_edit_close(m, tok, removed ? 1 : 0);
    r = dsj_obj();
    dsj_set_int(r, "removed", removed);
    return r;
}

/* ------------------------------------------------------------ 生成前的校验
 *
 * 以前属性留空也能生成:产出 `eng_set_f(eng_find(""), "", "", 3.0)` 这种**能编译但
 * 运行期什么都不做**的代码,用户完全查不出问题在哪(GitHub issue 式体验)。
 * 现在生成前先过一遍:缺什么、引用了哪个不存在的实体,一次性列出来并**带上节点 id**,
 * 前端可以把对应节点标红。graph.validate 把同一份结果交给界面。 */
static int opts_has_entity(Dsj *opts, const char *name)
{
    Dsj *ents = dsj_get(opts, "entities");
    int i;
    if (!name || !*name) return 0;
    for (i = 0; i < dsj_len(ents); i++) {
        if (!strcmp(dsj_get_str(dsj_at(ents, i), "name", ""), name)) return 1;
    }
    return 0;
}

static int opts_has_comp(Dsj *opts, const char *comp)
{
    Dsj *sch = dsj_get(opts, "schema");
    int i;
    if (!comp || !*comp) return 0;
    for (i = 0; i < dsj_len(sch); i++) {
        if (!strcmp(dsj_get_str(dsj_at(sch, i), "name", ""), comp)) return 1;
    }
    return 0;
}

static int opts_has_field(Dsj *opts, const char *comp, const char *field)
{
    Dsj *sch = dsj_get(opts, "schema");
    int i, k;
    Dsj *fields;
    if (!comp || !*comp || !field || !*field) return 0;
    for (i = 0; i < dsj_len(sch); i++) {
        Dsj *co = dsj_at(sch, i);
        if (strcmp(dsj_get_str(co, "name", ""), comp)) continue;
        fields = dsj_get(co, "fields");
        for (k = 0; k < dsj_len(fields); k++) {
            Dsj *f = dsj_at(fields, k);
            if (f && f->str && !strcmp(f->str, field)) return 1;
        }
        return 0;
    }
    return 0;
}

static void issue_add(Dsj *issues, long long id, const char *type, const char *field,
                      const char *level, const char *fmt, ...)
{
    Dsj *o = dsj_obj();
    char msg[512];
    const DsgTypeDef *d = type_def(type);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    dsj_set_int(o, "id", id);
    dsj_set_str(o, "type", type);
    dsj_set_str(o, "title", d && d->title ? d->title : type);
    dsj_set_str(o, "field", field ? field : "");
    dsj_set_str(o, "level", level);
    dsj_set_str(o, "msg", msg);
    dsj_push(issues, o);
}

/* 引用型属性(实体名)分两级:
 *   · 空            → **错误**:生成的代码必然无效;
 *   · 场景里找不到  → **警告**:逻辑图是全局的,而项目可以有多个场景
 *                     (你可能正在别的场景里编逻辑),所以不能拿它拦编译。 */
static void validate_field_ref(Dsj *issues, Dsj *opts, Dsj *n, const char *field)
{
    const char *v = dsj_get_str(dsj_get(n, "props"), field, "");
    if (!v || !*v) {
        issue_add(issues, dsj_get_int(n, "id", 0), dsj_get_str(n, "type", ""), field,
                  "error", "还没有填 %s", field);
        return;
    }
    if (!opts_has_entity(opts, v)) {
        issue_add(issues, dsj_get_int(n, "id", 0), dsj_get_str(n, "type", ""), field,
                  "warn", "当前场景里没有叫 '%s' 的实体(换场景了?还是名字写错了?)", v);
    }
}

/* 把整张图的问题收集到 issues(数组;空 = 没问题) */
static void validate_graph(DsModel *m, Dsj *issues)
{
    Dsj *opts = ds_model_scene_options(m);
    Dsj *nodes = graph_nodes(m);
    int i;
    for (i = 0; i < dsj_len(nodes); i++) {
        Dsj *n = dsj_at(nodes, i);
        const char *type = dsj_get_str(n, "type", "");
        Dsj *p = dsj_get(n, "props");
        long long id = dsj_get_int(n, "id", 0);
        const DsgTypeDef *d = type_def(type);
        if (!d) { issue_add(issues, id, type, "", "error", "节点类型不认识"); continue; }
        if (!strcmp(type, "set_field") || !strcmp(type, "add_field")
            || !strcmp(type, "field")) {
            const char *obj = dsj_get_str(p, "obj", "");
            const char *comp = dsj_get_str(p, "comp", "");
            const char *field = dsj_get_str(p, "field", "");
            validate_field_ref(issues, opts, n, "obj");
            if (!comp || !*comp) {
                issue_add(issues, id, type, "comp", "error", "还没有选组件");
            } else if (!opts_has_comp(opts, comp)) {
                issue_add(issues, id, type, "comp", "error", "没有叫 '%s' 的组件", comp);
            } else if (!field || !*field) {
                issue_add(issues, id, type, "field", "error", "还没有选字段");
            } else if (!opts_has_field(opts, comp, field)) {
                issue_add(issues, id, type, "field", "error", "组件 %s 没有字段 '%s'",
                          comp, field);
            }
            (void)obj;
        } else if (!strcmp(type, "destroy") || !strcmp(type, "on_ground")) {
            validate_field_ref(issues, opts, n, "obj");
        } else if (!strcmp(type, "camera_to")) {
            validate_field_ref(issues, opts, n, "cam");
            validate_field_ref(issues, opts, n, "obj");
        } else if (!strcmp(type, "play_sound")) {
            const char *path = dsj_get_str(p, "path", "");
            if (!path || !*path)
                issue_add(issues, id, type, "path", "error",
                          "还没有选声音(点下面的下拉挑一个 res/ 里的音频)");
        } else if (!strcmp(type, "on_action") || !strcmp(type, "action_down")
                   || !strcmp(type, "action_pressed")) {
            const char *act = dsj_get_str(p, "action", "");
            if (!act || !*act)
                issue_add(issues, id, type, "action", "error", "还没有选动作");
        } else if (!strcmp(type, "on_key") || !strcmp(type, "key_down")) {
            if (dsj_get_num(p, "key", 0) <= 0)
                issue_add(issues, id, type, "key", "error", "还没有选按键");
        } else if (!strcmp(type, "mouse_down")) {
            if (dsj_get_num(p, "btn", 0) <= 0)
                issue_add(issues, id, type, "btn", "error", "还没有选鼠标键");
        }
    }
    dsj_free(opts);
}

/* 把 issues 拼成一句人话(带节点号,前端可以据此高亮)。只有**错误**会拦住生成;
 * 警告(比如"当前场景里没有这个实体")照常生成 —— 逻辑图是全局的,项目可以有多个场景。 */
static void issues_summary(Dsj *issues, int errors, char *out, size_t outsz)
{
    int i, n = dsj_len(issues), shown = 0;
    size_t o = 0;
    if (n <= 0 || errors <= 0) { if (outsz) out[0] = 0; return; }
    o += (size_t)snprintf(out + o, outsz - o, "逻辑图有 %d 处问题:", errors);
    for (i = 0; i < n && shown < 3 && o + 8 < outsz; i++) {
        Dsj *it = dsj_at(issues, i);
        if (strcmp(dsj_get_str(it, "level", "error"), "error")) continue;
        o += (size_t)snprintf(out + o, outsz - o, "\n  · #%lld %s:%s",
                              dsj_get_int(it, "id", 0),
                              dsj_get_str(it, "title", ""),
                              dsj_get_str(it, "msg", ""));
        shown++;
    }
    if (errors > shown) snprintf(out + o, outsz - o, "\n  …还有 %d 处(点「校验」看全部)",
                                 errors - shown);
}

static Dsj *cmd_validate(DsModel *m)
{
    Dsj *issues = dsj_arr();
    Dsj *r = dsj_obj();
    int i, errs = 0, warns = 0;
    validate_graph(m, issues);
    for (i = 0; i < dsj_len(issues); i++) {
        if (!strcmp(dsj_get_str(dsj_at(issues, i), "level", "error"), "error")) errs++;
        else warns++;
    }
    dsj_set_bool(r, "ok", errs == 0);
    dsj_set_int(r, "count", dsj_len(issues));
    dsj_set_int(r, "errors", errs);
    dsj_set_int(r, "warnings", warns);
    dsj_set(r, "issues", issues);
    return r;
}

/* ------------------------------------------------------------ 代码生成 */

typedef struct {
    char *buf;
    size_t len, cap;
    int depth;         /* 缩进层数 */
    int steps;         /* 已展开的节点数(防环兜底) */
} Gen;

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

/* 只写缩进(接在 gput 裸文本前面用,例如一行不完整的 if 头) */
static void gindent(Gen *g)
{
    int i;
    for (i = 0; i < g->depth; i++) gput(g, "    ");
}

/* DexLang 字符串字面量(转义与 Python repr 一致的那几个字符) */
static void gstr(Gen *g, const char *s)
{
    gput(g, "\"");
    for (; s && *s; s++) {
        gput(g, (*s == '"' || *s == '\\') ? "\\" : "");
        {
            char one[2] = {*s, 0};
            gput(g, one);
        }
    }
    gput(g, "\"");
}

/* 数字:整数也写成浮点,免得 int/float 标签在原生调用里被当成两回事 */
static void gnum(Gen *g, double v)
{
    char tmp[48];
    if (v == (double)(long long)v && v < 1e15 && v > -1e15)
        snprintf(tmp, sizeof tmp, "%lld.0", (long long)v);
    else
        snprintf(tmp, sizeof tmp, "%.6g", v);
    gput(g, tmp);
}

static Dsj *find_link_from(DsModel *m, long long node, const char *pin)
{
    Dsj *links = graph_links(m);
    int i;
    for (i = 0; i < dsj_len(links); i++) {
        Dsj *l = dsj_at(links, i);
        if (dsj_get_int(l, "from", 0) == node
            && !strcmp(dsj_get_str(l, "from_pin", ""), pin)) return l;
    }
    return NULL;
}

static Dsj *find_link_to(DsModel *m, long long node, const char *pin)
{
    Dsj *links = graph_links(m);
    int i;
    for (i = 0; i < dsj_len(links); i++) {
        Dsj *l = dsj_at(links, i);
        if (dsj_get_int(l, "to", 0) == node
            && !strcmp(dsj_get_str(l, "to_pin", ""), pin)) return l;
    }
    return NULL;
}

static void emit_chain(DsModel *m, Gen *g, long long node_id, char *err, unsigned errsz);

/* 生成一个"数值表达式":DexLang 里没有 bool,条件用 1/0 表示,比较用 != 0 */
static void emit_expr(DsModel *m, Gen *g, long long node_id, const char *pin,
                      char *err, unsigned errsz)
{
    Dsj *l = find_link_to(m, node_id, pin);
    Dsj *n;
    const char *type;
    Dsj *p;
    if (!l) {
        if (err && !*err) snprintf(err, errsz, "节点 %lld 的 '%s' 没有接东西", node_id, pin);
        gput(g, "0.0");
        return;
    }
    n = node_by_id(m, dsj_get_int(l, "from", 0));
    if (!n) {
        if (err && !*err) snprintf(err, errsz, "连线指向了不存在的节点");
        gput(g, "0.0");
        return;
    }
    type = dsj_get_str(n, "type", "");
    p = dsj_get(n, "props");
    if (!strcmp(type, "num") || !strcmp(type, "number")) {
        gnum(g, dsj_get_num(p, "value", 0));
    } else if (!strcmp(type, "dt")) {
        /* 三个回调都收 dt(秒),所以这个节点在哪个事件里都合法 */
        gput(g, "dt");
    } else if (!strcmp(type, "field")) {
        gput(g, "eng_get_f(eng_find(");
        gstr(g, dsj_get_str(p, "obj", ""));
        gput(g, "), ");
        gstr(g, dsj_get_str(p, "comp", "transform"));
        gput(g, ", ");
        gstr(g, dsj_get_str(p, "field", "x"));
        gput(g, ")");
    } else if (!strcmp(type, "math")) {
        gput(g, "(");
        emit_expr(m, g, dsj_get_int(n, "id", 0), "a", err, errsz);
        gput(g, " ");
        gput(g, dsj_get_str(p, "op", "+"));
        gput(g, " ");
        emit_expr(m, g, dsj_get_int(n, "id", 0), "b", err, errsz);
        gput(g, ")");
    } else if (!strcmp(type, "compare")) {
        /* 比较结果就是 1/0:语言里没有三元运算符(实测 lexer 直接报 '?'),
         * 而布尔表达式可以直接当数值用(见 _zigtmp/probe_syntax.dex 的探针)。 */
        gput(g, "(");
        emit_expr(m, g, dsj_get_int(n, "id", 0), "a", err, errsz);
        gput(g, " ");
        gput(g, dsj_get_str(p, "op", "<"));
        gput(g, " ");
        emit_expr(m, g, dsj_get_int(n, "id", 0), "b", err, errsz);
        gput(g, ")");
    } else if (!strcmp(type, "action_down") || !strcmp(type, "action_pressed")) {
        gput(g, !strcmp(type, "action_down") ? "eng_action_down(" : "eng_action_pressed(");
        gstr(g, dsj_get_str(p, "action", "jump"));
        gput(g, ")");
    } else if (!strcmp(type, "key_down")) {
        gput(g, "eng_key_down(");
        gnum(g, dsj_get_num(p, "key", 32));
        gput(g, ")");
    } else if (!strcmp(type, "mouse_down")) {
        gput(g, "eng_mouse_down(");
        gnum(g, dsj_get_num(p, "btn", 0));
        gput(g, ")");
    } else if (!strcmp(type, "on_ground")) {
        gput(g, "eng_on_ground(eng_find(");
        gstr(g, dsj_get_str(p, "obj", ""));
        gput(g, "))");
    } else if (!strcmp(type, "mouse_world")) {
        const char *axis = dsj_get_str(p, "axis", "x");
        gput(g, !strcmp(axis, "y") ? "eng_mouse_world_y()" : "eng_mouse_world_x()");
    } else {
        if (err && !*err) snprintf(err, errsz, "节点类型 '%s' 不能当数值用", type);
        gput(g, "0.0");
    }
}

/* 把一个字段写成 DexLang 语句(不含分号,gline 之前由调用方补) */ 
static void emit_set_field(Gen *g, Dsj *p, const char *value_expr, int is_add)
{
    int as_int = !strcmp(dsj_get_str(p, "as", "f"), "i");
    const char *fn = as_int ? "eng_set_i" : "eng_set_f";
    if (is_add) {
        /* 读-改-写:没有 "+=" 这种语法,展开成一句 */
        if (as_int) {
            gput(g, "eng_set_i(eng_find(");
            gstr(g, dsj_get_str(p, "obj", ""));
            gput(g, "), ");
            gstr(g, dsj_get_str(p, "comp", "transform"));
            gput(g, ", ");
            gstr(g, dsj_get_str(p, "field", "x"));
            gput(g, ", eng_get_i(eng_find(");
            gstr(g, dsj_get_str(p, "obj", ""));
            gput(g, "), ");
            gstr(g, dsj_get_str(p, "comp", "transform"));
            gput(g, ", ");
            gstr(g, dsj_get_str(p, "field", "x"));
            gput(g, ") + ");
            gput(g, value_expr);
            gput(g, ")");
            return;
        }
        gput(g, fn);
        gput(g, "(eng_find(");
        gstr(g, dsj_get_str(p, "obj", ""));
        gput(g, "), ");
        gstr(g, dsj_get_str(p, "comp", "transform"));
        gput(g, ", ");
        gstr(g, dsj_get_str(p, "field", "x"));
        gput(g, ", eng_get_f(eng_find(");
        gstr(g, dsj_get_str(p, "obj", ""));
        gput(g, "), ");
        gstr(g, dsj_get_str(p, "comp", "transform"));
        gput(g, ", ");
        gstr(g, dsj_get_str(p, "field", "x"));
        gput(g, ") + ");
        gput(g, value_expr);
        gput(g, ")");
        return;
    }
    gput(g, fn);
    gput(g, "(eng_find(");
    gstr(g, dsj_get_str(p, "obj", ""));
    gput(g, "), ");
    gstr(g, dsj_get_str(p, "comp", "transform"));
    gput(g, ", ");
    gstr(g, dsj_get_str(p, "field", "x"));
    gput(g, ", ");
    gput(g, value_expr);
    gput(g, ")");
}

/* 展开一条执行链;每条语句用 gline 落一行(缩进由 gline 负责) */
static void emit_chain(DsModel *m, Gen *g, long long node_id, char *err, unsigned errsz)
{
    long long cur = node_id;
    while (cur) {
        Dsj *n = node_by_id(m, cur);
        const char *type;
        Dsj *p;
        Dsj *next;
        Gen st;
        if (!n) return;
        if (++g->steps > 4096) {
            if (err && !*err) snprintf(err, errsz, "逻辑图太大或成环(展开超过 4096 步)");
            return;
        }
        type = dsj_get_str(n, "type", "");
        p = dsj_get(n, "props");
        memset(&st, 0, sizeof st);
        if (!strcmp(type, "set_field") || !strcmp(type, "add_field")) {
            /* "引脚优先,没接就用属性里的字面值":这样单放一个"写字段"节点也能用,
             * 想接表达式再接 —— 少一层"必须连线"的摩擦。 */
            int is_add = !strcmp(type, "add_field");
            const char *pinname = is_add ? "delta" : "value";
            Gen val;
            memset(&val, 0, sizeof val);
            /* 注意:分支里 emit_expr 会往 err 写"没接东西",所以先看有没有连线 */
            if (find_link_to(m, cur, pinname)) {
                emit_expr(m, &val, cur, pinname, err, errsz);
            } else {
                gnum(&val, dsj_get_num(p, is_add ? "delta" : "value", 0));
            }
            emit_set_field(&st, p, val.buf ? val.buf : "0.0", is_add);
            free(val.buf);
            gput(&st, ";");
            gline(g, st.buf ? st.buf : "0;");
        } else if (!strcmp(type, "destroy")) {
            gput(&st, "eng_object_free(eng_find(");
            gstr(&st, dsj_get_str(p, "obj", ""));
            gput(&st, "));");
            gline(g, st.buf);
        } else if (!strcmp(type, "play_sound")) {
            gput(&st, "eng_sound_play(eng_sound_load(");
            gstr(&st, dsj_get_str(p, "path", ""));
            gput(&st, "), ");
            gnum(&st, dsj_get_num(p, "loop", 0));
            gput(&st, ", ");
            gnum(&st, dsj_get_num(p, "volume", 1));
            gput(&st, ");");
            gline(g, st.buf);
        } else if (!strcmp(type, "camera_to")) {
            /* 相机是"(x,y) = 屏幕左上角的世界坐标",所以跟随 = 目标世界坐标 + 偏移 */
            int idx;
            static const char *axis[2] = {"x", "y"};
            for (idx = 0; idx < 2; idx++) {
                Gen one;
                memset(&one, 0, sizeof one);
                gput(&one, "eng_set_f(eng_find(");
                gstr(&one, dsj_get_str(p, "cam", ""));
                gput(&one, "), \"camera\", \"");
                gput(&one, axis[idx]);
                gput(&one, "\", eng_world_");
                gput(&one, axis[idx]);
                gput(&one, "(eng_find(");
                gstr(&one, dsj_get_str(p, "obj", ""));
                gput(&one, ")) + ");
                gnum(&one, dsj_get_num(p, idx ? "oy" : "ox", 0));
                gput(&one, "));");
                gline(g, one.buf);
                free(one.buf);
            }
        } else if (!strcmp(type, "branch")) {
            Gen cond;
            memset(&cond, 0, sizeof cond);
            if (find_link_to(m, cur, "cond")) {
                emit_expr(m, &cond, cur, "cond", err, errsz);
            } else {
                gnum(&cond, dsj_get_num(p, "cond", 0));   /* 没接线就用属性里的常数 */
            }
            gindent(g);
            gput(g, "if (");
            gput(g, cond.buf ? cond.buf : "0.0");
            gput(g, ") != 0 {\n");
            free(cond.buf);
            next = find_link_from(m, cur, "true");
            g->depth++;
            if (next) emit_chain(m, g, dsj_get_int(next, "to", 0), err, errsz);
            g->depth--;
            next = find_link_from(m, cur, "false");
            if (next) {
                gline(g, "} else {");
                g->depth++;
                emit_chain(m, g, dsj_get_int(next, "to", 0), err, errsz);
                g->depth--;
            }
            gline(g, "}");
            free(st.buf);
            return;   /* 分支的两条路各自结束(一期不支持分支后再汇合) */
        } else {
            if (err && !*err) snprintf(err, errsz, "节点类型 '%s' 还不能生成代码", type);
            free(st.buf);
            return;
        }
        free(st.buf);
        next = find_link_from(m, cur, "out");
        cur = next ? dsj_get_int(next, "to", 0) : 0;
    }
}

/* 收集某一类事件节点,按 (y,x,id) 排序保证确定性 */
static int collect_events(DsModel *m, const char *type, Dsj *out)
{
    Dsj *nodes = graph_nodes(m);
    int i, n = 0;
    for (i = 0; i < dsj_len(nodes); i++) {
        Dsj *nd = dsj_at(nodes, i);
        if (!strcmp(dsj_get_str(nd, "type", ""), type)) {
            dsj_push(out, dsj_clone(nd));
            n++;
        }
    }
    /* 冒泡排序(节点数很少;dsj 数组没有通用排序) */
    for (i = 0; i < dsj_len(out); i++) {
        int j;
        for (j = 0; j + 1 < dsj_len(out); j++) {
            Dsj *a = dsj_at(out, j), *b = dsj_at(out, j + 1);
            double ay = dsj_get_num(a, "y", 0), by = dsj_get_num(b, "y", 0);
            double ax = dsj_get_num(a, "x", 0), bx = dsj_get_num(b, "x", 0);
            int swap = (by < ay) || (by == ay && bx < ax)
                || (by == ay && bx == ax
                    && dsj_get_int(b, "id", 0) < dsj_get_int(a, "id", 0));
            if (swap) {
                Dsj tmp = *a;
                *a = *b;
                *b = tmp;
            }
        }
    }
    return n;
}

/* 事件节点本身不产出代码,链要从它 out 引脚指向的第一个节点开始 */
static long long first_target(DsModel *m, long long event_id)
{
    Dsj *l = find_link_from(m, event_id, "out");
    return l ? dsj_get_int(l, "to", 0) : 0;
}

/* 把一类事件的链全部展开(事件之间按 (y,x,id) 排序,见 collect_events) */
static void emit_event_chains(DsModel *m, Gen *g, const char *event_type, char *err,
                              unsigned errsz)
{
    Dsj *evs = dsj_arr();
    int i;
    collect_events(m, event_type, evs);
    if (!dsj_len(evs)) gline(g, "# (这个事件还没有节点)");
    for (i = 0; i < dsj_len(evs); i++) {
        long long t = first_target(m, dsj_get_int(dsj_at(evs, i), "id", 0));
        if (t) emit_chain(m, g, t, err, errsz);
    }
    dsj_free(evs);
}

char *ds_graph_generate(DsModel *m, char *err, unsigned errsz)
{
    Gen g;
    Dsj *issues;
    int uses_action = 0;
    memset(&g, 0, sizeof g);
    if (err) *err = 0;

    /* 先生成前校验:缺属性(错误)→ 直接失败并**说清是哪个节点**。
     * "实体不在当前场景"只是警告(多场景项目里很常见),不拦。 */
    issues = dsj_arr();
    validate_graph(m, issues);
    {
        int i, errs = 0;
        for (i = 0; i < dsj_len(issues); i++) {
            if (!strcmp(dsj_get_str(dsj_at(issues, i), "level", "error"), "error")) errs++;
        }
        if (errs > 0) {
            if (err) issues_summary(issues, errs, err, errsz);
            dsj_free(issues);
            return NULL;
        }
    }
    dsj_free(issues);

    /* 用到动作类节点时才 include dexgame_fast 并绑默认键位(与 main.dex 里那次
     * 重复也没关系:DexLang 的 include 会去重)。以前谁都没绑,于是
     * eng_action_pressed("jump") 永远返回 0 —— 图连好了、按空格没反应。 */
    {
        Dsj *nodes = graph_nodes(m);
        int i;
        for (i = 0; i < dsj_len(nodes); i++) {
            const char *t = dsj_get_str(dsj_at(nodes, i), "type", "");
            if (!strcmp(t, "on_action") || !strcmp(t, "action_down")
                || !strcmp(t, "action_pressed")) { uses_action = 1; break; }
        }
    }

    gput(&g, "# 由 DexStudio 的逻辑图生成 —— 不要手改(改图,或者改 main.dex)\n");
    gput(&g, "include \"dexgame\";\n");
    if (uses_action) gput(&g, "include \"dexgame_fast\";\n");
    gput(&g, "\n");

    gline(&g, "func logic_start(dt: float) {");
    g.depth++;
    if (uses_action) {
        gline(&g, "# 默认键位(与 dexgame_fast 的 eng_bind_default_actions 同一份)");
        gline(&g, "eng_bind_default_actions();");
    }
    emit_event_chains(m, &g, "on_start", err, errsz);
    g.depth--;
    gline(&g, "}");

    gline(&g, "func logic_update(dt: float) {");
    g.depth++;
    emit_event_chains(m, &g, "on_update", err, errsz);
    /* on_key / on_action 用"帧内轮询"实现(DexLang 没有事件回调,引擎给的是轮询 API)。
     * 这两个事件因此并入 logic_update:位置就是它们的先后。 */
    {
        static const char *polled[2] = {"on_action", "on_key"};
        int k;
        for (k = 0; k < 2; k++) {
            Dsj *list = dsj_arr();
            int i;
            collect_events(m, polled[k], list);
            for (i = 0; i < dsj_len(list); i++) {
                Dsj *nd = dsj_at(list, i);
                Dsj *p = dsj_get(nd, "props");
                long long id = dsj_get_int(nd, "id", 0);
                long long t = first_target(m, id);
                gindent(&g);
                gput(&g, "if (");
                if (!strcmp(polled[k], "on_action")) {
                    gput(&g, "eng_action_pressed(");
                    gstr(&g, dsj_get_str(p, "action", "jump"));
                    gput(&g, ")");
                } else {
                    gput(&g, "eng_key_pressed(");
                    gnum(&g, dsj_get_num(p, "key", 32));
                    gput(&g, ")");
                }
                gput(&g, ") != 0 {\n");
                g.depth++;
                if (t) emit_chain(m, &g, t, err, errsz);
                g.depth--;
                gline(&g, "}");
            }
            dsj_free(list);
        }
    }
    g.depth--;
    gline(&g, "}");

    gline(&g, "func logic_draw(dt: float) {");
    g.depth++;
    emit_event_chains(m, &g, "on_draw", err, errsz);
    g.depth--;
    gline(&g, "}");

    if (err && *err) {   /* 生成过程中发现了问题:把源码丢掉,只报错 */
        free(g.buf);
        return NULL;
    }
    if (!g.buf) g.buf = calloc(1, 1);
    return g.buf;
}

/* ------------------------------------------------------------ 命令分发 */

static Dsj *cmd_generate(DsModel *m);

/* 生成 .dex 并落盘(顺带把图存盘,保证"生成的代码"与"存下来的图"一致)。 */
int ds_graph_generate_to_file(DsModel *m, char *out_path, unsigned pathsz,
                              char *err, unsigned errsz)
{
    const char *root = ds_project_dir(m);
    char *src;
    char path[1400];
    if (!root || !*root) {
        if (err) snprintf(err, errsz, "还没有打开项目,生成的代码没有地方放");
        return 0;
    }
    if (!graph_write(m, err, errsz)) return 0;
    src = ds_graph_generate(m, err, errsz);
    if (!src) return 0;
    snprintf(path, sizeof path, "%s\\%s", root, DS_GRAPH_DEX_REL);
    if (out_path && pathsz) snprintf(out_path, pathsz, "%s", path);
    if (!ds_write_text(path, src)) {
        if (err) snprintf(err, errsz, "无法写入 %s", path);
        free(src);
        return 0;
    }
    free(src);
    return 1;
}

Dsj *ds_graph_command(DsModel *m, const char *cmd, Dsj *args)
{
    if (!strcmp(cmd, "graph.types")) return cmd_types(m);
    if (!strcmp(cmd, "graph.options")) return cmd_options(m);
    if (!strcmp(cmd, "graph.validate")) return cmd_validate(m);
    if (!strcmp(cmd, "graph.new")) return cmd_graph_new(m);
    if (!strcmp(cmd, "graph.info")) return cmd_graph_info(m);
    if (!strcmp(cmd, "graph.save")) return cmd_graph_save(m);
    if (!strcmp(cmd, "graph.node.add")) return cmd_node_add(m, args);
    if (!strcmp(cmd, "graph.node.remove")) return cmd_node_remove(m, args);
    if (!strcmp(cmd, "graph.node.duplicate")) return cmd_node_duplicate(m, args);
    if (!strcmp(cmd, "graph.node.set")) return cmd_node_set(m, args);
    if (!strcmp(cmd, "graph.node.move")) return cmd_node_move(m, args);
    if (!strcmp(cmd, "graph.node.move_many")) return cmd_node_move_many(m, args);
    if (!strcmp(cmd, "graph.link")) return cmd_link(m, args);
    if (!strcmp(cmd, "graph.unlink")) return cmd_unlink(m, args);
    if (!strcmp(cmd, "graph.generate")) return cmd_generate(m);
    ds_model_error(m, "未知命令 '%s'", cmd);
    return NULL;
}

static Dsj *cmd_generate(DsModel *m)
{
    char err[512];
    char path[1400];
    char *src;
    Dsj *r;
    if (!ds_graph_generate_to_file(m, path, sizeof path, err, sizeof err)) {
        ds_model_error(m, "%s", err);
        return NULL;
    }
    src = ds_read_text(path, NULL);
    r = dsj_obj();
    dsj_set_str(r, "path", path);
    dsj_set_str(r, "source", src ? src : "");
    dsj_set_int(r, "lines", 0);
    free(src);
    return r;
}
