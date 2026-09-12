/* dg_phys.c — 碰撞 / 查询 / 运动学 / 瓦片碰撞(M3 核心)
 *
 * 设计(见 docs/DEXGAME_DESIGN.md §6):
 *   - **固定步长 + 累加器**(默认 120Hz):可变步长会让跳跃高度、摩擦、碰撞穿透
 *     随帧率漂移 —— 165Hz 显示器上尤其明显。渲染帧率与物理步长解耦。
 *   - **轴分离的 move-and-slide**:先沿 X 移动并解算,再沿 Y 移动并解算。滑动是
 *     自然结果(不需要"投影到法线"那套);对 AABB 是**精确**的,位置可直接断言。
 *     单个子步的位移上限 DG_PHYS_MAX_STEP_PX,超过就拆成多次(防穿透)。
 *   - **宽相用均匀网格空间哈希**(不是四叉树):2D 场景里实体尺寸相近时更快更简单。
 *     任何几何改动都 dg_phys_invalidate(),下次使用时重建 —— 重建是 O(n)。
 *   - 形状:AABB / 圆 / **竖直胶囊**(半径 hw + 半长 hh)。旋转**不参与碰撞**
 *     (transform.rot 只影响渲染):2D 不引入 OBB,一期不付这个复杂度。
 *   - 圆/胶囊作为**阻挡物**时,轴向解算按包围盒近似(重叠判定仍精确);
 *     一期的主要静态几何是瓦片与 AABB 碰撞体,这两条路径都是精确的。
 *   - 物理在**世界坐标**里算,写回时减掉父链贡献 —— 有父对象的运动体也能正常工作。
 *
 * 为什么查询是"游标式":DexLang **没有数组**,原生函数也只能返回 int/float/string。
 * 所以 dg_phys_query_*() 把命中集合存在 C 侧并返回游标 id,语言侧用 while +
 * eng_query_next() 遍历(可嵌套,最多 DG_MAX_CURSORS 层)。
 * 同理 raycast/sweep 这种"一个返回码 + 多个输出"用**结果槽**:查询覆盖槽,
 * 再用 eng_hit_*() 逐个取值。
 */
#include "dexgame.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 世界设置 ---------- */
#define DG_PHYS_DEFAULT_HZ      120.0
#define DG_PHYS_DEFAULT_GRAVITY 980.0f        /* 像素/秒²,向下 */
#define DG_PHYS_MAX_SUBSTEPS    8
#define DG_PHYS_MAX_STEP_PX     4.0f          /* 单个子步最大位移:必须**小于最薄的障碍**
                                                 (常见的瓦片是 8/16px),否则会跳过去。
                                                 极快的投射物应该用 sweep/raycast。 */
#define DG_PHYS_MAX_FRAME_DT    0.25          /* 单帧最多吃 250ms(调试断点后别雪崩)*/
#define DG_PHYS_SLEEP_EPS       1.0f          /* |v| < 1 px/s 且落地 → 休眠 */
#define DG_PHYS_BOUNCE_CUTOFF   20.0f         /* 反弹速度低于它就直接停,免得抖 */
#define DG_PHYS_MAX_CAND        4096

/* body.flags 位 */
#define DG_BF_GROUND   1
#define DG_BF_WALL     2
#define DG_BF_CEILING  4

static int    g_auto = 1;                     /* eng_frame_begin() 里自动推进 */
static int    g_paused = 0;
static int    g_interp = 1;                   /* 有 body 的实体做渲染插值 */
static float  g_gravity_x = 0.0f;
static float  g_gravity_y = DG_PHYS_DEFAULT_GRAVITY;
static float  g_step_hz = (float)DG_PHYS_DEFAULT_HZ;
static int    g_max_substeps = DG_PHYS_MAX_SUBSTEPS;
static double g_accum = 0.0;
static double g_time = 0.0;
static float  g_alpha = 0.0f;
static float  g_frame_dt = 0.0f;
static int    g_last_substeps = 0;

/* ============================ 形状 ============================ */
typedef struct {
    int   kind;                 /* DG_SHAPE_* */
    float x, y;                 /* 中心(AABB/圆)或线段中心(胶囊)*/
    float hw, hh;               /* AABB 半宽高;圆:半径在 hw;胶囊:半径 hw + 半长 hh */
} DgShape;

static float dg_support_x(const DgShape *s) { return s->hw; }
static float dg_support_y(const DgShape *s) {
    return s->kind == DG_SHAPE_CAPSULE ? s->hh + s->hw : s->hh;
}

