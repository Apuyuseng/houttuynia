// manager.rs 的 C++ 移植（默认 ADB 通道为 mw_adb 协议栈，见 protocol.hpp）。
#include "manager.hpp"

#include <QFileInfo>
#include <QSet>

#include "log.hpp"
#include "process.hpp"
#include "protocol.hpp"

namespace mw::app {
namespace {

// 与 Rust 版一致的各命令超时（秒 → 毫秒）
constexpr int kResolveTimeoutMs = 10 * 1000;
constexpr int kPmPathTimeoutMs = 15 * 1000;
constexpr int kPsTimeoutMs = 15 * 1000;
constexpr int kDumpsysOneTimeoutMs = 30 * 1000;
constexpr int kDumpsysAllTimeoutMs = 90 * 1000;
constexpr int kPmListTimeoutMs = 30 * 1000;
constexpr int kUninstallTimeoutMs = 60 * 1000;
constexpr int kAmStartTimeoutMs = 20 * 1000;
constexpr int kMonkeyTimeoutMs = 30 * 1000;
constexpr int kForceStopTimeoutMs = 15 * 1000;
constexpr int kInstallTimeoutMs = 600 * 1000;
constexpr int kGrantDumpsysTimeoutMs = 20 * 1000;
constexpr int kGrantTimeoutMs = 10 * 1000;

} // namespace

// ---- AdbShell ----

AdbShell::AdbShell(QString serial, QString adbProgram)
    : serial_(std::move(serial)), adbProgram_(std::move(adbProgram))
{
}

mw::Result<ShellReply> AdbShell::run(const QString& cmd, int timeoutMs)
{
    const auto out = mw::runProcess(adbProgram_,
                                    {QStringLiteral("-s"), serial_, QStringLiteral("shell"), cmd},
                                    timeoutMs);
    if (!out.error.isEmpty())
        return mw::err<ShellReply>(errAdb(out.error));
    QString text = out.stdOutStr();
    if (!out.stdErr.isEmpty()) {
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
            text += QLatin1Char('\n');
        text += QString::fromUtf8(out.stdErr);
    }
    return mw::Result<ShellReply>::Ok(ShellReply{text, out.ok()});
}

mw::Result<QByteArray> AdbShell::pullFile(const QString& remotePath, int timeoutMs)
{
    const auto out = mw::runProcess(
        adbProgram_,
        {QStringLiteral("-s"), serial_, QStringLiteral("exec-out"), QStringLiteral("cat"),
         remotePath},
        timeoutMs);
    if (!out.error.isEmpty())
        return mw::err<QByteArray>(errAdb(out.error));
    if (out.exitCode != 0)
        return mw::err<QByteArray>(errAdb(
            QStringLiteral("pull %1 失败: %2").arg(remotePath, out.stdErrTail())));
    return mw::Result<QByteArray>::Ok(out.stdOut);
}

mw::Result<bool> AdbShell::installApk(const QString& apkPath, int timeoutMs,
                                      std::function<void(const InstallProgress&)> onProgress)
{
    if (onProgress) {
        // CLI 通道拿不到字节级进度：Push 报总量，随即转 Installing（设备侧 pm install）
        onProgress(InstallProgress{InstallProgress::Phase::Push, 0,
                                   quint64(QFileInfo(apkPath).size())});
        onProgress(InstallProgress{InstallProgress::Phase::Installing, 0, 0});
    }
    const auto out = mw::runProcess(
        adbProgram_,
        {QStringLiteral("-s"), serial_, QStringLiteral("install"), QStringLiteral("-r"),
         QStringLiteral("-t"), apkPath},
        timeoutMs);
    if (!out.error.isEmpty())
        return mw::err<bool>(errAdb(out.error));
    const QString text = out.stdOutStr() + out.stdErrStr();
    if (out.exitCode != 0 || !text.contains(QLatin1String("Success")))
        return mw::Result<bool>::Err(errAdb(
            QStringLiteral("install %1 失败: %2").arg(apkPath, text.trimmed())));
    return mw::Result<bool>::Ok();
}

mw::Result<int> AdbShell::grantRuntimePermissions(const QString& package)
{
    // 对应 mwine-adb grant_runtime_permissions：runtime permissions 段落形如
    // `    android.permission.CAMERA: granted=false, flags=...`
    auto dump = run(QStringLiteral("dumpsys package %1").arg(package), kGrantDumpsysTimeoutMs);
    if (!dump)
        return mw::err<int>(dump.error());
    int granted = 0;
    const QStringList lines = dump.value().out.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString line : lines) {
        while (line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t')))
            line.remove(0, 1);
        if (!line.startsWith(QLatin1String("android.permission.")))
            continue;
        const QString rest = line.mid(QString("android.permission.").size());
        const int colon = rest.indexOf(QLatin1String(": "));
        if (colon < 0)
            continue;
        const QString perm = rest.left(colon);
        const QString state = rest.mid(colon + 2);
        if (!state.startsWith(QLatin1String("granted=false")))
            continue;
        auto out = run(QStringLiteral("pm grant %1 android.permission.%2").arg(package, perm),
                       kGrantTimeoutMs);
        if (out && out.value().ok)
            ++granted;
    }
    return mw::Result<int>::Ok(granted);
}

// ---- AdbAppManager ----

AdbAppManager::AdbAppManager(QString serial, QString adbProgram)
    : serial_(std::move(serial))
{
    // 默认走 mw_adb 协议栈（无外部 adb 进程依赖，安装有字节级进度）
    auto adb = std::make_unique<ProtocolAdbShell>(serial_, std::move(adbProgram));
    shell_ = std::make_unique<ProtocolAdbShell>(serial_); // 决策通道（独立副本）
    adbExt_ = std::move(adb);
}

AdbAppManager::AdbAppManager(std::unique_ptr<DeviceShell> shell, QString serial)
    : serial_(std::move(serial)), shell_(std::move(shell))
{
}

AdbAppManager::~AdbAppManager() = default;

mw::Result<ShellReply> AdbAppManager::shell(const QString& cmd, int timeoutMs)
{
    return shell_->run(cmd, timeoutMs);
}

mw::Result<bool> AdbAppManager::shellOk(const QString& cmd, int timeoutMs, QString* out)
{
    auto reply = shell(cmd, timeoutMs);
    if (!reply)
        return mw::err<bool>(reply.error());
    if (!reply.value().ok)
        return mw::Result<bool>::Err(errShell(cmd, reply.value().out));
    if (out)
        *out = reply.value().out;
    return mw::Result<bool>::Ok();
}

mw::Result<AdbAppManager::ReadLocalApkResult> AdbAppManager::readLocalApk(const QString& apkPath)
{
    auto zip = ZipArchive::open(apkPath);
    if (!zip)
        return mw::err<ReadLocalApkResult>(zip.error());
    auto axml = zip.value().readEntry(QStringLiteral("AndroidManifest.xml"));
    if (!axml)
        return mw::err<ReadLocalApkResult>(axml.error());
    if (!axml.value())
        return mw::err<ReadLocalApkResult>(
            errApkParse(apkPath, QStringLiteral("缺少 AndroidManifest.xml")));
    auto info = parseManifest(*axml.value(), apkPath);
    if (!info)
        return mw::err<ReadLocalApkResult>(info.error());
    ReadLocalApkResult r;
    r.manifest = std::move(info.value());
    if (auto tool = findAapt2())
        r.badging = aapt2Badging(*tool, apkPath);
    return mw::Result<ReadLocalApkResult>::Ok(std::move(r));
}

std::optional<AppIcon> AdbAppManager::iconFromZip(const ZipArchive& zip,
                                                  const std::optional<ApkBadging>& badging)
{
    std::optional<QString> entry;
    if (badging && badging->icon) {
        QString icon = *badging->icon;
        if (icon.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive)) {
            const QStringList entries = zip.entryNames();
            icon = pickRasterFallback(entries, icon).value_or(icon);
        }
        entry = icon;
    } else {
        entry = heuristicIcon(zip.entryNames());
    }
    if (!entry)
        return std::nullopt;
    auto bytes = zip.readEntry(*entry);
    if (!bytes || !bytes.value())
        return std::nullopt;
    const auto mime = detectIconMime(*entry, *bytes.value());
    if (!mime)
        return std::nullopt;
    return AppIcon{*bytes.value(), *mime};
}

