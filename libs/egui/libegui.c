/*
 * libegui.dll — EGUI(easyGUI):DexLang 的 Qt 类似物 GUI 库
 *
 * 完全由 DLL 驱动、纯 C 实现、零依赖、无 Python。信号式设计:
 *   1) 用编号操纵界面:每个控件有一个用户分配的整数编号(id)
 *   2) 信号连接:eg_connect(id, "信号", "回调函数名")
 *   3) 事件循环由语言侧驱动(无 Python,纯 VM):
 *        include "egui";                       // fast 模式语言模块
 *        func on_click(id) { ... }
 *        let win = eg_window("Demo", 400, 300);
 *        eg_add("button", win, 1, ...);  eg_set_rect 定位
 *        eg_connect(1, "clicked", "on_click");
 *        while eg_running() {
 *            eg_poll();                        // 泵消息,事件入队
 *            while eg_next() != 0 {            // 弹出队头事件
 *                let cb = eg_event_cb();       // 按 (id,信号) 查回调名
 *                if cb != "" { call(cb, eg_event_id()); }
 *            }
 *        }
 *
 * 信号:clicked(按钮)、changed(复选框/单选/编辑/滑块)、
 *      selected(组合框/列表框)、closed(窗口关闭)
 * 事件值:eg_event_value()(勾选状态/选中索引/滑块位置)、
 *        eg_event_str()(编辑框文本/选中文本)
 *
 * 构建(Windows,zig):
 *   zig cc -shared -target x86_64-windows-gnu -O2 -lcomctl32 \
 *       -o libs/egui/libegui.dll libs/egui/libegui.c
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <commctrl.h>

#define EXPORT __declspec(dllexport)

#define MAX_WIN     8
#define MAX_CTRL    512
#define MAX_EVENTS  256
#define MAX_CONN    256
#define MAX_STR     256

/* ---------- 数据结构 ---------- */

/* 一个控件:子窗口句柄 + 用户编号 + 类型 */
typedef struct {
    HWND hwnd;
    int  id;
    char kind[24];
} Ctrl;

/* 一个窗口实例 */
typedef struct {
    HWND hwnd;
    int  win_id;      /* 窗口 id(1 起) */
    int  running;
    Ctrl ctrls[MAX_CTRL];
    int  nctrls;
} Win;

/* 一个事件 */
typedef struct {
    int   id;
    char  signal[24];
    int   value;
    char  str[MAX_STR];
} Event;

/* 一条信号连接:(id, 信号) -> 回调函数名 */
typedef struct {
    int   id;
    char  signal[24];
    char  cb[64];
} Conn;

static Win    g_wins[MAX_WIN];
static int    g_win_count = 0;

static Event  g_events[MAX_EVENTS];
static int    g_ev_head = 0, g_ev_tail = 0;      /* 队列区间 [head, tail) */

static Conn   g_conns[MAX_CONN];
static int    g_conn_count = 0;

static Event  g_current = {0, "", 0, ""};        /* eg_next 弹出的事件 */
static int    g_global_quit = 0;

/* ---------- UTF-8 <-> UTF-16 共享缓冲 ---------- */
static char    g_buf[MAX_STR];
static wchar_t g_wbuf[MAX_STR];

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

/* ---------- 查找 ---------- */
static Win *win_of(HWND h) {
    return (Win *)GetWindowLongPtrW(h, GWLP_USERDATA);
}

static Win *get_win(int id) {
    if (id < 1 || id > g_win_count) return NULL;
    Win *wn = &g_wins[id - 1];
    return wn->hwnd ? wn : NULL;
}

static Ctrl *ctrl_of_id(int id) {
    for (int w = 0; w < g_win_count; w++) {
        Win *wn = &g_wins[w];
        if (!wn->hwnd) continue;
        for (int i = 0; i < wn->nctrls; i++) {
            if (wn->ctrls[i].id == id) return &wn->ctrls[i];
        }
    }
    return NULL;
}

