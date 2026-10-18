// ConsoleInjector 实现 ← crates/mwine-input/src/console.rs
#include "console.hpp"

#include <QFile>

#include <cmath>
#include <algorithm>

namespace mw::input {

namespace {

/// ABS_MT_SLOT 上限（设备 absinfo max=10 → 11 指）
constexpr quint64 kMaxSlots = 11;
constexpr qint32 kPressureOn = 50;

QString authTokenPath()
{
    const QByteArray home = qgetenv("HOME");
    const QString homeDir = home.isEmpty() ? QString() : QString::fromLocal8Bit(home);
    return homeDir + QStringLiteral("/.emulator_console_auth_token");
}

mw::Result<QString> readLine(mw::adb::TcpConn& conn)
{
    QByteArray buf;
    for (;;) {
        auto b = conn.readAtMost(1, 5000);
        if (!b.ok())
            return mw::err<QString>(errConsole(b.error()));
        const QByteArray& part = b.value();
        if (part.isEmpty())
            break; // EOF
        if (part.at(0) == '\n')
            break;
        buf.append(part);
        if (buf.size() > 4096)
            break;
    }
    return mw::Result<QString>::Ok(QString::fromUtf8(buf));
}

mw::Result<bool> readBanner(mw::adb::TcpConn& conn)
{
    // banner 固定 3~4 行，最后是带引号的 token 路径行（'~/...'）
    for (int i = 0; i < 8; ++i) {
        auto line = readLine(conn);
        if (!line.ok())
            return mw::err<bool>(line.error());
        const QString t = line.value().trimmed();
        if (t.startsWith(QLatin1Char('\'')) || t.isEmpty())
            return mw::Result<bool>::Ok();
    }
    return mw::Result<bool>::Ok();
}

} // namespace

quint16 consolePortOfSerial(const QString& serial)
{
    if (!serial.startsWith(QStringLiteral("emulator-")))
        return 0;
    const QString p = serial.mid(QString::fromLatin1("emulator-").size());
    bool ok = false;
    const quint16 port = quint16(p.toUShort(&ok));
    return ok ? port : 0;
}

mw::Result<QPair<quint16, quint16>> queryDisplaySize(const mw::adb::AdbClient& adb,
                                                     const QString& serial,
                                                     std::optional<quint32> displayId)
{
    const QString cmd = displayId.has_value()
                            ? QStringLiteral("wm size -d %1").arg(*displayId)
                            : QStringLiteral("wm size");
    auto out = adb.shellBlocking(serial, cmd, 8000);
    if (!out.ok())
        return mw::err<QPair<quint16, quint16>>(out.error());
    const QString text = out.value().outStr();
    std::optional<QPair<quint16, quint16>> resolved;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString kind = line.left(colon);
        const QString rest = line.mid(colon + 1);
        const QStringList dims = rest.split(QLatin1Char('x'));
        if (dims.size() != 2)
            continue;
        bool okW = false, okH = false;
        const quint16 w = quint16(dims[0].trimmed().toUShort(&okW));
        const quint16 h = quint16(dims[1].trimmed().toUShort(&okH));
        if (!okW || !okH)
            continue;
        // 输出行为 "Physical size:" / "Override size:"，按首词归类
        const QString first = kind.trimmed().split(QLatin1Char(' ')).value(0);
        if (first.compare(QLatin1String("Override"), Qt::CaseInsensitive) == 0)
            return mw::Result<QPair<quint16, quint16>>::Ok(qMakePair(w, h));
        if (first.compare(QLatin1String("Physical"), Qt::CaseInsensitive) == 0)
            resolved = qMakePair(w, h);
    }
    if (resolved.has_value())
        return mw::Result<QPair<quint16, quint16>>::Ok(*resolved);
    return mw::err<QPair<quint16, quint16>>(
        errAdb(QStringLiteral("wm size 输出无法解析: %1").arg(text.trimmed())));
}

QString touchCommand(QPair<quint16, quint16> screen, TouchAction action, quint32 slot,
                     qint32 tracking, qint32 x, qint32 y)
{
    auto axis = [&](qint32 v, quint16 dim) -> qint32 {
        const double a = std::round(double(v) / double(dim) * double(kAxisMax));
        return qint32(std::clamp(a, double(0), double(kAxisMax)));
    };
    const qint32 ax = axis(x, screen.first);
    const qint32 ay = axis(y, screen.second);
    switch (action) {
    case TouchAction::Down:
        return QStringLiteral(
                   "event send EV_ABS:ABS_MT_SLOT:%1 EV_ABS:ABS_MT_TRACKING_ID:%2 "
                   "EV_ABS:ABS_MT_POSITION_X:%3 EV_ABS:ABS_MT_POSITION_Y:%4 "
                   "EV_ABS:ABS_MT_PRESSURE:%5 EV_SYN:0:0")
            .arg(slot)
            .arg(tracking)
            .arg(ax)
            .arg(ay)
            .arg(kPressureOn);
    case TouchAction::Move:
        return QStringLiteral(
                   "event send EV_ABS:ABS_MT_SLOT:%1 "
                   "EV_ABS:ABS_MT_POSITION_X:%2 EV_ABS:ABS_MT_POSITION_Y:%3 "
                   "EV_ABS:ABS_MT_PRESSURE:%4 EV_SYN:0:0")
            .arg(slot)
            .arg(ax)
            .arg(ay)
            .arg(kPressureOn);
    case TouchAction::Up:
        return QStringLiteral(
                   "event send EV_ABS:ABS_MT_SLOT:%1 EV_ABS:ABS_MT_TRACKING_ID:-1 EV_SYN:0:0")
            .arg(slot);
    }
    return QString();
}

