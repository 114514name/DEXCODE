/* ============================================================================
 * ds_run.c — 一键编译 / 独立窗口运行 / 输出与错误定位(B5)。
 *
 * 分工(与产品 B 的一贯做法一致):**进程与诊断解析都在 C**,前端只显示。
 *   前端拿到的是结构化结果 {ok, code, out, diag:[{level,phase,line,col,msg}]},
 *   而不是一段要自己切分的文本 —— 于是"点错误跳到那一行"这件事在 C 侧就被钉住了,
 *   测试也就能断言"报错的行列号对不对"。
 *
 * 两个子命令:
 *   dexc.exe compile <脚本>        编译(捕获 stdout/stderr、解析诊断)
 *   vm.exe / vmnc.exe <字节码>     运行(vmnc 是无控制台版本 → 只有一个游戏窗口)
 *
 * 为什么用绝对路径找工具:project.json 里的 run.dexc / run.vm 是**相对仓库**写的
 * (`tools/dexc/dexc.exe`),而 IDE 的 cwd 可能是任何地方。所以按
 * 绝对 → exe 同目录 → 仓库根(exe_dir/../..)→ cwd 的顺序试。
 * ==========================================================================*/
#include "ds_run.h"
#include "ds_blocks.h"
#include "ds_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#endif

/* ------------------------------------------------------------ 小工具 */

static void seterr(DsModel *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ds_model_errbuf(m), ds_model_errbuf_size(), fmt, ap);
    va_end(ap);
}