std::optional<QString> AdbAppManager::heuristicIcon(const QStringList& entries)
{
    const auto densityOf = [](const QString& path) -> quint32 {
        static const std::pair<const char*, quint32> names[] = {
            {"xxxhdpi", 640}, {"xxhdpi", 480}, {"xhdpi", 320}, {"hdpi", 240}, {"mdpi", 160},
        };
        const QStringList segs = path.split(QLatin1Char('/'));
        for (const QString& seg : segs) {
            for (const auto& [name, dpi] : names) {
                if (seg.contains(QLatin1String(name)))
                    return dpi;
            }
        }
        return 0;
    };
    std::optional<QString> best;
    quint32 bestDpi = 0;
    for (const QString& e : entries) {
        const QString lower = e.toLower();
        if (!e.startsWith(QLatin1String("res/"))
            || !(lower.contains(QLatin1String("launcher")) || lower.contains(QLatin1String("/icon")))
            || !(lower.endsWith(QLatin1String(".png")) || lower.endsWith(QLatin1String(".webp"))))
            continue;
        const quint32 d = densityOf(e);
        if (!best || d > bestDpi) {
            best = e;
            bestDpi = d;
        }
    }
    return best;
}

mw::Result<std::optional<QString>> AdbAppManager::launcherOf(const QString& package)
{
    auto out = shell(QStringLiteral("cmd package resolve-activity --brief %1 | tail -1").arg(package),
                     kResolveTimeoutMs);
    if (!out)
        return mw::err<std::optional<QString>>(out.error());
    return mw::Result<std::optional<QString>>::Ok(parseResolveActivity(out.value().out));
}

