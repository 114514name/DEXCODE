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

#endif /* DG_UTF8_H */
