#include "appcache.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryFile>

#include "app.hpp"
#include "fs.hpp"
#include "log.hpp"

namespace mw::daemon {

static constexpr int kMaxQueue = 256;
static constexpr char wallpaperTask[] = "__wallpaper__";

// 壁纸任务上次尝试时刻（限速重试；atomic 成员可直接跨线程读写）


AppCache::AppCache(QString avdKey)
    : m_dir(QDir(mw::mwineDir()).filePath(QStringLiteral("appcache/") + avdKey)),
      m_avd(std::move(avdKey))
{
    QDir().mkpath(iconsDir());
    loadRegistry();
    m_worker = std::thread(&AppCache::workerLoop, this);
}

AppCache::~AppCache()
{
    shutdown();
}

QString AppCache::iconsDir() const
{
    return QDir(m_dir).filePath(QStringLiteral("icons"));
}

void AppCache::shutdown()
{
    if (!m_running.exchange(false))
        return;
    if (m_worker.joinable())
        m_worker.join();
}

void AppCache::loadRegistry()
{
    const auto bytes = mw::readFile(QDir(m_dir).filePath(QStringLiteral("apps.json")));
    if (bytes && !bytes.value().isEmpty()) {
        const auto doc = QJsonDocument::fromJson(bytes.value());
        if (doc.isObject()) {
            QMutexLocker l(&m_mu);
            m_registry = doc.object();
        }
    }
}

void AppCache::saveRegistry()
{
    QMutexLocker l(&m_mu);
    QFile f(QDir(m_dir).filePath(QStringLiteral("apps.json")));
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(m_registry).toJson(QJsonDocument::Compact));
}

QJsonObject AppCache::describe(const QString& pkg) const
{
    QMutexLocker l(&m_mu);
    const auto e = m_registry.value(pkg).toObject();
    QJsonObject out;
    out[QStringLiteral("label")] = e.value(QStringLiteral("label")).toString();
    if (e.value(QStringLiteral("icon")).toBool())
        out[QStringLiteral("icon")] = QDir(iconsDir()).filePath(pkg + QStringLiteral(".png"));
    else
        out[QStringLiteral("icon")] = QString();
    out[QStringLiteral("noicon")] = e.value(QStringLiteral("noicon")).toBool();
    return out;
}

QString AppCache::wallpaperPath() const
{
    const QString p = QDir(m_dir).filePath(QStringLiteral("wallpaper.png"));
    return QFileInfo::exists(p) ? p : QString();
}

void AppCache::syncApps(const QVector<QString>& packages)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool dirty = false;
    {
        QMutexLocker l(&m_mu);
        for (const auto& p : packages) {
            auto e = m_registry.value(p).toObject();
            if (e.isEmpty()) {
                e[QStringLiteral("firstSeen")] = double(now);
                dirty = true;
            }
            e[QStringLiteral("lastSeen")] = double(now);
            m_registry[p] = e;

            const bool hasIcon = e.value(QStringLiteral("icon")).toBool()
                                || QFileInfo::exists(QDir(iconsDir()).filePath(
                                       p + QStringLiteral(".png")));
            // noicon：已确认无可提取图标（抓取跑完无 PNG），不再反复入队
            const bool knownNoIcon = e.value(QStringLiteral("noicon")).toBool();
            if (!hasIcon && !knownNoIcon && !m_failed.contains(p)
                && !m_queue.contains(p) && m_queue.size() < kMaxQueue) {
                m_queue.append(p);
            }
        }
    }
    if (dirty)
        saveRegistry();
    else {
        // lastSeen 更新也持久化（低频，apps 轮询约 4s 一次，写盘可接受）
        saveRegistry();
    }

    // 壁纸指纹（su 可用时）：wallpaper_info.xml 变化或无缓存 → 排队重抓
    {
        const mw::adb::AdbClient adbProbe;
        QString stamp;
        if (const auto dev = adbProbe.findOnlineDevice()) {
            const auto st = adbProbe.shellBlocking(
                dev.value().serial,
                QStringLiteral("su 0 md5sum /data/system/users/0/wallpaper_info.xml 2>/dev/null | cut -d' ' -f1"),
                5000);
            if (st)
                stamp = st.value().outStr().trimmed();
        }
        const bool need = wallpaperPath().isEmpty()
                          || (!stamp.isEmpty() && stamp != m_wallpaperStamp);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool cooled = now - m_lastWallpaperAt > 30000; // 失败重试限速
        if (need && !stamp.isEmpty() && !m_queue.contains(wallpaperTask)
            && !m_failed.contains(wallpaperTask) && cooled) {
            QMutexLocker l(&m_mu);
            if (!m_queue.contains(wallpaperTask))
                m_queue.append(wallpaperTask);
            m_wallpaperStamp = stamp;
        }
    }
}

