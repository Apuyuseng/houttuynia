// 快照存档点管理（console + 文件系统）← lib.rs（latest_snapshot / save_snapshot_* /
// snapshot_load / snapshot_delete / avd_snapshot_names / save_snapshot_blocking）
#include "supervisor.hpp"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <sys/stat.h>

#include "adb_avail.hpp"
#include "fs.hpp"

namespace mw::supervisor {

static QString homeDir()
{
    const QString h = mw::homeDir();
    return h.isEmpty() ? QStringLiteral("/Users/Shared") : h;
}

std::optional<QString> latestSnapshot(const AvdInfo& avd)
{
    const QDir dir(QDir(avd.path).filePath(QStringLiteral("snapshots")));
    // 优先产品基准档 mwine_default（vm stop 存档、vm start 加载的固定闭环）
    if (QFileInfo::exists(dir.filePath(QStringLiteral("mwine_default/snapshot.pb"))))
        return QStringLiteral("mwine_default");
    // 否则取最新修改的一个
    std::optional<QDateTime> bestTime;
    QString best;
    const auto entries =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& e : entries) {
        const QDateTime m = e.lastModified();
        if (!bestTime || m > *bestTime) {
            bestTime = m;
            best = e.fileName();
        }
    }
    if (best.isEmpty())
        return std::nullopt;
    return best;
}

QVector<QPair<QString, qint64>> avdSnapshotNames(const QString& avdDir)
{
    QVector<QPair<QString, qint64>> out;
    const QDir dir(QDir(avdDir).filePath(QStringLiteral("snapshots")));
    const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& e : entries) {
        // 快照目录以 snapshot.pb 为完整标志
        if (!QFileInfo::exists(QDir(e.absoluteFilePath())
                                   .filePath(QStringLiteral("snapshot.pb"))))
            continue;
        out.push_back({ e.fileName(), e.lastModified().toMSecsSinceEpoch() });
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; }); // 新的在前
    return out;
}

mw::Result<bool> saveSnapshotAvd(const QString& serial, const QString& avdDir,
                                 const QString& avdName, const QString& snapName, int timeoutMs)
{
    const QDir mark(QDir(avdDir).filePath(QStringLiteral("snapshots/%1").arg(snapName)));
    const QString markPb = mark.filePath(QStringLiteral("snapshot.pb"));
    std::optional<QDateTime> before;
    if (QFileInfo::exists(markPb))
        before = QFileInfo(markPb).lastModified();

    // stderr 日志里已出现过的保存完成行数（本次保存 = 新增一行）
    const QString markerLine = QStringLiteral("Saving snapshot '%1' using").arg(snapName);
    auto savedMarker = [&markerLine](const QByteArray& body) {
        int hits = 0;
        for (const QByteArray& l : body.split('\n'))
            if (l.contains(markerLine.toUtf8()))
                ++hits;
        return hits;
    };
    const QString logPath = QDir(homeDir())
                                .filePath(QStringLiteral(".mwine/logs/emu-%1.log").arg(avdName));
    int marker = 0;
    {
        QFile lf(logPath);
        if (lf.open(QIODevice::ReadOnly)) {
            marker = savedMarker(lf.readAll());
            lf.close();
        }
    }

    auto resp = emuConsoleCmdLarge(
        serial, QStringLiteral("avd snapshot save %1").arg(snapName));
    if (!resp)
        return mw::err<bool>(resp.error());
    if (resp.value().contains("KO"))
        return mw::err<bool>(QStringLiteral("模拟器拒绝保存快照: %1")
                                 .arg(QString::fromUtf8(resp.value()).trimmed()));
    // `avd snapshot save` 是同步命令：console 应答 OK 时保存已落盘（emu 36.5
    // 实测增量保存亚秒完成，且不再打印 "Saving snapshot 'name' using" 日志行、
    // mtime 可能与上次同秒——marker/mtime 判定在同步语义下必然误报超时）。
    // 应答 OK + pb 存在即成功；pb 缺失（老版本异步语义）才走 mtime 轮询兜底。
    for (int i = 0; i < 4; ++i) {
        if (QFileInfo::exists(markPb))
            return mw::Result<bool>::Ok();
        QThread::msleep(500);
    }
    if (QFileInfo::exists(markPb))
        return mw::Result<bool>::Ok();

    QElapsedTimer t0;
    t0.start();
    std::optional<QDateTime> lastMtime = before;
    int stableRounds = 0;
    while (t0.elapsed() < qint64(timeoutMs)) {
        QThread::msleep(500);
        {
            QFile lf(logPath);
            if (lf.open(QIODevice::ReadOnly)) {
                const int hits = savedMarker(lf.readAll());
                lf.close();
                if (hits > marker)
                    return mw::Result<bool>::Ok();
            }
        }
        if (QFileInfo::exists(markPb)) {
            const QDateTime m = QFileInfo(markPb).lastModified();
            qWarning().noquote() << "[snapshot-save] pb=" << markPb << "m=" << m.toString()
                                 << "before=" << (before ? before->toString() : QStringLiteral("-"))
                                 << "rounds=" << stableRounds;
            const bool advanced = !lastMtime || m > *lastMtime;
            if (advanced) {
                lastMtime = m;
                stableRounds = 0;
            } else if (!before || m > *before) {
                stableRounds += 1;
                if (stableRounds >= 2)
                    return mw::Result<bool>::Ok();
            }
        } else {
            qWarning().noquote() << "[snapshot-save] pb 不存在:" << markPb;
        }
    }
    return mw::err<bool>(QStringLiteral("快照 %1 保存等待超时").arg(snapName));
}