static Ctrl *ctrl_of_hwnd(HWND h) {
    for (int w = 0; w < g_win_count; w++) {
        Win *wn = &g_wins[w];
        if (!wn->hwnd) continue;
        for (int i = 0; i < wn->nctrls; i++) {
            if (wn->ctrls[i].hwnd == h) return &wn->ctrls[i];
        }
    }
    return NULL;
}

static int has_conn(int id, const char *signal) {
    for (int i = 0; i < g_conn_count; i++)
        if (g_conns[i].id == id && strcmp(g_conns[i].signal, signal) == 0)
            return 1;
    return 0;
}

static const char *conn_cb(int id, const char *signal) {
    for (int i = 0; i < g_conn_count; i++)
        if (g_conns[i].id == id && strcmp(g_conns[i].signal, signal) == 0)
            return g_conns[i].cb;
    return "";
}

static void enqueue(int id, const char *signal, int value, const char *str) {
    if ((g_ev_tail + 1) % MAX_EVENTS == g_ev_head) return;  /* 队列满,丢弃 */
    Event *e = &g_events[g_ev_tail];
    g_ev_tail = (g_ev_tail + 1) % MAX_EVENTS;
    e->id = id;
    strncpy(e->signal, signal, sizeof e->signal - 1); e->signal[sizeof e->signal - 1] = 0;
    e->value = value;
    strncpy(e->str, str ? str : "", sizeof e->str - 1); e->str[sizeof e->str - 1] = 0;
}

