// CoreAudio 输出与桥接实现 ← crates/mwine-audio/src/coreaudio.rs
#include "coreaudio.hpp"

#include <cmath>

#ifdef __APPLE__
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudioTypes.h>
#endif

namespace mw::audio {

// ---- PcmQueue（纯逻辑，全平台）----

PcmQueue::PcmQueue(int channels, qsizetype capFrames, qsizetype preFrames)
    : m_channels(std::max(channels, 1)),
      m_capFrames(std::max<qsizetype>(capFrames, 1)),
      m_preFrames(std::max<qsizetype>(preFrames, 1))
{
}

void PcmQueue::pushI16(const QVector<qint16>& pcm)
{
    if (qsizetype(m_queue.size()) - m_head >= m_capFrames * m_channels) {
        m_droppedFrames += quint64((m_queue.size() - m_head) / m_channels);
        m_queue.clear();
        m_head = 0;
    }
    m_pushedFrames += quint64(pcm.size() / m_channels);
    for (qint16 s : pcm)
        m_queue.append(float(s) / 32768.0f);
}

void PcmQueue::pull(float* out, int n, float gain, bool discard)
{
    if (discard) {
        const int avail = m_queue.size() - m_head;
        const int take = std::min(n, avail);
        m_head += take;
        compact();
        for (int i = 0; i < n; ++i)
            out[i] = 0.0f;
        return;
    }
    const int ch = m_channels;
    const int wantFrames = n / ch;
    if (!m_primed) {
        const int buffered = (m_queue.size() - m_head) / ch;
        if (qsizetype(buffered) < std::max(m_preFrames, qsizetype(wantFrames))) {
            for (int i = 0; i < n; ++i)
                out[i] = 0.0f;
            return;
        }
        m_primed = true;
    }
    for (int i = 0; i < n; ++i) {
        float v = 0.0f;
        if (m_head < m_queue.size()) {
            v = m_queue[m_head] * gain;
            ++m_head;
        }
        out[i] = v;
    }
    compact();
}

void PcmQueue::compact()
{
    if (m_head == m_queue.size()) {
        m_queue.clear();
        m_head = 0;
    } else if (m_head >= 65536) {
        m_queue.remove(0, m_head);
        m_head = 0;
    }
}

PcmQueue::Stats PcmQueue::stats() const
{
    Stats s;
    s.pushedFrames = m_pushedFrames;
    s.droppedFrames = m_droppedFrames;
    s.bufferedFrames = (m_queue.size() - m_head) / m_channels;
    s.primed = m_primed;
    return s;
}

// ---- CoreAudioOutput ----

#ifdef __APPLE__

struct CoreAudioOutput::Inner {
    mutable std::mutex queueMutex;
    PcmQueue queue{1, 1, 1};
    mutable std::mutex volMutex;
    float volume = 1.0f;
    std::atomic<bool> muted{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> closed{false};
    int channels = 2;
    AudioConfig actual{};
    AudioUnit unit = nullptr;
    std::mutex unitMutex; // start/stop/dispose 串行化
    QVector<float> scratch; // 非交错设备的临时交织缓冲（仅回调线程使用）

    float gain() const
    {
        if (muted.load(std::memory_order_relaxed))
            return 0.0f;
        std::lock_guard<std::mutex> lock(volMutex);
        return volume;
    }

    void disposeUnit()
    {
        std::lock_guard<std::mutex> lock(unitMutex);
        if (unit) {
            AudioOutputUnitStop(unit);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            unit = nullptr;
        }
    }
};

namespace {

// 渲染回调：从队列拉样本填 ioData（音量增益 / 暂停丢弃语义都在这里）
OSStatus renderCallback(void* inRefCon, AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                        UInt32 /*inBusNumber*/, UInt32 inNumberFrames, AudioBufferList* ioData)
{
    auto* inner = static_cast<CoreAudioOutput::Inner*>(inRefCon);
    const float gain = inner->gain();
    const bool discard = inner->paused.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(inner->queueMutex);
    if (ioData->mNumberBuffers == 1) {
        // 交错格式：单缓冲连续帧
        float* out = static_cast<float*>(ioData->mBuffers[0].mData);
        const int n = int(inNumberFrames) * inner->channels;
        if (ioData->mBuffers[0].mDataByteSize >= UInt32(n * sizeof(float)))
            inner->queue.pull(out, n, gain, discard);
    } else {
        // 非交错格式：每声道一个缓冲，先拉交织再拆分
        const int ch = int(ioData->mNumberBuffers);
        const int n = int(inNumberFrames) * ch;
        inner->scratch.resize(n);
        inner->queue.pull(inner->scratch.data(), n, gain, discard);
        for (int c = 0; c < ch; ++c) {
            float* dst = static_cast<float*>(ioData->mBuffers[c].mData);
            for (UInt32 f = 0; f < inNumberFrames; ++f)
                dst[f] = inner->scratch[size_t(f) * size_t(ch) + size_t(c)];
        }
    }
    return noErr;
}

} // namespace

mw::Result<std::pair<CoreAudioOutput, AudioConfig>> CoreAudioOutput::open(const AudioConfig& requested)
{
    // 1. 默认输出单元（跟随系统默认设备）
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr)
        return mw::err<std::pair<CoreAudioOutput, AudioConfig>>(
            errNoOutputDevice(QStringLiteral("未找到默认输出设备")));