static float dg_seg_t(float ax, float ay, float bx, float by, float px, float py) {
    const float vx = bx - ax, vy = by - ay;
    const float wx = px - ax, wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    if (len2 <= 1e-12f) return 0.0f;
    float t = (wx * vx + wy * vy) / len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

static float dg_seg_point_d2(float ax, float ay, float bx, float by, float px, float py) {
    const float t = dg_seg_t(ax, ay, bx, by, px, py);
    const float dx = px - (ax + t * (bx - ax)), dy = py - (ay + t * (by - ay));
    return dx * dx + dy * dy;
}

/* 线段-线段最近距离平方(胶囊 vs 胶囊)*/
static float dg_seg_seg_d2(float ax, float ay, float bx, float by,
                           float cx, float cy, float dx, float dy) {
    const float ux = bx - ax, uy = by - ay;
    const float vx = dx - cx, vy = dy - cy;
    const float wx = ax - cx, wy = ay - cy;
    const float a = ux * ux + uy * uy, b = ux * vx + uy * vy, c = vx * vx + vy * vy;
    const float d = ux * wx + uy * wy, e = vx * wx + vy * wy;
    const float den = a * c - b * b;
    float s = 0.0f;
    if (den > 1e-9f) {
        s = (b * e - c * d) / den;
        if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
    } else if (a > 1e-9f) {
        s = -d / a;
        if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
    }
    /* 交替投影两次:对两个线段够用,而且结果确定(不迭代到收敛,免得受浮点抖动)*/
    float t = c > 1e-9f ? (e + b * s) / c : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    if (a > 1e-9f) {
        s = (b * t - d) / a;
        if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
    }
    const float px = ax + s * ux, py = ay + s * uy;
    const float qx = cx + t * vx, qy = cy + t * vy;
    const float ddx = px - qx, ddy = py - qy;
    return ddx * ddx + ddy * ddy;
}

static int dg_aabb_aabb(const DgShape *a, const DgShape *b) {
    return fabsf(a->x - b->x) < (a->hw + b->hw) && fabsf(a->y - b->y) < (a->hh + b->hh);
}

static int dg_aabb_circle(const DgShape *box, float cx, float cy, float r) {
    const float dx = fabsf(cx - box->x), dy = fabsf(cy - box->y);
    const float ox = dx - box->hw, oy = dy - box->hh;
    if (ox <= 0.0f && oy <= 0.0f) return 1;                     /* 圆心在盒内 */
    const float oxc = ox > 0.0f ? ox : 0.0f, oyc = oy > 0.0f ? oy : 0.0f;
    return oxc * oxc + oyc * oyc < r * r;
}

/* 胶囊 = 两个端圆 + 中间矩形 的**并集** —— 用它把复杂形状拆成已知精确的测试 */
static int dg_capsule_aabb(const DgShape *cap, const DgShape *box) {
    if (dg_aabb_circle(box, cap->x, cap->y - cap->hh, cap->hw)) return 1;
    if (dg_aabb_circle(box, cap->x, cap->y + cap->hh, cap->hw)) return 1;
    DgShape mid = *cap;
    mid.kind = DG_SHAPE_AABB;
    return dg_aabb_aabb(&mid, box);
}

/* 精确重叠测试(不管 layer/mask,过滤在外层做)*/
static int dg_shape_overlap(const DgShape *a, const DgShape *b) {
    if (a->kind == DG_SHAPE_AABB && b->kind == DG_SHAPE_AABB) return dg_aabb_aabb(a, b);
    if (a->kind == DG_SHAPE_AABB && b->kind == DG_SHAPE_CIRCLE) return dg_aabb_circle(a, b->x, b->y, b->hw);
    if (a->kind == DG_SHAPE_CIRCLE && b->kind == DG_SHAPE_AABB) return dg_aabb_circle(b, a->x, a->y, a->hw);
    if (a->kind == DG_SHAPE_CIRCLE && b->kind == DG_SHAPE_CIRCLE) {
        const float dx = a->x - b->x, dy = a->y - b->y, r = a->hw + b->hw;
        return dx * dx + dy * dy < r * r;
    }
    if (a->kind == DG_SHAPE_CAPSULE && b->kind == DG_SHAPE_AABB) return dg_capsule_aabb(a, b);
    if (a->kind == DG_SHAPE_AABB && b->kind == DG_SHAPE_CAPSULE) return dg_capsule_aabb(b, a);
    if (a->kind == DG_SHAPE_CAPSULE && b->kind == DG_SHAPE_CIRCLE) {
        const float r = a->hw + b->hw;
        return dg_seg_point_d2(a->x, a->y - a->hh, a->x, a->y + a->hh, b->x, b->y) < r * r;
    }
    if (a->kind == DG_SHAPE_CIRCLE && b->kind == DG_SHAPE_CAPSULE) {
        const float r = a->hw + b->hw;
        return dg_seg_point_d2(b->x, b->y - b->hh, b->x, b->y + b->hh, a->x, a->y) < r * r;
    }
    const float r = a->hw + b->hw;
    return dg_seg_seg_d2(a->x, a->y - a->hh, a->x, a->y + a->hh,
                         b->x, b->y - b->hh, b->x, b->y + b->hh) < r * r;
}

/* ============================ 宽相:均匀网格空间哈希 ============================ */
#define DG_MAX_ENTRIES     4096
#define DG_GRID_BUCKETS    2048         /* 必须是 2 的幂 */
#define DG_MAX_ENTRY_CELLS 4096         /* 单个形状最多占多少格,超过进 overflow */

typedef struct {
    uint32_t obj;                       /* 0 = 瓦片(世界几何),不是实体 */
    DgShape  sh;
    int32_t  layer, mask, trigger, motion;
} DgEntry;

typedef struct { int32_t kx, ky, start, count; } DgBucket;

static DgEntry   g_entry[DG_MAX_ENTRIES];
static int       g_nentry = 0;
static DgBucket  g_bucket[DG_GRID_BUCKETS];
static int32_t  *g_items = NULL;
static int32_t   g_items_cap = 0;
static int32_t   g_overflow[DG_MAX_ENTRIES];
static int       g_noverflow = 0;
static float     g_cell = 64.0f;
static int       g_dirty = 1;
static int32_t   g_stamp[DG_MAX_ENTRIES];
static int32_t   g_stamp_gen = 0;
static DgEntry   g_cand[DG_PHYS_MAX_CAND];

void dg_phys_invalidate(void) { g_dirty = 1; }

static uint32_t dg_hash_cell(int32_t cx, int32_t cy) {
    return (((uint32_t)cx * 73856093u) ^ ((uint32_t)cy * 19349663u)) & (DG_GRID_BUCKETS - 1);
}

static int dg_bucket_find(int32_t cx, int32_t cy, int create) {
    const uint32_t h = dg_hash_cell(cx, cy);
    for (uint32_t i = 0; i < DG_GRID_BUCKETS; i++) {
        const uint32_t idx = (h + i) & (DG_GRID_BUCKETS - 1);
        DgBucket *b = &g_bucket[idx];
        if (b->start < 0) {
            if (!create) return -1;
            b->kx = cx; b->ky = cy; b->start = 0; b->count = 0;
            return (int)idx;
        }
        if (b->kx == cx && b->ky == cy) return (int)idx;
    }
    return -1;                          /* 哈希表满:调用方按 overflow/线性兜底 */
}

/* 形状覆盖的格子范围(太大就标记 too_many,交给 overflow 列表)*/
typedef void (*DgCellFn)(void *user, int32_t cx, int32_t cy);
static void dg_shape_cells(const DgShape *s, void *user, DgCellFn fn, int *too_many) {
    const float sx = dg_support_x(s), sy = dg_support_y(s);
    const int32_t c0 = (int32_t)floorf((s->x - sx) / g_cell), c1 = (int32_t)floorf((s->x + sx) / g_cell);
    const int32_t r0 = (int32_t)floorf((s->y - sy) / g_cell), r1 = (int32_t)floorf((s->y + sy) / g_cell);
    if ((double)(c1 - c0 + 1) * (double)(r1 - r0 + 1) > (double)DG_MAX_ENTRY_CELLS) {
        if (too_many) *too_many = 1;
        return;
    }
    for (int32_t r = r0; r <= r1; r++)
        for (int32_t c = c0; c <= c1; c++) fn(user, c, r);
}

static void dg_count_cell(void *user, int32_t cx, int32_t cy) {
    (void)user;
    const int b = dg_bucket_find(cx, cy, 1);
    if (b >= 0) g_bucket[b].count++;
}

typedef struct { int32_t *cursor; int32_t bi; } DgFillCtx;
static void dg_emit_cell(void *user, int32_t cx, int32_t cy) {
    DgFillCtx *f = (DgFillCtx *)user;
    const int b = dg_bucket_find(cx, cy, 0);
    if (b < 0) return;
    g_items[f->cursor[b]++] = f->bi;
}

/* 收集一个实体的碰撞形状(需要 transform + collider)*/
static int dg_entry_of(uint32_t obj, DgEntry *out) {
    const DgCollider *c = (const DgCollider *)dg_comp_get(obj, DG_C_COLLIDER);
    if (!c) return 0;
    double wx = 0.0, wy = 0.0;
    if (dg_world_pos(obj, &wx, &wy)) return 0;
    out->obj = obj;
    out->sh.kind = c->kind;
    out->sh.x = (float)wx + c->ox;
    out->sh.y = (float)wy + c->oy;
    out->sh.hw = c->hw;
    out->sh.hh = (c->kind == DG_SHAPE_CIRCLE) ? c->hw : c->hh;
    out->layer = c->layer;
    out->mask = c->mask;
    out->trigger = c->is_trigger ? 1 : 0;
    const DgBody *b = (const DgBody *)dg_comp_get(obj, DG_C_BODY);
    out->motion = b ? b->motion : DG_MOTION_STATIC;
    return 1;
}

static int dg_phys_rebuild(void) {
    if (!g_dirty) return 0;
    for (uint32_t i = 0; i < DG_GRID_BUCKETS; i++) {
        g_bucket[i].start = -1;
        g_bucket[i].count = 0;
        g_bucket[i].kx = g_bucket[i].ky = 0;
    }
    g_nentry = 0;
    g_noverflow = 0;

    /* 1) 收集所有碰撞体 */
    static uint32_t ids[DG_MAX_ENTRIES];
    const int n = dg_scene_collect_objects(ids, DG_MAX_ENTRIES);
    for (int i = 0; i < n && g_nentry < DG_MAX_ENTRIES; i++) {
        DgEntry e;
        if (dg_entry_of(ids[i], &e)) g_entry[g_nentry++] = e;
    }

    /* 2) 数每格条目数 */
    for (int i = 0; i < g_nentry; i++) {
        int too_many = 0;
        dg_shape_cells(&g_entry[i].sh, NULL, dg_count_cell, &too_many);
        if (too_many && g_noverflow < DG_MAX_ENTRIES) g_overflow[g_noverflow++] = i;
    }

    /* 3) 前缀和 → 每格起点 */
    int32_t total_items = 0;
    for (uint32_t i = 0; i < DG_GRID_BUCKETS; i++) {
        if (g_bucket[i].start >= 0) {
            const int32_t c = g_bucket[i].count;
            g_bucket[i].start = total_items;
            total_items += c;
        }
    }
    if (total_items > g_items_cap) {
        int32_t *ni = (int32_t *)realloc(g_items, (size_t)total_items * sizeof(int32_t));
        if (!ni) { dg_error("out of memory building collision grid"); g_dirty = 0; return -1; }
        g_items = ni;
        g_items_cap = total_items;
    }

    /* 4) 填条目 */
    static int32_t cursor[DG_GRID_BUCKETS];
    for (uint32_t i = 0; i < DG_GRID_BUCKETS; i++)
        cursor[i] = g_bucket[i].start >= 0 ? g_bucket[i].start : 0;
    for (int i = 0; i < g_nentry; i++) {
        int too_many = 0;
        DgFillCtx f;
        f.cursor = cursor;
        f.bi = i;
        dg_shape_cells(&g_entry[i].sh, &f, dg_emit_cell, &too_many);
    }
    g_dirty = 0;
    return 0;
}

/* layer/mask 过滤:任意一方 mask=0 表示"不筛选" */
static int dg_mask_pair(int32_t la, int32_t ma, int32_t lb, int32_t mb) {
    if (ma == 0 || mb == 0) return 1;
    return (la & mb) != 0 && (lb & ma) != 0;
}

static int dg_mask_ok_raw(int32_t layer, int32_t mask, int32_t filter) {
    if (filter == 0 || mask == 0) return 1;    /* 调用方不筛选 / 该体不参与筛选 */
    return (layer & filter) != 0;
}

/* 收集与矩形相交的实体碰撞体。want_trigger=0 时排除触发区(移动解算用)。
   返回条数(-1 出错)。 */
static int dg_collect_rect(float x, float y, float w, float h, int want_trigger,
                           DgEntry *out, int cap) {
    if (dg_phys_rebuild()) return -1;
    if (g_stamp_gen == 0x7FFFFFFF) { memset(g_stamp, 0, sizeof g_stamp); g_stamp_gen = 0; }
    g_stamp_gen++;
    const DgShape probe = { DG_SHAPE_AABB, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f };
    int n = 0;

    const int32_t c0 = (int32_t)floorf(x / g_cell), c1 = (int32_t)floorf((x + w) / g_cell);
    const int32_t r0 = (int32_t)floorf(y / g_cell), r1 = (int32_t)floorf((y + h) / g_cell);
    const double ncells = (double)(c1 - c0 + 1) * (double)(r1 - r0 + 1);

    if (ncells > (double)DG_MAX_ENTRY_CELLS) {     /* 巨大查询:线性兜底 */
        for (int i = 0; i < g_nentry; i++) {
            if (g_stamp[i] == g_stamp_gen) continue;
            g_stamp[i] = g_stamp_gen;
            if (!want_trigger && g_entry[i].trigger) continue;
            if (!dg_shape_overlap(&probe, &g_entry[i].sh)) continue;
            if (n < cap) out[n++] = g_entry[i];
        }
        return n;
    }
    for (int32_t r = r0; r <= r1; r++)
        for (int32_t c = c0; c <= c1; c++) {
            const int b = dg_bucket_find(c, r, 0);
            if (b < 0) continue;
            for (int32_t k = 0; k < g_bucket[b].count; k++) {
                const int32_t ei = g_items[g_bucket[b].start + k];
                if (g_stamp[ei] == g_stamp_gen) continue;
                g_stamp[ei] = g_stamp_gen;
                if (!want_trigger && g_entry[ei].trigger) continue;
                if (!dg_shape_overlap(&probe, &g_entry[ei].sh)) continue;
                if (n < cap) out[n++] = g_entry[ei];
            }
        }
    for (int i = 0; i < g_noverflow; i++) {
        const int32_t ei = g_overflow[i];
        if (g_stamp[ei] == g_stamp_gen) continue;
        g_stamp[ei] = g_stamp_gen;
        if (!want_trigger && g_entry[ei].trigger) continue;
        if (!dg_shape_overlap(&probe, &g_entry[ei].sh)) continue;
        if (n < cap) out[n++] = g_entry[ei];
    }
    return n;
}

/* ============================ 查询游标 ============================ */
#define DG_MAX_CURSORS 8

typedef struct {
    int       used;
    uint32_t *ids;
    int       n, cap, pos;
} DgCursor;

static DgCursor g_cursors[DG_MAX_CURSORS + 1];    /* 1 基 id */

static DgCursor *dg_cursor_of(int32_t id) {
    if (id < 1 || id > DG_MAX_CURSORS || !g_cursors[id].used) return NULL;
    return &g_cursors[id];
}

static int32_t dg_cursor_alloc(void) {
    for (int i = 1; i <= DG_MAX_CURSORS; i++) {
        if (!g_cursors[i].used) {
            g_cursors[i].used = 1;
            g_cursors[i].n = g_cursors[i].pos = 0;
            return i;
        }
    }
    dg_error("too many open queries (max %d) —— 记得 eng_query_end()", DG_MAX_CURSORS);
    return -1;
}

static void dg_cursor_push(DgCursor *c, uint32_t id) {
    if (c->n >= c->cap) {
        const int nc = c->cap ? c->cap * 2 : 16;
        uint32_t *ni = (uint32_t *)realloc(c->ids, (size_t)nc * sizeof(uint32_t));
        if (!ni) { dg_error("out of memory growing query result"); return; }
        c->ids = ni;
        c->cap = nc;
    }
    c->ids[c->n++] = id;
}

int32_t dg_phys_query_end(int32_t cur) {
    DgCursor *c = dg_cursor_of(cur);
    if (!c) { dg_error("query cursor %d is not open", cur); return -1; }
    free(c->ids);
    memset(c, 0, sizeof *c);
    return 0;
}

int32_t dg_phys_query_count(int32_t cur) {
    DgCursor *c = dg_cursor_of(cur);
    if (!c) { dg_error("query cursor %d is not open", cur); return -1; }
    return c->n;
}

int32_t dg_phys_query_next(int32_t cur) {
    DgCursor *c = dg_cursor_of(cur);
    if (!c) { dg_error("query cursor %d is not open", cur); return 0; }
    if (c->pos >= c->n) return 0;
    return (int32_t)c->ids[c->pos++];
}

int32_t dg_phys_query_reset(int32_t cur) {
    DgCursor *c = dg_cursor_of(cur);
    if (!c) { dg_error("query cursor %d is not open", cur); return -1; }
    c->pos = 0;
    return 0;
}

int32_t dg_phys_query_at(int32_t cur, int32_t i) {
    DgCursor *c = dg_cursor_of(cur);
    if (!c) { dg_error("query cursor %d is not open", cur); return 0; }
    if (i < 0 || i >= c->n) return 0;
    return (int32_t)c->ids[i];
}

static int32_t dg_query_shape(const DgShape *probe, int32_t mask) {
    const float sx = dg_support_x(probe), sy = dg_support_y(probe);
    const int n = dg_collect_rect(probe->x - sx, probe->y - sy, sx * 2.0f, sy * 2.0f, 1,
                                  g_cand, DG_PHYS_MAX_CAND);
    if (n < 0) return -1;
    const int32_t cur = dg_cursor_alloc();
    if (cur < 0) return -1;
    DgCursor *c = &g_cursors[cur];
    for (int i = 0; i < n; i++) {
        if (!dg_mask_ok_raw(g_cand[i].layer, g_cand[i].mask, mask)) continue;
        if (!dg_shape_overlap(probe, &g_cand[i].sh)) continue;
        if (!dg_object_alive(g_cand[i].obj)) continue;      /* 宽相可能稍旧 */
        dg_cursor_push(c, g_cand[i].obj);
    }
    return cur;
}

int32_t dg_phys_query_rect(float x, float y, float w, float h, int32_t mask) {
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    const DgShape probe = { DG_SHAPE_AABB, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f };
    return dg_query_shape(&probe, mask);
}

int32_t dg_phys_query_circle(float cx, float cy, float r, int32_t mask) {
    if (r < 0.0f) { dg_error("query circle radius %g < 0", (double)r); return -1; }
    const DgShape probe = { DG_SHAPE_CIRCLE, cx, cy, r, r };
    return dg_query_shape(&probe, mask);
}

int32_t dg_phys_query_point(float x, float y, int32_t mask) {
    return dg_phys_query_rect(x, y, 0.0f, 0.0f, mask);
}

/* ============================ 命中结果槽 ============================ */
static DgHit g_hit;

void dg_phys_hit_clear(void) { memset(&g_hit, 0, sizeof g_hit); }
const DgHit *dg_phys_hit(void) { return &g_hit; }

/* ============================ 射线 ============================ */
static int dg_ray_aabb(float ox, float oy, float dx, float dy, float maxd,
                       const DgShape *b, float *t, float *nx, float *ny) {
    const float minx = b->x - b->hw, maxx = b->x + b->hw;
    const float miny = b->y - b->hh, maxy = b->y + b->hh;
    float tmin = 0.0f, tmax = maxd;
    float n_x = 0.0f, n_y = 0.0f;
    if (fabsf(dx) < 1e-9f) {
        if (ox < minx || ox > maxx) return 0;
    } else {
        float t1 = (minx - ox) / dx, t2 = (maxx - ox) / dx;
        float sign = -1.0f;
        if (t1 > t2) { const float tt = t1; t1 = t2; t2 = tt; sign = 1.0f; }
        if (t1 > tmin) { tmin = t1; n_x = sign; n_y = 0.0f; }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;
    }
    if (fabsf(dy) < 1e-9f) {
        if (oy < miny || oy > maxy) return 0;
    } else {
        float t1 = (miny - oy) / dy, t2 = (maxy - oy) / dy;
        float sign = -1.0f;
        if (t1 > t2) { const float tt = t1; t1 = t2; t2 = tt; sign = 1.0f; }
        if (t1 > tmin) { tmin = t1; n_x = 0.0f; n_y = sign; }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;
    }
    if (tmin > maxd) return 0;
    *t = tmin;
    *nx = n_x; *ny = n_y;
    return 1;
}

static int dg_ray_circle(float ox, float oy, float dx, float dy, float maxd,
                         float cx, float cy, float r, float *t, float *nx, float *ny) {
    const float mx = ox - cx, my = oy - cy;
    const float b = mx * dx + my * dy;
    const float c = mx * mx + my * my - r * r;
    if (c > 0.0f && b > 0.0f) return 0;                    /* 起点在外且背离 */
    const float disc = b * b - c;
    if (disc < 0.0f) return 0;
    float tt = -b - sqrtf(disc);
    if (tt < 0.0f) tt = 0.0f;                              /* 起点已在圆内 */
    if (tt > maxd) return 0;
    *t = tt;
    const float hx = ox + dx * tt - cx, hy = oy + dy * tt - cy;
    const float len = sqrtf(hx * hx + hy * hy);
    *nx = len > 1e-6f ? hx / len : 0.0f;
    *ny = len > 1e-6f ? hy / len : 0.0f;
    return 1;
}

/* inflate != 0 时把目标按移动体半宽高放大(AABB Minkowski 近似;对 AABB 精确)*/
static int dg_ray_shape(float ox, float oy, float dx, float dy, float maxd,
                        const DgShape *s, float inflate_hw, float inflate_hh,
                        float *t, float *nx, float *ny) {
    DgShape b = *s;
    if (inflate_hw != 0.0f || inflate_hh != 0.0f) {
        b.hw += inflate_hw;
        b.hh += inflate_hh;
        b.kind = DG_SHAPE_AABB;
    }
    switch (b.kind) {
    case DG_SHAPE_AABB:
        return dg_ray_aabb(ox, oy, dx, dy, maxd, &b, t, nx, ny);
    case DG_SHAPE_CIRCLE:
        return dg_ray_circle(ox, oy, dx, dy, maxd, b.x, b.y, b.hw, t, nx, ny);
    default: {
        int hit = 0;
        float bt = maxd, bx = 0.0f, by = 0.0f, tt, tx, ty;
        if (dg_ray_circle(ox, oy, dx, dy, maxd, b.x, b.y - b.hh, b.hw, &tt, &tx, &ty) && tt <= bt) {
            bt = tt; bx = tx; by = ty; hit = 1;
        }
        if (dg_ray_circle(ox, oy, dx, dy, maxd, b.x, b.y + b.hh, b.hw, &tt, &tx, &ty) && tt <= bt) {
            bt = tt; bx = tx; by = ty; hit = 1;
        }
        DgShape mid = b;
        mid.kind = DG_SHAPE_AABB;
        if (dg_ray_aabb(ox, oy, dx, dy, maxd, &mid, &tt, &tx, &ty) && tt <= bt) {
            bt = tt; bx = tx; by = ty; hit = 1;
        }
        if (hit) { *t = bt; *nx = bx; *ny = by; }
        return hit;
    }
    }
}

typedef struct {
    float ox, oy, dx, dy, maxd;
    float t, nx, ny;
    float inflate_hw, inflate_hh;
    int   hit;
    int32_t tile;
} DgRayTileCtx;

static void dg_ray_tile_fn(void *user, float x, float y, float w, float h, int32_t tile) {
    DgRayTileCtx *c = (DgRayTileCtx *)user;
    const DgShape s = { DG_SHAPE_AABB, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f };
    float t, nx, ny;
    if (!dg_ray_shape(c->ox, c->oy, c->dx, c->dy, c->maxd, &s, c->inflate_hw, c->inflate_hh,
                      &t, &nx, &ny)) return;
    if (!c->hit || t < c->t) {
        c->hit = 1; c->t = t; c->nx = nx; c->ny = ny; c->tile = tile;
    }
}

/* 场上所有瓦片地图(它们是普通实体,所以枚举一下)*/
static int32_t dg_each_tilemap(uint32_t *out, int cap) {
    static uint32_t ids[DG_MAX_ENTRIES];
    const int n = dg_scene_collect_objects(ids, DG_MAX_ENTRIES);
    int32_t m = 0;
    for (int i = 0; i < n && m < cap; i++)
        if (dg_comp_get(ids[i], DG_C_TILEMAP)) out[m++] = ids[i];
    return m;
}

int32_t dg_phys_raycast(float x, float y, float dx, float dy, float dist,
                        int32_t mask, uint32_t ignore) {
    dg_phys_hit_clear();
    if (dist <= 0.0f) { dg_error("raycast distance %g <= 0", (double)dist); return -1; }
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-9f) { dg_error("raycast direction is zero"); return -1; }
    dx /= len; dy /= len;

    const float minx = fminf(x, x + dx * dist), maxx = fmaxf(x, x + dx * dist);
    const float miny = fminf(y, y + dy * dist), maxy = fmaxf(y, y + dy * dist);
    const int n = dg_collect_rect(minx - g_cell, miny - g_cell,
                                  (maxx - minx) + 2 * g_cell, (maxy - miny) + 2 * g_cell,
                                  1, g_cand, DG_PHYS_MAX_CAND);
    if (n < 0) return -1;

    float best = dist;
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (ignore && g_cand[i].obj == ignore) continue;
        if (!dg_mask_ok_raw(g_cand[i].layer, g_cand[i].mask, mask)) continue;
        if (!dg_object_alive(g_cand[i].obj)) continue;
        float t, nx, ny;
        if (!dg_ray_shape(x, y, dx, dy, best, &g_cand[i].sh, 0.0f, 0.0f, &t, &nx, &ny)) continue;
        best = t;
        found = 1;
        g_hit.obj = (int32_t)g_cand[i].obj;
        g_hit.tile = 0;
        g_hit.t = t;
        g_hit.x = x + dx * t;
        g_hit.y = y + dy * t;
        g_hit.nx = nx; g_hit.ny = ny;
    }

    uint32_t maps[64];
    const int32_t nmaps = dg_each_tilemap(maps, 64);
    for (int32_t m = 0; m < nmaps; m++) {
        DgRayTileCtx ctx;
        ctx.ox = x; ctx.oy = y; ctx.dx = dx; ctx.dy = dy; ctx.maxd = best;
        ctx.t = best; ctx.nx = ctx.ny = 0.0f; ctx.hit = 0; ctx.tile = 0;
        ctx.inflate_hw = ctx.inflate_hh = 0.0f;
        dg_tilemap_each_solid_in(maps[m], minx - g_cell, miny - g_cell,
                                 (maxx - minx) + 2 * g_cell, (maxy - miny) + 2 * g_cell,
                                 &ctx, dg_ray_tile_fn);
        if (ctx.hit && ctx.t <= best) {
            best = ctx.t;
            found = 1;
            g_hit.obj = 0;                 /* obj = 0 且 tile != 0 表示"打到地形" */
            g_hit.tile = ctx.tile;
            g_hit.t = ctx.t;
            g_hit.x = x + dx * ctx.t;
            g_hit.y = y + dy * ctx.t;
            g_hit.nx = ctx.nx; g_hit.ny = ctx.ny;
        }
    }
    return found ? 1 : 0;
}

