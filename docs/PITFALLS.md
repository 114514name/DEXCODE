# DEXCODE 已知陷阱(踩过并记录)

> 这份表从 `AGENTS.md` §4 搬出来(AGENTS.md 太长会超出工作区指令预算被截断)。
> 每完成一个阶段都会往里加行;新>旧,最近的在最下面。
> **改代码前扫一眼这里** —— 每一条都是真踩过的,重复踩的代价通常远大于读它的时间。

| 陷阱 | 现象 | 应对 |
|------|------|------|
| zig 缓存复用失败的链接 | 修好编译参数后仍报旧的 `undefined symbol` | 构建前清空 `_zigcache/`;`build_libs.bat` 已内置 |
| sandbox/受限环境写 zig 缓存被拒 | `failed to create output directory ...AccessDenied` —— 报错**像编译器坏了或源码有问题**,其实只是 zig 的默认缓存(`%LOCALAPPDATA%\zig\tmp`)与 `%TEMP%` 在工作区外、写不进去 | `build_libs.bat` 与 `main.py build-vm`(内部 `_compiler_env()`)都已把 `ZIG_GLOBAL_CACHE_DIR`/`TMP` 指向工作区内的 `_zigcache/` 与 `_zigtmp/`;只有**手写** zig 命令时才需自己设。注意 `main.py` 直到 `e1c0a83` 才补上,此前同一环境下「库能构建、VM 不能构建」 |
| 测试临时文件重名 | 4 个测试文件曾共用 `_tmp_test.dexbc`,互相覆盖导致 VM 读到截断文件而随机崩溃 | 各测试文件使用带唯一后缀的路径(已修) |
| `str.endswith("([{")` | 参数是**后缀串**不是字符集合,该判断恒为假(曾让"行尾 `{` 自动缩进"从未生效) | 用「末字符 ∈ 集合」判断 |
| `tag_configure` 放在 `tag_add` 之后 | 配置不作用于已打区间(折叠的 `elide` 曾完全失效) | 先 `tag_configure` 再 `tag_add` |
| Tk 全局 mark 多用途复用 | 多个折叠块共用一个 mark,后者覆盖前者,导致展开时删不掉提示 | 按行使用唯一 tag 定位 |
| `Text.get("1.0","end-1c")` 与 `elide` | 折叠后仍返回全文(这是 Tk 设计,便于保存) | 判断是否隐藏要看 `tag_ranges`/`dlineinfo`,不是 `get` |
| **`tempfile.mkdtemp` 的 `0o700` ACL**(最坑的一条) | 受限沙箱(只放行工作区写入)下,`mkdtemp`/`TemporaryDirectory` 建出的目录**内部既不能建子目录也不能写文件**,连 `shutil.rmtree` 都被拒;最外层只看到 `PermissionError: [WinError 5] 拒绝访问` / `[Errno 13] Permission denied`,看不出病因。曾让 `test_bluedit`/`test_designer`/`test_module`/`test_vm_versions` 四个脚本 rc=1 | `mkdtemp` 内部是 `os.mkdir(p, 0o700)`。实测同一父目录下:`0o700` FAIL / `0o777` OK / 默认 OK。测试统一走 `tests/_tmpdir.py`(以 `0o777` 建目录)。见 §3.4 |
| 库函数失败只返回空值 | `bluedit.export.export_project` 失败时返回 `""`,裸调用后 `open(path)` 抛 `FileNotFoundError: ''`,把真实原因(「无法创建输出目录 …」)彻底吞掉 —— 排查时完全不知从何下手 | 测试改用 `_export()` 包装,失败时抛出 `export.last_error` 的内容。**写同类 API 时:失败要带得出原因,不要只返回空值**(库侧早已把原因记在 `last_error`,是调用方丢了它) |
| `tests/test_editor.py` 驱动的是哪份 exe | 该测试曾指向**不存在**的 `tools/galedit.exe`(实际在 `tools/legacy_galedit_c/`),两个用例永远 skip,`check(..., True)` 是死代码 | 已修:路径更正 + `.gitignore` 放行该 exe + exe 入库(否则 clone 后仍缺文件)。现为 18 项有效断言 |
| **`github.com:22` 被对端切断** | `git push` 报 `Connection closed by <ip> port 22`。注意这**不是** `Permission denied (publickey)` —— 用 `Test-NetConnection` 测 TCP 是通的,极容易被误判成「缺密钥/仓库不存在」而白折腾 | 走 GitHub 官方备用通道:在 `~/.ssh/config` 写 `Host github.com` / `HostName ssh.github.com` / `Port 443` / `User git`。验证:`ssh -T git@github.com` 应回 `Hi <用户名>! You've successfully authenticated` |
| 受限沙箱阻断 Git 出站 | 沙箱(仅工作区写)下 `git ls-remote`/`push` 报 `sh.exe: couldn't create signal pipe, Win32 error 5`,HTTPS 则报 `schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS` —— 看起来像 Git 坏了或没凭据,其实只是沙箱不放行出站 | 这两条都是**沙箱**造成,不是 Git 或凭据问题;提权后同一命令即恢复正常。诊断时先用 `git ls-remote <url>` 判定「通道是否通」「仓库是否存在」 |
| **常量池把 int `N` 与 float `N.0` 合并**(M0.5 时发现并修复) | `assembler.cidx()` 原先**只按值**当键,踩中 Python 的 `0 == 0.0` 且 `hash(0) == hash(0.0)`,于是 `print 0;` 与 `print 0.0;` 共用同一条常量,后出现的那一个拿到**错误的类型标签**。潜伏很久没暴露:直接 ABI 会按声明的参数类型做 int/float 互转,VM 的算术也对两者很宽容 | 键改成 `("int"|"float"|"str", value)`。值数组 ABI 让**类型标签变得可观测**,这个 bug 才浮出来 —— 是新 ABI 的**前置修复**。回归测试:`test_abi.py::test_const_pool_types` 与 `abi_tags` 断言 |
| 新 ABI 只在「类型标签可观测」时才暴露老 bug | 双 VM 一致性断言把上面那条抓了出来(`arr_greet("", 0, 0.0)` 的 `0.0` 在 C VM 成了 int) | 加 ABI/内存布局类改动时,**必须**同时加双 VM 逐行对比断言,否则 pyvm 与 C VM 会静默分叉 |
| **`-ld3dcompiler` 链不上**(M1) | zig 只提供 d3d11/dxgi/dwrite 等少数 Windows 导入库,`d3dcompiler` **没有**:`error: unable to find dynamic system library 'd3dcompiler'` | 运行时 `LoadLibraryA("d3dcompiler_47.dll")` + `GetProcAddress("D3DCompile")`(系统自带该 DLL)。反而更自包含 —— 不依赖任何导入库。见 `dg_draw.c` 的 `dg_d3dcompile()` |
| **`IID_xxx` 未定义**(M1) | 只 include 头文件时 `lld-link: error: undefined symbol: IID_IDWriteFactory` —— 因为 GUID 符号本应由导入库提供,而 zig 不提供 dwrite/dxgi 的导入库 | 在 `libs/dexgame/dg_guids.h` 里**显式写死**用到的那几个 GUID(比 `#include <initguid.h>` 更可预测,只需付出实际用到的) |
| **库往 stdout 打印**(M1) | 引擎初始化时 `printf` 一行 GPU 信息,结果把游戏的输出污染了 —— `test_dexgame.py` 的按行断言全部错位(5 项失败),而 DLL 本身完全正常 | **库不写 stdout**。改成按需查询(`eng_gpu_name()`/`eng_vram_mb()`)。写任何库时都该这样:输出是调用方的地盘 |
| **图集边缘渗漏**(M1) | 单纹素源用 `u0..u1` 覆盖整个纹素时,线性过滤会掺进邻居(实测纯绿变成 `0xff04ff04`) | 半纹素内缩:单纹素源直接用**纹素中心**(退化区间 `u0==u1`),即 `u=(x+0.5)/W`。`test_dexgame.py::test_atlas_png` 锁住这个约定 —— 将来做图集助手时要在库里自动完成内缩 |
| 引擎宿主进程的 DPI 只能设一次 | `SetProcessDpiAwarenessContext` 必须在**创建任何窗口之前**调用,且清单设过就再设会失败 | `dg_gfx_init_window` 里尽早调用并**忽略失败**(E_ACCESSDENIED 不算错);客户区尺寸要用 `AdjustWindowRectExForDpi` 算,否则 150% 缩放下会偏小 |
| **`zig cc -shared` 会往当前目录丢 import library**(M1) | lld 为 `-shared` 链接生成导入库,且**按第一个输入文件命名**:多文件的 dexgame 于是把 `dg_gfx.lib` 扔在了**仓库根**(单文件的 6 个库则是 `libs/*/lib<名>.lib`,已入库)。它不被 gitignore 覆盖,污染工作树 | 传 `-Wl,--out-implib=_zigtmp\dexgame.lib` 把它引到 gitignored 目录(`build_libs.bat` 已这么做)。我们运行时用 LoadLibrary,不需要导入库 |
| **DLL 重建不可复现,exe 可以**(M1) | 连续两次 `build_libs.bat` 编出的 DLL 哈希不同:PE `TimeDateStamp` 每次都变,设 `SOURCE_DATE_EPOCH` 也无效(实测)。而 `build-vm` 的三个 exe 逐字节可复现 | 重建库文件后出现二进制 diff 是**正常的**,与代码改动无关;判断"某次改动是否影响二进制"只能靠 exe 那边。见 §2 构建小节 |
| **游标式 JSON 迭代器的契约**(M2) | `djr_key`/`djr_arr_more` **只报告"到头了",不代劳收尾**;而且**必须自己消费元素之间的逗号**。第一版两处都写错(声明"不消费 `}`/`]`"却让调用方再收尾;数组迭代器不消费 `,`),症状是 `scene JSON: line 25: expected '{'` —— 报错点离病因十万八千里 | 契约写在 `dg_json.h` 的注释里,调用方必须调 `djr_obj_end`/`djr_arr_end`。**报错信息带上"实际看到的字符"**(`expected '{', found ','`)后一眼定位。片切数组时 `objs_len` 必须**包含**收尾的 `]`,否则子解析器的 `djr_arr_end` 找不到它 |
| **跳过未知值时不能借 `djr_string`**(M2) | `djr_skip_value` 里用 `djr_string(r, dump, sizeof dump)`(cap=2)跳过字符串,于是**任何长度 ≥ 2 的未知字符串字段**都让整个场景加载失败:`scene JSON: string too long (cap 2)`。向前兼容测试(未知字段值 `"ignored"`)当场抓住 | 单独写一个**不复制内容**的 `djr_skip_string`。规则:`skip` 系列永远不能依赖任何固定缓冲 |
| `dg_error` 是**覆盖**式 | 失败路径上后写的 `dg_error` 会盖掉前面更具体的原因(如"贴图路径不存在"),用户只看到含糊的上层消息 | 只在**原因还空着**时兜底:`if (!dg_last_error()[0]) dg_error(...)`。判断"某个失败是否带得出原因"只能靠测试断言原因内容,不能只看返回码 |
| **引擎自己算 uv 时也必须遵守单纹素规则**(M2) | M1 只在 `eng_draw_uv`(调用方给 uv)上定了这条,但 `eng_draw_scene` 从 Sprite 的源矩形算 uv 时又踩了一遍:单纹素精灵放大 4 倍后纯绿变成 `0xFF20FF20`(红色渗进来) | 规则落到引擎内部:跨多纹素用**区域边界**,单纹素(`sw/sh ≤ 1`)退化到**纹素中心**。`test_scene.py::test_sprite_texel_rule` 锁住并实测了两者差异(边界 uv 得 `0xFF20FF20`,纹素中心得 `0xFF00FF00`) |
| **JSON 里的 `parent` 是场景内索引,不是实体 id**(M2) | 实体 id 高位是世代号。写场景时拿运行时 id 直接和槽位数比较 → `parent` 恒写成 `-1`,层级在往返后**静默丢失**(只有像素断言才发现) | 写:先给活实体编 `index_of[]`,用 `dg_ent_idx(id)` 取出索引再查表。读:`on_load` 钩子经 `id_map` 把索引重映射为**新的**实体 id。`test_scene.py::test_json_shape` 同时断言"是索引 0"和"不是实体 id" |
| 加载失败会留下半成品场景(M2) | `dg_scene_from_json` 直接返回 -1,但已经创建的实体/组件留在池子里 —— 调用方拿到失败却又多出一堆东西(测试里就出现过"加载失败但计数 = 1") | `dg_scene_parse`(内部)+ `dg_scene_from_json`(外壳:失败即 `dg_object_clear()`)。**注册表/加载器一类的 API 都要这样:失败后状态必须回到调用前** |
| **字面量精度会打乱固定步长**(M3) | 用 `eng_physics_step(0.008333333)` 时,这个**十进制字面量比 1/120 略小**(差 3.3e-10),累加器每几次调用就少跑一个子步 —— 实测第 4 次调用起 `eng_physics_substeps()` 出现 0,速度积分少一步(0.9 而不是 1.0)。**症状极像物理算错了** | 测试里写 `1.0 / 120.0`(精确)。推论:凡是"按固定 dt 喂步进"的测试都不要写小数近似;引擎侧另有 `1e-9` 容差兜底 |
| **浮点返回值写成 `int64_t` 会返回垃圾**(M3) | `int64_t eng_physics_alpha(){ return (double)...; }` —— 双精度的位模式被当 float 读,调用方拿到 `5.2e-315`。**DexLang 侧完全看不出是签名错**,只看到数字离谱 | 值数组 ABI 只规定**入参**是 `DexValue` 数组,**返回值仍按 `.dexdef` 声明**:`-> float` 必须 `double` 返回。加浮点接口时逐个核对 `.dexdef` 与 C 签名 |
| **子步上限必须小于最薄的障碍**(M3) | 子步上限 32px 时,一个 20px 的位移会**整个跳过 16px 的瓦片墙**(只在终点做重叠判定)→ 玩家穿墙。这是"离散碰撞 + 大步长"的经典坑 | 上限改成 **4px**(比常见瓦片薄一半),`move-and-slide` 内部自动拆步;代价是快物体多几次解算。极快的投射物应该用 `eng_raycast`/`eng_sweep_box` |
| **瓦片 CSV 的解析行距与读取行距必须一致**(M3) | 解析时为了扩容按 `cap_cols`(64)当行距写,存储后读取端按 `cols`(2)索引 → 只有第一格对得上,其余全是空的(渲染只画出 1 块、碰撞全穿透) | 解析完**压紧成 `cols*rows`** 再交给组件。**"内部布局"和"对外布局"不一致是隐蔽 bug 的温床** |
| **瓦片 id 0 是合法图块,不能用 0 表示空**(M3) | 一开始约定"`0` 或 `-1` = 空格子",结果图集第 0 块永远画不出来 | 空 = **负数**(CSV 写 `-1`),`0..255` 全是合法图块。`solid[]` 表用 `-1 = 未指定(非空即实心)/0/1` 三态 |
| **射线从自己体内发出会命中自己**(M3) | 玩家向脚下的地面打射线,结果 `t=0` 且命中的是玩家自己的碰撞体 —— 示例里一眼就撞上了 | `eng_raycast`/`eng_sweep_box` 末尾加 `ignore` 参数(0 = 不忽略)。注意瓦片的 `obj` 也是 0,所以判断要写 `if (ignore && obj == ignore)` |
| **`restitution = 0` 会算出 `-0`**(M3) | 撞地后 `vy = -vy * 0.0` 得到 `-0.0`,打印成 `-0`,和 `0` 不相等,测试与用户判断都踩 | 先判 `restitution > 0` 再算;或统一 `if (fabsf(v) < 阈值) v = 0.0f` |
| **XAudio2 没有导入库,而且要 COM 初始化**(M4) | zig 只提供 mingw 的 `xaudio2.h`(接口的 C 版 vtable)不提供 `.lib`;直接 `-lxaudio2` 链不上。另外每个用 XAudio2 的线程都要 `CoInitializeEx` | 运行时 `LoadLibraryA("xaudio2_9.dll")`(Win8 回退 `xaudio2_8.dll`)+ `GetProcAddress("XAudio2Create")`;init 里 `CoInitializeEx(COINIT_APARTMENTTHREADED)` 并**忽略已初始化**的返回值 |
| **没有声卡不能让引擎崩**(M4) | 无音频设备时 `CreateMasteringVoice` 失败。若把 init 当致命错误,整个游戏都跑不起来 —— 而这只是"没声音" | `eng_audio_ok()` 暴露设备状态;失败时播放调用一律**带原因的失败**(而不是崩溃),测试据此 **SKIP** 而不是 FAIL。注意 `eng_audio_init` 在**两个** init 函数里都要调(offscreen 那次漏了就会得到 `ok=0` 且**原因为空**,很容易误判成"设备坏了") |
| **短音频的"正在播放"断言不稳**(M4) | 0.1 秒的音效在忙机器上可能在 `play` 与 `playing` 两次调用之间**自己播完**,`BuffersQueued` 变 0 → 断言偶发失败(实测整套跑时命中过一次) | 状态类断言一律用 **loop=1**(无限循环,永远不会自然结束);要测"播完"就显式等待或用长样本 |
| **边沿检测的拷贝时机**(M4) | `pressed`/`released` 靠"上一帧 vs 当前"。第一版在 `begin_frame`(也就是 pump **之后**)把 now 拷进 prev,于是**本帧刚按下的键立刻变成"上一帧也按着"**,pressed 永远为假 —— 而 down 一切正常,极容易以为"边沿就这样" | 拷贝放在**帧结束**(`dg_input_end_frame`,由 `eng_frame_end` 调)。判据:`down` 对但 `pressed` 恒为假 → 先查拷贝时机 |
| **语言没有全局变量 → 回调拿不到主程序的 `let`**(M4) | 主程序里 `let st = eng_object_new();`,然后在 `func on_update()` 里用 `st` → 编译期直接报 `undefined variable`。**回调式 API 撞上语言的第一个硬限制** | 跨帧状态放**组件字段**里,靠 `eng_set_name(obj,"state")` + `eng_find("state")` 取回(名字本来就是 IDE 场景树要的)。别指望闭包 |
| **`eng_dt()` 是帧间隔,不是物理步长**(M4) | 以为它能当"固定步长"用(手动物理时不调 `eng_frame_begin` 就恒为 0);另外它是**毫秒精度**的墙钟差,几帧挤在 1ms 内时是 0 | 固定步长用 `1.0 / eng_physics_set_step(hz)` 自己算;`eng_dt()` 只给"这帧该走多远"用。测试里断言 dt 前先忙等到下一毫秒 |
| **`main.py run` 只吃字节码**(M3) | 示例头注释一直写 `python main.py run examples/dexgame/demo_m1.dex`,但 `cmd_run` 直接把参数交给 VM → `error: not a valid DEXC bytecode file`。**注释与实现对不上,谁也没测过** | `cmd_run` 现在遇到 `.dex` 会先编译成同名 `.dexbc` 再跑(并接受 `-L`)。**文档里出现的命令要真的跑一遍** |
| **DirectWrite 的 IID 也要自己写,而且要 `-ldwrite`**(M4) | `DWriteCreateFactory(..., &IID_IDWriteFactory, ...)` 链接期报 `undefined symbol: IID_IDWriteFactory`(zig 不提供 dwrite 的 GUID 符号);另外构建 dexgame 必须显式加 `-ldwrite`(之前一直没加) | 用 `dg_guids.h` 里的 `DG_IID_IDWriteFactory`(M1 就写好了);构建脚本补 `-ldwrite`。**先写独立探针验证整条 API 链再集成** —— 探针几十行就能把"工厂/字体面/字形度量/alpha 位图"四步全验一遍,比在引擎里边调边猜快得多 |
| **文字光栅化必须整数像素定位**(M4) | 字形位图按整数像素光栅化,quad 落在半像素上会被线性过滤糊掉 | 画字时把(笔位+墨水偏移)与(基线+墨水偏移)四舍五入到整数;配合 §4.3 的 uv 规则(区域用边界)1:1 映射才清晰。**推论:一期文字不支持子像素定位** |
| **DexLang 没有位运算 → 测试不能拆像素通道**(M4) | 想验证"红字"写了 `(p>>16)&0xFF`,词法器直接报 `unexpected character '&'`(**也没有 `>>`**) | 用**算术恒等式**:白字 = base+cov·0x10101、红字 = base+cov·0x10000 → 两帧像素和之差必是 `cov总和×257` 的倍数(实测 3497770 = 257×13610)。写测试时最容易忘这条(库侧有 `eng_rgba` 兜底,测试里没有) |
| **`open(f,'w').write(open(f).read())` 会把文件清空**(最惨的一次自伤,B1) | 想一行式批量替换 `canatives`→`capnatives`,写成 `open(f,'w',…).write(open(f,…).read().replace(…))`。Python **先求值 `open(f,'w')`(立刻截断)**,再求值 `open(f).read()`(读到空串)→ `tools/dexc/` 的 **7 个 .c 文件全变成 0 字节**。当时还没提交,只能凭上下文把 ~3700 行重写一遍 | 批量改写一律 **read → 改 → write** 三步(或 `p = Path(f); p.write_text(p.read_text(...).replace(...))`)。**新写的代码要尽早 `git add`** —— 这次能恢复只是因为内容还在上下文里 |
| **按行切分写成 `while (p <= n)` = 死循环**(B1) | `dexc asm` 报 `out of memory (18446744056529682432 bytes)`(2^64−2^34,一看就是尺寸算崩)。真正病因却是切行循环:最后一轮 `p == n` 时会再切出一个空行、而 `p` 不再前进 → 无限 push 空行直到爆内存。**报错信息完全指不到病因** | 写成 `for (;;) { …; if (p >= n) break; p++; }`;缓冲区增长也要防溢出(`if (nc > SIZE_MAX/2)`)。“尺寸离谱的 OOM”先查**循环边界**,不要追分配器 |
| **空源文件会"编译成功"**(B1) | 源文件被上一条清空后 zig 不报错(空文件是合法 C),但产物**没有任何函数符号** → 链接期报 `lld-link: error: undefined symbol: WinMain`。这个报错看着像"子系统/启动对象选错了",于是白试了 `-mconsole`、`-Wl,--subsystem=console` 等一整天(zig 还回 `unsupported linker arg`) | 链接期缺 `main`/启动符号时,**先确认目标文件里有没有 `main`**(直接读 COFF 符号表:空文件的对象只有节符号与 `.file`),再怀疑链接参数。**别在链接参数上打转** |
| **签名类型码与 `.dexdef` 类型名是两套映射**(B1) | 把 `.dexdef` 的 `type_code("int"/"float"/"string"/"void")` 复用到签名串 `'ii:i'` 上,于是 **dexc 自己 disasm 出来的 `.native … siii:i` 自己 asm 不认** —— 往返只在"含 string 参数或 arity ≥ 3"的库上断裂 | `parse_sig` 用单字母映射(`v/i/f/s`)、`parse_type` 用全名映射,各自一个函数。**往返测试要覆盖真实库(166 个原生函数),玩具例子测不出来** |
| **PowerShell 没有 heredoc,还会吃掉 `-Wl,` 的逗号**(B1) | `python - <<'PY'` 在 PowerShell 里报"缺少文件规范"(不是 heredoc);`-Wl,-subsystem:console` 被当成 PowerShell 自己的参数(报"参数列表中缺少参量") | 要改文本就**写一个脚本文件**(write 工具)再 `python 脚本.py`;命令行里的 `-Wl,…` 必须整体加引号,或直接写进 Python 的 subprocess 参数列表 |
| **Python 的 `repr` 得自己实现**(B1) | dexc 的错误信息(`{x!r}`)与 `.dxasm` 里的浮点文本都要求与 Python 逐字节一致:引号选择、控制字符转 `\xNN`/`\uNNNN`、浮点用**最短往返**十进制且仅当指数 < −4 或 ≥ 16 才用科学计数法 | `tools/dexc/dexc_util.c` 里写 `dx_py_repr_str`/`dx_py_repr_float`,并由 `test_dexc.py` 用 16 个边界浮点(0.0/-0.0/1e15/1e16/1e-4/1e-5/5e-324/1.797…e308…)锁住 |
| **WebView2 注入的桥是 `chrome.webview`(全小写)**(B2) | 按文档常见写法用 `window.chrome.webView.postMessage(...)`:页面**正常渲染**、`title` 正确、导航 `ok=1`,但宿主机**一条消息都收不到** —— 表现像"桥没接通",而不是像 JS 报错 | 两个都认:`chrome.webview \|\| chrome.webView`(`dexstudio/web/index.html` 顶部内联脚本把可用实例存进 `window.__ds_bridge`)。诊断手段:`ds_wv_eval()` 让页面把 `Object.keys(window.chrome)` 报回来 —— 一眼看到实际名字是 `webview` |
| **注释里出现 `*/` 会提前结束块注释**(B2) | 在头注释里写 `字段表来自 eng_comp_*/eng_field_* 自省`,那个 `*/` **闭合了注释**,后面整段中文变成 C 代码 → 报一堆"unknown type name",真正的病因(注释被打断)完全看不出来 | 注释里不要写 `xxx_*/yyy_*`;要写就写 `` `eng_comp_*` 与 `eng_field_*` ``。(同类:`/*` 出现在注释里只会警告,`*/` 会直接破坏代码) |
| **`display:none` 里建出来的 canvas 是 1×1**(B4) | 逻辑图画布在 `body.mode-graph` 才显示,而 `init()` 在 DOMContentLoaded 时跑 —— 那时 `getBoundingClientRect()` 是 0×0,于是 `canvas.width=1`;切过去之后 CSS 把它拉伸到 500×800,画面糊成一团。像素断言当场抓住(`sampled:4`) | **切模式之后必须 resize**(`setMode('graph')` 里调 `Graph.resize()`,场景那边同理调 `Viewport.resize()`)。判据:画布看着有内容但像素统计只采到个位数样本 → 先查 canvas.width 是不是 1 |
| **没有全局变量 → 生成的回调必须自带 `dt` 参数**(B4) | DexLang 没有全局变量,`on_update(dt)` 的 dt 只能在函数里用。逻辑图生成的 `logic_update(dt)` 如果在 `logic_start` 里被引用(用户把「帧间隔」节点接到「开始时」的链上),就会生成**编译不过**的代码 —— 而用户在画布上完全看不出 | 生成器把**三个回调都写成收 `dt: float`**(`logic_start/logic_update/logic_draw`),模板 `main.dex` 传 `logic_start(0.0)`。于是任何事件链里都能用 dt。判据:凡是「引擎只在某个回调里提供」的东西,要在**所有**生成的回调签名里显式传进去 |
| **注释里写 `*/` 会把块注释提前关掉**(B5,JS 版) | 在 JS 的块注释里写 ``/* 注释: `//` 与 `#` 到行尾,`/*` 到 `*/` */`` —— 内层 `*/` 直接闭合了注释,后面的中文变成代码 → 页面白屏 + 一条看不懂的 SyntaxError。C 侧在 B2 踩过同款(见上) | 注释里不要出现 `*/`(要写就写「行注释」这种说法,或把 `*` 和 `/` 分开)。**加了自动守卫**:`tests/test_dexstudio.py::test_web_assets` 对每个前端 `.js` 跑 `node --check`(有 node 时),语法错当场失败 |
| **`display:none` 的容器里量出来的尺寸是 0**(B5) | 代码页签/逻辑图页签的面板在切过去之前是 `display:none`,此时 `clientWidth` 是 0;C 侧解析出来的诊断要"跳到第 N 行"就得先让面板可见并重排,否则 `scrollIntoView` 落空 | `setMode()` 先切 class 再让视图自己 `resize`/重排;自检里断言"切过去之后 panel 的 clientWidth > 0"(B4 的 canvas 1×1 是同一个坑的图像版) |
| **模态文件对话框会把无人值守流程挂死**(B6) | `res.pick` 调 `GetOpenFileNameA` —— 弹的是**模态**对话框。测试或离屏自测里只要调到它就是无限等待(实测 probe 挂到 120s 超时) | 给命令留一条不弹窗的路:`res.pick {dry:1}` 只回"能弹但不弹"。测试只用 dry;真弹窗的那条路归用户点。**凡是会阻塞的 UI 调用都要有可注入的"不阻塞"入口** |
| **"有没有未保存改动"不能只看脏标记**(B6) | 崩溃恢复的判据如果写成"有 autosave.json 就提示",那每次正常存盘后都会再提示一次(存盘不会删自动保存文件) | 判据用**时间**:自动保存比场景文件**新** ⇒ 上次没正常收尾。正常保存后场景更新,提示自然消失(测试用 `os.utime` 把场景改旧/改新来锁这条) |
| **只快照场景会恢复出"半对"的项目**(B6) | 项目状态分三处:场景 JSON、瓦片 CSV、逻辑图 JSON。自动保存只存场景的话,恢复后瓦片和图还是旧/空的 —— 而画面看起来"对了" | 自动保存写**整包**(`{scene_path, scene, files:{路径→文本}}`),复用撤销那套 `ds_scene_snapshot`/`ds_files_snapshot`/`ds_scene_restore`。判据:凡是"不在场景里的编辑状态",快照机制都要一起带上 |
| **给 DLL 用的系统库要加在 DLL 那条命令行上**(B6) | `ds_res.c` 用了 `GetOpenFileNameA`(comdlg32),只把 `-lcomdlg32` 加到两个宿主 exe 上 → DLL 链接报 `undefined symbol: __declspec(dllimport) GetOpenFileNameA`。而**测试只测 DLL**,宿主那边反而看不出来 | `main.py build-dexstudio` 里 `sys_libs` 要同时给 `-shared`(模型 DLL)与两个宿主;模型层新增系统 API 时先跑 `python tests/test_dexstudio.py`(它直接加载 DLL) |
| **发布形态下不能假设 exe 旁边有可写的兄弟目录**(B7) | 两处默认路径都写成 `exe_dir/../..\_zigtmp\...`(开发目录布局):① `--selftest` 的临时项目 → 干净目录里自测直接失败;② 没有项目时的**预览图目录** → 渲染图解码成 `0×0`、视口空白,看着像引擎坏了。**这两个都是"打包测试"抓出来的**,平时在仓库里跑永远发现不了 | 统一走 `ds_temp_dir()`(`%LOCALAPPDATA%\DexStudio` → `%TEMP%` → exe 同目录)。判据:凡是**写文件**的默认位置,都不能依赖"exe 旁边还有别的目录";而且要专门测一遍"拷贝到干净目录"的形态 |
| **前端资源内嵌 + 磁盘优先**(B7) | 只内嵌不找磁盘 → 开发时改一行 JS 都要重编 exe;只找磁盘不内嵌 → 发布出去没有 `web/` 就是白页 | 先找 `--web` → exe 同目录 `web/` → 仓库布局 `exe/../../dexstudio/web`,**都找不到**才用内嵌的那份,并解包到 `%LOCALAPPDATA%\DexStudio\web-<内容哈希>`。哈希进目录名 ⇒ 换版本自动换目录,不会读到旧文件 |
| **JSON 解析器把 UTF-8 的字节当成"码点"**(B8,最隐蔽的一条) | `ds_json.c` 的 `parse_string` 对**普通字符**也调 `put_utf8(c)`。文本本身是 UTF-8,一个汉字是 3 个字节(`E6 88 91`),被当成 3 个码点各编一次 → `读作 "我的" 写成 "æ\x88\x91ç\x9a\x84"`(二次编码)。症状千奇百怪:**磁盘上多出乱码目录/文件**(`我的项目` → `茅隆鹿…`)、资源"那个路径下没有那个资源"(前端拿到的是好的路径,发回来时被编坏了)、实体名/场景名乱码 | 普通字符一律 `put_byte`(**原样抄字节**),`put_utf8` 只用于 `\uXXXX` 转义。判据:**JSON 文本是 UTF-8,解析器不该做任何编码转换**;引擎侧的 `dg_json.c` 本来就是对的(可对照)。测试:`--selftest` 的"中文与编码"一节 + `test_utf8_paths` |
| **Windows 的窄字符 API 全都按 ANSI 解路径**(B8) | 中文系统 ACP=936,而宿主/引擎里的字符串是 UTF-8。`CreateWindowExA` → **窗口标题乱码**;`fopen`/`GetFileAttributesA`/`CopyFileA`/`DeleteFileA`/`FindFirstFileA`/`CreateDirectoryA` → 中文路径找不到文件或**建出乱码目录**;`GetOpenFileNameA` 拿回来的路径是 GBK,进 JSON 就是乱码;`CreateProcessA` 让 dexc 收到乱码路径;CRT 给的 `argv` 也已经被 ANSI 解过一遍(`--project 我的项目` 直接烂) | 凡是交给系统的东西都转宽字符走 W 版:宿主新增 `ds_utf8.c`(`dsu_fopen/dsu_exists/dsu_mkdir/dsu_mtime/dsu_copy_file/dsu_move_file/dsu_remove/dsu_list/dsu_env/dsu_argv`),引擎新增 `dg_utf8.c`(`dg_fopen`),dexc 新增 `dx_utf8.c`(`dxu_*`)。标题单独 `SetWindowTextW`,`argv` 从 `__wgetmainargs` 重取(它不需要 `-lshell32`)。判据:**代码里不该再出现 `CreateFileA`/`FindFirstFileA`/`GetOpenFileNameA`/`CreateProcessA`/`LoadLibraryA`/裸 `fopen`**;`fopen` 那个坑对**中文用户名**同样成立(`%LOCALAPPDATA%` 本身就带中文) |
| **捕获来的子进程输出可能不是合法 UTF-8**(B8) | dexc/vm 从 argv 拿到的路径被 CRT 转成了 ANSI(GBK),而它们自己的中文消息是 UTF-8 → 一份输出里混着两种编码。这段文本原样进 JSON 后,前端 `JSON.parse` 直接抛异常(`invalid continuation byte 0xce`),**整个输出面板就废了**(实测:中文目录里点"编译") | 两头都管:① 我们自己的 dexc 改成 UTF-8 原生(宽 argv + `_wfopen`),输出就统一了;② 宿主在捕获出口加一道 `dsu_utf8_clean()`(非法字节 → U+FFFD),**保证"响应永远是合法 UTF-8"这条契约**。判据:凡是要进 JSON 的外部字节,先过一遍合法性清洗 |
| **恢复提示不区分"谁写的自动保存"**(B8,用户报的) | 自动保存每 30 秒写一次,总是比场景文件新 → 界面**一直**显示"上次好像没有正常退出";点「恢复/丢弃」当时消失,**30 秒后自动保存又写出来**,提示条跟着回来,看起来像按钮没反应 | 自动保存包里记 `session`(进程号+启动时刻+计数器),`recoverable` 要求"**不是本次运行写的**";另外 `file_mtime` 读不出来时**不再当作 0**(0 会被当成"比自动保存旧"→ 假可恢复)。正常退出(`ds_model_destroy`)再盖一个 `clean=1` 章:进程被强杀走不到那里 ⇒ `clean=0` 就是"上次是崩的",前端据此选措辞(崩溃 vs 正常退出但没存盘) |
| **改了场景却没记住"起始场景"**(B8,顺带发现的) | `scene.new` 建出 `scenes/第一关.json` 并把实体都存在里面,但 `project.json` 的 `start_scene` 还是 `scenes/main.json` → 存盘后重开项目**回到空场景**,用户以为刚摆的东西丢了(其实还在那个场景文件里) | `scene_switch` 里顺手把当前场景写回 `project.start_scene`(项目相对路径、正斜杠),`project.save` 落盘即可。判据:**凡是"当前在编辑哪个文件"这种状态,都要能被重新打开时恢复** |
| **相对资源路径按「引擎自己的工作目录」解析**(B9,用户报的) | 场景里存的是**项目相对**路径(`res/hero.png`,这样项目才可搬),但 IDE 自己那个引擎实例的工作目录是 IDE 的目录 → `comp.set sprite.tex_path = "res/hero.png"` 必然失败(`cannot open image`),贴图设不上、sprite 的 `texture` 停在 -1。用绝对路径绕过去又让项目一搬家就废 | 引擎加**资源根**:`eng_set_asset_dir(dir)` + `dg_fopen_asset()`(相对路径先拼资源根、绝对路径原样),IDE 在 `project.new/open` 时设成项目根;游戏那边本来就以项目根为工作目录启动(ds_run 传的 cwd),天然一致。判据:凡是「用户能填的相对路径」,都要有一个明确的「相对谁」 |
| **拉线预览用的是 `pending`,guard 却看 `drag`**(B9,用户报的) | 逻辑图连线时那根预览线**不跟鼠标**,松手连线成功后才「啪」地出现。原因:`mousemove` 处理函数开头先 `if (!drag) return;`,而拉线时 `drag` 是 null(只有平移/拖节点才设 drag)—— `pending` 的更新与重绘被这行挡掉了 | 把 `if (pending) {…draw();}` 挪到 `drag` 的 guard **之前**。教训:**多个互斥的拖拽状态别共用一个「有没有在拖」的标志**。页面自测里派发 mousedown/mousemove 后读 `Graph.dragState()` 断言「跟着走」(只看像素会漏掉「线还在原点」这种) |
| **缓存过期时 UI 静默 `return` = 「点了没反应」**(B9) | `applyRes` 拿本地缓存 `DS.entities` 的 `byId(id)` 判断「要不要先挂 sprite」,缓存一过期就 `if (!e) return;` —— 用户点缩略图什么都不发生,也没有任何提示(写自测时同样因此拿不到贴图,一开始还以为是引擎的事) | 改成**现查一次** `entity.get`;查不到就明确报「实体已失效,先重选」。判据:UI 别拿缓存当「存在性」依据,更不能查不到就静默返回 |


---

## B10 —— DexStudio 体验/逻辑大修(用户报"用起来十分别扭")

| 陷阱 | 现象 | 应对 |
|------|------|------|
| **`hidden` 属性被作者样式的 `display` 盖掉**(B10,用户报的) | `style.css` 里 `.recover{display:flex}` / `.row{display:flex}` 这类规则**作者样式优先于浏览器默认样式**,于是 `[hidden]{display:none}` 失效:恢复提示条、橡皮选项、改名行在"该藏"的时候照样显示。JS 里 `el.hidden = true` 看起来生效了,写测试也只断言 `.hidden` 属性,所以长期没人发现 | `style.css` 顶部加 `[hidden] { display: none !important; }`;**断言要断 `getComputedStyle(el).display === 'none'`,不要断 `.hidden` 属性** —— 这两句话合起来才算把这类问题钉死(页面自测里 4 个元素逐个断 computed display) |
| **按钮引用了不存在的元素 id = 整块接线停摆**(B10) | `el('btn-x').onclick = …` 在 `init()` 里跑,id 写错/漏在 HTML 外就抛 TypeError,**后面所有按钮的接线都不会执行** —— 现象是"好几个按钮一起点了没反应",很难看出源头 | `tools/check_web_ids.py`:把 `$('…')` / `el('…')` / `getElementById('…')` 里的 id 与 `index.html` 的 id 集合对照,任何缺失直接退出码 1(已进 `tests/test_dexstudio.py`)。判据:**HTML 是前端的"接口",接口名要对得上,而且要有自动检查** |
| **`scene.list` 返回裸文件名,被当成项目相对路径**(B10,修复中自查发现) | `scene.list` 给的是 `main.json`,而 `scene_switch()` 需要的是项目相对路径。删掉当前场景后"切到下一个"时直接把裸名喂进去 → 加载/新建了 `<项目根>\main.json`,起始场景也跟着变成不存在的 `main.json`,游戏跑起来是空场景 | 列表函数统一返回 `scenes/x.json`(带前缀、正斜杠);判据:**同一个概念在系统里只能有一种表示法**,否则迟早有一处拼错 |

---

## B11 —— Scratch 式积木模式 + 用户报的四个"看不到效果"

用户的原话:「导入的预览图还是裂图标」「给物体改贴图了能看到框线改变,但并没有实际的图像被显示」「声音预览也没有效果」「代码块设计是认真的吗…完全就是把代码换了一种形式」。前三条**都不是前端画错**,是引擎/宿主侧三个真问题;第四条改成了 Scratch 式积木。

| 陷阱 | 现象 | 应对 |
|------|------|------|
| **`eng_run` 之前没人 `eng_init`:整个游戏 60ms 就跑完了**(B11,实测) | IDE 生成的 `main.dex` 模板只写 `on_start` + `eng_run(...)`,从不初始化引擎。而 `eng_run` 的循环条件是 `while eng_running()` —— 没建窗口时它**恒为 0**,于是:on_start 跑一遍 → 循环一次都不进 → 进程退出。用户按下「运行」看到的是"窗口闪都不闪,什么都没发生"(实测 60ms,exit 0,**没有任何输出**) | 模板在 `eng_run` 之前显式 `eng_init(dexstudio_game_title(), 960, 540, 1)`;`dexgame_fast.dex` 的 `eng_run`/`eng_run_frames` **自己兜底**(没起来就自动开一个 960×540 的窗口,失败则打印原因)。回归测试 `test_template_game_really_runs`:编译 IDE 生成的 `main.dex` 跑起来,2 秒后进程必须还活着 |
| **组件池没初始化 → 场景"加载成功"但一个组件都没有**(B11,实测) | `dg_comp_kind()` 一开始是 `if (!name || !g_pools_ready) return -1;`,而场景加载把 `-1` 当成**"不认识的组件(新版本写的)"静默跳过**。于是 on_start 里在 `eng_init` 之前 `eng_scene_load` → 实体都在、名字都在、`eng_find` 找得到,**但 transform/sprite 一个都没挂上** → 屏幕上什么都没有,还查不出原因(实测:`eng_has(玩家,"transform")` == 0,`eng_set_f` 返回 -1) | `dg_comp_kind()` 与 `dg_scene_parse()` 里**按需 `dg_scene_init()`**(组件池只是内存表,不需要 D3D);回归测试 `test_scene_load_before_init`。判据:**"未知"和"还没准备好"必须能区分**,否则容错逻辑会把真错误一起吃掉 |
| **同一个坐标公式在引擎和宿主里各写了一遍,还写得不一样**(B11,用户报的) | 引擎画精灵:原点 = `世界坐标 - 源尺寸 × 轴心 × 缩放`;宿主 `scene.outline` 写的是 `世界坐标 + 轴心`(外加我加缩放时照抄了这个错)。于是"框线偏在图片右下角外面",用户看到的就是**"改了贴图只有框线在动,图像没出现"**。同类的还有一处:视口用的是**活动相机的 camera.x/y/zoom**(不是 transform),而 `app.info` 报的是编辑器视图覆盖值 → 用户一挪相机,画面跟着走、框线不跟 | 宿主照抄引擎公式(`dg_scene.c` 的 sprite 绘制),并在注释里写明"必须与引擎逐字相同";`app.info`/`scene.render` 在没有视图覆盖时返回**实际生效**的活动相机视图。回归测试 `test_outline_matches_pixels`(直接比"框线"和"画面上的非背景像素包围盒")+ `test_viewport` 的相机断言 |
| **旧模板留下的 32×32 裁切 = "贴图设了却只看得到一角"**(B11,用户项目里实测) | `sprite.sw/sh` 是**从贴图里截取多大**(1:1,不是缩放),旧模板给的是 32×32。用户拿一张 300×400 的角色图设上去,屏幕上只有左上角 32×32 的一小块 —— 观感就是"图像没显示出来" | 换了 `sprite.tex_path` 时,若裁切**恰好是 32×32**(旧默认值)且贴图不是 32×32,就自动改成 0(= 整张贴图),并在响应里带一句 `note` 让前端说出来(不然像是偷偷改用户的东西);打开老项目时也做一次**只在内存里**的同样修正(不改用户的文件、不算脏)。用户自己裁过的大小一概不碰(有测试) |
| **`--wv-selftest` 带项目跑时,资源那一段全被跳过**(B11,自查发现) | 页面自测里"有没有打开项目"看的是 `DS.info.root`,而 `DS.info` 是 `refresh()` 写的 —— 宿主在 `ui.ready` 之后立刻触发自测,`refresh()` 还没写完,于是**带着项目跑也整段 SKIP**(缩略图/试听/换贴图这些用户报的症状恰好都在这一段里,等于没测) | 自测自己把 `app.info` 记进 `DS.info`;`tests/test_dexstudio.py` 新增 `test_page_with_project`:搭一个带 PNG+WAV 的临时项目再跑 `--wv-selftest`,断言"缩略图真的解码""声音能解码并播放""换贴图后选中框 = 整张贴图"。判据:**SKIP 也是覆盖率的洞** —— 用户报的症状落在 SKIP 段里,等于白测 |
| **硬件轮询每帧覆盖合成输入**(B11,插上手柄后才暴露) | `dg_input_begin_frame()` 里的 `dg_input_poll_pad()` 用 `XInputGetState` 把 `g_pad_now` **整个重写**成真实手柄状态 —— 于是测试里"喂一个 A 键"在**插着手柄的机器上**会被立刻冲掉:`test_input.py` 的"手柄按键也能触发同一个动作"就这一条挂,而且**同一份代码在不同机器上结论不同**(实测:同一天从 78/0 变成 77/1,查下来是手柄状态) | 合成输入优先:`eng_input_feed*` 里置 `g_synth = 1`,帧开始处 `if (!g_synth) dg_input_poll_pad();`。判据:**"测试通道"必须能压过"真实设备"**,否则无人值守测试的结果就依赖插了什么设备 |

---

## B12.3 —— 「复制进 res/ 了,缩略图还是裂的」

用户报:「我发现程序哪怕把文件复制到了 res 目录下缩略图还是显示失败,包括给实例设置了图片也没预览效果」。
先按用户的真实操作复现(手工把图片复制进 `res/`,文件名故意带上空格/中文/`#`/`%`/大写扩展名),
再一条条查下面这几处。

