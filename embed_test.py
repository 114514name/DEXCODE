import ctypes
import time
import tkinter as tk
import threading

DLL_PATH = r"C:\Users\ASUS\Desktop\Code\DEXCODE\libs\gal\libdexxgal.dll"
user32 = ctypes.windll.user32

ll = ctypes.c_longlong
cp = ctypes.c_char_p

print('Loading DLL...')
L = ctypes.CDLL(DLL_PATH)
L.gal_init.restype = ll; L.gal_init.argtypes = [ll, ll]
L.gal_close.restype = ll; L.gal_close.argtypes = []

# create tkinter window
root = tk.Tk()
root.title('Embed Test Host')
root.geometry('1000x700')
root.update()
parent_hwnd = int(root.winfo_id())
print('Tk parent HWND:', parent_hwnd)

def run_engine_and_embed():
    print('Calling gal_init...')
    rc = L.gal_init(960, 540)
    print('gal_init rc=', rc)
    # enum windows
    EnumWindows = user32.EnumWindows
    GetClassNameW = user32.GetClassNameW
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def find():
        result = []
        def enum_proc(h, l):
            buf = ctypes.create_unicode_buffer(64)
            GetClassNameW(h, buf, 64)
            if buf.value == 'DexGALWindow':
                result.append(h)
                return False
            return True
        EnumWindows(WNDENUMPROC(enum_proc), 0)
        return result[0] if result else None
    hw = None
    for i in range(40):
        hw = find()
        print('attempt', i, 'hw=', hw)
        if hw:
            break
        time.sleep(0.05)
    if not hw:
        print('DexGALWindow not found')
        return
    print('Found DexGALWindow', hw)
    # set parent
    GWL_STYLE = -16
    WS_CHILD = 0x40000000
    WS_POPUP = 0x80000000
    WS_CAPTION = 0x00C00000
    WS_THICKFRAME = 0x00040000
    user32.SetParent(ctypes.c_void_p(hw), ctypes.c_void_p(parent_hwnd))
    style = user32.GetWindowLongPtrW(ctypes.c_void_p(hw), GWL_STYLE)
    newstyle = (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME)) | WS_CHILD
    user32.SetWindowLongPtrW(ctypes.c_void_p(hw), GWL_STYLE, newstyle)
    user32.ShowWindow(ctypes.c_void_p(hw), 5)
    print('Embedded and shown')
    time.sleep(3)
    print('Closing engine')
    L.gal_close()

# run in thread so tkinter mainloop stays responsive
t = threading.Thread(target=run_engine_and_embed)
t.start()
root.mainloop()
print('Tkinter mainloop exited')
