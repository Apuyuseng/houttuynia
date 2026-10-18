// 音频流解复用：codec id + 帧包（PTS/flags 前缀）。
// 与 server 端 Streamer.java 对齐，大端；音频包只使用 config 标志位。
// ← crates/mwine-scrcpy/src/audio.rs
#pragma once

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>

#include "tcpconn.hpp"
#include "error.hpp"
#include "io.hpp"
#include "result.hpp"

namespace mw::scrcpy {

// 与 video.hpp 的 kVideoPacketFlagConfig 值不同（音频无 keyframe/session 位）。
inline constexpr quint64 kAudioPacketFlagConfig = quint64(1) << 63;

enum class AudioCodecId { Flac, Opus, Aac };

inline quint32 audioCodecWireId(AudioCodecId c)
{
    switch (c) {
    case AudioCodecId::Flac: return 0x666c6163; // "flac"
    case AudioCodecId::Opus: return 0x6f707573; // "opus"
    case AudioCodecId::Aac: return 0x61616363;  // "aac"
    }
    return 0;
}

inline mw::Result<AudioCodecId> audioCodecFromWire(quint32 id)
{
    switch (id) {
    case 0x666c6163: return mw::Result<AudioCodecId>::Ok(AudioCodecId::Flac);
    case 0x6f707573: return mw::Result<AudioCodecId>::Ok(AudioCodecId::Opus);
    case 0x61616363: return mw::Result<AudioCodecId>::Ok(AudioCodecId::Aac);
    default:
        return mw::err<AudioCodecId>(
            errProtocol(QStringLiteral("unknown audio codec id: 0x%1").arg(id, 0, 16)));
    }
}

inline const char* audioCodecName(AudioCodecId c)
{
    switch (c) {
    case AudioCodecId::Flac: return "flac";
    case AudioCodecId::Opus: return "opus";
    case AudioCodecId::Aac: return "aac";
    }
    return "";
}

/// 一个音频包（config 包为解码器初始化数据，如 FLAC 头）。
struct AudioPacket {
    quint64 pts = 0;
    bool config = false;
    QByteArray data;
};

/// 音频包解析核心，泛型于 reader（Rust AudioDemuxer<R>）。
class AudioDemuxer {
public:
    explicit AudioDemuxer(StreamReader& reader) : m_reader(reader) {}

    /// 读取下一个音频包；流结束返回空（nullopt）。
    mw::Result<std::optional<AudioPacket>> nextPacket();

private:
    StreamReader& m_reader;
};

/// 从 socket 读取并解析音频流。
class AudioStream {
public:
    /// 空流（moved-from / 未初始化）：nextPacket 返回协议错误。
    AudioStream() = default;

    /// 读取流头部：4 字节 codec id。
    static mw::Result<AudioStream> create(mw::adb::TcpConn conn);

    AudioStream(AudioStream&& other) noexcept;
    AudioStream& operator=(AudioStream&& other) noexcept;

    bool isValid() const { return m_demux != nullptr; }
    AudioCodecId codec() const { return m_codec; }
    /// 读取下一个音频包；流结束返回空。
    mw::Result<std::optional<AudioPacket>> nextPacket()
    {
        if (!m_demux)
            return mw::err<std::optional<AudioPacket>>(errProtocol(QStringLiteral("audio stream 空")));
        return m_demux->nextPacket();
    }
    /// 裸访问底层流（录制整流等用途）。
    mw::adb::TcpConn& stream() { return m_conn; }

private:
    AudioStream(mw::adb::TcpConn conn, AudioCodecId codec);

    // reader/demux 持有 m_conn 的引用，只能堆分配（引用成员会删除移动赋值，
    // 使 mw::Result<AudioStream> 无法按值返回）；成员顺序即析构顺序。
    mw::adb::TcpConn m_conn;
    std::unique_ptr<TcpReader> m_reader;
    std::unique_ptr<AudioDemuxer> m_demux;
    AudioCodecId m_codec = AudioCodecId::Flac;
};

} // namespace mw::scrcpy
