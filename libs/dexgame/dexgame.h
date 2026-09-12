/* dexgame.h — dexgame 引擎内部共享声明
 *
 * 模块划分(全部编译进同一个 libdexgame.dll):
 *   dg_gfx.c   设备 / 交换链 / 窗口 / DPI / 渲染目标 / 呈现 / 回读 / 计时
 *   dg_draw.c  HLSL 管线 / 精灵批渲染 / 纹理(含 stb_image 解码)
 *   dg_api.c   对 DexLang 暴露的 eng_* 导出(值数组 ABI)
 *
 * 设计约束(见 docs/DEXGAME_DESIGN.md):
 *   - C 侧拥有帧循环与渲染;DexLang 只发命令
 *   - 全部导出走**值数组 ABI**(.dexdef 顶部 `abi value_array;`),因为引擎 API
 *     大量是 4+ 参数(eng_draw_tex_uv 有 10 个)
 *   - 失败必须带得出原因:任何失败都 dg_error(...),由 eng_last_error() 取回
 *     —— 现有 6 个库"只返 -1 / 空串"是已知缺陷,不重蹈
 *   - 句柄带**代际**(index + generation),避免"删除后旧 id 误指新对象"
 *     —— 现有 6 个库的 id 单调不复用也是已知坑
 */
#ifndef DEXGAME_H
#define DEXGAME_H

#include <stdint.h>

/* ---------- 通用 ---------- */
#define DG_MAX_TEXTURES   4096
#define DG_MAX_VERTS      (1 << 16)   /* 单批顶点上限(16384 个四边形) */
#define DG_MAX_ERR        512
#define DG_PATH_MAX       260         /* 资源路径缓冲(与 Windows MAX_PATH 一致)*/

/* 组件种类。collider/body 在 M2 只是**数据**,M3 填行为;tilemap 在 M3 加入。 */
enum {
    DG_C_NONE = 0,
    DG_C_TRANSFORM,
    DG_C_SPRITE,
    DG_C_CAMERA,
    DG_C_ANIMATION,
    DG_C_COLLIDER,
    DG_C_BODY,
    DG_C_TILEMAP,
    DG_C_AUDIO,
    DG_C_COUNT
};

/* ---------- 组件数据(M2/M3)----------
   放在共享头里是因为 dg_scene.c(存储/读写/渲染)与 dg_phys.c(碰撞/运动)都要看;
   它们只在 DLL 内部使用,不属于对外 ABI(DexLang 侧一律走 eng_* 访问器)。*/
typedef struct { float x, y, rot, sx, sy; int32_t parent; } DgTransform;
typedef struct {
    char    tex_path[DG_PATH_MAX];
    int32_t texture;                 /* 运行时 */
    float   sx, sy, sw, sh;          /* 源矩形;sw/sh = 0 表示整张 */
    float   px, py;                  /* 轴心,∈[0,1] 表示原点在子矩形里的相对位置 */
    int32_t tint;
    int32_t flip;
    int32_t layer, order;
} DgSprite;
typedef struct { float x, y, zoom, rot; int32_t active; } DgCamera;
typedef struct { int32_t first, count; float fps, time; int32_t loop; } DgAnimation;

/* 碰撞体:M3 的几何。kind 见 DG_SHAPE_*;ox/oy 是相对实体世界坐标的偏移。
   layer/mask 按位与判定(0 = 不筛选,见 dg_phys.c)。 */
typedef struct {
    int32_t kind;
    float   hw, hh;                  /* AABB 半宽高;圆用 hw 当半径;胶囊=半径 hw + 半长 hh */
    float   ox, oy;
    int32_t is_trigger;
    int32_t layer, mask;
} DgCollider;

/* 运动体。motion 见 DG_MOTION_*。
   运行时字段:px/py = 上一固定步的位置(渲染插值),stepped = 是否已经跑过一步,
   flags = 接触状态位(dg_phys.c 的 DG_BF_*)。都不进 JSON。 */
typedef struct {
    int32_t motion;
    float   vx, vy;
    float   gravity_scale, friction, restitution;
    int32_t sleeping;
    float   px, py;
    int32_t stepped;
    int32_t flags;
} DgBody;

