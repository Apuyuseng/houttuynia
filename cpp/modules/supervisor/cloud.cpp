// 镜像安装：本地导入 + Google 官方镜像拉取 ← crates/mwine-supervisor/src/cloud.rs
// 设计：「导入」安装本地云分发包 tar.zst；「官方拉取」从 dl.google.com 下载
// 官方 system image。HTTP（断点续传+代理）/zstd 解压/tar 解包/zip 解压/sha256/
// 稀疏化全部内部实现，不假设用户环境有 curl/zstd 等外部工具。
//
// 分发包格式：houttuynia-image-<version>.tar.zst
//   └─ houttuynia-image/          ← arm64-v8a 目录原样（symlink 保留）
//       ├─ manifest.json          ← 每文件 sha256 + 版本信息（内嵌校验）
//       └─ system-qemu.img / vendor-qemu.img / ramdisk-qemu.img / kernel-ranchu …
#include "supervisor.hpp"

#include <zlib.h>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <vector>

#include "compress.hpp"
#include "fs.hpp"
#include "json.hpp"

namespace mw::supervisor {

namespace {

const char* kSysimgRepo =
    "https://dl.google.com/android/repository/sys-img/android/sys-img2-1.xml";
const char* kSysimgBase = "https://dl.google.com/android/repository/sys-img/android/";

QString home()
{
    const QString h = mw::homeDir();
    return h.isEmpty() ? QStringLiteral("/tmp") : h;
}

QString dlDir()
{
    return QDir(home()).filePath(QStringLiteral(".mwine/downloads"));
}

bool checkCancel(const std::atomic_bool& c)
{
    return c.load(std::memory_order_relaxed);
}

// ================= 内部化基础能力（替代 curl/shasum/dd/tar/zstd/ditto） =================

} // namespace

// 流式 sha256（4MiB 分块，块级取消点）
mw::Result<QByteArray> sha256File(const QString& path, const std::atomic_bool& cancel,
                                  const std::function<void(quint64)>& onBytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return mw::err<QByteArray>(
            QStringLiteral("打开 %1 失败: %2").arg(path, f.errorString()));
    mw::IncrementalHash h(QCryptographicHash::Sha256);
    std::vector<char> buf(4u << 20);
    quint64 done = 0;
    while (true) {
        if (checkCancel(cancel))
            return mw::err<QByteArray>(QStringLiteral("已取消"));
        const qint64 n = f.read(buf.data(), qint64(buf.size()));
        if (n < 0)
            return mw::err<QByteArray>(QStringLiteral("读取 %1 失败").arg(path));
        if (n == 0)
            break;
        h.add(QByteArray(buf.data(), int(n)));
        done += quint64(n);
        if (onBytes)
            onBytes(done);
    }
    return mw::Result<QByteArray>::Ok(h.resultHex());
}

// 稀疏化：全零 4MiB 块跳过不写（APFS 打洞）。逻辑内容字节不变。
mw::Result<bool> sparseRewriteFile(const QString& path, const std::atomic_bool& cancel,
                                   const std::function<void(quint64)>& onBytes)
{
    QFile r(path);
    if (!r.open(QIODevice::ReadOnly))
        return mw::err<bool>(QStringLiteral("打开 %1 失败").arg(path));
    const QString tmp = path + QStringLiteral(".sparse.tmp");
    QFile w(tmp);
    if (!w.open(QIODevice::ReadWrite | QIODevice::Truncate))
        return mw::err<bool>(QStringLiteral("创建临时文件失败: %1").arg(w.errorString()));

    constexpr qint64 kBS = 4ll << 20;
    std::vector<char> buf(kBS);
    quint64 pos = 0;
    while (true) {
        if (checkCancel(cancel))
            return mw::err<bool>(QStringLiteral("已取消"));
        const qint64 n = r.read(buf.data(), kBS);
        if (n < 0)
            return mw::err<bool>(QStringLiteral("读取 %1 失败").arg(path));
        if (n == 0)
            break;
        bool zero = true;
        for (qint64 i = 0; i < n; ++i)
            if (buf[size_t(i)] != 0) {
                zero = false;
                break;
            }
        if (zero) {
            if (!w.seek(qint64(pos) + n))
                return mw::err<bool>(QStringLiteral("稀疏写失败"));
        } else if (w.write(buf.data(), n) != n) {
            return mw::err<bool>(QStringLiteral("稀疏写失败"));
        }
        pos += quint64(n);
        if (onBytes)
            onBytes(pos);
    }
    w.flush();
    w.close();
    if (!QFile::resize(tmp, qint64(pos)))
        return mw::err<bool>(QStringLiteral("收尾文件长度失败"));
    // POSIX rename 原子覆盖原文件
    if (::rename(tmp.toUtf8().constData(), path.toUtf8().constData()) != 0)
        return mw::err<bool>(QStringLiteral("替换原文件失败: %1").arg(path));
    return mw::Result<bool>::Ok();
}

namespace {

void applyProxy(QNetworkAccessManager& nam, const std::optional<QString>& proxy)
{
    if (!proxy || proxy->isEmpty())
        return;
    const QUrl u(*proxy);
    QNetworkProxy::ProxyType t = QNetworkProxy::DefaultProxy;
    const QString scheme = u.scheme().toLower();
    if (scheme.startsWith(QStringLiteral("http")))
        t = QNetworkProxy::HttpProxy;
    else if (scheme.startsWith(QStringLiteral("socks")))
        t = QNetworkProxy::Socks5Proxy;
    else
        return;
    nam.setProxy(QNetworkProxy(t, u.host(), quint16(u.port(0)), u.userName(), u.password()));
}

// HTTP GET 到字节（官方仓库清单用）
mw::Result<QByteArray> httpFetch(const QUrl& url, const std::optional<QString>& proxy)
{
    QNetworkAccessManager nam;
    applyProxy(nam, proxy);
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "Houttuynia/1.0");

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(30000);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QString err = reply->errorString();
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300)
        return mw::Result<QByteArray>::Ok(body);
    return mw::err<QByteArray>(QStringLiteral("请求 %1 失败: %2%3")
                                   .arg(url.toString(), err,
                                        status ? QStringLiteral(" (HTTP %1)").arg(status)
                                               : QString()));
}

// Content-Range: "bytes a-b/total" → total（0 = 未知）
quint64 contentRangeTotal(const QByteArray& v)
{
    const int slash = v.lastIndexOf('/');
    if (slash < 0)
        return 0;
    bool ok = false;
    const quint64 t = v.mid(slash + 1).trimmed().toULongLong(&ok);
    return ok ? t : 0;
}

