// install / apps / files / camera / run / notify 子命令 + bundle 模式入口。
// ← crates/mwine-cli/src/main.rs：cmd_install / InstallBar / ProgressFileWriter /
//    cmd_apps_* / cmd_files_* / push_via_media_root / cmd_camera_* / cmd_run /
//    resolve_run_serial / cmd_notify / grant_and_rebind / handle_bridge_connection /
//    bundled_app_config / bundled_main
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

#include "adb.hpp"
#include "app.hpp"
#include "args.hpp"
#include "cli_common.hpp"
#include "commands.hpp"
#include "fs.hpp"
#include "net.hpp"
#include "process.hpp"
#include "supervisor.hpp"

namespace mwine {

// ===================== 安装进度条 =====================

// 安装进度条：TTY 下单行 \r 刷新（传输 % + 安装阶段转圈计时）；
// 非 TTY（日志/管道）按 25% 阶梯打印纯文本行。
class InstallBar {
public:
    InstallBar() : tty_(stdoutIsTty()) {}
    ~InstallBar() { finish(); }

    void update(const mw::adb::InstallProgress& p)
    {
        using namespace std::chrono;
        if (p.kind == mw::adb::InstallProgress::Kind::Push) {
            if (p.total == 0)
                return;
            const unsigned pct = static_cast<unsigned>(p.sent * 100.0 / p.total);
            if (tty_) {
                std::fprintf(stderr, "\r  传输 APK %3u%%（%.1f/%.1f MB）", pct,
                             p.sent / 1.0e6, p.total / 1.0e6);
                std::fflush(stderr);
            } else {
                const unsigned step = pct / 25;
                if (step > lastPctStep_) {
                    lastPctStep_ = step;
                    std::printf("  传输 APK %u%%\n", step * 25);
                }
            }
        } else { // Installing
            if (tty_) {
                std::fprintf(stderr, "\r\x1b[K  传输完成，设备安装中（dexopt 视包大小 10-60s）…");
                std::fflush(stderr);
                stop_ = std::make_unique<std::atomic_bool>(false);
                auto stop = stop_.get();
                std::thread([stop] {
                    const auto start = steady_clock::now();
                    const char* frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
                    int i = 0;
                    while (!stop->load(std::memory_order_relaxed)) {
                        const auto secs =
                            duration_cast<seconds>(steady_clock::now() - start).count();
                        std::fprintf(stderr, "\r  设备安装中… %s %2llds", frames[i % 10],
                                     static_cast<long long>(secs));
                        std::fflush(stderr);
                        i += 1;
                        std::this_thread::sleep_for(milliseconds(200));
                    }
                }).detach();
            } else {
                std::printf("  传输完成，设备安装中…\n");
            }
        }
    }

    void finish()
    {
        if (stop_) {
            stop_->store(true, std::memory_order_relaxed);
            stop_.reset();
        }
        if (tty_) {
            std::fprintf(stderr, "\r\x1b[K");
            std::fflush(stderr);
        }
    }

private:
    bool tty_;
    unsigned lastPctStep_ = 0;
    std::unique_ptr<std::atomic_bool> stop_;
};

// 安装进度状态文件写入（<pct> <label> 单行，供 JXA 进度窗口轮询）；
// 节流：百分比变化或每 400ms 最多写一次。
class ProgressFileWriter {
public:
    ProgressFileWriter(QString path, QString name)
        : path_(std::move(path)), name_(std::move(name))
    {
    }

    void update(const mw::adb::InstallProgress& p)
    {
        if (path_.isEmpty())
            return;
        unsigned pct;
        QString label;
        if (p.kind == mw::adb::InstallProgress::Kind::Push) {
            if (p.total == 0)
                return;
            pct = static_cast<unsigned>(p.sent * 100.0 / p.total);
            label = QStringLiteral("已传送 %1/%2 MB")
                        .arg(p.sent / 1.0e6, 0, 'f', 1)
                        .arg(p.total / 1.0e6, 0, 'f', 1);
        } else {
            // S2 = 设备侧安装（dexopt），窗口显示已进行秒数
            lastPct_ = 100;
            QFile f(path_);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write("S2 IND\n");
            return;
        }
        const bool changed = pct != lastPct_;
        const auto now = std::chrono::steady_clock::now();
        if (!changed && now - lastWrite_ < std::chrono::milliseconds(400))
            return;
        lastPct_ = pct;
        lastWrite_ = now;
        QFile f(path_);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QStringLiteral("%1 %2\n").arg(pct).arg(label).toUtf8());
    }

private:
    QString path_;
    QString name_;
    unsigned lastPct_ = 0;
    std::chrono::steady_clock::time_point lastWrite_ = std::chrono::steady_clock::now();
};

// ===================== install =====================

mw::Result<bool> cmdInstall(Args& args)
{
    const auto apkArg = args.take();
    if (!apkArg)
        return badUsage<bool>(QStringLiteral("Usage: mwine install <apk> [--serial <s>] [--progress-file <f>]"));
    const std::optional<QString> serialOpt = args.opt("serial");
    const std::optional<QString> progressFile = args.opt("progress-file");

    auto client = adbClient();
    client.ensureServer();
    auto vmLine = [&](const QString& text) {
        if (progressFile) {
            QFile f(*progressFile);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QStringLiteral("VM IND %1\n").arg(text).toUtf8());
        }
    };
    QString serial;
    if (serialOpt) {
        serial = *serialOpt;
    } else {
        const auto s = ensureInstallTarget(&client, vmLine);
        if (!s)
            return mw::err<bool>(s.error());
        serial = s.value();
    }
    const QString apk = QFileInfo(*apkArg).canonicalFilePath();
    if (apk.isEmpty())
        return mw::err<bool>(QStringLiteral("APK 路径无效: %1").arg(*apkArg));
    std::printf("安装 %s 到 %s ...\n", qPrintable(apk), qPrintable(serial));

    const QString name = QFileInfo(apk).fileName();
    InstallBar bar;
    ProgressFileWriter pw(progressFile.value_or(QString()), name);
    const auto result = client.installApkWithProgress(
        serial, apk, 600000, [&](const mw::adb::InstallProgress& p) {
            bar.update(p);
            pw.update(p);
        });
    bar.finish();
    if (!result)
        return mw::err<bool>(result.error());
    std::printf("✓ %s\n", qPrintable(result.value()));
    // 授予运行时权限（扫码/存储等）。包名用 mw::app 的纯 C++ APK 解析
    //（Rust 版经 aapt2 badging；C++ 侧 AXML 直读零 SDK 依赖）。
    QString pkg;
    if (const auto local = mw::app::AdbAppManager::readLocalApk(apk); local)
        pkg = local.value().manifest.package;
    if (!pkg.isEmpty()) {
        if (const auto n = client.grantRuntimePermissions(serial, pkg); n && n.value() > 0)
            std::printf("✓ 已授予 %d 项运行时权限\n", n.value());
    }
    return mw::Result<bool>::Ok();
}

