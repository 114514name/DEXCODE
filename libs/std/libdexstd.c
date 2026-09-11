/*
 * libdexstd.c — DEXCODE 标准库(纯 C 实现,零第三方依赖)
 *
 * 本库完全由 C 编写,编译为 DLL 供 DexLang 通过 include/refer 调用。
 * 不依赖任何 Python 脚本;运行时只需 vm.exe + .dexbc(+ 本 DLL 或静态内嵌)。
 *
 * 分组:
 *   math   —— 数学:abs/fabs/pow/fmin/fmax/imin/imax/floor/ceil/round
 *   string —— 字符串:strlen/upper/lower/trim/concat/repeat/sub/contains
 *   io     —— 文件与输入:read_file/write_file/append_file/file_exists/
 *             remove_file/input/print
 *   time   —— 时间:now_ms/sleep_ms
 *   random —— 随机:rand/srand
 *   system —— 系统:system
 *
 * 所有签名都落在 C VM(vm.c) FFI 支持集合内(扩展后含
 * (string,string)、(string,int)、(string,int,int) 等组合)。
 *
 * 构建(Windows,zig):
 *   zig cc -shared -target x86_64-windows-gnu -O2 \
 *       -o libs/std/libdexstd.dll libs/std/libdexstd.c
 */

#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#include <windows.h>
#include <process.h>
#else
#define EXPORT
#include <time.h>
#include <unistd.h>
#endif

/* 字符串操作共享缓冲(VM 会用 xstrdup 复制返回值,可安全复用) */
static char g_sbuf[1 << 16];   /* 64 KB */
/* 文件读取专用大缓冲 */
static char g_fbuf[1 << 20];   /* 1 MB */

/* ============ math ============ */

/* int -> int:绝对值 */
EXPORT int64_t dex_abs(int64_t x) {
    /* -INT64_MIN 溢出(有符号取负在 C 中是 UB)。数学上结果不可表示,
       这里与 VM 的 % 处理保持一致:返回 INT64_MIN 而不是产生未定义行为。 */
    if (x == INT64_MIN) return INT64_MIN;
    return x < 0 ? -x : x;
}

/* float -> float:绝对值 */
EXPORT double dex_fabs(double x) {
    return x < 0.0 ? -x : x;
}

/* float,float -> float:幂(x^y) */
EXPORT double dex_pow(double x, double y) {
    return pow(x, y);
}

/* float,float -> float:较小者 */
EXPORT double dex_fmin(double a, double b) {
    return a < b ? a : b;
}

/* float,float -> float:较大者 */
EXPORT double dex_fmax(double a, double b) {
    return a > b ? a : b;
}

/* int,int -> int:较小者 */
EXPORT int64_t dex_imin(int64_t a, int64_t b) {
    return a < b ? a : b;
}

/* int,int -> int:较大者 */
EXPORT int64_t dex_imax(int64_t a, int64_t b) {
    return a > b ? a : b;
}

/* float -> float:向下取整 */
EXPORT double dex_floor(double x) {
    return floor(x);
}

/* float -> float:向上取整 */
EXPORT double dex_ceil(double x) {
    return ceil(x);
}

/* float -> float:四舍五入 */
EXPORT double dex_round(double x) {
    return round(x);
}

/* ============ string ============ */

/* string -> int:字符串长度 */
EXPORT int64_t dex_strlen(const char *s) {
    return s ? (int64_t)strlen(s) : 0;
}

/* string -> string:转大写 */
EXPORT const char *dex_str_upper(const char *s) {
    if (!s) return "";
    size_t i;
    for (i = 0; s[i] && i + 1 < sizeof(g_sbuf); i++)
        g_sbuf[i] = (char)toupper((unsigned char)s[i]);
    g_sbuf[i] = '\0';
    return g_sbuf;
}

/* string -> string:转小写 */
EXPORT const char *dex_str_lower(const char *s) {
    if (!s) return "";
    size_t i;
    for (i = 0; s[i] && i + 1 < sizeof(g_sbuf); i++)
        g_sbuf[i] = (char)tolower((unsigned char)s[i]);
    g_sbuf[i] = '\0';
    return g_sbuf;
}

/* string -> string:去除首尾空白 */
EXPORT const char *dex_str_trim(const char *s) {
    if (!s) return "";
    const char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t n = strlen(p);
    while (n > 0 && isspace((unsigned char)p[n - 1])) n--;
    if (n >= sizeof(g_sbuf)) n = sizeof(g_sbuf) - 1;
    memcpy(g_sbuf, p, n);
    g_sbuf[n] = '\0';
    return g_sbuf;
}

/* string,string -> string:拼接 */
EXPORT const char *dex_str_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t na = strlen(a), nb = strlen(b);
    if (na + nb >= sizeof(g_sbuf)) {
        if (na >= sizeof(g_sbuf)) na = sizeof(g_sbuf) - 1, nb = 0;
        else nb = sizeof(g_sbuf) - 1 - na;
    }
    memcpy(g_sbuf, a, na);
    memcpy(g_sbuf + na, b, nb);
    g_sbuf[na + nb] = '\0';
    return g_sbuf;
}

