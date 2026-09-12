/* ============================================================================
 * ds_webview.c — WebView2 最小封装(见 ds_webview.h)。
 *
 * 关键点:
 *   1) 用 CINTERFACE 模式包含 vendor 的 WebView2.h,C 里直接按 vtable 调;
 *   2) **自己定义 IID 符号** —— 头文件里只有 `EXTERN_C const IID IID_xxx;` 声明,
 *      真正的值本应由 WebView2Loader.lib 提供,我们不链接它(见文件末尾的定义,
 *      值取自 WebView2.h 的 MIDL_INTERFACE 标注);
 *   3) CreateCoreWebView2EnvironmentWithOptions 等三个函数从 WebView2Loader.dll
 *      运行时取地址 —— 这样构建期只要有头文件。
 * ==========================================================================*/
#define CINTERFACE
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include "ds_webview.h"
#include "ds_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <objbase.h>
#include "third_party/webview2/include/WebView2.h"

/* ------------------------------------------------------------ IID 从哪来 */

/* 一开始我以为要像 dexgame 的 dg_guids.h 那样自己写 IID —— 其实 **不用**:
 * WebView2.h 在 CINTERFACE 模式下会自己给出定义
 * (`EXTERN_C __declspec(selectany) const IID IID_ICoreWebView2Environment = {…}`,
 *  见头文件靠后的位置),__declspec(selectany) 保证多 TU 重复定义也不冲突。
 * 于是我们只需要 vendor 头文件,不需要 WebView2Loader.lib。 */

/* ------------------------------------------------------------ Loader 绑定 */

typedef HRESULT(WINAPI *FnCreateEnvWithOptions)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);
typedef HRESULT(WINAPI *FnGetVersion)(PCWSTR browserExecutableFolder, LPWSTR *versionInfo);

static HMODULE g_loader;
static FnCreateEnvWithOptions g_create_env;
static FnGetVersion g_get_version;
static char g_load_err[512];

/* exe 可能整个装在中文目录里,而 LoadLibraryA 会按 ANSI(936)解路径 ——
 * 所以自己转宽字符再 LoadLibraryW。 */
static HMODULE ds_wv_load_library(const char *path)
{
    wchar_t *w = dsu_w(path);
    HMODULE h = w ? LoadLibraryW(w) : NULL;
    free(w);
    return h;
}

int ds_wv_load(const char *exe_dir)
{
    char path[MAX_PATH * 2];
    if (g_loader) return 1;
    if (exe_dir && *exe_dir)
        snprintf(path, sizeof path, "%s\\WebView2Loader.dll", exe_dir);
    else
        snprintf(path, sizeof path, "WebView2Loader.dll");
    g_loader = ds_wv_load_library(path);
    if (!g_loader) {
        /* 也试一下 PATH(调试时把 DLL 放在别处) */
        g_loader = ds_wv_load_library("WebView2Loader.dll");
    }
    if (!g_loader) {
        snprintf(g_load_err, sizeof g_load_err,
                 "无法加载 WebView2Loader.dll(找过 %s);它应从 "
                 "dexstudio/host/third_party/webview2/x64/ 复制到 exe 同目录", path);
        return 0;
    }
    g_create_env = (FnCreateEnvWithOptions)(void *)GetProcAddress(
        g_loader, "CreateCoreWebView2EnvironmentWithOptions");
    g_get_version = (FnGetVersion)(void *)GetProcAddress(
        g_loader, "GetAvailableCoreWebView2BrowserVersionString");
    if (!g_create_env || !g_get_version) {
        snprintf(g_load_err, sizeof g_load_err,
                 "WebView2Loader.dll 缺少需要的导出(环境创建/版本查询)");
        return 0;
    }
    return 1;
}

/* 运行时版本查询用宽字符 → UTF-8 */
static void wide_to_utf8(const wchar_t *w, char *out, size_t outsz)
{
    if (!w) { out[0] = 0; return; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outsz, NULL, NULL);
}

