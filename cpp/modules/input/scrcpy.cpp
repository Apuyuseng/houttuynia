// ScrcpyInjector 实现 ← crates/mwine-input/src/scrcpy.rs
#include "scrcpy.hpp"

#include "console.hpp"
#include "scrcpy/options.hpp"

namespace mw::input {

namespace {
constexpr quint32 kAnchorMaxSize = 320;      // 最低开销视频锚
constexpr float kAnchorMaxFps = 1.0f;
} // namespace

mw::scrcpy::ControlMessage touchMessage(QPair<quint16, quint16> screen, TouchAction action,
                                        quint64 pointerId, qint32 x, qint32 y)
{
    const bool pressed = action != TouchAction::Up;
    return mw::scrcpy::ControlMessage::injectTouch(
        touchActionU8(action), pointerId, x, y, screen.first, screen.second,
        pressed ? 1.0f : 0.0f, 1, pressed ? 1 : 0);
}

mw::scrcpy::ControlMessage scrollMessage(QPair<quint16, quint16> screen, qint32 x, qint32 y,
                                         float h, float v)
{
    return mw::scrcpy::ControlMessage::injectScroll(x, y, screen.first, screen.second, h, v, 0);
}

mw::scrcpy::ControlMessage keyMessage(const AndroidKeycode& keycode, KeyAction action)
{
    return mw::scrcpy::ControlMessage::injectKeycode(
        action == KeyAction::Down ? mw::scrcpy::KeyAction::Down : mw::scrcpy::KeyAction::Up,
        keycode.toI32(), 0, 0);
}

QVector<QByteArray> splitText(const QString& text)
{
    using mw::scrcpy::control::kInjectTextMax;

    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= kInjectTextMax)
        return { utf8 };

    QVector<QByteArray> chunks;
    int start = 0;
    while (start < utf8.size()) {
        int end = std::min(start + kInjectTextMax, int(utf8.size()));
        // 不撕裂 UTF-8：回退到字符边界（连续字节 (b & 0xC0) == 0x80）
        while (end > start && (quint8(utf8[end]) & 0xC0) == 0x80)
            --end;
        if (end == start)
            break; // 单字符超 300 字节（不可能的 UTF-8），防御
        chunks.append(utf8.mid(start, end - start));
        start = end;
    }
    return chunks;
}

ScrcpyInjector::ScrcpyInjector(mw::scrcpy::ControlChannel channel,
                               std::optional<quint32> displayId, QPair<quint16, quint16> screen)
    : m_channel(std::move(channel)), m_displayId(displayId), m_screen(screen)
{
}

mw::Result<bool> ScrcpyInjector::checkDisplay(quint32 displayId) const
{
    if (displayId == boundDisplay())
        return mw::Result<bool>::Ok();
    return mw::err<bool>(errDisplayNotBound(displayId));
}

mw::Result<ScrcpyInjector> ScrcpyInjector::connect(const mw::adb::AdbClient& adb,
                                                   const QString& serial,
                                                   std::optional<quint32> displayId,
                                                   std::optional<QPair<quint16, quint16>> screen)
{
    const QString jar = mw::scrcpy::findServerJar();
    if (jar.isEmpty()) {
        return mw::err<ScrcpyInjector>(errScrcpy(mw::scrcpy::errServerJarNotFound(
            QStringLiteral("~/.mwine/bin/scrcpy-server-v4.0"))));
    }
    QPair<quint16, quint16> resolvedScreen{0, 0};
    if (screen.has_value()) {
        resolvedScreen = *screen;
    } else {
        auto s = queryDisplaySize(adb, serial, displayId);
        if (!s.ok())
            return mw::err<ScrcpyInjector>(errAdb(s.error()));
        resolvedScreen = s.value();
    }

    mw::scrcpy::ServerOptions options;
    options.video = true;
    options.audio = false;
    options.control = true;
    options.maxSize = kAnchorMaxSize;
    options.maxFps = kAnchorMaxFps;

    mw::scrcpy::ScrcpyClient client(adb, serial, jar);
    client.options = options;
    auto started = std::move(client).start();
    if (!started.ok())
        return mw::err<ScrcpyInjector>(errScrcpy(started.error()));

    mw::scrcpy::Session::Parts parts = started.value().split();
    if (!parts.control.has_value()) {
        return mw::err<ScrcpyInjector>(
            errScrcpy(mw::scrcpy::errProtocol(QStringLiteral("server 未提供控制通道"))));
    }

    ScrcpyInjector inj;
    inj.m_channel = std::move(*parts.control);
    inj.m_displayId = displayId;
    inj.m_screen = resolvedScreen;
    inj.m_session = std::move(parts);
    return mw::Result<ScrcpyInjector>::Ok(std::move(inj));
}

mw::Result<bool> ScrcpyInjector::injectKeycodeRaw(qint32 keycode, KeyAction action,
                                                  qint32 metaState)
{
    return m_channel.send(mw::scrcpy::ControlMessage::injectKeycode(
        action == KeyAction::Down ? mw::scrcpy::KeyAction::Down : mw::scrcpy::KeyAction::Up,
        keycode, 0, metaState));
}

mw::Result<bool> ScrcpyInjector::setClipboard(const QString& text, bool paste)
{
    return m_channel.send(mw::scrcpy::ControlMessage::setClipboard(0, paste, text));
}

mw::Result<bool> ScrcpyInjector::injectTouch(quint32 displayId, TouchAction action,
                                             quint64 pointerId, qint32 x, qint32 y)
{
    auto ok = checkDisplay(displayId);
    if (!ok.ok())
        return ok;
    return m_channel.send(touchMessage(m_screen, action, pointerId, x, y));
}

mw::Result<bool> ScrcpyInjector::injectKey(const AndroidKeycode& keycode, KeyAction action)
{
    return m_channel.send(keyMessage(keycode, action));
}

mw::Result<bool> ScrcpyInjector::injectText(const QString& text)
{
    for (const QByteArray& chunk : splitText(text)) {
        auto r = m_channel.send(
            mw::scrcpy::ControlMessage::injectText(QString::fromUtf8(chunk)));
        if (!r.ok())
            return r;
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> ScrcpyInjector::injectScroll(quint32 displayId, qint32 x, qint32 y, float h,
                                              float v)
{
    auto ok = checkDisplay(displayId);
    if (!ok.ok())
        return ok;
    return m_channel.send(scrollMessage(m_screen, x, y, h, v));
}

} // namespace mw::input
