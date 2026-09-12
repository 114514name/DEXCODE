/* dg_scene.c — 实体 / 组件 / 描述符驱动的场景读写(M2 核心)
 *
 * 模型(见 docs/DEXGAME_DESIGN.md §5):
 *   - Unity 式:**GameObject + 挂载组件**。组件全部 C 内置。
 *   - 实体 id 带**世代**:id = (gen << 16) | index。释放后旧 id 立即失效,
 *     且槽位复用时 gen 递增 —— 不会出现"旧 id 误指新对象"(现有 6 个库的 id
 *     单调不复用正是已知坑)。
 *   - 组件按类型分池(AoS),每池两张索引表:槽→实体、实体→槽。
 *
 * 描述符驱动(docs/DEXGAME_DESIGN.md §7 的结论):
 *   每种组件一张 `DgField` 表 = 字段名 / 类型 / 是否持久 / 大小 / 偏移。
 *   JSON 的读与写都**遍历同一张表**,所以以后加一个字段只需往表里加一行,
 *   两种方向自动同步 —— 这正是"双格式值得做"的前提条件。
 *   运行时字段(如精灵解析后的纹理句柄)标 persist=0,不进 JSON;
 *   反序列化后由组件的 on_load 钩子重建(纹理路径→句柄、父实体索引→实体 id)。
 *
 * 为什么 JSON 的"可缺可多"很重要(§7.4):读取端遇到不认识的字段用
 * djr_skip_value() 跳过,所以**新版本的场景文件能被旧版本读、旧文件也能被新版本读**。
 * 二进制格式做不到这一点,这也是"JSON 先、二进制后"的一个理由。
 */
#include "dexgame.h"
#include "dg_json.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================ 实体 ============================ */
#define DG_MAX_OBJECTS 4096

typedef struct {
    uint32_t gen;      /* 世代号:每次创建 +1,永不为 0 */
    int32_t  live;
} DgEntSlot;

static DgEntSlot g_ents[DG_MAX_OBJECTS + 1];   /* 1 基索引,0 号不用 */
static int32_t   g_live_count = 0;

static uint32_t dg_ent_gen(uint32_t id) { return id >> 16; }
static int32_t  dg_ent_idx(uint32_t id) { return (int32_t)(id & 0xFFFF); }

int32_t dg_object_alive(uint32_t id) {
    const int32_t i = dg_ent_idx(id);
    if (i <= 0 || i > DG_MAX_OBJECTS) return 0;
    return g_ents[i].live && g_ents[i].gen == dg_ent_gen(id);
}

uint32_t dg_object_new(void) {
    for (int32_t i = 1; i <= DG_MAX_OBJECTS; i++) {
        if (!g_ents[i].live && g_ents[i].gen != 0) continue;   /* 已用过的空闲槽 */
        if (!g_ents[i].live) {
            g_ents[i].gen++;
            if (g_ents[i].gen == 0) g_ents[i].gen = 1;
            g_ents[i].live = 1;
            g_live_count++;
            return (g_ents[i].gen << 16) | (uint32_t)i;
        }
    }
    dg_error("object limit reached (max %d)", DG_MAX_OBJECTS);
    return 0;
}

int32_t dg_object_count(void) { return g_live_count; }

/* ============================ 字段描述符 ============================ */
static void dg_comp_on_unload(int kind, void *comp, int32_t *id_map, int n);

/* ============================ 组件池 ============================ */
#define DG_MAX_COMPS 4096

typedef struct {
    const char    *name;
    int            kind;
    const DgField *fields;
    int            nfields;
    int            stride;
    uint8_t       *data;
    int32_t       *owner;    /* 槽 → 实体索引(0 = 空闲)*/
    int32_t       *slot;     /* 实体索引 → 槽(-1 = 无)*/
    int            count, cap;
    int            used[DG_MAX_OBJECTS + 1];   /* 实体是否挂了本组件 */
} DgPool;

static DgPool g_pools[DG_C_COUNT];
static int   g_pools_ready = 0;

/* ============================ 组件结构 ============================ */
typedef struct { float x, y, rot, sx, sy; int32_t parent; } DgTransform;
typedef struct {
    char    tex_path[DG_PATH_MAX];
    int32_t texture;                 /* 运行时 */
    float   sx, sy, sw, sh;          /* 源矩形;sw/sh = 0 表示整张 */
    float   px, py;
    int32_t tint;
    int32_t flip;
    int32_t layer, order;
} DgSprite;
typedef struct { float x, y, zoom, rot; int32_t active; } DgCamera;
typedef struct { int32_t first, count; float fps, time; int32_t loop; } DgAnimation;
/* Collider/Body 在 M2 只是**数据**:让场景能预先带上物理属性,M3 才有东西可读。*/
typedef struct { int32_t kind; float hw, hh, ox, oy; int32_t is_trigger, layer, mask; } DgCollider;
typedef struct { int32_t motion; float vx, vy, gravity_scale, friction, restitution;
                 int32_t sleeping; } DgBody;