// 下载到文件：Range 断点续传 + 失败重试（服务端不支持 Range 时自动整档重下）。
// 进度经 prog 回调按 ~2s 粒度汇报（phase 固定「下载」）。
mw::Result<bool> httpDownloadResume(const QUrl& url, const QString& dest,
                                    const std::optional<QString>& proxy, const LogFn& log,
                                    const std::atomic_bool& cancel, const ProgressFn& prog,
                                    quint64 sizeHint)
{
    QNetworkAccessManager nam;
    applyProxy(nam, proxy);
    int attempt = 0;
    while (true) {
        ++attempt;
        if (checkCancel(cancel))
            return mw::err<bool>(QStringLiteral("已取消"));
        quint64 have = QFileInfo::exists(dest) ? quint64(QFileInfo(dest).size()) : 0;
        // 断点续传的一致性锚点（上次下载的 ETag，sidecar 落盘）
        QString lastEtag;
        {
            QFile ef(dest + QStringLiteral(".etag"));
            if (have > 0 && ef.open(QIODevice::ReadOnly))
                lastEtag = QString::fromUtf8(ef.readAll().trimmed());
        }

        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setRawHeader("User-Agent", "Houttuynia/1.0");
        if (have > 0)
            req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(have) + "-");

        QFile file(dest);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
            return mw::err<bool>(
                QStringLiteral("打开 %1 失败: %2").arg(dest, file.errorString()));

        QNetworkReply* reply = nam.get(req);
        QEventLoop loop;
        QTimer cancelTimer;
        quint64 done = have;
        quint64 total = 0;
        bool writeFail = false;
        bool alreadyDone = false; // 416：个别服务器表示已下完
        bool etagMismatch = false; // 续传源变了：放弃断点，整档重下
        QElapsedTimer lastProg;
        lastProg.start();

        cancelTimer.setInterval(100);
        QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&] {
            if (checkCancel(cancel))
                reply->abort();
        });
        cancelTimer.start();

        QObject::connect(reply, &QNetworkReply::metaDataChanged, [&] {
            const int st =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // 跨源一致性：SourceForge 等按连接轮换镜像，不同镜像的产物可能不同
            //（同内容不同压缩）。续传时 ETag 变了 = 对面已不是当初那个文件，
            // 半截拼接必然损坏——中止本次，清空从头整档重下
            if (!lastEtag.isEmpty() && st != 416) {
                const QByteArray et = reply->rawHeader("ETag");
                if (!et.isEmpty() && et != lastEtag) {
                    etagMismatch = true;
                    reply->abort();
                    return;
                }
            }
            if (st == 206) {
                total = contentRangeTotal(reply->rawHeader("Content-Range"));
                if (total == 0) {
                    const QVariant cl = reply->header(QNetworkRequest::ContentLengthHeader);
                    if (cl.isValid())
                        total = have + cl.toULongLong();
                }
            } else if (st == 200) {
                // 服务端忽略 Range（从头给）：本地已有字节必须丢弃
                if (have > 0) {
                    file.resize(0);
                    done = 0;
                }
                const QVariant cl = reply->header(QNetworkRequest::ContentLengthHeader);
                if (cl.isValid())
                    total = cl.toULongLong();
            } else if (st == 416 && have > 0) {
                alreadyDone = true;
            }
            if (!etagMismatch && (st == 200 || st == 206)) {
                const QByteArray et = reply->rawHeader("ETag");
                if (!et.isEmpty())
                    mw::writeFile(dest + QStringLiteral(".etag"), et);
            }
        });
        QObject::connect(reply, &QNetworkReply::readyRead, [&] {
            const QByteArray chunk = reply->readAll();
            if (file.write(chunk) != chunk.size()) {
                writeFail = true; // 写失败按中断处理，走外层重试
                reply->abort();
                return;
            }
            done += quint64(chunk.size());
            if (lastProg.elapsed() >= 2000) { // 2 秒粒度上报结构化进度
                lastProg.restart();
                if (prog)
                    prog(QStringLiteral("下载"), done, total ? total : sizeHint);
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        cancelTimer.stop();
        file.flush();
        file.close();

        if (alreadyDone)
            return mw::Result<bool>::Ok();

        if (etagMismatch) {
            if (log)
                log(QStringLiteral("下载源已更换（镜像轮换），丢弃断点整档重下…"));
            QFile::remove(dest);
            QFile::remove(dest + QStringLiteral(".etag"));
            if (attempt <= 6)
                continue;
            return mw::err<bool>(QStringLiteral("下载源反复变更，请稍后重试"));
        }

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool netErr = reply->error() != QNetworkReply::NoError;
        const QString errStr = reply->errorString();
        reply->deleteLater();

        if (checkCancel(cancel))
            return mw::err<bool>(QStringLiteral("已取消"));
        if ((netErr || writeFail) && attempt <= 5) {
            if (log)
                log(QStringLiteral("下载中断（%1），第 %2 次续传重试…").arg(errStr).arg(attempt));
            QThread::sleep(2);
            continue;
        }
        if (netErr || writeFail) {
            if (writeFail)
                return mw::err<bool>(QStringLiteral("下载重试次数耗尽（写入失败）"));
            if (attempt > 5)
                return mw::err<bool>(QStringLiteral("下载重试次数耗尽"));
            return mw::err<bool>(QStringLiteral("下载失败: %1").arg(errStr));
        }
        if (status == 206 || status == 200) {
            // 尺寸兜底：服务端明示总长时，落盘尺寸必须一致（防半档假成功）
            if (total > 0 && quint64(QFileInfo(dest).size()) != total) {
                if (attempt <= 5) {
                    if (log)
                        log(QStringLiteral("下载不完整（%1/%2），重试…")
                                .arg(QFileInfo(dest).size())
                                .arg(total));
                    continue;
                }
                return mw::err<bool>(QStringLiteral("下载尺寸不符：%1/%2")
                                         .arg(QFileInfo(dest).size())
                                         .arg(total));
            }
            return mw::Result<bool>::Ok();
        }
        if (status == 416 && have > 0)
            return mw::Result<bool>::Ok();
        return mw::err<bool>(QStringLiteral("下载失败：HTTP %1").arg(status));
    }
}

} // namespace

// ================= 官方仓库清单 =====================

