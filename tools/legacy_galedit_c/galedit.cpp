/*
 * galedit.cpp — GAL 编辑器(纯 GDI 自绘 UI · 指令式块编辑器)
 *
 * 不用任何 GUI 库:像游戏引擎一样,每帧把界面画到像素缓冲(g_bits),
 * 再通过 WndProc 收集鼠标/键盘状态做命中测试。完全可控、零依赖、稳定。
 *
 * 设计理念(Scratch / UE 蓝图式):
 *   镜头(Scene) = 一个舞台场景
 *   镜头内 = 指令块(Block)组成的程序,从上到下顺序执行(指令式,非定义式)
 *   块可嵌套(C 形块:选项/如果 包含子块),形成树形流程
 *   人物集(Character) = 每个人物含多张不同状态图 + 状态属性,块引用人物/状态
 *
 * 块类型(指令):
 *   说话 / 背景 / 立绘 / 音频 / 等待 / 清空文本 /
 *   选项(C形) / 选项项(C形) / 如果(C形) / 代码 / 结束
 *
 * 构建:
 *   zig c++ -target x86_64-windows-gnu -O2 \
 *       -o tools/galedit.exe tools/galedit.cpp \
 *       -lgdi32 -luser32 -lcomdlg32 -lcomctl32 -lshell32 -lole32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../libs/gal/stb_image.h"

/* 防止编译器内联合并导致超大栈帧(Scene 含大块数组) */
#ifdef __GNUC__
#define NOINLINE __attribute__((noinline))
#define NOINLINE_P __attribute__((noinline)) static
#else
#define NOINLINE
#define NOINLINE_P static
#endif

/* ---------- 引擎 DLL 函数指针 ---------- */
typedef int64_t(__cdecl *fn_0)(void);
typedef int64_t(__cdecl *fn_i)(int64_t);
typedef int64_t(__cdecl *fn_s)(const char *);
typedef int64_t(__cdecl *fn_i2)(int64_t, int64_t);
typedef int64_t(__cdecl *fn_is)(int64_t, const char *);

static HMODULE g_gal = NULL;
static fn_i2  pgal_init = NULL;
static fn_s   pgal_set_title = NULL, pgal_bg = NULL, pgal_text = NULL, pgal_speaker = NULL;
static fn_0   pgal_running = NULL, pgal_poll = NULL, pgal_close = NULL, pgal_text_clear = NULL;
static fn_0   pgal_text_done = NULL, pgal_clicked = NULL, pgal_picked = NULL,
             pgal_show_choices = NULL, pgal_hide_choices = NULL, pgal_stop_bgm = NULL;
static fn_i   pgal_wait = NULL, pgal_bg_color = NULL, pgal_box = NULL;
static fn_i   pgal_volume = NULL;
static fn_i   pgal_text_pos = NULL, pgal_box_autofit = NULL;
static fn_is  pgal_set_choice = NULL;
static fn_s   pgal_play_bgm = NULL, pgal_play_se = NULL;
static fn_i2  pgal_box_style = NULL, pgal_sprite_show = NULL, pgal_sprite_pos = NULL,
             pgal_sprite_scale = NULL, pgal_sprite_pos_mode = NULL,
             pgal_sprite_autofit = NULL, pgal_sprite_anim = NULL;
static fn_is  pgal_sprite = NULL;

/* ---------- 数据模型 ---------- */
#define MAX_SPR 4
#define MAX_CHOICES 4
#define MAX_BLOCKS 384
#define MAX_SCENES 32
#define MAX_CHARS 32
#define MAX_STATES 10

typedef enum {
    BL_SPEAK = 0, BL_BG, BL_SPRITE, BL_AUDIO, BL_WAIT, BL_CLEAR,
    BL_CHOICE, BL_OPTION, BL_IF, BL_CODE, BL_END
} BlockType;

/* 指令块。块数组为深度优先序(树用 depth 表示):C 形块(选项/选项项/如果)
   的子块 = 其后 depth 更大的连续块。 */
typedef struct {
    int type;
    int depth;                    /* 缩进层级 0=顶层 */
    /* 说话 */
    wchar_t speaker[128];
    wchar_t text[2048];
    int style_avatar;             /* 有头像对话框 */
    int style_name;               /* 显示人名 */
    int text_pos;                 /* 0中 1左 2右 3偏左 4偏右 */
    int auto_fit;                 /* 对话框自适应 */
    /* 背景 */
    int useBgImg;
    int bgColor;
    wchar_t bg[520];
    /* 立绘 */
    int charIdx;                  /* 人物集索引;-1=直接路径 */
    int stateIdx;                 /* 状态索引 */
    wchar_t sprPath[520];         /* 直接路径(人物集为空时用) */
    int spr_layer, spr_show, spr_pos, spr_autofit, spr_anim;
    /* 音频 */
    int audio_type;               /* 0BGM 1SE 2停止BGM 3音量 */
    wchar_t audio_path[520];
    int volume;
    /* 等待 */
    int wait_type;                /* 0点击 1毫秒 */
    int wait_ms;
    /* 选项(CHOICE) */
    wchar_t choice_text[2048];
    /* 选项项(OPTION) */
    wchar_t option_text[256];
    /* 如果(IF) */
    wchar_t cond[256];
    /* 代码(CODE) */
    wchar_t code_file[520];
    wchar_t code_cond[256];
} Block;

/* 人物集:名字 + 多张状态图 + 状态名 */
typedef struct {
    wchar_t name[64];
    wchar_t states[MAX_STATES][520];
    wchar_t stateNames[MAX_STATES][32];
    int nStates;
    int cur;
} Character;

typedef struct {
    wchar_t title[128];
    wchar_t bgDir[520];           /* 背景目录 */
    wchar_t bg[520];
    int useBgImg;
    int bgColor;
    wchar_t spr[MAX_SPR][520];    /* 镜头级立绘资源(兼容保留) */
    Block blocks[MAX_BLOCKS];
    int nBlocks;
    int cur;                      /* 选中块 */
} Scene;

typedef struct {
    wchar_t title[128];
    wchar_t resDir[520];
    Character chars[MAX_CHARS];
    int nChars;
    Scene scenes[MAX_SCENES];
    int nScenes;
    int cur;                      /* 选中镜头 */
} Project;

static Project P;
static wchar_t g_file[1024];
static HWND g_mainWnd = NULL;
static wchar_t g_statusMsg[160];   /* 状态栏临时提示 */

/* ---------- 设置(程序目录/项目根/Python 解释器) ---------- */
static wchar_t g_exeDir[1024];
static wchar_t g_projRoot[1024];
static wchar_t g_pythonExe[1024];
static int g_settingMode = 0;

/* ---------- 工具 ---------- */
static void utf8_to_w(const char *s, wchar_t *out, int cap) {
    MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap);
}
static void w_to_utf8(const wchar_t *w, char *out, int cap) {
    WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, out, cap, NULL, NULL);
}

/* ---------- 默认值 ---------- */
static void block_defaults(Block *b, int type, int depth) {
    memset(b, 0, sizeof *b);
    b->type = type;
    b->depth = depth;
    b->style_name = 1;
    b->bgColor = 0x101018;
    b->useBgImg = 1;
    b->charIdx = -1;
    b->stateIdx = 0;
    b->spr_layer = 0;
    b->spr_show = 1;
    b->spr_pos = 0;
    b->wait_type = 0;
    b->volume = 80;
}

static void scene_defaults(Scene *s) {
    memset(s, 0, sizeof *s);
    swprintf(s->title, 128, L"镜头 %d", P.nScenes + 1);
    s->bgColor = 0x101018;
    s->useBgImg = 1;
    s->nBlocks = 1; s->cur = 0;
    block_defaults(&s->blocks[0], BL_SPEAK, 0);
}

static void project_defaults(void) {
    memset(&P, 0, sizeof P);
    swprintf(P.title, 128, L"新项目");
    P.nScenes = 1; P.cur = 0;
    scene_defaults(&P.scenes[0]);
}

/* ---------- 场景读写(.galscene,UTF-8;指令块 + 人物集) ---------- */
static void block_save(FILE *f, Block *b) {
    char buf[4600];
    fprintf(f, "[block]\n");
    fprintf(f, "type=%d\ndepth=%d\n", b->type, b->depth);
    if (b->type == BL_SPEAK || b->type == BL_CHOICE) {
        w_to_utf8(b->speaker, buf, sizeof buf); fprintf(f, "speaker=%s\n", buf);
        w_to_utf8(b->text, buf, sizeof buf);
        for (char *p = buf; *p; p++) if (*p == '\n') *p = ' ';
        fprintf(f, "text=%s\n", buf);
    }
    if (b->type == BL_SPEAK)
        fprintf(f, "avatar=%d\nname=%d\ntextpos=%d\nautofit=%d\n",
                b->style_avatar, b->style_name, b->text_pos, b->auto_fit);
    if (b->type == BL_BG) {
        w_to_utf8(b->bg, buf, sizeof buf); fprintf(f, "bg=%s\n", buf);
        fprintf(f, "usebgimg=%d\nbgcolor=%d\n", b->useBgImg, b->bgColor);
    }
    if (b->type == BL_SPRITE) {
        fprintf(f, "charidx=%d\nstateidx=%d\n", b->charIdx, b->stateIdx);
        w_to_utf8(b->sprPath, buf, sizeof buf); fprintf(f, "sprpath=%s\n", buf);
        fprintf(f, "sprlayer=%d\nsprshow=%d\nsprpos=%d\nspr_autofit=%d\nspranim=%d\n",
                b->spr_layer, b->spr_show, b->spr_pos, b->spr_autofit, b->spr_anim);
    }
    if (b->type == BL_AUDIO) {
        fprintf(f, "audio_type=%d\nvolume=%d\n", b->audio_type, b->volume);
        w_to_utf8(b->audio_path, buf, sizeof buf); fprintf(f, "audio_path=%s\n", buf);
    }
    if (b->type == BL_WAIT) fprintf(f, "wait_type=%d\nwait_ms=%d\n", b->wait_type, b->wait_ms);
    if (b->type == BL_OPTION) {
        w_to_utf8(b->option_text, buf, sizeof buf);
        for (char *p = buf; *p; p++) if (*p == '\n') *p = ' ';
        fprintf(f, "option_text=%s\n", buf);
    }
    if (b->type == BL_IF) {
        w_to_utf8(b->cond, buf, sizeof buf); fprintf(f, "cond=%s\n", buf);
    }
    if (b->type == BL_CODE) {
        w_to_utf8(b->code_file, buf, sizeof buf); fprintf(f, "codefile=%s\n", buf);
        w_to_utf8(b->code_cond, buf, sizeof buf); fprintf(f, "codecond=%s\n", buf);
    }
    fprintf(f, "[/block]\n");
}

static void scene_save(const wchar_t *path) {
    char upath[1024]; w_to_utf8(path, upath, sizeof upath);
    FILE *f = fopen(upath, "w"); if (!f) return;
    char buf[4096];
    w_to_utf8(P.title, buf, sizeof buf); fprintf(f, "title=%s\n", buf);
    w_to_utf8(P.resDir, buf, sizeof buf); fprintf(f, "res=%s\n", buf);
    for (int c = 0; c < P.nChars; c++) {
        Character *ch = &P.chars[c];
        fprintf(f, "[char]\n");
        w_to_utf8(ch->name, buf, sizeof buf); fprintf(f, "cname=%s\n", buf);
        fprintf(f, "nstates=%d\n", ch->nStates);
        for (int s = 0; s < ch->nStates; s++) {
            w_to_utf8(ch->stateNames[s], buf, sizeof buf); fprintf(f, "stname=%s\n", buf);
            w_to_utf8(ch->states[s], buf, sizeof buf); fprintf(f, "stimg=%s\n", buf);
        }
        fprintf(f, "[/char]\n");
    }
    for (int s = 0; s < P.nScenes; s++) {
        Scene *sc = &P.scenes[s];
        fprintf(f, "[scene]\n");
        w_to_utf8(sc->title, buf, sizeof buf); fprintf(f, "stitle=%s\n", buf);
        w_to_utf8(sc->bgDir, buf, sizeof buf); fprintf(f, "bgdir=%s\n", buf);
        w_to_utf8(sc->bg, buf, sizeof buf); fprintf(f, "bg=%s\n", buf);
        fprintf(f, "bgcolor=%d\nusebgimg=%d\n", sc->bgColor, sc->useBgImg);
        for (int k = 0; k < MAX_SPR; k++) {
            w_to_utf8(sc->spr[k], buf, sizeof buf);
            fprintf(f, "spr%d=%s\n", k, buf);
        }
        for (int i = 0; i < sc->nBlocks; i++) block_save(f, &sc->blocks[i]);
        fprintf(f, "[/scene]\n");
    }
    fclose(f);
}

/* 旧格式(单场景 + 多个 [shot]):逐 shot 转成 背景/立绘/说话/选项 指令块 */
static void migrate_old(FILE *f, Scene *sc) {
    char line[4096], key[256], val[3600];
    int k, c;
    sc->nBlocks = 0;
    rewind(f);
    static wchar_t tbg[520], tSpr[MAX_SPR][520], tSpk[128], tTxt[2048], tCh[MAX_CHOICES][256];
    static int tUse, tCol, tNC;
    int in_shot = 0, shotNo = 0;

    auto reset = [&]() {
        tbg[0] = 0; tSpk[0] = 0; tTxt[0] = 0; tUse = 1; tCol = 0x101018; tNC = 0;
        memset(tSpr, 0, sizeof tSpr); memset(tCh, 0, sizeof tCh);
    };
    auto commit = [&]() {
        if (shotNo == 0) {
            /* 首个 shot 的舞台属性 → 场景级 */
            wcscpy(sc->bg, tbg); sc->useBgImg = tUse; sc->bgColor = tCol;
            for (int i = 0; i < MAX_SPR; i++) wcscpy(sc->spr[i], tSpr[i]);
        } else if (tbg[0] && wcscmp(tbg, sc->bg) != 0) {
            if (sc->nBlocks < MAX_BLOCKS) {
                Block *b = &sc->blocks[sc->nBlocks++];
                block_defaults(b, BL_BG, 0);
                wcscpy(b->bg, tbg); b->useBgImg = tUse; b->bgColor = tCol;
            }
        }
        for (int i = 0; i < MAX_SPR; i++) {
            if (tSpr[i][0]) {
                if (sc->nBlocks < MAX_BLOCKS) {
                    Block *b = &sc->blocks[sc->nBlocks++];
                    block_defaults(b, BL_SPRITE, 0);
                    b->spr_layer = i; b->spr_show = 1; b->spr_autofit = 1;
                    b->charIdx = -1; wcscpy(b->sprPath, tSpr[i]);
                }
            }
        }
        if (tNC > 0) {
            /* 先出前置台词,再出选项块 */
            if (tTxt[0] && sc->nBlocks < MAX_BLOCKS) {
                Block *b = &sc->blocks[sc->nBlocks++];
                block_defaults(b, BL_SPEAK, 0);
                wcscpy(b->speaker, tSpk); wcscpy(b->text, tTxt);
            }
            if (sc->nBlocks < MAX_BLOCKS) {
                Block *ch = &sc->blocks[sc->nBlocks++];
                block_defaults(ch, BL_CHOICE, 0);
                wcscpy(ch->choice_text, tTxt);   /* 问题文本 = 台词 */
            }
            for (int c2 = 0; c2 < tNC && c2 < MAX_CHOICES; c2++) {
                if (sc->nBlocks < MAX_BLOCKS) {
                    Block *op = &sc->blocks[sc->nBlocks++];
                    block_defaults(op, BL_OPTION, 1);
                    wcscpy(op->option_text, tCh[c2]);
                }
            }
        } else if (tTxt[0]) {
            if (sc->nBlocks < MAX_BLOCKS) {
                Block *b = &sc->blocks[sc->nBlocks++];
                block_defaults(b, BL_SPEAK, 0);
                wcscpy(b->speaker, tSpk); wcscpy(b->text, tTxt);
            }
        }
        shotNo++;
        reset();
    };

    reset();
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, "[shot]") == 0) { in_shot = 1; continue; }
        if (strcmp(line, "[/shot]") == 0) { if (in_shot) commit(); in_shot = 0; continue; }
        if (!in_shot) {
            if (sscanf(line, "title=%255[^\n]", key) == 1) utf8_to_w(key, sc->title, 128);
            continue;
        }
        if (sscanf(line, "bg=%3599[^\n]", val) == 1) utf8_to_w(val, tbg, 520);
        else if (sscanf(line, "bgcolor=%d", &tCol) == 1) {}
        else if (sscanf(line, "usebgimg=%d", &tUse) == 1) {}
        else if (sscanf(line, "spr%d=%3599[^\n]", &k, val) == 2 && k >= 0 && k < MAX_SPR)
            utf8_to_w(val, tSpr[k], 520);
        else if (sscanf(line, "speaker=%3599[^\n]", val) == 1) utf8_to_w(val, tSpk, 128);
        else if (sscanf(line, "text=%3599[^\n]", val) == 1) utf8_to_w(val, tTxt, 2048);
        else if (sscanf(line, "choice%d=%3599[^\n]", &c, val) == 2 && c >= 0 && c < MAX_CHOICES) {
            utf8_to_w(val, tCh[c], 256);
            if (c + 1 > tNC) tNC = c + 1;
        }
        else if (sscanf(line, "nchoices=%d", &c) == 1) {}
    }
    if (in_shot) commit();
    if (sc->nBlocks == 0) { block_defaults(&sc->blocks[0], BL_SPEAK, 0); sc->nBlocks = 1; }
    sc->cur = 0;
}

