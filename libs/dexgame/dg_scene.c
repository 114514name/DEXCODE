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
#include "dg_utf8.h"
#include "dg_json.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================ 实体 ============================ */
#define DG_MAX_OBJECTS 4096

typedef struct {
    uint32_t gen;      /* 世代号:每次创建 +1,永不为 0 */
    int32_t  live;
    char     name[DG_NAME_MAX];   /* 空串 = 未命名 */
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

/* 按序号取活实体 id —— 给 IDE 的场景树用(语言没有数组,没法从 DexLang 侧枚举)。
 * 顺序 = 槽位顺序(创建顺序);越界返回 0(0 不是合法实体 id)。 */
uint32_t dg_object_id_at(int32_t index) {
    int32_t i, seen = 0;
    if (index < 0) return 0;
    for (i = 1; i <= DG_MAX_OBJECTS; i++) {
        if (!g_ents[i].live) continue;
        if (seen == index) return (g_ents[i].gen << 16) | (uint32_t)i;
        seen++;
    }
    return 0;
}

int32_t dg_object_set_name(uint32_t id, const char *name) {
    if (!dg_object_alive(id)) { dg_error("object %u is not alive", id); return -1; }
    if (name && strlen(name) >= DG_NAME_MAX) {
        dg_error("name too long (max %d chars)", DG_NAME_MAX - 1);
        return -1;
    }
    snprintf(g_ents[dg_ent_idx(id)].name, DG_NAME_MAX, "%s", name ? name : "");
    return 0;
}

const char *dg_object_name(uint32_t id) {
    if (!dg_object_alive(id)) return "";
    return g_ents[dg_ent_idx(id)].name;
}

uint32_t dg_object_find(const char *name) {
    if (!name || !name[0]) return 0;
    for (int32_t i = 1; i <= DG_MAX_OBJECTS; i++)
        if (g_ents[i].live && strcmp(g_ents[i].name, name) == 0)
            return (g_ents[i].gen << 16) | (uint32_t)i;
    return 0;
}

int dg_scene_collect_objects(uint32_t *out, int cap) {
    int n = 0;
    for (int32_t i = 1; i <= DG_MAX_OBJECTS && n < cap; i++)
        if (g_ents[i].live) out[n++] = (g_ents[i].gen << 16) | (uint32_t)i;
    return n;
}

/* ============================ 字段描述符 ============================ */
static void dg_comp_on_unload(int kind, void *comp, int32_t *id_map, int n);
/* 从文件把瓦片网格装进给定组件(不含实体查询,on_load 也要用) */
static int  dg_tilemap_load_path(DgTilemap *t, const char *path);

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

/* ============================ 组件结构 ============================
   结构体定义在 dexgame.h(dg_phys.c 也要看);这里只留字段表。
   collider/body 的行为在 dg_phys.c,tilemap 的数据与渲染在本文件。 */

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
    /* 上一固定步的位置 + 接触状态 —— 渲染插值与物理用,不进 JSON */
    { "px",            DG_F_FLOAT, 0, 0, offsetof(DgBody, px) },
    { "py",            DG_F_FLOAT, 0, 0, offsetof(DgBody, py) },
    { "stepped",       DG_F_INT,   0, 0, offsetof(DgBody, stepped) },
    { "flags",         DG_F_INT,   0, 0, offsetof(DgBody, flags) },
};
static const DgField AUDIO_FIELDS[] = {
    { "path",          DG_F_STR,   1, (uint16_t)sizeof(((DgAudio *)0)->path),
      offsetof(DgAudio, path) },
    { "volume",        DG_F_FLOAT, 1, 0, offsetof(DgAudio, volume) },
    { "loop",          DG_F_INT,   1, 0, offsetof(DgAudio, loop) },
    { "play_on_start", DG_F_INT,   1, 0, offsetof(DgAudio, play_on_start) },
    { "handle",        DG_F_INT,   0, 0, offsetof(DgAudio, handle) },
    { "voice",         DG_F_INT,   0, 0, offsetof(DgAudio, voice) },
    { "playing",       DG_F_INT,   0, 0, offsetof(DgAudio, playing) },
};
static const DgField TILE_FIELDS[] = {
    { "path",       DG_F_STR,   1, (uint16_t)sizeof(((DgTilemap *)0)->path),
      offsetof(DgTilemap, path) },
    { "tex_path",   DG_F_STR,   1, (uint16_t)sizeof(((DgTilemap *)0)->tex_path),
      offsetof(DgTilemap, tex_path) },
    { "texture",    DG_F_INT,   0, 0, offsetof(DgTilemap, texture) },
    { "cols",       DG_F_INT,   0, 0, offsetof(DgTilemap, cols) },
    { "rows",       DG_F_INT,   0, 0, offsetof(DgTilemap, rows) },
    { "tw",         DG_F_FLOAT, 1, 0, offsetof(DgTilemap, tw) },
    { "th",         DG_F_FLOAT, 1, 0, offsetof(DgTilemap, th) },
    { "atlas_tile", DG_F_INT,   1, 0, offsetof(DgTilemap, atlas_tile) },
    { "atlas_cols", DG_F_INT,   1, 0, offsetof(DgTilemap, atlas_cols) },
    { "layer",      DG_F_INT,   1, 0, offsetof(DgTilemap, layer) },
    { "order",      DG_F_INT,   1, 0, offsetof(DgTilemap, order) },
    { "visible",    DG_F_INT,   1, 0, offsetof(DgTilemap, visible) },
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
    case DG_C_COLLIDER:
        /* 默认 8x8 的 AABB,第 0 层、与所有层碰撞(mask=0 表示不筛选) */
        ((DgCollider *)c)->kind = DG_SHAPE_AABB;
        ((DgCollider *)c)->hw = 4.0f;
        ((DgCollider *)c)->hh = 4.0f;
        ((DgCollider *)c)->layer = 1;
        break;
    case DG_C_AUDIO:
        ((DgAudio *)c)->volume = 1.0f;
        ((DgAudio *)c)->handle = -1;
        ((DgAudio *)c)->voice = 0;
        break;
    case DG_C_TILEMAP: {
        DgTilemap *t = (DgTilemap *)c;
        t->texture = -1;
        t->tw = 16.0f;
        t->th = 16.0f;
        t->atlas_tile = 16;
        t->atlas_cols = 8;
        t->visible = 1;
        memset(t->solid, -1, sizeof t->solid);      /* -1 = 未指定:非空即实心 */
        break;
    }
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
        { "tilemap",   DG_C_TILEMAP,   TILE_FIELDS,NFIELDS(TILE_FIELDS),(int)sizeof(DgTilemap) },
        { "audio",     DG_C_AUDIO,     AUDIO_FIELDS,NFIELDS(AUDIO_FIELDS),(int)sizeof(DgAudio) },
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
    /* 瓦片网格是 malloc 的,先释放(否则重建场景会泄漏) */
    for (int i = 0; i < g_pools[DG_C_TILEMAP].count; i++)
        free(((DgTilemap *)(g_pools[DG_C_TILEMAP].data + (size_t)i * g_pools[DG_C_TILEMAP].stride))->tiles);
    for (int k = 0; k < DG_C_COUNT; k++) {
        free(g_pools[k].data);   g_pools[k].data = NULL;
        free(g_pools[k].owner);  g_pools[k].owner = NULL;
        free(g_pools[k].slot);   g_pools[k].slot = NULL;
    }
    g_pools_ready = 0;
    memset(g_ents, 0, sizeof g_ents);
    g_live_count = 0;
    dg_phys_invalidate();
}

