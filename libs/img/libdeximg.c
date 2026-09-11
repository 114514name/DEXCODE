/*
 * libdeximg.c — DEXCODE 原生库:把图片渲染到控制台(ANSI 真彩色半块字符)
 *
 * 接口(见 img.dexdef):
 *   dex_render(path: string) -> string    渲染文本(含 ANSI 转义 + 换行),交给 print 输出
 *   dex_img_width(path: string) -> int    图片宽度(像素),失败返回 -1
 *   dex_img_height(path: string) -> int   图片高度(像素),失败返回 -1
 *
 * 当前支持 24/32 位 BMP(BI_RGB,零依赖)。渲染算法:
 *   每 2 个像素行合成 1 行"半块字符"(▀ U+2580),上像素作前景色、
 *   下像素作背景色,实现 2 倍纵向分辨率的真彩色输出。
 *
 * 构建(Windows,使用 zig):
 *   zig cc -shared -target x86_64-windows-gnu -O2 \
 *       -o libs/img/libdeximg.dll libs/img/libdeximg.c
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#include <windows.h>
#else
#define EXPORT
#endif

#define MAX_RENDER_W 80   /* 渲染最大列数(适配控制台宽度) */
#define MAX_RENDER_H 30   /* 渲染最大行数(半块字符行数) */

/* 启用 Windows 控制台 VT 处理,使 ANSI 转义序列真正上色 */
static void enable_vt(void) {
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

/* 读取 BMP(24/32 位,BI_RGB),返回 top-down 的 RGBA 缓冲(malloc);失败返回 NULL */
static unsigned char *read_bmp(const char *path, int *ow, int *oh) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return NULL;
    }
    int w = (int)(uint32_t)(hdr[18] | hdr[19] << 8 | hdr[20] << 16 | (uint32_t)hdr[21] << 24);
    int h = (int)(uint32_t)(hdr[22] | hdr[23] << 8 | hdr[24] << 16 | (uint32_t)hdr[25] << 24);
    uint16_t bpp = (uint16_t)(hdr[28] | hdr[29] << 8);
    uint32_t comp = (uint32_t)(hdr[30] | hdr[31] << 8 | hdr[32] << 16 | (uint32_t)hdr[33] << 24);
    uint32_t data_off = (uint32_t)(hdr[10] | hdr[11] << 8 | hdr[12] << 16 | (uint32_t)hdr[13] << 24);
    int bottom_up = 1;
    if (h < 0) { h = -h; bottom_up = 0; }   /* 负高度 = top-down */
    if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32) || comp != 0) {
        fclose(f);
        return NULL;
    }
    int bytes_pp = bpp / 8;
    int stride = ((w * bytes_pp) + 3) & ~3;   /* 每行 4 字节对齐 */
    unsigned char *row = (unsigned char *)malloc((size_t)stride);
    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
    if (!row || !rgba) { free(row); free(rgba); fclose(f); return NULL; }
    if (fseek(f, (long)data_off, SEEK_SET) != 0) {
        free(row); free(rgba); fclose(f); return NULL;
    }
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, (size_t)stride, f) != (size_t)stride) {
            free(row); free(rgba); fclose(f); return NULL;
        }
        int out_y = bottom_up ? (h - 1 - y) : y;
        unsigned char *dst = rgba + (size_t)out_y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[x * 4 + 0] = row[x * bytes_pp + 2];  /* R */
            dst[x * 4 + 1] = row[x * bytes_pp + 1];  /* G */
            dst[x * 4 + 2] = row[x * bytes_pp + 0];  /* B */
            dst[x * 4 + 3] = (bytes_pp == 4) ? row[x * bytes_pp + 3] : 255;
        }
    }
    free(row);
    fclose(f);
    *ow = w;
    *oh = h;
    return rgba;
}

/* ---------- 简单的动态字符串构建器 ---------- */
typedef struct { char *buf; size_t len, cap; } SB;

static void sb_init(SB *sb) { sb->buf = NULL; sb->len = 0; sb->cap = 0; }

static void sb_reserve(SB *sb, size_t add) {
    if (sb->len + add + 1 > sb->cap) {
        size_t nc = sb->cap ? sb->cap * 2 : 1024;
        while (nc < sb->len + add + 1) nc *= 2;
        sb->buf = (char *)realloc(sb->buf, nc);
        sb->cap = nc;
    }
}

static void sb_put(SB *sb, const char *s) {
    size_t n = strlen(s);
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_printf(SB *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    sb_reserve(sb, (size_t)n);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

static void sb_free(SB *sb) { free(sb->buf); sb->buf = NULL; }

/* 静态结果缓冲(VM 会用 xstrdup 复制返回值,可安全复用) */
static char g_render_buf[1 << 20];   /* 1 MB */

static const char *render_bmp(const char *path) {
    int w = 0, h = 0;
    unsigned char *rgba = read_bmp(path, &w, &h);
    if (!rgba)
        return "error: cannot decode image (need 24/32-bit BMP)";
    enable_vt();
    /* 等比缩小,适配控制台 */
    int scale = 1;
    while ((w / scale) > MAX_RENDER_W || (h / scale) > MAX_RENDER_H * 2) scale++;
    int tw = w / scale, th = h / scale;
    SB sb;
    sb_init(&sb);
    for (int y = 0; y < th; y += 2) {
        for (int x = 0; x < tw; x++) {
            int sx = x * scale, sy = y * scale;
            unsigned char *top = rgba + ((size_t)sy * w + sx) * 4;
            unsigned char *bot = NULL;
            if (y + 1 < th)
                bot = rgba + ((size_t)((y + 1) * scale) * w + sx) * 4;
            /* 半块字符 ▀:上半像素作前景色,下半像素作背景色 */
            sb_printf(&sb, "\x1b[38;2;%u;%u;%um", top[0], top[1], top[2]);
            if (bot)
                sb_printf(&sb, "\x1b[48;2;%u;%u;%um", bot[0], bot[1], bot[2]);
            else
                sb_put(&sb, "\x1b[48;2;0;0;0m");
            sb_put(&sb, "\xE2\x96\x80"); /* ▀ */
        }
        sb_put(&sb, "\x1b[0m\n");
    }
    free(rgba);
    if (sb.len >= sizeof(g_render_buf)) sb.len = sizeof(g_render_buf) - 1;
    memcpy(g_render_buf, sb.buf ? sb.buf : "", sb.len);
    g_render_buf[sb.len] = '\0';
    sb_free(&sb);
    return g_render_buf;
}

/* string -> string:把图片渲染成 ANSI 彩色文本 */
EXPORT const char *dex_render(const char *path) {
    return render_bmp(path ? path : "");
}

/* string -> int:图片宽度(像素) */
EXPORT int64_t dex_img_width(const char *path) {
    int w = 0, h = 0;
    unsigned char *rgba = read_bmp(path ? path : "", &w, &h);
    if (!rgba) return -1;
    free(rgba);
    return w;
}

/* string -> int:图片高度(像素) */
EXPORT int64_t dex_img_height(const char *path) {
    int w = 0, h = 0;
    unsigned char *rgba = read_bmp(path ? path : "", &w, &h);
    if (!rgba) return -1;
    free(rgba);
    return h;
}
