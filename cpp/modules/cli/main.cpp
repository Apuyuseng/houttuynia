// mwine CLI 入口：命令分发 + 跨命令共享工具。
// ← crates/mwine-cli/src/main.rs 的 main()/adb()/resolve_serial()/
//    pick_default_avd()/resolve_install_serial()/daemon_start_and_wait()/
//    ensure_install_target()/emu_cmd()/core_api()
//
// 子命令实现：commands_vm / commands_apps / commands_store /
//             commands_keys / commands_misc
//
// 并行转换中（桩）：mirror、run --window、debug displayfps/snap/setclip、
// keys watch 引擎体 —— 见各桩处注释。
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QThread>
#include <QUrl>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "adb.hpp"
#include "args.hpp"
#include "cli_common.hpp"
#include "commands.hpp"
#include "fs.hpp"
#include "net.hpp"
#include "process.hpp"
#include "supervisor.hpp"

namespace mwine {

mw::adb::AdbClient adbClient()
{
    return mw::adb::AdbClient();
}

QString mwineConfigDir()
{
    return QDir(mw::homeDir()).filePath(QStringLiteral(".config/mwine"));
}

void recordLastAvd(const QString& name)
{
    const QString dir = mwineConfigDir();
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("last-avd")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(name.toUtf8());
}

// 默认 AVD：MWINE_AVD > daemon 配置的当前镜像（~/.mwine/config.json 的 avd 字段，
// 与 daemon resolve_active_avd 第一梯队一致）> 上次使用过的 > 唯一 arm64 > 唯一。
std::optional<QString> pickDefaultAvd()
{
    if (const QString v = QString::fromLocal8Bit(qgetenv("MWINE_AVD")); !v.trimmed().isEmpty())
        return v;

    const auto sdkR = mw::supervisor::Sdk::detect();
    if (!sdkR)
        return std::nullopt;
    const auto avds = sdkR.value().listAvds();
    auto exists = [&](const QString& n) {
        return !n.isEmpty() && std::any_of(avds.cbegin(), avds.cend(),
                                           [&](const mw::supervisor::AvdInfo& a) {
                                               return a.name == n;
                                           });
    };

    // daemon 配置的当前镜像
    QFile cfg(QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/config.json")));
    if (cfg.open(QIODevice::ReadOnly)) {
        const auto v = QJsonDocument::fromJson(cfg.readAll()).object();
        const QString n = v.value(QStringLiteral("avd")).toString();
        if (exists(n))
            return n;
    }
    // 上次使用
    QFile last(QDir(mwineConfigDir()).filePath(QStringLiteral("last-avd")));
    if (last.open(QIODevice::ReadOnly)) {
        const QString l = QString::fromUtf8(last.readAll()).trimmed();
        if (exists(l))
            return l;
    }
    // 唯一 arm64 > 唯一
    QVector<mw::supervisor::AvdInfo> arm64;
    for (const auto& a : avds)
        if (a.isArm64())
            arm64.append(a);
    if (arm64.size() == 1)
        return arm64.first().name;
    if (avds.size() == 1)
        return avds.first().name;
    return std::nullopt;
}

mw::Result<QString> resolveSerial(const std::optional<QString>& explicitSerial)
{
    if (explicitSerial)
        return mw::Result<QString>::Ok(*explicitSerial);
    const auto dev = adbClient().findOnlineDevice();
    if (!dev)
        return mw::err<QString>(
            QStringLiteral("没有在线的 adb 设备；用 `mwine vm start <avd>` 启动一个"));
    return mw::Result<QString>::Ok(dev.value().serial);
}

QString shellQuote(const QString& s)
{
    return QStringLiteral("'%1'").arg(QString(s).replace(QLatin1Char('\''), QStringLiteral("'\\''")));
}

void postMacosNotification(const QString& title, const QString& subtitle, const QString& body)
{
    const QString script = QStringLiteral("display notification %1 with title %2 subtitle %3")
                               .arg(shellQuote(body), shellQuote(title), shellQuote(subtitle));
    QProcess p;
    p.start(QStringLiteral("osascript"), { QStringLiteral("-e"), script });
    p.waitForFinished(10000);
}

mw::Result<QString> emuCmd(const QString& serial, const QString& cmd)
{
    const auto resp = mw::supervisor::emuConsoleCmd(serial, cmd);
    if (!resp)
        return mw::err<QString>(QStringLiteral("控制台命令失败: %1").arg(cmd));
    const QString text = QString::fromUtf8(resp.value()).trimmed();
    if (text.contains(QLatin1String("KO")))
        return mw::err<QString>(QStringLiteral("模拟器拒绝 %1: %2").arg(cmd, text));
    return mw::Result<QString>::Ok(text);
}

std::optional<QString> findServerJar()
{
    // mwine_scrcpy::find_server_jar 候选路径（mw_scrcpy 模块落地后切换）
    const QString home = mw::homeDir();
    const QStringList candidates = {
        QDir(home).filePath(QStringLiteral(".mwine/bin/scrcpy-server-v4.1")),
        QDir(home).filePath(QStringLiteral(".mwine/bin/scrcpy-server-v4.0")),
        // bundle 内置位（打包版；干净机器没有 ~/.mwine/bin）
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../Resources/bin/scrcpy-server-v4.1")),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../Resources/bin/scrcpy-server-v4.0")),
        QStringLiteral("/opt/homebrew/share/scrcpy/scrcpy-server"),
        QStringLiteral("/opt/homebrew/opt/scrcpy/share/scrcpy/scrcpy-server"),
        QStringLiteral("/usr/local/share/scrcpy/scrcpy-server"),
    };
    for (const QString& p : candidates)
        if (QFileInfo::exists(p))
            return p;
    return std::nullopt;
}

