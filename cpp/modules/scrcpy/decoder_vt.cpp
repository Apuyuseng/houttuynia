// VideoToolbox 解码器实现（macOS，`__APPLE__` 门控；非 Apple 平台本文件不参与语义）。
// ← crates/mwine-scrcpy/src/decoder.rs
// Rust 侧手写的 8 个 C FFI 声明在此换为系统 C 头（CoreMedia/CoreVideo/
// VideoToolbox/CoreFoundation 均为 C ABI 稳定接口）。
#ifdef __APPLE__

#include "decoder.hpp"

#include <QByteArray>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>

namespace mw::scrcpy {

namespace {

constexpr uint32_t kCMTimeFlagValid = 1;
constexpr uint32_t kAssureMemoryNow = 1 << 1; // kCMBlockBufferAssureMemoryNowFlag
// '420v' = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange（NV12 limited）
constexpr int32_t kNv12VideoRange = 0x34323076;
constexpr int32_t kNv12FullRange = 0x34323230; // '420f'

/// 行对齐到 256 的倍数（GPU 纹理上传的 bytes_per_row 硬性要求）。
qsizetype align256(qsizetype n) { return (n + 255) & ~qsizetype(255); }

CMTime cmTimeMakeUs(quint64 us)
{
    CMTime t;
    t.value = int64_t(us);
    t.timescale = 1000000;
    t.flags = kCMTimeFlagValid;
    t.epoch = 0;
    return t;
}

CMTime cmTimeInvalid()
{
    CMTime t;
    t.value = 0;
    t.timescale = 0;
    t.flags = 0;
    t.epoch = 0;
    return t;
}

} // namespace

// ---- Shared：回调侧共享队列 ----

struct VideoToolboxDecoder::Shared {
    std::mutex mutex;
    std::deque<DecodedFrame> queue;
    quint64 decoded = 0;
};

namespace {

// VT 输出回调：retain 像素缓冲后入队（限延迟：队列只留 3 帧，超限丢最旧）
void vtOutputCallback(void* refcon, void* /*sourceFrameRefCon*/, OSStatus /*status*/,
                      VTDecodeInfoFlags infoFlags, CVImageBufferRef imageBuffer,
                      CMTime /*pts*/, CMTime /*duration*/)
{
    if (imageBuffer == nullptr || (infoFlags & kVTDecodeInfo_FrameDropped) != 0)
        return;
    auto* shared = static_cast<VideoToolboxDecoder::Shared*>(refcon);
    CVPixelBufferRef retained = CVPixelBufferRetain(static_cast<CVPixelBufferRef>(imageBuffer));
    if (retained == nullptr)
        return;
    std::lock_guard<std::mutex> lock(shared->mutex);
    while (shared->queue.size() >= 3)
        shared->queue.pop_front();
    const auto w = quint32(std::max<qsizetype>(CVPixelBufferGetWidth(retained), 0));
    const auto h = quint32(std::max<qsizetype>(CVPixelBufferGetHeight(retained), 0));
    const bool full = CVPixelBufferGetPixelFormatType(retained) == OSType(kNv12FullRange);
    shared->queue.emplace_back(retained, w, h, full);
    CVPixelBufferRelease(retained); // DecodedFrame 构造时已 retain 一份
    shared->decoded += 1;
}

// 用 SPS/PPS（裸 NAL，含 NAL 头字节）建 CMFormatDescription
bool buildFormatDescription(const QVector<QByteArray>& sets, CMVideoFormatDescriptionRef* fd,
                            quint32* w, quint32* h)
{
    QVector<const uint8_t*> ptrs;
    QVector<size_t> sizes;
    ptrs.reserve(sets.size());
    sizes.reserve(sets.size());
    for (const QByteArray& s : sets) {
        ptrs.append(reinterpret_cast<const uint8_t*>(s.constData()));
        sizes.append(size_t(s.size()));
    }
    CMVideoFormatDescriptionRef desc = nullptr;
    const OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        nullptr, CMItemCount(ptrs.size()), ptrs.constData(), sizes.constData(),
        4, // AVCC 长度前缀 4 字节
        &desc);
    if (st != noErr || desc == nullptr)
        return false;
    *fd = desc;
    const CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(desc);
    *w = quint32(std::max<qsizetype>(dims.width, 0));
    *h = quint32(std::max<qsizetype>(dims.height, 0));
    return true;
}

// 输出像素格式钉死 NV12（VideoRange）：双平面直上传 GPU
bool createSession(CMVideoFormatDescriptionRef fd, VideoToolboxDecoder::Shared* shared,
                   VTDecompressionSessionRef* sessionOut)
{
    int32_t pixelFormat = kNv12VideoRange;
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &pixelFormat);
    if (number == nullptr)
        return false;
    const void* keys[] = { kCVPixelBufferPixelFormatTypeKey };
    const void* values[] = { number };
    CFDictionaryRef attrs = CFDictionaryCreate(
        nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(number);
    if (attrs == nullptr)
        return false;

    VTDecompressionOutputCallbackRecord record;
    record.decompressionOutputCallback = vtOutputCallback;
    record.decompressionOutputRefCon = shared;
    VTDecompressionSessionRef session = nullptr;
    const OSStatus st = VTDecompressionSessionCreate(nullptr, fd, nullptr, attrs, &record, &session);
    CFRelease(attrs);
    if (st != noErr || session == nullptr)
        return false;
    *sessionOut = session;
    return true;
}

} // namespace

