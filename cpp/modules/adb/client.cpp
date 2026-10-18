#include "client.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "fs.hpp"
#include "devices.hpp"
#include "process.hpp"

namespace mw::adb {

namespace {

constexpr int kSyncDataMax = 64 * 1024;
// 0o100666：常规文件、所有人可读写
constexpr quint32 kRegularFileMode = 33206;

using Clock = std::chrono::steady_clock;
void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

mw::Result<bool> asBool(const mw::Result<qint64>& r)
{
    return r.ok() ? mw::Result<bool>::Ok() : mw::Result<bool>::Err(r.error());
}

} // namespace

// ===== AdbClient 基础 =====

AdbClient::AdbClient()
{
    host = QStringLiteral("127.0.0.1");
    port = kDefaultAdbPort;
    // MWINE_ADB_SERVER 置位（即使为空）→ server 模式
    direct = !qEnvironmentVariableIsSet("MWINE_ADB_SERVER");
}

AdbClient AdbClient::serverMode()
{
    AdbClient c;
    c.direct = false;
    return c;
}

QString AdbClient::discoveryDir()
{
    const QString home = mw::homeDir();
    if (home.isEmpty())
        return QString();
    return QDir(home).filePath(QStringLiteral("Library/Caches/TemporaryItems/avd/running"));
}

QVector<QPair<QString, quint16>> AdbClient::directDevices() const
{
    QVector<QPair<QString, quint16>> out;
    const QString dir = discoveryDir();
    if (dir.isEmpty())
        return out;
    // 目录不存在/不可读 → 空列表（Rust read_dir 失败 → 空）
    const QFileInfo dirInfo(dir);
    if (!dirInfo.isDir())
        return out;
    QDir d(dir);
    const auto entries = d.entryInfoList(QDir::Files);
    for (const QFileInfo& e : entries) {
        QFile f(e.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        // 找到首个以 key 起头的行并解析（对齐 Rust find + parse）
        auto get = [&lines](const char* key) -> std::optional<quint16> {
            for (const QString& l : lines) {
                if (!l.startsWith(QLatin1String(key)))
                    continue;
                bool ok = false;
                const quint16 v = l.mid(int(qstrlen(key))).trimmed().toUShort(&ok);
                return ok ? std::optional<quint16>(v) : std::nullopt;
            }
            return std::nullopt;
        };
        const auto serialPort = get("port.serial=");
        const auto adbPort = get("port.adb=");
        if (!serialPort.has_value() || !adbPort.has_value())
            continue;
        // qemu 重启后旧 pid 文件可能残留 → 按 TCP 可连性过滤
        if (TcpConn::connect(QStringLiteral("127.0.0.1"), *adbPort, 500).ok())
            out.append(qMakePair(QStringLiteral("emulator-%1").arg(*serialPort), *adbPort));
    }
    return out;
}

std::optional<quint16> AdbClient::resolveAdbdPort(const QString& serial) const
{
    for (const auto& [s, p] : directDevices()) {
        if (s == serial)
            return p;
    }
    // serial 规则兜底：emulator-<console> → console+1
    if (!serial.startsWith(QLatin1String("emulator-")))
        return std::nullopt;
    bool ok = false;
    const quint16 n = serial.mid(int(qstrlen("emulator-"))).toUShort(&ok);
    if (!ok || n == 0xFFFFu)
        return std::nullopt;
    const quint16 p = quint16(n + 1);
    if (TcpConn::connect(QStringLiteral("127.0.0.1"), p, 500).ok())
        return p;
    return std::nullopt;
}

mw::Result<SmartConnection> AdbClient::connectAdbd(quint16 adbPort, int timeoutMs) const
{
    auto c = TcpConn::connect(QStringLiteral("127.0.0.1"), adbPort, timeoutMs);
    if (!c.ok())
        return mw::err<SmartConnection>(
            errUnreachable(QStringLiteral("127.0.0.1:%1 (adbd)").arg(adbPort), c.error()));
    return mw::Result<SmartConnection>::Ok(SmartConnection::fromConn(std::move(c.value()), timeoutMs));
}

mw::Result<SmartConnection> AdbClient::connectServer(int timeoutMs) const
{
    const QString addr = QStringLiteral("%1:%2").arg(host).arg(port);
    auto c = TcpConn::connect(host, port, timeoutMs);
    if (!c.ok())
        return mw::err<SmartConnection>(errUnreachable(addr, c.error()));
    return mw::Result<SmartConnection>::Ok(SmartConnection::fromConn(std::move(c.value()), timeoutMs));
}

mw::Result<SmartConnection> AdbClient::transport(const QString& serial) const
{
    auto conn = connectServer(10000);
    if (!conn.ok())
        return conn;
    auto st = conn.value().sendService(QStringLiteral("host:transport:%1").arg(serial));
    if (!st.ok())
        return mw::err<SmartConnection>(st.error());
    return conn;
}

mw::Result<SmartConnection> AdbClient::deviceConn(const QString& serial, const QString& service) const
{
    if (direct) {
        if (const auto portOpt = resolveAdbdPort(serial)) {
            auto wire = WireConn::connect(*portOpt);
            if (wire.ok()) {
                // openStream 失败（如老设备拒绝 v2）→ 整体 Err（上层回退 v1）
                auto local = wire.value().openStream(service);
                if (!local.ok())
                    return mw::err<SmartConnection>(local.error());
                return mw::Result<SmartConnection>::Ok(
                    SmartConnection::fromConn(std::move(local.value()), 10000));
            }
        }
        // 直连不可达（server 时代遗留 / 真机）→ 走 server
    }
    auto conn = transport(serial);
    if (!conn.ok())
        return conn;
    auto st = conn.value().sendService(service);
    if (!st.ok())
        return mw::err<SmartConnection>(st.error());
    return conn;
}

// ===== 发现 / 生命周期 =====

mw::Result<QString> AdbClient::version() const
{
    auto conn = connectServer(5000);
    if (!conn.ok())
        return mw::err<QString>(conn.error());
    auto st = conn.value().sendService(QStringLiteral("host:version"));
    if (!st.ok())
        return mw::err<QString>(st.error());
    auto payload = conn.value().readPayload();
    if (!payload.ok())
        return mw::err<QString>(payload.error());
    return mw::Result<QString>::Ok(QString::fromUtf8(payload.value()).trimmed());
}

QString AdbClient::findAdbOnPath()
{
    // 共享定位逻辑（devices.hpp findAdbBinary）：PATH → 打包 .app runtime → 开发树 runtime
    return findAdbBinary();
}

mw::Result<bool> AdbClient::ensureServer(const QString& adbBin) const
{
    if (direct)
        return mw::Result<bool>::Ok();
    if (version().ok())
        return mw::Result<bool>::Ok();
    QString bin = adbBin;
    if (bin.isEmpty())
        bin = findAdbOnPath();
    if (bin.isEmpty())
        return mw::Result<bool>::Err(errAdbBinaryNotFound(
            QStringLiteral("adb not on PATH; pass sdk platform-tools path explicitly")));
    const auto st = mw::runProcess(bin, {QStringLiteral("start-server")}, 30000);
    if (!st.ok())
        return mw::Result<bool>::Err(errServiceFailed(QStringLiteral("adb start-server failed")));
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
        if (version().ok())
            return mw::Result<bool>::Ok();
        if (Clock::now() > deadline)
            return mw::Result<bool>::Err(errUnreachable(
                QStringLiteral("%1:%2").arg(host).arg(port),
                QStringLiteral("adb server did not come up after start-server")));
        sleepMs(300);
    }
}

mw::Result<bool> AdbClient::restartServer() const
{
    killServer(); // 尽力而为
    return ensureServer(QString());
}

mw::Result<bool> AdbClient::killServer() const
{
    auto conn = connectServer(5000);
    if (!conn.ok())
        return mw::Result<bool>::Err(conn.error());
    return conn.value().sendService(QStringLiteral("host:kill-server"));
}

mw::Result<QString> AdbClient::root(const QString& serial) const
{
    if (direct) {
        // root: 的应答依赖 adb server 的负载帧语义；模拟器 adbd 本就以
        // shell 权限运行产品所需命令，无需重启
        return mw::Result<QString>::Ok(
            QStringLiteral("direct: adbd 保持现状（模拟器无需 root 重启）"));
    }
    auto conn = transport(serial);
    if (!conn.ok())
        return mw::err<QString>(conn.error());
    auto st = conn.value().sendService(QStringLiteral("root:"));
    if (!st.ok())
        return mw::err<QString>(st.error());
    auto payload = conn.value().readPayload();
    if (!payload.ok())
        return mw::err<QString>(payload.error());
    return mw::Result<QString>::Ok(QString::fromUtf8(payload.value()).trimmed());
}

mw::Result<TcpConn> AdbClient::openAbstract(const QString& serial, const QString& name) const
{
    const auto port = resolveAdbdPort(serial);
    if (!port.has_value())
        return mw::err<TcpConn>(errDeviceNotFound(
            QStringLiteral("%1: adbd 端口未发现").arg(serial)));
    auto conn = connectAdbd(*port, 10000);
    if (!conn.ok())
        return mw::err<TcpConn>(conn.error());
    auto st = conn.value().sendService(QStringLiteral("localabstract:%1").arg(name));
    if (!st.ok())
        return mw::err<TcpConn>(st.error());
    return mw::Result<TcpConn>::Ok(conn.value().takeStream());
}

// ===== 设备枚举 =====

mw::Result<QVector<AdbDevice>> AdbClient::devices() const
{
    if (direct) {
        QVector<AdbDevice> list;
        const auto found = directDevices();
        for (const auto& [serial, port] : found) {
            Q_UNUSED(port);
            list.append(AdbDevice{serial, QStringLiteral("device")});
        }
        if (!list.isEmpty())
            return mw::Result<QVector<AdbDevice>>::Ok(list);
        // 无发现条目：回退 server 路径（可能真机在线）
    }
    auto conn = connectServer(5000);
    if (!conn.ok())
        return mw::err<QVector<AdbDevice>>(conn.error());
    auto st = conn.value().sendService(QStringLiteral("host:devices"));
    if (!st.ok())
        return mw::err<QVector<AdbDevice>>(st.error());
    auto payload = conn.value().readPayload();
    if (!payload.ok())
        return mw::err<QVector<AdbDevice>>(payload.error());
    const QStringList lines = QString::fromUtf8(payload.value()).split(QLatin1Char('\n'));
    QVector<AdbDevice> out;
    for (const QString& line : lines) {
        if (line.isEmpty())
            continue;
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() >= 2)
            out.append(AdbDevice{parts.at(0), parts.at(1)});
    }
    return mw::Result<QVector<AdbDevice>>::Ok(out);
}

