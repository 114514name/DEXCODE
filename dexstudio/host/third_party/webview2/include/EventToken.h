/* EventToken.h —— Windows SDK 里的小头文件,但 **mingw/zig 不提供**。
 *
 * WebView2.h(MIDL 生成的)里有 `#include "EventToken.h"`,它只为了
 * `EventRegistrationToken` 这一个类型(add_/remove_ 事件注册用)。
 * 内容与 Windows SDK 的 shared/EventToken.h 一致;放在 vendor 目录里,
 * 于是 `-I dexstudio/host/third_party/webview2/include` 就能解析到它。
 * -------------------------------------------------------------------------*/
#ifndef _EVENTTOKEN_H_
#define _EVENTTOKEN_H_

#if !defined(_WIN32)
#error EventToken.h 只在 Windows 上有意义
#endif

typedef struct EventRegistrationToken {
    LONGLONG value;
} EventRegistrationToken;

#endif /* _EVENTTOKEN_H_ */
