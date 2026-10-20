// REST 路由（crates/mwine-core/src/api.rs 忠实移植）。
//
// 契约红线：JSON 字段名 / 状态码 / 错误文案与 Rust 版逐字一致
//（SwiftUI / Qt 两个 UI 依赖此契约），包括 route() 中 ("POST","/api/config")
// 传 url 当 body 的历史怪癖。分区注释与 api.rs 一一对应。
#include "routes.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpSocket>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "capture.hpp"
#include "config.hpp"
#include "fs.hpp"
#include "httpserver.hpp"
#include "json.hpp"
#include "keymap.hpp"
#include "log.hpp"
#include "net.hpp"
#include "process.hpp"
#include "appcache.hpp"
#include "state.hpp"
#include "store_api.hpp"
#include "supervisor.hpp"
#include "internal.hpp" // mw::store::detail::downloadFile（运行时在线下载：续传+重试）

namespace mw::daemon {

namespace {

using supervisor::LogFn;
using supervisor::ProgressFn;

// ===================== 基础辅助 =====================

QByteArray jbytes(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray jbytes(const QJsonArray& a)
{
    return QJsonDocument(a).toJson(QJsonDocument::Compact);
}

Resp resp(int code, const QJsonObject& o)
{
    return qMakePair(code, jbytes(o));
}

// url 的 path 部分（? 之前），对齐 Rust url.split('?').next()。
QString pathOf(const QString& url)
{
    return url.section(QLatin1Char('?'), 0, 0);
}

// %XX 解码 + '+' → 空格（对齐 api.rs percent_decode）。
QString percentDecode(const QString& s)
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

std::optional<QString> queryParam(const QString& url, const QString& key)
{
    const int qi = url.indexOf(QLatin1Char('?'));
    if (qi < 0)
        return std::nullopt;
    const QString q = url.mid(qi + 1);
    for (const auto& kv : q.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        if (kv.left(eq) == key)
            return percentDecode(kv.mid(eq + 1));
    }
    return std::nullopt;
}

QString homeDotMwine()
{
    return mw::mwineDir();
}

QString homePath()
{
    return mw::homeDir();
}

// 第一台在线（state == "device"）的 emulator serial。
std::optional<QString> onlineEmulator(AppState* state)
{
    auto ds = state->adb.devices();
    if (!ds)
        return std::nullopt;
    for (const auto& d : ds.value()) {
        if (d.state == QStringLiteral("device")
            && d.serial.startsWith(QStringLiteral("emulator-")))
            return d.serial;
    }
    return std::nullopt;
}

// 任意（含 offline）emulator serial；无则默认 emulator-5554（vm_reset 用，
// 对齐 Rust find(|d| d.serial.starts_with("emulator-")) + unwrap_or 5554）。
QString anyEmulatorOr5554(AppState* state)
{
    auto ds = state->adb.devices();
    if (ds) {
        for (const auto& d : ds.value()) {
            if (d.serial.startsWith(QStringLiteral("emulator-")))
                return d.serial;
        }
    }
    return QStringLiteral("emulator-5554");
}

// 第一台在线 emulator；无则默认 emulator-5554（vm_stop / vm_disk_set / image_use 用）。
QString onlineOr5554(AppState* state)
{
    const auto s = onlineEmulator(state);
    return s ? *s : QStringLiteral("emulator-5554");
}

bool pidAlive(qint64 pid)
{
    if (pid <= 0)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

QVector<qint64> pgrepPids(const QString& pattern)
{
    QVector<qint64> out;
    const auto r = mw::runProcess(QStringLiteral("pgrep"),
                                  { QStringLiteral("-f"), pattern }, 15000);
    for (const QString& line : QString::fromUtf8(r.stdOut).split(QLatin1Char('\n'))) {
        const QString t = line.trimmed();
        bool ok = false;
        const qint64 p = t.toLongLong(&ok);
        if (ok && p > 0)
            out.append(p);
    }
    return out;
}

std::optional<qint64> hostQemuPid()
{
    const auto pids = pgrepPids(QStringLiteral("qemu-system-aarch64"));
    if (pids.isEmpty())
        return std::nullopt;
    return pids.first();
}

QString readTextFile(const QString& path)
{
    auto r = mw::readFile(path);
    return r ? QString::fromUtf8(r.value()) : QString();
}

// 行首（跳过空白）是否以 key 开头（对齐 Rust l.trim_start().starts_with(key)）。
bool trimmedStartsWith(const QString& line, const QString& key)
{
    int i = 0;
    while (i < line.size() && line.at(i).isSpace())
        ++i;
    return line.mid(i).startsWith(key);
}

// 重写 ini 单键：替换匹配行 + 末尾补空行（对齐 Rust lines().map().chain(once("")).join("\n")）。
QString rewriteIniLine(const QString& text, const QString& key, const QString& newLine)
{
    QStringList out;
    const QStringList raw = text.split(QLatin1Char('\n'));
    for (int i = 0; i < raw.size(); ++i) {
        const QString& line = raw.at(i);
        if (i == raw.size() - 1 && line.isEmpty())
            continue; // 末尾换行切出的空段
        out.append(trimmedStartsWith(line, key) ? newLine : line);
    }
    out.append(QString());
    return out.join(QLatin1Char('\n'));
}

// canonicalize：失败原样返回（对齐 Rust path.canonicalize().unwrap_or(path)）。
QString canonicalize(const QString& p)
{
    const QString c = QFileInfo(p).canonicalFilePath();
    return c.isEmpty() ? p : c;
}

// 组件级前缀判定（对齐 Rust Path::starts_with）。
bool pathStartsWith(const QString& path, const QString& base)
{
    return path == base || path.startsWith(base + QLatin1Char('/'));
}

// 解析 mwine CLI 路径（emuhide / run --window 拉起用）。
// 打包布局下 CLI 与 mwine-cored 同级（Contents/MacOS/）；开发树里守护进程
// 跑在 <repo>/cpp/build/daemon/，CLI 在 <repo>/cpp/build/modules/cli/——
// 只查同级会静默落空、window 模式退化成主屏启动且无痕迹（2026-10-13 踩坑）。
QString resolveMwineCli()
{
    const QString dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(dir).filePath(QStringLiteral("mwine")),
        QDir(dir).filePath(QStringLiteral("../modules/cli/mwine")),
    };
    for (const QString& c : candidates) {
        if (QFileInfo(c).isFile() && QFileInfo(c).isExecutable())
            return QFileInfo(c).absoluteFilePath();
    }
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("mwine"));
    if (!onPath.isEmpty())
        return onPath;
    return QString();
}

// 双 fork + exec：孙进程脱离守护进程管理，stdio 追加到日志文件。
// extraEnv：额外注入子进程的环境变量（"K=V" 形式），在 exec 前 setenv。
bool spawnDetachedLogged(const QString& program, const QStringList& args,
                         const QString& logFile,
                         const QStringList& extraEnv = QStringList())
{
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        const pid_t inner = ::fork();
        if (inner < 0)
            ::_exit(127);
        if (inner > 0)
            ::_exit(0); // 子退出，孙进程交给 init 收养
        for (const QString& kv : extraEnv) {
            const int eq = kv.indexOf(QLatin1Char('='));
            if (eq > 0)
                ::setenv(kv.left(eq).toUtf8().constData(),
                         kv.mid(eq + 1).toUtf8().constData(), 1);
        }
        const int fd = ::open(logFile.toUtf8().constData(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            if (fd > 2)
                ::close(fd);
        } else {
            const int n = ::open("/dev/null", O_WRONLY);
            if (n >= 0) {
                ::dup2(n, 1);
                ::dup2(n, 2);
            }
        }
        QVector<QByteArray> utf8Args;
        utf8Args.append(QByteArray("mwine"));
        for (const QString& a : args)
            utf8Args.append(a.toUtf8());
        QVector<char*> argv;
        for (const QByteArray& a : utf8Args)
            argv.append(const_cast<char*>(a.constData()));
        argv.append(nullptr);
        ::execv(program.toUtf8().constData(), argv.data());
        ::_exit(127);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    return true;
}

// 隐藏/恢复模拟器主窗：经 CLI 的 `debug emuhide` 在干净子进程里执行
//（守护进程的 Qt 应用上下文会干扰 AppKit hide()，见 Rust emuhide_standalone 注释）。
// hide_emulator_window 偏好的执行点：VM 引导后隐藏、emulator 模式应用启动前恢复。
void emuWindowSetHidden(bool hide)
{
    const QString cli = resolveMwineCli();
    if (cli.isEmpty())
        return;
    const QString log = QDir(homeDotMwine()).filePath(QStringLiteral("emuhide.log"));
    (void)spawnDetachedLogged(cli,
                              { QStringLiteral("debug"), QStringLiteral("emuhide"),
                                hide ? QStringLiteral("hide") : QStringLiteral("show") },
                              log);
}

// ===================== 前置声明（跨分区互调） =====================

Resp vmStart(AppState* state);
Resp vmStop(AppState* state);
void applyCfgToAvd(const config::VmConfig& cfg);

struct ActiveAvd {
    QString dir;
    QString home;
    QString name;
};
std::optional<ActiveAvd> resolveActiveAvd();
std::optional<QString> currentAvdSysdir();
std::optional<QString> relabelAvd(const supervisor::Sdk& sdk, const QString& sysdirRel);
QString sdkRoot();

// ===================== 性能快照（perf） =====================

Resp perf(AppState* state)
{
    struct CpuSample {
        quint64 busy = 0;
        quint64 total = 0;
        QJsonObject info;
    };
    static QMutex lastMu;
    struct Last {
        bool valid = false;
        QDateTime at;
        quint64 busy = 0;
        quint64 total = 0;
    };
    static Last last; // (t, busy, total) 两次采样差分

    const auto serial = onlineEmulator(state);
    if (!serial)
        return resp(200, QJsonObject{ { QStringLiteral("running"), false } });

    const auto readCpu = [&]() -> std::optional<CpuSample> {
        auto out = state->adb.shellBlocking(
            *serial,
            QStringLiteral("head -1 /proc/stat; head -2 /proc/meminfo; cat /proc/uptime"),
            5000);
        if (!out)
            return std::nullopt;
        const QStringList lines = out.value().outStr().split(QLatin1Char('\n'));
        if (lines.isEmpty())
            return std::nullopt;
        const QString cpuLine = lines.at(0);
        QVector<quint64> fields;
        for (const QString& tok : cpuLine.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            bool ok = false;
            const quint64 v = tok.toULongLong(&ok);
            if (ok)
                fields.append(v); // "cpu" 段 parse 失败自动跳过
        }
        if (fields.size() < 4)
            return std::nullopt;
        // user nice system idle iowait irq softirq → busy = total - idle - iowait
        quint64 total = 0;
        for (quint64 f : fields)
            total += f;
        const quint64 idle = fields.at(3) + (fields.size() > 4 ? fields.at(4) : 0);
        if (lines.size() < 4)
            return std::nullopt;
        const QStringList memTotalParts = lines.at(1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (memTotalParts.size() < 2)
            return std::nullopt;
        bool okMem = false;
        const quint64 memTotal = memTotalParts.at(1).toULongLong(&okMem);
        if (!okMem)
            return std::nullopt;
        // lines[2] = MemFree（跳过）；lines[3] = uptime
        const QString uptimeFirst = lines.at(3).section(QLatin1Char(' '), 0, 0);
        bool okUp = false;
        const double uptime = uptimeFirst.toDouble(&okUp);
        if (!okUp)
            return std::nullopt;
        quint64 availMb = 0;
        auto av = state->adb.shellBlocking(
            *serial, QStringLiteral("awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo"),
            3000);
        if (av)
            availMb = av.value().outStr().trimmed().toULongLong();
        QJsonObject mem;
        mem[QStringLiteral("total_mb")] = static_cast<double>(memTotal / 1024);
        mem[QStringLiteral("available_mb")] = static_cast<double>(availMb);
        QJsonObject info;
        info[QStringLiteral("uptime_s")] = static_cast<double>(quint64(uptime));
        info[QStringLiteral("mem")] = mem;
        CpuSample s;
        s.busy = total - idle;
        s.total = total;
        s.info = info;
        return s;
    };

    const auto sample = readCpu();
    if (!sample)
        return resp(200, QJsonObject{ { QStringLiteral("running"), true },
                                       { QStringLiteral("error"), QStringLiteral("采样失败") } });
    quint64 cpuPct = 0;
    {
        QMutexLocker l(&lastMu);
        if (last.valid) {
            const quint64 db = sample->busy >= last.busy ? sample->busy - last.busy : 0;
            const quint64 dtt = sample->total >= last.total ? sample->total - last.total : 0;
            if (dtt > 0)
                cpuPct = db * 100 / dtt;
        }
        last.valid = true;
        last.at = QDateTime::currentDateTime();
        last.busy = sample->busy;
        last.total = sample->total;
    }

    // 宿主 qemu 进程 CPU%（ps 两次采样太贵，用单次 CPU% 列）
    QString hostLine;
    if (const auto pid = hostQemuPid()) {
        const auto r = mw::runProcess(
            QStringLiteral("ps"),
            { QStringLiteral("-o"), QStringLiteral("pcpu,rss,comm"), QStringLiteral("-p"),
              QString::number(*pid) },
            10000);
        const QStringList lines = QString::fromUtf8(r.stdOut).split(QLatin1Char('\n'));
        if (lines.size() > 1)
            hostLine = lines.at(1).trimmed();
    }
    double hostCpu = 0.0;
    quint64 hostMemMb = 0;
    if (!hostLine.isEmpty()) {
        const QStringList it = hostLine.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!it.isEmpty())
            hostCpu = it.at(0).toDouble();
        if (it.size() > 1)
            hostMemMb = quint64(it.at(1).toDouble() / 1024.0);
    }

    QJsonObject info = sample->info;
    info[QStringLiteral("cpu_pct")] = static_cast<double>(cpuPct);
    info[QStringLiteral("running")] = true;
    QJsonObject host;
    host[QStringLiteral("qemu_cpu_pct")] = hostCpu;
    host[QStringLiteral("qemu_mem_mb")] = static_cast<double>(hostMemMb);
    info[QStringLiteral("host")] = host;
    return resp(200, info);
}

// ===================== 键位映射（keys） =====================

Resp keysGet(const QString& pkg)
{
    const QString path = QDir(homeDotMwine()).filePath(
        QStringLiteral("keymaps/%1.json").arg(pkg));
    QJsonValue profile;
    const auto body = mw::readFile(path);
    if (body) {
        const QJsonDocument doc = QJsonDocument::fromJson(body.value());
        if (doc.isObject())
            profile = doc.object();
        else if (doc.isArray())
            profile = doc.array();
    }
    QJsonObject o;
    o[QStringLiteral("profile")] = profile;
    return resp(200, o);
}

// 内置模板清单（含完整档案，编辑器一键套用）。
Resp keysTemplates()
{
    QJsonArray templates;
    for (const QString& id : keymap::templateIds()) {
        auto p = keymap::templateProfile(id);
        if (!p)
            continue;
        const QJsonObject pj = p.value().toJson();
        QJsonObject o;
        o[QStringLiteral("id")] = id;
        o[QStringLiteral("name")] = keymap::templateName(id);
        o[QStringLiteral("builtinName")] = pj.value(QStringLiteral("name")).toString();
        o[QStringLiteral("mouseMode")] =
            pj.value(QStringLiteral("mouseMode")).toString() == QStringLiteral("capture")
                ? QStringLiteral("capture")
                : QStringLiteral("free");
        o[QStringLiteral("nodes")] =
            static_cast<double>(pj.value(QStringLiteral("nodes")).toArray().size());
        o[QStringLiteral("profile")] = pj;
        templates.append(o);
    }
    QJsonObject o;
    o[QStringLiteral("templates")] = templates;
    return resp(200, o);
}

Resp keysSet(const QString& pkg, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    if (doc.isArray()) {
        // v1 旧格式：原样保存（watch 启动时自动迁移）
    } else if (doc.isObject()) {
        const auto p = keymap::Profile::parse(doc.object(), qMakePair(1280, 720));
        if (!p)
            return mw::jsonError(400,
                                 QStringLiteral("键位档案错误: %1").arg(p.error()));
    } else {
        return mw::jsonError(400, QStringLiteral("需为 v2 档案对象或 v1 绑定数组"));
    }
    const QString dir = QDir(homeDotMwine()).filePath(QStringLiteral("keymaps"));
    QDir().mkpath(dir);
    const auto w = mw::writeFile(QDir(dir).filePath(QStringLiteral("%1.json").arg(pkg)), body);
    if (!w)
        return mw::jsonError(500, w.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp keysShot(AppState* state, const QString& pkg)
{
    const auto serial = onlineEmulator(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    auto out = state->adb.shellBlocking(*serial, QStringLiteral("screencap -p"), 10000);
    if (!out)
        return mw::jsonError(500, out.error());
    if (!(out.value().out.size() > 8 && out.value().out.startsWith(QByteArray("\x89" "PNG", 4))))
        return mw::jsonError(500, QStringLiteral("截图失败"));
    const QString dir = QDir(homeDotMwine()).filePath(QStringLiteral("keyshots"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("%1.png").arg(pkg));
    const auto w = mw::writeFile(path, out.value().out);
    if (!w)
        return mw::jsonError(500, QStringLiteral("写入失败: %1").arg(path));
    return resp(200, QJsonObject{ { QStringLiteral("path"), path } });
}

// 本机已保存的键位档案清单（方案管理列表 + 前台提醒用）。
Resp keysList()
{
    const QString dir = QDir(homeDotMwine()).filePath(QStringLiteral("keymaps"));
    QStringList pkgs;
    QJsonArray profiles;
    const QDir d(dir);
    const auto entries = d.entryInfoList({ QStringLiteral("*.json") }, QDir::Files);
    for (const QFileInfo& e : entries) {
        const QString base = e.fileName();
        const QString pkg =
            base.endsWith(QLatin1String(".json")) ? base.chopped(5) : base;
        pkgs.append(pkg);
        const quint64 updated = quint64(e.lastModified().toSecsSinceEpoch());
        QJsonObject item;
        item[QStringLiteral("package")] = pkg;
        item[QStringLiteral("updatedAt")] = static_cast<double>(updated);
        const auto text = mw::readFile(e.absoluteFilePath());
        if (text) {
            const QJsonDocument doc = QJsonDocument::fromJson(text.value());
            if (doc.isObject()) {
                const QJsonObject v = doc.object();
                if (v.contains(QStringLiteral("name"))
                    && v.value(QStringLiteral("name")).isString())
                    item[QStringLiteral("name")] = v.value(QStringLiteral("name")).toString();
                if (v.value(QStringLiteral("nodes")).isArray())
                    item[QStringLiteral("nodes")] = static_cast<double>(
                        v.value(QStringLiteral("nodes")).toArray().size());
                if (v.contains(QStringLiteral("mouseMode"))
                    && v.value(QStringLiteral("mouseMode")).isString())
                    item[QStringLiteral("mouseMode")] =
                        v.value(QStringLiteral("mouseMode")).toString();
                if (v.contains(QStringLiteral("activity"))
                    && v.value(QStringLiteral("activity")).isString())
                    item[QStringLiteral("activity")] =
                        v.value(QStringLiteral("activity")).toString();
            }
        }
        profiles.append(item);
    }
    pkgs.sort();
    // QJsonArray 迭代器是代理引用（不可 std::sort 交换），转 QVector 排序后回填
    {
        QVector<QPair<QString, QJsonObject>> items;
        for (const QJsonValue& v : profiles)
            items.append(qMakePair(
                v.toObject().value(QStringLiteral("package")).toString(), v.toObject()));
        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        profiles = QJsonArray();
        for (const auto& it : items)
            profiles.append(it.second);
    }
    QJsonObject o;
    o[QStringLiteral("packages")] = QJsonArray::fromStringList(pkgs);
    o[QStringLiteral("profiles")] = profiles;
    return resp(200, o);
}

// 删除键位档案（连带截图缓存）。
Resp keysDelete(const QString& pkg)
{
    if (pkg.isEmpty() || pkg.contains(QLatin1Char('/')) || pkg.contains(QLatin1String("..")))
        return mw::jsonError(400, QStringLiteral("非法包名"));
    const QString km = QDir(QDir(homeDotMwine()).filePath(QStringLiteral("keymaps")))
                           .filePath(QStringLiteral("%1.json").arg(pkg));
    if (!QFileInfo(km).isFile())
        return mw::jsonError(404, QStringLiteral("档案不存在"));
    const bool r1 = QFile::remove(km);
    QFile::remove(QDir(QDir(homeDotMwine()).filePath(QStringLiteral("keyshots")))
                      .filePath(QStringLiteral("%1.png").arg(pkg)));
    if (!r1)
        return mw::jsonError(500, QStringLiteral("删除失败: %1").arg(km));
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp keysShotPath(const QString& pkg)
{
    const QString path = QDir(QDir(homeDotMwine()).filePath(QStringLiteral("keyshots")))
                             .filePath(QStringLiteral("%1.png").arg(pkg));
    QJsonObject o;
    o[QStringLiteral("path")] = path;
    o[QStringLiteral("exists")] = QFileInfo(path).isFile();
    return resp(200, o);
}

// ===================== 前台应用（foreground，1s 缓存） =====================

Resp foreground(AppState* state)
{
    struct Cache {
        QDateTime at;
        QString pkg;
        QString act;
    };
    static QMutex mu;
    static Cache cache;
    {
        QMutexLocker l(&mu);
        if (cache.at.isValid() && cache.at.msecsTo(QDateTime::currentDateTime()) < 1000) {
            return resp(200, QJsonObject{ { QStringLiteral("package"), cache.pkg },
                                           { QStringLiteral("activity"), cache.act } });
        }
    }
    const auto serial = onlineEmulator(state);
    if (!serial)
        return resp(200, QJsonObject{ { QStringLiteral("package"), QJsonValue() },
                                       { QStringLiteral("activity"), QJsonValue() } });
    auto out = state->adb.shellBlocking(
        *serial,
        QStringLiteral("dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity"
                       " | grep -oE '[a-z0-9._]+/[a-zA-Z0-9._$]+'"),
        6000);
    QString pkg;
    QString act;
    if (out) {
        const QString text = out.value().outStr().trimmed();
        const QStringList parts = text.split(QLatin1Char('/'));
        if (!parts.isEmpty())
            pkg = parts.at(0);
        if (parts.size() > 1)
            act = parts.at(1);
        if (pkg.isEmpty() || act.isEmpty()) {
            pkg.clear();
            act.clear();
        }
    }
    {
        QMutexLocker l(&mu);
        cache.at = QDateTime::currentDateTime();
        cache.pkg = pkg;
        cache.act = act;
    }
    return resp(200, QJsonObject{ { QStringLiteral("package"), pkg },
                                   { QStringLiteral("activity"), act } });
}

// ===================== 宿主在场判定 / 空闲看护 =====================

// 宿主侧"用户在场"信号（任一即算）：
// 1. keys watch 运行中（映射开启，正玩游戏）
// 2. 前台 app 是 Houttuynia（正在操作管理器）
// 3. 前台 app 是 qemu 模拟器（正对着模拟器玩——最不该关机的时刻）
bool hostUserActive()
{
    const auto r = mw::runProcess(QStringLiteral("pgrep"),
                                  { QStringLiteral("-f"), QStringLiteral("keys watch") }, 10000);
    if (!r.stdOut.isEmpty())
        return true;
    // 独立应用窗口开着就算使用中：窗口会话/应用内下载进行时不能被空闲关停
    //（曾只认 qemu 主窗口/Houttuynia 前台，独立窗口用户 30 分钟必被误杀）
    const auto win = mw::runProcess(QStringLiteral("pgrep"),
                                    { QStringLiteral("-x"), QStringLiteral("mwine-window") }, 10000);
    if (!win.stdOut.isEmpty())
        return true;
    const auto osa = mw::runProcess(
        QStringLiteral("osascript"),
        { QStringLiteral("-e"),
          QStringLiteral("tell application \"System Events\" to get name of first application"
                         " process whose frontmost is true") },
        10000);
    const QString frontName = QString::fromUtf8(osa.stdOut).trimmed();
    return frontName.startsWith(QStringLiteral("qemu-system")) || frontName == QStringLiteral("Houttuynia")
           || frontName == QStringLiteral("mwine-window");
}

// 空闲看护：前台一直是 launcher（无人使用）→ 到时存档关停。
// 任一前台应用切换即重置；emulator 消失则退出线程。
void idleWatchdog(AppState* state, const QString& serial)
{
    quint64 idleSecs = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        // emulator 还在吗
        bool alive = false;
        {
            auto ds = state->adb.devices();
            if (ds) {
                for (const auto& d : ds.value()) {
                    if (d.serial == serial && d.state == QStringLiteral("device")) {
                        alive = true;
                        break;
                    }
                }
            }
        }
        if (!alive)
            return;
        // 前台应用（全部显示器）：独立窗口的应用跑在副屏上，topResumedActivity
        // 每个显示器一行——只看第一行（-m1）永远拿到主屏 launcher，副屏应用
        // 不可见 → 独立窗口使用中被判空闲。改为任一显示器前台非 launcher
        // 即视为使用中
        QString fgAll;
        {
            auto out = state->adb.shellBlocking(
                serial,
                QStringLiteral("dumpsys activity activities 2>/dev/null | grep"
                               " topResumedActivity | grep -oE '[a-z0-9._]+/[a-zA-Z0-9._$]+'"
                               " | cut -d/ -f1"),
                8000);
            if (out)
                fgAll = out.value().outStr();
        }
        bool anyRealApp = false;
        for (const QString& pkg : fgAll.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QString p = pkg.trimmed();
            if (!p.isEmpty() && !p.contains(QStringLiteral("launcher")))
                anyRealApp = true;
        }
        // 豁免：键位映射运行中（宿主 keys watch 进程）、独立窗口开着、或宿主
        // 前台是本应用/模拟器/独立窗口（用户正在操作）都不算闲置
        if (hostUserActive()) {
            idleSecs = 0;
            continue;
        }
        if (anyRealApp)
            idleSecs = 0;
        else
            idleSecs += 60;
        // 时长实时跟配置（设置页改了立即生效；0 = 从不自动关停）
        const config::VmConfig cfg = config::VmConfig::load();
        const quint64 minutes = cfg.idleStopMin;
        if (minutes > 0 && idleSecs >= minutes * 60) {
            MW_LOG("idle", QStringLiteral("连续 %1 分钟无人使用，自动存档关停").arg(minutes));
            if (!cfg.noSnapshotSave && !cfg.noSnapshot) {
                (void)supervisor::saveSnapshotBlocking(serial,
                                                       QStringLiteral("mwine_default"), 20000);
            }
            (void)supervisor::emuKillWait(&state->adb, serial, 35000);
            return;
        }
    }
}

// ===================== 设备身份重置 =====================

Resp identityReset(AppState* state)
{
    const auto serial = onlineEmulator(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    const quint64 nanos = quint64(QDateTime::currentDateTime().toMSecsSinceEpoch()) * 1000000ULL;
    quint64 x = (nanos ^ (quint64(::getpid()) << 32)) | 1;
    QString hex;
    for (int i = 0; i < 4; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        hex += QString::number(x, 16).rightJustified(16, QLatin1Char('0'));
    }
    const QString aid = hex.left(16);
    const QString adid = QStringLiteral("%1-%2-%3-%4-%5")
                             .arg(hex.mid(0, 8), hex.mid(8, 4), hex.mid(12, 4), hex.mid(16, 4),
                                  hex.mid(20, 12));
    int ok = 0;
    const std::pair<const char*, QString> kvs[] = {
        { "android_id", aid },
        { "advertising_id", adid },
    };
    for (const auto& kv : kvs) {
        auto r = state->adb.shellBlocking(
            *serial,
            QStringLiteral("settings put secure %1 %2").arg(QLatin1String(kv.first), kv.second),
            5000);
        if (r)
            ++ok;
    }
    return resp(200, QJsonObject{ { QStringLiteral("ok"), ok == 2 },
                                   { QStringLiteral("android_id"), aid } });
}

// ===================== 虚拟机重置 / 磁盘容量 =====================

// 重置虚拟机：关停 → 清 userdata 与快照 → 下次启动全新（救急/转卖前清数据）。
Resp vmReset(AppState* state)
{
    const QString serial = anyEmulatorOr5554(state);
    // 关停（尽力存档无意义但要退净）
    (void)supervisor::emuKillWait(&state->adb, serial, 35000);
    // AVD 名随镜像版本改名（relabel），不能写死；以当前生效 AVD 为准
    const auto active = resolveActiveAvd();
    if (!active)
        return mw::jsonError(500, QStringLiteral("找不到可用的 AVD，无法重置"));
    const QString name = active->name;
    const QString home = homePath();
    const QString avdHome = QStringLiteral("%1/.android/avd").arg(home);
    int wiped = 0;
    // 内嵌模式 AVD 在 Application Support（两处都清）
    const QStringList bases = {
        avdHome,
        QStringLiteral("%1/Library/Application Support/MWine/avd").arg(home),
    };
    for (const QString& base : bases) {
        const QString dir = QStringLiteral("%1/%2.avd").arg(base, name);
        const QStringList subs = { QStringLiteral("snapshots"),
                                   QStringLiteral("cache.img.qcow2"),
                                   QStringLiteral("userdata-qemu.img.qcow2"),
                                   QStringLiteral("userdata.img") };
        for (const QString& sub : subs) {
            const QString p = QDir(dir).filePath(sub);
            QFileInfo fi(p);
            if (!fi.exists() && !fi.isSymLink())
                continue;
            const bool okRemove = fi.isDir() ? QDir(p).removeRecursively() : QFile::remove(p);
            if (okRemove)
                ++wiped;
        }
    }
    {
        QMutexLocker l(&state->vmMu);
        state->qemuPid.reset();
    }
    // VM 销毁联动：该 AVD 的应用注册表与图标缓存一并销毁
    state->appCache()->wipe();
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("wiped"), static_cast<double>(wiped) },
                                   { QStringLiteral("note"),
                                     QStringLiteral("已重置，下次启动为全新系统（应用与数据已清空）") } });
}

// 当前虚拟机磁盘容量（读 AVD config.ini 的 disk.dataPartition.size，单一事实源）。
Resp vmDiskGet(AppState* state)
{
    Q_UNUSED(state);
    const auto active = resolveActiveAvd();
    if (!active)
        return mw::jsonError(500, QStringLiteral("找不到可用的 AVD"));
    const QString text = readTextFile(QDir(active->dir).filePath(QStringLiteral("config.ini")));
    quint64 bytes = 0;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!trimmedStartsWith(line, QStringLiteral("disk.dataPartition.size")))
            continue;
        // 注意顺序坑：值是 " = 17179869184"，先 trim 会把 '=' 前的空格剥掉、'=' 反而露出
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        bool okNum = false;
        const quint64 b = line.mid(eq + 1).trimmed().toULongLong(&okNum);
        if (okNum) {
            bytes = b;
            break;
        }
    }
    QJsonObject o;
    o[QStringLiteral("bytes")] = static_cast<double>(bytes);
    o[QStringLiteral("gb")] = static_cast<double>(bytes / 1073741824ULL);
    return resp(200, o);
}

// 扩容（缩容同路径，语义同为重建）：改 dataPartition.size + 清磁盘链，下次启动按新容量重建。
Resp vmDiskSet(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QJsonObject v = doc.object();
    const quint64 gb =
        v.value(QStringLiteral("size_gb")).isDouble()
            ? static_cast<quint64>(v.value(QStringLiteral("size_gb")).toDouble())
            : 0;
    if (gb < 4 || gb > 512)
        return mw::jsonError(400, QStringLiteral("容量须在 4–512 GB 之间"));
    const QJsonValue stopVmV = v.value(QStringLiteral("stop_vm"));
    const bool stopVm = stopVmV.isBool() ? stopVmV.toBool()
                                         : (stopVmV.isString()
                                                && stopVmV.toString() == QStringLiteral("true"));
    const bool vmRunning = state->vmRunning();
    if (vmRunning && !stopVm) {
        QJsonObject o;
        o[QStringLiteral("running")] = true;
        o[QStringLiteral("needs_confirm")] = true;
        o[QStringLiteral("error")] =
            QStringLiteral("扩容需要重建磁盘（清空全部应用与数据）并重启虚拟机");
        return resp(409, o);
    }
    const auto active = resolveActiveAvd();
    if (!active)
        return mw::jsonError(500, QStringLiteral("找不到可用的 AVD"));
    // 关停（不存档：磁盘即将重建，存档无意义）
    if (vmRunning) {
        const auto kill = supervisor::emuKillWait(&state->adb, onlineOr5554(state), 35000);
        if (!kill)
            return mw::jsonError(500, QStringLiteral("关停虚拟机失败: %1").arg(kill.error()));
        QMutexLocker l(&state->vmMu);
        state->qemuPid.reset();
    }
    // 写新容量（字节）
    const QString cfgPath = QDir(active->dir).filePath(QStringLiteral("config.ini"));
    const QString text = readTextFile(cfgPath);
    const QString newText = rewriteIniLine(
        text, QStringLiteral("disk.dataPartition.size"),
        QStringLiteral("disk.dataPartition.size = %1").arg(gb * 1073741824ULL));
    if (!mw::writeFile(cfgPath, newText.toUtf8()))
        return mw::jsonError(500, QStringLiteral("写入 AVD 配置失败"));
    // 清磁盘链：连 raw 一起清（重建才会按新容量走 mkfs；只清 qcow2 只是重置不扩容）
    QDir(QDir(active->dir).filePath(QStringLiteral("snapshots"))).removeRecursively();
    const QStringList files = { QStringLiteral("userdata-qemu.img"),
                                QStringLiteral("userdata.img"),
                                QStringLiteral("cache.img"),
                                QStringLiteral("hardware-qemu.ini") };
    for (const QString& f : files)
        QFile::remove(QDir(active->dir).filePath(f));
    const QDir avdDir(active->dir);
    for (const QFileInfo& e : avdDir.entryInfoList(QDir::Files))
        if (e.fileName().endsWith(QLatin1String(".qcow2")))
            QFile::remove(e.absoluteFilePath());
    // 原本在跑就自动重启（首次为冷启 mkfs，比平时慢）
    if (vmRunning)
        vmStart(state);
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("gb"), static_cast<double>(gb) },
                                   { QStringLiteral("restarted"), vmRunning },
                                   { QStringLiteral("note"),
                                     QStringLiteral("已扩容（磁盘重建，应用与数据已清空）") } });
}

