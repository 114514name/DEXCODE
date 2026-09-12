/* dg_text.c — 文字:DirectWrite + 字形图集(M4-c)
 *
 * 设计(见 docs/DEXGAME_DESIGN.md §9):
 *   - **DirectWrite 提供字形**,我们自己拼图集与四边形 —— 不用 IDWriteTextLayout::Draw
 *     那种"自己画到 DC 上"的路子,因为引擎的渲染全在 D3D11 批次里。
 *   - 每个字形用 `IDWriteGlyphRunAnalysis` 单独光栅化成**覆盖率位图**
 *     (DWRITE_TEXTURE_CLEARTYPE_3x1 → 取 R/G/B 的**最大值**当 alpha;
 *      ClearType 的次像素能量分在三通道里,取最大值能保住笔画粗细),
 *     再写进一张 512x512 的图集纹理(货架式打包 + 1px 间隙防渗色)。
 *   - 画字 = 把字形位图当四边形画出去(白色 + alpha,颜色靠顶点色 tint)。
 *     **整数像素定位**:位图是按整数像素光栅化的,和 quad 1:1 才对得上
 *     (与 §4.3 的"区域用边界、单纹素用中心"规则配合,不会糊)。
 *   - 度量用 DirectWrite 的字体度量(designUnitsPerEm/ascent/descent/lineGap),
 *     **测量结果按字形缓存**,不是每次重新布局(现有 gal 每帧量两次文字是已知坑)。
 *   - 中文能显示:家族名走系统字体集合(`Microsoft YaHei UI` 自带 CJK 回退),
 *     UTF-8 直接解码成码点。
 *
 * 已知范围(一期):不做换行/对齐/富文本;`\n` 支持;字距/字偶不参与(直接用字形
 * advance);图集满了会明确报错(要求放大图集或减少字号种类)。
 */
#include "dexgame.h"

#include <windows.h>
#include <dwrite.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dg_guids.h"

#define DG_MAX_FONTS      16
#define DG_MAX_GLYPHS     4096
#define DG_ATLAS_W        512
#define DG_ATLAS_H        512
#define DG_ATLAS_PAD      1

typedef struct {
    int                 used;
    uint32_t            gen;
    char                family[64];
    float               size;
    IDWriteTextFormat  *format;
    IDWriteFontFace    *face;
    float               scale;         /* size / designUnitsPerEm */
    float               ascent;        /* 像素 */
    float               line_height;   /* 像素 */
} DgFont;

typedef struct {
    int      used;
    int32_t  font;                     /* 字体句柄(含代际)*/
    uint32_t cp;
    int      rasterized;
    float    advance;
    int16_t  ox, oy;                   /* 墨水左上角相对 (笔位, 基线) 的像素偏移 */
    int16_t  w, h;
    float    u0, v0, u1, v1;           /* 图集 uv(区域边界)*/
} DgGlyph;

static IDWriteFactory *g_dw = NULL;
static int             g_dw_ok = 0;
static int             g_dw_tried = 0;

static DgFont  g_fonts[DG_MAX_FONTS];
static DgGlyph g_glyphs[DG_MAX_GLYPHS];
static uint32_t g_font_gen = 0;
static int      g_nglyphs = 0;

static int      g_atlas_tex = -1;
static int      g_atlas_pen_x = 0, g_atlas_pen_y = 0, g_atlas_row_h = 0;
static uint8_t *g_tile = NULL;          /* 光栅化临时缓冲 */
static size_t   g_tile_cap = 0;

/* ---------- 生命周期 ---------- */
static void dg_text_free_all(void) {
    for (int i = 0; i < DG_MAX_FONTS; i++) {
        if (g_fonts[i].format) { g_fonts[i].format->lpVtbl->Release(g_fonts[i].format); g_fonts[i].format = NULL; }
        if (g_fonts[i].face) { g_fonts[i].face->lpVtbl->Release(g_fonts[i].face); g_fonts[i].face = NULL; }
        g_fonts[i].used = 0;
    }
    free(g_tile);
    g_tile = NULL;
    g_tile_cap = 0;
    g_nglyphs = 0;
    g_atlas_tex = -1;
    g_atlas_pen_x = g_atlas_pen_y = g_atlas_row_h = 0;
}