    auto inner = std::make_shared<Inner>();
    if (AudioComponentInstanceNew(comp, &inner->unit) != noErr || inner->unit == nullptr) {
        return mw::err<std::pair<CoreAudioOutput, AudioConfig>>(
            errOpenFailed(QStringLiteral("AudioComponentInstanceNew 失败")));
    }

    // 2. 取设备默认格式（input scope bus 0 = 我们喂给单元的数据格式）
    AudioStreamBasicDescription defaultFmt{};
    UInt32 fmtSize = sizeof(defaultFmt);
    if (AudioUnitGetProperty(inner->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                             0, &defaultFmt, &fmtSize) != noErr) {
        inner->disposeUnit();
        return mw::err<std::pair<CoreAudioOutput, AudioConfig>>(
            errOpenFailed(QStringLiteral("读取默认流格式失败")));
    }

    // 3. 尝试设成请求的采样率/声道（Float32 交错；单元内部做转换），
    //    失败则回退设备默认配置
    AudioStreamBasicDescription want = defaultFmt;
    want.mSampleRate = double(requested.sampleRate);
    want.mFormatID = kAudioFormatLinearPCM;
    want.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    want.mBitsPerChannel = 32;
    want.mChannelsPerFrame = UInt32(requested.channels);
    want.mFramesPerPacket = 1;
    want.mBytesPerFrame = 4 * want.mChannelsPerFrame;
    want.mBytesPerPacket = want.mBytesPerFrame;
    if (AudioUnitSetProperty(inner->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                             0, &want, sizeof(want)) != noErr) {
        // 回退：恢复默认格式
        AudioUnitSetProperty(inner->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                             0, &defaultFmt, sizeof(defaultFmt));
    }

    // 4. 读回实际生效格式（源端按此重采样对齐）
    AudioStreamBasicDescription actualFmt{};
    fmtSize = sizeof(actualFmt);
    if (AudioUnitGetProperty(inner->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                             0, &actualFmt, &fmtSize) != noErr) {
        inner->disposeUnit();
        return mw::err<std::pair<CoreAudioOutput, AudioConfig>>(
            errOpenFailed(QStringLiteral("读取生效流格式失败")));
    }

    const int channels = int(std::max<UInt32>(actualFmt.mChannelsPerFrame, 1));
    const quint32 rate = quint32(actualFmt.mSampleRate > 0 ? actualFmt.mSampleRate : 48000);
    const qsizetype capFrames = requested.bufferSize > 0
                                    ? qsizetype(requested.bufferSize)
                                    : qsizetype(double(rate) * 0.5); // 0.5s 上限
    const qsizetype preFrames = std::max<qsizetype>(capFrames / 5, 1); // 0.1s 起播阈值

