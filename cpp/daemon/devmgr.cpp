#include "devmgr.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "adb.hpp" // mw::adb（client + 真机配对/发现辅助）
#include "fs.hpp"
#include "qrmat.hpp"

namespace mw::dev {

namespace {

using Clock = std::chrono::steady_clock;
void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// server 模式客户端（真机通道；独立于 AppState 的 direct 客户端）
adb::AdbClient srvClient()
{
    return adb::AdbClient::serverMode();
}

// adb server 就绪（不在线则用捆绑二进制拉起）。
// 自动回连（同 Wi-Fi 发现已配对设备即 connect）只注入给"由本守护拉起"的
// adb server——start-server 子进程在启动瞬间读走环境变量；窗口外立即撤除，
// 绝不进守护全局环境（qemu/CLI 等子进程不携带，既有 server 语义不受影响）。
mw::Result<bool> ensureSrv()
{
    qputenv("ADB_MDNS_AUTO_CONNECT", QByteArrayLiteral("_adb-tls-connect._tcp"));
    const auto r = srvClient().ensureServer(adb::findAdbBinary());
    qunsetenv("ADB_MDNS_AUTO_CONNECT");
    if (!r.ok())
        return mw::err<bool>(QStringLiteral(
            "adb server 启动失败（运行时未安装？可到「镜像」页补装运行时）: %1").arg(r.error()));
    return r;
}

// serial 是否无线形态（ip:port）
bool isWifiSerial(const QString& serial)
{
    return adb::looksLikeAddr(serial);
}

} // namespace

QString DeviceManager::storePath() const
{
    return QDir(mw::mwineDir()).filePath(QStringLiteral("devices.json"));
}

void DeviceManager::load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray())
        return;
    for (const auto& v : doc.array()) {
        const QJsonObject o = v.toObject();
        KnownDevice d;
        d.key = o.value(QLatin1String("key")).toString();
        d.model = o.value(QLatin1String("model")).toString();
        d.brand = o.value(QLatin1String("brand")).toString();
        d.android = o.value(QLatin1String("android")).toString();
        d.lastAddr = o.value(QLatin1String("last_addr")).toString();
        d.lastSeen = static_cast<qint64>(o.value(QLatin1String("last_seen")).toDouble());
        if (!d.key.isEmpty())
            m_known.insert(d.key, d);
    }
}