| 陷阱 | 现象 | 应对 |
|------|------|------|
| **刷新合并把"上一轮"的 promise 还回去了**(B12.3,自查发现) | `refresh()` 里写的是 `if (refreshBusy) { refreshAgain = true; return refreshBusy; }` —— 后到的调用等的是**上一轮**(在它那条命令**之前**就开始的那一轮),于是 `await ds('undo'); await refresh(); 读 DS.entities` 拿到的可能是**撤销之前**的实体 id。页面自测因此报"实体 65544 没有 tilemap 组件",而同一次 `entity.list` 里那个名字明明带着 tilemap;更普遍的表现是"**点了没反应/面板和画面停在上一状态**"(缩略图、视口预览都像没生效) | 改成单飞 + 排队:`refreshRunning` 是正在跑的那轮,`refreshPending` 是排队补跑的那轮,**每个**调用都等到"排在它之后的那轮"结束才 resolve。判据:**"await 之后数据是新的"必须是这个 API 的语义**,合并只能省请求,不能改语义 |
| **一次断言失败把整个页面自测中断掉**(B12.3,自查发现) | 页面自测是一个大 `try`,里面任何一步抛错都直接跳到 `catch` → 后面所有断言(包括"缩略图真的解码""声音能播放""换贴图后选中框对")**一条都不跑**,报告里只有一句"自检中断"。用户报的缩略图问题因此长期没有被自动测到 | 瓦片/图层那一段包自己的 `try/catch`(失败**报出来**但继续);`tests/test_dexstudio.py` 的 `test_page_with_project` 断言 `"fail":[]`。判据:**自测的每一段都不能把别的段带下水** |
| **在资源管理器里复制文件,面板不会自己重读**(B12.3,用户场景) | 前端没有文件监视(不想引依赖),`res.list` 只在「导入…」/「刷新」/切项目时读一次 —— 用户复制完文件回到 IDE,看到的还是旧列表(或者干脆以为"缩略图坏了") | 用"窗口重新获得焦点 / 页面重新可见"当作"文件可能变过"的信号,自动重读一次 `res.list`;空列表的提示也改成"可以在资源管理器里直接复制进 res/,回到窗口会自动重读"。判据:**凡是"用户可以在 IDE 外面改的东西",都要有一个明确的重新读取时机** |

