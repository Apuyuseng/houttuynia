// scrcpy 音频源（路径 2）：把 mw::scrcpy::AudioStream（FLAC 压缩包）
// 解码为交错 s16 PCM，送入 AudioStream 回调。
// ← crates/mwine-audio/src/scrcpy.rs
//
// 解码用 ffmpeg 子进程（与 mwine-player 一致，重采样对齐输出参数）；
// 将来换原生解码器不影响本模块接口。
#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>

#include "audio.hpp"
#include "childproc.hpp"
#include "result.hpp"
#include "scrcpy/audio.hpp"

namespace mw::audio {

/// 音频包源。生产实现是 scrcpy socket；抽象出来便于离线注入 mock 测试解码管线。
class PacketSource {
public:
    virtual ~PacketSource() = default;
    virtual mw::Result<std::optional<mw::scrcpy::AudioPacket>> nextPacket() = 0;
    virtual mw::scrcpy::AudioCodecId codec() const = 0;
};

/// mw::scrcpy::AudioStream 的 PacketSource 适配（持有 socket 流）。
class ScrcpyAudioSource : public PacketSource {
public:
    explicit ScrcpyAudioSource(mw::scrcpy::AudioStream stream) : m_stream(std::move(stream)) {}

    mw::Result<std::optional<mw::scrcpy::AudioPacket>> nextPacket() override
    {
        return m_stream.nextPacket();
    }
    mw::scrcpy::AudioCodecId codec() const override { return m_stream.codec(); }

private:
    mw::scrcpy::AudioStream m_stream;
};

/// 解码线程组句柄。
class DecoderHandle {
public:
    DecoderHandle() = default;
    ~DecoderHandle() { stop(); }

    DecoderHandle(const DecoderHandle&) = delete;
    DecoderHandle& operator=(const DecoderHandle&) = delete;
    DecoderHandle(DecoderHandle&& other) noexcept;
    DecoderHandle& operator=(DecoderHandle&& other) noexcept;

    /// 停止解码（幂等）。杀掉 ffmpeg 并等待读线程退出；
    /// feed 线程可能阻塞在 scrcpy socket 读上——不 join，随会话结束（EOF）自然退出，滞留无害。
    void stop();

private:
    friend mw::Result<DecoderHandle> spawnDecoder(std::unique_ptr<PacketSource> source,
                                                  AudioStream target, const AudioConfig& out);

    std::thread m_feed;
    std::thread m_drain;
    std::shared_ptr<ChildProcess> m_ffmpeg;
    std::shared_ptr<std::atomic_bool> m_stopping;
};

/// 启动解码线程组：source（flac 包）→ ffmpeg（s16le，重采样对齐 out）→ target.emit。
/// 编码非 flac / 缺 ffmpeg 时返回 Err（不起任何线程）。
mw::Result<DecoderHandle> spawnDecoder(std::unique_ptr<PacketSource> source, AudioStream target,
                                       const AudioConfig& out);

/// server 的 FLAC config 包应为 "fLaC" + 块头 + STREAMINFO；
/// 兼容只发 STREAMINFO 的实现（补上文件头前缀）。
QByteArray ensureFlacHeader(QByteArray data);

/// 查找 ffmpeg 可执行文件（PATH 优先，其次常见 macOS 安装位置）。
/// 未找到返回空串。
QString resolveFfmpeg();

} // namespace mw::audio
