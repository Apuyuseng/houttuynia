// TcpConn —— POSIX fd 的 RAII 封装（替代 Rust std::net::TcpStream）。
// - 建连：getaddrinfo 解析 + 非阻塞 connect + poll 兑现 timeoutMs（此后保持阻塞模式）
// - 读：poll + 单次 MSG_DONTWAIT recv；EOF → Ok(空)；超时 → Err
// - fd 无线程亲和，可跨线程持有 / dup / shutdown（shutdown_handle 语义）
// ← crates/mwine-adb 之外的基础层（对应 Rust 侧对 std::net 的使用方式）
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <optional>

#include "result.hpp"

namespace mw {

namespace adb {

// 错误文案构造（对应 Rust crates/mwine-adb/src/error.rs 的 Display）
inline QString errServiceFailed(const QString& msg)
{
    return QStringLiteral("adb service request failed: ") + msg;
}
inline QString errProtocol(const QString& msg)
{
    return QStringLiteral("protocol error: ") + msg;
}
inline QString errDeviceNotFound(const QString& msg)
{
    return QStringLiteral("device %1 not found / offline").arg(msg);
}
inline QString errAdbBinaryNotFound(const QString& msg)
{
    return QStringLiteral("adb binary not found: ") + msg;
}
inline QString errIo(const QString& msg)
{
    return QStringLiteral("io error: ") + msg;
}
inline QString errUnreachable(const QString& addr, const QString& src)
{
    return QStringLiteral("adb server unreachable at %1: %2").arg(addr, src);
}

// 小端 u32 编解码（线协议/sync/shell v2 各处使用）
inline void putLE32(char* p, quint32 v)
{
    p[0] = char(v & 0xFF);
    p[1] = char((v >> 8) & 0xFF);
    p[2] = char((v >> 16) & 0xFF);
    p[3] = char((v >> 24) & 0xFF);
}
inline quint32 getLE32(const char* p)
{
    return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) | (quint32(quint8(p[2])) << 16)
           | (quint32(quint8(p[3])) << 24);
}

class TcpConn {
public:
    TcpConn() = default;
    TcpConn(TcpConn&& other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; }
    TcpConn& operator=(TcpConn&& other) noexcept
    {
        if (this != &other) {
            closeFd();
            m_fd = other.m_fd;
            other.m_fd = -1;
        }
        return *this;
    }
    TcpConn(const TcpConn&) = delete;
    TcpConn& operator=(const TcpConn&) = delete;
    ~TcpConn() { closeFd(); }

    // 域名解析（::getaddrinfo）+ 建连；timeoutMs 仅约束 connect 阶段（<0 无限等）
    static mw::Result<TcpConn> connect(const QString& host, quint16 port, int timeoutMs = 5000);
    // 接管一个已建立的 fd（本类负责 close）
    static TcpConn fromFd(qintptr fd) { return TcpConn(fd); }

    // 全量写（SO_SNDTIMEO 兜底；缓冲满时 poll 等待可写）
    mw::Result<qint64> writeAll(const QByteArray& data) const;
    // 读一批可用字节：EOF → Ok(空)；timeoutMs<0 无限等；0 立即返回（无数据 → Err）
    mw::Result<QByteArray> readSome(int timeoutMs) const { return readAtMost(64 * 1024, timeoutMs); }
    // 至多读 maxBytes 字节（EOF → Ok(空)）；readSome/readExact 的底层
    mw::Result<QByteArray> readAtMost(int maxBytes, int timeoutMs) const;
    // 精确读 n 字节；中途断开 → Err
    mw::Result<QByteArray> readExact(int n, int timeoutMs) const;

    qintptr fd() const { return m_fd; }
    bool isValid() const { return m_fd >= 0; }
    TcpConn dup() const;            // ::dup；失败返回无效连接（Rust try_clone 的弱化）
    void shutdownBoth() const;      // ::shutdown(SHUT_RDWR)——对 dup 出的所有句柄生效
    void shutdownWrite() const;     // ::shutdown(SHUT_WR)（桥接半关闭用）
    qintptr release();              // 放弃所有权并返回 fd（调用方负责 close）
    void setSocketTimeout(int timeoutMs) const; // SO_RCVTIMEO/SO_SNDTIMEO（<=0 清除）
    void setNoDelay(bool on) const;

private:
    explicit TcpConn(qintptr fd) : m_fd(fd) {}
    void closeFd();

    qintptr m_fd = -1;
};

} // namespace mw::adb
} // namespace mw
