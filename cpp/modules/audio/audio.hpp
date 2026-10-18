// mw::audio —— guest 音频 → macOS 输出路由。
// ← crates/mwine-audio/src/lib.rs
//
// 候选路径：
// 1. qemu `-audiodev coreaudio` 直出——零代码但路由/音量不可控（未采用）
// 2. qemu `-audiodev none` + scrcpy 音频流——可编程，本模块的实现路径
// 3. qemu gRPC audio——待后续调研，接口存在时另加 AudioBridge 实现
//
// 接口先行：mwine-core 只面向 AudioBridge 组装；
// 具体实现（CoreAudioBridge）在 coreaudio.hpp，接线时才被引用。
//
// 注意区分两个同名概念：mw::scrcpy::AudioStream 是压缩包 socket（flac 等），
// 本模块的 AudioStream 是解码后的交错 s16 PCM 回调流。
//
// 组装面子模块按需包含：coreaudio.hpp（CoreAudio 输出引擎）与
// scrcpy.hpp（ffmpeg 解码线程组）——本文件保持自包含，二者经
// "audio.hpp" 引用这里的类型，避免环形包含。
#pragma once

#include <QVector>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "result.hpp"

namespace mw::audio {

// ---- 错误文案构造（Rust enum Error → QString 约定）----
inline QString errNoOutputDevice(const QString& m) { return QStringLiteral("无音频输出设备: ") + m; }
inline QString errOpenFailed(const QString& m) { return QStringLiteral("音频输出流打开失败: ") + m; }
inline QString errAlreadyRunning() { return QStringLiteral("音频路由已在运行"); }
inline QString errNotRunning() { return QStringLiteral("音频路由未启动"); }
inline QString errFfmpegNotFound(const QString& m)
{
    return QStringLiteral("ffmpeg 未找到（brew install ffmpeg）: ") + m;
}
inline QString errUnsupportedCodec(const QString& m)
{
    return QStringLiteral("不支持的音频编码: %1（当前仅 flac）").arg(m);
}
inline QString errDecode(const QString& m) { return QStringLiteral("解码失败: ") + m; }
inline QString errIo(const QString& m) { return QStringLiteral("io error: ") + m; }

/// PCM 回调：一段交错 s16 样本（按实际生效配置的采样率/声道数）。
using PcmCallback = std::function<void(const QVector<qint16>&)>;

/// 音频参数。`start` 传入的是期望值，实际以设备为准
///（经 AudioStream::config 返回），源端应重采样对齐。
struct AudioConfig {
    /// 采样率（Hz）
    quint32 sampleRate = 48000;
    /// 交错声道数
    quint16 channels = 2;
    /// 输出缓冲上限（帧）。0 = 自动（0.5s）；积压超限即丢弃，限制延迟
    quint32 bufferSize = 0;

    /// 常用默认：立体声 48kHz，缓冲自动
    static AudioConfig stereo48k() { return AudioConfig{48000, 2, 0}; }
};

/// AudioStream 的累计统计
struct StreamStats {
    /// emit 调用次数
    quint64 emits = 0;
    /// 累计样本数（含声道）
    quint64 samples = 0;
};

/// 运行中的音频流：guest 侧解出的每段 PCM 都会送达回调
///（默认回调为 CoreAudio 输出队列，可替换以实现录制/电平表）。
///
/// 桥接实现的解码线程持有副本并调用 emit；
/// 调用方持有的副本用于换回调/查统计。副本共享同一底层状态。
class AudioStream {
public:
    /// 手动构造流（mock/自定义路由用；生产路径走 AudioBridge::start）。
    AudioStream(AudioConfig config, PcmCallback callback);

    /// 实际生效参数
    AudioConfig config() const { return m_inner->config; }

    /// 送一段交错 s16 PCM 给回调（由解码线程驱动；测试/手动路由可直接调用）。
    void emit(const QVector<qint16>& pcm);

    /// 换回调并返回旧回调（包装旧回调可实现 tap：既播放又录制）。
    PcmCallback setCallback(PcmCallback cb);

    StreamStats stats() const;

private:
    struct Inner {
        AudioConfig config;
        std::mutex cbMutex;
        PcmCallback callback;
        std::atomic<quint64> emits{0};
        std::atomic<quint64> samples{0};
    };
    std::shared_ptr<Inner> m_inner;
};

/// 音频路由：mwine-core 的组装面（仅依赖此接口，不依赖具体实现）。
class AudioBridge {
public:
    virtual ~AudioBridge() = default;
    /// 打开路由。config 为期望值，实际设备参数以返回流的 config() 为准。
    virtual mw::Result<AudioStream> start(const AudioConfig& config) = 0;
    /// 停止并释放资源（幂等）。
    virtual mw::Result<bool> stop() = 0;
    virtual bool isRunning() const = 0;
};

/// qemu 音频后端模式：mwine-qemu 组装命令行时取用。
enum class QemuAudioMode {
    /// 路径 1：qemu 直出 host CoreAudio——零代码，但路由/音量不可控
    Native,
    /// 路径 2（默认）：qemu 侧静音，guest 音频经 scrcpy 音频流带出
    ViaScrcpy,
};

/// 组装 qemu `-audiodev` 参数。ViaScrcpy 必须静音 qemu 侧，
/// 否则同一音频既走 qemu CoreAudio 又走 scrcpy 捕获，双重出声。
QVector<QString> qemuAudioArgs(QemuAudioMode mode);

} // namespace mw::audio