void DeviceManager::saveLocked()
{
    QJsonArray arr;
    for (const auto& d : m_known) {
        arr.append(QJsonObject{
            { QStringLiteral("key"), d.key },
            { QStringLiteral("model"), d.model },
            { QStringLiteral("brand"), d.brand },
            { QStringLiteral("android"), d.android },
            { QStringLiteral("last_addr"), d.lastAddr },
            { QStringLiteral("last_seen"), static_cast<double>(d.lastSeen) },
        });
    }
    QDir().mkpath(mw::mwineDir());
    QFile f(storePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QJsonObject DeviceManager::deviceInfo(const QString& serial)
{
    {
        QMutexLocker l(&m_mu);
        const auto it = m_infoCache.constFind(serial);
        if (it != m_infoCache.constEnd()
            && it.value().first > QDateTime::currentMSecsSinceEpoch() - 5 * 60 * 1000)
            return it.value().second;
    }
    QJsonObject info;
    const auto out = srvClient().shellBlocking(
        serial,
        QStringLiteral(
            "getprop ro.product.model; getprop ro.product.brand; "
            "getprop ro.build.version.release; getprop ro.build.version.sdk; "
            "getprop ro.serialno"),
        8000);
    if (out.ok() && out.value().ok()) {
        const QStringList lines =
            QString::fromUtf8(out.value().out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        auto lineAt = [&lines](int i) -> QString {
            return lines.size() > i ? lines.at(i).trimmed() : QString();
        };
        info[QStringLiteral("model")] = lineAt(0);
        info[QStringLiteral("brand")] = lineAt(1);
        info[QStringLiteral("android")] = lineAt(2);
        info[QStringLiteral("sdk")] = lineAt(3);
        info[QStringLiteral("key")] = lineAt(4).isEmpty() ? serial : lineAt(4);
    }
    QMutexLocker l(&m_mu);
    m_infoCache.insert(serial, qMakePair(QDateTime::currentMSecsSinceEpoch(), info));
    return info;
}

bool DeviceManager::waitOnline(const QString& serial, int timeoutMs)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto devs = srvClient().devices();
        if (devs.ok()) {
            for (const auto& d : devs.value()) {
                if (d.serial == serial && d.state == QLatin1String("device"))
                    return true;
            }
        }
        if (Clock::now() > deadline)
            return false;
        sleepMs(400);
    }
}

mw::Result<QJsonArray> DeviceManager::listDevices()
{
    const auto ok = ensureSrv();
    if (!ok.ok())
        return mw::err<QJsonArray>(ok.error());
    const auto devs = srvClient().devices();
    if (!devs.ok())
        return mw::err<QJsonArray>(devs.error());
    load();
    QJsonArray out;
    for (const auto& d : devs.value()) {
        if (d.serial.startsWith(QLatin1String("emulator-")))
            continue;
        const QJsonObject info = d.state == QLatin1String("device")
                                     ? deviceInfo(d.serial)
                                     : QJsonObject();
        const QString key = info.value(QLatin1String("key")).toString(d.serial);
        QMutexLocker l(&m_mu);
        const auto known = m_known.value(key);
        QJsonObject o;
        o[QStringLiteral("serial")] = d.serial;
        o[QStringLiteral("state")] = d.state;
        o[QStringLiteral("transport")] =
            isWifiSerial(d.serial) ? QStringLiteral("wifi") : QStringLiteral("usb");
        o[QStringLiteral("model")] = info.value(QLatin1String("model")).toString(known.model);
        o[QStringLiteral("brand")] = info.value(QLatin1String("brand")).toString(known.brand);
        o[QStringLiteral("android")] =
            info.value(QLatin1String("android")).toString(known.android);
        o[QStringLiteral("known")] = m_known.contains(key);
        l.unlock();
        out.append(o);
    }
    return mw::Result<QJsonArray>::Ok(out);
}

mw::Result<QJsonArray> DeviceManager::discovered()
{
    const auto ok = ensureSrv();
    if (!ok.ok())
        return mw::err<QJsonArray>(ok.error());
    const auto services = adb::discoverMdnsServices(adb::findAdbBinary());
    if (!services.ok())
        return mw::err<QJsonArray>(services.error());
    QJsonArray out;
    for (const auto& s : services.value()) {
        if (!s.isPairing() && !s.isConnect())
            continue;
        QJsonObject o;
        o[QStringLiteral("instance")] = s.instance;
        o[QStringLiteral("type")] = s.type;
        o[QStringLiteral("addr")] = s.addr();
        out.append(o);
    }
    return mw::Result<QJsonArray>::Ok(out);
}

mw::Result<QString> DeviceManager::connectAfterPair(const QVector<QString>& beforeConnectAddrs)
{
    // 双通道等待连接点：mDNS 新增 connect 服务 / adb 已自动连上（auto-connect）
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    for (;;) {
        const auto services = adb::discoverMdnsServices(adb::findAdbBinary());
        if (services.ok()) {
            for (const auto& s : services.value()) {
                if (!s.isConnect())
                    continue;
                if (beforeConnectAddrs.contains(s.addr()))
                    continue; // 配对前就在的服务（别的设备）
                const auto conn = srvClient().connectNetwork(s.addr());
                if (conn.ok() && waitOnline(s.addr(), 5000))
                    return mw::Result<QString>::Ok(s.addr());
            }
        }
        // auto-connect 可能已直连（服务名不新增，设备直接上线）
        const auto devs = srvClient().devices();
        if (devs.ok()) {
            for (const auto& d : devs.value()) {
                if (isWifiSerial(d.serial) && d.state == QLatin1String("device")
                    && !beforeConnectAddrs.contains(d.serial))
                    return mw::Result<QString>::Ok(d.serial);
            }
        }
        if (Clock::now() > deadline)
            break;
        sleepMs(700);
    }
    return mw::err<QString>(QStringLiteral(
        "配对成功，但未发现可连接端口（请确认手机停留在无线调试页，稍后可手动连接）"));
}

mw::Result<QJsonObject> DeviceManager::pair(const QJsonObject& req)
{
    const auto okSrv = ensureSrv();
    if (!okSrv.ok())
        return mw::err<QJsonObject>(okSrv.error());

    QString code, service, addr;
    const QString qr = req.value(QLatin1String("qr")).toString().trimmed();
    if (!qr.isEmpty()) {
        const auto parsed = adb::parsePairQr(qr);
        if (!parsed.ok())
            return mw::err<QJsonObject>(parsed.error());
        code = parsed.value().password;
        service = parsed.value().service;
        addr = parsed.value().addr;
    } else {
        code = req.value(QLatin1String("code")).toString().trimmed();
        addr = req.value(QLatin1String("addr")).toString().trimmed();
        service = req.value(QLatin1String("service")).toString().trimmed();
    }
    if (code.isEmpty())
        return mw::err<QJsonObject>(QStringLiteral("缺少配对码"));
    if (addr.isEmpty() && service.isEmpty())
        return mw::err<QJsonObject>(
            QStringLiteral("缺少配对地址（mDNS 未发现配对服务时可手输 ip:配对端口）"));

    // 配对前 connect 服务快照（用于识别配对后新增的连接端口）
    QVector<QString> beforeConnectAddrs;
    {
        const auto services = adb::discoverMdnsServices(adb::findAdbBinary());
        if (services.ok())
            for (const auto& s : services.value())
                if (s.isConnect())
                    beforeConnectAddrs.append(s.addr());
    }

    // 地址解析：显式地址 > 服务实例名 > 唯一配对服务自动选择
    QString pairAddr = addr;
    QString pairService;
    if (pairAddr.isEmpty()) {
        const auto services = adb::discoverMdnsServices(adb::findAdbBinary());
        if (!services.ok())
            return mw::err<QJsonObject>(services.error());
        QVector<adb::MdnsService> pairing;
        for (const auto& s : services.value())
            if (s.isPairing())
                pairing.append(s);
        if (!service.isEmpty()) {
            const auto it = std::find_if(
                pairing.cbegin(), pairing.cend(),
                [&](const adb::MdnsService& s) { return s.instance == service; });
            if (it == pairing.cend())
                return mw::err<QJsonObject>(
                    QStringLiteral("mDNS 中未发现配对服务「%1」（手机配对弹窗是否已关闭？）")
                        .arg(service));
            pairAddr = it->addr();
            pairService = it->instance;
        } else if (pairing.size() == 1) {
            pairAddr = pairing.at(0).addr();
            pairService = pairing.at(0).instance;
        } else if (pairing.isEmpty()) {
            return mw::err<QJsonObject>(
                QStringLiteral("未发现配对服务：请确认手机已打开「无线调试 → 使用配对码配对设备」，"
                               "或直接手输弹窗显示的 IP:端口"));
        } else {
            QStringList names;
            for (const auto& s : pairing)
                names << s.instance;
            return mw::err<QJsonObject>(QStringLiteral("发现 %1 个配对服务，请指定其一: %2")
                                            .arg(pairing.size())
                                            .arg(names.join(QStringLiteral(", "))));
        }
    }

    const auto paired = adb::runPair(adb::findAdbBinary(), pairAddr, code);
    if (!paired.ok())
        return mw::err<QJsonObject>(paired.error());

    // 自动连接
    const auto connected = connectAfterPair(beforeConnectAddrs);
    return mw::Result<QJsonObject>::Ok(
        pairOutcome(connected.ok(), connected.ok() ? connected.value() : QString(),
                    pairAddr, pairService, paired.value()));
}

QJsonObject DeviceManager::pairOutcome(bool connected, const QString& connectAddr,
                                       const QString& pairAddr, const QString& pairService,
                                       const QString& message)
{
    if (connected)
        upsertKnown(connectAddr, deviceInfo(connectAddr));
    QJsonObject result;
    result[QStringLiteral("paired")] = true;
    result[QStringLiteral("pair_addr")] = pairAddr;
    if (!pairService.isEmpty())
        result[QStringLiteral("pair_service")] = pairService;
    result[QStringLiteral("message")] = message;
    result[QStringLiteral("connected")] = connected;
    if (connected) {
        result[QStringLiteral("addr")] = connectAddr;
        const auto list = listDevices();
        if (list.ok()) {
            for (const auto& v : list.value()) {
                const QJsonObject o = v.toObject();
                if (o.value(QLatin1String("serial")).toString() == connectAddr) {
                    result[QStringLiteral("device")] = o;
                    break;
                }
            }
        }
    }
    return result;
}

// ===== 出码配对（Android Studio 同款方向）=====

DeviceManager::~DeviceManager()
{
    // 通知后台线程退出并收尸（子进程阶段不可中断，最长阻塞 ~30s）
    m_hostCancel = true;
    if (m_hostThread.joinable())
        m_hostThread.join();
}

mw::Result<QJsonObject> DeviceManager::hostPairBegin(int waitMs)
{
    // 全局单会话：原子占位防并发 begin 双起线程
    bool expected = false;
    if (!m_hostActive.compare_exchange_strong(expected, true))
        return mw::Result<QJsonObject>::Ok(hostPairStatus()); // 进行中：返回当前会话
    const auto release = [this]() { m_hostActive = false; };

    const auto okSrv = ensureSrv();
    if (!okSrv.ok()) {
        release();
        return mw::err<QJsonObject>(okSrv.error());
    }
    if (m_hostThread.joinable())
        m_hostThread.join(); // 上一会话已终态，收尸后开新会话

    // 配对前 connect 服务快照（用于识别配对后新增的连接端口）
    QVector<QString> beforeConnectAddrs;
    {
        const auto services = adb::discoverMdnsServices(adb::findAdbBinary());
        if (services.ok())
            for (const auto& s : services.value())
                if (s.isConnect())
                    beforeConnectAddrs.append(s.addr());
    }

    const adb::HostPairSession session = adb::makeHostPairSession();
    QJsonArray rows;
    for (const QString& r : adb::qrMatrixRows(session.qrPayload))
        rows.append(r);
    QJsonObject sj;
    sj[QStringLiteral("service")] = session.service;
    sj[QStringLiteral("qr")] = session.qrPayload;
    sj[QStringLiteral("rows")] = rows;
    QJsonObject st = sj;
    st[QStringLiteral("state")] = QStringLiteral("waiting");
    st[QStringLiteral("message")] = QStringLiteral("等待手机扫码…");
    {
        QMutexLocker l(&m_hostMu);
        m_hostStatus = st;
    }
    m_hostCancel = false;
    m_hostThread = std::thread(&DeviceManager::hostPairWorker, this, session, rows,
                               beforeConnectAddrs, waitMs);
    return mw::Result<QJsonObject>::Ok(st);
}

QJsonObject DeviceManager::hostPairStatus()
{
    QMutexLocker l(&m_hostMu);
    return m_hostStatus;
}

bool DeviceManager::hostPairCancel()
{
    QMutexLocker l(&m_hostMu);
    if (m_hostStatus.value(QLatin1String("state")).toString()
        != QLatin1String("waiting"))
        return false; // 配对/连接中顺其自然完成；终态无需取消
    m_hostCancel = true;
    return true;
}

void DeviceManager::hostPairWorker(adb::HostPairSession session, QJsonArray rows,
                                   QVector<QString> beforeConnectAddrs, int waitMs)
{
    // stdio 已被守护重定向到 ~/.mwine/logs/daemon.log；[hostpair] 前缀便于排查
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed = [&t0]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
    };
    const QString adbBin = adb::findAdbBinary();
    std::fprintf(stderr, "[hostpair] 会话开始 %s（等待扫码 %dms）\n",
                 qPrintable(session.service), waitMs);
    auto withState = [this, &session, &rows](const char* state, const QString& msg,
                                             const QJsonObject& extra = QJsonObject()) {
        QJsonObject st;
        st[QStringLiteral("service")] = session.service;
        st[QStringLiteral("qr")] = session.qrPayload;
        st[QStringLiteral("rows")] = rows;
        st[QStringLiteral("state")] = QLatin1String(state);
        st[QStringLiteral("message")] = msg;
        for (auto it = extra.begin(); it != extra.end(); ++it)
            st[it.key()] = it.value();
        QMutexLocker l(&m_hostMu);
        m_hostStatus = st;
    };

    // 1) 等手机扫码：扫码后手机以 S 为实例名通告 _adb-tls-pairing._tcp
    const auto addr = adb::waitForPairingInstance(
        session.service, [&adbBin] { return adb::discoverMdnsServices(adbBin); },
        waitMs, 900, m_hostCancel);
    if (!addr.ok()) {
        const bool cancelled = m_hostCancel.load();
        std::fprintf(stderr, "[hostpair] 等待结束 %sms：%s\n", qPrintable(QString::number(elapsed())),
                     qPrintable(addr.error()));
        withState(cancelled ? "cancelled" : "failed",
                  cancelled ? QStringLiteral("已取消")
                            : QStringLiteral("等待手机扫码超时：未发现配对服务 %1"
                                             "（手机需开启「无线调试」并与 Mac 同一 Wi-Fi）")
                                          .arg(session.service));
        m_hostActive = false;
        return;
    }
    std::fprintf(stderr, "[hostpair] %ldms 发现配对服务 %s @ %s\n", (long)elapsed(),
                 qPrintable(session.service), qPrintable(addr.value()));

    // 2) adb pair：手机是配对服务端，密码即二维码 P 字段
    withState("pairing", QStringLiteral("已发现设备，配对中…"));
    const auto paired = adb::runPair(adbBin, addr.value(), session.password);
    if (!paired.ok()) {
        std::fprintf(stderr, "[hostpair] %ldms 配对失败：%s\n", (long)elapsed(),
                     qPrintable(paired.error()));
        withState("failed", paired.error());
        m_hostActive = false;
        return;
    }
    std::fprintf(stderr, "[hostpair] %ldms 配对成功\n", (long)elapsed());

    // 3) 自动连接（与 pair() 同路径：mDNS 新增 connect 服务 / auto-connect）
    withState("connecting", QStringLiteral("配对成功，连接中…"));
    const auto conn = connectAfterPair(beforeConnectAddrs);
    const bool connected = conn.ok();
    const QJsonObject result =
        pairOutcome(connected, connected ? conn.value() : QString(), addr.value(),
                    session.service, paired.value());
    QJsonObject extra;
    extra[QStringLiteral("connected")] = connected;
    extra[QStringLiteral("result")] = result;
    withState("done",
              connected ? QStringLiteral("已连接 %1").arg(conn.value())
                        : QStringLiteral("配对成功，但连接端口未就绪；稍后可手动连接"),
              extra);
    std::fprintf(stderr, "[hostpair] %ldms 完成（connected=%d）\n", (long)elapsed(),
                 connected ? 1 : 0);
    m_hostActive = false;
}

