/* ============================================================================
 * ds_main.c — DexStudio 宿主:Win32 窗口 + 内嵌 WebView2 + 模型层命令通道。
 *
 * 三种用法:
 *   dexstudio.exe                     开 IDE 窗口(默认 $DEXSTUDIO_WEB 或 exe 同级 web/)
 *   dexstudio.exe --project <目录>     开窗口并直接打开项目
 *   dexstudio.exe --command '<json>'   **不开窗口**,跑一条模型命令并打印响应
 *   dexstudio.exe --selftest           **不开窗口**跑一套模型自测(退出码 0/1)
 *   dexstudio.exe --webview-version    打印系统 WebView2 运行时版本
 *
 * 后三种是给测试/CI 用的:模型层不依赖窗口,所以 IDE 的核心逻辑可以被脚本验证
 * (决策 #10;tests/test_dexstudio.py 会跑它们)。
 * ==========================================================================*/
#define _CRT_SECURE_NO_WARNINGS
#include "ds_model.h"
#include "ds_embed.h"
#include "ds_utf8.h"
#include "ds_webview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

/* 虚拟主机映射改指向之后,用它让窗口消息循环把页面重新导航一次。
 * 走消息而不是在命令处理里直接调:命令的响应要先回到 JS。 */
#define WM_APP_RELOAD (WM_APP + 1)

/* ------------------------------------------------------------ 工具 */

static char g_exe_dir[MAX_PATH * 2];

static void find_exe_dir(void)
{
    /* 用 W 版取自身路径:exe 可能就放在中文目录里(GetModuleFileNameA 会乱码),
     * 而整个"找前端目录/找引擎 DLL"都以它为基准。 */
    wchar_t wbuf[MAX_PATH * 2];
    char *u;
    char *p;
    DWORD n = GetModuleFileNameW(NULL, wbuf, (DWORD)(sizeof wbuf / sizeof wbuf[0]));
    if (!n) { g_exe_dir[0] = 0; return; }
    wbuf[sizeof wbuf / sizeof wbuf[0] - 1] = 0;
    u = dsu_u(wbuf);
    if (!u) { g_exe_dir[0] = 0; return; }
    p = strrchr(u, '\\');
    if (p) *p = 0;
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", u);
    free(u);
}

/* 前端目录:--web 指定 → exe 同级 web\ → 开发布局 ../../dexstudio/web */
/* 逐级建目录(实现在 dsu_mkdir 里,这里只留个名字说明用途) */
static void make_dirs(const char *path)
{
    dsu_mkdir(path);
}

/* 把内嵌的前端资源解包到一个缓存目录并返回它。
 * 缓存位置优先 LOCALAPPDATA(受限环境回退 TEMP,最后回退 exe 同目录)。 */
static int extract_embedded_web(char *out, size_t outsz)
{
    char dir[MAX_PATH * 3];
    char *env = dsu_env("LOCALAPPDATA");
    const char *base = (env && *env) ? env : NULL;
    char *env2 = NULL;
    int i, n = ds_embed_count(), need = 0;
    if (!base) {
        env2 = dsu_env("TEMP");
        base = (env2 && *env2) ? env2 : g_exe_dir;
    }
    snprintf(dir, sizeof dir, "%s\\DexStudio\\web-%s", base, ds_embed_hash());
    free(env);
    free(env2);
    /* 齐全就不用重写(每个文件都在) */
    for (i = 0; i < n; i++) {
        char p[MAX_PATH * 3];
        snprintf(p, sizeof p, "%s\\%s", dir, ds_embed_at(i)->name);
        if (!dsu_exists(p)) { need = 1; break; }
    }
    if (need) {
        make_dirs(dir);
        for (i = 0; i < n; i++) {
            const DsAsset *a = ds_embed_at(i);
            char p[MAX_PATH * 3];
            FILE *f;
            snprintf(p, sizeof p, "%s\\%s", dir, a->name);
            f = dsu_fopen(p, "wb");
            if (!f) return 0;
            fwrite(a->data, 1, a->size, f);
            fclose(f);
        }
    }
    snprintf(out, outsz, "%s", dir);
    return 1;
}

/* 清理**旧版本**留下的解包目录(web-<别的哈希>)。不清理的话每换一次前端资源就
 * 在 %LOCALAPPDATA% 里多留一份拷贝(曾经累积了 5 份,越用越乱)。 */
typedef struct { const char *keep; int removed; } CleanOld;
static void clean_old_cb(const char *name, long long size, unsigned long attrs, void *ud)
{
    CleanOld *c = (CleanOld *)ud;
    char *env, path[MAX_PATH * 3];
    (void)size;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return;
    if (strncmp(name, "web-", 4) != 0) return;
    if (c->keep && !strcmp(name, c->keep)) return;
    env = dsu_env("LOCALAPPDATA");
    if (!env || !*env) { free(env); return; }
    snprintf(path, sizeof path, "%s\\DexStudio\\%s", env, name);
    dsu_rmdir(path);
    c->removed++;
    free(env);
}

static void clean_old_web_dirs(const char *keep)
{
    char *env = dsu_env("LOCALAPPDATA");
    char pat[MAX_PATH * 3];
    CleanOld c;
    if (!env || !*env) { free(env); return; }
    snprintf(pat, sizeof pat, "%s\\DexStudio\\*", env);
    c.keep = keep;
    c.removed = 0;
    dsu_list(pat, clean_old_cb, &c);
    free(env);
}