std::optional<QPair<quint32, qint32>> Fingers::alloc(quint64 pointerId)
{
    if (quint64(map.size()) >= kMaxSlots)
        return std::nullopt;
    quint32 slot = 0;
    for (quint32 s = 0; s < quint32(kMaxSlots); ++s) {
        bool used = false;
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (it.value().first == s) {
                used = true;
                break;
            }
        }
        if (!used) {
            slot = s;
            break;
        }
    }
    const qint32 tracking = nextTracking;
    nextTracking = (nextTracking + 1) % 65535;
    map.insert(pointerId, qMakePair(slot, tracking));
    return qMakePair(slot, tracking);
}

mw::Result<ConsoleInjector> ConsoleInjector::connect(quint16 consolePort,
                                                     QPair<quint16, quint16> screen)
{
    auto conn = mw::adb::TcpConn::connect(QStringLiteral("127.0.0.1"), consolePort, 5000);
    if (!conn.ok())
        return mw::err<ConsoleInjector>(errIo(conn.error()));
    mw::adb::TcpConn stream = std::move(conn.value());
    stream.setNoDelay(true);
    stream.setSocketTimeout(5000);

    QFile tokenFile(authTokenPath());
    if (!tokenFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return mw::err<ConsoleInjector>(
            errConsole(QStringLiteral("读取 console auth token 失败: %1").arg(authTokenPath())));
    }
    const QString token = QString::fromUtf8(tokenFile.readAll()).trimmed();

    // 服务器先发 banner（逐行读到 "Authentication required" 段落结束）
    {
        auto banner = readBanner(stream);
        if (!banner.ok())
            return mw::err<ConsoleInjector>(banner.error());
    }
    {
        auto w = stream.writeAll(QStringLiteral("auth %1\n").arg(token).toUtf8());
        if (!w.ok())
            return mw::err<ConsoleInjector>(errIo(w.error()));
        auto resp = readLine(stream);
        if (!resp.ok())
            return mw::err<ConsoleInjector>(resp.error());
        QString line = resp.value();
        while (line.trimmed().isEmpty()) {
            auto next = readLine(stream);
            if (!next.ok())
                return mw::err<ConsoleInjector>(next.error());
            line = next.value();
        }
        if (!line.trimmed().endsWith(QLatin1String("OK"))) {
            return mw::err<ConsoleInjector>(
                errConsole(QStringLiteral("console 鉴权失败: %1").arg(line)));
        }
    }
    return mw::Result<ConsoleInjector>::Ok(ConsoleInjector(std::move(stream), screen));
}

mw::Result<ConsoleInjector> ConsoleInjector::connectSerial(const QString& serial,
                                                            QPair<quint16, quint16> screen)
{
    const quint16 port = consolePortOfSerial(serial);
    if (port == 0)
        return mw::err<ConsoleInjector>(errConsole(QStringLiteral("非模拟器 serial: %1").arg(serial)));
    return connect(port, screen);
}

mw::Result<bool> ConsoleInjector::send(const QString& cmd)
{
    auto w = m_conn.writeAll((cmd + QLatin1Char('\n')).toUtf8());
    if (!w.ok())
        return mw::err<bool>(errIo(w.error()));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> ConsoleInjector::injectTouch(quint32 displayId, TouchAction action,
                                              quint64 pointerId, qint32 x, qint32 y)
{
    if (displayId != 0)
        return mw::err<bool>(errDisplayNotBound(displayId));
    switch (action) {
    case TouchAction::Down: {
        const auto finger = m_fingers.alloc(pointerId);
        if (!finger.has_value()) {
            return mw::err<bool>(
                errConsole(QStringLiteral("并发手指超过 11（ABS_MT_SLOT 上限）")));
        }
        const QString cmd = touchCommand(m_screen, action, finger->first, finger->second, x, y);
        return send(cmd);
    }
    case TouchAction::Move:
        if (m_fingers.map.contains(pointerId)) {
            const quint32 slot = m_fingers.map.value(pointerId).first;
            const QString cmd = touchCommand(m_screen, action, slot, 0, x, y);
            return send(cmd);
        }
        return mw::Result<bool>::Ok();
    case TouchAction::Up:
        if (m_fingers.map.contains(pointerId)) {
            const quint32 slot = m_fingers.map.take(pointerId).first;
            const QString cmd = touchCommand(m_screen, action, slot, 0, 0, 0);
            return send(cmd);
        }
        return mw::Result<bool>::Ok();
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> ConsoleInjector::injectKey(const AndroidKeycode&, KeyAction)
{
    // 键盘注入不在键位映射路径上（游戏映射只产触摸流）；
    // 需要 Back 等系统键时走 console `event send EV_KEY:...` 可扩展。
    return mw::err<bool>(errUnsupported(QStringLiteral("console 后端暂不支持按键注入")));
}

mw::Result<bool> ConsoleInjector::injectText(const QString&)
{
    return mw::err<bool>(errUnsupported(QStringLiteral("console 后端暂不支持文本注入")));
}

mw::Result<bool> ConsoleInjector::injectScroll(quint32, qint32, qint32, float, float)
{
    return mw::err<bool>(errUnsupported(QStringLiteral("console 后端暂不支持滚轮注入")));
}

} // namespace mw::input
