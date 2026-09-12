/* dg_api.c — 对 DexLang 暴露的 eng_* 导出(全部走值数组 ABI)
 *
 * 约定:
 *   - 全部函数签名是 `int64_t eng_xxx(const DexValue *a, int n)`,
 *     返回字符串的用 `const char *`。见 libs/dexgame/dexgame.dexdef。
 *   - 失败返回负数,并把原因写进 dg_error(...),由 eng_last_error() 取回。
 *     **不静默失败** —— 现有 6 个库"只返 -1 / 空串"是已知缺陷。
 *   - 帧模型由 C 侧拥有(设计中已定):
 *       eng_frame_begin() → 抽消息、绑定目标、按清除色清屏、重置批次
 *       ... eng_draw* 多次 ...
 *       eng_frame_end()   → 提交批次、Present(垂直同步)
 *     DexLang 只负责在两者之间发绘制命令。
 */
#include <windows.h>
#include <d3d11.h>

#include "dexgame.h"
#include "dexvalue.h"

#include <stdio.h>

/* ---------- 帧状态 ---------- */
static uint32_t g_clear_color = DG_RGB(20, 20, 28);
static int64_t  g_frame_index = 0;
static int      g_target_fps = 0;      /* 0 = 不限速(靠 VSync) */
static double   g_frame_dt = 0.0;      /* 本帧墙钟间隔(秒);eng_dt() 读它 */
static int64_t  g_last_frame_ms = 0;   /* 上一帧的时间戳(算 dt 给物理用)*/

/* ---------- 生命周期 ---------- */
int64_t eng_init(const DexValue *a, int n) {
    dg_clear_error();
    const char *title = dv_str_or(a, n, 0, "dexgame");
    const int w = (int)dv_int_or(a, n, 1, 960);
    const int h = (int)dv_int_or(a, n, 2, 540);
    const int vsync = (int)dv_int_or(a, n, 3, 1);
    if (dg_gfx_init_window(title, w, h, vsync)) return -1;
    if (dg_draw_init()) { dg_gfx_shutdown(); return -1; }
    dg_scene_init();
    dg_phys_init();
    dg_input_init();
    g_frame_index = 0;
    return 0;
}

int64_t eng_init_offscreen(const DexValue *a, int n) {
    dg_clear_error();
    const int w = (int)dv_int_or(a, n, 0, 64);
    const int h = (int)dv_int_or(a, n, 1, 64);
    if (dg_gfx_init_offscreen(w, h)) return -1;
    if (dg_draw_init()) { dg_gfx_shutdown(); return -1; }
    dg_scene_init();
    dg_phys_init();
    dg_input_init();
    g_frame_index = 0;
    return 0;
}

int64_t eng_shutdown(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    dg_input_shutdown();
    dg_phys_shutdown();
    dg_scene_shutdown();
    dg_draw_shutdown();
    dg_gfx_shutdown();
    return 0;
}

int64_t eng_running(const DexValue *a, int n) {
    (void)a; (void)n;
    return dg_gfx_running();
}

int64_t eng_request_close(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_gfx_request_close();
    return 0;
}

/* ---------- 帧 ---------- */
int64_t eng_frame_begin(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    dg_gfx_pump();
    if (!dg_gfx_running()) return 0;
    /* 输入必须在物理/用户逻辑之前采样:边沿检测与 dt 都依赖它 */
    dg_input_begin_frame();
    /* 帧 dt:与物理模式无关都要算(eng_dt() 给用户看的就是它)*/
    const int64_t now = dg_gfx_now_ms();
    g_frame_dt = g_last_frame_ms ? (double)(now - g_last_frame_ms) / 1000.0 : 0.0;
    g_last_frame_ms = now;
    /* 物理默认**自动**推进:用墙钟 dt 喂固定步长累加器。手动模式
       (eng_physics_set_auto(0))下由用户自己调 eng_physics_step()。 */
    if (dg_phys_get_auto()) dg_phys_advance(g_frame_dt);
    dg_gfx_bind_target();
    dg_gfx_clear(g_clear_color);
    dg_draw_frame_begin();
    return 1;
}

int64_t eng_frame_end(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_input_end_frame();         /* 边沿检测:把本帧状态记为"上一帧" */
    dg_draw_frame_end();          /* 必须先提交批次再 Present */
    dg_gfx_frame_pace(g_target_fps);
    dg_gfx_present();
    g_frame_index++;
    return 0;
}

int64_t eng_set_clear_color(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_set_clear_color needs a color"); return -1; }
    g_clear_color = (uint32_t)dv_int(&a[0]);
    return 0;
}