static int find_web_dir(const char *override, char *out, size_t outsz)
{
    char cand[MAX_PATH * 3];
    if (override && *override) {
        snprintf(out, outsz, "%s", override);
        return dsu_exists(out);
    }
    snprintf(cand, sizeof cand, "%s\\web", g_exe_dir);
    if (dsu_exists(cand)) {
        snprintf(out, outsz, "%s", cand);
        return 1;
    }
    snprintf(cand, sizeof cand, "%s\\..\\..\\dexstudio\\web", g_exe_dir);
    if (dsu_exists(cand)) {
        snprintf(out, outsz, "%s", cand);
        return 1;
    }
    /* 发布形态:exe 旁边没有 web/ —— 用**内嵌**的那份,解包到缓存目录再用。
     * 缓存目录带内容哈希,所以换了前端资源会自动换目录(不会读到旧文件)。 */
    {
        int ok = extract_embedded_web(out, outsz);
        if (ok) {
            const char *slash = strrchr(out, '\\');
            clean_old_web_dirs(slash ? slash + 1 : out);
        }
        return ok;
    }
}

/* 把字符串安全地塞进 JSON(路径里有反斜杠时,不转义就会得到
 * "unknown escape at byte N" —— 自测第一版就踩了)。 */
static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0, i;
    for (i = 0; in && in[i] && o + 8 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\\' || c == '"') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ 无界面命令 */

static int cmd_command(const char *json, const char *project)
{
    DsModel *m = ds_model_create(g_exe_dir);
    const char *resp;
    if (project && *project) {
        char req[4096], esc[MAX_PATH * 2];
        json_escape(project, esc, sizeof esc);
        snprintf(req, sizeof req,
                 "{\"cmd\":\"project.open\",\"args\":{\"dir\":\"%s\"}}", esc);
        ds_command(m, req);
    }
    resp = ds_command(m, json);
    printf("%s\n", resp ? resp : "{\"ok\":false,\"error\":\"no response\"}");
    ds_model_destroy(m);
    return 0;
}

static int cmd_webview_version(void)
{
    char err[512];
    const char *v = ds_wv_runtime_version(err, sizeof err);
    if (!v) {
        printf("WebView2 运行时不可用:%s\n", err);
        return 1;
    }
    printf("%s\n", v);
    return 0;
}

/* 自测:模型层的端到端(不需要窗口) */
static int g_pass, g_fail;

static int json_int_field(const char *json, const char *key, int dflt);

static void check(const char *name, int cond, const char *detail)
{
    if (cond) { g_pass++; printf("  PASS  %s\n", name); }
    else { g_fail++; printf("  FAIL  %s  %s\n", name, detail ? detail : ""); }
}

static int cmd_selftest(void)
{
    DsModel *m = ds_model_create(g_exe_dir);
    char tmp[MAX_PATH];
    char esc[MAX_PATH * 2];
    char req[4096];
    const char *r;

    printf("DexStudio 自测(无窗口)\n");
    /* 临时项目放缓存目录:%LOCALAPPDATA%\\DexStudio\\selftest(B7 的发布形态
     * 下 exe 旁边没有可写的 ..\\..\\_zigtmp)。 */
    snprintf(tmp, sizeof tmp, "%s\\selftest", ds_temp_dir());
    dsu_rmdir(tmp);

    printf("[引擎与模型]\n");
    check("引擎加载", ds_model_engine_ok(m), ds_model_engine_error(m));
    r = ds_command(m, "{\"cmd\":\"app.info\"}");
    check("app.info 返回 ok", r && strstr(r, "\"ok\":true"), r);

    printf("[新建项目]\n");
    snprintf(tmp, sizeof tmp, "%s\\selftest", ds_temp_dir());
    json_escape(tmp, esc, sizeof esc);
    snprintf(req, sizeof req,
             "{\"cmd\":\"project.new\",\"args\":{\"dir\":\"%s\",\"name\":\"selftest\"}}",
             esc);
    r = ds_command(m, req);
    check("project.new", r && strstr(r, "\"ok\":true"), r);

    printf("[场景编辑]\n");
    r = ds_command(m, "{\"cmd\":\"entity.add\",\"args\":{\"name\":\"player\"}}");
    check("entity.add", r && strstr(r, "\"ok\":true"), r);
    r = ds_command(m, "{\"cmd\":\"entity.list\"}");
    check("entity.list 有 1 个实体", r && strstr(r, "\"player\""), r);
    r = ds_command(m, "{\"cmd\":\"comp.schema\"}");
    check("comp.schema 列出组件", r && strstr(r, "\"transform\""), r);
    r = ds_command(m, "{\"cmd\":\"undo\"}");
    check("undo", r && strstr(r, "\"ok\":true"), r);
    r = ds_command(m, "{\"cmd\":\"entity.list\"}");
    check("undo 后实体消失", r && !strstr(r, "\"player\""), r);
    r = ds_command(m, "{\"cmd\":\"redo\"}");
    check("redo", r && strstr(r, "\"ok\":true"), r);
    r = ds_command(m, "{\"cmd\":\"entity.list\"}");
    check("redo 后实体回来", r && strstr(r, "\"player\""), r);

    printf("[存盘]\n");
    r = ds_command(m, "{\"cmd\":\"project.save\"}");
    check("project.save", r && strstr(r, "\"ok\":true"), r);

    printf("[中文与编码]\n");
    /* 中文要穿过:JSON 解析 → 内存 → CreateFileW → 再回到 JSON。任何一环按 ANSI
     * 解一次,这里就会拿到乱码("我的场景" → "æ\x88\x91ç\x9a\x84…"),而且磁盘上
     * 会多出一个乱码目录/文件。发布形态下这几项最容易悄悄坏,所以放进自测。 */
    r = ds_command(m, "{\"cmd\":\"scene.new\",\"args\":{\"name\":\"我的场景\"}}");
    check("中文场景名能建出来", r && strstr(r, "我的场景"), r);
    check("中文场景名没有被二次编码", r && !strstr(r, "\xC3\xA6\xC2\x88"), r);
    {
        char p[MAX_PATH * 3];
        snprintf(p, sizeof p, "%s\\scenes\\我的场景.json", tmp);
        check("中文场景文件真的落盘了", dsu_exists(p), p);
    }
    {
        char src[MAX_PATH * 3], dst[MAX_PATH * 3], src_esc[1100], req2[1400];
        snprintf(src, sizeof src, "%s\\来源文件.png", ds_temp_dir());
        ds_write_text(src, "not really a png");
        snprintf(dst, sizeof dst, "%s\\res\\我的图片.png", tmp);
        dsu_remove(dst);          /* 自测目录会跨次复用,先清掉上次的 */
        json_escape(src, src_esc, sizeof src_esc);
        snprintf(req2, sizeof req2,
                 "{\"cmd\":\"res.import\",\"args\":{\"src\":\"%s\","
                 "\"name\":\"我的图片.png\"}}", src_esc);
        r = ds_command(m, req2);
        check("中文资源名能导入", r && strstr(r, "\"ok\":true"), r);
        check("中文资源名真的落盘了", dsu_exists(dst), dst);
        r = ds_command(m, "{\"cmd\":\"res.list\"}");
        check("res.list 里是 UTF-8 的中文名", r && strstr(r, "我的图片.png"), r);
    }
    /* 恢复提示:同一次运行自己写的自动保存**不该**提示恢复(B7 修的坑) */
    ds_command(m, "{\"cmd\":\"entity.add\",\"args\":{\"name\":\"autosave_probe\"}}");
    r = ds_command(m, "{\"cmd\":\"autosave.tick\"}");
    check("autosave.tick 写出了自动保存", r && strstr(r, "\"saved\":true"), r);
    r = ds_command(m, "{\"cmd\":\"recover.status\"}");
    check("本次运行自己写的自动保存不提示恢复",
          r && strstr(r, "\"recoverable\":false"), r);
    ds_command(m, "{\"cmd\":\"autosave.clear\"}");

    /* ---- 本次修复的回归点:下拉候选 / 父子 / 场景管理 / 存盘 / 生成前校验 ----
     * 这些以前全是"静默做错"的地方(死按钮、编译不存盘、空属性生成代码),
     * 所以每一步都要在无窗口自测里钉住。 */
    printf("[下拉候选与场景管理]\n");
    r = ds_command(m, "{\"cmd\":\"project.pick\",\"args\":{\"dry\":1}}");
    check("project.pick(dry) 不弹对话框",
          r && strstr(r, "\"picked\":false") && strstr(r, "\"dry\":true"), r);
    r = ds_command(m, "{\"cmd\":\"project.recent\"}");
    check("最近项目列表里有刚建的项目", r && strstr(r, "selftest"), r);
    r = ds_command(m, "{\"cmd\":\"scene.options\"}");
    check("场景选项给出实体与组件字段(下拉的候选)",
          r && strstr(r, "\"entities\"") && strstr(r, "\"schema\"")
            && strstr(r, "\"transform\""), r);
    r = ds_command(m, "{\"cmd\":\"graph.options\"}");
    check("逻辑图选项给出按键表/动作表/运算符表",
          r && strstr(r, "\"keys\"") && strstr(r, "jump") && strstr(r, "\"ops\""), r);
    check("运算符表里没有 DexLang 不认的 ^", r && !strstr(r, "\"^\""), r);
    r = ds_command(m, "{\"cmd\":\"comp.schema\"}");
    check("字段带人类语义(label/kind)",
          r && strstr(r, "\"label\"") && strstr(r, "\"kind\""), r);
    check("枚举字段带值域(collider.kind)",
          r && strstr(r, "矩形(AABB)") && strstr(r, "胶囊"), r);
    check("颜色字段被标成 color(sprite.tint)", r && strstr(r, "\"color\""), r);
    check("引擎没实现的字段被标成只读(transform.rot)",
          r && strstr(r, "引擎目前不读这个字段"), r);
    r = ds_command(m, "{\"cmd\":\"scene.new\",\"args\":{\"name\":\"../坏\"}}");
    check("场景名不允许路径穿越", r && strstr(r, "\"ok\":false"), r);
    r = ds_command(m, "{\"cmd\":\"scene.new\",\"args\":{\"name\":\"第二关\"}}");
    check("新建场景", r && strstr(r, "\"ok\":true"), r);
    r = ds_command(m, "{\"cmd\":\"scene.rename\",\"args\":{\"name\":\"第二关\",\"to\":\"第三关\"}}");
    check("场景改名", r && strstr(r, "第三关"), r);
    r = ds_command(m, "{\"cmd\":\"scene.set_start\",\"args\":{\"name\":\"第三关\"}}");
    check("设为起始场景", r && strstr(r, "第三关"), r);
    r = ds_command(m, "{\"cmd\":\"scene.delete\",\"args\":{\"name\":\"第三关\"}}");
    check("删除场景", r && strstr(r, "\"deleted\":true"), r);

    printf("[父子关系]\n");
    {
        int ida = 0, idb = 0;
        r = ds_command(m, "{\"cmd\":\"entity.add\",\"args\":{\"name\":\"父母\"}}");
        ida = json_int_field(r, "id", 0);
        r = ds_command(m, "{\"cmd\":\"entity.add\",\"args\":{\"name\":\"孩子\"}}");
        idb = json_int_field(r, "id", 0);
        snprintf(req, sizeof req,
                 "{\"cmd\":\"entity.set_parent\",\"args\":{\"id\":%d,\"parent\":%d}}",
                 idb, ida);
        r = ds_command(m, req);
        check("设父级", r && strstr(r, "\"ok\":true"), r);
        snprintf(req, sizeof req,
                 "{\"cmd\":\"entity.set_parent\",\"args\":{\"id\":%d,\"parent\":%d}}",
                 idb, idb);
        r = ds_command(m, req);
        check("自己当自己的父级被拒绝", r && strstr(r, "\"ok\":false"), r);
        snprintf(req, sizeof req,
                 "{\"cmd\":\"entity.set_parent\",\"args\":{\"id\":%d,\"parent\":%d}}",
                 ida, idb);
        r = ds_command(m, req);
        check("成环被拒绝", r && strstr(r, "\"ok\":false") && strstr(r, "环"), r);
        snprintf(req, sizeof req,
                 "{\"cmd\":\"comp.set\",\"args\":{\"id\":%d,\"comp\":\"transform\","
                 "\"field\":\"x\",\"value\":123}}", ida);
        ds_command(m, req);
        snprintf(req, sizeof req, "{\"cmd\":\"entity.get\",\"args\":{\"id\":%d}}", idb);
        r = ds_command(m, req);
        check("子实体的世界坐标含父级位移", r && strstr(r, "\"x\":123"), r);
    }

    printf("[一键编译:存盘 + 起始场景 + 生成前校验]\n");
    /* 编译要 dexc.exe;发布形态(干净目录)里可能找不到 —— 那就跳过这一段,
     * 而不是把"发布目录里 --selftest 全过"这条验收搞坏。 */
    {
        char cand[MAX_PATH * 3];
        int have_dexc = 0;
        snprintf(cand, sizeof cand, "%s\\tools\\dexc\\dexc.exe", g_exe_dir);
        if (dsu_exists(cand)) have_dexc = 1;
        if (!have_dexc) {
            snprintf(cand, sizeof cand, "%s\\..\\..\\tools\\dexc\\dexc.exe", g_exe_dir);
            if (dsu_exists(cand)) have_dexc = 1;
        }
        if (!have_dexc && dsu_exists("tools\\dexc\\dexc.exe")) have_dexc = 1;
        if (!have_dexc) {
            printf("  SKIP  编译相关检查(这个目录里找不到 tools/dexc/dexc.exe)\n");
            goto after_compile_checks;
        }
    }
    ds_command(m, "{\"cmd\":\"scene.load\",\"args\":{\"path\":\"scenes/main.json\"}}");
    ds_command(m, "{\"cmd\":\"entity.add\",\"args\":{\"name\":\"编译前存盘探针\"}}");
    r = ds_command(m, "{\"cmd\":\"build.compile\"}");
    check("build.compile 成功", r && strstr(r, "\"ok\":true"), r);
    {
        char p[MAX_PATH * 3];
        char *txt;
        snprintf(p, sizeof p, "%s\\scenes\\main.json", tmp);
        txt = ds_file_read_text(p, NULL);
        check("编译前先把场景存盘了(游戏跑的就是你看到的那份)",
              txt && strstr(txt, "存盘探针"), p);
        free(txt);
        snprintf(p, sizeof p, "%s\\scripts\\project_info.dex", tmp);
        txt = ds_file_read_text(p, NULL);
        check("起始场景生成成了 DexLang 模块(scripts/project_info.dex)",
              txt && strstr(txt, "dexstudio_start_scene"), p);
        free(txt);
    }
after_compile_checks:
    ds_command(m, "{\"cmd\":\"graph.new\"}");
    ds_command(m, "{\"cmd\":\"graph.node.add\",\"args\":{\"type\":\"set_field\"}}");
    r = ds_command(m, "{\"cmd\":\"graph.validate\"}");
    check("新加的「写字段」节点默认属性就是合法的",
          r && strstr(r, "\"count\":0"), r);
    ds_command(m, "{\"cmd\":\"graph.node.add\",\"args\":{\"type\":\"play_sound\"}}");
    r = ds_command(m, "{\"cmd\":\"graph.validate\"}");
    check("没选声音的节点会被校验指出来",
          r && strstr(r, "\"count\":1") && strstr(r, "声音"), r);
    r = ds_command(m, "{\"cmd\":\"graph.generate\"}");
    check("有问题的图**不再**生成代码(并说清是哪个节点)",
          r && strstr(r, "\"ok\":false") && strstr(r, "问题"), r);
    ds_command(m, "{\"cmd\":\"graph.new\"}");
    r = ds_command(m, "{\"cmd\":\"graph.generate\"}");
    check("空图仍然能生成(三个回调都在)", r && strstr(r, "logic_update"), r);
    ds_command(m, "{\"cmd\":\"graph.node.add\",\"args\":{\"type\":\"on_action\"}}");
    r = ds_command(m, "{\"cmd\":\"graph.generate\"}");
    check("用到动作节点时会自动绑默认键位(以前按空格永远没反应)",
          r && strstr(r, "eng_bind_default_actions"), r);
    ds_command(m, "{\"cmd\":\"graph.new\"}");
    ds_command(m, "{\"cmd\":\"graph.node.add\",\"args\":{\"type\":\"on_update\"}}");
    r = ds_command(m, "{\"cmd\":\"project.save\"}");
    check("project.save 成功", r && strstr(r, "\"ok\":true"), r);
    {
        char p[MAX_PATH * 3];
        char *txt;
        snprintf(p, sizeof p, "%s\\scripts\\logic.json", tmp);
        txt = ds_file_read_text(p, NULL);
        check("「保存」也把逻辑图写盘了(以前只有「保存图」才写)",
              txt && strstr(txt, "on_update"), p);
        free(txt);
    }
    r = ds_command(m, "{\"cmd\":\"file.open_external\",\"args\":{\"path\":\"scripts/main.dex\",\"dry\":1}}");
    check("file.open_external(dry) 只回路径不开进程",
          r && strstr(r, "\"opened\":false") && strstr(r, "main.dex"), r);

    printf("[错误路径]\n");
    r = ds_command(m, "{\"cmd\":\"entity.get\",\"args\":{\"id\":999999}}");
    check("无效实体带原因", r && strstr(r, "\"ok\":false") && strstr(r, "不存在"), r);
    r = ds_command(m, "{\"cmd\":\"nosuch\"}");
    check("未知命令带原因", r && strstr(r, "未知命令"), r);
    r = ds_command(m, "{not json}");
    check("坏 JSON 带原因", r && strstr(r, "\"ok\":false"), r);

    ds_model_destroy(m);
    printf("自测结果:%d 通过,%d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ------------------------------------------------------------ 窗口宿主 */

typedef struct {
    DsModel *model;
    DsWebView *wv;
    char web_dir[MAX_PATH * 3];
    HWND hwnd;
    /* 已经登记给 WebView2 的映射目标。用来判断"打开了另一个项目"——
     * 目标一变就必须重新导航一次,否则新映射对当前页面无效
     * (见 sync_host_mappings)。 */
    char map_proj[MAX_PATH * 3];
    char map_prev[MAX_PATH * 3];
    int page_loaded;         /* 页面已经加载过(只有这时才需要为改映射重新导航) */
} Host;

/* --wv-selftest:前端发来 ui.ready 就算整条链通了 */
static int g_ui_ready;
static char g_ui_ready_info[512];
static char g_ui_msg_log[1024];
/* --open-late <目录>:等页面**加载完**再打开项目(用户双击 exe 之后的真实顺序,
 * 也就是虚拟主机映射落在导航之后的那种情况)。配合 --wv-selftest 就是一条
 * "启动时没项目、后来才打开"的回归测试。 */
static const char *g_open_late = "";
/* 界面自测(页面里的 window.__ds_selftest)回传的原始 JSON。
 * 页面那一层(渲染图/层级树/属性面板/瓦片刷子)只有它自己能验,所以让页面
 * 把结果发回来,宿主只做断言 —— 不需要人看屏幕。 */
static char g_ui_self[16384];
static int g_ui_self_on = 0;
static int g_wv_fail = 0;

static int json_int_field(const char *json, const char *key, int dflt)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(json, pat);
    return p ? atoi(p + strlen(pat)) : dflt;
}

static int ui_self_fails(void) { return json_int_field(g_ui_self, "fails", -1); }

/* 虚拟主机映射的跟踪日志(DEXSTUDIO_WV_TRACE=1 打开)—— 排查"启动后才打开
 * 项目"这类时序问题时要能看到映射到底设了没、成没成。 */
static int wv_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DEXSTUDIO_WV_TRACE");
        on = (e && *e && *e != '0');
    }
    return on;
}