/* 瓦片地图。tiles 是 malloc 的 cols*rows 网格(-1 = 空,其余 = 图集索引)。
   tw/th 是世界单位下的瓦片尺寸,atlas_tile/atlas_cols 描述图集切分。 */
typedef struct {
    char    path[DG_PATH_MAX];       /* 数据文件(CSV);字符串加载的图不会自动回写 */
    char    tex_path[DG_PATH_MAX];
    int32_t texture;                 /* 运行时 */
    int16_t *tiles;                  /* 运行时(由 load 分配) */
    int32_t cols, rows;              /* 运行时 */
    float   tw, th;
    int32_t atlas_tile, atlas_cols;
    int32_t layer, order, visible;
    int8_t  solid[256];              /* -1 = 未指定(非空即实心),0/1 = 显式指定 */
} DgTilemap;

/* 音频源(组件版):场景里能带声音。handle/voice/playing 都是运行时。 */
typedef struct {
    char    path[DG_PATH_MAX];
    float   volume;
    int32_t loop;
    int32_t play_on_start;
    int32_t handle;                  /* 运行时:声音资源 id */
    int32_t voice;                   /* 运行时:播放实例 id(0 = 没在放)*/
    int32_t playing;                 /* 运行时 */
} DgAudio;

/* 碰撞形状种类(与 DgCollider.kind 一致) */
enum { DG_SHAPE_AABB = 0, DG_SHAPE_CIRCLE = 1, DG_SHAPE_CAPSULE = 2 };
/* 运动体种类 */
enum { DG_MOTION_STATIC = 0, DG_MOTION_KINEMATIC = 1, DG_MOTION_DYNAMIC = 2 };


/* 字段类型与描述符。**每种组件一张表**,JSON 的读与写都遍历同一张表 ——
   加字段只需加一行,两个方向自动同步(docs/DEXGAME_DESIGN.md §7 的结论)。*/
typedef enum {
    DG_F_INT = 0,
    DG_F_FLOAT,
    DG_F_BOOL,
    DG_F_STR
} DgFieldType;

typedef struct {
    const char *name;
    uint8_t     type;      /* DgFieldType */
    uint8_t     persist;   /* 0 = 运行时字段,不进 JSON */
    uint16_t    size;      /* DG_F_STR:缓冲字节数 */
    uint16_t    offset;    /* 结构体内偏移 */
} DgField;

/* 颜色约定:0xAARRGGBB(与 gal 的 0xRRGGBB 兼容,高位新增 alpha) */
#define DG_RGBA(r, g, b, a) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define DG_RGB(r, g, b) DG_RGBA(r, g, b, 255)

/* 错误上报:记录最近一次失败原因(不覆盖更早的?覆盖 —— 永远只关心最近一次) */
void dg_error(const char *fmt, ...);
const char *dg_last_error(void);
void dg_clear_error(void);

/* ---------- 渲染目标模式 ---------- */
typedef enum {
    DG_MODE_NONE = 0,
    DG_MODE_WINDOW,       /* 有窗口 + flip 交换链(正常游戏) */
    DG_MODE_OFFSCREEN     /* 无窗口 + 离屏纹理(测试 / 工具) */
} DgMode;

/* ---------- dg_gfx.c ---------- */

/* 有窗口:创建窗口 + D3D11 设备 + flip 交换链。返回 0 成功。 */
int  dg_gfx_init_window(const char *title, int w, int h, int vsync);
/* 无窗口:只创建设备 + 离屏渲染目标。返回 0 成功。 */
int  dg_gfx_init_offscreen(int w, int h);
void dg_gfx_shutdown(void);

int  dg_gfx_running(void);
void dg_gfx_pump(void);              /* 抽干消息队列 + 处理 DPI/尺寸变化 */
void dg_gfx_present(void);           /* 提交当前帧(窗口模式:Present;离屏:无操作)*/
void dg_gfx_bind_target(void);       /* 绑定渲染目标 */
void dg_gfx_clear(uint32_t argb);
void dg_gfx_set_vsync(int on);

int  dg_gfx_width(void);
int  dg_gfx_height(void);
int  dg_gfx_mode(void);

