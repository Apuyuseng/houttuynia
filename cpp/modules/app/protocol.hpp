// ProtocolAdbShell —— AdbShell 的 mw_adb 协议栈实现（无外部 adb 进程依赖）。
// 同一面（run/pullFile/installApk/grantRuntimePermissions）直接走 smart-socket
// 协议：省每命令一次 QProcess fork，且安装有真实字节级进度。
// 行为对齐 QProcess 版：run 的 stdout/stderr 追加合流、ok 按退出码（v1 无退出码
// 时按 ShellOutput::ok()）。
#pragma once

#include "manager.hpp"

#include "client.hpp"

namespace mw::app {

class ProtocolAdbShell : public AdbShell {
public:
    // adbProgram：ensureServer 拉起 adb server 用（可执行文件或 PATH 名）
    explicit ProtocolAdbShell(QString serial, QString adbProgram = QStringLiteral("adb"));
    ~ProtocolAdbShell() override = default;

    mw::Result<ShellReply> run(const QString& cmd, int timeoutMs) override;
    mw::Result<QByteArray> pullFile(const QString& remotePath, int timeoutMs = 60000) override;
    mw::Result<bool> installApk(const QString& apkPath, int timeoutMs = 600000,
                                std::function<void(const InstallProgress&)> onProgress = {}) override;
    mw::Result<int> grantRuntimePermissions(const QString& package) override;

private:
    adb::AdbClient adb_;
    QString adbProgramHint_; // ensureServer 拉起 adb server 用
};

} // namespace mw::app