/* 两个虚拟主机必须在**导航之前**就登记好,而且**改指向之后要重新导航**。
 * 这两句话合起来才是"打开项目后缩略图能用"的完整原因(实测):
 *
 *   WebView2 的 SetVirtualHostNameToFolderMapping 对一个**已经加载完的页面**
 *   改映射会返回 S_OK(所以日志上看着"映射成功了"),但那个页面发出的请求
 *   依旧失败 —— `img` 是裂图标(用户看到的"图片读取失败")、`fetch` 抛
 *   TypeError: Failed to fetch。只有**新的导航**才认新映射。
 *
 * 用户报的现象正是这条路径:双击 exe(启动时没项目)→ 在界面里打开项目 →
 * 映射是**页面加载之后**才设的 → 缩略图全裂、给实体设了贴图编辑器里也没反应,
 * 而「运行」走的是引擎自己读文件,所以一切正常。
 *
 * 所以:没有项目时先拿预览目录当占位把两个主机名占住;打开/新建项目时改指向,
 * 然后由 sync_host_mappings 安排一次重新导航。 */
static void map_hosts_before_nav(Host *host)
{
    const char *proj = ds_project_dir(host->model);
    const char *prev = ds_preview_dir(host->model);
    if (prev && *prev) ds_mkdir(prev);
    if (proj && *proj) ds_mkdir(proj);
    if (prev && *prev) {
        int ok = ds_wv_map_folder(host->wv, prev, "dexstudio-preview.local");
        if (wv_trace_on())
            fprintf(stderr, "[wv] 导航前映射 dexstudio-preview.local → %s : %s%s\n",
                    prev, ok ? "ok" : "失败", ok ? "" : ds_wv_error(host->wv));
        snprintf(host->map_prev, sizeof host->map_prev, "%s", prev);
    }
    /* 没有项目时用预览目录当占位:主机名先占住,资源请求会得到 404 而不是
     * 网络错误;打开项目后改指向 + 重新导航。 */
    {
        const char *dir = (proj && *proj) ? proj : prev;
        if (dir && *dir) {
            int ok = ds_wv_map_folder(host->wv, dir, "dexstudio-proj.local");
            if (wv_trace_on())
                fprintf(stderr, "[wv] 导航前映射 dexstudio-proj.local → %s : %s%s%s\n",
                        dir, ok ? "ok" : "失败", ok ? "" : ds_wv_error(host->wv),
                        (proj && *proj) ? "" : "(占位:还没打开项目)");
            snprintf(host->map_proj, sizeof host->map_proj, "%s", dir);
        }
    }
}

