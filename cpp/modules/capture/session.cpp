// 采集会话编排实现 ← crates/mwine-capture/src/session.rs
//
// 两种形态（session.hpp 头注）：
// - 本机模式 start()：ecapture 为本机子进程（QProcess 长驻），stdout 直接进
//   解析器，进程退出后带 2s 退避重启；
// - 设备模式 startOnDevice()：mwine-adb 自研协议栈在 guest 内部署/拉起
//   ecapture（root + 推二进制 + nohup），事件写 /data/local/tmp/mwine-ecap.log，
//   采集线程常驻 `tail -c +N -f` shell 会话；看门狗低频刷新 pid→包名映射
//   （pm list packages -U + ps）并观测 WebView 旁观产物体积；stop() 拉取
//   pcap + 密钥做宿主侧 TLS 解密入库（webviewFlowsFromPcap）并支持导出。
//
// WebView 旁观的 guest 侧动作（chmod/setenforce/命令行开关/tcpdump）与 pkill
// 分离原则的实证注释见 startOnDeviceUnlocked 内（与 Rust 版一致）。
#include "session.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QProcess>
#include <QThread>

#include <QDebug>

#include <algorithm>
#include <optional>

#include "adb.hpp"
#include "events.hpp"
#include "flow.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "pcap_decrypt.hpp"

namespace mw::capture {

namespace {

// ecapture 启动参数（本机模式：tls 模块、text 输出到 stdout——与解析器期望格式一致）。
QStringList ecaptureArgs()
{
    return { QStringLiteral("tls"), QStringLiteral("-m"), QStringLiteral("text") };
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void sleepInterruptible(const std::atomic_bool& stop, int ms)
{
    int left = ms;
    while (left > 0 && !stop.load()) {
        const int slice = std::min(left, 100);
        QThread::msleep(uint(slice));
        left -= slice;
    }
}

// 空连接（建连后无明文事件）的 top3 汇总，如 "ChromiumNet×560"。
QStringList topEmptyComms(const QHash<QString, quint64>& m)
{
    QVector<QPair<QString, quint64>> v;
    for (auto it = m.cbegin(); it != m.cend(); ++it)
        v.push_back(qMakePair(it.key(), it.value()));
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    QStringList out;
    for (int i = 0; i < std::min<qsizetype>(3, v.size()); ++i)
        out.push_back(QStringLiteral("%1×%2").arg(v.at(i).first).arg(v.at(i).second));
    return out;
}

// 导出目录时间戳（day 序号 + 当日时分秒，对应 Rust chrono_like_ts）。
QString chronoLikeTs()
{
    const qint64 ms = nowMs();
    const qint64 secs = ms / 1000;
    const qint64 t = secs % 86400;
    return QStringLiteral("%1-%2%3%4")
        .arg(secs / 86400 + 19700)
        .arg(t / 3600, 2, 10, QChar('0'))
        .arg((t / 60) % 60, 2, 10, QChar('0'))
        .arg(t % 60, 2, 10, QChar('0'));
}

} // namespace

// ---------------------------------------------------------------------------
// WebView 密文侧收尾纯逻辑（设备 stop 与测试共用）

QVector<Flow> webviewFlowsFromPcap(const QByteArray& pcap, const QByteArray& keylogText)
{
    const QVector<DecryptedStream> streams = decryptPcap(pcap, QString::fromUtf8(keylogText));
    if (streams.isEmpty())
        return {};
    Reassembler reasm;
    qint64 ts = nowMs();
    for (int i = 0; i < streams.size(); ++i) {
        // 合成 pid 避免与真实进程冲突（Reassembler 以 pid 为主键）
        const quint32 pid = 9100000u + quint32(i);
        const DecryptedStream& s = streams.at(i);
        const QString src = QStringLiteral("%1:%2").arg(ipToString(s.client.ip)).arg(s.client.port);
        const QString dst = QStringLiteral("%1:%2").arg(ipToString(s.server.ip)).arg(s.server.port);
        reasm.feed(Event::makeConnect(pid, QStringLiteral("webview"), 0, src, dst));
        if (!s.clientPlain.isEmpty()) {
            reasm.feed(Event::makeIo(QString(), ts, pid, pid, QStringLiteral("webview"),
                                     Direction::Write, s.clientPlain));
        }
        if (!s.serverPlain.isEmpty()) {
            reasm.feed(Event::makeIo(QString(), ts, pid, pid, QStringLiteral("webview"),
                                     Direction::Read, s.serverPlain));
        }
        ++ts;
    }
    QVector<Flow> done = reasm.flush();
    for (Flow& f : done) {
        if (f.comm == QLatin1String("webview"))
            f.comm = QStringLiteral("WebView(解密)");
    }
    return done;
}

// ---------------------------------------------------------------------------
// 线程共享状态

struct Session::Shared {
    std::atomic_bool stop { false };
    std::atomic<quint64> events { 0 };
    std::atomic<quint64> bytes { 0 };
    // 本机模式恒 0；设备模式为看门狗观测到的 guest 日志体积（滞后信号）
    std::atomic<qint64> logBytes { 0 };
    std::atomic_bool procAlive { false };
    // 设备模式：WebView 旁观产物体积（密钥日志/pcap，字节）
    std::atomic<quint64> wvKeys { 0 };
    std::atomic<quint64> wvPcap { 0 };
    bool device = false;
    QString serial;
    mw::adb::AdbClient adb;
    // 当前 tail 会话的停读句柄（stop 时 shutdown 解除阻塞读；见
    // deviceCollectorLoop 的赋值-检查顺序说明）
    QMutex tailMutex;
    std::optional<mw::adb::TcpConn> tailCtrl;
    QMutex noteMutex;
    QString guestNote;
    QMutex emptyMutex;
    QHash<QString, quint64> emptyComms;
    QMutex reasmMutex;
    Reassembler reasm;
    QMutex storeMutex;
    Store store;