int64_t eng_set_vsync(const DexValue *a, int n) {
    dg_clear_error();
    dg_gfx_set_vsync((int)dv_int_or(a, n, 0, 1));
    return 0;
}

int64_t eng_set_target_fps(const DexValue *a, int n) {
    dg_clear_error();
    g_target_fps = (int)dv_int_or(a, n, 0, 0);
    return 0;
}

int64_t eng_frame_index(const DexValue *a, int n) { (void)a; (void)n; return g_frame_index; }
int64_t eng_now_ms(const DexValue *a, int n) { (void)a; (void)n; return dg_gfx_now_ms(); }

/* ---------- 表面信息 ---------- */
int64_t eng_width(const DexValue *a, int n) { (void)a; (void)n; return dg_gfx_width(); }
int64_t eng_height(const DexValue *a, int n) { (void)a; (void)n; return dg_gfx_height(); }

/* ---------- 纹理 ---------- */
int64_t eng_tex_load(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1 || a[0].tag != DEXV_STR) { dg_error("eng_tex_load expects a path string"); return -1; }
    return dg_tex_load_file(dv_str(&a[0]));
}

/* 程序化生成纯色纹理 —— 测试与临时占位图用(不必准备图片文件) */
int64_t eng_tex_solid(const DexValue *a, int n) {
    dg_clear_error();
    const int w = (int)dv_int_or(a, n, 0, 8);
    const int h = (int)dv_int_or(a, n, 1, 8);
    const uint32_t argb = (uint32_t)dv_int_or(a, n, 2, (int64_t)DG_RGB(255, 255, 255));
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        dg_error("eng_tex_solid: invalid size %dx%d (1..4096)", w, h);
        return -1;
    }
    const uint8_t r = (uint8_t)((argb >> 16) & 0xFF);
    const uint8_t g = (uint8_t)((argb >> 8) & 0xFF);
    const uint8_t b = (uint8_t)(argb & 0xFF);
    const uint8_t al = (uint8_t)((argb >> 24) & 0xFF);
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    if (!px) { dg_error("out of memory for %dx%d texture", w, h); return -1; }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        px[i * 4 + 0] = r; px[i * 4 + 1] = g; px[i * 4 + 2] = b; px[i * 4 + 3] = al;
    }
    const int id = dg_tex_create_rgba(w, h, px);
    free(px);
    return id;
}

int64_t eng_tex_free(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_tex_free needs a texture id"); return -1; }
    return dg_tex_free((int)dv_int(&a[0]));
}

int64_t eng_tex_valid(const DexValue *a, int n) {
    dg_clear_error();
    return dg_tex_valid((int)dv_int_or(a, n, 0, 0)) ? 1 : 0;
}

int64_t eng_tex_width(const DexValue *a, int n) {
    dg_clear_error();
    return dg_tex_width((int)dv_int_or(a, n, 0, 0));
}

int64_t eng_tex_height(const DexValue *a, int n) {
    dg_clear_error();
    return dg_tex_height((int)dv_int_or(a, n, 0, 0));
}

/* ---------- 绘制 ---------- */
/* eng_draw(tex, x, y [, w, h, color]) —— 不给 w/h 就用纹理原始尺寸 */
int64_t eng_draw(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_draw needs (tex, x, y)"); return -1; }
    const int tex = (int)dv_int(&a[0]);
    const float x = (float)dv_float(&a[1]);
    const float y = (float)dv_float(&a[2]);
    float w = (float)dv_float_or(a, n, 3, -1.0);
    float h = (float)dv_float_or(a, n, 4, -1.0);
    const uint32_t col = (uint32_t)dv_int_or(a, n, 5, (int64_t)DG_RGB(255, 255, 255));
    if (w < 0.0f || h < 0.0f) {
        const int tw = dg_tex_width(tex), th = dg_tex_height(tex);
        if (tw < 0 || th < 0) return -1;
        if (w < 0.0f) w = (float)tw;
        if (h < 0.0f) h = (float)th;
    }
    return dg_draw_quad(tex, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, col);
}

/* eng_draw_uv(tex, x, y, w, h, u0, v0, u1, v1, color) —— 10 个参数,
   直接 ABI 的 3 参上限根本表达不了,正是引入值数组 ABI 的理由。 */