// ===================== 截屏 =====================

// 截屏到 ~/Desktop（screencap 走 SurfaceFlinger，不经坏掉的编码器）。
Resp screenshotSave(AppState* state)
{
    const auto serial = onlineEmulator(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    auto out = state->adb.shellBlocking(*serial, QStringLiteral("screencap -p"), 10000);
    if (!out)
        return mw::jsonError(500, out.error());
    if (!(out.value().out.size() > 8 && out.value().out.startsWith(QByteArray("\x89" "PNG", 4))))
        return mw::jsonError(500, QStringLiteral("截图失败"));
    const quint64 ts = quint64(QDateTime::currentDateTime().toSecsSinceEpoch());
    const QString path =
        QStringLiteral("%1/Desktop/MWine-%2.png").arg(homePath()).arg(ts);
    const auto w = mw::writeFile(path, out.value().out);
    if (!w)
        return mw::jsonError(500, w.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("path"), path } });
}

// ===================== 配置读写 =====================

Resp configGet(AppState* state)
{
    Q_UNUSED(state);
    return resp(200, config::VmConfig::load().toJson());
}

// 读快照里记录的 (宽, 高, dpi)（无快照返回 nullopt）。
std::optional<config::Lcd> snapshotHwLcd(const QString& avd)
{
    const QString hw = QStringLiteral("%1/.android/avd/%2.avd/snapshots/mwine_default/"
                                      "hardware.ini")
                           .arg(homePath(), avd);
    const QString text = readTextFile(hw);
    if (text.isEmpty())
        return std::nullopt;
    const auto get = [&](const QString& k) -> std::optional<int> {
        for (const QString& line : text.split(QLatin1Char('\n'))) {
            if (!trimmedStartsWith(line, k))
                continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq < 0)
                return std::nullopt;
            bool okNum = false;
            const int v = line.mid(eq + 1).trimmed().toInt(&okNum);
            if (!okNum)
                return std::nullopt;
            return v;
        }
        return std::nullopt;
    };
    const auto w = get(QStringLiteral("hw.lcd.width"));
    const auto h = get(QStringLiteral("hw.lcd.height"));
    const auto dpi = get(QStringLiteral("hw.lcd.density"));
    if (!w || !h || !dpi)
        return std::nullopt;
    config::Lcd lcd;
    lcd.w = *w;
    lcd.h = *h;
    lcd.dpi = *dpi;
    return lcd;
}

