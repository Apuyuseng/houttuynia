// store.hpp 实现 ← crates/mwine-store/src/lib.rs：
// HTTP 基础（fetch_text / probe_download / download_file）、哈希校验、
// fetch_apk 下载管线、模型 toJson 与 StoreHub 门面。
#include "store.hpp"

#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <memory>
#include <QJsonArray>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>

#include <zlib.h>

#include "../common/compress.hpp"
#include "../app/apk.hpp"
#include "../app/manifest.hpp"
#include "apk_min.hpp"
#include "fdroid.hpp"
#include "internal.hpp"
#include "wandoujia.hpp"

namespace mw::store {

// ================= 模型 JSON =================

QJsonObject StoreVersion::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("code")] = static_cast<double>(code);
    o[QStringLiteral("size")] = static_cast<double>(size);
    o[QStringLiteral("minSdk")] = minSdk ? QJsonValue(static_cast<double>(*minSdk))
                                         : QJsonValue(QJsonValue::Null);
    o[QStringLiteral("addedMs")] = addedMs ? QJsonValue(static_cast<double>(*addedMs))
                                           : QJsonValue(QJsonValue::Null);
    QJsonArray nc;
    for (const QString& s : nativeCodes)
        nc.append(s);
    o[QStringLiteral("nativeCodes")] = nc;
    o[QStringLiteral("isLatest")] = isLatest;
    return o;
}

StoreVersion StoreVersion::fromJson(const QJsonObject& o)
{
    StoreVersion v;
    v.name = o.value(QStringLiteral("name")).toString();
    v.code = static_cast<quint64>(o.value(QStringLiteral("code")).toDouble());
    v.size = static_cast<quint64>(o.value(QStringLiteral("size")).toDouble());
    const QJsonValue ms = o.value(QStringLiteral("minSdk"));
    if (ms.isDouble())
        v.minSdk = static_cast<quint32>(ms.toInt());
    const QJsonValue am = o.value(QStringLiteral("addedMs"));
    if (am.isDouble())
        v.addedMs = static_cast<quint64>(am.toDouble());
    for (const QJsonValue& n : o.value(QStringLiteral("nativeCodes")).toArray())
        v.nativeCodes.push_back(n.toString());
    v.isLatest = o.value(QStringLiteral("isLatest")).toBool();
    return v;
}

QJsonObject StoreApp::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("source")] = source;
    o[QStringLiteral("appId")] = appId;
    o[QStringLiteral("package")] = package;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("developer")] = developer ? QJsonValue(*developer) : QJsonValue(QJsonValue::Null);
    o[QStringLiteral("summary")] = summary ? QJsonValue(*summary) : QJsonValue(QJsonValue::Null);
    o[QStringLiteral("iconUrl")] = iconUrl ? QJsonValue(*iconUrl) : QJsonValue(QJsonValue::Null);
    o[QStringLiteral("installCount")] = installCount ? QJsonValue(*installCount)
                                                      : QJsonValue(QJsonValue::Null);
    o[QStringLiteral("license")] = license ? QJsonValue(*license) : QJsonValue(QJsonValue::Null);
    o[QStringLiteral("latest")] = latest ? QJsonValue(latest->toJson()) : QJsonValue(QJsonValue::Null);
    return o;
}

StoreApp StoreApp::fromJson(const QJsonObject& o)
{
    StoreApp a;
    a.source = o.value(QStringLiteral("source")).toString();
    a.appId = o.value(QStringLiteral("appId")).toString();
    a.package = o.value(QStringLiteral("package")).toString();
    a.name = o.value(QStringLiteral("name")).toString();
    const auto optStr = [&o](const char* k) -> std::optional<QString> {
        const QJsonValue v = o.value(QLatin1String(k));
        return v.isString() ? std::optional<QString>(v.toString()) : std::nullopt;
    };
    a.developer = optStr("developer");
    a.summary = optStr("summary");
    a.iconUrl = optStr("iconUrl");
    a.installCount = optStr("installCount");
    a.license = optStr("license");
    const QJsonValue latestV = o.value(QStringLiteral("latest"));
    if (latestV.isObject())
        a.latest = StoreVersion::fromJson(latestV.toObject());
    return a;
}