void dg_text_init(void) {
    dg_text_free_all();
    g_dw_ok = 0;
    g_dw_tried = 1;
    if (g_dw) { g_dw->lpVtbl->Release(g_dw); g_dw = NULL; }
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &DG_IID_IDWriteFactory,
                                     (IUnknown **)&g_dw);
    if (FAILED(hr) || !g_dw) {
        dg_error("DWriteCreateFactory failed (hr=0x%08lX)", (unsigned long)hr);
        g_dw = NULL;
        return;
    }
    g_dw_ok = 1;
}

void dg_text_shutdown(void) {
    dg_text_free_all();
    if (g_dw) { g_dw->lpVtbl->Release(g_dw); g_dw = NULL; }
    g_dw_ok = 0;
}

int dg_text_ok(void) { return g_dw_ok; }
int dg_text_glyph_count(void) { return g_nglyphs; }
int dg_text_font_count(void) {
    int n = 0;
    for (int i = 0; i < DG_MAX_FONTS; i++) if (g_fonts[i].used) n++;
    return n;
}

/* ---------- 字体 ---------- */
static DgFont *dg_font_at(int32_t id) {
    if (id <= 0) return NULL;
    const int idx = (id & 0xFFFF) - 1;
    if (idx < 0 || idx >= DG_MAX_FONTS) return NULL;
    if (!g_fonts[idx].used || (int32_t)g_fonts[idx].gen != (id >> 16)) return NULL;
    return &g_fonts[idx];
}

int32_t dg_text_font_load(const char *family, float size) {
    if (!g_dw_ok) { dg_error("DirectWrite is not available"); return -1; }
    if (!family || !family[0]) { dg_error("font family is empty"); return -1; }
    if (size <= 0.0f) { dg_error("font size %g must be > 0", (double)size); return -1; }
    for (int i = 0; i < DG_MAX_FONTS; i++)                    /* 同家族同字号复用 */
        if (g_fonts[i].used && g_fonts[i].size == size && strcmp(g_fonts[i].family, family) == 0)
            return (int32_t)((g_fonts[i].gen << 16) | (uint32_t)(i + 1));
    int slot = -1;
    for (int i = 0; i < DG_MAX_FONTS; i++) if (!g_fonts[i].used) { slot = i; break; }
    if (slot < 0) { dg_error("too many fonts (max %d)", DG_MAX_FONTS); return -1; }

    /* UTF-16 家族名(只支持 BMP,够用) */
    WCHAR wfam[64];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)family; *p && n < 63; p++)
        wfam[n++] = (WCHAR)(*p < 0x80 ? *p : '?');            /* 家族名只接受 ASCII/拉丁 */
    wfam[n] = 0;

    IDWriteTextFormat *format = NULL;
    HRESULT hr = g_dw->lpVtbl->CreateTextFormat(g_dw, wfam, NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               size, L"", &format);
    if (FAILED(hr) || !format) {
        dg_error("CreateTextFormat('%s', %g) failed (hr=0x%08lX)", family, (double)size,
                 (unsigned long)hr);
        return -1;
    }
    IDWriteFontCollection *coll = NULL;
    if (FAILED(g_dw->lpVtbl->GetSystemFontCollection(g_dw, &coll, FALSE)) || !coll) {
        format->lpVtbl->Release(format);
        dg_error("GetSystemFontCollection failed");
        return -1;
    }
    UINT32 famidx = 0; BOOL exists = FALSE;
    hr = coll->lpVtbl->FindFamilyName(coll, wfam, &famidx, &exists);
    IDWriteFontFamily *fam = NULL;
    IDWriteFont *font = NULL;
    IDWriteFontFace *face = NULL;
    if (SUCCEEDED(hr) && exists) hr = coll->lpVtbl->GetFontFamily(coll, famidx, &fam);
    if (SUCCEEDED(hr) && fam)
        hr = fam->lpVtbl->GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, &font);
    if (SUCCEEDED(hr) && font) hr = font->lpVtbl->CreateFontFace(font, &face);
    if (fam) fam->lpVtbl->Release(fam);
    if (font) font->lpVtbl->Release(font);
    coll->lpVtbl->Release(coll);
    if (FAILED(hr) || !face) {
        format->lpVtbl->Release(format);
        dg_error("font family '%s' not found (or no font face)", family);
        return -1;
    }
    DWRITE_FONT_METRICS fm;
    face->lpVtbl->GetMetrics(face, &fm);
    if (fm.designUnitsPerEm == 0) {
        face->lpVtbl->Release(face);
        format->lpVtbl->Release(format);
        dg_error("font '%s' reports designUnitsPerEm = 0", family);
        return -1;
    }
    DgFont *f = &g_fonts[slot];
    memset(f, 0, sizeof *f);
    f->used = 1;
    f->gen = ++g_font_gen;
    snprintf(f->family, sizeof f->family, "%s", family);
    f->size = size;
    f->format = format;
    f->face = face;
    f->scale = size / (float)fm.designUnitsPerEm;
    f->ascent = (float)fm.ascent * f->scale;
    f->line_height = (float)(fm.ascent + fm.descent + fm.lineGap) * f->scale;
    return (int32_t)((f->gen << 16) | (uint32_t)(slot + 1));
}

