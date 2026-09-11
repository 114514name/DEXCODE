/*
 * libdexxgal.dll — GAL 引擎核心(纯 C,零第三方运行时依赖)
 *
 * 能力:窗口渲染(GDI 双缓冲)、背景图/纯色、角色立绘(多图层,带透明)、
 *      打字机文本、点击推进、选项分支、BGM/音效(mci)、截图(BMP)。
 * 图片解码用 stb_image.h(单头文件,PNG/JPEG/BMP/GIF,含 alpha 通道)。
 *
 * 使用模式(语言侧主循环驱动):
 *   include "gal";
 *   gal_init(960, 540);
 *   gal_bg("res/bg.png");
 *   gal_sprite(1, "res/char.png"); gal_sprite_pos(1, 300);
 *   gal_text("你好,世界");
 *   while gal_running() {
 *       gal_poll();                     // 泵消息 + 推进打字机 + 渲染一帧
 *       if gal_text_done() && gal_clicked() {
 *           gal_text_clear();
 *           // 下一句 / 选项等
 *       }
 *       if gal_picked() >= 0 { ... }
 *       gal_wait(16);
 *   }
 *   gal_close();
 *
 * 构建:
 *   zig cc -shared -target x86_64-windows-gnu -O2 \
 *       -lgdi32 -luser32 -lwinmm -lmsimg32 \
 *       -o libs/gal/libdexxgal.dll libs/gal/libdexxgal.c
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <time.h>

#define EXPORT __declspec(dllexport)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define MAX_SPR 4       /* 立绘图层数 */
#define MAX_CHOICES 4   /* 选项数 */
#define MAX_TEXT 8192   /* 文本最大字符(wchar) */
#define MAX_MENU_CTRL 64   /* 菜单控件数 */
#define MAX_CTRL_NAME 48
#define MAX_CTRL_TEXT 256

/* 菜单控件 */
typedef struct {
    char name[MAX_CTRL_NAME];
    int type;          /* 0按钮 1文字 2图片 3面板 4输入框 */
    int x, y, w, h;
    int show;
    int bgColor, fgColor;
    char text[MAX_CTRL_TEXT];
    HBITMAP img; int imgW, imgH;
    int fit;           /* 图片适配:0拉伸 1原大小 2保持比例完整 3保持比例填满(相对控件) */
} MenuCtrl;

/* 场景特效位(可叠加) */
#define FX_FILM      (1ULL << 0)   /* 老电影 */
#define FX_MEMORY    (1ULL << 1)   /* 回忆 */
#define FX_HAZE      (1ULL << 2)   /* 朦胧 */
#define FX_SHAKE     (1ULL << 3)   /* 震动 */
#define FX_MOSAIC    (1ULL << 4)   /* 马赛克 */
#define FX_INVERT    (1ULL << 5)   /* 反色 */
#define FX_BLUR      (1ULL << 6)   /* 模糊 */
#define FX_MIRROR    (1ULL << 7)   /* 镜像 */
#define FX_GRAY      (1ULL << 8)   /* 灰度 */
#define FX_VIGNETTE  (1ULL << 9)   /* 暗角 */
#define FX_SCANLINE  (1ULL << 10)  /* 扫描线 */
#define FX_NIGHT     (1ULL << 11)  /* 夜视 */
#define FX_CRT       (1ULL << 12)  /* 老电视(CRT) */
#define FX_GLITCH    (1ULL << 13)  /* 故障 */
#define MAX_PATHW 1024

/* ---------- 引擎状态 ---------- */
typedef struct {
    /* 窗口 */
    HWND hwnd;
    int w, h;
    int running;
    HINSTANCE hInst;

    /* 双缓冲 */
    HDC memDC;            /* 主缓冲 DC(窗口客户区尺寸 32bpp) */
    HBITMAP memBmp;
    BITMAPINFO memInfo;
    void *memBits;

    /* 背景 */
    HBITMAP bgBmp; int bgW, bgH;
    int bgColor;          /* 0xRRGGBB, -1 表示使用图片 */
    int hasBg;
    int bgFit;            /* 背景适配:0拉伸 1原尺寸 2保持比例完整 3保持比例填满 */

    /* 立绘 */
    HBITMAP sprBmp[MAX_SPR]; int sprW[MAX_SPR]; int sprH[MAX_SPR];
    int sprX[MAX_SPR]; int sprScale[MAX_SPR]; int sprShow[MAX_SPR];
    HDC sprDC[MAX_SPR];
    int sprPosMode[MAX_SPR];   /* 0中间 1左 2右 3偏左 4偏右,-1=手动x */
    int sprY[MAX_SPR];         /* 手动Y坐标; -1=底部对齐 */
    int sprAnim[MAX_SPR];      /* 0无 1呼吸 2淡入 3上浮 4抖动 5脉冲 6消失 */
    int sprAlpha[MAX_SPR];     /* 0-255 */
    int sprAnimTick[MAX_SPR];
    int sprAutofit[MAX_SPR];   /* 自适应:裁剪透明区并缩放到屏高 78% */
    HBITMAP sprTrimBmp[MAX_SPR]; int sprTrimW[MAX_SPR]; int sprTrimH[MAX_SPR];
    HDC sprTrimDC[MAX_SPR];
    int sprFit[MAX_SPR];       /* 立绘适配:0原大小 1自动适配 2完整显示 3填满 */

    /* 文本 */
    wchar_t curText[MAX_TEXT];
    wchar_t speaker[128];
    size_t textChars;     /* 已显示 wchar 数 */
    DWORD textTick;
    int textActive, textDone, skip;
    int textColor;
    int fontSize;
    HFONT font, fontName;

    /* 输入 */
    int clicked;

    /* 选项 */
    wchar_t choiceText[MAX_CHOICES][256];
    int choiceShow;
    int picked;

    /* 对话框 */
    int boxOn;
    int boxAvatar;            /* 有头像对话框(用立绘图层0作头像) */
    int boxName;              /* 显示人名 */
    int boxAutoFit;           /* 对话框宽度自适应 */
    int textPos;              /* 文字位置:0中间 1左 2右 3偏左 4偏右 */
    int textMode;             /* 文字模式:0逐字 1直接全部 2先空框点击后显示 */
    int textHold;             /* 模式2:等待点击才开始显示 */
    int textSpeed;            /* 逐字间隔 ms(0=默认 40) */

    /* 音频 */
    int bgmOpen;
    int volume;

    /* 背景交叉淡化(柔和换背景) */
    HBITMAP bgTo; int bgToW, bgToH; int bgTransOn;
    double bgTransT0, bgTransDur;
    /* 镜头淡入淡出过渡 */
    int fadePhase;      /* 0无 1淡出(变黑) 2淡入(变亮) */
    double fadeT0, fadeDur;

    /* 场景特效 */
    unsigned long long fx;   /* 激活特效位掩码(可叠加) */
    int fxStrength;          /* 特效强度 0-100(默认 50) */
    int fxTick;              /* 特效动画/噪声时钟 */

    /* 菜单系统 */
    int menuMode;            /* 0无 1独占(modal) 2叠加(overlay) */
    int menuActive;          /* 独占:事件循环活跃 */
    int menuOverlayOn;       /* 叠加:是否显示 */
    int menuFocus;           /* 输入框焦点控件索引,-1=无 */
    char menuResult[MAX_CTRL_TEXT];
    char menuEvent[MAX_CTRL_NAME];   /* 最近点击控件名 */
    MenuCtrl menuCtrl[MAX_MENU_CTRL];
    int menuCtrlCount;
    HBITMAP menuBg; int menuBgW, menuBgH;
    unsigned char *menuShot;         /* 独占进入时镜头画面快照 */
    char menuSignalName[MAX_CTRL_NAME];
    char menuSignalData[MAX_CTRL_TEXT];

    /* 主题 / 鼠标 / 拖尾 */
    int mouseX, mouseY;
    int trailX[64], trailY[64];
    int trailN;
    int trailOn;                    /* 鼠标拖尾总开关(可在运行时/设置里关闭) */
    int boxHover;                   /* 鼠标是否悬停在对话框上 */
    double boxHoverT;               /* 悬停动画 0→1 */
} Gal;

/* ---------- 主题系统 ---------- */
typedef struct {
    const char *name;
    int boxBg; int boxAlpha;              /* 对话框背景 / 不透明度 */
    int boxBorder; int boxRound;          /* 对话框边框色 / 圆角半径(0=无边框) */
    int boxShape;                         /* 对话框形状:0圆角 1方角 2切角 3上圆下直 4双线框 */
    int boxDecor;                         /* 对话框装饰:0无 1樱花 2星芒 3藤叶 4光点 5三角 */
    int boxFx;                            /* 对话框特效:0无 1边框流光 2底部光带 3四角弧 4细线 */
    int nameColor;                        /* 说话人名 */
    int textColor;                        /* 正文 */
    int glow;                             /* 1=文字带柔和阴影 */
    int optBg, optBorder, optHover;       /* 选项填充/边框/悬停 */
    int optRound;                         /* 选项圆角 */
    int optText;                          /* 选项文字 */
    int btnBg, btnBorder, btnHover;       /* 菜单按钮填充/边框/悬停 */
    int btnRound;                         /* 菜单按钮圆角 */
    int ctrlText;                         /* 菜单控件文字 */
    int trail; int trailColor; int trailLen;  /* 鼠标拖尾 */
} Theme;

static const Theme THEMES[] = {
    /* 默认:现状 */
    {"默认", 0x0F0F14, 200, 0x000000, 0, 0, 0, 0, 0xFFFFDC, 0xFFFFFF, 0,
     0x282D42, 0x8C96C8, 0x3A4670, 14, 0xFFFFFF,
     0x3377DD, 0x8C96C8, 0x5599FF, 14, 0xFFFFFF, 0, 0x000000, 0},
    /* 中世纪:羊皮纸暖棕 + 暗金,木质按钮,金粉拖尾;双线框+四角弧+藤叶 */
    {"中世纪", 0x2A1E10, 220, 0xC9A24B, 12, 4, 3, 3, 0xF5DFA8, 0xF0E0C0, 0,
     0x3A2A18, 0xC9A24B, 0x5A4A28, 12, 0xFFE8C0,
     0x8A5A2A, 0xC9A24B, 0xA87A3A, 12, 0xFFE0B0, 1, 0xC9A24B, 36},
    /* 史诗:暗红 + 鎏金,庄重;圆角+光点+底部光带 */
    {"史诗", 0x1A0F12, 228, 0xE8C060, 14, 0, 4, 2, 0xFFD970, 0xF5E6C8, 1,
     0x2A161C, 0xE8C060, 0x4A2A30, 10, 0xFFE9A8,
     0x7A3A28, 0xE8C060, 0xA85630, 10, 0xFFDFA0, 1, 0xE8C060, 44},
    /* 简约:白 + 浅灰,极简;方角+细线 */
    {"简约", 0xF2F2F2, 235, 0xC8C8C8, 4, 1, 0, 4, 0x444444, 0x222222, 0,
     0xFFFFFF, 0xBBBBBB, 0xE8E8E8, 6, 0x222222,
     0xFFFFFF, 0xBBBBBB, 0xE0E0E0, 6, 0x333333, 0, 0x000000, 0},
    /* 科幻:深蓝黑 + 青色霓虹;切角+星芒+流光 */
    {"科幻", 0x0A0E1A, 222, 0x29E6FF, 10, 2, 2, 1, 0x7DF3FF, 0xE0F7FF, 1,
     0x0E1626, 0x29E6FF, 0x145066, 8, 0xBFF4FF,
     0x0E2A3A, 0x29E6FF, 0x145066, 8, 0xD0FAFF, 1, 0x29E6FF, 40},
    /* 樱花:粉白 + 柔和;上圆下直+樱花花瓣+流光 */
    {"樱花", 0x241320, 216, 0xFF9EC4, 14, 3, 1, 1, 0xFFD3E2, 0xFFF0F6, 0,
     0x2A1622, 0xFF9EC4, 0x4A2A3A, 14, 0xFFE4EE,
     0x4A2A4A, 0xFF9EC4, 0x6A3A5A, 14, 0xFFF0F8, 1, 0xFFB6D2, 42},
    /* 风雅:墨色和风 + 米色;上圆下直+藤叶+四角弧 */
    {"风雅", 0x14100E, 222, 0xD8C8A8, 8, 3, 3, 3, 0xE8D8B8, 0xF0E8D8, 0,
     0x1C1812, 0xD8C8A8, 0x3A3226, 8, 0xF0E4CC,
     0x3A3226, 0xD8C8A8, 0x544A38, 8, 0xF5EAD4, 0, 0xD8C8A8, 30},
    /* 压抑:深灰 + 暗紫;方角+细线 */
    {"压抑", 0x0A0A0F, 232, 0x5A5A6A, 6, 1, 0, 4, 0xA0A0B0, 0xC8C8D4, 0,
     0x14141C, 0x5A5A6A, 0x2A2A3A, 6, 0xB8B8C4,
     0x1A1A26, 0x5A5A6A, 0x2E2E3E, 6, 0xCCC8D4, 0, 0x5A5A6A, 22},
    /* 奇特:霓虹撞色(紫+粉+绿);切角+三角+流光 */
    {"奇特", 0x12081E, 226, 0xFF3D81, 12, 2, 5, 1, 0xFF9CFF, 0xF0E8FF, 1,
     0x1C0A2A, 0xFF3D81, 0x3A1460, 12, 0xFFC2FF,
     0x2A0A3A, 0x29FF8C, 0x1A5A40, 12, 0xC8FFE0, 1, 0xFF3D81, 46},
};

static Theme g_theme = { "默认", 0x0F0F14, 200, 0x000000, 0, 0, 0, 0, 0xFFFFDC, 0xFFFFFF, 0,
     0x282D42, 0x8C96C8, 0x3A4670, 14, 0xFFFFFF,
     0x3377DD, 0x8C96C8, 0x5599FF, 14, 0xFFFFFF, 0, 0x000000, 0 };

static Gal G;
static int g_inited = 0;

/* ---------- 工具 ---------- */
static void wpath(const char *utf8, wchar_t *out, int cap) {
    MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, out, cap);
}

/* 从 RGBA 像素(stb)创建 32bpp DIB 位图;GDI 为 BGRA,stb 为 RGBA,需交换 R/B */
static HBITMAP make_bitmap(int w, int h, const unsigned char *rgba) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;        /* 自顶向下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!bmp || !bits) return NULL;
    /* RGBA -> BGRA */
    unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < w * h; i++) {
        p[i * 4 + 0] = rgba[i * 4 + 2]; /* B */
        p[i * 4 + 1] = rgba[i * 4 + 1]; /* G */
        p[i * 4 + 2] = rgba[i * 4 + 0]; /* R */
        p[i * 4 + 3] = rgba[i * 4 + 3]; /* A */
    }
    return bmp;
}