const char *ds_wv_runtime_version(char *err, unsigned errsz)
{
    static char ver[128];
    LPWSTR w = NULL;
    HRESULT hr;
    if (!ds_wv_load(NULL)) {
        if (err) snprintf(err, errsz, "%s", g_load_err);
        return NULL;
    }
    hr = g_get_version(NULL, &w);
    if (FAILED(hr) || !w) {
        if (err) snprintf(err, errsz, "系统里没有 WebView2 运行时(hr=0x%08lx)",
                          (unsigned long)hr);
        return NULL;
    }
    wide_to_utf8(w, ver, sizeof ver);
    CoTaskMemFree(w);
    return ver;
}

/* ------------------------------------------------------------ 对象 */

#define DS_WV_MAX_MAP 4

struct DsWebView {
    void *parent;
    DsWebMessageFn on_message;
    void *user;
    ICoreWebView2Environment *env;
    ICoreWebView2Controller *controller;
    ICoreWebView2 *webview;
    int ready;
    int failed;
    char err[512];
    /* 待应用的导航(环境/控制器是异步就绪的,所以参数先存下来,
     * 就绪后**先设虚拟主机映射再导航** —— 顺序错了会白页且没有报错) */
    char pending_folder[2048];
    char pending_host[128];
    char pending_page[256];
    int has_pending;
    /* 额外映射(预览目录 / 项目根)。导航前后来回调用都允许,同 host 覆盖。 */
    struct { char host[128]; char folder[2048]; } map[DS_WV_MAX_MAP];
    int n_map;
};

/* 诊断开关:DEXSTUDIO_WV_TRACE=1 时把 WebView2 各阶段打到 stderr。
 * "白页 / 就绪失败"这类问题没有阶段日志基本查不出来。 */
static int wv_trace(void)
{
    static int t = -1;
    if (t < 0) {
        const char *e = getenv("DEXSTUDIO_WV_TRACE");
        t = (e && *e && *e != '0');
    }
    return t;
}
#define WVTR(...) do { if (wv_trace()) fprintf(stderr, "[wv] " __VA_ARGS__); } while (0)

/* --- COM 处理器通用部分:三个 handler 结构都以接口为第一个成员 --- */

#define DS_HANDLER_HEADER(iface_type)                                           \
    iface_type iface;                                                           \
    LONG ref;                                                                   \
    DsWebView *wv

