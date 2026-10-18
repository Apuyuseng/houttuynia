// mwine-cli 桌面壳核心逻辑（cmd_app_create / cmd_app_installer / bundled_main 配置面）
// 的 C++ 移植。差异：zip 提取用模块内 ZipArchive（Rust 侧调 unzip -p）；
// 默认处理器注册经运行时 dlopen CoreServices（不改模块链接配置）。
#include "shellgen.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QRegularExpression>

#include <cstdio>

#include "apk.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "process.hpp"

#ifdef Q_OS_UNIX
#include <sys/time.h>
#endif

namespace mw::app {
namespace {

QString mwineBin()
{
    // 启动脚本里优先 PATH 中的 mwine；这里只提供生成时刻的回退路径
    if (QCoreApplication::instance())
        return QCoreApplication::applicationFilePath();
    return QStringLiteral("mwine");
}

// run_tool：执行外部工具，失败（非 0 退出）报错；stdout 经 out 带出
//（mw::Result<QString> 与错误类型歧义，故用 out 参数）。
mw::Result<bool> runTool(const QString& program, const QStringList& args, QString* out = nullptr)
{
    const auto r = mw::runProcess(program, args);
    if (!r.error.isEmpty())
        return mw::err<bool>(QStringLiteral("无法执行 %1: %2").arg(program, r.error));
    if (!r.ok())
        return mw::Result<bool>::Err(QStringLiteral("%1 失败（%2）：%3").arg(
            program, QString::number(r.exitCode), r.stdErrStr().trimmed()));
    if (out)
        *out = r.stdOutStr();
    return mw::Result<bool>::Ok();
}

// LSSetDefaultRoleHandlerForContentType（kLSRolesAll）：CoreServices/CoreFoundation
// 经 QLibrary 动态解析——模块 CMake 不追加 framework 链接也能用。
bool setDefaultHandler(const QString& uti, const QString& bundleId, QString* err)
{
#ifdef Q_OS_MACOS
    QLibrary cf(QStringLiteral(
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"));
    QLibrary cs(QStringLiteral(
        "/System/Library/Frameworks/CoreServices.framework/CoreServices"));
    if (!cf.load() || !cs.load()) {
        if (err)
            *err = QStringLiteral("CoreServices/CoreFoundation 动态加载失败");
        return false;
    }
    using CFStringRef = const void*;
    auto cfStringCreate = reinterpret_cast<CFStringRef (*)(void*, const char*, quint32)>(
        cf.resolve("CFStringCreateWithCString"));
    auto lsSet = reinterpret_cast<int (*)(CFStringRef, quint32, CFStringRef)>(
        cs.resolve("LSSetDefaultRoleHandlerForContentType"));
    if (!cfStringCreate || !lsSet) {
        if (err)
            *err = QStringLiteral("LSSetDefaultRoleHandlerForContentType 符号缺失");
        return false;
    }
    constexpr quint32 kCfStringEncodingUtf8 = 0x08000100;
    constexpr quint32 kLsRolesAll = 0xFFFFFFFFu; // (UInt32)-1
    const CFStringRef utiRef =
        cfStringCreate(nullptr, uti.toUtf8().constData(), kCfStringEncodingUtf8);
    const CFStringRef handlerRef =
        cfStringCreate(nullptr, bundleId.toUtf8().constData(), kCfStringEncodingUtf8);
    const int rc = lsSet(utiRef, kLsRolesAll, handlerRef);
    return rc == 0; // OSStatus noErr = 0
#else
    Q_UNUSED(uti)
    Q_UNUSED(bundleId)
    Q_UNUSED(err)
    return true;
#endif
}

// 从我们写出的单行式 Info.plist 提取 <key>K</key><string>V</string>。
QString plistValue(const QByteArray& plist, const QString& key)
{
    const QRegularExpression re(
        QStringLiteral("<key>%1</key>\\s*<string>([^<]*)</string>").arg(key));
    const auto m = re.match(QString::fromUtf8(plist));
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

// —— 路径与文本工具 ——

QString defaultAppShellsDir()
{
    return QDir(mw::homeDir()).filePath(QStringLiteral("Applications/Mwine"));
}

QString sanitizeFileName(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (u == u'/' || u == u':' || u == u'\\' || u == u'"' || u == u'\''
            || u == u'$' || u == u'`' || u == u'\0')
            out += QLatin1Char('_');
        else
            out += c;
    }
    return out.trimmed();
}

QString xmlEscape(const QString& s)
{
    QString out = s;
    out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    out.replace(QLatin1Char('\''), QStringLiteral("&apos;"));
    return out;
}

// —— bundle 内嵌配置 ——

std::optional<BundledConfig> parseAppConf(const QByteArray& text)
{
    BundledConfig cfg;
    std::optional<QString> package;
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& line : lines) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString k = line.left(eq).trimmed();
        const QString v = line.mid(eq + 1).trimmed();
        if (k == QLatin1String("package")) {
            package = v;
        } else if (k == QLatin1String("title")) {
            cfg.title = v;
        } else if (k == QLatin1String("avd")) {
            cfg.avd = v.isEmpty() ? std::nullopt : std::optional<QString>(v);
        } else if (k == QLatin1String("display")) {
            cfg.display = v;
        } else if (k == QLatin1String("fps")) {
            bool ok = false;
            const float f = v.toFloat(&ok);
            cfg.fps = ok ? std::optional<float>(f) : std::nullopt;
        } else if (k == QLatin1String("fullscreen")) {
            cfg.fullscreen = v == QLatin1String("1");
        }
    }
    if (!package || package->isEmpty())
        return std::nullopt;
    cfg.package = *package;
    return cfg;
}

std::optional<BundledConfig> readBundledConfig()
{
    const QString exe = mwineBin();
    if (exe.isEmpty() || exe == QLatin1String("mwine"))
        return std::nullopt;
    const QDir macosDir = QFileInfo(exe).dir(); // <X>.app/Contents/MacOS
    if (macosDir.dirName() != QLatin1String("MacOS"))
        return std::nullopt;
    const QString contents =
        QDir::cleanPath(macosDir.absoluteFilePath(QStringLiteral(".."))); // <X>.app/Contents
    const QString appDirName = QFileInfo(contents).fileName();
    if (!appDirName.endsWith(QLatin1String(".app")))
        return std::nullopt;
    QFile conf(QDir(contents).filePath(QStringLiteral("Resources/app.conf")));
    if (!conf.open(QIODevice::ReadOnly))
        return std::nullopt;
    return parseAppConf(conf.readAll());
}

bool bundledAppWindowMode(const QString& package, const QString& appmodesPath)
{
    const QString path = appmodesPath.isEmpty()
        ? QDir(mw::mwineDir()).filePath(QStringLiteral("appmodes.json"))
        : appmodesPath;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return false;
    return doc.object().value(package).toString() == QLatin1String("window");
}

// —— 壳扫描 ——

QVector<AppShellEntry> scanAppShells(const QString& dir)
{
    QVector<AppShellEntry> out;
    const QDir d(dir.isEmpty() ? defaultAppShellsDir() : dir);
    const QStringList appDirs = d.entryList({QStringLiteral("*.app")},
                                            QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : appDirs) {
        AppShellEntry e;
        e.appDir = d.filePath(name);
        QFile conf(QDir(e.appDir).filePath(QStringLiteral("Contents/Resources/app.conf")));
        if (conf.open(QIODevice::ReadOnly)) {
            if (auto cfg = parseAppConf(conf.readAll()))
                e.conf = std::move(*cfg);
        }
        QFile plist(QDir(e.appDir).filePath(QStringLiteral("Contents/Info.plist")));
        if (plist.open(QIODevice::ReadOnly))
            e.exeName = plistValue(plist.readAll(), QStringLiteral("CFBundleExecutable"));
        e.hasIcon = QFileInfo(QDir(e.appDir).filePath(
                                  QStringLiteral("Contents/Resources/AppIcon.icns"))).isFile();
        out.push_back(std::move(e));
    }
    return out;
}

// —— 图标工具 ——

mw::Result<bool> makeIcns(const QString& sourceBitmap, const QString& destIcns)
{
    const QFileInfo dest(destIcns);
    const QString iconset = dest.dir().filePath(dest.completeBaseName()
                                                    + QStringLiteral(".iconset"));
    QDir().mkpath(iconset);
    const QString master = QDir(iconset).filePath(QStringLiteral("master.png"));
    auto r = runTool(QStringLiteral("sips"),
                     {QStringLiteral("-s"), QStringLiteral("format"), QStringLiteral("png"),
                      sourceBitmap, QStringLiteral("--out"), master});
    if (!r) {
        QDir(iconset).removeRecursively();
        return mw::Result<bool>::Err(
            QStringLiteral("sips 转换图标失败（webp/png 应支持）：%1").arg(r.error()));
    }
    static const std::pair<const char*, quint32> sizes[] = {
        {"icon_16x16.png", 16},     {"icon_16x16@2x.png", 32},
        {"icon_32x32.png", 32},     {"icon_32x32@2x.png", 64},
        {"icon_128x128.png", 128},  {"icon_128x128@2x.png", 256},
        {"icon_256x256.png", 256},  {"icon_256x256@2x.png", 512},
        {"icon_512x512.png", 512},  {"icon_512x512@2x.png", 1024},
    };
    for (const auto& [name, size] : sizes) {
        auto rz = runTool(QStringLiteral("sips"),
                          {QStringLiteral("-z"), QString::number(size), QString::number(size),
                           master, QStringLiteral("--out"), QDir(iconset).filePath(
                                                               QLatin1String(name))});
        if (!rz) {
            QDir(iconset).removeRecursively();
            return mw::Result<bool>::Err(rz.error());
        }
    }
    auto ri = runTool(QStringLiteral("iconutil"),
                      {QStringLiteral("-c"), QStringLiteral("icns"), iconset,
                       QStringLiteral("-o"), destIcns});
    QDir(iconset).removeRecursively();
    if (!ri)
        return mw::Result<bool>::Err(ri.error());
    return mw::Result<bool>::Ok();
}

std::optional<QString> extractAppIcon(const QString& apkPath,
                                      const std::optional<ApkBadging>& badging,
                                      const QString& resourcesDir)
{
    auto zip = ZipArchive::open(apkPath);
    if (!zip)
        return std::nullopt;
    std::optional<QString> entry;
    if (badging && badging->icon) {
        entry = *badging->icon;
        if (!isRasterIcon(*entry)) {
            const QStringList entries = zip.value().entryNames();
            entry = pickRasterFallback(entries, *entry);
        }
    } else {
        entry = AdbAppManager::heuristicIcon(zip.value().entryNames());
    }
    if (!entry)
        return std::nullopt;
    auto bytes = zip.value().readEntry(*entry);
    if (!bytes || !bytes.value())
        return std::nullopt;
    const QString tmp = QDir::temp().filePath(
        QStringLiteral("mwine-icon-%1").arg(QCoreApplication::applicationPid()));
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly) || f.write(*bytes.value()) != bytes.value()->size())
            return std::nullopt;
    }
    const QString icns = QDir(resourcesDir).filePath(QStringLiteral("AppIcon.icns"));
    if (!makeIcns(tmp, icns)) {
        QFile::remove(icns);
        QFile::remove(tmp);
        return std::nullopt;
    }
    QFile::remove(tmp);
    return icns;
}