// 配置保存（body 为 VmConfig JSON）。写 AVD 配置（下次启动生效），
// VM 运行中则同步做运行时 wm size/density 立即生效。
Resp configSet(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
        return mw::jsonError(400, QStringLiteral("配置格式错误: %1").arg(parseErr.errorString()));
    config::VmConfig cfg = config::VmConfig::fromJson(doc.object());
    const auto valid = cfg.validate();
    if (!valid)
        return mw::jsonError(400, valid.error());
    const config::VmConfig old = config::VmConfig::load();
    // 保留 daemon 侧的 avd 名：UI 加载的可能是旧值（镜像切换→relabel 后 UI 不知道新名）
    if (const auto active = resolveActiveAvd())
        cfg.avd = active->name;
    const auto saved = cfg.save();
    if (!saved)
        return mw::jsonError(500, saved.error());
    applyCfgToAvd(cfg);
    // 快照记录着保存时的屏幕配置，恢复会盖掉新尺寸 → 尺寸变化时作废快照
    //（下次启动冷启一次，之后再恢复 5s 快启）；刷新率属 display 类，同样作废。
    // 其余键（camera/feature 等）不在快照绑定集（lcd/ram/gpu/display）内，
    // 不兼容时模拟器会静默冷启自愈，不必主动清档。
    const config::Lcd lcd = cfg.lcd();
    bool lcdChanged = false;
    if (const auto snap = snapshotHwLcd(cfg.avd))
        lcdChanged = snap->w != lcd.w || snap->h != lcd.h || snap->dpi != lcd.dpi;
    if (lcdChanged || old.vsyncRate != cfg.vsyncRate)
        (void)supervisor::removeSnapshot(cfg.avd, QStringLiteral("mwine_default"));
    if (const auto dev = onlineEmulator(state))
        (void)supervisor::setLcdRuntime(&state->adb, *dev, lcd.w, lcd.h, lcd.dpi);
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

// api.rs 的 apply_cfg_to_avd（supervisor 未提供时的 daemon 侧实现）：
// 覆写已存在的键 + 末尾补缺失的键（config.ini 由脚本生成时可能缺）。
void applyCfgToAvd(const config::VmConfig& cfg)
{
    const auto active = resolveActiveAvd();
    if (!active)
        return;
    const config::Lcd lcd = cfg.lcd();
    const QString cfgPath = QDir(active->dir).filePath(QStringLiteral("config.ini"));
    const QString text = readTextFile(cfgPath);
    QSet<QString> seen;
    QString buf;
    const auto set = [&](const QString& key, const QString& val) {
        buf += QStringLiteral("%1 = %2\n").arg(key, val);
        seen.insert(key);
    };
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (line.isEmpty())
            continue;
        const QString key = line.section(QLatin1Char('='), 0, 0).trimmed();
        if (key == QStringLiteral("hw.lcd.width"))
            set(QStringLiteral("hw.lcd.width"), QString::number(lcd.w));
        else if (key == QStringLiteral("hw.lcd.height"))
            set(QStringLiteral("hw.lcd.height"), QString::number(lcd.h));
        else if (key == QStringLiteral("hw.lcd.density"))
            set(QStringLiteral("hw.lcd.density"), QString::number(lcd.dpi));
        else if (key == QStringLiteral("hw.initialOrientation"))
            set(QStringLiteral("hw.initialOrientation"),
                lcd.w > lcd.h ? QStringLiteral("landscape") : QStringLiteral("portrait"));
        // Mac 锁盘直通（no = guest 内弹软键盘，物理键盘不再直通）
        else if (key == QStringLiteral("hw.keyboard"))
            set(QStringLiteral("hw.keyboard"),
                cfg.hwKeyboard ? QStringLiteral("yes") : QStringLiteral("no"));
        else {
            buf += line;
            buf += QLatin1Char('\n');
        }
    }
    const QList<QPair<QString, QString>> tail = {
        { QStringLiteral("hw.lcd.width"), QString::number(lcd.w) },
        { QStringLiteral("hw.lcd.height"), QString::number(lcd.h) },
        { QStringLiteral("hw.lcd.density"), QString::number(lcd.dpi) },
        { QStringLiteral("hw.initialOrientation"),
          lcd.w > lcd.h ? QStringLiteral("landscape") : QStringLiteral("portrait") },
        { QStringLiteral("hw.keyboard"),
          cfg.hwKeyboard ? QStringLiteral("yes") : QStringLiteral("no") },
    };
    for (const auto& kv : tail) {
        if (!seen.contains(kv.first))
            buf += QStringLiteral("%1 = %2\n").arg(kv.first, kv.second);
    }
    (void)mw::writeFile(cfgPath, buf.toUtf8());
    QFile::remove(QDir(active->dir).filePath(QStringLiteral("hardware-qemu.ini"))); // 防旧值钉住
}

// ===================== 应用渲染模式 / 启动 =====================

// 应用渲染模式表（与 CLI `mwine apps mode` 同一份文件）。
QString appMode(const QString& package)
{
    const QString path = QDir(homeDotMwine()).filePath(QStringLiteral("appmodes.json"));
    const auto text = mw::readFile(path);
    if (!text)
        return QStringLiteral("emulator");
    const QJsonDocument doc = QJsonDocument::fromJson(text.value());
    if (!doc.isObject())
        return QStringLiteral("emulator");
    const QString m = doc.object().value(package).toString();
    return m == QStringLiteral("window") ? m : QStringLiteral("emulator");
}

Resp appSetMode(AppState* state, const QString& package, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    QString mode = QStringLiteral("emulator");
    if (parseErr.error == QJsonParseError::NoError && doc.isObject())
        mode = doc.object().value(QStringLiteral("mode")).toString(mode);
    if (mode != QStringLiteral("window") && mode != QStringLiteral("emulator"))
        return mw::jsonError(400, QStringLiteral("mode 只支持 window / emulator"));
    const QString path = QDir(homeDotMwine()).filePath(QStringLiteral("appmodes.json"));
    QJsonObject root;
    {
        const auto text = mw::readFile(path);
        if (text) {
            const QJsonDocument d = QJsonDocument::fromJson(text.value());
            if (d.isObject())
                root = d.object();
        }
    }
    if (mode == QStringLiteral("emulator")) {
        root.remove(package); // 默认模式不落盘
    } else {
        root[package] = mode;
    }
    QDir().mkpath(homeDotMwine());
    const auto w = mw::writeFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!w)
        return mw::jsonError(500, w.error());
    Q_UNUSED(state); // 预留：未来可推送事件刷新 GUI
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("package"), package },
                                   { QStringLiteral("mode"), mode } });
}

Resp appRun(AppState* state, const QString& package)
{
    // window 模式：独立 macOS 窗口（multidisplay 直通），交给 CLI 的
    // `mwine run --window`（含建屏/启动/帧泵/输入/resize 全链路）；
    // window_mode_all 总开关开启时忽略逐应用设置，全部走独立窗口
    const bool allWindow = config::VmConfig::load().windowModeAll;
    if (allWindow || appMode(package) == QStringLiteral("window")) {
        const QString cli = resolveMwineCli();
        if (!cli.isEmpty()) {
            // 日志落盘（窗口运行期的 resize/降级事件可追溯）
            const QString log = QDir(homeDotMwine())
                                    .filePath(QStringLiteral("run-window-%1.log").arg(package));
            // 偏好注入：hide_emulator_window / window_mode_all → 关窗保持主窗
            // 隐藏；window_borderless → 无边框窗口（⌘拖动移动、⌥拖动缩放）
            QStringList env;
            {
                const auto c = config::VmConfig::load();
                if (c.hideEmulatorWindow || c.windowModeAll)
                    env.append(QStringLiteral("MWINE_EMU_WINDOW=always"));
                if (c.windowBorderless)
                    env.append(QStringLiteral("MWINE_BORDERLESS=1"));
            }
            if (spawnDetachedLogged(cli,
                                    { QStringLiteral("run"), package,
                                      QStringLiteral("--window") },
                                    log, env)) {
                // 注意：不可在此 AppleScript 隐藏 qemu 进程——Metal 渲染层挂在
                // 窗口上，进程隐藏即渲染停摆（实测流只出 3 帧后冻结=黑屏）。
                // 视觉隐藏由启动参数 -qt-hide-window 承担（渲染继续）。
                return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                               { QStringLiteral("package"), package },
                                               { QStringLiteral("mode"),
                                                 QStringLiteral("window") } });
            }
            MW_LOG("core", QStringLiteral("window 模式拉起失败，回退主屏"));
        } else {
            MW_LOG("core", QStringLiteral("window 模式找不到 mwine CLI（已查同目录/target 构建树/PATH），回退主屏"));
        }
        // 找不到 CLI 或 spawn 失败 → 走默认主屏路径（下方）
    }

    QString serial = QStringLiteral("emulator-5554");
    {
        const auto dev = state->adb.findOnlineDevice();
        if (dev)
            serial = dev.value().serial;
    }

    // 解析入口 Activity 并启动
    auto resolve = state->adb.shellBlocking(
        serial,
        QStringLiteral("cmd package resolve-activity --brief %1 | tail -1").arg(package), 10000);
    QString comp;
    if (resolve) {
        const QString s = resolve.value().outStr().trimmed();
        if (s.contains(QLatin1Char('/')))
            comp = s;
    }
    if (!comp.isEmpty()) {
        // 主窗可能处于隐藏态（hide_emulator_window / window_mode_all）：
        // emulator 模式应用的显示界面就是主窗，启动前恢复（否则用户看不到应用）
        {
            const auto c = config::VmConfig::load();
            if (c.hideEmulatorWindow || c.windowModeAll)
                emuWindowSetHidden(false);
        }
        const auto r = state->adb.shellBlocking(
            serial, QStringLiteral("am start -n '%1'").arg(comp), 10000);
        if (!r)
            return mw::jsonError(500, r.error());
        return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                       { QStringLiteral("package"), package } });
    }
    return mw::jsonError(404, QStringLiteral("找不到启动入口"));
}

// ===================== 状态 / 启动 / 关停 =====================

Resp status(AppState* state)
{
    // 以 adb 在线设备为准：无论 VM 是谁拉起的（CLI / 守护进程 / 开机自启），
    // 只要有 emulator 设备在线就视为运行中。守护进程自身 spawn 的 pid 仅作兜底。
    bool running = state->vmRunning();
    if (!running) {
        QMutexLocker l(&state->vmMu);
        if (state->qemuPid.has_value())
            running = pidAlive(*state->qemuPid);
    }
    const QString err = state->vmErrorLocked();
    QString serial = state->currentSerial();
    // 自愈：daemon 重启后 VM 还在（adb 在线）但 serial 记录丢失 → 收养唯一在线者。
    // 安装目标解析（多设备优先管理器 VM）与壳应用启动都依赖 status.serial，
    // 曾出现 running=true / serial=null 的退化态（2026-10-09 排查发现）。
    if (running && serial.isEmpty()) {
        QStringList online;
        {
            auto ds = state->adb.devices();
            if (ds) {
                for (const auto& d : ds.value()) {
                    if (d.state == QStringLiteral("device")
                        && d.serial.startsWith(QStringLiteral("emulator-")))
                        online.append(d.serial);
                }
            }
        }
        if (online.size() == 1) {
            serial = online.at(0);
            state->adoptSerial(serial);
            MW_LOG("vm", QStringLiteral("status 自愈：收养在线 VM %1").arg(serial));
        }
    }
    QJsonObject vm;
    vm[QStringLiteral("running")] = running;
    vm[QStringLiteral("serial")] = serial.isEmpty() ? QJsonValue() : QJsonValue(serial);
    QJsonObject o;
    o[QStringLiteral("vm")] = vm;
    o[QStringLiteral("error")] = err;
    o[QStringLiteral("version")] = QStringLiteral("0.1.0");
    return resp(200, o);
}

// 宿主主语言 → Android BCP-47。`defaults read -g AppleLanguages` 首项形如
// zh-Hans-CN / en-US / ja；转 Android 写法：去掉文字脚本段（Hans/Hant 等 4 字母段），
// 语言-地区保留；裸语言补默认地区（zh→zh-CN，en→en-US）。失败返回 nullopt（不动语言）。
std::optional<QString> hostLocaleForAndroid()
{
    const auto read = [](const QString& key) -> std::optional<QString> {
        const auto r = mw::runProcess(QStringLiteral("defaults"),
                                      { QStringLiteral("read"), QStringLiteral("-g"), key },
                                      10000);
        if (r.exitCode != 0)
            return std::nullopt;
        return QString::fromUtf8(r.stdOut);
    };
    // 优先 AppleLanguages 首项；退化用 AppleLocale（zh_CN 下划线风格）
    std::optional<QString> raw;
    if (const auto t = read(QStringLiteral("AppleLanguages"))) {
        for (const QString& l : t->split(QLatin1Char('\n'))) {
            QString s = l.trimmed();
            if (s.endsWith(QLatin1Char(',')))
                s.chop(1);
            if (s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"'))
                && s.size() >= 2) {
                raw = s.mid(1, s.size() - 2);
                break;
            }
        }
    }
    if (!raw) {
        if (const auto t = read(QStringLiteral("AppleLocale"))) {
            QString s = t->trimmed().section(QLatin1Char('@'), 0, 0);
            raw = s.replace(QLatin1Char('_'), QLatin1Char('-'));
        }
    }
    if (!raw || raw->isEmpty())
        return std::nullopt;
    QStringList parts;
    for (const QString& p : raw->split(QLatin1Char('-'))) {
        if (p.size() == 4 && !p.isEmpty() && p.at(0).isUpper())
            continue; // 文字脚本段（Hans/Hant）
        parts.append(p);
    }
    if (parts.isEmpty())
        return std::nullopt;
    if (parts.size() == 1) {
        if (parts.at(0) == QStringLiteral("zh"))
            parts.append(QStringLiteral("CN"));
        else if (parts.at(0) == QStringLiteral("en"))
            parts.append(QStringLiteral("US"));
    }
    while (parts.size() > 2)
        parts.removeLast(); // 只留 语言-地区，忽略多余变体段
    return parts.join(QLatin1Char('-'));
}

// 轮询等 guest boot_completed（最长 3 分钟）。
void waitGuestBooted(AppState* state, const QString& serial)
{
    const QDateTime deadline =
        QDateTime::currentDateTime().addSecs(180);
    for (;;) {
        bool booted = false;
        auto out = state->adb.shellBlocking(serial,
                                            QStringLiteral("getprop sys.boot_completed"), 5000);
        if (out && out.value().outStr().trimmed() == QStringLiteral("1"))
            booted = true;
        if (booted || QDateTime::currentDateTime() > deadline)
            return;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
}

// 开机后 guest 默认值自愈（每次启动都跑，幂等）：
// 1) 语言：仅 system_locales 为 null（全新系统/清数据）时设为宿主主语言，用户改过则永不干预；
// 2) 联网验证地址：默认 gstatic 在国内不通 → AndroidWifi 被永久拉黑（清数据/快照回滚后
//    "没网"的根因）；仅当前值为空/默认(gstatic)时换成国内可达端点。
void provisionGuestDefaults(AppState* state, const QString& serial)
{
    waitGuestBooted(state, serial);
    // ---- 1) 语言：手动覆盖（设置了就强制对齐）> 仅全新系统跟随宿主 ----
    bool needReboot = false;
    {
        const QString manualLocale = config::VmConfig::load().locale.trimmed();
        auto out = state->adb.shellBlocking(serial,
                                            QStringLiteral("settings get system system_locales"),
                                            8000);
        if (out) {
            const QString cur = out.value().outStr().trimmed();
            std::optional<QString> desired;
            if (!manualLocale.isEmpty()) {
                desired = manualLocale;
            } else if (cur.isEmpty() || cur == QStringLiteral("null")) {
                desired = hostLocaleForAndroid();
            }
            // 用户在 guest 里自己改过且未设手动覆盖：永不干预
            if (desired) {
                const bool fresh = cur.isEmpty() || cur == QStringLiteral("null");
                if (fresh || cur != *desired) {
                    MW_LOG("vm", QStringLiteral("设置系统语言为 %1（%2）")
                                       .arg(*desired, fresh ? QStringLiteral("全新系统")
                                                            : QStringLiteral("手动覆盖")));
                    (void)state->adb.shellBlocking(
                        serial,
                        QStringLiteral("settings put system system_locales %1").arg(*desired),
                        8000);
                    needReboot = true; // 该 settings 不热切换，guest 内重启生效
                }
            }
        }
    }
    // ---- 2) 联网验证地址（仅默认/空时；不覆盖用户自定义）----
    {
        auto out = state->adb.shellBlocking(
            serial, QStringLiteral("settings get global captive_portal_http_url"), 8000);
        if (out) {
            const QString cur = out.value().outStr().trimmed();
            if (cur.isEmpty() || cur == QStringLiteral("null")
                || cur.contains(QStringLiteral("gstatic"))) {
                MW_LOG("vm", QStringLiteral("联网验证地址设为国内可达端点"));
                const QList<QPair<QString, QString>> kvs = {
                    { QStringLiteral("captive_portal_http_url"),
                      QStringLiteral("http://connect.rom.miui.com/generate_204") },
                    { QStringLiteral("captive_portal_https_url"),
                      QStringLiteral("https://connect.rom.miui.com/generate_204") },
                    { QStringLiteral("captive_portal_fallback_url"),
                      QStringLiteral("http://connect.rom.miui.com/generate_204") },
                    { QStringLiteral("captive_portal_other_fallback_url"),
                      QStringLiteral("http://wifi.vivo.com.cn/generate_204") },
                };
                for (const auto& kv : kvs) {
                    (void)state->adb.shellBlocking(
                        serial,
                        QStringLiteral("settings put global %1 %2").arg(kv.first, kv.second),
                        8000);
                }
            }
        }
    }
    // ---- 3) 语言变更的重启（qemu 进程与磁盘不动，~15-20s）----
    if (needReboot) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        (void)state->adb.shellBlocking(serial, QStringLiteral("reboot"), 8000);
        waitGuestBooted(state, serial);
    }
}

Resp vmStartWindowed(AppState* state, bool forceWindowed);

Resp vmStart(AppState* state)
{
    return vmStartWindowed(state, false);
}

