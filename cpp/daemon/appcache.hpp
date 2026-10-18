// 应用注册表 + 图标缓存（按 AVD 键控）：
//  - ~/.mwine/appcache/<avd>/apps.json     应用注册表（label/图标有无/首末次见到）
//  - ~/.mwine/appcache/<avd>/icons/*.png    图标缓存（PNG 原样，非 PNG 回退字母块）
//  - VM 重置（vm_reset）→ 销毁当前 AVD 的整份缓存；切镜像 → AVD 改名 → 天然新键
//  - 图标抓取在独立工作线程串行执行：pm path → 拉 APK → zip 内取图标 → 落盘
#pragma once

#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QVector>
#include <atomic>
#include <thread>

#include "adb.hpp"

namespace mw::daemon {

class AppCache {
public:
    explicit AppCache(QString avdKey);
    ~AppCache();

    // 应用清单同步（apps 路由每次列表后调用；后台线程补图标/标签）
    void syncApps(const QVector<QString>& packages);
    // 查询缓存描述：{"label": "...", "icon": "/abs/path.png"}（无则字段为空）
    QJsonObject describe(const QString& pkg) const;
    // 壁纸缓存路径（不存在返回空串）；壁纸变化自动重抓（wallpaper_info.xml 指纹）
    QString wallpaperPath() const;
    // 销毁当前 VM 的全部应用记录与图标（vm_reset 调用）
    void wipe();
    // 停止工作线程
    void shutdown();

    QString iconsDir() const;

private:
    void workerLoop();
    bool processOne(const QString& pkg); // 返回是否成功产出（图标或标签）
    void loadRegistry();
    void saveRegistry();

    QString m_dir;            // ~/.mwine/appcache/<avd>
    QString m_avd;

    mutable QMutex m_mu;
    QJsonObject m_registry;   // {pkg: {label,icon,firstSeen,lastSeen}}
    QVector<QString> m_queue; // 待抓取（含 "__wallpaper__" 伪任务）
    QString m_wallpaperStamp; // wallpaper_info.xml 的指纹（mtime+size）
    std::atomic<qint64> m_lastWallpaperAt{0}; // 上次壁纸抓取尝试（限速重试）
    QSet<QString> m_failed;   // 已尝试失败（本轮会话不再重试）

    std::atomic_bool m_running{true};
    std::thread m_worker;
};

} // namespace mw::daemon