/* 回读单个像素 → 0xAARRGGBB;失败 -1。慢(整帧拷到暂存纹理),仅供测试/调试。 */
int64_t dg_gfx_read_pixel(int x, int y);
/* 把渲染目标存成 24 位 BMP(测试产物 / 调试用)。返回 0 成功。 */
int  dg_gfx_save_bmp(const char *path);

void dg_gfx_request_close(void);

/* 设备访问器(dg_gfx.c 提供)。本头文件刻意不 include <d3d11.h>
   (否则每个 TU 都要拖进整套 D3D 头),故用不透明指针传出。 */
void dg_gfx_get_device(void **out_dev, void **out_ctx);

/* 适配器信息。**库不往 stdout 打印**(会污染游戏输出),需要时按需查询。 */
const char *dg_gfx_gpu_name(void);
double dg_gfx_vram_mb(void);

/* 计时:毫秒(基于 QueryPerformanceCounter)。 */
int64_t dg_gfx_now_ms(void);
/* 睡眠到本帧的呈现时刻(按目标帧率节流;0 = 不限速,只靠 VSync)。 */
void dg_gfx_frame_pace(int target_fps);

/* ---------- dg_draw.c ---------- */

int  dg_draw_init(void);             /* 编译着色器 + 建管线状态。返回 0 成功 */
int  dg_draw_ready(void);            /* 1 = 管线已就绪(文字光栅化前要问一句)*/
void dg_draw_shutdown(void);
void dg_draw_frame_begin(void);      /* 重置批次 + 设默认状态 */
void dg_draw_frame_end(void);        /* 提交剩余批次(必须在 Present 前调用)*/

/* 纹理。id 由 eng_tex_load 返回;内部用代际句柄校验。 */
int  dg_tex_load_file(const char *path);
/* 按路径缓存的加载:同一路径只解码一次(设计文档明确要求"禁止每帧重解码")*/
int  dg_tex_load_cached(const char *path);
void dg_tex_cache_clear(void);
int  dg_tex_create_rgba(int w, int h, const uint8_t *rgba);  /* 便于测试与程序化生成 */
/* 局部更新(字形图集用);rgba = w*h*4,0xAARRGGBB 字节序 */
int  dg_tex_update_rgba(int id, int x, int y, int w, int h, const uint8_t *rgba);
int  dg_tex_free(int id);
int  dg_tex_valid(int id);
int  dg_tex_width(int id);
int  dg_tex_height(int id);

/* 绘制:三角形按顺序入批;切换纹理/满批时自动 flush */
int  dg_draw_quad(int tex, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, uint32_t color);
int  dg_draw_rect(float x, float y, float w, float h, uint32_t color);

/* ---------- dg_scene.c(M2:实体 / 组件 / 场景读写) ---------- */
void     dg_scene_init(void);
void     dg_scene_shutdown(void);

uint32_t dg_object_new(void);
int32_t  dg_object_free(uint32_t id);
int32_t  dg_object_alive(uint32_t id);
int32_t  dg_object_count(void);
/* 第 index 个活实体的 id(按槽位顺序);越界返回 0。IDE 的场景树要按序号枚举。 */
uint32_t dg_object_id_at(int32_t index);
/* 实体名(最多 31 字符)。为什么需要它:
   1) 语言**没有全局变量**,所以回调函数(如 on_update)看不到主程序的 let 变量 ——
      只能靠"名字找实体"(状态本来就该放在实体/组件里);
   2) IDE 的场景树要显示名字。
   名字不是组件,属于实体本身,会跟场景 JSON 一起存。*/
#define DG_NAME_MAX 32
int32_t  dg_object_set_name(uint32_t id, const char *name);
const char *dg_object_name(uint32_t id);
uint32_t dg_object_find(const char *name);
int32_t  dg_object_clear(void);
/* 把所有活实体的 id 收进 out(给物理/渲染的批处理用);返回个数 */
int      dg_scene_collect_objects(uint32_t *out, int cap);

