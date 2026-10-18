// 视频流解复用：codec id + session meta + 帧包（PTS/flags 前缀）。
// 与 server 端 Streamer.java 对齐，大端。
// ← crates/mwine-scrcpy/src/video.rs
#pragma once

#include <QByteArray>
#include <QString>
#include <QPair>

#include <memory>
#include <optional>

#include "tcpconn.hpp"
#include "error.hpp"
#include "io.hpp"
#include "options.hpp"
#include "result.hpp"

namespace mw::scrcpy {

// Rust 侧 video.rs / audio.rs 各自定义 PACKET_FLAG_*；合入同一命名空间后
// 按流类型加前缀消歧（音频的 config 标志位值不同，见 audio.hpp）。
inline constexpr quint64 kVideoPacketFlagSession = quint64(1) << 63;
inline constexpr quint64 kVideoPacketFlagConfig = quint64(1) << 62;
inline constexpr quint64 kVideoPacketFlagKeyFrame = quint64(1) << 61;

/// 一个视频包（可能是配置包 SPS/PPS 或实际帧数据）。
struct VideoPacket {
    quint64 pts = 0;
    bool config = false;
    bool keyFrame = false;
    QByteArray data;
};

/// 视频包解析核心，泛型于 reader（Rust PacketDemuxer<R>）。
class PacketDemuxer {
public:
    explicit PacketDemuxer(StreamReader& reader) : m_reader(reader) {}

    quint32 width() const { return m_width; }
    quint32 height() const { return m_height; }

    /// 读取下一个视频包；同时吸收中途出现的 session meta（分辨率变化）。
    /// 流结束返回空（nullopt）。
    mw::Result<std::optional<VideoPacket>> nextPacket();

private:
    StreamReader& m_reader;
    quint32 m_width = 0;
    quint32 m_height = 0;
};

/// 读取视频 socket 的连接头：1 字节 dummy + 64 字节设备名。
/// 设备名在 server 完成 video+control 双 accept 之后才发送。
mw::Result<QString> readConnectionHeader(StreamReader& reader);

/// 仅读取 64 字节设备名（dummy 字节已被连接流程消费时使用）。
mw::Result<QString> readDeviceName(StreamReader& reader);

/// 从 socket 读取并解析视频流。
class VideoStream {
public:
    /// 空流（moved-from / 未初始化）：nextPacket 返回协议错误。
    VideoStream() = default;

    /// 读取流头部：4 字节 codec id。
    /// 就绪判据必须带超时：编码器初始化失败/极慢（冷启 guest 实测会出现）
    /// 时 server 永不发送 codec id，无超时会把客户端挂死在这里。
    static mw::Result<VideoStream> create(mw::adb::TcpConn conn);

    VideoStream(VideoStream&& other) noexcept;
    VideoStream& operator=(VideoStream&& other) noexcept;

    bool isValid() const { return m_demux != nullptr; }
    VideoCodecId codec() const { return m_codec; }
    QPair<quint32, quint32> size() const
    {
        return qMakePair(m_demux ? m_demux->width() : 0u, m_demux ? m_demux->height() : 0u);
    }
    mw::Result<std::optional<VideoPacket>> nextPacket()
    {
        if (!m_demux)
            return mw::err<std::optional<VideoPacket>>(errProtocol(QStringLiteral("video stream 空")));
        return m_demux->nextPacket();
    }
    /// 裸访问底层流（录制整流等用途）。
    mw::adb::TcpConn& stream() { return m_conn; }

private:
    VideoStream(mw::adb::TcpConn conn, VideoCodecId codec);

    // reader/demux 持有 m_conn 的引用，只能堆分配（引用成员会删除移动赋值，
    // 使 mw::Result<VideoStream> 无法按值返回）；成员顺序即析构顺序。
    mw::adb::TcpConn m_conn;
    std::unique_ptr<TcpReader> m_reader;
    std::unique_ptr<PacketDemuxer> m_demux;
    VideoCodecId m_codec = VideoCodecId::H264;
};

} // namespace mw::scrcpy