// forceWindowed：跳过 hide_emulator_window / window_mode_all 的隐藏配置，
// 以可见窗口启动（「显示」按钮的临时会话；不改用户配置，下次普通启动恢复）
Resp vmStartWindowed(AppState* state, bool forceWindowed)
{
    // 唯一权威判据 = adb 在线设备（pid 判定会被僵尸进程欺骗：forget 不收尸 → kill(pid,0) 恒真
    // → 永远 already 拒启，表现为「点启动没反应」）
    {
        QStringList online;
        auto ds = state->adb.devices();
        if (ds) {
            for (const auto& d : ds.value()) {
                if (d.state == QStringLiteral("device")
                    && d.serial.startsWith(QStringLiteral("emulator-")))
                    online.append(d.serial);
            }
        }
        if (!online.isEmpty()) {
            // direct 模式 devices() 只做 TCP 探活，半死实例（adbd 活、guest 已僵）会
            // 假阳性 → already 假成功 + 后续一切操作无响应。启动判定前做一次
            // shell 级探测（vmRunning 同判据），探测失败按无在线处理走清场重启。
            QStringList healthy;
            for (const QString& s : online) {
                if (const auto probe =
                        state->adb.shellBlocking(s, QStringLiteral("echo mw"), 4000);
                    probe.ok() && probe.value().outStr().trimmed() == QStringLiteral("mw")) {
                    healthy.append(s);
                } else {
                    MW_LOG("vm", QStringLiteral("设备 %1 shell 探测失败，按僵尸处理").arg(s));
                }
            }
            if (!healthy.isEmpty()) {
                // 收养：daemon 重启后遇到"本产品唯一在线 VM"时认领之（多台无法甄别归属，留空
                // 由安装侧按"多设备"规则处理），保证 /api/status.serial 尽早可用
                if (healthy.size() == 1)
                    state->adoptSerial(healthy.at(0));
                return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                               { QStringLiteral("already"), true } });
            }
        }
    }

    // AVD 名跟随镜像版本（=模拟器窗口标题）；放在 already 检查后，只在即将启动时做
    if (const auto sdk = supervisor::Sdk::detect()) {
        if (const auto sysdir = currentAvdSysdir())
            relabelAvd(sdk.value(), *sysdir);
    }
    // 走 supervisor 的统一启动路径：按用户配置（尺寸/内存/sdcard）启动 windowed VM，
    // emulator 自己的窗口就是显示界面；快照自动恢复（5s 级，配置变更后首次为冷启）
    const config::VmConfig cfg = config::VmConfig::load();
    state->setVmError(QString());
    std::thread([state, cfg, forceWindowed]() {
        // 僵尸清场：qemu 进程还在但 adb 无 device（上次启动失败/关停未净）→
        // 端口被占，新实例会静默失败（用户点启动"没反应"的根因）
        if (!state->vmRunning()) {
            const auto pids = pgrepPids(QStringLiteral("qemu-system-aarch64"));
            if (!pids.isEmpty()) {
                MW_LOG("vm", QStringLiteral("发现僵尸 qemu，清理后重启"));
                (void)mw::runProcess(QStringLiteral("pkill"),
                                     { QStringLiteral("-9"), QStringLiteral("-f"),
                                       QStringLiteral("qemu-system-aarch64") },
                                     15000);
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }

        const auto sdk = supervisor::Sdk::detect();
        if (!sdk) {
            MW_LOG("vm", QStringLiteral("SDK 探测失败: %1").arg(sdk.error()));
            state->setVmError(QStringLiteral("SDK 探测失败: %1").arg(sdk.error()));
            return;
        }
        const supervisor::Sdk sdkV = sdk.value();
        // 镜像预检：分发包未导入时给出明确引导（而不是 emulator 静默失败）
        if (const auto sysdir = currentAvdSysdir()) {
            const QString imgDir = QDir(sdkRoot()).filePath(*sysdir);
            struct stat st;
            const bool systemImgEntry = ::lstat(QFile::encodeName(
                                                    QDir(imgDir).filePath(QStringLiteral("system.img")))
                                                    .constData(),
                                                &st) == 0;
            const bool systemQemu = QFileInfo(
                QDir(imgDir).filePath(QStringLiteral("system-qemu.img"))).isFile();
            if (!systemImgEntry && !systemQemu) {
                MW_LOG("vm", QStringLiteral("镜像缺失: %1").arg(imgDir));
                state->setVmError(QStringLiteral(
                    "尚未安装系统镜像：请到「镜像」页导入 houttuynia-image-*.tar.zst 分发包"
                    "（或拉取官方镜像）"));
                return;
            }
        }
        // 9p 宿主共享目录（qemu -virtfs；emulator 参数须在 -qemu 透传块前）
        QStringList extra;
        if (!cfg.shareDir.isEmpty()) {
            extra.append(QStringLiteral("-virtfs"));
            extra.append(QStringLiteral(
                "local,path=%1,mount_tag=hostshare,security_model=none,id=hostshare")
                .arg(cfg.shareDir));
        }
        // AVD 实体可能在另一台主目录（历史分裂）：按实际位置覆盖 child 的 AVD home
        std::optional<QString> avdHomeOverride;
        if (const auto active = resolveActiveAvd()) {
            if (canonicalize(active->home) != canonicalize(sdkV.avdHome()))
                avdHomeOverride = active->home;
        }
        // 专业参数：QEMU 附加参数 + GPU 后端 + 网络模拟 + 启动行为
        for (const QString& a : cfg.qemuExtra.split(QChar(u' '), Qt::SkipEmptyParts))
            extra.append(a);
        if (!cfg.gpuMode.isEmpty()) {
            extra.append(QStringLiteral("-gpu"));
            extra.append(cfg.gpuMode);
        }
        if (!cfg.netspeed.isEmpty()) {
            extra.append(QStringLiteral("-netspeed"));
            extra.append(cfg.netspeed);
        }
        if (!cfg.netdelay.isEmpty()) {
            extra.append(QStringLiteral("-netdelay"));
            extra.append(cfg.netdelay);
        }
        // ===== emulator 源码调查换来的新能力（全部 -qemu 透传块之前的 emulator 参数）=====
        // 摄像头：none / emulated / virtualscene / webcam0（Mac 摄像头）
        if (!cfg.cameraBack.isEmpty()) {
            extra.append(QStringLiteral("-camera-back"));
            extra.append(cfg.cameraBack);
        }
        if (!cfg.cameraFront.isEmpty()) {
            extra.append(QStringLiteral("-camera-front"));
            extra.append(cfg.cameraFront);
        }
        // 虚拟场景需要 gRPC 摆相机位姿：默认 8554 端口要 JWT，
        // -grpc 开本地免认证端点（仅 127.0.0.1，扫码画面/贴图预览用）
        if (cfg.cameraBack == QStringLiteral("virtualscene")
            || cfg.cameraFront == QStringLiteral("virtualscene")) {
            extra.append(QStringLiteral("-grpc"));
            extra.append(QStringLiteral("8550"));
        }
        if (cfg.allowHostAudio) {
            extra.append(QStringLiteral("-allow-host-audio")); // 麦克风直通（默认输入为静音填充）
        }
        if (!cfg.dnsServers.isEmpty()) {
            extra.append(QStringLiteral("-dns-server"));
            extra.append(cfg.dnsServers);
        }
        if (cfg.vsyncRate > 0) {
            extra.append(QStringLiteral("-vsync-rate"));
            extra.append(QString::number(cfg.vsyncRate));
        }
        if (!cfg.phoneNumber.isEmpty()) {
            extra.append(QStringLiteral("-phone-number"));
            extra.append(cfg.phoneNumber);
        }
        if (cfg.noSim)
            extra.append(QStringLiteral("-no-sim"));
        if (!cfg.serialno.isEmpty()) {
            extra.append(QStringLiteral("-android-serialno"));
            extra.append(cfg.serialno);
        }
        if (cfg.noSnapshotSave)
            extra.append(QStringLiteral("-no-snapshot-save"));
        // -feature -Name：强制关闭 advancedFeatures 特性（优先级高于 ini，免疫镜像切换）
        const QList<QPair<bool, const char*>> featureOffs = {
            { cfg.hwDecoderOff, "HardwareDecoder" }, // 媒体死锁自救：关掉 → 纯软解
            { cfg.vulkanOff, "Vulkan" },
            { cfg.gldmaOff, "GLDMA" },
            { cfg.virtioInputOff, "VirtioInput" }, // 输入回退 goldfish_events
            { cfg.mouseAsTouch, "VirtioMouse" },   // 鼠标事件 → 触摸事件
        };
        for (const auto& fo : featureOffs) {
            if (fo.first) {
                extra.append(QStringLiteral("-feature"));
                extra.append(QStringLiteral("-%1").arg(QLatin1String(fo.second)));
            }
        }
        if (!cfg.selinux.isEmpty()) {
            extra.append(QStringLiteral("-selinux"));
            extra.append(cfg.selinux);
        }
        if (cfg.tcpdump) {
            const QString pcap = QDir(QDir(homeDotMwine()).filePath(QStringLiteral("logs")))
                                     .filePath(QStringLiteral("capture.pcap"));
            QDir().mkpath(QDir(homeDotMwine()).filePath(QStringLiteral("logs")));
            extra.append(QStringLiteral("-tcpdump"));
            extra.append(pcap);
        }
        if (cfg.detectImageHang)
            extra.append(QStringLiteral("-detect-image-hang"));
        if (cfg.writableSystem)
            extra.append(QStringLiteral("-writable-system"));
        // CPU 核数写入 AVD；同时清除 console `multidisplay add` 持久化进
        // config.ini 的 hw.display1/2/3.*（窗口崩溃残留会让每次启动多出副屏
        // 窗口 + SurfaceFlinger 多屏渲染开销）
        if (const auto active = resolveActiveAvd()) {
            const QString cfgP = QDir(active->dir).filePath(QStringLiteral("config.ini"));
            const QString txt = readTextFile(cfgP);
            QString newTxt = txt;
            if (cfg.cpuCores > 0)
                newTxt = rewriteIniLine(newTxt, QStringLiteral("hw.cpu.ncore"),
                                        QStringLiteral("hw.cpu.ncore = %1").arg(cfg.cpuCores));
            static const QRegularExpression hwDisplay(
                QStringLiteral("^hw\\.display[123]\\..*\\n?"), QRegularExpression::MultilineOption);
            if (newTxt.contains(hwDisplay))
                newTxt.remove(hwDisplay);
            if (newTxt != txt)
                (void)mw::writeFile(cfgP, newTxt.toUtf8());
        }
        supervisor::LaunchOptions opts;
        // 始终 windowed：qemu 无头模式会把 multidisplay 副屏的帧按主屏尺寸
        // 渲染（36.5.11 实测：786x1704 的屏给出 2560x1600 横向帧，直通窗口
        // 被信箱化），windowed 下帧尺寸才正确。window_mode_all 要的"无头"
        // 视觉用官方 -qt-hide-window（启动即有窗但隐藏）实现，帧不受影响
        opts.windowed = true;
        opts.hideWindow = !forceWindowed && (cfg.windowModeAll || cfg.hideEmulatorWindow);
        opts.avdHome = avdHomeOverride;
        opts.noSnapshot = cfg.noSnapshot;
        opts.noBootAnim = cfg.noBootAnim;
        if (!cfg.proxy.isEmpty())
            opts.proxy = cfg.proxy;
        if (cfg.memoryMb > 0)
            opts.memoryMb = cfg.memoryMb;
        if (!cfg.sdcardPath.isEmpty())
            opts.sdcard = cfg.sdcardPath;
        opts.extraArgs = extra;
        opts.wifiSsid = supervisor::ensureWifiSsid();

        auto handle = supervisor::EmulatorHandle::launchHeadless(sdkV, cfg.avd, opts,
                                                                 &state->adb);
        if (handle) {
            const qint64 pid = handle.value().childId();
            const QString serial = handle.value().serial();
            MW_LOG("vm", QStringLiteral("emulator pid=%1 serial=%2").arg(pid).arg(serial));
            {
                QMutexLocker l(&state->vmMu);
                state->qemuPid = static_cast<quint32>(pid);
                state->vmSerial = serial;
            }
            // TP1A 已知：管道 adb 可能失联（offline 卡死）——
            // 就绪后确保备用 TCP 通道（console redir 5055）
            const auto chan = supervisor::ensureControlChannel(&state->adb, serial, nullptr);
            if (!chan)
                MW_LOG("vm", QStringLiteral("备用通道建立失败: %1").arg(chan.error()));
            // guest 内代理（10.0.2.2=宿主；settings 即时生效）
            if (!cfg.proxy.isEmpty()) {
                const QString hostPort = cfg.proxy.section(QLatin1Char(':'), -1);
                if (!hostPort.isEmpty()) {
                    (void)state->adb.shellBlocking(
                        serial,
                        QStringLiteral("settings put global http_proxy 10.0.2.2:%1")
                            .arg(hostPort),
                        8000);
                }
            }
            // 快照恢复即时健康验证：TP1A 半死档恢复后 adbd 挂（serial 上线≠可用，
            // direct 探活被骗）。30s 内 shell 不通 → 坏档，就地 L2 自愈：
            // 杀进程（不存档——存档只会把半死固化）+ 删 mwine_default + 冷启重建。
            // 孤儿看护的两级自愈（main.cpp）作为运行期兜底。
            std::thread([state, serial]() {
                bool healthy = false;
                for (int i = 0; i < 6 && !healthy; ++i) {
                    const auto p =
                        state->adb.shellBlocking(serial, QStringLiteral("echo mw"), 5000);
                    healthy = p.ok() && p.value().outStr().trimmed() == QStringLiteral("mw");
                    if (!healthy)
                        QThread::msleep(5000);
                }
                if (healthy)
                    return;
                MW_LOG("vm", QStringLiteral("快照恢复后 shell 无响应（坏档），丢弃档案冷启重建"));
                QString avdName;
                {
                    const auto r = supervisor::emuConsoleCmd(serial, QStringLiteral("avd name"));
                    if (r.ok()) {
                        for (const QString& l :
                             QString::fromUtf8(r.value()).split(QLatin1Char('\n'))) {
                            const QString cand = l.trimmed();
                            if (!cand.isEmpty() && cand != QLatin1String("OK")
                                && !cand.contains(QLatin1Char(':'))) {
                                avdName = cand;
                                break;
                            }
                        }
                    }
                }
                (void)supervisor::emuConsoleKill(serial);
                for (int i = 0; i < 30; ++i) {
                    QThread::msleep(1000);
                    const auto r = mw::runProcess(
                        QStringLiteral("pgrep"),
                        { QStringLiteral("-f"), QStringLiteral("qemu-system-aarch64") }, 10000);
                    if (QString::fromUtf8(r.stdOut).trimmed().isEmpty())
                        break;
                }
                if (!avdName.isEmpty()) {
                    for (const QString& home :
                         { QDir(mw::homeDir()).filePath(QStringLiteral(".android/avd")),
                           QDir(mw::homeDir()).filePath(
                               QStringLiteral("Library/Application Support/MWine/avd")) }) {
                        const QDir snap(
                            QDir(QDir(home).filePath(
                                     avdName + QStringLiteral(".avd/snapshots")))
                                .filePath(QStringLiteral("mwine_default")));
                        if (snap.exists()) {
                            QDir(snap).removeRecursively();
                            MW_LOG("vm", QStringLiteral("已丢弃坏档 %1/mwine_default").arg(avdName));
                        }
                    }
                }
                (void)mw::httpRequest(
                    QStringLiteral("POST"),
                    QUrl(QStringLiteral("http://127.0.0.1:%1/api/vm/start")
                             .arg(qEnvironmentVariableIntValue("MWINE_PORT") > 0
                                      ? qEnvironmentVariableIntValue("MWINE_PORT")
                                      : 7788)),
                    QByteArray{}, {}, 10000);
            }).detach();
            // 不 forget：后台线程收尸观察（防僵尸判定恒真），emulator 仍独立运行
            std::thread([state, h = std::move(handle.value())]() mutable {
                const qint64 p = h.childId();
                while (pidAlive(p))
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                MW_LOG("vm", QStringLiteral("emulator 退出"));
                QMutexLocker l(&state->vmMu);
                state->qemuPid.reset();
                state->vmSerial.clear();
            }).detach();
            // 默认中文 provisioning（全新系统才动；用户改过语言则永不干预）
            std::thread([state, serial]() { provisionGuestDefaults(state, serial); }).detach();
            // 注意：不在此隐藏 qemu 进程——Metal 渲染依赖窗口存活（见 appRun 注释）；
            // hide 偏好由启动参数 -qt-hide-window（opts.hideWindow）承担。
            // 空闲看护：无前台应用持续 idle_stop_min → 存档自动关停
            std::thread([state, serial]() { idleWatchdog(state, serial); }).detach();
        } else {
            MW_LOG("vm", QStringLiteral("启动失败: %1").arg(handle.error()));
            state->setVmError(QStringLiteral("启动失败: %1").arg(handle.error()));
        }
    }).detach();

    return resp(202, QJsonObject{ { QStringLiteral("ok"), true },
                                  { QStringLiteral("message"), QStringLiteral("VM 启动中") } });
}

Resp vmStop(AppState* state)
{
    // 先存档再关停，并等 emulator 完全退出（优雅关停 ~20s，
    // 不等完就重启会因端口冲突把新实例一起带死）
    const QString serial = onlineOr5554(state);
    // 关机不存档（no_snapshot_save）：直接关停，保留上一次存档；
    // no_snapshot（排障冷启模式）同样不产生新档
    const config::VmConfig cfg = config::VmConfig::load();
    if (!cfg.noSnapshotSave && !cfg.noSnapshot) {
        (void)supervisor::saveSnapshotBlocking(serial, QStringLiteral("mwine_default"), 20000);
    }
    const auto kill = supervisor::emuKillWait(&state->adb, serial, 35000);
    if (!kill)
        return mw::jsonError(500, kill.error());
    {
        QMutexLocker l(&state->vmMu);
        state->qemuPid.reset();
        state->vmSerial.clear();
    }
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

// 显示 Android 主屏（概览页「显示」按钮）：起主屏镜像窗口（mwine mirror）。
// 说明：window_mode_all 下 qemu 以 -qt-hide-window 启动（LS 层即 UIElement
// 型进程），unhide/activate 都唤不出窗口；改用 App 级隐藏又会让 Metal
// 渲染停摆（副屏流冻结，NOTES 2026-10-16 实测）——镜像窗口与独立应用
// 窗口同管线（可拖/可关/跨平台），是"看主屏（系统设置/通知栏）"的可靠路径。
Resp vmShowWindow(AppState* state)
{
    // 显示 emulator 原生窗口。两条路径：
    // 1) qemu 本就是可见窗口形态（未启用隐藏偏好）：unhide + activate
    //    （经 CLI debug emuhide show）即刻唤出。
    // 2) 隐藏偏好生效（-qt-hide-window 启动，LS 层 UIElement 进程，
    //    unhide/activate 唤不出窗口）：存档关停 → 以可见窗口模式重启
    //    （快照恢复 5-8s，独立窗口会随会话中断自动关闭，属预期）。
    //    异步执行（vmStop 同步耗时 10-30s），前端 toast 提示切换中。
    static qint64 lastReqMs = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastReqMs < 5000)
        return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
    lastReqMs = now;

    // 以 qemu 进程实际形态为准（-qt-hide-window）：配置判定会漏掉本地兜底
    // 拉起的隐藏实例（cmdMirror 兜底曾带 hide 参数，配置却没开）
    // 三态：无 qemu（关停）/ 隐藏形态（-qt-hide-window）/ 可见窗口形态。
    // 关停态也走切换路径——vmStop 幂等 + vmStartWindowed 拉起带窗 VM
    //（「显示」的完整语义：确保有一个可见的 Android 窗口）
    const bool noVm =
        mw::runProcess(QStringLiteral("pgrep"),
                       { QStringLiteral("-f"), QStringLiteral("qemu-system-aarch64") }, 5000)
            .exitCode != 0;
    auto qemuHasHideFlag = [] {
        return mw::runProcess(QStringLiteral("pgrep"),
                              { QStringLiteral("-f"),
                                QStringLiteral("qemu-system-aarch64.*-qt-hide-window") },
                              5000)
                .exitCode == 0;
    };
    const bool hiddenMode = noVm || qemuHasHideFlag();
    if (!hiddenMode) {
        // 不做 vmRunning 预检（shell 探活在重启后窗口期会误 409）；
        // activate 对不存在的进程无害
        emuWindowSetHidden(false);
        return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                       { QStringLiteral("activated"), true } });
    }
    // 不做 vmRunning 预检：判定含 shell 探活，adbd 抖动/重启窗口期会误报
    // "虚拟机未运行"（用户实测反馈）。vmStop 对已停 VM 幂等（快照失败忽略、
    // kill 空进程无害），vmStartWindowed 自会兜底拉起——去检后全路径自愈。
    // 切换互斥：切换全程 10-40s，期间重复请求会叠出第二个 stop/start 线程
    //（实测两轮叠加拉出 4 台 qemu 互锁）——进行中直接吞掉
    static std::atomic_bool sSwitching{ false };
    bool expected = false;
    if (!sSwitching.compare_exchange_strong(expected, true))
        return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                       { QStringLiteral("switching"), true } });
    std::thread([state]() {
        MW_LOG("vm", QStringLiteral("「显示」：存档重启为窗口模式"));
        vmStop(state);
        vmStartWindowed(state, true);
        sSwitching.store(false);
    }).detach();
    return resp(202, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("switching"), true } });
}

// ===================== 运行时模拟（console 即时生效）与快照存档点 =====================

// 当前在线的 emulator serial（console 操作的前提）。
std::optional<QString> runningSerial(AppState* state)
{
    return onlineEmulator(state);
}