std::optional<QString> findCompanionApk()
{
    const QByteArray env = qgetenv("MWINE_BRIDGE_APK");
    if (!env.isEmpty() && QFileInfo(QFile::decodeName(env)).isFile())
        return QFile::decodeName(env);
    // .app 打包形态：<Houttuynia.app>/Contents/MacOS/mwine → ../Resources/bin/
    const QString exe = mwineBin();
    if (!exe.isEmpty() && exe != QLatin1String("mwine")) {
        QDir contents = QFileInfo(exe).dir(); // Contents/MacOS
        if (contents.cdUp()) {
            const QString bundled =
                contents.filePath(QStringLiteral("Resources/bin/mwine-bridge.apk"));
            if (QFileInfo(bundled).isFile())
                return bundled;
        }
    }
    // 从可执行文件向上找仓库的 assets/companion/dist/mwine-bridge.apk
    QDir dir = QFileInfo(exe).dir();
    do {
        const QString candidate = dir.filePath(QStringLiteral("assets/companion/dist/mwine-bridge.apk"));
        if (QFileInfo(candidate).isFile())
            return candidate;
    } while (dir.cdUp());
    return std::nullopt;
}

std::optional<QString> renderIconOnDevice(AdbShell& adb, const QString& package,
                                          const QString& resourcesDir)
{
    // companion 已装则复用，未装则安装（幂等）
    auto haveBridge = adb.run(QStringLiteral("pm path com.mwine.bridge"), 10 * 1000);
    if (!haveBridge || !haveBridge.value().out.contains(QLatin1String("package:"))) {
        const auto apk = findCompanionApk();
        if (!apk)
            return std::nullopt;
        if (!adb.installApk(*apk, 120 * 1000))
            return std::nullopt;
    }
    auto capReply = adb.run(QStringLiteral("pm path com.mwine.bridge"), 10 * 1000);
    if (!capReply)
        return std::nullopt;
    QString cap;
    const QStringList lines = capReply.value().out.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& l : lines) {
        const QString t = l.trimmed();
        if (t.startsWith(QLatin1String("package:"))) {
            cap = t.mid(QString("package:").size()).trimmed();
            break;
        }
    }
    if (cap.isEmpty())
        return std::nullopt;

    const QString remotePng = QStringLiteral("/data/local/tmp/mwine-icon-%1.png")
                                  .arg(QCoreApplication::applicationPid());
    auto dump = adb.run(
        QStringLiteral("CLASSPATH=%1 app_process / com.mwine.bridge.IconDump %2 %3 512")
            .arg(cap, package, remotePng),
        30 * 1000);
    if (!dump || !dump.value().out.contains(QLatin1String("ok"))) {
        if (dump)
            MW_LOG("app", QStringLiteral("设备渲染失败：%1").arg(dump.value().out.trimmed()));
        return std::nullopt;
    }
    auto png = adb.pullFile(remotePng);
    const bool got = png && png.value().size() > 1000;
    adb.run(QStringLiteral("rm -f '%1'").arg(remotePng), 10 * 1000);
    if (!got)
        return std::nullopt;
    const QString tmp = QDir::temp().filePath(
        QStringLiteral("mwine-icon-%1.png").arg(QCoreApplication::applicationPid()));
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly) || f.write(png.value()) != png.value().size())
            return std::nullopt;
    }
    const QString icns = QDir(resourcesDir).filePath(QStringLiteral("AppIcon.icns"));
    if (!makeIcns(tmp, icns)) {
        QFile::remove(icns);
        QFile::remove(tmp);
        return std::nullopt;
    }
    QFile::remove(tmp);
    return icns;
}

