// FdroidSource 实现 ← crates/mwine-store/src/fdroid.rs。
#include "fdroid.hpp"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>

#include "../common/compress.hpp"
#include "../common/fs.hpp"
#include "../common/json.hpp"
#include "../common/log.hpp"
#include "internal.hpp"

namespace mw::store::fdroid {

namespace {

std::optional<QString> jStrOpt(const QJsonObject& o, const char* k)
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isString() ? std::optional<QString>(v.toString()) : std::nullopt;
}

QString cacheDir()
{
    return QDir(mw::mwineDir()).filePath(QStringLiteral("store"));
}

// F-Droid 应用图标 URL（fastlane 目录结构：repo/<package>/en-US/icon.png）。
// 官方客户端即从此路径取图标；镜像（TUNA/NJU）同步该目录。索引里的 icon
// 文件名（icon_*.png）在镜像上 404，不可用——故不按索引名拼，统一用 fastlane。
QString iconUrlFor(const QString& base, const QString& package)
{
    return base + package + QStringLiteral("/en-US/icon.png");
}

}  // namespace

// ================= 镜像 =================

QVector<MirrorInfo> mirrors()
{
    QVector<MirrorInfo> out;
    for (const MirrorPreset& p : MIRROR_PRESETS) {
        MirrorInfo m;
        m.id = QString::fromLatin1(p.id);
        m.name = QString::fromUtf8(p.name);
        m.url = QString::fromLatin1(p.url);
        out.push_back(std::move(m));
    }
    return out;
}

QVector<QPair<QString, QString>> mirrorBases(const QString& mirror)
{
    QVector<QPair<QString, QString>> out;
    const auto append = [&out](const MirrorPreset& p) {
        out.append({ QString::fromLatin1(p.id), QString::fromLatin1(p.url) });
    };
    if (mirror.isEmpty() || mirror == QStringLiteral("auto")) {
        for (const MirrorPreset& p : MIRROR_PRESETS)
            append(p);
    } else if (mirror.startsWith(QLatin1String("http"))) {
        QString u = mirror;
        if (!u.endsWith(QLatin1Char('/')))
            u += QLatin1Char('/');
        out.append({ QStringLiteral("custom"), u });
    } else {
        for (const MirrorPreset& p : MIRROR_PRESETS) {
            if (mirror == QLatin1String(p.id)) {
                append(p);
                break;
            }
        }
    }
    return out;
}

// ================= 索引解析 =================

StoreVersion toStoreVersion(const VerEntry& v, bool isLatest)
{
    StoreVersion s;
    s.name = v.name;
    s.code = v.code;
    s.size = v.size;
    s.minSdk = v.minSdk;
    s.addedMs = v.addedMs;
    s.nativeCodes = v.nativeCode;
    s.isLatest = isLatest;
    return s;
}

