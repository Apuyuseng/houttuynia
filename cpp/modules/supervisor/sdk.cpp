// Sdk 探测 / AVD 列表 / doctor ← crates/mwine-supervisor/src/lib.rs（Sdk 部分）
#include "supervisor.hpp"

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <algorithm>

#include "fs.hpp"
#include "process.hpp"

namespace mw::supervisor {

static QString homeDir()
{
    // 对齐 Rust home_dir()：HOME 缺省时回落 /Users/Shared
    const QString h = mw::homeDir();
    return h.isEmpty() ? QStringLiteral("/Users/Shared") : h;
}

QString Sdk::externalRuntimeDir()
{
    return QDir(homeDir()).filePath(QStringLiteral("Library/Application Support/MWine/runtime"));
}

Sdk Sdk::fromRoot(const QString& root)
{
    Sdk s;
    s.root = root;
    const QDir r(root);
    s.emulatorBin = r.filePath(QStringLiteral("emulator/emulator"));
    s.adbBin = r.filePath(QStringLiteral("platform-tools/adb"));
    s.platformTools = r.filePath(QStringLiteral("platform-tools"));
    s.systemImages = r.filePath(QStringLiteral("system-images"));
    return s;
}

mw::Result<Sdk> Sdk::detect()
{
    // 线程安全 + 结果缓存：守护进程的 HTTP 池线程会并发调用（image_list 等
    // 路由 6s 轮询）。QCoreApplication::applicationDirPath 的惰性初始化并发
    // 首调不是线程安全的——曾导致守护 SIGSEGV 崩溃循环（终端会话随之反复
    // 重置）。SDK 位置在进程生命周期内不变，互斥算一次即可。
    static QMutex mu;
    static std::optional<mw::Result<Sdk>> cached;
    {
        QMutexLocker lock(&mu);
        if (cached)
            return *cached;
    }
    const mw::Result<Sdk> r = detectUncached();
    QMutexLocker lock(&mu);
    cached = r;
    return r;
}

mw::Result<Sdk> Sdk::detectUncached()
{
    // .app 内嵌运行时：<MWine.app>/Contents/Resources/runtime/{emulator,platform-tools,…}
    // exe = <MWine.app>/Contents/MacOS/<bin>；上溯两级 = Contents
    std::optional<QString> bundled;
    if (QCoreApplication::instance()) {
        const QString exeDir = QCoreApplication::applicationDirPath();
        if (!exeDir.isEmpty()) {
            QDir d(exeDir);
            if (d.cdUp() && d.cdUp()) {
                const QString cand = QDir(d).filePath(QStringLiteral("Resources/runtime"));
                if (QDir(cand).exists(QStringLiteral("emulator")))
                    bundled = cand;
            }
        }
    }

    QVector<QString> candidates;
    if (auto v = qEnvironmentVariable("MWINE_SDK_ROOT"); !v.isEmpty())
        candidates << v;
    candidates << externalRuntimeDir();
    if (bundled)
        candidates << *bundled;
    for (const char* name : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        if (auto v = qEnvironmentVariable(name); !v.isEmpty())
            candidates << v;
    }
    candidates << QDir(homeDir()).filePath(QStringLiteral("Library/Android/sdk"));

    for (const auto& c : candidates) {
        if (QDir(c).exists(QStringLiteral("emulator")))
            return mw::Result<Sdk>::Ok(Sdk::fromRoot(c));
    }
    return mw::err<Sdk>(QStringLiteral(
        "Android SDK not found (tried ANDROID_HOME, ANDROID_SDK_ROOT, ~/Library/Android/sdk)"));
}

QString Sdk::avdHome() const
{
    if (auto p = qEnvironmentVariable("ANDROID_AVD_HOME"); !p.isEmpty())
        return p;
    if (isBundled()) {
        // 零依赖模式：AVD 放 Application Support（首启从 bundle 模板种子）
        const QString d = QDir(homeDir()).filePath(
            QStringLiteral("Library/Application Support/MWine/avd"));
        QDir().mkpath(d);
        return d;
    }
    return QDir(homeDir()).filePath(QStringLiteral(".android/avd"));
}

bool Sdk::isBundled() const
{
    if (root == externalRuntimeDir())
        return true;
    // .app/Contents 出现在任一祖先路径里（对齐 Rust ancestors().any(contains)）
    return root.contains(QStringLiteral(".app/Contents"));
}

void Sdk::seedAvdIfNeeded(const QString& name) const
{
    if (!isBundled())
        return;
    const QString avdHomeDir = avdHome();
    const QString ini = QDir(avdHomeDir).filePath(name + QStringLiteral(".ini"));
    const QString dir = QDir(avdHomeDir).filePath(name + QStringLiteral(".avd"));
    if (QFile::exists(ini) && QFileInfo(dir).isDir())
        return;
    // 模板两处可放：基础 .app 的 Resources/avd-template（新版正式位）或运行时根下
    std::optional<QString> templateDir;
    if (QCoreApplication::instance()) {
        const QString exeDir = QCoreApplication::applicationDirPath();
        if (!exeDir.isEmpty()) {
            QDir d(exeDir);
            if (d.cdUp() && d.cdUp()) {
                const QString cand = QDir(d).filePath(QStringLiteral("Resources/avd-template"));
                if (QFileInfo(cand).isDir())
                    templateDir = cand;
            }
        }
    }
    if (!templateDir) {
        const QString fallback = QDir(root).filePath(QStringLiteral("avd-template"));
        if (QFileInfo(fallback).isDir())
            templateDir = fallback;
    }
    if (!templateDir) {
        qWarning().noquote()
            << QStringLiteral("[avd] 无 avd-template（基础包 Resources/ 或运行时根），无法种子 %1").arg(name);
        return;
    }
    QDir().mkpath(dir);
    QFile::copy(QDir(*templateDir).filePath(QStringLiteral("config.ini")),
                QDir(dir).filePath(QStringLiteral("config.ini")));
    const QString iniContent = QStringLiteral(
        "avd.ini.encoding=UTF-8\npath=%1\ntarget=android-33\n").arg(dir);
    QFile f(ini);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(iniContent.toUtf8());
    qInfo().noquote() << QStringLiteral("[avd] 已从 bundle 模板种子 %1 → %2").arg(name, dir);
}

QString Sdk::findBuildTool(const QString& name) const
{
    // build-tools 各版本目录按版本降序（字典序倒序对齐 Rust sort + rev）查找
    const QDir bt(QDir(root).filePath(QStringLiteral("build-tools")));
    const auto entries = bt.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const QString candidate = QDir(bt).filePath(*it + QLatin1Char('/') + name);
        if (QFileInfo(candidate).isFile())
            return candidate;
    }
    return QString();
}