// —— macOS 进程辅助 ——

bool codesignBestEffort(const QString& path)
{
#ifdef Q_OS_MACOS
    const auto sign = mw::runProcess(QStringLiteral("codesign"),
                                     {QStringLiteral("-f"), QStringLiteral("-s"),
                                      QStringLiteral("-"), path});
    const auto verified = mw::runProcess(
        QStringLiteral("codesign"), {QStringLiteral("--verify"), path});
    return sign.error.isEmpty() && verified.ok();
#else
    Q_UNUSED(path)
    return true;
#endif
}

mw::Result<bool> lsregister(const QString& appDir)
{
    const QString ls = QStringLiteral(
        "/System/Library/Frameworks/CoreServices.framework/Frameworks/"
        "LaunchServices.framework/Support/lsregister");
    if (auto r = runTool(ls, {QStringLiteral("-f"), appDir}); !r)
        return mw::Result<bool>::Err(
            QStringLiteral("lsregister 注册失败: %1").arg(r.error()));
    return mw::Result<bool>::Ok();
}

// —— .app 壳生成 ——

mw::Result<CreateAppShellResult> createAppShell(const CreateAppShellOptions& opts)
{
    CreateAppShellResult result;

    // 解析包名 / 显示名 / 图标来源
    std::optional<QString> apkPath;
    std::optional<ApkBadging> badging;
    std::optional<QString> manifestLabel;
    QString package = opts.package;
    if (!opts.apkPath.isEmpty()) {
        const QString apk = QFileInfo(opts.apkPath).canonicalFilePath();
        if (apk.isEmpty())
            return mw::err<CreateAppShellResult>(
                QStringLiteral("APK 路径无效（%1）").arg(opts.apkPath));
        apkPath = apk;
        // 纯 C++ 路径拿包名（零 SDK），aapt2 存在时叠加 badging（标签/图标更准）
        auto local = AdbAppManager::readLocalApk(apk);
        if (!local)
            return mw::err<CreateAppShellResult>(local.error());
        package = local.value().manifest.package;
        manifestLabel = local.value().manifest.label;
        badging = local.value().badging;
        if (package.isEmpty())
            return mw::err<CreateAppShellResult>(
                QStringLiteral("badging 里没有包名：%1").arg(apk));
        if (!opts.noInstall) {
            if (opts.serial.isEmpty())
                return mw::err<CreateAppShellResult>(
                    QStringLiteral("无在线设备且自动启动失败，安装已取消"));
            MW_LOG("app", QStringLiteral("安装 %1 到 %2 ...").arg(apk, opts.serial));
            AdbShell adb(opts.serial);
            if (auto r = adb.installApk(apk, 600 * 1000); !r)
                return mw::err<CreateAppShellResult>(r.error());
            result.installedToDevice = true;
            MW_LOG("app", QStringLiteral("✓ 已安装 %1").arg(package));
        }
    } else if (package.isEmpty()) {
        return mw::err<CreateAppShellResult>(
            QStringLiteral("请提供包名，或用 apkPath 从 APK 文件创建"));
    }

    QString display = opts.name;
    if (display.isEmpty() && badging && badging->label)
        display = *badging->label;
    if (display.isEmpty() && manifestLabel)
        display = *manifestLabel;
    if (display.isEmpty())
        display = package;
    const QString exeName = sanitizeFileName(display);
    if (exeName.isEmpty())
        return mw::err<CreateAppShellResult>(QStringLiteral("显示名无效"));

    const QString outDir = opts.outDir.isEmpty() ? defaultAppShellsDir() : opts.outDir;
    const QString appDir = QDir(outDir).filePath(exeName + QStringLiteral(".app"));
    const QString contents = QDir(appDir).filePath(QStringLiteral("Contents"));
    const QString macosDir = QDir(contents).filePath(QStringLiteral("MacOS"));
    const QString resources = QDir(contents).filePath(QStringLiteral("Resources"));
    // 全量重建：清掉旧壳里的残留（例如上次的图标）
    QDir(appDir).removeRecursively();
    if (!QDir().mkpath(macosDir) || !QDir().mkpath(resources))
        return mw::err<CreateAppShellResult>(
            errIo(QStringLiteral("无法创建 %1").arg(macosDir)));

    // 图标：本地提取位图 -> 失败则设备侧渲染（companion IconDump，经 Android 自身
    // 管线渲染 adaptive/矢量/混淆资源，宿主侧无解的图标全靠它）
    bool hasIcon = false;
    if (apkPath) {
        if (extractAppIcon(*apkPath, badging, resources)) {
            hasIcon = true;
            MW_LOG("app", QStringLiteral("✓ 图标已从 APK 提取"));
        } else if (!opts.serial.isEmpty()) {
            MW_LOG("app", QStringLiteral("～ APK 内无位图图标，改用设备侧渲染 ..."));
            AdbShell adb(opts.serial);
            if (renderIconOnDevice(adb, package, resources)) {
                hasIcon = true;
                MW_LOG("app", QStringLiteral("✓ 图标已由设备渲染"));
            }
        }
        if (!hasIcon)
            MW_LOG("app",
                   QStringLiteral("～ 图标提取失败（设备不在线且 APK 无位图图标），使用默认图标"));
    } else if (!opts.serial.isEmpty()) {
        // 包名模式：设备侧渲染最准（adaptive/矢量通吃），失败再回退拉 APK 解析
        AdbShell adb(opts.serial);
        if (renderIconOnDevice(adb, package, resources)) {
            hasIcon = true;
            MW_LOG("app", QStringLiteral("✓ 图标已由设备渲染"));
        } else {
            MW_LOG("app", QStringLiteral("拉取 %1 的 APK 以提取图标 ...").arg(package));
            auto remote = adb.run(QStringLiteral("pm path %1").arg(package), 15 * 1000);
            QString remotePath;
            if (remote) {
                const QStringList lines =
                    remote.value().out.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
                for (const QString& l : lines) {
                    const QString t = l.trimmed();
                    if (t.startsWith(QLatin1String("package:"))) {
                        remotePath = t.mid(QString("package:").size());
                        break;
                    }
                }
            }
            if (!remotePath.isEmpty()) {
                if (auto bytes = adb.pullFile(remotePath)) {
                    const QString tmpApk = QDir::temp().filePath(
                        QStringLiteral("mwine-icon-%1.apk").arg(package));
                    QFile tmp(tmpApk);
                    if (tmp.open(QIODevice::WriteOnly)
                        && tmp.write(bytes.value()) == bytes.value().size()) {
                        tmp.close();
                        std::optional<ApkBadging> bg;
                        if (auto tool = findAapt2())
                            bg = aapt2Badging(*tool, tmpApk);
                        if (extractAppIcon(tmpApk, bg, resources)) {
                            hasIcon = true;
                            MW_LOG("app", QStringLiteral("✓ 图标已从设备 APK 提取"));
                        }
                        QFile::remove(tmpApk);
                    }
                }
            } else {
                MW_LOG("app",
                       QStringLiteral("～ 设备上未安装 %1（或拉取失败），图标留空").arg(package));
            }
        }
    }

    // 嵌入式启动器：mwine 二进制本体拷入 .app 作为 CFBundleExecutable。
    // 正规 bundle 进程：macOS 系统菜单栏显示我们的菜单（摄像头/文件）、
    // Dock/Cmd+Tab 显示真实应用图标；二进制自检 bundle 环境读 app.conf 运行。
    const QString exe = mwineBin();
    const QString launcher = QDir(macosDir).filePath(exeName);
    if (!QFile::copy(exe, launcher))
        return mw::err<CreateAppShellResult>(
            errIo(QStringLiteral("拷贝 mwine 二进制到 %1 失败").arg(launcher)));
    QFile::setPermissions(launcher, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                        | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                        | QFileDevice::ExeOther);
    const QString conf = QStringLiteral("package=%1\ntitle=%2\ndisplay=970x2048/430\nfps=30\n"
                                         "fullscreen=0\navd=%3\n")
                             .arg(package, display, opts.avd);
    if (auto w = mw::writeFile(QDir(resources).filePath(QStringLiteral("app.conf")),
                               conf.toUtf8());
        !w)
        return mw::err<CreateAppShellResult>(w.error());

    const QString iconKey = hasIcon
        ? QStringLiteral("  <key>CFBundleIconFile</key><string>AppIcon</string>\n")
        : QString();
    const QString plist = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>CFBundleExecutable</key><string>%1</string>\n"
        "  <key>CFBundleIdentifier</key><string>%2</string>\n"
        "  <key>CFBundleName</key><string>%3</string>\n"
        "  <key>CFBundleDisplayName</key><string>%4</string>\n"
        "  <key>CFBundlePackageType</key><string>APPL</string>\n"
        "  <key>CFBundleShortVersionString</key><string>0.1</string>\n"
        "  <key>CFBundleVersion</key><string>1</string>\n"
        "  <key>LSMinimumSystemVersion</key><string>11.0</string>\n"
        "  <key>NSHighResolutionCapable</key><true/>\n"
        "%5</dict>\n</plist>\n")
        .arg(xmlEscape(exeName), xmlEscape(QStringLiteral("wine.mwine.") + package),
             xmlEscape(display), xmlEscape(display), iconKey);
    if (auto w = mw::writeFile(QDir(contents).filePath(QStringLiteral("Info.plist")),
                               plist.toUtf8());
        !w)
        return mw::err<CreateAppShellResult>(w.error());

    // macOS 26 对 GUI 上下文中拉起的未签名二进制会在 dyld 阶段阻塞（Gatekeeper 校验），
    // 生成的壳与 mwine 本体都做 ad-hoc 签名（失败仅告警，终端直接运行不受影响）
    codesignBestEffort(appDir);
    if (!codesignBestEffort(exe)) {
        MW_LOG("app", QStringLiteral("～ mwine 二进制未签名（Finder/.app 启动可能被 Gatekeeper 阻塞）"));
        MW_LOG("app", QStringLiteral("  修复：codesign -f -s - \"%1\"").arg(exe));
    }

    if (auto r = lsregister(appDir); !r)
        return mw::err<CreateAppShellResult>(r.error());
