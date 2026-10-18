// WandoujiaSource 实现 ← crates/mwine-store/src/wandoujia.rs。
#include "wandoujia.hpp"

#include <algorithm>

#include "html.hpp"
#include "internal.hpp"
#include "../common/log.hpp"

namespace mw::store::wandoujia {

mw::Result<QVector<StoreApp>> WandoujiaSource::search(const QString& q, const SourceOpts& opts) const
{
    const QString query = q.trimmed();
    if (query.isEmpty())
        return mw::Result<QVector<StoreApp>>::Ok({});
    const QString url = QStringLiteral("%1/search?key=%2")
                            .arg(QLatin1String(BASE), html::urlencode(query));
    auto page = detail::fetchText(url, opts, 30000);
    if (!page)
        return mw::err<QVector<StoreApp>>(page.error());
    if (page.value().count(QStringLiteral("<li class=\"search-item")) == 0) {
        // 抓到页面但无结果节点：可能触发反爬验证页或结构改版，留日志便于诊断
        MW_LOG("store", QStringLiteral("豌豆荚搜索 [%1]：页面 %2 字节但未解析到结果")
                            .arg(query).arg(page.value().size()));
    }
    return mw::Result<QVector<StoreApp>>::Ok(parseSearch(page.value()));
}

mw::Result<QVector<StoreVersion>> WandoujiaSource::versions(const QString& appId,
    const SourceOpts& opts) const
{
    QVector<StoreVersion> out;
    auto detail = detail::fetchText(QStringLiteral("%1/apps/%2").arg(QLatin1String(BASE), appId),
                                    opts, 30000);
    if (!detail)
        return mw::err<QVector<StoreVersion>>(detail.error());
    if (auto v = latestFromDetail(detail.value()))
        out.push_back(*v);
    auto hist = detail::fetchText(QStringLiteral("%1/apps/%2/history").arg(QLatin1String(BASE), appId),
                                  opts, 30000);
    if (!hist)
        return mw::err<QVector<StoreVersion>>(hist.error());
    const QVector<StoreVersion> history = parseHistory(hist.value());
    out += history;
    if (out.isEmpty()) {
        return mw::err<QVector<StoreVersion>>(
            QStringLiteral("页面/索引解析失败：应用 %1 没有解析到任何版本").arg(appId));
    }
    return mw::Result<QVector<StoreVersion>>::Ok(std::move(out));
}

mw::Result<ResolvedInternal> WandoujiaSource::resolve(const QString& appId, std::optional<quint64> code,
    const QString& nameHint, const SourceOpts& opts) const
{
    auto detail = detail::fetchText(QStringLiteral("%1/apps/%2").arg(QLatin1String(BASE), appId),
                                    opts, 30000);
    if (!detail)
        return mw::err<ResolvedInternal>(detail.error());
    auto latestOpt = latestFromDetail(detail.value());
    if (!latestOpt) {
        return mw::err<ResolvedInternal>(
            QStringLiteral("页面/索引解析失败：应用 %1 解析不到最新版信息").arg(appId));
    }
    StoreVersion latest = latestOpt.value();
    const bool pickLatest = !code || *code == latest.code;

    QString pageUrl;
    StoreVersion version;
    if (pickLatest) {
        pageUrl = QStringLiteral("%1/apps/%2/download/dot?ch=detail_qr_dl").arg(QLatin1String(BASE), appId);
        version = latest;
    } else {
        const quint64 c = *code;
        auto page = detail::fetchText(QStringLiteral("%1/apps/%2/history_v%3")
                                          .arg(QLatin1String(BASE), appId)
                                          .arg(c),
                                      opts, 30000);
        if (!page)
            return mw::err<ResolvedInternal>(page.error());
        // 老版本页面上的下载按钮是 binding?downloadUrl=<percent-encoded 直链>
        QString href;
        const QStringList segs = page.value().split(QStringLiteral("href=\""), Qt::KeepEmptyParts);
        for (const QString& seg : segs) {
            const int q = seg.indexOf(QLatin1Char('"'));
            const QString h = q < 0 ? seg : seg.left(q);
            if (h.contains(QStringLiteral("binding?downloadUrl="))) {
                href = h;
                break;
            }
        }
        if (href.isNull()) {
            return mw::err<ResolvedInternal>(QStringLiteral("未找到匹配的应用或版本：版本 %1 找不到下载入口（可能已下架）")
                .arg(c));
        }
        href.replace(QStringLiteral("&#38;"), QStringLiteral("&"));
        auto dl = html::queryParam(href, QStringLiteral("downloadUrl"));
        if (!dl) {
            return mw::err<ResolvedInternal>(
                QStringLiteral("页面/索引解析失败：binding 链接里缺少 downloadUrl"));
        }
        const QString dlUrl = dl.value();
        // 版本名优先取该页自身的 data-app-vname（首个匹配该 vcode 的锚点）
        std::optional<QString> pageName;
        qsizetype from = 0;
        while (true) {
            const qsizetype rel = page.value().indexOf(QStringLiteral("data-app-vcode=\""), from);
            if (rel < 0)
                break;
            const QString seg = page.value().mid(rel);
            bool ok = false;
            quint64 vc = 0;
            if (auto vs = html::attr(seg, QStringLiteral("data-app-vcode")))
                vc = vs->toULongLong(&ok);
            if (ok && vc == c) {
                if (auto n = html::attr(seg, QStringLiteral("data-app-vname"))) {
                    if (!n->isEmpty()) {
                        pageName = *n;
                        break;
                    }
                }
            }
            from = rel + 10;
        }
        version = latest;
        version.code = c;
        version.name = pageName ? *pageName : (!nameHint.isEmpty() ? nameHint : QStringLiteral("v%1").arg(c));
        version.isLatest = false;
        bool ok = false;
        if (auto s = html::queryParam(dlUrl, QStringLiteral("size")))
            version.size = s->toULongLong(&ok);
        if (!ok)
            version.size = 0;
        if (auto s = html::queryParam(dlUrl, QStringLiteral("minSDK"))) {
            bool okU = false;
            const uint u = s->toUInt(&okU);
            if (okU)
                version.minSdk = static_cast<quint32>(u);
        }
        pageUrl = dlUrl;
    }

    // 探测直链（跟 302 到 ucdl.25pp.com），顺便拿真实体积
    auto probed = detail::probeDownload(pageUrl, opts);
    if (!probed)
        return mw::err<ResolvedInternal>(probed.error());
    if (probed.value().total > 0)
        version.size = probed.value().total;
    std::optional<QString> md5 = html::queryParam(probed.value().finalUrl, QStringLiteral("md5"));
    if (!md5)
        md5 = html::queryParam(pageUrl, QStringLiteral("md5"));
    if (md5 && md5->size() != 32)
        md5.reset();

    ResolvedInternal r;
    r.url = probed.value().finalUrl;
    r.sha256 = std::nullopt;
    r.md5 = md5;
    r.size = version.size;
    r.version = version;
    return mw::Result<ResolvedInternal>::Ok(std::move(r));
}

// ================= 纯解析 =================

QVector<StoreApp> parseSearch(const QString& html)
{
    QVector<StoreApp> out;
    const QStringList items = html.split(QStringLiteral("<li class=\"search-item"), Qt::KeepEmptyParts);
    for (int i = 1; i < items.size(); ++i) {  // skip(1)：丢弃首个命中之前的内容
        const QString& item = items.at(i);
        auto package = html::attr(item, QStringLiteral("data-app-pname"));
        if (!package || package->isEmpty())
            continue;
        auto appId = html::attr(item, QStringLiteral("data-app-id"));
        if (!appId || appId->isEmpty())
            continue;
        // 名称：<a class="name">文本</a>，退化用图标 alt
        QString name;
        if (auto s = html::between(item, QStringLiteral("class=\"name\">"), QStringLiteral("</a>"))) {
            const QString t = html::decodeEntities(html::stripTags(*s));
            if (!t.isEmpty())
                name = t;
        }
        if (name.isNull()) {
            if (auto a = html::attr(item, QStringLiteral("alt")))
                name = html::decodeEntities(*a);
        }
        if (name.isNull())
            name = *package;
        std::optional<QString> icon;
        const int img = item.indexOf(QStringLiteral("<img"));
        if (img >= 0) {
            if (auto src = html::attr(item.mid(img), QStringLiteral("src"))) {
                if (src->startsWith(QLatin1String("http")))
                    icon = *src;
            }
        }
        std::optional<QString> installCount;
        if (auto s = html::between(item, QStringLiteral("<span>"), QStringLiteral("人安装</span>")))
            installCount = QStringLiteral("%1人安装").arg(html::decodeEntities(s->trimmed()));
        std::optional<QString> summary;
        if (auto s = html::between(item, QStringLiteral("class=\"comment\""), QStringLiteral("</div>"))) {
            const QString t = html::decodeEntities(html::stripTags(*s));
            if (!t.isEmpty())
                summary = t;
        }
        std::optional<StoreVersion> latest;
        {
            auto vname = html::attr(item, QStringLiteral("data-app-vname"));
            auto vcodeS = html::attr(item, QStringLiteral("data-app-vcode"));
            bool ok = false;
            quint64 vcode = 0;
            if (vcodeS)
                vcode = vcodeS->toULongLong(&ok);
            if (vname && !vname->isEmpty() && vcodeS && ok) {
                StoreVersion v;
                v.name = *vname;
                v.code = vcode;
                v.isLatest = true;
                latest = v;
            }
        }
        StoreApp a;
        a.source = QStringLiteral("wandoujia");
        a.appId = *appId;
        a.package = *package;
        a.name = name;
        a.developer = std::nullopt;
        a.summary = summary;
        a.iconUrl = icon;
        a.installCount = installCount;
        a.license = std::nullopt;
        a.latest = latest;
        out.push_back(std::move(a));
    }
    if (out.size() > 30)
        out.resize(30);
    return out;
}

std::optional<StoreVersion> latestFromDetail(const QString& html)
{
    // 详情页有多个带 vcode 的锚点（猜你喜欢等）；普通下载按钮的锚点带
    // auto-inspect-button-type="download"，取它之后第一个 data-app-vcode
    const int anchor = html.indexOf(QStringLiteral("auto-inspect-button-type=\"download\""));
    if (anchor < 0)
        return std::nullopt;
    const QString seg = html.mid(anchor);
    auto vname = html::attr(seg, QStringLiteral("data-app-vname"));
    auto vcodeS = html::attr(seg, QStringLiteral("data-app-vcode"));
    if (!vname || !vcodeS)
        return std::nullopt;
    bool ok = false;
    const quint64 vcode = vcodeS->toULongLong(&ok);
    if (!ok || vname->isEmpty() || vcode == 0)
        return std::nullopt;
    StoreVersion v;
    v.name = *vname;
    v.code = vcode;
    v.isLatest = true;
    return v;
}

QVector<StoreVersion> parseHistory(const QString& html)
{
    QVector<StoreVersion> out;
    const QStringList items = html.split(QStringLiteral("<li class=\"history-list"), Qt::KeepEmptyParts);
    for (int i = 1; i < items.size(); ++i) {
        const QString& item = items.at(i);
        auto vcodeS = html::attr(item, QStringLiteral("data-app-vcode"));
        if (!vcodeS)
            continue;
        bool ok = false;
        const quint64 vcode = vcodeS->toULongLong(&ok);
        if (!ok)
            continue;
        // 版本名可能带应用名前缀（"微信 8.0.53"）——历史页 <p> 里统一是「名 版本」
        const QString vname = html::attr(item, QStringLiteral("data-app-vname")).value_or(QString());
        quint64 size = 0;
        if (auto s = html::between(item, QStringLiteral("<span>"), QStringLiteral("</span>"))) {
            if (auto n = parseSizeCn(s->trimmed()))
                size = *n;
        }
        if (vname.isEmpty())
            continue;
        StoreVersion v;
        v.name = vname;
        v.code = vcode;
        v.size = size;
        v.isLatest = false;
        out.push_back(std::move(v));
    }
    // 去重（历史页分页链接可能重复出现同版本）并按版本号降序
    std::stable_sort(out.begin(), out.end(),
                     [](const StoreVersion& a, const StoreVersion& b) { return a.code > b.code; });
    QVector<StoreVersion> dedup;
    dedup.reserve(out.size());
    for (const StoreVersion& v : out) {
        if (dedup.isEmpty() || dedup.last().code != v.code)
            dedup.push_back(v);
    }
    if (dedup.size() > 50)
        dedup.resize(50);
    return dedup;
}

std::optional<quint64> parseSizeCn(const QString& s)
{
    const QString t = s.trimmed();
    int splitAt = -1;
    for (int i = 0; i < t.size(); ++i) {
        const ushort u = t.at(i).unicode();
        const bool digitOrDot = (u >= '0' && u <= '9') || u == '.';
        if (!digitOrDot) {
            splitAt = i;
            break;
        }
    }
    if (splitAt < 0)
        return std::nullopt;  // 无单位（纯数字）视为无法解析，对齐 Rust find 失败
    bool ok = false;
    const double n = t.left(splitAt).toDouble(&ok);
    if (!ok)
        return std::nullopt;
    const QString unit = t.mid(splitAt).trimmed().toUpper();
    double mult = 0.0;
    if (unit == QLatin1String("KB"))
        mult = 1024.0;
    else if (unit == QLatin1String("MB"))
        mult = 1024.0 * 1024.0;
    else if (unit == QLatin1String("GB"))
        mult = 1024.0 * 1024.0 * 1024.0;
    else
        return std::nullopt;
    return static_cast<quint64>(n * mult);
}

}  // namespace mw::store::wandoujia