static void scene_load(const wchar_t *path) {
    char upath[1024]; w_to_utf8(path, upath, sizeof upath);
    FILE *f = fopen(upath, "r"); if (!f) return;
    static Project np;   /* Project 巨大,必须 static */
    memset(&np, 0, sizeof np);
    char line[4096], key[256], val[3600];
    int k;
    Scene *sc = NULL;
    Block *b = NULL;
    Character *ch = NULL;
    int old_format = 0;

    while (fgets(line, sizeof line, f)) {
        if (strcmp(line, "[shot]\n") == 0 || strcmp(line, "[shot]\r\n") == 0) { old_format = 1; break; }
        if (strcmp(line, "[scene]\n") == 0 || strcmp(line, "[scene]\r\n") == 0) break;
    }
    rewind(f);

    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, "[char]") == 0) {
            if (np.nChars < MAX_CHARS) { ch = &np.chars[np.nChars++]; memset(ch, 0, sizeof *ch); }
            else ch = NULL;
            continue;
        }
        if (strcmp(line, "[/char]") == 0) { ch = NULL; continue; }
        if (strcmp(line, "[scene]") == 0) {
            if (np.nScenes < MAX_SCENES) { sc = &np.scenes[np.nScenes++]; memset(sc, 0, sizeof *sc); sc->nBlocks = 0; sc->cur = 0; sc->bgColor = 0x101018; sc->useBgImg = 1; }
            else sc = NULL;
            b = NULL;
            continue;
        }
        if (strcmp(line, "[/scene]") == 0) { sc = NULL; b = NULL; continue; }
        if (strcmp(line, "[block]") == 0) {
            if (sc && sc->nBlocks < MAX_BLOCKS) { b = &sc->blocks[sc->nBlocks]; block_defaults(b, BL_SPEAK, 0); }
            else b = NULL;
            continue;
        }
        if (strcmp(line, "[/block]") == 0) { if (sc && b) sc->nBlocks++; b = NULL; continue; }
        if (sscanf(line, "title=%255[^\n]", key) == 1) utf8_to_w(key, np.title, 128);
        else if (sscanf(line, "res=%255[^\n]", key) == 1) utf8_to_w(key, np.resDir, 520);
        else if (ch && sscanf(line, "cname=%63[^\n]", key) == 1) utf8_to_w(key, ch->name, 64);
        else if (ch && sscanf(line, "nstates=%d", &k) == 1) {
            /* nstates 仅作上限参考,不直接设 nStates(由 stimg 逐个递增) */
            if (k > MAX_STATES) k = MAX_STATES;
        }
        else if (ch && sscanf(line, "stname=%31[^\n]", key) == 1) {
            if (ch->nStates < MAX_STATES) utf8_to_w(key, ch->stateNames[ch->nStates], 32);
        }
        else if (ch && sscanf(line, "stimg=%3599[^\n]", val) == 1) {
            if (ch->nStates < MAX_STATES) { utf8_to_w(val, ch->states[ch->nStates], 520); ch->nStates++; }
        }
        else if (sc && sscanf(line, "stitle=%255[^\n]", key) == 1) utf8_to_w(key, sc->title, 128);
        else if (sc && sscanf(line, "bgdir=%3599[^\n]", val) == 1) utf8_to_w(val, sc->bgDir, 520);
        else if (sc && sscanf(line, "bg=%3599[^\n]", val) == 1) {
            if (b) utf8_to_w(val, b->bg, 520);
            else utf8_to_w(val, sc->bg, 520);
        }
        else if (sc && sscanf(line, "bgcolor=%d", &k) == 1) {
            if (b) b->bgColor = k;
            else sc->bgColor = k;
        }
        else if (sc && sscanf(line, "usebgimg=%d", &k) == 1) {
            if (b) b->useBgImg = k;
            else sc->useBgImg = k;
        }
        else if (sc && sscanf(line, "spr%d=%3599[^\n]", &k, val) == 2 && k >= 0 && k < MAX_SPR)
            utf8_to_w(val, sc->spr[k], 520);
        else if (sc && b && sscanf(line, "type=%d", &b->type) == 1) {
            if (b->type < BL_SPEAK || b->type > BL_END) b->type = BL_SPEAK;
        }
        else if (sc && b && sscanf(line, "depth=%d", &b->depth) == 1) {
            if (b->depth < 0 || b->depth > 12) b->depth = 0;
        }
        else if (sc && b && sscanf(line, "speaker=%3599[^\n]", val) == 1) utf8_to_w(val, b->speaker, 128);
        else if (sc && b && sscanf(line, "text=%3599[^\n]", val) == 1) utf8_to_w(val, b->text, 2048);
        else if (sc && b && sscanf(line, "avatar=%d", &b->style_avatar) == 1) {}
        else if (sc && b && sscanf(line, "name=%d", &b->style_name) == 1) {}
        else if (sc && b && sscanf(line, "textpos=%d", &b->text_pos) == 1) {}
        else if (sc && b && sscanf(line, "autofit=%d", &b->auto_fit) == 1) {}
        else if (sc && b && sscanf(line, "charidx=%d", &b->charIdx) == 1) {}
        else if (sc && b && sscanf(line, "stateidx=%d", &b->stateIdx) == 1) {}
        else if (sc && b && sscanf(line, "sprpath=%3599[^\n]", val) == 1) utf8_to_w(val, b->sprPath, 520);
        else if (sc && b && sscanf(line, "sprlayer=%d", &b->spr_layer) == 1) {}
        else if (sc && b && sscanf(line, "sprshow=%d", &b->spr_show) == 1) {}
        else if (sc && b && sscanf(line, "sprpos=%d", &b->spr_pos) == 1) {}
        else if (sc && b && sscanf(line, "spr_autofit=%d", &b->spr_autofit) == 1) {}
        else if (sc && b && sscanf(line, "spranim=%d", &b->spr_anim) == 1) {}
        else if (sc && b && sscanf(line, "audio_type=%d", &b->audio_type) == 1) {}
        else if (sc && b && sscanf(line, "volume=%d", &b->volume) == 1) {}
        else if (sc && b && sscanf(line, "audio_path=%3599[^\n]", val) == 1) utf8_to_w(val, b->audio_path, 520);
        else if (sc && b && sscanf(line, "wait_type=%d", &b->wait_type) == 1) {}
        else if (sc && b && sscanf(line, "wait_ms=%d", &b->wait_ms) == 1) {}
        else if (sc && b && sscanf(line, "option_text=%3599[^\n]", val) == 1) utf8_to_w(val, b->option_text, 256);
        else if (sc && b && sscanf(line, "cond=%255[^\n]", val) == 1) utf8_to_w(val, b->cond, 256);
        else if (sc && b && sscanf(line, "codefile=%3599[^\n]", val) == 1) utf8_to_w(val, b->code_file, 520);
        else if (sc && b && sscanf(line, "codecond=%255[^\n]", val) == 1) utf8_to_w(val, b->code_cond, 256);
    }

    if (old_format && np.nScenes == 0) {
        np.nScenes = 1;
        migrate_old(f, &np.scenes[0]);
    }
    fclose(f);

    if (np.nScenes == 0) { np.nScenes = 1; scene_defaults(&np.scenes[0]); }
    for (int s = 0; s < np.nScenes; s++) {
        Scene *sc2 = &np.scenes[s];
        if (sc2->nBlocks < 1) { sc2->nBlocks = 1; block_defaults(&sc2->blocks[0], BL_SPEAK, 0); }
        if (sc2->nBlocks > MAX_BLOCKS) sc2->nBlocks = MAX_BLOCKS;
        if (sc2->cur < 0 || sc2->cur >= sc2->nBlocks) sc2->cur = 0;
    }
    P = np;
    if (P.cur < 0 || P.cur >= P.nScenes) P.cur = 0;
}

/* ---------- 块树工具 ---------- */
static int block_end(Scene *sc, int i) {
    int d = sc->blocks[i].depth;
    int j = i + 1;
    while (j < sc->nBlocks && sc->blocks[j].depth > d) j++;
    return j;
}
static int block_parent(Scene *sc, int i) {
    int d = sc->blocks[i].depth;
    for (int j = i - 1; j >= 0; j--)
        if (sc->blocks[j].depth < d) return j;
    return -1;
}
static int prev_sibling(Scene *sc, int i) {
    int d = sc->blocks[i].depth;
    for (int j = i - 1; j >= 0; j--) {
        if (sc->blocks[j].depth < d) return -1;
        if (sc->blocks[j].depth == d) return j;
    }
    return -1;
}
static int next_sibling(Scene *sc, int i) {
    int d = sc->blocks[i].depth;
    int s = block_end(sc, i);
    for (int j = s; j < sc->nBlocks; j++) {
        if (sc->blocks[j].depth < d) return -1;
        if (sc->blocks[j].depth == d) return j;
    }
    return -1;
}
/* 移动块 i 的整棵子树:0=插到 target 前(同父) 1=插到 target 后(同父) 2=作为 target 子块 */
static void block_move(Scene *sc, int i, int target, int where) {
    if (i == target) return;
    int s = block_end(sc, i);
    int n = s - i;
    if (n <= 0) return;
    static Block tmp[MAX_BLOCKS];
    memcpy(tmp, sc->blocks + i, sizeof(Block) * n);
    int nd = sc->blocks[target].depth + (where == 2 ? 1 : 0);
    if (nd < 0) nd = 0;
    if (nd > 10) nd = 10;
    int delta = nd - tmp[0].depth;
    for (int k = 0; k < n; k++) tmp[k].depth += delta;
    memmove(sc->blocks + i, sc->blocks + s, sizeof(Block) * (sc->nBlocks - s));
    sc->nBlocks -= n;
    int t = target;
    if (t > i) t -= n;
    if (t < 0) t = 0;
    int pos;
    if (where == 0) pos = t;
    else if (where == 1) pos = block_end(sc, t);
    else pos = t + 1;
    if (pos < 0) pos = 0;
    if (pos > sc->nBlocks) pos = sc->nBlocks;
    memmove(sc->blocks + pos + n, sc->blocks + pos, sizeof(Block) * (sc->nBlocks - pos));
    memcpy(sc->blocks + pos, tmp, sizeof(Block) * n);
    sc->nBlocks += n;
    sc->cur = (i < pos) ? pos + n - 1 : pos;
}

/* 在选中块后插入新块(普通块同父;拖到 C 形块外嵌套为子块) */
static int insert_block(Scene *sc, int type) {
    if (sc->nBlocks >= MAX_BLOCKS) return -1;
    int i = sc->cur;
    if (i < 0 || i >= sc->nBlocks) i = sc->nBlocks - 1;
    int d = sc->blocks[i].depth;
    /* 若选中块是 C 形且尚未展开,新块作为其子块;否则同父 */
    int is_c = (sc->blocks[i].type == BL_CHOICE || sc->blocks[i].type == BL_OPTION ||
                sc->blocks[i].type == BL_IF);
    int nd = d;
    if (is_c && sc->blocks[i].type == BL_CHOICE && type == BL_OPTION) nd = d + 1;
    /* 插入到选中块之后(若选中块有子块,插到其子树末尾,保持同父) */
    int end = block_end(sc, i);
    int pos = end;
    /* 目标 depth:若插到 C 形块内作为子块 */
    memmove(sc->blocks + pos + 1, sc->blocks + pos, sizeof(Block) * (sc->nBlocks - pos));
    Block *nb = &sc->blocks[pos];
    block_defaults(nb, type, nd);
    sc->nBlocks++;
    sc->cur = pos;
    return pos;
}

/* ---------- 引擎加载/预览 ---------- */
static HWND hPreview = NULL;
static int g_havePreview = 0;

static int engine_load(void) {
    char path[MAX_PATH];
    if (g_projRoot[0]) {
        char root[MAX_PATH];
        w_to_utf8(g_projRoot, root, sizeof root);
        snprintf(path, sizeof path, "%s\\libs\\gal\\libdexxgal.dll", root);
    } else {
        GetCurrentDirectoryA(MAX_PATH, path);
        snprintf(path + strlen(path), MAX_PATH - strlen(path), "\\libs\\gal\\libdexxgal.dll");
    }
    g_gal = LoadLibraryA(path);
    if (!g_gal) return 0;
    pgal_init = (fn_i2)GetProcAddress(g_gal, "gal_init");
    pgal_set_title = (fn_s)GetProcAddress(g_gal, "gal_set_title");
    pgal_running = (fn_0)GetProcAddress(g_gal, "gal_running");
    pgal_poll = (fn_0)GetProcAddress(g_gal, "gal_poll");
    pgal_wait = (fn_i)GetProcAddress(g_gal, "gal_wait");
    pgal_close = (fn_0)GetProcAddress(g_gal, "gal_close");
    pgal_bg = (fn_s)GetProcAddress(g_gal, "gal_bg");
    pgal_sprite = (fn_is)GetProcAddress(g_gal, "gal_sprite");
    pgal_text = (fn_s)GetProcAddress(g_gal, "gal_text");
    pgal_speaker = (fn_s)GetProcAddress(g_gal, "gal_speaker");
    pgal_bg_color = (fn_i)GetProcAddress(g_gal, "gal_bg_color");
    pgal_text_clear = (fn_0)GetProcAddress(g_gal, "gal_text_clear");
    pgal_text_done = (fn_0)GetProcAddress(g_gal, "gal_text_done");
    pgal_clicked = (fn_0)GetProcAddress(g_gal, "gal_clicked");
    pgal_picked = (fn_0)GetProcAddress(g_gal, "gal_picked");
    pgal_show_choices = (fn_0)GetProcAddress(g_gal, "gal_show_choices");
    pgal_hide_choices = (fn_0)GetProcAddress(g_gal, "gal_hide_choices");
    pgal_stop_bgm = (fn_0)GetProcAddress(g_gal, "gal_stop_bgm");
    pgal_set_choice = (fn_is)GetProcAddress(g_gal, "gal_set_choice");
    pgal_play_bgm = (fn_s)GetProcAddress(g_gal, "gal_play_bgm");
    pgal_play_se = (fn_s)GetProcAddress(g_gal, "gal_play_se");
    pgal_volume = (fn_i)GetProcAddress(g_gal, "gal_volume");
    pgal_sprite_show = (fn_i2)GetProcAddress(g_gal, "gal_sprite_show");
    pgal_sprite_pos = (fn_i2)GetProcAddress(g_gal, "gal_sprite_pos");
    pgal_sprite_scale = (fn_i2)GetProcAddress(g_gal, "gal_sprite_scale");
    pgal_box = (fn_i)GetProcAddress(g_gal, "gal_box");
    pgal_text_pos = (fn_i)GetProcAddress(g_gal, "gal_text_pos");
    pgal_box_style = (fn_i2)GetProcAddress(g_gal, "gal_box_style");
    pgal_box_autofit = (fn_i)GetProcAddress(g_gal, "gal_box_autofit");
    pgal_sprite_pos_mode = (fn_i2)GetProcAddress(g_gal, "gal_sprite_pos_mode");
    pgal_sprite_autofit = (fn_i2)GetProcAddress(g_gal, "gal_sprite_autofit");
    pgal_sprite_anim = (fn_i2)GetProcAddress(g_gal, "gal_sprite_anim");
    return g_gal && pgal_init && pgal_poll && pgal_bg_color && pgal_bg && pgal_sprite_pos_mode;
}

