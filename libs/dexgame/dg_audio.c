/* dg_audio.c — 音频:XAudio2 + 手写 WAV 解析(M4)
 *
 * 为什么是 XAudio2 而不是 vendored miniaudio(已与用户确认):
 *   与 D3D11 同一风格 —— 用系统 API、不往仓库里塞第三方代码。项目里唯一 vendored
 *   的东西是 stb_image(283KB 的解码器,自己写不值);WAV 解析只要 ~120 行。
 *
 * 两个概念,别混:
 *   **声音资源**(sound):按路径缓存的解码结果(取样率/声道/时长 + PCM 数据)。
 *     WAVEFORMATEX 与 PCM 缓冲在播放期间必须一直有效,所以归它持有。
 *   **播放实例**(voice):一次播放。同一个声音可以同时响好几次(脚步/子弹)。
 *
 * 其他约定:
 *   - XAudio2 是 COM 接口但**没有导入库**:运行时 LoadLibrary("xaudio2_9.dll")
 *     (Win10)/"xaudio2_8.dll"(Win8)再 GetProcAddress("XAudio2Create")。zig 只在
 *     mingw 头里提供 xaudio2.h(接口的 C 版 vtable 声明),不提供 .lib。
 *   - **没有声卡也不报错**:eng_audio_ok() 会返回 0,播放调用一律变成"静默成功"式
 *     的失败(带原因),游戏不至于因为没声音直接崩。测试据此跳过。
 *   - 失败必须带原因(dg_error),与引擎其余部分一致。
 *   - 库不写 stdout。
 */
#include "dexgame.h"
#include "dg_utf8.h"

#include <windows.h>
#include <xaudio2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DG_MAX_SOUNDS 64
#define DG_MAX_VOICES 32

typedef struct {
    int             used;
    uint32_t        gen;                 /* 代际句柄:释放后旧 id 失效 */
    char            path[DG_PATH_MAX];
    BYTE           *data;                /* PCM(播放期间必须保活)*/
    DWORD           bytes;
    WAVEFORMATEX    fmt;
    double          duration;
} DgSound;

typedef struct {
    int                used;
    int32_t            sound;            /* 用代际句柄,不是槽位 */
    IXAudio2SourceVoice *voice;          /* NULL = 已停止 */
} DgVoice;

static IXAudio2               *g_xa = NULL;
static IXAudio2MasteringVoice *g_master = NULL;
static DgSound                 g_sounds[DG_MAX_SOUNDS];
static DgVoice                 g_voices[DG_MAX_VOICES];
static uint32_t                g_sound_gen = 0;
static float                   g_master_vol = 1.0f;
static int                     g_audio_ok = 0;
static int                     g_audio_tried = 0;

/* ---------- 生命周期 ---------- */
static int dg_audio_open_device(void) {
    /* XAudio2 每个使用它的线程都要求 COM 初始化;已被别的模式初始化过就不管 */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    HMODULE dll = LoadLibraryA("xaudio2_9.dll");
    if (!dll) dll = LoadLibraryA("xaudio2_8.dll");
    if (!dll) { dg_error("cannot load xaudio2_9.dll / xaudio2_8.dll"); return -1; }
    typedef HRESULT (WINAPI *PFN_XAudio2Create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
    PFN_XAudio2Create create = (PFN_XAudio2Create)(void *)GetProcAddress(dll, "XAudio2Create");
    if (!create) { dg_error("xaudio2.dll has no XAudio2Create"); return -1; }
    HRESULT hr = create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa) { dg_error("XAudio2Create failed (hr=0x%08lX)", (unsigned long)hr); g_xa = NULL; return -1; }
    hr = g_xa->lpVtbl->CreateMasteringVoice(g_xa, &g_master, 2, 48000, 0, NULL, NULL,
                                           AudioCategory_GameEffects);
    if (FAILED(hr) || !g_master) {
        dg_error("CreateMasteringVoice failed (hr=0x%08lX) —— 可能没有可用的音频设备", (unsigned long)hr);
        g_xa->lpVtbl->Release(g_xa);
        g_xa = NULL;
        return -1;
    }
    g_master->lpVtbl->SetVolume(g_master, g_master_vol, XAUDIO2_COMMIT_NOW);
    return 0;
}

