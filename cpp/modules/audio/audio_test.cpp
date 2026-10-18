// mw_audio 自测：PcmQueue 纯逻辑 / AudioStream 回调统计 / qemu 参数 /
// ensureFlacHeader / 非 flac 拒绝 / ffmpeg 端到端解码回环。
// 注意：不用 <cassert>（Release 下 NDEBUG 会使 assert 失效），自定义 CHECK。
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "audio.hpp"
#include "coreaudio.hpp"
#include "scrcpy.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            qCritical().noquote() << QStringLiteral("FAIL %1:%2 %3")            \
                                         .arg(QString::fromLatin1(__FILE__),   \
                                              QString::number(__LINE__),        \
                                              QString::fromLatin1(#cond));      \
        }                                                                      \
    } while (0)

static bool nearF(float a, float b, float eps = 1e-3f) { return qAbs(a - b) < eps; }

using namespace mw::audio;

// ---- PcmQueue（与 Rust coreaudio.rs mod tests 同构）----

static void testQueuePrimesThenPops()
{
    PcmQueue q(2, 24000, 4); // 起播阈值 4 帧
    float out[8];            // 4 帧
    q.pull(out, 8, 1.0f, false);
    for (float s : out)
        CHECK(s == 0.0f); // 起播阈值未到应静音

    const QVector<qint16> first = { 16384, -16384, 16384, -16384 }; // 2 帧（0.5 幅度）
    q.pushI16(first);
    q.pull(out, 8, 1.0f, false);
    for (float s : out)
        CHECK(s == 0.0f); // 仍低于阈值

    q.pushI16(QVector<qint16>(12, 32767)); // +6 帧 = 8 帧 ≥ 阈值
    float got[8];
    std::fill(got, got + 8, 1.0f);
    q.pull(got, 8, 1.0f, false);
    CHECK(nearF(got[0], 0.5f) && nearF(got[1], -0.5f));
    CHECK(nearF(got[2], 0.5f) && nearF(got[3], -0.5f));
    for (int i = 4; i < 8; ++i)
        CHECK(nearF(got[i], 1.0f));

    // 剩余 4 帧取尽后，再拉输出静音且不崩溃
    float next[8];
    std::fill(next, next + 8, 1.0f);
    q.pull(next, 8, 1.0f, false);
    for (int i = 0; i < 8; ++i)
        CHECK(nearF(next[i], 1.0f)); // 前 8 样本来自队列（≈1.0）
    float empty[4];
    std::fill(empty, empty + 4, 1.0f);
    q.pull(empty, 4, 1.0f, false);
    for (float s : empty)
        CHECK(s == 0.0f);
}

static void testQueueDropsBacklogBeyondCap()
{
    PcmQueue q(2, 100, 1); // 上限 100 帧
    q.pushI16(QVector<qint16>(200, 0)); // 100 帧
    q.pushI16(QVector<qint16>(200, 0)); // 再压入：触发整段丢弃
    const QueueStats s = q.stats();
    CHECK(s.droppedFrames >= 100);
    CHECK(s.bufferedFrames <= 100);
}

static void testQueueDiscardConsumesSilently()
{
    PcmQueue q(1, 1000, 1);
    q.pushI16(QVector<qint16>(100, 1000));
    float out[50];
    std::fill(out, out + 50, 1.0f);
    q.pull(out, 50, 1.0f, true); // 暂停语义
    for (float s : out)
        CHECK(s == 0.0f);
    CHECK(q.stats().bufferedFrames == 50); // 暂停期间照常消费
}

// ---- AudioStream：emit 统计 / 回调换装（tap）----