/* 解析块的立绘图片路径:优先人物集状态,否则直接路径 */
static void block_spr_path(Scene *sc, Block *b, wchar_t *out, int cap) {
    if (b->charIdx >= 0 && b->charIdx < P.nChars) {
        Character *ch = &P.chars[b->charIdx];
        if (b->stateIdx >= 0 && b->stateIdx < ch->nStates && ch->states[b->stateIdx][0]) {
            wcscpy(out, ch->states[b->stateIdx]);
            return;
        }
    }
    if (b->sprPath[0]) wcscpy(out, b->sprPath);
    else out[0] = 0;
}

static void apply_scene_preview(Scene *sc) NOINLINE;
static void apply_scene_preview(Scene *sc) {
    if (!g_gal || !g_havePreview) return;
    char buf[520];
    if (sc->useBgImg && sc->bg[0]) {
        w_to_utf8(sc->bg, buf, sizeof buf);
        if (pgal_bg(buf) != 0) { if (pgal_bg_color) pgal_bg_color(sc->bgColor); }
    } else {
        if (pgal_bg_color) pgal_bg_color(sc->bgColor);
    }
    for (int k = 0; k < MAX_SPR; k++) {
        if (sc->spr[k][0]) {
            w_to_utf8(sc->spr[k], buf, sizeof buf);
            if (pgal_sprite(k, buf) == 0) {
                if (pgal_sprite_scale) pgal_sprite_scale(k, 100);
                if (pgal_sprite_pos_mode) pgal_sprite_pos_mode(k, 0);
                if (pgal_sprite_show) pgal_sprite_show(k, 0);
            }
        } else if (pgal_sprite_show) {
            pgal_sprite_show(k, 0);
        }
    }
    if (pgal_poll) pgal_poll();
}

static void apply_block_preview(Scene *sc, Block *b) {
    if (!g_gal || !g_havePreview || !b) return;
    char buf[520];
    switch (b->type) {
    case BL_SPEAK:
        if (pgal_text_clear) pgal_text_clear();
        if (pgal_speaker && b->speaker[0]) { w_to_utf8(b->speaker, buf, sizeof buf); pgal_speaker(buf); }
        if (pgal_box_style) pgal_box_style(b->style_avatar, b->style_name);
        if (pgal_box_autofit) pgal_box_autofit(b->auto_fit);
        if (pgal_text_pos) pgal_text_pos(b->text_pos);
        if (b->text[0] && pgal_text) { w_to_utf8(b->text, buf, sizeof buf); pgal_text(buf); }
        break;
    case BL_BG:
        if (b->useBgImg && b->bg[0]) {
            w_to_utf8(b->bg, buf, sizeof buf);
            if (pgal_bg(buf) != 0) { if (pgal_bg_color) pgal_bg_color(b->bgColor); }
        } else if (pgal_bg_color) pgal_bg_color(b->bgColor);
        break;
    case BL_SPRITE: {
        int k = b->spr_layer;
        if (k < 0 || k >= MAX_SPR) k = 0;
        wchar_t wp[520]; block_spr_path(sc, b, wp, 520);
        if (wp[0]) {
            w_to_utf8(wp, buf, sizeof buf);
            if (pgal_sprite(k, buf) == 0) {
                if (pgal_sprite_show) pgal_sprite_show(k, b->spr_show);
                if (pgal_sprite_pos_mode) pgal_sprite_pos_mode(k, b->spr_pos);
                if (pgal_sprite_autofit) pgal_sprite_autofit(k, b->spr_autofit);
                if (pgal_sprite_anim) pgal_sprite_anim(k, b->spr_anim);
            }
        }
        break;
    }
    default:
        break;
    }
    if (pgal_poll) pgal_poll();
}

NOINLINE_P void create_preview(HWND parent, int x, int y, int w, int h) {
    if (g_havePreview) {
        static int lx = -1, ly = -1, lw = -1, lh = -1;
        if (x != lx || y != ly || w != lw || h != lh) {
            SetWindowPos(hPreview, NULL, x, y, w, h, SWP_NOZORDER);
            lx = x; ly = y; lw = w; lh = h;
        }
        return;
    }
    if (!engine_load()) return;
    pgal_init(960, 540);
    hPreview = FindWindowW(L"DexGALWindow", NULL);
    if (hPreview) {
        pgal_set_title("GAL 预览");
        SetParent(hPreview, parent);
        LONG style = GetWindowLongPtrW(hPreview, GWL_STYLE);
        SetWindowLongPtrW(hPreview, GWL_STYLE, (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME)) | WS_CHILD);
        SetWindowPos(hPreview, NULL, x, y, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
        ShowWindow(hPreview, SW_SHOW);
        g_havePreview = 1;
        apply_scene_preview(&P.scenes[P.cur]);
        apply_block_preview(&P.scenes[P.cur], &P.scenes[P.cur].blocks[P.scenes[P.cur].cur]);
    } else {
        if (pgal_close) pgal_close();
    }
}

/* ---------- 指令式导出 ---------- */
static void str_esc(FILE *f, const wchar_t *w) {
    char buf[4600];
    w_to_utf8(w, buf, sizeof buf);
    for (char *p = buf; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
}

static void emit_blocks(FILE *f, Scene *sc, int start, int parentDepth);

static void emit_normal(FILE *f, Scene *sc, Block *b) {
    switch (b->type) {
    case BL_SPEAK:
        if (b->speaker[0]) { fprintf(f, "    gal_speaker(\""); str_esc(f, b->speaker); fprintf(f, "\");\n"); }
        fprintf(f, "    gal_box_style(%d, %d);\n", b->style_avatar, b->style_name);
        fprintf(f, "    gal_box_autofit(%d);\n", b->auto_fit);
        fprintf(f, "    gal_text_pos(%d);\n", b->text_pos);
        if (b->text[0]) {
            fprintf(f, "    gal_text(\""); str_esc(f, b->text); fprintf(f, "\");\n");
            fprintf(f, "    wait_click();\n");
            fprintf(f, "    gal_text_clear();\n");
        }
        break;
    case BL_BG:
        if (b->useBgImg && b->bg[0]) { fprintf(f, "    gal_bg(\""); str_esc(f, b->bg); fprintf(f, "\");\n"); }
        else fprintf(f, "    gal_bg_color(0x%06X);\n", b->bgColor);
        break;
    case BL_SPRITE: {
        int k = b->spr_layer;
        if (k < 0 || k >= MAX_SPR) k = 0;
        wchar_t wp[520]; block_spr_path(sc, b, wp, 520);
        if (wp[0]) {
            fprintf(f, "    gal_sprite(%d, \"", k); str_esc(f, wp); fprintf(f, "\");\n");
            fprintf(f, "    gal_sprite_pos_mode(%d, %d);\n", k, b->spr_pos);
            fprintf(f, "    gal_sprite_autofit(%d, %d);\n", k, b->spr_autofit);
            fprintf(f, "    gal_sprite_anim(%d, %d);\n", k, b->spr_anim);
            fprintf(f, "    gal_sprite_show(%d, %d);\n", k, b->spr_show);
        }
        break;
    }
    case BL_AUDIO:
        if (b->audio_type == 0 && b->audio_path[0]) {
            fprintf(f, "    gal_play_bgm(\""); str_esc(f, b->audio_path); fprintf(f, "\");\n");
        } else if (b->audio_type == 1 && b->audio_path[0]) {
            fprintf(f, "    gal_play_se(\""); str_esc(f, b->audio_path); fprintf(f, "\");\n");
        } else if (b->audio_type == 2) {
            fprintf(f, "    gal_stop_bgm();\n");
        } else if (b->audio_type == 3) {
            fprintf(f, "    gal_volume(%d);\n", b->volume);
        }
        break;
    case BL_WAIT:
        if (b->wait_type == 1 && b->wait_ms > 0) fprintf(f, "    gal_wait(%d);\n", b->wait_ms);
        else {
            fprintf(f, "    while gal_running() { gal_poll(); if gal_clicked() { break; } gal_wait(16); }\n");
        }
        break;
    case BL_CLEAR:
        fprintf(f, "    gal_text_clear();\n");
        break;
    case BL_CODE: {
        char cf[560]; w_to_utf8(b->code_file, cf, sizeof cf);
        FILE *cf2 = fopen(cf, "rb");
        if (cf2) {
            if (b->code_cond[0]) { fprintf(f, "    if "); str_esc(f, b->code_cond); fprintf(f, " {\n"); }
            static char cbuf[65536];
            size_t r = fread(cbuf, 1, sizeof cbuf - 1, cf2);
            cbuf[r] = 0;
            fclose(cf2);
            int col = 0;
            for (size_t j = 0; j < r; j++) {
                char ch = cbuf[j];
                if (col == 0 && ch != '\n' && ch != '\r') { fputs("      ", f); col = 1; }
                fputc(ch, f);
                if (ch == '\n') col = 0;
            }
            if (col == 0 && r && cbuf[r - 1] != '\n') fputc('\n', f);
            if (b->code_cond[0]) fprintf(f, "    }\n");
        } else {
            fprintf(f, "    // 代码文件未找到: %s\n", cf);
        }
        break;
    }
    case BL_END:
    default:
        break;
    }
}

/* 递归导出块序列(从 start 开始,处理 depth>parentDepth 的块) */
static void emit_blocks(FILE *f, Scene *sc, int start, int parentDepth) {
    int i = start;
    while (i < sc->nBlocks && sc->blocks[i].depth > parentDepth) {
        Block *b = &sc->blocks[i];
        int d = b->depth;
        if (b->type == BL_CHOICE) {
            if (b->speaker[0]) { fprintf(f, "    gal_speaker(\""); str_esc(f, b->speaker); fprintf(f, "\");\n"); }
            if (b->choice_text[0]) { fprintf(f, "    gal_text(\""); str_esc(f, b->choice_text); fprintf(f, "\");\n"); }
            /* 数选项数(直接子块中 type==OPTION 且 depth==d+1) */
            int nopt = 0;
            for (int j = i + 1; j < sc->nBlocks && sc->blocks[j].depth > d; j++) {
                if (sc->blocks[j].type == BL_OPTION && sc->blocks[j].depth == d + 1) nopt++;
            }
            if (nopt < 1) nopt = 1;
            int optIdx = 0;
            int j = i + 1;
            while (j < sc->nBlocks && sc->blocks[j].depth > d) {
                if (sc->blocks[j].type == BL_OPTION && sc->blocks[j].depth == d + 1) {
                    if (optIdx >= nopt) break;
                    if (optIdx >= MAX_CHOICES) break;
                    fprintf(f, "    gal_set_choice(%d, \"", optIdx);
                    str_esc(f, sc->blocks[j].option_text[0] ? sc->blocks[j].option_text : L"…");
                    fprintf(f, "\");\n");
                    optIdx++;
                }
                j = block_end(sc, j);
            }
            fprintf(f, "    gal_show_choices();\n");
            fprintf(f, "    wait_pick();\n");
            fprintf(f, "    gal_hide_choices();\n");
            fprintf(f, "    gal_text_clear();\n");
            fprintf(f, "    let p = gal_picked();\n");
            int sel = 0;
            int j2 = i + 1;
            while (j2 < sc->nBlocks && sc->blocks[j2].depth > d) {
                if (sc->blocks[j2].type == BL_OPTION && sc->blocks[j2].depth == d + 1) {
                    fprintf(f, "    %sif p == %d {\n", sel == 0 ? "" : "else ", sel);
                    emit_blocks(f, sc, j2 + 1, d + 1);
                    fprintf(f, "    }\n");
                    sel++;
                }
                j2 = block_end(sc, j2);
            }
            if (sel == 0) { fprintf(f, "    if p >= 0 {}\n"); }
            i = j2;
        } else if (b->type == BL_OPTION) {
            /* OPTION 不应在顶层单独出现;忽略其自身,导出其子块 */
            i = block_end(sc, i);
        } else if (b->type == BL_IF) {
            fprintf(f, "    if ");
            str_esc(f, b->cond[0] ? b->cond : L"1");
            fprintf(f, " {\n");
            emit_blocks(f, sc, i + 1, d);
            fprintf(f, "    }\n");
            i = block_end(sc, i);
        } else {
            emit_normal(f, sc, b);
            i++;
        }
    }
}

static int write_scene_dex(const char *dp) {
    if (P.nScenes == 0) return 0;
    FILE *f = fopen(dp, "w"); if (!f) return 0;
    fprintf(f, "include \"gal\";\n\n");
    fprintf(f, "func wait_click() {\n  while gal_running() {\n    gal_poll();\n    if gal_text_done() && gal_clicked() { return; }\n    gal_wait(16);\n  }\n}\n");
    fprintf(f, "func wait_pick() {\n  while gal_running() {\n    gal_poll();\n    if gal_picked() >= 0 { return; }\n    gal_wait(16);\n  }\n}\n\n");
    fprintf(f, "gal_init(960, 540);\n");
    char b[520];
    w_to_utf8(P.title, b, sizeof b);
    fprintf(f, "gal_set_title(\"");
    for (char *p = b; *p; p++) { if (*p == '"' || *p == '\\') fputc('\\', f); fputc(*p, f); }
    fprintf(f, "\");\ngal_box(1);\n\n");

    for (int s = 0; s < P.nScenes; s++) {
        Scene *sc = &P.scenes[s];
        char tbuf[160]; w_to_utf8(sc->title, tbuf, sizeof tbuf);
        fprintf(f, "// ===== 镜头 %d: %s =====\n", s + 1, tbuf);
        fprintf(f, "func scene_%d() {\n", s);
        w_to_utf8(sc->bg, b, sizeof b);
        if (sc->useBgImg && sc->bg[0]) {
            fprintf(f, "  gal_bg(\""); str_esc(f, sc->bg); fprintf(f, "\");\n");
        } else {
            fprintf(f, "  gal_bg_color(0x%06X);\n", sc->bgColor);
        }
        for (int k = 0; k < MAX_SPR; k++) {
            if (sc->spr[k][0]) {
                fprintf(f, "  gal_sprite(%d, \"", k); str_esc(f, sc->spr[k]);
                fprintf(f, "\");\n  gal_sprite_pos_mode(%d, 0);\n  gal_sprite_scale(%d, 100);\n  gal_sprite_show(%d, 0);\n", k, k, k);
            }
        }
        emit_blocks(f, sc, 0, -1);
        /* 镜头结束 */
        if (s + 1 < P.nScenes) fprintf(f, "  scene_%d();\n", s + 1);
        else { fprintf(f, "  gal_close();\n  return;\n"); }
        fprintf(f, "}\n\n");
    }
    fprintf(f, "scene_0();\nprint \"end\";\n");
    fclose(f);
    return 1;
}

static void export_game(void) {
    /* 导出完整的可编译 DexLang 代码(scene_out.dex),不打包 */
    if (P.nScenes == 0) return;
    if (!g_projRoot[0]) {
        MessageBoxW(g_mainWnd, L"项目根目录未设置。请先打开「设置」配置。", L"GAL 编辑器", MB_OK);
        return;
    }
    wchar_t dexPath[1200]; swprintf(dexPath, 1200, L"%s\\scene_out.dex", g_projRoot);
    char dp[1200]; w_to_utf8(dexPath, dp, sizeof dp);
    if (!write_scene_dex(dp)) {
        MessageBoxW(g_mainWnd, L"导出代码失败。", L"GAL 编辑器", MB_OK);
        return;
    }
    swprintf(g_statusMsg, 160, L"✅ 已导出可编译代码: %s\\scene_out.dex(编译: python main.py compile scene_out.dex)", g_projRoot);
}

/* ---------- 像素缓冲与基础绘制 ---------- */
static unsigned char *g_bits = NULL;
static int g_winW = 0, g_winH = 0, g_stride = 0;

static inline void fb_set(int x, int y, unsigned r, unsigned g, unsigned b) {
    if (x < 0 || y < 0 || x >= g_winW || y >= g_winH) return;
    unsigned char *p = g_bits + (size_t)y * g_stride + (size_t)x * 4;
    p[0] = (unsigned char)b; p[1] = (unsigned char)g; p[2] = (unsigned char)r; p[3] = 255;
}
static void fb_fill(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            fb_set(i, j, r, g, b);
}
static void fb_border(int x, int y, int w, int h, int t, unsigned r, unsigned g, unsigned b) {
    for (int k = 0; k < t; k++) {
        for (int i = x; i < x + w; i++) { fb_set(i, y + k, r, g, b); fb_set(i, y + h - 1 - k, r, g, b); }
        for (int j = y; j < y + h; j++) { fb_set(x + k, j, r, g, b); fb_set(x + w - 1 - k, j, r, g, b); }
    }
}
static void fb_fill_circle(int cx, int cy, int r, unsigned cr, unsigned cg, unsigned cb) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r) fb_set(cx + i, cy + j, cr, cg, cb);
}

