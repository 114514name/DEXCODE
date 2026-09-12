/* dg_draw.c — HLSL 管线 / 2D 精灵批渲染 / 纹理(含 stb_image 解码)
 *
 * 为什么用批渲染:2D 游戏的绘制是"同纹理的一堆四边形",逐次 draw call 会让
 * 驱动开销成为瓶颈。这里把四边形累积到一个动态顶点缓冲,遇到纹理切换或缓冲
 * 将满时才提交一次 DrawIndexed —— 实测形态是"每纹理每帧 1 次 draw"。
 *
 * 顶点格式:pos(2f) + uv(2f) + rgba(4B) = 20 字节。
 * 颜色在顶点里以 **R,G,B,A 的字节序**存放(对应 R8G8B8A8_UNORM),故打包时
 * 要把外部的 0xAARRGGBB 重新排列 —— 见 dg_pack()。
 *
 * 着色器在运行时用 d3dcompiler_47.dll 编译(Windows 自带)。zig 不提供
 * d3dcompiler 的导入库,故走 LoadLibrary + GetProcAddress,见 dg_d3dcompile()。
 */
#include <windows.h>
#include <d3d11.h>

#include "dexgame.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_image 只在 gal 里 vendor 了一份(283KB),这里复用而不是再抄一份。
   只用它做内存解码(STBI 实现放在本 TU,与 gal 互不影响)。 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "../gal/stb_image.h"

/* ---------- 设备(从 dg_gfx.c 取一次,缓存下来) ---------- */
static ID3D11Device        *g_dev = NULL;
static ID3D11DeviceContext *g_ctx = NULL;

static void dg_fetch_device(void) {
    void *d = NULL, *c = NULL;
    dg_gfx_get_device(&d, &c);
    g_dev = (ID3D11Device *)d;
    g_ctx = (ID3D11DeviceContext *)c;
}

/* ---------- 运行时着色器编译 ---------- */
typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static PFN_D3DCompile dg_d3dcompile(void) {
    static PFN_D3DCompile fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (!m) m = LoadLibraryA("d3dcompiler.dll");
        if (m) fn = (PFN_D3DCompile)(void *)GetProcAddress(m, "D3DCompile");
    }
    return fn;
}

/* 2D 正交投影:像素坐标 (0,0)-(w,h) 映射到 NDC。
   顶点着色器用 mul(float4(pos,0,1), proj) —— 行向量左乘,故矩阵声明 row_major。 */
static const char *DG_HLSL =
    "cbuffer Params : register(b0) { row_major float4x4 proj; };\n"
    "struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0;"
    " float4 col : COLOR0; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0;"
    " float4 col : COLOR0; };\n"
    "VSOut vs_main(VSIn i) {\n"
    "  VSOut o;\n"
    "  o.pos = mul(float4(i.pos, 0.0, 1.0), proj);\n"
    "  o.uv  = i.uv;\n"
    "  o.col = i.col;\n"
    "  return o;\n"
    "}\n"
    "Texture2D    t0 : register(t0);\n"
    "SamplerState s0 : register(s0);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "  return t0.Sample(s0, i.uv) * i.col;\n"
    "}\n";

/* ---------- 顶点 ---------- */
#pragma pack(push, 1)
typedef struct {
    float x, y;
    float u, v;
    uint8_t r, g, b, a;
} DgVert;                        /* 20 字节 */
#pragma pack(pop)

/* 外部颜色是 0xAARRGGBB;顶点里要按 R,G,B,A 字节序存(R8G8B8A8_UNORM) */
static void dg_pack(DgVert *v, float x, float y, float u, float vv, uint32_t argb) {
    v->x = x; v->y = y; v->u = u; v->v = vv;
    v->r = (uint8_t)((argb >> 16) & 0xFF);
    v->g = (uint8_t)((argb >> 8) & 0xFF);
    v->b = (uint8_t)(argb & 0xFF);
    v->a = (uint8_t)((argb >> 24) & 0xFF);
}

/* ---------- 纹理表(带代际的句柄) ---------- */
typedef struct {
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    int width, height;
    uint32_t gen;                 /* 每次释放 +1,使旧 id 自动失效 */
} DgTex;

static DgTex g_texs[DG_MAX_TEXTURES];
static uint32_t g_tex_gen[DG_MAX_TEXTURES];