#ifdef Q_OS_UNIX
    // 重建同名壳时 Finder 图标缓存不会自动刷新，touch 触发重扫
    ::utimes(appDir.toUtf8().constData(), nullptr);
#endif
    result.appDir = appDir;
    result.package = package;
    result.display = display;
    result.hasIcon = hasIcon;
    if (opts.printPath) {
        MW_LOG("app", appDir);
    } else {
        MW_LOG("app", QStringLiteral("✓ 已生成 %1").arg(appDir));
        MW_LOG("app", QStringLiteral("  Finder 双击即启动应用窗口；Launchpad 稍候收录（也可拖入 Dock）"));
    }
    return mw::Result<CreateAppShellResult>::Ok(std::move(result));
}

// —— APK 双击安装器 ——

mw::Result<bool> createApkInstaller(const QString& outDir, QString* appDirOut)
{
    const QString dir = outDir.isEmpty() ? defaultAppShellsDir() : outDir;
    const QString appDir = QDir(dir).filePath(QStringLiteral("Houttuynia Installer.app"));
    if (!QDir().mkpath(dir))
        return mw::err<bool>(errIo(QStringLiteral("无法创建 %1").arg(dir)));
    QDir(appDir).removeRecursively();
    const QString contents = QDir(appDir).filePath(QStringLiteral("Contents"));

    // 1. osacompile 生成 applet（可执行名为 droplet）。
    // 双击文件走 Apple Events（odoc），bash 壳收不到路径 → `on open` 原生接收文件。
    static const char kAppleScript[] = R"AS(on open theFiles
	set appPath to POSIX path of (path to me)
	set shPath to quoted form of (appPath & "/Contents/Resources/install.sh")
	set args to ""
	repeat with f in theFiles
		set args to args & " " & quoted form of (POSIX path of f)
	end repeat
	try
		do shell script "/bin/bash " & shPath & args
	on error errMsg
		display notification ("安装器错误: " & errMsg) with title "Houttuynia 安装器"
	end try
end open

on run
	display notification "在 Finder 中双击任意 .apk 文件即可安装到 mwine" with title "Houttuynia 安装器"
end run)AS";
    const QString scriptPath = QDir::temp().filePath(
        QStringLiteral("mwine-installer-%1.applescript").arg(QCoreApplication::applicationPid()));
    if (auto w = mw::writeFile(scriptPath, QByteArray(kAppleScript)); !w)
        return mw::err<bool>(w.error());
    auto compiled = runTool(QStringLiteral("osacompile"),
                            {QStringLiteral("-o"), appDir, scriptPath});
    QFile::remove(scriptPath);
    if (!compiled)
        return mw::err<bool>(
            QStringLiteral("osacompile 生成 applet 失败: %1").arg(compiled.error()));
    const QString resources = QDir(contents).filePath(QStringLiteral("Resources"));
    if (!QDir().mkpath(resources))
        return mw::err<bool>(errIo(QStringLiteral("无法创建 %1").arg(resources)));

    // 2. 安装逻辑放 Resources/install.sh（AppleScript 只做文件转发）。
    // 说明：Rust 版此处还会用 swiftc 预编译悬浮进度窗口（progress-bin）；属于 CLI
    // 安装器增强，未随模块移植——脚本按存在性探测，缺失时自动降级为仅通知反馈。
    static const char kInstallSh[] = R"SH(#!/bin/bash