// ---- DecodedFrame ----

DecodedFrame::DecodedFrame(void* pixelBuffer, quint32 w, quint32 h, bool fullRange)
    : m_pb(pixelBuffer), m_width(w), m_height(h), m_fullRange(fullRange)
{
    if (m_pb)
        CVPixelBufferRetain(static_cast<CVPixelBufferRef>(m_pb));
}

DecodedFrame::~DecodedFrame()
{
    if (m_pb)
        CVPixelBufferRelease(static_cast<CVPixelBufferRef>(m_pb));
}

DecodedFrame::DecodedFrame(DecodedFrame&& other) noexcept
    : m_pb(other.m_pb), m_width(other.m_width), m_height(other.m_height),
      m_fullRange(other.m_fullRange)
{
    other.m_pb = nullptr;
}

DecodedFrame& DecodedFrame::operator=(DecodedFrame&& other) noexcept
{
    if (this != &other) {
        if (m_pb)
            CVPixelBufferRelease(static_cast<CVPixelBufferRef>(m_pb));
        m_pb = other.m_pb;
        m_width = other.m_width;
        m_height = other.m_height;
        m_fullRange = other.m_fullRange;
        other.m_pb = nullptr;
    }
    return *this;
}

mw::Result<Nv12Image> DecodedFrame::toNv12() const
{
    CVPixelBufferRef pb = static_cast<CVPixelBufferRef>(m_pb);
    if (pb == nullptr)
        return mw::err<Nv12Image>(errDecode(QStringLiteral("像素缓冲为空")));
    if (CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess)
        return mw::err<Nv12Image>(errDecode(QStringLiteral("CVPixelBuffer 锁定失败")));

    Nv12Image img;
    const OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    img.fullRange = fmt == OSType(kNv12FullRange);
    if (fmt != OSType(kNv12VideoRange) && !img.fullRange) {
        CVPixelBufferUnlockBaseAddress(pb, 0);
        return mw::err<Nv12Image>(
            errDecode(QStringLiteral("非 NV12 输出（fourcc 0x%1）").arg(quint32(fmt), 0, 16)));
    }
    const qsizetype w = std::max<qsizetype>(CVPixelBufferGetWidth(pb), 0);
    const qsizetype h = std::max<qsizetype>(CVPixelBufferGetHeight(pb), 0);
    const uint8_t* ySrc = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
    const qsizetype ySrcStride =
        std::max<qsizetype>(CVPixelBufferGetBytesPerRowOfPlane(pb, 0), 0);
    const uint8_t* uvSrc = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
    const qsizetype uvSrcStride =
        std::max<qsizetype>(CVPixelBufferGetBytesPerRowOfPlane(pb, 1), 0);
    if (ySrc == nullptr || uvSrc == nullptr || w == 0 || h == 0) {
        CVPixelBufferUnlockBaseAddress(pb, 0);
        return mw::err<Nv12Image>(errDecode(QStringLiteral("像素缓冲平面缺失")));
    }

    img.width = quint32(w);
    img.height = quint32(h);
    img.yStride = align256(w);
    const qsizetype uvRow = 2 * ((w + 1) / 2);
    img.uvStride = align256(uvRow);
    const qsizetype uvH = (h + 1) / 2;
    img.data.resize(int(img.yStride * h + img.uvStride * uvH));
    char* dst = img.data.data();
    for (qsizetype row = 0; row < h; ++row)
        ::memcpy(dst + row * img.yStride, ySrc + row * ySrcStride, size_t(w));
    img.uvOffset = img.yStride * h;
    for (qsizetype row = 0; row < uvH; ++row)
        ::memcpy(dst + img.uvOffset + row * img.uvStride, uvSrc + row * uvSrcStride, size_t(uvRow));

    CVPixelBufferUnlockBaseAddress(pb, 0);
    return mw::Result<Nv12Image>::Ok(std::move(img));
}

