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
    g_frame_index = 0;
    return 0;
}

int64_t eng_shutdown(const DexValue *a, int n) {
    (void)a; (void)n;
    dg_clear_error();
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
    dg_gfx_bind_target();
    dg_gfx_clear(g_clear_color);
    dg_draw_frame_begin();
    return 1;
}

int64_t eng_frame_end(const DexValue *a, int n) {
    (void)a; (void)n;
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
