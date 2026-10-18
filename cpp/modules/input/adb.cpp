// AdbInjector 实现 ← crates/mwine-input/src/adb.rs
#include "adb.hpp"

#include <algorithm>

namespace mw::input {

QString shellQuote(const QString& s)
{
    QString out = QStringLiteral("'");
    for (const QChar c : s) {
        if (c == QLatin1Char('\''))
            out += QStringLiteral("'\\''");
        else
            out += c;
    }
    out += QLatin1Char('\'');
    return out;
}

QString textArg(const QString& text)
{
    QString t = text;
    t.replace(QLatin1Char(' '), QStringLiteral("%s"));
    return shellQuote(t);
}

QString displayPrefix(quint32 displayId)
{
    if (displayId == 0)
        return QString();
    return QStringLiteral("--display %1 ").arg(displayId);
}

TouchPlan planTouchUp(const PendingTouch& p, std::chrono::steady_clock::time_point now)
{
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.start)
                               .count();
    const quint64 duration = quint64(std::clamp<qint64>(elapsedMs, 0, 60000));
    if (p.down == p.last)
        return TouchPlan::tap(p.down.first, p.down.second);
    return TouchPlan::swipe(p.down.first, p.down.second, p.last.first, p.last.second,
                            std::max<quint64>(duration, 100));
}

QPair<qint32, qint32> scrollTarget(qint32 x, qint32 y, float h, float v)
{
    const auto clamp1 = [](float f) { return std::clamp(f, -1.0f, 1.0f); };
    const float d = float(kScrollDistancePx);
    return { x + qint32(std::lround(clamp1(h) * d)), y + qint32(std::lround(clamp1(v) * d)) };
}

mw::Result<bool> AdbInjector::runInput(const QString& args)
{
    auto out = m_adb.shellBlocking(m_serial, QStringLiteral("input %1").arg(args), m_timeoutMs);
    if (!out.ok())
        return mw::err<bool>(errAdb(out.error()));
    if (!out.value().ok()) {
        return mw::err<bool>(errUnsupported(QStringLiteral("input %1 失败: %2%3")
                                                .arg(args, out.value().outStr(), out.value().errStr())));
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> AdbInjector::injectTouch(quint32 displayId, TouchAction action, quint64 pointerId,
                                          qint32 x, qint32 y)
{
    switch (action) {
    case TouchAction::Down:
        if (m_pendingTouch.has_value()) {
            return mw::err<bool>(
                errUnsupported(QStringLiteral("adb input 后端不支持多指并发（用 scrcpy 后端）")));
        }
        m_pendingTouch = qMakePair(pointerId, PendingTouch{ { x, y }, { x, y },
                                                            std::chrono::steady_clock::now() });
        return mw::Result<bool>::Ok();
    case TouchAction::Move:
        // 无 Down 的 Move：忽略
        if (m_pendingTouch.has_value())
            m_pendingTouch->second.last = { x, y };
        return mw::Result<bool>::Ok();
    case TouchAction::Up:
        if (m_pendingTouch.has_value()) {
            const PendingTouch p = m_pendingTouch->second;
            m_pendingTouch.reset();
            const TouchPlan plan = planTouchUp(p);
            switch (plan.kind) {
            case TouchPlan::Kind::Tap:
                return runInput(QStringLiteral("%1tap %2 %3")
                                    .arg(displayPrefix(displayId))
                                    .arg(plan.x)
                                    .arg(plan.y));
            case TouchPlan::Kind::Swipe:
                return runInput(QStringLiteral("%1swipe %2 %3 %4 %5 %6")
                                    .arg(displayPrefix(displayId))
                                    .arg(plan.x1)
                                    .arg(plan.y1)
                                    .arg(plan.x2)
                                    .arg(plan.y2)
                                    .arg(plan.durationMs));
            case TouchPlan::Kind::None:
                return mw::Result<bool>::Ok();
            }
        }
        return mw::Result<bool>::Ok(); // 无 Down 的 Up：忽略
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> AdbInjector::injectKey(const AndroidKeycode& keycode, KeyAction action)
{
    const qint32 code = keycode.toI32();
    switch (action) {
    case KeyAction::Down:
        m_pendingKeys.insert(code, std::chrono::steady_clock::now());
        return mw::Result<bool>::Ok();
    case KeyAction::Up: {
        std::optional<std::chrono::steady_clock::time_point> held;
        if (m_pendingKeys.contains(code))
            held = m_pendingKeys.take(code);
        const bool longpress = held.has_value()
                               && std::chrono::steady_clock::now() - *held > m_longPressMs;
        return runInput(QStringLiteral("keyevent %1%2")
                            .arg(longpress ? QStringLiteral("--longpress ") : QString())
                            .arg(code));
    }
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> AdbInjector::injectText(const QString& text)
{
    if (text.isEmpty())
        return mw::Result<bool>::Ok();
    return runInput(QStringLiteral("text %1").arg(textArg(text)));
}

mw::Result<bool> AdbInjector::injectScroll(quint32 displayId, qint32 x, qint32 y, float h, float v)
{
    const auto [tx, ty] = scrollTarget(x, y, h, v);
    if (tx == x && ty == y)
        return mw::Result<bool>::Ok(); // 零位移滚动：no-op
    return runInput(QStringLiteral("%1swipe %2 %3 %4 %5 %6")
                        .arg(displayPrefix(displayId))
                        .arg(x)
                        .arg(y)
                        .arg(tx)
                        .arg(ty)
                        .arg(kScrollDurationMs));
}

} // namespace mw::input