static const DgField TR_FIELDS[] = {
    { "x",      DG_F_FLOAT, 1, 0, offsetof(DgTransform, x) },
    { "y",      DG_F_FLOAT, 1, 0, offsetof(DgTransform, y) },
    { "rot",    DG_F_FLOAT, 1, 0, offsetof(DgTransform, rot) },
    { "sx",     DG_F_FLOAT, 1, 0, offsetof(DgTransform, sx) },
    { "sy",     DG_F_FLOAT, 1, 0, offsetof(DgTransform, sy) },
    /* parent 在 JSON 里存的是**场景内索引**,load 时经 on_load 重映射为实体 id */
    { "parent", DG_F_INT,   1, 0, offsetof(DgTransform, parent) },
};
static const DgField SP_FIELDS[] = {
    { "tex_path", DG_F_STR,   1, (uint16_t)sizeof(((DgSprite *)0)->tex_path),
      offsetof(DgSprite, tex_path) },
    { "texture",  DG_F_INT,   0, 0, offsetof(DgSprite, texture) },   /* 运行时 */
    { "sx",       DG_F_FLOAT, 1, 0, offsetof(DgSprite, sx) },
    { "sy",       DG_F_FLOAT, 1, 0, offsetof(DgSprite, sy) },
    { "sw",       DG_F_FLOAT, 1, 0, offsetof(DgSprite, sw) },
    { "sh",       DG_F_FLOAT, 1, 0, offsetof(DgSprite, sh) },
    { "px",       DG_F_FLOAT, 1, 0, offsetof(DgSprite, px) },
    { "py",       DG_F_FLOAT, 1, 0, offsetof(DgSprite, py) },
    { "tint",     DG_F_INT,   1, 0, offsetof(DgSprite, tint) },
    { "flip",     DG_F_INT,   1, 0, offsetof(DgSprite, flip) },
    { "layer",    DG_F_INT,   1, 0, offsetof(DgSprite, layer) },
    { "order",    DG_F_INT,   1, 0, offsetof(DgSprite, order) },
};
static const DgField CAM_FIELDS[] = {
    { "x",      DG_F_FLOAT, 1, 0, offsetof(DgCamera, x) },
    { "y",      DG_F_FLOAT, 1, 0, offsetof(DgCamera, y) },
    { "zoom",   DG_F_FLOAT, 1, 0, offsetof(DgCamera, zoom) },
    { "rot",    DG_F_FLOAT, 1, 0, offsetof(DgCamera, rot) },
    { "active", DG_F_INT,   1, 0, offsetof(DgCamera, active) },
};
static const DgField ANIM_FIELDS[] = {
    { "first", DG_F_INT,   1, 0, offsetof(DgAnimation, first) },
    { "count", DG_F_INT,   1, 0, offsetof(DgAnimation, count) },
    { "fps",   DG_F_FLOAT, 1, 0, offsetof(DgAnimation, fps) },
    { "loop",  DG_F_INT,   1, 0, offsetof(DgAnimation, loop) },
    { "time",  DG_F_FLOAT, 0, 0, offsetof(DgAnimation, time) },   /* 运行时 */
};
static const DgField COL_FIELDS[] = {
    { "kind",       DG_F_INT,   1, 0, offsetof(DgCollider, kind) },
    { "hw",         DG_F_FLOAT, 1, 0, offsetof(DgCollider, hw) },
    { "hh",         DG_F_FLOAT, 1, 0, offsetof(DgCollider, hh) },
    { "ox",         DG_F_FLOAT, 1, 0, offsetof(DgCollider, ox) },
    { "oy",         DG_F_FLOAT, 1, 0, offsetof(DgCollider, oy) },
    { "is_trigger", DG_F_INT,   1, 0, offsetof(DgCollider, is_trigger) },
    { "layer",      DG_F_INT,   1, 0, offsetof(DgCollider, layer) },
    { "mask",       DG_F_INT,   1, 0, offsetof(DgCollider, mask) },
};
static const DgField BODY_FIELDS[] = {
    { "motion",        DG_F_INT,   1, 0, offsetof(DgBody, motion) },
    { "vx",            DG_F_FLOAT, 1, 0, offsetof(DgBody, vx) },
    { "vy",            DG_F_FLOAT, 1, 0, offsetof(DgBody, vy) },
    { "gravity_scale", DG_F_FLOAT, 1, 0, offsetof(DgBody, gravity_scale) },
    { "friction",      DG_F_FLOAT, 1, 0, offsetof(DgBody, friction) },
    { "restitution",   DG_F_FLOAT, 1, 0, offsetof(DgBody, restitution) },
    { "sleeping",      DG_F_INT,   1, 0, offsetof(DgBody, sleeping) },
};

#define NFIELDS(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* 组件默认值:新建时把整个结构体 memset 成这些初值 */
static void dg_comp_defaults(int kind, void *c) {
    memset(c, 0, g_pools[kind].stride);
    switch (kind) {
    case DG_C_TRANSFORM:
        ((DgTransform *)c)->sx = 1.0f;
        ((DgTransform *)c)->sy = 1.0f;
        ((DgTransform *)c)->parent = -1;
        break;
    case DG_C_SPRITE:
        ((DgSprite *)c)->texture = -1;
        ((DgSprite *)c)->px = 0.5f;
        ((DgSprite *)c)->py = 0.5f;
        ((DgSprite *)c)->tint = (int32_t)DG_RGB(255, 255, 255);
        break;
    case DG_C_CAMERA:
        ((DgCamera *)c)->zoom = 1.0f;
        ((DgCamera *)c)->active = 1;
        break;
    case DG_C_ANIMATION:
        ((DgAnimation *)c)->count = 1;
        ((DgAnimation *)c)->fps = 8.0f;
        ((DgAnimation *)c)->loop = 1;
        break;
    case DG_C_BODY:
        ((DgBody *)c)->gravity_scale = 1.0f;
        ((DgBody *)c)->friction = 0.6f;
        ((DgBody *)c)->restitution = 0.2f;
        break;
    default:
        break;
    }
}