/* 打开/新建/切换项目之后:把两个映射指向新目录;真的变了就**重新导航**一次。
 * 只在目标变化时才动 —— 以前每条命令都重设一遍映射,既浪费又会掩盖"改了到底
 * 生效没有"这件事。 */
static void sync_host_mappings(Host *host)
{
    const char *proj = ds_project_dir(host->model);
    const char *prev = ds_preview_dir(host->model);
    int changed = 0, ok;
    if (!host->wv) return;
    if (proj && *proj && strcmp(proj, host->map_proj)) {
        ok = ds_wv_map_folder(host->wv, proj, "dexstudio-proj.local");
        snprintf(host->map_proj, sizeof host->map_proj, "%s", proj);
        changed = 1;
        if (wv_trace_on())
            fprintf(stderr, "[wv] 改指向 dexstudio-proj.local → %s : %s%s\n",
                    proj, ok ? "ok" : "失败", ok ? "" : ds_wv_error(host->wv));
    }
    if (prev && *prev && strcmp(prev, host->map_prev)) {
        ds_mkdir(prev);
        ok = ds_wv_map_folder(host->wv, prev, "dexstudio-preview.local");
        snprintf(host->map_prev, sizeof host->map_prev, "%s", prev);
        changed = 1;
        if (wv_trace_on())
            fprintf(stderr, "[wv] 改指向 dexstudio-preview.local → %s : %s%s\n",
                    prev, ok ? "ok" : "失败", ok ? "" : ds_wv_error(host->wv));
    }
    if (!changed) return;
    /* 页面加载过才需要重导航;没加载的话 apply_navigation 会带上新映射。
     * 用 PostMessage 而不是直接调:这条命令的**响应要先回到 JS**(前端在等它),
     * 再让页面重载。 */
    if (host->page_loaded && host->hwnd) PostMessageA(host->hwnd, WM_APP_RELOAD, 0, 0);
}

