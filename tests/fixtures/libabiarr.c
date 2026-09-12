/* tests/fixtures/libabiarr.c — M0.5(值数组 ABI)测试库
 *
 * 验证「值数组调用约定」:声明了 `abi value_array;` 的 .dexdef,其导出函数签名为
 *
 *     <ret> name(const DexValue *args, int argc);
 *
 * 与直接 ABI 的区别:
 *   - 参数个数不再受「arity × 类型组合手写分发表」限制(可用到 8 个)
 *   - 实参以带标签的值数组传入,不再按声明类型逐个展开成 C 参数
 *   - VM 直接把操作数栈上的 Value 传过来(零编组),故 DexValue 必须与
 *     vm/vm.c 的 Value 布局一致:4 字节 tag + 4 字节填充 + 8 字节联合 = 16 字节
 *
 * 构建:
 *   zig cc -shared -target x86_64-windows-gnu -O2 -std=c11 \
 *       -o tests/fixtures/libabiarr.dll tests/fixtures/libabiarr.c
 */
#include <stdint.h>
#include <stdio.h>

#define EXPORT __declspec(dllexport)

/* 标签取值必须与 vm/vm.c 的 VType 一致 */
#define DEXV_INT   0
#define DEXV_FLOAT 1
#define DEXV_STR   2

typedef struct {
    uint32_t tag;
    union {
        int64_t i;
        double f;
        const char *s;
    } as;
} DexValue;

/* arity = 8 —— 直接 ABI 的编译期上限是 3,这条只有值数组 ABI 能过 */
EXPORT int64_t abi_sum8(const DexValue *a, int n) {
    int64_t s = 0;
    for (int k = 0; k < n; k++) {
        if (a[k].tag == DEXV_INT) s += a[k].as.i;
        else if (a[k].tag == DEXV_FLOAT) s += (int64_t)a[k].as.f;
    }
    return s;
}

/* 混合类型 + 返回字符串。按契约返回的是**借用**的静态缓冲,VM 会自行复制 */
EXPORT const char *abi_greet(const DexValue *a, int n) {
    static char buf[256];
    const char *who = "?";
    double scale = 1.0;
    long long times = 0;
    if (n > 0 && a[0].tag == DEXV_STR) who = a[0].as.s;
    if (n > 1 && a[1].tag == DEXV_INT) times = (long long)a[1].as.i;
    if (n > 2 && a[2].tag == DEXV_FLOAT) scale = a[2].as.f;
    snprintf(buf, sizeof buf, "hello %s x%lld scale=%.4g", who, times, scale);
    return buf;
}

/* 浮点返回 */
EXPORT double abi_avg(const DexValue *a, int n) {
    if (n <= 0) return 0.0;
    double s = 0;
    for (int k = 0; k < n; k++) {
        if (a[k].tag == DEXV_INT) s += (double)a[k].as.i;
        else if (a[k].tag == DEXV_FLOAT) s += a[k].as.f;
    }
    return s / (double)n;
}

/* void 返回 */
EXPORT void abi_noop(const DexValue *a, int n) {
    (void)a;
    (void)n;
}

/* 把看到的标签原样报出来 —— 用于断言「类型标签没有被 int/float 合并污染」 */
EXPORT const char *abi_tags(const DexValue *a, int n) {
    static char buf[128];
    int w = 0;
    for (int k = 0; k < n; k++) {
        const char c = a[k].tag == DEXV_INT ? 'i'
                      : a[k].tag == DEXV_FLOAT ? 'f'
                      : a[k].tag == DEXV_STR ? 's' : '?';
        if (w < (int)sizeof buf - 1) buf[w++] = c;
    }
    buf[w] = 0;
    return buf;
}