void dg_scene_init(void) {
    if (g_pools_ready) return;
    static const struct { const char *n; int k; const DgField *f; int nf; int stride; } DEF[] = {
        { "transform", DG_C_TRANSFORM, TR_FIELDS,  NFIELDS(TR_FIELDS),  (int)sizeof(DgTransform) },
        { "sprite",    DG_C_SPRITE,    SP_FIELDS,  NFIELDS(SP_FIELDS),  (int)sizeof(DgSprite) },
        { "camera",    DG_C_CAMERA,    CAM_FIELDS, NFIELDS(CAM_FIELDS), (int)sizeof(DgCamera) },
        { "animation", DG_C_ANIMATION, ANIM_FIELDS,NFIELDS(ANIM_FIELDS),(int)sizeof(DgAnimation) },
        { "collider",  DG_C_COLLIDER,  COL_FIELDS, NFIELDS(COL_FIELDS), (int)sizeof(DgCollider) },
        { "body",      DG_C_BODY,      BODY_FIELDS,NFIELDS(BODY_FIELDS),(int)sizeof(DgBody) },
    };
    for (size_t i = 0; i < sizeof(DEF) / sizeof(DEF[0]); i++) {
        DgPool *p = &g_pools[DEF[i].k];
        p->name = DEF[i].n;
        p->kind = DEF[i].k;
        p->fields = DEF[i].f;
        p->nfields = DEF[i].nf;
        p->stride = DEF[i].stride;
        p->cap = DG_MAX_COMPS;
        p->data = (uint8_t *)calloc((size_t)p->cap, (size_t)p->stride);
        p->owner = (int32_t *)calloc((size_t)p->cap, sizeof(int32_t));
        p->slot = (int32_t *)malloc(((size_t)DG_MAX_OBJECTS + 1) * sizeof(int32_t));
        if (!p->data || !p->owner || !p->slot) {
            dg_error("out of memory initializing component pool '%s'", p->name);
            /* 已经建好的池子要回收,否则失败后重试会泄漏(init 只认 g_pools_ready) */
            for (int k = 0; k < DG_C_COUNT; k++) {
                free(g_pools[k].data);  g_pools[k].data = NULL;
                free(g_pools[k].owner); g_pools[k].owner = NULL;
                free(g_pools[k].slot);  g_pools[k].slot = NULL;
            }
            return;
        }
        for (int j = 0; j <= DG_MAX_OBJECTS; j++) p->slot[j] = -1;
    }
    g_pools_ready = 1;
}

void dg_scene_shutdown(void) {
    for (int k = 0; k < DG_C_COUNT; k++) {
        free(g_pools[k].data);   g_pools[k].data = NULL;
        free(g_pools[k].owner);  g_pools[k].owner = NULL;
        free(g_pools[k].slot);   g_pools[k].slot = NULL;
    }
    g_pools_ready = 0;
    memset(g_ents, 0, sizeof g_ents);
    g_live_count = 0;
}

int dg_comp_kind(const char *name) {
    if (!name || !g_pools_ready) return -1;
    for (int k = 1; k < DG_C_COUNT; k++)
        if (g_pools[k].name && strcmp(g_pools[k].name, name) == 0) return k;
    dg_error("unknown component '%s' (have: transform/sprite/camera/animation/collider/body)",
             name ? name : "(null)");
    return -1;
}

const char *dg_comp_name(int kind) {
    return (kind > 0 && kind < DG_C_COUNT && g_pools[kind].name) ? g_pools[kind].name : "";
}

/* ---------- 描述符表自省(给 IDE 生成属性面板用) ---------- */
int dg_comp_field_count(int kind) {
    if (kind <= 0 || kind >= DG_C_COUNT) return -1;
    return g_pools[kind].nfields;
}

const char *dg_comp_field_name(int kind, int i) {
    if (kind <= 0 || kind >= DG_C_COUNT) return "";
    const DgPool *p = &g_pools[kind];
    if (i < 0 || i >= p->nfields) { dg_error("field index %d out of range 0..%d for '%s'", i, p->nfields - 1, p->name); return ""; }
    return p->fields[i].name;
}

int dg_comp_field_type(int kind, int i) {
    if (kind <= 0 || kind >= DG_C_COUNT) return -1;
    const DgPool *p = &g_pools[kind];
    if (i < 0 || i >= p->nfields) { dg_error("field index %d out of range for '%s'", i, p->name); return -1; }
    return (int)p->fields[i].type;
}

int dg_comp_field_persist(int kind, int i) {
    if (kind <= 0 || kind >= DG_C_COUNT) return -1;
    const DgPool *p = &g_pools[kind];
    if (i < 0 || i >= p->nfields) { dg_error("field index %d out of range for '%s'", i, p->name); return -1; }
    return (int)p->fields[i].persist;
}

/* ---------- 挂载 / 卸载 ---------- */
void *dg_comp_add(uint32_t obj, int kind) {
    if (!g_pools_ready || kind <= 0 || kind >= DG_C_COUNT) { dg_error("bad component kind %d", kind); return NULL; }
    if (!dg_object_alive(obj)) { dg_error("object %u is not alive", obj); return NULL; }
    DgPool *p = &g_pools[kind];
    const int32_t ei = dg_ent_idx(obj);
    if (p->used[ei]) { dg_error("object already has '%s'", p->name); return NULL; }

    int32_t s = -1;
    for (int i = 0; i < p->count; i++) if (p->owner[i] == 0) { s = i; break; }
    if (s < 0) {
        if (p->count >= p->cap) { dg_error("component '%s' limit reached (max %d)", p->name, p->cap); return NULL; }
        s = p->count++;
    }
    void *c = p->data + (size_t)s * p->stride;
    dg_comp_defaults(kind, c);
    p->owner[s] = ei;
    p->slot[ei] = s;
    p->used[ei] = 1;
    return c;
}