    void setNote(const QString& s)
    {
        QMutexLocker l(&noteMutex);
        guestNote = s;
    }

    // 空连接诊断计数 + 落库（所有连接都保留：无明文的标 encrypted）。
    void persist(QVector<Flow> flows)
    {
        QHash<QString, quint64> emptyAdds;
        for (const Flow& f : flows) {
            if (f.wbytes + f.rbytes == 0) {
                const QString key = f.comm.isEmpty() ? f.src : f.comm;
                ++emptyAdds[key];
            }
        }
        if (!emptyAdds.isEmpty()) {
            QMutexLocker l(&emptyMutex);
            for (auto it = emptyAdds.cbegin(); it != emptyAdds.cend(); ++it)
                emptyComms[it.key()] += it.value();
        }
        if (flows.isEmpty())
            return;
        QMutexLocker l(&storeMutex);
        const auto r = store.upsertFlows(flows);
        if (!r)
            qWarning().noquote()
                << QStringLiteral("[mwine-capture] flow 落库失败：%1").arg(r.error());
    }
};

void Session::processEvents(Shared* sh, const QVector<Event>& events)
{
    QVector<Flow> done;
    {
        QMutexLocker l(&sh->reasmMutex);
        for (const Event& ev : events) {
            const QVector<Flow> batch = sh->reasm.feed(ev);
            done += batch;
        }
    }
    if (!done.isEmpty())
        sh->persist(std::move(done));
}

// ---------------------------------------------------------------------------
// 本机模式线程体

// 采集主循环：stdout 流 → 解析 → 重组 → 落库（进程死后退避重启）。
void Session::collectorLoop(Shared* sh, const QString& bin)
{
    EventParser parser; // 跨重启保留（半条事件不丢）
    quint64 restarts = 0;
    while (!sh->stop.load()) {
        QProcess proc;
        proc.setProgram(bin);
        proc.setArguments(ecaptureArgs());
        proc.setProcessChannelMode(QProcess::SeparateChannels);
        proc.start();
        if (!proc.waitForStarted(5000)) {
            sh->procAlive = false;
            sh->setNote(QStringLiteral("ecapture 启动失败: %1").arg(proc.errorString()));
            sleepInterruptible(sh->stop, 2000);
            continue;
        }
        sh->procAlive = true;
        ++restarts;
        QByteArray errTail;
        while (!sh->stop.load()) {
            proc.waitForReadyRead(200);
            if (sh->stop.load())
                break;
            const QByteArray chunk = proc.readAllStandardOutput();
            if (!chunk.isEmpty()) {
                sh->bytes.fetch_add(quint64(chunk.size()));
                parser.feed(chunk);
                const QVector<Event> events = parser.drain();
                sh->events.fetch_add(quint64(events.size()));
                if (!events.isEmpty())
                    processEvents(sh, events);
            }
            const QByteArray err = proc.readAllStandardError();
            if (!err.isEmpty()) {
                errTail.append(err);
                if (errTail.size() > 4096)
                    errTail.remove(0, errTail.size() - 4096);
            }
            if (proc.state() == QProcess::NotRunning) {
                // 读尽缓冲后结束本轮（外层决定重启）
                const QByteArray rest = proc.readAllStandardOutput();
                if (!rest.isEmpty()) {
                    sh->bytes.fetch_add(quint64(rest.size()));
                    parser.feed(rest);
                    const QVector<Event> events = parser.drain();
                    sh->events.fetch_add(quint64(events.size()));
                    if (!events.isEmpty())
                        processEvents(sh, events);
                }
                const QString tail = QString::fromUtf8(errTail.right(300)).simplified();
                sh->setNote(QStringLiteral("ecapture 退出 code=%1 %2")
                                .arg(proc.exitCode())
                                .arg(tail));
                break;
            }
        }
        sh->procAlive = false;
        if (proc.state() != QProcess::NotRunning) {
            proc.terminate();
            if (!proc.waitForFinished(2000))
                proc.kill(), proc.waitForFinished(1000);
        }
        if (!sh->stop.load()) {
            MW_LOG("capture",
                   QStringLiteral("ecapture 进程退出，%1ms 后重启（第 %2 次）")
                       .arg(2000)
                       .arg(restarts));
            sleepInterruptible(sh->stop, 2000);
        }
    }
    // 收尾：冲刷 live flow
    QVector<Flow> done;
    {
        QMutexLocker l(&sh->reasmMutex);
        done = sh->reasm.flush();
    }
    sh->persist(std::move(done));
}

// 看门狗：空闲 flow 定稿 + 爆增防御 + 可观测性。
void Session::watchdogLoop(Shared* sh)
{
    while (!sh->stop.load()) {
        sleepInterruptible(sh->stop, 5000);
        if (sh->stop.load())
            break;
        // 空闲超过 15s 的 flow 定稿落库
        QVector<Flow> done;
        {
            QMutexLocker l(&sh->reasmMutex);
            done = sh->reasm.flushIdle(nowMs(), 15000);
        }
        if (!done.isEmpty())
            sh->persist(std::move(done));
        // 爆增防御：live 过多（内存）→ 全量定稿走落库管道
        QVector<Flow> capped;
        {
            QMutexLocker l(&sh->reasmMutex);
            capped = sh->reasm.capLive(4096);
        }
        if (!capped.isEmpty())
            sh->persist(std::move(capped));
        // 可观测性
        sh->setNote(QStringLiteral("ecapture=%1 log=%2B")
                        .arg(sh->procAlive.load() ? 1 : 0)
                        .arg(qint64(sh->bytes.load())));
    }
}

// ---------------------------------------------------------------------------
// 设备模式线程体

// 设备采集主循环：tail 流 → 解析 → 重组 → 落库（会话断开后退避重开）。
// tailCtrl 的赋值-检查顺序保证无死锁：句柄在进入阻塞读之前已可见，
// stop() 在 tailMutex 下要么看到句柄（shutdown 解除阻塞），要么此时采集
// 线程尚未进入阻塞读（其后必先观察到 stop 标志而退出）。
void Session::deviceCollectorLoop(Shared* sh)
{
    EventParser parser; // 跨重开保留（半条事件不丢）
    quint64 consumed = 0;
    while (!sh->stop.load()) {
        const QString cmd = QStringLiteral("tail -c +%1 -f %2")
                                .arg(consumed + 1)
                                .arg(QString::fromLatin1(kGuestLog));
        auto sess = sh->adb.shellSession(sh->serial, cmd);
        if (!sess) {
            sh->setNote(QStringLiteral("tail 会话建立失败: %1").arg(sess.error()));
            sleepInterruptible(sh->stop, 2000);
            continue;
        }
        {
            QMutexLocker t(&sh->tailMutex);
            sh->tailCtrl = sess.value().shutdownHandle();
        }
        if (sh->stop.load())
            break;
        mw::adb::ShellSession s = std::move(sess.value());
        while (!sh->stop.load()) {
            const auto chunk = s.reader.read(16384);
            if (!chunk)
                break; // tail 进程退出/连接关闭 → 外层重开
            consumed += quint64(chunk.value().size());
            sh->bytes.fetch_add(quint64(chunk.value().size()));
            parser.feed(chunk.value());
            const QVector<Event> events = parser.drain();
            sh->events.fetch_add(quint64(events.size()));
            if (!events.isEmpty())
                processEvents(sh, events);
        }
        {
            QMutexLocker t(&sh->tailMutex);
            sh->tailCtrl.reset();
        }
        if (!sh->stop.load())
            sleepInterruptible(sh->stop, 2000);
    }
    // 收尾：冲刷 live flow
    QVector<Flow> done;
    {
        QMutexLocker l(&sh->reasmMutex);
        done = sh->reasm.flush();
    }
    sh->persist(std::move(done));
}

// 设备看门狗：包名富化 + 空闲定稿 + 爆增防御 + guest 可观测性。
void Session::deviceWatchdogLoop(Shared* sh)
{
    quint32 tick = 0;
    while (!sh->stop.load()) {
        sleepInterruptible(sh->stop, 5000);
        if (sh->stop.load())
            break;
        ++tick;
        // 每 3 个周期（15s）刷新一次 pid→包名映射
        if (tick % 3 == 1) {
            const QMap<quint32, QString> pkgs = pidPackages(sh->serial);
            if (!pkgs.isEmpty()) {
                QMutexLocker l(&sh->reasmMutex);
                sh->reasm.setPidPackages(pkgs);
            }
        }
        // 空闲超过 15s 的 flow 定稿落库
        {
            QVector<Flow> done;
            {
                QMutexLocker l(&sh->reasmMutex);
                done = sh->reasm.flushIdle(nowMs(), 15000);
            }
            if (!done.isEmpty())
                sh->persist(std::move(done));
        }
        // 爆增防御：live 过多（内存）→ 全量定稿走落库管道
        {
            QVector<Flow> capped;
            {
                QMutexLocker l(&sh->reasmMutex);
                capped = sh->reasm.capLive(4096);
            }
            if (!capped.isEmpty())
                sh->persist(std::move(capped));
        }
        // 可观测性：guest 日志体积（滞后 = log_bytes − bytes_seen）+ WebView 产物
        const auto out = sh->adb.shellBlocking(
            sh->serial,
            QStringLiteral("stat -c %s %1 2>/dev/null || echo 0; "
                           "ps -A | grep -c '[e]capture' || echo 0; "
                           "stat -c %s %2 2>/dev/null || echo 0; "
                           "stat -c %s %3 2>/dev/null || echo 0")
                .arg(QString::fromLatin1(kGuestLog), QString::fromLatin1(kGuestWvKeys),
                     QString::fromLatin1(kGuestWvPcap)),
            10000);
        if (out) {
            const QStringList vals = out.value().outStr().split(QLatin1Char('\n'));
            const auto get = [&vals](int i) -> qint64 {
                return i < vals.size() ? vals.at(i).trimmed().toLongLong() : 0;
            };
            const qint64 logSize = get(0);
            const qint64 alive = get(1);
            sh->wvKeys.store(quint64(std::max<qint64>(0, get(2))));
            sh->wvPcap.store(quint64(std::max<qint64>(0, get(3))));
            sh->logBytes.store(logSize);
            QString note = QStringLiteral("ecapture=%1 log=%2B").arg(alive).arg(logSize);
            const auto err = sh->adb.shellBlocking(
                sh->serial,
                QStringLiteral("grep -acE 'ring buffer|lost sample|perf event.*lost|events "
                               "lost' %1 2>/dev/null | wc -l")
                    .arg(QString::fromLatin1(kGuestErr)),
                10000);
            if (err) {
                const qint64 drops = err.value().outStr().trimmed().toLongLong();
                if (drops > 0)
                    note += QStringLiteral(" ⚠丢包告警×%1").arg(drops);
            }
            sh->setNote(note);
        }
    }
}

// WebView 密文侧终局：从 guest 拉 pcap + 密钥日志 → 宿主解密 → 入库。
// 空产物（无 WebView 流量/密钥未生成）静默跳过。
void Session::ingestWebviewFlows(const QString& serial, Shared* sh)
{
    mw::adb::AdbClient adb;
    QByteArray pcap, keys;
    if (const auto p = adb.pullFile(serial, QString::fromLatin1(kGuestWvPcap)))
        pcap = p.value();
    if (const auto k = adb.pullFile(serial, QString::fromLatin1(kGuestWvKeys)))
        keys = k.value();
    if (pcap.size() < 24 || keys.isEmpty())
        return;
    const QVector<Flow> done = webviewFlowsFromPcap(pcap, keys);
    if (done.isEmpty())
        return;
    MW_LOG("capture", QStringLiteral("WebView 解密入库：%1 条 flow").arg(done.size()));
    QMutexLocker l(&sh->storeMutex);
    const auto r = sh->store.upsertFlows(done);
    if (!r)
        qWarning().noquote()
            << QStringLiteral("[mwine-capture] WebView 解密入库失败：%1").arg(r.error());
}

// ---------------------------------------------------------------------------
// Session

Session::Session()
    : dbPath_(QDir(mw::mwineDir()).filePath(QStringLiteral("capture/flows.db")))
{
}

Session::~Session()
{
    if (running_) {
        // 守护进程未调 stop 的兜底：先收线程再析构共享态
        stop();
    }
}

QString Session::findHostBinary()
{
    if (QCoreApplication::instance()) {
        const QString dir = QCoreApplication::applicationDirPath();
        if (!dir.isEmpty()) {
            // 可执行文件同目录（开发/裸二进制场景）
            const QString beside = QDir(dir).filePath(QStringLiteral("ecapture"));
            if (QFileInfo(beside).isFile())
                return beside;
            // app bundle：Contents/MacOS → Contents/Resources/bin
            const QString resources =
                QDir(QDir(dir).filePath(QStringLiteral("../Resources/bin")))
                    .filePath(QStringLiteral("ecapture"));
            if (QFileInfo(resources).isFile())
                return QDir::cleanPath(resources);
        }
    }
    const QString homeBin = QDir(mw::mwineDir()).filePath(QStringLiteral("bin/ecapture"));
    if (QFileInfo(homeBin).isFile())
        return homeBin;
    return QString();
}

void Session::setDbPath(const QString& path)
{
    QMutexLocker l(&mutex_);
    if (running_)
        return;
    dbPath_ = path;
}

// ---------------------------------------------------------------------------
// 设备模式：部署前置与包名富化

mw::Result<bool> Session::ensureReady(const QString& serial)
{
    using R = mw::Result<bool>;
    mw::adb::AdbClient adb;
    const auto rooted = [](const QString& s) { return s.contains(QLatin1String("uid=0")); };
    QString id;
    if (const auto r = adb.shellBlocking(serial, QStringLiteral("id"), 10000))
        id = r.value().outStr();
    if (!rooted(id)) {
        adb.root(serial);
        QThread::sleep(2);
        QString id2;
        if (const auto r = adb.shellBlocking(serial, QStringLiteral("id"), 10000))
            id2 = r.value().outStr();
        if (!rooted(id2))
            return R::Err(QStringLiteral(
                "guest 未 root：adb root 后仍非 root（user 镜像不支持采集，需 userdebug 镜像）"));
    }
    bool have = false;
    if (const auto r = adb.shellBlocking(
            serial, QStringLiteral("ls %1").arg(QString::fromLatin1(kGuestBin)), 10000))
        have = r.value().ok();
    if (!have) {
        const QString bin = findHostBinary();
        if (bin.isEmpty())
            return R::Err(QStringLiteral(
                "流量分析组件未安装：请在流量分析页按提示下载 ecapture（约 13MB）"));
        QFile f(bin);
        if (!f.open(QIODevice::ReadOnly))
            return R::Err(QStringLiteral("IO 错误：无法读取 %1").arg(bin));
        const QByteArray data = f.readAll();
        const auto pushed = adb.pushBytes(serial, data, QString::fromLatin1(kGuestBin));
        if (!pushed)
            return R::Err(QStringLiteral("ADB 错误：%1").arg(pushed.error()));
        adb.shellBlocking(serial,
                          QStringLiteral("chmod 755 %1").arg(QString::fromLatin1(kGuestBin)),
                          10000);
    }
    return R::Ok();
}

QHash<QString, quint32> Session::packageUids(const QString& serial)
{
    QHash<QString, quint32> map;
    mw::adb::AdbClient adb;
    const auto out = adb.shellBlocking(serial, QStringLiteral("pm list packages -U"), 20000);
    if (!out)
        return map;
    const QStringList lines = out.value().outStr().split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        // package:com.xxx uid:10123
        if (!line.startsWith(QLatin1String("package:")))
            continue;
        const QString rest = line.mid(QStringLiteral("package:").size());
        const int sp = rest.indexOf(QLatin1String(" uid:"));
        if (sp < 0)
            continue;
        bool ok = false;
        const quint32 uid = rest.mid(sp + 5).trimmed().toUInt(&ok);
        if (ok)
            map.insert(rest.left(sp).trimmed(), uid);
    }
    return map;
}

QMap<quint32, QString> Session::pidPackages(const QString& serial)
{
    const QHash<QString, quint32> pkgUids = packageUids(serial);
    QHash<quint32, QString> uidPkg;
    for (auto it = pkgUids.cbegin(); it != pkgUids.cend(); ++it) {
        if (!uidPkg.contains(it.value()))
            uidPkg.insert(it.value(), it.key()); // 同 uid 多包：首个保留
    }
    QMap<quint32, QString> out;
    mw::adb::AdbClient adb;
    const auto ps = adb.shellBlocking(serial, QStringLiteral("ps -A -o PID,UID,NAME"), 20000);
    if (!ps)
        return out;
    const QStringList lines = ps.value().outStr().split(QLatin1Char('\n'));
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList t = lines.at(i).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (t.size() < 2)
            continue;
        bool okPid = false, okUid = false;
        const quint32 pid = t.at(0).toUInt(&okPid);
        const quint32 uid = t.at(1).toUInt(&okUid);
        if (!okPid || !okUid)
            continue;
        const auto p = uidPkg.constFind(uid);
        if (p != uidPkg.constEnd())
            out.insert(pid, p.value());
    }
    return out;
}