mw::Result<std::optional<QString>> AdbAppManager::baseApkPath(const QString& package)
{
    auto out = shell(QStringLiteral("pm path %1").arg(package), kPmPathTimeoutMs);
    if (!out)
        return mw::err<std::optional<QString>>(out.error());
    const QStringList lines = out.value().out.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& l : lines) {
        const QString t = l.trimmed();
        if (t.startsWith(QLatin1String("package:")))
            return mw::Result<std::optional<QString>>::Ok(t.mid(QString("package:").size()));
    }
    return mw::Result<std::optional<QString>>::Ok(std::nullopt);
}

void AdbAppManager::pullMetadata(AppInfo& info)
{
    // 脚本注入模式无 adb 通道：静默跳过（对应 Rust pull 失败即 return）
    if (!adbExt_)
        return;
    auto remote = baseApkPath(info.package);
    if (!remote || !remote.value())
        return;
    auto bytes = adbExt_->pullFile(*remote.value());
    if (!bytes)
        return;
    auto zip = ZipArchive::fromBytes(bytes.value(), info.package);
    if (!zip)
        return;
    auto axml = zip.value().readEntry(QStringLiteral("AndroidManifest.xml"));
    if (!axml || !axml.value())
        return;
    auto manifest = parseManifest(*axml.value(), info.package);
    if (manifest) {
        if (!info.name)
            info.name = manifest.value().label;
        if (!info.version)
            info.version = manifest.value().versionName;
        if (!info.versionCode)
            info.versionCode = manifest.value().versionCode;
    }
    if (!info.icon)
        info.icon = iconFromZip(zip.value(), std::nullopt);
}

mw::Result<AppInfo> AdbAppManager::getFast(const QString& package)
{
    AppInfo info;
    info.package = package;
    QString dumpsysOut;
    if (auto out = shellOk(QStringLiteral("dumpsys package %1").arg(package),
                           kDumpsysOneTimeoutMs, &dumpsysOut);
        !out)
        return mw::err<AppInfo>(out.error());
    const PackageDump dump = parseDumpsysPackage(dumpsysOut);
    info.version = dump.versionName;
    info.versionCode = dump.versionCode;
    info.installedAt = dump.firstInstallTime;
    info.updatedAt = dump.lastUpdateTime;
    {
        auto ps = shell(QStringLiteral("ps -A -o NAME"), kPsTimeoutMs);
        if (!ps)
            return mw::err<AppInfo>(ps.error());
        info.isRunning = packageRunning(parseProcessNames(ps.value().out), package);
    }
    {
        auto p = baseApkPath(package);
        if (!p)
            return mw::err<AppInfo>(p.error());
        info.apkPath = p.value();
    }
    {
        auto l = launcherOf(package);
        if (!l)
            return mw::err<AppInfo>(l.error());
        info.launcherActivity = l.value();
    }
    if (!info.launcherActivity) {
        // 已安装但无 launcher 入口（纯服务应用）不报错，交由调用方判断
        auto installed = isInstalled(package);
        if (!installed)
            return mw::err<AppInfo>(installed.error());
        if (!installed.value().value)
            return mw::err<AppInfo>(errNotInstalled(package));
    }
    return mw::Result<AppInfo>::Ok(std::move(info));
}

