/* ============================================================================
 * ds_main.c — DexStudio 宿主:Win32 窗口 + 内嵌 WebView2 + 模型层命令通道。
 *
 * 三种用法:
 *   dexstudio.exe                     开 IDE 窗口(默认 $DEXSTUDIO_WEB 或 exe 同级 web/)
 *   dexstudio.exe --project <目录>     开窗口并直接打开项目
 *   dexstudio.exe --command '<json>'   **不开窗口**,跑一条模型命令并打印响应
 *   dexstudio.exe --selftest           **不开窗口**跑一套模型自测(退出码 0/1)
 *   dexstudio.exe --webview-version    打印系统 WebView2 运行时版本
 *
 * 后三种是给测试/CI 用的:模型层不依赖窗口,所以 IDE 的核心逻辑可以被脚本验证
 * (决策 #10;tests/test_dexstudio.py 会跑它们)。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "ds_model.h"
#include "ds_webview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

/* ------------------------------------------------------------ 工具 */

static char g_exe_dir[MAX_PATH * 2];

static void find_exe_dir(void)
{
    char buf[MAX_PATH * 2];
    char *p;
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (!n) { g_exe_dir[0] = 0; return; }
    buf[sizeof buf - 1] = 0;
    p = strrchr(buf, '\\');
    if (p) *p = 0;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", buf);
}

/* 前端目录:--web 指定 → exe 同级 web\ → 开发布局 ../../dexstudio/web */
static int find_web_dir(const char *override, char *out, size_t outsz)
{
    char cand[MAX_PATH * 3];
    if (override && *override) {
        snprintf(out, outsz, "%s", override);
        return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
    }
    snprintf(cand, sizeof cand, "%s\\web", g_exe_dir);
    if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        snprintf(out, outsz, "%s", cand);
        return 1;
    }
    snprintf(cand, sizeof cand, "%s\\..\\..\\dexstudio\\web", g_exe_dir);
    if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES) {
        snprintf(out, outsz, "%s", cand);
        return 1;
    }
    return 0;
}

/* 把字符串安全地塞进 JSON(路径里有反斜杠时,不转义就会得到
 * "unknown escape at byte N" —— 自测第一版就踩了)。 */