static HRESULT STDMETHODCALLTYPE generic_qi(void *self, const IID *iid, void **out,
                                            const IID *my_iid, void *my_vtbl_self)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, my_iid)) {
        *out = my_vtbl_self;
        (void)self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

/* --- 环境创建完成 --- */

typedef struct EnvHandler {
    DS_HANDLER_HEADER(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler);
} EnvHandler;

static HRESULT STDMETHODCALLTYPE EnvHandler_QI(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, const IID *iid,
    void **out)
{
    return generic_qi(This, iid, out,
                      &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, This);
}
static ULONG STDMETHODCALLTYPE EnvHandler_AddRef(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This)
{
    EnvHandler *h = (EnvHandler *)This;
    return (ULONG)InterlockedIncrement(&h->ref);
}
static ULONG STDMETHODCALLTYPE EnvHandler_Release(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This)
{
    EnvHandler *h = (EnvHandler *)This;
    LONG r = InterlockedDecrement(&h->ref);
    if (r == 0) free(h);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE EnvHandler_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT result,
    ICoreWebView2Environment *env);

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_env_vtbl = {
    EnvHandler_QI, EnvHandler_AddRef, EnvHandler_Release, EnvHandler_Invoke
};

/* --- 控制器创建完成 --- */

typedef struct CtrlHandler {
    DS_HANDLER_HEADER(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler);
} CtrlHandler;

static HRESULT STDMETHODCALLTYPE CtrlHandler_QI(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, const IID *iid,
    void **out)
{
    return generic_qi(This, iid, out,
                      &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, This);
}
static ULONG STDMETHODCALLTYPE CtrlHandler_AddRef(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This)
{
    CtrlHandler *h = (CtrlHandler *)This;
    return (ULONG)InterlockedIncrement(&h->ref);
}
static ULONG STDMETHODCALLTYPE CtrlHandler_Release(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This)
{
    CtrlHandler *h = (CtrlHandler *)This;
    LONG r = InterlockedDecrement(&h->ref);
    if (r == 0) free(h);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE CtrlHandler_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT result,
    ICoreWebView2Controller *controller);

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrl_vtbl = {
    CtrlHandler_QI, CtrlHandler_AddRef, CtrlHandler_Release, CtrlHandler_Invoke
};

/* --- JS→C 消息 --- */

typedef struct MsgHandler {
    DS_HANDLER_HEADER(ICoreWebView2WebMessageReceivedEventHandler);
} MsgHandler;

static HRESULT STDMETHODCALLTYPE MsgHandler_QI(
    ICoreWebView2WebMessageReceivedEventHandler *This, const IID *iid, void **out)
{
    return generic_qi(This, iid, out,
                      &IID_ICoreWebView2WebMessageReceivedEventHandler, This);
}
static ULONG STDMETHODCALLTYPE MsgHandler_AddRef(
    ICoreWebView2WebMessageReceivedEventHandler *This)
{
    MsgHandler *h = (MsgHandler *)This;
    return (ULONG)InterlockedIncrement(&h->ref);
}
static ULONG STDMETHODCALLTYPE MsgHandler_Release(
    ICoreWebView2WebMessageReceivedEventHandler *This)
{
    MsgHandler *h = (MsgHandler *)This;
    LONG r = InterlockedDecrement(&h->ref);
    if (r == 0) free(h);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE MsgHandler_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This, ICoreWebView2 *sender,
    ICoreWebView2WebMessageReceivedEventArgs *args)
{
    MsgHandler *h = (MsgHandler *)This;
    LPWSTR wjson = NULL;
    char *json = NULL;
    char *resp = NULL;
    (void)sender;
    if (FAILED(args->lpVtbl->get_WebMessageAsJson(args, &wjson)) || !wjson) return S_OK;
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, wjson, -1, NULL, 0, NULL, NULL);
        json = malloc((size_t)n + 1);
        if (json) WideCharToMultiByte(CP_UTF8, 0, wjson, -1, json, n, NULL, NULL);
    }
    CoTaskMemFree(wjson);
    WVTR("收到 JS 消息 %s\n", json ? json : "(null)");
    if (!json) return S_OK;
    if (h->wv->on_message) {
        const char *r = h->wv->on_message(h->wv->user, json);
        if (r) resp = _strdup(r);
    }
    free(json);
    if (resp) {
        ICoreWebView2 *wv = h->wv->webview;
        if (wv) {
            int n = MultiByteToWideChar(CP_UTF8, 0, resp, -1, NULL, 0);
            LPWSTR w = malloc((size_t)n * sizeof(wchar_t));
            if (w) {
                MultiByteToWideChar(CP_UTF8, 0, resp, -1, w, n);
                wv->lpVtbl->PostWebMessageAsJson(wv, w);
                free(w);
            }
        }
        free(resp);
    }
    return S_OK;
}

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_msg_vtbl = {
    MsgHandler_QI, MsgHandler_AddRef, MsgHandler_Release, MsgHandler_Invoke
};

/* --- 导航完成(只在失败时记原因:白页而没有原因是最难查的一类问题) --- */

typedef struct NavHandler {
    DS_HANDLER_HEADER(ICoreWebView2NavigationCompletedEventHandler);
} NavHandler;

