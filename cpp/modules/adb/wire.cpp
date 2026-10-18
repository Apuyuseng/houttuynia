#include "wire.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace mw::adb {

// ===== smart socket（connection.rs）=====

mw::Result<int> parseHexLength(const QByteArray& four)
{
    if (four.size() != 4)
        return mw::err<int>(errProtocol(QStringLiteral("invalid hex length")));
    for (const char c : four) {
        const bool hexDigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hexDigit)
            return mw::err<int>(errProtocol(QStringLiteral("invalid hex length")));
    }
    bool ok = false;
    const int v = QString::fromLatin1(four).toInt(&ok, 16);
    if (!ok)
        return mw::err<int>(errProtocol(QStringLiteral("invalid hex length")));
    return mw::Result<int>::Ok(v);
}

mw::Result<int> readHexLength(TcpConn& conn, int timeoutMs)
{
    auto h = conn.readExact(4, timeoutMs);
    if (!h.ok())
        return mw::err<int>(h.error());
    return parseHexLength(h.value());
}

mw::Result<QByteArray> readHexPayload(TcpConn& conn, int timeoutMs)
{
    auto len = readHexLength(conn, timeoutMs);
    if (!len.ok())
        return mw::err<QByteArray>(len.error());
    return conn.readExact(len.value(), timeoutMs);
}

mw::Result<bool> readStatus(TcpConn& conn, int timeoutMs)
{
    auto h = conn.readExact(4, timeoutMs);
    if (!h.ok())
        return mw::Result<bool>::Err(h.error());
    const QByteArray& status = h.value();
    if (status == QByteArrayLiteral("OKAY"))
        return mw::Result<bool>::Ok();
    if (status == QByteArrayLiteral("FAIL")) {
        auto msg = readHexPayload(conn, timeoutMs);
        if (!msg.ok())
            return mw::Result<bool>::Err(msg.error());
        return mw::Result<bool>::Err(errServiceFailed(QString::fromUtf8(msg.value()).trimmed()));
    }
    return mw::Result<bool>::Err(
        errProtocol(QStringLiteral("unexpected status bytes: %1").arg(QString::fromUtf8(status))));
}

SmartConnection::SmartConnection(TcpConn conn, int timeoutMs)
    : m_conn(std::move(conn)), m_timeoutMs(timeoutMs)
{
}

mw::Result<SmartConnection> SmartConnection::connectTo(const QString& host, quint16 port, int timeoutMs)
{
    auto c = TcpConn::connect(host, port, timeoutMs);
    if (!c.ok())
        return mw::err<SmartConnection>(c.error());
    return mw::Result<SmartConnection>::Ok(SmartConnection(std::move(c.value()), timeoutMs));
}

SmartConnection SmartConnection::fromConn(TcpConn conn, int timeoutMs)
{
    return SmartConnection(std::move(conn), timeoutMs);
}

mw::Result<bool> SmartConnection::sendService(const QString& service)
{
    const QByteArray payload = service.toUtf8();
    if (payload.size() > 0xFFFF)
        return mw::Result<bool>::Err(errProtocol(QStringLiteral("service string too long")));
    const QByteArray framed = QByteArray::number(payload.size(), 16).rightJustified(4, '0');
    auto w1 = m_conn.writeAll(framed);
    if (!w1.ok())
        return mw::Result<bool>::Err(w1.error());
    auto w2 = m_conn.writeAll(payload);
    if (!w2.ok())
        return mw::Result<bool>::Err(w2.error());
    return mw::adb::readStatus(m_conn, m_timeoutMs); // 自由函数（成员同名，需限定）
}

mw::Result<QByteArray> SmartConnection::readPayload()
{
    auto len = readHexLength(m_conn, m_timeoutMs);
    if (!len.ok())
        return mw::err<QByteArray>(len.error());
    if (len.value() > kSmartMaxPayload)
        return mw::err<QByteArray>(errProtocol(QStringLiteral("payload too large: %1").arg(len.value())));
    return m_conn.readExact(len.value(), m_timeoutMs);
}

mw::Result<bool> SmartConnection::readStatus()
{
    return mw::adb::readStatus(m_conn, m_timeoutMs); // 自由函数（成员同名，需限定）
}

// ===== adbd 直连线协议（wire.rs）=====