static int ui_self_passes(void) { return json_int_field(g_ui_self, "pass_count", -1); }
static const char *ui_self_msg(void) { return g_ui_self; }
static int ui_self_done(void) { return g_ui_self_on; }

static const char *on_js_message(void *user, const char *json)
{
    Host *host = (Host *)user;
    const char *resp;
    if (json && *json) {
        char *p = g_ui_msg_log + strlen(g_ui_msg_log);
        size_t left = sizeof g_ui_msg_log - (size_t)(p - g_ui_msg_log);
        if (left > 8) snprintf(p, left, "%s%s", g_ui_msg_log[0] ? " | " : "", json);
    }
    if (json && strstr(json, "\"ui.ready\"")) {
        g_ui_ready = 1;
        host->page_loaded = 1;    /* 页面活了:之后改映射就得重新导航 */
        snprintf(g_ui_ready_info, sizeof g_ui_ready_info, "%s", json);
    }
    if (json && strstr(json, "\"ui.selftest\"")) {
        snprintf(g_ui_self, sizeof g_ui_self, "%s", json);
        g_ui_self_on = 1;
        return "{\"ok\":true,\"result\":{\"ack\":\"ui.selftest\"}}";
    }
    resp = ds_command(host->model, json);
    /* 打开/新建项目会换掉项目根与预览目录 —— 映射改指向之后**必须重新导航**,
     * 否则当前页面永远读不到资源(用户报的"缩略图全裂"就是这么来的)。 */
    sync_host_mappings(host);
    return resp;
}