int32_t dg_phys_sweep_box(float x, float y, float hw, float hh,
                          float dx, float dy, int32_t mask, uint32_t ignore) {
    dg_phys_hit_clear();
    if (hw < 0.0f || hh < 0.0f) { dg_error("sweep half extents must be >= 0"); return -1; }
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-9f) { dg_error("sweep direction is zero"); return -1; }
    const float ux = dx / len, uy = dy / len;
    const float dist = len;

    const float minx = fminf(x, x + dx), maxx = fmaxf(x, x + dx);
    const float miny = fminf(y, y + dy), maxy = fmaxf(y, y + dy);
    const float ex = hw + g_cell, ey = hh + g_cell;
    const int n = dg_collect_rect(minx - ex, miny - ey,
                                  (maxx - minx) + 2 * ex, (maxy - miny) + 2 * ey,
                                  1, g_cand, DG_PHYS_MAX_CAND);
    if (n < 0) return -1;

    float best = dist;
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (ignore && g_cand[i].obj == ignore) continue;
        if (!dg_mask_ok_raw(g_cand[i].layer, g_cand[i].mask, mask)) continue;
        if (!dg_object_alive(g_cand[i].obj)) continue;
        float t, nx, ny;
        if (!dg_ray_shape(x, y, ux, uy, best, &g_cand[i].sh, hw, hh, &t, &nx, &ny)) continue;
        best = t;
        found = 1;
        g_hit.obj = (int32_t)g_cand[i].obj;
        g_hit.tile = 0;
        g_hit.t = t;
        g_hit.x = x + ux * t;
        g_hit.y = y + uy * t;
        g_hit.nx = nx; g_hit.ny = ny;
    }
    uint32_t maps[64];
    const int32_t nmaps = dg_each_tilemap(maps, 64);
    for (int32_t m = 0; m < nmaps; m++) {
        DgRayTileCtx ctx;
        ctx.ox = x; ctx.oy = y; ctx.dx = ux; ctx.dy = uy; ctx.maxd = best;
        ctx.t = best; ctx.nx = ctx.ny = 0.0f; ctx.hit = 0; ctx.tile = 0;
        ctx.inflate_hw = hw; ctx.inflate_hh = hh;
        dg_tilemap_each_solid_in(maps[m], minx - ex, miny - ey,
                                 (maxx - minx) + 2 * ex, (maxy - miny) + 2 * ey,
                                 &ctx, dg_ray_tile_fn);
        if (ctx.hit && ctx.t <= best) {
            best = ctx.t;
            found = 1;
            g_hit.obj = 0;
            g_hit.tile = ctx.tile;
            g_hit.t = ctx.t;
            g_hit.x = x + ux * ctx.t;
            g_hit.y = y + uy * ctx.t;
            g_hit.nx = ctx.nx; g_hit.ny = ctx.ny;
        }
    }
    return found ? 1 : 0;
}

