// AppManager 的可编程 mock：供守护进程 / UI 并行开发与测试。
// 对应 crates/mwine-app/src/mock.rs（Rust feature="mock"；C++ 侧始终编译供测试复用）。
// 状态驱动——预置 installed/running 集合，操作改动集合并记录调用日志，供断言。
#pragma once

#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

#include "types.hpp"

namespace mw::app {

class MockAppManager : public AppManager {
public:
    // package -> AppInfo（install/get/list 的数据源）
    QMap<QString, AppInfo> apps;
    // 模拟运行中的包
    QSet<QString> running;
    // install() 接下来要“装”的包（默认按文件名推测 com.mock.<stem>）
    std::optional<QString> nextInstallPackage;
    // 调用日志：launch 的 (package, displayId)
    QVector<QPair<QString, std::optional<quint32>>> launchLog;
    QStringList uninstalledLog;
    QStringList stoppedLog;
    // 对匹配包名的操作强制失败（模拟设备错误）
    std::optional<QString> failOn;

    // 预置一个应用。
    MockAppManager& withApp(const QString& package, const std::optional<QString>& name = std::nullopt);

    // ---- AppManager 接口 ----
    mw::Result<AppInfo> install(const QString& apkPath) override;
    mw::Result<bool> uninstall(const QString& package) override;
    mw::Result<bool> launch(const QString& package, std::optional<quint32> displayId) override;
    mw::Result<QVector<AppInfo>> list(const ListOptions& options) override;
    mw::Result<AppInfo> get(const QString& package) override;
    mw::Result<bool> stop(const QString& package) override;
    mw::Result<BoolValue> isInstalled(const QString& package) override;

private:
    mw::Result<bool> checkFail(const QString& package);
};

} // namespace mw::app