static HRESULT STDMETHODCALLTYPE NavHandler_QI(
    ICoreWebView2NavigationCompletedEventHandler *This, const IID *iid, void **out)
{
    return generic_qi(This, iid, out,
                      &IID_ICoreWebView2NavigationCompletedEventHandler, This);
}
static ULONG STDMETHODCALLTYPE NavHandler_AddRef(
    ICoreWebView2NavigationCompletedEventHandler *This)
{
    NavHandler *h = (NavHandler *)This;
    return (ULONG)InterlockedIncrement(&h->ref);
}
static ULONG STDMETHODCALLTYPE NavHandler_Release(
    ICoreWebView2NavigationCompletedEventHandler *This)
{
    NavHandler *h = (NavHandler *)This;
    LONG r = InterlockedDecrement(&h->ref);
    if (r == 0) free(h);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE NavHandler_Invoke(
    ICoreWebView2NavigationCompletedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args)
{
    NavHandler *h = (NavHandler *)This;
    BOOL ok = TRUE;
    COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
    (void)sender;
    args->lpVtbl->get_IsSuccess(args, &ok);
    WVTR("导航完成 ok=%d\n", (int)ok);
    if (!ok) {
        args->lpVtbl->get_WebErrorStatus(args, &st);
        snprintf(h->wv->err, sizeof h->wv->err,
                 "前端页面加载失败(WebErrorStatus=%d):检查 dexstudio/web/index.html "
                 "是否与 exe 同级(或用 --web 指定目录)", (int)st);
    }
    return S_OK;
}

static ICoreWebView2NavigationCompletedEventHandlerVtbl g_nav_vtbl = {
    NavHandler_QI, NavHandler_AddRef, NavHandler_Release, NavHandler_Invoke
};

/* --- ExecuteScript 结果(诊断/测试:把页面里的答案带回来) --- */

typedef struct EvalHandler {
    DS_HANDLER_HEADER(ICoreWebView2ExecuteScriptCompletedHandler);
} EvalHandler;

static HRESULT STDMETHODCALLTYPE EvalHandler_QI(
    ICoreWebView2ExecuteScriptCompletedHandler *This, const IID *iid, void **out)
{
    return generic_qi(This, iid, out,
                      &IID_ICoreWebView2ExecuteScriptCompletedHandler, This);
}
static ULONG STDMETHODCALLTYPE EvalHandler_AddRef(
    ICoreWebView2ExecuteScriptCompletedHandler *This)
{
    EvalHandler *h = (EvalHandler *)This;
    return (ULONG)InterlockedIncrement(&h->ref);
}
static ULONG STDMETHODCALLTYPE EvalHandler_Release(
    ICoreWebView2ExecuteScriptCompletedHandler *This)
{
    EvalHandler *h = (EvalHandler *)This;
    LONG r = InterlockedDecrement(&h->ref);
    if (r == 0) free(h);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE EvalHandler_Invoke(
    ICoreWebView2ExecuteScriptCompletedHandler *This, HRESULT hr, LPCWSTR result)
{
    EvalHandler *h = (EvalHandler *)This;
    char *r8 = NULL;
    char *msg;
    if (result) {
        int n = WideCharToMultiByte(CP_UTF8, 0, result, -1, NULL, 0, NULL, NULL);
        r8 = malloc((size_t)n + 1);
        if (r8) WideCharToMultiByte(CP_UTF8, 0, result, -1, r8, n, NULL, NULL);
    }
    msg = malloc(strlen(r8 ? r8 : "null") + 128);
    sprintf(msg, "{\"cmd\":\"ui.eval\",\"args\":{\"hr\":%ld,\"result\":%s}}",
            (long)hr, r8 ? r8 : "null");
    WVTR("eval 结果 %s\n", msg);
    if (h->wv->on_message) h->wv->on_message(h->wv->user, msg);
    free(msg);
    free(r8);
    return S_OK;
}

static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_eval_vtbl = {
    EvalHandler_QI, EvalHandler_AddRef, EvalHandler_Release, EvalHandler_Invoke
};

/* ------------------------------------------------------------ 导航 */

static void do_navigate(DsWebView *wv, const char *url)
{
    int n;
    LPWSTR w;
    if (!wv->webview) return;
    n = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, url, -1, w, n);
    wv->webview->lpVtbl->Navigate(wv->webview, w);
    free(w);
}

/* 把一个目录映射到 https://<host>/。要求 webview 已就绪。 */
static int map_one(DsWebView *wv, const char *host, const char *folder)
{
    wchar_t wfolder[2048], whost[256];
    ICoreWebView2_3 *wv3 = NULL;
    HRESULT hr;
    if (!wv->webview) return 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, folder, -1, wfolder, 2048)) return 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256)) return 0;
    if (FAILED(wv->webview->lpVtbl->QueryInterface(
            wv->webview, &IID_ICoreWebView2_3, (void **)&wv3)) || !wv3) {
        snprintf(wv->err, sizeof wv->err,
                 "当前 WebView2 运行时太旧(缺 ICoreWebView2_3,无法映射本地目录)");
        return 0;
    }
    hr = wv3->lpVtbl->SetVirtualHostNameToFolderMapping(
        wv3, whost, wfolder, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    wv3->lpVtbl->Release(wv3);
    if (FAILED(hr)) {
        snprintf(wv->err, sizeof wv->err, "映射目录失败(hr=0x%08lx):%s → %s",
                 (unsigned long)hr, host, folder);
        return 0;
    }
    return 1;
}

