/* dg_input.c — 输入:键盘 / 鼠标 / 手柄(XInput)/ **动作映射**
 *
 * 设计(M4,见 docs/DEXGAME_DESIGN.md §8):
 *   - 两层接口:底层**轮询**(eng_key_down/pressed、eng_mouse_*、eng_pad_*)与
 *     **动作映射**(eng_action_bind("jump", ...) / eng_action_down("jump"))。
 *     前者给"我想自己解释按键"的人,后者给游戏逻辑 —— 也是 IDE 生成代码要用的层
 *     (IDE 里改键位只需改绑定,不用动逻辑)。
 *   - **边沿检测**(pressed/released)靠 begin_frame 时把"当前"拷进"上一帧",
 *     所以必须在**读输入之前**调用:eng_frame_begin() 内部已经做了。
 *   - 手柄用 XInput,运行时 LoadLibrary("xinput1_4.dll") 再取函数(与 d3dcompiler
 *     同样的做法:zig 不提供 xinput 的导入库,而且没插手柄也不该崩)。
 *   - **按键码是统一编号**(见下),所以动作可以绑"键盘 W"或"手柄 A"而逻辑不变。
 *   - eng_input_feed() 可以**注入合成输入** —— 测试与 IDE 的"脚本化试玩"要用它,
 *     否则输入相关的东西在无人值守环境里根本测不了。
 *
 * 按键码(eng_action_bind / eng_key_down 共用):
 *   0..255      Win32 虚拟键(VK_*),例如 65 = 'A'、32 = 空格、37/38/39/40 = 方向键
 *   256..258    鼠标左/右/中键
 *   300..313    手柄按键:300=上 301=下 302=左 303=右 304=Start 305=Back
 *               306=LS 307=RS 308=LB 309=RB 310=A 311=B 312=X 313=Y
 *   400+i       手柄轴 i 的**正向**(i:0=LX 1=LY 2=RX 3=RY 4=LT 5=RT)
 *   500+i       手柄轴 i 的**负向**
 *   轴约定:LY/RY 向上为负、向下为正(与屏幕坐标一致);LT/RT 是 0..1 的扳机。
 */
#include "dexgame.h"

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- 按键码 ---------- */
#define DG_CODE_KEY_MAX   256           /* 0..255 = VK */
#define DG_CODE_MOUSE     256           /* 256..258 = 鼠标左/右/中 */
#define DG_CODE_PAD       300           /* 300..313 = 手柄按键 */
#define DG_PAD_BUTTONS    14
#define DG_CODE_AXIS_POS  400           /* 400+i = 轴 i 正向 */
#define DG_CODE_AXIS_NEG  500           /* 500+i = 轴 i 负向 */
#define DG_AXIS_COUNT     6
#define DG_DEADZONE       0.35f         /* 摇杆死区 */

static uint8_t g_key_now[DG_CODE_KEY_MAX];
static uint8_t g_key_prev[DG_CODE_KEY_MAX];
static uint8_t g_mouse_now[3];
static uint8_t g_mouse_prev[3];
static float   g_mouse_x = 0.0f, g_mouse_y = 0.0f;
static int     g_wheel = 0;             /* 本帧滚轮格数(向上为正) */

static uint8_t g_pad_now[DG_PAD_BUTTONS];
static uint8_t g_pad_prev[DG_PAD_BUTTONS];
static float   g_axis[DG_AXIS_COUNT];   /* 已做过去死区处理 */
static int     g_pad_connected = 0;
static int     g_pad_checked = 0;
static int     g_pad_index = 0;         /* 用第几个手柄(0..3)*/

/* XInput 运行时加载 */
typedef struct {
    unsigned long dwPacketNumber;
    struct { unsigned short wButtons; unsigned char bLeftTrigger, bRightTrigger;
             short sThumbLX, sThumbLY, sThumbRX, sThumbRY; } Gamepad;
} DgXInputState;
typedef unsigned long (WINAPI *PFN_XInputGetState)(unsigned long, DgXInputState *);

static void *g_xinput_dll = NULL;
static PFN_XInputGetState g_xinput_get = NULL;

/* ---------- 动作表 ---------- */
#define DG_MAX_ACTIONS 32
#define DG_ACTION_SLOTS 4
typedef struct {
    char    name[32];
    int32_t code[DG_ACTION_SLOTS];
    int32_t n;
} DgAction;