void *dg_comp_get(uint32_t obj, int kind) {
    if (!g_pools_ready || kind <= 0 || kind >= DG_C_COUNT) return NULL;
    if (!dg_object_alive(obj)) return NULL;
    DgPool *p = &g_pools[kind];
    const int32_t ei = dg_ent_idx(obj);
    if (!p->used[ei]) return NULL;
    const int32_t s = p->slot[ei];
    if (s < 0 || s >= p->count || p->owner[s] != ei) return NULL;
    return p->data + (size_t)s * p->stride;
}

int32_t dg_comp_remove(uint32_t obj, int kind) {
    if (!g_pools_ready || kind <= 0 || kind >= DG_C_COUNT) { dg_error("bad component kind"); return -1; }
    DgPool *p = &g_pools[kind];
    if (!dg_object_alive(obj)) { dg_error("object %u is not alive", obj); return -1; }
    const int32_t ei = dg_ent_idx(obj);
    if (!p->used[ei]) { dg_error("object %u has no '%s'", obj, p->name); return -1; }
    const int32_t s = p->slot[ei];
    dg_comp_on_unload(kind, p->data + (size_t)s * p->stride, NULL, 0);
    p->owner[s] = 0;
    p->slot[ei] = -1;
    p->used[ei] = 0;
    return 0;
}

int32_t dg_object_free(uint32_t obj) {
    if (!dg_object_alive(obj)) { dg_error("object %u is not alive", obj); return -1; }
    /* 先摘掉所有组件(也顺带释放运行时资源,如纹理句柄的引用)*/
    for (int k = 1; k < DG_C_COUNT; k++)
        if (g_pools[k].used[dg_ent_idx(obj)]) dg_comp_remove(obj, k);
    const int32_t ei = dg_ent_idx(obj);
    g_ents[ei].live = 0;
    /* gen 保持不变:下次创建时 +1,所以指向本对象的旧句柄永远失效 */
    g_live_count--;
    return 0;
}

int32_t dg_object_clear(void) {
    for (int32_t i = 1; i <= DG_MAX_OBJECTS; i++)
        if (g_ents[i].live) dg_object_free((g_ents[i].gen << 16) | (uint32_t)i);
    g_live_count = 0;
    return 0;
}

/* ---------- 字段读写(描述符驱动) ---------- */
static const DgField *dg_field(int kind, const char *fname) {
    const DgPool *p = &g_pools[kind];
    for (int i = 0; i < p->nfields; i++)
        if (strcmp(p->fields[i].name, fname) == 0) return &p->fields[i];
    dg_error("component '%s' has no field '%s'", p->name, fname ? fname : "(null)");
    return NULL;
}

static int dg_field_check(const DgField *f, int want_float) {
    if (f->type == (want_float ? DG_F_FLOAT : DG_F_INT)) return 0;
    dg_error("field '%s' has type %s, not %s", f->name,
             f->type == DG_F_FLOAT ? "float" : f->type == DG_F_STR ? "string"
             : f->type == DG_F_BOOL ? "bool" : "int",
             want_float ? "float" : "int");
    return -1;
}

int32_t dg_set_f(uint32_t obj, const char *comp, const char *field, double v) {
    const int k = dg_comp_kind(comp);
    if (k < 0) return -1;
    void *c = dg_comp_get(obj, k);
    if (!c) { dg_error("object %u has no '%s'", obj, comp); return -1; }
    const DgField *f = dg_field(k, field);
    if (!f || dg_field_check(f, 1)) return -1;
    *(float *)((uint8_t *)c + f->offset) = (float)v;
    return 0;
}

int32_t dg_set_i(uint32_t obj, const char *comp, const char *field, int64_t v) {
    const int k = dg_comp_kind(comp);
    if (k < 0) return -1;
    void *c = dg_comp_get(obj, k);
    if (!c) { dg_error("object %u has no '%s'", obj, comp); return -1; }
    const DgField *f = dg_field(k, field);
    if (!f || dg_field_check(f, 0)) return -1;
    *(int32_t *)((uint8_t *)c + f->offset) = (int32_t)v;
    return 0;
}

int32_t dg_set_s(uint32_t obj, const char *comp, const char *field, const char *v) {
    const int k = dg_comp_kind(comp);
    if (k < 0) return -1;
    void *c = dg_comp_get(obj, k);
    if (!c) { dg_error("object %u has no '%s'", obj, comp); return -1; }
    const DgField *f = dg_field(k, field);
    if (!f) return -1;
    if (f->type != DG_F_STR) { dg_error("field '%s' is not a string", f->name); return -1; }
    snprintf((char *)((uint8_t *)c + f->offset), f->size, "%s", v ? v : "");
    /* 设置 sprite.tex_path 时立即解析纹理句柄 —— 否则用户还得自己再 eng_tex_load
       一次并把句柄塞进 runtime 字段,那是没必要的两步。有缓存,重复设置也不会重复解码。 */
    if (k == DG_C_SPRITE && strcmp(f->name, "tex_path") == 0) {
        DgSprite *s = (DgSprite *)c;
        s->texture = s->tex_path[0] ? dg_tex_load_cached(s->tex_path) : -1;
        if (s->texture < 0) return -1;     /* 路径错了要立刻报,不能等渲染时才失败 */
    }
    return 0;
}

