// AppManager 的 ADB 组合实现：包装 adb 设备 shell + 输出解析。
// 对应 crates/mwine-app/src/manager.rs。Rust 侧经 mwine-adb AdbClient（v2 协议）访问；
// C++ 侧 adb 协议栈属 mw_adb 模块（另组负责），本模块以 QProcess 调 `adb` CLI 等价实现，
// shell 访问仍经 DeviceShell 抽象（生产 AdbShell；测试注入脚本回放），决策逻辑可离线单测。
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "apk.hpp"
#include "badging.hpp"
#include "dumpsys.hpp"
#include "manifest.hpp"
#include "result.hpp"
#include "types.hpp"

namespace mw::app {

// shell 执行结果（stdout/stderr 合流 + 退出码是否为 0）。
struct ShellReply {
    QString out;
    bool ok = false;
};

// 安装进度（对应 mwine-adb InstallProgress）。
struct InstallProgress {
    enum class Phase { Push, Installing };
    Phase phase = Phase::Push;
    quint64 sent = 0;  // Push 阶段已传字节（CLI 路径只在开始时报告总量，无字节级进度）
    quint64 total = 0; // Push 阶段总量
};

// 设备 shell 抽象：生产为 ADB shell；测试为脚本回放。
class DeviceShell {
public:
    virtual ~DeviceShell() = default;
    virtual mw::Result<ShellReply> run(const QString& cmd, int timeoutMs) = 0;
};

// 生产实现：QProcess 调 `adb -s <serial> shell`（stderr 追加合流，ok = 退出码 0）。
// 附带文件拉取 / 安装（adb exec-out / adb install），供 pull 元数据与 install 用。
// 方法为虚：ProtocolAdbShell（protocol.hpp）用 mw_adb 协议栈覆盖同一面。
class AdbShell : public DeviceShell {
public:
    explicit AdbShell(QString serial, QString adbProgram = QStringLiteral("adb"));

    mw::Result<ShellReply> run(const QString& cmd, int timeoutMs) override;

    const QString& serial() const { return serial_; }

    // 拉取设备文件（`adb exec-out cat`，二进制安全）。
    virtual mw::Result<QByteArray> pullFile(const QString& remotePath,
                                             int timeoutMs = 60000);

    // 安装 APK（`adb install -r -t`）。onProgress 收到 Push(总量) 与 Installing 两档。
    virtual mw::Result<bool> installApk(
        const QString& apkPath, int timeoutMs = 600000,
        std::function<void(const InstallProgress&)> onProgress = {});

    // 补授运行时权限：dumpsys 找 granted=false 的危险权限逐个 pm grant，返回成功数。
    virtual mw::Result<int> grantRuntimePermissions(const QString& package);

private:
    QString serial_;
    QString adbProgram_;
};

// ADB 组合实现。readLocalApk / iconFromZip 为纯本地解析（可离线测试）。
class AdbAppManager : public AppManager {
public:
    // 绑定一台设备（生产入口）。
    explicit AdbAppManager(QString serial, QString adbProgram = QStringLiteral("adb"));
    // 测试入口：注入脚本化 shell（不持有 adb 通道，pull/install 不可用）。
    AdbAppManager(std::unique_ptr<DeviceShell> shell, QString serial);
    ~AdbAppManager() override;

    const QString& serial() const { return serial_; }

    // 解析本地 APK 元数据：纯 C++ 路径（zip + AXML）为主，
    // aapt2 存在时叠加 badging（标签引用/自适应图标更准）。
    struct ReadLocalApkResult {
        ManifestInfo manifest;
        std::optional<ApkBadging> badging;
    };
    static mw::Result<ReadLocalApkResult> readLocalApk(const QString& apkPath);

    // 从 APK zip 内挑启动图标：优先 badging 给出的 entry（xml 则回退同名位图），
    // 否则密度目录启发式扫描（无 arsc 时的尽力而为）。
    static std::optional<AppIcon> iconFromZip(const ZipArchive& zip,
                                              const std::optional<ApkBadging>& badging);

    // 无 arsc 时的图标启发式：res/ 下名字含 launcher/icon 的最高密度位图。
    static std::optional<QString> heuristicIcon(const QStringList& entries);

    // ---- AppManager 接口 ----
    mw::Result<AppInfo> install(const QString& apkPath) override;
    mw::Result<bool> uninstall(const QString& package) override;
    mw::Result<bool> launch(const QString& package, std::optional<quint32> displayId) override;
    mw::Result<QVector<AppInfo>> list(const ListOptions& options) override;
    mw::Result<AppInfo> get(const QString& package) override;
    mw::Result<bool> stop(const QString& package) override;
    mw::Result<BoolValue> isInstalled(const QString& package) override;

    // install 的进度版：进度回调 + 安装后补授运行时权限。
    mw::Result<AppInfo> installWithProgress(
        const QString& apkPath, std::function<void(const InstallProgress&)> onProgress = {});

private:
    mw::Result<ShellReply> shell(const QString& cmd, int timeoutMs);
    // mw::Result<QString> 与错误类型歧义：输出经 out 参数带出。
    mw::Result<bool> shellOk(const QString& cmd, int timeoutMs, QString* out);
    mw::Result<std::optional<QString>> launcherOf(const QString& package);
    mw::Result<std::optional<QString>> baseApkPath(const QString& package);
    void pullMetadata(AppInfo& info);
    mw::Result<AppInfo> getFast(const QString& package);

    QString serial_;
    std::unique_ptr<DeviceShell> shell_;    // 决策通道（可为脚本注入）
    std::unique_ptr<AdbShell> adbExt_;      // adb 附加能力（pull/install）；脚本模式为空
};

} // namespace mw::app
