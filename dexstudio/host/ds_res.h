/* ============================================================================
 * ds_res.h — 资源管理 + 自动保存 / 崩溃恢复(B6)。见 ds_res.c 顶部说明。
 * ==========================================================================*/
#ifndef DS_RES_H
#define DS_RES_H

#include "ds_model.h"

Dsj *ds_res_command(DsModel *m, const char *cmd, Dsj *args);

/* 有没有值得提示恢复的自动保存(app.info 会报给前端) */
int ds_res_recoverable(DsModel *m);

/* 提示恢复的原因:"" = 没有 / "crash" = 上次没正常收尾 / "unsaved" = 上次退出时
 * 还有没保存的改动。前端据此选措辞 —— 不能一律说"上次似乎没有正常退出"。 */
const char *ds_res_recover_kind(DsModel *m);

/* 正常退出时打个标记:把**本次运行写的**那份自动保存标成 clean=1。
 * 进程被强杀时走不到这里,于是"没打标记"就成了"上次是崩的"的证据。 */
void ds_res_autosave_mark_clean(DsModel *m);

#endif /* DS_RES_H */
