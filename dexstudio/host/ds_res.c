/* ============================================================================
 * ds_res.c — 资源管理 + 自动保存 / 崩溃恢复(B6)。
 *
 * 资源:
 *   res.list            <root>/res/ 下的文件(名字/大小/类型/虚拟主机 URL)
 *   res.import {src}    把一个外部文件**复制**进 res/(同名就报错,不覆盖用户文件)
 *   res.pick            弹系统「打开文件」对话框选一个文件(返回路径,不复制)
 *   res.delete {name}   删掉 res/ 里的一个文件
 *   res.rename {name,to}
 * 图片走**虚拟主机**显示(宿主把 <项目根> 映射成 dexstudio-proj.local),
 * 所以前端直接 `<img src="https://dexstudio-proj.local/res/x.png">`,不走消息通道。
 *
 * 自动保存 / 崩溃恢复:
 *   编辑期每隔一段时间把"当前场景 + 场景引用的 CSV + 逻辑图"整包写进
 *   <root>/.dexstudio/autosave.json(C 决定**存什么、存哪儿**,前端只负责按节拍
 *   调 autosave.tick —— 宿主没有自己的定时器,节拍由 UI 的消息循环带过来)。
 *   `app.info` 会报 recoverable:autosave 比场景文件**新**,说明上次没正常收尾
 *   (崩溃/强杀),于是前端提示"恢复 / 丢弃"。正常保存(project.save)会清掉它。
 *
 * 为什么存整包而不是只存场景:瓦片数据在外部 CSV、逻辑图在 logic.json —— 只恢复
 * 场景 JSON 会得到一个"看起来对了、瓦片和图都不对"的项目。
 * ==========================================================================*/
#include "ds_res.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <commdlg.h>
#endif

#define DS_AUTOSAVE_DIR ".dexstudio"
#define DS_AUTOSAVE_FILE ".dexstudio\\autosave.json"

static void seterr(DsModel *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ds_model_errbuf(m), ds_model_errbuf_size(), fmt, ap);
    va_end(ap);
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static long long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n < 0 ? -1 : (long long)n;
}

/* 修改时间(秒;0 = 读不到)。用来判断"自动保存比场景文件新"。 */
static long long file_mtime(const char *p)
{
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fd)) return 0;
    return ((long long)fd.ftLastWriteTime.dwHighDateTime << 32)
         | (long long)fd.ftLastWriteTime.dwLowDateTime;
#else
    (void)p;
    return 0;
#endif
}

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static int is_image(const char *ext)
{
    return !strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg")
        || !strcmp(ext, "bmp") || !strcmp(ext, "gif");
}

static int is_audio(const char *ext)
{
    return !strcmp(ext, "wav") || !strcmp(ext, "mp3") || !strcmp(ext, "ogg");
}

static char *res_dir(DsModel *m)
{
    const char *root = ds_project_dir(m);
    if (!root || !*root) return NULL;
    return ds_path_join(root, "res");
}

const char *ds_res_url(const char *name)
{
    /* 只在 JSON 里给前端一个相对 URL;前端自己拼虚拟主机前缀 */
    (void)name;
    return "";
}

/* ------------------------------------------------------------ res.* */