mw::Result<FdroidIndex> parseIndex(const QByteArray& jsonBytes)
{
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        return mw::err<FdroidIndex>(QStringLiteral("页面/索引解析失败：index-v1.json: %1（offset %2）")
                                        .arg(pe.errorString())
                                        .arg(pe.offset));
    }
    const QJsonObject root = doc.object();
    FdroidIndex idx;

    // apps：名字/摘要优先级 localized(en-US → 首个有 name 的 locale) > 顶层
    for (const QJsonValue& av : root.value(QStringLiteral("apps")).toArray()) {
        const QJsonObject a = av.toObject();
        AppEntry e;
        e.package = a.value(QStringLiteral("packageName")).toString();
        const QJsonObject localized = a.value(QStringLiteral("localized")).toObject();
        QJsonObject loc = localized.value(QStringLiteral("en-US")).toObject();
        if (loc.isEmpty() || !jStrOpt(loc, "name")) {
            for (auto it = localized.begin(); it != localized.end(); ++it) {
                if (it.value().isObject() && jStrOpt(it.value().toObject(), "name")) {
                    loc = it.value().toObject();
                    break;
                }
            }
        }
        QString name = jStrOpt(loc, "name").value_or(QString());
        if (name.isEmpty())
            name = jStrOpt(a, "name").value_or(QString());
        e.name = name.trimmed().isEmpty() ? e.package : name;
        e.summary = jStrOpt(loc, "summary").value_or(jStrOpt(a, "summary").value_or(QString()));
        e.icon = jStrOpt(loc, "icon").value_or(jStrOpt(a, "icon").value_or(QString()));
        e.author = jStrOpt(a, "authorName");
        e.license = jStrOpt(a, "license");
        idx.apps.push_back(std::move(e));
    }

    // packages：过滤无 apkName 的条目，按 versionCode 降序
    const QJsonObject packages = root.value(QStringLiteral("packages")).toObject();
    for (auto it = packages.begin(); it != packages.end(); ++it) {
        QVector<VerEntry> list;
        for (const QJsonValue& vv : it.value().toArray()) {
            const QJsonObject v = vv.toObject();
            VerEntry e;
            e.name = v.value(QStringLiteral("versionName")).toString();
            e.code = static_cast<quint64>(v.value(QStringLiteral("versionCode")).toDouble());
            e.apkName = v.value(QStringLiteral("apkName")).toString();
            e.size = static_cast<quint64>(v.value(QStringLiteral("size")).toDouble());
            e.hash = v.value(QStringLiteral("hash")).toString();
            const QJsonValue ms = v.value(QStringLiteral("minSdkVersion"));
            if (ms.isDouble())
                e.minSdk = static_cast<quint32>(ms.toInt());
            for (const QJsonValue& nc : v.value(QStringLiteral("nativecode")).toArray())
                e.nativeCode.push_back(nc.toString());
            const double added = v.value(QStringLiteral("added")).toDouble();
            e.addedMs = added > 0.0 ? static_cast<quint64>(added) : 0;
            if (!e.apkName.isEmpty())
                list.push_back(std::move(e));
        }
        std::stable_sort(list.begin(), list.end(),
                         [](const VerEntry& x, const VerEntry& y) { return x.code > y.code; });
        idx.packages.insert(it.key(), std::move(list));
    }
    return mw::Result<FdroidIndex>::Ok(std::move(idx));
}

// ================= 索引获取与缓存 =================