int dg_comp_kind(const char *name) {
    if (!name || !g_pools_ready) return -1;
    for (int k = 1; k < DG_C_COUNT; k++)
        if (g_pools[k].name && strcmp(g_pools[k].name, name) == 0) return k;
    dg_error("unknown component '%s' (have: transform/sprite/camera/animation/collider/body/tilemap/audio)",
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
    dg_phys_invalidate();
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
    dg_phys_invalidate();
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
    dg_phys_invalidate();
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
    dg_phys_after_set(obj, k, f->name);      /* 位置/速度变了要让宽相与休眠失效 */
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
    dg_phys_after_set(obj, k, f->name);
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
        /* 路径错了要立刻报,不能等渲染时才失败;但**清空**(设为 "")不是错误 ——
           那正是"把贴图摘掉"的做法,当成失败会让属性面板显示假的错误。 */
        if (s->tex_path[0] && s->texture < 0) return -1;
    } else if (k == DG_C_TILEMAP && strcmp(f->name, "tex_path") == 0) {
        DgTilemap *t = (DgTilemap *)c;
        t->texture = t->tex_path[0] ? dg_tex_load_cached(t->tex_path) : -1;
        if (t->tex_path[0] && t->texture < 0) return -1;
    } else if (k == DG_C_TILEMAP && strcmp(f->name, "path") == 0) {
        /* 直接给数据文件路径:立刻装网格,别等到渲染/碰撞才发现路径是错的 */
        if (v && v[0] && dg_tilemap_load_path((DgTilemap *)c, v)) return -1;
    } else if (k == DG_C_AUDIO && strcmp(f->name, "path") == 0) {
        /* 设了声音路径就立刻解码(路径错了要现在报,不要等到播放) */
        DgAudio *a = (DgAudio *)c;
        a->handle = (v && v[0]) ? dg_audio_load_cached(v) : -1;
        if (v && v[0] && a->handle < 0) return -1;
    } else if (k == DG_C_AUDIO && strcmp(f->name, "volume") == 0) {
        DgAudio *a = (DgAudio *)c;
        if (a->voice > 0) dg_audio_voice_set_volume(a->voice, a->volume);
    }
    dg_phys_after_set(obj, k, f->name);
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
    } else if (kind == DG_C_TILEMAP) {
        DgTilemap *t = (DgTilemap *)comp;
        t->texture = -1;
        free(t->tiles);                 /* 网格是 malloc 的,槽位会被复用 */
        t->tiles = NULL;
        t->cols = t->rows = 0;
    } else if (kind == DG_C_AUDIO) {
        DgAudio *a = (DgAudio *)comp;
        if (a->voice > 0) dg_audio_stop(a->voice);
        a->voice = 0;
        a->playing = 0;
        a->handle = -1;
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
    case DG_C_AUDIO: {
        DgAudio *a = (DgAudio *)comp;
        a->voice = 0;
        a->playing = 0;
        a->handle = -1;
        if (a->path[0]) {
            a->handle = dg_audio_load_cached(a->path);
            if (a->handle < 0) return -1;
            if (a->play_on_start) {
                a->voice = dg_audio_play(a->handle, a->loop, a->volume);
                if (a->voice < 0) return -1;
                a->playing = 1;
            }
        }
        break;
    }
    case DG_C_TILEMAP: {
        DgTilemap *t = (DgTilemap *)comp;
        t->texture = -1;
        t->tiles = NULL;
        t->cols = t->rows = 0;
        if (t->tex_path[0]) {
            t->texture = dg_tex_load_cached(t->tex_path);
            if (t->texture < 0) return -1;
        }
        /* 网格数据不进 JSON(它可能有几万格),只存 path;这里按 path 装回来 */
        if (t->path[0] && dg_tilemap_load_path(t, t->path)) return -1;
        break;
    }
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
        const char *nm = dg_object_name(obj);
        if (nm && nm[0]) djw_str(w, "name", nm);
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
        char pending_name[DG_NAME_MAX]; pending_name[0] = '\0';
        int rc2;
        int32_t idx = -1;
        while ((rc2 = djr_key(&p1, k2, sizeof k2)) == 0) {
            if (strcmp(k2, "index") == 0) {
                long long v;
                if (djr_int(&p1, &v)) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
                idx = (int32_t)v;
            } else if (strcmp(k2, "name") == 0) {
                char nm[DG_NAME_MAX];
                if (djr_str(&p1, nm, sizeof nm)) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
                snprintf(pending_name, sizeof pending_name, "%s", nm);   /* 实体还没建,先记下 */
            } else if (djr_skip_value(&p1)) {
                dg_error("scene JSON: %s", djr_error(&p1)); return -1;
            }
        }
        if (rc2 < 0) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
        if (djr_obj_end(&p1)) { dg_error("scene JSON: %s", djr_error(&p1)); return -1; }
        if (n_objs >= DG_MAX_OBJECTS) { dg_error("scene has too many objects"); return -1; }
        const uint32_t id = dg_object_new();
        if (!id) return -1;
        if (pending_name[0]) dg_object_set_name(id, pending_name);
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
    FILE *f = dg_fopen_asset(path, "wb");
    if (!f) { dg_error("cannot write scene '%s'", path); return -1; }
    const size_t n = fwrite(text, 1, strlen(text), f);
    const int ok = (n == strlen(text));
    fclose(f);
    if (!ok) { dg_error("short write on scene '%s'", path); return -1; }
    return 0;
}