static int file_exists(const char *p)
{
    FILE *f = dsu_fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* 相对路径 → 找到一个真实存在的绝对路径。返回 malloc 的字符串,失败返回 NULL。 */
static char *resolve_tool(DsModel *m, const char *rel, const char *fallback)
{
    char buf[1400];
    const char *use = (rel && *rel) ? rel : fallback;
    if (!use || !*use) return NULL;
    /* 绝对路径(盘符或 UNC)直接用 */
    if ((use[0] && use[1] == ':') || use[0] == '\\' || use[0] == '/') {
        return file_exists(use) ? ds_strdup(use) : NULL;
    }
    snprintf(buf, sizeof buf, "%s", use);
    if (file_exists(buf)) return ds_strdup(buf);
    {
        const char *exe = ds_model_exe_dir(m);
        if (exe && *exe) {
            snprintf(buf, sizeof buf, "%s\\%s", exe, use);
            if (file_exists(buf)) return ds_strdup(buf);
            snprintf(buf, sizeof buf, "%s\\..\\..\\%s", exe, use);
            if (file_exists(buf)) return ds_strdup(buf);
        }
    }
    return NULL;
}

/* 项目里的一个相对路径 → 绝对路径 */
static char *proj_path(DsModel *m, const char *rel)
{
    if (!rel || !*rel) return NULL;
    if ((rel[0] && rel[1] == ':') || rel[0] == '\\' || rel[0] == '/') return ds_strdup(rel);
    return ds_path_join(ds_project_dir(m), rel);
}

/* ------------------------------------------------------------ 跑进程 */

typedef struct {
    int code;
    int timed_out;
    char *out;
    size_t len;
} RunResult;

#if defined(_WIN32)

/* 启动一个进程。capture=1 时把 stdout/stderr 收进 r->out 并等待(timeout_ms);
 * capture=0 时**不等待**(独立窗口运行),把句柄交给调用方后面 stop。 */
static int spawn_ds(DsModel *m, const char *exe, const char *args, const char *cwd,
                    int capture, int timeout_ms, RunResult *r,
                    void **out_proc, unsigned long *out_pid, char *err, unsigned errsz)
{
    char cmd[4096];
    STARTUPINFOW si;          /* 与 CreateProcessW 配套(结构布局和 A 版相同,但别混用) */
    (void)m;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, nul = NULL;
    int ok = 0;
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args ? args : "");
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    if (capture) {
        SECURITY_ATTRIBUTES sa;
        memset(&sa, 0, sizeof sa);
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            snprintf(err, errsz, "CreatePipe 失败");
            return 0;
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          0, NULL);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.hStdInput = nul;
    }
    /* 走 W 版:命令行里有中文路径时,CreateProcessA 会按 ANSI 再解一遍,
     * 于是 dexc/vm 收到的就是乱码路径(然后报"找不到文件")。 */
    {
        wchar_t *wcmd = dsu_w(cmd);
        wchar_t *wcwd = (cwd && *cwd) ? dsu_w(cwd) : NULL;
        BOOL started = wcmd
            ? CreateProcessW(NULL, wcmd, NULL, NULL, capture ? TRUE : FALSE,
                             capture ? CREATE_NO_WINDOW : 0, NULL, wcwd, &si, &pi)
            : FALSE;
        free(wcmd);
        free(wcwd);
        if (!started) {
            snprintf(err, errsz, "启动失败(%lu):%s", (unsigned long)GetLastError(), cmd);
            if (rd) CloseHandle(rd);
            if (wr) CloseHandle(wr);
            if (nul) CloseHandle(nul);
            return 0;
        }
    }
    if (!capture) {
        if (out_proc) *out_proc = (void *)pi.hProcess;
        else CloseHandle(pi.hProcess);
        if (out_pid) *out_pid = (unsigned long)pi.dwProcessId;
        CloseHandle(pi.hThread);
        return 1;
    }
    CloseHandle(wr);          /* 我们自己不留写端,否则读不到 EOF */
    wr = NULL;
    /* 读干管道 */
    {
        char buf[4096];
        DWORD got = 0;
        size_t cap = 4096;
        if (r) {
            r->out = malloc(cap);
            r->out[0] = 0;
            r->len = 0;
        }
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
            if (avail == 0) {
                DWORD st = WaitForSingleObject(pi.hProcess, 30);
                if (st == WAIT_OBJECT_0) {
                    if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
                }
                continue;
            }
            if (!ReadFile(rd, buf, sizeof buf - 1, &got, NULL) || got == 0) break;
            buf[got] = 0;
            if (r) {
                if (r->len + got + 1 > cap) {
                    while (r->len + got + 1 > cap) cap *= 2;
                    r->out = realloc(r->out, cap);
                }
                memcpy(r->out + r->len, buf, got + 1);
                r->len += got;
            }
        }
        if (r && !r->out) { r->out = malloc(1); r->out[0] = 0; r->len = 0; }
    }
    /* 子进程的输出会原样进 JSON,而它可能混着 ANSI(GBK)字节(老工具从 argv 拿到的
     * 路径就是 GBK)—— 清成合法 UTF-8,否则前端 JSON.parse 会直接抛,面板全废。 */
    if (r && r->out) {
        char *clean = dsu_utf8_clean(r->out);
        if (clean) {
            free(r->out);
            r->out = clean;
            r->len = strlen(clean);
        }
    }
    {
        DWORD st = WaitForSingleObject(pi.hProcess, (DWORD)(timeout_ms > 0 ? timeout_ms : 20000));
        DWORD code = 0;
        if (st == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            if (r) r->timed_out = 1;
        }
        GetExitCodeProcess(pi.hProcess, &code);
        if (r) r->code = (int)code;
    }
    CloseHandle(rd);
    if (nul) CloseHandle(nul);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    ok = 1;
    return ok;
}

#else

static int spawn_ds(DsModel *m, const char *exe, const char *args, const char *cwd,
                    int capture, int timeout_ms, RunResult *r,
                    void **out_proc, unsigned long *out_pid, char *err, unsigned errsz)
{
    (void)m; (void)exe; (void)args; (void)cwd; (void)capture; (void)timeout_ms;
    (void)r; (void)out_proc; (void)out_pid;
    snprintf(err, errsz, "DexStudio 只在 Windows 上跑进程");
    return 0;
}

#endif

/* ------------------------------------------------------------ 诊断解析 */

/* dexc 的两种行:
 *   [parser]error at 6:5: expected ';', got 'eng_set_f'
 *   warning: unreachable statement at 3:5 (after 'return')
 * 其它行(正常输出、"汇编(IR): …")一律原样放进 out,由前端显示。 */