static int dg_tex_slot(int id, int *out_idx) {
    if (id <= 0) return -1;
    const int idx = (id & 0xFFF) - 1;                 /* 1 基,故 0 表示无效句柄 */
    const uint32_t gen = (uint32_t)(id >> 12);
    if (idx < 0 || idx >= DG_MAX_TEXTURES) return -1;
    if (g_tex_gen[idx] != gen) return -1;             /* 代际不符 = 过期句柄 */
    if (!g_texs[idx].tex) return -1;
    if (out_idx) *out_idx = idx;
    return 0;
}

static int dg_tex_new_slot(void) {
    for (int i = 0; i < DG_MAX_TEXTURES; i++)
        if (!g_texs[i].tex) return i;
    dg_error("texture slots exhausted (max %d)", DG_MAX_TEXTURES);
    return -1;
}

int dg_tex_valid(int id) { return dg_tex_slot(id, NULL) == 0; }

int dg_tex_width(int id) {
    int i;
    if (dg_tex_slot(id, &i)) { dg_error("invalid texture handle %d", id); return -1; }
    return g_texs[i].width;
}

int dg_tex_height(int id) {
    int i;
    if (dg_tex_slot(id, &i)) { dg_error("invalid texture handle %d", id); return -1; }
    return g_texs[i].height;
}

static int dg_tex_upload(int w, int h, const uint8_t *rgba, const char *what) {
    if (w <= 0 || h <= 0) { dg_error("%s: invalid size %dx%d", what, w, h); return -1; }
    const int slot = dg_tex_new_slot();
    if (slot < 0) return -1;

    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    /* 源数据是 RGBA(stb_image 输出),故用 R8G8B8A8(渲染目标才是 B8G8R8A8) */
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof sr);
    sr.pSysMem = rgba;
    sr.SysMemPitch = (UINT)w * 4;

    if (!g_dev) { dg_error("no D3D11 device"); return -1; }
    HRESULT hr = g_dev->lpVtbl->CreateTexture2D(g_dev, &td, &sr, &g_texs[slot].tex);
    if (FAILED(hr)) {
        dg_error("%s: CreateTexture2D failed (hr=0x%08lX)", what, (unsigned long)hr);
        g_texs[slot].tex = NULL;
        return -1;
    }
    hr = g_dev->lpVtbl->CreateShaderResourceView(g_dev, (ID3D11Resource *)g_texs[slot].tex,
                                                NULL, &g_texs[slot].srv);
    if (FAILED(hr)) {
        dg_error("%s: CreateShaderResourceView failed (hr=0x%08lX)", what, (unsigned long)hr);
        g_texs[slot].tex->lpVtbl->Release(g_texs[slot].tex);
        g_texs[slot].tex = NULL;
        return -1;
    }
    g_texs[slot].width = w;
    g_texs[slot].height = h;
    if (g_tex_gen[slot] == 0) g_tex_gen[slot] = 1;
    return (int)((g_tex_gen[slot] << 12) | (uint32_t)(slot + 1));
}

int dg_tex_create_rgba(int w, int h, const uint8_t *rgba) {
    return dg_tex_upload(w, h, rgba, "create_rgba");
}

int dg_tex_free(int id) {
    int i;
    if (dg_tex_slot(id, &i)) { dg_error("invalid texture handle %d", id); return -1; }
    if (g_texs[i].srv) { g_texs[i].srv->lpVtbl->Release(g_texs[i].srv); g_texs[i].srv = NULL; }
    if (g_texs[i].tex) { g_texs[i].tex->lpVtbl->Release(g_texs[i].tex); g_texs[i].tex = NULL; }
    g_texs[i].width = g_texs[i].height = 0;
    g_tex_gen[i]++;                       /* 使所有指向本槽的旧句柄失效 */
    if (g_tex_gen[i] == 0) g_tex_gen[i] = 1;
    return 0;
}

int dg_tex_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { dg_error("cannot open image '%s'", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); dg_error("image '%s' is empty", path); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); dg_error("out of memory reading '%s'", path); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f);
        dg_error("short read on image '%s'", path);
        return -1;
    }
    fclose(f);

    int w = 0, h = 0, comp = 0;
    /* 强制 4 通道,统一成 RGBA */
    uint8_t *px = stbi_load_from_memory(buf, (int)n, &w, &h, &comp, 4);
    free(buf);
    if (!px) {
        dg_error("cannot decode image '%s' (supported: PNG/JPEG/BMP/GIF)", path);
        return -1;
    }
    const int id = dg_tex_upload(w, h, px, path);
    stbi_image_free(px);
    return id;
}