/* 读取控件文本(UTF-8,共享缓冲) */
static const char *ctrl_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return "";
    if (n >= MAX_STR) n = MAX_STR - 1;
    GetWindowTextW(h, g_wbuf, n + 1);
    return to_utf8(g_wbuf);
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK winproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Win *wn = win_of(h);
    switch (msg) {
    case WM_COMMAND: {
        int cid = (int)LOWORD(w);
        int code = (int)HIWORD(w);
        Ctrl *c = ctrl_of_id(cid);
        if (!c) return 0;
        HWND ch = c->hwnd;
        if (code == BN_CLICKED) {
            if (strcmp(c->kind, "button") == 0) {
                enqueue(cid, "clicked", 0, "");
            } else if (strcmp(c->kind, "checkbox") == 0) {
                int v = (int)SendMessageW(ch, BM_GETCHECK, 0, 0);
                enqueue(cid, "changed", v, "");
            } else if (strcmp(c->kind, "radio") == 0) {
                int v = (int)SendMessageW(ch, BM_GETCHECK, 0, 0);
                enqueue(cid, "changed", v, "");
            }
        } else if (code == EN_CHANGE) {
            if (strcmp(c->kind, "edit") == 0 || strcmp(c->kind, "edit_multi") == 0)
                enqueue(cid, "changed", 0, ctrl_text(ch));
        } else if (code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW(ch, CB_GETCURSEL, 0, 0);
            char *item = (char *)"";
            if (idx >= 0) {
                int n = (int)SendMessageW(ch, CB_GETLBTEXTLEN, (WPARAM)idx, 0);
                if (n > 0 && n < MAX_STR) {
                    SendMessageW(ch, CB_GETLBTEXT, (WPARAM)idx, (LPARAM)g_wbuf);
                    item = (char *)to_utf8(g_wbuf);
                }
            }
            enqueue(cid, "selected", idx, item);
        } else if (code == LBN_SELCHANGE) {
            int idx = (int)SendMessageW(ch, LB_GETCURSEL, 0, 0);
            enqueue(cid, "selected", idx, "");
        }
        return 0;
    }
    case WM_HSCROLL:
    case WM_VSCROLL: {
        HWND ch = (HWND)l;
        Ctrl *c = ctrl_of_hwnd(ch);
        if (c && strcmp(c->kind, "slider") == 0) {
            int pos = (int)SendMessageW(ch, TBM_GETPOS, 0, 0);
            enqueue(c->id, "changed", pos, "");
        }
        return 0;
    }
    case WM_CLOSE: {
        if (wn) {
            /* 连接了 closed 信号则交给语言回调决定(应调用 eg_quit);
               否则直接关闭窗口 */
            if (has_conn(wn->win_id, "closed")) {
                enqueue(wn->win_id, "closed", 0, "");
                return 0;
            }
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_DESTROY:
        if (wn) wn->running = 0;
        return 0;
    default:
        return DefWindowProcW(h, msg, w, l);
    }
}

/* ---------- 初始化 ---------- */
static int ui_init = 0;

static void ensure_init(void) {
    if (ui_init) return;
    ui_init = 1;
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = winproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"DexEGUIWindow";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
}

/* 设置默认 UI 字体(支持中文)。
   用固定像素字号,与控件的像素坐标体系一致。这样在系统 DPI 缩放
   (125%/150% 等)下,字体不会被 GetDeviceCaps(LOGPIXELSY) 提前放大,
   避免控件(像素尺寸)放不下中文文本;未声明 DPI 感知的进程由系统
   统一虚拟化放大,文字与控件等比缩放,布局不会溢出、输入不会受限。 */
static void apply_font(HWND h, int pts) {
    HFONT font = CreateFontW(-(pts > 0 ? pts : 16), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    if (font) {
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        /* 不删除:为简单起见在整个生命周期内保留字体 */
    }
}

/* ---------- 窗口 / 控件创建 ---------- */
EXPORT int64_t eg_window(const char *title, int64_t w, int64_t h) {
    ensure_init();
    if (g_win_count >= MAX_WIN) return -1;
    Win *wn = &g_wins[g_win_count];
    memset(wn, 0, sizeof *wn);
    wn->win_id = g_win_count + 1;
    wn->hwnd = CreateWindowExW(0, L"DexEGUIWindow", to_wide(title),
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                               WS_MINIMIZEBOX | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               (int)w, (int)h, NULL, NULL,
                               GetModuleHandleW(NULL), NULL);
    if (!wn->hwnd) return -1;
    SetWindowLongPtrW(wn->hwnd, GWLP_USERDATA, (LONG_PTR)wn);
    wn->running = 1;
    apply_font(wn->hwnd, 0);
    g_win_count++;
    return wn->win_id;
}

EXPORT int64_t eg_add(const char *kind, int64_t parent, int64_t id) {
    Win *wn = get_win((int)parent);
    if (!wn || wn->nctrls >= MAX_CTRL) return -1;
    const wchar_t *cls;
    DWORD style = WS_CHILD | WS_VISIBLE;
    DWORD ex = 0;
    if (strcmp(kind, "label") == 0) {            cls = L"STATIC"; }
    else if (strcmp(kind, "button") == 0) {      cls = L"BUTTON"; style |= BS_PUSHBUTTON; }
    else if (strcmp(kind, "checkbox") == 0) {    cls = L"BUTTON"; style |= BS_AUTOCHECKBOX; }
    else if (strcmp(kind, "radio") == 0) {       cls = L"BUTTON"; style |= BS_AUTORADIOBUTTON; }
    else if (strcmp(kind, "group") == 0) {       cls = L"BUTTON"; style |= BS_GROUPBOX; }
    else if (strcmp(kind, "edit") == 0) {        cls = L"EDIT"; style |= WS_BORDER | ES_AUTOHSCROLL; }
    else if (strcmp(kind, "edit_multi") == 0) {  cls = L"EDIT"; style |= WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL; }
    else if (strcmp(kind, "combo") == 0) {       cls = L"COMBOBOX"; style |= CBS_DROPDOWNLIST | WS_VSCROLL; }
    else if (strcmp(kind, "list") == 0) {        cls = L"LISTBOX"; style |= LBS_NOTIFY | WS_VSCROLL | WS_BORDER; }
    else if (strcmp(kind, "slider") == 0) {      cls = L"msctls_trackbar32"; style |= TBS_AUTOTICKS; ex = WS_EX_CLIENTEDGE; }
    else if (strcmp(kind, "progress") == 0) {    cls = L"msctls_progress32"; ex = WS_EX_CLIENTEDGE; }
    else return -2;

    Ctrl *c = &wn->ctrls[wn->nctrls];
    c->hwnd = CreateWindowExW(ex, cls, L"", style, 0, 0, 80, 24,
                              wn->hwnd, (HMENU)(INT_PTR)(int)id,
                              GetModuleHandleW(NULL), NULL);
    if (!c->hwnd) return -3;
    c->id = (int)id;
    strncpy(c->kind, kind, sizeof c->kind - 1);
    c->kind[sizeof c->kind - 1] = 0;
    wn->nctrls++;
    apply_font(c->hwnd, 0);
    return id;
}

/* ---------- 控件属性(编号操纵) ---------- */
EXPORT int64_t eg_set_pos(int64_t id, int64_t x, int64_t y) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    RECT r;
    GetWindowRect(c->hwnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    MoveWindow(c->hwnd, (int)x, (int)y, w, h, TRUE);
    return 0;
}

EXPORT int64_t eg_set_size(int64_t id, int64_t w, int64_t h) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    RECT r;
    GetWindowRect(c->hwnd, &r);
    int x = r.left, y = r.top;
    /* 相对父窗口客户区 */
    POINT pt = {x, y};
    ScreenToClient(GetParent(c->hwnd), &pt);
    MoveWindow(c->hwnd, pt.x, pt.y, (int)w, (int)h, TRUE);
    return 0;
}

EXPORT int64_t eg_set_text(int64_t id, const char *text) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    SetWindowTextW(c->hwnd, to_wide(text));
    return 0;
}

