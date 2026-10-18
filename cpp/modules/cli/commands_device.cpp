// mwine device 子命令：真机连接（列表 / 扫码或配对码配对 / 连接 / 断开 /
// USB 转无线 / 忘记）。与 UI 同源，统一走 mwine-cored 的 /api/devices*。
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>

#include "cli_common.hpp"
#include "commands.hpp"

namespace mwine {

namespace {

// qrcode 子命令的 Ctrl-C 中断标记（signal handler 只置位）
volatile sig_atomic_t g_qrPairInterrupted = 0;

// REST 调用 + 非 200 → 打印并按 capture 命令同惯例 exit(1)
struct ApiOk {
    int code = 0;
    QJsonObject obj;
    QJsonArray arr;
    QString raw;
};

bool dieOnHttp(const mw::Result<QPair<int, QString>>& r, const char* what)
{
    if (!r) {
        std::fprintf(stderr, "✗ %s失败：%s（mwine-cored 未运行？先启动 Houttuynia 应用）\n",
                     what, qPrintable(r.error()));
        ::exit(1);
    }
    if (r.value().first != 200) {
        std::fprintf(stderr, "✗ %s失败（%d）：%s\n", what, r.value().first,
                     qPrintable(r.value().second));
        ::exit(1);
    }
    return true;
}

QJsonObject parseObj(const QString& text)
{
    const auto doc = QJsonDocument::fromJson(text.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject();
}

mw::Result<bool> deviceList()
{
    const auto r = coreApi(QStringLiteral("GET"), QStringLiteral("/api/devices"));
    if (!dieOnHttp(r, "列出设备"))
        return mw::Result<bool>::Ok();
    const QJsonObject obj = parseObj(r.value().second);
    const QJsonArray devices = obj.value(QLatin1String("devices")).toArray();
    if (devices.isEmpty()) {
        std::printf("暂无真机在线。USB 插线即用；无线可 `mwine device pair`。\n");
        return mw::Result<bool>::Ok();
    }
    std::printf("%-28s %-6s %-14s %-10s %s\n", "SERIAL", "传输", "型号", "ANDROID",
                "状态");
    for (const auto& v : devices) {
        const QJsonObject d = v.toObject();
        const QString transport = d.value(QLatin1String("transport")).toString();
        std::printf("%-28s %-6s %-14s %-10s %s\n",
                    qPrintable(d.value(QLatin1String("serial")).toString()),
                    qPrintable(transport == QLatin1String("wifi") ? "wifi" : "usb"),
                    qPrintable(d.value(QLatin1String("model")).toString()),
                    qPrintable(d.value(QLatin1String("android")).toString()),
                    qPrintable(d.value(QLatin1String("state")).toString()));
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> deviceDiscovered()
{
    const auto r = coreApi(QStringLiteral("GET"), QStringLiteral("/api/devices/discovered"));
    if (!dieOnHttp(r, "mDNS 发现"))
        return mw::Result<bool>::Ok();
    const QJsonArray services = parseObj(r.value().second)
                                    .value(QLatin1String("services"))
                                    .toArray();
    if (services.isEmpty()) {
        std::printf("未发现 adb mDNS 服务（手机打开「无线调试 → 使用配对码配对设备」后重试）\n");
        return mw::Result<bool>::Ok();
    }
    for (const auto& v : services) {
        const QJsonObject s = v.toObject();
        const QString type = s.value(QLatin1String("type")).toString();
        std::printf("%-34s %-24s %s\n", qPrintable(s.value(QLatin1String("instance")).toString()),
                    qPrintable(type), qPrintable(s.value(QLatin1String("addr")).toString()));
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> devicePair(Args& args)
{
    // mwine device pair --qr "WIFI:T:ADB;S:…;P:…;;"
    // mwine device pair --code 482615 [--addr 192.168.1.42:37831 | --service 实例名]
    QJsonObject body;
    if (const auto qr = args.opt("qr")) {
        body[QStringLiteral("qr")] = *qr;
    } else if (const auto code = args.opt("code")) {
        body[QStringLiteral("code")] = *code;
        if (const auto addr = args.opt("addr"))
            body[QStringLiteral("addr")] = *addr;
        if (const auto service = args.opt("service"))
            body[QStringLiteral("service")] = *service;
    } else {
        return badUsage<bool>(QStringLiteral(
            "Usage: mwine device pair --qr <二维码内容> | --code <配对码> [--addr ip:端口 | --service 服务名]"));
    }
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/devices/pair"),
                           QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!dieOnHttp(r, "配对"))
        return mw::Result<bool>::Ok();
    const QJsonObject obj = parseObj(r.value().second);
    std::printf("✓ 配对成功：%s\n",
                qPrintable(obj.value(QLatin1String("message")).toString()));
    if (obj.value(QLatin1String("connected")).toBool()) {
        const QJsonObject d = obj.value(QLatin1String("device")).toObject();
        std::printf("✓ 已连接 %s（%s，Android %s）\n",
                    qPrintable(obj.value(QLatin1String("addr")).toString()),
                    qPrintable(d.value(QLatin1String("model")).toString()),
                    qPrintable(d.value(QLatin1String("android")).toString()));
        std::printf("  投屏：mwine mirror --serial %s\n",
                    qPrintable(obj.value(QLatin1String("addr")).toString()));
    } else {
        std::printf("  稍后连接：mwine device connect <ip:连接端口>\n");
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> deviceConnect(Args& args)
{
    const auto addr = args.take();
    if (!addr || addr->isEmpty())
        return badUsage<bool>(QStringLiteral("Usage: mwine device connect <ip:端口>"));
    const QByteArray body = QJsonDocument(QJsonObject{ { QStringLiteral("addr"), *addr } })
                                .toJson(QJsonDocument::Compact);
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/devices/connect"), body);
    if (!dieOnHttp(r, "连接"))
        return mw::Result<bool>::Ok();
    const QJsonObject d = parseObj(r.value().second);
    std::printf("✓ 已连接 %s（%s，Android %s）\n", qPrintable(*addr),
                qPrintable(d.value(QLatin1String("model")).toString()),
                qPrintable(d.value(QLatin1String("android")).toString()));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> deviceDisconnect(Args& args)
{
    const auto serial = args.take();
    if (!serial || serial->isEmpty())
        return badUsage<bool>(QStringLiteral("Usage: mwine device disconnect <serial>"));
    const QByteArray body = QJsonDocument(QJsonObject{ { QStringLiteral("serial"), *serial } })
                                .toJson(QJsonDocument::Compact);
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/devices/disconnect"), body);
    if (!dieOnHttp(r, "断开"))
        return mw::Result<bool>::Ok();
    std::printf("✓ 已断开 %s\n", qPrintable(*serial));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> deviceForget(Args& args)
{
    const auto serial = args.take();
    if (!serial || serial->isEmpty())
        return badUsage<bool>(QStringLiteral("Usage: mwine device forget <serial>"));
    const QByteArray body = QJsonDocument(QJsonObject{ { QStringLiteral("serial"), *serial } })
                                .toJson(QJsonDocument::Compact);
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/devices/forget"), body);
    if (!dieOnHttp(r, "忘记设备"))
        return mw::Result<bool>::Ok();
    std::printf("✓ 已忘记 %s\n", qPrintable(*serial));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> deviceTcpip(Args& args)
{
    // mwine device tcpip [--serial s] [--port 5555]：USB 一键转无线
    const auto serialOpt = args.opt("serial");
    QString serial;
    if (serialOpt.has_value()) {
        serial = *serialOpt;
    } else {
        // 默认取唯一在线 USB 真机
        const auto devs = adbClient().devices();
        if (devs.ok()) {
            int found = 0;
            for (const auto& d : devs.value()) {
                if (d.state == QLatin1String("device")
                    && !d.serial.startsWith(QLatin1String("emulator-"))
                    && !d.serial.contains(QLatin1Char(':'))) {
                    serial = d.serial;
                    ++found;
                }
            }
            if (found > 1)
                return badUsage<bool>(QStringLiteral(
                    "多台 USB 设备在线，请用 --serial 指定"));
            if (found == 0)
                return badUsage<bool>(QStringLiteral("未发现在线 USB 真机（请插线并授权调试）"));
        }
    }
    if (serial.isEmpty())
        return badUsage<bool>(QStringLiteral("未发现在线 USB 真机（请插线并授权调试）"));
    const quint16 port = static_cast<quint16>(args.opt("port").value_or(
                                                   QStringLiteral("5555"))
                                                   .toUShort());
    const QByteArray body = QJsonDocument(
        QJsonObject{ { QStringLiteral("serial"), serial },
                      { QStringLiteral("port"), static_cast<double>(port) } })
                                .toJson(QJsonDocument::Compact);
    std::printf("… USB 转无线：%s（切换中，约 3~5 秒）\n", qPrintable(serial));
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/devices/tcpip"), body);
    if (!dieOnHttp(r, "USB 转无线"))
        return mw::Result<bool>::Ok();
    const QJsonObject d = parseObj(r.value().second);
    std::printf("✓ 已转无线并连接 %s\n",
                qPrintable(d.value(QLatin1String("serial")).toString()));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> deviceMirror(Args& args)
{
    const auto serial = args.take();
    if (!serial || serial->isEmpty())
        return badUsage<bool>(QStringLiteral("Usage: mwine device mirror <serial>"));
    const QByteArray body = QJsonDocument(QJsonObject{ { QStringLiteral("serial"), *serial } })
                                .toJson(QJsonDocument::Compact);
    const auto r = coreApi(QStringLiteral("POST"), QStringLiteral("/api/devices/mirror"), body);
    if (!dieOnHttp(r, "投屏"))
        return mw::Result<bool>::Ok();
    std::printf("✓ 投屏窗口已拉起（%s）\n", qPrintable(*serial));
    return mw::Result<bool>::Ok();
}

// 出码配对（Android Studio 同款）：daemon 生成 WIFI:T:ADB 二维码会话，
// 终端打印 ASCII 二维码 + 说明，轮询状态直到配对完成 / 失败 / 超时。
mw::Result<bool> deviceQrCode(Args& args)
{
    // mwine device qrcode [--timeout 180]
    const int timeoutSec = args.opt("timeout").value_or(QStringLiteral("180")).toInt();
    const QByteArray beginBody = QJsonDocument(
        QJsonObject{ { QStringLiteral("wait_ms"), timeoutSec * 1000 } })
                                    .toJson(QJsonDocument::Compact);
    const auto r = coreApi(QStringLiteral("POST"),
                           QStringLiteral("/api/devices/hostpair/begin"), beginBody);
    if (!dieOnHttp(r, "开始出码配对"))
        return mw::Result<bool>::Ok();
    const QJsonObject obj = parseObj(r.value().second);
    const QString qr = obj.value(QLatin1String("qr")).toString();
    const QJsonArray rows = obj.value(QLatin1String("rows")).toArray();
    if (qr.isEmpty())
        return badUsage<bool>(QStringLiteral("守护返回的会话缺少二维码载荷"));

    std::printf("用手机扫码配对（Android 11+，与 Mac 同一 Wi-Fi）：\n"
                "  设置 → 开发者选项 → 无线调试 → 使用二维码配对设备\n\n");
    // ASCII 二维码（每模块两字符保持近似方形；四周留 4 模块静区）
    if (!rows.isEmpty()) {
        const int n = rows.size(), quiet = 4;
        const QString blank((n + quiet * 2) * 2, QLatin1Char(' '));
        for (int i = 0; i < quiet; ++i)
            std::printf("%s\n", qPrintable(blank));
        for (const auto& v : rows) {
            QString line(quiet * 2, QLatin1Char(' '));
            for (const QChar c : v.toString())
                line += c == QLatin1Char('1') ? QStringLiteral("██")
                                              : QStringLiteral("  ");
            line += QString(quiet * 2, QLatin1Char(' '));
            std::printf("%s\n", qPrintable(line));
        }
        for (int i = 0; i < quiet; ++i)
            std::printf("%s\n", qPrintable(blank));
    } else {
        std::printf("（守护未提供二维码矩阵；payload：%s）\n", qPrintable(qr));
    }
    std::printf("等待手机扫码…（%d 秒内有效，Ctrl-C 取消）\n", timeoutSec);
    std::fflush(stdout);

    // 轮询状态；终态打印结果。Ctrl-C → 通知守护取消等待后退出。
    std::signal(SIGINT, [](int) { g_qrPairInterrupted = 1; });
    QString lastState;
    for (;;) {
        for (int i = 0; i < 10 && g_qrPairInterrupted == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_qrPairInterrupted) {
            (void)coreApi(QStringLiteral("POST"),
                          QStringLiteral("/api/devices/hostpair/cancel"));
            std::fprintf(stderr, "✗ 已取消\n");
            ::exit(1);
        }
        const auto s = coreApi(QStringLiteral("GET"),
                               QStringLiteral("/api/devices/hostpair"));
        if (!s || s.value().first != 200) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue; // 状态查询瞬时失败不致命
        }
        const QJsonObject st = parseObj(s.value().second);
        const QString state = st.value(QLatin1String("state")).toString();
        if (state != lastState && !state.isEmpty()) {
            std::printf("… %s\n", qPrintable(st.value(QLatin1String("message")).toString()));
            std::fflush(stdout);
            lastState = state;
        }
        if (state == QLatin1String("done")) {
            const QJsonObject res = st.value(QLatin1String("result")).toObject();
            std::printf("✓ 配对成功：%s\n",
                        qPrintable(res.value(QLatin1String("message")).toString()));
            if (st.value(QLatin1String("connected")).toBool()) {
                const QJsonObject d = res.value(QLatin1String("device")).toObject();
                std::printf("✓ 已连接 %s（%s，Android %s）\n",
                            qPrintable(res.value(QLatin1String("addr")).toString()),
                            qPrintable(d.value(QLatin1String("model")).toString()),
                            qPrintable(d.value(QLatin1String("android")).toString()));
                std::printf("  投屏：mwine mirror --serial %s\n",
                            qPrintable(res.value(QLatin1String("addr")).toString()));
            }
            return mw::Result<bool>::Ok();
        }
        if (state == QLatin1String("failed") || state == QLatin1String("cancelled")) {
            std::fprintf(stderr, "✗ %s\n",
                         qPrintable(st.value(QLatin1String("message")).toString()));
            ::exit(1);
        }
    }
}

} // namespace

mw::Result<bool> cmdDevice(Args& args)
{
    const auto action = args.take();
    if (!action)
        return badUsage<bool>(QStringLiteral(
            "Usage: mwine device <list|discovered|pair|qrcode|connect|disconnect|tcpip|forget|mirror> […]"));
    if (*action == QStringLiteral("list"))
        return deviceList();
    if (*action == QStringLiteral("discovered"))
        return deviceDiscovered();
    if (*action == QStringLiteral("pair"))
        return devicePair(args);
    if (*action == QStringLiteral("qrcode"))
        return deviceQrCode(args);
    if (*action == QStringLiteral("connect"))
        return deviceConnect(args);
    if (*action == QStringLiteral("disconnect"))
        return deviceDisconnect(args);
    if (*action == QStringLiteral("tcpip"))
        return deviceTcpip(args);
    if (*action == QStringLiteral("forget"))
        return deviceForget(args);
    if (*action == QStringLiteral("mirror"))
        return deviceMirror(args);
    return badUsage<bool>(QStringLiteral("unrecognized subcommand '%1'").arg(*action));
}

} // namespace mwine
