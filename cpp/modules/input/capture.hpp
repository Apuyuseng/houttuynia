// macOS 鼠标捕获辅助：光标关联开关 + 模拟器窗口 frame 查询。
// ← crates/mwine-input/src/capture.rs
//
// - associateMouseCursor：FPS 捕获模式下断开鼠标↔光标关联
//   （光标冻结、增量照常流动），退出时恢复。
// - emulatorWindowFrame：CGWindowList 找 qemu 窗口的屏幕位置，
//   free 模式下把全局鼠标坐标映射到 Android 屏归一化坐标（技能瞄准）。
//
// 非 Apple 平台：no-op / 空结果（TODO：对应平台的窗口查询）。
#pragma once

#include <QPair>

#include <optional>

namespace mw::input::capture {

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// 断开/恢复鼠标与光标位置关联。
/// connected=false：光标冻结在原地，鼠标增量事件照常派发（FPS 相对模式）。
void associateMouseCursor(bool connected);

/// 查找模拟器（qemu-system-*）窗口的屏幕 frame。找不到返回空。
std::optional<Rect> emulatorWindowFrame();

/// 模拟器窗口是否处于宿主前台（z 序最顶的普通窗口）。
/// FPS 捕获模式只在此时激活——否则会吞掉用户点回游戏窗口的鼠标事件，
/// 游戏失焦暂停后永远无法恢复（实测踩坑）。
bool emulatorFrontmost();

} // namespace mw::input::capture
