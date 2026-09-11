import ctypes
import time
import sys

DLL_PATH = r"C:\Users\ASUS\Desktop\Code\DEXCODE\libs\gal\libdexxgal.dll"
print('DLL path:', DLL_PATH)
if not ctypes.windll.kernel32.GetFileAttributesW(DLL_PATH):
    # GetFileAttributesW returns INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF on error
    pass

try:
    L = ctypes.CDLL(DLL_PATH)
    print('CDLL loaded OK')
except Exception as e:
    print('Failed to load DLL:', e)
    sys.exit(1)

# setup prototypes
ll = ctypes.c_longlong
cp = ctypes.c_char_p
try:
    L.gal_init.restype = ll
    L.gal_init.argtypes = [ll, ll]
except Exception:
    pass

rc = L.gal_init(960, 540)
print('gal_init rc:', rc)

# enumerate windows looking for DexGALWindow
user32 = ctypes.windll.user32
EnumWindows = user32.EnumWindows
GetClassNameW = user32.GetClassNameW

WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

def find_gal_once():
    result = []
    def enum_proc(h, lparam):
        buf = ctypes.create_unicode_buffer(64)
        GetClassNameW(h, buf, 64)
        if buf.value == 'DexGALWindow':
            result.append(h)
            return False
        return True
    EnumWindows(WNDENUMPROC(enum_proc), 0)
    return result[0] if result else None

print('Searching for DexGALWindow with retries...')
found = None
for i in range(40):
    found = find_gal_once()
    if found:
        print('Found DexGALWindow HWND:', found)
        break
    time.sleep(0.05)

if not found:
    print('DexGALWindow not found after retries')
else:
    print('Sleeping 2s to keep window alive...')
    time.sleep(2)

# try calling gal_close if exists
try:
    L.gal_close()
    print('gal_close called')
except Exception as e:
    print('gal_close failed:', e)

print('Done')
