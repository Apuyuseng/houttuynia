// VideoToolbox H.264 硬件解码（macOS）。
// ← crates/mwine-scrcpy/src/decoder.rs（Rust 手写 FFI 声明 → C++ 直接用系统 C 头）
//
// scrcpy server 送的是 Annex-B 裸流（config 包 = SPS/PPS，帧包 = 起始码分隔
// 的 NAL 序列），而 VideoToolbox 的输入是 AVCC（4 字节长度前缀），参数集要
// 单独建成 CMFormatDescription。转换逻辑（splitAnnexB / annexbToAvcc /
// parameterSets）是纯函数，带字节级测试。
//
// 参数集处理与 Rust 版一致：仅 H.264（CMVideoFormatDescriptionCreateFrom-
// H264ParameterSets）；H.265 需 HEVC 参数集变体（TODO），AV1 同。
//
// 输出形态：解码帧以 CVPixelBuffer（NV12）经 DecodedFrame::toNv12 拷贝交出，
// 或经 pixelBuffer() 直接取 CVPixelBufferRef —— player.rs（winit/wgpu 窗口
// 管线）不移植，未来 Qt 播放器从这里消费。
#pragma once

#include <QByteArray>
#include <QVector>

#include <optional>

#include "error.hpp"
#include "result.hpp"

namespace mw::scrcpy {

// ---- Annex-B 解析（纯函数，全平台可用）----

/// 切出所有 NAL（含 NAL 头字节，不含起始码）。无起始码时返回空。
/// 4 字节起始码（00 00 00 01）由扫描天然命中其后 3 字节；前一个 NAL
/// 残留的尾零（trailing_zero_8bits）对解码无影响，照实保留。
QVector<QByteArray> splitAnnexB(const QByteArray& data);

/// 从 Annex-B 流提取参数集（SPS=7 / PPS=8），保持出现顺序。
QVector<QByteArray> parameterSets(const QByteArray& config);

/// Annex-B → AVCC：4 字节大端长度前缀；剔除 AUD(9) 与 filler(12)。
void annexbToAvcc(const QByteArray& data, QByteArray& out);

#ifdef __APPLE__

struct Nv12Image;

/// 一帧解码输出（NV12，持有 CVPixelBuffer 引用）。
class DecodedFrame {
public:
    DecodedFrame() = default;
    explicit DecodedFrame(void* pixelBuffer, quint32 w, quint32 h, bool fullRange);
    ~DecodedFrame();
    DecodedFrame(DecodedFrame&& other) noexcept;
    DecodedFrame& operator=(DecodedFrame&& other) noexcept;
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;

    quint32 width() const { return m_width; }
    quint32 height() const { return m_height; }
    bool fullRange() const { return m_fullRange; }
    /// 裸 CVPixelBufferRef（未来 Qt 播放器可直接交 AVSampleBufferDisplayLayer）。
    void* pixelBuffer() const { return m_pb; }

    /// 锁定基址并拷贝出双平面（行对齐 256，可直接喂 GPU 上传）。
    mw::Result<Nv12Image> toNv12() const;

private:
    void* m_pb = nullptr; // CVPixelBufferRef（引用计数持有）
    quint32 m_width = 0;
    quint32 m_height = 0;
    bool m_fullRange = false;
};

/// 拷贝出的 NV12 双平面，行已 pad 到 256 对齐。
struct Nv12Image {
    quint32 width = 0;
    quint32 height = 0;
    bool fullRange = false;
    QByteArray data;
    qsizetype yStride = 0;
    qsizetype uvStride = 0;
    /// data 中 UV 平面的起始偏移。
    qsizetype uvOffset = 0;

    /// UV 平面的字节行宽（每样本 2 字节：CbCr 交错）。
    qsizetype uvRowBytes() const { return 2 * ((qsizetype(width) + 1) / 2); }
    /// UV 平面的行数。
    quint32 uvHeight() const { return (height + 1) / 2; }
};

/// H.264 硬件解码器（Apple Silicon 上直走专用解码单元，CPU 占用≈0）。
class VideoToolboxDecoder {
public:
    VideoToolboxDecoder() = default;
    ~VideoToolboxDecoder();
    VideoToolboxDecoder(const VideoToolboxDecoder&) = delete;
    VideoToolboxDecoder& operator=(const VideoToolboxDecoder&) = delete;
    // mw::Result<VideoToolboxDecoder> 按值返回需要移动语义（所有权整体转移）
    VideoToolboxDecoder(VideoToolboxDecoder&& other) noexcept;
    VideoToolboxDecoder& operator=(VideoToolboxDecoder&& other) noexcept;

    /// 回调侧共享队列（实现细节；公开以便 .cpp 内自由函数命名）。
    struct Shared;

    /// 用 config 包（Annex-B SPS/PPS）建立解码会话。
    static mw::Result<VideoToolboxDecoder> create(const QByteArray& config);

    quint32 width() const { return m_width; }
    quint32 height() const { return m_height; }

    /// 分辨率变化（flex display 重协商）后用新 SPS/PPS 重建会话。
    mw::Result<bool> reconfigure(const QByteArray& config);

    /// 送入一帧（Annex-B）。输出经回调进入内部队列，用 drain() 取。
    mw::Result<bool> decode(quint64 ptsUs, const QByteArray& annexb);

    /// 取出一帧解码输出（无则空）。
    std::optional<DecodedFrame> drain();

    /// 累计解码帧数（含已被队列丢弃的）。
    quint64 decodedCount() const;

private:
    VideoToolboxDecoder(void* session, void* formatDesc, Shared* shared, quint32 w, quint32 h);

    void* m_session = nullptr;     // VTDecompressionSessionRef
    void* m_formatDesc = nullptr;  // CMVideoFormatDescriptionRef
    Shared* m_shared = nullptr;    // 回调侧共享队列（堆分配，refcon 指向它）
    quint32 m_width = 0;
    quint32 m_height = 0;
};

#endif // __APPLE__

} // namespace mw::scrcpy