static Dsj *cmd_res_list(DsModel *m)
{
    Dsj *a = dsj_arr();
    char *dir = res_dir(m);
    Dsj *r;
    if (!dir) {
        seterr(m, "还没有打开项目");
        return NULL;
    }
    ds_mkdir(dir);
#if defined(_WIN32)
    {
        char pat[1400];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*", dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                Dsj *o;
                const char *ext;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (fd.cFileName[0] == '.' && fd.cFileName[1] == 0) continue;
                ext = ext_of(fd.cFileName);
                o = dsj_obj();
                dsj_set_str(o, "name", fd.cFileName);
                dsj_set_int(o, "size",
                            ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
                dsj_set_str(o, "ext", ext);
                dsj_set_str(o, "kind", is_image(ext) ? "image"
                                      : (is_audio(ext) ? "audio" : "other"));
                dsj_set_str(o, "url", "");
                dsj_push(a, o);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#endif
    free(dir);
    r = dsj_obj();
    dsj_set(r, "files", a);
    dsj_set_int(r, "count", dsj_len(a));
    return r;
}

static Dsj *cmd_res_import(DsModel *m, Dsj *args)
{
    const char *src = dsj_get_str(args, "src", "");
    const char *name = dsj_get_str(args, "name", "");
    char *dir, *dst;
    const char *base;
    Dsj *r;
    if (!src || !*src) {
        seterr(m, "res.import 需要 args.src(要导入的文件路径)");
        return NULL;
    }
    if (!file_exists(src)) {
        seterr(m, "要导入的文件不存在:%s", src);
        return NULL;
    }
    dir = res_dir(m);
    if (!dir) {
        seterr(m, "还没有打开项目");
        return NULL;
    }
    ds_mkdir(dir);
    if (name && *name) {
        base = name;
    } else {
        const char *slash = strrchr(src, '\\');
        const char *slash2 = strrchr(src, '/');
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        base = slash ? slash + 1 : src;
    }
    dst = ds_path_join(dir, base);
    free(dir);
    if (file_exists(dst)) {
        seterr(m, "res/ 里已经有 '%s' 了(先改名或删掉)", base);
        free(dst);
        return NULL;
    }
    if (!CopyFileA(src, dst, TRUE)) {
        seterr(m, "复制失败(%lu):%s → %s", (unsigned long)GetLastError(), src, dst);
        free(dst);
        return NULL;
    }
    r = dsj_obj();
    dsj_set_str(r, "name", base);
    dsj_set_int(r, "size", file_size(dst));
    dsj_set_str(r, "path", dst);
    free(dst);
    return r;
}

static Dsj *cmd_res_pick(DsModel *m, Dsj *args)
{
    Dsj *r = dsj_obj();
    (void)m;
    /* dry=1 只回"能弹但不能弹":自动测试(包括离屏自测)绝不能弹模态对话框 ——
     * 那会把整个无人值守流程挂住。所以留一个不开对话框的入口。 */
    if (dsj_get_bool(args, "dry", 0)) {
        dsj_set_bool(r, "picked", 0);
        dsj_set_bool(r, "dry", 1);
        dsj_set_str(r, "path", "");
        return r;
    }
#if defined(_WIN32)
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    const char *filter = dsj_get_str(args, "filter", "");
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = (filter && *filter) ? filter
        : "所有支持的资源\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.wav;*.mp3;*.ogg\0"
          "图片\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
          "声音\0*.wav;*.mp3;*.ogg\0所有文件\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        dsj_set_bool(r, "picked", 1);
        dsj_set_str(r, "path", buf);
    } else {
        dsj_set_bool(r, "picked", 0);
        dsj_set_str(r, "path", "");
    }
#else
    (void)args;
    dsj_set_bool(r, "picked", 0);
    dsj_set_str(r, "path", "");
    seterr(m, "文件对话框只在 Windows 上可用");
#endif
    return r;
}

/* 资源名必须是**纯文件名**:别让前端顺手把 `../../x` 传进来 */
static int name_is_safe(const char *name)
{
    if (!name || !*name) return 0;
    if (strstr(name, "..")) return 0;
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) return 0;
    return 1;
}

static Dsj *cmd_res_delete(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    char *dir, *path;
    Dsj *r;
    if (!name_is_safe(name)) {
        seterr(m, "资源名不合法:'%s'", name ? name : "");
        return NULL;
    }
    dir = res_dir(m);
    if (!dir) {
        seterr(m, "还没有打开项目");
        return NULL;
    }
    path = ds_path_join(dir, name);
    free(dir);
    if (!file_exists(path)) {
        seterr(m, "res/ 里没有 '%s'", name);
        free(path);
        return NULL;
    }
    if (!DeleteFileA(path)) {
        seterr(m, "删除失败(%lu):%s", (unsigned long)GetLastError(), path);
        free(path);
        return NULL;
    }
    r = dsj_obj();
    dsj_set_str(r, "deleted", name);
    free(path);
    return r;
}

