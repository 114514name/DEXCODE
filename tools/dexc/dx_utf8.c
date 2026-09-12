/* ============================================================================
 * dx_utf8.c — 见 dx_utf8.h(为什么 dexc 也要这一层)。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "dx_utf8.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#  include <process.h>

/* mingw 的 <stdlib.h> 不声明它(MSVC 的会),但它就在 msvcrt 里 */
int __cdecl __wgetmainargs(int *argc, wchar_t ***argv, wchar_t ***envp,
                           int expand_wildcards, int *newmode);
#endif

wchar_t *dxu_w(const char *utf8)
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

char *dxu_u(const wchar_t *w)
{
#if defined(_WIN32)
    int n;
    char *s;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) { free(s); return NULL; }
    return s;
#else
    (void)w;
    return NULL;
#endif
}

FILE *dxu_fopen(const char *utf8_path, const char *mode)
{
#if defined(_WIN32)
    wchar_t *wp = dxu_w(utf8_path);
    wchar_t *wm = dxu_w(mode ? mode : "rb");
    FILE *f = NULL;
    if (wp && wm) f = _wfopen(wp, wm);
    free(wp);
    free(wm);
    return f;
#else
    return fopen(utf8_path, mode ? mode : "rb");
#endif
}

static unsigned long dxu_attrs(const char *utf8_path)
{
#if defined(_WIN32)
    wchar_t *w = dxu_w(utf8_path);
    DWORD a;
    if (!w) return (unsigned long)-1;
    a = GetFileAttributesW(w);
    free(w);
    return (unsigned long)a;
#else
    return 0;
#endif
}

int dxu_exists(const char *utf8_path)
{
#if defined(_WIN32)
    return dxu_attrs(utf8_path) != (unsigned long)-1;
#else
    FILE *f = fopen(utf8_path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
#endif
}

int dxu_isdir(const char *utf8_path)
{
#if defined(_WIN32)
    unsigned long a = dxu_attrs(utf8_path);
    return a != (unsigned long)-1 && (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
#else
    return 0;
#endif
}

char *dxu_getcwd(void)
{
#if defined(_WIN32)
    wchar_t wbuf[4096];
    if (!_wgetcwd(wbuf, (int)(sizeof wbuf / sizeof wbuf[0]))) return NULL;
    return dxu_u(wbuf);
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static int dxu_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
#endif

char **dxu_listdir(const char *utf8_dir, int *out_n)
{
    char **names = NULL;
    int n = 0, cap = 0;
    if (out_n) *out_n = 0;
#if defined(_WIN32)
    {
        char *pat = (char *)malloc(strlen(utf8_dir) + 3);
        wchar_t *wp;
        WIN32_FIND_DATAW fd;
        HANDLE h;
        if (!pat) return NULL;
        sprintf(pat, "%s\\*", utf8_dir);
        wp = dxu_w(pat);
        free(pat);
        if (!wp) return NULL;
        h = FindFirstFileW(wp, &fd);
        free(wp);
        if (h == INVALID_HANDLE_VALUE) return NULL;
        do {
            char *nm;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            nm = dxu_u(fd.cFileName);            /* 名字转成 UTF-8 再交出去 */
            if (!nm) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                names = (char **)realloc(names, (size_t)cap * sizeof *names);
                if (!names) { free(nm); return NULL; }
            }
            names[n++] = nm;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (n > 1) qsort(names, (size_t)n, sizeof *names, dxu_cmp);
#endif
    if (out_n) *out_n = n;
    return names;
}

char **dxu_argv(int *argc)
{
#if defined(_WIN32)
    wchar_t **wargv = NULL, **wenv = NULL;
    char **argv;
    int n = 0, i, mode = 0;
    if (__wgetmainargs(&n, &wargv, &wenv, 0, &mode) != 0) return NULL;
    if (n <= 0 || !wargv) return NULL;
    argv = (char **)calloc((size_t)n + 1, sizeof(char *));
    if (!argv) return NULL;
    for (i = 0; i < n; i++) argv[i] = dxu_u(wargv[i]);
    if (argc) *argc = n;
    return argv;
#else
    (void)argc;
    return NULL;
#endif
}

int dxu_spawn_wait(const char *utf8_path, char *const argv[])
{
#if defined(_WIN32)
    wchar_t *wp = dxu_w(utf8_path);
    wchar_t **wargv;
    int n = 0, i, rc;
    if (!wp) return -1;
    while (argv && argv[n]) n++;
    wargv = (wchar_t **)calloc((size_t)n + 1, sizeof(wchar_t *));
    if (!wargv) { free(wp); return -1; }
    for (i = 0; i < n; i++) wargv[i] = dxu_w(argv[i]);
    wargv[n] = NULL;
    rc = _wspawnv(_P_WAIT, wp, (const wchar_t *const *)wargv);
    for (i = 0; i < n; i++) free(wargv[i]);
    free(wargv);
    free(wp);
    return rc;
#else
    (void)utf8_path; (void)argv;
    return -1;
#endif
}