// ===================== apps =====================

static QString appmodesPath()
{
    return QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/appmodes.json"));
}

static mw::Result<bool> appsList(Args& args)
{
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    const auto pkgs = adbClient().listPackages(serialR.value());
    if (!pkgs)
        return mw::err<bool>(pkgs.error());
    for (const auto& p : pkgs.value())
        std::printf("%s\n", qPrintable(p));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> appsLaunch(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(QStringLiteral("Usage: mwine apps launch <package> [--serial <s>]"));
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    if (auto r = adbClient().launchPackage(serialR.value(), *package); !r)
        return mw::err<bool>(r.error());
    std::printf("✓ 已启动 %s\n", qPrintable(*package));
    return mw::Result<bool>::Ok();
}

// 应用渲染模式表（~/.mwine/appmodes.json，守护进程 app_run 读取）。
static mw::Result<bool> appsMode(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(QStringLiteral("Usage: mwine apps mode <package> [window|emulator]"));
    const std::optional<QString> mode = args.take();
    const QString path = appmodesPath();
    QJsonObject root;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        root = QJsonDocument::fromJson(f.readAll()).object();
    if (mode) {
        if (*mode != QStringLiteral("window") && *mode != QStringLiteral("emulator"))
            return mw::err<bool>(QStringLiteral("mode 只支持 window / emulator，收到 \"%1\"").arg(*mode));
        if (*mode == QStringLiteral("emulator")) {
            root.remove(*package); // 默认模式不落盘，保持文件干净
        } else {
            root.insert(*package, *mode);
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (auto w = mw::writeFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented)); !w)
            return mw::err<bool>(w.error());
        std::printf("✓ %s → %s（%s）\n", qPrintable(*package), qPrintable(*mode), qPrintable(path));
    } else {
        const QString cur = root.value(*package).toString(QStringLiteral("emulator"));
        std::printf("%s: %s\n", qPrintable(*package), qPrintable(cur));
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> appsUninstall(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine apps uninstall <package> [--serial <s>]"));
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    if (auto r = adbClient().shellBlocking(
            serialR.value(), QStringLiteral("pm uninstall -k %1").arg(*package), 30000);
        !r)
        return mw::err<bool>(r.error());
    std::printf("✓ 已卸载 %s\n", qPrintable(*package));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdApps(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(QStringLiteral("Usage: mwine apps <list|launch|mode|uninstall> […]"));
    if (*action == QStringLiteral("list"))
        return appsList(args);
    if (*action == QStringLiteral("launch"))
        return appsLaunch(args);
    if (*action == QStringLiteral("mode"))
        return appsMode(args);
    if (*action == QStringLiteral("uninstall"))
        return appsUninstall(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

// ===================== files =====================

// /sdcard 不可用时的降级通道：/data/local/tmp 中转 → root 复制到
// /data/media/0/<目标> → chown media_rw → MediaStore 插行（content insert）。
static mw::Result<bool> pushViaMediaRoot(const mw::adb::AdbClient* client, const QString& serial,
                                         const QString& local, const QString& to,
                                         const QString& name)
{
    const QString remoteTmp = QStringLiteral("/data/local/tmp/mwine_%1").arg(name);
    if (auto r = client->pushPath(serial, local, QStringLiteral("/data/local/tmp")); !r)
        return mw::err<bool>(r.error());
    // 需要 root：确保 adbd 以 root 运行（userdebug 镜像；重启 adbd 会短暂掉线，
    // 重连后的首个 transport 可能仍 offline，脚本执行需带重试）
    client->root(serial);
    client->waitForDevice(serial, 30000);
    QThread::msleep(1500);
    // to 形如 /sdcard/Download → 真实根映射为 /data/media/0/Download
    QString sub = to;
    if (sub.startsWith(QStringLiteral("/sdcard")))
        sub = sub.mid(QStringLiteral("/sdcard").size());
    if (sub.startsWith(QStringLiteral("/storage/emulated/0")))
        sub = sub.mid(QStringLiteral("/storage/emulated/0").size());
    while (sub.startsWith(QLatin1Char('/')))
        sub.remove(0, 1);
    while (sub.endsWith(QLatin1Char('/')))
        sub.chop(1);
    const QString realDir = QStringLiteral("/data/media/0/%1").arg(sub);
    const QString realPath = QStringLiteral("%1/%2").arg(realDir, name);
    auto esc = [](const QString& s) { return QString(s).replace(QLatin1Char(' '), QStringLiteral("\\ ")); };
    const QString script =
        QStringLiteral("mkdir -p %1 && cp %2 %3 && chown media_rw:media_rw %3 && chmod 660 %3 "
                       "&& echo MWINE_OK")
            .arg(esc(realDir), esc(remoteTmp), esc(realPath));
    std::optional<mw::adb::ShellOutput> out;
    QString lastErr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (auto r = client->shellBlocking(serial, script, 30000)) {
            out = r.value();
            break;
        } else {
            lastErr = r.error();
            QThread::msleep(2000 * (attempt + 1));
        }
    }
    if (!out)
        return mw::err<bool>(QStringLiteral("root 写入失败（%1）").arg(lastErr));
    if (!out->outStr().contains(QLatin1String("MWINE_OK")))
        return mw::err<bool>(QStringLiteral("写入 %1 失败：%2").arg(realPath, out->outStr().trimmed()));
    // MediaStore 注册（应用立即可见）
    const QString ext = QFileInfo(local).suffix().toLower();
    QString mime = QStringLiteral("application/octet-stream");
    if (ext == QLatin1String("png"))
        mime = QStringLiteral("image/png");
    else if (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg"))
        mime = QStringLiteral("image/jpeg");
    else if (ext == QLatin1String("gif"))
        mime = QStringLiteral("image/gif");
    else if (ext == QLatin1String("mp4"))
        mime = QStringLiteral("video/mp4");
    else if (ext == QLatin1String("mp3"))
        mime = QStringLiteral("audio/mpeg");
    else if (ext == QLatin1String("txt"))
        mime = QStringLiteral("text/plain");
    else if (ext == QLatin1String("pdf"))
        mime = QStringLiteral("application/pdf");
    else if (ext == QLatin1String("apk"))
        mime = QStringLiteral("application/vnd.android.package-archive");
    const QString visiblePath = QStringLiteral("/storage/emulated/0/%1/%2").arg(sub, name);
    const QString insert =
        QStringLiteral(
            "content insert --uri content://media/external/file --bind _data:s:%1 --bind mime_type:s:%2")
            .arg(esc(visiblePath), mime);
    const auto reg = client->shellBlocking(serial, insert, 15000);
    if (reg && !reg.value().outStr().contains(QLatin1String("Error")))
        std::printf("  ✓ 已注册媒体库（%s）\n", qPrintable(visiblePath));
    else
        std::printf("  ⚠ 媒体库注册未生效（文件已在 %s，稍后系统扫描会收录）\n",
                    qPrintable(realPath));
    std::printf("  ✓ %s\n", qPrintable(realPath));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> filesPush(Args& args)
{
    QStringList local = args.rest();
    if (local.isEmpty())
        return badUsage<bool>(
            QStringLiteral("Usage: mwine files push <local…> [--to <dir>] [--serial <s>]"));
    QString to = QStringLiteral("/sdcard/Download");
    if (const auto t = args.opt("to"))
        to = *t;
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    const QString serial = serialR.value();
    auto client = adbClient();
    for (const QString& l : local) {
        const QString path = QFileInfo(l).canonicalFilePath();
        if (path.isEmpty())
            return mw::err<bool>(QStringLiteral("路径无效：%1").arg(l));
        const QString name = QFileInfo(path).fileName();
        std::printf("推送 %s -> %s:%s/ ...\n", qPrintable(path), qPrintable(serial), qPrintable(to));
        const auto pushed = client.pushPath(serial, path, to);
        if (pushed) {
            for (const QString& remote : pushed.value())
                std::printf("  ✓ %s\n", qPrintable(remote));
        } else {
            // 本镜像已知 bug：MediaProvider 启动期 codec 探测 ANR → FUSE 存储未挂载，
            // /sdcard 不可写。降级：root 直写 MediaProvider 真实根（/data/media/0/...）
            // + MediaStore 注册 —— 现代 app（SAF/MediaStore，如微信/TG 选文件）照常可见。
            std::printf("  ⚠ 直接推送失败（%s），尝试 root 直写媒体库 ...\n",
                        qPrintable(pushed.error()));
            if (auto r = pushViaMediaRoot(&client, serial, path, to, name); !r)
                return r;
        }
        std::printf("✓ %s 完成\n", qPrintable(name));
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> filesPull(Args& args)
{
    const auto remote = args.take();
    if (!remote)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine files pull <remote> [--to <dir>] [--serial <s>]"));
    const std::optional<QString> to = args.opt("to");
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    QString name = QStringLiteral("file");
    const int slash = remote->lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) {
        const QString tail = remote->mid(slash + 1);
        if (!tail.isEmpty())
            name = tail;
    } else if (!remote->isEmpty()) {
        name = *remote;
    }
    const QString outDir = to.value_or(QDir(mw::homeDir()).filePath(QStringLiteral("Downloads")));
    if (!QDir().mkpath(outDir))
        return mw::err<bool>(QStringLiteral("无法创建目录 %1").arg(outDir));
    const QString dest = QDir(outDir).filePath(name);
    std::printf("拉取 %s:%s -> %s ...\n", qPrintable(serialR.value()), qPrintable(*remote),
                qPrintable(dest));
    const auto data = adbClient().pullFile(serialR.value(), *remote);
    if (!data)
        return mw::err<bool>(data.error());
    if (auto w = mw::writeFile(dest, data.value()); !w)
        return mw::err<bool>(w.error());
    std::printf("✓ %s（%lld 字节）\n", qPrintable(dest),
                static_cast<long long>(data.value().size()));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> filesLs(Args& args)
{
    QString dir = QStringLiteral("/sdcard/Download");
    if (const auto d = args.take())
        dir = *d;
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    const QString escaped = QString(dir).replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    const auto out = adbClient().shellBlocking(
        serialR.value(), QStringLiteral("ls -lh '%1'").arg(escaped), 15000);
    if (!out)
        return mw::err<bool>(out.error());
    if (!out.value().ok())
        return mw::err<bool>(QStringLiteral("ls 失败：%1%2").arg(out.value().outStr(),
                                                                  out.value().errStr()));
    std::printf("%s:\n", qPrintable(dir));
    std::printf("%s", out.value().out.constData());
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdFiles(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(QStringLiteral("Usage: mwine files <push|pull|ls> […]"));
    if (*action == QStringLiteral("push"))
        return filesPush(args);
    if (*action == QStringLiteral("pull"))
        return filesPull(args);
    if (*action == QStringLiteral("ls"))
        return filesLs(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

// ===================== camera =====================

static mw::Result<bool> cameraImage(Args& args)
{
    const auto pathArg = args.take();
    if (!pathArg)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine camera image <path> [--serial <s>] [--front]"));
    const bool front = args.has("front");
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    const QString path = QFileInfo(*pathArg).canonicalFilePath();
    if (path.isEmpty())
        return mw::err<bool>(QStringLiteral("图片路径无效: %1").arg(*pathArg));
    const QString surface = front ? QStringLiteral("table") : QStringLiteral("wall");
    std::printf("注入图片到虚拟场景%s面 %s ...\n", qPrintable(surface), qPrintable(path));
    // emulator 控制台：virtualscene-image <wall|table> <path>
    //（emulated 摄像头默认即虚拟房间场景，无需切模式）
    if (auto r = emuCmd(serialR.value(),
                        QStringLiteral("virtualscene-image %1 %2").arg(surface, path)); !r)
        return mw::err<bool>(r.error());
    std::printf("✓ 完成。在应用里打开『扫一扫』，摄像头看到的就是这张图\n");
    std::printf("  （恢复普通模拟摄像头：mwine camera reset）\n");
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> cameraReset(Args& args)
{
    const bool front = args.has("front");
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    const QString surface = front ? QStringLiteral("table") : QStringLiteral("wall");
    // 空 path 恢复场景默认图
    if (auto r = emuCmd(serialR.value(),
                        QStringLiteral("virtualscene-image %1").arg(surface)); !r)
        return mw::err<bool>(r.error());
    std::printf("✓ 摄像头已恢复默认虚拟场景\n");
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdCamera(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(QStringLiteral("Usage: mwine camera <image|reset> […]"));
    if (*action == QStringLiteral("image"))
        return cameraImage(args);
    if (*action == QStringLiteral("reset"))
        return cameraReset(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

// ===================== run =====================

// 虚拟显示器尺寸解析："WxH/DPI" / "WxH" / "/DPI" / ""
struct NewDisplay {
    std::optional<quint32> width;
    std::optional<quint32> height;
    std::optional<quint32> dpi;
};

static mw::Result<NewDisplay> parseNewDisplay(const QString& s)
{
    NewDisplay nd;
    QString sizePart = s;
    std::optional<QString> dpiPart;
    if (const int slash = s.indexOf(QLatin1Char('/')); slash >= 0) {
        sizePart = s.left(slash);
        dpiPart = s.mid(slash + 1);
    }
    if (!sizePart.isEmpty()) {
        const int x = sizePart.indexOf(QLatin1Char('x'));
        if (x < 0)
            return mw::err<NewDisplay>(QStringLiteral("尺寸格式应为 WxH，如 1280x720"));
        bool okW = false, okH = false;
        const quint32 w = sizePart.left(x).toUInt(&okW);
        const quint32 h = sizePart.mid(x + 1).toUInt(&okH);
        if (!okW)
            return mw::err<NewDisplay>(QStringLiteral("宽度无效"));
        if (!okH)
            return mw::err<NewDisplay>(QStringLiteral("高度无效"));
        nd.width = w;
        nd.height = h;
    }
    if (dpiPart && !dpiPart->isEmpty()) {
        bool ok = false;
        const quint32 d = dpiPart->toUInt(&ok);
        if (!ok)
            return mw::err<NewDisplay>(QStringLiteral("DPI 无效"));
        nd.dpi = d;
    }
    return mw::Result<NewDisplay>::Ok(nd);
}

// run 类命令的设备选择：在线设备优先，否则启动默认 AVD 并等引导。
static mw::Result<QString> resolveRunSerial(const mw::adb::AdbClient* client,
                                            const std::optional<QString>& serial,
                                            const std::optional<QString>& avd)
{
    if (const auto s = resolveInstallSerial(client, serial); s && s.value())
        return mw::Result<QString>::Ok(*s.value());
    QString avdName = avd ? *avd : pickDefaultAvd().value_or(QString());
    if (avdName.isEmpty())
        return mw::err<QString>(
            QStringLiteral("无在线设备且无法确定默认 AVD；先运行 `mwine vm start <avd>` 或设置 MWINE_AVD"));
    const auto sdkR = mw::supervisor::Sdk::detect();
    if (!sdkR)
        return mw::err<QString>(QStringLiteral("Android SDK 未找到: %1").arg(sdkR.error()));
    std::printf("(无在线设备，启动 %s ...)\n", qPrintable(avdName));
    // 始终 windowed：曾认为 headless 建不出副屏（旧注释），36.5.11 实测
    // -no-window 下建屏/guest 枚举都正常——但副屏帧会按主屏尺寸渲染
    //（786x1704 的屏给出 2560x1600 横向帧，直通窗口被信箱化），故
    // multidisplay 直通仍必须 windowed；"无头"视觉由隐藏主窗等效。
    mw::supervisor::LaunchOptions opts;
    opts.windowed = true;
    auto handle = mw::supervisor::EmulatorHandle::launchHeadless(
        sdkR.value(), avdName, opts, const_cast<mw::adb::AdbClient*>(client));
    if (!handle)
        return mw::err<QString>(handle.error());
    std::printf("serial: %s，等待引导 ...\n", qPrintable(handle.value().serial()));
    if (auto b = handle.value().waitBootCompleted(const_cast<mw::adb::AdbClient*>(client), 300000);
        !b)
        return mw::err<QString>(b.error());
    recordLastAvd(avdName);
    const QString s = handle.value().serial();
    new mw::supervisor::EmulatorHandle(std::move(handle.value())); // detach
    return mw::Result<QString>::Ok(s);
}

// 主屏 + 模拟器窗口模式（默认）：am start 到主显示器，emulator 窗口置前。
static mw::Result<bool> runEmulatorMode(const QString& package,
                                        const std::optional<QString>& serialOpt,
                                        const std::optional<QString>& avd, const QString& display)
{
    if (auto nd = parseNewDisplay(display); !nd)
        return mw::err<bool>(nd.error());
    auto client = adbClient();
    client.ensureServer();
    const auto serialR = resolveRunSerial(&client, serialOpt, avd);
    if (!serialR)
        return mw::err<bool>(serialR.error());
    const QString serial = serialR.value();
    // 运行时权限补授（已装应用也会是 granted=false）
    if (const auto n = client.grantRuntimePermissions(serial, package); n && n.value() > 0)
        std::printf("✓ 已授予 %d 项运行时权限\n", n.value());

    // 解析入口并启动（主显示器）
    QString comp;
    if (const auto o = client.shellBlocking(
            serial,
            QStringLiteral("cmd package resolve-activity --brief %1 | tail -1").arg(package), 10000);
        o && o.value().outStr().trimmed().contains(QLatin1Char('/')))
        comp = o.value().outStr().trimmed();
    if (comp.isEmpty()) {
        // GUI 场景（点壳）用户看不到 stderr：弹 macOS 通知指路
        postMacosNotification(QStringLiteral("Houttuynia"), QString(),
                              QStringLiteral("%1 未安装，请把 APK 拖到 MWine 窗口重新安装").arg(package));
        return mw::err<bool>(QStringLiteral("无法解析 %1 的启动 Activity（未安装？）").arg(package));
    }
    if (auto r = client.shellBlocking(
            serial, QStringLiteral("am force-stop %1; am start -n '%2'").arg(package, comp), 15000);
        !r)
        return mw::err<bool>(QStringLiteral("am start 失败: %1").arg(r.error()));

    // 等 app 真正到前台（否则窗口置前了用户看到的还是桌面/上个应用）
    for (int i = 0; i < 12; ++i) {
        QThread::msleep(500);
        if (const auto o = client.shellBlocking(
                serial,
                QStringLiteral(
                    "dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity"),
                3000);
            o && o.value().outStr().contains(package))
            break;
    }

    // emulator 窗口置前（双保险：osascript + 首次运行会请求一次自动化授权）
    {
        QProcess p;
        p.start(QStringLiteral("osascript"),
                { QStringLiteral("-e"),
                  QStringLiteral("tell application \"System Events\" to set frontmost of (first "
                                 "process whose name begins with \"qemu-system\") to true") });
        p.waitForFinished(15000);
    }
    std::printf("✓ 已启动 %s（emulator 窗口已置前台）\n", qPrintable(package));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdRun(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(QStringLiteral("Usage: mwine run <package> [options]"));
    const std::optional<QString> serial = args.opt("serial");
    const std::optional<QString> avd = args.opt("avd");
    QString display = QStringLiteral("944x2048/384");
    if (const auto d = args.opt("display"))
        display = *d;
    const bool window = args.has("window");
    if (window) {
        // 直通窗口：scrcpy 虚拟显示器 + Qt 播放器（mwine-window）。
        // --serial 显式指定（真机：overlay 辅助显示路径）优先；缺省走
        // 安装目标解析（daemon 的 VM 优先，其次唯一在线设备）
        mw::adb::AdbClient client = adbClient();
        mw::Result<QString> serialR =
            serial.has_value()
                ? mw::Result<QString>::Ok(*serial)
                : ensureInstallTarget(
                      &client,
                      [](const QString& line) { std::printf("%s\n", qPrintable(line)); });
        if (!serialR.ok())
            return mw::err<bool>(serialR.error());
        return spawnWindowPlayer(*package, serialR.value(), display, false, QString());
    }
    return runEmulatorMode(*package, serial, avd, display);
}

// ---- 独立应用窗口播放器（mwine-window）----

QString resolveWindowPlayer()
{
    const QString dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(dir).filePath(QStringLiteral("mwine-window")),
        QDir(dir).filePath(QStringLiteral("../window/mwine-window")), // 开发树：cli 与 window 同级
    };
    for (const QString& c : candidates) {
        if (QFileInfo(c).isFile() && QFileInfo(c).isExecutable())
            return QFileInfo(c).absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("mwine-window"));
}

// 应用展示元信息（label + 图标缓存路径）：向 daemon 查应用清单。
// daemon 离线 / 未收录时返回空（调用方退化用包名）。
struct WindowAppMeta {
    QString label;
    QString icon;
};
static WindowAppMeta fetchAppMeta(const QString& package)
{
    WindowAppMeta meta;
    const auto r =
        mw::httpGet(QUrl(QStringLiteral("http://127.0.0.1:7788/api/apps")), 4000);
    if (r.status != 200)
        return meta;
    const QJsonArray apps =
        QJsonDocument::fromJson(r.body).object().value(QStringLiteral("apps")).toArray();
    for (const auto& item : apps) {
        const QJsonObject o = item.toObject();
        if (o.value(QStringLiteral("package")).toString() == package) {
            meta.label = o.value(QStringLiteral("name")).toString();
            meta.icon = o.value(QStringLiteral("icon")).toString();
            break;
        }
    }
    return meta;
}

#ifdef Q_OS_MAC
// 每应用窗口包装 .app：mwine-window 作为裸进程在 Dock 显示黑色终端图标、
// 名字永远是 mwine-window——多应用窗口无法区分。包进 bundle 后 Dock/⌘Tab
// 显示应用自己的名字与真彩图标（macOS 只给 bundle 进程正经身份）。
// 结构（window-apps/<标签>-<包尾>.app）：
//   Contents/MacOS/mwine-window   播放器二进制的拷贝（改版时按大小+mtime 重拷）
//   Contents/Frameworks/PlugIns   符号链接 → Houttuynia.app 内同名目录（Qt 依赖）
//   Contents/Resources/AppIcon.icns（appcache 图标 → makeIcns；图标更新自动重生成）
//   Contents/Resources/bin/scrcpy-server-v4.x（播放器自查找的兜底位）
// 返回 bundle 路径；失败返回空串（调用方回退裸 spawn）。
static QString makeWindowAppBundle(const QString& package, const QString& label,
                                   const QString& icon, const QString& playerExe)
{
    if (package.isEmpty())
        return QString();
    const QString shown = label.isEmpty() ? package : label;
    const QString base = mw::app::sanitizeFileName(shown);
    const QString root = QDir(mw::homeDir()).filePath(
        QStringLiteral("Library/Application Support/MWine/window-apps"));
    const QString bundle = QDir(root).filePath(
        QStringLiteral("%1-%2.app").arg(base, package.section(QLatin1Char('.'), -1)));
    const QString contents = QDir(bundle).filePath(QStringLiteral("Contents"));
    bool changed = false;
    const auto stamp = [&changed](bool ok) { if (ok) changed = true; };

    // 可执行拷贝（大小或 mtime 变了才重拷）
    QDir().mkpath(QDir(contents).filePath(QStringLiteral("MacOS")));
    const QString exeDst = QDir(contents).filePath(QStringLiteral("MacOS/mwine-window"));
    const QFileInfo srcFi(playerExe), dstFi(exeDst);
    if (!dstFi.isFile() || srcFi.size() != dstFi.size()
        || srcFi.lastModified() != dstFi.lastModified()) {
        QFile::remove(exeDst);
        if (!QFile::copy(playerExe, exeDst)) {
            QFile::setPermissions(exeDst, QFile::Permissions(
                QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup
                | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther));
            return QString();
        }
        QFile::setPermissions(exeDst, QFile::Permissions(
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup
            | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther));
        changed = true;
    }

    // Qt 依赖链接：播放器来自 Houttuynia.app 时，其 rpath/插件查找都指
    // bundle 内 Frameworks 与 PlugIns——包装 bundle 里放符号链接指回去。
    // （开发树版本链 Homebrew 绝对路径，两个目录不存在，跳过即可。）
    const QString exeDir = QFileInfo(playerExe).dir().absolutePath();
    for (const QString& sub : { QStringLiteral("Frameworks"), QStringLiteral("PlugIns") }) {
        const QString host =
            QFileInfo(QDir(exeDir).filePath(QStringLiteral("..") + QLatin1Char('/') + sub))
                .absoluteFilePath();
        if (!QFileInfo(host).isDir())
            continue;
        const QString link = QDir(contents).filePath(sub);
        const QFileInfo li(link);
        if (li.isSymLink()) {
            // 目标变了才重建
            if (QFileInfo(li.symLinkTarget()).absoluteFilePath()
                == QFileInfo(host).absoluteFilePath())
                continue;
            QFile::remove(link);
        } else if (li.exists()) {
            QDir(link).removeRecursively();
        }
        stamp(QFile::link(host, link));
    }

    // Info.plist（内容变化才重写）
    const QString plist = QDir(contents).filePath(QStringLiteral("Info.plist"));
    const QByteArray plistBody = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
        " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "    <key>CFBundleExecutable</key><string>mwine-window</string>\n"
        "    <key>CFBundleIdentifier</key><string>app.houttuynia.window.%1</string>\n"
        "    <key>CFBundleName</key><string>%2</string>\n"
        "    <key>CFBundleDisplayName</key><string>%2</string>\n"
        "    <key>CFBundleIconFile</key><string>AppIcon</string>\n"
        "    <key>CFBundlePackageType</key><string>APPL</string>\n"
        "    <key>CFBundleShortVersionString</key><string>1.0</string>\n"
        "    <key>CFBundleVersion</key><string>1</string>\n"
        "    <key>LSMinimumSystemVersion</key><string>14.0</string>\n"
        "    <key>NSHighResolutionCapable</key><true/>\n"
        "</dict>\n</plist>\n")
        .arg(package, mw::app::xmlEscape(shown))
        .toUtf8();
    {
        QFile pf(plist);
        QByteArray old;
        if (pf.open(QIODevice::ReadOnly))
            old = pf.readAll();
        if (old != plistBody) {
            if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                pf.write(plistBody);
                pf.close();
                changed = true;
            }
        }
    }

    // 图标：appcache PNG → icns（缺失或图标更新时生成，sips/iconutil 数百 ms）
    if (!icon.isEmpty() && QFileInfo(icon).isFile()) {
        const QString res = QDir(contents).filePath(QStringLiteral("Resources"));
        QDir().mkpath(res);
        const QString icns = QDir(res).filePath(QStringLiteral("AppIcon.icns"));
        const QFileInfo icnsFi(icns), iconFi(icon);
        if (!icnsFi.isFile() || icnsFi.lastModified() < iconFi.lastModified()) {
            QFile::remove(icns);
            if (mw::app::makeIcns(icon, icns).ok())
                changed = true;
        }
    }

    // scrcpy server 拷入播放器自查找位（干净机器兜底；~/.mwine/bin 不存在时生效）
    {
        const auto jar = mwine::findServerJar();
        if (jar.has_value() && !jar->isEmpty()) {
            const QString bin = QDir(contents).filePath(QStringLiteral("Resources/bin"));
            QDir().mkpath(bin);
            const QString dst = QDir(bin).filePath(QFileInfo(*jar).fileName());
            if (!QFileInfo(dst).isFile()) {
                if (QFile::copy(*jar, dst))
                    changed = true;
            }
        }
    }

    if (changed)
        mw::app::codesignBestEffort(bundle);
    return bundle;
}
#endif // Q_OS_MAC

mw::Result<bool> spawnWindowPlayer(const QString& package, const QString& serial,
                                   const QString& display, bool mirror, const QString& record,
                                   const QStringList& extra)
{
    const QString exe = resolveWindowPlayer();
    if (exe.isEmpty())
        return mw::err<bool>(QStringLiteral(
            "mwine-window 播放器未找到（构建后在 cpp/build/modules/window/，"
            "打包版位于 Houttuynia.app/Contents/MacOS/）"));
    // 应用名/图标（窗口标题 + 包装 bundle 用）；daemon 离线时退化包名
    WindowAppMeta meta;
    if (!package.isEmpty())
        meta = fetchAppMeta(package);
    const QString title = meta.label;

    QStringList args{ QStringLiteral("--serial"), serial };
    if (!package.isEmpty())
        args << QStringLiteral("--package") << package;
    if (!title.isEmpty())
        args << QStringLiteral("--title") << title;
    if (!meta.icon.isEmpty() && QFileInfo::exists(meta.icon))
        args << QStringLiteral("--icon") << meta.icon;
    if (!display.isEmpty())
        args << QStringLiteral("--display") << display;
    if (mirror)
        args << QStringLiteral("--mirror");
    if (!record.isEmpty())
        args << QStringLiteral("--record") << record;
    // daemon 注入的环境偏好 → 参数（open 拉起 bundle 不继承环境变量）
    if (qEnvironmentVariableIsSet("MWINE_BORDERLESS")
        && qEnvironmentVariable("MWINE_BORDERLESS") == QByteArrayLiteral("1"))
        args << QStringLiteral("--borderless");
    args << extra;

#ifdef Q_OS_MAC
    // 应用窗口：经每应用包装 .app 拉起（Dock 图标/名称正确；已在跑时 open
    // 只会激活既有窗口，等价于 mwine-window 的单实例唤醒）。诊断 mirror 裸拉。
    if (!mirror) {
        const QString bundle = makeWindowAppBundle(package, title, meta.icon, exe);
        if (!bundle.isEmpty()) {
            QProcess openProc;
            openProc.start(QStringLiteral("open"),
                           QStringList{ bundle, QStringLiteral("--args") } + args);
            if (openProc.waitForFinished(15000) && openProc.exitCode() == 0) {
                std::printf("✓ 独立窗口已启动（%s）\n",
                            qPrintable(title.isEmpty() ? package : title));
                return mw::Result<bool>::Ok();
            }
            std::fprintf(stderr, "⚠ 包装 bundle 拉起失败，回退裸进程：%s\n",
                         qPrintable(QString::fromUtf8(openProc.readAllStandardError()).trimmed()));
        }
    }
#endif
    if (!QProcess::startDetached(exe, args))
        return mw::err<bool>(QStringLiteral("启动 mwine-window 失败: %1").arg(exe));
    std::printf("✓ 独立窗口已启动（%s）\n", qPrintable(package.isEmpty() ? serial : package));
    return mw::Result<bool>::Ok();
}

// ===================== notify（通知桥） =====================

namespace {
constexpr quint16 kBridgeHostPort = 27186;
constexpr quint16 kBridgeDevicePort = 27185;
constexpr const char* kBridgeComponent = "com.mwine.bridge/.BridgeService";
constexpr const char* kBridgePackage = "com.mwine.bridge";
} // namespace

static bool listenerEnabled(const mw::adb::AdbClient* client, const QString& serial)
{
    const auto out = client->shellBlocking(
        serial, QStringLiteral("settings get secure enabled_notification_listeners"), 15000);
    return out && out.value().outStr().contains(QLatin1String("com.mwine.bridge"));
}

// 授权 NotificationListener 并强制重绑（触发 companion 的 onListenerConnected -> hello）。
// 首选 API 30+ 的 `cmd notification`（立即重绑）；老设备走 settings 双写触发观察者。
static mw::Result<bool> grantAndRebind(const mw::adb::AdbClient* client, const QString& serial)
{
    // Android 13 实测：包刚装完 PackageManager 未就绪时 allow_listener 返回成功但
    // 不生效（静默无效），需重试到 enabled_notification_listeners 真正出现为止。
    auto allowOnce = [&]() {
        const auto r = client->shellBlocking(
            serial, QStringLiteral("cmd notification allow_listener %1").arg(kBridgeComponent),
            15000);
        return r && r.value().ok();
    };
    if (allowOnce()) {
        // allow 对已授权组件是幂等操作、不会重绑；disallow -> allow 强制一次全新绑定
        client->shellBlocking(
            serial, QStringLiteral("cmd notification disallow_listener %1").arg(kBridgeComponent),
            15000);
        if (allowOnce()) {
            for (int attempt = 0; attempt < 6; ++attempt) {
                if (listenerEnabled(client, serial))
                    return mw::Result<bool>::Ok();
                if (attempt < 5) {
                    QThread::sleep(1);
                    allowOnce();
                }
            }
            return mw::err<bool>(
                QStringLiteral("listener 授权后未生效（enabled_notification_listeners 未包含 mwine）"));
        }
    }
    // 回退：settings 追加（保留其他 listener），空值->目标值两次写入触发观察者重绑
    QString cur;
    if (const auto r = client->shellBlocking(
            serial, QStringLiteral("settings get secure enabled_notification_listeners"), 15000))
        cur = r.value().outStr().trimmed();
    QStringList others;
    for (const QString& s : cur.split(QLatin1Char(':'), Qt::KeepEmptyParts)) {
        if (!s.isEmpty() && s != QLatin1String("null") && !s.contains(QLatin1String("com.mwine.bridge")))
            others.append(s);
    }
    QString target = others.join(QLatin1Char(':'));
    if (!target.isEmpty())
        target += QLatin1Char(':');
    target += QLatin1String(kBridgeComponent);
    auto put = [&](const QString& v) {
        return client->shellBlocking(
            serial,
            QStringLiteral("settings put secure enabled_notification_listeners '%1'").arg(v), 15000);
    };
    put(QString());
    if (auto r = put(target); !r)
        return mw::err<bool>(QStringLiteral("settings 授权失败: %1").arg(r.error()));
    if (listenerEnabled(client, serial))
        return mw::Result<bool>::Ok();
    return mw::err<bool>(
        QStringLiteral("listener 授权未生效；可在设备 设置->通知访问权 中手动开启 com.mwine.bridge"));
}

static mw::Result<bool> handleBridgeConnection(QTcpSocket* sock,
                                               QHash<QString, qint64>* seen)
{
    // 行流读取：companion 逐条发 JSON 行
    QByteArray buf;
    while (true) {
        if (sock->waitForReadyRead(30000)) {
            buf += sock->readAll();
        } else if (sock->state() != QAbstractSocket::ConnectedState) {
            break;
        }
        int nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
            const QByteArray line = buf.left(nl).trimmed();
            buf.remove(0, nl + 1);
            if (line.isEmpty())
                continue;
            const auto v = QJsonDocument::fromJson(line).object();
            if (v.isEmpty())
                continue;
            if (v.value(QStringLiteral("event")).toString() == QStringLiteral("hello"))
                continue; // 连接成功时已打印
            const QString title = v.value(QStringLiteral("title")).toString(QStringLiteral("mwine"));
            const QString pkg = v.value(QStringLiteral("pkg")).toString();
            const QString text = v.value(QStringLiteral("text")).toString();
            const QString key = v.value(QStringLiteral("key")).toString();
            const qint64 postTime = static_cast<qint64>(
                v.value(QStringLiteral("postTime")).toDouble());
            // 去重：同一通知的 (key, postTime) 只放行一次
            //（本循环单线程读取，无需加锁）
            {
                if (seen->size() > 4096)
                    seen->clear();
                const auto it = seen->constFind(key);
                if (it != seen->constEnd() && it.value() == postTime)
                    continue;
                seen->insert(key, postTime);
            }
            std::printf("[通知] %s: %s - %s\n", qPrintable(pkg), qPrintable(title),
                        qPrintable(text));
            postMacosNotification(title, pkg, text);
        }
        if (sock->state() != QAbstractSocket::ConnectedState)
            break;
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdNotify(Args& args)
{
    const std::optional<QString> serialOpt = args.opt("serial");
    const bool setupOnly = args.has("setup-only");
    auto client = adbClient();
    client.ensureServer();
    const auto serialR = resolveSerial(serialOpt);
    if (!serialR)
        return mw::err<bool>(serialR.error());
    const QString serial = serialR.value();
    const auto apk = mw::app::findCompanionApk();
    if (!apk)
        return mw::err<bool>(
            QStringLiteral("companion APK 未找到：先运行 scripts/build_companion.sh，或设置 MWINE_BRIDGE_APK"));

    // 安装（已装则 -r 覆盖，保持幂等）
    std::printf("安装通知桥 APK（%s）...\n", qPrintable(*apk));
    if (auto r = client.installApk(serial, *apk, 120000); !r)
        return mw::err<bool>(r.error());
    std::printf("✓ 已安装 com.mwine.bridge\n");

    // Android 13+：POST_NOTIFICATIONS 是运行时权限且默认拒绝
    if (const auto n = client.grantRuntimePermissions(serial, QString::fromLatin1(kBridgePackage));
        n && n.value() > 0)
        std::printf("✓ 已授予 %d 项运行时权限（含 POST_NOTIFICATIONS）\n", n.value());
    // 包刚装完 PackageManager 可能未就绪，listener 授权前稍等
    QThread::sleep(1);

    // 授权 + 强制重绑（重装后 companion 进程被杀，重绑保证 server 端口起好）
    if (auto r = grantAndRebind(&client, serial); !r)
        return mw::err<bool>(r.error());
    std::printf("✓ listener 已授权并重绑\n");

    // forward 模式（与 scrcpy 同向，不依赖 adb reverse）：
    // companion 在设备 127.0.0.1:BRIDGE_DEVICE_PORT 起 TCP server，宿主连接本机转发端口读取
    client.removeReverse(serial, kBridgeHostPort);
    if (auto r = client.forwardTcp(serial, kBridgeHostPort, kBridgeDevicePort); !r)
        return mw::err<bool>(r.error());
    std::printf("✓ forward 已建立（宿主 :%d -> 设备 :%d）\n", kBridgeHostPort, kBridgeDevicePort);

    if (setupOnly) {
        std::printf("✓ 安装完成。运行 `mwine notify` 开始监听\n");
        return mw::Result<bool>::Ok();
    }

    QHash<QString, qint64> seen;
    std::printf("监听中（Ctrl-C 退出）...\n");
    while (true) {
        QTcpSocket sock;
        sock.connectToHost(QStringLiteral("127.0.0.1"), kBridgeHostPort);
        if (sock.waitForConnected(5000)) {
            std::printf("✓ 通知桥已连接（com.mwine.bridge）\n");
            handleBridgeConnection(&sock, &seen);
            std::printf("～ 连接断开，2s 后重连 ...\n");
        } else {
            std::fprintf(stderr, "[mwine] 连接失败，重建 forward 后 2s 重试 ...\n");
            // adb server 重启等场景 forward 会丢，重建后重试
            client.removeReverse(serial, kBridgeHostPort);
            client.forwardTcp(serial, kBridgeHostPort, kBridgeDevicePort);
        }
        QThread::sleep(2);
    }
}

// ===================== bundle 模式 =====================

std::optional<int> maybeBundledMain()
{
    const auto cfg = mw::app::readBundledConfig();
    if (!cfg)
        return std::nullopt;

    // 日志：~/Library/Logs/mwine-apps/<app名>.log（stdout/stderr 重定向）
    const QString exe = QCoreApplication::applicationFilePath();
    const QString name = QFileInfo(exe).completeBaseName();
    const QString dir = QDir(mw::homeDir()).filePath(QStringLiteral("Library/Logs/mwine-apps"));
    QDir().mkpath(dir);
    const int fd = ::open(QFile::encodeName(QDir(dir).filePath(name + QStringLiteral(".log"))).constData(),
                          O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ::dup2(fd, 1);
        ::dup2(fd, 2);
    }
    postMacosNotification(
        QStringLiteral("mwine"), QString(),
        QStringLiteral("正在启动 %1…").arg(cfg->title ? *cfg->title : name));
    std::printf("[bundle] 启动: %s\n", qPrintable(cfg->package));

    // 壳启动时读 appmodes：window 模式的应用直接走直通窗口
    //（mwine-window 已落地：虚拟显示器 + scrcpy 直通 + 每应用 Dock 图标包装）。
    // 无在线设备时先经 ensureInstallTarget 拉起 VM（与 `mwine run --window` 同链）。
    if (mw::app::bundledAppWindowMode(cfg->package)) {
        mw::adb::AdbClient client;
        const auto serialR = ensureInstallTarget(
            &client, [](const QString& line) { std::printf("%s\n", qPrintable(line)); });
        if (serialR.ok()) {
            auto r = spawnWindowPlayer(cfg->package, serialR.value(), cfg->display,
                                       false, QString());
            return r ? 0 : 1;
        }
        std::fprintf(stderr, "⚠ 直通窗口启动失败（%s），回落主屏模式\n",
                     qPrintable(serialR.error()));
    }
    return runEmulatorMode(cfg->package, std::nullopt, cfg->avd, cfg->display) ? 0 : 1;
}

} // namespace mwine
