// adb 模块自适应接入：adb.hpp 由并行会话落地于 modules/adb/（已在 include 路径）。
// 就位前，依赖 adb 的函数体以「待 adb 模块接入」错误桩掉（保留签名）。
#pragma once

#if __has_include("adb.hpp")
#include "adb.hpp"
#define MW_SUPERVISOR_ADB_READY 1
#else
#define MW_SUPERVISOR_ADB_READY 0
#endif

namespace mw::supervisor {

inline constexpr const char* kAdbPendingMsg = "待 adb 模块接入";

#if MW_SUPERVISOR_ADB_READY

// ---------- 集中适配层：adb.hpp 实际形状如与假设不符，仅需调整本节 ----------
// 实际契约（modules/adb/client.hpp）：AdbClient{ host, port(quint16), direct }，
// 值类型构造（127.0.0.1:5037）；devices() → Result<QVector<AdbDevice{serial,state}>>；
// shellBlocking(serial, cmd, timeoutMs) → Result<ShellOutput{outStr()}>。

// shell/connectNetwork 等调用的值形态归一：
// QString / QByteArray / 带 outStr()（ShellOutput）或 stdOutStr() 的结构体均可。
inline QString adbOutTextImpl(const QString& v, int) { return v; }
inline QString adbOutTextImpl(const QByteArray& v, int) { return QString::fromUtf8(v); }
template <typename T>
auto adbOutTextImpl(const T& v, int) -> decltype(v.outStr()) { return v.outStr(); }
template <typename T>
auto adbOutTextImpl(const T& v, long) -> decltype(v.stdOutStr()) { return v.stdOutStr(); }

template <typename T>
QString adbOutText(const T& v)
{
    return adbOutTextImpl(v, 0);
}

// AdbClient::port 的取值（成员变量或方法均可；均无则回落 5037）
template <typename T, typename = void>
struct AdbHasPortCall : std::false_type {};
template <typename T>
struct AdbHasPortCall<T, std::void_t<decltype(std::declval<const T&>().port())>>
    : std::true_type {};
template <typename T, typename = void>
struct AdbHasPortData : std::false_type {};
template <typename T>
struct AdbHasPortData<T, std::void_t<decltype(std::declval<const T&>().port)>> : std::true_type {};

template <typename T>
int adbServerPortOf(const T* adb)
{
    if constexpr (AdbHasPortCall<T>::value)
        return int(adb->port());
    else if constexpr (AdbHasPortData<T>::value)
        return int(adb->port);
    else
        return 5037;
}

#endif // MW_SUPERVISOR_ADB_READY

} // namespace mw::supervisor