// ---------- 安装目标解析（install / app create / run 共用） ----------

QStringList onlineEmulators(const mw::adb::AdbClient* client)
{
    QStringList out;
    const auto devs = client->devices();
    if (!devs)
        return out;
    for (const auto& d : devs.value())
        if (d.state == QStringLiteral("device") && d.serial.startsWith(QStringLiteral("emulator-")))
            out.append(d.serial);
    return out;
}

// daemon 上报的 VM serial（/api/status 的 vm.serial；daemon 不在线返回空）
QString daemonVmSerial()
{
    const auto r = mw::httpGet(QUrl(QStringLiteral("http://127.0.0.1:7788/api/status")), 3000);
    if (r.status != 200)
        return {};
    const auto v = QJsonDocument::fromJson(r.body).object();
    return v.value(QStringLiteral("vm")).toObject().value(QStringLiteral("serial")).toString();
}

mw::Result<std::optional<QString>> resolveInstallSerial(const mw::adb::AdbClient* client,
                                                         const std::optional<QString>& explicitSerial)
{
    if (explicitSerial)
        return mw::Result<std::optional<QString>>::Ok(*explicitSerial);
    const QStringList online = onlineEmulators(client);
    if (online.isEmpty())
        return mw::Result<std::optional<QString>>::Ok(std::nullopt);
    // 守护进程的 VM 优先（双击安装进的是"管理器的虚拟机"，不是碰巧排第一台的）
    const QString serial = daemonVmSerial();
    if (!serial.isEmpty() && online.contains(serial)) {
        if (online.size() > 1)
            std::printf("（多设备环境，目标：管理器虚拟机 %s）\n", qPrintable(serial));
        return mw::Result<std::optional<QString>>::Ok(serial);
    }
    if (online.size() == 1)
        return mw::Result<std::optional<QString>>::Ok(online.first());
    return mw::err<std::optional<QString>>(
        QStringLiteral("检测到多台设备在线：%1。请用 --serial 指定目标（或关掉其他模拟器）")
            .arg(online.join(QStringLiteral(", "))));
}