**这一轮的验收证据**

```bash
python tests/test_dexstudio.py       # 481 项(新增两条:手工复制进 res/ 的图也要有缩略图、
                                     #   页面自测必须没有失败项)
dexstudio.exe --project <项目> --wv-selftest   # 页面自测 137 项、0 失败(20 个故意难看的文件名全过)
```

- 复现脚本(`_zigtmp/resprobe2.py`,不入库)把 20 个文件手工复制进 `res/`:
  `' 前后空格 .png'`、`'!感叹号.png'`、`'&和&.png'`、`"'引号'.png"`、`',逗号.png'`、
  `';分号.png'`、`'=等号.png'`、`'@at.png'`、`'UPPER.PNG'`、`'[方括号].png'`、
  `'plus+加.png'`、`'weird#hash.png'`、`'百分%号.png'`、`'（全角括号）.png'`、
  100 多字的超长名、`'照片.jpg'`、`'音效.wav'`……**每一个的缩略图都真的解码了**,
  声音也能播 —— 说明"缩略图加载"这条路(虚拟主机 + `encodeURIComponent` + `?v=` 破缓存)
  本身是好的,坏的是**上面那三处状态/流程**。


---

## B12.4 —— 「导入图片一直显示图片读取失败,但点运行有图」

用户报:「dexstudio项目的编辑器导入图片一直显示图片读取失败,给实例设置编辑器里也没有效果,
但点击运行实际是有图片的」。