void dg_audio_init(void) {
    for (int i = 0; i < DG_MAX_SOUNDS; i++) { free(g_sounds[i].data); g_sounds[i].data = NULL; }
    memset(g_sounds, 0, sizeof g_sounds);
    memset(g_voices, 0, sizeof g_voices);
    g_sound_gen = 0;
    g_master_vol = 1.0f;
    g_audio_ok = 0;
    g_audio_tried = 1;
    /* 设备打不开不算致命:记下原因,之后所有播放调用都会明确失败 */
    if (dg_audio_open_device()) g_audio_ok = 0;
    else g_audio_ok = 1;
}

void dg_audio_shutdown(void) {
    for (int i = 0; i < DG_MAX_VOICES; i++) {
        if (g_voices[i].voice) {
            g_voices[i].voice->lpVtbl->Stop(g_voices[i].voice, 0, XAUDIO2_COMMIT_NOW);
            g_voices[i].voice->lpVtbl->DestroyVoice(g_voices[i].voice);
            g_voices[i].voice = NULL;
        }
        g_voices[i].used = 0;
    }
    if (g_master) { g_master->lpVtbl->DestroyVoice(g_master); g_master = NULL; }
    if (g_xa) { g_xa->lpVtbl->Release(g_xa); g_xa = NULL; }
    for (int i = 0; i < DG_MAX_SOUNDS; i++) { free(g_sounds[i].data); g_sounds[i].data = NULL; }
    memset(g_sounds, 0, sizeof g_sounds);
    g_audio_ok = 0;
}

int dg_audio_ok(void) { return g_audio_ok; }

/* ---------- WAV 解析(RIFF/WAVE,PCM 8/16 位)---------- */
static uint32_t dg_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t dg_rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* 解析 WAV;成功时 out_data 指向 malloc 的 PCM(调用方负责 free)*/
static int dg_wav_parse(const char *path, BYTE **out_data, DWORD *out_bytes,
                        WAVEFORMATEX *out_fmt, double *out_duration) {
    FILE *f = dg_fopen_asset(path, "rb");
    if (!f) { dg_error("cannot open sound '%s'", path); return -1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 44) { fclose(f); dg_error("sound '%s' is too small to be a WAV", path); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); dg_error("out of memory reading sound '%s'", path); return -1; }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); dg_error("short read on sound '%s'", path); return -1; }

    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        dg_error("sound '%s' is not a RIFF/WAVE file", path);
        return -1;
    }
    int have_fmt = 0, have_data = 0;
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof fmt);
    uint8_t *pcm = NULL;
    DWORD pcm_bytes = 0;
    size_t off = 12;
    while (off + 8 <= (size_t)n) {
        const uint8_t *id = buf + off;
        uint32_t sz = dg_rd32(buf + off + 4);
        const size_t body = off + 8;
        if (body + sz > (size_t)n) sz = (uint32_t)((size_t)n - body);   /* 容忍截断的块 */
        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            fmt.wFormatTag = (WORD)dg_rd16(buf + body + 0);
            fmt.nChannels = (WORD)dg_rd16(buf + body + 2);
            fmt.nSamplesPerSec = dg_rd32(buf + body + 4);
            fmt.nAvgBytesPerSec = dg_rd32(buf + body + 8);
            fmt.nBlockAlign = (WORD)dg_rd16(buf + body + 12);
            fmt.wBitsPerSample = (WORD)dg_rd16(buf + body + 14);
            fmt.cbSize = 0;
            have_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0 && sz > 0) {
            pcm = (uint8_t *)malloc(sz);
            if (!pcm) { free(buf); dg_error("out of memory copying sound data"); return -1; }
            memcpy(pcm, buf + body, sz);
            pcm_bytes = sz;
            have_data = 1;
        }
        off = body + sz + (sz & 1);          /* 块按偶数字节对齐 */
    }
    free(buf);
    if (!have_fmt) { free(pcm); dg_error("sound '%s' has no 'fmt ' chunk", path); return -1; }
    if (!have_data) { free(pcm); dg_error("sound '%s' has no 'data' chunk", path); return -1; }
    if (fmt.wFormatTag != WAVE_FORMAT_PCM) {
        free(pcm);
        dg_error("sound '%s' is not PCM (format tag %u)", path, (unsigned)fmt.wFormatTag);
        return -1;
    }
    if (fmt.wBitsPerSample != 8 && fmt.wBitsPerSample != 16) {
        free(pcm);
        dg_error("sound '%s' is %u-bit; only 8/16-bit PCM is supported",
                 path, (unsigned)fmt.wBitsPerSample);
        return -1;
    }
    if (fmt.nChannels < 1 || fmt.nChannels > 2) {
        free(pcm);
        dg_error("sound '%s' has %u channels; only mono/stereo is supported",
                 path, (unsigned)fmt.nChannels);
        return -1;
    }
    if (fmt.nSamplesPerSec == 0 || fmt.nBlockAlign == 0) {
        free(pcm);
        dg_error("sound '%s' has a broken fmt chunk", path);
        return -1;
    }
    *out_data = pcm;
    *out_bytes = pcm_bytes;
    *out_fmt = fmt;
    *out_duration = (double)pcm_bytes / (double)fmt.nAvgBytesPerSec;
    return 0;
}

