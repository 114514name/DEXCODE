"""galide.preview — 引擎预览窗口(嵌入 tkinter 控件)。

引擎 gal_init 创建原生窗口(DexGALWindow),通过 SetParent 嵌入 tkinter 控件;
由 UI 层每 16ms 调 poll() 驱动渲染。引擎自身双缓冲 BitBlt,父窗口(tkinter)
不会每帧全量覆盖子窗口,因此不会出现 C 版那种父子绘制竞争的闪烁。
"""

import ctypes
import time, os

from .model import (MAX_SPR, BL_SPEAK, BL_BG, BL_SPRITE, BL_CLEAR,
                    block_end)
from .export import _block_spr_path

user32 = ctypes.windll.user32

GWL_STYLE = -16
WS_CHILD = 0x40000000
WS_POPUP = 0x80000000
WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000
SWP_NOZORDER = 0x0004
SWP_FRAMECHANGED = 0x0020
SW_SHOW = 5
HWND_TOP = 0


class Preview:
    def __init__(self, parent_widget, engine):
        """parent_widget: 承载引擎窗口的 tkinter 控件(Frame/Canvas)。"""
        self.parent = parent_widget
        self.engine = engine
        self.hwnd = None
        self.active = False
        self._embedded = False
        self._last_err = ""
        self._lx = self._ly = self._lw = self._lh = -1

    @staticmethod
    def _find_gal():
        """枚举所有顶层窗口,按类名找 DexGALWindow。"""
        result = []
        WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        def _enum(h, _):
            buf = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(h, buf, 64)
            if buf.value == "DexGALWindow":
                result.append(h)
                return False  # 找到一个就停
            return True
        user32.EnumWindows(WNDENUMPROC(_enum), 0)
        return result[0] if result else None

    @property
    def parent_hwnd(self):
        try:
            return int(self.parent.winfo_id())
        except Exception:
            return 0

    def start(self):
        """初始化引擎并嵌入窗口。返回是否成功。

        额外：把关键步骤与每次查找记录到日志文件 logs/preview_debug.log，便于排查在应用中无法找到窗口的原因。
        """
        if self.active:
            return True
        log_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), '..', 'logs')
        try:
            os.makedirs(log_path, exist_ok=True)
        except Exception:
            log_path = os.path.dirname(os.path.dirname(__file__))
        log_file = os.path.join(log_path, 'preview_debug.log')

        def llog(s):
            try:
                with open(log_file, 'a', encoding='utf-8') as f:
                    f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {s}\n")
            except Exception:
                pass

        llog('--- preview.start ---')
        llog(f'dll={self.engine.dll_path} loaded={self.engine.loaded}')

        # 确保引擎 DLL 已加载
        if not self.engine.loaded:
            if not self.engine.load():
                self._last_err = "DLL 加载失败: " + self.engine.dll_path
                llog('DLL load failed')
                return False
            llog('DLL loaded by engine.load()')

        # 清理可能残留的引擎窗口(上次半初始化导致)
        hwnd_stale = self._find_gal()
        llog(f'hwnd_stale={hwnd_stale}')
        if hwnd_stale:
            try:
                user32.DestroyWindow(ctypes.c_void_p(hwnd_stale))
                llog('Destroyed stale window')
            except Exception as e:
                llog(f'DestroyWindow failed: {e}')

        # 初始化引擎窗口
        rc = self.engine.init(960, 540)
        llog(f'engine.init rc={rc}')
        if rc == 0:
            self._last_err = f"引擎窗口初始化失败(可能是 DLL 加载不完整) 返回码: {rc}"
            llog('init returned 0 -> failure')
            return False

        # 用 EnumWindows 枚举找 DexGALWindow,比 FindWindowW 更可靠
        # 有时窗口创建稍有延迟，尝试多次查找
        self.hwnd = None
        max_attempts = 30
        for attempt in range(max_attempts):
            self.hwnd = self._find_gal()
            llog(f'attempt {attempt+1}: hwnd={self.hwnd}')
            if self.hwnd:
                break
            time.sleep(0.05)

        if not self.hwnd:
            # 额外把当前所有顶层窗口类名写入日志以便排查
            try:
                names = []
                WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
                def _enum_all(h, _):
                    buf = ctypes.create_unicode_buffer(128)
                    user32.GetClassNameW(h, buf, 128)
                    names.append((int(h), buf.value))
                    return True
                user32.EnumWindows(WNDENUMPROC(_enum_all), 0)
                llog('Top-level windows snapshot:')
                for hh, cn in names:
                    llog(f'  HWND={hh} class="{cn}"')
            except Exception as e:
                llog(f'EnumWindows snapshot failed: {e}')

            # 附加更多诊断信息到错误字符串，便于定位问题
            self._last_err = (
                f"引擎窗口创建后丢失(DexGALWindow 未找到) after {attempt+1} attempts; "
                f"dll={self.engine.dll_path}, loaded={self.engine.loaded}, init_rc={rc}"
            )
            llog(f'FAILED: {self._last_err}')
            try:
                self.engine.close()
            except Exception as e:
                llog(f'engine.close() failed: {e}')
            return False

        llog(f'Found DexGALWindow hwnd={self.hwnd}')
        # 仅首次启动时嵌入;再次启动只需 show + resize
        if not self._embedded:
            # 尝试等待 parent_hwnd 就绪(非 0)，这是 tkinter 在某些情形下才会在下一次循环中创建
            parent_hwnd = self.parent_hwnd
            parent_waited = 0
            if not parent_hwnd:
                for pw in range(10):
                    time.sleep(0.02)
                    parent_hwnd = self.parent_hwnd
                    if parent_hwnd:
                        break
                parent_waited = pw + 1
            llog(f'parent_hwnd={parent_hwnd} waited={parent_waited}')
            try:
                self.engine.set_title("GAL 预览")
                if parent_hwnd:
                    # 尝试嵌入
                    try:
                        user32.SetParent(ctypes.c_void_p(self.hwnd), ctypes.c_void_p(parent_hwnd))
                        style = user32.GetWindowLongPtrW(ctypes.c_void_p(self.hwnd), GWL_STYLE)
                        newstyle = (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME)) | WS_CHILD
                        user32.SetWindowLongPtrW(ctypes.c_void_p(self.hwnd), GWL_STYLE, newstyle)
                        self._embedded = True
                        llog('SetParent and styles applied')
                    except Exception as e:
                        # 嵌入失败：记录但不把其视作完全失败，改为保持为独立窗口（fallback）
                        llog(f'Embed attempt failed, falling back to standalone window: {e}')
                        self._embedded = False
                        self._last_err = f'嵌入窗口失败，使用独立窗口: {e}'
                else:
                    # 无 parent_hwnd，无法嵌入，使用独立窗口
                    llog('No parent_hwnd available, using standalone engine window')
                    self._embedded = False
            except Exception as e:
                llog(f'embed outer failed: {e}')
                self._embedded = False
                self._last_err = f'嵌入流程失败，使用独立窗口: {e}'
        # 如果嵌入失败，也不要把预览当作完全失败；作为降级方案，显示独立窗口
        self._last_err = self._last_err or ""
        try:
            user32.ShowWindow(ctypes.c_void_p(self.hwnd), SW_SHOW)
            self.active = True
            llog(f'ShowWindow OK and active=True (embedded={self._embedded})')
        except Exception as e:
            llog(f'ShowWindow failed: {e}')
            self._last_err = f'ShowWindow 失败: {e}'
            return False
        return True

    def resize(self, x, y, w, h):
        """重新定位引擎窗口(位置未变则跳过,避免触发无谓重绘)。"""
        if not self.active or not self.hwnd:
            return
        if (x, y, w, h) == (self._lx, self._ly, self._lw, self._lh):
            return
        user32.SetWindowPos(ctypes.c_void_p(self.hwnd), HWND_TOP,
                            x, y, w, h, SWP_NOZORDER)
        self._lx, self._ly, self._lw, self._lh = x, y, w, h

    def poll(self):
        if self.active:
            self.engine.poll()

    def close(self):
        """关闭预览(仅隐藏引擎窗口,不销毁;可再次 start 恢复)。"""
        if self.active and self.hwnd:
            user32.ShowWindow(ctypes.c_void_p(self.hwnd), 0)  # SW_HIDE
            self.active = False

    def destroy(self):
        """彻底销毁引擎(应用退出时调用)。"""
        if self.hwnd:
            try:
                self.engine.close()
            except Exception:
                pass
            self.active = False
            self._embedded = False
            self.hwnd = None

    # ---- 预览状态同步 ----
    def apply_scene(self, p, sc):
        """应用镜头级:背景 + 镜头级立绘资源(与 C 版 apply_scene_preview 对齐)。"""
        if not self.active:
            return
        eng = self.engine
        if sc.useBgImg and sc.bg:
            if eng.bg(sc.bg) != 0:
                eng.bg_color(sc.bgColor)
        else:
            eng.bg_color(sc.bgColor)
        for k in range(MAX_SPR):
            if sc.spr[k]:
                if eng.sprite(k, sc.spr[k]) == 0:
                    eng.sprite_scale(k, 100)
                    eng.sprite_pos_mode(k, 0)
                    eng.sprite_show(k, 0)
            else:
                eng.sprite_show(k, 0)
        self.poll()

    def apply_block(self, p, sc, b):
        """应用单个指令块到预览(与 C 版 apply_block_preview 对齐)。"""
        if not self.active:
            return
        eng = self.engine
        if b.type == BL_SPEAK:
            eng.text_clear()
            if b.speaker:
                eng.speaker(b.speaker)
            eng.box_style(b.style_avatar, b.style_name)
            eng.box_autofit(b.auto_fit)
            eng.text_pos(b.text_pos)
            if b.text:
                eng.text(b.text)
        elif b.type == BL_BG:
            if b.useBgImg and b.bg:
                if eng.bg(b.bg) != 0:
                    eng.bg_color(b.bgColor)
            else:
                eng.bg_color(b.bgColor)
        elif b.type == BL_SPRITE:
            k = b.spr_layer
            if k < 0 or k >= MAX_SPR:
                k = 0
            wp = _block_spr_path(p, sc, b)
            if wp:
                if eng.sprite(k, wp) == 0:
                    eng.sprite_show(k, b.spr_show)
                    eng.sprite_pos_mode(k, b.spr_pos)
                    eng.sprite_autofit(k, b.spr_autofit)
                    eng.sprite_anim(k, b.spr_anim)
        elif b.type == BL_CLEAR:
            eng.text_clear()
        self.poll()

    def play_block_audio(self, b):
        """试听音频块(不阻塞)。"""
        if not self.active:
            return
        eng = self.engine
        if b.type == BL_AUDIO:
            if b.audio_type == 0 and b.audio_path:
                eng.play_bgm(b.audio_path)
            elif b.audio_type == 1 and b.audio_path:
                eng.play_se(b.audio_path)
            elif b.audio_type == 2:
                eng.stop_bgm()
            elif b.audio_type == 3:
                eng.volume(b.volume)