/* ============================ 障碍收集 / move-and-slide ============================ */
typedef struct {
    uint32_t obj;                       /* 0 = 瓦片 */
    DgShape  sh;
} DgObs;

typedef struct { DgObs *v; int n, cap; } DgObsList;

static void dg_obs_push(void *user, float x, float y, float w, float h, int32_t tile) {
    DgObsList *l = (DgObsList *)user;
    (void)tile;
    if (l->n >= l->cap) return;
    l->v[l->n].obj = 0;
    l->v[l->n].sh.kind = DG_SHAPE_AABB;
    l->v[l->n].sh.x = x + w * 0.5f;
    l->v[l->n].sh.y = y + h * 0.5f;
    l->v[l->n].sh.hw = w * 0.5f;
    l->v[l->n].sh.hh = h * 0.5f;
    l->n++;
}

/* 收集 mover 周围(给定世界矩形)的障碍:非触发区的实体碰撞体 + 实心瓦片 */
static int dg_collect_obstacles(float x, float y, float w, float h,
                                int32_t mlayer, int32_t mmask, uint32_t skip,
                                DgObsList *out) {
    const int n = dg_collect_rect(x, y, w, h, 0, g_cand, DG_PHYS_MAX_CAND);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        if (g_cand[i].obj == skip) continue;
        if (!dg_mask_pair(mlayer, mmask, g_cand[i].layer, g_cand[i].mask)) continue;
        if (out->n < out->cap) {
            out->v[out->n].obj = g_cand[i].obj;
            out->v[out->n].sh = g_cand[i].sh;
            out->n++;
        }
    }
    uint32_t maps[64];
    const int32_t nmaps = dg_each_tilemap(maps, 64);
    for (int32_t m = 0; m < nmaps; m++)
        dg_tilemap_each_solid_in(maps[m], x, y, w, h, out, dg_obs_push);
    return out->n;
}

