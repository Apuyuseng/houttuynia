// scrcpy 注入后端（快路径）：毫秒级延迟，走 mw_scrcpy 控制通道。
// ← crates/mwine-input/src/scrcpy.rs
//
// 一个 ScrcpyInjector 实例 = 一条绑定了目标显示器的 scrcpy 会话：
// - 会话级绑定（displayId 在 server 启动参数里），消息不带显示器字段，
//   因此实例只接受自己绑定的 displayId，其余返回 errDisplayNotBound
// - control-only 会话会退回主屏（实测），定向副屏必须携带最低开销视频
//   （320px@1fps）把会话锚到目标显示器——封装在 connect
// - 视频流不消费但必须保活：socket 关闭会让 server 写失败退出
#pragma once

#include <QPair>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

#include "input.hpp"
#include "result.hpp"
#include "scrcpy/client.hpp"
#include "scrcpy/control.hpp"

namespace mw::input {

/// 查询 guest 显示器分辨率（`wm size`）——实现见 console.hpp。
mw::Result<QPair<quint16, quint16>> queryDisplaySize(const mw::adb::AdbClient& adb,
                                                     const QString& serial,
                                                     std::optional<quint32> displayId);

/// 触摸消息构造（纯函数，便于字节级测试）。
/// 压力与按钮位遵循 scrcpy 客户端惯例：Down/Move 主键按下，Up 归零。
mw::scrcpy::ControlMessage touchMessage(QPair<quint16, quint16> screen, TouchAction action,
                                        quint64 pointerId, qint32 x, qint32 y);

/// 滚轮消息构造（纯函数）。h/v ∈ [-1,1]。
mw::scrcpy::ControlMessage scrollMessage(QPair<quint16, quint16> screen, qint32 x, qint32 y,
                                         float h, float v);

/// 按键消息构造（纯函数）。本地 KeyAction → scrcpy 协议动作位。
mw::scrcpy::ControlMessage keyMessage(const AndroidKeycode& keycode, KeyAction action);

/// 按 INJECT_TEXT_MAX（300 字节）分块，保证不撕裂 UTF-8 字符。
QVector<QByteArray> splitText(const QString& text);

/// scrcpy 控制通道注入后端（会话绑定单个显示器）。
class ScrcpyInjector : public InputInjector {
public:
    /// 用已有控制通道构造（不接管会话生命周期；分辨率由调用方保证）。
    ScrcpyInjector(mw::scrcpy::ControlChannel channel, std::optional<quint32> displayId,
                   QPair<quint16, quint16> screen);

    /// 建立注入会话（恒定主屏 = 模拟器窗口；displayId 参数保留作
    /// 会话归属标记，非 0 主屏不再被 mw_scrcpy 支持）。
    ///
    /// - screen 为空 → 经 `wm size` 查询
    ///
    /// 约束：纯控制会话（video=false）协议不保活，恒带 320px@1fps
    /// 最低视频锚住会话（流不消费，仅保活）。
    static mw::Result<ScrcpyInjector> connect(const mw::adb::AdbClient& adb, const QString& serial,
                                              std::optional<quint32> displayId,
                                              std::optional<QPair<quint16, quint16>> screen);

    ScrcpyInjector(ScrcpyInjector&&) = default;
    ScrcpyInjector& operator=(ScrcpyInjector&&) = default;

    /// 会话绑定的显示器 id（空 → 0 主屏）。
    quint32 boundDisplay() const { return m_displayId.value_or(0); }
    QPair<quint16, quint16> screen() const { return m_screen; }

    /// 带元状态注入原始按键（快进阶用法：组合键如 Ctrl+V）。
    mw::Result<bool> injectKeycodeRaw(qint32 keycode, KeyAction action, qint32 metaState);

    /// 设置 guest 剪贴板（paste=true 时同时触发粘贴）。
    mw::Result<bool> setClipboard(const QString& text, bool paste);

    mw::Result<bool> injectTouch(quint32 displayId, TouchAction action, quint64 pointerId,
                                 qint32 x, qint32 y) override;
    mw::Result<bool> injectKey(const AndroidKeycode& keycode, KeyAction action) override;
    mw::Result<bool> injectText(const QString& text) override;
    mw::Result<bool> injectScroll(quint32 displayId, qint32 x, qint32 y, float h,
                                  float v) override;

private:
    ScrcpyInjector() = default;
    mw::Result<bool> checkDisplay(quint32 displayId) const;

    mw::scrcpy::ControlChannel m_channel;
    /// 会话锚定的显示器（空 = 主屏，即 display 0）
    std::optional<quint32> m_displayId;
    /// 目标分辨率（坐标归一化基准）
    QPair<quint16, quint16> m_screen{0, 0};
    /// 会话锚点：视频 socket 保活（不消费）+ server 进程守卫。
    /// 空 = 通道由外部持有会话（from_channel 构造，测试/复用已有会话）。
    std::optional<mw::scrcpy::Session::Parts> m_session;
};

} // namespace mw::input
