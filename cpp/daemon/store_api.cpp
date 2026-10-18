// 在线应用商店 API 实现（store_api.rs 移植）。
#include "store_api.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutexLocker>
#include <QRegularExpression>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include "adb.hpp" // mw::adb::findAdbBinary（真机安装通道）
#include "config.hpp"
#include "fs.hpp"
#include "httpserver.hpp"
#include "log.hpp"
#include "manager.hpp"
#include "state.hpp"
#include "store.hpp"

namespace mw::daemon {

namespace {

// 对齐 mwine_store::html::urldecode：%XX 解码 + '+' → 空格。
QString urldecode(const QString& s)
{
    const QByteArray bytes = s.toUtf8();
    QByteArray out;
    out.reserve(bytes.size());
    const auto hexv = [](char ch, bool* ok) -> int {
        if (ch >= '0' && ch <= '9') { *ok = true; return ch - '0'; }
        if (ch >= 'a' && ch <= 'f') { *ok = true; return ch - 'a' + 10; }
        if (ch >= 'A' && ch <= 'F') { *ok = true; return ch - 'A' + 10; }
        *ok = false;
        return 0;
    };
    int i = 0;
    while (i < bytes.size()) {
        const char c = bytes.at(i);
        if (c == '%' && i + 2 < bytes.size()) {
            bool ok1 = false, ok2 = false;
            const int v1 = hexv(bytes.at(i + 1), &ok1);
            const int v2 = hexv(bytes.at(i + 2), &ok2);
            if (ok1 && ok2) {
                out.append(char((v1 << 4) | v2));
                i += 3;
                continue;
            }
            out.append(c);
            ++i;
        } else if (c == '+') {
            out.append(' ');
            ++i;
        } else {
            out.append(c);
            ++i;
        }
    }
    return QString::fromUtf8(out);
}

// query 取值（对齐 Rust getq：split('?') 后按 & / = 切，键值都做 urldecode）。
std::optional<QString> getq(const QString& url, const QString& key)
{
    const int qi = url.indexOf(QLatin1Char('?'));
    if (qi < 0)
        return std::nullopt;
    const QString q = url.mid(qi + 1);
    for (const auto& kv : q.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq < 0) {
            if (urldecode(kv) == key)
                return QString();
            continue;
        }
        if (urldecode(kv.left(eq)) == key)
            return urldecode(kv.mid(eq + 1));
    }
    return std::nullopt;
}

QString apkDir()
{
    return QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/downloads/apk"));
}

// 请求未显式指定代理时沿用配置里的代理（用户环境需要代理时保持一致体验）。
store::SourceOpts optsFrom(const std::optional<QString>& proxyFromReq,
                           const std::optional<QString>& mirror)
{
    QString proxy = proxyFromReq.value_or(QString());
    if (!proxyFromReq.has_value()) {
        const QString p = config::VmConfig::load().proxy;
        if (!p.isEmpty())
            proxy = p;
    }
    store::SourceOpts opts;
    opts.mirror = mirror.value_or(QString());
    opts.proxy = proxy;
    return opts;
}

QString sanitize(const QString& s)
{
    QString out;
    for (const QChar& c : s) {
        const char16_t u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                        || (u >= '0' && u <= '9') || u == '.' || u == '_';
        out += ok ? c : QLatin1Char('_');
    }
    return out;
}

struct DownloadOutcome {
    QString path;   // 本地 APK 路径
    QString label;  // APK 内解析出的显示名
    QString vname;  // 版本名
};

// 下载任务主体：解析直链 → fetch_apk（下载/哈希/包名核对）。
mw::Result<DownloadOutcome> runDownload(
    store::StoreHub* hub, const QString& source, const QString& appId,
    const QString& package, const QString& appName, std::optional<quint64> versionCode,
    const QString& versionName, const store::SourceOpts& opts, std::atomic_bool& cancel,
    const std::function<void(const QString&, quint64, quint64)>& prog,
    const std::function<void(const QString&)>& log)
{
    prog(QStringLiteral("解析"), 0, 0);
    auto resolved = hub->resolve(source, appId, package, versionCode, versionName, opts);
    if (!resolved)
        return mw::err<DownloadOutcome>(resolved.error());
    QDir().mkpath(apkDir());
    const QString dest = QDir(apkDir()).filePath(
        QStringLiteral("%1_%2.apk").arg(sanitize(package)).arg(resolved.value().versionCode));
    // F-Droid 镜像（TUNA）拒绝浏览器 UA；豌豆荚 CDN 不挑
    const QByteArray ua = source == QStringLiteral("fdroid")
                              ? QByteArray(store::UA_NEUTRAL)
                              : QByteArray(store::UA_BROWSER);
    const store::FetchCallbacks cb{ log, prog };
    auto r = store::fetchApk(resolved.value(), dest, package, appName, ua, opts, cancel, cb);
    if (!r)
        return mw::err<DownloadOutcome>(r.error());
    DownloadOutcome out;
    out.path = dest;
    out.label = r.value().first;
    out.vname = r.value().second;
    return mw::Result<DownloadOutcome>::Ok(out);
}

} // namespace

