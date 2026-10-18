// 流读取抽象：对应 Rust 侧泛型 `<R: Read>` 的线协议解析层。
// - TcpReader：包 mw::adb::TcpConn（socket 流；timeoutMs<0 阻塞）
// - BufferReader：内存缓冲游标（测试 / 解析既有字节）
//
// readExact 契约（对齐 Rust read_full）：
//   Ok(n 字节)   —— 读满
//   Ok(空)       —— 起始即 EOF（流干净结束，包边界）
//   Err          —— 读错误 / 包中途 EOF
#pragma once

#include <QByteArray>
#include <QString>

#include "tcpconn.hpp"
#include "error.hpp"
#include "result.hpp"

namespace mw::scrcpy {

class StreamReader {
public:
    virtual ~StreamReader() = default;
    virtual mw::Result<QByteArray> readExact(int n) = 0;
};

// TcpConn 适配（conn 由调用方持有，作用域内有效）
class TcpReader : public StreamReader {
public:
    TcpReader(mw::adb::TcpConn& conn, int timeoutMs = -1) : m_conn(conn), m_timeoutMs(timeoutMs) {}

    mw::Result<QByteArray> readExact(int n) override
    {
        if (n <= 0)
            return mw::Result<QByteArray>::Ok(QByteArray());
        QByteArray buf(n, Qt::Uninitialized);
        char* dst = buf.data();
        int filled = 0;
        while (filled < n) {
            auto r = m_conn.readAtMost(n - filled, m_timeoutMs);
            if (!r.ok())
                return mw::err<QByteArray>(r.error());
            const QByteArray& part = r.value();
            if (part.isEmpty()) {
                if (filled == 0)
                    return mw::Result<QByteArray>::Ok(QByteArray()); // 干净结束
                return mw::err<QByteArray>(
                    errIo(QStringLiteral("eof in the middle of packet（已读 %1/%2）")
                              .arg(filled)
                              .arg(n)));
            }
            ::memcpy(dst + filled, part.constData(), static_cast<size_t>(part.size()));
            filled += part.size();
        }
        return mw::Result<QByteArray>::Ok(buf);
    }

private:
    mw::adb::TcpConn& m_conn;
    int m_timeoutMs;
};

// 回退读：把已从底层流读出的字节"塞回"（try-read 场景：先探测再解析）
class PushbackReader : public StreamReader {
public:
    explicit PushbackReader(StreamReader& inner) : m_inner(inner) {}

    void pushBack(QByteArray b)
    {
        m_pending = b + m_pending;
    }

    mw::Result<QByteArray> readExact(int n) override
    {
        if (n <= 0)
            return mw::Result<QByteArray>::Ok(QByteArray());
        QByteArray out;
        out.reserve(n);
        if (!m_pending.isEmpty()) {
            const int take = qMin(n, m_pending.size());
            out += m_pending.left(take);
            m_pending = m_pending.mid(take);
        }
        while (out.size() < n) {
            auto r = m_inner.readExact(n - out.size());
            if (!r.ok())
                return mw::err<QByteArray>(r.error());
            const QByteArray& part = r.value();
            if (part.isEmpty()) {
                if (out.isEmpty())
                    return mw::Result<QByteArray>::Ok(QByteArray()); // 干净结束
                return mw::err<QByteArray>(
                    errIo(QStringLiteral("eof in the middle of packet（已读 %1/%2）")
                              .arg(out.size())
                              .arg(n)));
            }
            out += part;
        }
        return mw::Result<QByteArray>::Ok(std::move(out));
    }

private:
    StreamReader& m_inner;
    QByteArray m_pending;
};

// 内存缓冲游标（数据拷贝持有）
class BufferReader : public StreamReader {
public:
    explicit BufferReader(QByteArray data) : m_data(std::move(data)) {}

    mw::Result<QByteArray> readExact(int n) override
    {
        if (n <= 0)
            return mw::Result<QByteArray>::Ok(QByteArray());
        if (m_pos >= m_data.size())
            return mw::Result<QByteArray>::Ok(QByteArray());
        if (m_pos + n > m_data.size())
            return mw::err<QByteArray>(
                errIo(QStringLiteral("buffer underflow（剩 %1 需 %2）")
                          .arg(m_data.size() - m_pos)
                          .arg(n)));
        QByteArray out = m_data.mid(m_pos, n);
        m_pos += n;
        return mw::Result<QByteArray>::Ok(out);
    }

    int pos() const { return m_pos; }

private:
    QByteArray m_data;
    int m_pos = 0;
};

// 大端整数解码
inline quint32 beU32(const char* p)
{
    return (quint32(quint8(p[0])) << 24) | (quint32(quint8(p[1])) << 16)
           | (quint32(quint8(p[2])) << 8) | quint32(quint8(p[3]));
}
inline quint64 beU64(const char* p)
{
    return (quint64(beU32(p)) << 32) | beU32(p + 4);
}
inline quint16 beU16(const char* p)
{
    return quint16((quint16(quint8(p[0])) << 8) | quint8(p[1]));
}

} // namespace mw::scrcpy
