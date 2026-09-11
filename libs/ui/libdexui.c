/*
 * libdexui.c — DexLang 的 WinAPI UI 库(纯 C 实现,零依赖,无 Python)
 *
 * 用 Win32 API 创建窗口与控件(标签/按钮/输入框),供 DexLang 通过
 * include/refer 调用。所有导出函数签名都落在 C VM(vm.c) FFI 支持
 * 集合内(参数 ≤ 3)。
 *
 * 使用模式(交互):
 *   include "ui";
 *   let win = dex_ui_create("标题", 340, 180);
 *   let lbl = dex_ui_label(win, 12);
 *   let ed  = dex_ui_edit(win, 44);
 *   let btn = dex_ui_button(win, 80);
 *   dex_ui_set_text(btn, "点我");
 *   while dex_ui_running(win) {
 *       let e = dex_ui_poll(win);          // 泵消息,返回被点击的按钮 id
 *       if e == btn { dex_ui_set_text(lbl, "你好, " + dex_ui_get_text(ed)); }
 *   }
 *
 * 或纯显示:
 *   dex_ui_create(...); dex_ui_label(...); ... dex_ui_run(win);
 *
 * 构建(Windows,zig):
 *   zig cc -shared -target x86_64-windows-gnu -O2 \
 *       -o libs/ui/libdexui.dll libs/ui/libdexui.c
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#define EXPORT __declspec(dllexport)

#define MAX_WIN   8
#define MAX_CTRL  64

/* 一个控件:子窗口句柄 + 唯一控件 id(从 1 开始,0 保留) */
typedef struct { HWND hwnd; int id; } Ctrl;

/* 一个窗口实例 */
typedef struct {
    HWND hwnd;
    int running;            /* 窗口是否还在(1/0) */
    Ctrl ctrls[MAX_CTRL];
    int nctrls;
    int next_ctrl_id;
} Win;

static Win g_wins[MAX_WIN];
static int g_next_win_id = 1;   /* 窗口 id 从 1 开始 */
static int g_last_event = 0;    /* 最近点击的控件 id;0=无;-1=窗口已关闭 */

/* ---------- UTF-8 <-> UTF-16(共享缓冲;VM 会 xstrdup 复制返回值) ---------- */
static char     g_buf[1 << 15];
static wchar_t  g_wbuf[1 << 13];

static const wchar_t *to_wide(const char *utf8) {
    if (!utf8) utf8 = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1,
                                g_wbuf, (int)(sizeof g_wbuf / sizeof(wchar_t)));
    if (n <= 0) g_wbuf[0] = 0;
    return g_wbuf;
}

static const char *to_utf8(const wchar_t *w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                g_buf, (int)sizeof g_buf, NULL, NULL);
    if (n <= 0) g_buf[0] = 0;
    return g_buf;
}

/* ---------- 窗口过程 ---------- */
static Win *win_of(HWND h) {
    return (Win *)GetWindowLongPtrW(h, GWLP_USERDATA);
}

static LRESULT CALLBACK winproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_COMMAND:
        /* 仅按钮点击(BN_CLICKED)记为事件,其余控件变化不打扰程序 */
        if (HIWORD(w) == BN_CLICKED)
            g_last_event = (int)LOWORD(w);
        return 0;
    case WM_DESTROY:
        if (win_of(h)) win_of(h)->running = 0;
        g_last_event = -1;
        return 0;
        /* 不调用 PostQuitMessage:避免 WM_QUIT 残留在消息队列里,
           污染同一线程后续窗口的 poll 事件 */
    default:
        return DefWindowProcW(h, msg, w, l);
    }
}

/* ---------- 内部工具 ---------- */
static Win *get_win(int64_t id) {
    if (id < 1 || id > MAX_WIN) return NULL;
    Win *wn = &g_wins[id - 1];
    return wn->hwnd ? wn : NULL;
}

/* 创建子控件并分配控件 id。关键:把 cid 作为 HMENU 传给 CreateWindowExW,
   这样 GetDlgCtrlID 与返回给调用方的 id 一致,WM_COMMAND 才能正确对应。 */
static int create_child(Win *wn, const wchar_t *cls, DWORD style, DWORD exstyle,
                        int x, int y, int w, int h) {
    if (wn->nctrls >= MAX_CTRL) return -1;
    int cid = wn->next_ctrl_id++;
    HWND ch = CreateWindowExW(exstyle, cls, L"", WS_CHILD | WS_VISIBLE | style,
                              x, y, w, h, wn->hwnd, (HMENU)(INT_PTR)cid,
                              GetModuleHandleW(NULL), NULL);
    if (!ch) return -1;
    wn->ctrls[wn->nctrls].hwnd = ch;
    wn->ctrls[wn->nctrls].id = cid;
    wn->nctrls++;
    return cid;
}

