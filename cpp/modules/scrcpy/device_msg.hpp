// 设备 -> 宿主 消息（控制通道反向）。
// 与 server 端 DeviceMessageWriter.java 对齐，大端。
// ← crates/mwine-scrcpy/src/device_msg.rs
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

#include "io.hpp"
#include "result.hpp"

namespace mw::scrcpy::device_msg {

inline constexpr quint8 kTypeClipboard = 0;
inline constexpr quint8 kTypeAckClipboard = 1;
inline constexpr quint8 kTypeUhidOutput = 2;

} // namespace mw::scrcpy::device_msg

namespace mw::scrcpy {

struct DeviceMessage {
    enum class Kind { Clipboard, AckClipboard, UhidOutput };

    Kind kind = Kind::Clipboard;
    QString text;      ///< Clipboard
    quint64 sequence = 0; ///< AckClipboard
    quint16 id = 0;    ///< UhidOutput
    QByteArray data;   ///< UhidOutput
};

/// 从 reader 读取一条完整的设备消息。
/// 返回 Ok(nullopt) 表示流已结束。
mw::Result<std::optional<DeviceMessage>> readDeviceMessage(StreamReader& reader);

} // namespace mw::scrcpy