quint32 wireCrc32(const QByteArray& data)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) != 0 ? quint32(0xEDB88320) ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (const char b : data)
        c = table[(c ^ quint8(b)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

QString wireTagStr(quint32 cmd)
{
    char b[5] = {char(cmd & 0xFF), char((cmd >> 8) & 0xFF), char((cmd >> 16) & 0xFF),
                 char((cmd >> 24) & 0xFF), 0};
    QString s = QString::fromLatin1(b, 4);
    while (s.endsWith(QChar(u'\0')))
        s.chop(1);
    return s;
}

static bool wireDebug()
{
    static const bool on = qEnvironmentVariableIsSet("MWINE_WIRE_DEBUG");
    return on;
}

mw::Result<bool> writeWirePacket(TcpConn& conn, quint32 command, quint32 arg1, quint32 arg2,
                                 const QByteArray& data)
{
    if (wireDebug())
        fprintf(stderr, "[wire >] %s arg1=%u arg2=%u len=%d\n",
                wireTagStr(command).toUtf8().constData(), arg1, arg2, int(data.size()));
    char head[24];
    putLE32(head + 0, command);
    putLE32(head + 4, arg1);
    putLE32(head + 8, arg2);
    putLE32(head + 12, quint32(data.size()));
    putLE32(head + 16, wireCrc32(data));
    putLE32(head + 20, command ^ 0xFFFFFFFFu);
    auto w1 = conn.writeAll(QByteArray(head, 24));
    if (!w1.ok())
        return mw::Result<bool>::Err(w1.error());
    auto w2 = conn.writeAll(data);
    if (!w2.ok())
        return mw::Result<bool>::Err(w2.error());
    return mw::Result<bool>::Ok();
}

mw::Result<WirePacket> readWirePacket(TcpConn& conn, int timeoutMs)
{
    auto hr = conn.readExact(24, timeoutMs);
    if (!hr.ok())
        return mw::err<WirePacket>(hr.error());
    const QByteArray head = hr.value();
    const quint32 command = getLE32(head.constData() + 0);
    WirePacket p;
    p.command = command;
    p.arg1 = getLE32(head.constData() + 4);
    p.arg2 = getLE32(head.constData() + 8);
    const quint32 len = getLE32(head.constData() + 12);
    if (getLE32(head.constData() + 20) != (command ^ 0xFFFFFFFFu))
        return mw::err<WirePacket>(errProtocol(QStringLiteral("adb 线协议 magic 不符")));
    if (len > 8 * 1024 * 1024)
        return mw::err<WirePacket>(errProtocol(QStringLiteral("adb 包过大: %1").arg(len)));
    if (len > 0) {
        auto dr = conn.readExact(int(len), timeoutMs);
        if (!dr.ok())
            return mw::err<WirePacket>(dr.error());
        p.data = dr.value();
    }
    if (wireDebug())
        fprintf(stderr, "[wire <] %s arg1=%u arg2=%u len=%u %s\n", wireTagStr(command).toUtf8().constData(),
                p.arg1, p.arg2, len,
                QString::fromUtf8(p.data.left(60)).toUtf8().constData());
    return mw::Result<WirePacket>::Ok(p);
}

qintptr makeLoopbackListener(quint16 port, quint16* boundPort)
{
    const qintptr fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || ::listen(fd, 8) != 0) {
        ::close(static_cast<int>(fd));
        return -1;
    }
    if (boundPort) {
        socklen_t sl = sizeof(sa);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &sl) == 0)
            *boundPort = ntohs(sa.sin_port);
    }
    return fd;
}

mw::Result<WireConn> WireConn::connect(quint16 port)
{
    auto cr = TcpConn::connect(QStringLiteral("127.0.0.1"), port, 10000);
    if (!cr.ok())
        return mw::err<WireConn>(errUnreachable(QStringLiteral("127.0.0.1:%1 (adbd)").arg(port),
                                                cr.error()));
    TcpConn conn = std::move(cr.value());
    const QByteArray ident =
        QByteArray("host::features=shell_v2,cmd,abb,fixed_push_mkdir,apex,stat_v2,abb_exec");
    // 握手期 10s 超时（TcpConn::connect 已设置）
    auto w = writeWirePacket(conn, wireCmd("CNXN"), kAVersion, quint32(kWireMaxData), ident);
    if (!w.ok())
        return mw::err<WireConn>(w.error());
    auto pr = readWirePacket(conn, 10000);
    if (!pr.ok())
        return mw::err<WireConn>(pr.error());
    const WirePacket& p = pr.value();
    if (p.command == wireCmd("AUTH"))
        return mw::err<WireConn>(errProtocol(
            QStringLiteral("adbd 要求鉴权（ro.adb.secure=1）——直连模式仅支持模拟器镜像")));
    if (p.command != wireCmd("CNXN"))
        return mw::err<WireConn>(errProtocol(
            QStringLiteral("CNXN 握手异常: %1").arg(QString::fromUtf8(p.data.left(40)))));
    conn.setSocketTimeout(0); // 握手完成，桥接线程长期阻塞读写
    quint32 md = p.arg2;
    if (md < 4096)
        md = 4096;
    if (md > quint32(kWireMaxData))
        md = quint32(kWireMaxData);
    return mw::Result<WireConn>::Ok(WireConn(std::move(conn), md));
}

// 流控：任一时刻至多一条未确认 WRTE（OKAY 抵达后放行下一条）
struct FlowControl {
    std::mutex mu;
    std::condition_variable cv;
    bool pending = false;
};

