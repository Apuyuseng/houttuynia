// 设备消息解析实现 ← crates/mwine-scrcpy/src/device_msg.rs
#include "device_msg.hpp"

namespace mw::scrcpy {

using namespace mw::scrcpy::device_msg;

namespace {

mw::Result<QString> readLp32String(StreamReader& reader)
{
    auto len = reader.readExact(4);
    if (!len.ok())
        return mw::err<QString>(len.error());
    if (len.value().isEmpty())
        return mw::err<QString>(errIo(QStringLiteral("eof before string length")));
    const quint32 n = beU32(len.value().constData());
    if (n == 0)
        return mw::Result<QString>::Ok(QString());
    auto buf = reader.readExact(int(n));
    if (!buf.ok())
        return mw::err<QString>(buf.error());
    if (buf.value().isEmpty())
        return mw::err<QString>(errIo(QStringLiteral("eof in the middle of string payload")));
    // from_utf8_lossy 语义：非法字节替换为 U+FFFD
    return mw::Result<QString>::Ok(QString::fromUtf8(buf.value()));
}

} // namespace

mw::Result<std::optional<DeviceMessage>> readDeviceMessage(StreamReader& reader)
{
    auto typeByte = reader.readExact(1);
    if (!typeByte.ok())
        return mw::err<std::optional<DeviceMessage>>(typeByte.error());
    if (typeByte.value().isEmpty())
        return mw::Result<std::optional<DeviceMessage>>::Ok(std::nullopt); // 流结束

    const quint8 type = quint8(typeByte.value().at(0));
    DeviceMessage m;
    switch (type) {
    case kTypeClipboard: {
        auto text = readLp32String(reader);
        if (!text.ok())
            return mw::err<std::optional<DeviceMessage>>(text.error());
        m.kind = DeviceMessage::Kind::Clipboard;
        m.text = text.value();
        return mw::Result<std::optional<DeviceMessage>>::Ok(std::move(m));
    }
    case kTypeAckClipboard: {
        auto seq = reader.readExact(8);
        if (!seq.ok())
            return mw::err<std::optional<DeviceMessage>>(seq.error());
        if (seq.value().isEmpty())
            return mw::err<std::optional<DeviceMessage>>(
                errIo(QStringLiteral("eof before clipboard ack sequence")));
        m.kind = DeviceMessage::Kind::AckClipboard;
        m.sequence = beU64(seq.value().constData());
        return mw::Result<std::optional<DeviceMessage>>::Ok(std::move(m));
    }
    case kTypeUhidOutput: {
        auto id = reader.readExact(2);
        if (!id.ok())
            return mw::err<std::optional<DeviceMessage>>(id.error());
        if (id.value().isEmpty())
            return mw::err<std::optional<DeviceMessage>>(
                errIo(QStringLiteral("eof before uhid id")));
        auto len = reader.readExact(2);
        if (!len.ok())
            return mw::err<std::optional<DeviceMessage>>(len.error());
        if (len.value().isEmpty())
            return mw::err<std::optional<DeviceMessage>>(
                errIo(QStringLiteral("eof before uhid data length")));
        m.kind = DeviceMessage::Kind::UhidOutput;
        m.id = beU16(id.value().constData());
        const quint16 n = beU16(len.value().constData());
        if (n > 0) {
            auto data = reader.readExact(int(n));
            if (!data.ok())
                return mw::err<std::optional<DeviceMessage>>(data.error());
            if (data.value().isEmpty())
                return mw::err<std::optional<DeviceMessage>>(
                    errIo(QStringLiteral("eof in the middle of uhid payload")));
            m.data = data.value();
        }
        return mw::Result<std::optional<DeviceMessage>>::Ok(std::move(m));
    }
    default:
        return mw::err<std::optional<DeviceMessage>>(
            errProtocol(QStringLiteral("unknown device message type: %1").arg(type)));
    }
}

} // namespace mw::scrcpy