static DgAction g_actions[DG_MAX_ACTIONS];
static int      g_nactions = 0;

/* ---------- 初始化 ---------- */
void dg_input_init(void) {
    memset(g_key_now, 0, sizeof g_key_now);
    memset(g_key_prev, 0, sizeof g_key_prev);
    memset(g_mouse_now, 0, sizeof g_mouse_now);
    memset(g_mouse_prev, 0, sizeof g_mouse_prev);
    memset(g_pad_now, 0, sizeof g_pad_now);
    memset(g_pad_prev, 0, sizeof g_pad_prev);
    memset(g_axis, 0, sizeof g_axis);
    g_mouse_x = g_mouse_y = 0.0f;
    g_wheel = 0;
    g_pad_connected = 0;
    g_pad_checked = 0;
    g_nactions = 0;
    memset(g_actions, 0, sizeof g_actions);
    if (!g_xinput_get) {
        g_xinput_dll = (void *)LoadLibraryA("xinput1_4.dll");
        if (!g_xinput_dll) g_xinput_dll = (void *)LoadLibraryA("xinput1_3.dll");
        if (g_xinput_dll)
            g_xinput_get = (PFN_XInputGetState)(void *)GetProcAddress((HMODULE)g_xinput_dll,
                                                                     "XInputGetState");
    }
}

void dg_input_shutdown(void) {
    /* DLL 不卸载:进程退出时系统回收,重复 LoadLibrary 也有引用计数 */
    g_nactions = 0;
}

/* ---------- 帧边界 ---------- */
static void dg_input_poll_pad(void) {
    if (!g_pad_checked) {
        g_pad_checked = 1;
        if (g_xinput_get) {
            DgXInputState st;
            for (int i = 0; i < 4; i++)
                if (g_xinput_get((unsigned long)i, &st) == 0) { g_pad_index = i; g_pad_connected = 1; break; }
        }
    }
    if (!g_pad_connected || !g_xinput_get) return;
    DgXInputState st;
    if (g_xinput_get((unsigned long)g_pad_index, &st) != 0) {
        g_pad_connected = 0;                 /* 拔了:清状态 */
        memset(g_pad_now, 0, sizeof g_pad_now);
        memset(g_axis, 0, sizeof g_axis);
        return;
    }
    const unsigned short b = st.Gamepad.wButtons;
    static const unsigned short bits[DG_PAD_BUTTONS] = {
        0x0001, 0x0002, 0x0004, 0x0008,      /* 上 下 左 右 */
        0x0010, 0x0020, 0x0040, 0x0080,      /* Start Back LS RS */
        0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000  /* LB RB A B X Y */
    };
    for (int i = 0; i < DG_PAD_BUTTONS; i++) g_pad_now[i] = (b & bits[i]) ? 1 : 0;

    /* 摇杆:死区 + 归一化;LY/RY 取反,让"下 = 正"与屏幕一致 */
    const float lx = st.Gamepad.sThumbLX / 32767.0f;
    const float ly = st.Gamepad.sThumbLY / 32767.0f;
    const float rx = st.Gamepad.sThumbRX / 32767.0f;
    const float ry = st.Gamepad.sThumbRY / 32767.0f;
    float ax[DG_AXIS_COUNT];
    ax[0] = lx; ax[1] = -ly; ax[2] = rx; ax[3] = -ry;
    ax[4] = st.Gamepad.bLeftTrigger / 255.0f;
    ax[5] = st.Gamepad.bRightTrigger / 255.0f;
    for (int i = 0; i < DG_AXIS_COUNT; i++) {
        float v = ax[i];
        if (i < 4) {                                    /* 摇杆:去死区并归一化 */
            const float a = fabsf(v);
            v = a < DG_DEADZONE ? 0.0f : (v > 0 ? (a - DG_DEADZONE) / (1 - DG_DEADZONE)
                                               : -(a - DG_DEADZONE) / (1 - DG_DEADZONE));
        }
        g_axis[i] = v;
    }
}

void dg_input_begin_frame(void) {
    /* 只采样设备 + 清本帧滚轮。**不要**在这里把 now 拷贛 prev:
       消息是在 eng_frame_begin() 里的 pump 才进来的,如果 begin 就拷贝,
       本帧新按下的键立刻变成"上一帧也按着",pressed 永远为假
       (第一版就是这么错的)。拷贝放在帧**结束**。 */
    dg_input_poll_pad();
    g_wheel = 0;
}

