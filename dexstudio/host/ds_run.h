/* ============================================================================
 * ds_run.h — 一键编译 / 独立窗口运行 / 输出与错误定位(B5)。
 * 见 ds_run.c 顶部的说明。
 * ==========================================================================*/
#ifndef DS_RUN_H
#define DS_RUN_H

#include "ds_model.h"

/* 命令分发:build.compile / build.run / build.stop / build.status /
 * project.scripts / file.read */
Dsj *ds_run_command(DsModel *m, const char *cmd, Dsj *args);

/* 独立窗口运行的状态(有游戏进程在跑时 running=1) */
int ds_run_active(DsModel *m);
unsigned long ds_run_pid(DsModel *m);
int ds_run_exit_code(DsModel *m);
void ds_run_kill(DsModel *m);

#endif /* DS_RUN_H */