// ---------------------------------------------------------------------------
// 启停

mw::Result<bool> Session::start(const QString& serial, const TargetSpec& targets)
{
    QMutexLocker l(&mutex_);
    if (running_)
        return mw::Result<bool>::Err(QStringLiteral("会话已在运行"));
    const QString bin = findHostBinary();
    if (bin.isEmpty()) {
        lastError_ = QStringLiteral(
                         "流量分析组件未安装：请在流量分析页按提示下载 ecapture（约 13MB）")
                         + QStringLiteral("（安装位 %1）")
                               .arg(QDir(mw::mwineDir()).filePath(QStringLiteral("bin/ecapture")));
        return mw::Result<bool>::Err(lastError_);
    }
    auto st = Store::open(dbPath_);
    if (!st) {
        lastError_ = st.error();
        return mw::Result<bool>::Err(lastError_);
    }
    shared_.reset(new Shared);
    shared_->store = std::move(st.value());
    shared_->serial = serial;
    running_ = true;
    deviceMode_ = false;
    serial_ = serial;
    targets_ = targets.packages;
    lastError_.clear();
    collector_ = std::thread([sh = shared_.data(), bin] { collectorLoop(sh, bin); });
    watchdog_ = std::thread([sh = shared_.data()] { watchdogLoop(sh); });
    return mw::Result<bool>::Ok();
}