int64_t eng_draw_uv(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 8) { dg_error("eng_draw_uv needs (tex,x,y,w,h,u0,v0[,u1,v1,color])"); return -1; }
    const int tex = (int)dv_int(&a[0]);
    const float x = (float)dv_float(&a[1]);
    const float y = (float)dv_float(&a[2]);
    const float w = (float)dv_float(&a[3]);
    const float h = (float)dv_float(&a[4]);
    const float u0 = (float)dv_float(&a[5]);
    const float v0 = (float)dv_float(&a[6]);
    const float u1 = (float)dv_float_or(a, n, 7, 1.0);
    const float v1 = (float)dv_float_or(a, n, 8, 1.0);
    const uint32_t col = (uint32_t)dv_int_or(a, n, 9, (int64_t)DG_RGB(255, 255, 255));
    return dg_draw_quad(tex, x, y, w, h, u0, v0, u1, v1, col);
}

/* eng_rect(x, y, w, h, color) —— 纯色矩形 */
int64_t eng_rect(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 5) { dg_error("eng_rect needs (x,y,w,h,color)"); return -1; }
    return dg_draw_rect((float)dv_float(&a[0]), (float)dv_float(&a[1]),
                        (float)dv_float(&a[2]), (float)dv_float(&a[3]),
                        (uint32_t)dv_int(&a[4]));
}

/* ---------- 测试 / 调试通道 ---------- */
int64_t eng_pixel(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_pixel needs (x, y)"); return -1; }
    return dg_gfx_read_pixel((int)dv_int(&a[0]), (int)dv_int(&a[1]));
}

int64_t eng_save_bmp(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1 || a[0].tag != DEXV_STR) { dg_error("eng_save_bmp expects a path string"); return -1; }
    return dg_gfx_save_bmp(dv_str(&a[0]));
}

/* ============================================================
   实体 / 组件 / 场景(M2)
   ============================================================ */

int64_t eng_object_new(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    return (int64_t)dg_object_new();
}

int64_t eng_object_free(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_object_free needs an object id"); return -1; }
    return dg_object_free((uint32_t)dv_int(&a[0]));
}

int64_t eng_object_alive(const DexValue *a, int n) {
    dg_clear_error();
    return dg_object_alive((uint32_t)dv_int_or(a, n, 0, 0)) ? 1 : 0;
}

int64_t eng_object_count(const DexValue *a, int n) {
    (void)a; (void)n;
    return dg_object_count();
}

/* 实体名:语言没有全局变量,回调函数只能靠名字找实体。也是 IDE 场景树要的东西。 */
int64_t eng_set_name(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_set_name needs (object, name)"); return -1; }
    return dg_object_set_name((uint32_t)dv_int(&a[0]), dv_str(&a[1]));
}
const char *eng_name(const DexValue *a, int n) {
    dg_clear_error();
    return dg_object_name((uint32_t)dv_int_or(a, n, 0, 0));
}
int64_t eng_find(const DexValue *a, int n) {
    dg_clear_error();
    return (int64_t)dg_object_find(dv_str_or(a, n, 0, ""));
}

int64_t eng_scene_clear(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    return dg_object_clear();
}

/* 组件名用字符串(如 "sprite"),便于生成的代码与 IDE 可读;
   名字解析失败会 dg_error 列出可用名字。 */
int64_t eng_attach(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_attach needs (object, component)"); return -1; }
    const int k = dg_comp_kind(dv_str(&a[1]));
    if (k < 0) return -1;
    return dg_comp_add((uint32_t)dv_int(&a[0]), k) ? 0 : -1;
}

int64_t eng_detach(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_detach needs (object, component)"); return -1; }
    const int k = dg_comp_kind(dv_str(&a[1]));
    if (k < 0) return -1;
    return dg_comp_remove((uint32_t)dv_int(&a[0]), k);
}

int64_t eng_has(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_has needs (object, component)"); return 0; }
    const int k = dg_comp_kind(dv_str(&a[1]));
    if (k < 0) return 0;
    return dg_comp_get((uint32_t)dv_int(&a[0]), k) ? 1 : 0;
}

int64_t eng_set_i(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 4) { dg_error("eng_set_i needs (object, component, field, value)"); return -1; }
    return dg_set_i((uint32_t)dv_int(&a[0]), dv_str(&a[1]), dv_str(&a[2]), dv_int(&a[3]));
}

int64_t eng_set_f(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 4) { dg_error("eng_set_f needs (object, component, field, value)"); return -1; }
    return dg_set_f((uint32_t)dv_int(&a[0]), dv_str(&a[1]), dv_str(&a[2]), dv_float(&a[3]));
}

int64_t eng_set_s(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 4) { dg_error("eng_set_s needs (object, component, field, value)"); return -1; }
    return dg_set_s((uint32_t)dv_int(&a[0]), dv_str(&a[1]), dv_str(&a[2]), dv_str(&a[3]));
}

