/* tests/fixtures/libleaky.c - P2(FFI 字符串所有权)测试库
 *
 * 模拟一个"返回堆分配字符串"的原生库,并提供 (string)->void 的释放函数。
 * 验证:定义文件里声明了释放函数时,VM 会释放原生返回的缓冲区;
 *       未声明时该缓冲区会泄漏(对照 leaky_norel.dexdef)。
 *
 * 记账用指针登记表精确区分"自己分配的"与"借来的"(常量池字符串),
 * 因此 leaky_live() 才是真实存活块数。
 *
 * 构建:
 *   zig cc -shared -target x86_64-windows-gnu -O2 -std=c11  *       -o tests/fixtures/libleaky.dll tests/fixtures/libleaky.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
#define MAXREC 100000
static void *g_ptrs[MAXREC];
static int g_n = 0;
static long long g_owned_frees = 0, g_foreign_frees = 0;

EXPORT const char *leaky_greet(const char *who) {
    size_t n = strlen(who) + 16;
    char *p = (char *)malloc(n);
    if (!p) return "";
    snprintf(p, n, "hello, %s", who);
    if (g_n < MAXREC) g_ptrs[g_n++] = p;
    return p;
}
EXPORT void leaky_free(const char *p) {
    if (!p) return;
    for (int i = 0; i < g_n; i++) {
        if (g_ptrs[i] == (void *)p) { free((void *)p); g_ptrs[i] = NULL; g_owned_frees++; return; }
    }
    g_foreign_frees++;
}
EXPORT long long leaky_live(void) {
    long long n = 0;
    for (int i = 0; i < g_n; i++) if (g_ptrs[i]) n++;
    return n;
}

/* 诊断:C 侧无法直接比较地址时,用本函数报告传入指针是否在登记表内 */
EXPORT long long leaky_is_owned(const char *p) {
    for (int i = 0; i < g_n; i++) if (g_ptrs[i] == (void *)p) return 1;
    return 0;
}

EXPORT long long leaky_owned_frees(void) { return g_owned_frees; }
EXPORT long long leaky_foreign_frees(void) { return g_foreign_frees; }