int32_t dg_scene_load(const char *path) {
    FILE *f = dg_fopen_asset(path, "rb");
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
typedef struct { uint32_t obj; int32_t layer, order, seq; int32_t tile; } DgDrawable;

static int dg_cmp_drawable(const void *a, const void *b) {
    const DgDrawable *x = (const DgDrawable *)a, *y = (const DgDrawable *)b;
    if (x->layer != y->layer) return x->layer - y->layer;
    if (x->order != y->order) return x->order - y->order;
    return x->seq - y->seq;                     /* 稳定 */
}

/* 绘制位置 = 父链世界坐标 + 自己位置的插值值。
   物理以固定步长跑(默认 120Hz),渲染帧率更高,不插值就会看到"有的帧没动";
   有 body 的实体默认按 alpha 在上一固定步与当前位置之间插值。 */
static void dg_draw_pos(uint32_t id, float *out_x, float *out_y) {
    float wx, wy;
    dg_world_xy(id, &wx, &wy);
    const DgTransform *t = (const DgTransform *)dg_comp_get(id, DG_C_TRANSFORM);
    float ix, iy;
    if (t && dg_phys_lerp_pos(id, t->x, t->y, &ix, &iy)) {
        /* 把"父链贡献"换掉:world = 父链 + 自己的插值位置 */
        *out_x = (wx - t->x) + ix;
        *out_y = (wy - t->y) + iy;
    } else {
        *out_x = wx;
        *out_y = wy;
    }
}

/* ============================ 瓦片地图 ============================
   数据**不进 JSON**(一张 128x128 的图有几万格,写进场景文件既大又难读):
   组件只存 `path`,网格由 dg_tilemap_load_path / load_csv 装进 C 侧。
   碰撞(实心判定)由 dg_phys.c 通过 dg_tilemap_each_solid_in 使用。 */

/* 解析 CSV:空白/逗号/分号分隔的整数。**负数 = 空格子**,0 及以上 = 图集索引
   (注意:0 是合法图块,所以不能用 0 表示空 —— 这与"瓦片 id 从 0 开始"一致)。
   行长度可以不一致(按最长行补齐,短行留空)—— 手写地图时很实用。 */
static int dg_tilemap_parse_csv(DgTilemap *t, const char *text) {
    int cap_cols = 64, cap_rows = 64;
    int16_t *grid = (int16_t *)malloc((size_t)cap_cols * cap_rows * sizeof(int16_t));
    if (!grid) { dg_error("out of memory parsing tilemap"); return -1; }
    for (int i = 0; i < cap_cols * cap_rows; i++) grid[i] = -1;

    int cols = 0, rows = 0, col = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\n') {                       /* 换行 = 换行,列数取最大 */
            if (col > cols) cols = col;
            rows++; col = 0;
            p++;
            continue;
        }
        if (*p == ',' || *p == ';') { p++; continue; }
        /* 解析一个整数 */
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) {
            dg_error("tilemap CSV: unexpected char '%c' at row %d col %d", *p, rows, col);
            free(grid);
            return -1;
        }
        p = end;
        if (rows >= cap_rows || col >= cap_cols) {
            const int nc = cap_cols * 2, nr = cap_rows * 2;
            int16_t *ng = (int16_t *)malloc((size_t)nc * nr * sizeof(int16_t));
            if (!ng) { dg_error("out of memory growing tilemap"); free(grid); return -1; }
            for (int i = 0; i < nc * nr; i++) ng[i] = -1;
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cap_cols; c++)
                    ng[(size_t)r * nc + c] = grid[(size_t)r * cap_cols + c];
            free(grid);
            grid = ng;
            cap_cols = nc; cap_rows = nr;
        }
        if (v > 32767) { dg_error("tilemap CSV: tile id %ld is too large (max 32767)", v); free(grid); return -1; }
        grid[(size_t)rows * cap_cols + col] = (int16_t)(v < 0 ? -1 : v);
        col++;
    }
    if (col > cols) cols = col;
    if (col > 0 || rows == 0) rows++;          /* 最后一行没有换行符也要算 */

    /* 解析时用 cap_cols 当行距(便于扩容),存储用 cols 当行距(与读取端一致)——
       这里压紧成 cols*rows,否则读取端会按 cols 索引到错位的格子。 */
    int16_t *final = (int16_t *)malloc((size_t)(cols > 0 ? cols : 1) * (size_t)(rows > 0 ? rows : 1) * sizeof(int16_t));
    if (!final) { dg_error("out of memory finalizing tilemap"); free(grid); return -1; }
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            final[(size_t)r * cols + c] = (c < cap_cols) ? grid[(size_t)r * cap_cols + c] : (int16_t)-1;
    free(grid);

    free(t->tiles);
    t->tiles = final;
    t->cols = cols;
    t->rows = rows;
    dg_phys_invalidate();
    return 0;
}

