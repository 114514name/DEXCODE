/* ============================================================================
 * ds_engine.h — DexStudio 对 dexgame 引擎的动态绑定。
 *
 * 为什么用"运行时 GetProcAddress"而不是链接导入库:
 *   - 引擎的导出是**值数组 ABI**(`int64_t eng_x(const DexValue*, int)`),没有
 *     .lib 可用(zig 只给出 DLL);而且 IDE 应该能在引擎缺失/换版本时给出**原因**
 *     而不是启动失败。这与 dexgame 自己用 d3dcompiler/xaudio2 的做法一致。
 *
 * 场景数据由引擎持有:IDE 不另造一套实体/组件模型 —— `eng_scene_json()` 就是
 * 场景文件的权威形式,`eng_comp_*`/`eng_field_*` 就是属性面板的数据源。
 * ==========================================================================*/
#ifndef DS_ENGINE_H
#define DS_ENGINE_H

#include "../../libs/dexgame/dexvalue.h"

typedef int64_t (*DsFnI)(const DexValue *, int);
typedef double (*DsFnF)(const DexValue *, int);
typedef const char *(*DsFnS)(const DexValue *, int);

typedef struct {
    void *dll;
    int ok;
    char err[512];

    /* 生命周期 */
    DsFnI init_offscreen, shutdown;
    /* 实体 */
    DsFnI object_new, object_free, object_alive, object_count, object_id_at, scene_clear;
    DsFnI set_name, find;
    DsFnS name;
    /* 组件与字段 */
    DsFnI attach, detach, has;
    DsFnI set_i, set_f, set_s;
    DsFnI get_i;
    DsFnF get_f;
    DsFnS get_s;
    DsFnI comp_count, field_count, field_type, field_persist;
    DsFnS comp_name_at, field_name;
    /* 场景 JSON */
    DsFnI scene_save, scene_load, scene_load_json;
    DsFnS scene_json;
    /* 渲染(离屏;B3 的视口预览用) */
    DsFnI frame_begin, frame_end, draw_scene, pixel, save_bmp, set_clear_color;
    DsFnF world_x, world_y;
    DsFnS last_error;
    DsFnI clear_error;
} DsEngine;

/* dll_path 为空时按顺序找:同目录 → ../../libs/dexgame/libdexgame.dll → 环境变量
 * DEXSTUDIO_ENGINE_DLL。成功返回 1。失败原因在 e->err。 */
int ds_engine_load(DsEngine *e, const char *dll_path, const char *exe_dir);
void ds_engine_unload(DsEngine *e);

/* 取引擎当前错误文本(没有则空串) */
const char *ds_engine_last_error(DsEngine *e);

#endif /* DS_ENGINE_H */
