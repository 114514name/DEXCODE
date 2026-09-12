/* ============================================================================
 * dg_utf8.c — 见 dg_utf8.h(为什么引擎也要这一层)。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dg_utf8.h"

#include <stdlib.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

wchar_t *dg_w(const char *utf8)
{
#if defined(_WIN32)
    int n;
    wchar_t *w;
    if (!utf8) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n)) { free(w); return NULL; }
    return w;
#else
    (void)utf8;
    return NULL;
#endif
}

FILE *dg_fopen(const char *utf8_path, const char *mode)
{
#if defined(_WIN32)
    wchar_t *wp = dg_w(utf8_path);
    wchar_t *wm = dg_w(mode ? mode : "rb");
    FILE *f = NULL;
    if (wp && wm) f = _wfopen(wp, wm);
    free(wp);
    free(wm);
    return f;
#else
    return fopen(utf8_path, mode ? mode : "rb");
#endif
}

/* ---------------------------------------------------------------- 资源根 */

static char g_asset_dir[1024];

void dg_asset_dir_set(const char *utf8_dir)
{
    snprintf(g_asset_dir, sizeof g_asset_dir, "%s", (utf8_dir && *utf8_dir) ? utf8_dir : "");
}

const char *dg_asset_dir(void) { return g_asset_dir; }

static int path_is_abs(const char *p)
{
    return (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/';
}

FILE *dg_fopen_asset(const char *utf8_path, const char *mode)
{
    char joined[2048];
    if (!utf8_path || !*utf8_path) return NULL;
    if (!g_asset_dir[0] || path_is_abs(utf8_path)) return dg_fopen(utf8_path, mode);
    snprintf(joined, sizeof joined, "%s/%s", g_asset_dir, utf8_path);
    return dg_fopen(joined, mode);
}