int64_t eng_get_i(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_get_i needs (object, component, field)"); return -1; }
    return dg_get_i((uint32_t)dv_int(&a[0]), dv_str(&a[1]), dv_str(&a[2]));
}

const char *eng_get_s(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_get_s needs (object, component, field)"); return ""; }
    return dg_get_s((uint32_t)dv_int(&a[0]), dv_str(&a[1]), dv_str(&a[2]));
}

const char *eng_scene_json(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    const char *t = dg_scene_to_json();
    return t ? t : "";
}

/* 注意:浮点返回值必须用 double 签名 —— 值数组 ABI 只规定**入参**是 DexValue 数组,
   返回值仍按 .dexdef 声明的类型(int/float/string/void)。所以这里返回 double。 */
double eng_get_f(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_get_f needs (object, component, field)"); return 0.0; }
    return dg_get_f((uint32_t)dv_int(&a[0]), dv_str(&a[1]), dv_str(&a[2]));
}

double eng_world_x(const DexValue *a, int n) {
    dg_clear_error();
    double x = 0.0, y = 0.0;
    if (dg_world_pos((uint32_t)dv_int_or(a, n, 0, 0), &x, &y)) return 0.0;
    return x;
}

double eng_world_y(const DexValue *a, int n) {
    dg_clear_error();
    double x = 0.0, y = 0.0;
    if (dg_world_pos((uint32_t)dv_int_or(a, n, 0, 0), &x, &y)) return 0.0;
    return y;
}

int64_t eng_draw_scene(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    return dg_draw_scene();
}

int64_t eng_scene_save(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1 || a[0].tag != DEXV_STR) { dg_error("eng_scene_save expects a path string"); return -1; }
    return dg_scene_save(dv_str(&a[0]));
}

int64_t eng_scene_load(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1 || a[0].tag != DEXV_STR) { dg_error("eng_scene_load expects a path string"); return -1; }
    return dg_scene_load(dv_str(&a[0]));
}

int64_t eng_scene_load_json(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1 || a[0].tag != DEXV_STR) { dg_error("eng_scene_load_json expects a string"); return -1; }
    return dg_scene_from_json(dv_str(&a[0]));
}

/* 描述符表的自省 —— 节点式 IDE 要靠它生成属性面板,
   所以这里不是"调试糖",而是产品 B 的接口。 */
int64_t eng_comp_count(const DexValue *a, int n) {
    (void)a; (void)n;
    return DG_C_COUNT - 1;
}

const char *eng_comp_name_at(const DexValue *a, int n) {
    dg_clear_error();
    const int i = (int)dv_int_or(a, n, 0, -1);
    if (i < 1 || i >= DG_C_COUNT) { dg_error("component index %d out of range 1..%d", i, DG_C_COUNT - 1); return ""; }
    return dg_comp_name(i);
}

int64_t eng_field_count(const DexValue *a, int n) {
    dg_clear_error();
    const int k = dg_comp_kind(dv_str_or(a, n, 0, ""));
    if (k < 0) return -1;
    return dg_comp_field_count(k);
}

const char *eng_field_name(const DexValue *a, int n) {
    dg_clear_error();
    const int k = dg_comp_kind(dv_str_or(a, n, 0, ""));
    if (k < 0) return "";
    return dg_comp_field_name(k, (int)dv_int_or(a, n, 1, -1));
}

int64_t eng_field_type(const DexValue *a, int n) {
    dg_clear_error();
    const int k = dg_comp_kind(dv_str_or(a, n, 0, ""));
    if (k < 0) return -1;
    return dg_comp_field_type(k, (int)dv_int_or(a, n, 1, -1));
}

int64_t eng_field_persist(const DexValue *a, int n) {
    dg_clear_error();
    const int k = dg_comp_kind(dv_str_or(a, n, 0, ""));
    if (k < 0) return -1;
    return dg_comp_field_persist(k, (int)dv_int_or(a, n, 1, -1));
}

/* ============================================================
   物理(M3):固定步长 / 查询 / 运动 / 瓦片
   ============================================================ */

/* ---------- 世界设置 ---------- */
int64_t eng_physics_set_auto(const DexValue *a, int n) {
    dg_clear_error();
    dg_phys_set_auto((int)dv_int_or(a, n, 0, 1));
    return dg_phys_get_auto();
}