int      dg_comp_kind(const char *name);      /* 名字 → kind;未知返回 -1 并记录原因 */
const char *dg_comp_name(int kind);
int      dg_comp_field_count(int kind);
const char *dg_comp_field_name(int kind, int i);
int      dg_comp_field_type(int kind, int i);
int      dg_comp_field_persist(int kind, int i);
void    *dg_comp_add(uint32_t obj, int kind);
void    *dg_comp_get(uint32_t obj, int kind);
int32_t  dg_comp_remove(uint32_t obj, int kind);

int32_t  dg_set_f(uint32_t obj, const char *comp, const char *field, double v);
int32_t  dg_set_i(uint32_t obj, const char *comp, const char *field, int64_t v);
int32_t  dg_set_s(uint32_t obj, const char *comp, const char *field, const char *v);
double   dg_get_f(uint32_t obj, const char *comp, const char *field);
int64_t  dg_get_i(uint32_t obj, const char *comp, const char *field);
const char *dg_get_s(uint32_t obj, const char *comp, const char *field);

int32_t  dg_world_pos(uint32_t obj, double *out_x, double *out_y);

const char *dg_scene_to_json(void);
int32_t  dg_scene_from_json(const char *text);
int32_t  dg_scene_save(const char *path);
int32_t  dg_scene_load(const char *path);

/* 把场景里所有 (sprite + tilemap) 的实体画出来;返回绘制个数。
   相机取第一个 active 的 camera 组件;按 (layer, order) 稳定排序。
   有 body 的实体按 dg_phys_lerp_pos() 做渲染插值(可用 eng_render_set_interp 关掉)。 */
int32_t  dg_draw_scene(void);

/* ---------- dg_phys.c(M3:碰撞 / 查询 / 运动 / 瓦片) ---------- */

/* 物理世界设置 */
void    dg_phys_init(void);
void    dg_phys_shutdown(void);
void    dg_phys_set_auto(int on);
int     dg_phys_get_auto(void);
void    dg_phys_set_gravity(float gx, float gy);
void    dg_phys_set_step_hz(float hz);
void    dg_phys_set_max_substeps(int n);
void    dg_phys_set_pause(int on);
void    dg_phys_set_interp(int on);
int     dg_phys_get_interp(void);
float   dg_phys_alpha(void);
float   dg_phys_frame_dt(void);
double  dg_phys_time(void);
int     dg_phys_last_substeps(void);

/* 推进物理:把 dt 按固定步长切分。返回执行的子步数;-1 出错。 */
int     dg_phys_advance(double dt);
/* 手动跑一个固定步(ignores 累加器) */
int     dg_phys_step_once(void);

/* 任何会改变碰撞几何的改动都要调它,让宽相在下次使用时重建 */
void    dg_phys_invalidate(void);
/* 字段被写入后的反应:几何相关 → 宽相失效;body.vx/vy → 唤醒 + 同步上一步位置 */
void    dg_phys_after_set(uint32_t obj, int kind, const char *field);

/* 运动学:轴分离的 move-and-slide。返回位掩码(1 = X 向受阻,2 = Y 向受阻)。 */
int32_t dg_phys_move(uint32_t obj, float dx, float dy, int *out_hit_x, int *out_hit_y);
int     dg_phys_on_ground(uint32_t obj);
int     dg_phys_on_wall(uint32_t obj);
int     dg_phys_on_ceiling(uint32_t obj);
void    dg_phys_wake(uint32_t obj);

/* 查询:游标式(语言没有数组,所以 C 侧存游标)
   dg_phys_query_* 返回游标 id(>0),失败返回 -1 并记录原因;游标用完必须 end。 */
int32_t dg_phys_query_rect(float x, float y, float w, float h, int32_t mask);
int32_t dg_phys_query_circle(float cx, float cy, float r, int32_t mask);
int32_t dg_phys_query_point(float x, float y, int32_t mask);
int32_t dg_phys_query_count(int32_t cur);
int32_t dg_phys_query_next(int32_t cur);      /* 0 = 到头了 / 游标无效 */
int32_t dg_phys_query_reset(int32_t cur);
int32_t dg_phys_query_end(int32_t cur);
/* 查询结果里第 i 个(供 IDE/调试;迭代仍然推荐 next) */
int32_t dg_phys_query_at(int32_t cur, int32_t i);
/* 某个几何形状命中的实体数(不建游标,直接数) */
int32_t dg_phys_count_rect(float x, float y, float w, float h, int32_t mask);

