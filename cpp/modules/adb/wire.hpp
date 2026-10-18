// ADB smart-socket 连接层 + adbd 直连线协议。
// ← crates/mwine-adb/src/connection.rs（SmartConnection：服务请求的长度前缀帧、
//    OKAY/FAIL 状态、十六进制长度前缀负载）
// ← crates/mwine-adb/src/wire.rs（WireConn：直连模拟器内建 adbd 的
//    CNXN 握手 + OPEN/WRTE/OKAY/CLSE 流帧 + 本地 TCP 桥接）
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include "result.hpp"
#include "tcpconn.hpp"

namespace mw::adb {

// ===== smart socket（connection.rs）=====

constexpr int kSmartMaxPayload = 4 * 1024 * 1024;

// "0010" → 16；非法十六进制 → Err（对齐 Rust usize::from_str_radix(s, 16)）
mw::Result<int> parseHexLength(const QByteArray& four);
mw::Result<int> readHexLength(TcpConn& conn, int timeoutMs);
mw::Result<QByteArray> readHexPayload(TcpConn& conn, int timeoutMs);
mw::Result<bool> readStatus(TcpConn& conn, int timeoutMs); // OKAY / FAIL(+错误文本)

// 在一条 TCP 连接上跑 smart-socket 协议（服务协商 + host 级负载读取）
class SmartConnection {
public:
    SmartConnection() = default;
    SmartConnection(TcpConn conn, int timeoutMs = 5000);
    // 阻止拷贝（TcpConn 独占 fd），可移动
    SmartConnection(const SmartConnection&) = delete;
    SmartConnection& operator=(const SmartConnection&) = delete;
    SmartConnection(SmartConnection&&) = default;
    SmartConnection& operator=(SmartConnection&&) = default;

    static mw::Result<SmartConnection> connectTo(const QString& host, quint16 port, int timeoutMs);
    static SmartConnection fromConn(TcpConn conn, int timeoutMs = 5000);

    TcpConn& stream() { return m_conn; }
    const TcpConn& stream() const { return m_conn; }
    TcpConn takeStream() { return std::move(m_conn); }
    void setReadTimeout(int ms) { m_timeoutMs = ms; }
    int readTimeout() const { return m_timeoutMs; }

    // 发送服务请求（自动加 4 位十六进制长度前缀），并校验 OKAY/FAIL
    mw::Result<bool> sendService(const QString& service);
    // 读取 host 级服务的负载（4 位十六进制长度前缀 + 数据）
    mw::Result<QByteArray> readPayload();
    // 读取以 OKAY/FAIL 开头的状态行
    mw::Result<bool> readStatus();

private:
    TcpConn m_conn;
    int m_timeoutMs = 5000;
};

// ===== adbd 直连线协议（wire.rs）=====

constexpr quint32 kAVersion = 0x01000000;
constexpr int kWireMaxData = 256 * 1024;

// 小端 u32 编解码见 tcpconn.hpp（putLE32/getLE32）

// IEEE crc32（adbd 校验 WRTE 负载；查表实现，C++11 局部静态量线程安全懒初始化）
quint32 wireCrc32(const QByteArray& data);
// "CNXN" 等标签（4 字节）→ 小端 u32
inline quint32 wireCmd(const char* tag)
{
    return quint32(quint8(tag[0])) | (quint32(quint8(tag[1])) << 8) | (quint32(quint8(tag[2])) << 16)
           | (quint32(quint8(tag[3])) << 24);
}
QString wireTagStr(quint32 cmd); // 逆操作（调试输出用）

struct WirePacket {
    quint32 command = 0;
    quint32 arg1 = 0;
    quint32 arg2 = 0;
    QByteArray data;
};

// 写一个线协议包（24 字节头 + 负载；头内含 crc32 与 magic）
mw::Result<bool> writeWirePacket(TcpConn& conn, quint32 command, quint32 arg1, quint32 arg2,
                                 const QByteArray& data);
// 读一个线协议包（校验 magic / 大小上限 8MB）
mw::Result<WirePacket> readWirePacket(TcpConn& conn, int timeoutMs);

// 在 127.0.0.1 上建监听（port=0 自动分配，实际端口写回 *boundPort）；失败返回 -1
qintptr makeLoopbackListener(quint16 port, quint16* boundPort);

// 与 adbd 的一条直连（CNXN 已完成）。openStream 后连接被消耗（移入桥接线程）。
class WireConn {
public:
    WireConn() = default;
    WireConn(const WireConn&) = delete;
    WireConn& operator=(const WireConn&) = delete;
    WireConn(WireConn&&) = default;
    WireConn& operator=(WireConn&&) = default;

    static mw::Result<WireConn> connect(quint16 port);

    quint32 maxData() const { return m_maxData; }

    // 打开一条流并桥接到本地 TCP；返回本地端（远端拒绝 → ServiceFailed）。
    // 连接生命周期与返回的 TcpConn 绑定：本地关闭/出错即发 CLSE 收尾。
    // 注意：调用后本对象持有的连接被移空，不可再次调用。
    mw::Result<TcpConn> openStream(const QString& service);

private:
    WireConn(TcpConn conn, quint32 maxData) : m_conn(std::move(conn)), m_maxData(maxData) {}

    TcpConn m_conn;
    quint32 m_maxData = 4096;
};

} // namespace mw::adb