static void apply_extra_maps(DsWebView *wv)
{
    int i;
    for (i = 0; i < wv->n_map; i++) {
        if (!wv->map[i].folder[0]) continue;
        map_one(wv, wv->map[i].host, wv->map[i].folder);
    }
}

/* 映射 + 导航。必须在**控制器就绪之后**做:SetVirtualHostNameToFolderMapping
 * 要经由 ICoreWebView2_3,而 webview 是控制器就绪时才拿得到的。 */
static int apply_navigation(DsWebView *wv)
{
    wchar_t whost[256], url[2400];
    if (!wv->webview || !wv->has_pending) return 0;
    MultiByteToWideChar(CP_UTF8, 0, wv->pending_host, -1, whost, 256);
    _snwprintf(url, 2400, L"https://%ls/%hs", whost, wv->pending_page);
    if (wv->pending_folder[0] && !map_one(wv, wv->pending_host, wv->pending_folder))
        return 0;
    apply_extra_maps(wv);
    {
        char url8[2400];
        WideCharToMultiByte(CP_UTF8, 0, url, -1, url8, sizeof url8, NULL, NULL);
        WVTR("映射完成,导航到 %s\n", url8);
        do_navigate(wv, url8);
    }
    return 1;
}

int ds_wv_map_folder(DsWebView *wv, const char *folder, const char *host)
{
    int i;
    if (!wv || !host || !*host) return 0;
    for (i = 0; i < wv->n_map; i++) {
        if (!strcmp(wv->map[i].host, host)) {
            snprintf(wv->map[i].folder, sizeof wv->map[i].folder, "%s",
                     folder ? folder : "");
            break;
        }
    }
    if (i == wv->n_map) {
        if (wv->n_map >= DS_WV_MAX_MAP) {
            snprintf(wv->err, sizeof wv->err, "虚拟主机映射数量超过上限(%d)",
                     DS_WV_MAX_MAP);
            return 0;
        }
        snprintf(wv->map[i].host, sizeof wv->map[i].host, "%s", host);
        snprintf(wv->map[i].folder, sizeof wv->map[i].folder, "%s",
                 folder ? folder : "");
        wv->n_map++;
    }
    WVTR("映射 %s → %s\n", host, folder ? folder : "");
    if (!wv->webview) return 1;    /* 还没就绪:apply_navigation 时会一起映射 */
    if (!folder || !*folder) return 1;
    return map_one(wv, host, folder);
}