int64_t eng_physics_set_gravity(const DexValue *a, int n) {
    dg_clear_error();
    dg_phys_set_gravity((float)dv_float_or(a, n, 0, 0.0), (float)dv_float_or(a, n, 1, 980.0));
    return 0;
}

int64_t eng_physics_set_step(const DexValue *a, int n) {
    dg_clear_error();
    const double hz = dv_float_or(a, n, 0, 120.0);
    if (hz < 1.0) { dg_error("physics step %g Hz is too low (min 1)", hz); return -1; }
    dg_phys_set_step_hz((float)hz);
    return 0;
}

int64_t eng_physics_step(const DexValue *a, int n) {
    dg_clear_error();
    const double dt = dv_float_or(a, n, 0, 1.0 / 120.0);
    if (dt < 0.0) { dg_error("physics step dt %g < 0", dt); return -1; }
    return dg_phys_advance(dt);
}

int64_t eng_physics_step_once(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    return dg_phys_step_once();
}

int64_t eng_physics_pause(const DexValue *a, int n) {
    dg_clear_error();
    dg_phys_set_pause((int)dv_int_or(a, n, 0, 0));
    return 0;
}

/* 注意:浮点返回值必须用 double 签名(见 eng_get_f 的说明)—— 写成 int64_t 会把
   双精度位模式当成 float 读,调用方拿到的是垃圾(实测 5.2e-315)。 */
double eng_physics_alpha(const DexValue *a, int n) {
    (void)a; (void)n;
    return (double)dg_phys_alpha();
}

int64_t eng_physics_substeps(const DexValue *a, int n) {
    (void)a; (void)n;
    return dg_phys_last_substeps();
}

/* 帧间隔(秒)。**不是**物理步长:物理是固定 120Hz,它是渲染帧的墙钟间隔。 */
double eng_dt(const DexValue *a, int n) {
    (void)a; (void)n;
    return g_frame_dt;
}

double eng_time(const DexValue *a, int n) {
    (void)a; (void)n;
    return dg_phys_time();
}

/* ---------- 运动 ---------- */
int64_t eng_move(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_move needs (object, dx, dy)"); return -1; }
    int hx = 0, hy = 0;
    const int32_t rc = dg_phys_move((uint32_t)dv_int(&a[0]), (float)dv_float(&a[1]),
                                    (float)dv_float(&a[2]), &hx, &hy);
    if (rc < 0) return -1;
    return rc;                       /* 位掩码:1 = X 向受阻,2 = Y 向受阻 */
}

int64_t eng_on_ground(const DexValue *a, int n) {
    dg_clear_error();
    return dg_phys_on_ground((uint32_t)dv_int_or(a, n, 0, 0)) ? 1 : 0;
}

int64_t eng_on_wall(const DexValue *a, int n) {
    dg_clear_error();
    return dg_phys_on_wall((uint32_t)dv_int_or(a, n, 0, 0)) ? 1 : 0;
}

int64_t eng_on_ceiling(const DexValue *a, int n) {
    dg_clear_error();
    return dg_phys_on_ceiling((uint32_t)dv_int_or(a, n, 0, 0)) ? 1 : 0;
}

int64_t eng_set_velocity(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_set_velocity needs (object, vx, vy)"); return -1; }
    const uint32_t obj = (uint32_t)dv_int(&a[0]);
    if (dg_set_f(obj, "body", "vx", dv_float(&a[1]))) return -1;
    if (dg_set_f(obj, "body", "vy", dv_float(&a[2]))) return -1;
    dg_phys_wake(obj);
    return 0;
}

double eng_velocity_x(const DexValue *a, int n) {
    dg_clear_error();
    return dg_get_f((uint32_t)dv_int_or(a, n, 0, 0), "body", "vx");
}

double eng_velocity_y(const DexValue *a, int n) {
    dg_clear_error();
    return dg_get_f((uint32_t)dv_int_or(a, n, 0, 0), "body", "vy");
}

int64_t eng_body_wake(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_body_wake needs an object"); return -1; }
    dg_phys_wake((uint32_t)dv_int(&a[0]));
    return 0;
}

int64_t eng_body_sleeping(const DexValue *a, int n) {
    dg_clear_error();
    return dg_get_i((uint32_t)dv_int_or(a, n, 0, 0), "body", "sleeping") ? 1 : 0;
}

/* ---------- 查询(游标式:语言没有数组)---------- */
int64_t eng_query_rect(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 4) { dg_error("eng_query_rect needs (x, y, w, h[, mask])"); return -1; }
    return dg_phys_query_rect((float)dv_float(&a[0]), (float)dv_float(&a[1]),
                              (float)dv_float(&a[2]), (float)dv_float(&a[3]),
                              (int32_t)dv_int_or(a, n, 4, 0));
}

