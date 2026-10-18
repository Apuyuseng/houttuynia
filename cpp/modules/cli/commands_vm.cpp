// vm / doctor / devices / kill / image 子命令。
// ← crates/mwine-cli/src/main.rs：print_doctor / cmd_vm_list / cmd_vm_start /
//    cmd_vm_stop / cmd_vm_reset_id / cmd_devices / cmd_kill /
//    cmd_image_import / cmd_image_official_list / cmd_image_official_pull
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>

#include "adb.hpp"
#include "app.hpp"
#include "args.hpp"
#include "cli_common.hpp"
#include "config.hpp"
#include "commands.hpp"
#include "fs.hpp"
#include "process.hpp"
#include "supervisor.hpp"

namespace mwine {

// ===================== doctor =====================

mw::Result<bool> cmdDoctor(Args& args)
{
    if (args.take())
        return badUsage<bool>(QStringLiteral("doctor 不接受参数"));
    std::printf("mwine doctor\n");
    for (const auto& item : mw::supervisor::doctor()) {
        std::printf("  [%s] %s\n        %s\n", item.ok ? "✓" : "✗", qPrintable(item.name),
                    qPrintable(item.detail));
    }
    const auto jar = findServerJar();
    std::printf("  [%s] scrcpy-server\n        %s\n", jar ? "✓" : "✗",
                qPrintable(jar ? *jar : QStringLiteral("未找到（brew install scrcpy）")));
    const auto companion = mw::app::findCompanionApk();
    std::printf("  [%s] 通知桥 companion APK\n        %s\n", companion ? "✓" : "✗",
                qPrintable(companion ? *companion
                                     : QStringLiteral("未找到（scripts/build_companion.sh）")));
    std::optional<QString> aapt2;
    if (const auto sdk = mw::supervisor::Sdk::detect())
        aapt2 = sdk.value().findBuildTool(QStringLiteral("aapt2"));
    std::printf("  [%s] aapt2（.app 图标提取）\n        %s\n", aapt2 && !aapt2->isEmpty() ? "✓" : "✗",
                qPrintable(aapt2 && !aapt2->isEmpty() ? *aapt2
                                                      : QStringLiteral("未找到（SDK build-tools）")));
    const auto devs = adbClient().devices();
    if (devs && !devs.value().isEmpty()) {
        std::printf("  [✓] adb 设备\n");
        for (const auto& d : devs.value())
            std::printf("        %s (%s)\n", qPrintable(d.serial), qPrintable(d.state));
    } else if (devs) {
        std::printf("  [-] adb 设备\n        无在线设备\n");
    } else {
        std::printf("  [✗] adb server 未运行\n");
    }
    return mw::Result<bool>::Ok();
}

// ===================== vm =====================

static mw::Result<bool> vmList(Args& args)
{
    Q_UNUSED(args);
    const auto sdkR = mw::supervisor::Sdk::detect();
    if (!sdkR)
        return mw::err<bool>(QStringLiteral("Android SDK 未找到: %1").arg(sdkR.error()));
    const auto sdk = sdkR.value();
    QStringList running;
    if (const auto devs = adbClient().devices())
        for (const auto& d : devs.value())
            if (d.state == QStringLiteral("device"))
                running.append(d.serial);
    std::printf("%-24s %-10s 状态\n", "AVD", "ABI");
    for (const auto& avd : sdk.listAvds()) {
        const QString underscore = QString(avd.name).replace(QLatin1Char('.'), QLatin1Char('_'));
        QString state;
        for (const QString& s : running)
            if (s.contains(underscore))
                state = QStringLiteral("运行中*");
        std::printf("%-24s %-10s %s\n", qPrintable(avd.name),
                    qPrintable(avd.abi().isEmpty() ? QStringLiteral("?") : avd.abi()),
                    qPrintable(state));
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> vmStart(Args& args)
{
    const auto avd = args.take();
    if (!avd)
        return badUsage<bool>(QStringLiteral(
            "Usage: mwine vm start <avd> [--image <dir>] [--mode phone|tablet] [--snapshot <s>] "
            "[--cold] [--memory <MB>] [--proxy <url>] [--data-dir <dir>] [-- extra args]"));
    const std::optional<QString> image = args.opt("image");
    const std::optional<QString> mode = args.opt("mode");
    const std::optional<QString> snapshot = args.opt("snapshot");
    const bool cold = args.has("cold");
    const std::optional<QString> memoryStr = args.opt("memory");
    const std::optional<QString> proxy = args.opt("proxy");
    const std::optional<QString> dataDir = args.opt("data-dir");
    const QStringList extra = args.rest(); // clap last=true：-- 之后的原始 emulator 参数

    std::optional<mw::supervisor::DeviceMode> deviceMode;
    if (mode) {
        if (*mode == QStringLiteral("phone"))
            deviceMode = mw::supervisor::DeviceMode::Phone;
        else if (*mode == QStringLiteral("tablet"))
            deviceMode = mw::supervisor::DeviceMode::Tablet;
        else
            return mw::err<bool>(
                QStringLiteral("未知设备模式 \"%1\"；支持 phone / tablet").arg(*mode));
    }
    std::optional<quint32> memory;
    if (memoryStr) {
        bool ok = false;
        const quint32 m = memoryStr->toUInt(&ok);
        if (!ok)
            return mw::err<bool>(QStringLiteral("无效的 --memory 值: %1").arg(*memoryStr));
        memory = m;
    }

    const auto sdkR = mw::supervisor::Sdk::detect();
    if (!sdkR)
        return mw::err<bool>(QStringLiteral("Android SDK 未找到: %1").arg(sdkR.error()));
    const auto sdk = sdkR.value();
    auto client = adbClient();
    if (auto es = client.ensureServer(sdk.adbBin); !es)
        return mw::err<bool>(es.error());
    if (image)
        std::printf("启动自建镜像 %s（headless，%s）...\n", qPrintable(*avd), qPrintable(*image));
    else
        std::printf("启动 %s（headless）...\n", qPrintable(*avd));

    mw::supervisor::LaunchOptions opts;
    opts.extraArgs = extra;
    opts.imageDir = image;
    opts.snapshot = snapshot;
    opts.noSnapshot = cold;
    opts.deviceMode = deviceMode;
    opts.proxy = proxy;
    opts.dataDir = dataDir;
    // 与 daemon 启动路径保持同一 qemu 参数集：wifiSsid / windowed+隐藏 /
    // 内存覆盖。任一漂移都会改变快照设备指纹 → 两种入口混用时快照加载失败
    // → 每次冷启动（slirp 报 Error -22 即此症状）
    opts.wifiSsid = mw::supervisor::ensureWifiSsid();
    opts.windowed = true;
    opts.hideWindow = true; // CLI 语义 = 无头视觉（有窗但隐藏，帧尺寸正确性同 daemon）
    {
        const mw::config::VmConfig cfg = mw::config::VmConfig::load();
        opts.memoryMb = memory > 0 ? memory : (cfg.memoryMb > 0 ? cfg.memoryMb : 0);
    }
    auto handleR = mw::supervisor::EmulatorHandle::launchHeadless(sdk, *avd, opts, &client);
    if (!handleR)
        return mw::err<bool>(QStringLiteral("启动失败: %1").arg(handleR.error()));
    auto handle = std::move(handleR.value());
    std::printf("serial: %s，等待引导完成...\n", qPrintable(handle.serial()));
    if (auto b = handle.waitBootCompleted(&client, 300000); !b)
        return mw::err<bool>(QStringLiteral("引导超时: %1").arg(b.error()));

    // 快照加载结果核对：emulator 把加载失败原因写进 ~/.mwine/logs/emu-<avd>.log
    //（配置不匹配时静默退回冷启动，不查日志用户无从得知）。
    // 注意只核对 mwine_default：emulator 对任何 AVD 都会自动尝试 default_boot，
    // 首次启动的失败属于正常现象，不能误报。
    if (!cold) {
        const QString log = QDir(mw::homeDir()).filePath(
            QStringLiteral(".mwine/logs/emu-%1.log").arg(*avd));
        const qint64 sinceMs = QFileInfo(log).lastModified().toMSecsSinceEpoch();
        const qint64 launchedAtMs = QDateTime::currentMSecsSinceEpoch();
        if (launchedAtMs - sinceMs < 120 * 1000) {
            QFile f(log);
            if (f.open(QIODevice::ReadOnly)) {
                const QString body = QString::fromUtf8(f.readAll());
                if (body.contains(QStringLiteral("Failed to load snapshot 'mwine_default'"))) {
                    std::printf("⚠ 快照 mwine_default 加载失败（配置与存档时不一致），本次为冷启动\n");
                    std::printf("  恢复方法：mwine vm stop（会以当前配置重存 mwine_default）后再次启动\n");
                } else if (body.contains(
                               QStringLiteral("Successfully loaded snapshot 'mwine_default'"))) {
                    std::printf("✓ 快照恢复启动\n");
                }
            }
        }
    }
    // 自建镜像的 qemu 管道 adb 可能失联（TP1A 世代已知问题），
    // 就绪后立即确保可用通道：优先管道，必要时挂 TCP 兜底（console redir）
    QString controlSerial = handle.serial();
    const auto chan = mw::supervisor::ensureControlChannel(&client, handle.serial(), &controlSerial);
    if (chan && controlSerial == handle.serial())
        std::printf("✓ %s 已就绪（%s）\n", qPrintable(*avd), qPrintable(handle.serial()));
    else if (chan)
        std::printf("✓ %s 已就绪（adb 通道：%s，管道失联已自动切换 TCP）\n", qPrintable(*avd),
                    qPrintable(controlSerial));
    else
        std::printf("✓ %s 已就绪（%s）⚠ 备用通道建立失败：%s\n", qPrintable(*avd),
                    qPrintable(handle.serial()), qPrintable(chan.error()));
    // 设备模式：运行时 wm size/density（resizable 镜像的显示权威源）
    if (deviceMode) {
        if (mw::supervisor::applyDeviceModeRuntime(&client, controlSerial, *deviceMode)) {
            const auto lcd = mw::supervisor::lcdSpec(*deviceMode);
            std::printf("✓ 设备模式：%s（%dx%d@%d）\n",
                        mw::supervisor::deviceName(*deviceMode), lcd.w, lcd.h, lcd.dpi);
        } else {
            std::printf("⚠ 设备模式设置失败\n");
        }
    }
    recordLastAvd(*avd);
    // detach：父进程退出不影响 emulator（emulator 自身为独立进程组）
    new mw::supervisor::EmulatorHandle(std::move(handle));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> vmStop(Args& args)
{
    const auto serial = args.take();
    if (!serial)
        return badUsage<bool>(QStringLiteral("Usage: mwine vm stop <serial>"));
    // emu kill 不会自动回存 -snapshot 指定的档：关停前显式存档到 mwine_default，
    // 下次 vm start 即可 5s 内恢复到当前状态
    if (auto r = mw::supervisor::saveSnapshotBlocking(*serial, QStringLiteral("mwine_default"), 20000); r.ok())
        std::printf("✓ 状态已存档（mwine_default）\n");
    else
        std::printf("⚠ 存档失败（%s），直接关停\n", qPrintable(r.error()));
    if (auto r = mw::supervisor::emuConsoleKill(*serial); !r)
        return mw::err<bool>(QStringLiteral("emu kill 失败: %1").arg(r.error()));
    std::printf("✓ 已向 %s 发送关停指令\n", qPrintable(*serial));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> vmResetId(Args& args)
{
    const std::optional<QString> serialOpt = args.opt("serial");
    const bool deep = args.has("deep");
    auto client = adbClient();
    client.ensureServer();
    const auto serialR = resolveSerial(serialOpt);
    if (!serialR)
        return mw::err<bool>(serialR.error());
    const QString serial = serialR.value();

    QString oldAndroidId;
    if (const auto o = client.shellBlocking(serial,
                                             QStringLiteral("settings get secure android_id"), 5000))
        oldAndroidId = o.value().outStr().trimmed();
    std::printf("当前 android_id: %s\n", qPrintable(oldAndroidId));

    // 16 位十六进制新 ID（标准格式）+ UUID v4 形态广告 ID（xorshift64 生成）
    const quint64 t = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
    quint64 x = (t ^ (static_cast<quint64>(::getpid()) << 32)) | 1ull;
    QString hex;
    for (int i = 0; i < 4; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        hex += QString::number(x, 16).rightJustified(16, QLatin1Char('0'));
    }
    const QString newAndroidId = hex.left(16);
    const QString newAdvertisingId = QStringLiteral("%1-%2-%3-%4-%5")
                                         .arg(hex.mid(0, 8), hex.mid(8, 4), hex.mid(12, 4),
                                              hex.mid(16, 4), hex.mid(20, 12));

    int changed = 0;
    const QList<QPair<QString, QString>> kv = {
        { QStringLiteral("android_id"), newAndroidId },
        // 广告 ID：UAID 是绝大多数广告 SDK 的主标识
        { QStringLiteral("advertising_id"), newAdvertisingId },
    };
    for (const auto& [key, val] : kv) {
        const QString cmd = QStringLiteral("settings put secure %1 %2").arg(key, val);
        if (client.shellBlocking(serial, cmd, 5000))
            changed += 1;
        else
            std::printf("⚠ 写入 %s 失败\n", qPrintable(key));
    }

    if (deep) {
        // 深度清理：应用缓存里的设备指纹（需要应用未运行；清的是系统广告ID缓存与剪贴板）
        for (const char* cmd : { "settings put secure limit_ad_tracking 1",
                                 "pm trim-caches 999999999999" }) {
            client.shellBlocking(serial, QString::fromLatin1(cmd), 10000);
        }
        std::printf("✓ 深度清理：广告跟踪已限制 + 应用缓存已裁剪\n");
    }
    std::printf("✓ 新 android_id: %s（%d/2 项已写入，即时生效）\n", qPrintable(newAndroidId),
                changed);
    if (oldAndroidId == newAndroidId)
        std::printf("⚠ 新旧 ID 相同（极小概率），可重跑一次\n");
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdVm(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(QStringLiteral(
            "Usage: mwine vm <list|start|stop|reset-id> […]\n  list             列出本机 AVD\n  "
            "start <avd>      headless 启动并等待引导\n  stop <serial>    存档快照后关停\n  "
            "reset-id         随机化 android_id / 广告 ID"));
    if (*action == QStringLiteral("list"))
        return vmList(args);
    if (*action == QStringLiteral("start"))
        return vmStart(args);
    if (*action == QStringLiteral("stop"))
        return vmStop(args);
    if (*action == QStringLiteral("reset-id"))
        return vmResetId(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

// ===================== devices =====================

mw::Result<bool> cmdDevices(Args& args)
{
    Q_UNUSED(args);
    const auto devs = adbClient().devices();
    if (!devs)
        return mw::err<bool>(QStringLiteral("adb server 未运行: %1").arg(devs.error()));
    for (const auto& d : devs.value())
        std::printf("%s  %s\n", qPrintable(d.serial), qPrintable(d.state));
    return mw::Result<bool>::Ok();
}

// ===================== kill =====================

mw::Result<bool> cmdKill(Args& args)
{
    const bool vm = args.has("vm");
    const QString wineApps = QDir(mw::homeDir()).filePath(QStringLiteral("Applications/Mwine"));
    // 本二进制的仓库根（cpp/CMakeLists.txt + app 同时存在）
    std::optional<QString> repoRoot;
    {
        QString dir = QCoreApplication::applicationDirPath();
        while (true) {
            const QFileInfo fiDir(dir);
            if (!fiDir.exists())
                break;
            if (QFileInfo::exists(dir + QStringLiteral("/cpp/CMakeLists.txt"))
                && QFileInfo::exists(dir + QStringLiteral("/app"))) {
                repoRoot = dir;
                break;
            }
            const QString parent = fiDir.dir().absolutePath();
            if (parent == dir)
                break;
            dir = parent;
        }
    }
    const auto myPid = static_cast<pid_t>(QCoreApplication::applicationPid());
    int killed = 0;
    // 两个专属路径模式；命中后仍用 ps comm= 校验完整可执行路径
    for (const char* pat :
         { "cpp/build/modules/cli/mwine", "mwine-window", "Applications/Mwine/" }) {
        const auto out = mw::runProcess(QStringLiteral("pgrep"),
                                        { QStringLiteral("-f"), QString::fromLatin1(pat) },
                                        10000);
        if (!out.ok())
            continue;
        const auto pids = QString::fromUtf8(out.stdOut).split(QRegularExpression(QStringLiteral("\\s+")),
                                                              Qt::SkipEmptyParts);
        for (const QString& p : pids) {
            bool ok = false;
            const int pid = p.toInt(&ok);
            if (!ok || pid == myPid)
                continue;
            const auto comm = mw::runProcess(
                QStringLiteral("ps"),
                { QStringLiteral("-p"), QString::number(pid), QStringLiteral("-o"),
                  QStringLiteral("comm=") },
                10000);
            if (!comm.ok())
                continue;
            const QString exe = comm.stdOutStr().trimmed();
            const bool isMwine = exe.startsWith(wineApps)
                                 || (repoRoot && exe.startsWith(*repoRoot));
            if (!isMwine)
                continue;
            ::kill(pid, SIGTERM);
            std::printf("  已停止 %d（%s）\n", pid, qPrintable(exe));
            killed += 1;
        }
    }
    std::printf("✓ 已停止 %d 个 mwine 进程\n", killed);
    if (vm) {
        auto client = adbClient();
        if (const auto devs = client.devices()) {
            for (const auto& d : devs.value()) {
                if (d.serial.startsWith(QStringLiteral("emulator-"))) {
                    mw::supervisor::emuConsoleKill(d.serial);
                    std::printf("✓ 已关停虚拟机 %s\n", qPrintable(d.serial));
                }
            }
        }
    }
    return mw::Result<bool>::Ok();
}

// ===================== image =====================

static void printNextStep(const mw::supervisor::Sdk& sdk, const QString& dest)
{
    Q_UNUSED(sdk);
    std::printf("\n下一步：把 AVD 指向新镜像（image.sysdir.1 = …）或\n");
    std::printf("      mwine vm start <avd> --image %s\n", qPrintable(dest));
}

static mw::Result<bool> imageImport(Args& args)
{
    const auto path = args.take();
    if (!path)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine image import <tar.zst> [--sha256 <hex>] [--name <dir>]"));
    const std::optional<QString> sha256 = args.opt("sha256");
    QString name = QStringLiteral("houttuynia");
    if (const auto n = args.opt("name"))
        name = *n;
    const auto sdkR = mw::supervisor::Sdk::detect();
    if (!sdkR)
        return mw::err<bool>(QStringLiteral("未找到 Android SDK: %1").arg(sdkR.error()));
    static std::atomic_bool never{ false };
    QString dest;
    const auto r = mw::supervisor::importImage(
        *path, sha256, name, sdkR.value(),
        [](const QString& l) { std::printf("%s\n", qPrintable(l)); }, never,
        [](const QString& phase, quint64 done, quint64 total) {
            if (total > 0)
                std::printf("  [%s] %llu / %llu MiB\n", qPrintable(phase), done >> 20, total >> 20);
        },
        &dest);
    if (!r)
        return mw::err<bool>(QStringLiteral("镜像导入失败: %1").arg(r.error()));
    printNextStep(sdkR.value(), dest);
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> imageOfficialList(Args& args)
{
    const std::optional<QString> filter = args.opt("filter");
    const std::optional<QString> proxy = args.opt("proxy");
    const auto list = mw::supervisor::officialList(filter.value_or(QString()), proxy);
    if (!list)
        return mw::err<bool>(QStringLiteral(
                                 "拉取官方仓库清单失败（dl.google.com 不可达时请配置 https_proxy）: %1")
                                 .arg(list.error()));
    if (list.value().isEmpty()) {
        std::printf("没有匹配的官方镜像\n");
        return mw::Result<bool>::Ok();
    }
    const auto& v = list.value();
    for (int i = 0; i < qMin(v.size(), 40); ++i)
        std::printf("%-52s %7llu MiB\n", qPrintable(v[i].path), v[i].size / 1048576ull);
    if (v.size() > 40)
        std::printf("… 共 %d 项（用 --filter 过滤）\n", int(v.size()));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> imageOfficialPull(Args& args)
{
    const auto target = args.take();
    if (!target)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine image official-pull <target> [--proxy <url>]"));
    const std::optional<QString> proxy = args.opt("proxy");
    const auto sdkR = mw::supervisor::Sdk::detect();
    if (!sdkR)
        return mw::err<bool>(QStringLiteral("未找到 Android SDK: %1").arg(sdkR.error()));
    static std::atomic_bool never{ false };
    QString dest;
    const auto r = mw::supervisor::officialPull(
        *target, sdkR.value(), proxy,
        [](const QString& l) { std::printf("%s\n", qPrintable(l)); }, never,
        [](const QString& phase, quint64 done, quint64 total) {
            if (total > 0)
                std::printf("  [%s] %llu / %llu MiB\n", qPrintable(phase), done >> 20, total >> 20);
        },
        &dest);
    if (!r)
        return mw::err<bool>(QStringLiteral("官方镜像拉取失败: %1").arg(r.error()));
    printNextStep(sdkR.value(), dest);
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdImage(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(QStringLiteral(
            "Usage: mwine image <import|official-list|official-pull> […]"));
    if (*action == QStringLiteral("import"))
        return imageImport(args);
    if (*action == QStringLiteral("official-list"))
        return imageOfficialList(args);
    if (*action == QStringLiteral("official-pull"))
        return imageOfficialPull(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

} // namespace mwine