mw::Result<AdbDevice> AdbClient::findOnlineDevice() const
{
    auto list = devices();
    if (!list.ok())
        return mw::err<AdbDevice>(list.error());
    for (const AdbDevice& d : list.value()) {
        if (d.state == QLatin1String("device"))
            return mw::Result<AdbDevice>::Ok(d);
    }
    return mw::err<AdbDevice>(errDeviceNotFound(QStringLiteral("no online device")));
}

mw::Result<QString> AdbClient::connectNetwork(const QString& addr) const
{
    auto conn = connectServer(5000);
    if (!conn.ok())
        return mw::err<QString>(conn.error());
    auto st = conn.value().sendService(QStringLiteral("host:connect %1").arg(addr));
    if (!st.ok())
        return mw::err<QString>(st.error());
    auto payload = conn.value().readPayload();
    if (!payload.ok())
        return mw::err<QString>(payload.error());
    return mw::Result<QString>::Ok(QString::fromUtf8(payload.value()).trimmed());
}

mw::Result<QString> AdbClient::disconnectNetwork(const QString& addr) const
{
    auto conn = connectServer(5000);
    if (!conn.ok())
        return mw::err<QString>(conn.error());
    auto st = conn.value().sendService(QStringLiteral("host:disconnect:%1").arg(addr));
    if (!st.ok())
        return mw::err<QString>(st.error());
    auto payload = conn.value().readPayload();
    if (!payload.ok())
        return mw::err<QString>(payload.error());
    return mw::Result<QString>::Ok(QString::fromUtf8(payload.value()).trimmed());
}