/* 加载图片(任意 stb 支持的格式),返回 32bpp 位图;失败返回 NULL。
 * 用宽字符打开文件(支持中文/Unicode 路径),再交给 stb 从内存解码。 */
static HBITMAP load_image(const char *path, int *ow, int *oh) {
    wchar_t wp[MAX_PATHW];
    wpath(path, wp, MAX_PATHW);
    FILE *f = _wfopen(wp, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(buf, (int)n, &w, &h, &comp, 4);
    free(buf);
    if (!px) return NULL;
    HBITMAP bmp = make_bitmap(w, h, px);
    stbi_image_free(px);
    if (bmp) { if (ow) *ow = w; if (oh) *oh = h; }
    return bmp;
}

/* 以 alpha 合成一张图到主缓冲(CPU 逐像素,支持透明,不依赖 GDI AlphaBlend 的 alpha 读取) */
static void blend_a(HDC dst, int dx, int dy, int dw, int dh,
                    HDC src, int sw, int sh, int alpha) {
    (void)dst;
    if (!G.memBits || !sw || !sh) return;
    HBITMAP srcBmp = (HBITMAP)GetCurrentObject(src, OBJ_BITMAP);
    if (!srcBmp) return;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sw; bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    unsigned char *spx = (unsigned char *)malloc((size_t)sw * sh * 4);
    if (!spx) return;
    if (!GetDIBits(src, srcBmp, 0, sh, spx, &bi, DIB_RGB_COLORS)) { free(spx); return; }
    unsigned char *dpx = (unsigned char *)G.memBits;
    for (int y = 0; y < dh; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= G.h) continue;
        int sy = y * sh / dh;
        if (sy < 0) sy = 0; else if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= G.w) continue;
            int sx = x * sw / dw;
            if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
            int si = sy * sw + sx;
            int sa = spx[si * 4 + 3];
            int a = (sa * alpha) / 255;
            if (a == 0) continue;
            unsigned char *d = dpx + (ty * G.w + tx) * 4;
            int ia = 255 - a;
            d[0] = (unsigned char)((d[0] * ia + spx[si * 4 + 0] * a) / 255);
            d[1] = (unsigned char)((d[1] * ia + spx[si * 4 + 1] * a) / 255);
            d[2] = (unsigned char)((d[2] * ia + spx[si * 4 + 2] * a) / 255);
            d[3] = 255;
        }
    }
    free(spx);
}
static void blend(HDC dst, int dx, int dy, int dw, int dh,
                  HDC src, int sw, int sh) {
    blend_a(dst, dx, dy, dw, dh, src, sw, sh, 255);
}

/* 同 blend_a,但目标像素额外限定在矩形 [cl,ct)-(cr,cb) 内(用于把图案裁进对话框)。
   blend 系是 CPU 直写 memBits,GDI 剪裁区对其无效,必须手动裁剪。 */
static void blend_a_clip(HDC dst, int dx, int dy, int dw, int dh,
                         HDC src, int sw, int sh, int alpha,
                         int cl, int ct, int cr, int cb) {
    (void)dst;
    if (!G.memBits || !sw || !sh) return;
    HBITMAP srcBmp = (HBITMAP)GetCurrentObject(src, OBJ_BITMAP);
    if (!srcBmp) return;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sw; bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    unsigned char *spx = (unsigned char *)malloc((size_t)sw * sh * 4);
    if (!spx) return;
    if (!GetDIBits(src, srcBmp, 0, sh, spx, &bi, DIB_RGB_COLORS)) { free(spx); return; }
    unsigned char *dpx = (unsigned char *)G.memBits;
    for (int y = 0; y < dh; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= G.h || ty < ct || ty >= cb) continue;
        int sy = y * sh / dh;
        if (sy < 0) sy = 0; else if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= G.w || tx < cl || tx >= cr) continue;
            int sx = x * sw / dw;
            if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
            int si = sy * sw + sx;
            int sa = spx[si * 4 + 3];
            int a = (sa * alpha) / 255;
            if (a == 0) continue;
            unsigned char *d = dpx + (ty * G.w + tx) * 4;
            int ia = 255 - a;
            d[0] = (unsigned char)((d[0] * ia + spx[si * 4 + 0] * a) / 255);
            d[1] = (unsigned char)((d[1] * ia + spx[si * 4 + 1] * a) / 255);
            d[2] = (unsigned char)((d[2] * ia + spx[si * 4 + 2] * a) / 255);
            d[3] = 255;
        }
    }
    free(spx);
}

/* ---------- 立绘布局(位置模式)/自适应/动画 ---------- */
static void spr_layout(int i) {
    if (G.sprPosMode[i] < 0 || !G.sprBmp[i]) return;   /* 手动模式不管 */
    int sw = G.sprW[i], sh = G.sprH[i];
    if (G.sprAutofit[i] && G.sprTrimBmp[i]) { sw = G.sprTrimW[i]; sh = G.sprTrimH[i]; }
    int dw = sw * G.sprScale[i] / 100, dh = sh * G.sprScale[i] / 100;
    switch (G.sprPosMode[i]) {
    case 0:  G.sprX[i] = (G.w - dw) / 2; break;           /* 中间 */
    case 1:  G.sprX[i] = 0; break;                        /* 左 */
    case 2:  G.sprX[i] = G.w - dw; break;                 /* 右 */
    case 3:  G.sprX[i] = G.w / 2 - dw - 60; break;        /* 偏左 */
    case 4:  G.sprX[i] = G.w / 2 + 60; break;             /* 偏右 */
    default: break;
    }
}

/* 自适应:裁剪透明包围盒,缩放使高度 ≈ 屏高 78% */
static void spr_autofit(int i) {
    if (G.sprTrimBmp[i]) { DeleteObject(G.sprTrimBmp[i]); G.sprTrimBmp[i] = NULL; }
    if (G.sprTrimDC[i]) { DeleteDC(G.sprTrimDC[i]); G.sprTrimDC[i] = NULL; }
    if (!G.sprBmp[i]) return;
    int w = G.sprW[i], h = G.sprH[i];
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ ob = SelectObject(dc, G.sprBmp[i]);
    GetDIBits(dc, G.sprBmp[i], 0, h, px, &bi, DIB_RGB_COLORS);
    SelectObject(dc, ob); DeleteDC(dc);
    /* 找 alpha>8 包围盒 */
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            if (row[x * 4 + 3] > 8) {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < x0 || y1 < y0) { free(px); return; }
    int tw = x1 - x0 + 1, th = y1 - y0 + 1;
    /* 裁剪出包围盒区域 */
    BITMAPINFO nbi; memset(&nbi, 0, sizeof nbi);
    nbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    nbi.bmiHeader.biWidth = tw; nbi.bmiHeader.biHeight = -th;
    nbi.bmiHeader.biPlanes = 1; nbi.bmiHeader.biBitCount = 32;
    nbi.bmiHeader.biCompression = BI_RGB;
    void *nbits = NULL;
    HDC tdc = GetDC(NULL);
    HBITMAP nbmp = CreateDIBSection(tdc, &nbi, DIB_RGB_COLORS, &nbits, NULL, 0);
    ReleaseDC(NULL, tdc);
    if (nbmp && nbits) {
        for (int y = 0; y < th; y++)
            memcpy((unsigned char *)nbits + (size_t)y * tw * 4,
                   px + ((size_t)(y0 + y) * w + x0) * 4, (size_t)tw * 4);
    }
    free(px);
    G.sprTrimBmp[i] = nbmp; G.sprTrimW[i] = tw; G.sprTrimH[i] = th;
    G.sprTrimDC[i] = CreateCompatibleDC(NULL);
    SelectObject(G.sprTrimDC[i], nbmp);
    /* 缩放到屏高 78% */
    G.sprScale[i] = (int)(G.h * 0.78 * 100.0 / th);
    if (G.sprScale[i] < 5) G.sprScale[i] = 5;
    if (G.sprScale[i] > 500) G.sprScale[i] = 500;
    spr_layout(i);
}

static void tick_sprites(void) {
    for (int i = 0; i < MAX_SPR; i++) G.sprAnimTick[i]++;
}

/* ---------- 渲染 ---------- */
/* 选项自绘(定义见文件末尾,此处前向声明) */
static int choice_count(void);
static void choice_rect(int i, int nc, int *x0, int *y0, int *x1, int *y1);
static void draw_choices(HDC dc);

/* 场景特效后处理:对主缓冲 memBits 逐像素应用(整帧数码效果,可叠加)。 */
static void apply_fx(void) {
    unsigned long long f = G.fx;
    if (!f || !G.memBits || G.w <= 0 || G.h <= 0) return;
    int W = G.w, H = G.h;
    int str = G.fxStrength;
    unsigned char *px = (unsigned char *)G.memBits;
    unsigned char *tmp = (unsigned char *)malloc((size_t)W * H * 4);
    if (!tmp) return;
    G.fxTick++;

    /* --- 整帧重排类特效(基于最新画面) --- */
    if (f & FX_SHAKE) {
        memcpy(tmp, px, (size_t)W * H * 4);
        int amp = 1 + str * 9 / 100;
        int ox = (rand() % (2 * amp + 1)) - amp;
        int oy = (rand() % (2 * amp + 1)) - amp;
        for (int y = 0; y < H; y++) {
            int sy = y + oy; if (sy < 0) sy = 0; else if (sy >= H) sy = H - 1;
            unsigned char *dp = px + (size_t)y * W * 4;
            unsigned char *sp = tmp + (size_t)sy * W * 4;
            for (int x = 0; x < W; x++) {
                int sx = x + ox; if (sx < 0) sx = 0; else if (sx >= W) sx = W - 1;
                dp[x * 4] = sp[sx * 4]; dp[x * 4 + 1] = sp[sx * 4 + 1]; dp[x * 4 + 2] = sp[sx * 4 + 2];
            }
        }
    }
    if (f & FX_MIRROR) {
        memcpy(tmp, px, (size_t)W * H * 4);
        for (int y = 0; y < H; y++) {
            unsigned char *dp = px + (size_t)y * W * 4;
            unsigned char *sp = tmp + (size_t)y * W * 4;
            for (int x = 0; x < W; x++) {
                int sx = W - 1 - x;
                dp[x * 4] = sp[sx * 4]; dp[x * 4 + 1] = sp[sx * 4 + 1]; dp[x * 4 + 2] = sp[sx * 4 + 2];
            }
        }
    }
    if (f & FX_MOSAIC) {
        memcpy(tmp, px, (size_t)W * H * 4);
        int bs = 2 + str * 10 / 100;
        for (int y = 0; y < H; y += bs) {
            for (int x = 0; x < W; x += bs) {
                int bx = x + bs / 2, by = y + bs / 2;
                if (bx >= W) bx = W - 1;
                if (by >= H) by = H - 1;
                unsigned char *s = tmp + ((size_t)by * W + bx) * 4;
                for (int yy = y; yy < y + bs && yy < H; yy++)
                    for (int xx = x; xx < x + bs && xx < W; xx++) {
                        unsigned char *d = px + ((size_t)yy * W + xx) * 4;
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                    }
            }
        }
    }
    if (f & FX_BLUR) {
        memcpy(tmp, px, (size_t)W * H * 4);
        int rad = 1 + str * 2 / 100;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int r = 0, g = 0, b = 0, n = 0;
                for (int dy = -rad; dy <= rad; dy++) {
                    int sy = y + dy; if (sy < 0 || sy >= H) continue;
                    for (int dx = -rad; dx <= rad; dx++) {
                        int sx = x + dx; if (sx < 0 || sx >= W) continue;
                        unsigned char *s = tmp + ((size_t)sy * W + sx) * 4;
                        b += s[0]; g += s[1]; r += s[2]; n++;
                    }
                }
                unsigned char *d = px + ((size_t)y * W + x) * 4;
                d[0] = (unsigned char)(b / n); d[1] = (unsigned char)(g / n); d[2] = (unsigned char)(r / n);
            }
        }
    }
    if (f & FX_GLITCH) {
        memcpy(tmp, px, (size_t)W * H * 4);
        int dr = 2 + str * 8 / 100;   /* RGB 通道分离幅度 */
        int db = 2 + str * 8 / 100;
        /* 通道分离:红取左侧、蓝取右侧(色差) */
        for (int y = 0; y < H; y++) {
            unsigned char *dp = px + (size_t)y * W * 4;
            unsigned char *sp = tmp + (size_t)y * W * 4;
            for (int x = 0; x < W; x++) {
                int sr = x - dr; if (sr < 0) sr = 0;
                int sb = x + db; if (sb >= W) sb = W - 1;
                dp[x * 4 + 2] = sp[sr * 4 + 2];       /* R */
                dp[x * 4 + 0] = sp[sb * 4 + 0];       /* B */
            }
        }
        /* 随机行段水平错位(故障条) */
        int nband = 1 + str * 4 / 100;
        for (int i = 0; i < nband; i++) {
            int y0 = rand() % H;
            int hh = 3 + rand() % (5 + str * 8 / 100);
            int off = ((rand() % (5 + str * 10 / 100)) - 2 - str * 3 / 100);
            memcpy(tmp, px, (size_t)W * H * 4);
            for (int yy = y0; yy < y0 + hh && yy < H; yy++) {
                unsigned char *dp = px + (size_t)yy * W * 4;
                unsigned char *sp = tmp + (size_t)yy * W * 4;
                for (int x = 0; x < W; x++) {
                    int sx = x - off; if (sx < 0) sx = 0; else if (sx >= W) sx = W - 1;
                    dp[x * 4] = sp[sx * 4]; dp[x * 4 + 1] = sp[sx * 4 + 1]; dp[x * 4 + 2] = sp[sx * 4 + 2];
                }
            }
        }
        /* 噪点块 */
        for (int i = 0; i < 6 + str * 4 / 100; i++) {
            int x0 = rand() % W, y0 = rand() % H;
            int bw = 4 + rand() % 24, bh = 1 + rand() % 4;
            for (int yy = y0; yy < y0 + bh && yy < H; yy++)
                for (int xx = x0; xx < x0 + bw && xx < W; xx++) {
                    unsigned char *d = px + ((size_t)yy * W + xx) * 4;
                    int n = rand() % 256;
                    d[0] = d[1] = d[2] = (unsigned char)n;
                }
        }
    }

    /* --- 逐像素颜色特效(单 pass 组合) --- */
    for (int y = 0; y < H; y++) {
        unsigned char *p = px + (size_t)y * W * 4;
        for (int x = 0; x < W; x++) {
            int b = p[0], g = p[1], r = p[2];
            if (f & FX_GRAY) {
                int l = (r * 299 + g * 587 + b * 114) / 1000;
                r = g = b = l;
            }
            if (f & FX_INVERT) { r = 255 - r; g = 255 - g; b = 255 - b; }
            if (f & FX_NIGHT) {
                int l = (r + g + b) / 3;
                r = l * 2 / 10;
                g = l * 9 / 10 + 40; if (g > 255) g = 255;
                b = l * 2 / 10;
            }
            if (f & FX_FILM) {
                int tr = (int)(r * 0.393 + g * 0.769 + b * 0.189);
                int tg = (int)(r * 0.349 + g * 0.686 + b * 0.168);
                int tb = (int)(r * 0.272 + g * 0.534 + b * 0.131);
                int nz = (rand() % (str + 1)) - str / 2;
                r = tr + nz; g = tg + nz; b = tb + nz;
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;
            }
            if (f & FX_MEMORY) {
                int k = 30 + str * 50 / 100;
                r = r + (255 - r) * k / 100;
                g = g + (255 - g) * k / 100;
                b = b + (255 - b) * (k + 10) / 100;
            }
            if (f & FX_HAZE) {
                int k = 25 + str * 55 / 100;
                r = r + (210 - r) * k / 100;
                g = g + (210 - g) * k / 100;
                b = b + (210 - b) * k / 100;
            }
            if (f & FX_VIGNETTE) {
                double dx = (x - W / 2.0) / (W / 2.0);
                double dy = (y - H / 2.0) / (H / 2.0);
                double d = dx * dx + dy * dy;
                double k = 1.0 - 0.55 * d * str / 100.0;
                if (k < 0.25) k = 0.25;
                r = (int)(r * k); g = (int)(g * k); b = (int)(b * k);
            }
            if (f & FX_SCANLINE) {
                if ((y & 1) == 0) { r = r * 82 / 100; g = g * 82 / 100; b = b * 82 / 100; }
            }
            if (f & FX_CRT) {
                /* 密集扫描线 + 缓慢滚动闪烁 */
                double scan = 0.92 + 0.08 * sin(y * 0.5 + G.fxTick * 0.15);
                if ((y & 1) == 0) scan *= 0.82;
                /* 圆角边缘暗角(屏幕弧面) */
                double nx = (x - W / 2.0) / (W / 2.0);
                double ny = (y - H / 2.0) / (H / 2.0);
                double d = nx * nx + ny * ny;
                double vig = 1.0 - 0.5 * d;
                if (vig < 0.3) vig = 0.3;
                /* 轻微雪花噪点 */
                int nz = (rand() % 24) - 12;
                /* 轻微色差(红偏暖) */
                r = (int)(r * scan * vig * 1.03) + nz;
                g = (int)(g * scan * vig) + nz / 2;
                b = (int)(b * scan * vig * 0.94);
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;
            }
            p[0] = (unsigned char)b; p[1] = (unsigned char)g; p[2] = (unsigned char)r;
            p += 4;
        }
    }

    /* 老电影上下黑边 */
    if (f & FX_FILM) {
        int band = H * 10 / 100;
        memset(px, 0, (size_t)band * W * 4);
        memset(px + (size_t)(H - band) * W * 4, 0, (size_t)band * W * 4);
    }
    free(tmp);
}

