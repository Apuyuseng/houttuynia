// Windows 窗口外观：DWM 圆角（Win11+）。
// Win11 有系统级窗口圆角偏好（DWMWA_WINDOW_CORNER_PREFERENCE）；
// Win10 及以下该属性不存在，调用失败即 no-op（阴影由 DWM 默认提供，
// frameless + WS_THICKFRAME 组合下通常已带边框阴影）。
// 本文件在非 Windows 平台不参与编译（CMake 挑源）。
#ifdef _WIN32

#include "chrome.hpp"

#include <windows.h>
#include <dwmapi.h>

namespace mwine {

void applyWindowChrome(QWindow* win, double radiusPt)
{
    if (!win)
        return;
    const HWND hwnd = reinterpret_cast<HWND>(win->winId());
    if (!hwnd)
        return;
    // DWMWCP_DEFAULT=0 / DONOTROUND=1 / ROUND=2 / ROUNDSMALL=3
    const DWORD pref = radiusPt > 0 ? 2 /*DWMWCP_ROUND*/ : 1 /*DWMWCP_DONOTROUND*/;
    ::DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                            &pref, sizeof(pref));
}

} // namespace mwine

#endif // _WIN32
