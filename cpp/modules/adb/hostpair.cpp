#include "hostpair.hpp"

#include <QRandomGenerator>

#include <chrono>
#include <thread>

namespace mw::adb {

namespace {

// 字母数字（去掉易混淆的 0/O、1/I/l，方便人工核对与口述）
constexpr char kCharset[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr int kCharsetLen = static_cast<int>(sizeof(kCharset)) - 1;
constexpr int kRandomLen = 10; // AS 同款：studio-<RANDOM-10>

QString randomToken()
{
    QString out;
    out.reserve(kRandomLen);
    for (int i = 0; i < kRandomLen; ++i)
        out.append(QLatin1Char(kCharset[QRandomGenerator::system()->bounded(kCharsetLen)]));
    return out;
}

void sleepCancelAware(int ms, const std::atomic_bool& cancelled)
{
    for (int slept = 0; slept < ms && !cancelled.load(std::memory_order_relaxed);)
    {
        const int step = qMin(100, ms - slept);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        slept += step;
    }
}

} // namespace

HostPairSession makeHostPairSession()
{
    HostPairSession s;
    s.service = QStringLiteral("studio-") + randomToken();
    s.password = randomToken();
    // 手机端 AdbQrCode 解析器要求：WIFI: 前缀、T 恰为 "ADB"、S/P 非空、';' 分隔
    s.qrPayload = QStringLiteral("WIFI:T:ADB;S:%1;P:%2;;").arg(s.service, s.password);
    return s;
}

mw::Result<QString> waitForPairingInstance(
    const QString& instance,
    const std::function<mw::Result<QVector<MdnsService>>()>& discover,
    int timeoutMs, int pollMs, const std::atomic_bool& cancelled)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (cancelled.load(std::memory_order_relaxed))
            return mw::err<QString>(QStringLiteral("已取消"));
        // 超时判定放在发现调用之前：单次发现含 2 个 adb 子进程（上限 ~25s），
        // 避免"到期后还要再跑一轮发现"拖长总时长
        if (std::chrono::steady_clock::now() >= deadline)
            return mw::err<QString>(QStringLiteral(
                "等待手机扫码超时（未发现配对服务 %1）").arg(instance));
        const auto r = discover();
        if (r.ok()) {
            for (const auto& s : r.value()) {
                if (s.isPairing() && s.instance == instance)
                    return mw::Result<QString>::Ok(s.addr());
            }
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return mw::err<QString>(QStringLiteral(
                "等待手机扫码超时（未发现配对服务 %1）").arg(instance));
        const int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count());
        sleepCancelAware(qMin(pollMs, remain), cancelled);
    }
}

} // namespace mw::adb