QJsonObject MirrorInfo::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("url")] = url;
    return o;
}

// ================= HTTP 基础（detail：lib.rs 对应物） =================

namespace detail {

namespace {

mw::Result<QNetworkProxy> parseProxy(const QString& p)
{
    const QUrl u(p);
    if (!u.isValid() || u.host().isEmpty() || u.port(-1) == -1) {
        return mw::err<QNetworkProxy>(
            QStringLiteral("网络请求失败（代理地址无效（%1）: 需要 scheme://host:port）").arg(p));
    }
    QNetworkProxy proxy;
    const QString scheme = u.scheme().toLower();
    if (scheme == QLatin1String("socks5") || scheme == QLatin1String("socks5h"))
        proxy.setType(QNetworkProxy::Socks5Proxy);
    else if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        proxy.setType(QNetworkProxy::HttpProxy);
    else
        return mw::err<QNetworkProxy>(
            QStringLiteral("网络请求失败（代理地址无效（%1）: 不支持的 scheme %2）").arg(p, scheme));
    proxy.setHostName(u.host());
    proxy.setPort(u.port());
    if (!u.userInfo().isEmpty()) {
        const int colon = u.userInfo().indexOf(QLatin1Char(':'));
        if (colon > 0) {
            proxy.setUser(u.userInfo().left(colon));
            proxy.setPassword(u.userInfo().mid(colon + 1));
        }
    }
    return mw::Result<QNetworkProxy>::Ok(proxy);
}

// mw::net 之外的补充通道：需要设置代理（mw::net 无代理支持）时的 GET。
// 行为对齐 mw::httpRequest（线程内事件循环 + 自动跟随重定向）。
mw::HttpReply namGet(const QUrl& url, const QMap<QByteArray, QByteArray>& headers, int timeoutMs,
    const QNetworkProxy& proxy)
{
    QNetworkAccessManager nam;
    nam.setProxy(proxy);
    nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    for (auto it = headers.begin(); it != headers.end(); ++it)
        req.setRawHeader(it.key(), it.value());
    auto* reply = nam.get(req);

    mw::HttpReply out;
    QEventLoop loop;
    QTimer timer;
    if (timeoutMs > 0) {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = reply->readAll();
    for (const auto& h : reply->rawHeaderPairs())
        out.headers.insert(h.first, h.second);
    if (reply->error() != QNetworkReply::NoError)
        out.error = reply->errorString();
    // 栈上 nam 析构前必须同步删除 reply：deleteLater 只是把删除事件排队，
    // 本线程 loop 已退出不再派发——nam 析构会先强删 reply，之后事件系统再处理
    // deleteLater 就 double-delete（下载线程堆损坏 → 后续 parseManifest 分配崩溃）。
    reply->deleteLater();
    QCoreApplication::sendPostedEvents(reply, QEvent::DeferredDelete);
    return out;
}

// 统一成功判据：QNAM 传输层失败 status=0；HTTP 判定只看状态码
// （errorString 在 NoError 时可能是 "Unknown error"，不能作为失败依据）。
QString httpErrText(const QString& url, const mw::HttpReply& r)
{
    if (r.status > 0)
        return QStringLiteral("%1: HTTP %2").arg(url).arg(r.status);
    return QStringLiteral("%1: %2").arg(url, r.error.isEmpty() ? QStringLiteral("网络错误") : r.error);
}

// gzip 透明解码：以 body 的 gzip 魔数（1F 8B）为准，而非仅看 Content-Encoding 头。
// QNetworkAccessManager 对 gzip 的处理不一致——有的响应它不解压但也不保留/透传
// Content-Encoding 头（豌豆荚即如此：body 是原始 gzip 字节，header 读不到 gzip），
// 只看头会漏解压导致 HTML 解析在二进制乱码上进行。魔数判断是唯一可靠依据。
mw::Result<QByteArray> decodeGzipBody(const mw::HttpReply& r)
{
    const QByteArray& body = r.body;
    const bool isGzip = body.size() >= 2
                        && static_cast<quint8>(body.at(0)) == 0x1F
                        && static_cast<quint8>(body.at(1)) == 0x8B;
    if (!isGzip)
        return mw::Result<QByteArray>::Ok(body);
    auto dec = mw::gzipDecompress(body);
    if (!dec)  // 解压失败回退原 body（可能是恰好以魔数开头的非 gzip 内容）
        return mw::Result<QByteArray>::Ok(body);
    return mw::Result<QByteArray>::Ok(dec.value());
}

}  // namespace

mw::Result<QByteArray> fetchBytes(const QString& url, const QMap<QByteArray, QByteArray>& headers,
    int timeoutMs, const QString& proxy)
{
    mw::HttpReply resp;
    if (proxy.isEmpty()) {
        resp = mw::httpRequest(QStringLiteral("GET"), QUrl(url), {}, headers, timeoutMs);
    } else {
        auto pp = parseProxy(proxy);
        if (!pp)
            return mw::err<QByteArray>(pp.error());
        resp = namGet(QUrl(url), headers, timeoutMs, pp.value());
    }
    if (resp.status < 200 || resp.status >= 300) {
        return mw::err<QByteArray>(QStringLiteral("网络请求失败（%1）").arg(httpErrText(url, resp)));
    }
    return decodeGzipBody(resp);
}

mw::Result<QString> fetchText(const QString& url, const SourceOpts& opts, int timeoutMs)
{
    QMap<QByteArray, QByteArray> headers;
    headers.insert("User-Agent", QByteArray(UA_BROWSER));
    headers.insert("Accept-Encoding", "gzip");
    auto body = fetchBytes(url, headers, timeoutMs, opts.proxy);
    if (!body)
        return mw::err<QString>(body.error());
    return mw::Result<QString>::Ok(QString::fromUtf8(body.value()));
}

QByteArray gzipCompress(const QByteArray& in)
{
    z_stream zs{};
    // 15 + 16 = deflate + gzip 头/尾（flate2 GzEncoder 等价，level 6 = default）
    if (deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = static_cast<uInt>(in.size());
    QByteArray out;
    out.reserve(in.size() / 2 + 64);
    char buf[65536];
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
            break;
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return out;
}

mw::Result<ProbeInfo> probeDownload(const QString& url, const SourceOpts& opts,
                                    const QByteArray& ua)
{
    // 一律走 mw::httpRequest（thread_local QNAM + 自动重定向 + finished 后清理）：
    // 自管 QNAM 的探测实现（栈 QNAM/提前 abort/手动删 reply 的各种组合）会在
    // daemon 工作线程上下文静默写坏内存——下载完成后首个大容器操作（zip/manifest
    // 解析）段错误，ASan 与 malloc 堆校验均无法定位（写入落在合法分配内）。
    // httpRequest 路径已实测稳定（含 67MB 整档 GET）。
    // 代价：拿不到重定向后的最终直链，finalUrl 回退原 url——downloadFile 的
    // 下载请求自带 NoLessSafeRedirectPolicy 自动跟随，仅多一跳重定向。
    // UA 必须与正式下载一致：TUNA 镜像按 UA 反爬（浏览器 UA 403、中性 UA 200）。
    const QByteArray effUa = ua.isEmpty() ? QByteArray(UA_BROWSER) : ua;
    QMap<QByteArray, QByteArray> headers;
    headers.insert("Range", QByteArray("bytes=0-0"));
    headers.insert("User-Agent", effUa);
    mw::HttpReply r;
    if (opts.proxy.isEmpty()) {
        r = mw::httpRequest(QStringLiteral("GET"), QUrl(url), {}, headers, 30000);
    } else {
        auto pp = parseProxy(opts.proxy);
        if (!pp)
            return mw::err<ProbeInfo>(pp.error());
        r = namGet(QUrl(url), headers, 30000, pp.value());
    }
    if (r.status == 0) {
        return mw::err<ProbeInfo>(QStringLiteral("网络请求失败（下载链接不可达: %1）")
                                      .arg(r.error.isEmpty() ? QStringLiteral("网络错误")
                                                             : r.error));
    }
    if (r.status != 200 && r.status != 206) {
        return mw::err<ProbeInfo>(
            QStringLiteral("网络请求失败（下载链接返回 HTTP %1）").arg(r.status));
    }
    ProbeInfo info;
    info.rangeOk = r.status == 206;
    quint64 total = 0;
    const QString cr = QString::fromLatin1(r.headers.value("Content-Range"));
    const int slash = cr.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        total = cr.mid(slash + 1).trimmed().toULongLong();
    if (total == 0)
        total = QString::fromLatin1(r.headers.value("Content-Length")).trimmed().toULongLong();
    info.total = total;
    info.finalUrl = url;
    return mw::Result<ProbeInfo>::Ok(info);
}

namespace {

struct SegmentResult {
    int status = 0;
    QString error;
};

// 单段下载：proxy 为空走 mw::httpDownload（追加写 + 进度 + 取消）；
// 带代理时用本地 QNAM 复刻同款行为。
SegmentResult downloadSegment(const QString& urlStr, const QString& destPath,
    const QMap<QByteArray, QByteArray>& headers, const std::function<void(qint64, qint64)>& prog,
    const std::atomic_bool& cancel, int timeoutMs, const QString& proxy)
{
    if (proxy.isEmpty()) {
        mw::HttpReply r = mw::httpDownload(QUrl(urlStr), destPath, prog, &cancel, headers, timeoutMs);
        return { r.status, r.error };
    }
    auto pp = parseProxy(proxy);
    if (!pp)
        return { 0, pp.error() };

    QNetworkAccessManager nam;
    nam.setProxy(pp.value());
    nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkRequest req{ QUrl(urlStr) };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    for (auto it = headers.begin(); it != headers.end(); ++it)
        req.setRawHeader(it.key(), it.value());

    QFile file(destPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return { 0, QStringLiteral("无法写入 %1: %2").arg(destPath, file.errorString()) };
    auto* reply = nam.get(req);
    QObject::connect(reply, &QNetworkReply::downloadProgress, [&prog](qint64 d, qint64 t) {
        if (prog)
            prog(d, t);
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &file,
                     [&]() { file.write(reply->readAll()); });

    QEventLoop loop;
    QTimer timer;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    bool timedOut = false;
    if (timeoutMs > 0) {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            reply->abort();
        });
        timer.start(timeoutMs);
    }
    QTimer cancelTimer;
    cancelTimer.setInterval(100);
    QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&]() { reply->abort(); });
    cancelTimer.start();
    loop.exec();
    cancelTimer.stop();
    file.write(reply->readAll());  // 余量
    file.close();

    SegmentResult out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (cancel.load(std::memory_order_relaxed))
        out.error = QStringLiteral("已取消");
    else if (timedOut)
        out.error = QStringLiteral("请求超时（%1ms）").arg(timeoutMs);
    else if (reply->error() != QNetworkReply::OperationCanceledError)
        out.error = reply->errorString();
    reply->deleteLater();
    // 同步完成删除，避免 nam 栈析构与排队的 deleteLater 双重释放（见上处注释）
    QCoreApplication::sendPostedEvents(reply, QEvent::DeferredDelete);
    return out;
}

quint64 fileSizeOf(const QString& path)
{
    const QFileInfo fi(path);
    return fi.exists() ? static_cast<quint64>(fi.size()) : 0;
}

}  // namespace

mw::Result<bool> downloadFile(const QString& url, const QString& destPath, const SourceOpts& opts,
    const QByteArray& ua, const std::atomic_bool& cancel,
    const std::function<void(quint64, quint64)>& prog, quint64 sizeHint,
    const std::function<void(const QString&)>& log, quint64* totalOut)
{
    if (cancel.load(std::memory_order_relaxed))
        return mw::Result<bool>::Err(QStringLiteral("已取消"));
    // mw::httpDownload 固定追加写：必须先探测服务端行为再决定续传或整档重下
    // （Rust 版是边发 Range 边看 206/200 分流，这里前置为一次 Range:0-0 探测）。
    auto probe = probeDownload(url, opts, ua);
    if (!probe)
        return mw::Result<bool>::Err(probe.error());
    const ProbeInfo probed = probe.value();
    const QString finalUrl = probed.finalUrl;
    quint64 total = probed.total;
    if (total == 0 && sizeHint > 0)
        total = sizeHint;
    if (totalOut)
        *totalOut = total;
    if (total > 0 && fileSizeOf(destPath) >= total)
        return mw::Result<bool>::Ok();  // 已是全量

    for (int attempt = 1;; ++attempt) {
        if (cancel.load(std::memory_order_relaxed))
            return mw::Result<bool>::Err(QStringLiteral("已取消"));
        quint64 have = fileSizeOf(destPath);
        const bool useRange = probed.rangeOk && have > 0 && (total == 0 || have < total);
        if (have > 0 && !useRange) {
            QFile::remove(destPath);  // 服务端不支持 Range：旧半档作废，整档重下
            have = 0;
        }
        QMap<QByteArray, QByteArray> headers;
        headers.insert("User-Agent", ua);
        if (useRange)
            headers.insert("Range", QByteArray("bytes=") + QByteArray::number(have) + "-");

        // 进度换算为全量口径（QNAM 报的是本段 Content-Length）+ 1s 节流（对齐 Rust）
        QElapsedTimer throttle;
        throttle.start();
        bool emitted = false;
        const auto segProg = [&](qint64 d, qint64 t) {
            if (!prog)
                return;
            const quint64 fullDone = have + (d > 0 ? static_cast<quint64>(d) : 0);
            quint64 fullTotal = total;
            if (fullTotal == 0 && t > 0)
                fullTotal = have + static_cast<quint64>(t);
            if (!emitted || throttle.elapsed() >= 1000) {
                emitted = true;
                throttle.restart();
                prog(fullDone, fullTotal);
            }
        };
        const SegmentResult seg =
            downloadSegment(finalUrl, destPath, headers, segProg, cancel, 120000, opts.proxy);
        if (cancel.load(std::memory_order_relaxed))
            return mw::Result<bool>::Err(QStringLiteral("已取消"));
        // 已是全量文件时续传请求返回 416（Range 不可满足）＝下载完成
        if (seg.status == 416 && have > 0)
            return mw::Result<bool>::Ok();
        if (seg.status != 200 && seg.status != 206) {
            if (attempt <= 5) {
                if (log)
                    log(QStringLiteral("下载中断（%1），第 %2 次续传重试…")
                            .arg(seg.status == 0
                                     ? (seg.error.isEmpty() ? QStringLiteral("网络错误") : seg.error)
                                     : QStringLiteral("HTTP %1").arg(seg.status))
                            .arg(attempt));
                QThread::sleep(2);
                continue;
            }
            return mw::Result<bool>::Err(
                QStringLiteral("网络请求失败（下载失败: %1）")
                    .arg(seg.status == 0
                             ? (seg.error.isEmpty() ? QStringLiteral("网络错误") : seg.error)
                             : QStringLiteral("HTTP %1").arg(seg.status)));
        }
        // 完整性：追加写无法中途分辨 200/206，用最终尺寸判定
        const quint64 fs = fileSizeOf(destPath);
        if (total == 0 || fs == total)
            return mw::Result<bool>::Ok();
        if (fs > total) {
            // 服务端忽略了 Range（整档追加进半档）→ 文件已污染，删掉重下
            QFile::remove(destPath);
            if (attempt <= 5) {
                if (log)
                    log(QStringLiteral("下载中断（文件尺寸异常 %1/%2），第 %3 次整档重试…")
                            .arg(fs)
                            .arg(total)
                            .arg(attempt));
                QThread::sleep(2);
                continue;
            }
            return mw::Result<bool>::Err(
                QStringLiteral("网络请求失败（下载失败：文件尺寸异常（%1/%2））").arg(fs).arg(total));
        }
        // fs < total：中断，续传重试
        if (attempt <= 5) {
            if (log)
                log(QStringLiteral("下载中断（%1/%2 字节），第 %3 次续传重试…")
                        .arg(fs)
                        .arg(total)
                        .arg(attempt));
            QThread::sleep(2);
            continue;
        }
        return mw::Result<bool>::Err(
            QStringLiteral("网络请求失败（下载中断: 尺寸不完整 %1/%2）").arg(fs).arg(total));
    }
}

}  // namespace detail