mw::Result<bool> AdbClient::waitForDevice(const QString& serial, int timeoutMs) const
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto list = devices();
        if (list.ok()) {
            for (const AdbDevice& d : list.value()) {
                if (d.serial == serial && d.state == QLatin1String("device"))
                    return mw::Result<bool>::Ok();
            }
        }
        if (Clock::now() > deadline)
            return mw::Result<bool>::Err(errDeviceNotFound(
                QStringLiteral("timeout waiting for %1 to be online").arg(serial)));
        sleepMs(500);
    }
}

// ===== shell =====

mw::Result<ShellOutput> AdbClient::shellBlocking(const QString& serial, const QString& cmd,
                                                 int timeoutMs) const
{
    // v2 优先；协商被拒（老设备 / 直连 OPEN 即 CLSE）时回退 v1（混流、无退出码）
    auto v2 = deviceConn(serial, v2Service(cmd));
    if (v2.ok()) {
        SmartConnection conn = std::move(v2.value());
        conn.setReadTimeout(timeoutMs);
        ShellV2Reader reader(conn.stream(), timeoutMs);
        return reader.collect();
    }
    auto v1 = deviceConn(serial, v1Service(cmd));
    if (!v1.ok())
        return mw::err<ShellOutput>(v1.error());
    v1.value().setReadTimeout(timeoutMs);
    auto data = readV1ToEnd(v1.value().stream(), timeoutMs);
    if (!data.ok())
        return mw::err<ShellOutput>(data.error());
    ShellOutput out;
    out.out = data.value();
    return mw::Result<ShellOutput>::Ok(out);
}