/* ---------- 文本渲染 ---------- */
static HFONT g_fonts[2][40];
static HFONT get_font(int size, bool bold) {
    int bi = bold ? 1 : 0;
    if (size < 0) size = 0; if (size >= 40) size = 39;
    if (g_fonts[bi][size]) return g_fonts[bi][size];
    g_fonts[bi][size] = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
                                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    return g_fonts[bi][size];
}

static void fb_text(int x, int y, const wchar_t *s, int size, unsigned cr, unsigned cg, unsigned cb, bool bold) {
    if (!g_bits || !s || !s[0]) return;
    int n = (int)wcslen(s);
    if (n > 1023) n = 1023;
    HDC dc = CreateCompatibleDC(NULL);
    HFONT hf = get_font(size, bold);
    HFONT ohf = (HFONT)SelectObject(dc, hf);
    SIZE sz; GetTextExtentPoint32W(dc, s, n, &sz);
    int dw = sz.cx + 8, dh = sz.cy + 8;
    if (dw < 8) dw = 8; if (dh < 8) dh = 8;
    if (dw > 2048) dw = 2048; if (dh > 2048) dh = 2048;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *tbits = NULL;
    HBITMAP tbmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &tbits, NULL, 0);
    if (!tbmp) { SelectObject(dc, ohf); DeleteDC(dc); return; }
    HBITMAP otb = (HBITMAP)SelectObject(dc, tbmp);
    RECT rc2 = { 4, 4, dw - 4, dh - 4 };
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, n, &rc2, DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE);
    for (int j = 0; j < dh; j++) {
        for (int i = 0; i < dw; i++) {
            unsigned char *tp = (unsigned char*)tbits + ((size_t)j * dw + i) * 4;
            if (tp[0] || tp[1] || tp[2])
                fb_set(x + i - 4, y + j - 4, cr, cg, cb);
        }
    }
    SelectObject(dc, otb); SelectObject(dc, ohf);
    DeleteObject(tbmp); DeleteDC(dc);
}

static void fb_text_center(int cx, int y, const wchar_t *s, int size, unsigned cr, unsigned cg, unsigned cb, bool bold) {
    HDC dc = CreateCompatibleDC(NULL);
    HFONT hf = get_font(size, bold);
    HFONT ohf = (HFONT)SelectObject(dc, hf);
    SIZE sz; GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, ohf); DeleteDC(dc);
    fb_text(cx - sz.cx / 2, y, s, size, cr, cg, cb, bold);
}

/* ---------- 输入状态 ---------- */
static int g_mx = 0, g_my = 0;
static bool g_btnL = false, g_clicked = false;
static wchar_t g_chars[64]; static int g_nchars = 0;
static bool g_keys[256] = { false };
static int g_frameTick = 0;
static int g_wheel = 0;

/* ---------- 立即模式 UI 控件 ---------- */
static int g_focusInput = -1;
static int g_focusSlider = -1;
static int g_dropOpen = -1;
static int g_dropX = 0, g_dropY = 0, g_dropW = 0, g_dropH = 0;
static int g_dropBtnX = 0, g_dropBtnY = 0, g_dropBtnW = 0, g_dropBtnH = 0;

static bool in_rect(int x, int y, int w, int h) {
    return g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
}

static bool ui_button(int x, int y, int w, int h, const wchar_t *text) {
    bool hover = in_rect(x, y, w, h);
    bool down = hover && g_btnL;
    unsigned r = 49, g = 50, b = 68;
    if (hover) { r = 69; g = 71; b = 90; }
    if (down) { r = 88; g = 91; b = 112; }
    fb_fill(x, y, w, h, r, g, b);
    fb_border(x, y, w, h, 1, 60, 62, 80);
    fb_text_center(x + w / 2, y + (h - 18) / 2, text, 18, 205, 214, 244, false);
    return hover && g_clicked;
}

static bool ui_input(int id, int x, int y, int w, int h, wchar_t *buf, int cap) {
    bool hover = in_rect(x, y, w, h);
    if (g_clicked) {
        if (hover) g_focusInput = id;
        else if (g_focusInput == id) g_focusInput = -1;
    }
    bool focused = (g_focusInput == id);
    fb_fill(x, y, w, h, 45, 48, 62);
    fb_border(x, y, w, h, 1, focused ? 137 : 50, focused ? 180 : 52, focused ? 250 : 66);
    fb_text(x + 6, y + (h - 18) / 2, buf, 18, 205, 214, 244, false);
    if (focused && ((g_frameTick / 12) % 2) == 0) {
        int tx = x + 6;
        HDC dc = CreateCompatibleDC(NULL);
        HFONT hf = get_font(18, false);
        HFONT ohf = (HFONT)SelectObject(dc, hf);
        SIZE sz; GetTextExtentPoint32W(dc, buf, (int)wcslen(buf), &sz);
        SelectObject(dc, ohf); DeleteDC(dc);
        tx += sz.cx;
        fb_fill(tx, y + 3, 2, h - 6, 205, 214, 244);
    }
    if (focused) {
        int len = (int)wcslen(buf);
        for (int i = 0; i < g_nchars; i++) {
            if (g_chars[i] >= 0x20 && len < cap - 1) buf[len++] = g_chars[i];
        }
        if (g_keys[VK_BACK] && len > 0) buf[--len] = 0;
        buf[len] = 0;
    }
    return focused;
}

static void ui_text(int x, int y, const wchar_t *s, unsigned cr, unsigned cg, unsigned cb, bool bold) {
    fb_text(x, y, s, 18, cr, cg, cb, bold);
}

static bool ui_checkbox(int x, int y, int w, int h, const wchar_t *text, int *val) {
    bool hover = in_rect(x, y, w, h);
    bool clicked = hover && g_clicked;
    fb_fill(x, y, 16, 16, *val ? 88 : 45, *val ? 91 : 48, *val ? 112 : 62);
    fb_border(x, y, 16, 16, 1, 60, 62, 80);
    if (*val) {
        fb_set(x + 4, y + 8, 205, 214, 244);
        fb_set(x + 6, y + 10, 205, 214, 244);
        fb_set(x + 10, y + 4, 205, 214, 244);
        fb_set(x + 8, y + 8, 205, 214, 244);
        fb_set(x + 5, y + 9, 205, 214, 244);
        fb_set(x + 9, y + 5, 205, 214, 244);
    }
    if (clicked) *val = !*val;
    fb_text(x + 22, y - 1, text, 18, 205, 214, 244, false);
    return clicked;
}

/* 下拉选择;返回 true 表示本帧有选中 */
static bool ui_dropdown(int id, int x, int y, int w, int h,
                        const wchar_t *const *items, int n, int *val) {
    bool hover = in_rect(x, y, w, h);
    fb_fill(x, y, w, h, 45, 48, 62);
    fb_border(x, y, w, h, 1, hover ? 137 : 50, hover ? 180 : 52, hover ? 250 : 66);
    if (*val >= 0 && *val < n) fb_text(x + 6, y + (h - 18) / 2, items[*val], 18, 205, 214, 244, false);
    fb_text(x + w - 16, y + (h - 18) / 2, L"▼", 15, 148, 154, 176, false);
    bool changed = false;
    int ph = n * 22 + 4;
    int py = y + h + 2;
    if (py + ph > g_winH) py = y - ph - 2;
    if (py < 0) py = 0;
    if (g_clicked && hover) {
        g_dropBtnX = x; g_dropBtnY = y; g_dropBtnW = w; g_dropBtnH = h;
        g_dropX = x; g_dropY = py; g_dropW = w; g_dropH = ph;
        g_dropOpen = (g_dropOpen == id) ? -1 : id;
    }
    if (g_dropOpen == id) {
        fb_fill(x, py, w, ph, 40, 42, 54);
        fb_border(x, py, w, ph, 1, 90, 92, 110);
        for (int i = 0; i < n; i++) {
            bool oh = in_rect(x, py + 2 + i * 22, w, 22);
            fb_fill(x, py + 2 + i * 22, w, 22,
                    (i == *val) ? 88 : (oh ? 60 : 40),
                    (i == *val) ? 91 : (oh ? 62 : 42),
                    (i == *val) ? 112 : (oh ? 80 : 54));
            fb_text(x + 6, py + 2 + i * 22, items[i], 16, 205, 214, 244, false);
            if (g_clicked && oh) { *val = i; g_dropOpen = -1; changed = true; }
        }
    }
    return changed;
}

/* 垂直滚动条 */
static int ui_vscroll(int x, int y, int h, int contentH, int viewH, int scroll, bool active) {
    int maxScroll = contentH - viewH;
    if (maxScroll < 0) maxScroll = 0;
    if (scroll > maxScroll) scroll = maxScroll;
    if (scroll < 0) scroll = 0;
    if (maxScroll > 0) {
        int trackH = h;
        int thumbH = trackH * viewH / contentH;
        if (thumbH < 24) thumbH = 24;
        if (thumbH > trackH) thumbH = trackH;
        float t = (float)scroll / maxScroll;
        int ty = y + (int)(t * (trackH - thumbH));
        fb_fill(x, y, 8, trackH, 36, 38, 50);
        bool overThumb = in_rect(x, ty, 8, thumbH);
        if (g_clicked && overThumb) g_focusSlider = 10000 + x;
        if (g_btnL && g_focusSlider == 10000 + x) {
            int ny = g_my - thumbH / 2;
            float nt = (float)(ny - y) / (trackH - thumbH);
            if (nt < 0) nt = 0; if (nt > 1) nt = 1;
            scroll = (int)(nt * maxScroll);
        }
        if (!g_btnL && g_focusSlider == 10000 + x) g_focusSlider = -1;
        fb_fill(x, ty, 8, thumbH, 137, 180, 250);
    }
    if (active && g_wheel != 0) {
        scroll -= g_wheel / 120 * 48;
        if (scroll < 0) scroll = 0;
        if (scroll > maxScroll) scroll = maxScroll;
    }
    return scroll;
}

/* ---------- 前向声明 ---------- */
static void cmd_new(void) NOINLINE;
static void cmd_open(void) NOINLINE;
static void cmd_save(void) NOINLINE;
static void export_game(void);
static void start_preview(void) NOINLINE;
static void stop_preview(void) NOINLINE;
static void tick_preview(void) NOINLINE;
static void ui_settings(void) NOINLINE;
static void load_settings(void) NOINLINE;
static void save_settings(void) NOINLINE;
static void ui_block_canvas(void) NOINLINE;
static void ui_block_palette(void) NOINLINE;
static void ui_scene_props(void) NOINLINE;
static void ui_block_props(void) NOINLINE;
static void ui_char_props(void) NOINLINE;
static void ui_picker(void) NOINLINE;
static void ui_char_picker(void) NOINLINE;
static bool ui_selectable2(int x, int y, int w, int h, const wchar_t *text, bool selected);

/* ---------- 布局状态 ---------- */
/* 预览(解释执行) */
static int g_previewMode = 0;
static int g_pvScene = 0, g_pvIdx = 0, g_pvSkip = 0, g_pvSel = 0;
static int g_pvState = 0;         /* 0空闲 1等点击 2等毫秒 3等选项 */
static DWORD g_pvStart = 0;
static int g_pvMs = 0;
static int g_pvOver = 0;
static int g_dbging = 0;
static int g_tabSel = 0;          /* 右面板:0镜头 1块 2人物 */
static int g_pickMode = 0, g_pickTarget = 0;   /* 0=背景 1-4=镜头立绘 100+=人物状态 */
static int g_pickChar = -1;       /* 人物状态选择器目标人物 */
static int g_pickScroll = 0;
static HBITMAP g_pickThumbs[1024];
static wchar_t g_pickNames[1024][560];
static int g_pickCount = 0;
static int g_blockScroll = 0;     /* 块画布滚动 */
static int g_propScroll = 0;      /* 属性面板滚动 */
static int g_palScroll = 0;       /* 块面板滚动 */
static int g_charScroll = 0;
/* 拖拽 */
static int g_dragIdx = -1;        /* 画布内拖拽的块 */
static int g_dragFromPanel = -1;  /* 从面板拖出的块类型 */
static int g_dragY = 0;           /* 拖拽起始鼠标 y */
static int g_pressBlock = -1;     /* 按下的块 */
static int g_pressY = 0;
static int g_dropLine = -1;       /* 拖拽插入指示:块索引 */
static int g_dropWhere = 0;       /* 0前 1后 2内 */
static bool g_pressing = false;

static HBITMAP load_thumb(const wchar_t *path, int tw, int th) {
    char p[1024]; w_to_utf8(path, p, sizeof p);
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(p, &w, &h, &n, 4);
    if (!px) return NULL;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (bmp && bits) {
        unsigned char *dst = (unsigned char*)bits;
        for (int y = 0; y < th; y++) {
            int sy = y * h / th;
            for (int x = 0; x < tw; x++) {
                int sx = x * w / tw;
                const unsigned char *s = px + (sy * w + sx) * 4;
                unsigned char *d = dst + (y * tw + x) * 4;
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
            }
        }
    }
    stbi_image_free(px);
    return bmp;
}

