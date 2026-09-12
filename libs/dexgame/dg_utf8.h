/* ============================================================================
 * dg_utf8.h — 引擎的「UTF-8 路径」小工具
 *
 * 引擎看到的路径全部来自 DexLang 字符串 / 场景 JSON,也就是 **UTF-8**;
 * 而 Windows 的窄字符 fopen 用的是 ANSI 代码页(中文系统 = 936)。
 * 直接用 fopen 的后果:资源放在中文目录里、或文件名是中文时,
 * "贴图/声音/瓦片 CSV/场景 JSON 一律加载失败"。
 *
 * 所以引擎里打开文件统一走 dg_fopen()(内部 _wfopen + UTF-8 → UTF-16)。
 * 判据:引擎代码里不该再出现裸的 fopen。
 * ==========================================================================*/
#ifndef DG_UTF8_H
#define DG_UTF8_H

#include <stdio.h>
#include <wchar.h>

/* UTF-8 → 宽字符;malloc 出来的,调用者 free。失败返回 NULL。 */
wchar_t *dg_w(const char *utf8);

/* fopen 的 UTF-8 版本(内部 _wfopen)。始终返回 FILE*,失败为 NULL。 */
FILE *dg_fopen(const char *utf8_path, const char *mode);

/* ---------------------------------------------------------------- 资源根 */

/* 资源根目录:相对路径先拼到它后面再打开;空 = 按当前工作目录(旧行为)。
 *
 * 为什么需要它:场景/代码里写的是 **项目相对** 路径(`res/hero.png`)——
 *   - 游戏由 IDE 启动时工作目录就是项目根,所以能用;
 *   - 但 IDE 自己那个引擎实例的工作目录是 IDE 的目录,`res/hero.png` 必然打不开
 *     ("设置 sprite.tex_path 失败: cannot open image"),而把绝对路径写进场景 JSON
 *     又让项目一搬家就废。
 * 于是加一个资源根:IDE 打开项目时设成项目根,运行时按需设成游戏目录。 */
void dg_asset_dir_set(const char *utf8_dir);
const char *dg_asset_dir(void);

/* 把相对路径按资源根拼好,再 dg_fopen。绝对路径(盘符或 UNC)原样用。 */
FILE *dg_fopen_asset(const char *utf8_path, const char *mode);

#endif /* DG_UTF8_H */