mw::Result<TcpConn> AdbClient::shellV1Session(const QString& serial, const QString& cmd) const
{
    // 空命令 → 纯交互 shell（无命令前缀）
    const QString service = cmd.isEmpty()
                                ? QStringLiteral("shell:")
                                : QStringLiteral("shell:%1").arg(cmd);
    auto conn = deviceConn(serial, service);
    if (!conn.ok())
        return mw::err<TcpConn>(conn.error());
    TcpConn stream = conn.value().takeStream();
    stream.setSocketTimeout(0); // 交互会话长期持有，清除建连阶段的超时
    return mw::Result<TcpConn>::Ok(std::move(stream));
}

mw::Result<ShellSession> AdbClient::shellSessionWith(const QString& serial,
                                                     const QString& service) const
{
    auto conn = deviceConn(serial, service);
    if (!conn.ok())
        return mw::err<ShellSession>(conn.error());
    TcpConn stream = conn.value().takeStream();
    TcpConn readerConn = stream.dup();
    if (!readerConn.isValid())
        return mw::err<ShellSession>(errIo(QStringLiteral("连接 dup 失败")));
    ShellSession s(ShellV2Stream(std::move(readerConn), -1), std::move(stream));
    return mw::Result<ShellSession>::Ok(std::move(s));
}

mw::Result<ShellSession> AdbClient::shellSession(const QString& serial, const QString& cmd) const
{
    return shellSessionWith(serial, v2Service(cmd));
}

mw::Result<ShellSession> AdbClient::shellPtySession(const QString& serial, const QString& cmd) const
{
    return shellSessionWith(serial, v2PtyService(cmd));
}

mw::Result<ShellStream> AdbClient::shellStream(const QString& serial, const QString& cmd) const
{
    auto conn = deviceConn(serial, v2Service(cmd));
    if (!conn.ok())
        return mw::err<ShellStream>(conn.error());
    return mw::Result<ShellStream>::Ok(ShellStream(conn.value().takeStream()));
}

mw::Result<qint64> ShellStream::drainAvailable()
{
    if (!m_conn.isValid())
        return mw::err<qint64>(QStringLiteral("连接无效"));
    char buf[4096];
    qint64 total = 0;
    for (;;) {
        const ssize_t n = ::recv(static_cast<int>(m_conn.fd()), buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            total += n;
            continue;
        }
        if (n == 0)
            break; // EOF
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break; // 无数据
        return mw::err<qint64>(errIo(QString::fromLatin1(::strerror(errno))));
    }
    return mw::Result<qint64>::Ok(total);
}

void ShellStream::shutdown()
{
    m_conn.shutdownBoth();
}

// ===== sync / 推送 / 拉取 =====

