/* ============================================================================
 * ds_utf8.h — 宿主的「UTF-8 路径」层
 *
 * 宿主里的字符串**全是 UTF-8**:JSON 命令通道、前端消息、磁盘上的
 * .dex/.json/.csv 路径、资源文件名……而 Windows 的**窄字符** API 用的是
 * ANSI 代码页(中文系统 = 936)。两边直接对接就会出三类现象:
 *
 *   1. 窗口标题中文乱码          (CreateWindowExA 把 UTF-8 字节当 GBK 解)
 *   2. 中文路径一律找不到文件    (fopen/GetFileAttributesA 同样按 GBK 解)
 *   3. 目录里的中文名进 JSON 变乱码(FindFirstFileA 返回 GBK 字节,前端按 UTF-8 解)
 *
 * 所以凡是要交给系统的东西,都在这一层转成宽字符,再调用 W 版 API。
 * 判据:代码里不该再出现 CreateFileA/FindFirstFileA/GetOpenFileNameA/CreateProcessA。
 * ==========================================================================*/
#ifndef DS_UTF8_H
#define DS_UTF8_H

#include <stdio.h>
#include <stddef.h>

/* UTF-8 → 宽字符;malloc 出来的,调用者 free。失败返回 NULL。 */
wchar_t *dsu_w(const char *utf8);
/* 宽字符 → UTF-8;malloc 出来的,调用者 free。失败返回 NULL。 */
char *dsu_u(const wchar_t *w);

FILE *dsu_fopen(const char *utf8_path, const char *mode);
int dsu_exists(const char *utf8_path);
long long dsu_mtime(const char *utf8_path);        /* 0 = 读不到 */
int dsu_mkdir(const char *utf8_path);
int dsu_rmdir(const char *utf8_path);
int dsu_remove(const char *utf8_path);
int dsu_copy_file(const char *src, const char *dst);  /* 目标已存在则失败 */
int dsu_move_file(const char *src, const char *dst);

/* 环境变量:窄的 getenv 在中文用户名下(路径本身带中文)就已经是乱码了。
 * 返回 malloc 的 UTF-8 串;没有这个变量返回 NULL。 */
char *dsu_env(const char *name);

/* 目录列举(模式是 UTF-8 的绝对路径,可含通配符,如 "C:\p\res\*")。
 * 回调拿到的是 **UTF-8** 名字,所以可以直接进 JSON。 */
typedef void (*DsuListFn)(const char *utf8_name, long long size,
                          unsigned long attrs, void *ud);
void dsu_list(const char *utf8_pattern, DsuListFn cb, void *ud);

/* 把一段**可能不是合法 UTF-8** 的字节清成合法 UTF-8(非法字节 → U+FFFD),
 * 返回 malloc 的字符串。
 *
 * 为什么要:被捕获的子进程输出会原样进 JSON,而老工具从 argv 里拿到的路径是
 * ANSI(GBK)字节 —— 混在 UTF-8 消息里就出现"半个汉字"。JSON 字符串一旦不是
 * 合法 UTF-8,前端的 JSON.parse 直接抛异常,整个输出面板就废了(实测踩到)。 */
char *dsu_utf8_clean(const char *s);

/* 命令行参数:CRT 给 main()/WinMain() 的 argv 已经按 ANSI 解过一遍,
 * 中文参数(项目路径、--command 里的 JSON)会烂掉。这里从宽字符重新取一份。
 * 返回 malloc 的数组(每项 malloc),长度写进 *argc;失败返回 NULL(用原来的 argv)。 */
char **dsu_argv(int *argc);
void dsu_argv_free(char **argv, int argc);

#endif /* DS_UTF8_H */