// ================= 校验 =================

namespace {

mw::Result<QByteArray> hashFileHex(const QString& path, QCryptographicHash::Algorithm algo,
    const std::atomic_bool& cancel, const std::function<void(quint64)>& onBytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return mw::err<QByteArray>(QStringLiteral("io: %1").arg(f.errorString()));
    QCryptographicHash h(algo);
    QByteArray buf(4 << 20, 0);
    quint64 done = 0;
    while (true) {
        if (cancel.load(std::memory_order_relaxed))
            return mw::err<QByteArray>(QStringLiteral("已取消"));
        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0)
            return mw::err<QByteArray>(QStringLiteral("io: %1").arg(f.errorString()));
        if (n == 0)
            break;
        h.addData(QByteArrayView(buf.constData(), static_cast<qsizetype>(n)));
        done += static_cast<quint64>(n);
        if (onBytes)
            onBytes(done);
    }
    return mw::Result<QByteArray>::Ok(h.result().toHex());
}

// ResolvedDownload → 公共 Resolved（version 完整形态 + versionName/versionCode/size 扁平镜像）
Resolved toPublic(const ResolvedInternal& r)
{
    Resolved p;
    p.url = r.url;
    p.version = r.version;
    p.versionName = r.version.name;
    p.versionCode = r.version.code;
    p.sha256 = r.sha256;
    p.md5 = r.md5;
    p.size = r.size;
    return p;
}