EXPORT const char *eg_get_text(int64_t id) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return "";
    return ctrl_text(c->hwnd);
}

EXPORT int64_t eg_set_value(int64_t id, int64_t v) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    if (strcmp(c->kind, "checkbox") == 0 || strcmp(c->kind, "radio") == 0)
        SendMessageW(c->hwnd, BM_SETCHECK, (WPARAM)(v ? BST_CHECKED : BST_UNCHECKED), 0);
    else if (strcmp(c->kind, "slider") == 0)
        SendMessageW(c->hwnd, TBM_SETPOS, TRUE, (LPARAM)v);
    else if (strcmp(c->kind, "progress") == 0)
        SendMessageW(c->hwnd, PBM_SETPOS, (WPARAM)v, 0);
    else if (strcmp(c->kind, "combo") == 0)
        SendMessageW(c->hwnd, CB_SETCURSEL, (WPARAM)v, 0);
    else if (strcmp(c->kind, "list") == 0)
        SendMessageW(c->hwnd, LB_SETCURSEL, (WPARAM)v, 0);
    return 0;
}

EXPORT int64_t eg_get_value(int64_t id) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    if (strcmp(c->kind, "checkbox") == 0 || strcmp(c->kind, "radio") == 0)
        return (int64_t)SendMessageW(c->hwnd, BM_GETCHECK, 0, 0);
    if (strcmp(c->kind, "slider") == 0)
        return (int64_t)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0);
    if (strcmp(c->kind, "progress") == 0)
        return (int64_t)SendMessageW(c->hwnd, PBM_GETPOS, 0, 0);
    if (strcmp(c->kind, "combo") == 0)
        return (int64_t)SendMessageW(c->hwnd, CB_GETCURSEL, 0, 0);
    if (strcmp(c->kind, "list") == 0)
        return (int64_t)SendMessageW(c->hwnd, LB_GETCURSEL, 0, 0);
    return 0;
}

EXPORT int64_t eg_add_item(int64_t id, const char *text) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    if (strcmp(c->kind, "combo") == 0)
        return (int64_t)SendMessageW(c->hwnd, CB_ADDSTRING, 0, (LPARAM)to_wide(text));
    if (strcmp(c->kind, "list") == 0)
        return (int64_t)SendMessageW(c->hwnd, LB_ADDSTRING, 0, (LPARAM)to_wide(text));
    return -2;
}

