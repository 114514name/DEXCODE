/* ============================================================================
 * ds_utf8.c — 见 ds_utf8.h 的说明(为什么需要这一层)。
 *
 * 实现要点:
 *   - 一律用 MultiByteToWideChar(CP_UTF8)/WideCharToMultiByte(CP_UTF8),
 *     不用 CP_ACP —— 用 CP_ACP 就等于什么都没做。
 *   - 不做"缓存路径"之类的小聪明:文件操作本来就少,简单正确优先。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "ds_utf8.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* mingw 的 <stdlib.h> 不声明它(MSVC 的会),但它就在 msvcrt 里 ——
 * 自己声明一份即可,省掉 CommandLineToArgvW 需要的 -lshell32。 */
#if defined(_WIN32)
int __cdecl __wgetmainargs(int *argc, wchar_t ***argv, wchar_t ***envp,
                           int expand_wildcards, int *newmode);
#endif

wchar_t *dsu_w(const char *utf8)
{
    int n;
    wchar_t *w;
    if (!utf8) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n)) { free(w); return NULL; }
    return w;
}

char *dsu_u(const wchar_t *w)
{
    int n;
    char *s;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) { free(s); return NULL; }
    return s;
}

FILE *dsu_fopen(const char *utf8_path, const char *mode)
{
    wchar_t *wp = dsu_w(utf8_path);
    wchar_t *wm = dsu_w(mode ? mode : "rb");
    FILE *f = NULL;
    if (wp && wm) f = _wfopen(wp, wm);
    free(wp);
    free(wm);
    return f;
}

int dsu_exists(const char *utf8_path)
{
    wchar_t *w = dsu_w(utf8_path);
    int ok;
    if (!w) return 0;
    ok = GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return ok;
}

long long dsu_mtime(const char *utf8_path)
{
    WIN32_FILE_ATTRIBUTE_DATA fd;
    wchar_t *w = dsu_w(utf8_path);
    int got;
    if (!w) return 0;
    got = GetFileAttributesExW(w, GetFileExInfoStandard, &fd) ? 1 : 0;
    free(w);
    if (!got) return 0;
    return ((long long)fd.ftLastWriteTime.dwHighDateTime << 32)
         | (long long)fd.ftLastWriteTime.dwLowDateTime;
}

int dsu_mkdir(const char *utf8_path)
{
    wchar_t *w = dsu_w(utf8_path);
    size_t i;
    DWORD a;
    if (!w) return 0;
    /* 逐级建目录:`C:\a\b\c` 里 b 不存在时也要能建出 c —— 用户在 IDE 里
     * 直接输入一个还不存在的多级项目路径是常事。中间层建不出来(比如已经存在、
     * 或碰到 UNC 的 \\server\share)就忽略,最后按"叶子存在且是目录"判定。 */
    for (i = 1; w[i]; i++) {
        if (w[i] == L'\\' || w[i] == L'/') {
            wchar_t c = w[i];
            w[i] = 0;
            CreateDirectoryW(w, NULL);
            w[i] = c;
        }
    }
    CreateDirectoryW(w, NULL);
    a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

int dsu_rmdir(const char *utf8_path)
{
    wchar_t *w = dsu_w(utf8_path);
    int ok = 0;
    if (w) ok = RemoveDirectoryW(w) ? 1 : 0;
    free(w);
    return ok;
}

int dsu_remove(const char *utf8_path)
{
    wchar_t *w = dsu_w(utf8_path);
    int ok = 0;
    if (w) ok = DeleteFileW(w) ? 1 : 0;
    free(w);
    return ok;
}

int dsu_copy_file(const char *src, const char *dst)
{
    wchar_t *ws = dsu_w(src);
    wchar_t *wd = dsu_w(dst);
    int ok = 0;
    if (ws && wd) ok = CopyFileW(ws, wd, TRUE) ? 1 : 0;   /* TRUE = 不覆盖 */
    free(ws);
    free(wd);
    return ok;
}

int dsu_move_file(const char *src, const char *dst)
{
    wchar_t *ws = dsu_w(src);
    wchar_t *wd = dsu_w(dst);
    int ok = 0;
    if (ws && wd) ok = MoveFileW(ws, wd) ? 1 : 0;
    free(ws);
    free(wd);
    return ok;
}

char *dsu_env(const char *name)
{
    wchar_t *wn = dsu_w(name);
    wchar_t buf[4096];
    DWORD n;
    char *out = NULL;
    if (!wn) return NULL;
    n = GetEnvironmentVariableW(wn, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    free(wn);
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return NULL;
    out = dsu_u(buf);
    return out;
}

void dsu_list(const char *utf8_pattern, DsuListFn cb, void *ud)
{
    wchar_t *wp = dsu_w(utf8_pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!wp || !cb) { free(wp); return; }
    h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *name = dsu_u(fd.cFileName);
        if (name) {
            long long size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            cb(name, size, (unsigned long)fd.dwFileAttributes, ud);
            free(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---------------------------------------------------------------- argv */

char *dsu_utf8_clean(const char *s)
{
    size_t n, i = 0, o = 0;
    char *out;
    if (!s) return NULL;
    n = strlen(s);
    out = (char *)malloc(n * 3 + 1);      /* 最坏:每个坏字节变成 3 字节 */
    if (!out) return NULL;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1
                : (c >= 0xC2 && c <= 0xDF) ? 2
                : (c >= 0xE0 && c <= 0xEF) ? 3
                : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        int ok = len > 0 && i + (size_t)len <= n;
        if (ok && len > 1) {
            int k;
            for (k = 1; k < len; k++) {
                unsigned char cc = (unsigned char)s[i + (size_t)k];
                if (cc < 0x80 || cc > 0xBF) { ok = 0; break; }
            }
        }
        if (ok) {
            memcpy(out + o, s + i, (size_t)len);
            o += (size_t)len;
            i += (size_t)len;
        } else {
            out[o++] = (char)0xEF;        /* U+FFFD */
            out[o++] = (char)0xBF;
            out[o++] = (char)0xBD;
            i++;
        }
    }
    out[o] = 0;
    return out;
}

char **dsu_argv(int *argc)
{
    wchar_t **wargv = NULL;
    char **argv;
    int n = 0, i;

#if defined(_WIN32)
    {
        /* __wgetmainargs 从 msvcrt 拿**宽**命令行(不经过 ANSI 那一趟),
         * 所以中文参数是完整的。它不需要 shell32/CommandLineToArgvW。 */
        wchar_t **wenv = NULL;
        int mode = 0;
        if (__wgetmainargs(&n, &wargv, &wenv, 0, &mode) != 0) return NULL;
    }
#else
    (void)wargv;
#endif
    if (n <= 0 || !wargv) return NULL;
    argv = (char **)calloc((size_t)n + 1, sizeof(char *));
    if (!argv) return NULL;
    for (i = 0; i < n; i++) argv[i] = dsu_u(wargv[i]);
    if (argc) *argc = n;
    return argv;
}

void dsu_argv_free(char **argv, int argc)
{
    int i;
    if (!argv) return;
    for (i = 0; i < argc; i++) free(argv[i]);
    free(argv);
}