# 由 mwine app installer 生成：接收 .apk 路径 -> 安装 -> 生成 .app 壳 -> 启动
LOG="$HOME/Library/Logs/mwine-installer.log"
mkdir -p "$(dirname "$LOG")"
exec >>"$LOG" 2>&1

MWINE="mwine"
if ! command -v mwine >/dev/null 2>&1; then
  MWINE="__MWINE_EXE__"
fi

notify() { osascript -e "display notification \"$1\" with title \"Houttuynia 安装器\" subtitle \"$2\"" >/dev/null 2>&1; }

ok=0; fail=0; last_app=""
N=$#
i=0
PROG="$(mktemp /tmp/mwine-progress.XXXXXX)"
echo "S1 0 准备安装…" > "$PROG"
APP_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
# 悬浮进度窗口（预编译 Swift）；失败不影响安装
if [ -x "$APP_DIR/Contents/Resources/progress-bin" ]; then
  "$APP_DIR/Contents/Resources/progress-bin" "$PROG" >>"$LOG" 2>&1 &
fi

for f in "$@"; do
  case "$f" in
    *.[Aa][Pp][Kk]) ;;
    *) continue ;;
  esac
  i=$((i+1))
  name=$(basename "$f" .apk); name=${name%.[Aa][Pp][Kk]}
  echo "[$(date '+%F %T')] install $f"
  echo "APP ${name}" > "$PROG"
  echo "S1 0 准备传送安装包（${i}/${N}）" > "$PROG"
  notify "正在安装 ${name}…（${i}/${N}）" "$f"
  err=$( { "$MWINE" install --progress-file "$PROG" "$f"; } 2>&1 ) || {
    fail=$((fail+1))
    echo "install failed: $err"
    reason=$(echo "$err" | tail -n 1 | cut -c1-150)
    case "$err" in
      *NO_MATCHING_ABIS*)
        reason="该 APK 不含 arm64 原生库（x86/32 位包），arm64 模拟器装不了。请换 arm64 版本 APK" ;;
      *无在线设备*|*虚拟机引导超时*|*offline*|*'no online'*|*DeviceNotFound*|*ServerUnreachable*)
        reason="虚拟机未能自动启动。请打开 Houttuynia 点「启动」后重试" ;;
      *INSUFFICIENT_STORAGE*)
        reason="设备存储不足。卸载部分应用后重试" ;;
    esac
    notify "❌ ${name}：${reason}" "$f"
    echo "ERROR 安装 ${name} 失败：${reason}" > "$PROG"
    break
  }
  echo "S3 IND" > "$PROG"
  notify "已安装 ${name}，正在生成应用图标…" "$f"
  if app=$( { "$MWINE" app create --apk "$f" --no-install --print-path; } 2>>"$LOG" | tail -n 1 ); then
    ok=$((ok+1)); last_app="$app"
    echo "created $app"
    notify "✓ ${name} 已就绪（${app/#$HOME/\~}），启动中…" "$f"
  else
    fail=$((fail+1))
    notify "❌ ${name} 安装成功但生成 .app 失败（详见 mwine-installer.log）" "$f"
  fi