// GPS 位置模拟：console `geo fix <lon> <lat> [alt]`，运行中即时生效。
Resp runtimeGeo(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QJsonObject v = doc.object();
    if (!v.value(QStringLiteral("lon")).isDouble())
        return mw::jsonError(400, QStringLiteral("缺少 lon（经度）"));
    if (!v.value(QStringLiteral("lat")).isDouble())
        return mw::jsonError(400, QStringLiteral("缺少 lat（纬度）"));
    const double lon = v.value(QStringLiteral("lon")).toDouble();
    const double lat = v.value(QStringLiteral("lat")).toDouble();
    const double alt =
        v.value(QStringLiteral("alt")).isDouble() ? v.value(QStringLiteral("alt")).toDouble() : 0.0;
    if (lon < -180.0 || lon > 180.0 || lat < -90.0 || lat > 90.0)
        return mw::jsonError(400, QStringLiteral("经纬度超出范围"));
    const auto serial = runningSerial(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    const QString cmd = QStringLiteral("geo fix %1 %2 %3")
                            .arg(lon, 0, 'f', 6)
                            .arg(lat, 0, 'f', 6)
                            .arg(alt, 0, 'f', 1);
    const auto r = supervisor::emuConsoleCmd(*serial, cmd);
    if (r) {
        if (!r.value().contains("KO"))
            return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                           { QStringLiteral("cmd"), cmd } });
        return mw::jsonError(500,
                             QStringLiteral("模拟器拒绝: %1")
                                 .arg(QString::fromUtf8(r.value()).trimmed()));
    }
    return mw::jsonError(500, r.error());
}

// 电池模拟：console `power capacity/status/ac`，运行中即时生效。
// 三项可全给也可只给其一（payload 里出现的才下发）。
Resp runtimeBattery(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QJsonObject v = doc.object();
    QStringList cmds;
    if (v.value(QStringLiteral("capacity")).isDouble()) {
        const qint64 cap = qint64(v.value(QStringLiteral("capacity")).toDouble());
        if (cap < 1 || cap > 100)
            return mw::jsonError(400, QStringLiteral("电量须在 1–100"));
        cmds.append(QStringLiteral("power capacity %1").arg(cap));
    }
    if (v.value(QStringLiteral("status")).isString()) {
        const QStringList ok = { QStringLiteral("charging"), QStringLiteral("discharging"),
                                 QStringLiteral("not-charging"), QStringLiteral("full"),
                                 QStringLiteral("unknown") };
        const QString status = v.value(QStringLiteral("status")).toString();
        if (!ok.contains(status))
            return mw::jsonError(400,
                                 QStringLiteral("状态须为 %1").arg(ok.join(QLatin1Char('/'))));
        cmds.append(QStringLiteral("power status %1").arg(status));
    }
    if (v.value(QStringLiteral("ac")).isString()) {
        const QString ac = v.value(QStringLiteral("ac")).toString();
        if (ac != QStringLiteral("on") && ac != QStringLiteral("off"))
            return mw::jsonError(400, QStringLiteral("ac 须为 on/off"));
        cmds.append(QStringLiteral("power ac %1").arg(ac));
    }
    if (cmds.isEmpty())
        return mw::jsonError(400, QStringLiteral("未指定任何电池参数"));
    const auto serial = runningSerial(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    for (const QString& cmd : cmds) {
        const auto r = supervisor::emuConsoleCmd(*serial, cmd);
        if (r) {
            if (!r.value().contains("KO"))
                continue;
            return mw::jsonError(
                500, QStringLiteral("%1 被拒: %2")
                         .arg(cmd, QString::fromUtf8(r.value()).trimmed()));
        }
        return mw::jsonError(500, r.error());
    }
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("applied"), QJsonArray::fromStringList(cmds) } });
}

// 虚拟场景海报已知缺陷警示（2026-10-10 全矩阵实测结论）：
// 贴图命令与格式均无问题（console .jpg 路径 / gRPC PNG 字节都到达模拟器），
// 但随包 emulator 36.5.11 的海报渲染器（mesh3dscene）在自建 AOSP33 镜像上
// 无法构造（emu 日志 "renderView: invalid scene renderer in mode mesh3dscene"），
// 相机显示的是无海报能力的 2D 回退房间 —— 照片永远不会出现。
// GPU 模式（lavapipe/swiftshader/angle 宿主GL）、贴图时机、相机数据清理均已对照排除。
// 换用修复版模拟器后移除本警示。
QString posterRendererWarning()
{
    return QStringLiteral(
        "图片已发送到模拟器，但当前随包模拟器(36.5.11)的海报渲染器(mesh3dscene)"
        "在此镜像上不可用（模拟器侧缺陷，与图片格式无关），相机画面暂时不会显示贴图。"
        "其余相机功能不受影响。");
}

// 摄像头画面贴图：Rust 版主通道为 gRPC VirtualSceneService.setPoster（字节直传 +
// 扫码友好尺寸），失败回退 console `virtualscene-image`。gRPC 通道（mwine-grpc）
// 尚未移植 —— 当前恒走 console 回退路径，接口形态保持一致。
Resp runtimeCameraImage(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QJsonObject v = doc.object();
    QString target = QStringLiteral("wall");
    if (v.value(QStringLiteral("target")).isString())
        target = v.value(QStringLiteral("target")).toString();
    if (target != QStringLiteral("wall") && target != QStringLiteral("table"))
        return mw::jsonError(400, QStringLiteral("target 须为 wall/table"));
    const QString path = v.value(QStringLiteral("path")).toString().trimmed();
    const config::VmConfig cfg = config::VmConfig::load();
    if (cfg.cameraBack != QStringLiteral("virtualscene")) {
        QJsonObject o;
        o[QStringLiteral("error")] =
            QStringLiteral("需先把「后置摄像头」设为「虚拟场景」（设置页）并重启虚拟机");
        o[QStringLiteral("needs_virtualscene")] = true;
        return resp(409, o);
    }
    if (!path.isEmpty()) {
        const QString ext = QFileInfo(path).suffix().toLower();
        const bool okExt = ext == QStringLiteral("png") || ext == QStringLiteral("jpg")
                           || ext == QStringLiteral("jpeg");
        if (!okExt || !QFileInfo(path).isFile())
            return mw::jsonError(400, QStringLiteral("图片文件不存在或非 png/jpg"));
    }
    const auto serial = runningSerial(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    // 主通道：gRPC setPoster —— TODO(集成): mwine-grpc（EmuClient 8550）移植后接入；
    // 当前直接进入 console 兜底路径（模拟器直读宿主路径；路径带空格实测可解析）
    const QString grpcErr = QStringLiteral("gRPC 通道未接入（mwine-grpc 尚未移植）");
    {
        const QString cmd = path.isEmpty()
                                ? QStringLiteral("virtualscene-image %1").arg(target)
                                : QStringLiteral("virtualscene-image %1 %2").arg(target, path);
        const auto console = supervisor::emuConsoleCmd(*serial, cmd);
        bool consoleOk = console && !console.value().contains("KO");
        if (!consoleOk) {
            const QString msg = console ? QStringLiteral("模拟器拒绝: %1")
                                               .arg(QString::fromUtf8(console.value()).trimmed())
                                       : console.error();
            return mw::jsonError(
                500, QStringLiteral("gRPC（%1）与 console（%2）均失败").arg(grpcErr, msg));
        }
    }
    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    o[QStringLiteral("cleared")] = path.isEmpty();
    o[QStringLiteral("warning")] =
        path.isEmpty() ? QString() : posterRendererWarning();
    o[QStringLiteral("hint")] =
        path.isEmpty() ? QString() : QStringLiteral("打开相机或扫码界面即可看到图片");
    return resp(200, o);
}

// 快照存档点列表（读 AVD snapshots 目录，VM 未运行也可查看）。
Resp snapshotsList(AppState* state)
{
    Q_UNUSED(state);
    const auto active = resolveActiveAvd();
    if (!active)
        return mw::jsonError(500, QStringLiteral("找不到可用的 AVD"));
    const auto snaps = supervisor::avdSnapshotNames(active->dir);
    QJsonArray arr;
    for (const auto& s : snaps) {
        QJsonObject o;
        o[QStringLiteral("name")] = s.first;
        o[QStringLiteral("mtime_ms")] = static_cast<double>(s.second);
        arr.append(o);
    }
    return resp(200, QJsonObject{ { QStringLiteral("snapshots"), arr } });
}

bool snapshotNameOk(const QString& name)
{
    if (name.isEmpty() || name.size() > 48)
        return false;
    for (const QChar& c : name) {
        const char16_t u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                        || (u >= '0' && u <= '9') || u == '_' || u == '-';
        if (!ok)
            return false;
    }
    return true;
}

// 保存当前状态为命名存档点（console 热存档，VM 运行中，不关机）。
Resp snapshotSave(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QString name = doc.object().value(QStringLiteral("name")).toString().trimmed();
    if (!snapshotNameOk(name))
        return mw::jsonError(400, QStringLiteral("存档名须为 1–48 位字母/数字/_/-"));
    const auto serial = runningSerial(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    const auto active = resolveActiveAvd();
    if (!active)
        return mw::jsonError(500, QStringLiteral("找不到可用的 AVD"));
    const auto r = supervisor::saveSnapshotAvd(*serial, active->dir, active->name, name, 90000);
    if (!r)
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("name"), name } });
}

// 回滚到指定存档点（console 热加载：guest 内部重启，宿主进程不动）。
Resp snapshotLoad(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QString name = doc.object().value(QStringLiteral("name")).toString().trimmed();
    if (!snapshotNameOk(name))
        return mw::jsonError(400, QStringLiteral("存档名无效"));
    const auto serial = runningSerial(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    const auto active = resolveActiveAvd();
    if (!active)
        return mw::jsonError(500, QStringLiteral("找不到可用的 AVD"));
    const auto r = supervisor::loadSnapshotAvd(*serial, active->dir, active->name, name, 150000);
    if (!r)
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("name"), name } });
}

// 删除存档点：运行中走 console（存储侧一致），已关停直接删目录。
Resp snapshotDelete(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QString name = doc.object().value(QStringLiteral("name")).toString().trimmed();
    if (!snapshotNameOk(name))
        return mw::jsonError(400, QStringLiteral("存档名无效"));
    const auto active = resolveActiveAvd();
    if (!active)
        return mw::jsonError(500, QStringLiteral("找不到可用的 AVD"));
    const QString snapDir = QDir(QDir(active->dir).filePath(QStringLiteral("snapshots")))
                                .filePath(name);
    if (!QFileInfo(QDir(snapDir).filePath(QStringLiteral("snapshot.pb"))).isFile())
        return mw::jsonError(404, QStringLiteral("存档点 %1 不存在").arg(name));
    if (const auto serial = runningSerial(state)) {
        // console 删（存储侧一致）；被拒时原文返回
        const auto r = supervisor::emuConsoleCmd(
            *serial, QStringLiteral("avd snapshot del %1").arg(name));
        if (!r || r.value().contains("KO")) {
            const QString e =
                r ? QStringLiteral("删除被拒: %1")
                        .arg(QString::fromUtf8(r.value()).trimmed())
                  : r.error();
            return mw::jsonError(500, e);
        }
    }
    // console 删除后目录残留（或未运行直接删）→ 文件系统兜底
    if (QFileInfo::exists(snapDir) && !QDir(snapDir).removeRecursively())
        return mw::jsonError(500, QStringLiteral("删除目录失败: %1").arg(snapDir));
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("name"), name } });
}

// ===================== 应用列表端点 =====================

Resp appsList(AppState* state)
{
    // 模拟器晚于 adb server 启动会错过端口扫描注册——发现不了就重启 server 重扫
    if (!state->vmRunning()) {
        QTcpSocket probe;
        probe.connectToHost(QStringLiteral("127.0.0.1"), 5555);
        const bool portOpen = probe.waitForConnected(300);
        probe.abort();
        if (portOpen)
            (void)state->adb.restartServer();
    }
    // 找到在线设备
    QString serial = QStringLiteral("emulator-5554");
    {
        const auto dev = state->adb.findOnlineDevice();
        if (dev)
            serial = dev.value().serial;
    }

    // 列出有启动入口的应用（用户可见的，含系统设置等）
    auto out = state->adb.shellBlocking(
        serial,
        QStringLiteral("pm query-activities --brief -a android.intent.action.MAIN"
                       " -c android.intent.category.LAUNCHER 2>/dev/null | grep '/' |"
                       " sed 's|.* ||;s|/.*||' | sort -u;"
                       " echo __NIGHT__$(settings get secure ui_night_mode)"),
        10000);
    if (!out)
        return mw::jsonError(500, out.error());
    // 末行 __NIGHT__<v>：VM 深色主题标志（ui_night_mode：MODE_NIGHT_NO=1、
    // MODE_NIGHT_YES=2——与字面直觉相反，2 才是深色；null = 未设置/自动）
    bool vmDark = false;
    QString listing = out.value().outStr();
    const int nightMark = listing.lastIndexOf(QLatin1String("__NIGHT__"));
    if (nightMark >= 0) {
        vmDark = listing.mid(nightMark + 9).trimmed().startsWith(QLatin1Char('2'));
        listing = listing.left(nightMark);
    }
    QJsonArray apps;
    QVector<QString> pkgs;
    for (const QString& p : listing.split(QLatin1Char('\n'))) {
        const QString s = p.trimmed();
        if (s.isEmpty())
            continue;
        QJsonObject o;
        o[QStringLiteral("package")] = s;
        // 注册表缓存：真实标签 + 图标文件路径（后台线程拉取，先到先用）
        const auto cached = state->appCache()->describe(s);
        // 已确认无可提取图标的应用（如系统 quicksearchbox，自适应图标
        // 无 PNG）不进应用库；未抓取过（刚安装）的仍展示，图标就绪前
        // 由前端回退字母块
        if (cached.value(QStringLiteral("noicon")).toBool())
            continue;
        const QString label = cached.value(QStringLiteral("label")).toString();
        o[QStringLiteral("name")] = label.isEmpty() ? s.section(QLatin1Char('.'), -1) : label;
        const QString icon = cached.value(QStringLiteral("icon")).toString();
        o[QStringLiteral("icon")] = icon.isEmpty() ? QJsonValue() : icon;
        o[QStringLiteral("running")] = false;
        o[QStringLiteral("mode")] = appMode(s);
        apps.append(o);
        pkgs.append(s);
    }
    // VM 在线：同步注册表并为缺图标的应用排队抓取（后台线程，不阻塞响应）
    state->appCache()->syncApps(pkgs);
    QJsonObject body{ { QStringLiteral("apps"), apps } };
    const QString wp = state->appCache()->wallpaperPath();
    if (!wp.isEmpty())
        body[QStringLiteral("wallpaper")] = wp;
    body[QStringLiteral("vm_dark")] = vmDark;
    return resp(200, body);
}

// ===================== 镜像管理（导入 + 官方拉取）与运行时导入 =====================

