// 启动器：环境组装 + spawn（替代 SDK emulator launcher 的核心职责）。
// ← crates/mwine-qemu/src/launcher.rs
#pragma once

#include <QString>

#include <optional>

#include "config.hpp"
#include "handle.hpp"
#include "result.hpp"

namespace mw::qemu {

/// qemu 启动器接口（mock 实现见 mock.hpp）。Rust trait QemuLauncher。
class QemuLauncher {
public:
    virtual ~QemuLauncher() = default;
    virtual mw::Result<QemuHandle> launch(const QemuConfig& config) const = 0;
};

/// 直启实现：用实测最小环境集 exec qemu-system-*-headless。Rust DirectLauncher。
class DirectLauncher : public QemuLauncher {
public:
    QemuRuntime runtime;
    /// qemu stdout/stderr 落盘路径（支持排障）；空时丢弃。
    std::optional<QString> logFile;

    explicit DirectLauncher(QemuRuntime rt);
    /// 从本机 Android SDK 布局探测（开发机/CLI 路径）。
    static mw::Result<DirectLauncher> detect();

    mw::Result<QemuHandle> launch(const QemuConfig& config) const override;
};

} // namespace mw::qemu