static int dg_tilemap_load_path(DgTilemap *t, const char *path) {
    FILE *f = dg_fopen_asset(path, "rb");
    if (!f) { dg_error("cannot open tilemap '%s'", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); dg_error("cannot size tilemap '%s'", path); return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); dg_error("out of memory reading tilemap '%s'", path); return -1; }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    const int rc = dg_tilemap_parse_csv(t, buf);
    free(buf);
    return rc;
}

static DgTilemap *dg_tilemap_of(uint32_t obj) {
    DgTilemap *t = (DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    if (!t) dg_error("object %u has no 'tilemap'", obj);
    return t;
}

int32_t dg_tilemap_load_csv(uint32_t obj, const char *text) {
    DgTilemap *t = dg_tilemap_of(obj);
    if (!t) return -1;
    if (!text) { dg_error("tilemap CSV text is null"); return -1; }
    return dg_tilemap_parse_csv(t, text);
}

int32_t dg_tilemap_load_file(uint32_t obj, const char *path) {
    DgTilemap *t = dg_tilemap_of(obj);
    if (!t) return -1;
    if (!path || !path[0]) { dg_error("tilemap path is empty"); return -1; }
    if (dg_tilemap_load_path(t, path)) return -1;
    snprintf(t->path, sizeof t->path, "%s", path);
    return 0;
}

int32_t dg_tilemap_save_csv(uint32_t obj, const char *path) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    if (!t) { dg_error("object %u has no 'tilemap'", obj); return -1; }
    if (!t->tiles) { dg_error("tilemap has no grid loaded"); return -1; }
    FILE *f = dg_fopen_asset(path, "wb");
    if (!f) { dg_error("cannot write tilemap '%s'", path); return -1; }
    for (int r = 0; r < t->rows; r++) {
        for (int c = 0; c < t->cols; c++) {
            if (c) fputc(',', f);
            fprintf(f, "%d", (int)t->tiles[(size_t)r * t->cols + c]);
        }
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

int32_t dg_tilemap_tile(uint32_t obj, int32_t col, int32_t row) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    if (!t) { dg_error("object %u has no 'tilemap'", obj); return -1; }
    if (!t->tiles) { dg_error("tilemap has no grid loaded"); return -1; }
    if (col < 0 || row < 0 || col >= t->cols || row >= t->rows) {
        dg_error("tile (%d,%d) out of range %dx%d", col, row, t->cols, t->rows);
        return -1;
    }
    return t->tiles[(size_t)row * t->cols + col];
}

int32_t dg_tilemap_cols(uint32_t obj) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    return t ? t->cols : -1;
}
int32_t dg_tilemap_rows(uint32_t obj) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    return t ? t->rows : -1;
}