static HRESULT STDMETHODCALLTYPE EnvHandler_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT result,
    ICoreWebView2Environment *env)
{
    EnvHandler *h = (EnvHandler *)This;
    DsWebView *wv = h->wv;
    CtrlHandler *ch;
    if (FAILED(result) || !env) {
        snprintf(wv->err, sizeof wv->err, "创建 WebView2 环境失败(hr=0x%08lx)",
                 (unsigned long)result);
        wv->failed = 1;
        return S_OK;
    }
    WVTR("环境就绪,创建控制器\n");
    wv->env = env;
    env->lpVtbl->AddRef(env);
    ch = calloc(1, sizeof *ch);
    ch->iface.lpVtbl = &g_ctrl_vtbl;
    ch->ref = 1;
    ch->wv = wv;
    if (FAILED(env->lpVtbl->CreateCoreWebView2Controller(
            env, (HWND)wv->parent, &ch->iface))) {
        snprintf(wv->err, sizeof wv->err, "创建 WebView2 控制器失败");
        wv->failed = 1;
        ch->iface.lpVtbl->Release(&ch->iface);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CtrlHandler_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT result,
    ICoreWebView2Controller *controller)
{
    CtrlHandler *h = (CtrlHandler *)This;
    DsWebView *wv = h->wv;
    ICoreWebView2 *webview = NULL;
    if (FAILED(result) || !controller) {
        snprintf(wv->err, sizeof wv->err, "创建 WebView2 控制器失败(hr=0x%08lx)",
                 (unsigned long)result);
        wv->failed = 1;
        return S_OK;
    }
    WVTR("控制器就绪\n");
    wv->controller = controller;
    controller->lpVtbl->AddRef(controller);
    if (FAILED(controller->lpVtbl->get_CoreWebView2(controller, &webview)) || !webview) {
        snprintf(wv->err, sizeof wv->err, "取 CoreWebView2 失败");
        wv->failed = 1;
        return S_OK;
    }
    wv->webview = webview;
    {
        MsgHandler *mh = calloc(1, sizeof *mh);
        EventRegistrationToken tok;
        mh->iface.lpVtbl = &g_msg_vtbl;
        mh->ref = 1;
        mh->wv = wv;
        webview->lpVtbl->add_WebMessageReceived(webview, &mh->iface, &tok);
    }
    {
        NavHandler *nh = calloc(1, sizeof *nh);
        EventRegistrationToken tok;
        nh->iface.lpVtbl = &g_nav_vtbl;
        nh->ref = 1;
        nh->wv = wv;
        webview->lpVtbl->add_NavigationCompleted(webview, &nh->iface, &tok);
    }
    {
        ICoreWebView2Settings *st = NULL;
        if (SUCCEEDED(webview->lpVtbl->get_Settings(webview, &st)) && st) {
            st->lpVtbl->put_AreDefaultContextMenusEnabled(st, FALSE);
            st->lpVtbl->put_IsZoomControlEnabled(st, FALSE);
            st->lpVtbl->Release(st);
        }
    }
    wv->ready = 1;
    WVTR("webview 可用,准备导航 pending=%d\n", wv->has_pending);
    if (wv->has_pending) apply_navigation(wv);
    return S_OK;
}

/* ------------------------------------------------------------ 对外接口 */

DsWebView *ds_wv_create(void *parent_hwnd, DsWebMessageFn on_message, void *user)
{
    DsWebView *wv;
    EnvHandler *eh;
    wchar_t user_data[MAX_PATH * 2];
    HRESULT hr;

    if (!ds_wv_load(NULL)) {
        /* 宿主通常已经调用过 ds_wv_load(exe_dir),这里再兜一次 */
    }
    if (!g_create_env) {
        fprintf(stderr, "dexstudio: %s\n", g_load_err);
        return NULL;
    }
    wv = calloc(1, sizeof *wv);
    wv->parent = parent_hwnd;
    wv->on_message = on_message;
    wv->user = user;
    /* 用户数据目录:放在 %LOCALAPPDATA%\DexStudio(不要污染仓库) */
    {
        wchar_t *lad = NULL;
        size_t n = 0;
        _wgetenv_s(&n, NULL, 0, L"LOCALAPPDATA");
        if (n) {
            lad = malloc(n * sizeof(wchar_t));
            _wgetenv_s(&n, lad, n, L"LOCALAPPDATA");
        }
        if (lad) {
            _snwprintf(user_data, MAX_PATH * 2, L"%ls\\DexStudio\\WebView2", lad);
            free(lad);
        } else {
            wcscpy(user_data, L"DexStudioWebView2");
        }
    }
    eh = calloc(1, sizeof *eh);
    eh->iface.lpVtbl = &g_env_vtbl;
    eh->ref = 1;
    eh->wv = wv;
    WVTR("创建环境 user_data=%ls\n", user_data);
    hr = g_create_env(NULL, user_data, NULL, &eh->iface);
    if (FAILED(hr)) {
        snprintf(wv->err, sizeof wv->err, "CreateCoreWebView2EnvironmentWithOptions 失败"
                                          "(hr=0x%08lx)", (unsigned long)hr);
        wv->failed = 1;
        eh->iface.lpVtbl->Release(&eh->iface);
    }
    eh->iface.lpVtbl->Release(&eh->iface);
    return wv;
}

void ds_wv_set_bounds(DsWebView *wv, int x, int y, int w, int h)
{
    RECT r;
    if (!wv || !wv->controller) return;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    wv->controller->lpVtbl->put_Bounds(wv->controller, r);
}

int ds_wv_navigate_folder(DsWebView *wv, const char *folder, const char *host,
                          const char *page)
{
    if (!wv) return 0;
    snprintf(wv->pending_folder, sizeof wv->pending_folder, "%s", folder ? folder : "");
    snprintf(wv->pending_host, sizeof wv->pending_host, "%s", host ? host : "local");
    snprintf(wv->pending_page, sizeof wv->pending_page, "%s", page ? page : "index.html");
    wv->has_pending = 1;
    WVTR("请求导航 %s (%s/%s) ready=%d\n", wv->pending_folder, wv->pending_host,
         wv->pending_page, wv->ready);
    if (wv->ready) return apply_navigation(wv);
    return 1;   /* 控制器还没就绪:就绪时自动应用(见 CtrlHandler_Invoke) */
}

int ds_wv_eval(DsWebView *wv, const char *js)
{
    EvalHandler *eh;
    int n;
    LPWSTR w;
    if (!wv || !wv->webview || !js) return 0;
    eh = calloc(1, sizeof *eh);
    eh->iface.lpVtbl = &g_eval_vtbl;
    eh->ref = 1;
    eh->wv = wv;
    n = MultiByteToWideChar(CP_UTF8, 0, js, -1, NULL, 0);
    w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) { free(eh); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, js, -1, w, n);
    WVTR("执行 JS:%s\n", js);
    wv->webview->lpVtbl->ExecuteScript(wv->webview, w, &eh->iface);
    free(w);
    return 1;
}

int ds_wv_post(DsWebView *wv, const char *json)
{
    int n;
    LPWSTR w;
    if (!wv || !wv->webview || !json) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, json, -1, NULL, 0);
    w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) return 0;
    MultiByteToWideChar(CP_UTF8, 0, json, -1, w, n);
    wv->webview->lpVtbl->PostWebMessageAsJson(wv->webview, w);
    free(w);
    return 1;
}

int ds_wv_ready(const DsWebView *wv) { return wv && wv->ready; }
const char *ds_wv_error(const DsWebView *wv) { return wv ? wv->err : g_load_err; }

void ds_wv_destroy(DsWebView *wv)
{
    if (!wv) return;
    if (wv->controller) {
        wv->controller->lpVtbl->Close(wv->controller);
        wv->controller->lpVtbl->Release(wv->controller);
    }
    if (wv->webview) wv->webview->lpVtbl->Release(wv->webview);
    if (wv->env) wv->env->lpVtbl->Release(wv->env);
    free(wv);
}