mw::Result<bool> Session::startOnDevice(const QString& serial, const TargetSpec& targets)
{
    {
        QMutexLocker l(&mutex_);
        if (running_)
            return mw::Result<bool>::Err(QStringLiteral("会话已在运行"));
        running_ = true; // 预占（guest 侧准备耗时数秒，期间 status() 可见）
    }
    const auto r = startOnDeviceUnlocked(serial);
    QMutexLocker l(&mutex_);
    if (!r) {
        running_ = false;
        lastError_ = r.error();
        return mw::Result<bool>::Err(r.error());
    }
    deviceMode_ = true;
    serial_ = serial;
    targets_ = targets.packages;
    lastError_.clear();
    return r;
}

// guest 侧准备（root/清理/拉起/WebView 旁观）→ 建 Shared 与线程。
// 不持 mutex_（adb 阻塞调用可达 10s+）；running_ 预占保证互斥。
mw::Result<bool> Session::startOnDeviceUnlocked(const QString& serial)
{
    using R = mw::Result<bool>;
    {
        const auto ready = ensureReady(serial);
        if (!ready)
            return ready;
    }
    mw::adb::AdbClient adb;
    // 清旧进程与旧日志
    adb.shellBlocking(serial, QStringLiteral("pkill -f 'ecapture tls'"), 5000);
    QThread::msleep(300);
    adb.shellBlocking(serial,
                      QStringLiteral("rm -f %1 %2; touch %1")
                          .arg(QString::fromLatin1(kGuestLog), QString::fromLatin1(kGuestErr)),
                      5000);
    // 拉起（guest 侧 nohup 孤儿化，adb 断开不退出）
    const QString startCmd =
        QStringLiteral("nohup %1 tls --libssl=%2 --eventaddr=%3 >%4 2>&1 &")
            .arg(QString::fromLatin1(kGuestBin), QString::fromLatin1(kConscryptLibssl),
                 QString::fromLatin1(kGuestLog), QString::fromLatin1(kGuestErr));
    const auto started = adb.shellBlocking(serial, startCmd, 10000);
    if (!started)
        return R::Err(QStringLiteral("ADB 错误：%1").arg(started.error()));
    QThread::sleep(3);
    bool alive = false;
    if (const auto c = adb.shellBlocking(
            serial, QStringLiteral("ps -A | grep -c '[e]capture'"), 10000))
        alive = c.value().outStr().trimmed().toUInt() > 0;
    if (!alive) {
        QString errTail;
        if (const auto t = adb.shellBlocking(
                serial, QStringLiteral("tail -5 %1").arg(QString::fromLatin1(kGuestErr)), 10000))
            errTail = t.value().outStr();
        return R::Err(QStringLiteral("ecapture 启动失败：%1").arg(errTail));
    }

    // WebView 旁观（纯被动）：Chromium 命令行开关 + 全量 pcap。
    // 实测补齐（缺一密钥文件不会生成）：
    //   a) /data/local/tmp 放开 app 写权限（默认 shell 属主，app 无法创建文件）；
    //   b) SELinux 切 permissive（Enforcing 下 untrusted_app 写 shell_data_file
    //      被拒，实测 chromium 报 "Failed opening SSL key log file"）；
    //   c) 杀掉存量 WebView 实例——network service 只在启动时打开一次密钥文件；
    //   d) 密钥文件由 root 预建为 666——否则首个 app 以 600 创建后其他 app 的
    //      chromium 打不开；
    //   e) pkill 必须与 tcpdump 启动分离——`pkill -f <pattern>` 会匹配到执行
    //      命令的 adb shell 自身（命令串含 pattern）导致整链自杀，这曾是
    //      "tcpdump 起不来/adb 间歇失联"的根因。
    adb.shellBlocking(
        serial,
        QStringLiteral(
            "chmod 777 /data/local/tmp; setenforce 0; "
            "echo '_ --ssl-key-log-file=%1 --disable-quic' > %2 && chmod 644 %2; "
            "rm -f %1; touch %1; chmod 666 %1; rm -f %3")
            .arg(QString::fromLatin1(kGuestWvKeys), QString::fromLatin1(kGuestWvCmdline),
                 QString::fromLatin1(kGuestWvPcap)),
        10000);
    // 重启存量 WebView 实例（独立调用：允许这条 shell 被 pkill 波及自终）
    adb.shellBlocking(serial,
                      QStringLiteral("pkill -f com.android.webview; pkill -f webview_zygote"),
                      5000);
    // 全量 pcap（独立调用，串内不含任何 pkill 模式）
    adb.shellBlocking(
        serial,
        QStringLiteral("nohup tcpdump -i any -s 0 -U -w %1 >/dev/null 2>&1 & sleep 1")
            .arg(QString::fromLatin1(kGuestWvPcap)),
        10000);

    auto st = Store::open(dbPath_);
    if (!st)
        return R::Err(st.error());
    shared_.reset(new Shared);
    shared_->store = std::move(st.value());
    shared_->device = true;
    shared_->serial = serial;
    collector_ = std::thread([sh = shared_.data()] { deviceCollectorLoop(sh); });
    watchdog_ = std::thread([sh = shared_.data()] { deviceWatchdogLoop(sh); });
    return R::Ok();
}