/* ---------- 按路径的纹理缓存 ----------
   现有 gal 每帧重新解码图片(见 AGENTS §4 列的坑),这里必须避免:
   同一路径只解码一次。缓存**不持有引用计数** —— 若句柄被显式释放,下次命中时
   检测到失效就重载,调用方的语义因此仍然正确。 */
#define DG_TEX_CACHE_MAX 256
typedef struct { char path[DG_PATH_MAX]; int id; } DgTexCacheEnt;
static DgTexCacheEnt g_tex_cache[DG_TEX_CACHE_MAX];
static int g_tex_cache_n = 0;

int dg_tex_load_cached(const char *path) {
    if (!path || !path[0]) { dg_error("empty texture path"); return -1; }
    for (int i = 0; i < g_tex_cache_n; i++) {
        if (strcmp(g_tex_cache[i].path, path) != 0) continue;
        if (dg_tex_valid(g_tex_cache[i].id)) return g_tex_cache[i].id;
        const int id = dg_tex_load_file(path);      /* 句柄已失效:重载并更新 */
        g_tex_cache[i].id = id;
        return id;
    }
    const int id = dg_tex_load_file(path);
    if (id < 0) return -1;
    if (g_tex_cache_n >= DG_TEX_CACHE_MAX) {
        dg_error("texture cache full (%d entries); '%s' loaded but not cached",
                 DG_TEX_CACHE_MAX, path);
        return id;
    }
    snprintf(g_tex_cache[g_tex_cache_n].path, DG_PATH_MAX, "%s", path);
    g_tex_cache[g_tex_cache_n].id = id;
    g_tex_cache_n++;
    return id;
}

void dg_tex_cache_clear(void) { g_tex_cache_n = 0; }

/* ---------- 管线对象 ---------- */
static int g_draw_ready = 0;
static ID3D11VertexShader      *g_vs = NULL;
static ID3D11PixelShader       *g_ps = NULL;
static ID3D11InputLayout       *g_il = NULL;
static ID3D11Buffer            *g_cb = NULL;     /* 投影矩阵 */
static ID3D11Buffer            *g_vb = NULL;     /* 动态顶点缓冲 */
static ID3D11BlendState        *g_blend = NULL;
static ID3D11SamplerState      *g_samp = NULL;
static ID3D11RasterizerState   *g_rast = NULL;
static ID3D11DepthStencilState *g_depth = NULL;
static int g_white_tex = 0;                      /* 1x1 白纹理(纯色矩形用)*/

static DgVert *g_verts = NULL;
static int g_vcount = 0;
static int g_batch_tex = 0;                      /* 当前批次绑定的纹理 */

static int dg_compile(const char *entry, const char *target, ID3DBlob **out) {
    PFN_D3DCompile comp = dg_d3dcompile();
    if (!comp) {
        dg_error("d3dcompiler_47.dll not available —— cannot compile shaders");
        return -1;
    }
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = comp(DG_HLSL, strlen(DG_HLSL), "dexgame.hlsl", NULL, NULL,
                      entry, target, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        dg_error("shader '%s' compile failed: %s", entry,
                 err ? (const char *)err->lpVtbl->GetBufferPointer(err) : "(no message)");
        if (err) err->lpVtbl->Release(err);
        return -1;
    }
    if (err) err->lpVtbl->Release(err);
    *out = blob;
    return 0;
}

