/* ============================================================================
 * ds_engine.c — 引擎动态绑定(见 ds_engine.h)。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "ds_engine.h"
#include "ds_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

#define DS_ENGINE_NAME "libdexgame.dll"

static void eng_strcpy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    snprintf(dst, n, "%s", src ? src : "");
}

#if defined(_WIN32)

#define BIND_I(field)                                                           \
    do {                                                                        \
        e->field = (DsFnI)(void *)GetProcAddress(h, "eng_" #field);              \
        if (!e->field) { missing[miss_n++] = "eng_" #field; }                    \
    } while (0)
#define BIND_F(field)                                                           \
    do {                                                                        \
        e->field = (DsFnF)(void *)GetProcAddress(h, "eng_" #field);              \
        if (!e->field) { missing[miss_n++] = "eng_" #field; }                    \
    } while (0)
#define BIND_S(field)                                                           \
    do {                                                                        \
        e->field = (DsFnS)(void *)GetProcAddress(h, "eng_" #field);              \
        if (!e->field) { missing[miss_n++] = "eng_" #field; }                    \
    } while (0)

static void *try_load(const char *path)
{
    if (!path || !*path) return NULL;
    {
        /* 引擎 DLL 可能就在中文目录里:LoadLibraryA 会按 ANSI 解路径 */
        wchar_t *w = dsu_w(path);
        void *h = w ? (void *)LoadLibraryW(w) : NULL;
        free(w);
        return h;
    }
}

int ds_engine_load(DsEngine *e, const char *dll_path, const char *exe_dir)
{
    void *h = NULL;
    const char *missing[32];
    int miss_n = 0;
    char cand[1024];
    char dir[1024];
    int i;

    memset(e, 0, sizeof *e);
    if (dll_path && *dll_path) h = try_load(dll_path);
    if (!h && exe_dir && *exe_dir) {
        snprintf(cand, sizeof cand, "%s\\%s", exe_dir, DS_ENGINE_NAME);
        h = try_load(cand);
    }
    if (!h) {
        /* 开发目录布局:<repo>/dexstudio/host/ → <repo>/libs/dexgame/ */
        if (exe_dir && *exe_dir) {
            snprintf(dir, sizeof dir, "%s\\..\\..\\libs\\dexgame\\%s", exe_dir, DS_ENGINE_NAME);
            h = try_load(dir);
            if (!h) {
                snprintf(dir, sizeof dir, "%s\\libs\\dexgame\\%s", exe_dir, DS_ENGINE_NAME);
                h = try_load(dir);
            }
        }
    }
    if (!h) {
        const char *env = getenv("DEXSTUDIO_ENGINE_DLL");
        if (env && *env) h = try_load(env);
    }
    if (!h) {
        eng_strcpy(e->err, sizeof e->err,
                   "找不到 " DS_ENGINE_NAME "(可用 DEXSTUDIO_ENGINE_DLL 指定路径)");
        return 0;
    }
    e->dll = h;
    BIND_I(init_offscreen); BIND_I(shutdown);
    BIND_I(object_new); BIND_I(object_free); BIND_I(object_alive);
    BIND_I(object_count); BIND_I(object_id_at); BIND_I(scene_clear);
    BIND_I(set_name); BIND_I(find); BIND_S(name);
    BIND_I(attach); BIND_I(detach); BIND_I(has);
    BIND_I(set_i); BIND_I(set_f); BIND_I(set_s);
    BIND_I(get_i); BIND_F(get_f); BIND_S(get_s);
    BIND_I(comp_count); BIND_I(field_count); BIND_I(field_type); BIND_I(field_persist);
    BIND_S(comp_name_at); BIND_S(field_name);
    BIND_I(scene_save); BIND_I(scene_load); BIND_I(scene_load_json); BIND_S(scene_json);
    BIND_I(frame_begin); BIND_I(frame_end); BIND_I(draw_scene);
    BIND_I(pixel); BIND_I(save_bmp); BIND_I(set_clear_color);
    BIND_I(width); BIND_I(height); BIND_I(set_view);
    BIND_I(tex_width); BIND_I(tex_height);
    BIND_I(set_asset_dir);
    BIND_I(tilemap_load_csv); BIND_I(tilemap_load_file); BIND_I(tilemap_save_csv);
    BIND_I(tilemap_tile); BIND_I(tilemap_cols); BIND_I(tilemap_rows);
    BIND_F(world_x); BIND_F(world_y);
    BIND_S(last_error); BIND_I(clear_error);

    if (miss_n) {
        char msg[512];
        size_t o = 0;
        o += (size_t)snprintf(msg + o, sizeof msg - o, "引擎缺少导出:");
        for (i = 0; i < miss_n && o + 40 < sizeof msg; i++)
            o += (size_t)snprintf(msg + o, sizeof msg - o, " %s", missing[i]);
        eng_strcpy(e->err, sizeof e->err, msg);
        ds_engine_unload(e);
        return 0;
    }
    e->ok = 1;
    return 1;
}

void ds_engine_unload(DsEngine *e)
{
    if (e->dll) FreeLibrary((HMODULE)e->dll);
    e->dll = NULL;
    e->ok = 0;
}

#else /* 非 Windows:IDE 只在 Windows 上构建,这里留个明确失败 */

int ds_engine_load(DsEngine *e, const char *dll_path, const char *exe_dir)
{
    (void)dll_path; (void)exe_dir;
    memset(e, 0, sizeof *e);
    eng_strcpy(e->err, sizeof e->err, "DexStudio 目前只在 Windows 上构建");
    return 0;
}
void ds_engine_unload(DsEngine *e) { e->ok = 0; e->dll = NULL; }

#endif

const char *ds_engine_last_error(DsEngine *e)
{
    if (!e->ok || !e->last_error) return e->err;
    {
        DexValue none[1];
        const char *s = e->last_error(none, 0);
        return s ? s : "";
    }
}
