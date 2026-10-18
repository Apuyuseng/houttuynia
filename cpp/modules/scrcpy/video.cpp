// 视频流解复用实现 ← crates/mwine-scrcpy/src/video.rs
#include "video.hpp"

namespace mw::scrcpy {

mw::Result<std::optional<VideoPacket>> PacketDemuxer::nextPacket()
{
    for (;;) {
        auto header = m_reader.readExact(8);
        if (!header.ok())
            return mw::err<std::optional<VideoPacket>>(header.error());
        if (header.value().isEmpty())
            return mw::Result<std::optional<VideoPacket>>::Ok(std::nullopt); // 流结束

        const quint64 ptsAndFlags = beU64(header.value().constData());
        if (ptsAndFlags & kVideoPacketFlagSession) {
            // session meta：前 8 字节 = [flags(4B)][width(4B)]，再读 4 字节 height
            const quint32 w = quint32(ptsAndFlags & 0xFFFFFFFFu);
            auto h = m_reader.readExact(4);
            if (!h.ok())
                return mw::err<std::optional<VideoPacket>>(h.error());
            if (h.value().isEmpty())
                return mw::err<std::optional<VideoPacket>>(
                    errIo(QStringLiteral("eof in the middle of session meta")));
            m_width = w;
            m_height = beU32(h.value().constData());
            continue;
        }
        VideoPacket p;
        p.config = (ptsAndFlags & kVideoPacketFlagConfig) != 0;
        p.keyFrame = (ptsAndFlags & kVideoPacketFlagKeyFrame) != 0;
        p.pts = ptsAndFlags & ~(kVideoPacketFlagConfig | kVideoPacketFlagKeyFrame);

        auto size = m_reader.readExact(4);
        if (!size.ok())
            return mw::err<std::optional<VideoPacket>>(size.error());
        if (size.value().isEmpty())
            return mw::err<std::optional<VideoPacket>>(
                errIo(QStringLiteral("eof before video packet size")));
        const quint32 len = beU32(size.value().constData());
        if (len == 0) {
            p.data.clear();
            return mw::Result<std::optional<VideoPacket>>::Ok(std::move(p));
        }
        auto data = m_reader.readExact(int(len));
        if (!data.ok())
            return mw::err<std::optional<VideoPacket>>(data.error());
        if (data.value().isEmpty())
            return mw::err<std::optional<VideoPacket>>(
                errIo(QStringLiteral("eof in the middle of video packet")));
        p.data = data.value();
        return mw::Result<std::optional<VideoPacket>>::Ok(std::move(p));
    }
}

mw::Result<QString> readConnectionHeader(StreamReader& reader)
{
    auto dummy = reader.readExact(1);
    if (!dummy.ok())
        return mw::err<QString>(dummy.error());
    if (dummy.value().isEmpty())
        return mw::err<QString>(errIo(QStringLiteral("eof before connection dummy byte")));
    return readDeviceName(reader);
}

mw::Result<QString> readDeviceName(StreamReader& reader)
{
    auto name = reader.readExact(64);
    if (!name.ok())
        return mw::err<QString>(name.error());
    const QByteArray& raw = name.value();
    const int end = int(raw.indexOf(char(0)));
    const QByteArray cut = end < 0 ? raw : raw.left(end);
    return mw::Result<QString>::Ok(QString::fromUtf8(cut));
}

VideoStream::VideoStream(mw::adb::TcpConn conn, VideoCodecId codec)
    : m_conn(std::move(conn)), m_codec(codec)
{
    m_reader = std::make_unique<TcpReader>(m_conn);
    m_demux = std::make_unique<PacketDemuxer>(*m_reader);
}

// 移动必须重建 reader/demux：它们持有指向本对象 m_conn 的引用，
// = default 会把引用留在已析构的旧对象上（悬垂 → 野 fd / ENOTSOCK）。
VideoStream::VideoStream(VideoStream&& other) noexcept
    : m_conn(std::move(other.m_conn)), m_codec(other.m_codec)
{
    m_reader = std::make_unique<TcpReader>(m_conn);
    m_demux = std::make_unique<PacketDemuxer>(*m_reader);
}

VideoStream& VideoStream::operator=(VideoStream&& other) noexcept
{
    if (this != &other) {
        m_conn = std::move(other.m_conn);
        m_codec = other.m_codec;
        m_reader = std::make_unique<TcpReader>(m_conn);
        m_demux = std::make_unique<PacketDemuxer>(*m_reader);
    }
    return *this;
}

mw::Result<VideoStream> VideoStream::create(mw::adb::TcpConn conn)
{
    {
        TcpReader reader(conn, 10000); // 头部读取带 10s 超时
        auto codecBytes = reader.readExact(4);
        if (!codecBytes.ok())
            return mw::err<VideoStream>(codecBytes.error());
        if (codecBytes.value().isEmpty()) {
            return mw::err<VideoStream>(
                errProtocol(QStringLiteral("video stream closed before codec id")));
        }
        const quint32 id = beU32(codecBytes.value().constData());
        if (id == 0) {
            return mw::err<VideoStream>(
                errProtocol(QStringLiteral("device disabled the video stream (codec id = 0)")));
        }
        if (id == 1) {
            return mw::err<VideoStream>(errProtocol(
                QStringLiteral("video stream configuration error on device (codec id = 1)")));
        }
        auto codec = videoCodecFromWire(id);
        if (!codec.ok())
            return mw::err<VideoStream>(codec.error());
        return mw::Result<VideoStream>::Ok(VideoStream(std::move(conn), codec.value()));
    }
}

} // namespace mw::scrcpy