mw::Result<AppInfo> AdbAppManager::install(const QString& apkPath)
{
    return installWithProgress(apkPath, {});
}

mw::Result<AppInfo> AdbAppManager::installWithProgress(
    const QString& apkPath, std::function<void(const InstallProgress&)> onProgress)
{
    auto local = readLocalApk(apkPath);
    if (!local)
        return mw::err<AppInfo>(local.error());
    const QString package = local.value().manifest.package;

    if (!adbExt_)
        return mw::err<AppInfo>(errAdb(QStringLiteral("脚本 shell 不支持安装")));
    auto installed = adbExt_->installApk(apkPath, kInstallTimeoutMs, onProgress);
    if (!installed)
        return mw::err<AppInfo>(installed.error());
    // 扫码/存储等运行时权限默认拒绝，模拟器场景统一补授
    if (auto granted = adbExt_->grantRuntimePermissions(package); granted)
        MW_LOG("app", QStringLiteral("已补授 %1 项运行时权限（%2）").arg(granted.value()).arg(package));

    auto info = getFast(package);
    if (!info)
        return info;
    if (!info.value().name) {
        info.value().name = local.value().manifest.label;
        if (!info.value().name && local.value().badging)
            info.value().name = local.value().badging->label;
    }
    if (!info.value().icon) {
        if (auto zip = ZipArchive::open(apkPath))
            info.value().icon = iconFromZip(zip.value(), local.value().badging);
    }
    return info;
}

mw::Result<bool> AdbAppManager::uninstall(const QString& package)
{
    auto installed = isInstalled(package);
    if (!installed)
        return mw::err<bool>(installed.error());
    if (!installed.value().value)
        return mw::Result<bool>::Err(errNotInstalled(package));
    const QString cmd = QStringLiteral("pm uninstall %1").arg(package);
    auto out = shell(cmd, kUninstallTimeoutMs);
    if (!out)
        return mw::err<bool>(out.error());
    if (!out.value().out.contains(QLatin1String("Success")))
        return mw::Result<bool>::Err(errShell(cmd, out.value().out));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> AdbAppManager::launch(const QString& package, std::optional<quint32> displayId)
{
    auto installed = isInstalled(package);
    if (!installed)
        return mw::err<bool>(installed.error());
    if (!installed.value().value)
        return mw::Result<bool>::Err(errNotInstalled(package));
    auto comp = launcherOf(package);
    if (!comp)
        return mw::err<bool>(comp.error());
    if (comp.value()) {
        const QString display = displayId
            ? QStringLiteral("--display %1 ").arg(*displayId)
            : QString();
        const QString cmd =
            QStringLiteral("am start %1-n '%2'").arg(display, *comp.value());
        auto out = shell(cmd, kAmStartTimeoutMs);
        if (!out)
            return mw::err<bool>(out.error());
        // am 对已存在的 Error 仍可能退出 0：按输出兜底判定
        if (out.value().ok && !out.value().out.contains(QLatin1String("Error")))
            return mw::Result<bool>::Ok();
    }
    // 回退：monkey（不依赖入口解析；--display 不适用则丢弃定向）
    const QString cmd = QStringLiteral(
        "monkey -p %1 -c android.intent.category.LAUNCHER 1").arg(package);
    auto out = shell(cmd, kMonkeyTimeoutMs);
    if (!out)
        return mw::err<bool>(out.error());
    if (out.value().out.contains(QLatin1String("aborted")) || !out.value().ok)
        return mw::Result<bool>::Err(errNoLauncher(package));
    return mw::Result<bool>::Ok();
}

mw::Result<QVector<AppInfo>> AdbAppManager::list(const ListOptions& options)
{
    QString pmListOut;
    if (auto out = shellOk(QStringLiteral("pm list packages -3"), kPmListTimeoutMs, &pmListOut);
        !out)
        return mw::err<QVector<AppInfo>>(out.error());
    QStringList packages;
    const QStringList lines = pmListOut.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& l : lines) {
        const QString t = l.trimmed();
        if (t.startsWith(QLatin1String("package:")))
            packages << t.mid(QString("package:").size());
    }

    QMap<QString, PackageDump> dumps;
    QSet<QString> procs;
    if (options.enrich || options.pullMetadata) {
        if (auto all = shell(QStringLiteral("dumpsys package"), kDumpsysAllTimeoutMs))
            dumps = parseDumpsysAll(all.value().out);
    }
    if (options.enrich) {
        if (auto ps = shell(QStringLiteral("ps -A -o NAME"), kPsTimeoutMs))
            procs = parseProcessNames(ps.value().out);
    }

    QVector<AppInfo> result;
    result.reserve(packages.size());
    for (const QString& pkg : packages) {
        const PackageDump* dump = dumps.contains(pkg) ? &dumps[pkg] : nullptr;
        AppInfo info;
        info.package = pkg;
        if (dump) {
            info.version = dump->versionName;
            info.versionCode = dump->versionCode;
            info.installedAt = dump->firstInstallTime;
            info.updatedAt = dump->lastUpdateTime;
        }
        info.isRunning = packageRunning(procs, pkg);
        if (options.pullMetadata)
            pullMetadata(info);
        result.push_back(std::move(info));
    }
    return mw::Result<QVector<AppInfo>>::Ok(std::move(result));
}