/* 半透明矩形(菜单面板用) */
static void draw_alpha_rect(HDC dc, int x, int y, int w, int h, int color, int a) {
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC tdc = CreateCompatibleDC(NULL);
    HBITMAP tbmp = CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP tob = (HBITMAP)SelectObject(tdc, tbmp);
    unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < w * h; i++) {
        p[i * 4 + 0] = color & 255; p[i * 4 + 1] = (color >> 8) & 255;
        p[i * 4 + 2] = (color >> 16) & 255; p[i * 4 + 3] = (unsigned char)a;
    }
    blend(dc, x, y, w, h, tdc, w, h);
    SelectObject(tdc, tob); DeleteObject(tbmp); DeleteDC(tdc);
}

/* ---------------- 对话框形状/装饰/特效(主题区分) ---------------- */
static int box_colr(int c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
static int box_mix(int a, int b, int t) {  /* a→b,t 0..255 */
    int r = (((a >> 16) & 255) * (255 - t) + ((b >> 16) & 255) * t) / 255;
    int g = (((a >> 8) & 255) * (255 - t) + ((b >> 8) & 255) * t) / 255;
    int bl = ((a & 255) * (255 - t) + (b & 255) * t) / 255;
    return (r << 16) | (g << 8) | bl;
}

/* 判断点是否在对话框形状内(0圆角 1方角 2切角 3上圆下直 4双线框) */
static int in_box_shape(int x, int y, int w, int h, int shape, int r) {
    if (shape == 2) {                     /* 底部两角斜切 */
        int c = 20;
        if (x < c && y > h - c && (c - x) < (y - (h - c))) return 0;
        if (x > w - c && y > h - c && (x - (w - c)) < (y - (h - c))) return 0;
        return 1;
    }
    if (shape == 3) {                     /* 上圆下直 */
        if (y < r) {
            if (x < r && (x - r) * (x - r) + (y - r) * (y - r) > r * r) return 0;
            if (x > w - r && (x - (w - r)) * (x - (w - r)) + (y - r) * (y - r) > r * r) return 0;
        }
        return 1;
    }
    if (shape == 0 && r > 0) {            /* 四角圆角 */
        if (x < r && y < r && (x - r) * (x - r) + (y - r) * (y - r) > r * r) return 0;
        if (x > w - r && y < r && (x - (w - r)) * (x - (w - r)) + (y - r) * (y - r) > r * r) return 0;
        if (x < r && y > h - r && (x - r) * (x - r) + (y - (h - r)) * (y - (h - r)) > r * r) return 0;
        if (x > w - r && y > h - r && (x - (w - r)) * (x - (w - r)) + (y - (h - r)) * (y - (h - r)) > r * r) return 0;
    }
    return 1;
}

/* 樱花简笔画:5 片椭圆花瓣 + 花心,半透明淡色背景。
   主体位于对话框内偏右上,仅右上角小部分溢出框外(被裁剪掉,不显示在屏幕上)。
   直径约占对话框宽 42%(五分之二)。 */
static void draw_sakura_bg(HDC dc, RECT d) {
    int W = d.right - d.left;
    int R = W * 42 / 200;                            /* 半径 ≈ 宽 21% → 直径 ≈ 42% */
    if (R < 40) R = 40;
    int cx = d.right - R * 55 / 100;                 /* 中心在框内偏右上 */
    int cy = d.top + R * 60 / 100;
    double pd = 0.52 * R;                            /* 花瓣中心距 */
    double pa = 0.42 * R;                            /* 花瓣半长(径向) */
    double pb = 0.27 * R;                            /* 花瓣半宽 */
    double rc = 0.13 * R;                            /* 花心半径 */
    int alpha = 72;                                  /* 淡色半透明 */
    int petal = 0xFFB9D6, core = 0xFFC9A8;
    int sz = R * 2;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz; bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC tdc = CreateCompatibleDC(NULL);
    HBITMAP tbmp = CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP tob = (HBITMAP)SelectObject(tdc, tbmp);
    memset(bits, 0, (size_t)sz * sz * 4);
    for (int py = 0; py < sz; py++) {
        int dy = py - R;
        unsigned char *row = (unsigned char *)bits + (size_t)py * sz * 4;
        for (int px = 0; px < sz; px++) {
            int dx = px - R;
            double rr = (double)dx * dx + (double)dy * dy;
            int in = (rr <= rc * rc);
            if (!in) {
                for (int k = 0; k < 5 && !in; k++) {
                    double th = k * 1.256637061;
                    double c = cos(th), s = sin(th);
                    double pcx = pd * c, pcy = pd * s;
                    double vx = dx - pcx, vy = dy - pcy;
                    double rx = vx * c + vy * s, ry = -vx * s + vy * c;
                    if ((rx * rx) / (pa * pa) + (ry * ry) / (pb * pb) <= 1.0) in = 1;
                }
            }
            if (in) {
                int col = (rr <= rc * rc) ? core : petal;
                row[px * 4 + 0] = (unsigned char)(col & 255);
                row[px * 4 + 1] = (unsigned char)((col >> 8) & 255);
                row[px * 4 + 2] = (unsigned char)((col >> 16) & 255);
                row[px * 4 + 3] = (unsigned char)alpha;
            }
        }
    }
    /* 手动裁剪到对话框矩形内(blend 系是 CPU 直写 memBits,GDI clip 无效) */
    blend_a_clip(dc, cx - R, cy - R, sz, sz, tdc, sz, sz, 255,
                 d.left, d.top, d.right, d.bottom);
    SelectObject(tdc, tob); DeleteObject(tbmp); DeleteDC(tdc);
}

/* 对话框额外特效:2底部光带 3四角弧 4细线 */
static void draw_box_fx(HDC dc, RECT d) {
    int bc = g_theme.boxBorder;
    if (!g_theme.boxBorder) return;
    if (g_theme.boxFx == 2) {
        draw_alpha_rect(dc, d.left + 6, d.bottom - 3, d.right - d.left - 12, 2, bc, 95);
        draw_alpha_rect(dc, d.left + 16, d.bottom - 6, d.right - d.left - 32, 2, bc, 45);
    } else if (g_theme.boxFx == 3) {
        HPEN pn = CreatePen(PS_SOLID, 3, box_colr(bc));
        HGDIOBJ op = SelectObject(dc, pn);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        int a = 26, off = 8;
        Arc(dc, d.left - off, d.top - off, d.left + a, d.top + a,
            d.left + a, d.top, d.left, d.top + a);
        Arc(dc, d.right - a - off, d.top - off, d.right + off, d.top + a,
            d.right, d.top + a, d.right - a, d.top);
        Arc(dc, d.left - off, d.bottom - a + off, d.left + a, d.bottom + off,
            d.left, d.bottom - a, d.left + a, d.bottom);
        Arc(dc, d.right - a - off, d.bottom - a + off, d.right + off, d.bottom + off,
            d.right - a, d.bottom, d.right, d.bottom - a);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pn);
    } else if (g_theme.boxFx == 4) {
        HPEN pn = CreatePen(PS_SOLID, 1, box_colr(bc));
        HGDIOBJ op = SelectObject(dc, pn);
        MoveToEx(dc, d.left + 8, d.top, NULL); LineTo(dc, d.right - 8, d.top);
        MoveToEx(dc, d.left + 8, d.bottom - 1, NULL); LineTo(dc, d.right - 8, d.bottom - 1);
        SelectObject(dc, op); DeleteObject(pn);
    }
}

/* 对话框装饰图案:1樱花 2星芒 3藤叶 4光点 5三角 */
static void draw_box_decor(HDC dc, RECT d) {
    int bc = g_theme.boxBorder;
    if (!g_theme.boxBorder) return;
    if (g_theme.boxDecor == 1) {          /* 樱花:右上角半透明背景图案 */
        draw_sakura_bg(dc, d);
    } else if (g_theme.boxDecor == 2) {   /* 星芒(四角) */
        HPEN pn = CreatePen(PS_SOLID, 1, box_colr(bc));
        HGDIOBJ op = SelectObject(dc, pn);
        HBRUSH br = CreateSolidBrush(box_colr(bc));
        HGDIOBJ ob = SelectObject(dc, br);
        int off = 14, r0 = 3, r1 = 7;
        POINT c[4] = {{d.left + off, d.top + off}, {d.right - off, d.top + off},
                      {d.left + off, d.bottom - off}, {d.right - off, d.bottom - off}};
        for (int k = 0; k < 4; k++) {
            int sx = c[k].x, sy = c[k].y;
            POINT p[8] = {{sx, sy - r1}, {sx + r0, sy - r0}, {sx + r1, sy},
                          {sx + r0, sy + r0}, {sx, sy + r1}, {sx - r0, sy + r0},
                          {sx - r1, sy}, {sx - r0, sy - r0}};
            Polygon(dc, p, 8);
        }
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pn);
    } else if (g_theme.boxDecor == 3) {   /* 藤叶(左下) */
        HPEN pn = CreatePen(PS_SOLID, 1, box_colr(bc));
        HGDIOBJ op = SelectObject(dc, pn);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        int cx = d.left + 24, cy = d.bottom - 24;
        Arc(dc, cx - 14, cy - 14, cx + 6, cy + 6, cx - 2, cy + 6, cx - 14, cy - 8);
        Arc(dc, cx - 6, cy - 20, cx + 14, cy, cx + 14, cy - 12, cx + 4, cy + 0);
        Ellipse(dc, cx - 8, cy - 8, cx - 2, cy - 2);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pn);
    } else if (g_theme.boxDecor == 4) {   /* 光点(四角) */
        HPEN pn = CreatePen(PS_SOLID, 1, box_colr(bc));
        HGDIOBJ op = SelectObject(dc, pn);
        HBRUSH br = CreateSolidBrush(box_colr(bc));
        HGDIOBJ ob = SelectObject(dc, br);
        int off = 10;
        Ellipse(dc, d.left + off - 3, d.top + off - 3, d.left + off + 3, d.top + off + 3);
        Ellipse(dc, d.right - off - 3, d.top + off - 3, d.right - off + 3, d.top + off + 3);
        Ellipse(dc, d.left + off - 3, d.bottom - off - 3, d.left + off + 3, d.bottom - off + 3);
        Ellipse(dc, d.right - off - 3, d.bottom - off - 3, d.right - off + 3, d.bottom - off + 3);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pn);
    } else if (g_theme.boxDecor == 5) {   /* 三角(顶部) */
        HPEN pn = CreatePen(PS_SOLID, 1, box_colr(bc));
        HGDIOBJ op = SelectObject(dc, pn);
        HBRUSH br = CreateSolidBrush(box_colr(bc));
        HGDIOBJ ob = SelectObject(dc, br);
        int cx = (d.left + d.right) / 2, ty = d.top + 8;
        POINT p[3] = {{cx, ty}, {cx - 7, ty + 8}, {cx + 7, ty + 8}};
        Polygon(dc, p, 3);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pn);
    }
}