// ---- VideoToolboxDecoder ----

VideoToolboxDecoder::VideoToolboxDecoder(void* session, void* formatDesc, Shared* shared,
                                         quint32 w, quint32 h)
    : m_session(session), m_formatDesc(formatDesc), m_shared(shared), m_width(w), m_height(h)
{
}

VideoToolboxDecoder::~VideoToolboxDecoder()
{
    if (m_session) {
        VTDecompressionSessionWaitForAsynchronousFrames(
            static_cast<VTDecompressionSessionRef>(m_session));
        VTDecompressionSessionInvalidate(static_cast<VTDecompressionSessionRef>(m_session));
        CFRelease(static_cast<VTDecompressionSessionRef>(m_session));
    }
    if (m_formatDesc)
        CFRelease(static_cast<CMVideoFormatDescriptionRef>(m_formatDesc));
    if (m_shared) {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->queue.clear();
        delete m_shared;
    }
}

VideoToolboxDecoder::VideoToolboxDecoder(VideoToolboxDecoder&& other) noexcept
    : m_session(other.m_session), m_formatDesc(other.m_formatDesc), m_shared(other.m_shared),
      m_width(other.m_width), m_height(other.m_height)
{
    other.m_session = nullptr;
    other.m_formatDesc = nullptr;
    other.m_shared = nullptr;
}

VideoToolboxDecoder& VideoToolboxDecoder::operator=(VideoToolboxDecoder&& other) noexcept
{
    if (this != &other) {
        this->~VideoToolboxDecoder();
        m_session = other.m_session;
        m_formatDesc = other.m_formatDesc;
        m_shared = other.m_shared;
        m_width = other.m_width;
        m_height = other.m_height;
        other.m_session = nullptr;
        other.m_formatDesc = nullptr;
        other.m_shared = nullptr;
    }
    return *this;
}

mw::Result<VideoToolboxDecoder> VideoToolboxDecoder::create(const QByteArray& config)
{
    const QVector<QByteArray> sets = parameterSets(config);
    if (sets.isEmpty())
        return mw::err<VideoToolboxDecoder>(errDecode(QStringLiteral("config 包中无 SPS/PPS")));
    CMVideoFormatDescriptionRef fd = nullptr;
    quint32 w = 0, h = 0;
    if (!buildFormatDescription(sets, &fd, &w, &h))
        return mw::err<VideoToolboxDecoder>(
            errDecode(QStringLiteral("CMVideoFormatDescription 创建失败")));
    auto* shared = new Shared();
    VTDecompressionSessionRef session = nullptr;
    if (!createSession(fd, shared, &session)) {
        delete shared;
        CFRelease(fd);
        return mw::err<VideoToolboxDecoder>(
            errDecode(QStringLiteral("VTDecompressionSession 创建失败")));
    }
    return mw::Result<VideoToolboxDecoder>::Ok(VideoToolboxDecoder(
        session, const_cast<void*>(static_cast<const void*>(fd)), shared, w, h));
}