mw::Result<Resolved> toPublicResult(const mw::Result<ResolvedInternal>& r)
{
    if (!r)
        return mw::err<Resolved>(r.error());
    return mw::Result<Resolved>::Ok(toPublic(r.value()));
}

}  // namespace

mw::Result<QPair<QByteArray, QByteArray>> fetchApk(const Resolved& resolved, const QString& destPath,
    const QString& expectedPackage, const QString& appName, const QByteArray& ua,
    const SourceOpts& opts, const std::atomic_bool& cancel,
    const std::function<void(const QString&, quint64, quint64)>& progress,
    const std::function<void(const QString&)>& log)
{
    const auto prog = [&](const QString& phase, quint64 d, quint64 t) {
        if (progress)
            progress(phase, d, t);
    };
    const auto logLine = [&](const QString& m) {
        if (log)
            log(m);
    };

    const QString vname = resolved.versionName;
    const quint64 size = resolved.size;
    logLine(QStringLiteral("↓ %1 %2（%3）")
            .arg(appName, vname,
                 size > 0
                     ? QStringLiteral("%1 MB").arg(QString::number(size / 1048576.0, 'f', 1))
                     : QStringLiteral("大小未知")));
    quint64 total = 0;
    auto dl = detail::downloadFile(resolved.url, destPath, opts, ua, cancel,
                                   [&](quint64 d, quint64 t) { prog(QStringLiteral("下载"), d, t); },
                                   size, logLine, &total);
    if (!dl)
        return mw::err<QPair<QByteArray, QByteArray>>(dl.error());

    prog(QStringLiteral("校验"), 0, 0);
    const quint64 verifyTotal = std::max<quint64>(size > 0 ? size : total, 1);
    if (resolved.sha256 && !resolved.sha256->isEmpty()) {
        auto got = hashFileHex(destPath, QCryptographicHash::Sha256, cancel,
                               [&](quint64 d) { prog(QStringLiteral("校验"), d, verifyTotal); });
        if (!got)
            return mw::err<QPair<QByteArray, QByteArray>>(got.error());
        if (QString::fromLatin1(got.value()) != resolved.sha256->toLower()) {
            QFile::remove(destPath);
            return mw::err<QPair<QByteArray, QByteArray>>(QStringLiteral("哈希校验失败：期望 %1，实际 %2")
                                                        .arg(*resolved.sha256,
                                                             QString::fromLatin1(got.value())));
        }
        logLine(QStringLiteral("✓ sha256 校验通过"));
    } else if (resolved.md5 && !resolved.md5->isEmpty()) {
        auto got = hashFileHex(destPath, QCryptographicHash::Md5, cancel,
                               [&](quint64 d) { prog(QStringLiteral("校验"), d, verifyTotal); });
        if (!got)
            return mw::err<QPair<QByteArray, QByteArray>>(got.error());
        if (QString::fromLatin1(got.value()) != resolved.md5->toLower()) {
            QFile::remove(destPath);
            return mw::err<QPair<QByteArray, QByteArray>>(QStringLiteral("哈希校验失败：期望 %1，实际 %2")
                                                        .arg(*resolved.md5, QString::fromLatin1(got.value())));
        }
        logLine(QStringLiteral("✓ md5 校验通过"));
    } else {
        logLine(QStringLiteral("～ 源未提供哈希，跳过校验"));
    }

    // 包名核对：检索到的包与 APK 实际包名必须一致（防源串包/投喂错误文件）。
    // 解析统一走 mw::app 的 ZipArchive/parseManifest（安装链路同款、生产验证）：
    // apk_min 的最小实现在 daemon 下载线程上下文里有 Release 优化相关的段错误
    // （parseStringPool 的 QVector 分配必崩，崩溃地址=stringCount 小整数被当指针；
    //  Debug/ASan/gmalloc 均不复现），且 apk_min.hpp 本就规划统一到 mw::app。
    prog(QStringLiteral("检查"), 0, 0);
    auto zip = app::ZipArchive::open(destPath);
    if (!zip)
        return mw::err<QPair<QByteArray, QByteArray>>(QStringLiteral("APK 解析失败: %1").arg(zip.error()));
    auto axml = zip.value().readEntry(QStringLiteral("AndroidManifest.xml"));
    if (!axml)
        return mw::err<QPair<QByteArray, QByteArray>>(QStringLiteral("APK 解析失败: %1").arg(axml.error()));
    if (!axml.value() || axml.value()->isEmpty())
        return mw::err<QPair<QByteArray, QByteArray>>(
            QStringLiteral("页面/索引解析失败：APK 缺少 AndroidManifest.xml"));
    auto manifest = app::parseManifest(*axml.value(), destPath);
    if (!manifest)
        return mw::err<QPair<QByteArray, QByteArray>>(QStringLiteral("APK 解析失败: %1").arg(manifest.error()));
    const app::ManifestInfo& mi = manifest.value();
    if (mi.package != expectedPackage) {
        QFile::remove(destPath);
        return mw::err<QPair<QByteArray, QByteArray>>(
            QStringLiteral("APK 包名不符：期望 %1，实际 %2（源可能返回了错误文件，已删除）")
                .arg(expectedPackage, mi.package));
    }
    QString label = appName;
    if (mi.label && !mi.label->trimmed().isEmpty())
        label = *mi.label;
    logLine(QStringLiteral("✓ 包名核对通过（%1 %2）").arg(mi.package, vname));
    return mw::Result<QPair<QByteArray, QByteArray>>::Ok(
        qMakePair(label.toUtf8(), vname.toUtf8()));
}

