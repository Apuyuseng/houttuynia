#include "shell.hpp"

#include <cstring>

namespace mw::adb {

namespace {

// 头部读：完整 5 字节；0 字节即 EOF（filled==0 时）；读到一半断开 → Err
//（对齐 Rust read_full）
mw::Result<std::optional<QByteArray>> readFullHeader(TcpConn& conn, int timeoutMs)
{
    char hdr[5];
    int filled = 0;
    while (filled < 5) {
        auto r = conn.readAtMost(5 - filled, timeoutMs);
        if (!r.ok())
            return mw::err<std::optional<QByteArray>>(r.error());
        const QByteArray& part = r.value();
        if (part.isEmpty()) {
            if (filled == 0)
                return mw::Result<std::optional<QByteArray>>::Ok(std::nullopt); // EOF
            return mw::err<std::optional<QByteArray>>(
                errIo(QStringLiteral("eof in the middle of shell v2 header")));
        }
        ::memcpy(hdr + filled, part.constData(), static_cast<size_t>(part.size()));
        filled += part.size();
    }
    return mw::Result<std::optional<QByteArray>>::Ok(QByteArray(hdr, 5));
}

} // namespace

mw::Result<std::optional<ShellV2Chunk>> ShellV2Reader::nextChunk()
{
    auto hr = readFullHeader(m_conn, m_timeoutMs);
    if (!hr.ok())
        return mw::err<std::optional<ShellV2Chunk>>(hr.error());
    if (!hr.value().has_value())
        return mw::Result<std::optional<ShellV2Chunk>>::Ok(std::nullopt); // EOF

    const QByteArray& header = *hr.value();
    const int id = quint8(header.constData()[0]);
    const quint32 len = getLE32(header.constData() + 1);
    ShellV2Chunk chunk;
    if (len > 0) {
        auto dr = m_conn.readExact(int(len), m_timeoutMs);
        if (!dr.ok())
            return mw::err<std::optional<ShellV2Chunk>>(dr.error());
        chunk.data = dr.value();
    }
    switch (id) {
    case 0x1:
        chunk.kind = ShellChunk::Stdout;
        break;
    case 0x2:
        chunk.kind = ShellChunk::Stderr;
        break;
    case 0x3:
        chunk.kind = ShellChunk::Exit;
        break;
    default:
        return mw::err<std::optional<ShellV2Chunk>>(
            errProtocol(QStringLiteral("unknown shell v2 chunk id: 0x%1").arg(id, 0, 16)));
    }
    return mw::Result<std::optional<ShellV2Chunk>>::Ok(chunk);
}

mw::Result<ShellOutput> ShellV2Reader::collect()
{
    ShellOutput out;
    for (;;) {
        auto r = nextChunk();
        if (!r.ok())
            return mw::err<ShellOutput>(r.error());
        if (!r.value().has_value())
            break;
        const ShellV2Chunk& chunk = *r.value();
        switch (chunk.kind) {
        case ShellChunk::Stdout:
            out.out += chunk.data;
            break;
        case ShellChunk::Stderr:
            out.err += chunk.data;
            break;
        case ShellChunk::Exit:
            out.exitCode = chunk.data.isEmpty()
                               ? std::nullopt
                               : std::optional<int>(int(quint8(chunk.data.constData()[0])));
            break;
        }
    }
    return mw::Result<ShellOutput>::Ok(out);
}

mw::Result<QByteArray> readV1ToEnd(TcpConn& conn, int timeoutMs)
{
    QByteArray buf;
    for (;;) {
        auto r = conn.readSome(timeoutMs);
        if (!r.ok())
            return mw::err<QByteArray>(r.error());
        if (r.value().isEmpty())
            break; // EOF
        buf.append(r.value());
    }
    return mw::Result<QByteArray>::Ok(buf);
}

mw::Result<QByteArray> ShellV2Stream::nextStdoutPacket()
{
    for (;;) {
        // 交互流：EOF 一律按错误上抛（调用方按 EOF 处理）——对齐 Rust read_exact_or_eof
        char header[5];
        int filled = 0;
        while (filled < 5) {
            auto r = m_conn.readAtMost(5 - filled, m_timeoutMs);
            if (!r.ok())
                return mw::err<QByteArray>(r.error());
            const QByteArray& part = r.value();
            if (part.isEmpty())
                return mw::err<QByteArray>(errIo(QStringLiteral("shell v2 流结束")));
            ::memcpy(header + filled, part.constData(), static_cast<size_t>(part.size()));
            filled += part.size();
        }
        const int id = quint8(header[0]);
        // 保留 Rust 实现原样：交互路径长度字段为 大端
        const quint32 len = (quint32(quint8(header[1])) << 24) | (quint32(quint8(header[2])) << 16)
                            | (quint32(quint8(header[3])) << 8) | quint32(quint8(header[4]));
        QByteArray payload;
        if (len > 0) {
            auto dr = m_conn.readExact(int(len), m_timeoutMs);
            if (!dr.ok())
                return mw::err<QByteArray>(dr.error());
            payload = dr.value();
        }
        if (id == 1)
            return mw::Result<QByteArray>::Ok(payload);
        // 0=stdin 回显不该出现；2=stderr；3=exit——跳过
    }
}

mw::Result<QByteArray> ShellV2Stream::read(int maxBytes)
{
    while (m_pending.isEmpty()) {
        auto p = nextStdoutPacket();
        if (!p.ok())
            return mw::err<QByteArray>(p.error());
        m_pending = p.value(); // 空 stdout 包 → 继续读下一个（对齐 Rust continue）
    }
    const QByteArray out = m_pending.left(maxBytes);
    m_pending.remove(0, out.size());
    return mw::Result<QByteArray>::Ok(out);
}

} // namespace mw::adb
