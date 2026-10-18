// 其余平台（Linux/X11/Wayland 等）的窗口外观：暂 no-op。
// X11 无统一圆角协议（依赖 WM/合成器），Wayland 的 xdg-decoration 由
// WM 决定；移植时在此接对应扩展即可，调用方无需改动。
#include "chrome.hpp"

#include <QWindow>

namespace mwine {

void applyWindowChrome(QWindow* win, double radiusPt)
{
    Q_UNUSED(win)
    Q_UNUSED(radiusPt)
}

} // namespace mwine
