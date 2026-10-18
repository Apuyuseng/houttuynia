// mw::scrcpy 错误文案构造。
// ← crates/mwine-scrcpy/src/error.rs（Rust enum Error → 本项目统一的 QString 错误约定）
#pragma once

#include <QString>

namespace mw::scrcpy {

inline QString errAdb(const QString& m) { return QStringLiteral("adb error: ") + m; }
inline QString errServerExited(const QString& m)
{
    return QStringLiteral("scrcpy server exited unexpectedly: ") + m;
}
inline QString errLaunchFailed(const QString& m) { return QStringLiteral("launch step failed: ") + m; }
inline QString errConnectTimeout() { return QStringLiteral("connection to scrcpy server timed out"); }
inline QString errProtocol(const QString& m) { return QStringLiteral("protocol error: ") + m; }
inline QString errIo(const QString& m) { return QStringLiteral("io error: ") + m; }
inline QString errDecode(const QString& m) { return QStringLiteral("decode error: ") + m; }
inline QString errServerJarNotFound(const QString& m)
{
    return QStringLiteral("server jar not found: ") + m;
}
inline QString errInvalidOption(const QString& m) { return QStringLiteral("invalid option: ") + m; }

} // namespace mw::scrcpy
