// ffmpeg 解码线程组实现 ← crates/mwine-audio/src/scrcpy.rs
#include "scrcpy.hpp"

#include <QFileInfo>

namespace mw::audio {

DecoderHandle::DecoderHandle(DecoderHandle&& other) noexcept
    : m_feed(std::move(other.m_feed)), m_drain(std::move(other.m_drain)),
      m_ffmpeg(std::move(other.m_ffmpeg)), m_stopping(std::move(other.m_stopping))
{
}

DecoderHandle& DecoderHandle::operator=(DecoderHandle&& other) noexcept
{
    if (this != &other) {
        stop();
        m_feed = std::move(other.m_feed);
        m_drain = std::move(other.m_drain);
        m_ffmpeg = std::move(other.m_ffmpeg);
        m_stopping = std::move(other.m_stopping);
    }
    return *this;
}

void DecoderHandle::stop()
{
    if (m_stopping)
        m_stopping->store(true, std::memory_order_relaxed);
    if (m_ffmpeg)
        m_ffmpeg->killAndReap();
    if (m_drain.joinable())
        m_drain.join();
    // feed 线程可能阻塞在 socket 读：detach，随会话 EOF 自然退出
    if (m_feed.joinable())
        m_feed.detach();
}

QByteArray ensureFlacHeader(QByteArray data)
{
    if (data.startsWith("fLaC"))
        return data;
    QByteArray out;
    out.append("fLaC");
    // last-metadata-block=1, type=STREAMINFO(0) → 0x80；块头共 4 字节（1B 标志 + 3B 长度），长度 34 = 0x000022
    out.append(char(0x80));
    out.append(char(0x00));
    out.append(char(0x00));
    out.append(char(0x22));
    if (data.size() < 34) {
        // 尺寸不足时补零（采样率等由 ffmpeg 从首帧兜底）
        data.resize(34, '\0');
    }
    out += data;
    return out;
}

QString resolveFfmpeg()
{
    {
        // PATH 探测：能启动即视为可用（对齐 Rust status().is_ok() 语义）
        auto probe = ChildProcess::spawnPiped(QStringLiteral("ffmpeg"),
                                              { QStringLiteral("-version") });
        if (probe.ok()) {
            probe.value().killAndReap();
            return QStringLiteral("ffmpeg");
        }
    }
    const QVector<QString> candidates = {
        QStringLiteral("/opt/homebrew/bin/ffmpeg"),
        QStringLiteral("/usr/local/bin/ffmpeg"),
        QStringLiteral("/opt/local/bin/ffmpeg"),
    };
    for (const QString& p : candidates) {
        if (QFileInfo(p).isFile())
            return p;
    }
    return QString();
}

mw::Result<DecoderHandle> spawnDecoder(std::unique_ptr<PacketSource> source, AudioStream target,
                                       const AudioConfig& out)
{
    if (!source)
        return mw::err<DecoderHandle>(errDecode(QStringLiteral("音频源为空")));
    if (source->codec() != mw::scrcpy::AudioCodecId::Flac)
        return mw::err<DecoderHandle>(
            errUnsupportedCodec(QString::fromLatin1(mw::scrcpy::audioCodecName(source->codec()))));

    const QString ffmpeg = resolveFfmpeg();
    if (ffmpeg.isEmpty()) {
        return mw::err<DecoderHandle>(
            errFfmpegNotFound(QStringLiteral("PATH 与常见安装位置均未找到 ffmpeg")));
    }

    auto child = std::make_shared<ChildProcess>();
    {
        auto spawned = ChildProcess::spawnPiped(
            ffmpeg,
            { QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
              QStringLiteral("-f"), QStringLiteral("flac"), QStringLiteral("-i"),
              QStringLiteral("pipe:0"), QStringLiteral("-f"), QStringLiteral("s16le"),
              QStringLiteral("-ar"), QString::number(out.sampleRate), QStringLiteral("-ac"),
              QString::number(out.channels), QStringLiteral("pipe:1") });
        if (!spawned.ok())
            return mw::err<DecoderHandle>(
                errDecode(QStringLiteral("ffmpeg 启动失败: %1").arg(spawned.error())));
        *child = std::move(spawned.value());
    }

    auto stopping = std::make_shared<std::atomic_bool>(false);

    DecoderHandle handle;
    handle.m_ffmpeg = child;
    handle.m_stopping = stopping;

    // feed：source → stdin
    handle.m_feed = std::thread([child, stopping, src = std::move(source)]() mutable {
        while (!stopping->load(std::memory_order_relaxed)) {
            auto packet = src->nextPacket();
            if (!packet.ok())
                break;
            if (!packet.value().has_value())
                break; // 流结束（会话停止）
            const mw::scrcpy::AudioPacket& p = *packet.value();
            const QByteArray bytes = p.config ? ensureFlacHeader(p.data) : p.data;
            if (!child->writeStdin(bytes))
                break; // 解码端已退出
        }
        child->closeStdin(); // EOF → ffmpeg 收尾
        child->wait();      // 收尸（stop() 已杀掉时幂等）
    });

    // 解码线程：ffmpeg stdout（s16le）→ target 回调
    handle.m_drain = std::thread([child, stopping, target]() mutable {
        char chunk[16384];
        for (;;) {
            const qint64 n = child->readStdout(chunk, int(sizeof(chunk)));
            if (n <= 0)
                break;
            // 小端字节对 → s16 样本（丢弃奇数尾字节）
            const qsizetype pairs = qsizetype(n) / 2;
            QVector<qint16> samples;
            samples.reserve(int(pairs));
            const quint8* b = reinterpret_cast<const quint8*>(chunk);
            for (qsizetype i = 0; i < pairs; ++i)
                samples.append(qint16(quint16(b[2 * i]) | (quint16(b[2 * i + 1]) << 8)));
            target.emit(samples);
        }
        (void)stopping;
    });

    return mw::Result<DecoderHandle>::Ok(std::move(handle));
}

} // namespace mw::audio