int64_t eng_query_circle(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_query_circle needs (cx, cy, r[, mask])"); return -1; }
    return dg_phys_query_circle((float)dv_float(&a[0]), (float)dv_float(&a[1]),
                                (float)dv_float(&a[2]), (int32_t)dv_int_or(a, n, 3, 0));
}

int64_t eng_query_point(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_query_point needs (x, y[, mask])"); return -1; }
    return dg_phys_query_point((float)dv_float(&a[0]), (float)dv_float(&a[1]),
                               (int32_t)dv_int_or(a, n, 2, 0));
}

int64_t eng_query_next(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_query_next needs a cursor"); return 0; }
    return dg_phys_query_next((int32_t)dv_int(&a[0]));
}

int64_t eng_query_count(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_query_count needs a cursor"); return -1; }
    return dg_phys_query_count((int32_t)dv_int(&a[0]));
}

int64_t eng_query_at(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_query_at needs (cursor, index)"); return 0; }
    return dg_phys_query_at((int32_t)dv_int(&a[0]), (int32_t)dv_int(&a[1]));
}

int64_t eng_query_reset(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_query_reset needs a cursor"); return -1; }
    return dg_phys_query_reset((int32_t)dv_int(&a[0]));
}

int64_t eng_query_end(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_query_end needs a cursor"); return -1; }
    return dg_phys_query_end((int32_t)dv_int(&a[0]));
}

int64_t eng_overlap(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_overlap needs (objectA, objectB)"); return -1; }
    return dg_phys_overlap((uint32_t)dv_int(&a[0]), (uint32_t)dv_int(&a[1]));
}

/* ---------- 射线 / 扫掠(结果槽)---------- */
int64_t eng_raycast(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 5) { dg_error("eng_raycast needs (x, y, dx, dy, dist, mask, ignore)"); return -1; }
    return dg_phys_raycast((float)dv_float(&a[0]), (float)dv_float(&a[1]),
                           (float)dv_float(&a[2]), (float)dv_float(&a[3]),
                           (float)dv_float(&a[4]), (int32_t)dv_int_or(a, n, 5, 0),
                           (uint32_t)dv_int_or(a, n, 6, 0));
}

int64_t eng_sweep_box(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 6) { dg_error("eng_sweep_box needs (x, y, hw, hh, dx, dy, mask, ignore)"); return -1; }
    return dg_phys_sweep_box((float)dv_float(&a[0]), (float)dv_float(&a[1]),
                             (float)dv_float(&a[2]), (float)dv_float(&a[3]),
                             (float)dv_float(&a[4]), (float)dv_float(&a[5]),
                             (int32_t)dv_int_or(a, n, 6, 0),
                             (uint32_t)dv_int_or(a, n, 7, 0));
}

int64_t eng_hit_obj(const DexValue *a, int n) {
    (void)a; (void)n;
    return dg_phys_hit()->obj;
}
double eng_hit_x(const DexValue *a, int n) { (void)a; (void)n; return (double)dg_phys_hit()->x; }
double eng_hit_y(const DexValue *a, int n) { (void)a; (void)n; return (double)dg_phys_hit()->y; }
double eng_hit_nx(const DexValue *a, int n) { (void)a; (void)n; return (double)dg_phys_hit()->nx; }
double eng_hit_ny(const DexValue *a, int n) { (void)a; (void)n; return (double)dg_phys_hit()->ny; }
double eng_hit_t(const DexValue *a, int n) { (void)a; (void)n; return (double)dg_phys_hit()->t; }
int64_t eng_hit_tile(const DexValue *a, int n) { (void)a; (void)n; return dg_phys_hit()->tile; }

/* ---------- 渲染插值开关 ---------- */
int64_t eng_render_set_interp(const DexValue *a, int n) {
    dg_clear_error();
    dg_phys_set_interp((int)dv_int_or(a, n, 0, 1));
    return dg_phys_get_interp();
}

/* ---------- 瓦片地图 ---------- */
static DgTilemap *eng_tilemap_arg(const DexValue *a, int n, const char *who) {
    if (n < 1) { dg_error("%s needs an object", who); return NULL; }
    DgTilemap *t = (DgTilemap *)dg_comp_get((uint32_t)dv_int(&a[0]), DG_C_TILEMAP);
    if (!t) dg_error("object %d has no 'tilemap'", (int)dv_int(&a[0]));
    return t;
}