EXPORT int64_t eg_clear_items(int64_t id) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    if (strcmp(c->kind, "combo") == 0) SendMessageW(c->hwnd, CB_RESETCONTENT, 0, 0);
    else if (strcmp(c->kind, "list") == 0) SendMessageW(c->hwnd, LB_RESETCONTENT, 0, 0);
    else return -2;
    return 0;
}

EXPORT int64_t eg_set_enabled(int64_t id, int64_t on) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    EnableWindow(c->hwnd, on ? TRUE : FALSE);
    return 0;
}

EXPORT int64_t eg_set_visible(int64_t id, int64_t on) {
    Ctrl *c = ctrl_of_id((int)id);
    if (!c) return -1;
    ShowWindow(c->hwnd, on ? SW_SHOW : SW_HIDE);
    return 0;
}

/* ---------- 信号连接 ---------- */
EXPORT int64_t eg_connect(int64_t id, const char *signal, const char *cb) {
    if (g_conn_count >= MAX_CONN) return -1;
    /* 覆盖同名 (id, 信号) 连接 */
    for (int i = 0; i < g_conn_count; i++) {
        if (g_conns[i].id == (int)id && strcmp(g_conns[i].signal, signal) == 0) {
            strncpy(g_conns[i].cb, cb, sizeof g_conns[i].cb - 1);
            g_conns[i].cb[sizeof g_conns[i].cb - 1] = 0;
            return 0;
        }
    }
    Conn *c = &g_conns[g_conn_count++];
    c->id = (int)id;
    strncpy(c->signal, signal, sizeof c->signal - 1); c->signal[sizeof c->signal - 1] = 0;
    strncpy(c->cb, cb, sizeof c->cb - 1); c->cb[sizeof c->cb - 1] = 0;
    return 0;
}

/* ---------- 事件循环 ---------- */
EXPORT int64_t eg_poll(void) {
    MSG msg;
    int n = 0;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        n++;
    }
    return n;
}

EXPORT int64_t eg_running(void) {
    if (g_global_quit) return 0;
    for (int i = 0; i < g_win_count; i++)
        if (g_wins[i].hwnd && g_wins[i].running) return 1;
    return 0;
}

EXPORT int64_t eg_quit(void) {
    g_global_quit = 1;
    return 0;
}

/* 弹出队头事件到"当前事件";返回其 id(0=队列空) */
EXPORT int64_t eg_next(void) {
    if (g_ev_head == g_ev_tail) {
        memset(&g_current, 0, sizeof g_current);
        return 0;
    }
    g_current = g_events[g_ev_head];
    g_ev_head = (g_ev_head + 1) % MAX_EVENTS;
    return g_current.id;
}

EXPORT int64_t eg_event_id(void)    { return g_current.id; }
EXPORT const char *eg_event_signal(void) { return g_current.signal; }
EXPORT const char *eg_event_cb(void)     { return conn_cb(g_current.id, g_current.signal); }
EXPORT int64_t eg_event_value(void) { return g_current.value; }
EXPORT const char *eg_event_str(void)    { return g_current.str; }

/* 销毁窗口并结束 */
EXPORT int64_t eg_close(int64_t id) {
    Win *wn = get_win((int)id);
    if (!wn) return -1;
    DestroyWindow(wn->hwnd);
    wn->hwnd = NULL;
    wn->running = 0;
    return 0;
}

EXPORT int64_t eg_set_title(int64_t id, const char *title) {
    Win *wn = get_win((int)id);
    if (!wn) return -1;
    SetWindowTextW(wn->hwnd, to_wide(title));
    return 0;
}

/* ---------- 测试辅助 ---------- */
EXPORT int64_t eg_win_hwnd(int64_t id) {
    Win *wn = get_win((int)id);
    return wn ? (int64_t)(INT_PTR)wn->hwnd : 0;
}

EXPORT int64_t eg_ctrl_hwnd(int64_t id) {
    Ctrl *c = ctrl_of_id((int)id);
    return c ? (int64_t)(INT_PTR)c->hwnd : 0;
}