/* 悬停"继续"三角指示器:右上角小三角 + 淡发光(参考 JASONBASEENG 的悬停高亮动画) */
static void draw_box_hover_tri(HDC dc, RECT d) {
    if (!G.boxHover || G.boxHoverT < 0.03) return;
    double t = G.boxHoverT;
    int bc = g_theme.boxBorder ? g_theme.boxBorder : 0xFFFFFF;
    int cx = d.right - 16, cy = d.top + 15;   /* 对话框右上角内侧 */
    int s = 4 + (int)(2 * t);                 /* 小小的三角 */
    for (int i = 2; i >= 1; i--) {            /* 淡发光(两层,小) */
        int gs = s + i * 3;
        int dim = 255 - i * 75; if (dim < 70) dim = 70;
        int col = box_mix(0x000000, bc, dim);
        HPEN pn = CreatePen(PS_SOLID, 1, box_colr(col));
        HGDIOBJ op = SelectObject(dc, pn);
        HBRUSH br = CreateSolidBrush(box_colr(col));
        HGDIOBJ ob = SelectObject(dc, br);
        POINT p[3] = {{cx - gs, cy + gs}, {cx + gs, cy + gs}, {cx, cy - gs}};
        Polygon(dc, p, 3);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(br); DeleteObject(pn);
    }
    HPEN pn = CreatePen(PS_SOLID, 1, box_colr(bc));
    HGDIOBJ op = SelectObject(dc, pn);
    HBRUSH br = CreateSolidBrush(box_colr(bc));
    HGDIOBJ ob = SelectObject(dc, br);
    POINT p[3] = {{cx - s, cy + s}, {cx + s, cy + s}, {cx, cy - s}};
    Polygon(dc, p, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pn);
}

/* 渲染菜单控件(独占画面或叠加层) */
static void draw_menu_ctrls(HDC dc) {
    for (int i = 0; i < G.menuCtrlCount; i++) {
        MenuCtrl *c = &G.menuCtrl[i];
        if (!c->show) continue;
        if (c->type == 2 && c->img) {           /* 图片(按适配模式,相对控件矩形) */
            HDC sdc = CreateCompatibleDC(NULL);
            HBITMAP ob = (HBITMAP)SelectObject(sdc, c->img);
            int dw = c->w, dh = c->h, dx = c->x, dy = c->y;
            if (c->imgW > 0 && c->imgH > 0) {
                if (c->fit == 1) { dw = c->imgW; dh = c->imgH; }
                else if (c->fit == 2 || c->fit == 3) {
                    double s = (c->fit == 2)
                        ? fmin((double)c->w / c->imgW, (double)c->h / c->imgH)
                        : fmax((double)c->w / c->imgW, (double)c->h / c->imgH);
                    dw = (int)(c->imgW * s); dh = (int)(c->imgH * s);
                }
                dx = c->x + (c->w - dw) / 2;
                dy = c->y + (c->h - dh) / 2;
            }
            blend(dc, dx, dy, dw, dh, sdc, c->imgW, c->imgH);
            SelectObject(sdc, ob); DeleteDC(sdc);
            continue;
        }
        if (c->type == 3) {                      /* 面板:半透明 */
            draw_alpha_rect(dc, c->x, c->y, c->w, c->h, c->bgColor, 170);
        } else if (c->type == 0) {               /* 按钮:圆角矩形(主题色 + 悬停) */
            int hover = (G.mouseX >= c->x && G.mouseX <= c->x + c->w &&
                         G.mouseY >= c->y && G.mouseY <= c->y + c->h);
            int fill = hover ? g_theme.btnHover : c->bgColor;
            int bc = g_theme.btnBorder;
            HPEN pen = CreatePen(PS_SOLID, hover ? 3 : 2,
                                 RGB((bc >> 16) & 255, (bc >> 8) & 255, bc & 255));
            HGDIOBJ op = SelectObject(dc, pen);
            HBRUSH br = CreateSolidBrush(RGB((fill >> 16) & 255, (fill >> 8) & 255, fill & 255));
            HGDIOBJ ob = SelectObject(dc, br);
            RoundRect(dc, c->x, c->y, c->x + c->w, c->y + c->h, g_theme.btnRound, g_theme.btnRound);
            SelectObject(dc, ob); DeleteObject(br);
            SelectObject(dc, op); DeleteObject(pen);
        } else if (c->type == 4) {               /* 输入框:矩形+描边 */
            int bc = g_theme.btnBorder;
            HPEN pen = CreatePen(PS_SOLID, 2, RGB((bc >> 16) & 255, (bc >> 8) & 255, bc & 255));
            HGDIOBJ op = SelectObject(dc, pen);
            HBRUSH br = CreateSolidBrush(RGB(20, 22, 34));
            HGDIOBJ ob = SelectObject(dc, br);
            Rectangle(dc, c->x, c->y, c->x + c->w, c->y + c->h);
            SelectObject(dc, ob); DeleteObject(br);
            SelectObject(dc, op); DeleteObject(pen);
        }
        /* 控件文字 */
        if (c->text[0]) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB((g_theme.ctrlText >> 16) & 255,
                                 (g_theme.ctrlText >> 8) & 255,
                                 g_theme.ctrlText & 255));
            HFONT of = (HFONT)SelectObject(dc, G.font);
            wchar_t wt[MAX_CTRL_TEXT];
            wpath(c->text, wt, MAX_CTRL_TEXT);
            if (c->type == 4) {
                /* 输入框:左对齐,文字超宽自动滚动到显示尾部,光标精确跟随末尾 */
                int len = (int)wcslen(wt);
                SIZE sz;
                GetTextExtentPoint32W(dc, wt, len, &sz);
                int avail = c->w - 20;
                int off = (sz.cx > avail) ? (sz.cx - avail) : 0;   /* 左移量:露出尾部 */
                int ty = c->y + (c->h - sz.cy) / 2;                 /* 垂直居中 */
                int tx = c->x + 10 - off;
                /* 裁剪到输入框矩形,防止文字/光标从左边溢出 */
                int saved = SaveDC(dc);
                IntersectClipRect(dc, c->x, c->y, c->x + c->w, c->y + c->h);
                TextOutW(dc, tx, ty, wt, len);
                if (i == G.menuFocus && (GetTickCount() / 400) % 2 == 0) {
                    int cxx = tx + sz.cx;                           /* 光标在文字末尾 */
                    HPEN pen = CreatePen(PS_SOLID, 2,
                                         RGB((g_theme.ctrlText >> 16) & 255,
                                             (g_theme.ctrlText >> 8) & 255,
                                             g_theme.ctrlText & 255));
                    HGDIOBJ op = SelectObject(dc, pen);
                    MoveToEx(dc, cxx, c->y + 6, NULL);
                    LineTo(dc, cxx, c->y + c->h - 6);
                    SelectObject(dc, op);
                    DeleteObject(pen);
                }
                RestoreDC(dc, saved);
            } else {
                RECT tr = {c->x + 10, c->y + 4, c->x + c->w - 10, c->y + c->h - 4};
                if (c->type == 1)                /* 文字:左对齐顶部,可换行 */
                    DrawTextW(dc, wt, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK);
                else                             /* 按钮/面板:居中 */
                    DrawTextW(dc, wt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(dc, of);
        }
    }
}

/* ---------- 鼠标拖尾 ---------- */
static void update_trail(void) {
    int len = (g_theme.trail && G.trailOn) ? g_theme.trailLen : 0;
    if (len <= 0) { G.trailN = 0; return; }
    if (G.trailN > 0 && G.trailX[G.trailN - 1] == G.mouseX && G.trailY[G.trailN - 1] == G.mouseY)
        return;
    if (G.trailN >= 64) {
        memmove(G.trailX, G.trailX + 1, (64 - 1) * sizeof(int));
        memmove(G.trailY, G.trailY + 1, (64 - 1) * sizeof(int));
        G.trailN--;
    }
    G.trailX[G.trailN] = G.mouseX;
    G.trailY[G.trailN] = G.mouseY;
    G.trailN++;
    if (G.trailN > len) {
        memmove(G.trailX, G.trailX + 1, (64 - 1) * sizeof(int));
        memmove(G.trailY, G.trailY + 1, (64 - 1) * sizeof(int));
        G.trailN--;
    }
}

static void draw_trail(HDC dc) {
    if (!g_theme.trail || !G.trailOn || G.trailN < 2) return;
    for (int k = 0; k < G.trailN; k++) {
        int idx = G.trailN - 1 - k;   /* k=0 最新 */
        int t = k * 255 / G.trailN;
        int r = (k < 2) ? 3 : 2;
        if (k > G.trailN * 2 / 3) r = 1;
        int c = g_theme.trailColor;
        int cr = ((c >> 16) & 255) * (255 - t) / 255;
        int cg = ((c >> 8) & 255) * (255 - t) / 255;
        int cb = (c & 255) * (255 - t) / 255;
        HBRUSH br = CreateSolidBrush(RGB(cr, cg, cb));
        HPEN pn = CreatePen(PS_SOLID, 1, RGB(cr, cg, cb));
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, pn);
        Ellipse(dc, G.trailX[idx] - r, G.trailY[idx] - r, G.trailX[idx] + r, G.trailY[idx] + r);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(br); DeleteObject(pn);
    }
}

static double now_ms(void) { return (double)GetTickCount(); }

/* 镜头淡入淡出:在帧上叠加全屏黑色(alpha 按过渡进度) */
static void render_fade(HDC dc) {
    (void)dc;
    if (!G.fadePhase || !G.memBits) return;
    double p = (now_ms() - G.fadeT0) / (G.fadeDur > 0 ? G.fadeDur : 1);
    if (p >= 1) p = 1;
    int a = (G.fadePhase == 1) ? (int)(p * 255) : (int)((1 - p) * 255);
    if (a <= 0) return;
    int ia = 255 - a;
    unsigned char *dpx = (unsigned char *)G.memBits;
    for (int y = 0; y < G.h; y++) {
        unsigned char *d = dpx + (size_t)y * G.w * 4;
        for (int x = 0; x < G.w; x++) {
            d[0] = (unsigned char)((d[0] * ia) / 255);
            d[1] = (unsigned char)((d[1] * ia) / 255);
            d[2] = (unsigned char)((d[2] * ia) / 255);
            d += 4;
        }
    }
}

/* ---------- 快速消息提示(Toast) ---------- */
#define MAX_TOASTS 8
typedef struct { wchar_t text[160]; int corner; double t0, dur; } GalToast;
static GalToast g_toasts[MAX_TOASTS];
static int g_toastCount = 0;

/* 角落提示:corner 0左上 1右上 2左下 3右下;自动淡入淡出后消失 */
EXPORT int64_t gal_toast(const char *text, int64_t corner, int64_t ms) {
    wchar_t wt[160];
    wpath(text, wt, 160);
    double now = now_ms();
    /* 清理过期 */
    int k = 0;
    for (int i = 0; i < g_toastCount; i++)
        if (now - g_toasts[i].t0 < g_toasts[i].dur)
            g_toasts[k++] = g_toasts[i];
    g_toastCount = k;
    /* 满则移除最旧 */
    if (g_toastCount >= MAX_TOASTS) {
        memmove(&g_toasts[0], &g_toasts[1],
                (size_t)(g_toastCount - 1) * sizeof(GalToast));
        g_toastCount--;
    }
    GalToast *t = &g_toasts[g_toastCount++];
    wcsncpy(t->text, wt, 159); t->text[159] = 0;
    t->corner = (int)(corner & 3);
    t->t0 = now;
    t->dur = (ms > 0) ? (double)ms : 1500.0;
    return 0;
}

static void render_toast(HDC dc) {
    if (g_toastCount == 0) return;
    for (int i = 0; i < g_toastCount; i++) {
        GalToast *t = &g_toasts[i];
        double age = now_ms() - t->t0;
        if (age >= t->dur) continue;
        int alpha;
        if (age < 200) alpha = (int)(255 * age / 200);          /* 淡入 */
        else if (age >= t->dur - 300) alpha = (int)(255 * (t->dur - age) / 300); /* 淡出 */
        else alpha = 255;
        if (alpha <= 0) continue;
        HDC mdc = CreateCompatibleDC(NULL);
        HFONT mf = (HFONT)SelectObject(mdc, G.font);
        SIZE sz; GetTextExtentPoint32W(mdc, t->text, (int)wcslen(t->text), &sz);
        SelectObject(mdc, mf); DeleteDC(mdc);
        int tw = sz.cx + 28, th = sz.cy + 16;
        if (tw < 100) tw = 100;
        int px, py, off = i * (th + 8);
        switch (t->corner) {
        case 0: px = 12;          py = 12 + off; break;                 /* 左上 */
        case 1: px = G.w - 12 - tw; py = 12 + off; break;               /* 右上 */
        case 2: px = 12;          py = G.h - 12 - th - off; break;      /* 左下 */
        default: px = G.w - 12 - tw; py = G.h - 12 - th - off; break;   /* 右下 */
        }
        /* 深色半透明背景 + 白字,整体按 alpha 合成 */
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HDC tdc = CreateCompatibleDC(NULL);
        HBITMAP tbmp = CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HBITMAP tob = (HBITMAP)SelectObject(tdc, tbmp);
        memset(bits, 0, (size_t)tw * th * 4);
        for (int yy = 0; yy < th; yy++) {
            unsigned char *row = (unsigned char *)bits + (size_t)yy * tw * 4;
            for (int xx = 0; xx < tw; xx++) {
                row[xx * 4 + 0] = 46; row[xx * 4 + 1] = 52; row[xx * 4 + 2] = 70;
                row[xx * 4 + 3] = 200;
            }
        }
        SetBkMode(tdc, TRANSPARENT);
        SetTextColor(tdc, RGB(255, 255, 255));
        HFONT tf = (HFONT)SelectObject(tdc, G.font);
        TextOutW(tdc, 14, 8, t->text, (int)wcslen(t->text));
        SelectObject(tdc, tf);
        blend_a(dc, px, py, tw, th, tdc, tw, th, alpha);
        SelectObject(tdc, tob); DeleteObject(tbmp); DeleteDC(tdc);
    }
}