mw::Result<bool> Session::stop()
{
    bool device = false;
    QString serial;
    {
        QMutexLocker l(&mutex_);
        if (!running_)
            return mw::Result<bool>::Err(QStringLiteral("会话未在运行"));
        running_ = false;
        device = deviceMode_;
        serial = serial_;
    }
    if (shared_) {
        shared_->stop = true;
        if (device) {
            // 解除 tail 会话阻塞读（对 dup 出的所有句柄生效）
            QMutexLocker t(&shared_->tailMutex);
            if (shared_->tailCtrl)
                shared_->tailCtrl->shutdownBoth();
        }
        if (collector_.joinable())
            collector_.join();
        if (watchdog_.joinable())
            watchdog_.join();
        if (device) {
            mw::adb::AdbClient adb;
            adb.shellBlocking(serial, QStringLiteral("pkill -f 'ecapture tls'"), 5000);
            // WebView 旁观收尾：停 pcap、移除开关（密钥/pcap 文件保留，可导出）
            adb.shellBlocking(
                serial,
                QStringLiteral("pkill -x tcpdump; rm -f %1")
                    .arg(QString::fromLatin1(kGuestWvCmdline)),
                5000);
            // WebView 密文侧终局：拉 pcap + 密钥 → 宿主解密 → 明文 flows 入库
            //（ChromiumNet 静态 BoringSSL 挂不上钩子的流量，从这里补齐）
            ingestWebviewFlows(serial, shared_.data());
        }
        QMutexLocker l(&mutex_);
        shared_.reset();
    }
    return mw::Result<bool>::Ok();
}