int32_t dg_text_font_free(int32_t id) {
    DgFont *f = dg_font_at(id);
    if (!f) { dg_error("font %d does not exist", id); return -1; }
    if (f->format) { f->format->lpVtbl->Release(f->format); f->format = NULL; }
    if (f->face) { f->face->lpVtbl->Release(f->face); f->face = NULL; }
    f->used = 0;
    for (int i = 0; i < DG_MAX_GLYPHS; i++)             /* 该字体的字形缓存一并失效 */
        if (g_glyphs[i].used && g_glyphs[i].font == id) g_glyphs[i].used = 0;
    return 0;
}

double dg_text_line_height(int32_t id) {
    const DgFont *f = dg_font_at(id);
    if (!f) { dg_error("font %d does not exist", id); return 0.0; }
    return (double)f->line_height;
}
double dg_text_ascent(int32_t id) {
    const DgFont *f = dg_font_at(id);
    if (!f) { dg_error("font %d does not exist", id); return 0.0; }
    return (double)f->ascent;
}
const char *dg_text_family(int32_t id) {
    const DgFont *f = dg_font_at(id);
    return f ? f->family : "";
}
double dg_text_size(int32_t id) {
    const DgFont *f = dg_font_at(id);
    return f ? (double)f->size : 0.0;
}

/* ---------- 字形(度量 + 光栅化进图集)---------- */
static int dg_atlas_ensure(void) {
    if (g_atlas_tex >= 0 && dg_tex_valid(g_atlas_tex)) return 0;
    uint8_t *blank = (uint8_t *)calloc((size_t)DG_ATLAS_W * DG_ATLAS_H * 4, 1);
    if (!blank) { dg_error("out of memory creating glyph atlas"); return -1; }
    g_atlas_tex = dg_tex_create_rgba(DG_ATLAS_W, DG_ATLAS_H, blank);
    free(blank);
    if (g_atlas_tex < 0) { dg_error("cannot create glyph atlas texture"); return -1; }
    g_atlas_pen_x = g_atlas_pen_y = g_atlas_row_h = 0;
    return 0;
}

/* 把 3 字节/像素的 ClearType 覆盖率转成 RGBA(白字 + alpha)*/
static int dg_glyph_blit(int x, int y, int w, int h, const uint8_t *cov) {
    const size_t need = (size_t)w * h * 4;
    if (need > g_tile_cap) {
        uint8_t *nt = (uint8_t *)realloc(g_tile, need);
        if (!nt) { dg_error("out of memory for glyph tile"); return -1; }
        g_tile = nt;
        g_tile_cap = need;
    }
    for (int i = 0; i < w * h; i++) {
        const uint8_t r = cov[i * 3 + 0], g = cov[i * 3 + 1], b = cov[i * 3 + 2];
        uint8_t a = r > g ? r : g;
        if (b > a) a = b;
        g_tile[i * 4 + 0] = 255;
        g_tile[i * 4 + 1] = 255;
        g_tile[i * 4 + 2] = 255;
        g_tile[i * 4 + 3] = a;
    }
    return dg_tex_update_rgba(g_atlas_tex, x, y, w, h, g_tile);
}