/* 沿一个轴把 mover 推出所有重叠的障碍。返回 1 = 该轴被挡住。 */
static int dg_resolve_axis(DgShape *m, int axis, float dir, DgObs *obs, int n) {
    int hit = 0;
    for (int iter = 0; iter < 4; iter++) {
        int moved = 0;
        for (int i = 0; i < n; i++) {
            if (!dg_shape_overlap(m, &obs[i].sh)) continue;
            const float need = dg_support_x(m) + dg_support_x(&obs[i].sh);
            const float needy = dg_support_y(m) + dg_support_y(&obs[i].sh);
            if (axis == 0) {
                if (dir > 0.0f) {
                    const float nx = obs[i].sh.x - need;
                    if (nx < m->x) { m->x = nx; moved = 1; }
                } else if (dir < 0.0f) {
                    const float nx = obs[i].sh.x + need;
                    if (nx > m->x) { m->x = nx; moved = 1; }
                } else {                                   /* 静止:往浅的一侧推 */
                    const float nx = (m->x >= obs[i].sh.x) ? obs[i].sh.x + need
                                                           : obs[i].sh.x - need;
                    if (nx != m->x) { m->x = nx; moved = 1; }
                }
            } else {
                if (dir > 0.0f) {
                    const float ny = obs[i].sh.y - needy;
                    if (ny < m->y) { m->y = ny; moved = 1; }
                } else if (dir < 0.0f) {
                    const float ny = obs[i].sh.y + needy;
                    if (ny > m->y) { m->y = ny; moved = 1; }
                } else {
                    const float ny = (m->y >= obs[i].sh.y) ? obs[i].sh.y + needy
                                                           : obs[i].sh.y - needy;
                    if (ny != m->y) { m->y = ny; moved = 1; }
                }
            }
            hit = 1;
        }
        if (!moved) break;
    }
    return hit;
}