std::optional<QString> androidVersionName(const QString& api)
{
    QString n = api;
    if (n.startsWith(QStringLiteral("android-")))
        n.remove(0, 8);
    const QString head = n.section(QLatin1Char('-'), 0, 0);
    bool ok = false;
    const uint v = head.toUInt(&ok);
    if (!ok)
        return std::nullopt;
    const char* name = nullptr;
    switch (v) {
    case 19: name = "4.4"; break;
    case 21: name = "5.0"; break;
    case 22: name = "5.1"; break;
    case 23: name = "6.0"; break;
    case 24: name = "7.0"; break;
    case 25: name = "7.1"; break;
    case 26: name = "8.0"; break;
    case 27: name = "8.1"; break;
    case 28: name = "9"; break;
    case 29: name = "10"; break;
    case 30: name = "11"; break;
    case 31: name = "12"; break;
    case 32: name = "12L"; break;
    case 33: name = "13"; break;
    case 34: name = "14"; break;
    case 35: name = "15"; break;
    case 36: name = "16"; break;
    default: return std::nullopt;
    }
    return QStringLiteral("Android %1").arg(QString::fromLatin1(name));
}

mw::Result<QVector<OfficialImage>> parseRepoXml(const QString& xml, const QString& filter)
{
    QVector<OfficialImage> list;
    const auto pkgs = xml.split(QStringLiteral("<remotePackage "));
    for (int i = 1; i < pkgs.size(); ++i) {
        const QString& pkg = pkgs.at(i);
        const QStringList quoted = pkg.split(QLatin1Char('"'));
        const QString path = quoted.size() > 1 ? quoted.at(1) : QString();
        if (!path.startsWith(QStringLiteral("system-images;")))
            continue;
        // 本产品只在 Apple Silicon 上跑 arm64 模拟器：非 arm64-v8a 不列出
        if (!path.endsWith(QStringLiteral(";arm64-v8a")))
            continue;
        // 段扫描：<url>…</url> 与 <size>…</size>
        QString url;
        quint64 size = 0;
        for (const QString& seg : pkg.split(QLatin1Char('<'))) {
            if (url.isEmpty() && seg.startsWith(QStringLiteral("url>")))
                url = seg.mid(4).split(QLatin1Char('<')).value(0).trimmed();
            else if (size == 0 && seg.startsWith(QStringLiteral("size>"))) {
                bool ok = false;
                const quint64 s =
                    seg.mid(5).split(QLatin1Char('<')).value(0).trimmed().toULongLong(&ok);
                if (ok)
                    size = s;
            }
        }
        if (url.isEmpty())
            continue;
        if (!filter.isEmpty()) {
            const QString apiSeg = path.section(QLatin1Char(';'), 1, 1);
            const auto verName = androidVersionName(apiSeg);
            const bool hit = path.contains(filter) || (verName && verName->contains(filter));
            if (!hit)
                continue;
        }
        const QString apiSeg = path.section(QLatin1Char(';'), 1, 1);
        const auto verName = androidVersionName(apiSeg);
        const QString display = verName ? QStringLiteral("%1 · %2").arg(*verName, path) : path;
        list.push_back(OfficialImage{ path, size,
                                      QString::fromLatin1(kSysimgBase) + url, display });
    }
    std::sort(list.begin(), list.end(),
              [](const OfficialImage& a, const OfficialImage& b) { return b.path < a.path; });
    return mw::Result<QVector<OfficialImage>>::Ok(list);
}

mw::Result<QVector<OfficialImage>> officialList(const QString& filter,
                                                const std::optional<QString>& proxy)
{
    auto body = httpFetch(QUrl(QString::fromLatin1(kSysimgRepo)), proxy);
    if (!body)
        return mw::err<QVector<OfficialImage>>(body.error());
    return parseRepoXml(QString::fromUtf8(body.value()), filter.trimmed());
}

// ================= 安装落地（导入/官方共用） =====================

namespace {

// 把解压好的镜像目录稀疏化并原子落地到 SDK；installedDir 出参返回最终目录。
mw::Result<bool> installDir(const QString& inner, uint androidApi, const QString& name,
                            const Sdk& sdk, const LogFn& log, const std::atomic_bool& cancel,
                            const ProgressFn& prog, QString* installedDir)
{
    // 大文件打洞（逻辑 8.6G → 实占 ~1.3G）；symlink 不动（顺着链接会把链接盖成实体）
    const QDir innerDir(inner);
    const auto entries = innerDir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& e : entries) {
        if (e.isSymbolicLink())
            continue;
        const qint64 len = e.size();
        if (len < 100ll * 1024 * 1024)
            continue;
        if (checkCancel(cancel))
            return mw::err<bool>(QStringLiteral("已取消"));
        if (log)
            log(QStringLiteral("稀疏化 %1…").arg(e.fileName()));
        if (auto r = sparseRewriteFile(
                e.absoluteFilePath(), cancel,
                [&prog, len](quint64 d) {
                    if (prog)
                        prog(QStringLiteral("稀疏化"), d, quint64(len));
                });
            !r)
            return r;
    }

    const QDir destBase(
        QDir(sdk.systemImages)
            .filePath(QStringLiteral("android-%1/%2").arg(androidApi).arg(name)));
    const QString dest = destBase.filePath(QStringLiteral("arm64-v8a"));
    if (prog)
        prog(QStringLiteral("落地"), 0, 0);
    QDir().mkpath(destBase.absolutePath());
    // 覆盖语义：目标已存在 = 用户重拉刷新该镜像（UI 无 --name 入口，报错
    // 是死路）。原子换入：旧目录先改名（同卷 rename），新目录就位后删旧，
    // rename 失败回滚旧目录。运行中的 VM 持有旧 inode 不受影响，下次启动
    // 用新镜像（指纹变化 → 当次为冷启动，属预期）
    QString backup;
    if (QFileInfo::exists(dest)) {
        backup = dest + QStringLiteral(".old-%1")
                            .arg(QDateTime::currentMSecsSinceEpoch());
        if (::rename(dest.toUtf8().constData(), backup.toUtf8().constData()) != 0)
            return mw::err<bool>(
                QStringLiteral("旧镜像改名失败，无法覆盖：%1").arg(dest));
        if (log)
            log(QStringLiteral("旧镜像已移开，准备覆盖安装"));
    }
    if (::rename(inner.toUtf8().constData(), dest.toUtf8().constData()) != 0) {
        if (!backup.isEmpty())
            (void)::rename(backup.toUtf8().constData(), dest.toUtf8().constData());
        return mw::err<bool>(QStringLiteral("落地失败（rename %1 → %2）").arg(inner, dest));
    }
    if (!backup.isEmpty())
        QDir(backup).removeRecursively();
    QDir(QDir(home()).filePath(QStringLiteral(".mwine/downloads/stage")))
        .removeRecursively();
    if (log)
        log(QStringLiteral("✓ 已安装到 %1").arg(dest));
    if (installedDir)
        *installedDir = dest;
    return mw::Result<bool>::Ok();
}

} // namespace