mw::Result<bool> VideoToolboxDecoder::reconfigure(const QByteArray& config)
{
    const QVector<QByteArray> sets = parameterSets(config);
    if (sets.isEmpty())
        return mw::err<bool>(errDecode(QStringLiteral("config 包中无 SPS/PPS")));
    CMVideoFormatDescriptionRef fd = nullptr;
    quint32 w = 0, h = 0;
    if (!buildFormatDescription(sets, &fd, &w, &h))
        return mw::err<bool>(errDecode(QStringLiteral("CMVideoFormatDescription 创建失败")));
    VTDecompressionSessionRef session = nullptr;
    if (!createSession(fd, m_shared, &session)) {
        CFRelease(fd);
        return mw::err<bool>(errDecode(QStringLiteral("VTDecompressionSession 创建失败")));
    }
    VTDecompressionSessionWaitForAsynchronousFrames(
        static_cast<VTDecompressionSessionRef>(m_session));
    VTDecompressionSessionInvalidate(static_cast<VTDecompressionSessionRef>(m_session));
    CFRelease(static_cast<VTDecompressionSessionRef>(m_session));
    CFRelease(static_cast<CMVideoFormatDescriptionRef>(m_formatDesc));
    m_session = session;
    m_formatDesc = const_cast<void*>(static_cast<const void*>(fd));
    m_width = w;
    m_height = h;
    return mw::Result<bool>::Ok();
}

mw::Result<bool> VideoToolboxDecoder::decode(quint64 ptsUs, const QByteArray& annexb)
{
    QByteArray avcc;
    avcc.reserve(annexb.size() + 16);
    annexbToAvcc(annexb, avcc);
    if (avcc.isEmpty())
        return mw::Result<bool>::Ok();

    CMBlockBufferRef block = nullptr;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(
        nullptr, nullptr, size_t(avcc.size()), nullptr, nullptr, 0, size_t(avcc.size()),
        kAssureMemoryNow, &block);
    if (st != kCMBlockBufferNoErr || block == nullptr)
        return mw::err<bool>(errDecode(QStringLiteral("CMBlockBuffer 创建失败: %1").arg(st)));
    st = CMBlockBufferReplaceDataBytes(avcc.constData(), block, 0, size_t(avcc.size()));
    if (st != kCMBlockBufferNoErr) {
        CFRelease(block);
        return mw::err<bool>(errDecode(QStringLiteral("CMBlockBuffer 写入失败: %1").arg(st)));
    }

    CMSampleTimingInfo timing;
    timing.duration = cmTimeInvalid();
    timing.presentationTimeStamp = cmTimeMakeUs(ptsUs);
    timing.decodeTimeStamp = cmTimeInvalid();
    size_t sampleSize = size_t(avcc.size());
    CMSampleBufferRef sample = nullptr;
    st = CMSampleBufferCreateReady(nullptr, block,
                                   static_cast<CMVideoFormatDescriptionRef>(m_formatDesc), 1, 1,
                                   &timing, 1, &sampleSize, &sample);
    CFRelease(block);
    if (st != noErr || sample == nullptr)
        return mw::err<bool>(errDecode(QStringLiteral("CMSampleBuffer 创建失败: %1").arg(st)));

    st = VTDecompressionSessionDecodeFrame(static_cast<VTDecompressionSessionRef>(m_session),
                                           sample, 0, nullptr, nullptr);
    CFRelease(sample);
    if (st != noErr)
        return mw::err<bool>(errDecode(QStringLiteral("VT 解码失败: %1").arg(st)));
    return mw::Result<bool>::Ok();
}

std::optional<DecodedFrame> VideoToolboxDecoder::drain()
{
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    if (m_shared->queue.empty())
        return std::nullopt;
    DecodedFrame f = std::move(m_shared->queue.front());
    m_shared->queue.pop_front();
    return std::optional<DecodedFrame>(std::move(f));
}

quint64 VideoToolboxDecoder::decodedCount() const
{
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    return m_shared->decoded;
}

} // namespace mw::scrcpy

#endif // __APPLE__
