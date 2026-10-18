// 测试与演示用 mock 启动器 ← crates/mwine-qemu/src/mock.rs
#include "mock.hpp"

#include <QMutexLocker>

namespace mw::qemu {

MockLauncher::MockLauncher()
{
    consoleResponder = [](const QString&) { return QByteArrayLiteral("OK\r\n"); };
}

MockLauncher::MockLauncher(quint16 console, quint16 grpc)
    : ports{ console, grpc }
{
    consoleResponder = [](const QString&) { return QByteArrayLiteral("OK\r\n"); };
}

MockLauncher MockLauncher::withPorts(quint16 console, quint16 grpc)
{
    return MockLauncher(console, grpc); // C++17 保证省略拷贝（QMutex 不可移动）
}

mw::Result<QemuHandle> MockLauncher::launch(const QemuConfig& config) const
{
    if (!config.imageDir.has_value() && config.avdName.isEmpty())
        return mw::err<QemuHandle>(errNoTarget());
    {
        QMutexLocker lock(&mutex_);
        lastConfig_ = config;
    }
    return mw::Result<QemuHandle>::Ok(QemuHandle::detached(ports.first, ports.second));
}

std::optional<QemuConfig> MockLauncher::capturedConfig() const
{
    QMutexLocker lock(&mutex_);
    return lastConfig_;
}

} // namespace mw::qemu
