// scrcpy 音频流解复用实现 ← crates/mwine-scrcpy/src/audio.rs
#include "audio.hpp"

namespace mw::scrcpy {

mw::Result<std::optional<AudioPacket>> AudioDemuxer::nextPacket()
{
    auto header = m_reader.readExact(8);
    if (!header.ok())
        return mw::err<std::optional<AudioPacket>>(header.error());
    if (header.value().isEmpty())
        return mw::Result<std::optional<AudioPacket>>::Ok(std::nullopt); // 流结束

    const quint64 ptsAndFlags = beU64(header.value().constData());
    AudioPacket p;
    p.config = (ptsAndFlags & kAudioPacketFlagConfig) != 0;
    p.pts = ptsAndFlags & ~kAudioPacketFlagConfig;

    auto size = m_reader.readExact(4);
    if (!size.ok())
        return mw::err<std::optional<AudioPacket>>(size.error());
    if (size.value().isEmpty())
        return mw::err<std::optional<AudioPacket>>(
            errIo(QStringLiteral("eof before audio packet size")));
    const quint32 len = beU32(size.value().constData());
    if (len == 0) {
        p.data.clear();
        return mw::Result<std::optional<AudioPacket>>::Ok(std::move(p));
    }
    auto data = m_reader.readExact(int(len));
    if (!data.ok())
        return mw::err<std::optional<AudioPacket>>(data.error());
    if (data.value().isEmpty())
        return mw::err<std::optional<AudioPacket>>(
            errIo(QStringLiteral("eof in the middle of audio packet")));
    p.data = data.value();
    return mw::Result<std::optional<AudioPacket>>::Ok(std::move(p));
}

AudioStream::AudioStream(mw::adb::TcpConn conn, AudioCodecId codec)
    : m_conn(std::move(conn)), m_codec(codec)
{
    m_reader = std::make_unique<TcpReader>(m_conn);
    m_demux = std::make_unique<AudioDemuxer>(*m_reader);
}

// 与 VideoStream 同理：移动必须重建持引用的 reader/demux（防悬垂）。
AudioStream::AudioStream(AudioStream&& other) noexcept
    : m_conn(std::move(other.m_conn)), m_codec(other.m_codec)
{
    m_reader = std::make_unique<TcpReader>(m_conn);
    m_demux = std::make_unique<AudioDemuxer>(*m_reader);
}

AudioStream& AudioStream::operator=(AudioStream&& other) noexcept
{
    if (this != &other) {
        m_conn = std::move(other.m_conn);
        m_codec = other.m_codec;
        m_reader = std::make_unique<TcpReader>(m_conn);
        m_demux = std::make_unique<AudioDemuxer>(*m_reader);
    }
    return *this;
}

mw::Result<AudioStream> AudioStream::create(mw::adb::TcpConn conn)
{
    TcpReader reader(conn, 10000); // 头部读取带 10s 超时（对齐 video 侧就绪判据）
    auto codecBytes = reader.readExact(4);
    if (!codecBytes.ok())
        return mw::err<AudioStream>(codecBytes.error());
    if (codecBytes.value().isEmpty())
        return mw::err<AudioStream>(errProtocol(
            QStringLiteral("audio stream closed before codec id (codec id = none)")));
    const quint32 id = beU32(codecBytes.value().constData());
    if (id == 0) {
        return mw::err<AudioStream>(errProtocol(
            QStringLiteral("device disabled the audio stream (codec id = 0)")));
    }
    if (id == 1) {
        return mw::err<AudioStream>(errProtocol(
            QStringLiteral("audio stream configuration error on device (codec id = 1)")));
    }
    auto codec = audioCodecFromWire(id);
    if (!codec.ok())
        return mw::err<AudioStream>(codec.error());
    return mw::Result<AudioStream>::Ok(AudioStream(std::move(conn), codec.value()));
}

} // namespace mw::scrcpy