mw::Result<QJsonObject> DeviceManager::connectAddr(const QString& addr)
{
    const auto ok = ensureSrv();
    if (!ok.ok())
        return mw::err<QJsonObject>(ok.error());
    if (!adb::looksLikeAddr(addr))
        return mw::err<QJsonObject>(QStringLiteral("地址格式应为 ip:端口"));
    const auto conn = srvClient().connectNetwork(addr);
    if (!conn.ok())
        return mw::err<QJsonObject>(conn.error());
    const QString text = conn.value();
    if (!text.contains(QLatin1String("connected to"), Qt::CaseInsensitive))
        return mw::err<QJsonObject>(
            QStringLiteral("连接失败: %1（设备未配对或不在同一网络）").arg(text));
    if (!waitOnline(addr, 8000))
        return mw::err<QJsonObject>(QStringLiteral("连接超时：设备未进入 device 状态"));
    upsertKnown(addr, deviceInfo(addr));
    const auto list = listDevices();
    if (!list.ok())
        return mw::err<QJsonObject>(list.error());
    for (const auto& v : list.value()) {
        const QJsonObject o = v.toObject();
        if (o.value(QLatin1String("serial")).toString() == addr)
            return mw::Result<QJsonObject>::Ok(o);
    }
    return mw::err<QJsonObject>(QStringLiteral("连接成功但设备未出现在列表中"));
}

