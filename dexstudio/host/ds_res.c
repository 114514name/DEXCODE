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
#include "ds_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <commdlg.h>
#  include <shobjidl.h>
#  include <objbase.h>
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
    return dsu_exists(p);
}

static long long file_size(const char *p)
{
    FILE *f = dsu_fopen(p, "rb");
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
    return dsu_mtime(p);
}

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static int is_image(const char *ext)
{
    return !_stricmp(ext, "png") || !_stricmp(ext, "jpg")
        || !_stricmp(ext, "jpeg") || !_stricmp(ext, "bmp")
        || !_stricmp(ext, "gif");
}

static int is_audio(const char *ext)
{
    return !_stricmp(ext, "wav") || !_stricmp(ext, "mp3")
        || !_stricmp(ext, "ogg");
}

static char *res_dir(DsModel *m)
{
    const char *root = ds_project_dir(m);
    if (!root || !*root) return NULL;
    return ds_path_join(root, "res");
}

/* 资源名必须是**纯文件名**(定义在后面,这里先声明:导入路径也要校验) */
static int name_is_safe(const char *name);

const char *ds_res_url(const char *name)
{
    /* 只在 JSON 里给前端一个相对 URL;前端自己拼虚拟主机前缀 */
    (void)name;
    return "";
}

/* ------------------------------------------------------------ res.* */

#if defined(_WIN32)
/* "a\0b\0\0" 这种多字符串(OpenFileName 的 filter 就是它)按段转宽字符 */
static void utf8_multisz_to_wide(const char *s, wchar_t *out, size_t outsz)
{
    size_t o = 0, n = 0;
    if (!outsz) return;
    while (o + 1 < outsz) {
        size_t len = strlen(s + n);
        wchar_t *w;
        size_t i;
        if (len == 0) { out[o++] = 0; break; }   /* 空段 = 结束(补上第二个 0) */
        w = dsu_w(s + n);
        if (!w) break;
        for (i = 0; w[i] && o + 1 < outsz; i++) out[o++] = w[i];
        out[o++] = 0;
        free(w);
        n += len + 1;
    }
    out[outsz - 1] = 0;
}
#endif

/* 「选择文件夹」对话框(新建/打开项目用)。
 * 以前前端只能 prompt() 让用户手打路径 —— 键盘负担最大的一处。
 * 同一套 UTF-8 纪律:走 IFileDialog 的宽字符接口,拿回来再转 UTF-8。 */
int ds_pick_folder_utf8(char *out, size_t outsz)
{
#if defined(_WIN32)
    IFileDialog *dlg = NULL;
    IShellItem *item = NULL;
    HRESULT hr;
    int ok = 0;
    if (out && outsz) out[0] = 0;
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    /* RPC_E_CHANGED_MODE = 已经初始化过(别的线程模式),照样能用 */
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&dlg);
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->lpVtbl->GetOptions(dlg, &opts))) {
            dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM
                                             | FOS_PATHMUSTEXIST);
        }
        dlg->lpVtbl->SetTitle(dlg, L"选择一个文件夹");
        if (SUCCEEDED(dlg->lpVtbl->Show(dlg, NULL))
            && SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
            LPWSTR wpath = NULL;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &wpath))
                && wpath) {
                char *u = dsu_u(wpath);
                if (u) {
                    snprintf(out, outsz, "%s", u);
                    free(u);
                    ok = out[0] ? 1 : 0;
                }
                CoTaskMemFree(wpath);
            }
            item->lpVtbl->Release(item);
        }
        dlg->lpVtbl->Release(dlg);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok;
#else
    (void)out; (void)outsz;
    return 0;
#endif
}

/* dsu_list 的回调。名字由 dsu_list 转成 **UTF-8** 再交过来 —— 用
 * FindFirstFileA 的话这里是 GBK 字节,进了 JSON 前端就是乱码。 */
