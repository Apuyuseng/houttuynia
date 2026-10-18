// macOS 窗口外观（chrome.hpp 的 AppKit 实现）：圆角 + 阴影。
// frameless 窗口系统不给圆角和阴影（裸矩形贴桌面）——这里给 NSWindow 的
// contentView 开 layer-backed 裁剪（cornerRadius），并补上窗口阴影。
// 注意：QOpenGLWidget 的视频层若未挂在 contentView layer 树下，圆角只对
// 常规子部件（底部功能条）生效，视频区四角为直角——出现该情况时退路是
// 窗口级 QRegion mask（有锯齿）或 GL shader 内做圆角（见 NOTES）。
#import <AppKit/AppKit.h>

#include "chrome.hpp"

namespace mwine {

void applyWindowChrome(QWindow* win, double radiusPt)
{
    if (!win)
        return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(win->winId());
    if (!view)
        return;
    NSWindow* nswin = view.window;
    if (!nswin)
        return;
    nswin.hasShadow = YES;
    view.wantsLayer = YES;
    view.layer.masksToBounds = YES;
    view.layer.cornerRadius = radiusPt;
}

} // namespace mwine