/* 单点测试:实体之间是否重叠(触发区/接触判定最常用)*/
int32_t dg_phys_overlap(uint32_t a, uint32_t b);

/* 射线与扫掠(AABB/圆/胶囊):命中填 dg_phys_hit_*,返回 1/0/-1。
   **每次查询都会覆盖结果槽** —— 需要跨语句保留就自己存起来。 */
typedef struct {
    int32_t obj;                 /* 0 = 没命中;或者打到地形(tile != 0)*/
    int32_t tile;                /* != 0 = 打到瓦片地图的地形(obj 为 0)*/
    float   x, y;                /* 命中点 */
    float   nx, ny;              /* 命中面法线(指向射线来的方向) */
    float   t;                   /* 沿射线的距离(或扫掠距离) */
} DgHit;
void    dg_phys_hit_clear(void);
const DgHit *dg_phys_hit(void);

int32_t dg_phys_raycast(float x, float y, float dx, float dy, float dist,
                        int32_t mask, uint32_t ignore);
int32_t dg_phys_sweep_box(float x, float y, float hw, float hh,
                          float dx, float dy, int32_t mask, uint32_t ignore);

/* 渲染插值:有 body 的实体返回上一固定步与当前位置之间的插值位置。
   返回 1 = 已插值(out 有效),0 = 不插值(调用方直接用当前位置)。 */
int     dg_phys_lerp_pos(uint32_t obj, float cur_x, float cur_y, float *out_x, float *out_y);

/* ---------- 瓦片地图(dg_scene.c 持有数据,dg_phys.c 读它做碰撞) ---------- */
int32_t dg_tilemap_load_csv(uint32_t obj, const char *text);
int32_t dg_tilemap_load_file(uint32_t obj, const char *path);
int32_t dg_tilemap_save_csv(uint32_t obj, const char *path);
int32_t dg_tilemap_tile(uint32_t obj, int32_t col, int32_t row);
int32_t dg_tilemap_cols(uint32_t obj);
int32_t dg_tilemap_rows(uint32_t obj);
int32_t dg_tilemap_set_solid(uint32_t obj, int32_t tile, int on);
int32_t dg_tilemap_is_solid(uint32_t obj, int32_t tile);
int32_t dg_tilemap_solid_at(uint32_t obj, float wx, float wy);
/* 世界矩形 → 实心瓦片的世界 AABB(遍历用):把命中的瓦片逐个交给回调。
   返回命中的瓦片数;-1 出错。 */
typedef void (*DgTileFn)(void *user, float x, float y, float w, float h, int32_t tile);
int32_t dg_tilemap_each_tile(uint32_t obj, void *user, DgTileFn fn);
int32_t dg_tilemap_each_solid_in(uint32_t obj, float x, float y, float w, float h,
                                 void *user, DgTileFn fn);
/* 绘制瓦片层(供 dg_draw_scene 调用);返回绘制的瓦片数。 */
int32_t dg_tilemap_draw(uint32_t obj, const float *view);   /* view = {x,y,w,h} 世界可见矩形 */

/* ---------- dg_input.c(M4:键盘 / 鼠标 / 手柄 / 动作映射) ---------- */
void  dg_input_init(void);
void  dg_input_shutdown(void);
/* 帧边界:采样手柄 + 把"当前"变"上一帧"(边沿检测靠它)。eng_frame_begin 会调。 */
void  dg_input_begin_frame(void);   /* 帧开始:采样手柄 + 清滚轮 */
void  dg_input_end_frame(void);     /* 帧结束:当前 → 上一帧(边沿检测靠它)*/
/* Win32 消息入口(由 dg_gfx.c 的窗口过程转发)。返回 1 = 已消费。 */
int   dg_input_on_message(unsigned int msg, uint64_t wp, int64_t lp);