mw::Result<TcpConn> WireConn::openStream(const QString& service)
{
    TcpConn wire = std::move(m_conn); // 本对象连接被消耗（对齐 Rust 消耗 self）
    const quint32 localId = 1;
    QByteArray svc = service.toUtf8();
    svc.append(char(0));
    auto ow = writeWirePacket(wire, wireCmd("OPEN"), localId, 0, svc);
    if (!ow.ok())
        return mw::err<TcpConn>(ow.error());
    auto pr = readWirePacket(wire, 10000);
    if (!pr.ok())
        return mw::err<TcpConn>(pr.error());
    WirePacket p = std::move(pr.value());
    if (p.command == wireCmd("CLSE"))
        return mw::err<TcpConn>(errServiceFailed(QStringLiteral("adbd 拒绝服务: %1").arg(service)));
    if (p.command != wireCmd("OKAY"))
        return mw::err<TcpConn>(
            errProtocol(QStringLiteral("OPEN 后期待 OKAY，得到 0x%1").arg(p.command, 0, 16)));
    // OKAY(arg1=发送方流id, arg2=对端流id)：设备侧流 id 在 arg1
    const quint32 remoteId = p.arg1;
    const quint32 maxData = m_maxData;

    // 本地桥接端：调用方持有 local；两个泵线程都操作 peer 的克隆——
    // 设备→调用方：写 peer（调用方从 local 读到）；调用方→设备：读 peer
    //（调用方写 local）。peer 本体随线程移动，不可在此析构。
    quint16 localPort = 0;
    qintptr lfd = makeLoopbackListener(0, &localPort);
    if (lfd < 0)
        return mw::err<TcpConn>(errIo(QStringLiteral("本地桥接监听失败")));
    auto localRes = TcpConn::connect(QStringLiteral("127.0.0.1"), localPort, 5000);
    if (!localRes.ok()) {
        ::close(static_cast<int>(lfd));
        return mw::err<TcpConn>(localRes.error());
    }
    const qintptr cfd = ::accept(lfd, nullptr, nullptr);
    ::close(static_cast<int>(lfd));
    if (cfd < 0)
        return mw::err<TcpConn>(errIo(QStringLiteral("本地桥接 accept 失败")));
    TcpConn local = std::move(localRes.value());
    TcpConn peer = TcpConn::fromFd(cfd);
    local.setNoDelay(true);

    auto flow = std::make_shared<FlowControl>();
    TcpConn wireRd = wire.dup();
    TcpConn peerRd = peer.dup();
    TcpConn peerWr = std::move(peer);
    TcpConn wireWr = std::move(wire);

    // 调用方 → 设备（读 peer；写 WRTE，串行等待流控 ACK）
    std::thread(
        [wireWr = std::move(wireWr), peerRd = std::move(peerRd), flow, localId, remoteId]() mutable {
            for (;;) {
                const auto r = peerRd.readSome(-1);
                if (!r.ok() || r.value().isEmpty())
                    break; // EOF / 出错
                {
                    std::unique_lock<std::mutex> lk(flow->mu);
                    while (flow->pending)
                        flow->cv.wait(lk);
                    flow->pending = true;
                }
                if (!writeWirePacket(wireWr, wireCmd("WRTE"), localId, remoteId, r.value()).ok())
                    return; // Rust：直接返回（不发 CLSE）
            }
            writeWirePacket(wireWr, wireCmd("CLSE"), localId, remoteId, QByteArray());
        })
        .detach();

    // 设备 → 调用方（写 peer；兼流控 ACK 分发）
    std::thread(
        [wireRd = std::move(wireRd), peerWr = std::move(peerWr), flow, localId, remoteId]() mutable {
            for (;;) {
                const auto pr2 = readWirePacket(wireRd, -1);
                if (!pr2.ok()) {
                    if (wireDebug())
                        fprintf(stderr, "[wire-rd] 读线程退出: %s\n", pr2.error().toUtf8().constData());
                    peerWr.shutdownBoth();
                    break;
                }
                const WirePacket& q = pr2.value();
                if (q.command == wireCmd("WRTE")) {
                    if (!writeWirePacket(wireRd, wireCmd("OKAY"), localId, remoteId, QByteArray()).ok()
                        || !peerWr.writeAll(q.data).ok())
                        break;
                } else if (q.command == wireCmd("OKAY")) {
                    {
                        std::lock_guard<std::mutex> lk(flow->mu);
                        flow->pending = false;
                    }
                    flow->cv.notify_all();
                } else if (q.command == wireCmd("CLSE")) {
                    peerWr.shutdownBoth();
                    break;
                }
                // 其他包：忽略
            }
        })
        .detach();

    return mw::Result<TcpConn>::Ok(std::move(local));
}

} // namespace mw::adb