static Dsj *cmd_res_rename(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    const char *to = dsj_get_str(args, "to", "");
    char *dir, *a, *b;
    Dsj *r;
    if (!name_is_safe(name) || !name_is_safe(to)) {
        seterr(m, "资源名不合法");
        return NULL;
    }
    dir = res_dir(m);
    if (!dir) {
        seterr(m, "还没有打开项目");
        return NULL;
    }
    a = ds_path_join(dir, name);
    b = ds_path_join(dir, to);
    free(dir);
    if (!file_exists(a)) {
        seterr(m, "res/ 里没有 '%s'", name);
        free(a); free(b);
        return NULL;
    }
    if (file_exists(b)) {
        seterr(m, "已经有一个 '%s' 了", to);
        free(a); free(b);
        return NULL;
    }
    if (!MoveFileA(a, b)) {
        seterr(m, "改名失败(%lu)", (unsigned long)GetLastError());
        free(a); free(b);
        return NULL;
    }
    r = dsj_obj();
    dsj_set_str(r, "name", to);
    free(a); free(b);
    return r;
}

/* ------------------------------------------------------------ 自动保存 */

static char *autosave_path(DsModel *m)
{
    const char *root = ds_project_dir(m);
    if (!root || !*root) return NULL;
    return ds_path_join(root, DS_AUTOSAVE_FILE);
}

/* 整包:场景 + 外部文件(瓦片 CSV / 逻辑图)。复用模型里那套快照原语。 */
static Dsj *autosave_bundle(DsModel *m)
{
    Dsj *o = dsj_obj();
    char *scene = ds_scene_snapshot(m);
    Dsj *files = ds_files_snapshot(m);
    dsj_set_int(o, "format", 1);
    dsj_set_str(o, "scene_path", ds_model_scene_path(m));
    dsj_set_str(o, "scene", scene ? scene : "{}");
    dsj_set(o, "files", files ? files : dsj_arr());
    free(scene);
    return o;
}

static int autosave_write(DsModel *m, char *err, unsigned errsz)
{
    char *path = autosave_path(m);
    char *dir;
    char *txt;
    int ok;
    if (!path) {
        if (err) snprintf(err, errsz, "还没有打开项目");
        return 0;
    }
    dir = ds_path_join(ds_project_dir(m), DS_AUTOSAVE_DIR);
    ds_mkdir(dir);
    free(dir);
    txt = dsj_dump(autosave_bundle(m));
    if (!txt) {
        if (err) snprintf(err, errsz, "序列化自动保存失败");
        free(path);
        return 0;
    }
    ok = ds_write_text(path, txt);
    if (!ok && err) snprintf(err, errsz, "写不进 %s", path);
    free(txt);
    free(path);
    return ok;
}

static Dsj *cmd_autosave_tick(DsModel *m)
{
    Dsj *r = dsj_obj();
    char err[256];
    if (!ds_project_dir(m) || !*ds_project_dir(m)) {
        dsj_set_bool(r, "saved", 0);
        dsj_set_str(r, "reason", "没有打开项目");
        return r;
    }
    if (!ds_model_dirty(m)) {
        dsj_set_bool(r, "saved", 0);
        dsj_set_str(r, "reason", "没有未保存改动");
        return r;
    }
    if (!autosave_write(m, err, sizeof err)) {
        seterr(m, "%s", err);
        return NULL;
    }
    r = dsj_obj();
    dsj_set_bool(r, "saved", 1);
    dsj_set_int(r, "seq", ds_model_autosave_seq(m));
    dsj_set_bool(r, "dirty", 1);
    return r;
}