done

if [ "$fail" -eq 0 ] && [ "$ok" -gt 0 ]; then
  echo "DONE 安装完成：成功 $ok 个应用" > "$PROG"
  notify "✓ 本次共安装 $ok 个应用" "完成"
  [ -n "$last_app" ] && open "$last_app"
fi
( sleep 4; rm -f "$PROG" ) &
)SH";
    const QString installSh =
        QString::fromUtf8(kInstallSh).replace(QStringLiteral("__MWINE_EXE__"), mwineBin());
    const QString shPath = QDir(resources).filePath(QStringLiteral("install.sh"));
    if (auto w = mw::writeFile(shPath, installSh.toUtf8()); !w)
        return mw::err<bool>(w.error());
    QFile::setPermissions(shPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                         | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                         | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                         | QFileDevice::ExeOther);

    // 3. 覆盖 Info.plist：文档类型声明 + UTI + 后台运行
    static const char kInstallerPlist[] = R"PL(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>droplet</string>
  <key>CFBundleIdentifier</key><string>com.houttuynia.installer</string>
  <key>CFBundleName</key><string>Houttuynia Installer</string>
  <key>CFBundleDisplayName</key><string>mwine APK 安装器</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleIconFile</key><string>droplet</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <!-- 后台工具：不占 Dock -->
  <key>LSUIElement</key><true/>
  <key>UTImportedTypeDeclarations</key>
  <array>
    <dict>
      <key>UTTypeIdentifier</key><string>com.android.apk</string>
      <key>UTTypeDescription</key><string>Android 应用包</string>
      <key>UTTypeConformsTo</key>
      <array>
        <string>public.data</string>
        <string>public.archive.zip</string>
      </array>
      <key>UTTypeTagSpecification</key>
      <dict>
        <key>public.filename-extension</key><array><string>apk</string></array>
        <key>public.mime-type</key><array><string>application/vnd.android.package-archive</string></array>
      </dict>
    </dict>
  </array>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Android 应用包</string>
      <key>LSHandlerRank</key><string>Owner</string>
      <key>LSItemContentTypes</key><array><string>com.android.apk</string></array>
      <key>CFBundleTypeExtensions</key><array><string>apk</string></array>
      <key>LSIsAppleDefaultForType</key><true/>
    </dict>
  </array>
