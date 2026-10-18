// 运行中的 qemu 实例句柄 ← crates/mwine-qemu/src/handle.rs
#include "handle.hpp"

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "console.hpp"

namespace mw::qemu {

struct QemuHandle::Impl {
    quint32 pid = 0;
    quint16 consolePort = 0;
    quint16 grpcPort = 0;
    QString adbSerial; // emulator-<console_port>
    QString token;     // console auth token（构造时快照）
    mw::adb::AdbClient adb;

    mutable QMutex mutex; // 序列化 waitpid/kill（Rust Mutex<Option<Child>> 的对应物）
    bool hasChild = false; // detached 句柄无进程
    bool reaped = false;   // 已被 waitpid 收割（Rust Child::try_wait 的 Some 状态）

    ~Impl()
    {
        // 对齐 Rust：句柄 Drop 不杀进程；若子进程已自行退出则顺手收尸防僵尸。
        if (hasChild && !reaped) {
            int st = 0;
            if (::waitpid(pid_t(pid), &st, WNOHANG) == pid_t(pid))
                reaped = true;
        }
    }

    // waitpid(WNOHANG) 探活：返回 true=仍在运行；已退出则记录收割状态。
    bool tryWaitRunningLocked()
    {
        if (!hasChild || reaped)
            return false;
        int st = 0;
        const pid_t r = ::waitpid(pid_t(pid), &st, WNOHANG);
        if (r == 0)
            return true; // 仍在运行
        if (r == pid_t(pid))
            reaped = true; // 刚收割到退出状态
        // r < 0（ECHILD 等）：Rust try_wait Err → 视为不在运行
        return false;
    }
};

// ===================== 构造 =====================

QemuHandle QemuHandle::detached(quint16 consolePort, quint16 grpcPort)
{
    auto impl = std::make_shared<Impl>();
    impl->consolePort = consolePort;
    impl->grpcPort = grpcPort;
    impl->pid = 0;
    impl->adbSerial = QStringLiteral("emulator-%1").arg(consolePort);
    impl->token = consoleToken();
    return QemuHandle(std::move(impl));
}

QemuHandle QemuHandle::fromChild(quint32 pid, quint16 consolePort, quint16 grpcPort)
{
    auto impl = std::make_shared<Impl>();
    impl->pid = pid;
    impl->consolePort = consolePort;
    impl->grpcPort = grpcPort;
    impl->adbSerial = QStringLiteral("emulator-%1").arg(consolePort);
    impl->token = consoleToken();
    impl->hasChild = true;
    return QemuHandle(std::move(impl));
}

quint32 QemuHandle::pid() const { return impl_ ? impl_->pid : 0; }
quint16 QemuHandle::consolePort() const { return impl_ ? impl_->consolePort : 0; }
quint16 QemuHandle::grpcPort() const { return impl_ ? impl_->grpcPort : 0; }
QString QemuHandle::adbSerial() const { return impl_ ? impl_->adbSerial : QString(); }

// ===================== console =====================

mw::Result<QByteArray> QemuHandle::sendConsole(const QString& cmd) const
{
    return consoleCommand(impl_->consolePort, impl_->token, cmd);
}

bool QemuHandle::isRunning() const
{
    QMutexLocker lock(&impl_->mutex);
    // detached 句柄无从判断，保守视为不在运行
    return impl_->tryWaitRunningLocked();
}

// ===================== adb / boot 等待 =====================

mw::Result<bool> QemuHandle::waitAdb(int timeoutMs) const
{
    (void)impl_->adb.ensureServer(); // Rust: ensure_server(None).ok()
    QElapsedTimer t0;
    t0.start();
    while (true) {
        if (auto dr = impl_->adb.devices(); dr) {
            for (const auto& d : dr.value()) {
                if (d.serial == impl_->adbSerial && d.state == QStringLiteral("device"))
                    return mw::Result<bool>::Ok();
            }
        }
        if (t0.elapsed() > qint64(timeoutMs))
            // Error::SerialTimeout(serial)
            return mw::err<bool>(QStringLiteral("等待 %1 出现在 adb 设备列表超时").arg(impl_->adbSerial));
        QThread::msleep(800);
    }
}

mw::Result<bool> QemuHandle::waitBoot(int timeoutMs) const
{
    QElapsedTimer t0;
    t0.start();
    if (auto r = waitAdb(timeoutMs); !r)
        return mw::err<bool>(r.error()); // Error::SerialTimeout 透传
    // 剩余预算（Rust checked_sub → 负数兜底 1s）
    qint64 remain = qint64(timeoutMs) - t0.elapsed();
    if (remain < 1000)
        remain = 1000;

    QElapsedTimer t;
    t.start();
    while (true) {
        if (auto r = impl_->adb.shellBlocking(impl_->adbSerial,
                                              QStringLiteral("getprop sys.boot_completed"), 5000);
            r) {
            if (r.value().outStr().trimmed() == QStringLiteral("1")) {
                const auto anim = impl_->adb.shellBlocking(
                    impl_->adbSerial, QStringLiteral("getprop init.svc.bootanim"), 5000);
                const bool stopped = !anim || anim.value().outStr().trimmed() == QStringLiteral("stopped");
                if (stopped)
                    return mw::Result<bool>::Ok();
            }
        }
        if (t.elapsed() > remain)
            return mw::err<bool>(QStringLiteral("qemu 引导超时（sys.boot_completed 未达 1）")); // Error::BootTimeout
        QThread::msleep(1000);
    }
}

// ===================== multidisplay =====================

mw::Result<bool> QemuHandle::multidisplayAdd(quint32 id, quint32 w, quint32 h, quint32 dpi) const
{
    auto resp = sendConsole(
        QStringLiteral("multidisplay add %1 %2 %3 %4 0").arg(id).arg(w).arg(h).arg(dpi));
    if (!resp)
        return mw::err<bool>(resp.error());
    if (isKo(resp.value()))
        return mw::err<bool>(QStringLiteral("multidisplay add 被拒: %1")
                                 .arg(QString::fromUtf8(resp.value()).trimmed()));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> QemuHandle::multidisplayDel(quint32 id) const
{
    auto resp = sendConsole(QStringLiteral("multidisplay del %1").arg(id));
    if (!resp)
        return mw::err<bool>(resp.error());
    if (isKo(resp.value()))
        return mw::err<bool>(QStringLiteral("multidisplay del 被拒: %1")
                                 .arg(QString::fromUtf8(resp.value()).trimmed()));
    return mw::Result<bool>::Ok();
}

// ===================== 关停 =====================

mw::Result<bool> QemuHandle::shutdown() const
{
    (void)sendConsole(QStringLiteral("kill"));
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 20000) {
        {
            QMutexLocker lock(&impl_->mutex);
            if (!impl_->hasChild)
                return mw::Result<bool>::Ok(); // detached：无进程句柄关停立即成功
            if (!impl_->tryWaitRunningLocked())
                return mw::Result<bool>::Ok(); // 已退出
        }
        QThread::msleep(500);
    }
    {
        QMutexLocker lock(&impl_->mutex);
        if (impl_->hasChild && !impl_->reaped) {
            // SIGKILL 兜底（Rust child.kill() + wait()）
            ::kill(pid_t(impl_->pid), SIGKILL);
            int st = 0;
            if (::waitpid(pid_t(impl_->pid), &st, 0) == pid_t(impl_->pid))
                impl_->reaped = true;
        }
    }
    return mw::Result<bool>::Ok();
}

} // namespace mw::qemu