static void parse_diag(const char *text, const char *file, Dsj *arr)
{
    const char *p = text ? text : "";
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
        if (line[0] == '[') {
            const char *close = strchr(line, ']');
            const char *at = close ? strstr(close, "error at ") : NULL;
            if (close && at) {
                char phase[32];
                size_t pl = (size_t)(close - line - 1);
                int lineno = 0, col = 0;
                if (pl >= sizeof phase) pl = sizeof phase - 1;
                memcpy(phase, line + 1, pl);
                phase[pl] = 0;
                if (sscanf(at + 9, "%d:%d", &lineno, &col) == 2) {
                    const char *colon = strchr(at + 9, ':');
                    colon = colon ? strchr(colon + 1, ':') : NULL;
                    {
                        Dsj *d = dsj_obj();
                        dsj_set_str(d, "level", "error");
                        dsj_set_str(d, "phase", phase);
                        dsj_set_str(d, "file", file ? file : "");
                        dsj_set_int(d, "line", lineno);
                        dsj_set_int(d, "col", col);
                        dsj_set_str(d, "msg", colon ? colon + 2 : "");
                        dsj_push(arr, d);
                    }
                }
            }
        } else if (!strncmp(line, "warning:", 8)) {
            int lineno = 0, col = 0;
            const char *at = strstr(line, " at ");
            if (at && sscanf(at + 4, "%d:%d", &lineno, &col) == 2) {
                char msg[512];
                const char *src = line + 8;
                size_t ml;
                while (*src == ' ') src++;      /* "warning:  xxx" 的空格去掉 */
                ml = (size_t)(at - src);
                if (ml >= sizeof msg) ml = sizeof msg - 1;
                memcpy(msg, src, ml);
                msg[ml] = 0;
                {
                    Dsj *d = dsj_obj();
                    dsj_set_str(d, "level", "warning");
                    dsj_set_str(d, "phase", "compiler");
                    dsj_set_str(d, "file", file ? file : "");
                    dsj_set_int(d, "line", lineno);
                    dsj_set_int(d, "col", col);
                    dsj_set_str(d, "msg", msg);
                    dsj_push(arr, d);
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
}

/* ------------------------------------------------------------ 运行状态 */

int ds_run_active(DsModel *m)
{
    void **slot = ds_model_proc_slot(m);
#if defined(_WIN32)
    if (!slot || !*slot) return 0;
    {
        DWORD code = 0;
        if (!GetExitCodeProcess((HANDLE)*slot, &code)) return 0;
        return code == STILL_ACTIVE;
    }
#else
    (void)slot;
    return 0;
#endif
}

unsigned long ds_run_pid(DsModel *m)
{
    unsigned long *p = ds_model_pid_slot(m);
    return p ? *p : 0;
}

int ds_run_exit_code(DsModel *m)
{
    void **slot = ds_model_proc_slot(m);
#if defined(_WIN32)
    DWORD code = 0;
    if (!slot || !*slot) return -1;
    if (!GetExitCodeProcess((HANDLE)*slot, &code)) return -1;
    return (int)code;
#else
    (void)slot;
    return -1;
#endif
}

void ds_run_kill(DsModel *m)
{
    void **slot = ds_model_proc_slot(m);
    unsigned long *pid = ds_model_pid_slot(m);
#if defined(_WIN32)
    if (slot && *slot) {
        if (ds_run_active(m)) TerminateProcess((HANDLE)*slot, 0);
        CloseHandle((HANDLE)*slot);
        *slot = NULL;
    }
#endif
    if (pid) *pid = 0;
}

/* ------------------------------------------------------------ 命令 */

/* scripts/ 下的 .dex 清单(前端"代码"页签的文件列表) */
static void dex_push_cb(const char *name, long long size, unsigned long attrs,
                        void *ud)
{
    (void)size;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return;
    dsj_push((Dsj *)ud, dsj_str(name));
}

static Dsj *cmd_scripts(DsModel *m)
{
    Dsj *a = dsj_arr();
    const char *root = ds_project_dir(m);
    char *dir;
    if (!root || !*root) {
        seterr(m, "还没有打开项目");
        return NULL;
    }
    dir = ds_path_join(root, "scripts");
    {
        char *pat = ds_path_join(dir, "*.dex");
        dsu_list(pat, dex_push_cb, a);
        free(pat);
    }
    free(dir);
    return a;
}

static Dsj *cmd_file_read(DsModel *m, Dsj *args)
{
    const char *rel = dsj_get_str(args, "path", "");
    char *full;
    char *txt;
    Dsj *r;
    if (!rel || !*rel) {
        seterr(m, "file.read 需要 args.path");
        return NULL;
    }
    full = proj_path(m, rel);
    if (!full) {
        seterr(m, "路径无效");
        return NULL;
    }
    txt = ds_file_read_text(full, NULL);
    if (!txt) {
        seterr(m, "读不到文件:%s", full);
        free(full);
        return NULL;
    }
    r = dsj_obj();
    dsj_set_str(r, "path", full);
    dsj_set_str(r, "text", txt);
    free(txt);
    free(full);
    return r;
}

/* 编译:先把场景/项目/逻辑图**存盘**(一键 = 不用先点保存),再调 dexc。
 * 为什么必须存盘:游戏进程读的是磁盘上的场景,而 IDE 视口读的是内存里的 ——
 * 不存盘就会出现"IDE 里摆好了,运行起来还是旧场景"(实测踩到过)。 */
static Dsj *cmd_build_compile(DsModel *m, Dsj *args)
{
    const char *rel = dsj_get_str(args, "path", "scripts/main.dex");
    char *script = proj_path(m, rel);
    char *dexc, *out_file;
    char argbuf[1600];
    RunResult rr;
    Dsj *r, *diag;
    char err[512];
    int generated = 0, patched = 0;
    if (!script || !file_exists(script)) {
        seterr(m, "找不到脚本:%s", script ? script : rel);
        free(script);
        return NULL;
    }
    /* ① 存盘:项目 + 当前场景 + 逻辑图(project_save 三样都写) */
    if (ds_project_dir(m) && *ds_project_dir(m)) {
        if (!ds_model_project_save(m)) {
            char why[512];
            snprintf(why, sizeof why, "%s",
                     ds_model_errbuf(m)[0] ? ds_model_errbuf(m) : "未知原因");
            seterr(m, "编译前的存盘失败:%s", why);
            free(script);
            return NULL;
        }
    }
    /* ② 玩法 → scripts/logic.dex。**用哪一套生成由 project.json 的 logic_mode 决定**
     *    (新项目默认积木,老项目默认节点图)。失败就整个失败:以前静默跳过,
     *    于是用户改了玩法点编译、"成功",跑起来还是旧逻辑。 */
    {
        const char *mode = ds_model_logic_mode(m);
        int ok = !strcmp(mode, "blocks")
                 ? ds_blocks_generate_to_file(m, NULL, 0, err, sizeof err)
                 : ds_graph_generate_to_file(m, NULL, 0, err, sizeof err);
        if (!ok) {
            seterr(m, "%s", err);
            free(script);
            return NULL;
        }
        generated = 1;
    }
    /* ③ 起始场景 → scripts/project_info.dex(游戏 on_start 读它),老项目顺手改写 */
    if (ds_project_dir(m) && *ds_project_dir(m)) {
        ds_write_project_info(m);
        patched = ds_patch_main_scene(m);
    }
    dexc = resolve_tool(m, dsj_get_str(args, "dexc", ""), "tools/dexc/dexc.exe");
    if (!dexc) {
        seterr(m, "找不到 dexc.exe(project.json 的 run.dexc,或 tools/dexc/dexc.exe)");
        free(script);
        return NULL;
    }
    out_file = ds_path_replace_ext(script, ".dexbc");
    memset(&rr, 0, sizeof rr);
    /* 脚本与输出都用引号包住:项目路径里可能有空格 */
    snprintf(argbuf, sizeof argbuf, "compile \"%s\" -o \"%s\"", script, out_file);
    if (!spawn_ds(m, dexc, argbuf, ds_project_dir(m), 1,
                  (int)dsj_get_int(args, "timeout_ms", 30000), &rr, NULL, NULL,
                  err, sizeof err)) {
        seterr(m, "%s", err);
        free(script); free(dexc); free(out_file); free(rr.out);
        return NULL;
    }
    r = dsj_obj();
    dsj_set_bool(r, "ok", rr.code == 0 && !rr.timed_out);
    dsj_set_int(r, "code", rr.code);
    dsj_set_bool(r, "timeout", rr.timed_out);
    dsj_set_str(r, "script", script);
    dsj_set_str(r, "bytecode", out_file);
    dsj_set_bool(r, "bytecode_exists", file_exists(out_file));
    dsj_set_str(r, "out", rr.out ? rr.out : "");
    dsj_set_bool(r, "graph_generated", generated);
    dsj_set_int(r, "main_patched", patched);
    dsj_set_str(r, "start_scene", ds_model_start_scene(m));
    dsj_set_str(r, "scene_file", ds_model_scene_path(m));
    diag = dsj_arr();
    parse_diag(rr.out, script, diag);
    dsj_set(r, "diag", diag);
    dsj_set_int(r, "errors", 0);
    dsj_set_int(r, "warnings", 0);
    {
        int i, ne = 0, nw = 0;
        for (i = 0; i < dsj_len(diag); i++) {
            if (!strcmp(dsj_get_str(dsj_at(diag, i), "level", ""), "error")) ne++;
            else nw++;
        }
        dsj_set_int(r, "errors", ne);
        dsj_set_int(r, "warnings", nw);
    }
    free(script);
    free(dexc);
    free(out_file);
    free(rr.out);
    return r;
}

static void run_args(DsModel *m, Dsj *args, char *argbuf, size_t n,
                     char **bc_out, char **vm_out)
{
    const char *rel = dsj_get_str(args, "path", "scripts/main.dexbc");
    char *bc = proj_path(m, rel);
    char *vm;
    const char *rel_vm = dsj_get_str(args, "vm", "");
    int detach = (int)dsj_get_int(args, "detach", 0);
    if (!bc || !file_exists(bc)) {
        /* 直接给了 .dex 就换成 .dexbc */
        if (bc) {
            char *alt = ds_path_replace_ext(bc, ".dexbc");
            if (file_exists(alt)) { free(bc); bc = alt; }
            else { free(alt); }
        }
    }
    vm = resolve_tool(m, rel_vm, "vm/vm.exe");
    /* 独立窗口运行优先用**无控制台**的那个 VM:不然会多一个黑框 */
    if (vm && detach) {
        char *nc = ds_path_replace_ext(vm, "nc.exe");
        if (nc && file_exists(nc)) { free(vm); vm = nc; }
        else free(nc);
    }
    snprintf(argbuf, n, "\"%s\"", bc ? bc : "");
    *bc_out = bc;
    *vm_out = vm;
}

static Dsj *cmd_build_run(DsModel *m, Dsj *args)
{
    char argbuf[1600];
    char *bc = NULL, *vm = NULL;
    int detach = (int)dsj_get_int(args, "detach", 0);
    char err[512];
    Dsj *r;
    if (!ds_project_dir(m) || !*ds_project_dir(m)) {
        seterr(m, "还没有打开项目");
        return NULL;
    }
    run_args(m, args, argbuf, sizeof argbuf, &bc, &vm);
    if (!bc || !file_exists(bc)) {
        seterr(m, "找不到字节码:%s(先「编译」一次)", bc ? bc : dsj_get_str(args, "path", ""));
        free(bc); free(vm);
        return NULL;
    }
    if (!vm) {
        seterr(m, "找不到 VM(project.json 的 run.vm,或 vm/vm.exe)");
        free(bc);
        return NULL;
    }
    if (detach) {
        RunResult dummy;
        memset(&dummy, 0, sizeof dummy);
        if (ds_run_active(m)) {
            seterr(m, "已经有一个游戏在运行(先「停止」)");
            free(bc); free(vm);
            return NULL;
        }
        if (!spawn_ds(m, vm, argbuf, ds_project_dir(m), 0, 0, NULL,
                      ds_model_proc_slot(m), ds_model_pid_slot(m), err, sizeof err)) {
            seterr(m, "%s", err);
            free(bc); free(vm);
            return NULL;
        }
        r = dsj_obj();
        dsj_set_bool(r, "running", 1);
        dsj_set_int(r, "pid", (long long)ds_run_pid(m));
        dsj_set_str(r, "exe", vm);
        dsj_set_str(r, "bytecode", bc);
        free(bc); free(vm);
        return r;
    }
    {
        RunResult rr;
        memset(&rr, 0, sizeof rr);
        if (!spawn_ds(m, vm, argbuf, ds_project_dir(m), 1,
                      (int)dsj_get_int(args, "timeout_ms", 30000), &rr, NULL, NULL,
                      err, sizeof err)) {
            seterr(m, "%s", err);
            free(bc); free(vm);
            return NULL;
        }
        r = dsj_obj();
        dsj_set_bool(r, "running", 0);
        dsj_set_int(r, "code", rr.code);
        dsj_set_bool(r, "timeout", rr.timed_out);
        dsj_set_str(r, "out", rr.out ? rr.out : "");
        dsj_set_str(r, "exe", vm);
        dsj_set_str(r, "bytecode", bc);
        free(rr.out);
        free(bc); free(vm);
        return r;
    }
}

static Dsj *cmd_build_stop(DsModel *m)
{
    Dsj *r = dsj_obj();
    int was = ds_run_active(m);
    ds_run_kill(m);
    dsj_set_bool(r, "stopped", was);
    return r;
}

static Dsj *cmd_build_status(DsModel *m)
{
    Dsj *r = dsj_obj();
    dsj_set_bool(r, "running", ds_run_active(m) != 0);
    dsj_set_int(r, "pid", (long long)ds_run_pid(m));
    dsj_set_int(r, "exit_code", ds_run_exit_code(m));
    return r;
}

/* 用系统默认程序打开脚本(代码页签是**只读**的:改 DexLang 代码用外部编辑器,
 * 免得在 IDE 里造一个半吊子编辑器)。dry:1 只回"会开哪个文件",不真的开进程。 */
static Dsj *cmd_file_open_external(DsModel *m, Dsj *args)
{
    const char *rel = dsj_get_str(args, "path", "scripts/main.dex");
    char *full = proj_path(m, rel);
    Dsj *r = dsj_obj();
    if (!full) {
        seterr(m, "路径无效");
        return NULL;
    }
    if (!file_exists(full)) {
        seterr(m, "文件不存在:%s", full);
        free(full);
        return NULL;
    }
    dsj_set_str(r, "path", full);
    if (dsj_get_bool(args, "dry", 0)) {
        dsj_set_bool(r, "opened", 0);
        dsj_set_bool(r, "dry", 1);
        free(full);
        return r;
    }
#if defined(_WIN32)
    {
        wchar_t *w = dsu_w(full);
        HINSTANCE rc = w ? ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL)
                         : (HINSTANCE)0;
        free(w);
        if ((INT_PTR)rc <= 32) {
            seterr(m, "打不开 %s(系统没有关联的编辑器?)", full);
            free(full);
            return NULL;
        }
    }
    dsj_set_bool(r, "opened", 1);
#else
    dsj_set_bool(r, "opened", 0);
#endif
    free(full);
    return r;
}

Dsj *ds_run_command(DsModel *m, const char *cmd, Dsj *args)
{
    if (!strcmp(cmd, "build.compile")) return cmd_build_compile(m, args);
    if (!strcmp(cmd, "build.run")) return cmd_build_run(m, args);
    if (!strcmp(cmd, "build.stop")) return cmd_build_stop(m);
    if (!strcmp(cmd, "build.status")) return cmd_build_status(m);
    if (!strcmp(cmd, "project.scripts")) return cmd_scripts(m);
    if (!strcmp(cmd, "file.read")) return cmd_file_read(m, args);
    if (!strcmp(cmd, "file.open_external")) return cmd_file_open_external(m, args);
    seterr(m, "未知命令 '%s'", cmd);
    return NULL;
}