int32_t dg_tilemap_set_solid(uint32_t obj, int32_t tile, int on) {
    DgTilemap *t = dg_tilemap_of(obj);
    if (!t) return -1;
    if (tile < 0 || tile > 255) { dg_error("tile id %d out of range 0..255", tile); return -1; }
    t->solid[tile] = on ? 1 : 0;
    dg_phys_invalidate();
    return 0;
}

int32_t dg_tilemap_is_solid(uint32_t obj, int32_t tile) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    if (!t) { dg_error("object %u has no 'tilemap'", obj); return 0; }
    if (tile < 0 || tile > 255) return 0;          /* 空/越界都不是实心 */
    return t->solid[tile] < 0 ? 1 : t->solid[tile]; /* 默认:非空即实心 */
}

/* 世界坐标 → 瓦片坐标需要知道地图的原点(实体世界坐标) */
static void dg_tilemap_origin(uint32_t obj, const DgTilemap *t, float *ox, float *oy) {
    float wx = 0.0f, wy = 0.0f;
    dg_world_xy(obj, &wx, &wy);
    *ox = wx;
    *oy = wy;
    (void)t;
}

int32_t dg_tilemap_solid_at(uint32_t obj, float wx, float wy) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    if (!t || !t->tiles) return 0;
    float ox, oy;
    dg_tilemap_origin(obj, t, &ox, &oy);
    const int c = (int)floorf((wx - ox) / (t->tw > 0.0f ? t->tw : 1.0f));
    const int r = (int)floorf((wy - oy) / (t->th > 0.0f ? t->th : 1.0f));
    if (c < 0 || r < 0 || c >= t->cols || r >= t->rows) return 0;
    const int tile = t->tiles[(size_t)r * t->cols + c];
    if (tile < 0 || tile > 255) return 0;
    return t->solid[tile] < 0 ? 1 : t->solid[tile];
}

