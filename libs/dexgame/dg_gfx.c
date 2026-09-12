/* dg_gfx.c — D3D11 设备 / 窗口 / 交换链 / 渲染目标 / 呈现 / 回读 / 计时
 *
 * 设计要点(见 docs/DEXGAME_DESIGN.md §4):
 *   - flip 模型交换链(DXGI_SWAP_EFFECT_FLIP_DISCARD)+ Present(1,0) 垂直同步。
 *     本机显示器 2560x1600@165Hz,一帧预算 6.06ms —— 不能用 Sleep(16) 控帧,
 *     那会把 165Hz 锁死成 60Hz(现有 gal 就是这么做的)。
 *   - 进程 DPI 感知设为 PER_MONITOR_AWARE_V2。系统当前是 150% 缩放,
 *     不声明的话 2560x1600 会被拉伸成 1707x1067 再放大,画面直接糊掉。
 *   - 渲染目标统一用 B8G8R8A8_UNORM(窗口与离屏一致),这样回读只有一条代码路径。
 *   - 有窗口与无窗口(离屏)两种模式,离屏模式是测试断言的基础。
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "dexgame.h"
#include "dg_utf8.h"
#include "dg_guids.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 错误上报 ---------- */
static char g_err[DG_MAX_ERR];

void dg_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}

const char *dg_last_error(void) { return g_err; }
void dg_clear_error(void) { g_err[0] = '\0'; }

/* ---------- 状态 ---------- */
#define DG_RT_FORMAT DXGI_FORMAT_B8G8R8A8_UNORM

static DgMode g_mode = DG_MODE_NONE;
static ID3D11Device        *g_dev  = NULL;
static ID3D11DeviceContext *g_ctx  = NULL;
static IDXGISwapChain      *g_swap = NULL;
static ID3D11RenderTargetView *g_rtv = NULL;   /* 当前帧的渲染目标 */
static ID3D11Texture2D     *g_off_tex = NULL;  /* 离屏模式的目标纹理 */
static ID3D11Texture2D     *g_stage = NULL;    /* 回读用暂存纹理 */
static int g_stage_w = 0, g_stage_h = 0;
static HWND g_hwnd = NULL;
static int g_w = 0, g_h = 0;
static int g_running = 0;
static int g_vsync = 1;
static int g_resize_pending = 0;
static int g_class_registered = 0;
static int g_timer_period = 0;             /* timeBeginPeriod(1) 是否已生效 */
static LARGE_INTEGER g_freq, g_t0;
static double g_next_deadline_ms = 0.0;

static int dg_create_device_only(void);
static void dg_gfx_capture_gpu_name(void);

/* ---------- 计时 ---------- */
int64_t dg_gfx_now_ms(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart);
}

