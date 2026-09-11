/*
 * libdexmath.c — DEXCODE 原生库示例(演示 include/refer + NCALL)
 *
 * 导出函数签名需与 math.dexdef 中的声明一致。
 * 构建(Windows,使用 zig): 
 *   zig cc -shared -target x86_64-windows-gnu -O2 -o libs/math/libdexmath.dll libs/math/libdexmath.c
 */

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

/* int,int -> int */
EXPORT int64_t dex_add(int64_t a, int64_t b) {
    return a + b;
}

/* int -> int :阶乘 */
EXPORT int64_t dex_fact(int64_t n) {
    int64_t r = 1;
    for (int64_t i = 2; i <= n; i++) r *= i;
    return r;
}

/* float -> float :开方(牛顿迭代,避免依赖 libm 链接) */
static double sqrt_newton(double x) {
    if (x <= 0.0) return 0.0;
    double g = x;
    for (int i = 0; i < 64; i++) g = (g + x / g) / 2.0;
    return g;
}
EXPORT double dex_sqrt(double x) {
    return sqrt_newton(x);
}

/* string -> int :字符串长度 */
EXPORT int64_t dex_len(const char *s) {
    return s ? (int64_t)strlen(s) : 0;
}

/* () -> string :返回字符串(VM 会复制) */
EXPORT const char *dex_hello(void) {
    return "hello from dll";
}

/* int,int,int -> int :三个数求和 */
EXPORT int64_t dex_sum3(int64_t a, int64_t b, int64_t c) {
    return a + b + c;
}