double dg_get_f(uint32_t obj, const char *comp, const char *field) {
    const int k = dg_comp_kind(comp);
    if (k < 0) return 0.0;
    void *c = dg_comp_get(obj, k);
    if (!c) { dg_error("object %u has no '%s'", obj, comp); return 0.0; }
    const DgField *f = dg_field(k, field);
    if (!f || dg_field_check(f, 1)) return 0.0;
    return (double)*(float *)((uint8_t *)c + f->offset);
}

int64_t dg_get_i(uint32_t obj, const char *comp, const char *field) {
    const int k = dg_comp_kind(comp);
    if (k < 0) return 0;
    void *c = dg_comp_get(obj, k);
    if (!c) { dg_error("object %u has no '%s'", obj, comp); return 0; }
    const DgField *f = dg_field(k, field);
    if (!f || dg_field_check(f, 0)) return 0;
    return (int64_t)*(int32_t *)((uint8_t *)c + f->offset);
}

const char *dg_get_s(uint32_t obj, const char *comp, const char *field) {
    static char buf[1024];
    buf[0] = '\0';
    const int k = dg_comp_kind(comp);
    if (k < 0) return buf;
    void *c = dg_comp_get(obj, k);
    if (!c) { dg_error("object %u has no '%s'", obj, comp); return buf; }
    const DgField *f = dg_field(k, field);
    if (!f) return buf;
    if (f->type != DG_F_STR) { dg_error("field '%s' is not a string", f->name); return buf; }
    snprintf(buf, sizeof buf, "%s", (const char *)((uint8_t *)c + f->offset));
    return buf;
}

/* ---------- 世界变换(层级解析) ----------
   递归解析父链,带深度上限防环。M2 只做**位置**继承(旋转/缩放继承与缓存留到后续),
   这一点在 DEXGAME_DESIGN 里记为已知简化。 */
#define DG_MAX_DEPTH 32

static void dg_world_xy(uint32_t obj, float *out_x, float *out_y) {
    float x = 0.0f, y = 0.0f;
    uint32_t cur = obj;
    for (int depth = 0; depth < DG_MAX_DEPTH; depth++) {
        DgTransform *t = (DgTransform *)dg_comp_get(cur, DG_C_TRANSFORM);
        if (!t) break;
        x += t->x; y += t->y;
        if (t->parent < 0 || !dg_object_alive((uint32_t)t->parent)) break;
        cur = (uint32_t)t->parent;
        if (depth == DG_MAX_DEPTH - 1)
            dg_error("transform hierarchy deeper than %d (cycle?)", DG_MAX_DEPTH);
    }
    *out_x = x; *out_y = y;
}

int32_t dg_world_pos(uint32_t obj, double *out_x, double *out_y) {
    if (!dg_object_alive(obj)) { dg_error("object %u is not alive", obj); return -1; }
    if (!dg_comp_get(obj, DG_C_TRANSFORM)) { dg_error("object %u has no transform", obj); return -1; }
    float x, y;
    dg_world_xy(obj, &x, &y);
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    return 0;
}

/* ============================ 运行时资源钩子 ============================ */
/* 卸载组件时释放运行时引用(纹理是缓存共享的,这里只是逻辑解除)*/
static void dg_comp_on_unload(int kind, void *comp, int32_t *id_map, int n) {
    (void)id_map; (void)n;
    if (kind == DG_C_SPRITE) {
        DgSprite *s = (DgSprite *)comp;
        s->texture = -1;
    }
}

/* 反序列化后重建运行时字段。id_map 把"场景内索引"映射为新的实体 id(供 parent 用)。
   返回 -1 = 该组件恢复失败(已由具体步骤记录原因),整次加载应当失败。*/
static int dg_comp_on_load(int kind, void *comp, const int32_t *id_map, int n) {
    switch (kind) {
    case DG_C_TRANSFORM: {
        DgTransform *t = (DgTransform *)comp;
        const int32_t old = t->parent;
        t->parent = (old >= 0 && old < n) ? id_map[old] : -1;
        break;
    }
    case DG_C_SPRITE: {
        DgSprite *s = (DgSprite *)comp;
        s->texture = -1;
        if (s->tex_path[0]) {
            s->texture = dg_tex_load_cached(s->tex_path);   /* 有缓存,不会每帧/每次重解码 */
            if (s->texture < 0) return -1;   /* 场景引用了不存在的贴图:加载期就报,别拖到渲染 */
        }
        break;
    }
    case DG_C_ANIMATION:
        ((DgAnimation *)comp)->time = 0.0f;
        break;
    default:
        break;
    }
    return 0;
}

/* ============================ 场景 JSON ============================ */
#define DG_SCENE_FORMAT 1

/* 把字段值写进 JSON(遍历描述符表) */
static void dg_write_field(DgJsonW *w, const DgField *f, const void *comp) {
    const uint8_t *base = (const uint8_t *)comp;
    switch (f->type) {
    case DG_F_FLOAT: djw_float(w, f->name, (double)*(const float *)(base + f->offset)); break;
    case DG_F_INT:   djw_int(w, f->name, (long long)*(const int32_t *)(base + f->offset)); break;
    case DG_F_BOOL:  djw_bool(w, f->name, *(const int32_t *)(base + f->offset) ? 1 : 0); break;
    case DG_F_STR:   djw_str(w, f->name, (const char *)(base + f->offset)); break;
    }
}