namespace {

// meta 文件名按 Rust 侧固定；磁盘缓存（任何镜像的索引内容等价，缓存不区分来源）
constexpr int kIndexMinBytes = 1000000;      // < 1MB 视为异常
constexpr qint64 kCacheTtlSec = 12 * 3600;   // 12 小时

std::optional<FdroidIndex> readDiskCache()
{
    const auto metaBytes = mw::readFile(QDir(cacheDir()).filePath(QStringLiteral("fdroid-index.meta.json")));
    if (!metaBytes)
        return std::nullopt;
    bool ok = false;
    const QJsonObject meta = mw::parseJson(metaBytes.value(), &ok);
    if (!ok)
        return std::nullopt;
    const qint64 fetchedAt = static_cast<qint64>(mw::jNum(meta, "fetched_at"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (std::max<qint64>(now - fetchedAt, 0) > kCacheTtlSec)
        return std::nullopt;
    const auto gz = mw::readFile(QDir(cacheDir()).filePath(QStringLiteral("fdroid-index.json.gz")));
    if (!gz)
        return std::nullopt;
    const auto json = mw::gzipDecompress(gz.value());
    if (!json)
        return std::nullopt;
    auto idx = parseIndex(json.value());
    if (!idx)
        return std::nullopt;
    return idx.value();
}

// 从网络拉取索引：成功后落盘 gzip + meta（缓存写失败不阻断本次搜索）。
mw::Result<QByteArray> fetchIndex(const QString& base, const SourceOpts& opts)
{
    const QString url = base + QStringLiteral("index-v1.json");
    QMap<QByteArray, QByteArray> headers;
    headers.insert("User-Agent", QByteArray(UA_NEUTRAL));
    headers.insert("Accept-Encoding", "gzip");
    auto bytes = detail::fetchBytes(url, headers, 300000, opts.proxy);
    if (!bytes)
        return mw::err<QByteArray>(bytes.error());
    if (bytes.value().size() < kIndexMinBytes) {
        return mw::err<QByteArray>(
            QStringLiteral("页面/索引解析失败：索引异常地小（%1 字节）").arg(bytes.value().size()));
    }
    QDir().mkpath(cacheDir());
    const auto writeRes = mw::writeFile(QDir(cacheDir()).filePath(QStringLiteral("fdroid-index.json.gz")),
                                        detail::gzipCompress(bytes.value()));
    if (!writeRes)
        MW_LOG("store", QStringLiteral("F-Droid 索引缓存写失败: %1").arg(writeRes.error()));
    QJsonObject meta;
    meta[QStringLiteral("fetched_at")] = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    (void)mw::writeFile(QDir(cacheDir()).filePath(QStringLiteral("fdroid-index.meta.json")),
                        mw::toJsonBytes(meta));
    return bytes;
}

}  // namespace

// ================= 源实现 =================

mw::Result<std::shared_ptr<const FdroidIndex>> FdroidSource::load(const SourceOpts& opts,
    QVector<QPair<QString, QString>>* basesOut) const
{
    const QVector<QPair<QString, QString>> bases = mirrorBases(opts.mirror);
    if (bases.isEmpty()) {
        return mw::err<std::shared_ptr<const FdroidIndex>>(
            QStringLiteral("未找到匹配的应用或版本：未知镜像 %1").arg(opts.mirror));
    }
    if (basesOut)
        *basesOut = bases;
    {
        QMutexLocker l(&mu_);
        if (cache_)
            return mw::Result<std::shared_ptr<const FdroidIndex>>::Ok(cache_);
    }
    if (auto idx = readDiskCache()) {
        auto arc = std::make_shared<const FdroidIndex>(std::move(*idx));
        QMutexLocker l(&mu_);
        cache_ = arc;
        return mw::Result<std::shared_ptr<const FdroidIndex>>::Ok(arc);
    }
    // 网络：按镜像顺序尝试（全部失败才报错，错误逐个列出）
    QStringList errs;
    for (const auto& b : bases) {
        auto raw = fetchIndex(b.second, opts);
        if (raw) {
            auto parsed = parseIndex(raw.value());
            if (parsed) {
                auto arc = std::make_shared<const FdroidIndex>(std::move(parsed.value()));
                QMutexLocker l(&mu_);
                cache_ = arc;
                return mw::Result<std::shared_ptr<const FdroidIndex>>::Ok(arc);
            }
            errs << QStringLiteral("%1: %2").arg(b.first, parsed.error());
            continue;
        }
        errs << QStringLiteral("%1: %2").arg(b.first, raw.error());
    }
    return mw::err<std::shared_ptr<const FdroidIndex>>(QStringLiteral(
        "网络请求失败（F-Droid 索引获取失败（已尝试 %1 个镜像）：%2）")
        .arg(bases.size())
        .arg(errs.join(QStringLiteral("；"))));
}

mw::Result<QVector<StoreApp>> FdroidSource::search(const QString& q, const SourceOpts& opts) const
{
    const QString query = q.trimmed();
    if (query.isEmpty())
        return mw::Result<QVector<StoreApp>>::Ok({});
    QVector<QPair<QString, QString>> bases;
    auto idx = load(opts, &bases);
    if (!idx)
        return mw::err<QVector<StoreApp>>(idx.error());
    const QString base = bases.first().second;
    const QString ql = query.toLower();
    QVector<QPair<int, const AppEntry*>> scored;
    for (const AppEntry& app : idx.value()->apps) {
        const QString nameL = app.name.toLower();
        const QString pkgL = app.package.toLower();
        int score;
        if (nameL == ql || pkgL == ql)
            score = 100;
        else if (nameL.startsWith(ql) || pkgL.startsWith(ql))
            score = 70;
        else if (nameL.contains(ql))
            score = 50;
        else if (pkgL.contains(ql))
            score = 35;
        else if (app.summary.toLower().contains(ql))
            score = 10;
        else
            continue;
        scored.append({ score, &app });
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second->name < b.second->name;
    });
    QVector<StoreApp> out;
    const int limit = std::min<int>(scored.size(), 40);
    for (int i = 0; i < limit; ++i) {
        const AppEntry& a = *scored[i].second;
        StoreApp s;
        s.source = QStringLiteral("fdroid");
        s.appId = a.package;
        s.package = a.package;
        s.name = a.name;
        s.developer = a.author;
        s.summary = a.summary.isEmpty() ? std::nullopt : std::optional<QString>(a.summary);
        s.iconUrl = iconUrlFor(base, a.package);
        s.license = a.license;
        const QVector<VerEntry> vers = idx.value()->packages.value(a.package);
        if (!vers.isEmpty())
            s.latest = toStoreVersion(vers.first(), true);
        out.push_back(std::move(s));
    }
    return mw::Result<QVector<StoreApp>>::Ok(std::move(out));
}

mw::Result<QVector<StoreApp>> FdroidSource::recommend(const SourceOpts& opts) const
{
    QVector<QPair<QString, QString>> bases;
    auto idx = load(opts, &bases);
    if (!idx)
        return mw::err<QVector<StoreApp>>(idx.error());
    const QString base = bases.first().second;
    QVector<StoreApp> out;
    for (const char* curated : kCuratedPackages) {
        const QString package = QString::fromLatin1(curated);
        const QVector<VerEntry> vers = idx.value()->packages.value(package);
        if (vers.isEmpty())
            continue;  // 索引没有该包（或无 APK）：静默跳过
        const auto it = std::find_if(idx.value()->apps.cbegin(), idx.value()->apps.cend(),
                                     [&package](const AppEntry& a) { return a.package == package; });
        if (it == idx.value()->apps.cend())
            continue;
        const AppEntry& a = *it;
        StoreApp s;
        s.source = QStringLiteral("fdroid");
        s.appId = a.package;
        s.package = a.package;
        s.name = a.name;
        s.developer = a.author;
        s.summary = a.summary.isEmpty() ? std::nullopt : std::optional<QString>(a.summary);
        s.iconUrl = iconUrlFor(base, a.package);
        s.license = a.license;
        s.latest = toStoreVersion(vers.first(), true);
        out.push_back(std::move(s));
    }
    return mw::Result<QVector<StoreApp>>::Ok(std::move(out));
}

mw::Result<QVector<StoreVersion>> FdroidSource::versions(const QString& package,
    const SourceOpts& opts) const
{
    auto idx = load(opts, nullptr);
    if (!idx)
        return mw::err<QVector<StoreVersion>>(idx.error());
    const QVector<VerEntry> vers = idx.value()->packages.value(package);
    if (vers.isEmpty())
        return mw::err<QVector<StoreVersion>>(
            QStringLiteral("未找到匹配的应用或版本：F-Droid 里没有 %1").arg(package));
    QVector<StoreVersion> out;
    for (int i = 0; i < vers.size(); ++i)
        out.push_back(toStoreVersion(vers[i], i == 0));
    if (out.size() > 50)
        out.resize(50);
    return mw::Result<QVector<StoreVersion>>::Ok(std::move(out));
}

mw::Result<ResolvedInternal> FdroidSource::resolve(const QString& package, std::optional<quint64> code,
    const SourceOpts& opts) const
{
    QVector<QPair<QString, QString>> bases;
    auto idx = load(opts, &bases);
    if (!idx)
        return mw::err<ResolvedInternal>(idx.error());
    const QString base = bases.first().second;
    const QVector<VerEntry>& vers = idx.value()->packages.value(package);
    if (vers.isEmpty())
        return mw::err<ResolvedInternal>(
            QStringLiteral("未找到匹配的应用或版本：F-Droid 里没有 %1").arg(package));
    const VerEntry* ver = nullptr;
    if (code) {
        for (const VerEntry& v : vers) {
            if (v.code == *code) {
                ver = &v;
                break;
            }
        }
    } else if (!vers.isEmpty()) {
        ver = &vers.first();
    }
    if (!ver) {
        return mw::err<ResolvedInternal>(QStringLiteral("未找到匹配的应用或版本：%1 没有 versionCode=%2")
            .arg(package)
            .arg(code.value_or(0)));
    }
    ResolvedInternal r;
    r.version = toStoreVersion(*ver, ver->code == vers.first().code);
    r.url = base + ver->apkName;
    r.sha256 = ver->hash.isEmpty() ? std::nullopt : std::optional<QString>(ver->hash);
    r.size = ver->size;
    return mw::Result<ResolvedInternal>::Ok(std::move(r));
}

}  // namespace mw::store::fdroid