static void render_frame(void) {
    HDC dc = G.memDC;
    RECT rc = {0, 0, G.w, G.h};
    update_trail();    /* 独占菜单:独立画面,不渲染镜头内容 */
    if (G.menuMode == 1 && G.menuActive) {
        if (G.menuBg) {
            HDC bdc = CreateCompatibleDC(NULL);
            HBITMAP ob = (HBITMAP)SelectObject(bdc, G.menuBg);
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, 0, 0, G.w, G.h, bdc, 0, 0, G.menuBgW, G.menuBgH, SRCCOPY);
            SelectObject(bdc, ob); DeleteDC(bdc);
        } else {
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        draw_menu_ctrls(dc);
        draw_trail(dc);
        apply_fx();
        HDC wdc = GetDC(G.hwnd);
        BitBlt(wdc, 0, 0, G.w, G.h, dc, 0, 0, SRCCOPY);
        ReleaseDC(G.hwnd, wdc);
        return;
    }
    /* 背景:先填底色,再按适配模式用 alpha 混合背景图(透明区域透出底色而非纯黑) */
    {
        int c = (G.bgColor < 0) ? RGB(0, 0, 0) : RGB(G.bgColor >> 16, (G.bgColor >> 8) & 255, G.bgColor & 255);
        HBRUSH br = CreateSolidBrush(c);
        FillRect(dc, &rc, br);
        DeleteObject(br);
    }
    if (G.hasBg && G.bgBmp) {
        HDC bdc = CreateCompatibleDC(NULL);
        HBITMAP ob = (HBITMAP)SelectObject(bdc, G.bgBmp);
        int dx = 0, dy = 0, dw = G.w, dh = G.h;
        if (G.bgW > 0 && G.bgH > 0) {
            if (G.bgFit == 1) {                 /* 原大小:居中 */
                dw = G.bgW; dh = G.bgH;
            } else if (G.bgFit == 2 || G.bgFit == 3) {
                double s = (G.bgFit == 2)
                    ? fmin((double)G.w / G.bgW, (double)G.h / G.bgH)   /* 完整显示,留边 */
                    : fmax((double)G.w / G.bgW, (double)G.h / G.bgH);  /* 填满,裁剪 */
                dw = (int)(G.bgW * s); dh = (int)(G.bgH * s);
            }
            dx = (G.w - dw) / 2; dy = (G.h - dh) / 2;
        }
        blend(dc, dx, dy, dw, dh, bdc, G.bgW, G.bgH);   /* alpha 混合:透明区域透出底色 */
        SelectObject(bdc, ob);
        DeleteDC(bdc);
    }
    /* 背景交叉淡化(柔和换背景):新旧背景按进度混合 */
    if (G.bgTransOn && G.bgTo) {
        double p = (now_ms() - G.bgTransT0) / (G.bgTransDur > 0 ? G.bgTransDur : 1);
        if (p >= 1) {
            if (G.bgBmp) DeleteObject(G.bgBmp);
            G.bgBmp = G.bgTo; G.bgW = G.bgToW; G.bgH = G.bgToH;
            G.bgTo = NULL; G.bgTransOn = 0;
        } else {
            HDC bdc = CreateCompatibleDC(NULL);
            HBITMAP ob = (HBITMAP)SelectObject(bdc, G.bgTo);
            int dx = 0, dy = 0, dw = G.w, dh = G.h;
            if (G.bgToW > 0 && G.bgToH > 0) {
                if (G.bgFit == 1) { dw = G.bgToW; dh = G.bgToH; }
                else if (G.bgFit == 2 || G.bgFit == 3) {
                    double s = (G.bgFit == 2)
                        ? fmin((double)G.w / G.bgToW, (double)G.h / G.bgToH)
                        : fmax((double)G.w / G.bgToW, (double)G.h / G.bgToH);
                    dw = (int)(G.bgToW * s); dh = (int)(G.bgToH * s);
                }
                dx = (G.w - dw) / 2; dy = (G.h - dh) / 2;
            }
            blend_a(dc, dx, dy, dw, dh, bdc, G.bgToW, G.bgToH, (int)(p * 255));
            SelectObject(bdc, ob);
            DeleteDC(bdc);
        }
    }
    /* 立绘(按图层序,底部对齐;支持位置模式/动画/alpha) */
    for (int i = 0; i < MAX_SPR; i++) {
        if (!G.sprShow[i] || !G.sprBmp[i]) continue;
        int sw = G.sprW[i], sh = G.sprH[i];
        HDC sdc = G.sprDC[i];
        if (G.sprAutofit[i] && G.sprTrimBmp[i]) { sw = G.sprTrimW[i]; sh = G.sprTrimH[i]; sdc = G.sprTrimDC[i]; }
        /* 动画 */
        int tick = G.sprAnimTick[i];
        double fscale = G.sprScale[i];
        int alpha = 255; int ox = 0, oy = 0;
        switch (G.sprAnim[i]) {
        case 1: /* 呼吸:微微缩放 */
            fscale *= 1.0 + 0.03 * sin(tick / 300.0 * 6.2832);
            break;
        case 2: /* 淡入:alpha 0→255 */
            alpha = tick * 8; if (alpha > 255) alpha = 255;
            break;
        case 3: /* 上浮:从下方上移 + 淡入 */
            alpha = tick * 8; if (alpha > 255) alpha = 255;
            oy = (255 - alpha) * 80 / 255;
            break;
        case 4: /* 抖动:水平小幅抖动 */
            ox = (int)(sin(tick / 40.0) * 6);
            break;
        case 5: /* 脉冲:alpha 波动 */
            alpha = 180 + (int)(75 * sin(tick / 150.0));
            break;
        case 6: /* 消失:alpha → 0 后隐藏 */
            alpha = 255 - tick * 8;
            if (alpha < 0) { alpha = 0; G.sprShow[i] = 0; }
            break;
        default: break;
        }
        /* 叠加 gal_sprite_alpha() 设定的基础透明度:动画 alpha 作为相对系数,
           因此「基础 128 + 淡入动画」会淡入到半透明,而不是忽略基础值。 */
        alpha = alpha * G.sprAlpha[i] / 255;
        if (alpha <= 0) continue;
        int dw, dh;
        if (G.sprFit[i] == 1) {                 /* 自动适配:裁剪透明 + 屏高78% */
            if (!G.sprTrimBmp[i]) spr_autofit(i);
            if (G.sprTrimBmp[i]) { sw = G.sprTrimW[i]; sh = G.sprTrimH[i]; sdc = G.sprTrimDC[i]; }
            dw = (int)(sw * fscale / 100.0); dh = (int)(sh * fscale / 100.0);
        } else if (G.sprFit[i] == 2 || G.sprFit[i] == 3) {
            double s = (G.sprFit[i] == 2)
                ? fmin(G.w * 0.95 / (double)sw, G.h * 0.85 / (double)sh)   /* 完整显示 */
                : fmax(G.w / (double)sw, G.h / (double)sh);                 /* 填满 */
            s *= fscale / 100.0;
            dw = (int)(sw * s); dh = (int)(sh * s);
        } else {
            dw = (int)(sw * fscale / 100.0); dh = (int)(sh * fscale / 100.0);
        }
        if (dw <= 0 || dh <= 0) continue;
        int dx = G.sprX[i] + ox;
        int dy = (G.sprY[i] >= 0) ? G.sprY[i] + oy : G.h - dh + oy;
        /* fit 模式按实际渲染尺寸重算水平位置(非手动) */
        if (G.sprFit[i] != 0 && G.sprPosMode[i] >= 0) {
            switch (G.sprPosMode[i]) {
            case 0: dx = (G.w - dw) / 2; break;
            case 1: dx = 0; break;
            case 2: dx = G.w - dw; break;
            case 3: dx = G.w / 2 - dw - 60; break;
            case 4: dx = G.w / 2 + 60; break;
            default: break;
            }
            dy = G.h - dh + oy;
        }
        if (sdc) blend_a(dc, dx, dy, dw, dh, sdc, sw, sh, alpha);
    }
    /* 对话框(底部半透明) */
    if (G.boxOn) {
        int dlgL = 10, dlgR = G.w - 10;
        if (G.boxAutoFit) {
            int tw = 0;
            HDC mdc = CreateCompatibleDC(NULL);
            HFONT mf = (HFONT)SelectObject(mdc, G.font);
            SIZE sz; GetTextExtentPoint32W(mdc, G.curText, (int)wcslen(G.curText), &sz);
            tw = sz.cx + 90;
            SelectObject(mdc, mf); DeleteDC(mdc);
            int minw = G.w * 45 / 100;
            if (tw < minw) tw = minw;
            if (tw > G.w - 20) tw = G.w - 20;
            dlgL = (G.w - tw) / 2; dlgR = dlgL + tw;
        }
        /* 文本区域(与下方绘制一致),用于高度自适应 */
        int textL0 = dlgL + 18;
        if (G.boxAvatar) textL0 = dlgL + 120;
        int txA = textL0, txB = dlgR - 18;
        UINT alignA = DT_LEFT;
        switch (G.textPos) {
        case 0: alignA = DT_CENTER; break;                                                 /* 中间 */
        case 1: alignA = DT_LEFT; break;                                                   /* 左 */
        case 2: alignA = DT_RIGHT; break;                                                  /* 右 */
        case 3: alignA = DT_LEFT; txB = dlgL + (dlgR - dlgL) * 3 / 4; break;               /* 偏左 */
        case 4: alignA = DT_RIGHT; txA = dlgL + (dlgR - dlgL) / 4; break;                  /* 偏右 */
        default: break;
        }
        /* 计算换行后完整文本所需高度 → 对话框高度自动适配(长文本不溢出) */
        int needH = 0;
        {
            HDC mdc2 = CreateCompatibleDC(NULL);
            HFONT mf2 = (HFONT)SelectObject(mdc2, G.font);
            RECT tr0 = {txA, 0, txB, 0};
            DrawTextW(mdc2, G.curText, (int)wcslen(G.curText), &tr0,
                      alignA | DT_CALCRECT | DT_WORDBREAK);
            needH = tr0.bottom;
            SelectObject(mdc2, mf2); DeleteDC(mdc2);
        }
        int bh = G.boxAvatar ? 210 : 170;
        int need = 42 + needH + 22;                    /* 顶部人名区 42 + 文本 + 底部留白(含余量) */
        if (need > bh) {
            bh = need;
            int maxh = G.h - 70;                       /* 顶部至少留 70px 给画面/立绘 */
            if (bh > maxh) bh = maxh;
        }
        RECT dlg = {dlgL, G.h - bh - 10, dlgR, G.h - 10};
        /* 半透明:画到临时 32bpp 再 AlphaBlend */
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = dlg.right - dlg.left;
        bi.bmiHeader.biHeight = -(dlg.bottom - dlg.top);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = NULL;
        HDC tdc = CreateCompatibleDC(NULL);
        HBITMAP tbmp = CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HBITMAP tob = (HBITMAP)SelectObject(tdc, tbmp);
        memset(bits, 0, (dlg.bottom - dlg.top) * (dlg.right - dlg.left) * 4);
        for (int y = 0; y < (dlg.bottom - dlg.top); y++) {
            unsigned char *row = (unsigned char *)bits + y * (dlg.right - dlg.left) * 4;
            for (int x = 0; x < (dlg.right - dlg.left); x++) {
                if (!in_box_shape(x, y, dlg.right - dlg.left, dlg.bottom - dlg.top,
                                  g_theme.boxShape, g_theme.boxRound)) {
                    continue;                       /* 形状外保持透明(异形对话框) */
                }
                int bb = g_theme.boxBg;
                row[x * 4 + 0] = (unsigned char)(bb & 255);
                row[x * 4 + 1] = (unsigned char)((bb >> 8) & 255);
                row[x * 4 + 2] = (unsigned char)((bb >> 16) & 255);
                row[x * 4 + 3] = (unsigned char)g_theme.boxAlpha;
            }
        }
        blend(dc, dlg.left, dlg.top, dlg.right - dlg.left, dlg.bottom - dlg.top, tdc, dlg.right - dlg.left, dlg.bottom - dlg.top);
        SelectObject(tdc, tob); DeleteObject(tbmp); DeleteDC(tdc);

        /* 鼠标悬停状态 + 平滑动画(驱动"继续"三角指示器) */
        int hover = (G.mouseX >= dlg.left && G.mouseX <= dlg.right &&
                     G.mouseY >= dlg.top && G.mouseY <= dlg.bottom);
        if (G.boxHover != (hover ? 1 : 0)) {
            G.boxHover = hover ? 1 : 0;
            G.boxHoverT = hover ? 0.0 : G.boxHoverT;   /* 重新进入时从头 */
        }
        G.boxHoverT += ((hover ? 1.0 : 0.0) - G.boxHoverT) * 0.22;

        /* 对话框边框(按形状)+ 悬停提亮 + 边框流光特效 */
        if (g_theme.boxBorder) {
            int bc = g_theme.boxBorder;
            if (g_theme.boxFx == 1) {                /* 流光:色相随时间摆动 */
                double ph = fmod(now_ms() / 2600.0, 1.0);
                int rr = (int)(((bc >> 16) & 255) * (0.55 + 0.45 * sin(ph * 6.283)));
                int gg = (int)(((bc >> 8) & 255) * (0.55 + 0.45 * sin(ph * 6.283 + 2.094)));
                int bb2 = (int)((bc & 255) * (0.55 + 0.45 * sin(ph * 6.283 + 4.188)));
                if (rr < 0) rr = 0; if (gg < 0) gg = 0; if (bb2 < 0) bb2 = 0;
                if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb2 > 255) bb2 = 255;
                bc = (rr << 16) | (gg << 8) | bb2;
            }
            if (hover) {                             /* 悬停提亮 */
                int r2 = ((bc >> 16) & 255) * 140 / 100; if (r2 > 255) r2 = 255;
                int g2 = ((bc >> 8) & 255) * 140 / 100; if (g2 > 255) g2 = 255;
                int b2 = (bc & 255) * 140 / 100; if (b2 > 255) b2 = 255;
                bc = (r2 << 16) | (g2 << 8) | b2;
            }
            HPEN pen = CreatePen(PS_SOLID, hover ? 3 : 2, box_colr(bc));
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            if (g_theme.boxShape == 1 || g_theme.boxShape == 4) {
                Rectangle(dc, dlg.left, dlg.top, dlg.right, dlg.bottom);
            } else if (g_theme.boxShape == 2) {
                POINT pts[4] = {{dlg.left, dlg.top}, {dlg.right, dlg.top},
                                {dlg.right - 20, dlg.bottom}, {dlg.left + 20, dlg.bottom}};
                Polygon(dc, pts, 4);
            } else if (g_theme.boxRound > 0) {
                RoundRect(dc, dlg.left, dlg.top, dlg.right, dlg.bottom,
                          g_theme.boxRound, g_theme.boxRound);
            }
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
            if (g_theme.boxShape == 4 && g_theme.boxRound > 0) {   /* 双线框:内框 */
                HPEN pen2 = CreatePen(PS_SOLID, 1, box_colr(box_mix(0, bc, 150)));
                HGDIOBJ op2 = SelectObject(dc, pen2);
                HGDIOBJ ob2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, dlg.left + 7, dlg.top + 7, dlg.right - 7, dlg.bottom - 7,
                          g_theme.boxRound, g_theme.boxRound);
                SelectObject(dc, ob2); SelectObject(dc, op2); DeleteObject(pen2);
            }
        }
        /* 额外特效(底部光带/四角弧/细线) */
        draw_box_fx(dc, dlg);
        /* 装饰图案(樱花/星芒/藤叶/光点/三角) */
        draw_box_decor(dc, dlg);
        /* 悬停"继续"三角指示器(带发光动画) */
        draw_box_hover_tri(dc, dlg);
        /* 头像(有头像模式;用立绘图层0作头像) */
        int textL = dlg.left + 18;
        if (G.boxAvatar) {
            int aw = 92, ah = 92;
            int ax = dlg.left + 16, ay = dlg.top + (bh - ah) / 2;
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(150, 160, 230));
            HGDIOBJ op = (HGDIOBJ)SelectObject(dc, pen);
            HBRUSH br = CreateSolidBrush(RGB(28, 30, 48));
            HGDIOBJ ob = (HGDIOBJ)SelectObject(dc, br);
            Ellipse(dc, ax, ay, ax + aw, ay + ah);
            SelectObject(dc, ob); DeleteObject(br);
            SelectObject(dc, op); DeleteObject(pen);
            if (G.sprBmp[0] && G.sprDC[0]) {
                int mx = G.sprW[0], mn = G.sprH[0];
                int big = mx > mn ? mx : mn;
                if (big > 0) {
                    int dw2 = aw * mx / big, dh2 = ah * mn / big;
                    blend(dc, ax + (aw - dw2) / 2, ay + (ah - dh2) / 2, dw2, dh2,
                          G.sprDC[0], G.sprW[0], G.sprH[0]);
                }
            }
            textL = dlg.left + 120;
        }
        /* 说话人(有人名模式) */
        if (G.boxName && G.speaker[0]) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB((g_theme.nameColor >> 16) & 255,
                                 (g_theme.nameColor >> 8) & 255,
                                 g_theme.nameColor & 255));
            HFONT of = (HFONT)SelectObject(dc, G.fontName);
            TextOutW(dc, textL, dlg.top + 8, G.speaker, (int)wcslen(G.speaker));
            SelectObject(dc, of);
        }
        /* 文本(文字位置) */
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB((G.textColor >> 16) & 255, (G.textColor >> 8) & 255, G.textColor & 255));
        HFONT of = (HFONT)SelectObject(dc, G.font);
        if (G.textChars > 0 && G.textActive) {
            int tx0 = textL, tx1 = dlg.right - 18;
            UINT align = DT_LEFT;
            switch (G.textPos) {
            case 0: align = DT_CENTER; break;                                                /* 中间 */
            case 1: align = DT_LEFT; break;                                                  /* 左 */
            case 2: align = DT_RIGHT; break;                                                 /* 右 */
            case 3: align = DT_LEFT; tx1 = dlg.left + (dlg.right - dlg.left) * 3 / 4; break; /* 偏左 */
            case 4: align = DT_RIGHT; tx0 = dlg.left + (dlg.right - dlg.left) / 4; break;    /* 偏右 */
            default: break;
            }
            RECT tr = {tx0, dlg.top + 42, tx1, dlg.bottom - 12};
            /* 裁剪到文本区:即使行数超出也不画出对话框(防止长文本溢出) */
            int saved = SaveDC(dc);
            IntersectClipRect(dc, tr.left, tr.top, tr.right, tr.bottom);
            if (g_theme.glow) {   /* 文字柔和阴影:先偏移画暗色 */
                SetTextColor(dc, RGB(0, 0, 0));
                RECT trs = {tr.left + 2, tr.top + 2, tr.right + 2, tr.bottom + 2};
                DrawTextW(dc, G.curText, (int)G.textChars, &trs, align | DT_TOP | DT_WORDBREAK);
                SetTextColor(dc, RGB((G.textColor >> 16) & 255, (G.textColor >> 8) & 255, G.textColor & 255));
            }
            DrawTextW(dc, G.curText, (int)G.textChars, &tr, align | DT_TOP | DT_WORDBREAK);
            RestoreDC(dc, saved);
        }
        SelectObject(dc, of);
    }
    /* 选项按钮(自绘) */
    draw_choices(dc);
    /* 叠加菜单控件 */
    if (G.menuMode == 2 && G.menuOverlayOn) draw_menu_ctrls(dc);
    /* 鼠标拖尾 */
    draw_trail(dc);
    /* 场景特效后处理(整帧数码效果) */
    apply_fx();
    /* 镜头淡入淡出过渡 */
    render_fade(dc);
    /* 快速消息提示(Toast) */
    render_toast(dc);
    /* 输出到窗口 */
    HDC wdc = GetDC(G.hwnd);
    BitBlt(wdc, 0, 0, G.w, G.h, dc, 0, 0, SRCCOPY);
    ReleaseDC(G.hwnd, wdc);
}

