/* dg_guids.h — 本项目自己声明需要的 COM GUID
 *
 * 为什么不用 INITGUID / IID_xxx 符号:
 *   zig 不提供 dwrite / dxgi 的导入库(只提供 d3d11 等少数几个),所以
 *   `IID_IDWriteFactory` 这类符号在链接期是 undefined 的 —— 实测报
 *   `lld-link: error: undefined symbol: IID_IDWriteFactory`。
 *   显式写死这几个 GUID 比 `#include <initguid.h>` 更可预测,也只需要付出
 *   实际用到的那几个。
 */
#ifndef DG_GUIDS_H
#define DG_GUIDS_H

#include <windows.h>

/* IDXGIDevice —— 取适配器信息(显卡名/显存),用于诊断输出 */
static const GUID DG_IID_IDXGIDevice =
    { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

/* IDXGIFactory2 —— 需要时用 CreateSwapChainForHwnd(M1 暂用 D3D11CreateDeviceAndSwapChain) */
static const GUID DG_IID_IDXGIFactory2 =
    { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };

/* IDWriteFactory —— DirectWrite 工厂(M1b 文字渲染用) */
static const GUID DG_IID_IDWriteFactory =
    { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

#endif /* DG_GUIDS_H */
