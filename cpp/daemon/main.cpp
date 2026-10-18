// 守护进程入口（crates/mwine-core/src/daemon.rs 的 run() + main.rs 移植）。
//
// 端口 = MWINE_PORT 环境变量或 7788，只听 127.0.0.1；
// stdio 落 ~/.mwine/logs/daemon.log（>4MB 轮转）；孤儿 VM 看护常驻线程。
#include <QCoreApplication>
#include <QProcess>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QSslSocket>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs.hpp"
#include "httpserver.hpp"
#include "net.hpp"
#include "log.hpp"
#include "process.hpp"
#include "routes.hpp"
#include "state.hpp"
#include "store_api.hpp"
#include "supervisor.hpp"

namespace {

// 把 stdout/stderr 追加到 ~/.mwine/logs/daemon.log（超 4MB 截断重开）。
// daemon 常以 GUI 子进程（管道 stdio）方式被拉起，GUI 退出后管道断裂，
// 此后任何输出都会 EPIPE——落文件后 daemon 与父进程生命周期解耦，
// GUI 重启可直接收养旧 daemon。失败（无 HOME 等）保持原样。
void redirectStdioToLog()
{
    const QString dir = QDir(mw::mwineDir()).filePath(QStringLiteral("logs"));
    if (!QDir().mkpath(dir))
        return;
    const QString path = QDir(dir).filePath(QStringLiteral("daemon.log"));
    // 简单轮转：超 4MB 截断
    const QFileInfo fi(path);
    if (fi.exists() && fi.size() > 4 * 1024 * 1024)
        QFile::remove(path);
    const int fd = ::open(path.toUtf8().constData(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        ::close(fd);
}

// 孤儿 VM 看护（daemon.rs spawn_orphan_wdog 逻辑照抄）。
//
// 两类故障源（均实测出现过）：
// 1. 强杀/崩溃留下的陈旧 AVD 锁与失效注册表项——后续任何启动都静默挂死；
// 2. 无人看管的僵死 qemu：进程活着但 adb 早已不在线（guest 半死），
//    lavapipe 软渲染下空转即可烧满 7 核数小时（风扇狂转事故）。
//
// 策略（保守）：只杀"adb 确认不可达且持续 3 分钟"的实例——健康 VM 的
// adb 始终在线，绝不会误伤；TERM 优先，10s 后 KILL 兜底。
void orphanWatchdogLoop(mw::daemon::AppState* state)
{
    const mw::adb::AdbClient adb;
    const QString avdDir = QDir(mw::homeDir())
                              .filePath(QStringLiteral(
                                  "Library/Application Support/MWine/avd"));
    const QString registry = QDir(mw::homeDir())
                                 .filePath(QStringLiteral(
                                     "Library/Caches/TemporaryItems/avd/running"));
    qint64 wedgedPid = 0;
    QDateTime wedgedAt;
    int shellFailCount = 0;
    int healLevel = 0; // 自愈级别（0 正常 / 1 已做过存档重启）
    for (;;) {
        // --- 清扫：无 qemu 运行时的陈旧 AVD 锁 ---
        QVector<qint64> qemuPids;
        {
            const auto r = mw::runProcess(
                QStringLiteral("pgrep"),
                { QStringLiteral("-f"), QStringLiteral("qemu-system-aarch64") }, 15000);
            for (const QString& line :
                 QString::fromUtf8(r.stdOut).split(QLatin1Char('\n'))) {
                bool ok = false;
                const qint64 p = line.trimmed().toLongLong(&ok);
                if (ok && p > 0)
                    qemuPids.append(p);
            }
        }
        if (qemuPids.isEmpty()) {
            const QDir ad(avdDir);
            for (const QFileInfo& avd :
                 ad.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                for (const QString& lock :
                     { QStringLiteral("multiinstance.lock"),
                       QStringLiteral("hardware-qemu.ini.lock") }) {
                    const QString p = QDir(avd.absoluteFilePath()).filePath(lock);
                    if (QFileInfo(p).isFile()) {
                        QFile::remove(p);
                        MW_LOG("orphan", QStringLiteral("清除陈旧 AVD 锁 %1").arg(p));
                    }
                }
            }
        }
        // --- 清扫：注册表里 pid 已死的条目 ---
        {
            const QDir rd(registry);
            for (const QFileInfo& e : rd.entryInfoList(QDir::Files)) {
                const QString name = e.fileName();
                QString pidStr = name;
                if (pidStr.startsWith(QStringLiteral("pid_")))
                    pidStr.remove(0, 4);
                if (pidStr.endsWith(QLatin1String(".ini")))
                    pidStr.chop(4);
                bool ok = false;
                const qint64 pid = pidStr.toLongLong(&ok);
                if (!ok || pid <= 0)
                    continue;
                const bool alive =
                    ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
                if (!alive) {
                    QFile::remove(e.absoluteFilePath());
                    MW_LOG("orphan", QStringLiteral("清除失效注册表项 %1").arg(name));
                }
            }
        }
        // --- 回收：qemu 活着但 adb 不可达且持续 3 分钟 ---
        bool adbOnline = false;
        QString onlineSerial;
        {
            auto ds = adb.devices();
            if (ds) {
                for (const auto& d : ds.value()) {
                    if (d.state == QStringLiteral("device")
                        && d.serial.startsWith(QStringLiteral("emulator-"))) {
                        adbOnline = true;
                        onlineSerial = d.serial;
                        break;
                    }
                }
            }
        }
        // 冷启动保护：进程年龄 <8 分钟不参与回收（合法冷启 adbd 可晚至
        // 数分钟上线；僵死实例实测都是运行数十分钟以上才被发现）。
        // 全部 qemu 实例都要过年龄关（多实例时最年轻的也可能在冷启）
        bool procAgeOk = !qemuPids.isEmpty();
        for (const qint64 pid : qemuPids) {
            const auto r = mw::runProcess(
                QStringLiteral("ps"),
                { QStringLiteral("-o"), QStringLiteral("etimes="), QStringLiteral("-p"),
                  QString::number(pid) },
                10000);
            bool ok = false;
            const quint64 secs = QString::fromUtf8(r.stdOut).trimmed().toULongLong(&ok);
            if (!ok || secs <= 480) {
                procAgeOk = false;
                break;
            }
        }
        if (!qemuPids.isEmpty() && !adbOnline && procAgeOk) {
            const QDateTime now = QDateTime::currentDateTime();
            if (wedgedPid != qemuPids.first()) {
                wedgedPid = qemuPids.first();
                wedgedAt = now;
            } else if (wedgedAt.isValid()
                       && wedgedAt.msecsTo(now) > 180 * 1000) {
                const qint64 pid = qemuPids.first();
                MW_LOG("orphan",
                       QStringLiteral("回收僵死 qemu（pid %1，adb 不可达已超 3 分钟）")
                           .arg(pid));
                (void)mw::runProcess(QStringLiteral("kill"),
                                     { QStringLiteral("-TERM"), QString::number(pid) },
                                     10000);
                std::this_thread::sleep_for(std::chrono::seconds(10));
                (void)mw::runProcess(
                    QStringLiteral("kill"),
                    { QStringLiteral("-9"), QString::number(pid) }, 10000);
                wedgedPid = 0;
            }
        } else {
            wedgedPid = 0;
        }

        // --- 健康看护：注册表判定"在线"但 shell 挂（半死 guest）---
        // TP1A 已知问题：快照恢复后 guest adbd 可失联卡 offline——TCP 端口仍活
        // （direct 探活被骗），shell 无响应，应用库/安装/窗口全部失效。
        // 更糟的是 vm stop 会把半死状态存进快照 → 档案永久坏 → 恢复即半死。
        // 两级自愈：L1 存档重启（赌档案没坏）；L2 仍失败 → 杀进程不存档 +
        // 删坏档 + 冷启重建干净档案。连续 3 次 shell 探活失败触发一级。
        if (!qemuPids.isEmpty() && adbOnline && procAgeOk && !onlineSerial.isEmpty()) {
            // 下载/安装大 APK 期间 adb 被长占用是预期（adb push / install 阻塞），
            // shell 探活必然超时——此时绝不能误判「VM 半死」重启，否则会打断任务
            // （实测：下载完成 → 自动安装 → 自愈重启 → done 状态丢失、安装中断）。
            const bool taskBusy = state
                && (state->storeTask.running.load() || state->installTask.running.load());
            if (taskBusy) {
                shellFailCount = 0;  // 任务期间不计失败、不自愈
            } else {
            const auto probe =
                adb.shellBlocking(onlineSerial, QStringLiteral("echo mw"), 8000);
            const bool healthy = probe.ok()
                && probe.value().outStr().trimmed() == QStringLiteral("mw");
            if (healthy) {
                shellFailCount = 0;
                healLevel = 0;
            } else if (++shellFailCount >= 3) {
                shellFailCount = 0;
                const int port =
                    qEnvironmentVariableIntValue("MWINE_PORT") > 0
                        ? qEnvironmentVariableIntValue("MWINE_PORT") : 7788;
                if (healLevel == 0) {
                    healLevel = 1;
                    MW_LOG("orphan",
                           QStringLiteral("设备 shell 连续无响应，自愈 L1：存档重启"));
                    (void)mw::httpRequest(
                        QStringLiteral("POST"),
                        QUrl(QStringLiteral("http://127.0.0.1:%1/api/vm/stop").arg(port)),
                        QByteArray{}, {}, 90000);
                    (void)mw::httpRequest(
                        QStringLiteral("POST"),
                        QUrl(QStringLiteral("http://127.0.0.1:%1/api/vm/start").arg(port)),
                        QByteArray{}, {}, 10000);
                } else {
                    healLevel = 0;
                    MW_LOG("orphan",
                           QStringLiteral("自愈 L1 后仍无响应，L2：丢弃坏档冷启重建"));
                    // 快照里保存的就是半死状态：杀进程（不存档）+ 删 mwine_default
                    QString avdName;
                    {
                        const auto r = mw::supervisor::emuConsoleCmd(
                            onlineSerial, QStringLiteral("avd name"));
                        if (r.ok()) {
                            for (const QString& l :
                                 QString::fromUtf8(r.value()).split(QLatin1Char('\n'))) {
                                const QString cand = l.trimmed();
                                if (!cand.isEmpty() && cand != QLatin1String("OK")
                                    && !cand.contains(QLatin1Char(':'))) {
                                    avdName = cand;
                                    break;
                                }
                            }
                        }
                    }
                    (void)mw::supervisor::emuConsoleKill(onlineSerial);
                    for (int i = 0; i < 30; ++i) { // 等完全退出（最多 30s）
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        const auto r = mw::runProcess(
                            QStringLiteral("pgrep"),
                            { QStringLiteral("-f"), QStringLiteral("qemu-system-aarch64") },
                            10000);
                        if (!QString::fromUtf8(r.stdOut).trimmed().isEmpty())
                            continue;
                        break;
                    }
                    if (!avdName.isEmpty()) {
                        for (const QString& home :
                             { QDir(mw::homeDir()).filePath(QStringLiteral(".android/avd")),
                               QDir(mw::homeDir()).filePath(
                                   QStringLiteral("Library/Application Support/MWine/avd")) }) {
                            const QDir snap(
                                QDir(QDir(home).filePath(
                                         avdName + QStringLiteral(".avd/snapshots")))
                                    .filePath(QStringLiteral("mwine_default")));
                            if (snap.exists()) {
                                QDir(snap).removeRecursively();
                                MW_LOG("orphan",
                                       QStringLiteral("已丢弃坏档 %1/mwine_default").arg(avdName));
                            }
                        }
                    }
                    (void)mw::httpRequest(
                        QStringLiteral("POST"),
                        QUrl(QStringLiteral("http://127.0.0.1:%1/api/vm/start").arg(port)),
                        QByteArray{}, {}, 10000);
                }
            }
            }  // end else（非任务繁忙才跑 shell 自愈）
        } else {
            shellFailCount = 0;
        }
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
}

void spawnOrphanWatchdog(mw::daemon::AppState* state)
{
    std::thread(orphanWatchdogLoop, state).detach();
}

// ===== bundle 外置迁移（daemon.rs migrate_* 移植；打包态 .app 首启时生效）=====

// bundle 内嵌运行时（emulator/platform-tools 等）整体迁到外置目录：
// 外置已有完整运行时 → 删 bundle 内冗余副本；否则同卷 rename 迁出（原子）。
void migrateBundleRuntimeOut()
{
    const QString exe = QCoreApplication::applicationFilePath();
    const QString contents = QFileInfo(QFileInfo(exe).absolutePath()).absolutePath(); // Contents/
    if (contents.isEmpty() || contents == QStringLiteral("/"))
        return;
    const QString bundled = QDir(contents).filePath(QStringLiteral("Resources/runtime"));
    const QString external = mw::supervisor::Sdk::externalRuntimeDir();
    const bool bundledOk = QDir(bundled).filePath(QStringLiteral("emulator")) != bundled
                           && QFileInfo::exists(QDir(bundled).filePath(QStringLiteral("emulator")));
    const bool externalOk = QFileInfo::exists(QDir(external).filePath(QStringLiteral("emulator")));
    if (!bundledOk)
        return; // 新版瘦包或开发态
    if (externalOk) {
        if (!QDir(bundled).removeRecursively())
            MW_LOG("runtime", QStringLiteral("清理 bundle 内冗余运行时失败"));
        else
            MW_LOG("runtime", QStringLiteral("已清理 bundle 内冗余运行时（外置副本生效）"));
        return;
    }
    QDir().mkpath(QFileInfo(external).absolutePath());
    if (!QDir().rename(bundled, external))
        MW_LOG("runtime", QStringLiteral("迁移失败（跨卷？）"));
    else
        MW_LOG("runtime", QStringLiteral("运行时已迁出至 %1").arg(external));
}

// bundle 内真实 system-images 迁到 Application Support，原位换符号链接。
// 幂等：已是链接则只确保外置目标存在；子目录合并迁移（外置已有的保留）。
void migrateBundleImagesOut()
{
#ifdef Q_OS_UNIX
    const QString exe = QCoreApplication::applicationFilePath();
    const QString contents = QFileInfo(QFileInfo(exe).absolutePath()).absolutePath();
    const QString images = QDir(contents).filePath(QStringLiteral("Resources/runtime/system-images"));
    const QFileInfo st(images);
    if (!st.exists() && !st.isSymLink())
        return; // 非打包运行
    if (st.isSymLink()) {
        const QString target = QFile::symLinkTarget(images);
        if (QFileInfo(target).isRelative())
            return; // 相对链接（非本布局），不动
        QDir().mkpath(target);
        return;
    }
    const QString support = QDir(mw::homeDir())
                                .filePath(QStringLiteral("Library/Application Support/MWine"));
    const QString target = QDir(support).filePath(QStringLiteral("system-images"));
    if (!QDir().mkpath(target)) {
        MW_LOG("images", QStringLiteral("外置目录创建失败"));
        return;
    }
    int migrated = 0;
    const auto entries = QDir(images).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& e : entries) {
        const QString dst = QDir(target).filePath(e.fileName());
        if (QFileInfo::exists(dst))
            continue; // 外置已有，保留
        if (QDir().rename(e.absoluteFilePath(), dst))
            ++migrated;
    }
    if (!QDir(images).isEmpty()) {
        MW_LOG("images", QStringLiteral("bundle 内有未迁完的镜像，跳过链接替换"));
        return;
    }
    if (!QDir(images).rmdir(images)) // remove 非空才可用；rmdir 要求空
        return;
    // 绝不在 .app bundle 内创建指向外部的符号链接：codesign 拒签
    // （invalid destination for symbolic link）→ 整个 app 报「已损坏」
    //（2026-10-18 实测）。外置目录本就是 Sdk::detect 的优先候选，留空目录即可
    QDir().mkpath(images);
    MW_LOG("images",
           QStringLiteral("已外置到 %1（迁移 %2 项；bundle 内留空目录）").arg(target).arg(migrated));
#endif
}

// 通知桥自启：`mwine notify`（guest→macOS 通知桥）宿主常驻；退出 5 分钟后重拉。
void spawnNotifyBridge()
{
    std::thread([]() {
        forever {
            const QString exe = QCoreApplication::applicationFilePath();
            const QString dir = QFileInfo(exe).absolutePath();
            const QString cli = QDir(dir).filePath(QStringLiteral("mwine"));
            if (!QFileInfo::exists(cli))
                return;
            QProcess p;
            p.setProgram(cli);
            p.setArguments({ QStringLiteral("notify") });
            p.start(QIODevice::NotOpen);
            if (p.waitForFinished(3600 * 1000 * 24))
                std::this_thread::sleep_for(std::chrono::seconds(300));
        }
    }).detach();
}

} // namespace

namespace {
// SIGTERM 在镜像/下载任务进行中会被延迟处理：GUI 的离线误判会带着
// REPLACE=1 拉新实例顶替旧守护（2026-10-18 实测：下载至 400MB/23MB 时
// 旧守护被无声 TERM，任务与断点全丢——daemon 死前 /api/status 一直 2ms
// 响应，绝非自身卡死）。拉取动辄十几分钟，必须扛住误杀：任务收尾后才退。
std::atomic_bool g_termRequested{ false };
mw::daemon::AppState* g_state = nullptr;  // main 稍后赋值（TERM 延迟检查用）
void onSigTerm(int)
{
    g_termRequested.store(true);
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mwine-cored"));

    // TLS 预热：Qt 的 SSL 后端并发首次初始化有竞态（官方列表请求与下载线程
    // 同时首次走 https → "TLS initialization failed"，2026-10-18 镜像拉取
    // 连续失败 5 次的根因）。主线程开工前同步探一次，之后所有线程安全。
    (void)QSslSocket::supportsSsl();

    ::signal(SIGTERM, onSigTerm);
    {
        // TERM 到达时：无任务 → 正常退出；有任务 → 等收尾（上限 30 分钟）
        auto* termTimer = new QTimer(&app);
        termTimer->setInterval(1000);
        static const auto taskBusy = [](mw::daemon::AppState* st) {
            return st->imageTask.running.load() || st->storeTask.running.load()
                   || st->installTask.running.load() || st->runtimeTask.running.load();
        };
        QObject::connect(termTimer, &QTimer::timeout, &app, [&app] {
            static int waitedSec = 0;
            if (!g_termRequested.load())
                return;
            // AppState 裸指针在 main 栈上（见下方声明顺序），这里经 lambda 捕获
            if (taskBusy(g_state) && waitedSec < 1800) {
                if (waitedSec == 0)
                    MW_LOG("core", QStringLiteral("收到退出请求，但后台任务进行中——延迟退出（最长 30 分钟）"));
                ++waitedSec;
                return;
            }
            MW_LOG("core", QStringLiteral("处理延迟的退出请求"));
            QCoreApplication::quit();
        });
        termTimer->start();
    }


    // stdio 落日志文件（daemon 常以 GUI 子进程方式被拉起，管道断裂后 EPIPE）
    redirectStdioToLog();

    // 主线程预热 Qt 的惰性路径缓存（工作线程并发首调非线程安全，SDK 检测崩溃根因）
    (void)QCoreApplication::applicationDirPath();

    // bundle 内嵌镜像/运行时外置到 Application Support（幂等；开发态自动跳过）
    migrateBundleImagesOut();
    migrateBundleRuntimeOut();
    // 通知桥自启：mwine notify（CLI 就绪后生效；找不到二进制则静默退出）
    spawnNotifyBridge();

    // 清理 7 天前的残留下载安装包
    mw::daemon::purgeStaleApks();

    // 守护进程常驻状态（进程生命周期；后台线程经裸指针共享，不回收）。
    // g_state 供 TERM 延迟检查用（lambda 捕获本指针）
    auto* state = new mw::daemon::AppState();
    g_state = state;
    // 孤儿 VM 看护：清扫陈旧锁/注册表 + 回收"活着但已僵死"的 qemu 实例
    // （传 state：下载/安装任务进行中抑制 shell 自愈，防止 adb 忙被误判 VM 半死）
    spawnOrphanWatchdog(state);

    bool portOk = false;
    const int envPort = qEnvironmentVariableIntValue("MWINE_PORT", &portOk);
    const quint16 port =
        (portOk && envPort > 0 && envPort <= 65535) ? quint16(envPort) : quint16(7788);

    mw::HttpServer server(
        [state](const mw::HttpRequest& req) {
            // 读取请求体上限 64KB（对齐 Rust reader.take(64 * 1024)）
            const QByteArray body = req.body.left(64 * 1024);
            return mw::daemon::routeWithBody(req.method, req.path, body, state);
        },
        port);
    if (!server.start()) {
        // 升级残留：旧版守护仍占着端口。Qt 前端 spawn 时注入 MWINE_REPLACE_DAEMON=1，
        // 此时替换旧实例（SIGTERM 同名守护，等端口释放后重试绑定一次）。
        if (qEnvironmentVariableIsSet("MWINE_REPLACE_DAEMON")) {
            MW_LOG("core", QStringLiteral("端口 %1 被旧守护占用，尝试替换…").arg(port));
            const auto out = mw::runProcess(
                QStringLiteral("pgrep"),
                { QStringLiteral("-x"), QStringLiteral("mwine-cored") }, 5000);
            if (out.ok()) {
                const auto myPid = QCoreApplication::applicationPid();
                for (const QString& s :
                     QString::fromUtf8(out.stdOut).split(QRegularExpression(QStringLiteral("\\s+")),
                                                         Qt::SkipEmptyParts)) {
                    bool ok = false;
                    const int pid = s.toInt(&ok);
                    if (ok && pid != myPid) {
                        ::kill(pid, SIGTERM);
                        MW_LOG("core", QStringLiteral("已请求旧守护 %1 退出").arg(pid));
                    }
                }
            }
            for (int i = 0; i < 50 && !server.start(); ++i)
                QThread::msleep(100);
            if (!server.start())
                return 1;
        } else {
            return 1;
        }
    }
    MW_LOG("core", QStringLiteral("✓ MWine Core 就绪: http://127.0.0.1:%1").arg(port));

    return app.exec();
}
