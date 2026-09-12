@echo off
REM ============================================================
REM  build_libs.bat - build all native libraries (libs/*/lib*.dll)
REM
REM  Why this exists: only prebuilt lib*.dll used to be shipped and no
REM  build script existed for them, so the binaries had to be committed
REM  (otherwise a fresh clone cannot run any example or test). This
REM  script makes libs/*/lib*.dll reproducible from source.
REM
REM  The authoritative build command for each library is the comment at
REM  the top of its .c file.
REM
REM  NOTE: this file is deliberately ASCII-only. cmd.exe parses .bat
REM  files using the OEM code page, so UTF-8 text in a batch file turns
REM  into garbage commands. Keep it ASCII; put docs in README instead.
REM ============================================================
setlocal

set ROOT=%~dp0
cd /d "%ROOT%"

REM zig needs a writable cache; keep it inside the workspace so that
REM restricted/sandboxed environments do not fail on %LOCALAPPDATA%\zig
REM
REM Always start from a clean cache: a previously failed link can be
REM reused by zig and keep reporting the old undefined-symbol error even
REM after the flags are fixed.
set ZIGCACHE=%ROOT%_zigcache
if exist "%ZIGCACHE%" rmdir /s /q "%ZIGCACHE%" 2>nul
mkdir "%ZIGCACHE%" 2>nul
set ZIG_GLOBAL_CACHE_DIR=%ZIGCACHE%
set ZIG_LOCAL_CACHE_DIR=%ZIGCACHE%
if not exist "%ROOT%_zigtmp" mkdir "%ROOT%_zigtmp"
set TMP=%ROOT%_zigtmp
set TEMP=%ROOT%_zigtmp

REM ---- locate zig (PATH first, then the pip ziglang package) ----
set ZIG=
for %%p in (zig.exe) do if not defined ZIG set ZIG=%%~$PATH:p
if not defined ZIG (
  for /f "delims=" %%p in ('python -c "import ziglang,pathlib;print(pathlib.Path(ziglang.__file__).parent/'zig.exe')" 2^>nul') do set ZIG=%%p
)
if not defined ZIG (
  echo [ERROR] zig not found. Install it with: pip install ziglang
  exit /b 1
)
echo Using zig: %ZIG%
echo.

set COMMON=-shared -target x86_64-windows-gnu -O2 -std=c11

echo === building native libraries ===

echo [1/6] libs\math\libdexmath.dll
"%ZIG%" cc %COMMON% -o libs\math\libdexmath.dll libs\math\libdexmath.c || exit /b 1

echo [2/6] libs\std\libdexstd.dll
"%ZIG%" cc %COMMON% -o libs\std\libdexstd.dll libs\std\libdexstd.c || exit /b 1

echo [3/7] libs\img\libdeximg.dll
"%ZIG%" cc %COMMON% -o libs\img\libdeximg.dll libs\img\libdeximg.c || exit /b 1

echo [4/7] libs\ui\libdexui.dll
"%ZIG%" cc %COMMON% -o libs\ui\libdexui.dll libs\ui\libdexui.c || exit /b 1

echo [5/7] libs\egui\libegui.dll
"%ZIG%" cc %COMMON% -lcomctl32 -luser32 -lgdi32 -lwinmm -lshell32 -o libs\egui\libegui.dll libs\egui\libegui.c || exit /b 1

echo [6/7] libs\gal\libdexxgal.dll
"%ZIG%" cc %COMMON% -lgdi32 -luser32 -lwinmm -lmsimg32 -o libs\gal\libdexxgal.dll libs\gal\libdexxgal.c || exit /b 1

REM dexgame = modular 2D game engine (M1: D3D11 renderer).
REM NOTE 1: no d3dcompiler import library is needed -- shaders are compiled at
REM   runtime by loading d3dcompiler_47.dll (a Windows system DLL) via LoadLibrary.
REM   (zig ships d3d11/dxgi/dwrite import libs but NOT d3dcompiler; see dg_draw.c)
REM NOTE 2: -Wl,--out-implib is required here. lld emits an import library for a
REM   -shared link and names it after the FIRST input file, so a multi-file build
REM   would drop "dg_gfx.lib" into the CURRENT DIRECTORY. The 6 single-file libs
REM   get "lib<name>.lib" next to their DLL instead (and those .lib files are
REM   committed); dexgame does not need an import library at all, so send it to
REM   the gitignored _zigtmp\ instead of polluting the tree.
echo [7/7] libs\dexgame\libdexgame.dll
"%ZIG%" cc %COMMON% -I libs\dexgame -ld3d11 -ldxgi -luser32 -lgdi32 -lole32 -luuid -lwinmm ^
  -Wl,--out-implib=_zigtmp\dexgame.lib ^
  -o libs\dexgame\libdexgame.dll ^
  libs\dexgame\dg_gfx.c libs\dexgame\dg_draw.c libs\dexgame\dg_api.c || exit /b 1

echo.
echo Done. Rebuilt 7 native libraries.
echo Cache dirs _zigcache\ and _zigtmp\ are excluded by .gitignore.
endlocal