int32_t dg_tilemap_each_tile(uint32_t obj, void *user, DgTileFn fn) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    if (!t) { dg_error("object %u has no 'tilemap'", obj); return -1; }
    if (!t->tiles) return 0;
    float ox, oy;
    dg_tilemap_origin(obj, t, &ox, &oy);
    int32_t n = 0;
    for (int r = 0; r < t->rows; r++)
        for (int c = 0; c < t->cols; c++) {
            const int tile = t->tiles[(size_t)r * t->cols + c];
            if (tile < 0) continue;
            fn(user, ox + (float)c * t->tw, oy + (float)r * t->th, t->tw, t->th, tile);
            n++;
        }
    return n;
}
/* 遍历与给定世界矩形相交的瓦片;solid_only=1 时只给实心的(碰撞热路径),
   =0 时给所有非空格子(渲染)。 */
static int32_t dg_tilemap_each_in(uint32_t obj, float x, float y, float w, float h,
                                  int solid_only, void *user, DgTileFn fn) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    if (!t || !t->tiles) return 0;
    float ox, oy;
    dg_tilemap_origin(obj, t, &ox, &oy);
    const float tw = t->tw > 0.0f ? t->tw : 1.0f;
    const float th = t->th > 0.0f ? t->th : 1.0f;
    int c0 = (int)floorf((x - ox) / tw), c1 = (int)floorf((x + w - ox) / tw);
    int r0 = (int)floorf((y - oy) / th), r1 = (int)floorf((y + h - oy) / th);
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 >= t->cols) c1 = t->cols - 1;
    if (r1 >= t->rows) r1 = t->rows - 1;
    int32_t n = 0;
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
            const int tile = t->tiles[(size_t)r * t->cols + c];
            if (tile < 0 || tile > 255) continue;
            if (solid_only && t->solid[tile] == 0) continue;
            fn(user, ox + (float)c * tw, oy + (float)r * th, tw, th, tile);
            n++;
        }
    return n;
}