/* 把 mover 的世界位置写回 transform(减掉父链贡献)*/
static void dg_write_back(uint32_t obj, DgTransform *t, float wx, float wy,
                         float poff_x, float poff_y) {
    t->x = wx - poff_x;
    t->y = wy - poff_y;
    (void)obj;
    dg_phys_invalidate();
}

int32_t dg_phys_move(uint32_t obj, float dx, float dy, int *out_hit_x, int *out_hit_y) {
    if (out_hit_x) *out_hit_x = 0;
    if (out_hit_y) *out_hit_y = 0;

    /* 先判存活:否则下面取 transform 失败会报"没有 transform",把真正的原因盖掉 */
    if (!dg_object_alive(obj)) { dg_error("object %u is not alive", obj); return -1; }
    DgTransform *t = (DgTransform *)dg_comp_get(obj, DG_C_TRANSFORM);
    if (!t) { dg_error("object %u has no 'transform'", obj); return -1; }
    const DgCollider *c = (const DgCollider *)dg_comp_get(obj, DG_C_COLLIDER);
    if (!c) {                                   /* 没有碰撞体:纯位移,不碰撞 */
        t->x += dx;
        t->y += dy;
        dg_phys_invalidate();
        return 0;
    }

    double dwx = 0.0, dwy = 0.0;
    if (dg_world_pos(obj, &dwx, &dwy)) return -1;
    const float poff_x = (float)dwx - t->x, poff_y = (float)dwy - t->y;
    float wx = (float)dwx, wy = (float)dwy;     /* 实体世界位置 */

    DgShape m;
    m.kind = c->kind;
    m.hw = c->hw;
    m.hh = (c->kind == DG_SHAPE_CIRCLE) ? c->hw : c->hh;

    DgBody *b = (DgBody *)dg_comp_get(obj, DG_C_BODY);
    if (b && b->flags) b->flags = 0;

    /* 把一次位移拆成不超过 DG_PHYS_MAX_STEP_PX 的子步(防穿透)*/
    const float total = fmaxf(fabsf(dx), fabsf(dy));
    int nstep = 1 + (int)(total / DG_PHYS_MAX_STEP_PX);
    const float sx = dx / (float)nstep, sy = dy / (float)nstep;

    static DgObs obs[2 * DG_PHYS_MAX_CAND];
    DgObsList list;
    list.v = obs;

    int hit_x = 0, hit_y = 0;
    for (int s = 0; s < nstep; s++) {
        /* --- X --- */
        if (sx != 0.0f) {
            wx += sx;
            m.x = wx + c->ox;
            m.y = wy + c->oy;
            list.n = 0;
            list.cap = (int)(sizeof obs / sizeof obs[0]);
            const float ex = dg_support_x(&m) + fabsf(sx) + 1.0f;
            const float ey = dg_support_y(&m) + 1.0f;
            if (dg_collect_obstacles(m.x - ex, m.y - ey, ex * 2.0f, ey * 2.0f,
                                     c->layer, c->mask, obj, &list) < 0) return -1;
            if (dg_resolve_axis(&m, 0, sx, obs, list.n)) {
                hit_x = 1;
                wx = m.x - c->ox;
            }
        }
        /* --- Y --- */
        if (sy != 0.0f) {
            wy += sy;
            m.x = wx + c->ox;
            m.y = wy + c->oy;
            list.n = 0;
            list.cap = (int)(sizeof obs / sizeof obs[0]);
            const float ex = dg_support_x(&m) + 1.0f;
            const float ey = dg_support_y(&m) + fabsf(sy) + 1.0f;
            if (dg_collect_obstacles(m.x - ex, m.y - ey, ex * 2.0f, ey * 2.0f,
                                     c->layer, c->mask, obj, &list) < 0) return -1;
            if (dg_resolve_axis(&m, 1, sy, obs, list.n)) {
                hit_y = 1;
                wy = m.y - c->oy;
            }
        }
    }

    if (b) {
        if (hit_x) b->flags |= DG_BF_WALL;
        if (hit_y) b->flags |= (sy > 0.0f) ? DG_BF_GROUND : DG_BF_CEILING;
    }
    dg_write_back(obj, t, wx, wy, poff_x, poff_y);
    if (out_hit_x) *out_hit_x = hit_x;
    if (out_hit_y) *out_hit_y = hit_y;
    return (hit_x ? 1 : 0) | (hit_y ? 2 : 0);
}

