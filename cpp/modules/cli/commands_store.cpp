// store 子命令（在线应用商店：豌豆荚 / F-Droid）。
// ← crates/mwine-cli/src/main.rs：cmd_store_search / cmd_store_versions / cmd_store_install
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <atomic>
#include <cstdio>

#include "adb.hpp"
#include "app.hpp"
#include "args.hpp"
#include "cli_common.hpp"
#include "commands.hpp"
#include "fs.hpp"
#include "process.hpp"
#include "store.hpp"
#include "supervisor.hpp"

namespace mwine {

static QString pad(const QString& s, int width)
{
    return s.leftJustified(width);
}

static QString truncateChars(const QString& s, int n)
{
    return s.size() <= n ? s : s.left(qMax(0, n - 1)) + QStringLiteral("…");
}

static mw::store::SourceOpts storeOpts(const std::optional<QString>& mirror,
                                       const std::optional<QString>& proxy)
{
    mw::store::SourceOpts o;
    o.mirror = mirror.value_or(QString());
    o.proxy = proxy.value_or(QString());
    return o;
}

static mw::Result<bool> storeSearch(Args& args)
{
    const auto keyword = args.take();
    if (!keyword)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine store search <keyword> [--source all|wandoujia|fdroid] "
                           "[--mirror <m>] [--proxy <url>]"));
    QString source = QStringLiteral("all");
    if (const auto s = args.opt("source"))
        source = *s;
    const std::optional<QString> mirror = args.opt("mirror");
    const std::optional<QString> proxy = args.opt("proxy");

    mw::store::StoreHub hub;
    const auto r = hub.search(source == QStringLiteral("all") ? QString() : source, *keyword,
                              storeOpts(mirror, proxy));
    const auto& apps = r.first;
    const auto& errors = r.second;
    for (auto it = errors.cbegin(); it != errors.cend(); ++it)
        std::fprintf(stderr, "⚠ 来源 %s 不可用：%s\n", qPrintable(it.key()), qPrintable(it.value()));
    if (apps.isEmpty()) {
        std::printf("没有找到「%s」\n", qPrintable(*keyword));
        return mw::Result<bool>::Ok();
    }
    std::printf("%s %s %s %s %s 来源\n", qPrintable(pad(QStringLiteral("APPID"), 10)),
                qPrintable(pad(QStringLiteral("包名"), 36)),
                qPrintable(pad(QStringLiteral("名称"), 20)),
                qPrintable(pad(QStringLiteral("最新版"), 12)),
                qPrintable(pad(QStringLiteral("安装量"), 10)));
    for (const auto& a : apps) {
        std::printf("%s %s %s %s %s %s\n",
                    qPrintable(pad(a.appId.left(10), 10)),
                    qPrintable(pad(a.package.left(36), 36)),
                    qPrintable(pad(truncateChars(a.name, 18), 20)),
                    qPrintable(pad(a.latest ? a.latest->name.left(12) : QString(), 12)),
                    qPrintable(pad(a.installCount.value_or(QString()).left(10), 10)),
                    qPrintable(a.source));
    }
    if (errors.isEmpty()) {
        std::printf("\n安装：mwine store install <包名> --source %s [--app-id <id>] [--code <版本号>]\n",
                    qPrintable(apps.first().source));
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> storeVersions(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine store versions <package> [--source <s>] [--app-id <id>] …"));
    QString source = QStringLiteral("fdroid");
    if (const auto s = args.opt("source"))
        source = *s;
    const std::optional<QString> appId = args.opt("app-id");
    const std::optional<QString> mirror = args.opt("mirror");
    const std::optional<QString> proxy = args.opt("proxy");
    mw::store::StoreHub hub;
    const auto versions = hub.versions(source, appId.value_or(QString()), *package,
                                       storeOpts(mirror, proxy));
    if (!versions)
        return mw::err<bool>(versions.error());
    std::printf("%s %s %s %s 架构\n", qPrintable(pad(QStringLiteral("版本号"), 12)),
                qPrintable(pad(QStringLiteral("版本名"), 14)),
                qPrintable(pad(QStringLiteral("大小"), 10)),
                qPrintable(pad(QStringLiteral("minSdk"), 8)));
    for (const auto& v : versions.value()) {
        const QString size = v.size > 0
                                 ? QStringLiteral("%1MB").arg(v.size / 1048576.0, 0, 'f', 1)
                                 : QStringLiteral("—");
        std::printf("%s %s %s %s %s%s\n",
                    qPrintable(pad(QString::number(v.code), 12)),
                    qPrintable(pad(v.name.left(14), 14)), qPrintable(pad(size, 10)),
                    qPrintable(pad(v.minSdk ? QString::number(*v.minSdk) : QString(), 8)),
                    qPrintable(v.nativeCodes.join(QLatin1Char(','))),
                    qPrintable(v.isLatest ? QStringLiteral("  ← 最新") : QString()));
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> storeInstall(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine store install <package> [--source <s>] [--app-id <id>] "
                           "[--code <n>] [--keep-apk] …"));
    QString source = QStringLiteral("fdroid");
    if (const auto s = args.opt("source"))
        source = *s;
    const std::optional<QString> appId = args.opt("app-id");
    std::optional<quint64> code;
    if (const auto c = args.opt("code")) {
        bool ok = false;
        const quint64 n = c->toULongLong(&ok);
        if (!ok)
            return mw::err<bool>(QStringLiteral("无效的 --code 值: %1").arg(*c));
        code = n;
    }
    const bool keepApk = args.has("keep-apk");
    const std::optional<QString> mirror = args.opt("mirror");
    const std::optional<QString> proxy = args.opt("proxy");

    mw::store::StoreHub hub;
    const auto opts = storeOpts(mirror, proxy);
    if (source == QStringLiteral("wandoujia") && !appId)
        return mw::err<bool>(QStringLiteral("豌豆荚安装需要 --app-id（mwine store search 查看APPID列）"));
    const auto resolved = hub.resolve(source, appId.value_or(QString()), *package, code, QString(),
                                      opts);
    if (!resolved)
        return mw::err<bool>(resolved.error());

    const QString dir = QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/downloads/apk"));
    if (!QDir().mkpath(dir))
        return mw::err<bool>(QStringLiteral("无法创建目录 %1").arg(dir));
    QString safePkg;
    for (const QChar c : *package) {
        const bool okChar = (c.isLetterOrNumber() && c.toLatin1() != 0) || c == QLatin1Char('.')
                            || c == QLatin1Char('_');
        safePkg += okChar ? c : QLatin1Char('_');
    }
    const QString dest =
        QDir(dir).filePath(QStringLiteral("%1_%2.apk").arg(safePkg).arg(resolved.value().versionCode));
    const QByteArray ua = source == QStringLiteral("fdroid") ? QByteArray(mw::store::UA_NEUTRAL)
                                                             : QByteArray(mw::store::UA_BROWSER);
    static std::atomic_bool never{ false };
    const auto fetched = mw::store::fetchApk(
        resolved.value(), dest, *package, *package, ua, opts, never,
        [](const QString& phase, quint64 done, quint64 total) {
            if (total > 0)
                std::printf("  [%s] %.1f / %.1f MB\n", qPrintable(phase), done / 1048576.0,
                            total / 1048576.0);
        },
        [](const QString& l) { std::printf("%s\n", qPrintable(l)); });
    if (!fetched)
        return mw::err<bool>(fetched.error());

    // 安装 + 生成桌面壳（与 GUI 拖放安装同一条链路，含自动开机）
    auto client = adbClient();
    client.ensureServer();
    const auto serialR = ensureInstallTarget(&client, [](const QString&) {});
    if (!serialR)
        return mw::err<bool>(serialR.error());
    mw::app::CreateAppShellOptions opts2;
    opts2.apkPath = dest;
    opts2.serial = serialR.value();
    const auto shell = mw::app::createAppShell(opts2);
    if (!shell)
        return mw::err<bool>(shell.error());
    std::printf("✓ 已安装 %s\n", qPrintable(shell.value().package));
    std::printf("✓ 已生成 %s\n", qPrintable(shell.value().appDir));
    if (!keepApk) {
        QFile::remove(dest);
        std::printf("✓ 已删除安装包（--keep-apk 可保留）\n");
    } else {
        std::printf("安装包保留在 %s\n", qPrintable(dest));
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdStore(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(QStringLiteral("Usage: mwine store <search|versions|install> […]"));
    if (*action == QStringLiteral("search"))
        return storeSearch(args);
    if (*action == QStringLiteral("versions"))
        return storeVersions(args);
    if (*action == QStringLiteral("install"))
        return storeInstall(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

} // namespace mwine
