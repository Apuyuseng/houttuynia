// 窗口外观（圆角 / 阴影）跨平台接口。
// frameless 窗口各平台默认都没有系统圆角与阴影，由各平台实现补齐：
//   chrome_mac.mm    AppKit layer 圆角 + hasShadow
//   chrome_win.cpp   Win11+ DWM 圆角（DWMWA_WINDOW_CORNER_PREFERENCE）
//   chrome_stub.cpp  其余平台 no-op（未来 X11/Wayland 补实现）
// radiusPt <= 0 表示恢复直角。跨平台移植时只需新增对应源文件并在
// CMake 挑源，调用方（main.cpp）无平台分支。
#pragma once

#include <QWindow>

namespace mwine {

void applyWindowChrome(QWindow* win, double radiusPt);

} // namespace mwine