/* ---------- 声音资源(按路径缓存)---------- */
static DgSound *dg_sound_at(int32_t id) {
    if (id <= 0) return NULL;
    const int idx = (id & 0xFFFF) - 1;
    if (idx < 0 || idx >= DG_MAX_SOUNDS) return NULL;
    if (!g_sounds[idx].used || (int32_t)g_sounds[idx].gen != (id >> 16)) return NULL;
    return &g_sounds[idx];
}

int32_t dg_audio_load_cached(const char *path) {
    if (!path || !path[0]) { dg_error("empty sound path"); return -1; }
    for (int i = 0; i < DG_MAX_SOUNDS; i++)
        if (g_sounds[i].used && strcmp(g_sounds[i].path, path) == 0)
            return (int32_t)((g_sounds[i].gen << 16) | (uint32_t)(i + 1));
    for (int i = 0; i < DG_MAX_SOUNDS; i++) {
        if (g_sounds[i].used) continue;
        DgSound s;
        memset(&s, 0, sizeof s);
        if (dg_wav_parse(path, &s.data, &s.bytes, &s.fmt, &s.duration)) return -1;
        s.used = 1;
        s.gen = ++g_sound_gen;
        snprintf(s.path, sizeof s.path, "%s", path);
        g_sounds[i] = s;
        return (int32_t)((s.gen << 16) | (uint32_t)(i + 1));
    }
    dg_error("sound table is full (max %d)", DG_MAX_SOUNDS);
    return -1;
}

int32_t dg_audio_free(int32_t id) {
    DgSound *s = dg_sound_at(id);
    if (!s) { dg_error("sound %d does not exist", id); return -1; }
    for (int i = 0; i < DG_MAX_VOICES; i++)      /* 正在放这个声音的实例先停掉 */
        if (g_voices[i].used && g_voices[i].sound == id && g_voices[i].voice) {
            g_voices[i].voice->lpVtbl->Stop(g_voices[i].voice, 0, XAUDIO2_COMMIT_NOW);
            g_voices[i].voice->lpVtbl->DestroyVoice(g_voices[i].voice);
            g_voices[i].voice = NULL;
        }
    free(s->data);
    s->data = NULL;
    s->used = 0;
    return 0;
}

double dg_audio_duration(int32_t id) {
    const DgSound *s = dg_sound_at(id);
    if (!s) { dg_error("sound %d does not exist", id); return 0.0; }
    return s->duration;
}
int32_t dg_audio_sample_rate(int32_t id) {
    const DgSound *s = dg_sound_at(id);
    if (!s) { dg_error("sound %d does not exist", id); return -1; }
    return (int32_t)s->fmt.nSamplesPerSec;
}
int32_t dg_audio_channels(int32_t id) {
    const DgSound *s = dg_sound_at(id);
    if (!s) { dg_error("sound %d does not exist", id); return -1; }
    return (int32_t)s->fmt.nChannels;
}
int32_t dg_audio_bits(int32_t id) {
    const DgSound *s = dg_sound_at(id);
    if (!s) { dg_error("sound %d does not exist", id); return -1; }
    return (int32_t)s->fmt.wBitsPerSample;
}
const char *dg_audio_path(int32_t id) {
    const DgSound *s = dg_sound_at(id);
    return s ? s->path : "";
}
int32_t dg_audio_count(void) {
    int n = 0;
    for (int i = 0; i < DG_MAX_SOUNDS; i++) if (g_sounds[i].used) n++;
    return n;
}

/* ---------- 播放 ---------- */
static DgVoice *dg_voice_at(int32_t id) {
    if (id <= 0 || id > DG_MAX_VOICES) return NULL;
    if (!g_voices[id - 1].used) return NULL;
    return &g_voices[id - 1];
}