mw::Result<bool> SyncChannel::writeRequest(const char cmd[4], const QByteArray& payload)
{
    auto w1 = m_conn.writeAll(QByteArray(cmd, 4));
    if (!w1.ok())
        return mw::Result<bool>::Err(w1.error());
    char len[4];
    putLE32(len, quint32(payload.size()));
    auto w2 = m_conn.writeAll(QByteArray(len, 4));
    if (!w2.ok())
        return mw::Result<bool>::Err(w2.error());
    if (!payload.isEmpty()) {
        auto w3 = m_conn.writeAll(payload);
        if (!w3.ok())
            return mw::Result<bool>::Err(w3.error());
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> SyncChannel::sendHeader(const QString& remotePath, quint32 mode)
{
    return writeRequest("SEND", QStringLiteral("%1,%2").arg(remotePath).arg(mode).toUtf8());
}

mw::Result<bool> SyncChannel::dataChunk(const QByteArray& data)
{
    return writeRequest("DATA", data);
}

mw::Result<bool> SyncChannel::done(quint32 mtime)
{
    auto w1 = m_conn.writeAll(QByteArray("DONE", 4));
    if (!w1.ok())
        return mw::Result<bool>::Err(w1.error());
    char b[4];
    putLE32(b, mtime);
    return asBool(m_conn.writeAll(QByteArray(b, 4)));
}

mw::Result<bool> SyncChannel::quit()
{
    auto w1 = m_conn.writeAll(QByteArray("QUIT", 4));
    if (!w1.ok())
        return mw::Result<bool>::Err(w1.error());
    char b[4] = {0, 0, 0, 0};
    return asBool(m_conn.writeAll(QByteArray(b, 4)));
}

mw::Result<bool> SyncChannel::readReply()
{
    auto h = m_conn.readExact(8, m_timeoutMs);
    if (!h.ok())
        return mw::Result<bool>::Err(h.error());
    const QByteArray header = h.value();
    const quint32 len = getLE32(header.constData() + 4);
    if (header.startsWith("OKAY"))
        return mw::Result<bool>::Ok();
    if (header.startsWith("FAIL")) {
        auto msg = m_conn.readExact(int(len), m_timeoutMs);
        if (!msg.ok())
            return mw::Result<bool>::Err(msg.error());
        return mw::Result<bool>::Err(errServiceFailed(QString::fromUtf8(msg.value()).trimmed()));
    }
    return mw::Result<bool>::Err(
        errProtocol(QStringLiteral("unexpected sync reply: %1").arg(QString::fromUtf8(header.left(4)))));
}

mw::Result<bool> SyncChannel::pushStream(QIODevice& reader, const QString& remotePath,
                                         const std::function<void(quint64)>& onProgress)
{
    auto h = sendHeader(remotePath, kRegularFileMode);
    if (!h.ok())
        return h;
    char buf[kSyncDataMax];
    quint64 sent = 0;
    for (;;) {
        const qint64 n = reader.read(buf, sizeof(buf));
        if (n < 0)
            return mw::Result<bool>::Err(errIo(QStringLiteral("读取本地文件失败")));
        if (n == 0)
            break;
        auto c = dataChunk(QByteArray(buf, int(n)));
        if (!c.ok())
            return c;
        sent += quint64(n);
        if (onProgress)
            onProgress(sent);
    }
    auto d = done(0); // mtime=0 让设备使用当前时间
    if (!d.ok())
        return d;
    return readReply();
}

mw::Result<bool> SyncChannel::pushBytes(const QByteArray& data, const QString& remotePath)
{
    auto h = sendHeader(remotePath, kRegularFileMode);
    if (!h.ok())
        return h;
    for (int off = 0; off < data.size(); off += kSyncDataMax) {
        auto c = dataChunk(data.mid(off, kSyncDataMax));
        if (!c.ok())
            return c;
    }
    auto d = done(0);
    if (!d.ok())
        return d;
    return readReply();
}

mw::Result<QByteArray> SyncChannel::pullFile(const QString& remotePath)
{
    auto st = writeRequest("STAT", remotePath.toUtf8());
    if (!st.ok())
        return mw::err<QByteArray>(st.error());
    auto statRes = m_conn.readExact(16, m_timeoutMs);
    if (!statRes.ok())
        return mw::err<QByteArray>(statRes.error());
    const QByteArray stat = statRes.value();
    if (!stat.startsWith("STAT"))
        return mw::err<QByteArray>(errProtocol(QStringLiteral("unexpected sync stat reply: %1")
                                                   .arg(QString::fromUtf8(stat.left(4)))));
    const quint32 mode = getLE32(stat.constData() + 4);
    if (mode == 0)
        return mw::err<QByteArray>(
            errServiceFailed(QStringLiteral("no such file: %1").arg(remotePath)));

    auto rv = writeRequest("RECV", remotePath.toUtf8());
    if (!rv.ok())
        return mw::err<QByteArray>(rv.error());
    QByteArray out;
    for (;;) {
        auto h = m_conn.readExact(8, m_timeoutMs);
        if (!h.ok())
            return mw::err<QByteArray>(h.error());
        const QByteArray header = h.value();
        const quint32 len = getLE32(header.constData() + 4);
        if (header.startsWith("DATA")) {
            auto chunk = m_conn.readExact(int(len), m_timeoutMs);
            if (!chunk.ok())
                return mw::err<QByteArray>(chunk.error());
            out.append(chunk.value());
        } else if (header.startsWith("DONE")) {
            // DONE 的长度字段实际是 mtime
            return mw::Result<QByteArray>::Ok(out);
        } else if (header.startsWith("FAIL")) {
            auto msg = m_conn.readExact(int(len), m_timeoutMs);
            if (!msg.ok())
                return mw::err<QByteArray>(msg.error());
            return mw::err<QByteArray>(errServiceFailed(QString::fromUtf8(msg.value()).trimmed()));
        } else {
            return mw::err<QByteArray>(errProtocol(
                QStringLiteral("unexpected sync recv reply: %1").arg(QString::fromUtf8(header.left(4)))));
        }
    }
}

mw::Result<bool> AdbClient::pushBytes(const QString& serial, const QByteArray& data,
                                      const QString& remotePath) const
{
    auto conn = deviceConn(serial, QStringLiteral("sync:"));
    if (!conn.ok())
        return mw::Result<bool>::Err(conn.error());
    conn.value().setReadTimeout(60000);
    SyncChannel sync(conn.value().stream(), 60000);
    auto r = sync.pushBytes(data, remotePath);
    if (!r.ok())
        return r;
    return sync.quit();
}

mw::Result<bool> AdbClient::pushFile(const QString& serial, const QString& local,
                                     const QString& remotePath) const
{
    auto data = mw::readFile(local);
    if (!data.ok())
        return mw::Result<bool>::Err(data.error());
    return pushBytes(serial, data.value(), remotePath);
}

mw::Result<QVector<QString>> AdbClient::pushPath(const QString& serial, const QString& local,
                                                 const QString& remoteDir) const
{
    QVector<QString> pushed;
    const QFileInfo info(local);
    const QString name = info.fileName();
    if (name.isEmpty())
        return mw::err<QVector<QString>>(
            errServiceFailed(QStringLiteral("无法确定 %1 的文件名").arg(local)));
    if (info.isFile()) {
        const QString remote = remoteDir + QLatin1Char('/') + name;
        auto r = pushFile(serial, local, remote);
        if (!r.ok())
            return mw::err<QVector<QString>>(r.error());
        pushed.append(remote);
    } else if (info.isDir()) {
        const QString base = remoteDir + QLatin1Char('/') + name;
        QVector<QPair<QString, QString>> stack;
        stack.append(qMakePair(info.absoluteFilePath(), base));
        while (!stack.isEmpty()) {
            const auto top = stack.takeLast();
            const QString& lDir = top.first;
            const QString& rDir = top.second;
            auto mk = shellBlocking(
                serial,
                QStringLiteral("mkdir -p '%1'").arg(QString(rDir).replace(QLatin1Char('\''),
                                                                           QLatin1String("'\\''"))),
                15000);
            if (!mk.ok())
                return mw::err<QVector<QString>>(mk.error());
            QDir d(lDir);
            const auto entries = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
            for (const QFileInfo& e : entries) {
                if (e.isDir()) {
                    stack.append(qMakePair(e.absoluteFilePath(), rDir + QLatin1Char('/') + e.fileName()));
                } else {
                    const QString remote = rDir + QLatin1Char('/') + e.fileName();
                    auto r = pushFile(serial, e.absoluteFilePath(), remote);
                    if (!r.ok())
                        return mw::err<QVector<QString>>(r.error());
                    pushed.append(remote);
                }
            }
        }
    } else {
        return mw::err<QVector<QString>>(
            errServiceFailed(QStringLiteral("%1 不存在").arg(local)));
    }
    return mw::Result<QVector<QString>>::Ok(pushed);
}

mw::Result<bool> AdbClient::pushFileProgress(const QString& serial, const QString& local,
                                             const QString& remotePath,
                                             const std::function<void(quint64, quint64)>& onProgress) const
{
    const QFileInfo info(local);
    if (!info.exists())
        return mw::Result<bool>::Err(
            errIo(QStringLiteral("读取 %1 失败: 文件不存在").arg(local)));
    const quint64 total = quint64(info.size());
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly))
        return mw::Result<bool>::Err(
            errIo(QStringLiteral("无法打开 %1").arg(local)));
    auto conn = transport(serial);
    if (!conn.ok())
        return mw::Result<bool>::Err(conn.error());
    conn.value().setReadTimeout(180000);
    auto st = conn.value().sendService(QStringLiteral("sync:"));
    if (!st.ok())
        return st;
    SyncChannel sync(conn.value().stream(), 180000);
    return sync.pushStream(file, remotePath, [&onProgress, total](quint64 sent) {
        if (onProgress)
            onProgress(sent, total);
    });
}