int dg_draw_init(void) {
    if (g_draw_ready) return 0;
    dg_fetch_device();
    if (!g_dev) { dg_error("dg_draw_init before gfx init"); return -1; }

    ID3DBlob *vsb = NULL, *psb = NULL;
    if (dg_compile("vs_main", "vs_4_0", &vsb)) return -1;
    if (dg_compile("ps_main", "ps_4_0", &psb)) { vsb->lpVtbl->Release(vsb); return -1; }

    const void *vbytes = vsb->lpVtbl->GetBufferPointer(vsb);
    SIZE_T vlen = vsb->lpVtbl->GetBufferSize(vsb);
    HRESULT hr = g_dev->lpVtbl->CreateVertexShader(g_dev, vbytes, vlen, NULL, &g_vs);
    if (FAILED(hr)) { dg_error("CreateVertexShader failed (hr=0x%08lX)", (unsigned long)hr); return -1; }

    D3D11_INPUT_ELEMENT_DESC ie[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = g_dev->lpVtbl->CreateInputLayout(g_dev, ie, 3, vbytes, vlen, &g_il);
    vsb->lpVtbl->Release(vsb);
    if (FAILED(hr)) { dg_error("CreateInputLayout failed (hr=0x%08lX)", (unsigned long)hr); return -1; }

    const void *pbytes = psb->lpVtbl->GetBufferPointer(psb);
    SIZE_T plen = psb->lpVtbl->GetBufferSize(psb);
    hr = g_dev->lpVtbl->CreatePixelShader(g_dev, pbytes, plen, NULL, &g_ps);
    psb->lpVtbl->Release(psb);
    if (FAILED(hr)) { dg_error("CreatePixelShader failed (hr=0x%08lX)", (unsigned long)hr); return -1; }

    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 64;                       /* float4x4 */
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dev->lpVtbl->CreateBuffer(g_dev, &bd, NULL, &g_cb);
    if (FAILED(hr)) { dg_error("CreateBuffer(cb) failed"); return -1; }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (UINT)(sizeof(DgVert) * DG_MAX_VERTS);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dev->lpVtbl->CreateBuffer(g_dev, &bd, NULL, &g_vb);
    if (FAILED(hr)) { dg_error("CreateBuffer(vb %u bytes) failed", bd.ByteWidth); return -1; }

    D3D11_BLEND_DESC bld;
    memset(&bld, 0, sizeof bld);
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_dev->lpVtbl->CreateBlendState(g_dev, &bld, &g_blend);
    if (FAILED(hr)) { dg_error("CreateBlendState failed"); return -1; }

    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_dev->lpVtbl->CreateSamplerState(g_dev, &sd, &g_samp);
    if (FAILED(hr)) { dg_error("CreateSamplerState failed"); return -1; }

    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    hr = g_dev->lpVtbl->CreateRasterizerState(g_dev, &rd, &g_rast);
    if (FAILED(hr)) { dg_error("CreateRasterizerState failed"); return -1; }

    /* 2D 不需要深度/模板;显式建一个全关的状态,免得残留状态影响绘制 */
    D3D11_DEPTH_STENCIL_DESC dsd;
    memset(&dsd, 0, sizeof dsd);
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    hr = g_dev->lpVtbl->CreateDepthStencilState(g_dev, &dsd, &g_depth);
    if (FAILED(hr)) { dg_error("CreateDepthStencilState failed"); return -1; }

    g_verts = (DgVert *)malloc(sizeof(DgVert) * DG_MAX_VERTS);
    if (!g_verts) { dg_error("out of memory for vertex staging"); return -1; }

    /* 1x1 白纹理:纯色矩形复用它,省一套"无纹理"着色器路径 */
    {
        const uint8_t white[4] = { 255, 255, 255, 255 };
        g_white_tex = dg_tex_create_rgba(1, 1, white);
        if (g_white_tex < 0) return -1;
    }
    g_draw_ready = 1;
    return 0;
}

void dg_draw_shutdown(void) {
    dg_tex_cache_clear();
    if (g_white_tex > 0) { dg_tex_free(g_white_tex); g_white_tex = 0; }
    for (int i = 0; i < DG_MAX_TEXTURES; i++) {
        if (g_texs[i].srv) { g_texs[i].srv->lpVtbl->Release(g_texs[i].srv); g_texs[i].srv = NULL; }
        if (g_texs[i].tex) { g_texs[i].tex->lpVtbl->Release(g_texs[i].tex); g_texs[i].tex = NULL; }
    }
    if (g_depth) { g_depth->lpVtbl->Release(g_depth); g_depth = NULL; }
    if (g_rast) { g_rast->lpVtbl->Release(g_rast); g_rast = NULL; }
    if (g_samp) { g_samp->lpVtbl->Release(g_samp); g_samp = NULL; }
    if (g_blend) { g_blend->lpVtbl->Release(g_blend); g_blend = NULL; }
    if (g_vb) { g_vb->lpVtbl->Release(g_vb); g_vb = NULL; }
    if (g_cb) { g_cb->lpVtbl->Release(g_cb); g_cb = NULL; }
    if (g_il) { g_il->lpVtbl->Release(g_il); g_il = NULL; }
    if (g_ps) { g_ps->lpVtbl->Release(g_ps); g_ps = NULL; }
    if (g_vs) { g_vs->lpVtbl->Release(g_vs); g_vs = NULL; }
    free(g_verts); g_verts = NULL;
    g_vcount = 0;
    g_batch_tex = 0;
    g_draw_ready = 0;
    g_dev = NULL;
    g_ctx = NULL;
}

/* 提交当前批次 */
static void dg_flush(void) {
    if (g_vcount == 0) return;
    if (!g_ctx) { g_vcount = 0; return; }

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->lpVtbl->Map(g_ctx, (ID3D11Resource *)g_vb, 0,
                                 D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        dg_error("Map(vertex buffer) failed");
        g_vcount = 0;
        return;
    }
    memcpy(m.pData, g_verts, sizeof(DgVert) * (size_t)g_vcount);
    g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);

    ID3D11ShaderResourceView *srv = NULL;
    int ti = -1;
    if (g_batch_tex > 0 && dg_tex_slot(g_batch_tex, &ti) == 0) srv = g_texs[ti].srv;

    UINT stride = sizeof(DgVert), offset = 0;
    g_ctx->lpVtbl->IASetInputLayout(g_ctx, g_il);
    g_ctx->lpVtbl->IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &offset);
    g_ctx->lpVtbl->IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->lpVtbl->VSSetShader(g_ctx, g_vs, NULL, 0);
    g_ctx->lpVtbl->VSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    g_ctx->lpVtbl->PSSetShader(g_ctx, g_ps, NULL, 0);
    g_ctx->lpVtbl->PSSetShaderResources(g_ctx, 0, 1, &srv);
    g_ctx->lpVtbl->PSSetSamplers(g_ctx, 0, 1, &g_samp);
    g_ctx->lpVtbl->OMSetBlendState(g_ctx, g_blend, NULL, 0xFFFFFFFF);
    g_ctx->lpVtbl->OMSetDepthStencilState(g_ctx, g_depth, 0);
    g_ctx->lpVtbl->RSSetState(g_ctx, g_rast);
    g_ctx->lpVtbl->Draw(g_ctx, (UINT)g_vcount, 0);
    g_vcount = 0;
}