/* 帧结束:把"当前"记为"上一帧",下一帧才能算出边沿 */
void dg_input_end_frame(void) {
    memcpy(g_key_prev, g_key_now, sizeof g_key_now);
    memcpy(g_mouse_prev, g_mouse_now, sizeof g_mouse_now);
    memcpy(g_pad_prev, g_pad_now, sizeof g_pad_now);
}

/* ---------- Win32 消息 ---------- */
/* 返回 1 = 已消费 */
int dg_input_on_message(unsigned int msg, uint64_t wp, int64_t lp) {
    const int x = (int)(short)LOWORD((uintptr_t)lp);
    const int y = (int)(short)HIWORD((uintptr_t)lp);
    switch (msg) {
    case 0x0100: case 0x0104:                 /* WM_KEYDOWN / WM_SYSKEYDOWN */
        if ((wp & 0xFF) < DG_CODE_KEY_MAX) g_key_now[wp & 0xFF] = 1;
        return 1;
    case 0x0101: case 0x0105:                 /* WM_KEYUP / WM_SYSKEYUP */
        if ((wp & 0xFF) < DG_CODE_KEY_MAX) g_key_now[wp & 0xFF] = 0;
        return 1;
    case 0x0200:                              /* WM_MOUSEMOVE */
        g_mouse_x = (float)x;
        g_mouse_y = (float)y;
        return 1;
    case 0x0201: g_mouse_now[0] = 1; return 1;   /* WM_LBUTTONDOWN */
    case 0x0202: g_mouse_now[0] = 0; return 1;   /* WM_LBUTTONUP */
    case 0x0204: g_mouse_now[1] = 1; return 1;   /* WM_RBUTTONDOWN */
    case 0x0205: g_mouse_now[1] = 0; return 1;   /* WM_RBUTTONUP */
    case 0x0207: g_mouse_now[2] = 1; return 1;   /* WM_MBUTTONDOWN */
    case 0x0208: g_mouse_now[2] = 0; return 1;   /* WM_MBUTTONUP */
    case 0x020A:                                 /* WM_MOUSEWHEEL */
        g_wheel += (int)(short)HIWORD((uintptr_t)wp) / 120;
        return 1;
    case 0x0021:                                 /* WM_KILLFOCUS:别让按键卡住 */
        memset(g_key_now, 0, sizeof g_key_now);
        memset(g_mouse_now, 0, sizeof g_mouse_now);
        return 1;
    default:
        return 0;
    }
}

/* ---------- 底层状态 ---------- */
static int dg_code_valid(int code) {
    return code >= 0 && (code < DG_CODE_KEY_MAX || (code >= DG_CODE_MOUSE && code < DG_CODE_MOUSE + 3) ||
                         (code >= DG_CODE_PAD && code < DG_CODE_PAD + DG_PAD_BUTTONS) ||
                         (code >= DG_CODE_AXIS_POS && code < DG_CODE_AXIS_POS + DG_AXIS_COUNT) ||
                         (code >= DG_CODE_AXIS_NEG && code < DG_CODE_AXIS_NEG + DG_AXIS_COUNT));
}

/* -1 = 非法;否则 0/1 */
static int dg_code_state(int code, int prev) {
    if (code < DG_CODE_KEY_MAX)
        return prev ? g_key_prev[code] : g_key_now[code];
    if (code < DG_CODE_MOUSE + 3) {
        const int i = code - DG_CODE_MOUSE;
        return prev ? g_mouse_prev[i] : g_mouse_now[i];
    }
    if (code < DG_CODE_PAD + DG_PAD_BUTTONS) {
        const int i = code - DG_CODE_PAD;
        return prev ? g_pad_prev[i] : g_pad_now[i];
    }
    /* 轴当数字源:正向 = value > 0.5,负向 = value < -0.5 */
    if (code < DG_CODE_AXIS_POS + DG_AXIS_COUNT) {
        const float v = g_axis[code - DG_CODE_AXIS_POS];
        return v > 0.5f ? 1 : 0;
    }
    if (code < DG_CODE_AXIS_NEG + DG_AXIS_COUNT) {
        const float v = g_axis[code - DG_CODE_AXIS_NEG];
        return v < -0.5f ? 1 : 0;
    }
    return -1;
}