int32_t dg_tilemap_each_solid_in(uint32_t obj, float x, float y, float w, float h,
                                 void *user, DgTileFn fn) {
    return dg_tilemap_each_in(obj, x, y, w, h, 1, user, fn);
}

/* --- 瓦片绘制回调:把命中的瓦片变成四边形 --- */
typedef struct {
    const DgTilemap *t;
    int tw, th;              /* 图集像素尺寸 */
    float zx, cx, cy;
} DgTileDrawCtx;

static void dg_tile_emit(void *user, float x, float y, float w, float h, int32_t tile) {
    DgTileDrawCtx *ctx = (DgTileDrawCtx *)user;
    const int at = ctx->t->atlas_tile > 0 ? ctx->t->atlas_tile : 16;
    const int acols = ctx->t->atlas_cols > 0 ? ctx->t->atlas_cols : 1;
    const int sx = (tile % acols) * at, sy = (tile / acols) * at;
    /* uv 规则与 dg_draw_scene 一致:跨多纹素用区域边界,单纹素退化到纹素中心 */
    const float half = 0.5f;
    float u0, u1, v0, v1;
    if (at <= 1) {
        u0 = u1 = ((float)sx + half) / (float)ctx->tw;
        v0 = v1 = ((float)sy + half) / (float)ctx->th;
    } else {
        u0 = (float)sx / (float)ctx->tw;
        u1 = (float)(sx + at) / (float)ctx->tw;
        v0 = (float)sy / (float)ctx->th;
        v1 = (float)(sy + at) / (float)ctx->th;
    }
    const float dx = (x - ctx->cx) * ctx->zx, dy = (y - ctx->cy) * ctx->zx;
    dg_draw_quad(ctx->t->texture, dx, dy, w * ctx->zx, h * ctx->zx, u0, v0, u1, v1,
                 (uint32_t)DG_RGB(255, 255, 255));
}

int32_t dg_tilemap_draw(uint32_t obj, const float *view) {
    const DgTilemap *t = (const DgTilemap *)dg_comp_get(obj, DG_C_TILEMAP);
    if (!t || !t->tiles || t->texture < 0 || !t->visible) return 0;
    const int tw = dg_tex_width((int)t->texture), th = dg_tex_height((int)t->texture);
    if (tw <= 0 || th <= 0) return 0;
    DgTileDrawCtx ctx;
    ctx.t = t;
    ctx.tw = tw;
    ctx.th = th;
    ctx.zx = view[4];
    ctx.cx = view[0];
    ctx.cy = view[1];
    return dg_tilemap_each_in(obj, view[0], view[1], view[2], view[3], 0, &ctx, dg_tile_emit);
}

/* ---------- IDE 的编辑器视图覆盖(产品 B 的视口预览用) ----------
 * 视口要能自由平移/缩放,但不该为此去改用户场景里的相机实体(那会写进场景 JSON,
 * 也会污染撤销历史)。设了覆盖之后,渲染与"屏幕→世界"都用它。 */
static int   g_view_override = 0;
static float g_view_x = 0.0f, g_view_y = 0.0f, g_view_zoom = 1.0f;

void dg_scene_set_view_override(int on, float x, float y, float zoom) {
    g_view_override = on ? 1 : 0;
    g_view_x = x;
    g_view_y = y;
    g_view_zoom = (zoom > 0.0f) ? zoom : 1.0f;
}

int dg_scene_view_override(float *x, float *y, float *zoom) {
    if (!g_view_override) return 0;
    if (x) *x = g_view_x;
    if (y) *y = g_view_y;
    if (zoom) *zoom = g_view_zoom;
    return 1;
}

/* 活动相机(第一个 active 的 camera 组件)。返回 0 = 没有相机(即 世界 == 屏幕)。
 * IDE 设了视图覆盖时优先用覆盖值。 */