mw::Result<QByteArray> AdbClient::pullFile(const QString& serial, const QString& remotePath) const
{
    auto conn = transport(serial);
    if (!conn.ok())
        return mw::err<QByteArray>(conn.error());
    conn.value().setReadTimeout(180000);
    auto st = conn.value().sendService(QStringLiteral("sync:"));
    if (!st.ok())
        return mw::err<QByteArray>(st.error());
    SyncChannel sync(conn.value().stream(), 180000);
    auto data = sync.pullFile(remotePath);
    if (!data.ok())
        return data;
    auto q = sync.quit();
    if (!q.ok())
        return mw::err<QByteArray>(q.error());
    return data;
}

// ===== 包管理 =====

mw::Result<QString> AdbClient::packageApkPath(const QString& serial, const QString& package) const
{
    auto out = shellBlocking(serial, QStringLiteral("pm path %1").arg(package), 15000);
    if (!out.ok())
        return mw::err<QString>(out.error());
    const QStringList lines = out.value().outStr().split(QLatin1Char('\n'));
    for (const QString& l : lines) {
        const QString t = l.trimmed();
        if (t.startsWith(QLatin1String("package:")))
            return mw::Result<QString>::Ok(t.mid(qstrlen("package:")));
    }
    return mw::err<QString>(
        errServiceFailed(QStringLiteral("pm path returned nothing for %1").arg(package)));
}