/* ---------- 打字机推进 ---------- */
static void tick_text(void) {
    if (!G.textActive || G.textDone || G.skip) return;
    if (G.textHold) return;   /* 先空框:等待点击才开始显示 */
    size_t total = wcslen(G.curText);
    DWORD now = GetTickCount();
    if (G.textTick == 0) G.textTick = now;   /* 新一段:记录起点(gal_text 置 0) */
    int spd = G.textSpeed > 0 ? G.textSpeed : 40;
    if ((int)(now - G.textTick) >= spd) {
        G.textChars++;
        G.textTick = now;
        if (G.textChars >= total) { G.textChars = total; G.textDone = 1; }
    }
}

EXPORT int64_t gal_menu_exit(const char *result);   /* 前向声明(定义在后面) */

/* ---------- 输入框辅助 ---------- */
static void ctrl_append_utf8(MenuCtrl *c, wchar_t ch) {
    char buf[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, buf, (int)sizeof buf, NULL, NULL);
    if (n <= 0) return;
    size_t len = strlen(c->text);
    if (len + (size_t)n >= sizeof c->text - 1) return;
    memcpy(c->text + len, buf, (size_t)n);
    c->text[len + (size_t)n] = 0;
}

static void ctrl_backspace(MenuCtrl *c) {
    size_t len = strlen(c->text);
    if (len == 0) return;
    size_t i = len - 1;
    while (i > 0 && ((unsigned char)c->text[i] & 0xC0) == 0x80) i--;
    c->text[i] = 0;
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK winproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CHAR:
        if (G.menuFocus >= 0 && G.menuFocus < G.menuCtrlCount &&
            G.menuCtrl[G.menuFocus].type == 4) {
            if (w >= 32 && w != 127)
                ctrl_append_utf8(&G.menuCtrl[G.menuFocus], (wchar_t)w);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        /* 输入框键盘:退格/回车确认/ESC 取消 */
        if (G.menuFocus >= 0 && G.menuFocus < G.menuCtrlCount &&
            G.menuCtrl[G.menuFocus].type == 4) {
            if (w == VK_BACK) { ctrl_backspace(&G.menuCtrl[G.menuFocus]); return 0; }
            if (w == VK_RETURN) {
                strncpy(G.menuEvent, G.menuCtrl[G.menuFocus].name, sizeof G.menuEvent - 1);
                G.menuEvent[sizeof G.menuEvent - 1] = 0;
                G.menuFocus = -1;
                return 0;
            }
            if (w == VK_ESCAPE) { G.menuFocus = -1; return 0; }
        }
        /* 独占菜单中按 ESC 兜底退出(即使没有配置退出路径也能救回来) */
        if (w == VK_ESCAPE && G.menuMode == 1 && G.menuActive) {
            gal_menu_exit("");
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        G.mouseX = (short)LOWORD(l);
        G.mouseY = (short)HIWORD(l);
        break;
    case WM_COMMAND: {
        int id = (int)LOWORD(w);
        if (G.choiceShow && id >= 1000 && id < 1000 + MAX_CHOICES) {
            G.picked = id - 1000;
            G.clicked = 1;
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(l), my = (short)HIWORD(l);
        /* 选项命中(自绘) */
        if (G.choiceShow) {
            int nc = choice_count();
            for (int i = 0; i < nc; i++) {
                int x0, y0, x1, y1;
                choice_rect(i, nc, &x0, &y0, &x1, &y1);
                if (mx >= x0 && mx <= x1 && my >= y0 && my <= y1) {
                    G.picked = i;
                    G.clicked = 1;
                    return 0;
                }
            }
        }
        /* 菜单控件命中(反向遍历:后创建的控件在上层,优先命中;与渲染 z-order 一致) */
        if ((G.menuMode == 1 && G.menuActive) || (G.menuMode == 2 && G.menuOverlayOn)) {
            int hit = 0;
            for (int i = G.menuCtrlCount - 1; i >= 0; i--) {
                MenuCtrl *c = &G.menuCtrl[i];
                if (!c->show) continue;
                if (mx >= c->x && mx <= c->x + c->w && my >= c->y && my <= c->y + c->h) {
                    hit = 1;
                    if (c->type == 4) {
                        G.menuFocus = i;              /* 输入框:进入输入模式 */
                    } else {
                        strncpy(G.menuEvent, c->name, sizeof G.menuEvent - 1);
                        G.menuEvent[sizeof G.menuEvent - 1] = 0;
                        G.menuFocus = -1;
                    }
                    G.clicked = 1;
                    break;
                }
            }
            if (!hit) G.menuFocus = -1;   /* 点空白取消输入焦点 */
            if (hit) return 0;
        }
        /* 点击:空框等待→开始显示;打字中→直接显示完整;完成→翻页 */
        if (G.textActive && !G.textDone) {
            if (G.textHold) {
                G.textHold = 0;
                G.textChars = 1;
                G.textTick = GetTickCount();
            } else if (!G.skip) {
                G.textChars = wcslen(G.curText);
                G.textDone = 1;
            } else {
                G.clicked = 1;
            }
        } else {
            G.clicked = 1;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        G.running = 0;
        return 0;
    case WM_ERASEBKGND:
        return 1;   /* 禁止擦背景,消除预览/运行画面闪烁 */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        render_frame();
        EndPaint(h, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(h, msg, w, l);
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ---------- 初始化 ---------- */
static void mci_cmd(const wchar_t *cmd) {
    mciSendStringW(cmd, NULL, 0, NULL);
}

/* 把 G.volume(0-100)应用到某个已打开的 MCI 别名。
   MCI 的 setaudio 音量范围是 0-1000,故乘以 10。
   此前 gal_volume() 只写入 G.volume 而从不被读取,音量控制完全无效。 */
static void apply_volume_alias(const wchar_t *alias) {
    wchar_t cmd[64];
    swprintf(cmd, 64, L"setaudio %ls volume to %d", alias, G.volume * 10);
    mci_cmd(cmd);
}

EXPORT int64_t gal_init(int64_t w, int64_t h) {
    if (g_inited) return 1;
    memset(&G, 0, sizeof G);
    g_theme = THEMES[0];
    G.trailOn = 1;               /* 鼠标拖尾默认开,可 gal_trail_enable(0) 关闭 */
    G.textColor = g_theme.textColor;
    G.w = (int)w; G.h = (int)h;
    G.bgColor = -1;
    G.textColor = 0xFFFFFF;
    G.fontSize = 26;
    G.boxOn = 1;
    G.boxName = 1;
    G.boxAvatar = 0;
    G.boxAutoFit = 0;
    G.textPos = 0;
    G.picked = -1;
    G.volume = 80;
    for (int i = 0; i < MAX_SPR; i++) {
        G.sprScale[i] = 100; G.sprShow[i] = 0;
        G.sprPosMode[i] = 0; G.sprAnim[i] = 0; G.sprY[i] = -1;
        G.sprAlpha[i] = 255; G.sprAnimTick[i] = 0;
        G.sprAutofit[i] = 0; G.sprTrimBmp[i] = NULL; G.sprTrimDC[i] = NULL;
        G.sprFit[i] = 0;
    }
    G.hInst = GetModuleHandleW(NULL);

    /* 注册窗口类 */
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = winproc;
    wc.hInstance = G.hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"DexGALWindow";
    RegisterClassW(&wc);

    /* 客户区 = w×h */
    RECT rc = {0, 0, G.w, G.h};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    G.hwnd = CreateWindowExW(0, L"DexGALWindow", L"GAL Engine",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, G.hInst, NULL);
    if (!G.hwnd) return 0;

    /* 主缓冲 */
    /* 主缓冲:自顶向下 32bpp DIB(AlphaBlend 目标坐标 = 逻辑左上原点,正常) */
    G.memDC = CreateCompatibleDC(NULL);
    memset(&G.memInfo, 0, sizeof G.memInfo);
    G.memInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    G.memInfo.bmiHeader.biWidth = G.w;
    G.memInfo.bmiHeader.biHeight = -G.h;   /* 自顶向下:第一行在内存开头 */
    G.memInfo.bmiHeader.biPlanes = 1;
    G.memInfo.bmiHeader.biBitCount = 32;
    G.memInfo.bmiHeader.biCompression = BI_RGB;
    G.memBmp = CreateDIBSection(G.memDC, &G.memInfo, DIB_RGB_COLORS, &G.memBits, NULL, 0);
    SelectObject(G.memDC, G.memBmp);

    /* 字体 */
    G.font = CreateFontW(-G.fontSize, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    G.fontName = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    /* 选项文本初始清空(自绘渲染,见 render_frame) */
    for (int i = 0; i < MAX_CHOICES; i++) G.choiceText[i][0] = 0;

    /* 随机种子(gal_rand 用) */
    srand((unsigned)(GetTickCount() ^ (size_t)&G));

    g_inited = 1;
    G.running = 1;
    render_frame();
    return 1;
}

/* ---------- 窗口 / 基础 ---------- */
EXPORT int64_t gal_set_title(const char *title) {
    wchar_t wt[MAX_PATHW];
    wpath(title, wt, MAX_PATHW);
    SetWindowTextW(G.hwnd, wt);
    return 0;
}

EXPORT int64_t gal_running(void) { return G.running; }

EXPORT int64_t gal_poll(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    tick_text();
    tick_sprites();
    render_frame();
    return 1;
}

EXPORT int64_t gal_wait(int64_t ms) {
    Sleep((DWORD)ms);
    return 0;
}

/* 随机整数: 0 .. max-1(max<=0 时返回 0) */
EXPORT int64_t gal_rand(int64_t max) {
    if (max <= 0) return 0;
    return (int64_t)(rand() % (unsigned)max);
}

EXPORT int64_t gal_close(void) {
    if (!g_inited) return 0;
    /* G.hwnd 可能尚未创建或已被销毁;gal_poll 已有空值检查,这里同样需要,
       否则无条件 DestroyWindow(NULL) 会在窗口未创建时崩溃。 */
    if (G.hwnd) DestroyWindow(G.hwnd);
    return 0;
}

/* ---------- 背景 ---------- */
EXPORT int64_t gal_bg(const char *path) {
    HBITMAP bmp = load_image(path, &G.bgW, &G.bgH);
    if (!bmp) return -1;
    if (G.bgBmp) DeleteObject(G.bgBmp);
    G.bgBmp = bmp;
    G.hasBg = 1;
    return 0;
}

EXPORT int64_t gal_bg_color(int64_t color) {
    if (G.bgBmp) { DeleteObject(G.bgBmp); G.bgBmp = NULL; }
    G.bgColor = (int)color;
    G.hasBg = 0;
    return 0;
}

/* 背景适配:0拉伸填满(默认) 1原尺寸居中 2保持比例完整显示(留边) 3保持比例填满(裁剪) */
EXPORT int64_t gal_bg_fit(int64_t m) {
    if (m < 0 || m > 3) return -1;
    G.bgFit = (int)m;
    return 0;
}

/* 柔和换背景:新图与当前背景交叉淡化(ms 过渡时长)。
   过渡在渲染时推进;结束后新背景成为当前背景。 */
EXPORT int64_t gal_bg_trans(const char *path, int64_t ms) {
    int w = 0, h = 0;
    HBITMAP bmp = load_image(path, &w, &h);
    if (!bmp) return -1;
    if (G.bgTo) DeleteObject(G.bgTo);
    G.bgTo = bmp; G.bgToW = w; G.bgToH = h;
    G.bgTransT0 = now_ms();
    G.bgTransDur = (ms > 0) ? (double)ms : 400.0;
    G.bgTransOn = 1;
    G.hasBg = 1;   /* 渐变完成后新背景会成为当前背景,先标记有背景,避免结束瞬间黑屏 */
    return 0;
}

/* ---------- 场景特效 ---------- */
EXPORT int64_t gal_fx_add(int64_t f) {
    G.fx |= (unsigned long long)f;
    return 0;
}

EXPORT int64_t gal_fx_remove(int64_t f) {
    G.fx &= ~(unsigned long long)f;
    return 0;
}

EXPORT int64_t gal_fx_clear(void) {
    G.fx = 0;
    G.fxStrength = 50;
    return 0;
}

/* 特效强度 0-100(影响震动幅度/马赛克块/模糊半径/泛白/噪点等) */
EXPORT int64_t gal_fx_strength(int64_t v) {
    if (v < 0 || v > 100) return -1;
    G.fxStrength = (int)v;
    return 0;
}

/* ---------- 菜单系统 ---------- */
static MenuCtrl *menu_find(const char *name) {
    for (int i = 0; i < G.menuCtrlCount; i++)
        if (strcmp(G.menuCtrl[i].name, name) == 0) return &G.menuCtrl[i];
    return NULL;
}

static void menu_clear(void) {
    for (int i = 0; i < G.menuCtrlCount; i++)
        if (G.menuCtrl[i].img) DeleteObject(G.menuCtrl[i].img);
    G.menuCtrlCount = 0;
    G.menuEvent[0] = 0;
    G.menuFocus = -1;
}

/* 进入独占菜单:保存镜头画面快照,进入独立画面事件循环 */
EXPORT int64_t gal_menu_begin(void) {
    menu_clear();
    if (G.menuShot) { free(G.menuShot); G.menuShot = NULL; }
    if (G.memBits) {
        G.menuShot = (unsigned char *)malloc((size_t)G.w * G.h * 4);
        if (G.menuShot) memcpy(G.menuShot, G.memBits, (size_t)G.w * G.h * 4);
    }
    G.menuMode = 1;
    G.menuActive = 1;
    G.menuResult[0] = 0;
    return 0;
}

EXPORT int64_t gal_menu_bg(const char *path) {
    HBITMAP bmp = load_image(path, &G.menuBgW, &G.menuBgH);
    if (!bmp) return -1;
    if (G.menuBg) DeleteObject(G.menuBg);
    G.menuBg = bmp;
    return 0;
}

/* 新建控件:type 0按钮 1文字 2图片 3面板 4输入框 */
EXPORT int64_t gal_menu_new(const char *name, int64_t type) {
    if (G.menuCtrlCount >= MAX_MENU_CTRL) return -1;
    if (menu_find(name)) return -1;
    MenuCtrl *c = &G.menuCtrl[G.menuCtrlCount++];
    memset(c, 0, sizeof *c);
    strncpy(c->name, name, MAX_CTRL_NAME - 1);
    c->type = (int)type;
    c->w = 160; c->h = 48;
    c->show = 1;
    c->fit = 0;
    c->bgColor = 0x282D42;
    c->fgColor = 0xFFFFFF;
    return 0;
}

/* 设置控件属性:prop = text/x/y/w/h/show/bg/fg/img(动态控制) */
EXPORT int64_t gal_menu_set(const char *name, const char *prop, const char *value) {
    MenuCtrl *c = menu_find(name);
    if (!c) return -1;
    if (strcmp(prop, "text") == 0) {
        strncpy(c->text, value ? value : "", sizeof c->text - 1);
        c->text[sizeof c->text - 1] = 0;
    } else if (strcmp(prop, "x") == 0) c->x = atoi(value);
    else if (strcmp(prop, "y") == 0) c->y = atoi(value);
    else if (strcmp(prop, "w") == 0) c->w = atoi(value);
    else if (strcmp(prop, "h") == 0) c->h = atoi(value);
    else if (strcmp(prop, "show") == 0) c->show = atoi(value) ? 1 : 0;
    else if (strcmp(prop, "fit") == 0) c->fit = atoi(value);
    else if (strcmp(prop, "bg") == 0) c->bgColor = atoi(value);
    else if (strcmp(prop, "fg") == 0) c->fgColor = atoi(value);
    else if (strcmp(prop, "img") == 0) {
        if (c->img) { DeleteObject(c->img); c->img = NULL; }
        int iw = 0, ih = 0;
        HBITMAP bmp = load_image(value, &iw, &ih);
        if (bmp) { c->img = bmp; c->imgW = iw; c->imgH = ih; c->type = 2; }
    } else return -1;
    return 0;
}

/* 退出独占菜单:设结果,恢复镜头画面 */
EXPORT int64_t gal_menu_exit(const char *result) {
    strncpy(G.menuResult, result ? result : "", sizeof G.menuResult - 1);
    G.menuResult[sizeof G.menuResult - 1] = 0;
    G.menuActive = 0;
    G.menuMode = 0;
    if (G.menuShot && G.memBits) memcpy(G.memBits, G.menuShot, (size_t)G.w * G.h * 4);
    if (G.menuShot) { free(G.menuShot); G.menuShot = NULL; }
    if (G.menuBg) { DeleteObject(G.menuBg); G.menuBg = NULL; }
    return 0;
}

EXPORT int64_t gal_menu_active(void) { return G.menuActive; }

/* 取并消费最近点击的控件名(无则空串) */
EXPORT const char *gal_menu_event(void) {
    static char buf[MAX_CTRL_NAME];
    strncpy(buf, G.menuEvent, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    G.menuEvent[0] = 0;
    return buf;
}

EXPORT const char *gal_menu_result(void) { return G.menuResult; }

/* 叠加菜单:不阻塞镜头,控件叠加显示 */
EXPORT int64_t gal_menu_overlay_show(void) {
    menu_clear();
    G.menuMode = 2;
    G.menuOverlayOn = 1;
    return 0;
}

EXPORT int64_t gal_menu_overlay_hide(void) {
    G.menuOverlayOn = 0;
    G.menuMode = 0;
    menu_clear();
    return 0;
}

EXPORT int64_t gal_menu_overlay_on(void) { return G.menuOverlayOn; }

/* 读取控件文字(输入框内容等),返回 UTF-8;无则空串 */
EXPORT const char *gal_menu_get_text(const char *name) {
    MenuCtrl *c = menu_find(name);
    return c ? c->text : "";
}

/* ---------- 主题系统 ---------- */
EXPORT int64_t gal_theme_apply(const char *name) {
    int n = (int)(sizeof(THEMES) / sizeof(THEMES[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(THEMES[i].name, name ? name : "") == 0) {
            g_theme = THEMES[i];
            G.textColor = g_theme.textColor;
            G.trailN = 0;
            return 0;
        }
    }
    return -1;
}

/* 鼠标拖尾开关:on=0 关闭,1 开启(不随主题重置) */
EXPORT int64_t gal_trail_enable(int64_t on) {
    G.trailOn = on ? 1 : 0;
    if (!G.trailOn) G.trailN = 0;
    return 0;
}

/* 当前鼠标拖尾开关(0/1) */
EXPORT int64_t gal_trail_get(void) {
    return G.trailOn;
}

/* ---------- 信号(镜头 ↔ 叠加菜单) ---------- */
EXPORT int64_t gal_signal_emit(const char *name, const char *data) {
    strncpy(G.menuSignalName, name ? name : "", sizeof G.menuSignalName - 1);
    strncpy(G.menuSignalData, data ? data : "", sizeof G.menuSignalData - 1);
    return 0;
}

EXPORT const char *gal_signal_name(void) { return G.menuSignalName; }
EXPORT const char *gal_signal_data(void) { return G.menuSignalData; }

/* ---------- 立绘 ---------- */
static void ensure_spr_dc(int i) {
    if (G.sprDC[i]) return;
    G.sprDC[i] = CreateCompatibleDC(NULL);
    SelectObject(G.sprDC[i], G.sprBmp[i]);
}

EXPORT int64_t gal_sprite(int64_t id, const char *path) {
    if (id < 0 || id >= MAX_SPR) return -1;
    HBITMAP bmp = load_image(path, &G.sprW[id], &G.sprH[id]);
    if (!bmp) return -1;
    if (G.sprBmp[id]) DeleteObject(G.sprBmp[id]);
    if (G.sprDC[id]) { DeleteDC(G.sprDC[id]); G.sprDC[id] = NULL; }
    if (G.sprTrimBmp[id]) { DeleteObject(G.sprTrimBmp[id]); G.sprTrimBmp[id] = NULL; }
    if (G.sprTrimDC[id]) { DeleteDC(G.sprTrimDC[id]); G.sprTrimDC[id] = NULL; }
    G.sprBmp[id] = bmp;
    G.sprPosMode[id] = 0;
    G.sprAnim[id] = 0;
    G.sprAlpha[id] = 255;
    G.sprAnimTick[id] = 0;
    G.sprAutofit[id] = 0;
    G.sprFit[id] = 0;
    ensure_spr_dc((int)id);
    spr_layout((int)id);
    return 0;
}

EXPORT int64_t gal_sprite_pos(int64_t id, int64_t x) {
    if (id < 0 || id >= MAX_SPR) return -1;
    G.sprX[id] = (int)x;
    return 0;
}

EXPORT int64_t gal_sprite_scale(int64_t id, int64_t pct) {
    if (id < 0 || id >= MAX_SPR) return -1;
    if (pct < 5 || pct > 500) return -1;
    G.sprScale[id] = (int)pct;
    return 0;
}

EXPORT int64_t gal_sprite_show(int64_t id, int64_t on) {
    if (id < 0 || id >= MAX_SPR) return -1;
    G.sprShow[id] = (int)(on ? 1 : 0);
    return 0;
}

/* 位置模式:0中间 1左 2右 3偏左 4偏右;-1=手动(绝对 x) */
EXPORT int64_t gal_sprite_pos_mode(int64_t id, int64_t pos) {
    if (id < 0 || id >= MAX_SPR) return -1;
    G.sprPosMode[id] = (int)pos;
    spr_layout((int)id);
    return 0;
}

/* 自适应:裁剪透明区并缩放到屏高 78% */
EXPORT int64_t gal_sprite_autofit(int64_t id, int64_t on) {
    if (id < 0 || id >= MAX_SPR) return -1;
    G.sprAutofit[id] = (int)(on ? 1 : 0);
    if (G.sprAutofit[id]) spr_autofit((int)id);
    spr_layout((int)id);
    return 0;
}

/* 立绘适配:0原大小(×缩放%) 1自动适配(裁剪透明+高78%) 2完整显示(等比) 3填满(等比) */
EXPORT int64_t gal_sprite_fit(int64_t id, int64_t mode) {
    if (id < 0 || id >= MAX_SPR) return -1;
    if (mode < 0 || mode > 3) return -1;
    G.sprFit[id] = (int)mode;
    if (mode == 1 && !G.sprTrimBmp[id]) spr_autofit((int)id);
    spr_layout((int)id);
    return 0;
}

/* 动画:0无 1呼吸 2淡入 3上浮 4抖动 5脉冲 6消失 */
EXPORT int64_t gal_sprite_anim(int64_t id, int64_t anim) {
    if (id < 0 || id >= MAX_SPR) return -1;
    if (anim < 0 || anim > 6) return -1;
    G.sprAnim[id] = (int)anim;
    G.sprAnimTick[id] = 0;
    return 0;
}

/* ---------- 文本 ---------- */
EXPORT int64_t gal_text(const char *s) {
    wchar_t wt[MAX_TEXT];
    wpath(s, wt, MAX_TEXT);
    wcsncpy(G.curText, wt, MAX_TEXT - 1);
    G.curText[MAX_TEXT - 1] = 0;
    G.textChars = 0;
    G.textActive = 1;
    G.textDone = (G.skip || wcslen(G.curText) == 0 || G.textMode == 1);
    if (G.textMode == 1) G.textChars = wcslen(G.curText);   /* 直接全部 */
    G.textHold = (G.textMode == 2 && !G.textDone);          /* 先空框点击 */
    G.textTick = 0;
    return 0;
}

EXPORT int64_t gal_text_done(void) { return G.textDone; }

EXPORT int64_t gal_text_clear(void) {
    G.curText[0] = 0;
    G.speaker[0] = 0;
    G.textChars = 0;
    G.textActive = 0;
    G.textDone = 0;
    G.clicked = 0;
    return 0;
}

EXPORT int64_t gal_speaker(const char *s) {
    wchar_t wt[128];
    wpath(s, wt, 128);
    wcsncpy(G.speaker, wt, 127);
    G.speaker[127] = 0;
    return 0;
}

EXPORT int64_t gal_text_color(int64_t color) {
    G.textColor = (int)color;
    return 0;
}

EXPORT int64_t gal_text_size(int64_t px) {
    if (px < 8 || px > 72) return -1;
    G.fontSize = (int)px;
    if (G.font) DeleteObject(G.font);
    G.font = CreateFontW(-G.fontSize, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    return 0;
}

EXPORT int64_t gal_clicked(void) { return G.clicked; }

EXPORT int64_t gal_skip(int64_t on) {
    G.skip = (int)(on ? 1 : 0);
    if (G.skip) { G.textChars = wcslen(G.curText); G.textDone = 1; }
    return 0;
}

/* 文字模式:0逐字 1直接全部 2先空框点击后显示 */
EXPORT int64_t gal_text_mode(int64_t m) {
    if (m < 0 || m > 2) return -1;
    G.textMode = (int)m;
    return 0;
}

/* 逐字间隔(毫秒/字);0 恢复默认 40 */
EXPORT int64_t gal_text_speed(int64_t ms) {
    if (ms < 0 || ms > 2000) return -1;
    G.textSpeed = (int)ms;
    return 0;
}

EXPORT int64_t gal_box(int64_t on) {
    G.boxOn = (int)(on ? 1 : 0);
    return 0;
}

/* 对话框风格:avatar=有头像(用立绘图层0),name=显示人名 */
EXPORT int64_t gal_box_style(int64_t avatar, int64_t name) {
    G.boxAvatar = (int)(avatar ? 1 : 0);
    G.boxName = (int)(name ? 1 : 0);
    return 0;
}

/* 对话框宽度自适应(按文本宽度收缩) */
EXPORT int64_t gal_box_autofit(int64_t on) {
    G.boxAutoFit = (int)(on ? 1 : 0);
    return 0;
}

/* 文字位置:0中间 1左 2右 3偏左 4偏右 */
EXPORT int64_t gal_text_pos(int64_t pos) {
    if (pos < 0 || pos > 4) return -1;
    G.textPos = (int)pos;
    return 0;
}

/* ---------- 选项 ---------- */
EXPORT int64_t gal_set_choice(int64_t i, const char *s) {
    if (i < 0 || i >= MAX_CHOICES) return -1;
    wpath(s, G.choiceText[i], 256);
    return 0;
}

EXPORT int64_t gal_show_choices(void) {
    G.choiceShow = 1;
    G.picked = -1;
    return 0;
}

EXPORT int64_t gal_hide_choices(void) {
    G.choiceShow = 0;
    G.picked = -1;
    return 0;
}

/* 选项按钮几何(自绘渲染与点击命中共用) */
static int choice_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_CHOICES; i++) if (G.choiceText[i][0]) n++;
    return n;
}
static int choice_start_y(int nc) {
    int bh = 34, gap = 8;
    int total = nc * bh + (nc - 1) * gap;
    return G.h - 190 - total;
}
static void choice_rect(int i, int nc, int *x0, int *y0, int *x1, int *y1) {
    int bh = 34, gap = 8;
    *x0 = 40; *x1 = G.w - 40;
    *y0 = choice_start_y(nc) + i * (bh + gap);
    *y1 = *y0 + bh;
}

/* 绘制选项按钮(在 render_frame 中调用,dc = memDC) */
static void draw_choices(HDC dc) {
    if (!G.choiceShow) return;
    int nc = choice_count();
    if (nc <= 0) return;
    for (int i = 0; i < nc; i++) {
        int x0, y0, x1, y1;
        choice_rect(i, nc, &x0, &y0, &x1, &y1);
        int hover = (G.mouseX >= x0 && G.mouseX <= x1 && G.mouseY >= y0 && G.mouseY <= y1);
        int fill = hover ? g_theme.optHover : g_theme.optBg;
        int bc = g_theme.optBorder;
        HPEN pen = CreatePen(PS_SOLID, hover ? 3 : 2,
                             RGB((bc >> 16) & 255, (bc >> 8) & 255, bc & 255));
        HGDIOBJ op = (HGDIOBJ)SelectObject(dc, pen);
        HBRUSH br = CreateSolidBrush(RGB((fill >> 16) & 255, (fill >> 8) & 255, fill & 255));
        HGDIOBJ ob = (HGDIOBJ)SelectObject(dc, br);
        RoundRect(dc, x0, y0, x1, y1, g_theme.optRound, g_theme.optRound);
        SelectObject(dc, ob); DeleteObject(br);
        SelectObject(dc, op); DeleteObject(pen);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB((g_theme.optText >> 16) & 255,
                             (g_theme.optText >> 8) & 255,
                             g_theme.optText & 255));
        HFONT of = (HFONT)SelectObject(dc, G.font);
        RECT tr = {x0 + 14, y0, x1 - 10, y1};
        DrawTextW(dc, G.choiceText[i], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
}

EXPORT int64_t gal_picked(void) { return G.picked; }

/* ---------- 音频(mci) ---------- */
EXPORT int64_t gal_play_bgm(const char *path) {
    wchar_t wt[MAX_PATHW];
    wchar_t cmd[MAX_PATHW + 128];
    wpath(path, wt, MAX_PATHW);
    if (G.bgmOpen) {
        mci_cmd(L"close bgm");
        G.bgmOpen = 0;
    }
    swprintf(cmd, MAX_PATHW + 128, L"open \"%ls\" type mpegvideo alias bgm", wt);
    if (mciSendStringW(cmd, NULL, 0, NULL) != 0) return -1;
    apply_volume_alias(L"bgm");
    mci_cmd(L"play bgm repeat");
    G.bgmOpen = 1;
    return 0;
}

EXPORT int64_t gal_stop_bgm(void) {
    if (G.bgmOpen) {
        mci_cmd(L"stop bgm");
        mci_cmd(L"close bgm");
        G.bgmOpen = 0;
    }
    return 0;
}

EXPORT int64_t gal_play_se(const char *path) {
    wchar_t wt[MAX_PATHW];
    wchar_t cmd[MAX_PATHW + 128];
    wpath(path, wt, MAX_PATHW);
    mci_cmd(L"close se");
    swprintf(cmd, MAX_PATHW + 128, L"open \"%ls\" alias se", wt);
    if (mciSendStringW(cmd, NULL, 0, NULL) != 0) return -1;
    apply_volume_alias(L"se");
    mci_cmd(L"play se");
    return 0;
}

EXPORT int64_t gal_stop_se(void) {
    mci_cmd(L"stop se");
    mci_cmd(L"close se");
    return 0;
}

/* 停止全部声音(BGM + SE) */
EXPORT int64_t gal_stop_sound(void) {
    gal_stop_bgm();
    gal_stop_se();
    return 0;
}

/* 镜头过渡:淡出到全黑(不阻塞;画面保持全黑,直到 gal_fade_in) */
EXPORT int64_t gal_fade_out(int64_t ms) {
    G.fadePhase = 1;
    G.fadeT0 = now_ms();
    G.fadeDur = (ms > 0) ? (double)ms : 300.0;
    return 0;
}

/* 镜头过渡:从全黑淡入(阻塞到完成)。无淡出状态时直接返回(no-op)。 */
EXPORT int64_t gal_fade_in(int64_t ms) {
    if (G.fadePhase == 0) return 0;
    double dur = (ms > 0) ? (double)ms : (G.fadeDur > 0 ? G.fadeDur : 300.0);
    G.fadePhase = 2; G.fadeT0 = now_ms(); G.fadeDur = dur;
    while ((now_ms() - G.fadeT0) < dur) { render_frame(); Sleep(16); }
    G.fadePhase = 0; render_frame();
    return 0;
}

EXPORT int64_t gal_volume(int64_t v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    G.volume = (int)v;
    /* 立即作用于正在播放的声音(对未打开的别名,setaudio 会静默失败,无副作用) */
    apply_volume_alias(L"bgm");
    apply_volume_alias(L"se");
    return 0;
}

/* ---------- 截图(保存当前帧为 BMP,自顶向下) ---------- */
EXPORT int64_t gal_screenshot(const char *path) {
    if (!G.memBits) return -1;
    BITMAPFILEHEADER bf;
    BITMAPINFOHEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.biSize = sizeof bi;
    bi.biWidth = G.w;
    bi.biHeight = -G.h;          /* 自顶向下:文件第一行 = 图像顶部 */
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    int row = G.w * 4;
    int img = row * G.h;
    memset(&bf, 0, sizeof bf);
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof bf + sizeof bi;
    bf.bfSize = bf.bfOffBits + img;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(&bf, 1, sizeof bf, f);
    fwrite(&bi, 1, sizeof bi, f);
    /* memBits 是自顶向下 DIB,第一行在内存开头,顺序写入即可 */
    unsigned char *bits = (unsigned char *)G.memBits;
    fwrite(bits, 1, img, f);
    fclose(f);
    return 0;
}


/* ================= 变量存储(全局,进程级) ================= */
#define MAX_VARS 256
#define VAR_NAME 64
#define VAR_VAL 512
typedef struct { char name[VAR_NAME]; char val[VAR_VAL]; } GalVar;
static GalVar g_vars[MAX_VARS];
static int g_nVars = 0;
static char g_varBuf[VAR_VAL];

static GalVar *var_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_nVars; i++)
        if (strcmp(g_vars[i].name, name) == 0) return &g_vars[i];
    return NULL;
}

/* 设置变量(值以字符串存储,支持任意类型文本) */
EXPORT int64_t gal_var_set(const char *name, const char *value) {
    if (!name || !name[0]) return -1;
    GalVar *v = var_find(name);
    if (!v) {
        if (g_nVars >= MAX_VARS) return -1;
        v = &g_vars[g_nVars++];
        strncpy(v->name, name, VAR_NAME - 1); v->name[VAR_NAME - 1] = 0;
    }
    if (value) { strncpy(v->val, value, VAR_VAL - 1); v->val[VAR_VAL - 1] = 0; }
    else v->val[0] = 0;
    return 0;
}

/* 获取变量(返回静态缓冲,立即使用) */
EXPORT const char *gal_var_get(const char *name) {
    GalVar *v = var_find(name);
    if (v) { strncpy(g_varBuf, v->val, VAR_VAL - 1); g_varBuf[VAR_VAL - 1] = 0; }
    else g_varBuf[0] = 0;
    return g_varBuf;
}

/* 变量数值(int 解析) */
EXPORT int64_t gal_var_num(const char *name) {
    GalVar *v = var_find(name);
    return v ? atoi(v->val) : 0;
}

/* 整数转字符串 */
EXPORT const char *gal_itoa(int64_t v) {
    snprintf(g_varBuf, VAR_VAL, "%lld", (long long)v);
    return g_varBuf;
}

/* 字符串转浮点 */
EXPORT double gal_atof(const char *s) {
    return s ? atof(s) : 0.0;
}

/* 浮点转字符串 */
EXPORT const char *gal_ftoa(double f) {
    snprintf(g_varBuf, VAR_VAL, "%g", f);
    return g_varBuf;
}

/* ================= 文件 IO(存档/读档/读写文本) ================= */
static char g_fileBuf[16384];

static HANDLE gal_open_file(const char *path, DWORD access, DWORD creation) {
    wchar_t wt[MAX_PATHW];
    wpath(path, wt, MAX_PATHW);
    return CreateFileW(wt, access, FILE_SHARE_READ, NULL, creation,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

/* 写入文本文件(覆盖,UTF-8 字节原样) */
EXPORT int64_t gal_file_write(const char *path, const char *content) {
    if (!path || !path[0]) return -1;
    HANDLE h = gal_open_file(path, GENERIC_WRITE, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD w = 0;
    if (content) WriteFile(h, content, (DWORD)strlen(content), &w, NULL);
    CloseHandle(h);
    return 0;
}

/* 读取文本文件(返回静态缓冲,立即使用);失败返回空串 */
EXPORT const char *gal_file_read(const char *path) {
    g_fileBuf[0] = 0;
    if (!path || !path[0]) return g_fileBuf;
    HANDLE h = gal_open_file(path, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return g_fileBuf;
    DWORD n = 0;
    ReadFile(h, g_fileBuf, sizeof(g_fileBuf) - 1, &n, NULL);
    CloseHandle(h);
    g_fileBuf[n] = 0;
    return g_fileBuf;
}

/* 存档:把所有 gal_var 变量写入文件(每行 name=value)。成功返回 0。 */
EXPORT int64_t gal_save_vars(const char *path) {
    if (!path || !path[0]) return -1;
    HANDLE h = gal_open_file(path, GENERIC_WRITE, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD w = 0;
    char line[VAR_NAME + VAR_VAL + 2];
    for (int i = 0; i < g_nVars; i++) {
        int n = snprintf(line, sizeof line, "%s=%s\n", g_vars[i].name, g_vars[i].val);
        if (n > 0) WriteFile(h, line, (DWORD)n, &w, NULL);
    }
    CloseHandle(h);
    return 0;
}

/* 读档:从文件恢复 gal_var 变量(追加/覆盖同名)。成功返回 0。 */
EXPORT int64_t gal_load_vars(const char *path) {
    if (!path || !path[0]) return -1;
    HANDLE h = gal_open_file(path, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD n = 0;
    ReadFile(h, g_fileBuf, sizeof(g_fileBuf) - 1, &n, NULL);
    CloseHandle(h);
    g_fileBuf[n] = 0;
    /* 逐行解析 name=value */
    char *p = g_fileBuf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            gal_var_set(p, eq + 1);
        }
        if (nl) p = nl + 1; else break;
    }
    return 0;
}

/* ================= 立绘增强 ================= */
/* 精确坐标(手动模式); posMode 设为 -1 */
EXPORT int64_t gal_sprite_xy(int64_t id, int64_t x, int64_t y) {
    if (id < 0 || id >= MAX_SPR) return -1;
    G.sprPosMode[id] = -1;
    G.sprX[id] = (int)x;
    G.sprY[id] = (int)y;
    return 0;
}

/* 立绘透明度(0-255):0隐藏 255正常 中间变暗(用于高亮当前说话人) */
EXPORT int64_t gal_sprite_alpha(int64_t id, int64_t a) {
    if (id < 0 || id >= MAX_SPR) return -1;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    G.sprAlpha[id] = (int)a;
    return 0;
}
