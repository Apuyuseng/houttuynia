// mock.rs 的 C++ 移植。
#include "mock.hpp"

#include <QFileInfo>

namespace mw::app {

MockAppManager& MockAppManager::withApp(const QString& package,
                                        const std::optional<QString>& name)
{
    AppInfo info;
    info.package = package;
    info.name = name;
    info.version = QStringLiteral("1.0.0");
    apps.insert(package, info);
    return *this;
}

mw::Result<bool> MockAppManager::checkFail(const QString& package)
{
    if (failOn && *failOn == package)
        return mw::Result<bool>::Err(errShell(
            QStringLiteral("mock failure"),
            QStringLiteral("mock: 拒绝对 %1 的操作").arg(package)));
    return mw::Result<bool>::Ok();
}

mw::Result<AppInfo> MockAppManager::install(const QString& apkPath)
{
    QString package = nextInstallPackage.value_or(QString());
    if (package.isEmpty()) {
        const QString stem = QFileInfo(apkPath).completeBaseName();
        package = QStringLiteral("com.mock.%1").arg(stem.isEmpty() ? QStringLiteral("app") : stem);
    }
    if (auto fail = checkFail(package); !fail)
        return mw::err<AppInfo>(fail.error());
    AppInfo info;
    info.package = package;
    info.name = package;
    info.version = QStringLiteral("1.0.0-mock");
    apps.insert(package, info);
    return mw::Result<AppInfo>::Ok(std::move(info));
}

mw::Result<bool> MockAppManager::uninstall(const QString& package)
{
    if (auto fail = checkFail(package); !fail)
        return fail;
    if (!apps.contains(package))
        return mw::Result<bool>::Err(errNotInstalled(package));
    apps.remove(package);
    running.remove(package);
    uninstalledLog << package;
    return mw::Result<bool>::Ok();
}

mw::Result<bool> MockAppManager::launch(const QString& package, std::optional<quint32> displayId)
{
    if (auto fail = checkFail(package); !fail)
        return fail;
    if (!apps.contains(package))
        return mw::Result<bool>::Err(errNotInstalled(package));
    running.insert(package);
    launchLog.push_back({package, displayId});
    return mw::Result<bool>::Ok();
}

mw::Result<QVector<AppInfo>> MockAppManager::list(const ListOptions& options)
{
    QVector<AppInfo> out;
    for (const AppInfo& a : apps) {
        AppInfo copy = a;
        copy.isRunning = options.enrich && running.contains(copy.package);
        out.push_back(std::move(copy));
    }
    return mw::Result<QVector<AppInfo>>::Ok(std::move(out));
}

mw::Result<AppInfo> MockAppManager::get(const QString& package)
{
    if (!apps.contains(package))
        return mw::err<AppInfo>(errNotInstalled(package));
    return mw::Result<AppInfo>::Ok(apps.value(package));
}

mw::Result<bool> MockAppManager::stop(const QString& package)
{
    if (auto fail = checkFail(package); !fail)
        return fail;
    running.remove(package);
    stoppedLog << package;
    return mw::Result<bool>::Ok();
}

mw::Result<BoolValue> MockAppManager::isInstalled(const QString& package)
{
    return mw::Result<BoolValue>::Ok(BoolValue{apps.contains(package)});
}

} // namespace mw::app
