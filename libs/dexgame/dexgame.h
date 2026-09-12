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
void dg_draw_shutdown(void);
void dg_draw_frame_begin(void);      /* 重置批次 + 设默认状态 */
void dg_draw_frame_end(void);        /* 提交剩余批次(必须在 Present 前调用)*/

/* 纹理。id 由 eng_tex_load 返回;内部用代际句柄校验。 */
int  dg_tex_load_file(const char *path);
int  dg_tex_create_rgba(int w, int h, const uint8_t *rgba);  /* 便于测试与程序化生成 */
int  dg_tex_free(int id);
int  dg_tex_valid(int id);
int  dg_tex_width(int id);
int  dg_tex_height(int id);

/* 绘制:三角形按顺序入批;切换纹理/满批时自动 flush */
int  dg_draw_quad(int tex, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, uint32_t color);
int  dg_draw_rect(float x, float y, float w, float h, uint32_t color);

#endif /* DEXGAME_H */