int64_t eng_tilemap_load_csv(const DexValue *a, int n) {
    dg_clear_error();
    if (!eng_tilemap_arg(a, n, "eng_tilemap_load_csv")) return -1;
    if (n < 2) { dg_error("eng_tilemap_load_csv needs (object, text)"); return -1; }
    return dg_tilemap_load_csv((uint32_t)dv_int(&a[0]), dv_str(&a[1]));
}

int64_t eng_tilemap_load_file(const DexValue *a, int n) {
    dg_clear_error();
    if (!eng_tilemap_arg(a, n, "eng_tilemap_load_file")) return -1;
    if (n < 2) { dg_error("eng_tilemap_load_file needs (object, path)"); return -1; }
    return dg_tilemap_load_file((uint32_t)dv_int(&a[0]), dv_str(&a[1]));
}

int64_t eng_tilemap_save_csv(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_tilemap_save_csv needs (object, path)"); return -1; }
    return dg_tilemap_save_csv((uint32_t)dv_int(&a[0]), dv_str(&a[1]));
}

int64_t eng_tilemap_tile(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_tilemap_tile needs (object, col, row)"); return -1; }
    return dg_tilemap_tile((uint32_t)dv_int(&a[0]), (int32_t)dv_int(&a[1]), (int32_t)dv_int(&a[2]));
}

int64_t eng_tilemap_cols(const DexValue *a, int n) {
    dg_clear_error();
    return dg_tilemap_cols((uint32_t)dv_int_or(a, n, 0, 0));
}

int64_t eng_tilemap_rows(const DexValue *a, int n) {
    dg_clear_error();
    return dg_tilemap_rows((uint32_t)dv_int_or(a, n, 0, 0));
}

int64_t eng_tilemap_set_solid(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_tilemap_set_solid needs (object, tile, solid)"); return -1; }
    return dg_tilemap_set_solid((uint32_t)dv_int(&a[0]), (int32_t)dv_int(&a[1]),
                                (int)dv_int(&a[2]));
}

int64_t eng_tilemap_is_solid(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_tilemap_is_solid needs (object, tile)"); return 0; }
    return dg_tilemap_is_solid((uint32_t)dv_int(&a[0]), (int32_t)dv_int(&a[1])) ? 1 : 0;
}

int64_t eng_tilemap_solid_at(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 3) { dg_error("eng_tilemap_solid_at needs (object, worldX, worldY)"); return 0; }
    return dg_tilemap_solid_at((uint32_t)dv_int(&a[0]), (float)dv_float(&a[1]),
                               (float)dv_float(&a[2])) ? 1 : 0;
}

/* ============================================================
   输入(M4):轮询 + 动作映射
   ============================================================ */
int64_t eng_key_down(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_down((int)dv_int_or(a, n, 0, -1)) ? 1 : 0;
}
int64_t eng_key_pressed(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_pressed((int)dv_int_or(a, n, 0, -1)) ? 1 : 0;
}
int64_t eng_key_released(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_released((int)dv_int_or(a, n, 0, -1)) ? 1 : 0;
}
double eng_mouse_x(const DexValue *a, int n) { (void)a; (void)n; return (double)dg_input_mouse_x(); }
double eng_mouse_y(const DexValue *a, int n) { (void)a; (void)n; return (double)dg_input_mouse_y(); }
double eng_mouse_world_x(const DexValue *a, int n) {
    (void)a; (void)n;
    float wx = 0.0f, wy = 0.0f;
    dg_scene_screen_to_world(dg_input_mouse_x(), dg_input_mouse_y(), &wx, &wy);
    return (double)wx;
}
double eng_mouse_world_y(const DexValue *a, int n) {
    (void)a; (void)n;
    float wx = 0.0f, wy = 0.0f;
    dg_scene_screen_to_world(dg_input_mouse_x(), dg_input_mouse_y(), &wx, &wy);
    return (double)wy;
}
int64_t eng_mouse_down(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_mouse_down((int)dv_int_or(a, n, 0, 0)) ? 1 : 0;
}
int64_t eng_mouse_pressed(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_mouse_pressed((int)dv_int_or(a, n, 0, 0)) ? 1 : 0;
}
int64_t eng_mouse_released(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_mouse_released((int)dv_int_or(a, n, 0, 0)) ? 1 : 0;
}
int64_t eng_mouse_wheel(const DexValue *a, int n) { (void)a; (void)n; return dg_input_wheel(); }
int64_t eng_pad_connected(const DexValue *a, int n) { (void)a; (void)n; return dg_input_pad_connected() ? 1 : 0; }
double eng_pad_axis(const DexValue *a, int n) {
    dg_clear_error();
    return (double)dg_input_code_axis((int)dv_int_or(a, n, 0, 0));
}

