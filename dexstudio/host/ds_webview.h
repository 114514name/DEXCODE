/* ============================================================================
 * ds_webview.h — WebView2 的最小封装(Windows 专用)。
 *
 * 为什么不用官方 WebView2Loader.lib:仓库里的第三方依赖一律"运行时加载 + 自己写
 * IID"(见 dexgame 的 dg_guids.h / d3dcompiler / xaudio2)。这样:
 *   - 构建只需要头文件(vendor 在 third_party/webview2/include);
 *   - WebView2Loader.dll 运行期从 exe 同目录加载,缺了就给出**明确原因**而不是启动崩。
 *
 * 线程模型:创建/导航都在主(UI)线程;消息处理在 WebView2 的回调线程上同步跑完,
 * 所以模型层不需要加锁 —— 宿主保证同一时刻只有一次 ds_command 调用。
 * ==========================================================================*/
#ifndef DS_WEBVIEW_H
#define DS_WEBVIEW_H

/* JS 消息回调:入参是请求 JSON,返回响应 JSON(借用指针,回调返回后即可释放)。
 * 返回 NULL 表示"不回消息"。 */
typedef const char *(*DsWebMessageFn)(void *user, const char *json);

typedef struct DsWebView DsWebView;

DsWebView *ds_wv_create(void *parent_hwnd, DsWebMessageFn on_message, void *user);
void ds_wv_set_bounds(DsWebView *wv, int x, int y, int w, int h);
/* 把 folder 目录映射到 https://<host>/ 并打开 <host>/<page>(推荐:给前端一个正常
 * 的 origin,JS 模块/fetch/本地存储都能用)。 */
int ds_wv_navigate_folder(DsWebView *wv, const char *folder, const char *host,
                          const char *page);
/* 直接朝 JS 推一条消息(宿主主动通知,如"项目已打开")。 */
int ds_wv_post(DsWebView *wv, const char *json);
/* 把**额外**的本地目录映射到 `https://<host>/`(如 dexstudio-preview.local →
 * 渲染预览目录、dexstudio-proj.local → 项目根)。前端就能用 `<img src>` 直接
 * 显示引擎渲染出的 BMP 与项目里的贴图,而不是把像素塞进 JSON。
 *
 * ⚠ **改指向之后必须重新导航**(ds_wv_reload),否则不生效:实测 WebView2 对
 * 一个**已经加载完的页面**再改映射,SetVirtualHostNameToFolderMapping 会返回
 * S_OK,但那个页面发出的请求照样失败(`img` 裂图标 / `fetch` 抛
 * TypeError: Failed to fetch)。宿主在打开/新建项目后自动重导航一次,见
 * ds_main.c 的 sync_host_mappings。 */
int ds_wv_map_folder(DsWebView *wv, const char *folder, const char *host);
/* 重新加载当前页面(映射改指向后调用它,新页面才认新映射)。没有导航过则忽略。 */
int ds_wv_reload(DsWebView *wv);
/* 在页面里跑一段 JS,结果经 on_message 以 `{"cmd":"ui.eval","args":{"result":…}}`
 * 的形式回来(ExecuteScript 本身是异步的)。诊断与测试用:能问出"桥在不在"、
 * "页面到底是哪一个"。 */
int ds_wv_eval(DsWebView *wv, const char *js);
int ds_wv_ready(const DsWebView *wv);
const char *ds_wv_error(const DsWebView *wv);      /* 失败原因(带得出原因) */
void ds_wv_destroy(DsWebView *wv);

/* 系统里装的 WebView2 运行时版本;失败返回 NULL 并把原因写进 err。
 * 不需要窗口 —— tests/test_dexstudio.py 用它做"整条链可用"的冒烟检查。 */
const char *ds_wv_runtime_version(char *err, unsigned errsz);

/* 加载 WebView2Loader.dll(<exe_dir>\WebView2Loader.dll)。
 * ds_wv_create 会自动调用;这里暴露给 --webview-version 之类的前置检查。 */
int ds_wv_load(const char *exe_dir);

#endif /* DS_WEBVIEW_H */