int dg_phys_on_ground(uint32_t obj) {
    const DgBody *b = (const DgBody *)dg_comp_get(obj, DG_C_BODY);
    return b ? ((b->flags & DG_BF_GROUND) != 0) : 0;
}
int dg_phys_on_wall(uint32_t obj) {
    const DgBody *b = (const DgBody *)dg_comp_get(obj, DG_C_BODY);
    return b ? ((b->flags & DG_BF_WALL) != 0) : 0;
}
int dg_phys_on_ceiling(uint32_t obj) {
    const DgBody *b = (const DgBody *)dg_comp_get(obj, DG_C_BODY);
    return b ? ((b->flags & DG_BF_CEILING) != 0) : 0;
}

void dg_phys_wake(uint32_t obj) {
    DgBody *b = (DgBody *)dg_comp_get(obj, DG_C_BODY);
    if (!b) { dg_error("object %u has no 'body'", obj); return; }
    b->sleeping = 0;
    const DgTransform *t = (const DgTransform *)dg_comp_get(obj, DG_C_TRANSFORM);
    if (t) { b->px = t->x; b->py = t->y; b->stepped = 1; }
}

/* ============================ 固定步长推进 ============================ */
static void dg_phys_substep(float dt) {
    static uint32_t ids[DG_MAX_ENTRIES];
    const int n = dg_scene_collect_objects(ids, DG_MAX_ENTRIES);

    /* 1) 记录上一位置(渲染插值用)*/
    for (int i = 0; i < n; i++) {
        DgBody *b = (DgBody *)dg_comp_get(ids[i], DG_C_BODY);
        if (!b || b->motion == DG_MOTION_STATIC) continue;
        const DgTransform *t = (const DgTransform *)dg_comp_get(ids[i], DG_C_TRANSFORM);
        if (!t) continue;
        b->px = t->x;
        b->py = t->y;
        b->stepped = 1;
    }

    /* 2) 积分 + move-and-slide */
    for (int i = 0; i < n; i++) {
        DgBody *b = (DgBody *)dg_comp_get(ids[i], DG_C_BODY);
        if (!b || b->motion == DG_MOTION_STATIC) continue;
        if (b->sleeping) continue;

        if (b->motion == DG_MOTION_DYNAMIC) {
            b->vx += g_gravity_x * b->gravity_scale * dt;
            b->vy += g_gravity_y * b->gravity_scale * dt;
            if (b->friction > 0.0f) {                  /* 水平阻尼(每秒)*/
                const float k = 1.0f - b->friction * dt;
                b->vx *= (k > 0.0f ? k : 0.0f);
            }
        }
        if (b->vx == 0.0f && b->vy == 0.0f) continue;

        int hx = 0, hy = 0;
        if (dg_phys_move(ids[i], b->vx * dt, b->vy * dt, &hx, &hy) < 0) continue;

        if (b->motion == DG_MOTION_DYNAMIC) {
            /* 简单弹性:速度够大**且**弹性系数 > 0 才反弹,否则直接停
               (注意 restitution=0 时 `-vy*0` 会得到 -0.0,打印出来是 "-0")*/
            if (hx) b->vx = (b->restitution > 0.0f && fabsf(b->vx) > DG_PHYS_BOUNCE_CUTOFF)
                            ? -b->vx * b->restitution : 0.0f;
            if (hy) b->vy = (b->restitution > 0.0f && fabsf(b->vy) > DG_PHYS_BOUNCE_CUTOFF)
                            ? -b->vy * b->restitution : 0.0f;
            if ((b->flags & DG_BF_GROUND) && fabsf(b->vx) < DG_PHYS_SLEEP_EPS &&
                fabsf(b->vy) < DG_PHYS_SLEEP_EPS)
                b->sleeping = 1;
        } else {                                       /* 运动学:撞了就停在该轴上 */
            if (hx) b->vx = 0.0f;
            if (hy) b->vy = 0.0f;
        }
    }
}