int32_t dg_audio_play(int32_t sound, int loop, float volume) {
    if (!g_audio_ok) { dg_error("audio device is not available (see eng_audio_error)"); return -1; }
    const DgSound *s = dg_sound_at(sound);
    if (!s) { dg_error("sound %d does not exist", sound); return -1; }
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    int slot = -1;
    for (int i = 0; i < DG_MAX_VOICES; i++)
        if (!g_voices[i].used || !g_voices[i].voice) { slot = i; break; }
    if (slot < 0) { dg_error("too many concurrent sounds (max %d)", DG_MAX_VOICES); return -1; }

    IXAudio2SourceVoice *v = NULL;
    HRESULT hr = g_xa->lpVtbl->CreateSourceVoice(g_xa, &v, &s->fmt, 0,
                                                XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr) || !v) { dg_error("CreateSourceVoice failed (hr=0x%08lX)", (unsigned long)hr); return -1; }
    v->lpVtbl->SetVolume(v, volume, XAUDIO2_COMMIT_NOW);

    XAUDIO2_BUFFER buf;
    memset(&buf, 0, sizeof buf);
    buf.AudioBytes = s->bytes;
    buf.pAudioData = s->data;
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    hr = v->lpVtbl->SubmitSourceBuffer(v, &buf, NULL);
    if (FAILED(hr)) {
        v->lpVtbl->DestroyVoice(v);
        dg_error("SubmitSourceBuffer failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    hr = v->lpVtbl->Start(v, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        v->lpVtbl->DestroyVoice(v);
        dg_error("voice Start failed (hr=0x%08lX)", (unsigned long)hr);
        return -1;
    }
    g_voices[slot].used = 1;
    g_voices[slot].sound = sound;
    g_voices[slot].voice = v;
    return slot + 1;
}

int32_t dg_audio_stop(int32_t voice) {
    DgVoice *v = dg_voice_at(voice);
    if (!v) { dg_error("voice %d does not exist", voice); return -1; }
    if (v->voice) {
        v->voice->lpVtbl->Stop(v->voice, 0, XAUDIO2_COMMIT_NOW);
        v->voice->lpVtbl->FlushSourceBuffers(v->voice);
        v->voice->lpVtbl->DestroyVoice(v->voice);
        v->voice = NULL;
    }
    return 0;
}

int32_t dg_audio_voice_playing(int32_t voice) {
    DgVoice *v = dg_voice_at(voice);
    if (!v) return 0;
    if (!v->voice) return 0;
    XAUDIO2_VOICE_STATE st;
    memset(&st, 0, sizeof st);
    v->voice->lpVtbl->GetState(v->voice, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return st.BuffersQueued > 0 ? 1 : 0;
}

int32_t dg_audio_voice_set_volume(int32_t voice, float volume) {
    DgVoice *v = dg_voice_at(voice);
    if (!v || !v->voice) { dg_error("voice %d is not playing", voice); return -1; }
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    v->voice->lpVtbl->SetVolume(v->voice, volume, XAUDIO2_COMMIT_NOW);
    return 0;
}

int32_t dg_audio_voice_set_loop(int32_t voice, int loop) {
    DgVoice *v = dg_voice_at(voice);
    if (!v || !v->voice) { dg_error("voice %d is not playing", voice); return -1; }
    /* XAudio2 不支持改已提交缓冲的循环次数:要改就重放(调用方自己 re-play)*/
    (void)loop;
    dg_error("loop cannot be changed on a playing voice; re-play it instead");
    return -1;
}

int32_t dg_audio_voice_sound(int32_t voice) {
    DgVoice *v = dg_voice_at(voice);
    return v ? v->sound : 0;
}

int32_t dg_audio_playing_count(void) {
    int n = 0;
    for (int i = 0; i < DG_MAX_VOICES; i++) if (dg_audio_voice_playing(i + 1)) n++;
    return n;
}

int32_t dg_audio_stop_all(void) {
    for (int i = 0; i < DG_MAX_VOICES; i++)
        if (g_voices[i].voice) dg_audio_stop(i + 1);
    return 0;
}

int32_t dg_audio_set_master_volume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    g_master_vol = volume;
    if (g_master) g_master->lpVtbl->SetVolume(g_master, volume, XAUDIO2_COMMIT_NOW);
    return 0;
}
float dg_audio_master_volume(void) { return g_master_vol; }
