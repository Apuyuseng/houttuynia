// mw::store ← crates/mwine-store（在线应用检索与 APK 下载：豌豆荚 + F-Droid）
//
// 数据源：
// - F-Droid（index-v1.json，清华 TUNA / 南大 NJU 等国内镜像，
//   索引磁盘缓存 12h + 进程内常驻；每个版本自带 sha256）
// - 豌豆荚（网页解析：搜索 / 历史版本可选装 / 下载直链带 md5）
//
// 下载完成后的「校验哈希 / 校验包名」也在本层完成——装进虚拟机的 APK
// 必须与检索到的包名一致。安装本身不在此层：调用方（daemon）把下载好的
// APK 路径交给既有的 app create --apk 流程（安装 + 生成桌面壳）。
//
// 错误统一为 QString（mw::Result）；文案对齐 Rust 侧 thiserror Display。
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "../common/result.hpp"

namespace mw::store {

// ================= 公共数据模型 =================

/// 来源选项：镜像预设 id（或完整 repo URL）+ 代理。
struct SourceOpts {
    QString mirror;  // 空 = auto（fdroid：tuna → nju → official 依次回退）
    QString proxy;   // 代理 URL（http/https/socks5），空 = 直连
};

/// 一个可安装的历史/最新版本。
struct StoreVersion {
    QString name;             // 版本名（如 8.0.78 / 1.23.0）
    quint64 code = 0;         // versionCode
    quint64 size = 0;         // APK 字节数（未知为 0）
    std::optional<quint32> minSdk;     // 最低 Android API（未知 None）
    std::optional<quint64> addedMs;    // 发布时间（epoch 毫秒，未知 None）
    QVector<QString> nativeCodes;      // native 库架构（F-Droid 索引提供；空 = 通用）
    bool isLatest = false;    // 是否当前最新版

    QJsonObject toJson() const;
    static StoreVersion fromJson(const QJsonObject&);  // toJson 的逆（lenient）
};

/// 检索结果中的应用条目。
struct StoreApp {
    QString source;   // fdroid / wandoujia
    QString appId;    // 源内应用 id（豌豆荚为数字 id，F-Droid 即包名）
    QString package;
    QString name;
    std::optional<QString> developer;
    std::optional<QString> summary;
    std::optional<QString> iconUrl;
    std::optional<QString> installCount;  // 安装量文案（豌豆荚，如「2.3亿人安装」）
    std::optional<QString> license;       // 许可证（F-Droid）
    std::optional<StoreVersion> latest;   // 最新版摘要（可缺）