int dg_phys_advance(double dt) {
    g_frame_dt = (float)dt;
    if (g_paused) { g_alpha = 0.0f; g_last_substeps = 0; return 0; }
    if (dt < 0.0) dt = 0.0;
    if (dt > DG_PHYS_MAX_FRAME_DT) dt = DG_PHYS_MAX_FRAME_DT;
    g_time += dt;
    const double h = 1.0 / (double)g_step_hz;
    g_accum += dt;
    int steps = 0;
    while (g_accum + 1e-9 >= h && steps < g_max_substeps) {
        dg_phys_substep((float)h);
        g_accum -= h;
        steps++;
    }
    if (g_accum + 1e-9 >= h) g_accum = 0.0;            /* 落后太多:丢弃,别雪崩 */
    g_alpha = (float)(g_accum / h);
    if (g_alpha < 0.0f) g_alpha = 0.0f;
    if (g_alpha > 1.0f) g_alpha = 1.0f;
    g_last_substeps = steps;
    return steps;
}

int dg_phys_step_once(void) {
    dg_phys_substep((float)(1.0 / (double)g_step_hz));
    g_last_substeps = 1;
    return 1;
}

/* ============================ 设置 / 状态 ============================ */
void dg_phys_init(void) {
    g_auto = 1;
    g_paused = 0;
    g_gravity_x = 0.0f;
    g_gravity_y = DG_PHYS_DEFAULT_GRAVITY;
    g_step_hz = (float)DG_PHYS_DEFAULT_HZ;
    g_max_substeps = DG_PHYS_MAX_SUBSTEPS;
    g_accum = 0.0;
    g_time = 0.0;
    g_alpha = 0.0f;
    g_frame_dt = 0.0f;
    g_last_substeps = 0;
    g_cell = 64.0f;
    for (int i = 0; i <= DG_MAX_CURSORS; i++) {
        free(g_cursors[i].ids);
        memset(&g_cursors[i], 0, sizeof g_cursors[i]);
    }
    dg_phys_hit_clear();
    dg_phys_invalidate();
}

void dg_phys_shutdown(void) {
    for (int i = 0; i <= DG_MAX_CURSORS; i++) {
        free(g_cursors[i].ids);
        memset(&g_cursors[i], 0, sizeof g_cursors[i]);
    }
    free(g_items);
    g_items = NULL;
    g_items_cap = 0;
    g_nentry = 0;
    g_noverflow = 0;
    g_dirty = 1;
}

void dg_phys_set_auto(int on) { g_auto = on ? 1 : 0; }
int  dg_phys_get_auto(void) { return g_auto; }
void dg_phys_set_gravity(float gx, float gy) { g_gravity_x = gx; g_gravity_y = gy; }
void dg_phys_set_step_hz(float hz) {
    if (hz < 1.0f) { dg_error("physics step %g Hz is too low (min 1)", (double)hz); return; }
    g_step_hz = hz;
}
void dg_phys_set_max_substeps(int n) {
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    g_max_substeps = n;
}
void dg_phys_set_pause(int on) { g_paused = on ? 1 : 0; }
void dg_phys_set_interp(int on) { g_interp = on ? 1 : 0; }
int  dg_phys_get_interp(void) { return g_interp; }
float dg_phys_alpha(void) { return g_alpha; }
float dg_phys_frame_dt(void) { return g_frame_dt; }
double dg_phys_time(void) { return g_time; }
int  dg_phys_last_substeps(void) { return g_last_substeps; }

int dg_phys_lerp_pos(uint32_t obj, float cur_x, float cur_y, float *out_x, float *out_y) {
    if (!g_interp) return 0;
    const DgBody *b = (const DgBody *)dg_comp_get(obj, DG_C_BODY);
    if (!b || !b->stepped || b->motion == DG_MOTION_STATIC) return 0;
    *out_x = b->px + (cur_x - b->px) * g_alpha;
    *out_y = b->py + (cur_y - b->py) * g_alpha;
    return 1;
}

void dg_phys_after_set(uint32_t obj, int kind, const char *field) {
    if (kind != DG_C_BODY && kind != DG_C_TRANSFORM &&
        kind != DG_C_COLLIDER && kind != DG_C_TILEMAP) return;
    dg_phys_invalidate();
    DgBody *b = (DgBody *)dg_comp_get(obj, DG_C_BODY);
    if (!b) return;
    b->sleeping = 0;
    /* 用户直接写速度/位置:把插值基准也拉过来,否则会被插值"拽回"旧位置 */
    const DgTransform *t = (const DgTransform *)dg_comp_get(obj, DG_C_TRANSFORM);
    if (t) { b->px = t->x; b->py = t->y; b->stepped = 1; }
    (void)field;
}

/* 实体之间是否重叠(触发区判定最常用)*/
int32_t dg_phys_overlap(uint32_t a, uint32_t b) {
    DgEntry ea, eb;
    if (!dg_object_alive(a)) { dg_error("object %u is not alive", a); return -1; }
    if (!dg_object_alive(b)) { dg_error("object %u is not alive", b); return -1; }
    if (!dg_entry_of(a, &ea)) { dg_error("object %u has no 'collider'", a); return -1; }
    if (!dg_entry_of(b, &eb)) { dg_error("object %u has no 'collider'", b); return -1; }
    if (!dg_mask_pair(ea.layer, ea.mask, eb.layer, eb.mask)) return 0;
    return dg_shape_overlap(&ea.sh, &eb.sh) ? 1 : 0;
}