/* 取字形;need_bitmap=1 时确保图集里已有位图。返回 NULL 表示失败(已 dg_error)*/
static DgGlyph *dg_glyph_get(int32_t font_id, uint32_t cp, int need_bitmap) {
    DgFont *f = dg_font_at(font_id);
    if (!f) { dg_error("font %d does not exist", font_id); return NULL; }
    DgGlyph *g = NULL;
    for (int i = 0; i < DG_MAX_GLYPHS; i++)
        if (g_glyphs[i].used && g_glyphs[i].font == font_id && g_glyphs[i].cp == cp) { g = &g_glyphs[i]; break; }
    if (!g) {
        int slot = -1;
        for (int i = 0; i < DG_MAX_GLYPHS; i++) if (!g_glyphs[i].used) { slot = i; break; }
        if (slot < 0) { dg_error("glyph cache full (max %d)", DG_MAX_GLYPHS); return NULL; }
        UINT16 gi = 0;
        if (FAILED(f->face->lpVtbl->GetGlyphIndices(f->face, &cp, 1, &gi))) {
            dg_error("GetGlyphIndices failed for U+%04X", (unsigned)cp);
            return NULL;
        }
        DWRITE_GLYPH_METRICS gm;
        if (FAILED(f->face->lpVtbl->GetDesignGlyphMetrics(f->face, &gi, 1, &gm, FALSE))) {
            dg_error("GetDesignGlyphMetrics failed for U+%04X", (unsigned)cp);
            return NULL;
        }
        g = &g_glyphs[slot];
        memset(g, 0, sizeof *g);
        g->used = 1;
        g->font = font_id;
        g->cp = cp;
        g->advance = (float)gm.advanceWidth * f->scale;
        g_nglyphs++;
    }
    if (!need_bitmap || g->rasterized) return g;

    /* --- 光栅化这一个字形 --- */
    if (dg_atlas_ensure()) return NULL;
    UINT16 gi = 0;
    if (FAILED(f->face->lpVtbl->GetGlyphIndices(f->face, &cp, 1, &gi))) return NULL;
    const FLOAT adv = g->advance;
    DWRITE_GLYPH_RUN run;
    memset(&run, 0, sizeof run);
    run.fontFace = f->face;
    run.fontEmSize = f->size;
    run.glyphCount = 1;
    run.glyphIndices = &gi;
    run.glyphAdvances = &adv;
    DWRITE_GLYPH_OFFSET off;
    off.advanceOffset = 0.0f;
    off.ascenderOffset = 0.0f;
    run.glyphOffsets = &off;
    IDWriteGlyphRunAnalysis *an = NULL;
    HRESULT hr = g_dw->lpVtbl->CreateGlyphRunAnalysis(
        g_dw, &run, 1.0f, NULL, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL,
        DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, &an);
    if (FAILED(hr) || !an) { dg_error("CreateGlyphRunAnalysis failed (hr=0x%08lX)", (unsigned long)hr); return NULL; }
    RECT b;
    memset(&b, 0, sizeof b);
    hr = an->lpVtbl->GetAlphaTextureBounds(an, DWRITE_TEXTURE_CLEARTYPE_3x1, &b);
    if (FAILED(hr)) { an->lpVtbl->Release(an); dg_error("GetAlphaTextureBounds failed"); return NULL; }
    const int gw = (int)(b.right - b.left), gh = (int)(b.bottom - b.top);
    if (gw <= 0 || gh <= 0) {                 /* 空格/控制符:没有墨水,只有 advance */
        an->lpVtbl->Release(an);
        g->rasterized = 1;
        g->ox = g->oy = 0;
        g->w = g->h = 0;
        return g;
    }
    const UINT32 bytes = (UINT32)(gw * gh * 3);
    uint8_t *cov = (uint8_t *)malloc(bytes);
    if (!cov) { an->lpVtbl->Release(an); dg_error("out of memory for glyph bitmap"); return NULL; }
    hr = an->lpVtbl->CreateAlphaTexture(an, DWRITE_TEXTURE_CLEARTYPE_3x1, &b, cov, bytes);
    an->lpVtbl->Release(an);
    if (FAILED(hr)) { free(cov); dg_error("CreateAlphaTexture failed (hr=0x%08lX)", (unsigned long)hr); return NULL; }

    /* 货架式打包 */
    if (g_atlas_pen_x + gw + DG_ATLAS_PAD > DG_ATLAS_W) {
        g_atlas_pen_x = 0;
        g_atlas_pen_y += g_atlas_row_h + DG_ATLAS_PAD;
        g_atlas_row_h = 0;
    }
    if (g_atlas_pen_y + gh + DG_ATLAS_PAD > DG_ATLAS_H) {
        free(cov);
        dg_error("glyph atlas is full (%dx%d) —— 减少字号种类或调大图集", DG_ATLAS_W, DG_ATLAS_H);
        return NULL;
    }
    if (dg_glyph_blit(g_atlas_pen_x, g_atlas_pen_y, gw, gh, cov)) { free(cov); return NULL; }
    free(cov);
    g->ox = (int16_t)b.left;
    g->oy = (int16_t)b.top;
    g->w = (int16_t)gw;
    g->h = (int16_t)gh;
    g->u0 = (float)g_atlas_pen_x / (float)DG_ATLAS_W;
    g->v0 = (float)g_atlas_pen_y / (float)DG_ATLAS_H;
    g->u1 = (float)(g_atlas_pen_x + gw) / (float)DG_ATLAS_W;
    g->v1 = (float)(g_atlas_pen_y + gh) / (float)DG_ATLAS_H;
    g->rasterized = 1;
    g_atlas_pen_x += gw + DG_ATLAS_PAD;
    if (gh > g_atlas_row_h) g_atlas_row_h = gh;
    return g;
}

