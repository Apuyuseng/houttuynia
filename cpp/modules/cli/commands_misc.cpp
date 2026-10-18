// capture（经 daemon REST）/ mirror（桩）/ debug / app 子命令。
// ← crates/mwine-cli/src/main.rs：cmd_capture_* / cmd_mirror / cmd_debug_* /
//    cmd_app_create / cmd_app_installer / emuhide_standalone / emuhide_raw
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QThread>

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "adb.hpp"
#include "app.hpp"
#include "args.hpp"
#include "cli_common.hpp"
#include "commands.hpp"
#include "fs.hpp"
#include "process.hpp"
#include "scrcpy/client.hpp"
#include "supervisor.hpp"

namespace mwine {

// ===================== capture（流量分析，经 mwine-cored REST） =====================

static QString urlencode(const QString& s)
{
    static const char* hex = "0123456789ABCDEF";
    const QByteArray bytes = s.toUtf8();
    QByteArray out;
    for (char c : bytes) {
        const unsigned char b = static_cast<unsigned char>(c);
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-'
            || b == '_' || b == '.' || b == '~')
            out.append(char(b));
        else {
            out.append('%');
            out.append(hex[b >> 4]);
            out.append(hex[b & 0xf]);
        }
    }
    return QString::fromLatin1(out);
}

static QString truncateChars(const QString& s, int n)
{
    return s.size() <= n ? s : s.left(qMax(0, n - 1)) + QStringLiteral("…");
}