// 任务错误"已读"确认（UI 确认后才关闭提示；进行中的任务不可 ack）
Resp imageTaskAck(AppState* state)
{
    state->imageTask.ack();
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp imageTaskGet(AppState* state)
{
    return resp(200, state->imageTask.toJson());
}

Resp runtimeTaskGet(AppState* state)
{
    return resp(200, state->runtimeTask.toJson());
}

// 单任务模型：同一时间只允许一个导入/拉取（互斥足够，队列属于过度设计）。
Resp spawnImageTask(AppState* state, const QString& title,
                    std::function<mw::Result<QByteArray>(
                        const LogFn&, const std::atomic_bool&, const ProgressFn&)>
                        work)
{
    if (!state->imageTask.tryBegin(title)) {
        QJsonObject o;
        o[QStringLiteral("ok")] = false;
        o[QStringLiteral("error")] = QStringLiteral("已有镜像任务在进行中");
        return resp(409, o);
    }
    auto flag = std::make_shared<std::atomic_bool>(false);
    state->imageCancel = flag;
    std::thread([state, flag, work = std::move(work)]() {
        LogFn log = [state](const QString& line) {
            MW_LOG("image", line);
            state->imageTask.log(line, 200);
        };
        ProgressFn prog = [state](const QString& phase, quint64 done, quint64 total) {
            state->imageTask.setProgress(phase, done, total);
        };
        auto r = work(log, *flag, prog);
        if (r)
            state->imageTask.finishOk(QString::fromUtf8(r.value()));
        else {
            // LaunchFailed 的 Display 前缀对用户无意义，剥掉
            QString e = r.error();
            const QString prefix = QStringLiteral("emulator launch failed: ");
            while (e.startsWith(prefix))
                e.remove(0, prefix.size());
            state->imageTask.finishErr(e);
        }
    }).detach();
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp imageImport(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QJsonObject v = doc.object();
    const QString path = v.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 path"));
    std::optional<QString> sha256;
    if (v.value(QStringLiteral("sha256")).isString())
        sha256 = v.value(QStringLiteral("sha256")).toString();
    const QString name =
        v.value(QStringLiteral("name")).toString(QStringLiteral("houttuynia"));
    const auto sdk = supervisor::Sdk::detect();
    if (!sdk)
        return mw::jsonError(500, QStringLiteral("SDK 探测失败: %1").arg(sdk.error()));
    const QString title =
        QStringLiteral("导入 %1").arg(QFileInfo(path).fileName());
    const supervisor::Sdk sdkV = sdk.value();
    return spawnImageTask(
        state, title,
        [path, sha256, sdkV, name](const LogFn& log, const std::atomic_bool& cancel,
                                   const ProgressFn& prog) -> mw::Result<QByteArray> {
            QString installedDir;
            auto r = supervisor::importImage(path, sha256, name, sdkV, log, cancel, prog,
                                             &installedDir);
            if (!r)
                return mw::err<QByteArray>(r.error());
            return mw::Result<QByteArray>::Ok(installedDir.toUtf8());
        });
}

Resp imageOfficialPull(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QString target = doc.object().value(QStringLiteral("target")).toString();
    if (target.isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 target"));
    const auto sdk = supervisor::Sdk::detect();
    if (!sdk)
        return mw::jsonError(500, QStringLiteral("SDK 探测失败: %1").arg(sdk.error()));
    std::optional<QString> proxy;
    if (doc.object().value(QStringLiteral("proxy")).isString()) {
        const QString p = doc.object().value(QStringLiteral("proxy")).toString();
        if (!p.isEmpty())
            proxy = p;
    }
    const bool community = target.startsWith(QStringLiteral("community;"));
    const QString pullId =
        community ? target.section(QLatin1Char(';'), 1, 1) : target;
    const QString title =
        community ? QStringLiteral("拉取社区镜像 %1").arg(pullId)
                  : QStringLiteral("拉取官方镜像 %1").arg(target);
    const supervisor::Sdk sdkV = sdk.value();
    return spawnImageTask(
        state, title,
        [pullId, community, sdkV, proxy](const LogFn& log, const std::atomic_bool& cancel,
                                         const ProgressFn& prog) -> mw::Result<QByteArray> {
            QString installedDir;
            const auto r =
                community
                    ? supervisor::communityPull(pullId, sdkV, proxy, log, cancel, prog,
                                                &installedDir)
                    : supervisor::officialPull(pullId, sdkV, proxy, log, cancel, prog,
                                               &installedDir);
            if (!r)
                return mw::err<QByteArray>(r.error());
            return mw::Result<QByteArray>::Ok(installedDir.toUtf8());
        });
}

Resp imageOfficialList(const QString& url)
{
    std::optional<QString> filter;
    if (const auto f = queryParam(url, QStringLiteral("filter")))
        filter = *f;
    std::optional<QString> proxy;
    if (const auto p = queryParam(url, QStringLiteral("proxy"))) {
        if (!p->isEmpty())
            proxy = *p;
    }
    auto list = supervisor::officialList(filter.value_or(QString()), proxy);
    // 社区镜像（GSI 等）与官方合并展示：官方仓库不可达时社区仍可列出
    QVector<supervisor::OfficialImage> merged =
        list ? list.value() : QVector<supervisor::OfficialImage>();
    merged += supervisor::communityList(filter.value_or(QString()));
    if (!list && merged.isEmpty())
        return mw::jsonError(
            502, QStringLiteral("官方仓库不可达: %1（可配 https_proxy）").arg(list.error()));
    QJsonArray items;
    for (const auto& i : merged) {
        QJsonObject o;
        o[QStringLiteral("path")] = i.path;
        QJsonValue display;
        const QStringList segs = i.path.split(QLatin1Char(';'));
        if (segs.size() > 1) {
            if (const auto name = supervisor::androidVersionName(segs.at(1)))
                display = *name;
        }
        // 社区条目自带完整 display（版本+特性），官方仅版本名 → 拼 path
        if (i.source == QLatin1String("community"))
            display = i.display;
        o[QStringLiteral("display")] = display;
        o[QStringLiteral("size")] = static_cast<double>(i.size);
        o[QStringLiteral("url")] = i.url;
        o[QStringLiteral("source")] = i.source;
        items.append(o);
    }
    return resp(200, QJsonObject{ { QStringLiteral("images"), items } });
}

// 导入运行时分发包（zip，内含 runtime/ 目录：emulator/、platform-tools/…）。
// 解压到 Application Support/MWine/runtime（ditto 保留符号链接与权限）。
// 核心体与 runtimeDownload 共用：调用前 runtimeTask 已 tryBegin，这里只做
// 解压/落位/收尾（失败 finishErr，成功 finishOk）。
static void runRuntimeImport(AppState* state, const QString& path)
{
    const QString support =
        QDir(homePath()).filePath(QStringLiteral("Library/Application Support/MWine"));
    const QString tmp = QDir(support).filePath(QStringLiteral("runtime.importing"));
    const QString finalDir = QDir(support).filePath(QStringLiteral("runtime"));
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp);
    const auto log = [state](const QString& line) { state->runtimeTask.log(line, 200); };
    const auto fail = [state](const QString& msg) {
        state->runtimeTask.finishErr(msg);
    };
    // ditto 解压（macOS 自带；保留 symlink/权限，zip 内含顶层 runtime/）
    log(QStringLiteral("正在解压（约 1G，需数分钟）…"));
    const auto out = mw::runProcess(QStringLiteral("ditto"),
                                    { QStringLiteral("-x"), QStringLiteral("-k"), path,
                                      tmp },
                                    20 * 60 * 1000);
    if (!out.error.isEmpty()) {
        fail(QStringLiteral("ditto 启动失败: %1").arg(out.error));
        return;
    }
    if (out.exitCode != 0) {
        fail(QStringLiteral("解压失败: %1").arg(QString::fromUtf8(out.stdErr).trimmed()));
        return;
    }
    // zip 内顶层为 runtime/ → 移到正式位；其他布局按根目录处理
    QString src;
    if (QFileInfo(QDir(tmp).filePath(QStringLiteral("runtime/emulator"))).isDir())
        src = QDir(tmp).filePath(QStringLiteral("runtime"));
    else if (QFileInfo(QDir(tmp).filePath(QStringLiteral("emulator"))).isDir())
        src = tmp;
    else {
        fail(QStringLiteral("包内未找到 emulator 目录（布局应为 runtime/emulator 或 emulator）"));
        QDir(tmp).removeRecursively();
        return;
    }
    QDir(finalDir).removeRecursively();
    if (!QDir().rename(src, finalDir)) {
        fail(QStringLiteral("安装失败: 无法移动 %1 → %2").arg(src, finalDir));
        QDir(tmp).removeRecursively();
        return;
    }
    // 镜像与运行时分属两个外置存储：运行时内补 system-images 符号链接
    //（与 bundle 时代同构，镜像导入/下载经它落位）
    {
        const QString imgStore = QDir(support).filePath(QStringLiteral("system-images"));
        QDir().mkpath(imgStore);
        const QString link = QDir(finalDir).filePath(QStringLiteral("system-images"));
        if (!QFileInfo(link).isSymLink()) {
            const QFileInfo li(link);
            if (li.isDir() && !li.isSymLink())
                QDir(link).removeRecursively();
            else if (li.exists())
                QFile::remove(link);
            QFile::link(imgStore, link);
        }
    }
    QDir(tmp).removeRecursively();
    state->runtimeTask.finishOk(finalDir);
    log(QStringLiteral("✓ 运行时安装完成"));
}

Resp runtimeImport(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QString path = doc.object().value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 path"));
    if (!QFileInfo(path).isFile())
        return mw::jsonError(400, QStringLiteral("文件不存在: %1").arg(path));
    if (QFileInfo(QDir(supervisor::Sdk::externalRuntimeDir())
                      .filePath(QStringLiteral("emulator")))
            .isDir())
        return mw::jsonError(
            409,
            QStringLiteral("运行时已安装（如需重装先删除 Application Support/MWine/runtime）"));
    if (!state->runtimeTask.tryBegin(QStringLiteral("导入运行时: %1").arg(path))) {
        QJsonObject o;
        o[QStringLiteral("ok")] = false;
        o[QStringLiteral("error")] = QStringLiteral("已有运行时任务在进行中");
        return resp(409, o);
    }
    std::thread([state, path]() { runRuntimeImport(state, path); }).detach();
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

// 运行时在线下载默认源（GitHub Releases latest 资产；单文件 ≤2G、Range 可续传）。
// 优先级：请求 body.url > 环境变量 MWINE_RUNTIME_URL > 内置默认。
static QString runtimeDownloadUrl(const QJsonObject& body)
{
    const QString fromBody = body.value(QStringLiteral("url")).toString().trimmed();
    if (!fromBody.isEmpty())
        return fromBody;
    const QByteArray env = qgetenv("MWINE_RUNTIME_URL");
    if (!env.trimmed().isEmpty())
        return QString::fromLocal8Bit(env);
    return QStringLiteral(
        "https://github.com/Apuyuseng/houttuynia/releases/latest/download/"
        "Houttuynia-runtime-v1.zip");
}

// 在线下载运行时分发包 → 落盘 downloads/Houttuynia-runtime.zip（断点续传）→
// 复用导入管线装到正式位。下载中断可重试续传；解压阶段不可取消。
Resp runtimeDownload(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    const QJsonObject req = parseErr.error == QJsonParseError::NoError && doc.isObject()
                                ? doc.object() : QJsonObject{};
    if (QFileInfo(QDir(supervisor::Sdk::externalRuntimeDir())
                      .filePath(QStringLiteral("emulator")))
            .isDir())
        return mw::jsonError(
            409,
            QStringLiteral("运行时已安装（如需重装先删除 Application Support/MWine/runtime）"));
    if (!state->runtimeTask.tryBegin(QStringLiteral("准备下载运行时…"))) {
        QJsonObject o;
        o[QStringLiteral("ok")] = false;
        o[QStringLiteral("error")] = QStringLiteral("已有运行时任务在进行中");
        return resp(409, o);
    }
    auto flag = std::make_shared<std::atomic_bool>(false);
    state->runtimeCancel = flag;
    const QString url = runtimeDownloadUrl(req);
    std::thread([state, flag, url]() {
        const QString support = QDir(homePath()).filePath(
            QStringLiteral("Library/Application Support/MWine"));
        const QString downloads = QDir(support).filePath(QStringLiteral("downloads"));
        QDir().mkpath(downloads);
        const QString zipPath = QDir(downloads).filePath(QStringLiteral("Houttuynia-runtime.zip"));
        const auto log = [state](const QString& line) {
            MW_LOG("core", line);
            state->runtimeTask.log(line, 200);
        };
        log(QStringLiteral("下载 %1").arg(url));
        const store::SourceOpts opts; // 无代理；downloadFile 自带探测 + 续传 + 重试
        // 速度估算：downloadFile 的进度回调约 1s 节流，两次回调间字节差/时间差
        qint64 lastBytes = 0;
        QElapsedTimer speedClock;
        speedClock.start();
        const auto prog = [state, &lastBytes, &speedClock](quint64 done, quint64 total) {
            const qint64 elapsed = speedClock.elapsed();
            QString phase = QStringLiteral("下载运行时 %1%")
                                .arg(total > 0 ? QString::number(done * 100 / total)
                                               : QStringLiteral("?"));
            if (elapsed >= 1000) {
                const double mbps = (static_cast<qint64>(done) - lastBytes) / 1024.0 / 1024.0
                                    / (elapsed / 1000.0);
                if (mbps > 0.05)
                    phase += QStringLiteral("  %1 MB/s").arg(QString::number(mbps, 'f', 1));
                lastBytes = static_cast<qint64>(done);
                speedClock.restart();
            }
            state->runtimeTask.setProgress(phase, done, total);
        };
        const auto r = mw::store::detail::downloadFile(
            url, zipPath, opts, QByteArrayLiteral("Houttuynia/1.0"), *flag, prog, 0, log);
        if (!r) {
            // 取消/网络中断保留半档，重试即续传
            state->runtimeTask.finishErr(
                QStringLiteral("%1（已下载部分已保留，重新下载将断点续传）").arg(r.error()));
            return;
        }
        log(QStringLiteral("✓ 下载完成，开始解压安装…"));
        runRuntimeImport(state, zipPath);
    }).detach();
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp runtimeCancel(AppState* state)
{
    state->runtimeCancel->store(true, std::memory_order_relaxed);
    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    return resp(200, o);
}

// 运行时状态：present=可直接启动 VM；source 说明用哪份
//（external=外置正式位 / sdk=本机 Android SDK / bundled=内嵌旧版 / none=缺失）。
Resp runtimeStatus(AppState* state)
{
    Q_UNUSED(state);
    const QString ext = supervisor::Sdk::externalRuntimeDir();
    const QString sdk = QDir(homePath()).filePath(QStringLiteral("Library/Android/sdk"));
    QString source = QStringLiteral("none");
    if (QFileInfo(QDir(ext).filePath(QStringLiteral("emulator"))).isDir()) {
        source = QStringLiteral("external");
    } else {
        const QString contents =
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("../Resources/runtime/emulator"));
        if (QFileInfo(contents).isDir())
            source = QStringLiteral("bundled");
    }
    const bool sdkPresent = QFileInfo(QDir(sdk).filePath(QStringLiteral("emulator"))).isDir();
    if (source == QStringLiteral("none") && sdkPresent)
        source = QStringLiteral("sdk");
    QString pathStr;
    if (source == QStringLiteral("external"))
        pathStr = ext;
    else if (source == QStringLiteral("sdk"))
        pathStr = sdk;
    QJsonObject o;
    o[QStringLiteral("present")] = source != QStringLiteral("none");
    o[QStringLiteral("source")] = source;
    o[QStringLiteral("path")] = pathStr;
    o[QStringLiteral("sdk_available")] = sdkPresent;
    return resp(200, o);
}

Resp imageDelete(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QString path = doc.object().value(QStringLiteral("path")).toString();
    const auto sdk = supervisor::Sdk::detect();
    if (!sdk)
        return mw::jsonError(500, QStringLiteral("SDK 探测失败: %1").arg(sdk.error()));
    const QString sysroot = QDir(sdk.value().root).filePath(QStringLiteral("system-images"));
    // 路径安全：必须在 system-images 下、是目录、确实是镜像目录（含 system*.img）。
    // system-images 是符号链接（镜像外置 Application Support）：真实归属用解析后的
    // 双份形态判定（防逃逸），rel 提取用逻辑形态（不解析链接，与 config.ini 同构）。
    const QString abs = QDir::cleanPath(QDir(path).absolutePath());
    const QString canon = canonicalize(abs);
    const QString sysrootCanon = canonicalize(sysroot);
    const QString systemQemu =
        QDir(canon).filePath(QStringLiteral("system-qemu.img"));
    const QString systemImg = QDir(canon).filePath(QStringLiteral("system.img"));
    if (!pathStartsWith(canon, sysrootCanon) || !QFileInfo(canon).isDir()
        || !(QFileInfo(systemQemu).isFile() || QFileInfo(systemImg).isFile()))
        return mw::jsonError(400, QStringLiteral("非法的镜像目录"));
    // 在用检测：当前 AVD 指向的镜像不许删
    // 与 config.ini 的 image.sysdir.1 同构：system-images/<api>/<name>/arm64-v8a（去尾斜杠）
    QString rel;
    if (pathStartsWith(abs, sysroot))
        rel = abs.mid(sdk.value().root.size() + 1);
    else if (pathStartsWith(canon, sysrootCanon))
        rel = QStringLiteral("system-images/") + canon.mid(sysrootCanon.size() + 1);
    if (const auto cur = currentAvdSysdir()) {
        QString c = *cur;
        while (c.endsWith(QLatin1Char('/')))
            c.chop(1);
        if (c == rel)
            return mw::jsonError(409,
                                 QStringLiteral("该镜像正在使用中，请先切换到其他镜像"));
    }
    // 运行中检测：VM 在跑时一律拒绝删除（qemu 会 mmap 镜像文件）
    if (state->vmRunning())
        return mw::jsonError(409, QStringLiteral("虚拟机运行中，请先关停再删除镜像"));
    if (!QDir(canon).removeRecursively())
        return mw::jsonError(500, QStringLiteral("删除失败: %1").arg(canon));
    // 清掉因此变空的父目录（…/android-21/default 这类空壳不留；
    // 与真实根 sysrootCanon 比较——canon 已解析符号链接，拿逻辑根 sysroot
    // 比会失配并一路向上误删外置存储根目录）
    QDir cur = QFileInfo(canon).dir();
    while (cur.path() != sysrootCanon) {
        const auto entries = cur.entryList(QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs);
        if (!entries.isEmpty())
            break;
        if (!QDir().rmdir(cur.path()))
            break;
        cur = QFileInfo(cur.path()).dir();
        if (cur.path() == QLatin1String("/") || cur.path().isEmpty())
            break;
    }
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

// 切换当前 AVD 使用的镜像。完整业务流：
// 1. 先校验路径合法性（无效则不动虚拟机）
// 2. VM 运行中且未确认 → 409 needs_confirm（UI 弹确认框）
// 3. stop_vm=true：不存档直接关停（切换后快照本就作废）→ 切换 → 自动用新镜像重启
Resp imageUse(AppState* state, const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误: %1").arg(parseErr.errorString()));
    const QJsonObject v = doc.object();
    const QString path = v.value(QStringLiteral("path")).toString();
    const QJsonValue stopVmV = v.value(QStringLiteral("stop_vm"));
    const bool stopVm = stopVmV.isBool() ? stopVmV.toBool()
                                         : (stopVmV.isString()
                                                && stopVmV.toString() == QStringLiteral("true"));
    const auto sdk = supervisor::Sdk::detect();
    if (!sdk)
        return mw::jsonError(500, QStringLiteral("SDK 探测失败: %1").arg(sdk.error()));
    // ---- 先校验，避免停了机才报路径错 ----
    // system-images 是符号链接（镜像外置）：真实归属用解析后的双份形态判定，
    // rel 提取用逻辑形态（与 image.sysdir.1 同构）
    const QString sysroot = QDir(sdk.value().root).filePath(QStringLiteral("system-images"));
    const QString abs = QDir::cleanPath(QDir(path).absolutePath());
    const QString canon = canonicalize(abs);
    const QString sysrootCanon = canonicalize(sysroot);
    struct stat st;
    const bool systemImgEntry =
        ::lstat(QFile::encodeName(QDir(canon).filePath(QStringLiteral("system.img"))).constData(),
                &st) == 0;
    if (!pathStartsWith(canon, sysrootCanon) || !systemImgEntry)
        return mw::jsonError(400, QStringLiteral("非法的镜像目录"));
    const bool vmRunning = state->vmRunning();
    if (vmRunning && !stopVm) {
        QJsonObject o;
        o[QStringLiteral("running")] = true;
        o[QStringLiteral("needs_confirm")] = true;
        o[QStringLiteral("error")] = QStringLiteral("虚拟机运行中，切换需要先关停");
        return resp(409, o);
    }

    // ---- 关停（不存档：旧镜像的快照切换后即作废）----
    if (vmRunning) {
        const auto kill = supervisor::emuKillWait(&state->adb, onlineOr5554(state), 35000);
        if (!kill)
            return mw::jsonError(500, QStringLiteral("关停虚拟机失败: %1").arg(kill.error()));
        QMutexLocker l(&state->vmMu);
        state->qemuPid.reset();
    }

    // ---- 切换 sysdir + 清不兼容快照 ----
    // rel 必须是逻辑形态 system-images/<api>/<name>/arm64-v8a（写进 config.ini
    // 供 emulator 以 SDK 根相对解析）；取不出来说明路径形态非法，不能写配置
    QString rel;
    if (pathStartsWith(abs, sysroot))
        rel = abs.mid(sdk.value().root.size() + 1);
    else if (pathStartsWith(canon, sysrootCanon))
        rel = QStringLiteral("system-images/") + canon.mid(sysrootCanon.size() + 1);
    if (rel.isEmpty())
        return mw::jsonError(400, QStringLiteral("非法的镜像目录"));
    const auto active = resolveActiveAvd();
    if (!active) {
        return mw::jsonError(
            500, QStringLiteral("找不到可用的 AVD（两台主目录均无）。请先在概览页点一次启动完成初始化"));
    }
    const QString cfgPath = QDir(active->dir).filePath(QStringLiteral("config.ini"));
    const QString text = readTextFile(cfgPath);
    const QString newText = rewriteIniLine(
        text, QStringLiteral("image.sysdir.1"), QStringLiteral("image.sysdir.1 = %1/").arg(rel));
    if (!mw::writeFile(cfgPath, newText.toUtf8()))
        return mw::jsonError(500, QStringLiteral("写入 AVD 配置失败"));
    QDir(QDir(active->dir).filePath(QStringLiteral("snapshots"))).removeRecursively();
    QFile::remove(QDir(active->dir).filePath(QStringLiteral("hardware-qemu.ini")));
    const QDir avdDir(active->dir);
    for (const QFileInfo& e : avdDir.entryInfoList(QDir::Files))
        if (e.fileName().endsWith(QLatin1String(".qcow2")))
            QFile::remove(e.absoluteFilePath());

    // AVD 改名跟随新镜像版本（窗口标题）
    relabelAvd(sdk.value(), rel);

    // ---- 原本在跑就自动用新镜像重启 ----
    if (vmRunning)
        vmStart(state);
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("restarted"), vmRunning },
                                   { QStringLiteral("sysdir"), rel } });
}

Resp imageCancel(AppState* state)
{
    const bool wasRunning = state->imageTask.running.load();
    if (!wasRunning)
        return mw::jsonError(409, QStringLiteral("当前没有进行中的镜像任务"));
    state->imageCancel->store(true, std::memory_order_relaxed);
    state->imageTask.log(QStringLiteral("收到取消请求，正在中止…"));
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp imageList(AppState* state)
{
    Q_UNUSED(state);
    const auto sdk = supervisor::Sdk::detect();
    if (!sdk)
        return mw::jsonError(500, QStringLiteral("SDK 探测失败: %1").arg(sdk.error()));
    const std::optional<QString> currentSysdir = currentAvdSysdir();
    QJsonArray out;
    const QString sysdir = QDir(sdk.value().root).filePath(QStringLiteral("system-images"));
    QDir().mkpath(sysdir);
    const QDir sysdirD(sysdir);
    for (const QFileInfo& api : sysdirD.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString apiName = api.fileName();
        const QDir apiDir(api.absoluteFilePath());
        for (const QFileInfo& tag : apiDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString abi = QDir(tag.absoluteFilePath())
                                    .filePath(QStringLiteral("arm64-v8a"));
            const bool hasSystem =
                QFileInfo(QDir(abi).filePath(QStringLiteral("system-qemu.img"))).isFile()
                || QFileInfo(QDir(abi).filePath(QStringLiteral("system.img"))).isFile();
            if (!hasSystem)
                continue;
            quint64 size = 0;
            const QDir abiD(abi);
            for (const QFileInfo& f : abiD.entryInfoList(QDir::Files))
                size += quint64(f.size());
            // du 语义的实占更接近用户感知，但无 statvfs 磁粒度；给逻辑大小即可
            const QString rel = QStringLiteral("system-images/%1/%2/arm64-v8a")
                                    .arg(apiName, tag.fileName());
            QJsonObject o;
            o[QStringLiteral("api")] = apiName;
            QJsonValue display;
            if (const auto name = supervisor::androidVersionName(apiName))
                display = *name;
            o[QStringLiteral("display")] = display;
            o[QStringLiteral("name")] = tag.fileName();
            o[QStringLiteral("path")] = abi;
            o[QStringLiteral("rel")] = rel;
            o[QStringLiteral("in_use")] = currentSysdir.has_value() && *currentSysdir == rel;
            o[QStringLiteral("size_mb")] = static_cast<double>(size / 1048576ULL);
            out.append(o);
        }
    }
    return resp(200, QJsonObject{ { QStringLiteral("images"), out } });
}

// ===================== AVD 解析（双主目录 + relabel） =====================

// AVD 双主目录查找：AVD 实体可能在 bundled 主目录（Application Support）或
// 开发态主目录（~/.android/avd）——由 daemon 的启动环境决定，历史上分裂过。
// 返回 (avd 目录, 所属主目录)。
std::optional<QPair<QString, QString>> findAvdDir(const QString& name)
{
    QStringList homes;
    if (const auto sdk = supervisor::Sdk::detect())
        homes.append(sdk.value().avdHome());
    homes.append(QDir(homePath()).filePath(QStringLiteral(".android/avd")));
    homes.append(QDir(homePath())
                     .filePath(QStringLiteral("Library/Application Support/MWine/avd")));
    for (const QString& h : homes) {
        const QString d = QDir(h).filePath(QStringLiteral("%1.avd").arg(name));
        if (QFileInfo(QDir(d).filePath(QStringLiteral("config.ini"))).isFile())
            return qMakePair(d, h);
    }
    return std::nullopt;
}

// 解析"当前生效的 AVD"（带自愈）：
// 1) config.avd 指名的；2) 产品种子名 MWine4；3) 任一台主目录里带 sysdir 的首个 AVD。
// 情形 2/3 会把 config.avd 回写为实际名（历史上改名漂移产生过幽灵名）。
std::optional<ActiveAvd> resolveActiveAvd()
{
    const QString named = config::VmConfig::load().avd;
    if (const auto hit = findAvdDir(named)) {
        ActiveAvd a;
        a.dir = hit->first;
        a.home = hit->second;
        a.name = named;
        return a;
    }
    if (const auto hit = findAvdDir(QStringLiteral("MWine4"))) {
        config::VmConfig cfg = config::VmConfig::load();
        cfg.avd = QStringLiteral("MWine4");
        (void)cfg.save();
        MW_LOG("avd", QStringLiteral("自愈：幽灵名 %1 → MWine4").arg(named));
        ActiveAvd a;
        a.dir = hit->first;
        a.home = hit->second;
        a.name = QStringLiteral("MWine4");
        return a;
    }
    // 兜底扫描：两台主目录中任一含 image.sysdir 的 AVD
    const QStringList homes = {
        QDir(homePath()).filePath(QStringLiteral(".android/avd")),
        QDir(homePath()).filePath(QStringLiteral("Library/Application Support/MWine/avd")),
    };
    for (const QString& h : homes) {
        const QDir hd(h);
        for (const QFileInfo& e : hd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString name = e.fileName();
            if (name.endsWith(QLatin1String(".avd")))
                name.chop(4);
            const QString cfgP = QDir(e.absoluteFilePath())
                                     .filePath(QStringLiteral("config.ini"));
            const QString txt = readTextFile(cfgP);
            if (e.isDir() && txt.contains(QStringLiteral("image.sysdir.1"))) {
                config::VmConfig cfg = config::VmConfig::load();
                cfg.avd = name;
                (void)cfg.save();
                MW_LOG("avd", QStringLiteral("自愈：幽灵名 %1 → %2").arg(named, name));
                ActiveAvd a;
                a.dir = e.absoluteFilePath();
                a.home = h;
                a.name = name;
                return a;
            }
        }
    }
    return std::nullopt;
}

// 读当前 AVD 的 image.sysdir.1（无 AVD/无配置返回 nullopt）。
std::optional<QString> currentAvdSysdir()
{
    const auto active = resolveActiveAvd();
    if (!active)
        return std::nullopt;
    const QString text =
        readTextFile(QDir(active->dir).filePath(QStringLiteral("config.ini")));
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!trimmedStartsWith(line, QStringLiteral("image.sysdir.1")))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            return std::nullopt;
        QString v = line.mid(eq + 1).trimmed();
        while (v.endsWith(QLatin1Char('/')))
            v.chop(1);
        if (v.isEmpty())
            return std::nullopt;
        return v;
    }
    return std::nullopt;
}