void dg_gfx_frame_pace(int target_fps) {
    /* VSync 打开时 Present 自己就会卡住帧率,这里不需要再睡。
       只有关掉 VSync(测性能/跑基准)时才用软件节流。 */
    if (target_fps <= 0 || (g_mode == DG_MODE_WINDOW && g_vsync)) {
        g_next_deadline_ms = 0.0;
        return;
    }
    const double step = 1000.0 / (double)target_fps;
    const double now = (double)dg_gfx_now_ms();
    if (g_next_deadline_ms <= 0.0) g_next_deadline_ms = now + step;
    while (now + 1.0 < g_next_deadline_ms) {
        Sleep(1);
    }
    g_next_deadline_ms += step;
    if (g_next_deadline_ms < now) g_next_deadline_ms = now + step;   /* 落后太多则重置 */
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK dg_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    /* 输入消息先交给 dg_input.c(它自己判断是否消费) */
    if (dg_input_on_message((unsigned int)msg, (uint64_t)wp, (int64_t)lp)) return 0;
    switch (msg) {
    case WM_CLOSE:
        g_running = 0;
        return 0;
    case WM_DESTROY:
        g_running = 0;
        return 0;
    case WM_SIZE:
        if (LOWORD(lp) > 0 && HIWORD(lp) > 0) g_resize_pending = 1;
        return 0;
    case WM_ERASEBKGND:
        /* 我们每帧整屏重绘,交默认处理会让 Windows 多刷一次背景(现有 gal 就是
           被这个坑到,WM_PAINT 里又整帧渲染了一遍)。 */
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------- DPI ---------- */
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef BOOL (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);

static void dg_enable_dpi_awareness(void) {
    /* PER_MONITOR_AWARE_V2 = -4。必须在创建任何窗口之前调用。
       若清单或宿主已经设过,这里会失败(E_ACCESSDENIED),不视为错误。 */
    HMODULE u = GetModuleHandleA("user32.dll");
    PFN_SetProcessDpiAwarenessContext fn =
        (PFN_SetProcessDpiAwarenessContext)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (fn) {
        fn((HANDLE)(intptr_t)-4);
        return;
    }
    /* 老系统回退:Win8.1 的 SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE=2) */
    HMODULE s = LoadLibraryA("shcore.dll");
    if (s) {
        typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
        PFN_SetProcessDpiAwareness f2 =
            (PFN_SetProcessDpiAwareness)(void *)GetProcAddress(s, "SetProcessDpiAwareness");
        if (f2) f2(2);
    }
}

/* ---------- 设备与交换链 ---------- */
static int dg_create_device_and_swapchain(HWND hwnd, int w, int h, int vsync) {
    DXGI_SWAP_CHAIN_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 2;
    sd.BufferDesc.Width = (UINT)w;
    sd.BufferDesc.Height = (UINT)h;
    sd.BufferDesc.Format = DG_RT_FORMAT;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;   /* flip 模型 */

    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                 D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 4, D3D11_SDK_VERSION,
        &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {
        dg_error("D3D11CreateDeviceAndSwapChain failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    g_vsync = vsync ? 1 : 0;
    return 0;
}

static int dg_create_backbuffer_rtv(void) {
    ID3D11Texture2D *back = NULL;
    HRESULT hr = g_swap->lpVtbl->GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (FAILED(hr) || !back) {
        dg_error("swapchain GetBuffer(0) failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    if (g_rtv) { g_rtv->lpVtbl->Release(g_rtv); g_rtv = NULL; }
    hr = g_dev->lpVtbl->CreateRenderTargetView(g_dev, (ID3D11Resource *)back, NULL, &g_rtv);
    back->lpVtbl->Release(back);
    if (FAILED(hr)) {
        dg_error("CreateRenderTargetView(backbuffer) failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    return 0;
}

static int dg_create_offscreen_target(int w, int h) {
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DG_RT_FORMAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g_dev->lpVtbl->CreateTexture2D(g_dev, &td, NULL, &g_off_tex);
    if (FAILED(hr)) {
        dg_error("CreateTexture2D(offscreen) failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    if (g_rtv) { g_rtv->lpVtbl->Release(g_rtv); g_rtv = NULL; }
    hr = g_dev->lpVtbl->CreateRenderTargetView(g_dev, (ID3D11Resource *)g_off_tex, NULL, &g_rtv);
    if (FAILED(hr)) {
        dg_error("CreateRenderTargetView(offscreen) failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    return 0;
}

static int dg_create_device_only(void) {
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                 D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = 0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 4,
                                   D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {
        dg_error("D3D11CreateDevice failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    return 0;
}

/* ---------- 适配器信息 ---------- */
static char g_gpu[256] = "";
static double g_vram_mb = 0.0;

/* 库不该往 stdout 打印,所以只把信息记下来,由 eng_gpu_name() 按需取。 */
static void dg_gfx_capture_gpu_name(void) {
    g_gpu[0] = '\0';
    if (!g_dev) return;
    IDXGIDevice *dxdev = NULL;
    if (FAILED(g_dev->lpVtbl->QueryInterface(g_dev, &DG_IID_IDXGIDevice, (void **)&dxdev)) || !dxdev)
        return;
    IDXGIAdapter *ad = NULL;
    if (SUCCEEDED(dxdev->lpVtbl->GetAdapter(dxdev, &ad)) && ad) {
        DXGI_ADAPTER_DESC d;
        if (SUCCEEDED(ad->lpVtbl->GetDesc(ad, &d))) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, g_gpu, sizeof g_gpu, NULL, NULL);
            g_vram_mb = d.DedicatedVideoMemory / 1048576.0;
        }
        ad->lpVtbl->Release(ad);
    }
    dxdev->lpVtbl->Release(dxdev);
}

const char *dg_gfx_gpu_name(void) { return g_gpu; }
double dg_gfx_vram_mb(void) { return g_vram_mb; }

/* ---------- 初始化 ---------- */
int dg_gfx_init_window(const char *title, int w, int h, int vsync) {
    if (g_mode != DG_MODE_NONE) {
        dg_error("already initialized");
        return -1;
    }
    if (w <= 0 || h <= 0) { dg_error("invalid size %dx%d", w, h); return -1; }

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    dg_enable_dpi_awareness();

    /* 1ms 粒度的 Sleep(关掉 VSync 时软件节流才准) */
    {
        HMODULE wm = LoadLibraryA("winmm.dll");
        if (wm) {
            typedef UINT (WINAPI *PFN_timeBeginPeriod)(UINT);
            PFN_timeBeginPeriod f =
                (PFN_timeBeginPeriod)(void *)GetProcAddress(wm, "timeBeginPeriod");
            if (f && f(1) == 0) g_timer_period = 1;
        }
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = dg_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"DexGameWindow";
    if (!g_class_registered) {
        if (!RegisterClassExW(&wc)) {
            dg_error("RegisterClassExW failed (err=%lu)", GetLastError());
            return -1;
        }
        g_class_registered = 1;
    }

    /* 客户区精确等于 w x h。注意当前是 150% DPI:必须先设 DPI 感知再算,
       否则算出来的是虚拟化后的尺寸,客户区会偏小。 */
    RECT rc = { 0, 0, w, h };
    DWORD style = WS_OVERLAPPEDWINDOW;
    {
        PFN_AdjustWindowRectExForDpi f = (PFN_AdjustWindowRectExForDpi)(void *)
            GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi");
        HDC screen = GetDC(NULL);
        UINT dpi = screen ? (UINT)GetDeviceCaps(screen, LOGPIXELSX) : 96;
        if (screen) ReleaseDC(NULL, screen);
        if (f) f(&rc, style, FALSE, 0, dpi);
        else AdjustWindowRect(&rc, style, FALSE);
    }

    wchar_t wtitle[256];
    if (!title) title = "dexgame";
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);

    g_hwnd = CreateWindowExW(0, L"DexGameWindow", wtitle, style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_hwnd) {
        dg_error("CreateWindowExW failed (err=%lu)", GetLastError());
        return -1;
    }

    if (dg_create_device_and_swapchain(g_hwnd, w, h, vsync)) return -1;
    if (dg_create_backbuffer_rtv()) return -1;

    g_w = w;
    g_h = h;
    g_mode = DG_MODE_WINDOW;
    g_running = 1;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    /* 记下适配器名,供 eng_gpu_name() 查询。
       注意**不要 printf**:这是个库,往 stdout 打印会污染游戏的输出
       (M1 测试就是被这一行坑到 —— 输出里多一行,所有按行断言全部错位)。 */
    dg_gfx_capture_gpu_name();
    return 0;
}

int dg_gfx_init_offscreen(int w, int h) {
    if (g_mode != DG_MODE_NONE) { dg_error("already initialized"); return -1; }
    if (w <= 0 || h <= 0) { dg_error("invalid size %dx%d", w, h); return -1; }

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    if (dg_create_device_only()) return -1;
    if (dg_create_offscreen_target(w, h)) return -1;

    g_w = w;
    g_h = h;
    g_mode = DG_MODE_OFFSCREEN;
    g_running = 1;
    dg_gfx_capture_gpu_name();
    return 0;
}

void dg_gfx_shutdown(void) {
    if (g_timer_period) {
        HMODULE wm = GetModuleHandleA("winmm.dll");
        if (wm) {
            typedef UINT (WINAPI *PFN_timeEndPeriod)(UINT);
            PFN_timeEndPeriod f = (PFN_timeEndPeriod)(void *)GetProcAddress(wm, "timeEndPeriod");
            if (f) f(1);
        }
        g_timer_period = 0;
    }
    if (g_ctx) g_ctx->lpVtbl->ClearState(g_ctx);
    if (g_stage) { g_stage->lpVtbl->Release(g_stage); g_stage = NULL; }
    g_stage_w = g_stage_h = 0;
    if (g_rtv) { g_rtv->lpVtbl->Release(g_rtv); g_rtv = NULL; }
    if (g_off_tex) { g_off_tex->lpVtbl->Release(g_off_tex); g_off_tex = NULL; }
    if (g_swap) {
        /* flip 模型要求退出前把 Fullscreen 状态清掉,否则下次创建设备可能失败 */
        g_swap->lpVtbl->SetFullscreenState(g_swap, FALSE, NULL);
        g_swap->lpVtbl->Release(g_swap);
        g_swap = NULL;
    }
    if (g_ctx) { g_ctx->lpVtbl->Release(g_ctx); g_ctx = NULL; }
    if (g_dev) { g_dev->lpVtbl->Release(g_dev); g_dev = NULL; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    g_mode = DG_MODE_NONE;
    g_running = 0;
    g_w = g_h = 0;
}

/* ---------- 帧 ---------- */
int dg_gfx_running(void) { return g_running; }
int dg_gfx_width(void) { return g_w; }
int dg_gfx_height(void) { return g_h; }
int dg_gfx_mode(void) { return (int)g_mode; }
void dg_gfx_set_vsync(int on) { g_vsync = on ? 1 : 0; }
void dg_gfx_request_close(void) { g_running = 0; }

void dg_gfx_get_device(void **out_dev, void **out_ctx) {
    if (out_dev) *out_dev = (void *)g_dev;
    if (out_ctx) *out_ctx = (void *)g_ctx;
}

static void dg_resize(int w, int h) {
    if (g_mode != DG_MODE_WINDOW || w <= 0 || h <= 0) return;
    if (w == g_w && h == g_h) return;
    if (g_rtv) { g_rtv->lpVtbl->Release(g_rtv); g_rtv = NULL; }
    g_ctx->lpVtbl->OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    HRESULT hr = g_swap->lpVtbl->ResizeBuffers(g_swap, 0, (UINT)w, (UINT)h,
                                              DG_RT_FORMAT, 0);
    if (FAILED(hr)) {
        dg_error("ResizeBuffers failed (hr=0x%08lX)", (unsigned long)hr);
        return;
    }
    if (dg_create_backbuffer_rtv()) return;
    g_w = w;
    g_h = h;
    if (g_stage) { g_stage->lpVtbl->Release(g_stage); g_stage = NULL; g_stage_w = g_stage_h = 0; }
}

void dg_gfx_pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { g_running = 0; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_resize_pending) {
        g_resize_pending = 0;
        RECT rc;
        if (g_hwnd && GetClientRect(g_hwnd, &rc))
            dg_resize(rc.right - rc.left, rc.bottom - rc.top);
    }
}

void dg_gfx_bind_target(void) {
    if (!g_rtv || !g_ctx) return;
    g_ctx->lpVtbl->OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (FLOAT)g_w;
    vp.Height = (FLOAT)g_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g_ctx->lpVtbl->RSSetViewports(g_ctx, 1, &vp);
}

void dg_gfx_clear(uint32_t argb) {
    if (!g_rtv || !g_ctx) return;
    const float a = ((argb >> 24) & 0xFF) / 255.0f;
    const float r = ((argb >> 16) & 0xFF) / 255.0f;
    const float g = ((argb >> 8) & 0xFF) / 255.0f;
    const float b = (argb & 0xFF) / 255.0f;
    FLOAT c[4] = { r, g, b, a };
    g_ctx->lpVtbl->ClearRenderTargetView(g_ctx, g_rtv, c);
}

void dg_gfx_present(void) {
    if (g_mode == DG_MODE_WINDOW && g_swap) {
        HRESULT hr = g_swap->lpVtbl->Present(g_swap, g_vsync ? 1 : 0, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            dg_error("device removed/reset (hr=0x%08lX) —— 需要重建设备", (unsigned long)hr);
            g_running = 0;
        }
    }
}

/* ---------- 回读 ---------- */
static int dg_ensure_stage(void) {
    if (g_stage && g_stage_w == g_w && g_stage_h == g_h) return 0;
    if (g_stage) { g_stage->lpVtbl->Release(g_stage); g_stage = NULL; }
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)g_w;
    td.Height = (UINT)g_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DG_RT_FORMAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = g_dev->lpVtbl->CreateTexture2D(g_dev, &td, NULL, &g_stage);
    if (FAILED(hr)) {
        dg_error("CreateTexture2D(staging) failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    g_stage_w = g_w;
    g_stage_h = g_h;
    return 0;
}

/* 把当前渲染目标拷进暂存纹理并映射;调用方负责 Unmap。 */
static int dg_map_target(D3D11_MAPPED_SUBRESOURCE *out) {
    if (!g_ctx) { dg_error("not initialized"); return -1; }
    if (dg_ensure_stage()) return -1;
    ID3D11Texture2D *src = NULL;
    if (g_mode == DG_MODE_WINDOW) {
        if (g_swap->lpVtbl->GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&src) != S_OK || !src) {
            dg_error("GetBuffer(0) failed");
            return -1;
        }
    } else {
        src = g_off_tex;
    }
    g_ctx->lpVtbl->CopyResource(g_ctx, (ID3D11Resource *)g_stage, (ID3D11Resource *)src);
    if (g_mode == DG_MODE_WINDOW) src->lpVtbl->Release(src);
    /* 离屏的 CopyResource 之后要等 GPU 做完才能 Map,否则可能读到未完成的内容 */
    g_ctx->lpVtbl->Flush(g_ctx);
    HRESULT hr = g_ctx->lpVtbl->Map(g_ctx, (ID3D11Resource *)g_stage, 0,
                                    D3D11_MAP_READ, 0, out);
    if (FAILED(hr)) {
        dg_error("Map(staging) failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    return 0;
}

int64_t dg_gfx_read_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) {
        dg_error("pixel (%d,%d) out of range %dx%d", x, y, g_w, g_h);
        return -1;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (dg_map_target(&m)) return -1;
    const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
    const uint8_t *px = row + (size_t)x * 4;
    const uint32_t bgra = (uint32_t)px[0] | ((uint32_t)px[1] << 8) |
                          ((uint32_t)px[2] << 16) | ((uint32_t)px[3] << 24);
    g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_stage, 0);
    /* 统一成 0xAARRGGBB */
    const uint32_t a = (bgra >> 24) & 0xFF;
    const uint32_t r = (bgra >> 16) & 0xFF;
    const uint32_t g = (bgra >> 8) & 0xFF;
    const uint32_t b = bgra & 0xFF;
    return (int64_t)((a << 24) | (r << 16) | (g << 8) | b);
}

int dg_gfx_save_bmp(const char *path) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (dg_map_target(&m)) return -1;

    const int rowbytes = g_w * 3;
    const int pad = (4 - (rowbytes % 4)) % 4;
    const int stride = rowbytes + pad;
    const uint32_t datalen = (uint32_t)stride * (uint32_t)g_h;
    const uint32_t filesize = 54 + datalen;

    FILE *f = dg_fopen_asset(path, "wb");
    if (!f) {
        dg_error("cannot open '%s' for writing", path);
        g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_stage, 0);
        return -1;
    }
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesize, 4);
    const uint32_t off = 54;
    memcpy(hdr + 10, &off, 4);
    const uint32_t ih = 40;
    memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &g_w, 4);
    memcpy(hdr + 22, &g_h, 4);
    const uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &datalen, 4);
    fwrite(hdr, 1, sizeof hdr, f);

    uint8_t *line = (uint8_t *)malloc((size_t)stride);
    if (!line) {
        dg_error("out of memory for BMP row");
        fclose(f);
        g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_stage, 0);
        return -1;
    }
    memset(line, 0, (size_t)stride);
    for (int y = g_h - 1; y >= 0; y--) {          /* BMP 是自底向上 */
        const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        for (int x = 0; x < g_w; x++) {
            const uint8_t *px = row + (size_t)x * 4;
            line[x * 3 + 0] = px[0];              /* B */
            line[x * 3 + 1] = px[1];              /* G */
            line[x * 3 + 2] = px[2];              /* R */
        }
        fwrite(line, 1, (size_t)stride, f);
    }
    free(line);
    fclose(f);
    g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_stage, 0);
    return 0;
}