/* 模拟量:只有轴有意义(按键返回 0/1)*/
static float dg_code_value(int code) {
    if (code >= DG_CODE_AXIS_POS && code < DG_CODE_AXIS_POS + DG_AXIS_COUNT) {
        const float v = g_axis[code - DG_CODE_AXIS_POS];
        return v > 0.0f ? v : 0.0f;
    }
    if (code >= DG_CODE_AXIS_NEG && code < DG_CODE_AXIS_NEG + DG_AXIS_COUNT) {
        const float v = -g_axis[code - DG_CODE_AXIS_NEG];
        return v > 0.0f ? v : 0.0f;
    }
    const int s = dg_code_state(code, 0);
    return s == 1 ? 1.0f : 0.0f;
}

int dg_input_down(int code) {
    if (!dg_code_valid(code)) { dg_error("input code %d is not a valid key code", code); return 0; }
    return dg_code_state(code, 0) == 1;
}

int dg_input_pressed(int code) {
    if (!dg_code_valid(code)) { dg_error("input code %d is not a valid key code", code); return 0; }
    return dg_code_state(code, 0) == 1 && dg_code_state(code, 1) == 0;
}

int dg_input_released(int code) {
    if (!dg_code_valid(code)) { dg_error("input code %d is not a valid key code", code); return 0; }
    return dg_code_state(code, 0) == 0 && dg_code_state(code, 1) == 1;
}

float dg_input_code_axis(int axis) {
    if (axis < 0 || axis >= DG_AXIS_COUNT) { dg_error("pad axis %d out of range 0..%d", axis, DG_AXIS_COUNT - 1); return 0.0f; }
    return g_axis[axis];
}

float dg_input_mouse_x(void) { return g_mouse_x; }
float dg_input_mouse_y(void) { return g_mouse_y; }
int   dg_input_mouse_down(int btn) {
    if (btn < 0 || btn > 2) { dg_error("mouse button %d out of range 0..2", btn); return 0; }
    return g_mouse_now[btn] == 1;
}
int   dg_input_mouse_pressed(int btn) {
    if (btn < 0 || btn > 2) { dg_error("mouse button %d out of range 0..2", btn); return 0; }
    return g_mouse_now[btn] == 1 && g_mouse_prev[btn] == 0;
}
int   dg_input_mouse_released(int btn) {
    if (btn < 0 || btn > 2) { dg_error("mouse button %d out of range 0..2", btn); return 0; }
    return g_mouse_now[btn] == 0 && g_mouse_prev[btn] == 1;
}
int   dg_input_wheel(void) { return g_wheel; }
int   dg_input_pad_connected(void) { return g_pad_connected; }

/* ---------- 合成输入(测试 / IDE 脚本化试玩)---------- */
int dg_input_feed(int code, int down) {
    if (!dg_code_valid(code)) { dg_error("input code %d is not a valid key code", code); return -1; }
    if (code < DG_CODE_KEY_MAX) g_key_now[code] = down ? 1 : 0;
    else if (code < DG_CODE_MOUSE + 3) g_mouse_now[code - DG_CODE_MOUSE] = down ? 1 : 0;
    else if (code < DG_CODE_PAD + DG_PAD_BUTTONS) g_pad_now[code - DG_CODE_PAD] = down ? 1 : 0;
    else if (code < DG_CODE_AXIS_POS + DG_AXIS_COUNT) g_axis[code - DG_CODE_AXIS_POS] = down ? 1.0f : 0.0f;
    else g_axis[code - DG_CODE_AXIS_NEG] = down ? -1.0f : 0.0f;
    return 0;
}

int dg_input_feed_axis(int axis, float value) {
    if (axis < 0 || axis >= DG_AXIS_COUNT) { dg_error("pad axis %d out of range 0..%d", axis, DG_AXIS_COUNT - 1); return -1; }
    if (value < -1.0f) value = -1.0f;
    if (value > 1.0f) value = 1.0f;
    g_axis[axis] = value;
    return 0;
}

int dg_input_feed_mouse(float x, float y) { g_mouse_x = x; g_mouse_y = y; return 0; }
void dg_input_feed_wheel(int delta) { g_wheel += delta; }