    QJsonObject toJson() const;
    static StoreApp fromJson(const QJsonObject&);
};

/// 解析出的可下载文件（daemon 的下载任务起点）。
/// `version` 为完整版本信息（Rust ResolvedDownload::version）；
/// `versionName`/`versionCode`/`size` 是它的扁平镜像（契约字段，由 resolve 填充）。
struct Resolved {
    QString url;
    StoreVersion version;
    QString versionName;
    quint64 versionCode = 0;
    std::optional<QString> sha256;  // F-Droid
    std::optional<QString> md5;     // 豌豆荚直链查询串
    quint64 size = 0;               // APK 字节数（未知 0），进度/日志用
};

/// 供 UI 展示的镜像选项。
struct MirrorInfo {
    QString id;
    QString name;
    QString url;  // repo 基址（以 / 结尾）
    QJsonObject toJson() const;  // {id,name,url}，对齐 Rust serde Serialize
};

/// 供 UI 展示的来源元信息。
struct SourceInfo {
    QString id;
    QString name;
    QVector<MirrorInfo> mirrors;
};

// ================= UA 常量（照抄 lib.rs） =================

/// 豌豆荚网页需要浏览器 UA；F-Droid 镜像恰好相反——TUNA 对浏览器 UA 回 403
/// （实测 Chrome UA 403 / 中性 UA 206），镜像请求用产品 UA。
inline constexpr const char* UA_BROWSER =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0 Safari/537.36";
inline constexpr const char* UA_NEUTRAL = "Houttuynia/1.0";

/// daemon 侧的 camelCase 取用形式（值同上）。
inline QByteArray uaBrowser() { return QByteArray(UA_BROWSER); }
inline QByteArray uaNeutral() { return QByteArray(UA_NEUTRAL); }

// ================= 下载管线 =================

/// 进度/日志回调（均为可空调用；progress：(phase, 已完成字节, 总字节)，总=0 不定态）。
struct FetchCallbacks {
    std::function<void(const QString&)> log;
    std::function<void(const QString&, quint64, quint64)> progress;
};

/// 下载管线（daemon 与 CLI 共用）：断点续传下载 → 哈希校验 → 包名核对。
/// `destPath` 由调用方给出（daemon 与 CLI 用同一目录约定，共享断点续传进度）。
/// 成功返回 (label, versionName)（UTF-8 字节；daemon 用 QString::fromUtf8 解码）；
/// 哈希/包名不符时会删除已下载文件并报错。
/// 包名核对用本模块内置的最小 zip+AXML 读取（mw::store::apkmin），
/// 将来统一到 mw::app 后可替换为 mw::app 的完整解析。
mw::Result<QPair<QByteArray, QByteArray>> fetchApk(const Resolved& resolved,
    const QString& destPath, const QString& expectedPackage, const QString& appName,
    const QByteArray& ua, const SourceOpts& opts, const std::atomic_bool& cancel,
    const std::function<void(const QString&, quint64, quint64)>& progress,
    const std::function<void(const QString&)>& log);

/// FetchCallbacks 形态的重载（回调可缺省）。
inline mw::Result<QPair<QByteArray, QByteArray>> fetchApk(const Resolved& resolved,
    const QString& destPath, const QString& expectedPackage, const QString& appName,
    const QByteArray& ua, const SourceOpts& opts, const std::atomic_bool& cancel,
    const FetchCallbacks& callbacks)
{
    return fetchApk(resolved, destPath, expectedPackage, appName, ua, opts, cancel,
                    callbacks.progress, callbacks.log);
}

// ================= 门面 =================

/// 应用商店门面：daemon 持有一个实例（F-Droid 索引缓存随它常驻）。
class StoreHub {
public:
    StoreHub();
    ~StoreHub();
    StoreHub(const StoreHub&) = delete;
    StoreHub& operator=(const StoreHub&) = delete;

    /// 来源列表（含 F-Droid 镜像选项；fdroid 在前）。
    QVector<SourceInfo> sources() const;

    /// 搜索。source 空 = 聚合全部来源（豌豆荚在前，F-Droid 在后）；
    /// 单源失败不拖垮整体，失败信息进 errors（source → 错误文案），由 UI 提示。
    QPair<QVector<StoreApp>, QMap<QString, QString>> search(const QString& source, const QString& q,
        const SourceOpts& opts) const;

    /// 精选推荐（当前仅 F-Droid：索引内精选清单；source 非空且非 fdroid 时报错）。
    mw::Result<QVector<StoreApp>> recommend(const QString& source, const SourceOpts& opts) const;

    /// 某应用的版本列表（新→旧；豌豆荚含「最新版」标记）。
    mw::Result<QVector<StoreVersion>> versions(const QString& source, const QString& appId,
        const QString& package, const SourceOpts& opts) const;

    /// 解析下载：code=nullopt 取最新版。返回直链 + 校验哈希。
    /// `versionNameHint`：UI 已知的版本名（豌豆荚老版本页解析失败时兜底；空 = 无）。
    mw::Result<Resolved> resolve(const QString& source, const QString& appId,
        const QString& package, std::optional<quint64> code, const QString& versionNameHint,
        const SourceOpts& opts) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mw::store