/* 扫描目录图片(背景/立绘) */
static void scan_picks(void) {
    for (int i = 0; i < g_pickCount; i++) if (g_pickThumbs[i]) { DeleteObject(g_pickThumbs[i]); g_pickThumbs[i] = NULL; }
    g_pickCount = 0;
    g_pickScroll = 0;
    const wchar_t *exts[] = {L"*.png", L"*.jpg", L"*.jpeg", L"*.bmp", L"*.gif"};
    wchar_t dir[520];
    Scene *sc = &P.scenes[P.cur];
    if (sc->bgDir[0]) wcscpy(dir, sc->bgDir);
    else if (P.resDir[0]) wcscpy(dir, P.resDir);
    else wcscpy(dir, L".");
    for (int e = 0; e < 5; e++) {
        wchar_t pat[560]; swprintf(pat, 560, L"%s\\%s", dir, exts[e]);
        WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW(pat, &fd);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (g_pickCount >= 1024) break;
            wcscpy(g_pickNames[g_pickCount], fd.cFileName);
            wchar_t full[560]; swprintf(full, 560, L"%s\\%s", dir, fd.cFileName);
            g_pickThumbs[g_pickCount] = load_thumb(full, 64, 64);
            g_pickCount++;
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
}

static void fb_image(int x, int y, int w, int h, HBITMAP bmp) {
    if (!bmp) return;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP ob = (HBITMAP)SelectObject(dc, bmp);
    BITMAP bm; GetObjectW(bmp, sizeof bm, &bm);
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0) { SelectObject(dc, ob); DeleteDC(dc); return; }
    for (int j = 0; j < h; j++) {
        int sy = j * bm.bmHeight / h;
        for (int i = 0; i < w; i++) {
            int sx = i * bm.bmWidth / w;
            COLORREF col = GetPixel(dc, sx, sy);
            fb_set(x + i, y + j, GetRValue(col), GetGValue(col), GetBValue(col));
        }
    }
    SelectObject(dc, ob); DeleteDC(dc);
}

/* ---------- 输入框 id ---------- */
enum {
    ID_SPK = 100, ID_TXT, ID_CHOICE_TXT, ID_OPTION_TXT,
    ID_BG = 130, ID_BGCOL, ID_SPRPATH,
    ID_CFILE = 140, ID_CCOND, ID_AUDIO_PATH, ID_WAITMS, ID_IFCOND,
    ID_SC_TITLE = 200, ID_BGDIR,
    ID_CH_NAME = 230, ID_ST_NAME0 = 240,
    ID_PAL = 260,
    ID_SET_ROOT = 300, ID_SET_PY
};

static wchar_t g_colbuf[16];

/* ---------- 块外观 ---------- */
static void block_color(int type, unsigned *cr, unsigned *cg, unsigned *cb) {
    switch (type) {
    case BL_SPEAK:   *cr = 90;  *cg = 140; *cb = 230; break;   /* 蓝 */
    case BL_BG:      *cr = 60;  *cg = 180; *cb = 180; break;   /* 青 */
    case BL_SPRITE:  *cr = 100; *cg = 190; *cb = 120; break;   /* 绿 */
    case BL_AUDIO:   *cr = 180; *cg = 120; *cb = 220; break;   /* 紫 */
    case BL_WAIT:    *cr = 150; *cg = 150; *cb = 165; break;   /* 灰 */
    case BL_CLEAR:   *cr = 130; *cg = 130; *cb = 140; break;   /* 灰 */
    case BL_CHOICE:  *cr = 240; *cg = 160; *cb = 80;  break;   /* 橙 */
    case BL_OPTION:  *cr = 250; *cg = 130; *cb = 60;  break;   /* 橙亮 */
    case BL_IF:      *cr = 230; *cg = 90;  *cb = 90;  break;   /* 红 */
    case BL_CODE:    *cr = 230; *cg = 70;  *cb = 180; break;   /* 洋红 */
    default:         *cr = 200; *cg = 70;  *cb = 70;  break;  /* 深红(结束) */
    }
}
static const wchar_t *block_name(int type) {
    switch (type) {
    case BL_SPEAK:  return L"说话";
    case BL_BG:     return L"背景";
    case BL_SPRITE: return L"立绘";
    case BL_AUDIO:  return L"音频";
    case BL_WAIT:   return L"等待";
    case BL_CLEAR:  return L"清空文本";
    case BL_CHOICE: return L"选项";
    case BL_OPTION: return L"选项项";
    case BL_IF:     return L"如果";
    case BL_CODE:   return L"代码";
    default:        return L"结束";
    }
}
/* 块摘要 */
static void block_label(Scene *sc, Block *b, wchar_t *out, int cap) {
    switch (b->type) {
    case BL_SPEAK:
        if (b->speaker[0]) swprintf(out, cap, L"%ls: %ls", b->speaker, b->text);
        else swprintf(out, cap, L"%ls", b->text);
        break;
    case BL_BG:
        if (b->useBgImg && b->bg[0]) swprintf(out, cap, L"%ls", b->bg);
        else swprintf(out, cap, L"颜色 0x%06X", b->bgColor);
        break;
    case BL_SPRITE: {
        wchar_t who[80] = L"(图片)";
        if (b->charIdx >= 0 && b->charIdx < P.nChars) {
            Character *ch = &P.chars[b->charIdx];
            if (b->stateIdx >= 0 && b->stateIdx < ch->nStates && ch->stateNames[b->stateIdx][0])
                swprintf(who, 80, L"%ls-%ls", ch->name, ch->stateNames[b->stateIdx]);
            else if (ch->name[0]) swprintf(who, 80, L"%ls", ch->name);
        }
        swprintf(out, cap, L"%ls %s", who, b->spr_show ? L"显示" : L"隐藏");
        break;
    }
    case BL_AUDIO:
        if (b->audio_type == 0) swprintf(out, cap, L"BGM: %ls", b->audio_path);
        else if (b->audio_type == 1) swprintf(out, cap, L"SE: %ls", b->audio_path);
        else if (b->audio_type == 2) swprintf(out, cap, L"停止 BGM");
        else swprintf(out, cap, L"音量 %d", b->volume);
        break;
    case BL_WAIT:
        if (b->wait_type == 1) swprintf(out, cap, L"等待 %dms", b->wait_ms);
        else swprintf(out, cap, L"等待点击");
        break;
    case BL_CLEAR: swprintf(out, cap, L"清空文本"); break;
    case BL_CHOICE: swprintf(out, cap, L"%ls", b->choice_text); break;
    case BL_OPTION: swprintf(out, cap, L"%ls", b->option_text); break;
    case BL_IF: swprintf(out, cap, L"条件: %ls", b->cond); break;
    case BL_CODE: swprintf(out, cap, L"%ls", b->code_file[0] ? b->code_file : L"(未选代码文件)"); break;
    default: swprintf(out, cap, L"结束本镜头"); break;
    }
    if (wcslen(out) > 26) out[26] = 0;
}

/* ---------- 目录浏览 ---------- */
static void browse_dir(wchar_t *out, int cap) {
    BROWSEINFOW bi; memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_mainWnd;
    bi.lpszTitle = L"选择背景/资源目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, out)) {}
        CoTaskMemFree(pidl);
    }
}

/* ---------- 块画布(中央,Scratch 式) ---------- */
static void ui_block_canvas(void) {
    Scene *sc = &P.scenes[P.cur];
    int px = 228, py = 204, pw = g_winW - 228 - 372, ph = g_winH - 204 - 30;
    if (pw < 60) pw = 60;
    if (ph < 60) ph = 60;
    fb_fill(px, py, pw, ph, 26, 28, 38);
    fb_border(px, py, pw, ph, 1, 50, 52, 66);
    ui_text(px + 8, py + 6, L"指令块程序(从上到下执行)", 148, 154, 176, true);

    /* 工具按钮:添加 常用块 */
    int by = py + 28;
    const struct { int t; const wchar_t *n; } quick[] = {
        {BL_SPEAK, L"+说话"}, {BL_SPRITE, L"+立绘"}, {BL_BG, L"+背景"},
        {BL_CHOICE, L"+选项"}, {BL_IF, L"+如果"}, {BL_END, L"+结束"}
    };
    int qw = (pw - 12 - 18) / 6;
    for (int i = 0; i < 6; i++) {
        if (ui_button(px + 4 + i * (qw + 3), by, qw, 22, quick[i].n)) {
            if (insert_block(sc, quick[i].t) >= 0) g_blockScroll = 0;
        }
    }
    by += 27;
    if (ui_button(px + 4, by, (pw - 12) / 3, 22, L"删除块") && sc->nBlocks > 1) {
        int i = sc->cur;
        int s = block_end(sc, i);
        if (i >= 0 && i < sc->nBlocks) {
            memmove(sc->blocks + i, sc->blocks + s, sizeof(Block) * (sc->nBlocks - s));
            sc->nBlocks -= (s - i);
            if (sc->cur >= sc->nBlocks) sc->cur = sc->nBlocks - 1;
        }
    }
    if (ui_button(px + 6 + (pw - 12) / 3, by, (pw - 12) / 3, 22, L"上移")) {
        int p = prev_sibling(sc, sc->cur);
        if (p >= 0) block_move(sc, sc->cur, p, 0);
    }
    if (ui_button(px + 8 + 2 * (pw - 12) / 3, by, (pw - 12) / 3, 22, L"下移")) {
        int n = next_sibling(sc, sc->cur);
        if (n >= 0) block_move(sc, sc->cur, n, 1);
    }

    /* 块列表(深度优先,缩进) */
    int ly = by + 28;
    int viewH = py + ph - ly - 8;
    int rowH = 30;
    int contentH = 0;
    for (int i = 0; i < sc->nBlocks; i++) contentH += rowH;
    int sx = px + pw - 16;
    g_blockScroll = ui_vscroll(sx, ly, viewH, contentH, viewH, g_blockScroll, in_rect(px, py, pw, ph));

    /* 拖拽中计算插入位置 */
    g_dropLine = -1; g_dropWhere = 0;
    if ((g_dragIdx >= 0 || g_dragFromPanel >= 0) && g_btnL) {
        int hover = -1;
        for (int i = 0; i < sc->nBlocks; i++) {
            int ry = ly - g_blockScroll + i * rowH;
            if (g_my >= ry && g_my < ry + rowH) { hover = i; break; }
        }
        if (hover >= 0) {
            int ry = ly - g_blockScroll + hover * rowH;
            int mid = ry + rowH / 2;
            int is_c = (sc->blocks[hover].type == BL_CHOICE || sc->blocks[hover].type == BL_IF);
            if (g_my < ry + 6) { g_dropLine = hover; g_dropWhere = 0; }
            else if (g_my > ry + rowH - 6) { g_dropLine = hover; g_dropWhere = 1; }
            else if (is_c) { g_dropLine = hover; g_dropWhere = 2; }
        }
    }

    for (int i = 0; i < sc->nBlocks; i++) {
        Block *b = &sc->blocks[i];
        int ry = ly - g_blockScroll + i * rowH;
        if (ry < ly - rowH || ry > ly + viewH) continue;
        int indent = b->depth * 14;
        bool sel = (i == sc->cur);
        bool hover = in_rect(px + 4 + indent, ry, pw - 30 - indent, rowH - 2);
        unsigned cr, cg, cb; block_color(b->type, &cr, &cg, &cb);
        /* 块体 */
        unsigned br = 45, bg = 47, bb = 62;
        if (b->type == BL_CHOICE || b->type == BL_IF || b->type == BL_OPTION) { br = 55; bg = 52; bb = 62; }
        if (sel) { br = 60; bg = 62; bb = 80; }
        else if (hover) { br = 49; bg = 50; bb = 68; }
        fb_fill(px + 4 + indent, ry, pw - 30 - indent, rowH - 2, br, bg, bb);
        fb_border(px + 4 + indent, ry, pw - 30 - indent, rowH - 2, 1, cr, cg, cb);
        /* 类型色条 */
        fb_fill(px + 4 + indent, ry, 6, rowH - 2, cr, cg, cb);
        /* C 形标记 */
        if (b->type == BL_CHOICE || b->type == BL_IF) fb_text(px + 14 + indent, ry + 1, L"▸", 18, cr, cg, cb, false);
        if (b->type == BL_OPTION) fb_text(px + 14 + indent, ry + 1, L"•", 18, cr, cg, cb, false);
        /* 类型名 + 摘要 */
        fb_text(px + 30 + indent, ry + 1, block_name(b->type), 16, cr, cg, cb, true);
        wchar_t lb[64]; block_label(sc, b, lb, 64);
        fb_text(px + 86 + indent, ry + 1, lb, 15, 205, 214, 244, false);
        /* 点击选中 */
        if (g_clicked && hover) {
            sc->cur = i;
            g_pressing = true; g_pressBlock = i; g_pressY = g_my; g_dragY = g_my;
        }
    }
    /* 拖拽中的插入线 */
    if ((g_dragIdx >= 0 || g_dragFromPanel >= 0) && g_btnL && g_dropLine >= 0) {
        int ry = ly - g_blockScroll + g_dropLine * rowH;
        if (g_dropWhere == 0) fb_fill(px + 4, ry - 2, pw - 30, 3, 255, 255, 255);
        else if (g_dropWhere == 1) fb_fill(px + 4, ry + rowH, pw - 30, 3, 255, 255, 255);
        else fb_border(px + 4, ry, pw - 30, rowH, 2, 255, 255, 120);
    }
}

/* ---------- 块面板(左) ---------- */
static void ui_block_palette(void) {
    int px = 4, py = 204, pw = 220, ph = g_winH - 204 - 30;
    if (ph < 60) ph = 60;
    fb_fill(px, py, pw, ph, 30, 32, 44);
    fb_border(px, py, pw, ph, 1, 50, 52, 66);
    ui_text(px + 8, py + 6, L"指令块(点击添加到程序)", 148, 154, 176, true);
    /* 分类块(硬编码列表) */
    static const int pal[] = {
        BL_SPEAK, BL_SPRITE, BL_BG, BL_AUDIO, BL_WAIT, BL_CLEAR,
        BL_CHOICE, BL_OPTION, BL_IF, BL_CODE, BL_END
    };
    static const wchar_t *palDesc[] = {
        L"显示一句对话", L"显示/隐藏立绘", L"切换背景", L"播放 BGM/SE/音量",
        L"等待点击或毫秒", L"清空文本", L"玩家选项(嵌套选项项)",
        L"一个选项分支", L"条件执行子块", L"运行代码文件(带条件)", L"结束本镜头"
    };
    int ly = py + 28;
    int viewH = ph - 34;
    int rowH = 30;
    int contentH = (int)(sizeof(pal) / sizeof(pal[0])) * rowH;
    int sx = px + pw - 16;
    g_palScroll = ui_vscroll(sx, ly, viewH, contentH, viewH, g_palScroll, in_rect(px, py, pw, ph));
    int n = (int)(sizeof(pal) / sizeof(pal[0]));
    Scene *sc = &P.scenes[P.cur];
    for (int i = 0; i < n; i++) {
        int ry = ly - g_palScroll + i * rowH;
        if (ry < ly - rowH || ry > ly + viewH) continue;
        int t = pal[i];
        unsigned cr, cg, cb; block_color(t, &cr, &cg, &cb);
        bool hover = in_rect(px + 4, ry, pw - 26, rowH - 2);
        fb_fill(px + 4, ry, pw - 26, rowH - 2, hover ? 49 : 40, hover ? 50 : 42, hover ? 68 : 54);
        fb_fill(px + 4, ry, 6, rowH - 2, cr, cg, cb);
        fb_text(px + 16, ry + 1, block_name(t), 16, cr, cg, cb, true);
        fb_text(px + 16, ry + 17, palDesc[i], 12, 148, 154, 176, false);
        if (g_clicked && hover) {
            if (insert_block(sc, t) >= 0) { g_blockScroll = 0; }
        }
    }
    fb_text(px + 8, py + ph - 22, L"提示:点面板块插入;画布内拖动块可排序/嵌套", 11, 110, 118, 140, false);
}