</dict>
</plist>
)PL";
    if (auto w = mw::writeFile(QDir(contents).filePath(QStringLiteral("Info.plist")),
                               QByteArray(kInstallerPlist));
        !w)
        return mw::err<bool>(w.error());

    // 4. ad-hoc 签名（绑定评估与 Gatekeeper 都需要）+ 注册 + 显式默认处理器
    const QString exe = mwineBin();
    if (!codesignBestEffort(appDir))
        MW_LOG("app", QStringLiteral("～ 安装器签名失败（macOS 可能拒绝绑定 .apk 双击）"));
    if (!codesignBestEffort(exe)) {
        MW_LOG("app", QStringLiteral("～ mwine 二进制未签名（GUI 启动可能被 Gatekeeper 阻塞）"));
        MW_LOG("app", QStringLiteral("  修复：codesign -f -s - \"%1\"").arg(exe));
    }
    if (auto r = lsregister(appDir); !r)
        return mw::err<bool>(r.error());
    // .apk 在用户机器上可能被 UC 浏览器等抢注为 com.ucweb.pkg（Finder 据此选默认 App），
    // 必须 UTI 全接管：自己的声明 + 抢注者 + 系统动态 UTI 三路都设
    for (const char* uti :
         {"com.android.apk", "com.ucweb.pkg", "com.android.package-archive"}) {
        QString err;
        if (!setDefaultHandler(QLatin1String(uti), QStringLiteral("com.houttuynia.installer"),
                               &err))
            MW_LOG("app", QStringLiteral("～ 设置 %1 处理器失败: %2").arg(QLatin1String(uti), err));
    }

    MW_LOG("app", QStringLiteral("✓ 已生成 %1").arg(appDir));
    MW_LOG("app", QStringLiteral("  之后在 Finder 双击任意 .apk：自动安装 -> 生成应用 .app -> 自动启动"));
    if (appDirOut)
        *appDirOut = appDir;
    return mw::Result<bool>::Ok();
}

} // namespace mw::app
