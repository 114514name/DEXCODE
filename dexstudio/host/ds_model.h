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

const char *ds_version(void);

#endif /* DS_MODEL_H */