这次坏的不是"资源加载这条路",而是**虚拟主机映射的生命周期** —— 库说成功,正在跑的那个文档不认。

| 陷阱 | 现象 | 应对 |
|------|------|------|
| **WebView2 对"已经加载完的页面"改虚拟主机映射不生效**(B12.4,用户报的根因) | 资源缩略图 / 图集预览 / 视口 BMP 都靠虚拟主机(`dexstudio-proj.local` → 项目根、`dexstudio-preview.local` → 渲染目录)交给网页显示。启动时**没有项目**,映射此时才登记;用户是**双击 exe 之后**再从界面里打开项目 → 映射是在页面加载完之后才设的 → `SetVirtualHostNameToFolderMapping` **返回 S_OK**(日志上看着"映射成功了"),但那个页面发出的请求照样失败:`<img>` 是裂图标(app.js 显示"图片读取失败")、`fetch` 抛 `TypeError: Failed to fetch`。而「运行」走的是引擎自己读文件 —— 所以**运行有图、编辑器没图** | 映射改指向之后**必须重新导航一次**:`ds_main.c` 的 `sync_host_mappings` 只在目标真的变了时改指向,然后 `PostMessage(WM_APP_RELOAD)` → `ds_wv_reload()` → `location.reload()`(走窗口消息是为了让这条命令的响应**先**回到 JS)。判据:**"改配置"和"让配置生效"是两件事 —— 库返回成功 ≠ 正在跑的那个文档会认** |
| **`--project` 恰好绕开了这条路径,所以以前测不出来**(B12.4) | `--project <目录>` 是"**导航之前**就映射好",一直是好的:`--wv-selftest`、`test_page_with_project`、发布形态自测全走这条路 → 缩略图/试听/换贴图那一串断言全过,而用户真实顺序(先加载页面、再从界面打开项目)**一次都没被测过** | 新增 `--open-late <目录>`(等 `ui.ready` 之后用 `openProjectAt()` 打开项目 = 双击 exe 的顺序),以及 `tests/test_dexstudio.py` 的 `test_page_open_late`:断言"资源检查真的跑了(不是当成没项目 SKIP)""缩略图真的解码了""项目根映射是通的"。判据:**"用户按什么顺序操作"也要有对应的一条测试** |
| **`applyRes` 的自动缩放只问浏览器,不问引擎**(B12.4,同一条用户症状的另一半) | "按贴图大小自动缩小"是 `new Image()` 解码读尺寸。浏览器读不到图时(就是上面那个映射问题,或者格式它不认)这段**静默失效** → 用户把图设给实体,编辑器里看不到任何变化 | 改成先问引擎:`scene.outline` 是**引擎按贴图算出来的世界包围盒**(贴图尺寸 × 缩放,除掉缩放就是原尺寸),引擎读得出来的图它一定量得准;浏览器那条只当兜底。判据:**同一个量有两处来源时,取"真正画它的那一个"** |
| **用 PowerShell 把中文路径传给 exe:无 BOM 的 `.ps1` 会被按 ANSI(936) 读**(B12.4,排查时自己踩的) | 临时探针脚本里写的 `$proj = '…\我的游戏'` 到 exe 手里变成 `鎴戠殑娓告垙` → `project.open` 静默失败 → 自测跑成"没项目"那 95 项分支,看上去像"复现不出来"。同一类:GBK 控制台里 `print` 带 `↔` 的字符串直接抛 `UnicodeEncodeError`,测试看着"失败"其实是打印挂的 | 探针脚本要么纯 ASCII(把项目复制到 ASCII 路径再跑),要么存成**带 BOM** 的 UTF-8;看中文输出用 `python -X utf8` 或 `read` 工具。判据:**排查脚本本身也是代码,它也会骗你** |
| **`--wv-selftest` 绕过单实例互斥量,会和"别人正开着的实例"抢 WebView2 用户数据目录**(B12.4) | 跑离屏自测时撞上 `创建 WebView2 控制器失败(hr=0x800700aa)`(ERROR_BUSY)。这条错误信息里**完全没有**"是别人占着"这层意思,当时第一反应是"IDE 坏了 / 运行时坏了" | 自动化一律 `DEXSTUDIO_WV_DATA=<独立目录>`(代码里早留了这个口子);`--wv-selftest` 不检测互斥量是刻意的(要能在用户开着 IDE 时也跑)。判据:**BUSY(0x800700aa)= "有人占着"、缺 `ICoreWebView2_3` = "运行时太旧",两者必须能区分**。(实测补充:把共享目录改名以验证"是否被锁"是**不可靠**的 —— 改名成功也不代表没进程在用里面的文件;别用这个当证据) |

