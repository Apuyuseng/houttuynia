// 测试与演示用 mock 启动器（测试用 mock 实现）。
// ← crates/mwine-qemu/src/mock.rs
//
// 供 mwine-display（M2）/ mwine-input（M3）等下游在无真机时开发联调：
// 返回预设端口的 detached 句柄，console 命令由应答函数回答。
#pragma once

#include <QByteArray>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <optional>

#include "launcher.hpp"

namespace mw::qemu {

/// mock 启动器：不 spawn 任何进程。Rust MockLauncher。
class MockLauncher : public QemuLauncher {
public:
    /// 预设返回的 (console_port, grpc_port)。
    QPair<quint16, quint16> ports{ 5554, 8554 };
    /// console 命令应答函数（默认回 OK\r\n）。Rust console_responder。
    std::function<QByteArray(const QString&)> consoleResponder;

    MockLauncher();
    static MockLauncher withPorts(quint16 console, quint16 grpc);

    mw::Result<QemuHandle> launch(const QemuConfig& config) const override;

    /// 捕获到的最近一次启动配置（断言用）。Rust last_config。
    std::optional<QemuConfig> capturedConfig() const;

private:
    // QMutex 不可移动：端口预设经此构造函数直出（C++17 保证拷贝省略）
    explicit MockLauncher(quint16 console, quint16 grpc);

    mutable QMutex mutex_;
    mutable std::optional<QemuConfig> lastConfig_; // Rust Mutex<Option<QemuConfig>> 的内部可变语义
};

} // namespace mw::qemu