// ================= 导入分发包 =====================

mw::Result<bool> importImage(const QString& tarZstPath, const std::optional<QString>& sha256,
                             const QString& name, const Sdk& sdk, const LogFn& log,
                             const std::atomic_bool& cancel, const ProgressFn& prog,
                             QString* installedDir)
{
    const qint64 size = QFileInfo::exists(tarZstPath) ? QFileInfo(tarZstPath).size() : 0;
    if (size < 10ll * 1024 * 1024)
        return mw::err<bool>(QStringLiteral("%1 仅 %2 字节，不是有效的镜像分发包")
                                 .arg(tarZstPath)
                                 .arg(size));
    QDir().mkpath(dlDir());

    if (prog)
        prog(QStringLiteral("准备"), 0, 0);
    if (sha256 && !sha256->isEmpty()) {
        auto got = sha256File(tarZstPath, cancel, [&prog, size](quint64 d) {
            if (prog)
                prog(QStringLiteral("校验整包"), d, quint64(size));
        });
        if (!got)
            return mw::err<bool>(got.error());
        if (QString::fromLatin1(got.value()) != sha256->toLower())
            return mw::err<bool>(QStringLiteral("sha256 不匹配：期望 %1，实际 %2")
                                     .arg(*sha256, QString::fromLatin1(got.value())));
        if (log)
            log(QStringLiteral("✓ 整包 sha256 校验通过"));
    }

    if (checkCancel(cancel))
        return mw::err<bool>(QStringLiteral("已取消"));
    if (prog)
        prog(QStringLiteral("解压"), 0, 0);
    const QString stage = QDir(dlDir()).filePath(QStringLiteral("stage"));
    QDir(stage).removeRecursively();
    QDir().mkpath(stage);
    if (auto r = extractTarZst(tarZstPath, stage); !r)
        return mw::err<bool>(r.error());
    if (checkCancel(cancel))
        return mw::err<bool>(QStringLiteral("已取消"));

    // 包内顶层目录（分发包布局：顶层为单一镜像目录）
    std::optional<QString> inner;
    {
        const QDir sd(stage);
        const auto es = sd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& e : es) {
            inner = e.absoluteFilePath();
            break;
        }
    }
    if (!inner)
        return mw::err<bool>(QStringLiteral("压缩包里找不到目录"));

    // manifest.json：{ version, android_api, files: { 相对路径: sha256 } }
    QFile mf(QDir(*inner).filePath(QStringLiteral("manifest.json")));
    if (!mf.open(QIODevice::ReadOnly))
        return mw::err<bool>(
            QStringLiteral("manifest.json 解析失败: 无法读取 %1").arg(mf.fileName()));
    const QByteArray mbytes = mf.readAll();
    mf.close();
    const QJsonObject m = QJsonDocument::fromJson(mbytes).object();
    if (m.isEmpty())
        return mw::err<bool>(QStringLiteral("manifest.json 解析失败"));
    const QString version = mw::jStr(m, "version");
    const uint api = uint(mw::jNum(m, "android_api", 33));
    QMap<QString, QString> files;
    {
        const QJsonObject fo = m.value(QStringLiteral("files")).toObject();
        for (auto it = fo.begin(); it != fo.end(); ++it)
            files.insert(it.key(), it.value().toString());
    }
    if (log)
        log(QStringLiteral("✓ 包版本 %1（%2 个文件待校验）").arg(version).arg(files.size()));

    // 校验阶段：按累计字节给整体百分比
    quint64 base = 0;
    quint64 totalBytes = 0;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        totalBytes += quint64(QFileInfo(QDir(*inner).filePath(it.key())).size());
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (checkCancel(cancel))
            return mw::err<bool>(QStringLiteral("已取消"));
        const QString& f = it.key();
        const QString want = it.value();
        const QString fp = QDir(*inner).filePath(f);
        const quint64 sz = quint64(QFileInfo(fp).size());
        auto got = sha256File(fp, cancel, [&prog, base, totalBytes](quint64 d) {
            if (prog)
                prog(QStringLiteral("校验文件"), base + d, totalBytes);
        });
        if (!got)
            return mw::err<bool>(got.error());
        if (QString::fromLatin1(got.value()) != want.toLower())
            return mw::err<bool>(QStringLiteral("%1 校验失败：期望 %2，实际 %3")
                                     .arg(f, want, QString::fromLatin1(got.value())));
        base += sz;
    }
    if (log)
        log(QStringLiteral("✓ 全部文件 sha256 校验通过"));

    return installDir(*inner, api, name, sdk, log, cancel, prog, installedDir);
}

// ================= Google 官方镜像拉取 =====================

mw::Result<bool> officialPull(const QString& target, const Sdk& sdk,
                              const std::optional<QString>& proxy, const LogFn& log,
                              const std::atomic_bool& cancel, const ProgressFn& prog,
                              QString* installedDir)
{
    auto list = officialList(target, proxy);
    if (!list)
        return mw::err<bool>(list.error());
    if (list.value().isEmpty())
        return mw::err<bool>(QStringLiteral("官方仓库里找不到匹配 %1 的镜像").arg(target));
    const OfficialImage& pick = list.value().first();
    const QStringList parts = pick.path.split(QLatin1Char(';'));
    const QString tag = parts.size() > 2 ? parts.at(2) : QStringLiteral("default");
    QString apiHead = parts.size() > 1 ? parts.at(1) : QStringLiteral("android-33");
    if (apiHead.startsWith(QStringLiteral("android-")))
        apiHead.remove(0, 8);
    apiHead = apiHead.section(QLatin1Char('-'), 0, 0);
    bool ok = false;
    uint api = apiHead.toUInt(&ok);
    if (!ok)
        api = 33; // 对齐 Rust unwrap_or(33)

    QDir().mkpath(dlDir());
    const QString zip = QDir(dlDir()).filePath(
        QStringLiteral("official-%1.zip")
            .arg(pick.url.section(QLatin1Char('/'), -1)));
    QFile::remove(zip);
    if (log)
        log(QStringLiteral("↓ 下载官方镜像 %1（%2 MiB）")
                .arg(pick.path)
                .arg(pick.size / 1048576));
    if (log)
        log(QStringLiteral("  %1").arg(pick.url));
    if (auto r = httpDownloadResume(QUrl(pick.url), zip, proxy, log, cancel, prog, pick.size);
        !r)
        return mw::err<bool>(r.error());
    if (checkCancel(cancel))
        return mw::err<bool>(QStringLiteral("已取消"));

    const QString stage = QDir(dlDir()).filePath(QStringLiteral("official-stage"));
    QDir(stage).removeRecursively();
    QDir().mkpath(stage);
    if (log)
        log(QStringLiteral("✓ 解压中…"));
    if (auto r = zipExtract(zip, stage); !r)
        return mw::err<bool>(r.error());
    if (checkCancel(cancel))
        return mw::err<bool>(QStringLiteral("已取消"));
    QFile::remove(zip);

    // 官方 zip 布局：顶层就是 <abi>/ 目录（内含 system.img 等稀疏镜像）
    std::optional<QString> inner;
    {
        const QDir sd(stage);
        const auto es = sd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& e : es) {
            inner = e.absoluteFilePath();
            break;
        }
    }
    if (!inner)
        return mw::err<bool>(QStringLiteral("官方 zip 里找不到目录"));
    return installDir(*inner, api, tag, sdk, log, cancel, prog, installedDir);
}