// ================= 门面 =================

struct StoreHub::Impl {
    fdroid::FdroidSource fdroid;
    wandoujia::WandoujiaSource wdj;
};

StoreHub::StoreHub() : impl_(std::make_unique<Impl>()) {}
StoreHub::~StoreHub() = default;

QVector<SourceInfo> StoreHub::sources() const
{
    QVector<SourceInfo> out;
    SourceInfo f;
    f.id = QStringLiteral("fdroid");
    f.name = QStringLiteral("F-Droid");
    f.mirrors = fdroid::mirrors();
    out.push_back(std::move(f));
    SourceInfo w;
    w.id = QStringLiteral("wandoujia");
    w.name = QStringLiteral("豌豆荚");
    out.push_back(std::move(w));
    return out;
}

QPair<QVector<StoreApp>, QMap<QString, QString>> StoreHub::search(const QString& source,
    const QString& q, const SourceOpts& opts) const
{
    QVector<StoreApp> apps;
    QMap<QString, QString> errors;
    const auto run = [&](const char* id, const auto& fn) {
        auto r = fn();
        if (r)
            apps += r.value();
        else
            errors.insert(QString::fromLatin1(id), r.error());
    };
    if (source.isEmpty()) {
        run("fdroid", [&] { return impl_->fdroid.search(q, opts); });
        run("wandoujia", [&] { return impl_->wdj.search(q, opts); });
    } else if (source == QLatin1String("fdroid")) {
        run("fdroid", [&] { return impl_->fdroid.search(q, opts); });
    } else if (source == QLatin1String("wandoujia")) {
        run("wandoujia", [&] { return impl_->wdj.search(q, opts); });
    } else {
        errors.insert(source, QStringLiteral("未知来源"));
    }
    // 豌豆荚（国内源、结果相关度排序）在前，F-Droid（英文开源生态）在后
    const auto key = [](const StoreApp& a) { return a.source == QLatin1String("wandoujia") ? 0 : 1; };
    std::stable_sort(apps.begin(), apps.end(),
                     [&key](const StoreApp& a, const StoreApp& b) { return key(a) < key(b); });
    return { apps, errors };
}