void purgeStaleApks()
{
    const QString dir = apkDir();
    QDir d(dir);
    if (!d.exists())
        return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-7);
    for (const QFileInfo& fi : d.entryInfoList({ QStringLiteral("*.apk") }, QDir::Files)) {
        if (fi.lastModified() < cutoff)
            QFile::remove(fi.absoluteFilePath());
    }
}

Resp storeSources(AppState* state)
{
    QJsonArray sources;
    for (const auto& s : state->storeHub.sources()) {
        QJsonArray mirrors;
        for (const auto& m : s.mirrors) {
            QJsonObject mi;
            mi[QStringLiteral("id")] = m.id;
            mi[QStringLiteral("name")] = m.name;
            mi[QStringLiteral("url")] = m.url;
            mirrors.append(mi);
        }
        QJsonObject o;
        o[QStringLiteral("id")] = s.id;
        o[QStringLiteral("name")] = s.name;
        o[QStringLiteral("mirrors")] = mirrors;
        sources.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("sources")] = sources;
    return qMakePair(200, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

Resp storeSearch(AppState* state, const QString& url)
{
    const auto q = getq(url, QStringLiteral("q"));
    if (!q.has_value() || q->trimmed().isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 q"));
    const auto sourceOpt = getq(url, QStringLiteral("source"));
    QString source = sourceOpt.value_or(QString());
    const store::SourceOpts opts = optsFrom(getq(url, QStringLiteral("proxy")),
                                            getq(url, QStringLiteral("mirror")));
    // 首次搜索可能要拉 F-Droid 索引（gzip ~15MB），放宽到 6 分钟
    const auto result = state->storeHub.search(source, *q, opts);
    QJsonArray apps;
    for (const auto& a : result.first)
        apps.append(a.toJson());
    QJsonObject errors;
    for (auto it = result.second.constBegin(); it != result.second.constEnd(); ++it)
        errors[it.key()] = it.value();
    QJsonObject root;
    root[QStringLiteral("apps")] = apps;
    root[QStringLiteral("errors")] = errors;
    return qMakePair(200, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

Resp storeRecommend(AppState* state, const QString& url)
{
    const QString source = getq(url, QStringLiteral("source")).value_or(
        QStringLiteral("fdroid"));
    const store::SourceOpts opts = optsFrom(getq(url, QStringLiteral("proxy")),
                                            getq(url, QStringLiteral("mirror")));
    // 首次可能要拉 F-Droid 索引（gzip ~15MB），与 search 同口径
    const auto r = state->storeHub.recommend(source, opts);
    if (!r)
        return mw::jsonError(502, r.error());
    QJsonArray apps;
    for (const auto& a : r.value())
        apps.append(a.toJson());
    QJsonObject root;
    root[QStringLiteral("apps")] = apps;
    return qMakePair(200, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

Resp storeVersions(AppState* state, const QString& url)
{
    const QString source = getq(url, QStringLiteral("source")).value_or(QString());
    const QString appId = getq(url, QStringLiteral("app_id")).value_or(QString());
    const QString package = getq(url, QStringLiteral("package")).value_or(QString());
    if (source.isEmpty() || (appId.isEmpty() && package.isEmpty()))
        return mw::jsonError(400, QStringLiteral("缺少 source/app_id/package"));
    const store::SourceOpts opts = optsFrom(getq(url, QStringLiteral("proxy")),
                                            getq(url, QStringLiteral("mirror")));
    auto vs = state->storeHub.versions(source, appId, package, opts);
    if (!vs)
        return mw::jsonError(502, vs.error());

    // 已装版本号（VM 在线时查 dumpsys；失败/未装不阻断，前端据 0 判断）
    double installedCode = 0;
    {
        QString serial;
        {
            QMutexLocker l(&state->vmMu);
            serial = state->vmSerial;
        }
        if (serial.isEmpty()) {
            const auto dev = state->adb.findOnlineDevice();
            if (dev.ok() && dev.value().serial.startsWith(QStringLiteral("emulator-")))
                serial = dev.value().serial;
        }
        if (!serial.isEmpty() && !package.isEmpty()) {
            const auto out = state->adb.shellBlocking(
                serial,
                QStringLiteral("dumpsys package %1 2>/dev/null | grep -m1 'versionCode='")
                    .arg(package),
                5000);
            if (out) {
                static const QRegularExpression re(QStringLiteral("versionCode=(\\d+)"));
                const auto m = re.match(out.value().outStr());
                if (m.hasMatch())
                    installedCode = m.captured(1).toDouble();
            }
        }
    }

    QJsonArray arr;
    for (const auto& v : vs.value())
        arr.append(v.toJson());
    QJsonObject root;
    root[QStringLiteral("versions")] = arr;
    root[QStringLiteral("installed_code")] = installedCode;
    return qMakePair(200, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

Resp storeTaskGet(AppState* state)
{
    return qMakePair(200,
                     QJsonDocument(state->storeTask.toJson(/*includeStoreFields=*/true))
                         .toJson(QJsonDocument::Compact));
}

Resp storeCancel(AppState* state)
{
    const bool running = state->storeTask.running.load();
    if (!running)
        return mw::jsonError(409, QStringLiteral("当前没有进行中的下载"));
    state->storeCancel->store(true, std::memory_order_relaxed);
    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    return qMakePair(200, QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// ===================== APK 安装任务（daemon 端，字节级进度） =====================

Resp appsInstallTaskGet(AppState* state)
{
    return qMakePair(200,
                     QJsonDocument(state->installTask.toJson(/*includeStoreFields=*/true))
                         .toJson(QJsonDocument::Compact));
}

Resp appsInstallPost(AppState* state, const QByteArray& body)
{
    const QJsonObject req = QJsonDocument::fromJson(body).object();
    const QString path = req.value(QStringLiteral("path")).toString();
    if (path.isEmpty() || !QFileInfo::exists(path))
        return mw::jsonError(400, QStringLiteral("APK 文件不存在: %1").arg(path));
    const QString name = QFileInfo(path).fileName();

    // 单任务模型：与镜像/商店任务互斥语义一致（一次只装一个）
    if (!state->installTask.tryBegin(QStringLiteral("安装 %1").arg(name)))
        return mw::jsonError(409, QStringLiteral("已有安装任务在进行中"));

    std::thread([state, path, name,
                 target = req.value(QStringLiteral("serial")).toString().trimmed()]() mutable {
        // 目标设备：显式 serial（真机页安装）优先；否则 daemon 认养的 VM，
        // 无认养时退而取唯一在线模拟器
        QString serial = target;
        const bool toRealDevice =
            !serial.isEmpty() && !serial.startsWith(QStringLiteral("emulator-"));
        if (serial.isEmpty()) {
            QMutexLocker l(&state->vmMu);
            serial = state->vmSerial;
        }
        if (serial.isEmpty()) {
            const auto dev = state->adb.findOnlineDevice();
            if (dev.ok() && dev.value().serial.startsWith(QStringLiteral("emulator-")))
                serial = dev.value().serial;
        }
        if (serial.isEmpty()) {
            state->installTask.finishErr(QStringLiteral("虚拟机未运行"));
            return;
        }
        // 真机通道：adb server（无线 TLS 密钥/USB 由 server 掌握）；拉起并等待就绪
        mw::adb::AdbClient realClient;
        if (toRealDevice) {
            realClient = mw::adb::AdbClient::serverMode();
            if (!realClient.ensureServer(mw::adb::findAdbBinary()).ok()) {
                state->installTask.finishErr(QStringLiteral("adb server 未就绪（运行时未安装？）"));
                return;
            }
        }
        const mw::adb::AdbClient& client = toRealDevice ? realClient : state->adb;
        // 本地解析包名/标签/图标（供完成回执、前端刷新与安装进度卡展示）。
        // 图标写 install-icons/<pkg>.png（PNG 魔数校验，webp/xml 拒收），
        // 解析失败不阻塞安装——进度卡回退 APK 文件名。
        QString pkg, label, iconPath;
        {
            const auto local = mw::app::AdbAppManager::readLocalApk(path);
            if (local.ok()) {
                pkg = local.value().manifest.package;
                label = local.value().manifest.label.value_or(QString());
                const auto zipR = mw::app::ZipArchive::open(path);
                if (zipR) {
                    auto icon = mw::app::AdbAppManager::iconFromZip(zipR.value(),
                                                                   local.value().badging);
                    if (icon && icon->mime == mw::app::IconMime::Png
                        && icon->data.startsWith(QByteArrayLiteral("\x89PNG"))) {
                        const QString dir = QDir(mw::homeDir()).filePath(
                            QStringLiteral("Library/Application Support/MWine/install-icons"));
                        QDir().mkpath(dir);
                        const QString p = QDir(dir).filePath(pkg + QStringLiteral(".png"));
                        QFile f(p);
                        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                            f.write(icon->data);
                            f.close();
                            iconPath = p;
                        }
                    }
                }
            }
        }
        if (!label.isEmpty())
            state->installTask.setStoreMeta(pkg, label, QString(), iconPath);
        else if (!pkg.isEmpty())
            state->installTask.setStoreMeta(pkg, name, QString(), iconPath);
        auto r = client.installApkWithProgress(
            serial, path, 600000,
            [&state](const mw::adb::InstallProgress& p) {
                if (p.kind == mw::adb::InstallProgress::Kind::Push)
                    state->installTask.setProgress(QStringLiteral("传输 APK"), p.sent, p.total);
                else
                    state->installTask.setProgress(QStringLiteral("设备安装中"), 0, 0);
            });
        if (!r.ok()) {
            state->installTask.finishErr(r.error());
            return;
        }
        if (!pkg.isEmpty()) {
            state->installTask.log(
                QStringLiteral("已授予 %1 项运行时权限")
                    .arg(client.grantRuntimePermissions(serial, pkg).valueOr(0)));
        }
        state->installTask.finishStoreOk(
            path, pkg, label.isEmpty() ? name : label, QString());
    }).detach();
    return qMakePair(202, QJsonDocument(QJsonObject{ { QStringLiteral("ok"), true } })
                              .toJson(QJsonDocument::Compact));
}

Resp storeDownload(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QJsonObject v = doc.object();
    const QString source = v.value(QStringLiteral("source")).toString();
    const QString appId = v.value(QStringLiteral("app_id")).toString();
    const QString package = v.value(QStringLiteral("package")).toString();
    const QString appName = v.value(QStringLiteral("name")).toString();
    std::optional<quint64> versionCode;
    if (v.value(QStringLiteral("version_code")).isDouble())
        versionCode = static_cast<quint64>(v.value(QStringLiteral("version_code")).toDouble());
    std::optional<QString> versionName;
    if (v.contains(QStringLiteral("version_name"))
        && v.value(QStringLiteral("version_name")).isString())
        versionName = v.value(QStringLiteral("version_name")).toString();
    std::optional<QString> mirror;
    if (v.contains(QStringLiteral("mirror")) && v.value(QStringLiteral("mirror")).isString())
        mirror = v.value(QStringLiteral("mirror")).toString();
    std::optional<QString> proxy;
    if (v.contains(QStringLiteral("proxy")) && v.value(QStringLiteral("proxy")).isString())
        proxy = v.value(QStringLiteral("proxy")).toString();

    if (source.isEmpty() || package.isEmpty()
        || (source == QStringLiteral("wandoujia") && appId.isEmpty()))
        return mw::jsonError(400, QStringLiteral("缺少 source/package/app_id"));

    if (!state->storeTask.tryBegin(QStringLiteral("准备下载 %1…").arg(appName))) {
        QJsonObject o;
        o[QStringLiteral("ok")] = false;
        o[QStringLiteral("error")] = QStringLiteral("已有下载任务在进行中");
        return qMakePair(409, QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    auto flag = std::make_shared<std::atomic_bool>(false);
    state->storeCancel = flag;
    const store::SourceOpts opts = optsFrom(proxy, mirror);

    std::thread([state, flag, source, appId, package, appName, versionCode, versionName,
                 opts]() {
        auto log = [state](const QString& line) {
            MW_LOG("store", line);
            state->storeTask.log(line, 100);
        };
        auto prog = [state](const QString& phase, quint64 done, quint64 total) {
            state->storeTask.setProgress(phase, done, total);
        };
        auto result = runDownload(&state->storeHub, source, appId, package, appName,
                                  versionCode, versionName.value_or(QString()), opts, *flag,
                                  prog, log);
        if (result)
            state->storeTask.finishStoreOk(result.value().path, package,
                                           result.value().label, result.value().vname);
        else
            state->storeTask.finishErr(result.error());
    }).detach();

    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    return qMakePair(200, QJsonDocument(o).toJson(QJsonDocument::Compact));
}

} // namespace mw::daemon
