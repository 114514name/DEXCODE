/* ============================================================================
 * ds_res.h — 资源管理 + 自动保存 / 崩溃恢复(B6)。见 ds_res.c 顶部说明。
 * ==========================================================================*/
#ifndef DS_RES_H
#define DS_RES_H

#include "ds_model.h"

Dsj *ds_res_command(DsModel *m, const char *cmd, Dsj *args);

/* 有没有"上次没正常收尾"的自动保存(app.info 会报给前端) */
int ds_res_recoverable(DsModel *m);

#endif /* DS_RES_H */