void DeviceManager::upsertKnown(const QString& serial, const QJsonObject& info)
{
    const QString key = info.value(QLatin1String("key")).toString(serial);
    QMutexLocker l(&m_mu);
    KnownDevice d = m_known.value(key);
    d.key = key;
    const auto merge = [&info, &d](const char* prop, QString KnownDevice::*field) {
        const QString v = info.value(QLatin1String(prop)).toString();
        if (!v.isEmpty())
            d.*field = v;
    };
    merge("model", &KnownDevice::model);
    merge("brand", &KnownDevice::brand);
    merge("android", &KnownDevice::android);
    if (isWifiSerial(serial))
        d.lastAddr = serial;
    d.lastSeen = QDateTime::currentSecsSinceEpoch();
    m_known.insert(key, d);
    saveLocked();
}

mw::Result<bool> DeviceManager::disconnect(const QString& serial)
{
    if (!isWifiSerial(serial))
        return mw::err<bool>(QStringLiteral("USB 设备不支持断开（拔线即断）"));
    const auto ok = ensureSrv();
    if (!ok.ok())
        return mw::err<bool>(ok.error());
    const auto r = srvClient().disconnectNetwork(serial);
    if (!r.ok())
        return mw::err<bool>(r.error());
    return mw::Result<bool>::Ok();
}