// AVD 名 = 模拟器窗口标题的唯一来源（本版本 displayname/-name 均无效）。
// 把 AVD 重命名为版本化标签（如 Android13-Houttuynia / Android16-AOSP），
// 标题随之显示 "Android Emulator - Android13-Houttuynia:5554"。
// 幂等；必须在 VM 关停状态下调用。返回（可能不变的）AVD 名。
std::optional<QString> relabelAvd(const supervisor::Sdk& sdk, const QString& sysdirRel)
{
    Q_UNUSED(sdk);
    // sysdir_rel 形如 system-images/android-33/default/arm64-v8a
    QStringList segs = sysdirRel.trimmed().split(QLatin1Char('/'));
    while (!segs.isEmpty() && segs.first().isEmpty())
        segs.removeFirst();
    while (!segs.isEmpty() && segs.last().isEmpty())
        segs.removeLast();
    if (segs.isEmpty())
        return std::nullopt;
    segs.removeFirst(); // system-images
    if (segs.isEmpty())
        return std::nullopt;
    const QString apiSeg = segs.first(); // android-33
    const QString tag = segs.size() > 1 ? segs.at(1)
                                        : QStringLiteral("default"); // default / houttuynia / …
    QString ver = apiSeg;
    if (ver.startsWith(QStringLiteral("android-")))
        ver = ver.mid(8);
    ver = ver.section(QLatin1Char('-'), 0, 0);
    if (ver.isEmpty())
        return std::nullopt;
    QString suffix;
    if (tag.contains(QStringLiteral("houttuynia")))
        suffix = QStringLiteral("Houttuynia");
    else if (tag == QStringLiteral("default"))
        suffix = QStringLiteral("AOSP");
    else
        suffix = tag;
    suffix.remove(QLatin1Char('-')).remove(QLatin1Char('_'));
    const QString newName = QStringLiteral("Android%1-%2").arg(ver, suffix);
    const auto active = resolveActiveAvd();
    if (!active)
        return std::nullopt; // 无 AVD：跳过改名
    if (active->name == newName)
        return newName; // 已是目标名
    const QString toDir = QDir(active->home).filePath(QStringLiteral("%1.avd").arg(newName));
    const QString fromIni = QDir(active->home).filePath(QStringLiteral("%1.ini").arg(active->name));
    const QString toIni = QDir(active->home).filePath(QStringLiteral("%1.ini").arg(newName));
    // 旧名字可能钉住的运行时文件一并清掉
    QFile::remove(QDir(active->dir).filePath(QStringLiteral("hardware-qemu.ini")));
    // 同名旧实体挡路（历史改名残留）：目录 rename 遇非空目标会 ENOTEMPTY 静默失败，
    // config.avd 从此钉死旧名。切换镜像本就确认清数据，直接吸收残留实体。
    if (QFileInfo(toDir).isDir())
        QDir(toDir).removeRecursively();
    else if (QFileInfo::exists(toDir))
        QFile::remove(toDir);
    if (!QDir().rename(active->dir, toDir))
        return std::nullopt;
    if (QFileInfo(fromIni).isFile()) {
        QFile::rename(fromIni, toIni);
        QString txt = readTextFile(toIni);
        if (!txt.isEmpty()) {
            const QString newText = txt.replace(
                QStringLiteral("%1.avd").arg(active->name),
                QStringLiteral("%1.avd").arg(newName));
            (void)mw::writeFile(toIni, newText.toUtf8());
        }
    }
    config::VmConfig cfg = config::VmConfig::load();
    cfg.avd = newName;
    (void)cfg.save();
    MW_LOG("avd", QStringLiteral("重命名 %1 → %2（窗口标题随镜像版本）").arg(active->name, newName));
    return newName;
}

QString sdkRoot()
{
    if (const auto sdk = supervisor::Sdk::detect())
        return sdk.value().root;
    return QDir(homePath()).filePath(QStringLiteral("Library/Android/sdk"));
}

// ===================== 流量分析（eBPF 明文旁挂） =====================

// 解析目标 VM serial：优先 vm_serial，其次第一台在线 emulator。
std::optional<QString> captureSerial(AppState* state)
{
    const QString s = state->currentSerial();
    if (!s.isEmpty())
        return s;
    return onlineEmulator(state);
}

Resp captureStart(AppState* state, const QByteArray& body)
{
    QStringList packages;
    std::optional<QString> serialOverride;
    if (!body.trimmed().isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            const QJsonObject v = doc.object();
            if (v.value(QStringLiteral("apps")).isArray()) {
                for (const QJsonValue& p : v.value(QStringLiteral("apps")).toArray())
                    if (p.isString())
                        packages.append(p.toString());
            }
            if (v.value(QStringLiteral("serial")).isString())
                serialOverride = v.value(QStringLiteral("serial")).toString();
        }
    }
    std::optional<QString> serial;
    if (serialOverride)
        serial = serialOverride;
    else
        serial = captureSerial(state);
    if (!serial)
        return mw::jsonError(409, QStringLiteral("没有在线的虚拟机"));
    capture::TargetSpec spec;
    spec.packages = packages;
    const auto r = state->capture.start(*serial, spec);
    if (!r)
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("serial"), *serial } });
}

Resp captureStop(AppState* state)
{
    const auto r = state->capture.stop();
    if (!r)
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp captureStatus(AppState* state)
{
    return resp(200, state->capture.status());
}

Resp captureFlows(AppState* state, const QString& url)
{
    capture::FlowsQuery q;
    if (const auto v = queryParam(url, QStringLiteral("q")))
        q.q = *v;
    if (const auto v = queryParam(url, QStringLiteral("host")))
        q.host = *v;
    if (const auto v = queryParam(url, QStringLiteral("package")))
        q.package = *v;
    if (const auto v = queryParam(url, QStringLiteral("method")))
        q.method = *v;
    if (const auto v = queryParam(url, QStringLiteral("status"))) {
        bool ok = false;
        const int s = v->toInt(&ok);
        if (ok)
            q.status = s;
    }
    if (const auto v = queryParam(url, QStringLiteral("status_class"))) {
        bool ok = false;
        const int s = v->toInt(&ok);
        if (ok)
            q.statusClass = s;
    }
    if (const auto v = queryParam(url, QStringLiteral("kind")))
        q.kind = *v;
    if (const auto v = queryParam(url, QStringLiteral("since"))) {
        bool ok = false;
        const qint64 s = v->toLongLong(&ok);
        if (ok)
            q.sinceMs = s;
    }
    if (const auto v = queryParam(url, QStringLiteral("limit"))) {
        bool ok = false;
        const int l = v->toInt(&ok);
        if (ok && l > 0)
            q.limit = l;
    }
    if (const auto v = queryParam(url, QStringLiteral("offset"))) {
        bool ok = false;
        const int o = v->toInt(&ok);
        if (ok && o > 0)
            q.offset = o;
    }
    if (q.limit > 1000)
        q.limit = 1000;
    const bool wantTotal =
        queryParam(url, QStringLiteral("total")).value_or(QString()) == QStringLiteral("1");
    const auto flows = state->capture.queryFlows(q);
    if (!flows)
        return mw::jsonError(500, flows.error());
    QJsonArray arr;
    for (const auto& f : flows.value())
        arr.append(f);
    QJsonObject o;
    o[QStringLiteral("flows")] = arr;
    o[QStringLiteral("count")] = static_cast<double>(flows.value().size());
    o[QStringLiteral("total")] = wantTotal
                                     ? QJsonValue(static_cast<double>(
                                           state->capture.totalFlows(q).valueOr(0)))
                                     : QJsonValue();
    return resp(200, o);
}

Resp captureLive(AppState* state)
{
    const auto live = state->capture.liveFlows();
    QJsonArray arr;
    for (const auto& f : live)
        arr.append(f);
    return resp(200, QJsonObject{ { QStringLiteral("flows"), arr },
                                   { QStringLiteral("count"),
                                     static_cast<double>(live.size()) } });
}

Resp captureClear(AppState* state)
{
    const auto r = state->capture.clear();
    if (!r)
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp captureWebviewExport(AppState* state)
{
    // WebView 密钥/pcap 旁观导出：拉 guest 密钥与抓包到
    // ~/.mwine/capture/webview-<ts>/，返回目录路径
    const auto dir = state->capture.exportWebviewBundle();
    if (!dir)
        return mw::jsonError(500, dir.error());
    QJsonObject o;
    o[QStringLiteral("ok")] = true;
    o[QStringLiteral("dir")] = dir.value();
    return mw::jsonReply(200, mw::toJsonBytes(o));
}

Resp captureStats(AppState* state)
{
    return resp(200, state->capture.stats());
}

Resp captureFlowDetail(AppState* state, const QString& idStr)
{
    bool ok = false;
    const qint64 id = idStr.toLongLong(&ok);
    if (!ok)
        return mw::jsonError(400, QStringLiteral("bad id"));
    const auto r = state->capture.flowDetail(id);
    if (!r)
        return mw::jsonError(500, r.error());
    if (r.value().isEmpty())
        return mw::jsonError(404, QStringLiteral("not found"));
    return resp(200, r.value());
}

// ---------------------------------------------------------------------------
// ecapture 组件按需安装（app 包不内置，开启流量分析时下载到 ~/.mwine/bin/）

// 安装位 = findHostBinary() 的兜底位，装完无需重启即可被采集链路找到
static QString captureBinInstallPath()
{
    return QDir(mw::mwineDir()).filePath(QStringLiteral("bin/ecapture"));
}

// 下载源优先级：请求 body.url > 环境变量 MWINE_ECAPTURE_URL > 内置默认
//（与运行时分发包同库 latest 资产，upx 压缩后约 13MB）
static QString captureBinDownloadUrl(const QJsonObject& body)
{
    const QString fromBody = body.value(QStringLiteral("url")).toString().trimmed();
    if (!fromBody.isEmpty())
        return fromBody;
    const QByteArray env = qgetenv("MWINE_ECAPTURE_URL");
    if (!env.trimmed().isEmpty())
        return QString::fromLocal8Bit(env);
    return QStringLiteral(
        "https://github.com/Apuyuseng/houttuynia/releases/latest/download/"
        "ecapture-android-arm64");
}

Resp captureBinStatus(AppState* state)
{
    const QString bin = capture::Session::findHostBinary();
    QJsonObject o;
    o[QStringLiteral("present")] = !bin.isEmpty();
    o[QStringLiteral("path")] = bin;
    o[QStringLiteral("installing")] = state->captureBinTask.running.load();
    return resp(200, o);
}

Resp captureBinInstall(AppState* state, const QByteArray& body)
{
    QJsonObject req;
    if (!body.trimmed().isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject())
            req = doc.object();
    }
    if (!state->captureBinTask.tryBegin(QStringLiteral("准备下载流量分析组件…")))
        return mw::jsonError(409, QStringLiteral("组件下载已在进行中"));
    auto flag = std::make_shared<std::atomic_bool>(false);
    state->captureBinCancel = flag;
    const QString url = captureBinDownloadUrl(req);
    std::thread([state, flag, url]() {
        const auto log = [state](const QString& line) {
            MW_LOG("core", line);
            state->captureBinTask.log(line, 100);
        };
        const QString dest = captureBinInstallPath();
        QDir().mkpath(QFileInfo(dest).absolutePath());
        // .part 落盘：失败/取消保留半档，重试断点续传
        const QString part = dest + QStringLiteral(".part");
        log(QStringLiteral("下载 %1").arg(url));
        const auto prog = [state](quint64 done, quint64 total) {
            state->captureBinTask.setProgress(
                QStringLiteral("下载组件 %1%")
                    .arg(total > 0 ? QString::number(done * 100 / total)
                                   : QStringLiteral("?")),
                done, total);
        };
        const store::SourceOpts opts; // 无代理；downloadFile 自带探测/续传/重试
        const auto r = mw::store::detail::downloadFile(
            url, part, opts, QByteArrayLiteral("Houttuynia/1.0"), *flag, prog, 0, log);
        if (!r) {
            state->captureBinTask.finishErr(
                QStringLiteral("%1（已下载部分已保留，重新下载将断点续传）").arg(r.error()));
            return;
        }
        // 校验：ELF + arm64（e_machine=0xB7）+ 体积下限——防 404 HTML/半档被装进去
        QFile f(part);
        if (!f.open(QIODevice::ReadOnly)) {
            state->captureBinTask.finishErr(QStringLiteral("IO 错误：无法读取 %1").arg(part));
            return;
        }
        const QByteArray head = f.read(20);
        const qint64 size = f.size();
        f.close();
        const bool validElf = head.size() >= 20
            // "\x7f" "ELF"：拆开写字面量，避免 \x7fE 被当超长十六进制转义
            && head.startsWith(QByteArrayLiteral("\x7f" "ELF"))
            && static_cast<uchar>(head.at(18)) == 0xb7;
        if (!validElf || size < 1024 * 1024) {
            QFile::remove(part); // 校验失败的半档没有续传价值
            state->captureBinTask.finishErr(QStringLiteral(
                "下载内容不是有效的 arm64 ELF（%1 字节）——下载源可能暂不可用，请稍后重试")
                .arg(size));
            return;
        }
        if (QFile::exists(dest))
            QFile::remove(dest);
        if (!QFile::rename(part, dest)) {
            state->captureBinTask.finishErr(QStringLiteral("IO 错误：无法落位 %1").arg(dest));
            return;
        }
        // 宿主本机模式直接执行；设备模式仅读取内容 push 给 guest，权限都需 755
        QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                         | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                                         | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                         | QFileDevice::ExeOther);
        log(QStringLiteral("✓ ecapture 组件就绪: %1").arg(dest));
        state->captureBinTask.finishOk(dest);
    }).detach();
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp captureBinCancel(AppState* state)
{
    state->captureBinCancel->store(true, std::memory_order_relaxed);
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp captureBinTaskStatus(AppState* state)
{
    return resp(200, state->captureBinTask.toJson());
}

// ===================== 交互式 ADB 终端 =====================

// 建立终端会话：设备侧长驻 PTY shell，UI 轮询 read 增量拉输出、write 送输入。
Resp termOpen(AppState* state) // 日志版

{
    const auto serial = onlineEmulator(state);
    if (!serial)
        return mw::jsonError(503, QStringLiteral("Android 未运行"));
    auto s = state->terminals.open(&state->adb, *serial);
    if (!s)
        return mw::jsonError(500, s.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("id"),
                                     static_cast<double>(s.value()->id) },
                                   { QStringLiteral("serial"), s.value()->serial },
                                   { QStringLiteral("pty"), s.value()->pty } });
}