void AppCache::wipe()
{
    shutdown(); // 停 worker 再删目录
    QDir(m_dir).removeRecursively();
    QDir().mkpath(iconsDir());
    {
        QMutexLocker l(&m_mu);
        m_registry = QJsonObject{};
        m_queue.clear();
        m_failed.clear();
    }
    saveRegistry();
    MW_LOG("appcache", QStringLiteral("已销毁 %1 的应用记录与图标（VM 重置）").arg(m_avd));
    m_running.store(true);
    m_worker = std::thread(&AppCache::workerLoop, this);
}

void AppCache::workerLoop()
{
    while (m_running.load()) {
        QString pkg;
        {
            QMutexLocker l(&m_mu);
            if (m_queue.isEmpty()) {
                // 空闲等待：sleep + 标志检查
            } else {
                pkg = m_queue.takeFirst();
            }
        }
        if (pkg.isEmpty()) {
            struct timespec ts {0, 200 * 1000 * 1000}; // 200ms
            nanosleep(&ts, nullptr);
            continue;
        }
        if (!processOne(pkg) && pkg != QLatin1String(wallpaperTask)) {
            QMutexLocker l(&m_mu);
            m_failed.insert(pkg);
        }
    }
}

bool AppCache::processOne(const QString& pkg)
{
    const mw::adb::AdbClient adb;
    const auto serialR = adb.findOnlineDevice();
    if (!serialR)
        return false;

    // 壁纸伪任务：HOME 截屏 → 模糊 → wallpaper.png（子进程隔离）。
    // 失败不进 m_failed（避免整会话不再重试），由 30s 限速控制重试节奏。
    if (pkg == wallpaperTask) {
        const QString out = QDir(m_dir).filePath(QStringLiteral("wallpaper.png"));
        const QString helper = QCoreApplication::applicationDirPath()
                               + QStringLiteral("/mwine-iconfetch");
        if (QFileInfo::exists(helper)) {
            const bool ok = QProcess::execute(helper,
                                              { QStringLiteral("--wallpaper"),
                                                serialR.value().serial, out }) == 0;
            m_lastWallpaperAt = QDateTime::currentMSecsSinceEpoch();
            if (ok)
                return true;
        }
        return false; // worker 会把失败者放入 m_failed —— 壁纸任务在入队侧排除
    }

    bool needIcon = true, needLabel = true;
    {
        QMutexLocker l(&m_mu);
        const auto e = m_registry.value(pkg).toObject();
        needIcon = !e.value(QStringLiteral("icon")).toBool()
                   && !QFileInfo::exists(QDir(iconsDir()).filePath(
                          pkg + QStringLiteral(".png")));
        needLabel = e.value(QStringLiteral("label")).toString().isEmpty();
    }
    if (!needIcon && !needLabel)
        return true;

    // 子进程隔离抓取（解析器在守护进程内有堆损坏史；helper 与守护同目录）
    const QString helper = QCoreApplication::applicationDirPath()
                           + QStringLiteral("/mwine-iconfetch");
    if (!QFileInfo::exists(helper)) {
        MW_LOG("appcache", QStringLiteral("缺少 mwine-iconfetch 子进程工具"));
        return false;
    }
    const int rc = QProcess::execute(
        helper, { serialR.value().serial, pkg, iconsDir() });

    // 收割产物
    const QString iconPath = QDir(iconsDir()).filePath(pkg + QStringLiteral(".png"));
    const bool hasIcon = QFileInfo::exists(iconPath);
    QString label;
    {
        QFile lf(QDir(iconsDir()).filePath(pkg + QStringLiteral(".label")));
        if (lf.open(QIODevice::ReadOnly))
            label = QString::fromUtf8(lf.readAll().trimmed());
    }
    if (!hasIcon && label.isEmpty()) {
        // 产物全无（如 quicksearchbox：iconfetch 秒退 0 且零产物）：
        // 标记 noicon——应用库不再展示该应用，也不再反复入队。
        // rc!=0 的环境性失败同样落入此路径（误判概率低，可通过清
        // ~/.mwine/appcache 重建）
        {
            QMutexLocker l(&m_mu);
            auto e = m_registry.value(pkg).toObject();
            e[QStringLiteral("noicon")] = true;
            m_registry[pkg] = e;
        }
        saveRegistry();
        return false; // 仍进 m_failed：双保险，本会话不再重排
    }

    {
        QMutexLocker l(&m_mu);
        auto e = m_registry.value(pkg).toObject();
        if (!label.isEmpty())
            e[QStringLiteral("label")] = label;
        if (hasIcon) {
            e[QStringLiteral("icon")] = true;
            e.remove(QStringLiteral("noicon"));
        } else {
            // 抓取已跑完但无可提取的 PNG 图标（如 quicksearchbox 的
            // 自适应图标）：标记 noicon——应用库不再展示该应用
            e[QStringLiteral("noicon")] = true;
        }
        m_registry[pkg] = e;
    }
    saveRegistry();
    return true;
}

} // namespace mw::daemon