std::optional<QString> daemonStartAndWait(const mw::adb::AdbClient* client,
                                          const std::function<void(const QString&)>& vmLine)
{
    // 只在 daemon 活着时走这条路；GUI 没开就静默转本地兜底
    if (mw::httpGet(QUrl(QStringLiteral("http://127.0.0.1:7788/api/status")), 3000).status != 200)
        return std::nullopt;
    if (mw::httpPost(QUrl(QStringLiteral("http://127.0.0.1:7788/api/vm/start")), {}, {}, 30000)
            .status != 200)
        return std::nullopt;
    std::printf("(已请 Houttuynia 启动虚拟机，等待引导 ...)\n");
    vmLine(QStringLiteral("正在启动虚拟机…（冷启动最长约 2 分钟）"));
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 300 * 1000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        QThread::sleep(2);
        const QStringList online = onlineEmulators(client);
        if (!online.isEmpty()) {
            // daemon 上报的 serial 优先（它就是这次启动的归属者）
            const QString s = daemonVmSerial();
            if (!s.isEmpty() && online.contains(s))
                return s;
            return online.first();
        }
    }
    std::printf("～ daemon 虚拟机引导超时，转本地兜底\n");
    return std::nullopt;
}

mw::Result<QString> ensureInstallTarget(const mw::adb::AdbClient* client,
                                        const std::function<void(const QString&)>& vmLine)
{
    if (const auto s = resolveInstallSerial(client, std::nullopt); s && s.value())
        return mw::Result<QString>::Ok(*s.value());
    vmLine(QStringLiteral("正在启动虚拟机…"));
    const bool daemonAlive =
        mw::httpGet(QUrl(QStringLiteral("http://127.0.0.1:7788/api/status")), 3000).status == 200;
    if (const auto s = daemonStartAndWait(client, vmLine)) {
        vmLine(QStringLiteral("虚拟机已就绪"));
        return mw::Result<QString>::Ok(*s);
    }
    if (daemonAlive) {
        // 守护进程在线但未能拉起虚拟机：本地兜底会与守护竞争同一 AVD
        // （双 qemu 端口/锁冲突 → 半死设备，下一次打开也跟着卡）。等待重试
        // 交给用户侧（GUI 会在概览页显示 vm_error）
        return mw::err<QString>(
            QStringLiteral("守护进程未能启动虚拟机（可能在关停/启动过渡，请稍候重试；"
                           "详见 Houttuynia 概览页错误信息）"));
    }
    const auto avd = pickDefaultAvd();
    if (!avd)
        return mw::err<QString>(
            QStringLiteral("无在线设备且无法确定默认 AVD；请先打开 Houttuynia 启动虚拟机，或设置 MWINE_AVD"));
    const auto sdkR = mw::supervisor::Sdk::detect();
    if (!sdkR)
        return mw::err<QString>(QStringLiteral("Android SDK 未找到: %1").arg(sdkR.error()));
    const auto sdk = sdkR.value();
    std::printf("(无在线设备，本地启动 %s ...)\n", qPrintable(*avd));
    // AVD 实体可能在另一台 home（daemon 双 home 创建），按实际位置传给 emulator
    std::optional<QString> avdHome;
    for (const auto& a : sdk.listAvds()) {
        if (a.name == *avd) {
            const QString parent = QFileInfo(a.path).absolutePath();
            if (parent != sdk.avdHome())
                avdHome = parent;
            break;
        }
    }
    mw::supervisor::LaunchOptions opts;
    opts.avdHome = avdHome;
    opts.wifiSsid = mw::supervisor::ensureWifiSsid();
    auto handle = mw::supervisor::EmulatorHandle::launchHeadless(sdk, *avd, opts,
                                                                 const_cast<mw::adb::AdbClient*>(client));
    if (!handle)
        return mw::err<QString>(handle.error());
    std::printf("serial: %s，等待引导 ...\n", qPrintable(handle.value().serial()));
    vmLine(QStringLiteral("正在启动虚拟机…（冷启动最长约 2 分钟）"));
    if (auto b = handle.value().waitBootCompleted(const_cast<mw::adb::AdbClient*>(client), 300000); !b) {
        // 引导失败：关停刚拉起的实例再报错——留着就是半死孤儿（adb 上
        // 假在线，后续一切操作无响应；实测"第二次打开卡住"的元凶）
        std::printf("～ 本地虚拟机引导失败，正在关停实例 …\n");
        (void)handle.value().shutdown(const_cast<mw::adb::AdbClient*>(client));
        return mw::err<QString>(QStringLiteral("虚拟机引导超时: %1").arg(b.error()));
    }
    recordLastAvd(*avd);
    vmLine(QStringLiteral("虚拟机已就绪"));
    const QString serial = handle.value().serial();
    // detach：父进程退出不影响 emulator（泄漏句柄 = Rust 侧 std::mem::forget）
    new mw::supervisor::EmulatorHandle(std::move(handle.value()));
    return mw::Result<QString>::Ok(serial);
}