mw::Result<QVector<StoreApp>> StoreHub::recommend(const QString& source, const SourceOpts& opts) const
{
    if (source.isEmpty() || source == QLatin1String("fdroid"))
        return impl_->fdroid.recommend(opts);
    return mw::err<QVector<StoreApp>>(
        QStringLiteral("未找到匹配的应用或版本：%1 暂不支持推荐").arg(source));
}

mw::Result<QVector<StoreVersion>> StoreHub::versions(const QString& source, const QString& appId,
    const QString& package, const SourceOpts& opts) const
{
    if (source == QLatin1String("fdroid"))
        return impl_->fdroid.versions(package, opts);
    if (source == QLatin1String("wandoujia"))
        return impl_->wdj.versions(appId, opts);
    return mw::err<QVector<StoreVersion>>(
        QStringLiteral("未找到匹配的应用或版本：未知来源 %1").arg(source));
}

mw::Result<Resolved> StoreHub::resolve(const QString& source, const QString& appId,
    const QString& package, std::optional<quint64> code, const QString& versionNameHint,
    const SourceOpts& opts) const
{
    if (source == QLatin1String("fdroid"))
        return toPublicResult(impl_->fdroid.resolve(package, code, opts));
    if (source == QLatin1String("wandoujia"))
        return toPublicResult(impl_->wdj.resolve(appId, code, versionNameHint, opts));
    return mw::err<Resolved>(QStringLiteral("未找到匹配的应用或版本：未知来源 %1").arg(source));
}

}  // namespace mw::store