mw::Result<QString> AdbClient::installApkWithProgress(
    const QString& serial, const QString& apk, int timeoutMs,
    const std::function<void(const InstallProgress&)>& onProgress) const
{
    QString fileName = QFileInfo(apk).fileName();
    if (fileName.isEmpty())
        fileName = QStringLiteral("app.apk");
    const QString remote = QStringLiteral("/data/local/tmp/mwine_install_%1").arg(fileName);

    auto push = pushFileProgress(
        serial, apk, remote,
        [&onProgress](quint64 sent, quint64 total) {
            if (onProgress)
                onProgress(InstallProgress::push(sent, total));
        });
    if (!push.ok())
        return mw::err<QString>(push.error());
    if (onProgress)
        onProgress(InstallProgress::installing());

    auto out = shellBlocking(serial, QStringLiteral("pm install -r -t \"%1\"").arg(remote), timeoutMs);
    if (!out.ok())
        return mw::err<QString>(out.error());
    QString text = out.value().outStr();
    if (!out.value().err.isEmpty())
        text += out.value().errStr();
    // 清理临时文件（尽力而为）
    shellBlocking(serial, QStringLiteral("rm -f \"%1\"").arg(remote), 10000);
    if (!text.contains(QLatin1String("Success")))
        return mw::err<QString>(errServiceFailed(
            QStringLiteral("pm install failed: %1").arg(text.trimmed())));
    return mw::Result<QString>::Ok(text.trimmed());
}

mw::Result<QString> AdbClient::installApk(const QString& serial, const QString& apkPath,
                                          int timeoutMs) const
{
    return installApkWithProgress(serial, apkPath, timeoutMs, nullptr);
}

mw::Result<int> AdbClient::grantRuntimePermissions(const QString& serial, const QString& package) const
{
    auto dump = shellBlocking(serial, QStringLiteral("dumpsys package %1").arg(package), 20000);
    if (!dump.ok())
        return mw::err<int>(dump.error());
    // runtime permissions 段落形如：`    android.permission.CAMERA: granted=false, flags=...`
    int granted = 0;
    const QStringList lines = dump.value().outStr().split(QLatin1Char('\n'));
    const int kPrefixLen = int(qstrlen("android.permission."));
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QLatin1String("android.permission.")))
            continue;
        const int colon = trimmed.indexOf(QLatin1String(": "));
        if (colon < 0)
            continue;
        const QString perm = trimmed.mid(kPrefixLen, colon - kPrefixLen);
        const QString state = trimmed.mid(colon + 2);
        if (!state.startsWith(QLatin1String("granted=false")))
            continue;
        auto out = shellBlocking(
            serial,
            QStringLiteral("pm grant %1 android.permission.%2").arg(package, perm), 10000);
        if (out.ok() && out.value().ok())
            granted++;
    }
    return mw::Result<int>::Ok(granted);
}

mw::Result<QVector<QString>> AdbClient::listPackages(const QString& serial) const
{
    auto out = shellBlocking(serial, QStringLiteral("pm list packages -3"), 30000);
    if (!out.ok())
        return mw::err<QVector<QString>>(out.error());
    QVector<QString> pkgs;
    const QStringList lines = out.value().outStr().split(QLatin1Char('\n'));
    for (const QString& l : lines) {
        if (l.startsWith(QLatin1String("package:")))
            pkgs.append(l.mid(qstrlen("package:")).trimmed());
    }
    return mw::Result<QVector<QString>>::Ok(pkgs);
}

mw::Result<bool> AdbClient::launchPackage(const QString& serial, const QString& package) const
{
    auto out = shellBlocking(
        serial,
        QStringLiteral("monkey -p %1 -c android.intent.category.LAUNCHER 1").arg(package), 30000);
    if (!out.ok())
        return mw::Result<bool>::Err(out.error());
    const QString so = out.value().outStr();
    if (so.contains(QLatin1String("aborted")) || !out.value().ok())
        return mw::Result<bool>::Err(errServiceFailed(
            QStringLiteral("failed to launch %1: %2%3").arg(package, so, out.value().errStr())));
    return mw::Result<bool>::Ok();
}

// ===== 端口转发 =====

// 直连转发的单连接泵：local ↔ peer 双向搬运，一侧结束后半关闭另一侧
static void fwdPump(TcpConn local, TcpConn peer)
{
    TcpConn la = local.dup();
    TcpConn pa = peer.dup();
    std::thread t1([l = std::move(local), p = std::move(peer)]() mutable {
        for (;;) {
            const auto r = l.readSome(-1);
            if (!r.ok() || r.value().isEmpty())
                break;
            if (!p.writeAll(r.value()).ok())
                break;
        }
        p.shutdownWrite();
    });
    for (;;) {
        const auto r = pa.readSome(-1);
        if (!r.ok() || r.value().isEmpty())
            break;
        if (!la.writeAll(r.value()).ok())
            break;
    }
    // 半关闭对称性：peer 方向结束后也要关 local 的写端——否则对端（如
    // scrcpy 视频线程）永远等不到 EOF，读线程卡死、上层 join 卡死
    //（关闭 25s 兜底 + 析构双重 join 崩溃的根源链）
    la.shutdownWrite();
    t1.join();
}