static void testAudioStreamStatsAndTap()
{
    AudioStream stream(AudioConfig::stereo48k(),
                       [](const QVector<qint16>&) {});
    stream.emit({ 1, 2, 3, 4 });
    stream.emit({ 5, 6 });
    stream.emit({}); // 空段不计
    const StreamStats s = stream.stats();
    CHECK(s.emits == 2);
    CHECK(s.samples == 6);
    CHECK(stream.config().sampleRate == 48000);
    CHECK(stream.config().channels == 2);

    // 换回调（tap 契约）：setCallback 返回"上一个"回调，取回后仍可调用/再包装
    int hitsA = 0, hitsB = 0;
    PcmCallback original = stream.setCallback([&](const QVector<qint16>&) { ++hitsA; });
    CHECK(original != nullptr); // 构造时传入的无操作回调非空
    stream.emit({ 7 });         // A 触发
    PcmCallback gotA = stream.setCallback([&](const QVector<qint16>&) { ++hitsB; });
    stream.emit({ 8 });         // B 触发
    if (gotA)
        gotA({ 9 }); // tap：手动委托旧回调（录制+播放场景）
    CHECK(hitsA == 2); // A 在安装期触发一次 + 取回后委托一次
    CHECK(hitsB == 1);
}

// ---- qemu 参数组装 ----

static void testQemuAudioArgs()
{
    const QVector<QString> native = qemuAudioArgs(QemuAudioMode::Native);
    CHECK(native == QVector<QString>({ QStringLiteral("-audiodev"),
                                       QStringLiteral("coreaudio,id=snd0") }));
    const QVector<QString> via = qemuAudioArgs(QemuAudioMode::ViaScrcpy);
    CHECK(via == QVector<QString>({ QStringLiteral("-audiodev"), QStringLiteral("none,id=snd0") }));
}

// ---- ensureFlacHeader ----

static void testEnsureFlacHeader()
{
    // 已带头：原样透传
    const QByteArray full = QByteArrayLiteral("fLaC\x80\x00\x22rest-of-file");
    CHECK(ensureFlacHeader(full) == full);

    // 只发 STREAMINFO（且尺寸不足）：补 fLaC + 块头 + 零垫到 34
    QByteArray bare(10, char(7));
    const QByteArray padded = ensureFlacHeader(bare);
    CHECK(padded.size() == 4 + 4 + 34);
    CHECK(padded.startsWith(QByteArrayLiteral("fLaC")));
    CHECK(quint8(padded[4]) == 0x80 && quint8(padded[5]) == 0x00 && quint8(padded[6]) == 0x00
          && quint8(padded[7]) == 0x22);
    CHECK(quint8(padded[8]) == 7); // 原数据保留在 STREAMINFO 位
}

// ---- 非 flac 编码拒绝（不起任何线程/进程）----

namespace {

class OpusSource : public PacketSource {
public:
    mw::Result<std::optional<mw::scrcpy::AudioPacket>> nextPacket() override
    {
        return mw::Result<std::optional<mw::scrcpy::AudioPacket>>::Ok(std::nullopt);
    }
    mw::scrcpy::AudioCodecId codec() const override { return mw::scrcpy::AudioCodecId::Opus; }
};

} // namespace

static void testRejectNonFlacCodec()
{
    AudioStream target(AudioConfig::stereo48k(), [](const QVector<qint16>&) {});
    auto r = spawnDecoder(std::make_unique<OpusSource>(), target, AudioConfig::stereo48k());
    CHECK(r.isErr());
    CHECK(r.error().contains(QStringLiteral("opus")));
}

// ---- 端到端：ffmpeg 生成正弦 FLAC → 解码线程组 → s16 回调 ----

