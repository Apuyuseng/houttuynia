// F-Droid 源：index-v1.json 检索 + 版本列表 + 直链解析。
// ← crates/mwine-store/src/fdroid.rs（内部头文件，不对 daemon 暴露）。
#pragma once

#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

#include "../common/result.hpp"
#include "store.hpp"

namespace mw::store {

// Rust ResolvedDownload 的内部全量形态（公共 Resolved 是它的扁平子集）。
struct ResolvedInternal {
    QString url;
    std::optional<QString> sha256;
    std::optional<QString> md5;
    quint64 size = 0;
    StoreVersion version;
};

}  // namespace mw::store

namespace mw::store::fdroid {

// ================= 镜像 =================

// (id, 展示名, repo 基址)。auto 的尝试顺序 = 列表顺序（官方殿后）。
struct MirrorPreset {
    const char* id;
    const char* name;
    const char* url;
};
inline constexpr MirrorPreset MIRROR_PRESETS[] = {
    { "tuna", "清华 TUNA", "https://mirrors.tuna.tsinghua.edu.cn/fdroid/repo/" },
    { "nju", "南京大学", "https://mirror.nju.edu.cn/fdroid/repo/" },
    { "official", "F-Droid 官方", "https://f-droid.org/repo/" },
};

QVector<MirrorInfo> mirrors();

/// 把 `mirror` 选项解析为按序尝试的 (id, repo 基址) 列表。
/// - 空 / "auto"：全部预设（国内在前）
/// - 预设 id：单个
/// - `http(s)://` 开头：当作自定义 repo 基址（补尾部 /）
QVector<QPair<QString, QString>> mirrorBases(const QString& mirror);

// ================= 索引模型 =================

struct AppEntry {
    QString package;
    QString name;
    QString summary;
    QString icon;
    std::optional<QString> author;
    std::optional<QString> license;
};

struct VerEntry {
    QString name;
    quint64 code = 0;
    QString apkName;
    quint64 size = 0;
    QString hash;
    std::optional<quint32> minSdk;
    QVector<QString> nativeCode;
    quint64 addedMs = 0;
};

/// 精简后的进程内索引（不含长描述）。
struct FdroidIndex {
    QVector<AppEntry> apps;
    QHash<QString, QVector<VerEntry>> packages;
};

StoreVersion toStoreVersion(const VerEntry& v, bool isLatest);

/// 解析 index-v1.json 字节流（长描述等无关字段直接跳过）。
mw::Result<FdroidIndex> parseIndex(const QByteArray& jsonBytes);

// ================= 精选推荐 =================

// 推荐应用包名清单（人工精选，顺序即展示顺序）。
// 索引中缺失或无版本的条目在 recommend() 里自动跳过——更新清单无需同步代码。
inline constexpr const char* kCuratedPackages[] = {
    "org.mozilla.fennec_fdroid",       // Fennec 浏览器
    "com.fsck.k9",                     // K-9 Mail
    "org.videolan.vlc",                // VLC
    "org.schabi.newpipe",              // NewPipe
    "com.termux",                      // Termux
    "org.kde.kdeconnect_tp",           // KDE Connect
    "chat.simplex.app",                // SimpleX
    "com.beemdevelopment.aegis",       // Aegis 两步验证
    "com.kunzisoft.keepass.libre",     // KeePassDX
    "net.osmand.plus",                 // OsmAnd+
    "de.danoeh.antennapod",            // AntennaPod
    "org.wikipedia",                   // Wikipedia
    "com.duckduckgo.mobile.android",   // DuckDuckGo
    "eu.faircode.email",               // FairEmail
    "com.nextcloud.client",            // Nextcloud
    "at.bitfire.davdroid",             // DAVx⁵
    "net.gsantner.markor",             // Markor
    "net.sourceforge.opencamera",      // Open Camera
    "org.briarproject.briar.android",  // Briar
    "org.jitsi.meet",                  // Jitsi Meet
    "com.aurora.store",                // Aurora Store
    "com.github.libretube",            // LibreTube
    "com.keylesspalace.tusky",         // Tusky
    "com.nononsenseapps.feeder",       // Feeder
    "fr.gaulupeau.apps.InThePoche",    // wallabag
    "org.adaway",                      // AdAway
    "io.github.muntashirakon.AppManager",  // App Manager
    "org.fdroid.fdroid",               // F-Droid
};

// ================= 源实现 =================

class FdroidSource {
public:
    FdroidSource() = default;

    mw::Result<QVector<StoreApp>> search(const QString& q, const SourceOpts& opts) const;
    /// 精选推荐（kCuratedPackages ∩ 索引，含可装版本；空 = 索引拉取失败）。
    mw::Result<QVector<StoreApp>> recommend(const SourceOpts& opts) const;
    mw::Result<QVector<StoreVersion>> versions(const QString& package, const SourceOpts& opts) const;
    mw::Result<ResolvedInternal> resolve(const QString& package, std::optional<quint64> code,
        const SourceOpts& opts) const;

private:
    /// 取索引（内存 → 磁盘(<12h) → 网络）。basesOut 回传可用镜像基址表。
    mw::Result<std::shared_ptr<const FdroidIndex>> load(const SourceOpts& opts,
        QVector<QPair<QString, QString>>* basesOut) const;

    mutable QMutex mu_;
    mutable std::shared_ptr<const FdroidIndex> cache_;  // 进程内索引缓存
};

}  // namespace mw::store::fdroid