void dg_input_clear(void) {
    memset(g_key_now, 0, sizeof g_key_now);
    memset(g_key_prev, 0, sizeof g_key_prev);
    memset(g_mouse_now, 0, sizeof g_mouse_now);
    memset(g_mouse_prev, 0, sizeof g_mouse_prev);
    memset(g_pad_now, 0, sizeof g_pad_now);
    memset(g_pad_prev, 0, sizeof g_pad_prev);
    memset(g_axis, 0, sizeof g_axis);
    g_wheel = 0;
}

/* ---------- 动作映射 ---------- */
static DgAction *dg_action_find(const char *name) {
    for (int i = 0; i < g_nactions; i++)
        if (strcmp(g_actions[i].name, name) == 0) return &g_actions[i];
    return NULL;
}

int dg_input_action_bind(const char *name, int code) {
    if (!name || !name[0]) { dg_error("action name is empty"); return -1; }
    if (!dg_code_valid(code)) { dg_error("input code %d is not a valid key code", code); return -1; }
    DgAction *a = dg_action_find(name);
    if (!a) {
        if (g_nactions >= DG_MAX_ACTIONS) {
            dg_error("too many actions (max %d)", DG_MAX_ACTIONS);
            return -1;
        }
        a = &g_actions[g_nactions++];
        snprintf(a->name, sizeof a->name, "%s", name);
        a->n = 0;
    }
    for (int i = 0; i < a->n; i++)
        if (a->code[i] == code) return 0;           /* 重复绑定:无害 */
    if (a->n >= DG_ACTION_SLOTS) {
        dg_error("action '%s' already has %d bindings", name, DG_ACTION_SLOTS);
        return -1;
    }
    a->code[a->n++] = code;
    return 0;
}

/* 解绑 = 把动作整个移出表(而不是只清空来源):
   否则 eng_action_bound 仍然说"已绑定",重复解绑也不报错 —— 两种语义都能自圆其说,
   但"解绑后就没这个动作了"对用户最直观。用末尾交换保持数组连续。 */
int dg_input_action_unbind(const char *name) {
    int idx = -1;
    for (int i = 0; i < g_nactions; i++)
        if (strcmp(g_actions[i].name, name) == 0) { idx = i; break; }
    if (idx < 0) { dg_error("action '%s' is not bound", name); return -1; }
    g_nactions--;
    if (idx != g_nactions) g_actions[idx] = g_actions[g_nactions];
    memset(&g_actions[g_nactions], 0, sizeof g_actions[0]);
    return 0;
}

int dg_input_action_down(const char *name) {
    const DgAction *a = dg_action_find(name);
    if (!a) { dg_error("action '%s' is not bound (use eng_action_bind first)", name); return 0; }
    for (int i = 0; i < a->n; i++) if (dg_code_state(a->code[i], 0) == 1) return 1;
    return 0;
}

int dg_input_action_pressed(const char *name) {
    const DgAction *a = dg_action_find(name);
    if (!a) { dg_error("action '%s' is not bound", name); return 0; }
    for (int i = 0; i < a->n; i++)
        if (dg_code_state(a->code[i], 0) == 1 && dg_code_state(a->code[i], 1) == 0) return 1;
    return 0;
}

int dg_input_action_released(const char *name) {
    const DgAction *a = dg_action_find(name);
    if (!a) { dg_error("action '%s' is not bound", name); return 0; }
    for (int i = 0; i < a->n; i++)
        if (dg_code_state(a->code[i], 0) == 0 && dg_code_state(a->code[i], 1) == 1) return 1;
    return 0;
}

/* 判定型数字源:任一绑定按下就是 1;模拟量取所有绑定里"绝对值最大"的那个 */
float dg_input_action_value(const char *name) {
    const DgAction *a = dg_action_find(name);
    if (!a) { dg_error("action '%s' is not bound", name); return 0.0f; }
    float best = 0.0f;
    for (int i = 0; i < a->n; i++) {
        const float v = dg_code_value(a->code[i]);
        if (fabsf(v) > fabsf(best)) best = v;
    }
    return best;
}

int dg_input_action_bound(const char *name) { return dg_action_find(name) ? 1 : 0; }
int dg_input_action_count(void) { return g_nactions; }
const char *dg_input_action_name(int i) {
    if (i < 0 || i >= g_nactions) return "";
    return g_actions[i].name;
}
int dg_input_action_code(const char *name, int slot) {
    const DgAction *a = dg_action_find(name);
    if (!a) { dg_error("action '%s' is not bound", name); return -1; }
    if (slot < 0 || slot >= a->n) return -1;
    return a->code[slot];
}