namespace {

/// 解析 FLAC：返回 (裸 STREAMINFO, 元数据块后的音频帧)。
bool splitFlac(const QByteArray& flac, QByteArray* streaminfo, QByteArray* frames)
{
    if (!flac.startsWith(QByteArrayLiteral("fLaC")))
        return false;
    int pos = 4;
    bool gotInfo = false;
    for (;;) {
        if (pos + 4 > flac.size())
            return false;
        const uchar h0 = uchar(flac[pos]);
        const bool last = (h0 & 0x80) != 0;
        const quint32 blockType = h0 & 0x7f;
        const quint32 len = (quint32(uchar(flac[pos + 1])) << 16)
                            | (quint32(uchar(flac[pos + 2])) << 8) | quint32(uchar(flac[pos + 3]));
        if (blockType == 0) {
            if (pos + 4 + int(len) > flac.size() || len != 34)
                return false;
            *streaminfo = flac.mid(pos + 4, int(len));
            gotInfo = true;
        }
        pos += 4 + int(len);
        if (last)
            break;
    }
    if (!gotInfo)
        return false;
    *frames = flac.mid(pos);
    return true;
}

class FakeSource : public PacketSource {
public:
    explicit FakeSource(std::deque<mw::scrcpy::AudioPacket> packets)
        : m_packets(std::move(packets))
    {
    }
    mw::Result<std::optional<mw::scrcpy::AudioPacket>> nextPacket() override
    {
        if (m_packets.empty())
            return mw::Result<std::optional<mw::scrcpy::AudioPacket>>::Ok(std::nullopt);
        mw::scrcpy::AudioPacket p = std::move(m_packets.front());
        m_packets.pop_front();
        return mw::Result<std::optional<mw::scrcpy::AudioPacket>>::Ok(std::move(p));
    }
    mw::scrcpy::AudioCodecId codec() const override { return mw::scrcpy::AudioCodecId::Flac; }

private:
    std::deque<mw::scrcpy::AudioPacket> m_packets;
};

} // namespace

static void testDecodeFlacRoundtrip()
{
    const QString ffmpeg = resolveFfmpeg();
    if (ffmpeg.isEmpty()) {
        qInfo() << "[mw_audio] 无 ffmpeg，跳过解码回环测试";
        return;
    }
    auto gen = ChildProcess::spawnPiped(
        ffmpeg,
        { QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
          QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
          QStringLiteral("aevalsrc=sin(2*PI*440*t):d=0.2:s=48000"), QStringLiteral("-f"),
          QStringLiteral("flac"), QStringLiteral("pipe:1") });
    if (!gen.ok()) {
        qInfo() << "[mw_audio] ffmpeg 启动失败，跳过解码回环测试";
        return;
    }
    ChildProcess& proc = gen.value();
    QByteArray flac;
    {
        char buf[16384];
        qint64 n;
        while ((n = proc.readStdout(buf, int(sizeof(buf)))) > 0)
            flac.append(buf, int(n));
        proc.killAndReap();
    }
    QByteArray streaminfo, frames;
    if (!splitFlac(flac, &streaminfo, &frames)) {
        qInfo() << "[mw_audio] FLAC 解析失败，跳过解码回环测试";
        return;
    }

    std::deque<mw::scrcpy::AudioPacket> packets;
    packets.push_back(mw::scrcpy::AudioPacket{ 0, true, streaminfo }); // config = 裸 STREAMINFO
    packets.push_back(mw::scrcpy::AudioPacket{ 1, false, frames });

    std::mutex mu;
    QVector<qint16> received;
    AudioStream target(AudioConfig::stereo48k(), [&](const QVector<qint16>& pcm) {
        std::lock_guard<std::mutex> lock(mu);
        received += pcm;
    });

    auto handle = spawnDecoder(std::make_unique<FakeSource>(std::move(packets)), target,
                               AudioConfig::stereo48k());
    if (!handle.ok()) {
        qInfo().noquote() << "[mw_audio] 解码线程组启动失败，跳过:" << handle.error();
        return;
    }
    // 0.2s @48k 立体声 ≈ 19200 样本
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (target.stats().samples >= 10000)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    handle.value().stop();

    std::lock_guard<std::mutex> lock(mu);
    qint32 peak = 0;
    for (qint16 s : received)
        peak = qMax<qint32>(peak, qAbs<qint32>(s));
    CHECK(received.size() > 10000);
    CHECK(peak > 8000); // 正弦应有显著幅度
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testQueuePrimesThenPops();
    testQueueDropsBacklogBeyondCap();
    testQueueDiscardConsumesSilently();
    testAudioStreamStatsAndTap();
    testQemuAudioArgs();
    testEnsureFlacHeader();
    testRejectNonFlacCodec();
    testDecodeFlacRoundtrip();

    if (g_failures == 0) {
        qInfo().noquote() << QStringLiteral("mw_audio_test: 全部通过");
        return 0;
    }
    qCritical().noquote() << QStringLiteral("mw_audio_test: %1 项失败").arg(g_failures);
    return 1;
}
