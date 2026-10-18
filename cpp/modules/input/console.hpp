// Android 模拟器控制台注入后端（原生零依赖通道，**不使用 scrcpy**）。
// ← crates/mwine-input/src/console.rs
//
// 通道：宿主 TCP → 模拟器 qemu 进程内 console（`event send`）→
// goldfish/virtio 多点触控设备模型 → guest 内核 input 子系统。
// 事件以"真实硬件触摸"身份进入内核（无 InputManager injected 标记），
// 单次注入往返微秒级（实测 getevent 时间戳 <1ms），游戏级延迟要求下的
// 本机最优路径——与模拟器自身 UI 输入同源。
//
// 协议要点（Type B 多点触控）：
// - 坐标域 `ABS_MT_POSITION_X/Y` = 0..32767（按 `wm size` 分辨率换算）
// - `ABS_MT_SLOT`（0..10）选手指；`ABS_MT_TRACKING_ID`：>=0 新触点/保持，
//   -1 抬起该 slot
// - `EV_SYN:0:0`（SYN_REPORT 无别名，必须用数字 code）
// - `ABS_MT_PRESSURE` 0..1024
//
// 鉴权：`~/.emulator_console_auth_token`，连接后发 `auth <token>`。
#pragma once

#include <QHash>
#include <QPair>
#include <QString>

#include <optional>

#include "client.hpp"
#include "input.hpp"
#include "result.hpp"
#include "tcpconn.hpp"

namespace mw::input {

/// 触摸轴坐标域（virtio_input_multi_touch 设备 ABS max，实测一致）
inline constexpr qint32 kAxisMax = 32767;

/// 从 adb serial 推导 console 端口（emulator-5554 → 5554）。失败返回 0。
quint16 consolePortOfSerial(const QString& serial);

/// 查询 guest 显示器分辨率（`wm size`，与注入通道无关的纯 adb 查询）。
/// 优先 Override（生效值），回退 Physical。
mw::Result<QPair<quint16, quint16>> queryDisplaySize(const mw::adb::AdbClient& adb,
                                                     const QString& serial,
                                                     std::optional<quint32> displayId);

/// 纯函数：一次触摸更新 → console `event send` 命令行。
/// 坐标为 guest 像素，按 screen 换算到 0..32767。
QString touchCommand(QPair<quint16, quint16> screen, TouchAction action, quint32 slot,
                     qint32 tracking, qint32 x, qint32 y);

/// 活动手指表：pointerId → (slot, trackingId)
struct Fingers {
    QHash<quint64, QPair<quint32, qint32>> map;
    qint32 nextTracking = 0;

    /// 为 pointerId 分配空闲 slot（0..10）与递增 tracking id
    /// （0..65534，避开 -1 哨兵），直接登记。表满返回空。
    std::optional<QPair<quint32, qint32>> alloc(quint64 pointerId);
};

/// console 注入后端：一条到模拟器 console 的持久 TCP 连接。
class ConsoleInjector : public InputInjector {
public:
    /// 连接并完成 auth。screen = `wm size` 分辨率（坐标换算基准）。
    static mw::Result<ConsoleInjector> connect(quint16 consolePort,
                                               QPair<quint16, quint16> screen);
    /// 从 serial 出发的一站式连接（emulator-5554 → port 5554）。
    static mw::Result<ConsoleInjector> connectSerial(const QString& serial,
                                                     QPair<quint16, quint16> screen);

    QPair<quint16, quint16> screen() const { return m_screen; }
    /// 分辨率变化时更新换算基准（连接不重建）。
    void setScreen(QPair<quint16, quint16> screen) { m_screen = screen; }

    mw::Result<bool> injectTouch(quint32 displayId, TouchAction action, quint64 pointerId,
                                 qint32 x, qint32 y) override;
    mw::Result<bool> injectKey(const AndroidKeycode& keycode, KeyAction action) override;
    mw::Result<bool> injectText(const QString& text) override;
    mw::Result<bool> injectScroll(quint32 displayId, qint32 x, qint32 y, float h,
                                  float v) override;

private:
    ConsoleInjector(mw::adb::TcpConn conn, QPair<quint16, quint16> screen)
        : m_conn(std::move(conn)), m_screen(screen)
    {
    }
    mw::Result<bool> send(const QString& cmd);

    mw::adb::TcpConn m_conn;
    QPair<quint16, quint16> m_screen{0, 0};
    Fingers m_fingers;
};

} // namespace mw::input