static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0, i;
    for (i = 0; in && in[i] && o + 8 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\\' || c == '"') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ 无界面命令 */

static int cmd_command(const char *json, const char *project)
{
    DsModel *m = ds_model_create(g_exe_dir);
    const char *resp;
    if (project && *project) {
        char req[4096], esc[MAX_PATH * 2];
        json_escape(project, esc, sizeof esc);
        snprintf(req, sizeof req,
                 "{\"cmd\":\"project.open\",\"args\":{\"dir\":\"%s\"}}", esc);
        ds_command(m, req);
    }
    resp = ds_command(m, json);
    printf("%s\n", resp ? resp : "{\"ok\":false,\"error\":\"no response\"}");
    ds_model_destroy(m);
    return 0;
}

static int cmd_webview_version(void)
{
    char err[512];
    const char *v = ds_wv_runtime_version(err, sizeof err);
    if (!v) {
        printf("WebView2 运行时不可用:%s\n", err);
        return 1;
    }
    printf("%s\n", v);
    return 0;
}

/* 自测:模型层的端到端(不需要窗口) */
static int g_pass, g_fail;

static void check(const char *name, int cond, const char *detail)
{
    if (cond) { g_pass++; printf("  PASS  %s\n", name); }
    else { g_fail++; printf("  FAIL  %s  %s\n", name, detail ? detail : ""); }
}

static int cmd_selftest(void)
{
    DsModel *m = ds_model_create(g_exe_dir);
    char tmp[MAX_PATH];
    char esc[MAX_PATH * 2];
    char req[4096];
    const char *r;

    printf("DexStudio 自测(无窗口)\n");
    snprintf(tmp, sizeof tmp, "%s\\..\\..\\_zigtmp\\ds_selftest", g_exe_dir);
    RemoveDirectoryA(tmp);

    printf("[引擎与模型]\n");
    check("引擎加载", ds_model_engine_ok(m), ds_model_engine_error(m));
    r = ds_command(m, "{\"cmd\":\"app.info\"}");
    check("app.info 返回 ok", r && strstr(r, "\"ok\":true"), r);

    printf("[新建项目]\n");
    snprintf(tmp, sizeof tmp, "%s\\..\\..\\_zigtmp\\ds_selftest", g_exe_dir);
    json_escape(tmp, esc, sizeof esc);
    snprintf(req, sizeof req,
             "{\"cmd\":\"project.new\",\"args\":{\"dir\":\"%s\",\"name\":\"selftest\"}}",
             esc);
    r = ds_command(m, req);
    check("project.new", r && strstr(r, "\"ok\":true"), r);

    printf("[场景编辑]\n");
    r = ds_command(m, "{\"cmd\":\"entity.add\",\"args\":{\"name\":\"player\"}}");
    check("entity.add", r && strstr(r, "\"ok\":true"), r);
    r = ds_command(m, "{\"cmd\":\"entity.list\"}");
    check("entity.list 有 1 个实体", r && strstr(r, "\"player\""), r);
    r = ds_command(m, "{\"cmd\":\"comp.schema\"}");
    check("comp.schema 列出组件", r && strstr(r, "\"transform\""), r);
    r = ds_command(m, "{\"cmd\":\"undo\"}");
    check("undo", r && strstr(r, "\"ok\":true"), r);
    r = ds_command(m, "{\"cmd\":\"entity.list\"}");
    check("undo 后实体消失", r && !strstr(r, "\"player\""), r);
    r = ds_command(m, "{\"cmd\":\"redo\"}");
    check("redo", r && strstr(r, "\"ok\":true"), r);
    r = ds_command(m, "{\"cmd\":\"entity.list\"}");
    check("redo 后实体回来", r && strstr(r, "\"player\""), r);

    printf("[存盘]\n");
    r = ds_command(m, "{\"cmd\":\"project.save\"}");
    check("project.save", r && strstr(r, "\"ok\":true"), r);

    printf("[错误路径]\n");
    r = ds_command(m, "{\"cmd\":\"entity.get\",\"args\":{\"id\":999999}}");
    check("无效实体带原因", r && strstr(r, "\"ok\":false") && strstr(r, "不存在"), r);
    r = ds_command(m, "{\"cmd\":\"nosuch\"}");
    check("未知命令带原因", r && strstr(r, "未知命令"), r);
    r = ds_command(m, "{not json}");
    check("坏 JSON 带原因", r && strstr(r, "\"ok\":false"), r);

    ds_model_destroy(m);
    printf("自测结果:%d 通过,%d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ------------------------------------------------------------ 窗口宿主 */

typedef struct {
    DsModel *model;
    DsWebView *wv;
    char web_dir[MAX_PATH * 3];
} Host;

/* --wv-selftest:前端发来 ui.ready 就算整条链通了 */
static int g_ui_ready;
static char g_ui_ready_info[512];
static char g_ui_msg_log[1024];

static const char *on_js_message(void *user, const char *json)
{
    if (json && *json) {
        char *p = g_ui_msg_log + strlen(g_ui_msg_log);
        size_t left = sizeof g_ui_msg_log - (size_t)(p - g_ui_msg_log);
        if (left > 8) snprintf(p, left, "%s%s", g_ui_msg_log[0] ? " | " : "", json);
    }
    if (json && strstr(json, "\"ui.ready\"")) {
        g_ui_ready = 1;
        snprintf(g_ui_ready_info, sizeof g_ui_ready_info, "%s", json);
    }
    return ds_command((DsModel *)user, json);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Host *h = (Host *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        Host *host = (Host *)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)host);
        return 0;
    }
    case WM_SIZE:
        if (h && h->wv) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            ds_wv_set_bounds(h->wv, 0, 0, rc.right, rc.bottom);
        }
        return 0;
    case WM_SETFOCUS:
        if (h && h->wv) SetFocus(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int run_window(const char *project, const char *web_override, int wv_selftest)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    Host *host = calloc(1, sizeof *host);
    RECT rc = {0, 0, 1440, 900};

    /* DPI 感知:必须在创建窗口之前;失败(如清单已设)不算错 */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!find_web_dir(web_override, host->web_dir, sizeof host->web_dir)) {
        fprintf(stderr, "dexstudio: 找不到前端目录(web/index.html);用 --web 指定\n");
        free(host);
        return 1;
    }
    if (!ds_wv_load(g_exe_dir)) {
        fprintf(stderr, "dexstudio: %s\n", ds_wv_error(NULL));
        free(host);
        return 1;
    }
    host->model = ds_model_create(g_exe_dir);
    if (project && *project) {
        char req[4096], esc[MAX_PATH * 2];
        json_escape(project, esc, sizeof esc);
        snprintf(req, sizeof req,
                 "{\"cmd\":\"project.open\",\"args\":{\"dir\":\"%s\"}}", esc);
        ds_command(host->model, req);
    }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "DexStudioWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassA(&wc)) {
        fprintf(stderr, "dexstudio: RegisterClass 失败\n");
        return 1;
    }
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "DexStudioWindow", "DexStudio — DexLang 可视化 IDE",
                           WS_OVERLAPPEDWINDOW,
                           wv_selftest ? -4000 : CW_USEDEFAULT,
                           wv_selftest ? -4000 : CW_USEDEFAULT,
                           rc.right - rc.left, rc.top ? rc.bottom - rc.top : 900,
                           NULL, NULL, GetModuleHandleA(NULL), host);
    if (!hwnd) {
        fprintf(stderr, "dexstudio: 创建窗口失败\n");
        free(host);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    host->wv = ds_wv_create(hwnd, on_js_message, host->model);
    if (!host->wv) {
        fprintf(stderr, "dexstudio: 创建 WebView2 失败:%s\n", ds_wv_error(NULL));
        /* 仍然开窗(用户能看到窗口),但明确报错 */
    } else {
        ds_wv_navigate_folder(host->wv, host->web_dir, "dexstudio.local", "index.html");
        ds_wv_set_bounds(host->wv, 0, 0, rc.right - rc.left, rc.bottom - rc.top);
    }

    if (!wv_selftest) {
        while (GetMessageA(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    } else {
        /* 无人值守:等前端发 ui.ready(窗口在屏幕外,不会打扰人) */
        DWORD t0 = GetTickCount();
        int done = 0, probed = 0;
        static const char *PROBE =
            "JSON.stringify({bridge: !!(window.chrome && (window.chrome.webview || window.chrome.webView)),"
            " title: document.title, href: location.href,"
            " ready: document.readyState,"
            " scripts: document.scripts ? document.scripts.length : -1,"
            " hasDs: typeof ds === 'function',"
            " chromeKeys: Object.keys(window.chrome || {})})";
        printf("DexStudio WebView2 自测(窗口在屏幕外)\n");
        while (!done) {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { done = 1; break; }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (g_ui_ready) break;
            /* 导航完成后主动问一遍页面状态:桥在不在、加载的是哪一个页面 */
            if (!probed && host->wv && ds_wv_ready(host->wv)
                && GetTickCount() - t0 > 1500) {
                ds_wv_eval(host->wv, PROBE);
                probed = 1;
            }
            if (GetTickCount() - t0 > 12000) break;
            Sleep(10);
        }
        if (g_ui_ready) {
            printf("  PASS  前端已就绪(窗口 + 本地页面 + JS→C→JS 往返)\n");
            printf("        请求:%s\n", g_ui_ready_info);
        } else {
            const char *e = host->wv ? ds_wv_error(host->wv) : "未创建 WebView2";
            const char *info = ds_command(host->model, "{\"cmd\":\"app.info\"}");
            printf("  FAIL  等不到 ui.ready\n");
            if (e && *e) printf("        WebView2:%s\n", e);
            printf("        收到过的消息:%s\n",
                   g_ui_msg_log[0] ? g_ui_msg_log : "(一条也没有 → 页面里的 JS 没跑起来)");
            printf("        app.info:%s\n", info ? info : "");
            printf("        排查:dexstudio/web/index.html 是否与 exe 同级(或用 --web 指定);"
                   "DEXSTUDIO_WV_TRACE=1 看阶段日志\n");
        }
        if (host->wv) ds_wv_destroy(host->wv);
        ds_model_destroy(host->model);
        free(host);
        return g_ui_ready ? 0 : 1;
    }
    if (host->wv) ds_wv_destroy(host->wv);
    ds_model_destroy(host->model);
    free(host);
    return 0;
}

/* ------------------------------------------------------------ 入口 */

static int ds_host_main(int argc, char **argv)
{
    const char *project = NULL, *web = NULL, *command = NULL;
    int selftest = 0, wv_selftest = 0, i;

    SetConsoleOutputCP(65001);
    find_exe_dir();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--project") && i + 1 < argc) project = argv[++i];
        else if (!strcmp(argv[i], "--web") && i + 1 < argc) web = argv[++i];
        else if (!strcmp(argv[i], "--command") && i + 1 < argc) command = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--wv-selftest")) wv_selftest = 1;
        else if (!strcmp(argv[i], "--version")) {
            printf("DexStudio %s\n", ds_version());
            return 0;
        } else if (!strcmp(argv[i], "--webview-version")) return cmd_webview_version();
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("DexStudio %s\n"
                   "  dexstudio.exe [--project DIR] [--web DIR]\n"
                   "  dexstudio.exe --command '<json>'   跑一条模型命令(不开窗口)\n"
                   "  dexstudio.exe --selftest           模型自测(不开窗口)\n"
                   "  dexstudio.exe --webview-version\n"
                   "  dexstudio.exe --version\n", ds_version());
            return 0;
        } else {
            fprintf(stderr, "dexstudio: 未知参数 %s\n", argv[i]);
            return 2;
        }
    }
    if (command) return cmd_command(command, project);
    if (selftest) return cmd_selftest();
    return run_window(project, web, wv_selftest);
}

#ifndef DS_NO_CONSOLE
int main(int argc, char **argv) { return ds_host_main(argc, argv); }
#else
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d)
{
    (void)a; (void)b; (void)c; (void)d;
    return ds_host_main(__argc, __argv);
}
#endif
