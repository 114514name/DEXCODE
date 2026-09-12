/* ============================================================================
 * ds_graph.h — DexStudio 的**逻辑图**(UE 蓝图式节点图,产品 B 的 B4)。
 *
 * 为什么自己实现而不是复用 bluedit:
 *   bluedit 是 Python + Tk,而产品 B 的目标是"整条链零 Python"。所以这里把它的
 *   **模型**搬成 C(与 B1 把 dexlang 搬成 dexc 是同一套路),前端换成 HTML canvas。
 *
 * 图 = 一棵 JSON DOM(与 m->project 同款),存在 <项目>/scripts/logic.json。
 * 用 DOM 而不是 C 结构体的理由:节点类型会长(事件/条件/动作/变量/表达式都在里面),
 * 而编辑器真正需要的操作只有"取字段、改字段、连线、写回去"—— 这正是 ds_json 的强项。
 * 节点语义(参数校验、代码生成)集中在 ds_graph.c 里,前端不做任何判断。
 * ==========================================================================*/
#ifndef DS_GRAPH_H
#define DS_GRAPH_H

#include "ds_model.h"

/* 逻辑图文件在项目里的相对路径 */
#define DS_GRAPH_REL "scripts/logic.json"
/* 由图生成的 DexLang 源文件(相对路径) */
#define DS_GRAPH_DEX_REL "scripts/logic.dex"

/* 从磁盘重载图(没有项目/没有文件 → 空图)。撤销恢复文件之后也要调它,
 * 否则内存里的图与磁盘会不一致。 */
void ds_graph_reload(DsModel *m);

/* 当前图的 JSON 文本(借用指针;没有图时返回一个空图) */
const char *ds_graph_json(DsModel *m);

/* 把图写到 <项目>/scripts/logic.json。失败返回 0 并把原因写进模型 err。 */
int ds_graph_save(DsModel *m);

/* 把图编译成 DexLang 源码文本(返回 malloc 的字符串 + 失败时写 err)。
 * 生成的源码交给 dexc.exe 编译 —— 生成的代码必须能过编译器,这是 B4 的验收线。 */
char *ds_graph_generate(DsModel *m, char *err, unsigned errsz);

/* 由 ds_command 分发的图相关命令(全部带原因失败)。 */
Dsj *ds_graph_command(DsModel *m, const char *cmd, Dsj *args);

/* 把图生成到 <项目>/scripts/logic.dex(同时把 logic.json 存盘)。失败返回 0,
 * 原因写进 err。 */
int ds_graph_generate_to_file(DsModel *m, char *out_path, unsigned pathsz,
                              char *err, unsigned errsz);

#endif /* DS_GRAPH_H */