mw::Result<AppInfo> AdbAppManager::get(const QString& package)
{
    auto info = getFast(package);
    if (!info)
        return info;
    // 标签/图标不常变，get() 全量补齐（每包一次 pull，量级可控）
    if (!info.value().name || !info.value().icon)
        pullMetadata(info.value());
    return info;
}

mw::Result<bool> AdbAppManager::stop(const QString& package)
{
    const QString cmd = QStringLiteral("am force-stop %1").arg(package);
    auto out = shell(cmd, kForceStopTimeoutMs);
    if (!out)
        return mw::err<bool>(out.error());
    if (!out.value().ok)
        return mw::Result<bool>::Err(errShell(cmd, out.value().out));
    return mw::Result<bool>::Ok();
}

mw::Result<BoolValue> AdbAppManager::isInstalled(const QString& package)
{
    auto out = shell(QStringLiteral("pm list packages %1").arg(package), kPmListTimeoutMs);
    if (!out)
        return mw::err<BoolValue>(out.error());
    const QStringList lines = out.value().out.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const QString want = QStringLiteral("package:") + package;
    for (const QString& l : lines) {
        if (l.trimmed() == want)
            return mw::Result<BoolValue>::Ok(BoolValue{true});
    }
    return mw::Result<BoolValue>::Ok(BoolValue{false});
}

// ---- 任务约定的便捷 API：parseApk / ApkInfo::displayName ----

QString ApkInfo::displayName() const
{
    if (!label.trimmed().isEmpty())
        return label;
    const int idx = package.lastIndexOf(QLatin1Char('.'));
    return idx < 0 ? package : package.mid(idx + 1);
}

mw::Result<ApkInfo> parseApk(const QString& apkPath)
{
    auto local = AdbAppManager::readLocalApk(apkPath);
    if (!local)
        return mw::err<ApkInfo>(local.error());
    const auto& [manifest, badging] = local.value();

    ApkInfo info;
    info.package = manifest.package;
    info.label = manifest.label.value_or(QString());
    info.versionName = manifest.versionName.value_or(QString());
    info.versionCode = manifest.versionCode.value_or(0);
    if (info.label.isEmpty() && badging && badging->label)
        info.label = *badging->label;

    auto zip = ZipArchive::open(apkPath);
    if (!zip)
        return mw::Result<ApkInfo>::Ok(std::move(info)); // 壳信息仍返回（图标降级）
    if (auto icon = AdbAppManager::iconFromZip(zip.value(), badging)) {
        info.iconPng = icon->data;
        info.iconMime = QLatin1String(iconMimeStr(icon->mime));
    }
    return mw::Result<ApkInfo>::Ok(std::move(info));
}

} // namespace mw::app