// ===================== AvdInfo / parseProperties =====================

QString AvdInfo::abi() const
{
    return config.value(QStringLiteral("abi.type"));
}

bool AvdInfo::isArm64() const
{
    return abi().contains(QStringLiteral("arm64"));
}

QMap<QString, QString> parseProperties(const QString& text)
{
    QMap<QString, QString> map;
    const auto lines = text.split(QLatin1Char('\n'));
    for (QString raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('!')))
            continue;
        const int idx = line.indexOf(QLatin1Char('='));
        if (idx < 0)
            continue;
        map.insert(line.left(idx).trimmed(), line.mid(idx + 1).trimmed());
    }
    return map;
}

QVector<AvdInfo> Sdk::listAvds() const
{
    QVector<AvdInfo> avds;
    // 双 home：主 home + ~/.android/avd（AVD 实体可能在任意一台）
    QVector<QString> homes{avdHome()};
    const QString dotAndroid = QDir(homeDir()).filePath(QStringLiteral(".android/avd"));
    if (!homes.contains(dotAndroid))
        homes << dotAndroid;

    for (const auto& home : homes) {
        const QDir dir(home);
        const auto inis = dir.entryList({ QStringLiteral("*.ini") }, QDir::Files);
        for (const QString& ini : inis) {
            const QString name = QFileInfo(ini).completeBaseName();
            // 同名 AVD 以先扫到的 home（主 home）为准
            if (std::any_of(avds.cbegin(), avds.cend(),
                            [&name](const AvdInfo& a) { return a.name == name; }))
                continue;
            QFile f(dir.filePath(ini));
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const auto meta = parseProperties(QString::fromUtf8(f.readAll()));
            f.close();
            QString avdDir = meta.value(QStringLiteral("path"));
            if (avdDir.isEmpty())
                avdDir = dir.filePath(name + QStringLiteral(".avd"));
            QMap<QString, QString> config;
            QFile cfg(QDir(avdDir).filePath(QStringLiteral("config.ini")));
            if (cfg.open(QIODevice::ReadOnly)) {
                config = parseProperties(QString::fromUtf8(cfg.readAll()));
                cfg.close();
            }
            avds.push_back(AvdInfo{ name, avdDir, config });
        }
    }
    std::sort(avds.begin(), avds.end(),
              [](const AvdInfo& a, const AvdInfo& b) { return a.name < b.name; });
    return avds;
}

mw::Result<AvdInfo> Sdk::findAvd(const QString& name) const
{
    const auto avds = listAvds();
    for (const auto& a : avds) {
        if (a.name == name)
            return mw::Result<AvdInfo>::Ok(a);
    }
    QStringList names;
    for (const auto& a : avds)
        names << a.name;
    return mw::err<AvdInfo>(QStringLiteral("no AVD named %1 (available: %2)")
                                .arg(name, names.join(QStringLiteral(", "))));
}

// ===================== doctor =====================

QVector<DoctorItem> doctor()
{
    QVector<DoctorItem> items;

    const auto hv = mw::runProcess(QStringLiteral("sysctl"),
                                   { QStringLiteral("kern.hv_support") }, 10000);
    const QString hvOut = hv.stdOutStr().trimmed();
    items.push_back(DoctorItem{ QStringLiteral("Hypervisor 硬件虚拟化 (kern.hv_support)"),
                                hvOut.endsWith(QLatin1Char('1')),
                                hv.error.isEmpty() ? hvOut : hv.error });

    if (auto sdkR = Sdk::detect(); sdkR) {
        const auto& sdk = sdkR.value();
        const auto avds = sdk.listAvds();
        const int arm64 = int(std::count_if(avds.cbegin(), avds.cend(),
                                            [](const AvdInfo& a) { return a.isArm64(); }));
        items.push_back(DoctorItem{
            QStringLiteral("Android SDK"), true,
            QStringLiteral("%1（%2 个 AVD，其中 %3 个 arm64）").arg(sdk.root).arg(avds.size()).arg(arm64) });
    } else {
        items.push_back(DoctorItem{ QStringLiteral("Android SDK"), false,
                                    QStringLiteral("未找到（ANDROID_HOME / ~/Library/Android/sdk）") });
    }

    const auto qemu = mw::runProcess(QStringLiteral("qemu-system-aarch64"),
                                     { QStringLiteral("--version") }, 15000);
    QString qemuDetail;
    if (qemu.error.isEmpty()) {
        const auto lines = qemu.stdOutStr().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        qemuDetail = lines.isEmpty() ? QString() : lines.first();
    } else {
        qemuDetail = QStringLiteral("未安装: %1").arg(qemu.error);
    }
    items.push_back(DoctorItem{ QStringLiteral("QEMU"), qemu.error.isEmpty(), qemuDetail });

    return items;
}

} // namespace mw::supervisor
