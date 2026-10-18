#include "protocol.hpp"

#include <QFileInfo>

namespace mw::app {

ProtocolAdbShell::ProtocolAdbShell(QString serial, QString adbProgram)
    : AdbShell(std::move(serial)), adbProgramHint_(std::move(adbProgram))
{
    // adb server 不在时拉起（幂等；失败留待各命令自行报错）
    adb_.ensureServer(adbProgramHint_);
}

mw::Result<ShellReply> ProtocolAdbShell::run(const QString& cmd, int timeoutMs)
{
    const auto out = adb_.shellBlocking(AdbShell::serial(), cmd, timeoutMs);
    if (!out)
        return mw::err<ShellReply>(errAdb(out.error()));
    QString text = out.value().outStr();
    if (!out.value().err.isEmpty()) {
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
            text += QLatin1Char('\n');
        text += QString::fromUtf8(out.value().err);
    }
    return mw::Result<ShellReply>::Ok(ShellReply{text, out.value().ok()});
}

mw::Result<QByteArray> ProtocolAdbShell::pullFile(const QString& remotePath, int timeoutMs)
{
    Q_UNUSED(timeoutMs) // sync 协议路径自身有套接字超时
    const auto out = adb_.pullFile(AdbShell::serial(), remotePath);
    if (!out)
        return mw::err<QByteArray>(errAdb(
            QStringLiteral("pull %1 失败: %2").arg(remotePath, out.error())));
    return mw::Result<QByteArray>::Ok(out.value());
}

mw::Result<bool> ProtocolAdbShell::installApk(const QString& apkPath, int timeoutMs,
                                              std::function<void(const InstallProgress&)> onProgress)
{
    const auto wrap = [&onProgress](const adb::InstallProgress& p) {
        if (!onProgress)
            return;
        if (p.kind == adb::InstallProgress::Kind::Push)
            onProgress(InstallProgress{InstallProgress::Phase::Push, p.sent, p.total});
        else
            onProgress(InstallProgress{InstallProgress::Phase::Installing, 0, 0});
    };
    const auto out = adb_.installApkWithProgress(AdbShell::serial(), apkPath, timeoutMs, wrap);
    if (!out)
        return mw::Result<bool>::Err(errAdb(
            QStringLiteral("install %1 失败: %2").arg(apkPath, out.error())));
    if (!out.value().contains(QLatin1String("Success")))
        return mw::Result<bool>::Err(errAdb(
            QStringLiteral("install %1 失败: %2").arg(apkPath, out.value().trimmed())));
    return mw::Result<bool>::Ok();
}

mw::Result<int> ProtocolAdbShell::grantRuntimePermissions(const QString& package)
{
    const auto out = adb_.grantRuntimePermissions(AdbShell::serial(), package);
    if (!out)
        return mw::err<int>(errAdb(out.error()));
    return mw::Result<int>::Ok(out.value());
}

} // namespace mw::app
