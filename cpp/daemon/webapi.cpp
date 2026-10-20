#include "webapi.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "appcache.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "routes.hpp"
#include "state.hpp"
#include "store_api.hpp"
#include "streamweb.hpp"

namespace mw::daemon {

namespace {

// ===================== 静态资源 =====================

QByteArray mimeFor(const QString& suffix)
{
    static const QMap<QString, QByteArray> m = {
        { QStringLiteral("html"), "text/html; charset=utf-8" },
        { QStringLiteral("js"), "text/javascript; charset=utf-8" },
        { QStringLiteral("mjs"), "text/javascript; charset=utf-8" },
        { QStringLiteral("css"), "text/css; charset=utf-8" },
        { QStringLiteral("json"), "application/json" },
        { QStringLiteral("png"), "image/png" },
        { QStringLiteral("jpg"), "image/jpeg" },
        { QStringLiteral("jpeg"), "image/jpeg" },
        { QStringLiteral("svg"), "image/svg+xml" },
        { QStringLiteral("webp"), "image/webp" },
        { QStringLiteral("ico"), "image/x-icon" },
        { QStringLiteral("woff"), "font/woff" },
        { QStringLiteral("woff2"), "font/woff2" },
        { QStringLiteral("ttf"), "font/ttf" },
        { QStringLiteral("map"), "application/json" },
        { QStringLiteral("txt"), "text/plain; charset=utf-8" },
        { QStringLiteral("webmanifest"), "application/manifest+json" },
    };
    return m.value(suffix.toLower(), QByteArrayLiteral("application/octet-stream"));
}

mw::HttpResponse serveFile(const QString& absPath, bool headOnly = false)
{
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) {
        const auto e = mw::jsonError(404, QStringLiteral("not found"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    const QByteArray data = f.readAll();
    // 静态文件上限 64MB（超出多半是误配路径）
    if (data.size() > 64 * 1024 * 1024) {
        const auto e = mw::jsonError(413, QStringLiteral("文件过大"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    mw::HttpResponse r;
    r.code = 200;
    r.contentType = mimeFor(QFileInfo(absPath).suffix());
    r.body = headOnly ? QByteArray() : data;
    // HTML 不缓存（发版即生效）；带扩展名的静态资源短缓存
    if (r.contentType.startsWith("text/html"))
        r.extraHeaders.insert("Cache-Control", "no-cache");
    else
        r.extraHeaders.insert("Cache-Control", "public, max-age=300");
    return r;
}

// 静态路由：净化路径（拒 .. 穿越），目录回落 index.html；非 /api 且
// 无扩展名的路径按 SPA 前端路由处理回落 index.html。
mw::HttpResponse serveStatic(const QString& webDir, const QString& urlPath)
{
    QString rel = urlPath.section(QLatin1Char('?'), 0, 0);
    if (rel.isEmpty() || rel == QLatin1String("/"))
        rel = QStringLiteral("/index.html");
    // 归一化：去掉重复的 /，拒绝任何 .. 段
    const QStringList segs = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& s : segs) {
        if (s == QLatin1String("..") || s.contains(QLatin1Char('\\')) || s.contains(QLatin1Char('\0'))) {
            const auto e = mw::jsonError(403, QStringLiteral("非法路径"));
            return mw::HttpResponse::json(e.first, e.second);
        }
    }
    const QString clean = segs.join(QLatin1Char('/'));
    const QString abs = QDir(webDir).absoluteFilePath(clean);
    const QString canonical = QFileInfo(abs).canonicalFilePath();
    const QString canonicalRoot = QFileInfo(webDir).canonicalFilePath();
    // 符号链接穿越防护：落点必须仍在 webDir 内
    if (canonical.isEmpty() || canonicalRoot.isEmpty()
        || !(canonical == canonicalRoot
             || canonical.startsWith(canonicalRoot + QLatin1Char('/')))) {
        // SPA 回落：无扩展名的路径交给前端路由
        if (!clean.contains(QLatin1Char('.'))) {
            const QString index = QDir(webDir).absoluteFilePath(QStringLiteral("index.html"));
            if (QFileInfo::exists(index))
                return serveFile(index);
        }
        const auto e = mw::jsonError(404, QStringLiteral("not found"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    if (QFileInfo(canonical).isDir())
        return serveFile(QDir(canonical).absoluteFilePath(QStringLiteral("index.html")));
    return serveFile(canonical);
}

// ===================== Token 鉴权 =====================

QString expectedToken()
{
    return qEnvironmentVariable("MWINE_TOKEN").trimmed();
}

bool authorized(const mw::HttpRequest& req, const QString& token)
{
    if (token.isEmpty())
        return true;
    const QString hdr = req.headers.value(QStringLiteral("authorization"));
    if (hdr == QStringLiteral("Bearer %1").arg(token))
        return true;
    // 浏览器 <img> 标签无法带头的场景（截图/图标）允许 query 携带
    if (req.query(QStringLiteral("token")) == token)
        return true;
    return false;
}

mw::HttpResponse unauthorized()
{
    const auto e = mw::jsonError(401, QStringLiteral("未授权：请提供访问令牌"));
    auto r = mw::HttpResponse::json(e.first, e.second);
    r.extraHeaders.insert("WWW-Authenticate", "Bearer");
    return r;
}

// ===================== 扩展 API（二进制 / 上传） =====================

// 目标 serial：query 指定 > daemon 收养 > 唯一在线模拟器
QString resolveSerial(const mw::HttpRequest& req, AppState* state)
{
    QString serial = req.query(QStringLiteral("serial")).trimmed();
    if (!serial.isEmpty())
        return serial;
    serial = state->currentSerial();
    if (!serial.isEmpty())
        return serial;
    const auto dev = state->adb.findOnlineDevice();
    if (dev.ok())
        return dev.value().serial;
    return QString();
}

// GET /api/screenshot.png?serial=&token= —— 设备屏幕 PNG 直出（不落盘）
mw::HttpResponse screenshotPng(AppState* state, const mw::HttpRequest& req)
{
    const QString serial = resolveSerial(req, state);
    if (serial.isEmpty()) {
        const auto e = mw::jsonError(503, QStringLiteral("Android 未运行"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    auto out = state->adb.shellBlocking(serial, QStringLiteral("screencap -p"), 15000);
    if (!out) {
        const auto e = mw::jsonError(500, out.error());
        return mw::HttpResponse::json(e.first, e.second);
    }
    if (!(out.value().out.size() > 8
          && out.value().out.startsWith(QByteArray("\x89" "PNG", 4)))) {
        const auto e = mw::jsonError(500, QStringLiteral("截图失败"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    mw::HttpResponse r;
    r.code = 200;
    r.contentType = QByteArrayLiteral("image/png");
    r.body = out.value().out;
    r.extraHeaders.insert("Cache-Control", "no-store");
    return r;
}

// GET /api/apps/icon?pkg= —— 应用图标 PNG（appcache 图标库 + 安装任务图标目录）
mw::HttpResponse appIcon(AppState* state, const mw::HttpRequest& req)
{
    static const QRegularExpression kPkg(QStringLiteral("^[A-Za-z0-9_.]+$"));
    const QString pkg = req.query(QStringLiteral("pkg"));
    if (!kPkg.match(pkg).hasMatch()) {
        const auto e = mw::jsonError(400, QStringLiteral("包名非法"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    QString path = state->appCache()->describe(pkg)
                       .value(QStringLiteral("icon"))
                       .toString();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        // 安装任务图标（安装刚完成、缓存尚未抓取的窗口期）
        const QString alt = QDir(mw::homeDir())
                                .filePath(QStringLiteral(
                                    "Library/Application Support/MWine/install-icons/%1.png")
                                              .arg(pkg));
        if (QFileInfo::exists(alt))
            path = alt;
    }
    if (path.isEmpty()) {
        const auto e = mw::jsonError(404, QStringLiteral("图标未就绪"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    // 只允许出两个已知图标目录，防任意文件读取
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString cacheRoot =
        QFileInfo(QDir(mw::mwineDir()).filePath(QStringLiteral("appcache")))
            .canonicalFilePath();
    const QString installRoot =
        QFileInfo(QDir(mw::homeDir())
                      .filePath(QStringLiteral(
                          "Library/Application Support/MWine/install-icons")))
            .canonicalFilePath();
    const bool underCache = !cacheRoot.isEmpty()
        && canonical.startsWith(cacheRoot + QLatin1Char('/'));
    const bool underInstall = !installRoot.isEmpty()
        && canonical.startsWith(installRoot + QLatin1Char('/'));
    if (!underCache && !underInstall) {
        const auto e = mw::jsonError(403, QStringLiteral("图标路径越界"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    QFile f(canonical);
    if (!f.open(QIODevice::ReadOnly)) {
        const auto e = mw::jsonError(404, QStringLiteral("图标读取失败"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    const QByteArray data = f.readAll();
    if (!data.startsWith(QByteArray("\x89" "PNG", 4))) {
        const auto e = mw::jsonError(404, QStringLiteral("图标格式不支持"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    mw::HttpResponse r;
    r.code = 200;
    r.contentType = QByteArrayLiteral("image/png");
    r.body = data;
    r.extraHeaders.insert("Cache-Control", "public, max-age=300");
    return r;
}

// GET /api/wallpaper.png —— 设备壁纸（概览页背景装饰用；未就绪 404）
mw::HttpResponse wallpaperPng(AppState* state)
{
    const QString p = state->appCache()->wallpaperPath();
    if (p.isEmpty() || !QFileInfo::exists(p)) {
        const auto e = mw::jsonError(404, QStringLiteral("壁纸未就绪"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) {
        const auto e = mw::jsonError(404, QStringLiteral("壁纸读取失败"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    mw::HttpResponse r;
    r.code = 200;
    r.contentType = QByteArrayLiteral("image/png");
    r.body = f.readAll();
    r.extraHeaders.insert("Cache-Control", "public, max-age=60");
    return r;
}

// POST /api/apps/install-upload?name=x.apk&serial=
// 原始 APK 字节（application/octet-stream），落盘 ~/.mwine/uploads 后
// 复用既有安装任务（与 /api/apps/install 同语义，JSON 应答）。
mw::HttpResponse installUpload(AppState* state, const mw::HttpRequest& req)
{
    // 体积上限 512MB（content-length 预检 + 实际双保险）
    const qint64 declared = req.headers.value(QStringLiteral("content-length")).toLongLong();
    if (declared > 512LL * 1024 * 1024 || req.body.size() > 512LL * 1024 * 1024) {
        const auto e = mw::jsonError(413, QStringLiteral("APK 超过 512MB 上限"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    if (req.body.size() < 4 || !req.body.startsWith(QByteArray("PK\x03\x04", 4))) {
        const auto e = mw::jsonError(400, QStringLiteral("不是有效的 APK（zip）文件"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    QString name = req.query(QStringLiteral("name"));
    name = QFileInfo(name).fileName(); // 剥掉任何路径成分
    if (name.isEmpty())
        name = QStringLiteral("upload.apk");
    if (!name.endsWith(QLatin1String(".apk"), Qt::CaseInsensitive))
        name += QStringLiteral(".apk");

    const QString dir = QDir(mw::mwineDir()).filePath(QStringLiteral("uploads"));
    QDir().mkpath(dir);
    // 顺手清理 24h 前的历史上传
    for (const QFileInfo& fi :
         QDir(dir).entryInfoList(QDir::Files, QDir::Time)) {
        if (fi.lastModified().secsTo(QDateTime::currentDateTime()) > 24 * 3600)
            QFile::remove(fi.absoluteFilePath());
    }
    const QString path = QDir(dir).filePath(
        QStringLiteral("%1-%2").arg(QDateTime::currentDateTime().toString(
                                        QStringLiteral("yyyyMMdd-HHmmss")),
                                    name));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const auto e = mw::jsonError(500, QStringLiteral("落盘失败: %1").arg(f.errorString()));
        return mw::HttpResponse::json(e.first, e.second);
    }
    if (f.write(req.body) != req.body.size()) {
        const auto e = mw::jsonError(500, QStringLiteral("写入不完整"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    f.close();

    // 复用 JSON 契约安装路由（它接管后续任务生命周期）
    QJsonObject reqJson;
    reqJson[QStringLiteral("path")] = path;
    const QString serial = req.query(QStringLiteral("serial")).trimmed();
    if (!serial.isEmpty())
        reqJson[QStringLiteral("serial")] = serial;
    const auto r = appsInstallPost(state, QJsonDocument(reqJson).toJson(QJsonDocument::Compact));
    return mw::HttpResponse::json(r.first, r.second);
}

// POST /api/apps/<pkg>/uninstall —— 卸载应用（daemon REST 本无此路由，
// 原为 CLI 专属；Web/NAS 模式补齐，与 CLI 同语义 pm uninstall -k）
mw::HttpResponse appUninstall(AppState* state, const QString& pathOnly)
{
    static const QRegularExpression kPkg(QStringLiteral("^[A-Za-z0-9_.]+$"));
    QString pkg = pathOnly.mid(QStringLiteral("/api/apps/").size());
    pkg.chop(QStringLiteral("/uninstall").size());
    if (!kPkg.match(pkg).hasMatch()) {
        const auto e = mw::jsonError(400, QStringLiteral("包名非法"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    const QString serial = resolveSerial(mw::HttpRequest{}, state);
    if (serial.isEmpty()) {
        const auto e = mw::jsonError(503, QStringLiteral("Android 未运行"));
        return mw::HttpResponse::json(e.first, e.second);
    }
    auto r = state->adb.shellBlocking(
        serial, QStringLiteral("pm uninstall -k %1").arg(pkg), 30000);
    if (!r) {
        const auto e = mw::jsonError(500, r.error());
        return mw::HttpResponse::json(e.first, e.second);
    }
    const QString outStr = r.value().outStr().trimmed();
    QJsonObject o;
    o[QStringLiteral("ok")] = outStr.contains(QLatin1String("Success"));
    o[QStringLiteral("output")] = outStr;
    o[QStringLiteral("package")] = pkg;
    return mw::HttpResponse::json(
        200, QJsonDocument(o).toJson(QJsonDocument::Compact));
}

} // namespace

QString resolveWebDir()
{
    if (const QString env = qEnvironmentVariable("MWINE_WEB_DIR"); !env.isEmpty()) {
        if (QFileInfo(QDir(env).absoluteFilePath(QStringLiteral("index.html"))).isFile())
            return QDir(env).absolutePath();
    }
    if (QCoreApplication::instance()) {
        const QString exeDir = QCoreApplication::applicationDirPath();
        const QString sibling = QDir(exeDir).absoluteFilePath(QStringLiteral("web"));
        if (QFileInfo(QDir(sibling).absoluteFilePath(QStringLiteral("index.html"))).isFile())
            return sibling;
        // .app 打包：exe = Contents/MacOS/x → Contents/Resources/web
        QDir d(exeDir);
        if (d.cdUp() && d.cdUp()) {
            const QString res = d.absoluteFilePath(QStringLiteral("Resources/web"));
            if (QFileInfo(QDir(res).absoluteFilePath(QStringLiteral("index.html"))).isFile())
                return res;
        }
    }
    const QString cwd = QDir::current().absoluteFilePath(QStringLiteral("web"));
    if (QFileInfo(QDir(cwd).absoluteFilePath(QStringLiteral("index.html"))).isFile())
        return cwd;
    return QString();
}

mw::HttpResponse webDispatch(const mw::HttpRequest& req, AppState* state)
{
    const QString pathOnly = req.pathOnly();

    // 健康检查恒开放（NAS 容器探活 / 负载均衡探测用，不泄露任何信息）
    if (pathOnly == QStringLiteral("/health"))
        return mw::HttpResponse::json(200, QByteArrayLiteral("{\"ok\":true}"));

    const bool isApi = pathOnly.startsWith(QStringLiteral("/api/"));

    if (!isApi) {
        static const QString webDir = resolveWebDir();
        if (webDir.isEmpty()) {
            const auto e = mw::jsonError(
                404, QStringLiteral("Web 前端未安装（MWINE_WEB_DIR 未指向有效目录）"));
            return mw::HttpResponse::json(e.first, e.second);
        }
        return serveStatic(webDir, pathOnly);
    }

    // ---- /api：Token 鉴权 ----
    if (!authorized(req, expectedToken()))
        return unauthorized();

    // ---- 扩展路由（二进制/上传/投屏；先于 JSON 契约，无 64KB 体上限）----
    if (pathOnly.startsWith(QStringLiteral("/api/stream"))) {
        mw::HttpResponse r;
        if (streamDispatch(req, state, pathOnly, &r))
            return r;
    }
    if (req.method == QStringLiteral("GET") && pathOnly == QStringLiteral("/api/screenshot.png"))
        return screenshotPng(state, req);
    if (req.method == QStringLiteral("GET") && pathOnly == QStringLiteral("/api/apps/icon"))
        return appIcon(state, req);
    if (req.method == QStringLiteral("GET") && pathOnly == QStringLiteral("/api/wallpaper.png"))
        return wallpaperPng(state);
    if (req.method == QStringLiteral("POST")
        && pathOnly == QStringLiteral("/api/apps/install-upload"))
        return installUpload(state, req);
    if (req.method == QStringLiteral("POST")
        && pathOnly.startsWith(QStringLiteral("/api/apps/"))
        && pathOnly.endsWith(QStringLiteral("/uninstall")))
        return appUninstall(state, pathOnly);

    // ---- 既有 JSON 契约路由（body 上限 64KB，对齐 Rust reader.take）----
    const QByteArray body = req.body.left(64 * 1024);
    const auto [code, respBody] = mw::daemon::routeWithBody(req.method, req.path, body, state);
    return mw::HttpResponse::json(code, respBody);
}

} // namespace mw::daemon