/* ---------- 属性:镜头 ---------- */
static const wchar_t *g_posNames[] = {L"中间", L"左", L"右", L"偏左", L"偏右"};
static const wchar_t *g_animNames[] = {L"无", L"呼吸", L"淡入", L"上浮", L"抖动", L"脉冲", L"消失"};
static const wchar_t *g_layerNames[] = {L"图层 1", L"图层 2", L"图层 3", L"图层 4"};

static void ui_scene_props(void) {
    Scene *sc = &P.scenes[P.cur];
    int x = g_winW - 368, y = 66, w = 364, ph = g_winH - 44 - 30 - 26;
    if (ph < 60) ph = 60;
    fb_fill(x, y, w, ph, 30, 32, 44);
    fb_border(x, y, w, ph, 1, 50, 52, 66);
    int viewH = ph - 8;
    int contentH = 520;
    int sx = x + w - 16;
    g_propScroll = ui_vscroll(sx, y + 4, viewH, contentH, viewH, g_propScroll, in_rect(x, y, w, ph));
    int cy = y + 10 - g_propScroll;
    ui_text(x + 10, cy, L"镜头属性", 148, 154, 176, true); cy += 24;
    ui_text(x + 10, cy, L"标题", 148, 154, 176, false); cy += 20;
    ui_input(ID_SC_TITLE, x + 10, cy, w - 24, 24, sc->title, 128); cy += 32;
    ui_text(x + 10, cy, L"背景目录", 148, 154, 176, false); cy += 20;
    ui_input(ID_BGDIR, x + 10, cy, w - 82, 24, sc->bgDir, 520); cy += 30;
    if (ui_button(x + w - 66, cy - 24, 56, 22, L"浏览")) { browse_dir(sc->bgDir, 520); }
    ui_text(x + 10, cy, L"背景图", 148, 154, 176, false); cy += 20;
    ui_input(ID_BG, x + 10, cy, w - 110, 24, sc->bg, 520);
    if (ui_button(x + w - 96, cy, 84, 24, L"选择图片")) { g_pickTarget = 0; g_pickMode = 1; scan_picks(); }
    cy += 30;
    ui_checkbox(x + 10, cy + 2, 16, 16, L"用图片", &sc->useBgImg);
    ui_text(x + 140, cy + 1, L"颜色:", 148, 154, 176, false);
    if (g_focusInput != ID_BGCOL) swprintf(g_colbuf, 16, L"0x%06X", sc->bgColor);
    ui_input(ID_BGCOL, x + 190, cy, 120, 24, g_colbuf, 15);
    if (g_focusInput != ID_BGCOL) { unsigned v; if (swscanf(g_colbuf, L"%x", &v) == 1) sc->bgColor = (int)v; }
    cy += 34;
    ui_text(x + 10, cy, L"镜头级立绘资源(兼容)", 148, 154, 176, true); cy += 24;
    for (int k = 0; k < MAX_SPR; k++) {
        wchar_t t[64]; swprintf(t, 64, L"图层 %d", k + 1);
        ui_text(x + 10, cy, t, 148, 154, 176, false); cy += 20;
        ui_input(ID_BGCOL + 10 + k, x + 10, cy, w - 96, 22, sc->spr[k], 520);
        if (ui_button(x + w - 82, cy, 70, 22, L"选择")) { g_pickTarget = k + 1; g_pickMode = 1; scan_picks(); }
        cy += 30;
    }
}

/* ---------- 属性:块(按类型) ---------- */
static void ui_block_props(void) {
    Scene *sc = &P.scenes[P.cur];
    Block *b = &sc->blocks[sc->cur];
    int x = g_winW - 368, y = 66, w = 364, ph = g_winH - 44 - 30 - 26;
    if (ph < 60) ph = 60;
    fb_fill(x, y, w, ph, 30, 32, 44);
    fb_border(x, y, w, ph, 1, 50, 52, 66);
    int viewH = ph - 8;
    int contentH = 620;
    int sx = x + w - 16;
    g_propScroll = ui_vscroll(sx, y + 4, viewH, contentH, viewH, g_propScroll, in_rect(x, y, w, ph));
    int cy = y + 10 - g_propScroll;
    wchar_t hd[96]; swprintf(hd, 96, L"块 %d · %s", sc->cur + 1, block_name(b->type));
    ui_text(x + 10, cy, hd, 148, 154, 176, true); cy += 24;
    switch (b->type) {
    case BL_SPEAK:
        ui_text(x + 10, cy, L"说话人", 148, 154, 176, false); cy += 20;
        ui_input(ID_SPK, x + 10, cy, w - 24, 24, b->speaker, 128); cy += 32;
        ui_text(x + 10, cy, L"文本", 148, 154, 176, false); cy += 20;
        ui_input(ID_TXT, x + 10, cy, w - 24, 70, b->text, 2048); cy += 76;
        ui_checkbox(x + 10, cy, 16, 16, L"有头像对话框", &b->style_avatar); cy += 24;
        ui_checkbox(x + 10, cy, 16, 16, L"显示人名", &b->style_name); cy += 24;
        ui_text(x + 10, cy, L"文字位置", 148, 154, 176, false); cy += 20;
        ui_dropdown(2, x + 10, cy, w - 24, 24, g_posNames, 5, &b->text_pos); cy += 32;
        ui_checkbox(x + 10, cy, 16, 16, L"对话框自适应", &b->auto_fit); cy += 24;
        break;
    case BL_BG:
        ui_text(x + 10, cy, L"背景图", 148, 154, 176, false); cy += 20;
        ui_input(ID_BG, x + 10, cy, w - 110, 24, b->bg, 520);
        if (ui_button(x + w - 96, cy, 84, 24, L"选择")) { g_pickTarget = 0; g_pickMode = 1; scan_picks(); }
        cy += 30;
        ui_checkbox(x + 10, cy + 2, 16, 16, L"用图片", &b->useBgImg);
        ui_text(x + 140, cy + 1, L"颜色:", 148, 154, 176, false);
        if (g_focusInput != ID_BGCOL) swprintf(g_colbuf, 16, L"0x%06X", b->bgColor);
        ui_input(ID_BGCOL, x + 190, cy, 120, 24, g_colbuf, 15);
        if (g_focusInput != ID_BGCOL) { unsigned v; if (swscanf(g_colbuf, L"%x", &v) == 1) b->bgColor = (int)v; }
        cy += 34;
        break;
    case BL_SPRITE: {
        /* 人物 + 状态 */
        ui_text(x + 10, cy, L"人物", 148, 154, 176, false); cy += 20;
        if (P.nChars > 0) {
            static const wchar_t *cdummy[32];
            wchar_t names[MAX_CHARS][80];
            int ci = b->charIdx < 0 ? 0 : b->charIdx;
            if (ci >= P.nChars) ci = 0;
            for (int c = 0; c < P.nChars; c++) {
                swprintf(names[c], 80, L"%ls", P.chars[c].name[0] ? P.chars[c].name : L"(无名)");
                cdummy[c] = names[c];
            }
            if (ui_dropdown(3, x + 10, cy, w - 24, 24, cdummy, P.nChars, &ci)) {
                b->charIdx = ci; b->stateIdx = 0;
                if (P.chars[ci].nStates > 0) b->stateIdx = P.chars[ci].cur;
            } else if (!g_dropOpen) { b->charIdx = ci; }
            cy += 32;
        } else {
            ui_text(x + 10, cy, L"尚未建立人物 → 切到「人物」页创建", 200, 120, 100, false); cy += 24;
        }
        if (b->charIdx >= 0 && b->charIdx < P.nChars) {
            Character *ch = &P.chars[b->charIdx];
            if (ch->nStates > 0) {
                static const wchar_t *sdummy[16];
                wchar_t snames[MAX_STATES][40];
                int si = b->stateIdx;
                if (si < 0 || si >= ch->nStates) si = 0;
                for (int s = 0; s < ch->nStates; s++) {
                    swprintf(snames[s], 40, L"%ls", ch->stateNames[s][0] ? ch->stateNames[s] : L"状态");
                    sdummy[s] = snames[s];
                }
                ui_text(x + 10, cy, L"状态", 148, 154, 176, false); cy += 20;
                if (ui_dropdown(4, x + 10, cy, w - 24, 24, sdummy, ch->nStates, &si)) b->stateIdx = si;
                else if (!g_dropOpen) b->stateIdx = si;
                cy += 32;
            } else {
                ui_text(x + 10, cy, L"该人物尚无状态图", 200, 120, 100, false); cy += 22;
            }
        } else {
            ui_text(x + 10, cy, L"直接图片路径(或选人物)", 148, 154, 176, false); cy += 20;
            ui_input(ID_SPRPATH, x + 10, cy, w - 96, 24, b->sprPath, 520);
            if (ui_button(x + w - 82, cy, 70, 24, L"选择")) { g_pickTarget = 1; g_pickMode = 1; scan_picks(); }
            cy += 32;
        }
        ui_text(x + 10, cy, L"图层", 148, 154, 176, false); cy += 20;
        ui_dropdown(5, x + 10, cy, w - 24, 24, g_layerNames, 4, &b->spr_layer); cy += 32;
        ui_checkbox(x + 10, cy, 16, 16, L"显示人物图片", &b->spr_show); cy += 24;
        ui_text(x + 10, cy, L"人物位置", 148, 154, 176, false); cy += 20;
        ui_dropdown(6, x + 10, cy, w - 24, 24, g_posNames, 5, &b->spr_pos); cy += 32;
        ui_checkbox(x + 10, cy, 16, 16, L"人物自适应", &b->spr_autofit); cy += 24;
        ui_text(x + 10, cy, L"动画", 148, 154, 176, false); cy += 20;
        ui_dropdown(7, x + 10, cy, w - 24, 24, g_animNames, 7, &b->spr_anim); cy += 32;
        break;
    }
    case BL_AUDIO:
        ui_text(x + 10, cy, L"类型", 148, 154, 176, false); cy += 20;
        {
            static const wchar_t *anames[] = {L"播放 BGM", L"播放 SE", L"停止 BGM", L"设置音量"};
            ui_dropdown(8, x + 10, cy, w - 24, 24, anames, 4, &b->audio_type); cy += 32;
        }
        if (b->audio_type == 0 || b->audio_type == 1) {
            ui_text(x + 10, cy, L"音频文件", 148, 154, 176, false); cy += 20;
            ui_input(ID_AUDIO_PATH, x + 10, cy, w - 82, 24, b->audio_path, 520);
            if (ui_button(x + w - 66, cy, 56, 24, L"浏览")) {
                wchar_t path[1024] = {0};
                OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
                ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_mainWnd;
                ofn.lpstrFilter = L"音频 (*.mp3;*.wav)\0*.mp3;*.wav\0All\0*.*\0";
                ofn.lpstrFile = path; ofn.nMaxFile = 1024;
                ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) wcscpy(b->audio_path, path);
            }
            cy += 30;
        }
        if (b->audio_type == 3) {
            ui_text(x + 10, cy, L"音量(0-100)", 148, 154, 176, false); cy += 20;
            {
                wchar_t vb[16]; swprintf(vb, 16, L"%d", b->volume);
                if (g_focusInput != ID_WAITMS + 1) { /* 复用无冲突 id */ }
                ui_input(ID_WAITMS + 1, x + 10, cy, w - 24, 24, vb, 12);
                b->volume = _wtoi(vb);
            }
            cy += 32;
        }
        break;
    case BL_WAIT:
        ui_text(x + 10, cy, L"方式", 148, 154, 176, false); cy += 20;
        {
            static const wchar_t *wnames[] = {L"等待点击", L"等待毫秒"};
            ui_dropdown(9, x + 10, cy, w - 24, 24, wnames, 2, &b->wait_type); cy += 32;
        }
        if (b->wait_type == 1) {
            ui_text(x + 10, cy, L"毫秒", 148, 154, 176, false); cy += 20;
            wchar_t mb[16]; swprintf(mb, 16, L"%d", b->wait_ms);
            ui_input(ID_WAITMS, x + 10, cy, w - 24, 24, mb, 12);
            b->wait_ms = _wtoi(mb);
            cy += 32;
        }
        break;
    case BL_CLEAR:
        ui_text(x + 10, cy, L"清空当前文本与说话人", 148, 154, 176, false); cy += 24;
        break;
    case BL_CHOICE:
        ui_text(x + 10, cy, L"说话人", 148, 154, 176, false); cy += 20;
        ui_input(ID_SPK, x + 10, cy, w - 24, 24, b->speaker, 128); cy += 32;
        ui_text(x + 10, cy, L"问题文本", 148, 154, 176, false); cy += 20;
        ui_input(ID_CHOICE_TXT, x + 10, cy, w - 24, 46, b->choice_text, 2048); cy += 52;
        ui_text(x + 10, cy, L"在下方用「+选项」添加选项项块,每个选项项内放子指令", 140, 140, 160, false); cy += 24;
        break;
    case BL_OPTION:
        ui_text(x + 10, cy, L"选项文本", 148, 154, 176, false); cy += 20;
        ui_input(ID_OPTION_TXT, x + 10, cy, w - 24, 24, b->option_text, 256); cy += 32;
        ui_text(x + 10, cy, L"该选项执行什么:选中它,再用顶部按钮/面板在其后添加子块", 140, 140, 160, false); cy += 24;
        break;
    case BL_IF:
        ui_text(x + 10, cy, L"条件(表达式,例: hp > 50)", 148, 154, 176, false); cy += 20;
        ui_input(ID_IFCOND, x + 10, cy, w - 24, 24, b->cond, 256); cy += 32;
        ui_text(x + 10, cy, L"条件成立时执行其后子块", 140, 140, 160, false); cy += 24;
        break;
    case BL_CODE:
        ui_text(x + 10, cy, L"代码文件(.dex 语句片段)", 148, 154, 176, false); cy += 20;
        ui_input(ID_CFILE, x + 10, cy, w - 82, 24, b->code_file, 520);
        if (ui_button(x + w - 66, cy, 56, 24, L"浏览")) {
            wchar_t path[1024] = {0};
            OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
            ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_mainWnd;
            ofn.lpstrFilter = L"DexLang (*.dex)\0*.dex\0All\0*.*\0";
            ofn.lpstrFile = path; ofn.nMaxFile = 1024;
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) wcscpy(b->code_file, path);
        }
        cy += 30;
        ui_text(x + 10, cy, L"运行条件(表达式,空=总是)", 148, 154, 176, false); cy += 20;
        ui_input(ID_CCOND, x + 10, cy, w - 24, 24, b->code_cond, 256); cy += 32;
        break;
    case BL_END:
    default:
        ui_text(x + 10, cy, L"结束本镜头:自动进入下一镜头或退出", 148, 154, 176, false); cy += 24;
        break;
    }
    if (ui_button(x + 10, y + ph - 30, (w - 24) / 2, 24, L"应用预览")) {
        apply_scene_preview(sc);
        apply_block_preview(sc, b);
    }
    if (ui_button(x + 16 + (w - 24) / 2, y + ph - 30, (w - 24) / 2, 24, L"应用整场景")) {
        /* 从头跑到选中块(近似:逐块应用) */
        for (int i = 0; i < sc->nBlocks; i++) apply_block_preview(sc, &sc->blocks[i]);
    }
}