mw::Result<bool> saveSnapshotBlocking(const QString& serial, const QString& name, int timeoutMs)
{
    // 双主目录（bundled=Application Support / 开发态=~/.android/avd）都扫
    QVector<QString> homes;
    if (auto sdk = Sdk::detect(); sdk)
        homes << sdk.value().avdHome();
    homes << QDir(homeDir()).filePath(QStringLiteral(".android/avd"))
          << QDir(homeDir()).filePath(QStringLiteral("Library/Application Support/MWine/avd"));

    // AVD 目录解析：console `avd name` 直接问当前 VM（唯一权威——多 AVD 共存时
    // 按"已含快照目录"猜会命中别的 AVD 的旧档（实测 Android28 的六天前旧档），
    // 判定盯错文件 → 永远超时误报"存档失败"，而实际保存每次都成功）
    std::optional<QString> exactDir;
    {
        auto r = emuConsoleCmd(serial, QStringLiteral("avd name"));
        if (r.ok()) {
            const QStringList lines = QString::fromUtf8(r.value())
                                          .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString& l : lines) {
                const QString cand = l.trimmed();
                if (cand.isEmpty() || cand == QLatin1String("OK")
                    || cand.startsWith(QLatin1String("Android Console"))
                    || cand.contains(QLatin1Char(':')))
                    continue;
                for (const QString& h : homes) {
                    const QString d = QDir(h).filePath(cand + QStringLiteral(".avd"));
                    if (QFileInfo::exists(QDir(d).filePath(QStringLiteral("config.ini")))) {
                        exactDir = d;
                        break;
                    }
                }
                if (exactDir)
                    break;
            }
        }
    }

    // 优先：console 报告的当前 AVD；退化：已含该快照目录的 AVD；
    // 再退化：最近改动过的 .avd（快照首次保存时目录尚不存在）
    std::optional<QString> bestHit;
    std::optional<QString> bestAny;
    qint64 bestAnyMtime = 0;
    for (const QString& h : homes) {
        const QDir dir(h);
        const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& e : entries) {
            const QString d = e.absoluteFilePath();
            if (!QFileInfo::exists(QDir(d).filePath(QStringLiteral("config.ini"))))
                continue;
            if (QFileInfo::exists(QDir(d).filePath(
                    QStringLiteral("snapshots/%1/snapshot.pb").arg(name)))) {
                if (!exactDir || d == *exactDir) {
                    bestHit = d;
                    break;
                }
            }
            const qint64 m = e.lastModified().toMSecsSinceEpoch();
            if (!bestAny || m > bestAnyMtime) {
                bestAny = d;
                bestAnyMtime = m;
            }
        }
        if (bestHit)
            break;
    }
    const QString avdDir = exactDir ? *exactDir
                                    : (bestHit ? *bestHit
                                               : (bestAny ? *bestAny : QString()));
    if (avdDir.isEmpty())
        return mw::err<bool>(QStringLiteral("两台主目录均无 AVD，无法存档"));
    QString avdName = QFileInfo(avdDir).fileName();
    if (avdName.endsWith(QStringLiteral(".avd")))
        avdName = avdName.chopped(4);
    return saveSnapshotAvd(serial, avdDir, avdName, name, timeoutMs);
}

