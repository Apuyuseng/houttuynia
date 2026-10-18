// 运行中的 qemu 实例句柄：console 命令、boot 等待、优雅关停。
// ← crates/mwine-qemu/src/handle.rs
//
// 与 Rust 版一致：句柄析构不会杀进程——守护进程场景要求 qemu 超越本进程生命周期。
// 因此子进程不用 QProcess（其析构会 kill），而以 pid + POSIX waitpid/kill 直接管理
//（等价于 Rust std::process::Child：drop 不杀、try_wait 探活、kill 兜底）。
//
// `serial` 约定为 `emulator-<console_port>`（与 qemu `-ports c,c+1` 布局一致）。
// 内部状态（child/收割标记/token/adb 客户端）藏在 shared_ptr<Impl>，
// 对应 Rust 的 Mutex<Option<Child>> + 字段拷贝语义：句柄可自由移动/共享，
// isRunning()/sendConsole()/shutdown() 均可 const 调用。
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <memory>

#include "adb.hpp"
#include "result.hpp"

namespace mw::qemu {

class QemuHandle {
public:
    /// 无进程句柄（mock / 重连已存在实例用）。pid 置 0 表示非真实进程。
    static QemuHandle detached(quint16 consolePort, quint16 grpcPort);

    QemuHandle() = default; // 无效句柄（可选语义，与 detached 区分：impl 为空）

    // —— Rust pub 字段的只读访问器 ——
    quint32 pid() const;          ///< 0 = detached（非真实进程）
    quint16 consolePort() const;
    quint16 grpcPort() const;
    QString adbSerial() const;    ///< emulator-<console_port>

    bool isValid() const { return impl_ != nullptr; }

    /// 执行一条 emulator console 命令（kill/multidisplay/redir/avd/...），
    /// 返回原始应答文本。
    mw::Result<QByteArray> sendConsole(const QString& cmd) const;

    bool isRunning() const;

    /// 等待实例出现在 adb 设备列表且状态为 device（M1 验收：adb 可连）。
    /// 自建镜像冷启 guest adbd 上线可能要 1-2 分钟。
    mw::Result<bool> waitAdb(int timeoutMs) const;

    /// 等待系统完成引导（sys.boot_completed=1 且 bootanim stopped）。
    /// 内部先等 adb 可连（共享超时预算）。
    mw::Result<bool> waitBoot(int timeoutMs) const;

    /// 为应用创建独立显示器（每显示器 = 一个宿主渲染目标）。
    /// `multidisplay add` 是 upsert 语义——同 id 重复调用即缩放。
    mw::Result<bool> multidisplayAdd(quint32 id, quint32 w, quint32 h, quint32 dpi) const;

    /// 移除显示器。
    mw::Result<bool> multidisplayDel(quint32 id) const;

    /// 优雅关停：console kill → 20s 等待退出 → SIGKILL 兜底。
    mw::Result<bool> shutdown() const;

private:
    friend class DirectLauncher; // launch 构造真实句柄（Rust QemuHandle::new 为 pub(super)）
    static QemuHandle fromChild(quint32 pid, quint16 consolePort, quint16 grpcPort);

    struct Impl;
    explicit QemuHandle(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

    std::shared_ptr<Impl> impl_;
};

} // namespace mw::qemu