**这一轮的验收证据**

```bash
python tests/test_dexstudio.py                  # 490 项(新增 5 条:后开项目这条路)
dexstudio.exe --wv-selftest --open-late <项目>   # 页面自测 118 项、0 失败(修前 114/3)
```

- 修前(实测)`--open-late` 的 3 条失败原文:
  `项目根映射出去了(资源缩略图靠它) TypeError: Failed to fetch`、
  `缩略图真的解码了 猫_悲伤.png https://dexstudio-proj.local/res/%E7%8C%AB_…png?v=20`、
  `换上贴图后选中框 = 整张贴图 × 缩放 … 贴图={"w":0,"h":0} k=1`。
- 修后四种走法全部 0 失败:仓库内 `--open-late` / 仓库内 `--project` /
  发布包(`dist/DexStudio` 副本,内嵌前端)`--open-late` / 不带项目。
- 顺带把缩略图读失败时的提示改准了:能区分"项目资源服务没连上(重新打开项目能救)"
  和"这个文件本身读不出来(损坏 / 格式不认)"—— 以前一律说"请确认项目已打开且文件在
  res/ 下",把人往错方向带。


---

## B12.5 —— 「框线应该适配图片大小,而且我要能拉能转」+ 拖动掉帧/图片比框线慢

用户报:「我给示例套用图片之后,框线应该适配图片大小,而不是原来的示例,我可以通过框线
旋转拉伸图片」以及「拖动编辑器框线时掉帧很严重,并且有图片的实例的贴图在移动框线时有
延时,就是卡顿的感觉,图片比框线运动的慢一点」。