    {
        std::lock_guard<std::mutex> lock(inner->queueMutex);
        inner->queue = PcmQueue(channels, capFrames, preFrames);
    }
    inner->channels = channels;
    inner->actual = AudioConfig{rate, quint16(channels), quint32(capFrames)};

    // 5. 渲染回调 + 启动
    AURenderCallbackStruct cb{};
    cb.inputProc = renderCallback;
    cb.inputProcRefCon = inner.get();
    if (AudioUnitSetProperty(inner->unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Global, 0, &cb, sizeof(cb)) != noErr) {
        inner->disposeUnit();
        return mw::err<std::pair<CoreAudioOutput, AudioConfig>>(
            errOpenFailed(QStringLiteral("设置渲染回调失败")));
    }
    if (AudioUnitInitialize(inner->unit) != noErr) {
        inner->disposeUnit();
        return mw::err<std::pair<CoreAudioOutput, AudioConfig>>(
            errOpenFailed(QStringLiteral("AudioUnitInitialize 失败")));
    }
    if (AudioOutputUnitStart(inner->unit) != noErr) {
        inner->disposeUnit();
        return mw::err<std::pair<CoreAudioOutput, AudioConfig>>(
            errOpenFailed(QStringLiteral("play 失败: AudioOutputUnitStart")));
    }

    CoreAudioOutput out;
    out.m_inner = inner;
    return mw::Result<std::pair<CoreAudioOutput, AudioConfig>>::Ok(
        std::make_pair(out, inner->actual));
}

AudioConfig CoreAudioOutput::actualConfig() const
{
    return m_inner ? m_inner->actual : AudioConfig{};
}

void CoreAudioOutput::pushPcm(const QVector<qint16>& pcm)
{
    if (!m_inner)
        return;
    std::lock_guard<std::mutex> lock(m_inner->queueMutex);
    m_inner->queue.pushI16(pcm);
}

void CoreAudioOutput::setVolume(float volume)
{
    if (!m_inner)
        return;
    const float v = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    std::lock_guard<std::mutex> lock(m_inner->volMutex);
    m_inner->volume = v;
}

float CoreAudioOutput::volume() const
{
    if (!m_inner)
        return 0.0f;
    std::lock_guard<std::mutex> lock(m_inner->volMutex);
    return m_inner->volume;
}

void CoreAudioOutput::setMuted(bool muted)
{
    if (m_inner)
        m_inner->muted.store(muted, std::memory_order_relaxed);
}

bool CoreAudioOutput::muted() const
{
    return m_inner && m_inner->muted.load(std::memory_order_relaxed);
}

mw::Result<bool> CoreAudioOutput::pause()
{
    if (!m_inner || m_inner->closed.load())
        return mw::err<bool>(errNotRunning());
    m_inner->paused.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_inner->unitMutex);
    if (m_inner->unit)
        AudioOutputUnitStop(m_inner->unit);
    return mw::Result<bool>::Ok();
}

mw::Result<bool> CoreAudioOutput::resume()
{
    if (!m_inner || m_inner->closed.load())
        return mw::err<bool>(errNotRunning());
    m_inner->paused.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_inner->unitMutex);
    if (m_inner->unit)
        AudioOutputUnitStart(m_inner->unit);
    return mw::Result<bool>::Ok();
}

QueueStats CoreAudioOutput::stats() const
{
    if (!m_inner)
        return QueueStats{};
    std::lock_guard<std::mutex> lock(m_inner->queueMutex);
    return m_inner->queue.stats();
}

void CoreAudioOutput::close()
{
    if (!m_inner)
        return;
    if (m_inner->closed.exchange(true))
        return;
    m_inner->disposeUnit();
}

#else // !__APPLE__

// 非 Apple 平台：CoreAudio 不可用（TODO：接入对应平台音频输出 API）
struct CoreAudioOutput::Inner {
    AudioConfig actual{};
};

mw::Result<std::pair<CoreAudioOutput, AudioConfig>> CoreAudioOutput::open(const AudioConfig&)
{
    return mw::err<std::pair<CoreAudioOutput, AudioConfig>>(
        errNoOutputDevice(QStringLiteral("本平台暂无音频输出实现（TODO）")));
}