int64_t eng_action_bind(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_action_bind needs (action, keycode)"); return -1; }
    return dg_input_action_bind(dv_str(&a[0]), (int)dv_int(&a[1]));
}
int64_t eng_action_unbind(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_action_unbind needs an action name"); return -1; }
    return dg_input_action_unbind(dv_str(&a[0]));
}
int64_t eng_action_down(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_action_down(dv_str_or(a, n, 0, "")) ? 1 : 0;
}
int64_t eng_action_pressed(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_action_pressed(dv_str_or(a, n, 0, "")) ? 1 : 0;
}
int64_t eng_action_released(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_action_released(dv_str_or(a, n, 0, "")) ? 1 : 0;
}
double eng_action_value(const DexValue *a, int n) {
    dg_clear_error();
    return (double)dg_input_action_value(dv_str_or(a, n, 0, ""));
}
int64_t eng_action_bound(const DexValue *a, int n) {
    dg_clear_error();
    return dg_input_action_bound(dv_str_or(a, n, 0, "")) ? 1 : 0;
}
int64_t eng_action_count(const DexValue *a, int n) { (void)a; (void)n; return dg_input_action_count(); }
const char *eng_action_name_at(const DexValue *a, int n) {
    dg_clear_error();
    const int i = (int)dv_int_or(a, n, 0, -1);
    if (i < 0 || i >= dg_input_action_count()) {
        dg_error("action index %d out of range 0..%d", i, dg_input_action_count() - 1);
        return "";
    }
    return dg_input_action_name(i);
}
int64_t eng_action_code(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_action_code needs (action, slot)"); return -1; }
    return dg_input_action_code(dv_str(&a[0]), (int)dv_int(&a[1]));
}

/* 合成输入(测试 / IDE 脚本化试玩)*/
int64_t eng_input_feed(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_input_feed needs (keycode, down)"); return -1; }
    return dg_input_feed((int)dv_int(&a[0]), (int)dv_int(&a[1]));
}
int64_t eng_input_feed_axis(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_input_feed_axis needs (axis, value)"); return -1; }
    return dg_input_feed_axis((int)dv_int(&a[0]), (float)dv_float(&a[1]));
}
int64_t eng_input_feed_mouse(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 2) { dg_error("eng_input_feed_mouse needs (x, y)"); return -1; }
    return dg_input_feed_mouse((float)dv_float(&a[0]), (float)dv_float(&a[1]));
}
int64_t eng_input_feed_wheel(const DexValue *a, int n) {
    dg_clear_error();
    if (n < 1) { dg_error("eng_input_feed_wheel needs a delta"); return -1; }
    dg_input_feed_wheel((int)dv_int(&a[0]));
    return 0;
}
int64_t eng_input_clear(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    dg_input_clear();
    return 0;
}

/* ---------- 颜色辅助 ----------
   DexLang **没有位移运算**(词法器里没有 << >>),所以用户无法从 r/g/b/a 拼出
   0xAARRGGBB —— 这个辅助是必需的,不是可选糖。 */
int64_t eng_rgba(const DexValue *a, int n) {
    dg_clear_error();
    const uint32_t r = (uint32_t)(dv_int_or(a, n, 0, 255) & 0xFF);
    const uint32_t g = (uint32_t)(dv_int_or(a, n, 1, 255) & 0xFF);
    const uint32_t b = (uint32_t)(dv_int_or(a, n, 2, 255) & 0xFF);
    const uint32_t al = (uint32_t)(dv_int_or(a, n, 3, 255) & 0xFF);
    return (int64_t)DG_RGBA(r, g, b, al);
}

/* ---------- 错误 ---------- */
const char *eng_last_error(const DexValue *a, int n) {
    (void)a; (void)n;
    return dg_last_error();
}

/* ---------- 适配器信息(不往 stdout 打印,按需查询) ---------- */
const char *eng_gpu_name(const DexValue *a, int n) {
    (void)a; (void)n;
    return dg_gfx_gpu_name();
}

int64_t eng_vram_mb(const DexValue *a, int n) {
    (void)a; (void)n;
    return (int64_t)dg_gfx_vram_mb();
}

int64_t eng_clear_error(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
    return 0;
}

/* 库版本号:宿主与库不同步时便于排查。1 = M1(仅渲染),2 = M2(+实体/组件/场景) */
int64_t eng_version(const DexValue *a, int n) {
    (void)a; (void)n;
    return 2;
}