/* ---------- UTF-8 → 码点 ---------- */
static uint32_t dg_utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    if (!*s) return 0;
    uint32_t cp = *s++;
    if (cp < 0x80) { *p = (const char *)s; return cp; }
    int extra = 0;
    if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; extra = 1; }
    else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; extra = 2; }
    else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; extra = 3; }
    else { *p = (const char *)s; return '?'; }        /* 非法首字节 */
    for (int i = 0; i < extra; i++) {
        if ((*s & 0xC0) != 0x80) { *p = (const char *)s; return '?'; }
        cp = (cp << 6) | (*s++ & 0x3F);
    }
    *p = (const char *)s;
    return cp;
}

/* ---------- 测量 / 绘制 ---------- */
double dg_text_measure_w(int32_t font_id, const char *text) {
    const DgFont *f = dg_font_at(font_id);
    if (!f) { dg_error("font %d does not exist", font_id); return 0.0; }
    if (!text) return 0.0;
    double maxw = 0.0, line = 0.0;
    const char *p = text;
    for (;;) {
        const uint32_t cp = dg_utf8_next(&p);
        if (!cp) break;
        if (cp == '\n') { if (line > maxw) maxw = line; line = 0.0; continue; }
        if (cp == '\r') continue;
        const DgGlyph *g = dg_glyph_get(font_id, cp, 0);
        if (!g) return 0.0;
        line += g->advance;
    }
    return line > maxw ? line : maxw;
}

double dg_text_measure_h(int32_t font_id, const char *text) {
    const DgFont *f = dg_font_at(font_id);
    if (!f) { dg_error("font %d does not exist", font_id); return 0.0; }
    int lines = 1;
    if (text)
        for (const char *p = text; *p; p++) if (*p == '\n') lines++;
    return (double)f->line_height * lines;
}

int32_t dg_text_draw(int32_t font_id, float x, float y, const char *text, uint32_t color) {
    const DgFont *f = dg_font_at(font_id);
    if (!f) { dg_error("font %d does not exist", font_id); return -1; }
    if (!text || !text[0]) return 0;
    if (!dg_draw_ready()) { dg_error("text cannot draw: renderer not initialized"); return -1; }
    float pen_x = x, pen_y = y;
    int32_t drawn = 0;
    const char *p = text;
    for (;;) {
        const uint32_t cp = dg_utf8_next(&p);
        if (!cp) break;
        if (cp == '\n') { pen_x = x; pen_y += f->line_height; continue; }
        if (cp == '\r') continue;
        DgGlyph *g = dg_glyph_get(font_id, cp, 1);
        if (!g) return -1;
        if (g->w > 0 && g->h > 0) {
            /* 整数像素定位:位图是按整数像素光栅化的,和 quad 1:1 才不糊 */
            const float gx = (float)((int)(pen_x + 0.5f) + g->ox);
            const float gy = (float)((int)(pen_y + f->ascent + 0.5f) + g->oy);
            if (dg_draw_quad(g_atlas_tex, gx, gy, (float)g->w, (float)g->h,
                             g->u0, g->v0, g->u1, g->v1, color) == 0)
                drawn++;
        }
        pen_x += g->advance;
    }
    return drawn;
}