static HWND find_ctrl(int64_t cid) {
    for (int i = 0; i < MAX_WIN; i++) {
        Win *wn = &g_wins[i];
        for (int j = 0; j < wn->nctrls; j++)
            if (wn->ctrls[j].id == (int)cid) return wn->ctrls[j].hwnd;
    }
    return NULL;
}

/* 客户区宽度(用于布局) */
static int client_w(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    return rc.right;
}

/* ---------- 导出 API ---------- */

/* string,int,int -> int:创建主窗口(标题、宽、高),返回窗口 id */
EXPORT int64_t dex_ui_create(const char *title, int64_t w, int64_t h) {
    int id = g_next_win_id++;
    if (id > MAX_WIN) return -1;
    Win *wn = &g_wins[id - 1];
    memset(wn, 0, sizeof *wn);
    wn->next_ctrl_id = 1;

    wchar_t cls[32];
    swprintf(cls, 32, L"dexui_w%d", id);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = winproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    if (!RegisterClassW(&wc)) return -1;

    wn->hwnd = CreateWindowExW(0, cls, to_wide(title), WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, (int)w, (int)h,
                               NULL, NULL, wc.hInstance, NULL);
    if (!wn->hwnd) return -1;
    SetWindowLongPtrW(wn->hwnd, GWLP_USERDATA, (LONG_PTR)wn);
    wn->running = 1;
    ShowWindow(wn->hwnd, SW_SHOW);
    UpdateWindow(wn->hwnd);
    return id;
}

/* int,int -> int:添加标签(win, y),返回控件 id */
EXPORT int64_t dex_ui_label(int64_t win, int64_t y) {
    Win *wn = get_win(win);
    if (!wn) return -1;
    int cw = client_w(wn->hwnd);
    return create_child(wn, L"STATIC", 0, 0, 12, (int)y, cw - 140, 24);
}

/* int,int -> int:添加按钮(win, y),返回控件 id(点击产生事件) */
EXPORT int64_t dex_ui_button(int64_t win, int64_t y) {
    Win *wn = get_win(win);
    if (!wn) return -1;
    int cw = client_w(wn->hwnd);
    return create_child(wn, L"BUTTON", BS_PUSHBUTTON, 0, cw - 118, (int)y, 104, 24);
}

/* int,int -> int:添加输入框(win, y),返回控件 id */
EXPORT int64_t dex_ui_edit(int64_t win, int64_t y) {
    Win *wn = get_win(win);
    if (!wn) return -1;
    int cw = client_w(wn->hwnd);
    return create_child(wn, L"EDIT", WS_BORDER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                        12, (int)y, cw - 24, 24);
}

/* int,string -> int:设置控件文本;成功 0,失败 -1 */
EXPORT int64_t dex_ui_set_text(int64_t control, const char *text) {
    HWND h = find_ctrl(control);
    if (!h) return -1;
    SetWindowTextW(h, to_wide(text));
    return 0;
}

/* int -> string:读取控件文本 */
EXPORT const char *dex_ui_get_text(int64_t control) {
    HWND h = find_ctrl(control);
    if (!h) return "";
    GetWindowTextW(h, g_wbuf, (int)(sizeof g_wbuf / sizeof(wchar_t)));
    return to_utf8(g_wbuf);
}

/* int -> int:窗口是否还在(1/0) */
EXPORT int64_t dex_ui_running(int64_t win) {
    Win *wn = get_win(win);
    return (wn && wn->running) ? 1 : 0;
}

/* int -> int:泵消息并返回事件;0=无,>0=被点击的控件 id,-1=窗口已关闭 */
EXPORT int64_t dex_ui_poll(int64_t win) {
    (void)win;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { g_last_event = -1; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    int e = g_last_event;
    g_last_event = 0;
    return e;
}

/* int -> int:阻塞运行消息循环直到窗口关闭(纯显示用) */
EXPORT int64_t dex_ui_run(int64_t win) {
    Win *wn = get_win(win);
    if (!wn) return -1;
    MSG msg;
    while (wn->running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);   /* 让出 CPU,避免忙等 */
    }
    return 0;
}

/* int -> int:销毁窗口 */
EXPORT int64_t dex_ui_destroy(int64_t win) {
    Win *wn = get_win(win);
    if (!wn) return -1;
    DestroyWindow(wn->hwnd);
    wn->hwnd = NULL;
    wn->running = 0;
    return 0;
}
