/* dexvalue.h — 值数组 ABI 的元素类型(原生库公开 ABI,见 docs/SPEC.md 4.5)
 *
 * 在 .dexdef 顶部写 `abi value_array;` 的库,其导出函数签名为:
 *
 *     <ret> name(const DexValue *args, int argc);
 *
 * 与"直接 ABI"的区别:
 *   - 参数个数上限 8(直接 ABI 只有 3)
 *   - 实参以**带标签**的数组传入,类型在运行时由 tag 决定,不再按声明类型
 *     逐个展开成 C 参数
 *   - VM 直接把操作数栈上的值原样传过来(零编组),所以本结构必须与
 *     vm/vm.c 的 Value 布局一致:4 字节 tag + 4 字节填充 + 8 字节联合 = 16 字节
 *     (vm.c 里有 _Static_assert 守着这一点)
 *
 * 返回字符串仍遵循既有契约:视为**借用**,VM 会立即复制一份自己的副本。
 */
#ifndef DEXVALUE_H
#define DEXVALUE_H

#include <stdint.h>

/* 标签。必须与 vm/vm.c 的 VType 枚举同序。 */
#define DEXV_INT   0
#define DEXV_FLOAT 1
#define DEXV_STR   2
#define DEXV_OBJ   3   /* 预留:结构体/引用(尚未实现)*/

typedef struct {
    uint32_t tag;
    union {
        int64_t i;
        double f;
        const char *s;
    } as;
} DexValue;

/* ---------- 取参辅助 ----------
   整数/浮点之间做**宽松互转**,与直接 ABI 的既有行为一致
   (老 ABI 会把 int 传给 float 形参、反之亦然),避免用户因为字面量写成
   1 还是 1.0 就踩坑。 */
static inline int64_t dv_int(const DexValue *v) {
    return v->tag == DEXV_FLOAT ? (int64_t)v->as.f : v->as.i;
}
static inline double dv_float(const DexValue *v) {
    return v->tag == DEXV_FLOAT ? v->as.f : (double)v->as.i;
}
static inline const char *dv_str(const DexValue *v) {
    return (v->tag == DEXV_STR && v->as.s) ? v->as.s : "";
}
/* 缺参时的默认值(引擎 API 大量用可选尾参) */
static inline int64_t dv_int_or(const DexValue *a, int n, int i, int64_t dflt) {
    return i < n ? dv_int(&a[i]) : dflt;
}
static inline double dv_float_or(const DexValue *a, int n, int i, double dflt) {
    return i < n ? dv_float(&a[i]) : dflt;
}
static inline const char *dv_str_or(const DexValue *a, int n, int i, const char *dflt) {
    return i < n ? dv_str(&a[i]) : dflt;
}

#endif /* DEXVALUE_H */