int32_t dg_scene_active_camera(float *x, float *y, float *zoom) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    if (zoom) *zoom = 1.0f;
    if (g_view_override) {
        if (x) *x = g_view_x;
        if (y) *y = g_view_y;
        if (zoom) *zoom = g_view_zoom;
        return 1;
    }
    for (int32_t i = 1; i <= DG_MAX_OBJECTS; i++) {
        if (!g_ents[i].live) continue;
        const uint32_t id = (g_ents[i].gen << 16) | (uint32_t)i;
        const DgCamera *c = (const DgCamera *)dg_comp_get(id, DG_C_CAMERA);
        if (c && c->active) {
            if (x) *x = c->x;
            if (y) *y = c->y;
            if (zoom) *zoom = c->zoom;
            return 1;
        }
    }
    return 0;
}

/* 屏幕 → 世界(相机约定的逆运算,见 dg_draw_scene 的说明)*/
void dg_scene_screen_to_world(float sx, float sy, float *wx, float *wy) {
    float cx = 0.0f, cy = 0.0f, zoom = 1.0f;
    dg_scene_active_camera(&cx, &cy, &zoom);
    if (zoom == 0.0f) zoom = 1.0f;
    if (wx) *wx = sx / zoom + cx;
    if (wy) *wy = sy / zoom + cy;
}

int32_t dg_draw_scene(void) {
    if (!g_pools_ready) { dg_error("scene not initialized"); return -1; }
    /* 找活动相机(第一个 active 的 camera 组件) */
    float cam_x = 0.0f, cam_y = 0.0f, cam_zoom = 1.0f;
    const int has_cam = dg_scene_active_camera(&cam_x, &cam_y, &cam_zoom);

    const float zx = cam_zoom;
    const float cx = cam_x;
    const float cy = cam_y;
    if (zx == 0.0f) {
        dg_error("camera zoom is 0");
        return -1;
    }
    (void)has_cam;
    /* 可见的世界矩形 —— 瓦片层用它裁剪(大地图只画看得见的部分)。
       view = {x, y, w, h, zoom} */
    const float view[5] = {
        cx, cy, (float)dg_gfx_width() / zx, (float)dg_gfx_height() / zx, zx
    };

    /* 可绘制对象:sprite 与 tilemap 一起排序(它们共用 layer/order)。
       同一个实体可以同时贡献两者(例如"地图 + 地图上的装饰精灵"),所以用静态数组。 */
    static DgDrawable list[DG_MAX_OBJECTS * 2];
    int n = 0;
    for (int32_t i = 1; i <= DG_MAX_OBJECTS; i++) {
        if (!g_ents[i].live) continue;
        const uint32_t id = (g_ents[i].gen << 16) | (uint32_t)i;
        if (!dg_comp_get(id, DG_C_TRANSFORM)) continue;   /* 必须有变换才画 */
        const DgSprite *s = (const DgSprite *)dg_comp_get(id, DG_C_SPRITE);
        if (s && s->texture >= 0 && n < (int)(sizeof list / sizeof list[0])) {
            list[n].obj = id; list[n].layer = s->layer; list[n].order = s->order;
            list[n].seq = n; list[n].tile = 0;
            n++;
        }
        const DgTilemap *t = (const DgTilemap *)dg_comp_get(id, DG_C_TILEMAP);
        if (t && t->visible && t->texture >= 0 && n < (int)(sizeof list / sizeof list[0])) {
            list[n].obj = id; list[n].layer = t->layer; list[n].order = t->order;
            list[n].seq = n; list[n].tile = 1;
            n++;
        }
    }
    qsort(list, (size_t)n, sizeof list[0], dg_cmp_drawable);

    int drawn = 0;
    for (int k = 0; k < n; k++) {
        const uint32_t id = list[k].obj;
        if (list[k].tile) {
            const int32_t nt = dg_tilemap_draw(id, view);
            if (nt > 0) drawn += nt;
            continue;
        }
        const DgSprite *s = (const DgSprite *)dg_comp_get(id, DG_C_SPRITE);
        float wx, wy;
        dg_draw_pos(id, &wx, &wy);

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