// ================= 社区镜像（非 Google 来源 GSI；与官方列表合并展示） =================

namespace {

struct CommunityEntry {
    const char* id;
    const char* display;
    const char* url;
    quint64 size;   // 字节（gz 压缩态）
    uint api;       // 配对基底的 API 级别
    const char* tag; // 落地 system-images/android-<api>/<tag>/
    bool enabled;   // false = 暂不列出（格式适配未完成）
};

// 目录来源与用途说明：
// - Lineage GSI（Andy Yan 社区构建，userdebug → 免授权 adb 可用）；
// - GSI 只含 system 分区，拉取时以本地同 API 完整镜像（AOSP 自建/官方）
//   为基底组装 kernel/vendor/ramdisk 等；
// - API 对齐：GSI 的 Android 版本必须 ≥ 基底 vendor 版本（Treble 规则）。
const CommunityEntry kCommunityImages[] = {
    // 暂缓上线：AOSP36 基底是 super 动态分区容器（LP 元数据 @2MB 对齐），
    // GSI 裸 system 分区 init 等不到 super 设备 10s 超时致命重启（实测）。
    // 拉取/解压/组装/落地管线已全通，差「GSI → super 打包」最后一步
    //（lpmake 级工程：LP 元数据重写 + system_ext/product/dlkm 并入）。
    // 完成后置 enabled=true 即对用户可见。
    { "lineage-gsi-23",
      "LineageOS 23.2 · 社区（Android 16 · userdebug · 无 Google 服务 · GSI）",
      "https://downloads.sourceforge.net/andyyan-gsi/lineage-pure-userdebug/"
      "lineage-23.2-20260210-UNOFFICIAL-gsi_arm64.img.gz",
      1127967997, 36, "lineage", true },
    { "aosp-gsi-17",
      "AOSP 纯净 · 社区（Android 17 · userdebug · 官方源码构建 · 抢先体验）",
      "https://downloads.sourceforge.net/andyyan-gsi/aosp-pure-userdebug/"
      "CP2A.260605.016-gsi_arm64-cp2a-userdebug.img.gz",
      934300000, 36, "aosp17", true },
    { "aosp-gsi-16",
      "AOSP 纯净 · 社区（Android 16 · userdebug · 官方源码构建）",
      "https://downloads.sourceforge.net/andyyan-gsi/aosp-pure-userdebug/"
      "BP4A.251205.006-gsi_arm64-bp4a-userdebug.img.gz",
      886100000, 36, "aosp16", true },
};

/// 流式 gunzip：inPath(.gz) → outPath（64KB 分片，整档解压不占内存）。
mw::Result<bool> gunzipFile(const QString& inPath, const QString& outPath,
                            const std::atomic_bool& cancel, const ProgressFn& prog,
                            quint64 rawEstimate)
{
    QFile in(inPath);
    if (!in.open(QIODevice::ReadOnly))
        return mw::err<bool>(QStringLiteral("io: 无法读取 %1").arg(inPath));
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return mw::err<bool>(QStringLiteral("io: 无法写入 %1").arg(outPath));

    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) // gzip 头
        return mw::err<bool>(QStringLiteral("解压初始化失败"));
    std::vector<char> inStore(65536), outStore(65536);
    char* inBuf = inStore.data();
    char* outBuf = outStore.data();
    quint64 written = 0;
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        if (checkCancel(cancel)) {
            inflateEnd(&zs);
            return mw::err<bool>(QStringLiteral("已取消"));
        }
        if (zs.avail_in == 0) {
            const qint64 n = in.read(inBuf, sizeof(inBuf));
            if (n <= 0)
                break;
            zs.next_in = reinterpret_cast<Bytef*>(inBuf);
            zs.avail_in = static_cast<uInt>(n);
        }
        zs.next_out = reinterpret_cast<Bytef*>(outBuf);
        zs.avail_out = sizeof(outBuf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return mw::err<bool>(QStringLiteral("gzip 解压失败: %1")
                                     .arg(QString::fromLatin1(zs.msg ? zs.msg : "?")));
        }
        const quint64 got = sizeof(outBuf) - zs.avail_out;
        if (got > 0) {
            if (out.write(outBuf, got) != qint64(got)) {
                inflateEnd(&zs);
                return mw::err<bool>(QStringLiteral("io: 写出失败 %1").arg(outPath));
            }
            written += got;
            if (prog && rawEstimate > 0 && (written % (64 << 20)) < (64 << 20))
                prog(QStringLiteral("解压"), written, rawEstimate);
        }
        if (ret == Z_BUF_ERROR && zs.avail_in == 0 && in.atEnd())
            break;
    }
    inflateEnd(&zs);
    out.close();
    if (ret != Z_STREAM_END)
        return mw::err<bool>(QStringLiteral("gzip 数据不完整"));
    return mw::Result<bool>::Ok();
}

/// Android sparse image 魔数（0x3AFF26ED）探测：true=sparse / false=raw /
/// nullopt=io 错误（Result<bool> 特化携带不了 false，用 optional）
std::optional<bool> peekSparse(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray head = f.read(4);
    return head.size() == 4 && quint8(head.at(0)) == 0x3A
           && quint8(head.at(1)) == 0xFF && quint8(head.at(2)) == 0x26
           && quint8(head.at(3)) == 0xED;
}

