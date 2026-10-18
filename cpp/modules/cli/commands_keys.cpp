// keys（键盘→屏幕映射）+ monitor（后台资源监护）子命令。
// ← crates/mwine-cli/src/main.rs：cmd_keys_shot/get/set/rm/template/watch /
//    cmd_monitor / install_monitor_agent / uninstall_monitor_agent /
//    ensure_single_monitor_instance / emulator_pid / proc_cputime / proc_rss_mb
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "adb.hpp"
#include "args.hpp"
#include "cli_common.hpp"
#include "commands.hpp"
#include "fs.hpp"
#include "input/console.hpp"
#include "input/engine.hpp"
#include "input/event_tap.hpp"
#include "input/input.hpp"
#include "keymap.hpp"
#include "process.hpp"
#include "supervisor.hpp"

namespace mwine {

static QString keysDir()
{
    const QString d = QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/keymaps"));
    QDir().mkpath(d);
    return d;
}

// ===================== keys =====================

static mw::Result<bool> keysShot(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(QStringLiteral("Usage: mwine keys shot <package> [--serial <s>]"));
    const auto serialR = resolveSerial(args.opt("serial"));
    if (!serialR)
        return mw::err<bool>(serialR.error());
    auto client = adbClient();
    const auto out = client.shellBlocking(serialR.value(), QStringLiteral("screencap -p"), 10000);
    if (!out)
        return mw::err<bool>(QStringLiteral("截图失败: %1").arg(out.error()));
    // shell v2 协议分离 stdout，无 tty 的 \n→\r\n 污染，PNG 直出；魔数校验
    const QByteArray& data = out.value().out;
    if (data.size() <= 8 || !data.startsWith(QByteArrayLiteral("\x89PNG")))
        return mw::err<bool>(QStringLiteral("非 PNG 输出"));
    const QString dir = QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/keyshots"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("%1.png").arg(*package));
    if (auto w = mw::writeFile(path, data); !w)
        return mw::err<bool>(w.error());
    std::printf("%s\n", qPrintable(path));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> keysGet(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(QStringLiteral("Usage: mwine keys get <package>"));
    const QString path = QDir(keysDir()).filePath(QStringLiteral("%1.json").arg(*package));
    if (QFileInfo::exists(path)) {
        const auto data = mw::readFile(path);
        if (!data)
            return mw::err<bool>(data.error());
        std::printf("%s\n", data.value().constData());
    } else {
        std::printf("[]\n");
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> keysSet(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(QStringLiteral("Usage: mwine keys set <package> [file]"));
    const std::optional<QString> file = args.take();
    QByteArray json;
    if (file) {
        const auto d = mw::readFile(*file);
        if (!d)
            return mw::err<bool>(d.error());
        json = d.value();
    } else {
        // stdin 全量读取
        char buf[65536];
        ssize_t n;
        while ((n = ::read(0, buf, sizeof(buf))) > 0)
            json.append(buf, int(n));
    }
    // 校验：v2 对象或 v1 数组均可
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError)
        return mw::err<bool>(QStringLiteral("JSON 格式错误: %1").arg(err.errorString()));
    if (!doc.isArray() && !doc.isObject())
        return mw::err<bool>(QStringLiteral("需为 v2 档案对象或 v1 绑定数组"));
    const QString path = QDir(keysDir()).filePath(QStringLiteral("%1.json").arg(*package));
    if (auto w = mw::writeFile(path, json); !w)
        return mw::err<bool>(w.error());
    int n = 0;
    if (doc.isArray())
        n = doc.array().size();
    else
        n = doc.object().value(QStringLiteral("nodes")).toArray().size();
    std::printf("✓ 已保存 %s 的键位映射（%d 项）\n", qPrintable(*package), n);
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> keysRm(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(QStringLiteral("Usage: mwine keys rm <package>"));
    const QString path = QDir(keysDir()).filePath(QStringLiteral("%1.json").arg(*package));
    if (!QFileInfo::exists(path))
        return mw::err<bool>(QStringLiteral("%1 没有键位档案").arg(*package));
    QFile::remove(path);
    QFile::remove(QDir(QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/keyshots")))
                      .filePath(QStringLiteral("%1.png").arg(*package)));
    std::printf("✓ 已删除 %s 的键位档案\n", qPrintable(*package));
    return mw::Result<bool>::Ok();
}

// 内置模板 → 键位档案文件（编辑器里可继续调整）。
static mw::Result<bool> keysTemplate(Args& args)
{
    const auto id = args.take();
    const auto package = args.take();
    if (!id || !package)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine keys template <honor_of_kings|peace_elite|blank> <package>"));
    auto profileR = mw::keymap::templateProfile(*id);
    if (!profileR)
        return mw::err<bool>(QStringLiteral("未知模板 %1（可选: %2）")
                                 .arg(*id,
                                      mw::keymap::templateIds().join(QStringLiteral(", "))));
    auto profile = profileR.value();
    profile.package = *package;
    const QString path = QDir(keysDir()).filePath(QStringLiteral("%1.json").arg(*package));
    const QByteArray json = QJsonDocument(profile.toJson()).toJson(QJsonDocument::Indented);
    if (auto w = mw::writeFile(path, json); !w)
        return mw::err<bool>(w.error());
    std::printf("✓ 已写入模板 %s（%s，%d 个节点）→ %s\n", qPrintable(mw::keymap::templateName(*id)),
                qPrintable(profile.name), int(profile.nodes.size()), qPrintable(path));
    std::printf("  在「按键映射」页微调各按钮位置后保存即可\n");
    return mw::Result<bool>::Ok();
}

// 键位映射运行时（前台）：全局键鼠捕获 → keymap 引擎 → console 多点触控。
static mw::Result<bool> keysWatch(Args& args)
{
    const auto package = args.take();
    if (!package)
        return badUsage<bool>(QStringLiteral("Usage: mwine keys watch <package>"));
    const QString keymap = QDir(keysDir()).filePath(QStringLiteral("%1.json").arg(*package));
    if (!QFileInfo::exists(keymap))
        return mw::err<bool>(QStringLiteral(
            "%1 没有键位映射：先在「按键映射」页编辑保存，或运行 `mwine keys template peace_elite "
            "%1`")
            .arg(*package, *package));

    // 单例：同一时间只允许一个 watch（UI/终端混用防双开互抢事件）
    const QString pidfile =
        QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/keys-watch.pid"));
    QFile pf(pidfile);
    if (pf.open(QIODevice::ReadOnly)) {
        const int old = QString::fromUtf8(pf.readAll()).trimmed().toInt();
        if (old > 0 && old != int(QCoreApplication::applicationPid()) && ::kill(old, 0) == 0) {
            std::printf("替换旧映射进程 %d…\n", old);
            ::kill(old, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                if (::kill(old, 0) != 0)
                    break;
                QThread::msleep(100);
            }
        }
    }
    {
        QFile wf(pidfile);
        if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            wf.write(QByteArray::number(QCoreApplication::applicationPid()));
    }

    // 1. 辅助功能授权（active tap 硬性要求）：未授权时弹引导并轮询 180s
    if (!mw::input::isProcessTrusted()) {
        std::printf("需要「辅助功能」授权：在系统设置中勾选 mwine 后自动继续（180s 超时）…\n");
        mw::input::requestTrustPrompt();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        while (!mw::input::isProcessTrusted()) {
            if (std::chrono::steady_clock::now() > deadline) {
                QFile::remove(pidfile);
                std::fprintf(stderr, "✗ 未获辅助功能授权，退出\n");
                ::_exit(4);
            }
            QThread::msleep(500);
        }
        std::printf("✓ 已获授权\n");
    }

    // 2. 解析档案（v1 数组按当前分辨率迁移为 v2）
    const auto rawR = mw::readFile(keymap);
    if (!rawR)
        return mw::err<bool>(rawR.error());
    auto client = adbClient();
    client.ensureServer();
    // 优先模拟器（本产品场景），没有再退回任意在线设备
    QString serial;
    if (const auto devs = client.devices()) {
        for (const auto& d : devs.value()) {
            if (d.state == QStringLiteral("device")
                && d.serial.startsWith(QStringLiteral("emulator-"))) {
                serial = d.serial;
                break;
            }
        }
    }
    if (serial.isEmpty()) {
        const auto s = resolveSerial(std::nullopt);
        if (!s)
            return mw::err<bool>(QStringLiteral("未找到运行中的 Android（先 `mwine vm start`）"));
        serial = s.value();
    }
    // 查询 Android 分辨率（wm size）——对应 mwine_input::console::query_display_size
    QPair<int, int> screen{ 1080, 2400 };
    if (const auto o = client.shellBlocking(serial, QStringLiteral("wm size"), 15000); o) {
        static const QRegularExpression re(
            QStringLiteral("Physical size:\\s*(\\d+)x(\\d+)"));
        const auto m = re.match(o.value().outStr());
        if (m.hasMatch())
            screen = qMakePair(m.captured(1).toInt(), m.captured(2).toInt());
    }
    const auto profileR = mw::keymap::Profile::parse(rawR.value(), screen);
    if (!profileR)
        return mw::err<bool>(QStringLiteral("键位档案解析失败: %1").arg(profileR.error()));
    auto profile = profileR.value();
    {
        const auto doc = QJsonDocument::fromJson(rawR.value());
        if (doc.isArray()) {
            mw::writeFile(keymap,
                          QJsonDocument(profile.toJson()).toJson(QJsonDocument::Indented));
            std::printf("✓ 旧版键位已自动迁移为 v2 并保存\n");
        }
    }

    // 3. 模拟器 console 通道（原生内核级多点触控，无 guest 进程）
    const QPair<quint16, quint16> screenU16 = qMakePair(quint16(screen.first), quint16(screen.second));
    auto injectorR = mw::input::ConsoleInjector::connectSerial(serial, screenU16);
    if (!injectorR)
        return mw::err<bool>(QStringLiteral("console 注入通道连接失败: %1").arg(injectorR.error()));
    auto sharedInjector = std::make_shared<mw::input::SharedInjector>(
        std::make_unique<mw::input::ConsoleInjector>(std::move(injectorR.value())));
    auto sink = std::make_shared<mw::input::EngineSink>(sharedInjector, 0);
    auto engine = mw::input::Engine::create(
        profile, screenU16, mw::input::SharedSink{ sink, std::make_shared<std::mutex>() });

    // 4. 引擎 + 全局键鼠捕获（active tap：命中绑定的事件被吞，宿主无感）
    auto runner = std::make_shared<mw::input::KeymapRunner>(engine);
    auto tapR = runner->spawnGlobal(true);
    if (!tapR)
        return mw::err<bool>(QStringLiteral("全局键鼠捕获启动失败: %1").arg(tapR.error()));
    auto tap = std::move(tapR.value());

    // 5. 前台门线程：目标应用不在前台时挂起映射（游戏对局页检测）
    static std::atomic_bool stop{ false };
    std::thread gate([client, serial, package = *package, runner]() mutable {
        while (!stop.load()) {
            const auto o = client.shellBlocking(
                serial,
                QStringLiteral(
                    "dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity"),
                3000);
            const bool ok = o && o.value().outStr().contains(package);
            runner->setActivityOk(ok);
            for (int i = 0; i < 10 && !stop.load(); ++i)
                QThread::msleep(100);
        }
    });

    const QString mode = profile.mouseMode == mw::keymap::MouseMode::Capture
                             ? QStringLiteral("FPS 鼠标捕获")
                             : QStringLiteral("自由鼠标");
    const QString displayName = profile.name.isEmpty() ? *package : profile.name;
    std::printf("按键映射已启用：%s（%s · %d 个节点 · %dx%d · console 内核注入）\n",
                qPrintable(displayName), qPrintable(mode), int(profile.nodes.size()),
                screen.first, screen.second);
    std::printf("  总开关：`%s` 键切换映射开/关；Ctrl-C 退出\n", qPrintable(profile.switchKey));

    // 6. 等待退出信号（pidfile 生命周期与 Rust 版一致）
    signal(SIGINT, [](int) { stop.store(true); });
    signal(SIGTERM, [](int) { stop.store(true); });
    while (!stop.load()) {
        QThread::msleep(200);
        QCoreApplication::processEvents(); // 供 QProcess/adb 心跳
    }
    stop.store(true);
    gate.join();
    runner->shutdown(); // 释放虚拟手指 + 恢复光标
    tap.stop();
    engine->shutdown();
    QFile::remove(pidfile);
    std::printf("按键映射已停止\n");
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdKeys(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine keys <shot|get|set|watch|template|rm> […]"));
    if (*action == QStringLiteral("shot"))
        return keysShot(args);
    if (*action == QStringLiteral("get"))
        return keysGet(args);
    if (*action == QStringLiteral("set"))
        return keysSet(args);
    if (*action == QStringLiteral("watch"))
        return keysWatch(args);
    if (*action == QStringLiteral("template"))
        return keysTemplate(args);
    if (*action == QStringLiteral("rm"))
        return keysRm(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

// ===================== monitor（后台资源监护） =====================

namespace {
std::atomic_bool g_monitorStop{ false };
void monitorSigint(int)
{
    // 信号安全：仅原子置位（SIGINT/SIGTERM 都置停止标志）
    g_monitorStop.store(true);
}
} // namespace

// 模拟器 qemu 进程 pid（精确匹配 Android SDK emulator 二进制路径，
// 绝不误伤 libvirt 等其他 qemu）
static std::optional<uint> emulatorPid()
{
    const auto out = mw::runProcess(
        QStringLiteral("pgrep"),
        { QStringLiteral("-f"),
          QStringLiteral("sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless") },
        10000);
    if (!out.ok())
        return std::nullopt;
    const auto pids = QString::fromUtf8(out.stdOut)
                          .split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString& p : pids) {
        bool ok = false;
        const uint pid = p.toUInt(&ok);
        if (ok)
            return pid;
    }
    return std::nullopt;
}

// 进程累计 CPU 时间（秒）
static std::optional<double> procCputime(uint pid)
{
    const auto out = mw::runProcess(
        QStringLiteral("ps"),
        { QStringLiteral("-p"), QString::number(pid), QStringLiteral("-o"),
          QStringLiteral("cputime=") },
        10000);
    if (!out.ok())
        return std::nullopt;
    const QString text = out.stdOutStr().trimmed();
    // 格式 "MM:SS.ss" 或 "HH:MM:SS.ss"
    QList<double> parts;
    for (const QString& p : text.split(QLatin1Char(':'))) {
        bool ok = false;
        const double v = p.trimmed().toDouble(&ok);
        if (ok)
            parts.append(v);
    }
    if (parts.size() == 2)
        return parts[0] * 60.0 + parts[1];
    if (parts.size() == 3)
        return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
    return std::nullopt;
}

static std::optional<double> procRssMb(uint pid)
{
    const auto out = mw::runProcess(
        QStringLiteral("ps"),
        { QStringLiteral("-p"), QString::number(pid), QStringLiteral("-o"), QStringLiteral("rss=") },
        10000);
    if (!out.ok())
        return std::nullopt;
    bool ok = false;
    const double kb = out.stdOutStr().trimmed().toDouble(&ok);
    if (!ok)
        return std::nullopt;
    return kb / 1024.0;
}

static int runningWindowCount()
{
    int n = 0;
    for (const char* pat :
         { "cpp/build/modules/cli/mwine run", "mwine-window", "Applications/Mwine/" }) {
        const auto out = mw::runProcess(
            QStringLiteral("pgrep"),
            { QStringLiteral("-f"), QString::fromLatin1(pat) }, 10000);
        if (out.ok())
            n += QString::fromUtf8(out.stdOut)
                     .split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)
                     .size();
    }
    return n;
}

static void monitorNotify(const QString& body)
{
    const QString b = QString(body).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    QProcess p;
    p.start(QStringLiteral("osascript"),
            { QStringLiteral("-e"),
              QStringLiteral("display notification \"%1\" with title \"mwine 资源监护\"").arg(b) });
    p.waitForFinished(10000);
}

// 单实例锁：已有一个存活的 monitor 时拒绝启动（LaunchAgent 与手动
// 双开会重复限流/重复通知）。锁文件持有进程退出自动失效（存 pid 校验）。
static mw::Result<bool> ensureSingleMonitorInstance()
{
    const QString lock = QDir::temp().filePath(QStringLiteral("mwine-monitor.lock"));
    QFile f(lock);
    if (f.open(QIODevice::ReadOnly)) {
        const uint pid = QString::fromUtf8(f.readAll()).trimmed().toUInt();
        if (pid != 0 && pid != QCoreApplication::applicationPid()) {
            const auto out = mw::runProcess(
                QStringLiteral("ps"),
                { QStringLiteral("-p"), QString::number(pid), QStringLiteral("-o"),
                  QStringLiteral("comm=") },
                10000);
            if (out.ok() && out.stdOutStr().trimmed().contains(QLatin1String("mwine")))
                return mw::err<bool>(QStringLiteral(
                    "monitor 已在运行（pid %1）；如需调整参数先结束旧实例或 mwine kill")
                    .arg(pid));
        }
    }
    QFile wf(lock);
    if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate))
        wf.write(QByteArray::number(QCoreApplication::applicationPid()));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> runMonitor(double cpuAlert, quint64 memAlertMb, double limitCpu,
                                   quint64 idleStopMin)
{
    if (auto r = ensureSingleMonitorInstance(); !r)
        return mw::err<bool>(r.error());
    std::printf("mwine 资源监护启动（Ctrl-C 退出）\n");
    std::printf("  提醒阈值：CPU %.0f%% / 内存 %lluMB；限流：%s；闲置关停：%s\n", cpuAlert,
                static_cast<unsigned long long>(memAlertMb),
                qPrintable(limitCpu > 0.0 ? QStringLiteral("超限时节流至 %1%").arg(limitCpu)
                                          : QStringLiteral("关")),
                qPrintable(idleStopMin > 0 ? QStringLiteral("%1 分钟").arg(idleStopMin)
                                           : QStringLiteral("关")));

    std::optional<std::pair<double, std::chrono::steady_clock::time_point>> lastCpuTime;
    double cpuNow = 0.0;
    std::optional<std::chrono::steady_clock::time_point> highSince;
    bool alertedCpu = false;
    bool alertedMem = false;
    std::optional<std::chrono::steady_clock::time_point> idleSince;
    signal(SIGINT, monitorSigint);
    signal(SIGTERM, monitorSigint);

    while (!g_monitorStop.load()) {
        QThread::sleep(1);
        const auto pid = emulatorPid();
        if (!pid) {
            // 模拟器不在：清空状态，闲置计时也作废
            lastCpuTime.reset();
            highSince.reset();
            alertedCpu = false;
            alertedMem = false;
            idleSince.reset();
            continue;
        }

        // 瞬时 CPU：cputime 差分
        if (const auto t = procCputime(*pid)) {
            const auto now = std::chrono::steady_clock::now();
            if (lastCpuTime) {
                const double dt = std::max(
                    0.001,
                    std::chrono::duration<double>(now - lastCpuTime->second).count());
                cpuNow = std::max(0.0, (*t - lastCpuTime->first) / dt * 100.0);
            }
            lastCpuTime = std::make_pair(*t, now);
        }
        const double rss = procRssMb(*pid).value_or(0.0);

        // --- CPU 异常提醒（持续 30s 超阈值才报，避免抖动）---
        if (cpuNow > cpuAlert) {
            if (!highSince)
                highSince = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() - *highSince >= std::chrono::seconds(30)
                && !alertedCpu) {
                alertedCpu = true;
                monitorNotify(QStringLiteral(
                                  "虚拟机 CPU 持续 %1%（阈值 %2%）%3。可关闭不用的窗口，或 mwine kill "
                                  "--vm 全部关停")
                                  .arg(cpuNow, 0, 'f', 0)
                                  .arg(cpuAlert, 0, 'f', 0)
                                  .arg(limitCpu > 0.0 ? QStringLiteral("，已自动限流")
                                                      : QString()));
            }
        } else {
            highSince.reset();
            alertedCpu = false;
        }

        // --- 内存提醒（10 分钟冷却一次）---
        if (rss > double(memAlertMb) && !alertedMem) {
            alertedMem = true;
            monitorNotify(QStringLiteral(
                              "虚拟机内存 %1GB（阈值 %2MB）。建议重启虚拟机：mwine kill --vm 后再打开应用"
                              "（快照秒级唤醒）")
                              .arg(rss / 1024.0, 0, 'f', 1)
                              .arg(memAlertMb));
        } else if (rss < double(memAlertMb) * 0.8) {
            alertedMem = false;
        }

        // --- 限流：已移除（Rust 版注释：SIGSTOP/SIGCONT 冻结多线程 qemu 会触发其
        //     内部看门狗 abort；异常处置改为提醒 + 建议关窗口/关停虚拟机）---
        Q_UNUSED(limitCpu);

        // --- 闲置关停（无窗口 + 低 CPU 持续 N 分钟）---
        if (idleStopMin > 0) {
            const bool idle = runningWindowCount() == 0 && cpuNow < 30.0;
            if (idle) {
                if (!idleSince)
                    idleSince = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - *idleSince
                    >= std::chrono::seconds(idleStopMin * 60)) {
                    monitorNotify(QStringLiteral("虚拟机已闲置，自动关停（重新使用时会自动唤醒）"));
                    if (mw::supervisor::emuConsoleKill(QStringLiteral("emulator-5554")))
                        std::printf("[mwine] 闲置关停虚拟机 ✓\n");
                    else
                        std::printf("[mwine] 闲置关停失败\n");
                    idleSince.reset();
                }
            } else {
                idleSince.reset();
            }
        }
    }
    // 退出前恢复可能的 SIGSTOP（历史版本遗留的兜底）
    if (const auto pid = emulatorPid())
        ::kill(int(*pid), SIGCONT);
    std::printf("监护退出\n");
    return mw::Result<bool>::Ok();
}

// ---------------- monitor 开机自启（LaunchAgent，用户自选） ----------------

static const char* kMonitorAgentLabel = "wine.mwine.monitor";

static QString monitorAgentPlist()
{
    return QDir(mw::homeDir()).filePath(
        QStringLiteral("Library/LaunchAgents/wine.mwine.monitor.plist"));
}

static mw::Result<bool> plutilOk(const QString& plist)
{
    const auto out = mw::runProcess(QStringLiteral("plutil"),
                                    { QStringLiteral("-lint"), plist }, 30000);
    if (!out.ok())
        return mw::err<bool>(QStringLiteral("plutil 执行失败: %1").arg(out.error));
    if (out.exitCode != 0)
        return mw::err<bool>(QStringLiteral("plist 无效：%1").arg(out.stdOutStr().trimmed()));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> installMonitorAgent(double cpu, quint64 memMb, double limit,
                                            quint64 idleStop)
{
    const QString exe = QCoreApplication::applicationFilePath();
    const QString plist = monitorAgentPlist();
    QDir().mkpath(QFileInfo(plist).absolutePath());
    const QString args = QStringLiteral(
                             "<string>--cpu</string><string>%1</string>"
                             "<string>--mem-mb</string><string>%2</string>"
                             "<string>--limit</string><string>%3</string>"
                             "<string>--idle-stop</string><string>%4</string>")
                             .arg(cpu)
                             .arg(memMb)
                             .arg(limit)
                             .arg(idleStop);
    const QString xml = QStringLiteral(
                             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                             "<plist version=\"1.0\">\n<dict>\n"
                             "  <key>Label</key><string>%1</string>\n"
                             "  <key>ProgramArguments</key>\n  <array>\n"
                             "    <string>%2</string>\n"
                             "    <string>monitor</string>\n"
                             "    %3\n"
                             "  </array>\n"
                             "  <key>RunAtLoad</key><true/>\n"
                             "  <key>KeepAlive</key><true/>\n"
                             "</dict>\n</plist>\n")
                             .arg(QString::fromLatin1(kMonitorAgentLabel), exe, args);
    if (auto w = mw::writeFile(plist, xml.toUtf8()); !w)
        return mw::err<bool>(w.error());
    if (auto r = plutilOk(plist); !r)
        return mw::err<bool>(r.error());
    // 卸载旧实例（若有）再加载，避免双开
    {
        QProcess p;
        p.start(QStringLiteral("launchctl"),
                { QStringLiteral("unload"), QStringLiteral("-w"), plist });
        p.waitForFinished(15000);
    }
    const auto out = mw::runProcess(QStringLiteral("launchctl"),
                                    { QStringLiteral("load"), QStringLiteral("-w"), plist }, 30000);
    if (!out.ok() || out.exitCode != 0)
        return mw::err<bool>(QStringLiteral("launchctl load 失败：%1").arg(out.stdErrStr().trimmed()));
    std::printf("✓ 开机自启已安装并启动（%s）\n", qPrintable(plist));
    std::printf("  参数：CPU %.0f%% / 内存 %lluMB / 限流 %g / 闲置关停 %llu 分钟\n", cpu,
                static_cast<unsigned long long>(memMb), limit,
                static_cast<unsigned long long>(idleStop));
    std::printf("  卸载：mwine monitor --remove-auto-start\n");
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> uninstallMonitorAgent()
{
    const QString plist = monitorAgentPlist();
    if (!QFileInfo::exists(plist)) {
        std::printf("（未安装开机自启）\n");
        return mw::Result<bool>::Ok();
    }
    {
        QProcess p;
        p.start(QStringLiteral("launchctl"),
                { QStringLiteral("unload"), QStringLiteral("-w"), plist });
        p.waitForFinished(15000);
    }
    if (!QFile::remove(plist))
        return mw::err<bool>(QStringLiteral("删除 plist 失败"));
    std::printf("✓ 开机自启已卸载\n");
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdMonitor(Args& args)
{
    double cpu = 500.0;
    if (const auto v = args.opt("cpu")) {
        bool ok = false;
        cpu = v->toDouble(&ok);
        if (!ok)
            return badUsage<bool>(QStringLiteral("无效的 --cpu 值: %1").arg(*v));
    }
    quint64 memMb = 8000;
    if (const auto v = args.opt({ "mem-mb", "mem_mb" })) {
        bool ok = false;
        memMb = v->toULongLong(&ok);
        if (!ok)
            return badUsage<bool>(QStringLiteral("无效的 --mem-mb 值: %1").arg(*v));
    }
    double limit = 0.0;
    if (const auto v = args.opt("limit")) {
        bool ok = false;
        limit = v->toDouble(&ok);
        if (!ok)
            return badUsage<bool>(QStringLiteral("无效的 --limit 值: %1").arg(*v));
    }
    quint64 idleStop = 0;
    if (const auto v = args.opt({ "idle-stop", "idle_stop" })) {
        bool ok = false;
        idleStop = v->toULongLong(&ok);
        if (!ok)
            return badUsage<bool>(QStringLiteral("无效的 --idle-stop 值: %1").arg(*v));
    }
    const bool autoStart = args.has("auto-start");
    const bool removeAutoStart = args.has("remove-auto-start");
    if (removeAutoStart)
        return uninstallMonitorAgent();
    if (autoStart)
        // 安装即由 launchd 拉起实例，本进程不再前台运行（避免双开）
        return installMonitorAgent(cpu, memMb, limit, idleStop);
    return runMonitor(cpu, memMb, limit, idleStop);
}

} // namespace mwine