static int32_t dg_scene_write_json(DgJsonW *w) {
    /* 先给每个活实体编一个"场景内索引" —— JSON 里引用别的对象时用索引,
       因为实体 id 是运行时句柄,跨进程无意义。 */
    int32_t index_of[DG_MAX_OBJECTS + 1];
    int32_t ids[DG_MAX_OBJECTS];
    int n = 0;
    for (int32_t i = 1; i <= DG_MAX_OBJECTS; i++) {
        index_of[i] = -1;
        if (g_ents[i].live) {
            index_of[i] = n;
            ids[n] = (int32_t)((g_ents[i].gen << 16) | (uint32_t)i);
            n++;
        }
    }

    djw_obj_begin(w, NULL);
    djw_int(w, "format", DG_SCENE_FORMAT);
    djw_int(w, "count", n);
    djw_arr_begin(w, "objects");
    for (int k = 0; k < n; k++) {
        const uint32_t obj = (uint32_t)ids[k];
        const int32_t ei = dg_ent_idx(obj);
        djw_obj_begin(w, NULL);
        djw_int(w, "index", k);
        for (int kind = 1; kind < DG_C_COUNT; kind++) {
            DgPool *p = &g_pools[kind];
            if (!p->used[ei]) continue;
            const void *c = p->data + (size_t)p->slot[ei] * p->stride;
            djw_obj_begin(w, p->name);
            /* parent 特殊处理:写场景索引而不是实体 id */
            for (int fi = 0; fi < p->nfields; fi++) {
                const DgField *f = &p->fields[fi];
                if (!f->persist) continue;
                if (kind == DG_C_TRANSFORM && strcmp(f->name, "parent") == 0) {
                    const int32_t old = *(const int32_t *)((const uint8_t *)c + f->offset);
                    int32_t si = -1;
                    /* old 是**运行时实体 id**(高位是世代),必须先取出索引再查表 ——
                       拿 id 直接和 DG_MAX_OBJECTS 比是错的(id 远大于槽位数)。 */
                    if (old > 0) {
                        const int32_t pi = dg_ent_idx((uint32_t)old);
                        if (pi > 0 && pi <= DG_MAX_OBJECTS) si = index_of[pi];
                    }
                    djw_int(w, "parent", si);
                    continue;
                }
                dg_write_field(w, f, c);
            }
            djw_obj_end(w);
        }
        djw_obj_end(w);
    }
    djw_arr_end(w);
    djw_obj_end(w);
    return w->oom ? -1 : 0;
}

const char *dg_scene_to_json(void) {
    static DgJsonW w;
    static int inited = 0;
    if (!inited) { djw_init(&w); inited = 1; }
    w.len = 0;
    w.depth = 0;
    w.oom = 0;
    if (w.buf) w.buf[0] = '\0';
    if (dg_scene_write_json(&w)) { dg_error("out of memory serializing scene"); return NULL; }
    return djw_text(&w);
}

/* 读一个字段值进组件(遍历描述符表) */
static int dg_read_field(DgJsonR *r, const DgField *f, void *comp) {
    uint8_t *base = (uint8_t *)comp;
    switch (f->type) {
    case DG_F_FLOAT: { double d; if (djr_float(r, &d)) return -1;
                       *(float *)(base + f->offset) = (float)d; return 0; }
    case DG_F_INT:
    case DG_F_BOOL:  { long long v; if (djr_int(r, &v)) return -1;
                       *(int32_t *)(base + f->offset) = (int32_t)v; return 0; }
    case DG_F_STR:   return djr_str(r, (char *)(base + f->offset), f->size);
    }
    return -1;
}