void dg_draw_frame_begin(void) {
    g_vcount = 0;
    g_batch_tex = 0;
    if (!g_draw_ready) return;
    if (!g_ctx) return;

    /* 每帧刷新投影(窗口可能被改过尺寸) */
    const float w = (float)dg_gfx_width();
    const float h = (float)dg_gfx_height();
    float mat[16] = {
        2.0f / w, 0.0f,      0.0f, 0.0f,
        0.0f,    -2.0f / h,  0.0f, 0.0f,
        0.0f,     0.0f,      1.0f, 0.0f,
       -1.0f,     1.0f,      0.0f, 1.0f,
    };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->lpVtbl->Map(g_ctx, (ID3D11Resource *)g_cb, 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, mat, sizeof mat);
        g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
    }
}

void dg_draw_frame_end(void) {
    dg_flush();
}

int dg_draw_quad(int tex, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, uint32_t color) {
    if (!g_draw_ready) { dg_error("dg_draw_quad before dg_draw_init"); return -1; }
    if (tex != g_batch_tex) {
        dg_flush();
        g_batch_tex = tex;
    }
    if (g_vcount + 6 > DG_MAX_VERTS) dg_flush();

    DgVert *v = g_verts + g_vcount;
    dg_pack(v + 0, x,     y,     u0, v0, color);
    dg_pack(v + 1, x + w, y,     u1, v0, color);
    dg_pack(v + 2, x + w, y + h, u1, v1, color);
    dg_pack(v + 3, x,     y,     u0, v0, color);
    dg_pack(v + 4, x + w, y + h, u1, v1, color);
    dg_pack(v + 5, x,     y + h, u0, v1, color);
    g_vcount += 6;
    return 0;
}

int dg_draw_rect(float x, float y, float w, float h, uint32_t color) {
    /* g_white_tex 是 1x1,uv 全取中心即可 */
    return dg_draw_quad(g_white_tex, x, y, w, h, 0.5f, 0.5f, 0.5f, 0.5f, color);
}