static void res_push_cb(const char *name, long long size, unsigned long attrs,
                        void *ud)
{
    Dsj *a = (Dsj *)ud;
    Dsj *o;
    const char *ext;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return;
    if (name[0] == '.' && name[1] == 0) return;
    ext = ext_of(name);
    o = dsj_obj();
    dsj_set_str(o, "name", name);
    dsj_set_int(o, "size", size);
    dsj_set_str(o, "ext", ext);
    dsj_set_str(o, "kind", is_image(ext) ? "image"
                          : (is_audio(ext) ? "audio" : "other"));
    dsj_set_str(o, "url", "");
    dsj_push(a, o);
}

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
    {
        char *pat = ds_path_join(dir, "*");
        dsu_list(pat, res_push_cb, a);
        free(pat);
    }
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
        if (!name_is_safe(name)) {          /* 与 delete/rename 同一条规则 */
            seterr(m, "资源名不合法:'%s'(只能是文件名,不能带路径)", name);
            free(dir);
            return NULL;
        }
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
    if (!dsu_copy_file(src, dst)) {
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
    /* 用 W 版对话框:中文路径(以及中文用户名下的整个 %USERPROFILE%)在 A 版里
     * 会被按 ANSI 解一遍,拿回来的就是乱码,`res.import` 随后必然"文件不存在"。 */
    wchar_t wbuf[MAX_PATH * 4] = {0};
    wchar_t wfilter[512];
    OPENFILENAMEW ofn;
    const char *filter = dsj_get_str(args, "filter", "");
    int multi = dsj_get_bool(args, "multi", 0);
    const char *use = (filter && *filter) ? filter
        : "所有支持的资源\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.wav;*.mp3;*.ogg\0"
          "图片\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0"
          "声音\0*.wav;*.mp3;*.ogg\0所有文件\0*.*\0";
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = wbuf;
    ofn.nMaxFile = (DWORD)(sizeof wbuf / sizeof wbuf[0]);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
              | (multi ? (OFN_ALLOWMULTISELECT | OFN_EXPLORER) : 0);
    /* 过滤器是 UTF-8,内嵌 '\0' 分段、最后双 '\0' 结尾 —— 手工转宽字符 */
    utf8_multisz_to_wide(use, wfilter, sizeof wfilter / sizeof wfilter[0]);
    ofn.lpstrFilter = wfilter;
    if (GetOpenFileNameW(&ofn)) {
        /* 多选时缓冲区是 "目录\0文件1\0文件2\0\0";单选时是完整路径 */
        wchar_t *p = wbuf;
        if (multi && p[wcslen(p) + 1] != 0) {
            wchar_t dir[MAX_PATH * 4];
            Dsj *arr = dsj_arr();
            int n = 0;
            wcscpy(dir, p);
            p += wcslen(p) + 1;
            while (*p) {
                wchar_t full[MAX_PATH * 4];
                char *u;
                _snwprintf(full, MAX_PATH * 4, L"%ls\\%ls", dir, p);
                u = dsu_u(full);
                if (u) {
                    if (n == 0) dsj_set_str(r, "path", u);
                    dsj_push(arr, dsj_str(u));
                    free(u);
                    n++;
                }
                p += wcslen(p) + 1;
            }
            dsj_set(r, "paths", arr);
            dsj_set_bool(r, "picked", n > 0);
            dsj_set_int(r, "count", n);
        } else {
            char *u = dsu_u(wbuf);
            dsj_set_bool(r, "picked", 1);
            dsj_set_str(r, "path", u ? u : "");
            {
                Dsj *arr = dsj_arr();
                if (u && *u) dsj_push(arr, dsj_str(u));
                dsj_set(r, "paths", arr);
            }
            free(u);
        }
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
    int refs = 0;
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
    /* 还被场景/逻辑图引用着就先别删(除非调用方明确 force:1)—— 用户视角
     * "删了个文件,游戏里贴图全裂" 是没法自己查出来的。 */
    refs = ds_model_asset_refs(m, name, NULL, 0);
    if (refs > 0 && !dsj_get_bool(args, "force", 0)) {
        seterr(m, "'%s' 还被 %d 处引用(贴图/声音/瓦片)。要删的话再确认一次,"
                  "或者先把引用改到别的资源上", name, refs);
        free(path);
        return NULL;
    }
    if (!dsu_remove(path)) {
        seterr(m, "删除失败(%lu):%s", (unsigned long)GetLastError(), path);
        free(path);
        return NULL;
    }
    r = dsj_obj();
    dsj_set_str(r, "deleted", name);
    dsj_set_int(r, "refs", refs);
    free(path);
    return r;
}

static Dsj *cmd_res_rename(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    const char *to = dsj_get_str(args, "to", "");
    char *dir, *a, *b;
    Dsj *r;
    int refs = 0, updated = 0;
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
    /* 先把引用改掉再改名(改引用失败就不改文件,免得留下"改完名字引用还是旧的") */
    if (dsj_get_bool(args, "update_refs", 1)) {
        refs = ds_model_asset_refs(m, name, to, 1);
        updated = refs;
    } else {
        refs = ds_model_asset_refs(m, name, NULL, 0);
    }
    if (!dsu_move_file(a, b)) {
        seterr(m, "改名失败(%lu)", (unsigned long)GetLastError());
        free(a); free(b);
        return NULL;
    }
    /* 只有**真的改了引用**才算场景脏(否则光改个文件名不该触发"未保存"提示与自动保存) */
    if (updated > 0) ds_model_mark_dirty(m);
    r = dsj_obj();
    dsj_set_str(r, "name", to);
    dsj_set_int(r, "refs", refs);
    dsj_set_int(r, "updated", updated);
    free(a); free(b);
    return r;
}

/* 某个资源被多少处引用(前端可以在改名前提示"有 N 处引用,是否一并更新") */
static Dsj *cmd_res_refs(DsModel *m, Dsj *args)
{
    const char *name = dsj_get_str(args, "name", "");
    Dsj *r = dsj_obj();
    if (!name_is_safe(name)) {
        seterr(m, "资源名不合法");
        return NULL;
    }
    dsj_set_int(r, "refs", ds_model_asset_refs(m, name, NULL, 0));
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
    /* 谁写的:恢复提示只认**上一次运行**留下的自动保存(见 recoverable)。
     * clean 由正常退出时补成 1 —— 强杀时补不上,所以 clean=0 就是"上次是崩的"。 */
    dsj_set_str(o, "session", ds_model_session(m));
    dsj_set_bool(o, "clean", 0);
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

/* 有没有值得提示恢复的自动保存。
 *
 * 三条判据(缺一不可):
 *   1. 自动保存文件在,并且**读得出来时间**(读不出来 = 判断不了 ⇒ 不提示);
 *   2. 它比场景文件新(或场景文件已经不在了)—— 旧的自动保存是上次正常存盘后的残留;
 *   3. 它**不是本次运行写的**。这一条是必须的:自动保存每 30 秒写一次,若不区分
 *      会话,正在编辑的这一次运行会不停地把"比场景新"的自动保存写出来,界面就会
 *      一直喊"上次好像没有正常退出",而且点恢复/丢弃都没用(30 秒后它又回来了)。 */
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
    if (ta <= 0) {            /* 读不出来 ⇒ 判断不了,别乱提示 */
        free(path);
        return 0;
    }
    ts = ds_model_scene_path(m) && *ds_model_scene_path(m)
        ? file_mtime(ds_model_scene_path(m)) : 0;
    if (ts > 0 && ta <= ts) {  /* 场景比自动保存新 → 上次正常保存后的残留 */
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
    /* 本次运行自己写的 → 不是"上次没正常退出",不提示(见上面第 3 条) */
    {
        const char *sess = dsj_get_str(dom, "session", "");
        const char *mine = ds_model_session(m);
        if (sess && *sess && mine && *mine && !strcmp(sess, mine)) {
            dsj_free(dom);
            free(path);
            return 0;
        }
    }
    dsj_set_str(dom, "autosave_path", path);
    dsj_set_int(dom, "autosave_time", ta);
    if (out) *out = dom;          /* out 为空 = 只问"有没有" */
    else dsj_free(dom);
    free(path);
    return 1;
}

int ds_res_recoverable(DsModel *m) { return recoverable(m, NULL); }

/* 提示恢复的原因。场景文件比自动保存新时压根不提示(见 recoverable)。 */
const char *ds_res_recover_kind(DsModel *m)
{
    Dsj *b = NULL;
    const char *kind = "";
    if (!recoverable(m, &b)) return "";
    kind = dsj_get_bool(b, "clean", 0) ? "unsaved" : "crash";
    dsj_free(b);
    return kind;
}

/* 正常退出:把**本次运行写的**那份自动保存标成 clean=1(不是我们的就不动) */
void ds_res_autosave_mark_clean(DsModel *m)
{
    char *path = autosave_path(m);
    char *txt;
    Dsj *dom;
    char perr[128];
    if (!path) return;
    txt = ds_file_read_text(path, NULL);
    if (!txt) { free(path); return; }
    dom = dsj_parse(txt, perr, sizeof perr);
    free(txt);
    if (dom && dom->t == DSJ_OBJ) {
        const char *sess = dsj_get_str(dom, "session", "");
        const char *mine = ds_model_session(m);
        if (sess && *sess && mine && *mine && !strcmp(sess, mine)) {
            char *out;
            dsj_set_bool(dom, "clean", 1);
            out = dsj_dump(dom);
            if (out) { ds_write_text(path, out); free(out); }
        }
    }
    if (dom) dsj_free(dom);
    free(path);
}

static Dsj *cmd_recover_status(DsModel *m)
{
    Dsj *r = dsj_obj();
    Dsj *b = NULL;
    if (recoverable(m, &b)) {
        dsj_set_bool(r, "recoverable", 1);
        dsj_set_str(r, "kind", dsj_get_bool(b, "clean", 0) ? "unsaved" : "crash");
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
            dsu_remove(path);
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
        ok = file_exists(path) ? (dsu_remove(path) ? 1 : 0) : 1;
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
    if (!strcmp(cmd, "res.refs")) return cmd_res_refs(m, args);
    if (!strcmp(cmd, "autosave.tick")) return cmd_autosave_tick(m);
    if (!strcmp(cmd, "autosave.clear")) return cmd_autosave_clear(m);
    if (!strcmp(cmd, "recover.status")) return cmd_recover_status(m);
    if (!strcmp(cmd, "recover.apply")) return cmd_recover_apply(m);
    if (!strcmp(cmd, "recover.discard")) return cmd_recover_discard(m);
    seterr(m, "未知命令 '%s'", cmd);
    return NULL;
}