AudioConfig CoreAudioOutput::actualConfig() const { return m_inner ? m_inner->actual : AudioConfig{}; }
void CoreAudioOutput::pushPcm(const QVector<qint16>&) {}
void CoreAudioOutput::setVolume(float) {}
float CoreAudioOutput::volume() const { return 0.0f; }
void CoreAudioOutput::setMuted(bool) {}
bool CoreAudioOutput::muted() const { return false; }
mw::Result<bool> CoreAudioOutput::pause() { return mw::err<bool>(errNotRunning()); }
mw::Result<bool> CoreAudioOutput::resume() { return mw::err<bool>(errNotRunning()); }
QueueStats CoreAudioOutput::stats() const { return QueueStats{}; }
void CoreAudioOutput::close() {}

#endif // __APPLE__

// ---- CoreAudioBridge ----

CoreAudioBridge CoreAudioBridge::withScrcpy(mw::scrcpy::AudioStream audio)
{
    CoreAudioBridge b;
    b.m_source = std::make_unique<ScrcpyAudioSource>(std::move(audio));
    return b;
}

mw::Result<CoreAudioOutput> CoreAudioBridge::output() const
{
    if (m_route)
        return mw::Result<CoreAudioOutput>::Ok(m_route->output);
    return mw::err<CoreAudioOutput>(errNotRunning());
}

mw::Result<QueueStats> CoreAudioBridge::stats() const
{
    auto o = output();
    if (!o.ok())
        return mw::err<QueueStats>(o.error());
    return mw::Result<QueueStats>::Ok(o.value().stats());
}

mw::Result<bool> CoreAudioBridge::setVolume(float volume) const
{
    auto o = output();
    if (!o.ok())
        return mw::err<bool>(o.error());
    o.value().setVolume(volume);
    return mw::Result<bool>::Ok();
}

mw::Result<bool> CoreAudioBridge::setMuted(bool muted) const
{
    auto o = output();
    if (!o.ok())
        return mw::err<bool>(o.error());
    o.value().setMuted(muted);
    return mw::Result<bool>::Ok();
}

mw::Result<bool> CoreAudioBridge::pause() const
{
    auto o = output();
    if (!o.ok())
        return mw::err<bool>(o.error());
    return o.value().pause();
}

mw::Result<bool> CoreAudioBridge::resume() const
{
    auto o = output();
    if (!o.ok())
        return mw::err<bool>(o.error());
    return o.value().resume();
}

mw::Result<AudioStream> CoreAudioBridge::start(const AudioConfig& config)
{
    if (m_route)
        return mw::err<AudioStream>(errAlreadyRunning());

    auto opened = CoreAudioOutput::open(config);
    if (!opened.ok())
        return mw::err<AudioStream>(opened.error());
    CoreAudioOutput output = opened.value().first;
    const AudioConfig actual = opened.value().second;

    // 默认回调：PCM 直入输出队列
    CoreAudioOutput sink = output;
    AudioStream stream(actual, [sink](const QVector<qint16>& pcm) mutable { sink.pushPcm(pcm); });

    std::optional<DecoderHandle> decoder;
    if (m_source) {
        auto d = spawnDecoder(std::move(m_source), stream, actual);
        if (!d.ok()) {
            output.close(); // 解码线程组起不来时释放已开设备
            return mw::err<AudioStream>(d.error());
        }
        decoder = std::move(d.value());
    }
    // 纯输出路由（无源）：emit 手动驱动（测试/未来 gRPC 路径）

    m_route = Route{ output, std::move(decoder) };
    return mw::Result<AudioStream>::Ok(stream);
}

mw::Result<bool> CoreAudioBridge::stop()
{
    if (m_route) {
        if (m_route->decoder)
            m_route->decoder->stop();
        m_route->output.close();
        m_route.reset();
    }
    return mw::Result<bool>::Ok();
}

bool CoreAudioBridge::isRunning() const
{
    return m_route.has_value();
}

} // namespace mw::audio