static mw::Result<bool> captureStart(Args& args)
{
    const QStringList apps = args.multi({ "app" });
    const QByteArray body =
        QJsonDocument(QJsonObject{ { QStringLiteral("apps"),
                                     QJsonArray::fromStringList(apps) } })
            .toJson(QJsonDocument::Compact);
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/capture/start"), body);
    if (!r)
        return mw::err<bool>(r.error());
    const auto [code, resp] = r.value();
    if (code == 200)
        std::printf("✓ 采集已启动（%s）\n",
                    qPrintable(apps.isEmpty() ? QStringLiteral("全量") : apps.join(QStringLiteral(", "))));
    else {
        std::fprintf(stderr, "启动失败（%d）：%s\n", code, qPrintable(resp));
        ::exit(1);
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> captureStop(Args& args)
{
    Q_UNUSED(args);
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/capture/stop"));
    if (!r)
        return mw::err<bool>(r.error());
    const auto [code, resp] = r.value();
    if (code == 200)
        std::printf("✓ 采集已停止\n");
    else {
        std::fprintf(stderr, "停止失败（%d）：%s\n", code, qPrintable(resp));
        ::exit(1);
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> captureStatus(Args& args)
{
    Q_UNUSED(args);
    const auto r = coreApi(QStringLiteral("GET"), QStringLiteral("/api/capture/status"));
    if (!r)
        return mw::err<bool>(r.error());
    const auto [code, resp] = r.value();
    if (code != 200) {
        std::fprintf(stderr, "查询失败（%d）：%s\n", code, qPrintable(resp));
        ::exit(1);
    }
    const auto v = QJsonDocument::fromJson(resp.toUtf8()).object();
    QStringList targets;
    for (const auto& x : v.value(QStringLiteral("targets")).toArray())
        targets.append(x.toString());
    std::printf("运行: %s  目标: %s  事件: %lld  flow(live/库): %lld/%lld  已见字节: %lld\n",
                v.value(QStringLiteral("running")).toBool() ? "true" : "false",
                qPrintable(targets.isEmpty() ? QStringLiteral("全量")
                                             : targets.join(QStringLiteral(","))),
                static_cast<long long>(v.value(QStringLiteral("events_parsed")).toDouble()),
                static_cast<long long>(v.value(QStringLiteral("flows_live")).toDouble()),
                static_cast<long long>(v.value(QStringLiteral("flows_stored")).toDouble()),
                static_cast<long long>(v.value(QStringLiteral("bytes_seen")).toDouble()));
    const QString err = v.value(QStringLiteral("last_error")).toString();
    if (!err.isEmpty())
        std::printf("上次错误：%s\n", qPrintable(err));
    std::printf("数据库：%s\n",
                qPrintable(v.value(QStringLiteral("db_path")).toString(QStringLiteral("?"))));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> captureFlows(Args& args)
{
    const std::optional<QString> q = args.opt("q");
    const std::optional<QString> host = args.opt("host");
    const std::optional<QString> package = args.opt("package");
    const std::optional<QString> method = args.opt("method");
    const std::optional<QString> statusStr = args.opt("status");
    const std::optional<QString> kind = args.opt("kind");
    quint32 limit = 30;
    if (const auto l = args.opt("limit"))
        limit = l->toUInt();

    QString path = QStringLiteral("/api/capture/flows?limit=%1").arg(limit);
    for (const auto& [k, v] : std::vector<std::pair<const char*, const std::optional<QString>&>>{
             { "q", q }, { "host", host }, { "package", package }, { "method", method },
             { "kind", kind } }) {
        if (v && !v->isEmpty())
            path += QStringLiteral("&%1=%2").arg(QString::fromLatin1(k), urlencode(*v));
    }
    if (statusStr && !statusStr->isEmpty())
        path += QStringLiteral("&status=%1").arg(*statusStr);
    const auto r = coreApi(QStringLiteral("GET"), path);
    if (!r)
        return mw::err<bool>(r.error());
    const auto [code, resp] = r.value();
    if (code != 200) {
        std::fprintf(stderr, "查询失败（%d）：%s\n", code, qPrintable(resp));
        ::exit(1);
    }
    const auto v = QJsonDocument::fromJson(resp.toUtf8()).object();
    const auto flows = v.value(QStringLiteral("flows")).toArray();
    if (flows.isEmpty()) {
        std::printf("（无匹配 flow）\n");
        return mw::Result<bool>::Ok();
    }
    std::printf("%-5s %-12s %-28s %-7s %-6s %-40s %-10s\n", "ID", "时间", "HOST", "方法", "状态",
                "PATH", "APP");
    for (const auto& fv : flows) {
        const auto f = fv.toObject();
        const qint64 ts = static_cast<qint64>(f.value(QStringLiteral("first_ts_ms")).toDouble());
        // ms → HH:MM:SS（Rust 版同款朴素换算）
        const qint64 secs = ts / 1000;
        const QString time = QStringLiteral("%1:%2:%3")
                                 .arg((secs / 3600) % 24, 2, 10, QLatin1Char('0'))
                                 .arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
                                 .arg(secs % 60, 2, 10, QLatin1Char('0'));
        QString app = f.value(QStringLiteral("package")).toString();
        if (app.isEmpty())
            app = f.value(QStringLiteral("comm")).toString(QStringLiteral("-"));
        std::printf("%-5s %-12s %-28s %-7s %-6d %-40s %-10s\n",
                    qPrintable(QString::number(static_cast<qint64>(
                        f.value(QStringLiteral("id")).toDouble()))),
                    qPrintable(time),
                    qPrintable(truncateChars(f.value(QStringLiteral("host")).toString(QStringLiteral("-")), 28)),
                    qPrintable(f.value(QStringLiteral("method")).toString(QStringLiteral("-"))),
                    static_cast<int>(f.value(QStringLiteral("status")).toDouble()),
                    qPrintable(truncateChars(f.value(QStringLiteral("path")).toString(QStringLiteral("-")), 40)),
                    qPrintable(truncateChars(app, 10)));
    }
    std::printf("（%d 条）\n", int(flows.size()));
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> captureShow(Args& args)
{
    const auto idArg = args.take();
    if (!idArg)
        return badUsage<bool>(QStringLiteral("Usage: mwine capture show <id>"));
    const auto r = coreApi(QStringLiteral("GET"),
                           QStringLiteral("/api/capture/flows/%1").arg(*idArg));
    if (!r)
        return mw::err<bool>(r.error());
    const auto [code, resp] = r.value();
    if (code == 404) {
        std::fprintf(stderr, "flow %s 不存在\n", qPrintable(*idArg));
        ::exit(1);
    }
    if (code != 200) {
        std::fprintf(stderr, "查询失败（%d）：%s\n", code, qPrintable(resp));
        ::exit(1);
    }
    const auto v = QJsonDocument::fromJson(resp.toUtf8()).object();
    std::printf("=== flow %lld ===\n",
                static_cast<long long>(v.value(QStringLiteral("id")).toDouble()));
    std::printf("%s %s  →  %s:\n", qPrintable(v.value(QStringLiteral("method")).toString()),
                qPrintable(v.value(QStringLiteral("path")).toString()),
                qPrintable(v.value(QStringLiteral("host")).toString()));
    std::printf("状态: %d  类型: %s  应用: %s\n",
                static_cast<int>(v.value(QStringLiteral("status")).toDouble()),
                qPrintable(v.value(QStringLiteral("kind")).toString()),
                qPrintable(v.value(QStringLiteral("package")).toString()));
    std::printf("五元组: %s -> %s\n", qPrintable(v.value(QStringLiteral("src")).toString()),
                qPrintable(v.value(QStringLiteral("dst")).toString()));
    std::printf("\n--- 请求头 ---\n%s",
                qPrintable(v.value(QStringLiteral("req_head")).toString()));
    const QString reqBody =
        v.value(QStringLiteral("req_body")).toObject().value(QStringLiteral("text")).toString();
    if (!reqBody.isEmpty())
        std::printf("--- 请求体 ---\n%s", qPrintable(reqBody));
    std::printf("\n--- 响应头 ---\n%s",
                qPrintable(v.value(QStringLiteral("resp_head")).toString()));
    const QString respBody =
        v.value(QStringLiteral("resp_body")).toObject().value(QStringLiteral("text")).toString();
    if (!respBody.isEmpty())
        std::printf("--- 响应体 ---\n%s", qPrintable(respBody));
    std::printf("\n");
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> captureClear(Args& args)
{
    Q_UNUSED(args);
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/capture/clear"));
    if (!r)
        return mw::err<bool>(r.error());
    const auto [code, resp] = r.value();
    if (code == 200)
        std::printf("✓ 已清空\n");
    else {
        std::fprintf(stderr, "失败（%d）：%s\n", code, qPrintable(resp));
        ::exit(1);
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdCapture(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine capture <start|stop|status|flows|show|clear> […]"));
    if (*action == QStringLiteral("start"))
        return captureStart(args);
    if (*action == QStringLiteral("stop"))
        return captureStop(args);
    if (*action == QStringLiteral("status"))
        return captureStatus(args);
    if (*action == QStringLiteral("flows"))
        return captureFlows(args);
    if (*action == QStringLiteral("show"))
        return captureShow(args);
    if (*action == QStringLiteral("clear"))
        return captureClear(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

// ===================== mirror（scrcpy 镜像窗口） =====================

mw::Result<bool> cmdMirror(Args& args)
{
    // mwine mirror [--serial s] [--display-id N] [--record out.h264]
    //   → mwine-window --mirror（主屏镜像 + 可选裸流录制，诊断/录制用）
    //   --serial 指定真机（ip:port / USB serial）；缺省走安装目标解析（VM 优先）
    const std::optional<QString> record = args.opt("record");
    mw::adb::AdbClient client = adbClient();
    const std::optional<QString> explicitSerial = args.opt("serial");
    // 查看语义：镜像窗口绝不允许拉起新 VM——ensureInstallTarget 是安装
    // 语义（无设备就开机），曾把"点一下显示"变成"启动第二台 qemu"
    //（AVD 锁顶掉原实例，全链卡死）。daemon 上报的 serial 优先，退而取
    // 唯一在线 emulator，都没有就明确报错
    mw::Result<QString> serialR = mw::Result<QString>::Ok(QString());
    if (explicitSerial.has_value()) {
        serialR = mw::Result<QString>::Ok(*explicitSerial);
    } else {
        QString serial = daemonVmSerial();
        if (serial.isEmpty()) {
            const QStringList online = onlineEmulators(&client);
            if (online.size() == 1)
                serial = online.first();
        }
        if (serial.isEmpty())
            return mw::err<bool>(
                QStringLiteral("没有在线的 Android 设备（镜像不会自动开机；"
                               "请先在 Houttuynia 启动虚拟机）"));
        serialR = mw::Result<QString>::Ok(serial);
    }
    if (!serialR.ok())
        return mw::err<bool>(serialR.error());
    QStringList extra;
    if (const auto did = args.opt("display-id"))
        extra << QStringLiteral("--display-id") << *did;
    // 流调优透传（真机 Wi-Fi 隧道卡顿时降码率/分辨率；缺省按设备类型取默认）
    if (const auto v = args.opt("bitrate"))
        extra << QStringLiteral("--bitrate") << *v;
    if (const auto v = args.opt("max-size"))
        extra << QStringLiteral("--max-size") << *v;
    if (const auto v = args.opt("max-fps"))
        extra << QStringLiteral("--max-fps") << *v;
    return spawnWindowPlayer(QString(), serialR.value(), QString(), true,
                             record.value_or(QString()), extra);
}

// ===================== debug =====================

static QString debugSerial(const std::optional<QString>& serial)
{
    if (serial)
        return *serial;
    if (const auto d = adbClient().findOnlineDevice())
        return d.value().serial;
    return QStringLiteral("emulator-5554");
}

// 用 AppKit NSRunningApplication 隐藏/恢复模拟器主窗口（免辅助功能权限）。
// AppKit/objc 经运行时 dlopen 引入（不改 CMake 链接配置；Rust 版为 link 指令）。
// hide=nullopt 仅查询可见性；Some(hide) 执行 hide/unhide 并返回可见性。
static std::optional<bool> emuhideRaw(std::optional<bool> hide)
{
    void* objc = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
    ::dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY);
    if (!objc)
        return std::nullopt;
    using FnGetClass = void* (*)(const char*);
    using FnSel = void* (*)(const char*);
    using FnMsgSendInt = void* (*)(void*, void*, int);
    using FnMsgSendU8 = unsigned char (*)(void*, void*);
    const auto getClass = reinterpret_cast<FnGetClass>(::dlsym(objc, "objc_getClass"));
    const auto sel = reinterpret_cast<FnSel>(::dlsym(objc, "sel_registerName"));
    const auto withPid = reinterpret_cast<FnMsgSendInt>(::dlsym(objc, "objc_msgSend"));
    const auto boolRet = reinterpret_cast<FnMsgSendU8>(::dlsym(objc, "objc_msgSend"));
    if (!getClass || !sel || !withPid || !boolRet)
        return std::nullopt;

    // qemu-system 进程 pid
    const auto out = mw::runProcess(QStringLiteral("pgrep"), { QStringLiteral("-f"),
                                                                QStringLiteral("qemu-system") },
                                    10000);
    if (!out.ok())
        return std::nullopt;
    int pid = 0;
    for (const QString& l : QString::fromUtf8(out.stdOut).split(QLatin1Char('\n'))) {
        bool ok = false;
        const int p = l.trimmed().toInt(&ok);
        if (ok && p > 0) {
            pid = p;
            break;
        }
    }
    if (pid == 0)
        return std::nullopt;

    void* cls = getClass("NSRunningApplication");
    if (!cls)
        return std::nullopt;
    void* app = withPid(cls, sel("runningApplicationWithProcessIdentifier:"), pid);
    if (!app)
        return std::nullopt;
    // 原写法 `if (hide) sel(*hide ? "hide" : "unhide")`：外层只在 hide 时调、
    // 内层恒选 "hide" —— show 路径从未执行过（unhide 一直是空操作）。
    // show 还需 activate：-qt-hide-window 启动的进程 unhide 后窗口仍是
    // orderOut，激活才把窗口带前台
    if (hide)
        boolRet(app, sel("hide"));
    else {
        boolRet(app, sel("unhide"));
        withPid(app, sel("activateWithOptions:"),
                1 << 1 /* NSApplicationActivateIgnoringOtherApps */);
    }
    return boolRet(app, sel("isHidden")) == 0;
}

// 裸进程版隐藏实现（mwine debug emuhide 子命令：干净子进程里执行）。
// hide=true 时先睡 delay_ms 再持续压制：qemu 对 console multidisplay add 的
// 自动反隐藏是异步的（实测 1~9s），延迟+重复监控压制竞速。
static void emuhideStandalone(bool hide, quint64 delayMs)
{
    if (hide && delayMs > 0)
        QThread::msleep(delayMs);
    emuhideRaw(hide);
    if (hide) {
        // 守卫期：可见即压回
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 10 * 1000;
        while (QDateTime::currentMSecsSinceEpoch() < deadline) {
            QThread::msleep(1200);
            if (emuhideRaw(std::nullopt).value_or(false))
                emuhideRaw(true);
        }
    } else {
        QThread::msleep(500);
    }
}

static mw::Result<bool> debugDisplayFps(Args& args)
{
    Q_UNUSED(args);
    // TODO(mw_display): GrpcDisplayBackend + FramePump 帧率测量
    //（mw_display 模块尚未转换；依赖 gRPC 帧泵）。
    return mw::err<bool>(QStringLiteral("debug displayfps 待 mw_display 接入（gRPC 帧泵模块转换中）"));
}

static mw::Result<bool> debugSnap(Args& args)
{
    Q_UNUSED(args);
    // TODO(mw_display): GrpcDisplayBackend::snapshot_rgba + PNG 写入
    return mw::err<bool>(QStringLiteral("debug snap 待 mw_display 接入（gRPC 显示模块转换中）"));
}

static mw::Result<bool> debugEmuhide(Args& args)
{
    const auto action = args.take();
    if (!action || (*action != QStringLiteral("hide") && *action != QStringLiteral("show")))
        return badUsage<bool>(QStringLiteral("Usage: mwine debug emuhide <hide|show> [--delay-ms <n>]"));
    quint64 delayMs = 0;
    if (const auto d = args.opt("delay-ms"))
        delayMs = d->toULongLong();
    emuhideStandalone(*action == QStringLiteral("hide"), delayMs);
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> debugDisplays(Args& args)
{
    const std::optional<QString> serialOpt = args.opt("serial");
    const std::optional<QString> delStr = args.opt("del");
    if (delStr) {
        // 删除指定模拟器侧显示器（主屏 0 不可删）——console 通道可用
        bool ok = false;
        const quint32 id = delStr->toUInt(&ok);
        if (!ok || id == 0)
            return mw::err<bool>(QStringLiteral("主屏（display 0）不可删除"));
        const QString serial = debugSerial(serialOpt);
        if (auto r = mw::supervisor::multidisplayDel(serial, id); !r)
            return mw::err<bool>(QStringLiteral("删除显示器失败: %1").arg(r.error()));
        std::printf("✓ 已删除显示器 %u\n", id);
        return mw::Result<bool>::Ok();
    }
    // TODO(mw_display): 列表需 gRPC list_displays（mw_display 模块转换中）。
    std::fprintf(stderr, "⚠ 显示器列表待 mw_display 接入（gRPC list_displays）；"
                         "孤儿副屏清理可用 --del <id>\n");
    Q_UNUSED(serialOpt);
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> debugSetClip(Args& args)
{
    // mwine debug setclip [--serial s] <text>
    // 走 scrcpy 控制通道 SetClipboard：微流会话（maxSize=1）只为满足
    // server 的 video 约束，发完剪贴板立即断开。
    const QString text = args.take().value_or(QString());
    const QString serial = debugSerial(args.opt("serial"));
    const QString jar = mw::scrcpy::findServerJar();
    if (jar.isEmpty())
        return mw::err<bool>(QStringLiteral("scrcpy-server 未找到（~/.mwine/bin/scrcpy-server-v4.1）"));

    mw::scrcpy::ServerOptions so;
    so.video = true;
    so.audio = false;
    so.control = true;
    so.maxSize = 1; // 微流：最小化编码开销
    so.videoBitRate = 800000;
    so.displayId = 0;
    so.keepActive = false;

    auto client = adbClient();
    mw::scrcpy::ScrcpyClient sc{ client, serial, jar };
    sc.options = so;
    auto sessionR = std::move(sc).start();
    if (!sessionR)
        return mw::err<bool>(sessionR.error());
    auto& session = sessionR.value();
    if (!session.control)
        return mw::err<bool>(QStringLiteral("控制通道未建立"));
    auto sent = session.control->send(
        mw::scrcpy::ControlMessage::setClipboard(1, false, text));
    if (!sent.ok()) {
        session.stop();
        return mw::err<bool>(QStringLiteral("发送剪贴板失败: %1").arg(sent.error()));
    }
    // 读回确认：GetClipboard → 设备回 Clipboard 消息（1s 窗口内轮询；
    // binder 往返有毫秒级延迟，单次 0ms 探测会漏）
    QString readBack;
    if (session.control->send(mw::scrcpy::ControlMessage::getClipboard(
                                  mw::scrcpy::control::COPY_KEY_NONE))
            .ok()) {
        for (int i = 0; i < 10 && readBack.isEmpty(); ++i) {
            if (auto reply = session.control->tryReadDeviceMessage(); reply && reply.value()
                && reply.value()->kind == mw::scrcpy::DeviceMessage::Kind::Clipboard) {
                readBack = reply.value()->text;
                break;
            }
            QThread::msleep(100);
        }
    }
    session.stop();
    if (!readBack.isEmpty() || text.isEmpty()) {
        std::printf("✓ 设备剪贴板已设置并读回确认（%lld 字符）\n", (long long)text.size());
        return mw::Result<bool>::Ok();
    }
    return mw::err<bool>(QStringLiteral("已发送但读回为空（server 未确认，剪贴板可能未生效）"));
}

mw::Result<bool> cmdDebug(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine debug <displayfps|snap|emuhide|displays|setclip> […]"));
    if (*action == QStringLiteral("displayfps"))
        return debugDisplayFps(args);
    if (*action == QStringLiteral("snap"))
        return debugSnap(args);
    if (*action == QStringLiteral("emuhide"))
        return debugEmuhide(args);
    if (*action == QStringLiteral("displays"))
        return debugDisplays(args);
    if (*action == QStringLiteral("setclip"))
        return debugSetClip(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

// ===================== app（.app 壳 / APK 双击安装器） =====================

static mw::Result<bool> appCreate(Args& args)
{
    std::optional<QString> package = args.opt("package");
    // 兼容位置参数形态：mwine app create <包名>
    if (!package)
        if (const auto p = args.take())
            package = p;
    const std::optional<QString> apk = args.opt("apk");
    if (!package && !apk)
        return badUsage<bool>(
            QStringLiteral("Usage: mwine app create <package> | --apk <path> [--name <n>] "
                           "[--out <dir>] [--avd <name>] [--no-install] [--print-path]"));
    const std::optional<QString> name = args.opt("name");
    const std::optional<QString> out = args.opt("out");
    const std::optional<QString> avd = args.opt("avd");
    const bool noInstall = args.has("no-install");
    const bool printPath = args.has("print-path");

    auto client = adbClient();
    client.ensureServer();
    // 设备目标与 mwine install 同链：daemon VM 优先、无在线时自动拉起、多台绝不猜
    std::optional<QString> onlineSerial;
    if (noInstall) {
        // 壳生成模式（install 刚装完）：尽力拿设备做图标渲染，拿不到不报错
        if (const auto s = resolveInstallSerial(&client, std::nullopt); s)
            onlineSerial = s.value();
    } else {
        const auto s = ensureInstallTarget(&client, [](const QString&) {});
        if (!s)
            return mw::err<bool>(s.error());
        onlineSerial = s.value();
    }

    mw::app::CreateAppShellOptions opts;
    opts.package = package.value_or(QString());
    opts.apkPath = apk.value_or(QString());
    opts.name = name.value_or(QString());
    opts.outDir = out.value_or(QString());
    opts.avd = avd.value_or(QString());
    opts.noInstall = noInstall;
    opts.serial = onlineSerial.value_or(QString());
    opts.printPath = printPath;
    const auto r = mw::app::createAppShell(opts);
    if (!r)
        return mw::err<bool>(r.error());
    const auto res = r.value();
    if (printPath)
        std::printf("%s\n", qPrintable(res.appDir));
    else {
        std::printf("✓ 已生成 %s\n", qPrintable(res.appDir));
        std::printf("  Finder 双击即启动应用窗口；Launchpad 稍候收录（也可拖入 Dock）\n");
    }
    return mw::Result<bool>::Ok();
}

static mw::Result<bool> appInstaller(Args& args)
{
    const std::optional<QString> out = args.opt("out");
    QString appDir;
    // 生成 APK 双击安装器 .app 并注册为 .apk 默认打开方式
    //（osacompile applet + install.sh + 进度窗口 + Info.plist + codesign + lsregister +
    //  LSSetDefaultRoleHandlerForContentType —— mw::app::createApkInstaller 全链实现）
    if (auto r = mw::app::createApkInstaller(out.value_or(QString()), &appDir); !r)
        return mw::err<bool>(r.error());
    std::printf("✓ 已生成 %s\n", qPrintable(appDir));
    std::printf("  之后在 Finder 双击任意 .apk：自动安装 -> 生成应用 .app -> 自动启动\n");
    return mw::Result<bool>::Ok();
}

mw::Result<bool> cmdApp(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(QStringLiteral("Usage: mwine app <create|installer> […]"));
    if (*action == QStringLiteral("create"))
        return appCreate(args);
    if (*action == QStringLiteral("installer"))
        return appInstaller(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

} // namespace mwine