// ---------------------------------------------------------------------------
// 查询面

QJsonObject Session::status() const
{
    QJsonObject st;
    {
        QMutexLocker l(&mutex_);
        st.insert(QStringLiteral("running"), running_);
        st.insert(QStringLiteral("serial"), serial_);
        {
            QJsonArray arr;
            for (const QString& t : targets_)
                arr.append(t);
            st.insert(QStringLiteral("targets"), arr);
        }
        st.insert(QStringLiteral("db_path"), dbPath_);
        st.insert(QStringLiteral("last_error"), lastError_);
        st.insert(QStringLiteral("events_parsed"), qint64(0));
        st.insert(QStringLiteral("bytes_seen"), qint64(0));
        st.insert(QStringLiteral("collector_lag_bytes"), qint64(0));
        st.insert(QStringLiteral("flows_live"), qint64(0));
        st.insert(QStringLiteral("flows_stored"), qint64(0));
        st.insert(QStringLiteral("guest_note"), QString());
        st.insert(QStringLiteral("connections"), qint64(0));
        st.insert(QStringLiteral("empty_connect_comms"), QJsonArray());
        st.insert(QStringLiteral("webview_keys_bytes"),
                  qint64(running_ && shared_ ? shared_->wvKeys.load() : quint64(0)));
        st.insert(QStringLiteral("webview_pcap_bytes"),
                  qint64(running_ && shared_ ? shared_->wvPcap.load() : quint64(0)));

        if (running_ && shared_) {
            st.insert(QStringLiteral("events_parsed"), qint64(shared_->events.load()));
            st.insert(QStringLiteral("bytes_seen"), qint64(shared_->bytes.load()));
            st.insert(QStringLiteral("collector_lag_bytes"),
                      std::max<qint64>(0, shared_->logBytes.load()
                                               - qint64(shared_->bytes.load())));
            {
                QMutexLocker n(&shared_->noteMutex);
                st.insert(QStringLiteral("guest_note"), shared_->guestNote);
            }
            {
                QMutexLocker r(&shared_->reasmMutex);
                st.insert(QStringLiteral("connections"),
                          qint64(shared_->reasm.connectsTotal()));
                st.insert(QStringLiteral("flows_live"), qint64(shared_->reasm.liveCount()));
            }
            {
                QMutexLocker e(&shared_->emptyMutex);
                st.insert(QStringLiteral("empty_connect_comms"),
                          QJsonArray::fromStringList(topEmptyComms(shared_->emptyComms)));
            }
            {
                QMutexLocker s(&shared_->storeMutex);
                const auto n = shared_->store.count();
                if (n)
                    st.insert(QStringLiteral("flows_stored"), qint64(n.value()));
            }
        } else {
            if (auto s = Store::open(dbPath_)) {
                const auto n = s.value().count();
                if (n)
                    st.insert(QStringLiteral("flows_stored"), qint64(n.value()));
            }
        }
    }
    return st;
}