两条其实是**同一个架构问题的两面**:编辑器把"引擎渲染的一张静态 BMP"当背景,而框线是
前端画的 —— 于是"谁决定框线多大"、"谁负责跟手"都变成了需要明确设计的事。

| 陷阱 | 现象 | 应对 |
|------|------|------|
| **框线由碰撞盒决定,而不是由"用户看到的那张图"决定**(B12.5,用户报的) | `scene.outline` 的老逻辑是 `if (有 collider) 用碰撞盒 else if (有 sprite) 用精灵`。模板给玩家的 collider 是 32×32,于是用户套上 300×400 的图之后**框线还是 32×32**(他的原话:"框线应该适配图片大小,而不是原来的示例") | 改成**精灵优先**:精灵 > 碰撞体 > 瓦片地图;碰撞盒**单独给一份**(`outline.collider`),前端画成青色虚线 —— 逻辑框和视觉框从此是两件事,不再互相顶掉。判据:**主框线必须围着"用户看得见的东西"** |
| **"换一张图"没有把这张图的大小带上**(B12.5,同一条的另一半) | 老模板留下 `sprite.sw/sh = 32`(从图里裁 32×32),换图时只清裁切、不碰缩放 —— 300×400 的图 1:1 铺满 1024×640 视口,或反过来只显示一角 | `comp.set sprite.tex_path` 里补齐两件事:①清掉"恰好 32×32"的旧模板裁切;②**用户没手动缩放过**(sx/sy 都是 1)时按新图大小给一个合适缩放(长边 > 256 → 缩到 128)。做过什么写进返回的 `note`,前端 `call()` 会弹出来。判据:**"我换了一张图"应该等于"新图以合适的尺寸出现",而且要让用户知道程序顺手改了什么** |
| **旋转在前端有、在引擎没有**(B12.5,用户要"拖框线旋转") | `transform.rot` 从 M2 起就是"标了只读、改了没效果"的字段(引擎不读)。要做旋转手柄,必须先在引擎里真的转 —— 而且**四角公式要在两处逐字相同**(引擎 `dg_scene.c` 画、宿主 `ds_model.c` 算框线),否则框线会像 B11 那样漂到图外面 | 引擎加 `dg_draw_quad_corners`(四角自己给)+ sprite 绘制绕**轴心**转(单位:**度**,屏幕顺时针为正);宿主 `scene.outline` 用同一公式算出四角、连同 `rot`/`pivot`/`rw`/`rh` 一起给前端;前端**只画 C 给的四角**,不再算第二遍。判据:**旋转也要有一条"框线 vs 像素"的断言**(`test_sprite_transform`) |
| **拖动时"引擎的旧图"和"前端跟手的框"同屏**(B12.5,用户报的"图片比框线慢半拍") | 拖动时前端只更新本地位置 + 重画框线,引擎那张 BMP 还是**上一个位置**的 → 用户看到框线在动、图不动/慢半拍。而如果改成每帧都让引擎重渲染:离屏 D3D11 → BMP 落盘(1.9MB)→ `<img>` 解码,一帧几十毫秒,**必然掉帧**(用户报的"掉帧很严重") | 学一般编辑器:**按下时让引擎把被拖的精灵藏起来**(新增 `sprite.visible` + `scene.render {hide:[…]}`,渲染完立刻还原、不动场景数据),画布上自己画一份**正好跟手**的贴图(仿射矩阵把源矩形映射到四角,天然支持旋转/翻转/染色),**拖动过程里一条命令、一次引擎渲染都不发**,松手才 `comp.set_many` 一条撤销。判据:**拖动是纯前端的事,引擎只负责"松开之后那一帧"** |
| **125~1000Hz 的鼠标 → 每个 mousemove 画一次画布**(B12.5,掉帧的另一半) | 视口的 `draw()` 挂在 `mousemove` 上直接调用(还每次都重画整张预览图 + 几十条网格线),游戏鼠标一秒能触发几百次 | ①`requestDraw()` 用 rAF 合帧(每帧最多画一次);②把"预览图 + 网格"画进**离屏画布缓存**(视图没变就直接 blit);③滚轮/平移时的 `view.set` 从"每个 mousemove 一条"改成搭渲染那一拍发(60ms 一次)。判据:**热路径上不许有"每次事件都做一遍"的重活**;页面自测用 `DS.cmds` 计数断言"拖动过程里一条命令都不发" |
| **撤销会换实体 id,自测里拿着旧 id 继续操作**(B12.5,排查时自己踩的) | 页面自测里"拖一下 → 撤销 → 再拖一下",第二次拖的 `DS.sel`/`e0.id` 还是撤销**之前**的 id → 报 `object 65551 is not alive`,而真正的原因是快照式撤销会重建实体 | 撤销之后**按名字把它找回来**(`entity.list` 里按 name 取新 id),并且把 `DS.sel` 一起换掉 —— 拖动手柄的代码要求"选中的实体在 `scene.outline` 里存在",id 一死手柄就整个消失。判据:**测试里凡是跨过 undo 的操作,必须重新解析 id** |
| **`entity.list` 的 `comps` 是"组件名数组",不是字段表**(B12.5,自己踩的) | 拖动预览要读 `sprite.tex_path / px / py`、`transform.sx/sy/rot`,而 `byId(id).comps` 是 `["transform","sprite"]` → `comps.sprite` 永远是 undefined → 跟手的图**压根画不出来**(表现为"拖动时只有框线没有图") | 要字段值就 `entity.get`(`beginGhost` 里逐个取);`entity.list` 只用来画树/拿 x/y/parent。判据:**两个 API 返回的 comps 形状不一样,不能混用** |

**这一轮的验收证据**

```bash
python tests/test_dexstudio.py                 # 508 项(新增 test_sprite_transform + 页面自测的手柄/跟手/一条撤销)
dexstudio.exe --wv-selftest                    # 页面自测 97 项、0 失败(不带项目)
dexstudio.exe --wv-selftest --project <项目>    # 带项目:手柄真的能拉能转、拖动过程里 0 条命令
```

- 模型层(`test_sprite_transform`)直接**数像素**:转 90° 之后框线的外接矩形必须与画面里
  非背景像素的包围盒对齐(旋转版"框线不许漂");`visible=0` 时画面上必须一个像素都没有;
  `scene.render {hide}` 渲染完 `visible` 必须回到 1(否则实体就"消失"了)。
- 页面自测新增:有碰撞盒时框线仍然围着图 / 碰撞盒单独给 / 8 个缩放手柄 + 旋转手柄都在 /
  **拖动预览的四角与 C 给的四角逐点一致** / 拖动过程里不写模型且 0 条命令 /
  松手写回的就是预览那份 / 一次拖动 = 一条撤销 / 拖旋转手柄真的转 90° / Shift 吸附 15°。