/* 关窗口时如果还有没保存的改动,先问一句 —— 以前直接 DestroyWindow,
 * 而自动保存是 30 秒一次,于是"改完就关"必定丢东西。 */
static int confirm_close(Host *h, HWND hwnd)
{
    int rc;
    if (!h || !h->model || !ds_model_dirty(h->model)) return 1;
    /* 先自动保存一次:即使选"直接退出",也留下可恢复的那一份 */
    ds_command(h->model, "{\"cmd\":\"autosave.tick\"}");
    rc = MessageBoxW(hwnd,
        L"场景/逻辑图还有未保存的改动。\n\n"
        L"「是」= 保存并退出\n「否」= 直接退出(已自动保存一份,下次可恢复)\n"
        L"「取消」= 回去继续编辑",
        L"DexStudio", MB_YESNOCANCEL | MB_ICONWARNING);
    if (rc == IDCANCEL) return 0;
    if (rc == IDYES) {
        if (!ds_model_project_save(h->model)) {
            wchar_t *w = dsu_w(ds_model_errbuf(h->model));
            MessageBoxW(hwnd, w ? w : L"保存失败", L"DexStudio 保存失败",
                        MB_OK | MB_ICONERROR);
            free(w);
            return 0;
        }
    }
    return 1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Host *h = (Host *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        Host *host = (Host *)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)host);
        return 0;
    }
    case WM_SIZE:
        if (h && h->wv) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            ds_wv_set_bounds(h->wv, 0, 0, rc.right, rc.bottom);
        }
        return 0;
    case WM_SETFOCUS:
        if (h && h->wv) SetFocus(hwnd);
        return 0;
    case WM_APP_RELOAD:
        /* 映射改指向了:重新导航让新映射对页面生效(见 sync_host_mappings)。 */
        if (h && h->wv) ds_wv_reload(h->wv);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        if (!confirm_close(h, hwnd)) return 0;
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int run_window(const char *project, const char *web_override, int wv_selftest)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    Host *host = calloc(1, sizeof *host);
    RECT rc = {0, 0, 1440, 900};
    HANDLE once = NULL;

    /* 单实例:WebView2 的用户数据目录是固定的,第二个实例会失败在一句
     * "创建 WebView2 控制器失败(hr=0x800700aa)" 上 —— 用户完全看不懂。
     * 这里直接检测:**已经有一个在跑就把它的窗口激活**,自己退出。 */
    if (!wv_selftest) {
        once = CreateMutexW(NULL, FALSE, L"Local\\DexStudio_SingleInstance");
        if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND prev = FindWindowA("DexStudioWindow", NULL);
            if (prev) {
                ShowWindow(prev, SW_RESTORE);
                SetForegroundWindow(prev);
                if (once) CloseHandle(once);
                free(host);
                return 0;
            }
            /* 找不到窗口(上次崩了留下的?):继续启动,不拦着用户 */
        }
    }

    /* DPI 感知:必须在创建窗口之前;失败(如清单已设)不算错 */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!find_web_dir(web_override, host->web_dir, sizeof host->web_dir)) {
        fprintf(stderr, "dexstudio: 找不到前端目录(web/index.html);用 --web 指定\n");
        free(host);
        return 1;
    }
    if (!ds_wv_load(g_exe_dir)) {
        fprintf(stderr, "dexstudio: %s\n", ds_wv_error(NULL));
        free(host);
        return 1;
    }
    host->model = ds_model_create(g_exe_dir);
    if (project && *project) {
        char req[4096], esc[MAX_PATH * 2];
        json_escape(project, esc, sizeof esc);
        snprintf(req, sizeof req,
                 "{\"cmd\":\"project.open\",\"args\":{\"dir\":\"%s\"}}", esc);
        ds_command(host->model, req);
    }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "DexStudioWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassA(&wc)) {
        fprintf(stderr, "dexstudio: RegisterClass 失败\n");
        return 1;
    }
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "DexStudioWindow", "DexStudio",
                           WS_OVERLAPPEDWINDOW,
                           wv_selftest ? -4000 : CW_USEDEFAULT,
                           wv_selftest ? -4000 : CW_USEDEFAULT,
                           rc.right - rc.left, rc.top ? rc.bottom - rc.top : 900,
                           NULL, NULL, GetModuleHandleA(NULL), host);
    if (!hwnd) {
        fprintf(stderr, "dexstudio: 创建窗口失败\n");
        free(host);
        return 1;
    }
    /* 标题单独用 W 版设:CreateWindowExA 会把 UTF-8 标题按 ANSI(936)解,
     * 中文就成了乱码 —— 窗口标题恰好是用户第一眼看到的东西。 */
    {
        wchar_t *wt = dsu_w("DexStudio — DexLang 可视化 IDE");
        if (wt) { SetWindowTextW(hwnd, wt); free(wt); }
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    host->hwnd = hwnd;

    host->wv = ds_wv_create(hwnd, on_js_message, host);
    if (!host->wv) {
        fprintf(stderr, "dexstudio: 创建 WebView2 失败:%s\n", ds_wv_error(NULL));
        /* 仍然开窗(用户能看到窗口),但明确报错 */
    } else {
        /* 虚拟主机的映射必须在**导航之前**登记好,见 map_hosts_before_nav 的说明。 */
        map_hosts_before_nav(host);
        ds_wv_navigate_folder(host->wv, host->web_dir, "dexstudio.local", "index.html");
        ds_wv_set_bounds(host->wv, 0, 0, rc.right - rc.left, rc.bottom - rc.top);
    }

    if (!wv_selftest) {
        while (GetMessageA(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    } else {
        /* 无人值守:等前端发 ui.ready(窗口在屏幕外,不会打扰人) */
        DWORD t0 = GetTickCount();
        int done = 0, probed = 0;
        static const char *PROBE =
            "JSON.stringify({bridge: !!(window.chrome && (window.chrome.webview || window.chrome.webView)),"
            " title: document.title, href: location.href,"
            " ready: document.readyState,"
            " scripts: document.scripts ? document.scripts.length : -1,"
            " hasDs: typeof ds === 'function',"
            " chromeKeys: Object.keys(window.chrome || {})})";
        printf("DexStudio WebView2 自测(窗口在屏幕外)\n");
        while (!done) {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { done = 1; break; }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (g_ui_ready) break;
            /* 导航完成后主动问一遍页面状态:桥在不在、加载的是哪一个页面 */
            if (!probed && host->wv && ds_wv_ready(host->wv)
                && GetTickCount() - t0 > 1500) {
                ds_wv_eval(host->wv, PROBE);
                probed = 1;
            }
            if (GetTickCount() - t0 > 12000) break;
            Sleep(10);
        }
        if (g_ui_ready) {
            printf("  PASS  前端已就绪(窗口 + 本地页面 + JS→C→JS 往返)\n");
            printf("        请求:%s\n", g_ui_ready_info);
            /* --open-late <目录>:**先让页面加载完,再打开项目** —— 这正是用户
             * 双击 exe(不带 --project)之后从界面里打开项目的顺序,虚拟主机映射
             * 是在导航之后才设的。缩略图/视口预览出问题时就是这条路径。 */
            if (g_open_late[0]) {
                char js[4096], esc[3000];
                size_t i, o = 0;
                for (i = 0; g_open_late[i] && o + 3 < sizeof esc; i++) {
                    char c = g_open_late[i];
                    /* JS 字符串字面量:反斜杠要转义成 \\,不能顺手改成 / ——
                     * 路径分隔符保持原样,才能和"用户从对话框选的路径"一致。 */
                    if (c == '\\' || c == '\'' || c == '"') esc[o++] = '\\';
                    esc[o++] = c;
                }
                esc[o] = 0;
                snprintf(js, sizeof js, "openProjectAt('%s')", esc);
                printf("        后开项目:%s\n", g_open_late);
                if (ds_wv_eval(host->wv, js)) {
                    DWORD t2 = GetTickCount();
                    while (GetTickCount() - t2 < 2500) {
                        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                            if (msg.message == WM_QUIT) break;
                            TranslateMessage(&msg);
                            DispatchMessageA(&msg);
                        }
                        Sleep(10);
                    }
                }
                /* 映射改指向之后宿主会自动重新导航一次(sync_host_mappings →
                 * WM_APP_RELOAD),所以这里只要等页面重新加载完、映射生效。 */
                { DWORD t3 = GetTickCount();
                  while (GetTickCount() - t3 < 3500) {
                      while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                          if (msg.message == WM_QUIT) break;
                          TranslateMessage(&msg);
                          DispatchMessageA(&msg);
                      }
                      Sleep(10);
                  } }
            }
            /* 窗口标题是用户第一眼看到的东西,回读一遍证明它不是乱码 ——
             * CreateWindowExA 的坑见陷阱表;这里不用人看屏幕也能断言。 */
            {
                static const char *WANT = "DexStudio — DexLang 可视化 IDE";
                wchar_t wt[256] = {0};
                char *u;
                GetWindowTextW(hwnd, wt, 256);
                u = dsu_u(wt);
                printf("%s  窗口标题:[%s]\n",
                       (u && !strcmp(u, WANT)) ? "  PASS" : "  FAIL", u ? u : "(读不到)");
                if (!u || strcmp(u, WANT)) g_wv_fail = 1;
                free(u);
            }
            /* 让页面把自己那一层也验一遍(渲染图/层级树/属性面板/瓦片刷子),
             * 结果由页面用 ui.selftest 消息发回来 —— 不需要人看屏幕。 */
            if (ds_wv_eval(host->wv,
                           "window.__ds_selftest ? window.__ds_selftest() : 'no-selftest'")) {
                DWORD t1 = GetTickCount();
                while (!ui_self_done() && GetTickCount() - t1 < 20000) {
                    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                        if (msg.message == WM_QUIT) break;
                        TranslateMessage(&msg);
                        DispatchMessageA(&msg);
                    }
                    Sleep(10);
                }
            }
            if (ui_self_done()) {
                int fails = ui_self_fails();
                printf("%s  界面自测:%d 项通过,%d 项失败\n",
                       fails ? "  FAIL" : "  PASS", ui_self_passes(), fails);
                printf("        %s\n", ui_self_msg());
                if (fails) g_wv_fail = 1;
            } else {
                printf("  FAIL  界面自测没有回消息(前端 __ds_selftest 没跑或抛异常)\n");
                g_wv_fail = 1;
            }
        } else {
            const char *e = host->wv ? ds_wv_error(host->wv) : "未创建 WebView2";
            const char *info = ds_command(host->model, "{\"cmd\":\"app.info\"}");
            printf("  FAIL  等不到 ui.ready\n");
            if (e && *e) printf("        WebView2:%s\n", e);
            printf("        收到过的消息:%s\n",
                   g_ui_msg_log[0] ? g_ui_msg_log : "(一条也没有 → 页面里的 JS 没跑起来)");
            printf("        app.info:%s\n", info ? info : "");
            printf("        排查:dexstudio/web/index.html 是否与 exe 同级(或用 --web 指定);"
                   "DEXSTUDIO_WV_TRACE=1 看阶段日志\n");
        }
        if (host->wv) ds_wv_destroy(host->wv);
        ds_model_destroy(host->model);
        free(host);
        return (g_ui_ready && !g_wv_fail) ? 0 : 1;
    }
    if (host->wv) ds_wv_destroy(host->wv);
    ds_model_destroy(host->model);
    free(host);
    return 0;
}