/* 有没有"没正常收尾"的自动保存:它在,而且比场景文件新 */
static int recoverable(DsModel *m, Dsj **out)
{
    char *path = autosave_path(m);
    long long ta, ts;
    Dsj *dom = NULL;
    if (!path) return 0;
    if (!file_exists(path)) {
        free(path);
        return 0;
    }
    ta = file_mtime(path);
    ts = ds_model_scene_path(m) && *ds_model_scene_path(m)
        ? file_mtime(ds_model_scene_path(m)) : 0;
    if (ta <= ts) {           /* 场景比自动保存新 → 那是上一次正常保存后的残留 */
        free(path);
        return 0;
    }
    {
        char *txt = ds_file_read_text(path, NULL);
        char perr[128];
        if (txt) {
            dom = dsj_parse(txt, perr, sizeof perr);
            free(txt);
        }
    }
    if (!dom || dom->t != DSJ_OBJ) {
        if (dom) dsj_free(dom);
        free(path);
        return 0;                 /* 自动保存读不出来/不是对象:当没有 */
    }
    dsj_set_str(dom, "autosave_path", path);
    dsj_set_int(dom, "autosave_time", ta);
    if (out) *out = dom;          /* out 为空 = 只问"有没有" */
    else dsj_free(dom);
    free(path);
    return 1;
}

int ds_res_recoverable(DsModel *m) { return recoverable(m, NULL); }

static Dsj *cmd_recover_status(DsModel *m)
{
    Dsj *r = dsj_obj();
    Dsj *b = NULL;
    if (recoverable(m, &b)) {
        dsj_set_bool(r, "recoverable", 1);
        dsj_set_str(r, "scene_path", dsj_get_str(b, "scene_path", ""));
        dsj_set_int(r, "autosave_time", (long long)dsj_get_int(b, "autosave_time", 0));
        dsj_set_int(r, "files", dsj_len(dsj_get(b, "files")));
        dsj_free(b);
    } else {
        dsj_set_bool(r, "recoverable", 0);
    }
    return r;
}

/* 恢复:把自动保存里的场景 + 文件写回去(和撤销恢复走同一套原语) */
static Dsj *cmd_recover_apply(DsModel *m)
{
    Dsj *b = NULL;
    Dsj *r;
    if (!recoverable(m, &b)) {
        seterr(m, "没有可恢复的自动保存");
        return NULL;
    }
    if (!ds_scene_restore(m, dsj_get_str(b, "scene", "{}"),
                          dsj_get(b, "files"))) {
        dsj_free(b);
        return NULL;                      /* 原因已在模型 err 里 */
    }
    /* 恢复出来的东西还没存盘 → 标脏,并把自动保存删掉(它已经用过了) */
    ds_model_mark_dirty(m);
    {
        char *path = autosave_path(m);
        if (path) {
            DeleteFileA(path);
            free(path);
        }
    }
    dsj_free(b);
    r = dsj_obj();
    dsj_set_bool(r, "recovered", 1);
    dsj_set_str(r, "scene_path", ds_model_scene_path(m));
    return r;
}

static Dsj *cmd_recover_discard(DsModel *m)
{
    char *path = autosave_path(m);
    Dsj *r = dsj_obj();
    int ok = 0;
    if (path) {
        ok = file_exists(path) ? (DeleteFileA(path) ? 1 : 0) : 1;
        free(path);
    }
    if (!ok) {
        seterr(m, "删不掉自动保存文件");
        return NULL;
    }
    dsj_set_bool(r, "discarded", 1);
    return r;
}

/* 正常保存之后自动保存就没意义了 */
static Dsj *cmd_autosave_clear(DsModel *m)
{
    return cmd_recover_discard(m);
}

Dsj *ds_res_command(DsModel *m, const char *cmd, Dsj *args)
{
    if (!strcmp(cmd, "res.list")) return cmd_res_list(m);
    if (!strcmp(cmd, "res.import")) return cmd_res_import(m, args);
    if (!strcmp(cmd, "res.pick")) return cmd_res_pick(m, args);
    if (!strcmp(cmd, "res.delete")) return cmd_res_delete(m, args);
    if (!strcmp(cmd, "res.rename")) return cmd_res_rename(m, args);
    if (!strcmp(cmd, "autosave.tick")) return cmd_autosave_tick(m);
    if (!strcmp(cmd, "autosave.clear")) return cmd_autosave_clear(m);
    if (!strcmp(cmd, "recover.status")) return cmd_recover_status(m);
    if (!strcmp(cmd, "recover.apply")) return cmd_recover_apply(m);
    if (!strcmp(cmd, "recover.discard")) return cmd_recover_discard(m);
    seterr(m, "未知命令 '%s'", cmd);
    return NULL;
}