mw::Result<QVector<QJsonObject>> Session::queryFlows(const FlowsQuery& q) const
{
    QMutexLocker l(&mutex_);
    if (running_ && shared_) {
        QMutexLocker s(&shared_->storeMutex);
        auto r = shared_->store.querySummaries(q);
        if (!r)
            return mw::err<QVector<QJsonObject>>(r.error());
        QVector<QJsonObject> out;
        for (const FlowSummary& sum : r.value())
            out.push_back(sum.toJson());
        return mw::Result<QVector<QJsonObject>>::Ok(std::move(out));
    }
    auto s = Store::open(dbPath_);
    if (!s)
        return mw::err<QVector<QJsonObject>>(s.error());
    auto r = s.value().querySummaries(q);
    if (!r)
        return mw::err<QVector<QJsonObject>>(r.error());
    QVector<QJsonObject> out;
    for (const FlowSummary& sum : r.value())
        out.push_back(sum.toJson());
    return mw::Result<QVector<QJsonObject>>::Ok(std::move(out));
}

mw::Result<quint64> Session::totalFlows(const FlowsQuery& q) const
{
    QMutexLocker l(&mutex_);
    if (running_ && shared_) {
        QMutexLocker s(&shared_->storeMutex);
        return shared_->store.countFiltered(q);
    }
    auto s = Store::open(dbPath_);
    if (!s)
        return mw::err<quint64>(s.error());
    return s.value().countFiltered(q);
}