/* ------------------------------------------------------------ 入口 */

static int ds_host_args(int argc, char **argv);

/* 真正的入口:先把参数换成 UTF-8 的,再交给 ds_host_args。
 * CRT 给的 argv 已经按 ANSI 解过一遍(中文系统 = 936),所以
 * `--project 我的项目` / `--command '{"名字":"测试"}'` 到这儿都是烂的。
 * 从宽命令行重新取一份;取不到就退回原来的 argv。 */
static int ds_host_main(int argc, char **argv)
{
    int wide_argc = 0;
    char **wide_argv;
    int rc;
    SetConsoleOutputCP(65001);
    wide_argv = dsu_argv(&wide_argc);
    if (wide_argv && wide_argc > 0) {
        rc = ds_host_args(wide_argc, wide_argv);
        dsu_argv_free(wide_argv, wide_argc);
        return rc;
    }
    return ds_host_args(argc, argv);
}

static int ds_host_args(int argc, char **argv)
{
    const char *project = NULL, *web = NULL, *command = NULL;
    int selftest = 0, wv_selftest = 0, i;

    find_exe_dir();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--project") && i + 1 < argc) project = argv[++i];
        else if (!strcmp(argv[i], "--open-late") && i + 1 < argc) g_open_late = argv[++i];
        else if (!strcmp(argv[i], "--web") && i + 1 < argc) web = argv[++i];
        else if (!strcmp(argv[i], "--command") && i + 1 < argc) command = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--wv-selftest")) wv_selftest = 1;
        else if (!strcmp(argv[i], "--version")) {
            printf("DexStudio %s\n", ds_version());
            return 0;
        } else if (!strcmp(argv[i], "--webview-version")) return cmd_webview_version();
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("DexStudio %s\n"
                   "  dexstudio.exe [--project DIR] [--web DIR]\n"
                   "  dexstudio.exe --open-late DIR       启动后再打开项目(测试用)\n"
                   "  dexstudio.exe --command '<json>'   跑一条模型命令(不开窗口)\n"
                   "  dexstudio.exe --selftest           模型自测(不开窗口)\n"
                   "  dexstudio.exe --webview-version\n"
                   "  dexstudio.exe --version\n", ds_version());
            return 0;
        } else {
            fprintf(stderr, "dexstudio: 未知参数 %s\n", argv[i]);
            return 2;
        }
    }
    if (command) return cmd_command(command, project);
    if (selftest) return cmd_selftest();
    return run_window(project, web, wv_selftest);
}

#ifndef DS_NO_CONSOLE
int main(int argc, char **argv) { return ds_host_main(argc, argv); }
#else
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d)
{
    (void)a; (void)b; (void)c; (void)d;
    return ds_host_main(__argc, __argv);
}
#endif
