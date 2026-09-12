/* ============================================================================
 * dx_utf8.h — dexc 的「UTF-8 路径」小工具
 *
 * dexc 的字符串是 **UTF-8**(源码路径来自命令行、.dex 里的 include/资源路径,
 * 输出也全是 UTF-8),而 Windows 的窄字符 API 用 ANSI 代码页(中文系统 936)。
 * 直接拿 UTF-8 去 fopen/GetFileAttributesA,中文路径一律找不到;更麻烦的是
 * **命令行参数**:CRT 给的 argv 已经被按 ANSI 解过一遍,中文源码路径到这里就是乱的。
 *
 * 所以:参数从宽命令行重新取一次(UTF-8),文件系统操作统一走这一层。
 * 判据:dexc 代码里不该再出现裸的 fopen/GetFileAttributesA/_spawnv/_getcwd。
 * ==========================================================================*/
#ifndef DX_UTF8_H
#define DX_UTF8_H

#include <stdio.h>
#include <stddef.h>

/* UTF-8 → 宽字符 / 宽字符 → UTF-8(都是 malloc,调用者 free) */
wchar_t *dxu_w(const char *utf8);
char *dxu_u(const wchar_t *w);

FILE *dxu_fopen(const char *utf8_path, const char *mode);
int dxu_exists(const char *utf8_path);
int dxu_isdir(const char *utf8_path);
/* 当前工作目录(UTF-8;malloc)。取不到返回 NULL。 */
char *dxu_getcwd(void);
/* 目录列举:返回 malloc 的 UTF-8 名字数组(不含 . 与 ..,已排序),个数写进 *out_n */
char **dxu_listdir(const char *utf8_dir, int *out_n);

/* 命令行参数:从宽命令行重新取一份 UTF-8 的(CRT 的 argv 已是 ANSI 解码的)。
 * 返回 malloc 数组,长度写进 *argc;失败返回 NULL(调用方用原来的 argv)。 */
char **dxu_argv(int *argc);

/* 等价于 _spawnv(_P_WAIT, path, argv):argv 是 UTF-8 的 NULL 结尾数组 */
int dxu_spawn_wait(const char *utf8_path, char *const argv[]);

#endif /* DX_UTF8_H */
