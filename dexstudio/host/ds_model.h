/* ============================================================================
 * ds_model.h — DexStudio 的**模型层**(IDE 的全部状态都在这里,UI 只是视图)。
 *
 * 为什么放在 C 宿主而不是 JS:
 *   1) 决策 #10(见 docs/DEXGAME_DESIGN.md §9.1):模型可以像 libdexgame 一样
 *      **离屏单测**(tests/test_dexstudio.py 用 ctypes 直接调),不需要开窗口;
 *   2) 顺带得到 `dexstudio --build <项目>` 这种无界面用法;
 *   3) 场景数据由引擎持有(eng_scene_json / eng_comp_* 自省),引擎加组件不用改 IDE。
 *
 * 对外只有一个入口:`ds_command(m, request_json) -> response_json`。
 * 前端(WebView2 里的 JS)与测试(ctypes)走的是**同一条**命令通道 —— 于是
 * "UI 能做的" 与 "测试能断言的" 天然一致。
 *
 * 协议:
 *   请求  {"cmd":"entity.add","args":{"name":"player"}}
 *   响应  {"ok":true,"result":{...}}  或  {"ok":false,"error":"原因"}
 * 返回的指针属于模型内部缓冲,**下次调用前有效**(与仓库里库函数的"借用"契约一致)。
 * ==========================================================================*/
#ifndef DS_MODEL_H
#define DS_MODEL_H

#include "ds_json.h"   /* 模型对外的少量命令返回/接收 JSON DOM(B4 的逻辑图要用) */

typedef struct DsModel DsModel;

/* exe_dir:宿主 exe 所在目录(用来找 libdexgame.dll);可为空。 */
DsModel *ds_model_create(const char *exe_dir);
void ds_model_destroy(DsModel *m);

int ds_model_engine_ok(const DsModel *m);
const char *ds_model_engine_error(const DsModel *m);

/* 命令通道(单一入口)。request 为 NULL/空 → 返回 app.info。 */
const char *ds_command(DsModel *m, const char *request);
const char *ds_state(DsModel *m);

/* 支持的组件名清单(自省自引擎;引擎不可用时返回空) */
const char *ds_components_json(DsModel *m);

/* 渲染预览落盘的目录(保证已创建)。前端用虚拟主机 dexstudio-preview.local
 * 映射到它,于是 `<img src=".../preview.bmp?t=N">` 就能显示离屏渲染结果。
 * 返回的是模型内部缓冲,下次调用前有效。 */
const char *ds_preview_dir(DsModel *m);

/* 当前项目的根目录(没有项目时为空串) */
const char *ds_project_dir(DsModel *m);
/* ---------------------------------------------------------------- 供 ds_graph.c 用
 * 逻辑图放在单独一个编译单元(ds_graph.c)里,所以这里给它最小的访问面:
 * 图的 DOM、项目根目录、带原因的失败、以及"一次编辑"的撤销包装。 */
Dsj *ds_model_graph(DsModel *m);              /* 内存里的图(可改;没有时为空图) */
void ds_model_set_graph(DsModel *m, Dsj *g);  /* 接管 g 的所有权 */
void ds_model_error(DsModel *m, const char *fmt, ...);
/* 一次编辑:open 拍快照,close 时 changed=1 才压撤销栈(逻辑图的改动也要能撤销)。 */
void *ds_model_edit_open(DsModel *m);
void ds_model_edit_close(DsModel *m, void *tok, int changed);
/* 文件小工具(逻辑图要用;与模型内部同一套实现,避免两份行为不一致) */
int ds_mkdir(const char *path);
int ds_write_text(const char *path, const char *text);
char *ds_read_text(const char *path, size_t *out_len);

/* ---------------------------------------------------------------- 给 ds_run.c 用
 * 编译/运行要拼路径、读源码、写错误信息、以及保存"正在运行的游戏进程"。 */
char *ds_strdup(const char *s);
char *ds_path_join(const char *a, const char *b);
char *ds_path_replace_ext(const char *path, const char *newext);
char *ds_file_read_text(const char *path, size_t *out_len);
char *ds_model_errbuf(DsModel *m);          /* 错误信息缓冲区(至少 512 字节) */
size_t ds_model_errbuf_size(void);
const char *ds_model_exe_dir(DsModel *m);
void **ds_model_proc_slot(DsModel *m);      /* HANDLE * 的地址(没有进程时为 NULL) */
unsigned long *ds_model_pid_slot(DsModel *m);

const char *ds_version(void);

/* 逻辑图(实现在 ds_graph.c) */
void ds_graph_reload(DsModel *m);
const char *ds_graph_json(DsModel *m);
int ds_graph_save(DsModel *m);
char *ds_graph_generate(DsModel *m, char *err, unsigned errsz);
Dsj *ds_graph_command(DsModel *m, const char *cmd, Dsj *args);
int ds_graph_generate_to_file(DsModel *m, char *out_path, unsigned pathsz,
                              char *err, unsigned errsz);
/* 编译/运行(实现在 ds_run.c) */
Dsj *ds_run_command(DsModel *m, const char *cmd, Dsj *args);

#endif /* DS_MODEL_H */