/* ---------- 属性:人物管理 ---------- */
static void ui_char_props(void) {
    int x = g_winW - 368, y = 66, w = 364, ph = g_winH - 44 - 30 - 26;
    if (ph < 60) ph = 60;
    fb_fill(x, y, w, ph, 30, 32, 44);
    fb_border(x, y, w, ph, 1, 50, 52, 66);
    int viewH = ph - 8;
    int contentH = 700;
    int sx = x + w - 16;
    g_propScroll = ui_vscroll(sx, y + 4, viewH, contentH, viewH, g_propScroll, in_rect(x, y, w, ph));
    int cy = y + 10 - g_propScroll;
    ui_text(x + 10, cy, L"人物集", 148, 154, 176, true); cy += 24;
    /* 人物列表(滚动) */
    if (ui_button(x + 10, cy, (w - 24) / 2, 22, L"+ 添加人物") && P.nChars < MAX_CHARS) {
        Character *ch = &P.chars[P.nChars++];
        memset(ch, 0, sizeof *ch);
        swprintf(ch->name, 64, L"人物%d", P.nChars);
    }
    if (ui_button(x + 16 + (w - 24) / 2, cy, (w - 24) / 2, 22, L"删除选中") && P.nChars > 0) {
        /* 删除最后选中的(这里简单删最后一个,选中逻辑用 g_pickChar 保留) */
        int ci = g_pickChar >= 0 && g_pickChar < P.nChars ? g_pickChar : P.nChars - 1;
        for (int i = ci; i < P.nChars - 1; i++) P.chars[i] = P.chars[i + 1];
        P.nChars--;
        if (g_pickChar >= P.nChars) g_pickChar = P.nChars - 1;
    }
    cy += 28;
    /* 人物列表 */
    for (int c = 0; c < P.nChars; c++) {
        Character *ch = &P.chars[c];
        bool sel = (g_pickChar == c);
        bool hover = in_rect(x + 10, cy, w - 40, 22);
        fb_fill(x + 10, cy, w - 40, 22, sel ? 60 : (hover ? 49 : 40), sel ? 62 : (hover ? 50 : 42), sel ? 80 : (hover ? 68 : 54));
        wchar_t lb[96]; swprintf(lb, 96, L"%d. %ls (%d 状态)", c + 1, ch->name, ch->nStates);
        fb_text(x + 16, cy + 1, lb, 16, sel ? 250 : 205, sel ? 220 : 214, sel ? 255 : 244, false);
        if (g_clicked && hover) g_pickChar = c;
        cy += 26;
    }
    if (P.nChars == 0) { ui_text(x + 10, cy, L"暂无人物", 148, 154, 176, false); cy += 24; }
    cy += 8;
    /* 选中人物详情 */
    if (g_pickChar >= 0 && g_pickChar < P.nChars) {
        Character *ch = &P.chars[g_pickChar];
        ui_text(x + 10, cy, L"人物详情", 148, 154, 176, true); cy += 24;
        ui_text(x + 10, cy, L"名字", 148, 154, 176, false); cy += 20;
        ui_input(ID_CH_NAME, x + 10, cy, w - 24, 24, ch->name, 64); cy += 32;
        ui_text(x + 10, cy, L"状态(表情/姿态)列表 — 每个状态一张图", 148, 154, 176, false); cy += 22;
        if (ui_button(x + 10, cy, 90, 22, L"+ 添加状态") && ch->nStates < MAX_STATES) {
            int s = ch->nStates;
            swprintf(ch->stateNames[s], 32, L"状态%d", s + 1);
            ch->nStates++;
        }
        if (ui_button(x + 106, cy, 90, 22, L"删除状态") && ch->nStates > 0) {
            ch->nStates--;
            if (ch->cur >= ch->nStates) ch->cur = ch->nStates - 1;
        }
        cy += 28;
        for (int s = 0; s < ch->nStates; s++) {
            wchar_t lb[64]; swprintf(lb, 64, L"状态 %d", s + 1);
            ui_text(x + 10, cy, lb, 148, 154, 176, false); cy += 20;
            ui_input(ID_ST_NAME0 + s, x + 10, cy, w - 96, 22, ch->stateNames[s], 32);
            if (ui_button(x + w - 82, cy, 70, 22, L"选图")) {
                g_pickChar = g_pickChar;
                g_pickTarget = 100 + s;
                g_pickMode = 2; scan_picks();
            }
            /* 显示当前图缩略名 */
            if (ch->states[s][0]) {
                fb_text(x + w - 96, cy + 2, ch->states[s], 12, 120, 190, 120, false);
            }
            cy += 26;
        }
        if (ch->nStates == 0) { ui_text(x + 10, cy, L"无状态(立绘块将用直接路径)", 140, 140, 160, false); cy += 22; }
    }
}

/* ---------- 选择器(背景/立绘/人物状态) ---------- */
static void ui_picker(void) {
    if (!g_pickMode) return;
    int pw = 400, phh = 460;
    int x = (g_winW - pw) / 2, y = (g_winH - phh) / 2;
    fb_fill(x, y, pw, phh, 40, 42, 54);
    fb_border(x, y, pw, phh, 1, 90, 92, 110);
    ui_text(x + 10, y + 6, g_pickCount ? L"点击缩略图选择:" : L"该目录下没有图片", 205, 214, 244, false);
    if (ui_button(x + pw - 50, y + 4, 42, 22, L"关闭")) g_pickMode = 0;
    int per = 5;
    int cell = 76;
    int gx = x + 8, gy = y + 34;
    int gridH = phh - 42;
    int rows = (g_pickCount + per - 1) / per;
    int contentH = rows * cell;
    int sx = x + pw - 16;
    g_pickScroll = ui_vscroll(sx, gy, gridH, contentH, gridH, g_pickScroll, in_rect(x, y, pw, phh));
    for (int i = 0; i < g_pickCount; i++) {
        int cx = gx + (i % per) * cell;
        int cyy = gy - g_pickScroll + (i / per) * cell;
        if (cyy < gy - cell || cyy > gy + gridH) continue;
        bool hover = in_rect(cx, cyy, 64, 64);
        if (hover) fb_fill(cx - 2, cyy - 2, 68, 68, 60, 62, 80);
        fb_image(cx, cyy, 64, 64, g_pickThumbs[i]);
        if (g_clicked && hover) {
            wchar_t full[560];
            Scene *sc = &P.scenes[P.cur];
            wchar_t dir[520];
            if (sc->bgDir[0]) wcscpy(dir, sc->bgDir);
            else if (P.resDir[0]) wcscpy(dir, P.resDir);
            else wcscpy(dir, L".");
            swprintf(full, 560, L"%s\\%s", dir, g_pickNames[i]);
            if (g_pickTarget == 0) { sc->bg[0] = 0; }
            if (g_pickTarget == 0) { wcscpy(sc->bg, full); sc->useBgImg = 1; }
            else if (g_pickTarget >= 1 && g_pickTarget <= MAX_SPR) wcscpy(sc->spr[g_pickTarget - 1], full);
            else if (g_pickTarget >= 100 && g_pickChar >= 0 && g_pickChar < P.nChars) {
                int s = g_pickTarget - 100;
                if (s < P.chars[g_pickChar].nStates) wcscpy(P.chars[g_pickChar].states[s], full);
            }
            g_pickMode = 0;
            apply_scene_preview(&P.scenes[P.cur]);
            apply_block_preview(&P.scenes[P.cur], &P.scenes[P.cur].blocks[P.scenes[P.cur].cur]);
        }
    }
}

/* 字符辅助(清理残留) */
static void bzero_w(wchar_t *p) { p[0] = 0; }

/* ---------- 主绘制 ---------- */
static void ui_draw(void) {
    /* 顶部菜单栏 */
    fb_fill(0, 0, g_winW, 36, 33, 35, 47);
    fb_fill(0, 36, g_winW, 1, 50, 52, 66);
    int bx = 4;
    if (ui_button(bx, 4, 70, 28, L"新建")) cmd_new(); bx += 76;
    if (ui_button(bx, 4, 70, 28, L"打开")) cmd_open(); bx += 76;
    if (ui_button(bx, 4, 70, 28, L"保存")) cmd_save(); bx += 76;
    if (ui_button(bx, 4, 90, 28, L"导出成品")) export_game(); bx += 96;
    if (ui_button(bx, 4, 76, 28, L"▶ 预览")) start_preview(); bx += 82;
    if (ui_button(bx, 4, 70, 28, L"设置")) { g_settingMode = 1; } bx += 76;
    if (ui_button(bx, 4, 70, 28, L"退出")) PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);

    /* 全屏预览模式:菜单栏 + 大预览区 */
    if (g_previewMode) {
        if (ui_button(g_winW - 150, 4, 142, 28, L"■ 退出预览")) stop_preview();
        int px = 4, py = 40, pw = g_winW - 8, ph = g_winH - 44 - 30;
        if (pw < 40) pw = 40;
        if (ph < 40) ph = 40;
        create_preview(g_mainWnd, px, py, pw, ph);
        /* 状态栏 */
        fb_fill(0, g_winH - 26, g_winW, 26, 33, 35, 47);
        wchar_t st[200];
        swprintf(st, 200, L"▶ 预览中 — 点击画面推进 / 点选项分支 / 按 ■ 退出预览",
                 P.cur + 1);
        fb_text(8, g_winH - 24, st, 15, 148, 154, 176, false);
        return;
    }

    /* 镜头列表(左,顶部) */
    {
        Scene *sc = &P.scenes[P.cur];
        int px = 4, py = 40, pw = 220, ph = 160;
        fb_fill(px, py, pw, ph, 30, 32, 44);
        fb_border(px, py, pw, ph, 1, 50, 52, 66);
        ui_text(px + 8, py + 4, L"镜头", 148, 154, 176, true);
        int y = py + 24;
        int nshow = 4;
        for (int i = 0; i < P.nScenes && i < nshow; i++) {
            wchar_t lb[128]; swprintf(lb, 128, L"%d. %ls%s", i + 1, P.scenes[i].title, i == P.cur ? L" ◄" : L"");
            if (ui_selectable2(px + 4, y, pw - 8, 22, lb, i == P.cur)) { P.cur = i; apply_scene_preview(&P.scenes[i]); apply_block_preview(&P.scenes[i], &P.scenes[i].blocks[P.scenes[i].cur]); }
            y += 24;
        }
        int bw3 = (pw - 16) / 3;
        if (ui_button(px + 4, py + ph - 26, bw3, 22, L"+镜头") && P.nScenes < MAX_SCENES) {
            scene_defaults(&P.scenes[P.nScenes]);
            P.nScenes++; P.cur = P.nScenes - 1;
        }
        if (ui_button(px + 6 + bw3, py + ph - 26, bw3, 22, L"改名")) {
            g_tabSel = 0; g_propScroll = 0; g_focusInput = ID_SC_TITLE;
        }
        if (ui_button(px + 8 + 2 * bw3, py + ph - 26, bw3, 22, L"删除") && P.nScenes > 1) {
            for (int i = P.cur; i < P.nScenes - 1; i++) P.scenes[i] = P.scenes[i + 1];
            P.nScenes--; if (P.cur >= P.nScenes) P.cur = P.nScenes - 1;
        }
    }

    /* 预览条(中央上方) */
    {
        int px = 228, py = 40, pw = g_winW - 228 - 372, ph = 160;
        if (pw < 40) pw = 40;
        create_preview(g_mainWnd, px, py, pw, ph);
    }

    ui_block_palette();
    ui_block_canvas();

    /* 右侧 tab */
    {
        int x = g_winW - 368, y = 38, w = 364;
        if (ui_button(x + 4, y, 116, 26, g_tabSel == 0 ? L"[ 镜头 ]" : L"镜头")) g_tabSel = 0;
        if (ui_button(x + 124, y, 116, 26, g_tabSel == 1 ? L"[ 块 ]" : L"块")) g_tabSel = 1;
        if (ui_button(x + 244, y, 116, 26, g_tabSel == 2 ? L"[ 人物 ]" : L"人物")) g_tabSel = 2;
    }
    if (g_tabSel == 0) ui_scene_props();
    else if (g_tabSel == 1) ui_block_props();
    else ui_char_props();
    ui_picker();
    if (g_settingMode) ui_settings();

    /* 底部状态栏 */
    fb_fill(0, g_winH - 26, g_winW, 26, 33, 35, 47);
    if (g_statusMsg[0]) {
        fb_text(8, g_winH - 24, g_statusMsg, 15, 120, 210, 130, false);
    } else {
        wchar_t st[256];
        swprintf(st, 256, L"GAL 编辑器 · 镜头 %d/%d · 块 %d/%d · 人物 %d — 指令式块编辑(Scratch 式)",
                 P.cur + 1, P.nScenes, P.scenes[P.cur].nBlocks, P.scenes[P.cur].nBlocks, P.nChars);
        fb_text(8, g_winH - 24, st, 15, 148, 154, 176, false);
    }
}

/* selectable 小助手(镜头列表用) */
static bool ui_selectable2(int x, int y, int w, int h, const wchar_t *text, bool selected) {
    bool hover = in_rect(x, y, w, h);
    unsigned r, g, b;
    if (selected) { r = 88; g = 91; b = 112; }
    else if (hover) { r = 49; g = 50; b = 68; }
    else { r = 30; g = 32; b = 44; }
    fb_fill(x, y, w, h, r, g, b);
    fb_text(x + 8, y + (h - 18) / 2, text, 18, selected ? 17 : 205, selected ? 17 : 214, selected ? 27 : 244, false);
    return hover && g_clicked;
}

/* ---------- 文件命令 ---------- */
static void cmd_new(void) {
    project_defaults();
    g_file[0] = 0;
    g_blockScroll = g_propScroll = 0;
    apply_scene_preview(&P.scenes[P.cur]);
    apply_block_preview(&P.scenes[P.cur], &P.scenes[P.cur].blocks[P.scenes[P.cur].cur]);
}

static void cmd_open(void) {
    wchar_t path[1024] = {0};
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_mainWnd;
    ofn.lpstrFilter = L"GAL 项目 (*.galscene)\0*.galscene\0All\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = 1024;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        scene_load(path);
        wcscpy(g_file, path);
        g_blockScroll = g_propScroll = 0;
        apply_scene_preview(&P.scenes[P.cur]);
        apply_block_preview(&P.scenes[P.cur], &P.scenes[P.cur].blocks[P.scenes[P.cur].cur]);
    }
}

static void cmd_save(void) {
    if (g_file[0]) { scene_save(g_file); return; }
    wchar_t path[1024] = {0};
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_mainWnd;
    ofn.lpstrFilter = L"GAL 项目 (*.galscene)\0*.galscene\0";
    ofn.lpstrFile = path; ofn.nMaxFile = 1024;
    ofn.lpstrDefExt = L"galscene"; ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&ofn)) {
        scene_save(path);
        wcscpy(g_file, path);
    }
}

/* ---------- 设置 ---------- */
static void settings_ini_path(wchar_t *out, int cap) {
    swprintf(out, cap, L"%s\\galedit.ini", g_exeDir);
}

static void load_settings(void) {
    GetModuleFileNameW(NULL, g_exeDir, 1024);
    wchar_t *sl = wcsrchr(g_exeDir, L'\\');
    if (sl) *sl = 0;
    /* 项目根默认 = 程序目录上一级 */
    wcscpy(g_projRoot, g_exeDir);
    wchar_t *sl2 = wcsrchr(g_projRoot, L'\\');
    if (sl2) *sl2 = 0;
    wcscpy(g_pythonExe, L"python");
    wchar_t ini[1100];
    settings_ini_path(ini, 1100);
    GetPrivateProfileStringW(L"settings", L"root", g_projRoot, g_projRoot, 1024, ini);
    GetPrivateProfileStringW(L"settings", L"python", g_pythonExe, g_pythonExe, 1024, ini);
}

static void save_settings(void) {
    wchar_t ini[1100];
    settings_ini_path(ini, 1100);
    WritePrivateProfileStringW(L"settings", L"root", g_projRoot, ini);
    WritePrivateProfileStringW(L"settings", L"python", g_pythonExe, ini);
}

