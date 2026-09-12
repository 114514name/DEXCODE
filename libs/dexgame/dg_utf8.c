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
