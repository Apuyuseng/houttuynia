// 音频参数协商 / 回调流 / qemu 参数组装实现 ← crates/mwine-audio/src/lib.rs
#include "audio.hpp"

namespace mw::audio {

AudioStream::AudioStream(AudioConfig config, PcmCallback callback)
    : m_inner(std::make_shared<Inner>())
{
    m_inner->config = config;
    m_inner->callback = std::move(callback);
}

void AudioStream::emit(const QVector<qint16>& pcm)
{
    if (pcm.isEmpty())
        return;
    m_inner->emits.fetch_add(1, std::memory_order_relaxed);
    m_inner->samples.fetch_add(quint64(pcm.size()), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_inner->cbMutex);
    if (m_inner->callback)
        m_inner->callback(pcm);
}

PcmCallback AudioStream::setCallback(PcmCallback cb)
{
    std::lock_guard<std::mutex> lock(m_inner->cbMutex);
    PcmCallback old = std::move(m_inner->callback);
    m_inner->callback = std::move(cb);
    return old;
}

StreamStats AudioStream::stats() const
{
    StreamStats s;
    s.emits = m_inner->emits.load(std::memory_order_relaxed);
    s.samples = m_inner->samples.load(std::memory_order_relaxed);
    return s;
}

QVector<QString> qemuAudioArgs(QemuAudioMode mode)
{
    if (mode == QemuAudioMode::Native)
        return { QStringLiteral("-audiodev"), QStringLiteral("coreaudio,id=snd0") };
    return { QStringLiteral("-audiodev"), QStringLiteral("none,id=snd0") };
}

} // namespace mw::audio