/* string,int -> string:重复 n 次 */
EXPORT const char *dex_str_repeat(const char *s, int64_t n) {
    if (!s) return "";
    size_t len = strlen(s);
    if (len == 0 || n <= 0) { g_sbuf[0] = '\0'; return g_sbuf; }
    size_t total = (size_t)n * len;
    if (total >= sizeof(g_sbuf)) total = sizeof(g_sbuf) - 1;
    size_t written = 0;
    while (written < total) {
        size_t take = (total - written < len) ? (total - written) : len;
        memcpy(g_sbuf + written, s, take);
        written += take;
    }
    g_sbuf[written] = '\0';
    return g_sbuf;
}

/* string,int,int -> string:子串 [start, start+len) */
EXPORT const char *dex_str_sub(const char *s, int64_t start, int64_t len) {
    if (!s) return "";
    size_t n = strlen(s);
    if (start < 0) start = 0;
    if ((size_t)start > n) start = (int64_t)n;
    if (len < 0) len = 0;
    if ((size_t)(start + len) > n) len = (int64_t)(n - (size_t)start);
    if ((size_t)len >= sizeof(g_sbuf)) len = (int64_t)(sizeof(g_sbuf) - 1);
    memcpy(g_sbuf, s + start, (size_t)len);
    g_sbuf[len] = '\0';
    return g_sbuf;
}

/* string,string -> int:是否包含子串(1/0) */
EXPORT int64_t dex_str_contains(const char *s, const char *sub) {
    if (!s || !sub) return 0;
    return strstr(s, sub) != NULL ? 1 : 0;
}

/* int -> string:整数转十进制字符串 */
EXPORT const char *dex_itoa(int64_t n) {
    snprintf(g_sbuf, sizeof(g_sbuf), "%lld", (long long)n);
    return g_sbuf;
}

/* float -> string:浮点转字符串 */
EXPORT const char *dex_ftoa(double x) {
    snprintf(g_sbuf, sizeof(g_sbuf), "%g", x);
    return g_sbuf;
}

/* ============ io / file ============ */

/* string -> string:读取整个文件;失败返回空串(可用 file_exists 判断) */
EXPORT const char *dex_read_file(const char *path) {
    if (!path) return "";
    FILE *f = fopen(path, "rb");
    if (!f) return "";
    size_t got = fread(g_fbuf, 1, sizeof(g_fbuf) - 1, f);
    fclose(f);
    g_fbuf[got] = '\0';
    return g_fbuf;
}

/* string,string -> int:写入文件(覆盖);成功 0,失败 -1 */
EXPORT int64_t dex_write_file(const char *path, const char *content) {
    if (!path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (content) fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 0;
}

/* string,string -> int:追加到文件;成功 0,失败 -1 */
EXPORT int64_t dex_append_file(const char *path, const char *content) {
    if (!path) return -1;
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    if (content) fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 0;
}

/* string -> int:文件是否存在(1/0) */
EXPORT int64_t dex_file_exists(const char *path) {
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* string -> int:删除文件;成功 0,失败 -1 */
EXPORT int64_t dex_remove_file(const char *path) {
    if (!path) return -1;
    return remove(path) == 0 ? 0 : -1;
}

/* () -> string:从标准输入读取一行(去掉换行) */
EXPORT const char *dex_input(void) {
    if (!fgets(g_sbuf, (int)sizeof(g_sbuf), stdin)) {
        g_sbuf[0] = '\0';
        return g_sbuf;
    }
    size_t n = strlen(g_sbuf);
    while (n > 0 && (g_sbuf[n - 1] == '\n' || g_sbuf[n - 1] == '\r')) g_sbuf[--n] = '\0';
    return g_sbuf;
}

/* string -> int:写入标准输出(不带换行),返回写入字节数 */
EXPORT int64_t dex_print(const char *s) {
    if (!s) return 0;
    fputs(s, stdout);
    return (int64_t)strlen(s);
}

/* string -> int:输出到标准输出,不自动换行(与 dex_print 相同)。
   需要换行时在字符串里写 "\n"(词法器会转义成真实换行),可精确控制输出。 */
EXPORT int64_t dex_out(const char *s) {
    if (!s) return 0;
    fputs(s, stdout);
    return (int64_t)strlen(s);
}

/* ============ time ============ */

/* () -> int:当前毫秒时间戳 */
EXPORT int64_t dex_now_ms(void) {
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#else
    return (int64_t)time(NULL) * 1000;
#endif
}

/* int -> int:休眠毫秒;返回 0 */
EXPORT int64_t dex_sleep_ms(int64_t ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)(ms * 1000));
#endif
    return 0;
}

/* ============ random ============ */

/* () -> int:0..RAND_MAX 随机数 */
EXPORT int64_t dex_rand(void) {
    return (int64_t)rand();
}

/* int -> int:设置随机种子;返回 0 */
EXPORT int64_t dex_srand(int64_t seed) {
    srand((unsigned)seed);
    return 0;
}

/* ============ system ============ */

/* string -> int:执行系统命令;返回退出码 */
EXPORT int64_t dex_system(const char *cmd) {
    if (!cmd) return -1;
    return (int64_t)system(cmd);
}