Resp termRead(AppState* state, quint64 id, const QString& url)
{
    const auto s = state->terminals.get(id);
    if (!s) {
        return resp(404, QJsonObject{ { QStringLiteral("error"),
                                        QStringLiteral("会话不存在") },
                                      { QStringLiteral("alive"), false } });
    }
    quint64 cursor = 0;
    if (const auto c = queryParam(url, QStringLiteral("cursor"))) {
        bool ok = false;
        const quint64 v = c->toULongLong(&ok);
        if (ok)
            cursor = v;
    }
    const auto r = s->readSince(cursor);
    return resp(200, QJsonObject{ { QStringLiteral("id"), static_cast<double>(id) },
                                   { QStringLiteral("cursor"),
                                     static_cast<double>(r.first) },
                                   { QStringLiteral("data"),
                                     QString::fromUtf8(r.second) },
                                   { QStringLiteral("alive"), s->alive.load() },
                                   { QStringLiteral("pty"), s->pty } });
}

Resp termWrite(AppState* state, quint64 id, const QByteArray& body)
{
    const auto s = state->terminals.get(id);
    if (!s) {
        return resp(404, QJsonObject{ { QStringLiteral("error"),
                                        QStringLiteral("会话不存在") },
                                      { QStringLiteral("alive"), false } });
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError)
        return mw::jsonError(400, QStringLiteral("JSON 错误"));
    const QString data = doc.object().value(QStringLiteral("data")).toString();
    const auto r = s->write(data.toUtf8());
    if (!r)
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp termClose(AppState* state, quint64 id)
{
    const bool ok = state->terminals.close(id);
    return resp(200, QJsonObject{ { QStringLiteral("ok"), ok } });
}

// ===================== 真机连接 =====================

// body 解析（与 termWrite 同风格）；JSON 坏 → nullopt（已回 400）
static std::optional<QJsonObject> bodyJson(const QByteArray& body)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    return doc.object();
}

Resp devicesList(AppState* state)
{
    const auto r = state->devices.listDevices();
    if (!r.ok())
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("devices"), r.value() } });
}

Resp devicesDiscovered(AppState* state)
{
    const auto r = state->devices.discovered();
    if (!r.ok())
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("services"), r.value() } });
}

Resp devicesPair(AppState* state, const QByteArray& body)
{
    const auto o = bodyJson(body);
    if (!o.has_value())
        return mw::jsonError(400, QStringLiteral("JSON 错误"));
    const auto r = state->devices.pair(*o);
    if (!r.ok())
        return mw::jsonError(500, r.error());
    return resp(200, r.value());
}

// 出码配对（Android Studio 同款方向）：Mac 出 WIFI:T:ADB 二维码，手机扫码
// 后在手机上起配对服务端；守护等 mDNS 指定实例出现 → adb pair → 自动连接。
// begin 可带 {"wait_ms":…}；进行中重复 begin 幂等返回当前会话。
Resp devicesHostPairBegin(AppState* state, const QByteArray& body)
{
    int waitMs = 180000;
    if (const auto o = bodyJson(body))
        waitMs = o->value(QLatin1String("wait_ms")).toInt(180000);
    const auto r = state->devices.hostPairBegin(waitMs);
    if (!r.ok())
        return mw::jsonError(500, r.error());
    return resp(200, r.value());
}

Resp devicesHostPairStatus(AppState* state)
{
    return resp(200, state->devices.hostPairStatus());
}

Resp devicesHostPairCancel(AppState* state)
{
    const bool cancelled = state->devices.hostPairCancel();
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true },
                                   { QStringLiteral("cancelled"), cancelled } });
}

Resp devicesConnect(AppState* state, const QByteArray& body)
{
    const auto o = bodyJson(body);
    if (!o.has_value())
        return mw::jsonError(400, QStringLiteral("JSON 错误"));
    const QString addr = o->value(QLatin1String("addr")).toString().trimmed();
    if (addr.isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 addr（ip:端口）"));
    const auto r = state->devices.connectAddr(addr);
    if (!r.ok())
        return mw::jsonError(500, r.error());
    return resp(200, r.value());
}

Resp devicesDisconnect(AppState* state, const QByteArray& body)
{
    const auto o = bodyJson(body);
    if (!o.has_value())
        return mw::jsonError(400, QStringLiteral("JSON 错误"));
    const QString serial = o->value(QLatin1String("serial")).toString().trimmed();
    if (serial.isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 serial"));
    const auto r = state->devices.disconnect(serial);
    if (!r.ok())
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp devicesForget(AppState* state, const QByteArray& body)
{
    const auto o = bodyJson(body);
    if (!o.has_value())
        return mw::jsonError(400, QStringLiteral("JSON 错误"));
    const QString serial = o->value(QLatin1String("serial")).toString().trimmed();
    if (serial.isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 serial"));
    const auto r = state->devices.forget(serial);
    if (!r.ok())
        return mw::jsonError(500, r.error());
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

Resp devicesTcpip(AppState* state, const QByteArray& body)
{
    const auto o = bodyJson(body);
    if (!o.has_value())
        return mw::jsonError(400, QStringLiteral("JSON 错误"));
    const QString serial = o->value(QLatin1String("serial")).toString().trimmed();
    if (serial.isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 serial"));
    const quint16 port = static_cast<quint16>(
        o->value(QLatin1String("port")).toInt(5555));
    const auto r = state->devices.usbToWifi(serial, port);
    if (!r.ok())
        return mw::jsonError(500, r.error());
    return resp(200, r.value());
}

// 真机投屏：拉 mwine CLI 的 mirror 路径（mwine-window --mirror，独立窗口进程）。
// 可选流参数：bitrate(bps) / max_size(px) / max_fps——缺省按设备默认
//（真机 10M/1600/60，见 mwine-window）。
Resp devicesMirror(AppState* state, const QByteArray& body)
{
    Q_UNUSED(state);
    const auto o = bodyJson(body);
    if (!o.has_value())
        return mw::jsonError(400, QStringLiteral("JSON 错误"));
    const QString serial = o->value(QLatin1String("serial")).toString().trimmed();
    if (serial.isEmpty())
        return mw::jsonError(400, QStringLiteral("缺少 serial"));
    const QString cli = resolveMwineCli();
    if (cli.isEmpty())
        return mw::jsonError(500, QStringLiteral("mwine CLI 未找到"));
    QStringList args{ QStringLiteral("mirror"), QStringLiteral("--serial"), serial };
    const auto optInt = [o](const char* key) -> QString {
        const double v = o->value(QLatin1String(key)).toDouble(0);
        return v > 0 ? QString::number(qint64(v)) : QString();
    };
    if (const auto v = optInt("bitrate"); !v.isEmpty())
        args << QStringLiteral("--bitrate") << v;
    if (const auto v = optInt("max_size"); !v.isEmpty())
        args << QStringLiteral("--max-size") << v;
    if (const auto v = optInt("max_fps"); !v.isEmpty())
        args << QStringLiteral("--max-fps") << v;
    const QString log = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                            .filePath(QStringLiteral("device-mirror-%1.log")
                                          .arg(QString(serial).replace(QLatin1Char(':'),
                                                                       QLatin1Char('-'))));
    if (!spawnDetachedLogged(cli, args, log))
        return mw::jsonError(500, QStringLiteral("mirror 进程拉起失败"));
    return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
}

} // namespace

// 应用缓存访问：按当前生效 AVD 键控；AVD 随镜像切换改名 → 自动换新缓存键
AppCache* AppState::appCache()
{
    // 互斥：HTTP 池线程并发调用 appsList 时，懒初始化双重 new/delete 会
    // 造成旧实例工作线程 use-after-free（守护段错误循环的根因）。
    // AVD 变更时旧实例只退休（shutdown 线程后保留对象，不 delete），
    // 杜绝请求侧仍持指针的窗口；磁盘目录保留，切回时重建。
    static QMutex mu;
    static QVector<AppCache*> retired;
    QMutexLocker lock(&mu);
    QString key = QStringLiteral("default");
    if (const auto avd = resolveActiveAvd())
        key = avd->name;
    key.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (m_appCache && m_appCacheKey == key)
        return m_appCache;
    if (m_appCache)
        retired.append(m_appCache); // 退休：dtor 会 join 工作线程
    m_appCache = new AppCache(key);
    m_appCacheKey = key;
    return m_appCache;
}


// ===================== 两级分发（api.rs route_with_body / route） =====================

Resp routeWithBody(const QString& method, const QString& url, const QByteArray& body,
                   AppState* state)
{
    // 动态路由：POST /api/apps/<pkg>/mode（body: {"mode":"window"|"emulator"}）
    if (method == QStringLiteral("POST") && url.startsWith(QStringLiteral("/api/apps/"))
        && url.endsWith(QStringLiteral("/mode"))) {
        QString pkg = url.mid(QString("/api/apps/").size());
        while (pkg.endsWith(QStringLiteral("/mode")))
            pkg.chop(QStringLiteral("/mode").size());
        return appSetMode(state, pkg, body);
    }
    // POST /api/config 的载荷在 body
    // 在线应用商店（搜索/版本/下载任务）
    if (method == QStringLiteral("GET") && pathOf(url) == QStringLiteral("/api/store/sources"))
        return storeSources(state);
    if (method == QStringLiteral("GET") && pathOf(url) == QStringLiteral("/api/store/search"))
        return storeSearch(state, url);
    if (method == QStringLiteral("GET") && pathOf(url) == QStringLiteral("/api/store/recommend"))
        return storeRecommend(state, url);
    if (method == QStringLiteral("GET") && pathOf(url) == QStringLiteral("/api/store/versions"))
        return storeVersions(state, url);
    if (method == QStringLiteral("GET") && pathOf(url) == QStringLiteral("/api/store/task"))
        return storeTaskGet(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/store/download"))
        return storeDownload(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/store/cancel"))
        return storeCancel(state);
    // APK 安装任务（须在前缀路由 /api/apps/<pkg>/... 之前注册）
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/apps/install"))
        return appsInstallPost(state, body);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/apps/install/task"))
        return appsInstallTaskGet(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/vm/show-window"))
        return vmShowWindow(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/runtime/import"))
        return runtimeImport(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/runtime/download"))
        return runtimeDownload(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/runtime/cancel"))
        return runtimeCancel(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/image/import"))
        return imageImport(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/image/official-pull"))
        return imageOfficialPull(state, body);
    if (method == QStringLiteral("GET") && url.startsWith(QStringLiteral("/api/image/official")))
        return imageOfficialList(url);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/image/delete"))
        return imageDelete(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/image/use"))
        return imageUse(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/image/cancel"))
        return imageCancel(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/vm/disk"))
        return vmDiskSet(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/config"))
        return configSet(state, body);
    // 运行时模拟（console 即时生效，需 VM 在跑）
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/runtime/geo"))
        return runtimeGeo(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/runtime/battery"))
        return runtimeBattery(state, body);
    // 摄像头画面贴图（虚拟场景海报，运行中即时生效）
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/runtime/camera-image"))
        return runtimeCameraImage(state, body);
    // 快照存档点管理
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/snapshot/save"))
        return snapshotSave(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/snapshot/load"))
        return snapshotLoad(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/snapshot/delete"))
        return snapshotDelete(state, body);
    // guest 前台应用（运行检测 / Activity 校准）
    if (method == QStringLiteral("GET") && pathOf(url) == QStringLiteral("/api/foreground"))
        return foreground(state);
    // 键位映射：模板 / 读写 / 截图
    if (method == QStringLiteral("GET") && pathOf(url) == QStringLiteral("/api/keys/templates"))
        return keysTemplates();
    if (method == QStringLiteral("GET") && pathOf(url) == QStringLiteral("/api/keys/list"))
        return keysList();
    if (url.startsWith(QStringLiteral("/api/keys/"))) {
        const QString pkg = url.mid(QString("/api/keys/").size());
        if (!pkg.contains(QLatin1Char('/'))) {
            if (method == QStringLiteral("GET"))
                return keysGet(pkg);
            if (method == QStringLiteral("PUT"))
                return keysSet(pkg, body);
            if (method == QStringLiteral("DELETE"))
                return keysDelete(pkg);
        } else if (pkg.endsWith(QStringLiteral("/shot"))) {
            const QString base = pkg.chopped(QStringLiteral("/shot").size());
            if (!base.isEmpty() && !base.contains(QLatin1Char('/'))) {
                if (method == QStringLiteral("POST"))
                    return keysShot(state, base);
                if (method == QStringLiteral("GET"))
                    return keysShotPath(base);
            }
        }
    }
    // 流量分析（eBPF 明文旁挂）
    if (url == QStringLiteral("/api/capture/start") && method == QStringLiteral("POST"))
        return captureStart(state, body);
    if (url == QStringLiteral("/api/capture/stop") && method == QStringLiteral("POST"))
        return captureStop(state);
    if (url == QStringLiteral("/api/capture/status") && method == QStringLiteral("GET"))
        return captureStatus(state);
    if (pathOf(url) == QStringLiteral("/api/capture/flows") && method == QStringLiteral("GET"))
        return captureFlows(state, url);
    if (url == QStringLiteral("/api/capture/live") && method == QStringLiteral("GET"))
        return captureLive(state);
    if (url == QStringLiteral("/api/capture/clear") && method == QStringLiteral("POST"))
        return captureClear(state);
    if (url.startsWith(QStringLiteral("/api/capture/flows/"))) {
        const QString id = url.mid(QString("/api/capture/flows/").size());
        if (method == QStringLiteral("GET") && !id.isEmpty())
            return captureFlowDetail(state, id);
    }
    if (pathOf(url) == QStringLiteral("/api/capture/stats") && method == QStringLiteral("GET"))
        return captureStats(state);
    if (url == QStringLiteral("/api/capture/webview-export") && method == QStringLiteral("POST"))
        return captureWebviewExport(state);
    // ecapture 组件按需安装（app 不内置；present/install 接口供前端引导下载）
    if (url == QStringLiteral("/api/capture/binary") && method == QStringLiteral("GET"))
        return captureBinStatus(state);
    if (url == QStringLiteral("/api/capture/binary/install") && method == QStringLiteral("POST"))
        return captureBinInstall(state, body);
    if (url == QStringLiteral("/api/capture/binary/cancel") && method == QStringLiteral("POST"))
        return captureBinCancel(state);
    if (url == QStringLiteral("/api/capture/binary/task") && method == QStringLiteral("GET"))
        return captureBinTaskStatus(state);
    // 交互式 ADB 终端（UI 终端页）：open 建会话，read/write/close 带会话 id
    if (method == QStringLiteral("POST") && pathOf(url) == QStringLiteral("/api/term/open"))
        return termOpen(state);
    if (url.startsWith(QStringLiteral("/api/term/"))) {
        const QString rest0 = url.mid(QString("/api/term/").size());
        const QString rest = rest0.section(QLatin1Char('?'), 0, 0);
        const int slash = rest.indexOf(QLatin1Char('/'));
        if (slash > 0) {
            const QString idStr = rest.left(slash);
            const QString action = rest.mid(slash + 1);
            bool ok = false;
            const quint64 id = idStr.toULongLong(&ok);
            if (ok) {
                if (method == QStringLiteral("GET") && action == QStringLiteral("read"))
                    return termRead(state, id, url);
                if (method == QStringLiteral("POST") && action == QStringLiteral("write"))
                    return termWrite(state, id, body);
                if (method == QStringLiteral("POST") && action == QStringLiteral("close"))
                    return termClose(state, id);
            }
        }
        return resp(400, QJsonObject{ { QStringLiteral("error"),
                                        QStringLiteral("bad term route") } });
    }
    // ===== 真机连接（USB / 扫码配对 / 配对码 / 手动 / USB 转无线 / 镜像） =====
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/devices"))
        return devicesList(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/devices/discovered"))
        return devicesDiscovered(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/devices/pair"))
        return devicesPair(state, body);
    if (method == QStringLiteral("POST")
        && url == QStringLiteral("/api/devices/hostpair/begin"))
        return devicesHostPairBegin(state, body);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/devices/hostpair"))
        return devicesHostPairStatus(state);
    if (method == QStringLiteral("POST")
        && url == QStringLiteral("/api/devices/hostpair/cancel"))
        return devicesHostPairCancel(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/devices/connect"))
        return devicesConnect(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/devices/disconnect"))
        return devicesDisconnect(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/devices/forget"))
        return devicesForget(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/devices/tcpip"))
        return devicesTcpip(state, body);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/devices/mirror"))
        return devicesMirror(state, body);
    return route(method, url, state);
}

Resp route(const QString& method, const QString& url, AppState* state)
{
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/status"))
        return status(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/vm/start"))
        return vmStart(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/vm/stop"))
        return vmStop(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/vm/disk"))
        return vmDiskGet(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/apps"))
        return appsList(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/config"))
        return configGet(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/snapshots"))
        return snapshotsList(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/perf"))
        return perf(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/identity/reset"))
        return identityReset(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/screenshot"))
        return screenshotSave(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/vm/reset"))
        return vmReset(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/image/list"))
        return imageList(state);
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/image/task/ack"))
        return imageTaskAck(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/image/task"))
        return imageTaskGet(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/runtime/status"))
        return runtimeStatus(state);
    if (method == QStringLiteral("GET") && url == QStringLiteral("/api/runtime/task"))
        return runtimeTaskGet(state);
    // 历史怪癖（照抄 api.rs）：route() 分支把 url 当 body 传给 config_set —— 实际
    // 请求总被 route_with_body 的 POST /api/config 分支先截走，本分支不可达。
    if (method == QStringLiteral("POST") && url == QStringLiteral("/api/config"))
        return configSet(state, url.toUtf8());
    if (method == QStringLiteral("GET") && url == QStringLiteral("/health"))
        return resp(200, QJsonObject{ { QStringLiteral("ok"), true } });
    // 动态路由：POST /api/apps/<pkg>/run
    if (method == QStringLiteral("POST") && url.startsWith(QStringLiteral("/api/apps/"))
        && url.endsWith(QStringLiteral("/run"))) {
        QString pkg = url.mid(QString("/api/apps/").size());
        if (pkg.endsWith(QStringLiteral("/run")))
            pkg.chop(QStringLiteral("/run").size());
        return appRun(state, pkg);
    }
    // keys 动态路由已在 routeWithBody 前置分发（见函数开头）
    return resp(404, QJsonObject{ { QStringLiteral("error"), QStringLiteral("not found") } });
}

} // namespace mw::daemon