mw::Result<bool> DeviceManager::forget(const QString& serialOrAddr)
{
    load();
    // 在线无线连接一并断开
    if (isWifiSerial(serialOrAddr)) {
        const auto ok = ensureSrv();
        if (ok.ok())
            (void)srvClient().disconnectNetwork(serialOrAddr);
    }
    QMutexLocker l(&m_mu);
    for (auto it = m_known.begin(); it != m_known.end();) {
        if (it.value().key == serialOrAddr || it.value().lastAddr == serialOrAddr)
            it = m_known.erase(it);
        else
            ++it;
    }
    saveLocked();
    return mw::Result<bool>::Ok();
}

mw::Result<QJsonObject> DeviceManager::usbToWifi(const QString& serial, quint16 port)
{
    const auto ok = ensureSrv();
    if (!ok.ok())
        return mw::err<QJsonObject>(ok.error());
    if (isWifiSerial(serial))
        return mw::err<QJsonObject>(QStringLiteral("仅 USB 连接的设备可转无线"));
    if (!waitOnline(serial, 3000))
        return mw::err<QJsonObject>(QStringLiteral("USB 设备不在线"));

    // 设备 wlan IP（tcpip 重启 adbd 前取，USB 通道稳定）
    const auto ipOut = srvClient().shellBlocking(
        serial,
        QStringLiteral(
            "ip -f inet addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | head -1"),
        8000);
    if (!ipOut.ok())
        return mw::err<QJsonObject>(QStringLiteral("获取设备 IP 失败: %1").arg(ipOut.error()));
    QString ip;
    {
        const QString t = QString::fromUtf8(ipOut.value().out);
        const int sp = t.indexOf(QLatin1String("inet "));
        if (sp >= 0)
            ip = t.mid(sp + 5).split(QLatin1Char('\n')).at(0).trimmed();
    }
    if (ip.isEmpty())
        return mw::err<QJsonObject>(QStringLiteral("设备未连接 Wi-Fi（找不到 wlan0 地址）"));

    const auto switched = adb::runTcpip(adb::findAdbBinary(), serial, port);
    if (!switched.ok())
        return mw::err<QJsonObject>(switched.error());
    // adbd 重启监听需数秒；USB 侧会短暂掉线
    sleepMs(2000);
    const QString addr = QStringLiteral("%1:%2").arg(ip).arg(port);
    const auto conn = connectAddr(addr);
    if (!conn.ok())
        return mw::err<QJsonObject>(QStringLiteral("已切 TCP 模式但连接失败（%1）；可稍后手动连接 %2")
                                         .arg(conn.error(), addr));
    return conn;
}

} // namespace mw::dev