mw::Result<bool> loadSnapshotAvd(const QString& serial, const QString& avdDir,
                                 const QString& avdName, const QString& snapName, int timeoutMs)
{
    Q_UNUSED(avdDir)
    Q_UNUSED(avdName)
#if MW_SUPERVISOR_ADB_READY
    auto resp = emuConsoleCmdLarge(
        serial, QStringLiteral("avd snapshot load %1").arg(snapName));
    if (!resp)
        return mw::err<bool>(resp.error());
    if (resp.value().contains("KO"))
        return mw::err<bool>(
            QStringLiteral("加载被拒: %1").arg(QString::fromUtf8(resp.value()).trimmed()));
    // 加载期间 adb 会短暂失联，轮询到 boot_completed 为止（契约无 adb 参数，
    // 用默认 AdbClient（127.0.0.1:5037）轮询）
    mw::adb::AdbClient adb;
    QElapsedTimer t0;
    t0.start();
    while (t0.elapsed() < qint64(timeoutMs)) {
        QThread::msleep(1000);
        auto r = adb.shellBlocking(serial, QStringLiteral("getprop sys.boot_completed"), 5000);
        if (r && adbOutText(r.value()).trimmed() == QStringLiteral("1"))
            return mw::Result<bool>::Ok();
    }
    return mw::err<bool>(QStringLiteral("快照加载后等待系统就绪超时"));
#else
    Q_UNUSED(serial)
    Q_UNUSED(snapName)
    Q_UNUSED(timeoutMs)
    return mw::err<bool>(QStringLiteral(kAdbPendingMsg));
#endif
}

mw::Result<bool> removeSnapshot(const QString& avdName, const QString& snapName)
{
    // 按 avdName 反查 AVD 实体目录（双主目录），删 snapshots/<snapName>（文件系统侧）
    QVector<QString> homes;
    if (auto sdk = Sdk::detect(); sdk)
        homes << sdk.value().avdHome();
    homes << QDir(homeDir()).filePath(QStringLiteral(".android/avd"))
          << QDir(homeDir()).filePath(QStringLiteral("Library/Application Support/MWine/avd"));

    for (const QString& h : homes) {
        const QString avdDir = QDir(h).filePath(avdName + QStringLiteral(".avd"));
        const QString snapDir =
            QDir(avdDir).filePath(QStringLiteral("snapshots/%1").arg(snapName));
        if (!QFileInfo::exists(snapDir))
            continue;
        if (!QDir(snapDir).removeRecursively())
            return mw::err<bool>(QStringLiteral("删除 %1 失败").arg(snapDir));
        return mw::Result<bool>::Ok();
    }
    return mw::err<bool>(QStringLiteral("未找到 AVD %1 的快照 %2").arg(avdName, snapName));
}

} // namespace mw::supervisor