/// 把 GSI raw system 打进基底 super 容器：基底复制 + GSI 追加（1MB 对齐）+
/// LP 元数据重写（extent/group/blockdev + SHA256 校验和）+ GPT 扩 super 分区
///（含备份 GPT 迁移与 CRC32）。算法已在 Python 原型上实测可启动（2026-10-18）。
/// LP/GPT 结构按 AOSP liblp v10 与 GPT 标准硬编码解析，异常布局即报错。
mw::Result<bool> packGsiIntoSuper(const QString& baseSuper, const QString& gsiRaw,
                                  const QString& outSuper, const LogFn& log,
                                  const std::atomic_bool& cancel, const ProgressFn& prog)
{
    QFile baseF(baseSuper);
    if (!baseF.open(QIODevice::ReadOnly))
        return mw::err<bool>(QStringLiteral("io: 读基底失败 %1").arg(baseSuper));
    const quint64 baseSize = quint64(baseF.size());
    const quint64 gsiSize = QFileInfo(gsiRaw).size();
    if (baseSize < 0x300000 || (baseSize & 0x1FF) || (gsiSize & 0x1FF))
        return mw::err<bool>(QStringLiteral("镜像尺寸异常（非 512 对齐）"));

    // ---- 定位 super 头区（geometry gDla 扫描 2..4MB）----
    QByteArray head = baseF.read(0x400000);
    int geoOff = head.indexOf(QByteArray("\x67\x44\x6c\x61", 4)); // "gDla"
    if (geoOff < 0x100000)
        return mw::err<bool>(QStringLiteral("基底里找不到 LP geometry（非 super 布局？）"));
    const int metaOff = geoOff + 0x2000;

    // ---- LP 元数据读入 ----
    QByteArray hdr = head.mid(metaOff, 128);
    if (hdr.size() < 128 || quint32(quint8(hdr[0])) != 0x30)
        return mw::err<bool>(QStringLiteral("LP metadata 头缺失"));
    const quint32 tablesSize = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(hdr.constData()) + 44);
    QByteArray tables = head.mid(metaOff + 128, int(tablesSize));
    if (quint32(tables.size()) < tablesSize)
        return mw::err<bool>(QStringLiteral("LP 表读取不完整"));
    const auto u32at = [](const QByteArray& b, int off) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData()) + off);
    };
    const quint32 extOff = u32at(hdr, 92);   // extent 表描述符（offset,num,entry）
    const quint32 grpOff = u32at(hdr, 104);
    const quint32 devOff = u32at(hdr, 116);

    // ---- 新布局 ----
    const quint64 newOff = (baseSize + 0xFFFFF) & ~quint64(0xFFFFF);
    const quint64 gsiSec = ((gsiSize / 512) + 7) & ~quint64(7);
    const quint64 payloadEnd = newOff + gsiSec * 512;
    const quint64 newSize = (payloadEnd + 0x1FFFFF) & ~quint64(0xFFFFF); // 载荷后再留 ≥1MB（GPT 备份）
    if (log)
        log(QStringLiteral("super 打包: 基底 %1 GiB → %2 GiB（GSI system %3 GiB @+%4 MB）")
                .arg(baseSize / 1073741824.0, 0, 'f', 2)
                .arg(newSize / 1073741824.0, 0, 'f', 2)
                .arg(gsiSec * 512 / 1073741824.0, 0, 'f', 2)
                .arg((newOff - baseSize) >> 20));

    // ---- LP 表改写（little-endian 手写）----
    auto put64 = [](QByteArray& b, int off, quint64 v) {
        for (int i = 0; i < 8; ++i)
            b[off + i] = char(quint8(v >> (8 * i)));
    };
    auto put32 = [](QByteArray& b, int off, quint32 v) {
        for (int i = 0; i < 4; ++i)
            b[off + i] = char(quint8(v >> (8 * i)));
    };
    // extent[0]（system）：num_sectors(u64) device(u32) offset_super_relative(u32) pad(u64)
    const quint32 gsiOffInSuper = quint32((newOff - 0x200000) / 512); // super 分区起点 = GPT p2 @2MB
    put64(tables, int(extOff), gsiSec);
    put32(tables, int(extOff) + 8, 0);
    put32(tables, int(extOff) + 12, gsiOffInSuper);
    put64(tables, int(extOff) + 16, 0);
    // group[1].maximum_size = newSize（覆盖组上限）
    put64(tables, int(grpOff) + 48 + 40, newSize);
    // block_device[0].size = newSize
    put64(tables, int(devOff) + 16, newSize);
    // 校验和
    {
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(tables);
        hdr.replace(48, 32, h.result());
        QByteArray hc = hdr;
        memset(hc.data() + 12, 0, 32);
        QCryptographicHash hh(QCryptographicHash::Sha256);
        hh.addData(hc);
        hdr.replace(12, 32, hh.result());
    }
    const QByteArray meta = hdr + tables;

    // ---- 写出：基底复制 + GSI 追加 ----
    QFile outF(outSuper);
    if (outF.exists())
        outF.remove();
    // ReadWrite：GPT/LP 补丁段要从写出的文件里读回（WriteOnly 读为空，
    // 曾致「基底无 GPT」误报——2026-10-18 e2e 实测）
    if (!outF.open(QIODevice::ReadWrite | QIODevice::Truncate))
        return mw::err<bool>(QStringLiteral("io: 写出失败 %1").arg(outSuper));
    baseF.seek(0);
    // 大缓冲必须堆分配：std::thread 默认 512KB 栈，2MB 栈数组在函数入口
    // chkstk 即撞保护页 SIGBUS（2026-10-18 daemon「解压即死」崩溃根因）
    std::vector<char> bufStore(1 << 21);
    char* buf = bufStore.data();
    quint64 copied = 0;
    while (copied < baseSize) {
        if (checkCancel(cancel))
            return mw::err<bool>(QStringLiteral("已取消"));
        const qint64 n = baseF.read(buf, sizeof(buf));
        if (n <= 0 || outF.write(buf, n) != n)
            return mw::err<bool>(QStringLiteral("io: 基底复制中断"));
        copied += quint64(n);
        if (prog)
            prog(QStringLiteral("组装"), copied * 2 / 3, baseSize + gsiSize);
    }
    outF.resize(qint64(newOff));
    QFile gsiF(gsiRaw);
    if (!gsiF.open(QIODevice::ReadOnly))
        return mw::err<bool>(QStringLiteral("io: 读 GSI 失败"));
    copied = 0;
    while (copied < gsiSize) {
        if (checkCancel(cancel))
            return mw::err<bool>(QStringLiteral("已取消"));
        const qint64 n = gsiF.read(buf, sizeof(buf));
        if (n <= 0 || outF.write(buf, n) != n)
            return mw::err<bool>(QStringLiteral("io: GSI 写入中断"));
        copied += quint64(n);
        if (prog)
            prog(QStringLiteral("组装"), baseSize + copied * 1 / 3, baseSize + gsiSize);
    }
    outF.resize(qint64(newSize));
    for (int off = metaOff; off <= metaOff + 0x30000; off += 0x10000)
        outF.seek(off), outF.write(meta);

    // ---- GPT 扩 super 分区 ----
    outF.seek(512);
    QByteArray H = outF.read(92);
    if (H.size() < 92 || !H.startsWith("EFI PART"))
        return mw::err<bool>(QStringLiteral("基底无 GPT（非模拟器镜像布局？）"));
    outF.seek(1024);
    QByteArray E = outF.read(128 * 128);
    if (quint32(E.size()) < 128 * 128)
        return mw::err<bool>(QStringLiteral("GPT 项读取失败"));
    const quint64 total = newSize / 512;
    const quint64 newBak = total - 1;
    const quint64 newBakEnts = newBak - 32;
    const quint64 nlu = newBakEnts - 1;
    bool patched = false;
    for (int i = 0; i < 128; ++i) {
        QString name;
        for (int c = 0; c < 36; ++c) {
            const quint16 ch = quint16(quint8(E[i * 128 + 56 + c * 2])
                                       | (quint8(E[i * 128 + 56 + c * 2 + 1]) << 8));
            if (!ch)
                break;
            name.append(QChar(ch));
        }
        if (name != QLatin1String("super"))
            continue;
        put64(E, i * 128 + 40, nlu); // last_lba
        patched = true;
        break;
    }
    if (!patched)
        return mw::err<bool>(QStringLiteral("GPT 里找不到 super 分区"));
    const auto crc32buf = [](const char* d, int n) {
        return static_cast<quint32>(crc32(0L, reinterpret_cast<const Bytef*>(d), n));
    };
    put32(H, 88, crc32buf(E.constData(), E.size()));
    put64(H, 32, newBak);
    put64(H, 48, nlu);
    put32(H, 16, 0);
    put32(H, 16, crc32buf(H.constData(), 92));
    outF.seek(512);  outF.write(H);
    outF.seek(1024); outF.write(E);
    QByteArray BH = H;
    put64(BH, 24, newBak);
    put64(BH, 32, 1);
    put64(BH, 72, newBakEnts);
    put32(BH, 16, 0);
    put32(BH, 16, crc32buf(BH.constData(), 92));
    outF.seek(qint64(newBakEnts * 512)); outF.write(E);
    outF.seek(qint64(newBak * 512));     outF.write(BH);
    outF.close();
    return mw::Result<bool>::Ok();
}