// ---------- capture 经 daemon REST（Rust core_api） ----------

static int daemonPort()
{
    bool ok = false;
    const int p = QString::fromLocal8Bit(qgetenv("MWINE_PORT")).toInt(&ok);
    return (ok && p > 0 && p <= 65535) ? p : 7788;
}

mw::Result<QPair<int, QString>> coreApi(const QString& method, const QString& path,
                                        const QByteArray& body)
{
    const QUrl url(QStringLiteral("http://127.0.0.1:%1%2").arg(daemonPort()).arg(path));
    const mw::HttpReply r = body.isNull()
                                ? mw::httpRequest(method, url, {}, {}, 30000)
                                : mw::httpRequest(method, url, body,
                                                  { { QByteArrayLiteral("Content-Type"),
                                                      QByteArrayLiteral("application/json") } },
                                                  30000);
    if (r.status == 0)
        return mw::err<QPair<int, QString>>(
            QStringLiteral("无法连接 mwine-cored（%1）：%2").arg(daemonPort()).arg(r.error));
    return mw::Result<QPair<int, QString>>::Ok(qMakePair(r.status, QString::fromUtf8(r.body)));
}

bool stdoutIsTty()
{
    return ::isatty(1) == 1;
}

} // namespace mwine

// ===================== 帮助文案 =====================

namespace mwine {

void printVersion()
{
    std::printf("mwine 0.1.0\n");
}

void printMainHelp()
{
    std::printf(
        "在 macOS 上运行 Android 应用的工具链\n\n"
        "Usage: mwine <COMMAND>\n\n"
        "Commands:\n"
        "  doctor   环境体检：HVF / SDK / AVD / QEMU / scrcpy-server / adb\n"
        "  capture  流量分析（eBPF 明文旁挂，需 userdebug 镜像 + ecapture）\n"
        "  vm       虚拟机（AVD 实例）管理\n"
        "  devices  列出 adb 设备\n"
        "  device   真机连接：USB / 扫码配对 / 配对码 / 手动地址 / USB 转无线\n"
        "  install  安装 APK\n"
        "  apps     应用管理\n"
        "  files    文件互通：macOS <-> 设备传输（默认设备目录 /sdcard/Download）\n"
        "  kill     停止 mwine 窗口/桥接进程\n"
        "  monitor  后台资源监护：模拟器 CPU/内存异常时提醒/限流，闲置可自动关停\n"
        "  camera   摄像头注入：把本地图片喂给应用摄像头\n"
        "  keys     键盘→屏幕映射（游戏按键 DIY）\n"
        "  notify   通知桥：设备通知 -> macOS 通知中心\n"
        "  app      .app 壳 / APK 双击安装器\n"
        "  run      在独立 macOS 窗口中运行一个 Android 应用\n"
        "  mirror   启动虚拟显示器并获取视频流\n"
        "  image    镜像管理：导入云分发包 / 拉取 Google 官方镜像\n"
        "  debug    诊断工具：显示管线帧率 / 显示器快照 / 设备剪贴板\n"
        "  store    在线应用商店（豌豆荚 / F-Droid）\n\n"
        "Options:\n"
        "  -h, --help     Print help\n"
        "  -V, --version  Print version\n");
}

} // namespace mwine

