// CoreAudio 输出引擎（macOS，AudioUnit C API）+ CoreAudioBridge（路径 2 组装）。
// ← crates/mwine-audio/src/coreaudio.rs（Rust 经 cpal → C++ 直接用 C API）
//
// 与 Rust 版的差异：cpal 的 Stream 是 !Send（需要专属 owner 线程停泊），
// AudioUnit C API 无此限制（AudioOutputUnitStart/Stop 可跨线程调用），
// 故去掉 owner 线程与命令通道——play/pause/close 直接驱动 AudioUnit，
// 队列/音量/静音语义保持一致。
//
// PcmQueue（交错 f32 样本队列）是纯逻辑，全平台可测试：
// 延迟策略与 Rust 一致：超上限丢积压（限制延迟），起播前攒阈值（避免起步欠载爆音）。
#pragma once

#include <QVector>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "audio.hpp"
#include "result.hpp"
#include "scrcpy.hpp"

namespace mw::audio {

/// 交错 f32 样本队列：生产者 push，渲染回调拉取。
class PcmQueue {
public:
    PcmQueue(int channels, qsizetype capFrames, qsizetype preFrames);

    /// 压入交错 s16（调用方保证声道数与队列一致；ffmpeg 侧已重采样对齐）。
    /// 压入前若积压已达上限，整段丢弃（不清新数据，保持延迟有界）。
    void pushI16(const QVector<qint16>& pcm);

    /// 回调拉取：填满 out（不足输出静音）并乘增益。
    /// `discard = true`（暂停语义）：照常消费但输出静音，防止积压失控。
    void pull(float* out, int n, float gain, bool discard);

private:
    QVector<float> m_queue;
    int m_head = 0; // 已消费前缀长度（拉取用；避免每次 pop 的 O(n) 移动）
    void compact(); // 消费后回收前缀空间
    int m_channels = 1;
    qsizetype m_capFrames = 1;
    qsizetype m_preFrames = 1;
    bool m_primed = false;
    quint64 m_pushedFrames = 0;
    quint64 m_droppedFrames = 0;

public:
    /// 输出队列统计（延迟观测）
    struct Stats {
        /// 累计压入帧数
        quint64 pushedFrames = 0;
        /// 因超上限丢弃的帧数（延迟控制）
        quint64 droppedFrames = 0;
        /// 当前缓冲帧数
        qsizetype bufferedFrames = 0;
        /// 是否已过起播阈值
        bool primed = false;
    };
    Stats stats() const;
};

using QueueStats = PcmQueue::Stats;

/// CoreAudio 输出句柄，浅拷贝（共享队列与设备）。
/// close 后所有副本一同失效。
class CoreAudioOutput {
public:
    CoreAudioOutput() = default;

    /// 打开默认输出设备。返回句柄与实际生效参数（源端按此重采样对齐）。
    /// 请求的采样率/声道不被支持时回退设备默认配置。
    static mw::Result<std::pair<CoreAudioOutput, AudioConfig>> open(const AudioConfig& requested);

    /// 实际生效参数
    AudioConfig actualConfig() const;
    /// 压入交错 s16 PCM（按 actual 参数；上游已重采样，此处不做声道转换）
    void pushPcm(const QVector<qint16>& pcm);
    /// 软件音量（0.0..=1.0，回调内乘增益）
    void setVolume(float volume);
    float volume() const;
    void setMuted(bool muted);
    bool muted() const;
    /// 暂停输出：设备暂停 + 回调侧消费丢弃，双保险防积压
    mw::Result<bool> pause();
    mw::Result<bool> resume();
    QueueStats stats() const;
    /// 停止并释放设备（幂等；所有副本一同失效）
    void close();

    bool valid() const { return m_inner != nullptr; }

    /// 内部共享状态（渲染回调 refcon 指向它；public 供 coreaudio.cpp 回调访问）
    struct Inner;
    std::shared_ptr<Inner> m_inner;
};

/// 路径 2 组装：scrcpy 音频源（guest）→ flac 解码 → CoreAudio 输出。
///
/// 用法：scrcpy 会话 split() 拆出音频 socket 后经 CoreAudioBridge::withScrcpy
/// 注入；qemu 应以 qemuAudioArgs(QemuAudioMode::ViaScrcpy)（-audiodev none）
/// 启动，避免 qemu CoreAudio 与 scrcpy 捕获双重出声。
class CoreAudioBridge : public AudioBridge {
public:
    CoreAudioBridge() = default;
    ~CoreAudioBridge() override { (void)stop(); }

    CoreAudioBridge(const CoreAudioBridge&) = delete;
    CoreAudioBridge& operator=(const CoreAudioBridge&) = delete;
    CoreAudioBridge(CoreAudioBridge&&) = default;
    CoreAudioBridge& operator=(CoreAudioBridge&&) = default;

    /// 注入 scrcpy 音频源；start 后开始解码路由
    static CoreAudioBridge withScrcpy(mw::scrcpy::AudioStream audio);

    /// 当前路由的输出统计（延迟观测）
    mw::Result<QueueStats> stats() const;
    mw::Result<bool> setVolume(float volume) const;
    mw::Result<bool> setMuted(bool muted) const;
    mw::Result<bool> pause() const;
    mw::Result<bool> resume() const;

    // ---- AudioBridge ----
    mw::Result<AudioStream> start(const AudioConfig& config) override;
    mw::Result<bool> stop() override;
    bool isRunning() const override;

private:
    struct Route {
        CoreAudioOutput output;
        std::optional<DecoderHandle> decoder;
    };
    mw::Result<CoreAudioOutput> output() const;

    std::unique_ptr<PacketSource> m_source;
    std::optional<Route> m_route;
};

} // namespace mw::audio