static void ui_settings(void) {
    int pw = 460, phh = 320;
    int x = (g_winW - pw) / 2, y = (g_winH - phh) / 2;
    fb_fill(x, y, pw, phh, 40, 42, 54);
    fb_border(x, y, pw, phh, 1, 90, 92, 110);
    ui_text(x + 12, y + 8, L"设置", 205, 214, 244, true);
    int cy = y + 38;
    ui_text(x + 12, cy, L"项目根目录(导出代码写到这里)", 148, 154, 176, false); cy += 20;
    ui_input(ID_SET_ROOT, x + 12, cy, pw - 96, 24, g_projRoot, 1024);
    if (ui_button(x + pw - 80, cy, 68, 24, L"浏览")) browse_dir(g_projRoot, 1024);
    cy += 34;
    ui_text(x + 12, cy, L"Python 解释器(仅打包参考,留空用 python)", 148, 154, 176, false); cy += 20;
    ui_input(ID_SET_PY, x + 12, cy, pw - 24, 24, g_pythonExe, 1024);
    cy += 42;
    if (ui_button(x + 12, cy, (pw - 32) / 2, 26, L"保存")) { save_settings(); g_settingMode = 0; }
    if (ui_button(x + 20 + (pw - 32) / 2, cy, (pw - 32) / 2, 26, L"取消")) g_settingMode = 0;
    cy += 36;
    fb_text(x + 12, cy, L"也可直接编辑程序目录下的 galedit.ini", 13, 120, 128, 150, false);
}

/* ---------- 预览(解释执行块序列) ---------- */
static void start_preview(void) {
    if (!g_havePreview) create_preview(g_mainWnd, 4, 40, g_winW - 8, g_winH - 80);
    if (!g_havePreview) return;
    g_previewMode = 1;
    g_pvScene = P.cur;
    g_pvIdx = 0; g_pvSkip = 0; g_pvSel = -1;
    g_pvState = 0; g_pvOver = 0;
    apply_scene_preview(&P.scenes[g_pvScene]);
    if (pgal_text_clear) pgal_text_clear();
    if (pgal_box) pgal_box(1);
}

static void stop_preview(void) {
    g_previewMode = 0;
    if (pgal_hide_choices) pgal_hide_choices();
    apply_scene_preview(&P.scenes[P.cur]);
    apply_block_preview(&P.scenes[P.cur], &P.scenes[P.cur].blocks[P.scenes[P.cur].cur]);
}

static void tick_preview(void) {
    if (!g_previewMode || g_pvOver) return;
    if (g_pvScene < 0 || g_pvScene >= P.nScenes) { g_pvOver = 1; return; }
    Scene *sc = &P.scenes[g_pvScene];
    if (g_pvState == 1) {
        if (pgal_text_done && pgal_clicked && pgal_text_done() && pgal_clicked()) {
            g_pvState = 0;
            if (pgal_text_clear) pgal_text_clear();
            g_pvIdx++;
        }
        return;
    }
    if (g_pvState == 2) {
        if (GetTickCount() - g_pvStart >= (DWORD)g_pvMs) { g_pvState = 0; g_pvIdx++; }
        return;
    }
    if (g_pvState == 3) {
        if (pgal_picked && pgal_picked() >= 0) {
            int pk = (int)pgal_picked();
            int chIdx = g_pvIdx;
            int d = sc->blocks[chIdx].depth;
            int n = 0, found = -1;
            for (int j = chIdx + 1; j < sc->nBlocks && sc->blocks[j].depth > d; j = block_end(sc, j)) {
                if (sc->blocks[j].type == BL_OPTION && sc->blocks[j].depth == d + 1) {
                    if (n == pk) { found = j; break; }
                    n++;
                }
            }
            if (pgal_hide_choices) pgal_hide_choices();
            if (pgal_text_clear) pgal_text_clear();
            if (found >= 0) { g_pvSel = found; g_pvIdx = found + 1; }
            else { g_pvIdx = block_end(sc, chIdx); }
            g_pvState = 0;
        }
        return;
    }
    if (g_pvIdx < 0) g_pvIdx = 0;
    if (g_pvIdx >= sc->nBlocks) {
        if (g_pvScene + 1 < P.nScenes) { g_pvScene++; g_pvIdx = 0; apply_scene_preview(&P.scenes[g_pvScene]); }
        else { g_pvOver = 1; }
        return;
    }
    if (g_pvSkip > 0 && g_pvIdx < g_pvSkip) { g_pvIdx++; return; }
    if (g_pvSkip > 0 && g_pvIdx == g_pvSkip) g_pvSkip = 0;
    Block *b = &sc->blocks[g_pvIdx];
    if (b->type == BL_OPTION) {
        if (g_pvIdx != g_pvSel) g_pvSkip = block_end(sc, g_pvIdx);
        g_pvIdx++;
        return;
    }
    if (b->type == BL_CHOICE) {
        if (b->speaker[0] && pgal_speaker) { char buf[520]; w_to_utf8(b->speaker, buf, sizeof buf); pgal_speaker(buf); }
        if (b->choice_text[0] && pgal_text) { char buf[2200]; w_to_utf8(b->choice_text, buf, sizeof buf); pgal_text(buf); }
        int d = b->depth;
        int nopt = 0;
        for (int j = g_pvIdx + 1; j < sc->nBlocks && sc->blocks[j].depth > d; j = block_end(sc, j))
            if (sc->blocks[j].type == BL_OPTION && sc->blocks[j].depth == d + 1) nopt++;
        if (nopt < 1) nopt = 1;
        int sel = 0;
        for (int j = g_pvIdx + 1; j < sc->nBlocks && sc->blocks[j].depth > d && sel < nopt; j = block_end(sc, j)) {
            if (sc->blocks[j].type == BL_OPTION && sc->blocks[j].depth == d + 1) {
                char buf[300];
                w_to_utf8(sc->blocks[j].option_text[0] ? sc->blocks[j].option_text : L"\u2026", buf, sizeof buf);
                if (pgal_set_choice) pgal_set_choice(sel, buf);
                sel++;
            }
        }
        if (pgal_show_choices) pgal_show_choices();
        g_pvState = 3;
        return;
    }
    if (b->type == BL_IF) { g_pvIdx++; return; }
    if (b->type == BL_END) {
        if (g_pvScene + 1 < P.nScenes) { g_pvScene++; g_pvIdx = 0; apply_scene_preview(&P.scenes[g_pvScene]); }
        else { g_pvOver = 1; }
        return;
    }
    apply_block_preview(sc, b);
    if (b->type == BL_SPEAK && b->text[0]) { g_pvState = 1; return; }
    if (b->type == BL_WAIT && b->wait_type == 0) { g_pvState = 1; return; }
    if (b->type == BL_WAIT && b->wait_type == 1) {
        g_pvState = 2; g_pvMs = b->wait_ms > 0 ? b->wait_ms : 100; g_pvStart = GetTickCount(); return;
    }
    g_pvIdx++;
}

/* ---------- 主缓冲 / 渲染 ---------- */
static char g_shotPath[520] = {0};
static int g_frameCount = 0;

static void fb_resize(int w, int h) {
    if (g_bits) { free(g_bits); g_bits = NULL; }
    g_winW = w; g_winH = h; g_stride = w * 4;
    g_bits = (unsigned char*)malloc((size_t)w * h * 4);
    if (g_bits) memset(g_bits, 0, (size_t)w * h * 4);
}

static void save_shot(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f || !g_bits) return;
    int w = g_winW, h = g_winH;
    unsigned int row = (unsigned int)w * 4;
    unsigned int img = row * (unsigned int)h;
    unsigned char bfh[14] = { 'B','M',0,0,0,0,0,0,0,0,54,0,0,0 };
    unsigned int total = 54 + img;
    bfh[2] = (unsigned char)(total & 0xFF); bfh[3] = (unsigned char)((total >> 8) & 0xFF);
    bfh[4] = (unsigned char)((total >> 16) & 0xFF); bfh[5] = (unsigned char)((total >> 24) & 0xFF);
    unsigned char bih[40]; memset(bih, 0, 40);
    bih[0] = 40;
    bih[4] = (unsigned char)(w & 0xFF); bih[5] = (unsigned char)((w >> 8) & 0xFF);
    bih[6] = (unsigned char)((w >> 16) & 0xFF); bih[7] = (unsigned char)((w >> 24) & 0xFF);
    bih[8] = (unsigned char)(h & 0xFF); bih[9] = (unsigned char)((h >> 8) & 0xFF);
    bih[10] = (unsigned char)((h >> 16) & 0xFF); bih[11] = (unsigned char)((h >> 24) & 0xFF);
    bih[12] = 1; bih[14] = 32;
    bih[20] = (unsigned char)(img & 0xFF); bih[21] = (unsigned char)((img >> 8) & 0xFF);
    bih[22] = (unsigned char)((img >> 16) & 0xFF); bih[23] = (unsigned char)((img >> 24) & 0xFF);
    fwrite(bfh, 1, 14, f); fwrite(bih, 1, 40, f); fwrite(g_bits, 1, img, f);
    fclose(f);
}

static void frame(HWND hwnd) {
    if (!g_bits) return;
    g_frameTick++;
    fb_fill(0, 0, g_winW, g_winH, 40, 42, 54);
    ui_draw();
    /* 拖拽结束处理 */
    if (g_dragIdx >= 0 || g_dragFromPanel >= 0) {
        if (!g_btnL) {
            Scene *sc = &P.scenes[P.cur];
            if (g_dragIdx >= 0 && g_dropLine >= 0 && g_dropLine != g_dragIdx) {
                block_move(sc, g_dragIdx, g_dropLine, g_dropWhere);
            } else if (g_dragFromPanel >= 0) {
                if (g_dropLine >= 0 && g_dropLine < sc->nBlocks) {
                    int i = insert_block(sc, g_dragFromPanel);
                    if (i >= 0) block_move(sc, i, g_dropLine, g_dropWhere);
                } else {
                    insert_block(sc, g_dragFromPanel);
                }
            }
            g_dragIdx = -1; g_dragFromPanel = -1;
        }
    }
    /* 点击下拉外部关闭 */
    if (g_dropOpen >= 0 && g_clicked &&
        !in_rect(g_dropBtnX, g_dropBtnY, g_dropBtnW, g_dropBtnH) &&
        !in_rect(g_dropX, g_dropY, g_dropW, g_dropH)) {
        g_dropOpen = -1;
    }
    if (g_previewMode) tick_preview();
    if (g_havePreview && pgal_poll) pgal_poll();
    HDC wdc = GetDC(hwnd);
    if (g_havePreview && hPreview && IsWindow(hPreview)) {
        /* 父窗口不绘制预览子窗口区域:由引擎子窗口独立呈现,消除覆盖闪烁 */
        RECT pr; GetWindowRect(hPreview, &pr);
        MapWindowPoints(NULL, hwnd, (POINT*)&pr, 2);
        ExcludeClipRect(wdc, pr.left, pr.top, pr.right, pr.bottom);
    }
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_winW;
    bi.bmiHeader.biHeight = -g_winH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(wdc, 0, 0, g_winW, g_winH, 0, 0, 0, g_winH, g_bits, &bi, DIB_RGB_COLORS);
    ReleaseDC(hwnd, wdc);
    g_clicked = false;
    g_nchars = 0;
    g_wheel = 0;
    g_frameCount++;
    if (g_shotPath[0] && g_frameCount >= 6) { save_shot(g_shotPath); PostQuitMessage(0); }
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;   /* 自绘 UI,禁止擦背景避免闪烁 */
    case WM_CREATE:
        g_mainWnd = hwnd;
        return 0;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) fb_resize(LOWORD(l), HIWORD(l));
        return 0;
    case WM_MOUSEMOVE:
        g_mx = (short)LOWORD(l); g_my = (short)HIWORD(l);
        if (g_pressing && g_btnL && g_dragIdx < 0 && g_dragFromPanel < 0 && abs(g_my - g_pressY) > 6) {
            g_dragIdx = g_pressBlock;
        }
        return 0;
    case WM_LBUTTONDOWN:
        g_mx = (short)LOWORD(l); g_my = (short)HIWORD(l);
        g_btnL = true; g_clicked = true;
        g_pressing = true; g_pressBlock = -1; g_pressY = g_my;
        return 0;
    case WM_LBUTTONUP:
        g_btnL = false; g_pressing = false;
        return 0;
    case WM_MOUSEWHEEL:
        g_wheel += (short)HIWORD(w);
        return 0;
    case WM_CHAR:
        if (w > 0 && w < 0x10000 && g_nchars < 63) g_chars[g_nchars++] = (wchar_t)w;
        return 0;
    case WM_KEYDOWN:
        if (w < 256) g_keys[w] = true;
        return 0;
    case WM_KEYUP:
        if (w < 256) g_keys[w] = false;
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_gal && pgal_close) pgal_close();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, w, l);
    }
}

/* ---------- 调试 ---------- */
static char g_log[65536]; static int g_lp = 0;
static void L(const char *s) {
    if (!g_dbging) return;
    int n = (int)strlen(s);
    if (n > 4000) n = 4000;
    if (g_lp + n < (int)sizeof g_log - 1) { memcpy(g_log + g_lp, s, n); g_lp += n; g_log[g_lp] = 0; }
}
static LONG WINAPI dbg_vh(PEXCEPTION_POINTERS ep) {
    FILE *f = fopen("_dbg.txt", "w");
    if (f) {
        fprintf(f, "CRASH addr=%p code=%lx\n%s\n",
                ep->ExceptionRecord->ExceptionAddress,
                (unsigned long)ep->ExceptionRecord->ExceptionCode,
                g_log);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------- 入口 ---------- */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    static wchar_t g_openFile[1024];
    static wchar_t g_writeFile[1024];
    static wchar_t g_saveFile[1024];
    AddVectoredExceptionHandler(1, dbg_vh);
    const wchar_t *p = wcsstr(GetCommandLineW(), L"-shot");
    if (p) {
        g_dbging = 1;
        p += 5;
        while (*p == L' ') p++;
        wchar_t tp[520]; int i = 0;
        while (*p && *p != L' ' && i < 519) tp[i++] = *p++;
        tp[i] = 0;
        WideCharToMultiByte(CP_UTF8, 0, tp, -1, g_shotPath, 520, NULL, NULL);
    }
    p = wcsstr(GetCommandLineW(), L"-open");
    if (p) {
        p += 5;
        while (*p == L' ') p++;
        wchar_t tp[1024]; int i = 0;
        while (*p && *p != L' ' && i < 1023) tp[i++] = *p++;
        tp[i] = 0;
        wcscpy(g_openFile, tp);
    }
    p = wcsstr(GetCommandLineW(), L"-write");
    if (p) {
        p += 6;
        while (*p == L' ') p++;
        wchar_t tp[1024]; int i = 0;
        while (*p && *p != L' ' && i < 1023) tp[i++] = *p++;
        tp[i] = 0;
        wcscpy(g_writeFile, tp);
    }
    p = wcsstr(GetCommandLineW(), L"-save");
    if (p) {
        p += 5;
        while (*p == L' ') p++;
        wchar_t tp[1024]; int i = 0;
        while (*p && *p != L' ' && i < 1023) tp[i++] = *p++;
        tp[i] = 0;
        wcscpy(g_saveFile, tp);
    }

    project_defaults();
    load_settings();
    if (g_projRoot[0]) SetCurrentDirectoryW(g_projRoot);   /* 工作目录切到项目根,图片/DLL 相对路径正确 */
    if (g_openFile[0]) {
        scene_load(g_openFile);
        wcscpy(g_file, g_openFile);
    }
    if (g_writeFile[0]) {
        char wp[1024]; w_to_utf8(g_writeFile, wp, sizeof wp);
        write_scene_dex(wp);
        return 0;
    }
    if (g_saveFile[0]) {
        scene_save(g_saveFile);
        return 0;
    }

    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"DexGALEditor7";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"DexGALEditor7", L"GAL 编辑器",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0,
                                1400, 900, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g_mainWnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 16, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_TIMER) frame(hwnd);
    }
    return 0;
}