mw::Result<QJsonObject> Session::flowDetail(qint64 id) const
{
    QMutexLocker l(&mutex_);
    if (running_ && shared_) {
        QMutexLocker s(&shared_->storeMutex);
        return shared_->store.getDetail(id);
    }
    auto s = Store::open(dbPath_);
    if (!s)
        return mw::err<QJsonObject>(s.error());
    return s.value().getDetail(id);
}

QVector<QJsonObject> Session::liveFlows() const
{
    QMutexLocker l(&mutex_);
    if (!(running_ && shared_))
        return {};
    QMutexLocker r(&shared_->reasmMutex);
    const QVector<Flow> flows = shared_->reasm.liveFlows();
    QVector<QJsonObject> out;
    for (const Flow& f : flows)
        out.push_back(FlowSummary::from(f).toJson());
    return out;
}

QJsonObject Session::stats() const
{
    QMutexLocker l(&mutex_);
    if (running_ && shared_) {
        QMutexLocker s(&shared_->storeMutex);
        const auto v = shared_->store.kindStats();
        if (v)
            return v.value();
    }
    if (auto s = Store::open(dbPath_)) {
        const auto v = s.value().kindStats();
        if (v)
            return v.value();
    }
    return QJsonObject {
        { QStringLiteral("total"), qint64(0) },
        { QStringLiteral("parsed"), qint64(0) },
        { QStringLiteral("parse_rate"), 0.0 },
        { QStringLiteral("kinds"), QJsonObject() },
    };
}

mw::Result<bool> Session::clear() const
{
    QMutexLocker l(&mutex_);
    if (running_ && shared_) {
        QMutexLocker s(&shared_->storeMutex);
        return shared_->store.clear();
    }
    auto s = Store::open(dbPath_);
    if (!s)
        return mw::Result<bool>::Err(s.error());
    return s.value().clear();
}

// ---------------------------------------------------------------------------
// WebView 旁观包导出

mw::Result<QString> Session::exportWebviewBundle() const
{
    using R = mw::Result<QString>;
    QString serial;
    {
        QMutexLocker l(&mutex_);
        if (serial_.isEmpty())
            return R::Err(QStringLiteral("会话未在运行"));
        serial = serial_;
    }
    const QString dir = QDir(mw::mwineDir())
                            .filePath(QStringLiteral("capture/webview-%1")
                                          .arg(chronoLikeTs()));
    if (!QDir().mkpath(dir))
        return R::Err(QStringLiteral("无法创建导出目录 %1").arg(dir));
    mw::adb::AdbClient adb;
    // 密钥/pcap 文件可能尚未生成（还没有 WebView 流量）——空文件占位
    QByteArray keys, pcap;
    if (const auto k = adb.pullFile(serial, QString::fromLatin1(kGuestWvKeys)))
        keys = k.value();
    if (const auto p = adb.pullFile(serial, QString::fromLatin1(kGuestWvPcap)))
        pcap = p.value();
    if (const auto w = mw::writeFile(QDir(dir).filePath(QStringLiteral("sslkeys.log")), keys); !w)
        return R::Err(w.error());
    if (const auto w = mw::writeFile(QDir(dir).filePath(QStringLiteral("capture.pcap")), pcap); !w)
        return R::Err(w.error());
    // 附使用说明（Wireshark 打开 pcap，TLS 配置里选 sslkeys.log）
    if (const auto w = mw::writeFile(
            QDir(dir).filePath(QStringLiteral("README.txt")),
            QByteArray("Wireshark 打开 capture.pcap → 设置 → Protocols → TLS → "
                       "(Pre)-Master-Secret log filename 选择 sslkeys.log\n"
                       "或命令行：tshark -r capture.pcap -o tls.keylog_file:sslkeys.log -Y "
                       "http\n"));
        !w)
        return R::Err(w.error());
    return R::Ok(dir);
}

} // namespace mw::capture