mw::Result<bool> sparseToRaw(const QString& sparsePath, const QString& rawPath,
                             const std::atomic_bool& cancel)
{
    QFile in(sparsePath);
    if (!in.open(QIODevice::ReadOnly))
        return mw::err<bool>(QStringLiteral("io: %1").arg(sparsePath));
    quint32 blkSz = 0;
    quint32 nBlk = 0;
    {
        QByteArray h = in.read(28);
        if (h.size() < 28)
            return mw::err<bool>(QStringLiteral("sparse 头截断"));
        const auto u32 = [&](int off) {
            return quint32(quint8(h.at(off))) | (quint32(quint8(h.at(off + 1))) << 8)
                   | (quint32(quint8(h.at(off + 2))) << 16)
                   | (quint32(quint8(h.at(off + 3))) << 24);
        };
        blkSz = u32(12);
        nBlk = u32(16);
    }
    QFile out(rawPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return mw::err<bool>(QStringLiteral("io: %1").arg(rawPath));
    quint64 doneBlk = 0;
    while (doneBlk < nBlk) {
        if (checkCancel(cancel))
            return mw::err<bool>(QStringLiteral("已取消"));
        QByteArray ch = in.read(12);
        if (ch.size() < 12)
            return mw::err<bool>(QStringLiteral("sparse chunk 头截断"));
        const quint16 type = quint16(quint8(ch.at(0)) | (quint8(ch.at(1)) << 8));
        const quint32 n = quint32(quint8(ch.at(4))) | (quint32(quint8(ch.at(5))) << 8)
                          | (quint32(quint8(ch.at(6))) << 16)
                          | (quint32(quint8(ch.at(7))) << 24);
        const quint64 bytes = quint64(n) * blkSz;
        if (type == 0xCAC1) { // RAW
            quint64 left = bytes;
            char buf[65536];
            while (left > 0) {
                const qint64 r = in.read(buf, int(qMin<quint64>(left, sizeof(buf))));
                if (r <= 0)
                    return mw::err<bool>(QStringLiteral("sparse 数据截断"));
                if (out.write(buf, r) != r)
                    return mw::err<bool>(QStringLiteral("io: 写出失败"));
                left -= quint64(r);
            }
        } else if (type == 0xCAC2) { // FILL：4 字节重复
            QByteArray pat = in.read(4);
            if (pat.size() < 4)
                return mw::err<bool>(QStringLiteral("sparse fill 截断"));
            QByteArray block(int(qMin<quint64>(bytes, 1 << 20)), Qt::Uninitialized);
            for (int i = 0; i < block.size(); i += 4)
                memcpy(block.data() + i, pat.constData(), 4);
            quint64 left = bytes;
            while (left > 0) {
                const int w = int(qMin<quint64>(left, quint64(block.size())));
                if (out.write(block.constData(), w) != w)
                    return mw::err<bool>(QStringLiteral("io: 写出失败"));
                left -= quint64(w);
            }
        } else if (type == 0xCAC3) { // DONT_CARE
            if (!in.seek(in.pos() + qint64(bytes)))
                return mw::err<bool>(QStringLiteral("sparse 跳越截断"));
        } else {
            return mw::err<bool>(QStringLiteral("不支持的 sparse chunk 0x%1")
                                     .arg(type, 4, 16));
        }
        doneBlk += n;
    }
    return mw::Result<bool>::Ok();
}

/// 基底镜像目录：同 API 的完整本地镜像（含 kernel+vendor），排除目标 tag 自身
QString findBaseImageDir(const Sdk& sdk, uint api, const QString& excludeTag)
{
    const QDir base(QDir(sdk.systemImages)
                        .filePath(QStringLiteral("android-%1").arg(api)));
    // 优先 default（AOSP/官方），其余自建 tag 兜底
    QStringList tags = { QStringLiteral("default") };
    const auto dirs = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& d : dirs)
        if (d != excludeTag && !tags.contains(d))
            tags.append(d);
    for (const QString& tag : tags) {
        if (tag == excludeTag)
            continue;
        const QString dir = base.filePath(tag + QStringLiteral("/arm64-v8a"));
        if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("kernel-ranchu")))
            && (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("vendor.img")))
                || QFileInfo::exists(QDir(dir).filePath(QStringLiteral("vendor-qemu.img")))))
            return dir;
    }
    return QString();
}

} // namespace