static int32_t dg_scene_parse(const char *text) {
    DgJsonR r;
    djr_init(&r, text, strlen(text));
    if (djr_obj_begin(&r)) { dg_error("scene JSON: %s", djr_error(&r)); return -1; }

    /* 第一遍:只创建实体并记录 index → 实体 id 的映射。
       第二遍才填字段 —— 因为 parent 引用可能指向后面的对象。 */
    dg_object_clear();

    int32_t id_map[DG_MAX_OBJECTS];
    int n_map = 0;
    char key[64];
    key[0] = '\0';
    int rc;
    int saw_format = 0;
    char objects_text[1];   (void)objects_text;

    /* 由于是游标式解析,不能回退;所以先把整个 objects 数组读进内存再二次解析。
       场景规模不大(数千实体),这个代价可接受。 */
    const char *objs_begin = NULL;
    size_t objs_len = 0;
    while ((rc = djr_key(&r, key, sizeof key)) == 0) {
        if (strcmp(key, "format") == 0) {
            long long v;
            if (djr_int(&r, &v)) { dg_error("scene JSON: %s", djr_error(&r)); return -1; }
            if (v > DG_SCENE_FORMAT) {
                dg_error("scene format %lld is newer than supported %d", v, DG_SCENE_FORMAT);
                return -1;
            }
            saw_format = 1;
        } else if (strcmp(key, "objects") == 0) {
            if (djr_arr_begin(&r)) { dg_error("scene JSON: %s", djr_error(&r)); return -1; }
            objs_begin = r.p;
            /* 扫描到匹配的 ']' */
            int depth = 0;
            const char *q = r.p;
            while (q < r.end) {
                if (*q == '[') depth++;
                else if (*q == ']') { if (!depth) break; depth--; }
                else if (*q == '"') {
                    q++;
                    while (q < r.end && *q != '"') { if (*q == '\\') q++; q++; }
                }
                q++;
            }
            if (q >= r.end) { dg_error("scene JSON: unterminated objects array"); return -1; }
            /* 切片**包含**收尾的 ']' —— 两个子解析器要用 djr_arr_end 收尾,
               它们得看得到那个 ']'。 */
            objs_len = (size_t)(q - objs_begin) + 1;
            r.p = q + 1;                       /* 跳过 ']' */
        } else {
            if (djr_skip_value(&r)) { dg_error("scene JSON: %s", djr_error(&r)); return -1; }
        }
    }
    (void)saw_format;
    if (djr_obj_end(&r)) { dg_error("scene JSON: %s", djr_error(&r)); return -1; }
    if (!objs_begin) { dg_error("scene JSON: no 'objects' array"); return -1; }

    /* ---- 第一遍:数出对象个数并创建 ---- */
    DgJsonR p1;
    djr_init(&p1, objs_begin, objs_len);
    int32_t objs[DG_MAX_OBJECTS];
    int n_objs = 0;
    int more;
    while ((more = djr_arr_more(&p1)) == 1) {
        if (djr_obj_begin(&p1)) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
        char k2[64]; k2[0] = '\0';
        int rc2;
        int32_t idx = -1;
        while ((rc2 = djr_key(&p1, k2, sizeof k2)) == 0) {
            if (strcmp(k2, "index") == 0) {
                long long v;
                if (djr_int(&p1, &v)) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
                idx = (int32_t)v;
            } else if (djr_skip_value(&p1)) {
                dg_error("scene JSON: %s", djr_error(&p1)); return -1;
            }
        }
        if (rc2 < 0) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
        if (djr_obj_end(&p1)) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
        if (n_objs >= DG_MAX_OBJECTS) { dg_error("scene has too many objects"); return -1; }
        const uint32_t id = dg_object_new();
        if (!id) return -1;
        objs[n_objs] = (int32_t)id;
        /* index 可能乱序/缺省:按出现顺序兜底 */
        const int32_t slot = (idx >= 0 && idx < DG_MAX_OBJECTS) ? idx : n_objs;
        if (slot >= n_map) { for (int j = n_map; j <= slot; j++) id_map[j] = 0; n_map = slot + 1; }
        id_map[slot] = (int32_t)id;
        n_objs++;
    }
    if (more < 0) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
    if (djr_arr_end(&p1)) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }

    /* ---- 第二遍:填字段 ---- */
    DgJsonR p2;
    djr_init(&p2, objs_begin, objs_len);
    int oi = 0;
    while ((more = djr_arr_more(&p2)) == 1) {
        if (djr_obj_begin(&p2)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
        const uint32_t obj = (uint32_t)objs[oi++];
        char k3[64]; k3[0] = '\0';
        int rc3;
        while ((rc3 = djr_key(&p2, k3, sizeof k3)) == 0) {
            if (strcmp(k3, "index") == 0) {
                long long v;
                if (djr_int(&p2, &v)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
                continue;
            }
            const int kind = dg_comp_kind(k3);
            if (kind < 0) {
                dg_clear_error();
                /* 不认识的组件:跳过整段 —— 新版本写的场景旧版本也能读 */
                if (djr_skip_value(&p2)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
                continue;
            }
            void *c = dg_comp_add(obj, kind);
            if (!c) return -1;
            if (djr_obj_begin(&p2)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
            const DgPool *pool = &g_pools[kind];
            char k4[64]; k4[0] = '\0';
            int rc4;
            while ((rc4 = djr_key(&p2, k4, sizeof k4)) == 0) {
                const DgField *f = NULL;
                for (int fi = 0; fi < pool->nfields; fi++)
                    if (strcmp(pool->fields[fi].name, k4) == 0) { f = &pool->fields[fi]; break; }
                if (!f) {                                    /* 未知字段:跳过 */
                    if (djr_skip_value(&p2)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
                    continue;
                }
                if (dg_read_field(&p2, f, c)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
            }
            if (rc4 < 0) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
            /* 关掉组件对象 —— 迭代器不消费 '}',必须显式收尾 */
            if (djr_obj_end(&p2)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
            if (dg_comp_on_load(kind, c, id_map, n_map)) {
                /* 具体原因(on_load 里的贴图加载失败等)已经记好了,别覆盖 */
                if (!dg_last_error()[0])
                    dg_error("scene JSON: cannot restore component '%s'", dg_comp_name(kind));
                return -1;
            }
        }
        if (rc3 < 0) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
        /* 关掉场景对象 */
        if (djr_obj_end(&p2)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
    }
    if (more < 0) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
    if (djr_arr_end(&p2)) { dg_error("scene JSON: %s", djr_error(&p2)); return -1; }
    return 0;
}

int32_t dg_scene_from_json(const char *text) {
    const int32_t rc = dg_scene_parse(text);
    /* 失败时不留半成品场景:否则调用方拿到 -1 却已经多出一堆实体/组件,
       继续跑下去的行为无法预料(测试里就出现过"加载失败但计数 = 1")。 */
    if (rc < 0) dg_object_clear();
    return rc;
}

int32_t dg_scene_save(const char *path) {
    const char *text = dg_scene_to_json();
    if (!text) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { dg_error("cannot write scene '%s'", path); return -1; }
    const size_t n = fwrite(text, 1, strlen(text), f);
    const int ok = (n == strlen(text));
    fclose(f);
    if (!ok) { dg_error("short write on scene '%s'", path); return -1; }
    return 0;
}

int32_t dg_scene_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { dg_error("cannot open scene '%s'", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); dg_error("cannot size scene '%s'", path); return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); dg_error("out of memory reading scene '%s'", path); return -1; }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    const int32_t rc = dg_scene_from_json(buf);
    free(buf);
    return rc;
}

/* ============================ 从组件渲染 ============================ */
typedef struct { uint32_t obj; int32_t layer, order, seq; } DgDrawable;

static int dg_cmp_drawable(const void *a, const void *b) {
    const DgDrawable *x = (const DgDrawable *)a, *y = (const DgDrawable *)b;
    if (x->layer != y->layer) return x->layer - y->layer;
    if (x->order != y->order) return x->order - y->order;
    return x->seq - y->seq;                     /* 稳定 */
}

int32_t dg_draw_scene(void) {
    if (!g_pools_ready) { dg_error("scene not initialized"); return -1; }
    /* 找活动相机(第一个 active 的 camera 组件) */
    const DgCamera *cam = NULL;
    for (int32_t i = 1; i <= DG_MAX_OBJECTS && !cam; i++) {
        if (!g_ents[i].live) continue;
        const uint32_t id = (g_ents[i].gen << 16) | (uint32_t)i;
        const DgCamera *c = (const DgCamera *)dg_comp_get(id, DG_C_CAMERA);
        if (c && c->active) {
            static DgCamera cc;                 /* 复制一份,免得后面又被改 */
            cc = *c;
            cam = &cc;
        }
    }

    DgDrawable list[DG_MAX_OBJECTS];
    int n = 0;
    for (int32_t i = 1; i <= DG_MAX_OBJECTS; i++) {
        if (!g_ents[i].live) continue;
        const uint32_t id = (g_ents[i].gen << 16) | (uint32_t)i;
        const DgSprite *s = (const DgSprite *)dg_comp_get(id, DG_C_SPRITE);
        if (!s || s->texture < 0) continue;
        if (!dg_comp_get(id, DG_C_TRANSFORM)) continue;   /* 必须有变换才画 */
        list[n].obj = id;
        list[n].layer = s->layer;
        list[n].order = s->order;
        list[n].seq = n;
        n++;
    }
    qsort(list, (size_t)n, sizeof list[0], dg_cmp_drawable);

    const float zx = cam ? cam->zoom : 1.0f;
    const float cx = cam ? cam->x : 0.0f;
    const float cy = cam ? cam->y : 0.0f;
    if (zx == 0.0f) {
        dg_error("camera zoom is 0");
        return -1;
    }

    int drawn = 0;
    for (int k = 0; k < n; k++) {
        const uint32_t id = list[k].obj;
        const DgSprite *s = (const DgSprite *)dg_comp_get(id, DG_C_SPRITE);
        float wx, wy;
        dg_world_xy(id, &wx, &wy);

        const int tw = dg_tex_width((int)s->texture);
        const int th = dg_tex_height((int)s->texture);
        if (tw <= 0 || th <= 0) continue;
        const float sw = s->sw > 0.0f ? s->sw : (float)tw;
        const float sh = s->sh > 0.0f ? s->sh : (float)th;

        /* 源区域 → uv。约定与 `test_dexgame.py::test_atlas_png` 的 M1 结论一致:
           - 区域跨**多个**纹素:用区域边界(sx/W .. (sx+sw)/W)。这样 1:1 绘制时
             屏幕像素中心正好落在纹素中心,放大时也不会整体缩半个纹素;
           - 区域是**单个**纹素(sw/sh ≤ 1):退化到纹素中心(u0==u1)。否则线性
             过滤会把邻居掺进来 —— M1 实测纯绿读成 0xff04ff04。
           图集助手将来要自动做的事就是这个,所以这里必须先在引擎里做对。 */
        const float half = 0.5f;
        float u0 = (sw <= 1.0f) ? (s->sx + half) / (float)tw : s->sx / (float)tw;
        float u1 = (sw <= 1.0f) ? u0 : (s->sx + sw) / (float)tw;
        float v0 = (sh <= 1.0f) ? (s->sy + half) / (float)th : s->sy / (float)th;
        float v1 = (sh <= 1.0f) ? v0 : (s->sy + sh) / (float)th;
        /* 轴心:px/py ∈ [0,1] 表示绘制原点在子矩形里的相对位置 */
        float x = wx - sw * s->px;
        float y = wy - sh * s->py;
        /* 相机约定:(x, y) 是**显示在屏幕左上角的世界坐标**,zoom 为缩放。
           于是没有相机(或相机在 0,0)时 世界坐标 == 屏幕坐标 —— 最不意外。
           若游戏要"相机居中跟随",把 cam.x 设为 target_x - 屏宽/2 即可。
           (先前这里多加了屏幕中心偏移,导致无相机时整个世界被平移半个屏幕。)*/
        x = (x - cx) * zx;
        y = (y - cy) * zx;
        const float dw = sw * zx, dh = sh * zx;

        if (s->flip & 1) { float t = u0; u0 = u1; u1 = t; }
        if (s->flip & 2) { float t = v0; v0 = v1; v1 = t; }

        if (dg_draw_quad((int)s->texture, x, y, dw, dh, u0, v0, u1, v1,
                         (uint32_t)s->tint) == 0)
            drawn++;
    }
    return drawn;
}