mw::Result<bool> AdbClient::forwardAbstract(const QString& serial, quint16 tcpPort,
                                            const QString& abstractName) const
{
    const std::optional<quint16> portOpt =
        direct ? resolveAdbdPort(serial) : std::nullopt;
    if (portOpt && WireConn::connect(*portOpt).ok()) {
        // 直连可达：宿主本地 accept 池 + adbd wire 直开 abstract socket
        const quint16 adbdPort = *portOpt;
        const QString name = abstractName;
        std::thread([adbdPort, name, tcpPort] {
            // 简易 accept 池：scrcpy 重试/重连会多次接入，上限 8 条
            const qintptr lfd = makeLoopbackListener(tcpPort, nullptr);
            if (lfd < 0)
                return; // 端口占用等：与 Rust 一致，静默放弃
            for (int i = 0; i < 8; ++i) {
                const qintptr cfd = ::accept(lfd, nullptr, nullptr);
                if (cfd < 0)
                    break;
                TcpConn local = TcpConn::fromFd(cfd);
                // 对齐 adb forward 语义：目标 abstract socket 未就绪时
                //（server 仍在启动）保持重试 ~10s 而不是立刻掐线——
                // 单次连接的调用方（scrcpy control）没有重试机会
                TcpConn opened;
                for (int retry = 0; retry < 50; ++retry) {
                    auto wire = WireConn::connect(adbdPort);
                    if (wire.ok()) {
                        auto st = wire.value().openStream(
                            QStringLiteral("localabstract:%1").arg(name));
                        if (st.ok()) {
                            opened = std::move(st.value());
                            break;
                        }
                        if (retry == 0 || retry == 49)
                            qWarning().noquote()
                                << "[adb-forward] openStream(" << name << ") 失败:" << st.error();
                    } else if (retry == 0 || retry == 49) {
                        qWarning().noquote() << "[adb-forward] connect adbd:" << adbdPort
                                             << "失败:" << wire.error();
                    }
                    sleepMs(200);
                }
                if (opened.isValid()) {
                    // 每连接独立线程泵（不可在 accept 循环内 join——
                    // video 常驻会堵死后续 control 连接）
                    std::thread(&fwdPump, std::move(local), std::move(opened)).detach();
                } else {
                    // 10s 内 abstract socket 仍未出现（server 未启动/已死）
                    local.shutdownBoth();
                }
            }
            ::close(static_cast<int>(lfd));
        }).detach();
        return mw::Result<bool>::Ok();
    }
    // 直连被拒（secure 镜像 ro.adb.secure=1 在 CNXN 即要求鉴权）或端口不可达：
    // 走 adb server 注册原生 forward（push/shell 的 deviceConn 同款回落）
    if (direct) {
        auto conn = connectServer(10000);
        if (!conn.ok())
            return mw::Result<bool>::Err(conn.error());
        return conn.value().sendService(
            QStringLiteral("host-serial:%1:forward:tcp:%2;localabstract:%3")
                .arg(serial)
                .arg(tcpPort)
                .arg(abstractName));
    }
    auto conn = connectServer(10000);
    if (!conn.ok())
        return mw::Result<bool>::Err(conn.error());
    return conn.value().sendService(
        QStringLiteral("host-serial:%1:forward:tcp:%2;localabstract:%3")
            .arg(serial)
            .arg(tcpPort)
            .arg(abstractName));
}

mw::Result<bool> AdbClient::forwardTcp(const QString& serial, quint16 hostPort,
                                       quint16 devicePort) const
{
    auto conn = connectServer(10000);
    if (!conn.ok())
        return mw::Result<bool>::Err(conn.error());
    return conn.value().sendService(
        QStringLiteral("host-serial:%1:forward:tcp:%2;tcp:%3").arg(serial).arg(hostPort).arg(devicePort));
}

mw::Result<bool> AdbClient::removeForward(const QString& serial, quint16 tcpPort) const
{
    auto conn = connectServer(10000);
    if (!conn.ok())
        return mw::Result<bool>::Err(conn.error());
    return conn.value().sendService(
        QStringLiteral("host-serial:%1:remove-forward:tcp:%2").arg(serial).arg(tcpPort));
}

mw::Result<bool> AdbClient::reverseTcp(const QString& serial, quint16 devicePort,
                                       quint16 hostPort) const
{
    auto conn = transport(serial);
    if (!conn.ok())
        return mw::Result<bool>::Err(conn.error());
    return conn.value().sendService(
        QStringLiteral("reverse:forward:tcp:%1;tcp:%2").arg(devicePort).arg(hostPort));
}

mw::Result<bool> AdbClient::removeReverse(const QString& serial, quint16 devicePort) const
{
    auto conn = transport(serial);
    if (!conn.ok())
        return mw::Result<bool>::Err(conn.error());
    return conn.value().sendService(
        QStringLiteral("reverse:killforward:tcp:%1").arg(devicePort));
}

} // namespace mw::adb