/* 按键码见 dg_input.c 顶部注释(0..255 = VK,256..258 = 鼠标,300.. = 手柄,400+/500+ = 轴) */
int   dg_input_down(int code);
int   dg_input_pressed(int code);
int   dg_input_released(int code);
float dg_input_mouse_x(void);
float dg_input_mouse_y(void);
int   dg_input_mouse_down(int btn);
int   dg_input_mouse_pressed(int btn);
int   dg_input_mouse_released(int btn);
int   dg_input_wheel(void);
float dg_input_code_axis(int axis);          /* 0=LX 1=LY 2=RX 3=RY 4=LT 5=RT */
int   dg_input_pad_connected(void);

/* 合成输入:测试与 IDE 的脚本化试玩用(不碰真实设备) */
int   dg_input_feed(int code, int down);
int   dg_input_feed_axis(int axis, float value);
int   dg_input_feed_mouse(float x, float y);
void  dg_input_feed_wheel(int delta);
void  dg_input_clear(void);

/* 动作映射:一个动作最多绑 4 个来源(键盘/鼠标/手柄可以混)*/
int   dg_input_action_bind(const char *name, int code);
int   dg_input_action_unbind(const char *name);
int   dg_input_action_down(const char *name);
int   dg_input_action_pressed(const char *name);
int   dg_input_action_released(const char *name);
float dg_input_action_value(const char *name);   /* 模拟量(摇杆);数字键给 0/1 */
int   dg_input_action_bound(const char *name);
int   dg_input_action_count(void);
const char *dg_input_action_name(int i);
int   dg_input_action_code(const char *name, int slot);

/* ---------- dg_audio.c(M4:声音)---------- */
void   dg_audio_init(void);
void   dg_audio_shutdown(void);
int    dg_audio_ok(void);            /* 0 = 设备不可用(没声卡);播放会失败并给原因 */

int32_t dg_audio_load_cached(const char *path);   /* 按路径缓存,返回声音 id */
int32_t dg_audio_free(int32_t sound);
double  dg_audio_duration(int32_t sound);         /* 秒 */
int32_t dg_audio_sample_rate(int32_t sound);
int32_t dg_audio_channels(int32_t sound);
int32_t dg_audio_bits(int32_t sound);
const char *dg_audio_path(int32_t sound);
int32_t dg_audio_count(void);

int32_t dg_audio_play(int32_t sound, int loop, float volume);   /* 返回播放实例 id */
int32_t dg_audio_stop(int32_t voice);
int32_t dg_audio_voice_playing(int32_t voice);
int32_t dg_audio_voice_set_volume(int32_t voice, float volume);
int32_t dg_audio_voice_sound(int32_t voice);
int32_t dg_audio_playing_count(void);
int32_t dg_audio_stop_all(void);
int32_t dg_audio_set_master_volume(float volume);
float   dg_audio_master_volume(void);

/* ---------- dg_text.c(M4-c:文字:DirectWrite + 字形图集)---------- */
void   dg_text_init(void);
void   dg_text_shutdown(void);
int    dg_text_ok(void);                 /* DirectWrite 工厂是否可用 */
int    dg_text_glyph_count(void);        /* 已光栅化的字形数(自省/测试)*/
int    dg_text_font_count(void);

int32_t dg_text_font_load(const char *family, float size);   /* 同家族+同字号复用 */
int32_t dg_text_font_free(int32_t font);
double  dg_text_line_height(int32_t font);
double  dg_text_ascent(int32_t font);
const char *dg_text_family(int32_t font);
double  dg_text_size(int32_t font);

double  dg_text_measure_w(int32_t font, const char *text);   /* `\n` 取最宽一行 */
double  dg_text_measure_h(int32_t font, const char *text);   /* 行数 × 行高 */
/* 画字符串;整数像素定位。返回画出的字形数(空串 = 0,失败 = -1)。 */
int32_t dg_text_draw(int32_t font, float x, float y, const char *text, uint32_t color);

/* ---------- 屏幕 ↔ 世界(dg_scene.c;鼠标坐标换算要用) ---------- */
int32_t dg_scene_active_camera(float *x, float *y, float *zoom);
void    dg_scene_screen_to_world(float sx, float sy, float *wx, float *wy);

#endif /* DEXGAME_H */
