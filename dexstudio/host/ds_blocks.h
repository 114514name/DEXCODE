/* ============================================================================
 * ds_blocks.h — DexStudio 的**积木脚本**(给零基础用户的"Scratch 式"玩法)。
 *
 * 为什么要有这一层:节点图(UE 蓝图式)对没写过代码的人仍然是"代码换了个形状"——
 * 引脚、执行流、数据线、表达式,门槛和写代码差不多。积木走的是另一条路:
 *
 *   · 每一段脚本 = **一个帽子积木**(当游戏开始 / 每一帧 / 当按下空格) + 一摞动作;
 *   · 积木读起来是**中文句子**("让 [玩家] 往 [右] 走,速度 [中]"),每个空都是下拉;
 *   · 不连线、不打字、不出现变量与表达式;
 *   · "如果…那么…"是 C 形积木,身子里面再放动作(和 Scratch 一样)。
 *
 * 数据在 <项目>/scripts/blocks.json;它和节点图共用同一份产物脚本 scripts/logic.dex,
 * 由 project.json 的 `logic_mode` 决定**当前用哪一套生成**(默认给新项目用积木)。
 * ==========================================================================*/
#ifndef DS_BLOCKS_H
#define DS_BLOCKS_H

#include "ds_model.h"

#define DS_BLOCKS_REL "scripts/blocks.json"
#define DS_BLOCKS_DEX_REL "scripts/logic.dex"

/* 从磁盘重载(没有项目/没有文件 → 空)。撤销恢复文件之后也要调。 */
void ds_blocks_reload(DsModel *m);
/* 内存里的积木文档 JSON 文本(借用指针) */
const char *ds_blocks_json(DsModel *m);
/* 存到 <项目>/scripts/blocks.json */
int ds_blocks_save(DsModel *m);
/* 生成 DexLang 源码(malloc;失败返回 NULL 并写 err)。 */
char *ds_blocks_generate(DsModel *m, char *err, unsigned errsz);
/* 生成到 scripts/logic.dex(同时把 blocks.json 存盘) */
int ds_blocks_generate_to_file(DsModel *m, char *out_path, unsigned pathsz,
                               char *err, unsigned errsz);
/* 命令分发 */
Dsj *ds_blocks_command(DsModel *m, const char *cmd, Dsj *args);

/* 一键模板:id = "move_jump" / "camera" / "sound"。失败返回 0 并写原因。
 * (project.new 的"能跑的最小项目"也用它,所以导出。) */
int ds_blocks_apply_template(DsModel *m, const char *id, char *err, unsigned errsz);

#endif /* DS_BLOCKS_H */