QVector<OfficialImage> communityList(const QString& filter)
{
    QVector<OfficialImage> out;
    const QString f = filter.trimmed();
    for (const auto& e : kCommunityImages) {
        if (!e.enabled)
            continue;
        const OfficialImage img{
            QStringLiteral("community;%1;arm64-v8a").arg(QLatin1String(e.id)),
            e.size, QString::fromUtf8(e.url), QString::fromUtf8(e.display),
            QStringLiteral("community")
        };
        if (!f.isEmpty() && !img.display.contains(f, Qt::CaseInsensitive)
            && !img.path.contains(f, Qt::CaseInsensitive))
            continue;
        out.push_back(img);
    }
    return out;
}

mw::Result<bool> communityPull(const QString& id, const Sdk& sdk,
                               const std::optional<QString>& proxy, const LogFn& log,
                               const std::atomic_bool& cancel, const ProgressFn& prog,
                               QString* installedDir)
{
    const CommunityEntry* pick = nullptr;
    for (const auto& e : kCommunityImages)
        if (id == QLatin1String(e.id))
            pick = &e;
    if (!pick)
        return mw::err<bool>(QStringLiteral("未知社区镜像: %1").arg(id));

    // 基底：同 API 完整镜像（kernel/vendor/ramdisk 提供方）
    const QString baseDir = findBaseImageDir(sdk, pick->api, QLatin1String(pick->tag));
    if (baseDir.isEmpty())
        return mw::err<bool>(QStringLiteral(
            "缺少 Android %1 基础镜像：社区 GSI 只含 system 分区，需要本地同版本"
            "完整镜像（kernel/vendor）作基底；请先拉取/导入官方或自建 Android %1 镜像")
                                 .arg(pick->api));
    if (log)
        log(QStringLiteral("基底: %1").arg(baseDir));

    QDir().mkpath(dlDir());
    // 已存在完整/半档文件由 httpDownloadResume 断点续传或 416 直接判定完成
    const QString gz = QDir(dlDir()).filePath(
        QStringLiteral("community-%1.img.gz").arg(QLatin1String(pick->id)));
    if (log)
        log(QStringLiteral("↓ 下载 %1（%2 GiB，SourceForge 社区源）")
                .arg(QLatin1String(pick->id))
                .arg(pick->size / 1073741824.0, 0, 'f', 1));
    if (auto r = httpDownloadResume(QUrl(QString::fromLatin1(pick->url)), gz, proxy,
                                    log, cancel, prog, pick->size);
        !r)
        return mw::err<bool>(r.error());
    if (checkCancel(cancel))
        return mw::err<bool>(QStringLiteral("已取消"));

    // 解压 + 稀疏转换 → 装配目录
    const QString stage = QDir(dlDir()).filePath(QStringLiteral("community-stage"));
    QDir(stage).removeRecursively();
    const QString inner = QDir(stage).filePath(QStringLiteral("arm64-v8a"));
    QDir().mkpath(inner);
    if (log)
        log(QStringLiteral("✓ 解压 system 分区…"));
    const QString rawGsi = QDir(stage).filePath(QStringLiteral("gsi-raw.img"));
    if (auto r = gunzipFile(gz, rawGsi, cancel, prog, pick->size * 3); !r)
        return mw::err<bool>(r.error());
    QFile::remove(gz);
    if (checkCancel(cancel))
        return mw::err<bool>(QStringLiteral("已取消"));

    const auto isSparse = peekSparse(rawGsi);
    if (!isSparse)
        return mw::err<bool>(QStringLiteral("io: 读取 %1 失败").arg(rawGsi));
    QString gsiFinal = rawGsi;
    if (*isSparse) {
        gsiFinal = QDir(stage).filePath(QStringLiteral("gsi-raw2.img"));
        if (log)
            log(QStringLiteral("✓ sparse → raw 转换…"));
        if (auto r = sparseToRaw(rawGsi, gsiFinal, cancel); !r)
            return mw::err<bool>(r.error());
        QFile::remove(rawGsi);
    }
    // GSI 打进基底 super 容器（模拟器镜像 = GPT 磁盘 + super 动态分区；
    // 裸 system 分区 init 等不到 super 设备必死——2026-10-18 实测定论）
    const QString baseSuper = QDir(baseDir).filePath(QStringLiteral("system.img"));
    const QString systemImg = QDir(inner).filePath(QStringLiteral("system.img"));
    if (auto r = packGsiIntoSuper(baseSuper, gsiFinal, systemImg, log, cancel, prog); !r)
        return mw::err<bool>(r.error());
    QFile::remove(gsiFinal);

    // 基底拷贝（除 system 分区与包元数据；symlink 保持）
    if (log)
        log(QStringLiteral("✓ 组装（基底 kernel/vendor/ramdisk + GSI system）…"));
    {
        const QDir b(baseDir);
        const auto es = b.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo& e : es) {
            const QString name = e.fileName();
            if (name == QLatin1String("system.img") || name == QLatin1String("system-qemu.img")
                || name == QLatin1String("source.properties")
                || name.startsWith(QStringLiteral("userdata")))
                continue;
            const QString dst = QDir(inner).filePath(name);
            if (e.isDir()) {
                if (!QDir().rename(e.absoluteFilePath(), dst)
                    && !QDir(dst).removeRecursively())
                    return mw::err<bool>(QStringLiteral("io: 拷贝目录失败 %1").arg(name));
            } else if (!QFile::copy(e.absoluteFilePath(), dst)) {
                return mw::err<bool>(QStringLiteral("io: 拷贝失败 %1").arg(name));
            }
        }
    }
    // 目录级元数据：API 与基底一致；tag 标注社区来源
    {
        QFile sp(QDir(inner).filePath(QStringLiteral("source.properties")));
        if (sp.open(QIODevice::WriteOnly)) {
            sp.write(QStringLiteral("Pkg.Desc=Community GSI (LineageOS)\n"
                                    "AndroidVersion.ApiLevel=%1\n"
                                    "SystemImage.Abi=arm64-v8a\n"
                                    "SystemImage.TagId=%2\n"
                                    "SystemImage.TagDisplay=LineageOS (community GSI)\n")
                         .arg(pick->api)
                         .arg(QLatin1String(pick->tag))
                         .toUtf8());
            sp.close();
        }
    }
    if (checkCancel(cancel))
        return mw::err<bool>(QStringLiteral("已取消"));
    return installDir(inner, pick->api, QLatin1String(pick->tag), sdk, log, cancel,
                      prog, installedDir);
}

} // namespace mw::supervisor