// ===================== main：分发 =====================

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mwine"));

    const QStringList argvList = [](int c, char** v) {
        QStringList out;
        for (int i = 1; i < c; ++i)
            out.append(QString::fromLocal8Bit(v[i]));
        return out;
    }(argc, argv);

    using Handler = mw::Result<bool> (*)(mwine::Args&);
    struct CmdEntry {
        const char* name;
        Handler handler;
        const char* help; // 子命令级简短用法
    };
    // 与 Rust enum Command 一致的子命令表
    const CmdEntry kCommands[] = {
        { "doctor", mwine::cmdDoctor, "mwine doctor" },
        { "capture", mwine::cmdCapture, "mwine capture <start|stop|status|flows|show|clear> […]" },
        { "vm", mwine::cmdVm,
          "mwine vm <list|start|stop|reset-id> […]" },
        { "devices", mwine::cmdDevices, "mwine devices" },
        { "device", mwine::cmdDevice,
          "mwine device <list|discovered|pair|connect|disconnect|tcpip|forget|mirror> […]" },
        { "install", mwine::cmdInstall, "mwine install <apk> [--serial <s>] [--progress-file <f>]" },
        { "apps", mwine::cmdApps, "mwine apps <list|launch|mode|uninstall> […]" },
        { "files", mwine::cmdFiles, "mwine files <push|pull|ls> […]" },
        { "kill", mwine::cmdKill, "mwine kill [--vm]" },
        { "monitor", mwine::cmdMonitor, "mwine monitor [--cpu N] [--mem-mb N] […]" },
        { "camera", mwine::cmdCamera, "mwine camera <image|reset> […]" },
        { "keys", mwine::cmdKeys, "mwine keys <shot|get|set|watch|template|rm> […]" },
        { "notify", mwine::cmdNotify, "mwine notify [--serial <s>] [--setup-only]" },
        { "app", mwine::cmdApp, "mwine app <create|installer> […]" },
        { "run", mwine::cmdRun, "mwine run <package> [options]" },
        { "mirror", mwine::cmdMirror, "mwine mirror [options]" },
        { "image", mwine::cmdImage, "mwine image <import|official-list|official-pull> […]" },
        { "debug", mwine::cmdDebug, "mwine debug <displayfps|snap|emuhide|displays|setclip> […]" },
        { "store", mwine::cmdStore, "mwine store <search|versions|install> […]" },
    };

    // bundle 模式：本二进制作为 .app 的 CFBundleExecutable 被双击启动
    //（正规 bundle 进程：macOS 菜单栏/Dock 图标/Cmd+Tab 全部生效）。
    // 读 Contents/Resources/app.conf 得到要运行的应用，无视命令行参数。
    if (const auto r = mwine::maybeBundledMain(); r)
        return *r ? 0 : 1;

    if (argvList.isEmpty()) {
        mwine::printMainHelp();
        return 0;
    }
    if (argvList.first() == QStringLiteral("-h") || argvList.first() == QStringLiteral("--help")) {
        mwine::printMainHelp();
        return 0;
    }
    if (argvList.first() == QStringLiteral("-V") || argvList.first() == QStringLiteral("--version")) {
        mwine::printVersion();
        return 0;
    }

    const QString name = argvList.first();
    const CmdEntry* found = nullptr;
    for (const auto& c : kCommands)
        if (name == QLatin1String(c.name))
            found = &c;
    if (!found) {
        std::fprintf(stderr, "error: unrecognized subcommand '%s'\n\n", qPrintable(name));
        mwine::printMainHelp();
        return 2;
    }

    mwine::Args args(argvList.mid(1));
    const auto r = found->handler(args);
    if (!r) {
        // 用法错误对齐 clap（stderr "error: ..." + exit 2）；业务错误对齐 anyhow（"Error: ..." + exit 1）
        const QString e = r.error();
        if (e.startsWith(QStringLiteral("Usage:")) || e.contains(QStringLiteral("unrecognized subcommand"))) {
            std::fprintf(stderr, "error: %s\n\n%s\n\nFor more information, try '--help'.\n",
                         qPrintable(e), qPrintable(QString::fromUtf8(found->help)));
            return 2;
        }
        std::fprintf(stderr, "Error: %s\n", qPrintable(e));
        return 1;
    }
    return 0;
}
