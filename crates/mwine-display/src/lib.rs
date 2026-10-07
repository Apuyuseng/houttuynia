//! mwine-display（显示管线）：从 qemu/emulator 获取帧缓冲，供 macOS
//! 原生窗口渲染。
//!
//! 接口定义：
//! - [`DisplayBackend`]：显示器管理（增删查 + 单帧快照）
//! - [`FrameSource`]：连续帧流（变化驱动或轮询，取决于后端模式）
//!
//! 后端实现：
//! - [`grpc`][mod@grpc]（首选）：emulator 内置 gRPC（`-grpc <port>`），
//!   `streamScreenshot` 变化驱动流 + `getScreenshot` 轮询兜底 +
//!   `get/setDisplayConfigurations` 多显示器管理。headless 模式实测可用。
//! - [`vnc`][mod@vnc]（备选）：qemu VNC server（需 `-qemu -vnc` 透传启动），
//!   纯 std RFB 客户端。
//! - [`render`][mod@render]（feature = "render"）：winit + wgpu(Metal) 窗口
//!   渲染，M2 验收载体。
//!
//! 设计说明：`FrameBuffer.data` 用 `Vec<u8>`（IOSurface 零拷贝路径留给
//! 后续 qemu patch，接口已预留 [`PixelFormat`]）；`metal.rs` 以
//! `render.rs`（wgpu，macOS 上即 Metal 后端）实现，与 mwine-player 渲染栈
//! 保持一致。

use std::sync::{Arc, Mutex};

pub mod grpc;
#[cfg(feature = "render")]
pub mod render;
pub mod vnc;

/// 帧的像素格式（字节序）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    /// 每像素 4 字节，内存序 R,G,B,A。emulator gRPC RGBA8888 即此序。
    Rgba,
    /// 每像素 4 字节，内存序 B,G,R,A。RFB 小端像素布局即此序。
    Bgra,
}

impl PixelFormat {
    pub fn bytes_per_pixel(self) -> usize {
        4
    }
}

/// 一帧完整帧缓冲。
#[derive(Debug, Clone)]
pub struct FrameBuffer {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    pub data: Vec<u8>,
    /// 服务端时间戳（微秒；gRPC timestampUs，0=未知）。
    pub timestamp_us: u64,
    /// 服务端帧序号（gRPC seq；VNC 无对应，恒 0）。
    pub seq: u64,
}

impl FrameBuffer {
    /// w*h*4，乘法防溢出。
    pub fn expected_len(width: u32, height: u32) -> usize {
        (width as usize)
            .checked_mul(height as usize)
            .and_then(|px| px.checked_mul(4))
            .expect("framebuffer size overflow")
    }

    /// 构造并校验数据长度。
    pub fn new(
        width: u32,
        height: u32,
        format: PixelFormat,
        data: Vec<u8>,
        timestamp_us: u64,
        seq: u64,
    ) -> Result<Self> {
        let expected = Self::expected_len(width, height);
        if data.len() != expected {
            return Err(DisplayError::FrameSizeMismatch {
                len: data.len(),
                expected,
                width,
                height,
            });
        }
        Ok(Self {
            width,
            height,
            format,
            data,
            timestamp_us,
            seq,
        })
    }

    /// 原地转换到 RGBA（BGRA 帧做 R/B 交换；RGBA 帧不动）。
    pub fn to_rgba_inplace(&mut self) {
        if self.format == PixelFormat::Rgba {
            return;
        }
        for px in self.data.chunks_exact_mut(4) {
            px.swap(0, 2);
        }
        self.format = PixelFormat::Rgba;
    }

    /// 保存为 PNG（先归一到 RGBA）。
    pub fn save_png(&self, path: &std::path::Path) -> Result<()> {
        let mut rgba = self.clone();
        rgba.to_rgba_inplace();
        let file = std::fs::File::create(path)?;
        let mut enc = png::Encoder::new(std::io::BufWriter::new(file), rgba.width, rgba.height);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        let mut writer = enc
            .write_header()
            .map_err(|e| DisplayError::DecodePng(format!("PNG 编码失败: {e}")))?;
        writer
            .write_image_data(&rgba.data)
            .map_err(|e| DisplayError::DecodePng(format!("PNG 编码失败: {e}")))?;
        Ok(())
    }
}

/// 目标显示器配置（`display_id`：0=主屏，2+=multidisplay）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DisplayConfig {
    pub display_id: u32,
    pub width: u32,
    pub height: u32,
    pub dpi: u32,
}

/// 已存在显示器的实际状态。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DisplayInfo {
    pub display_id: u32,
    pub width: u32,
    pub height: u32,
    pub dpi: u32,
}

/// `create_display` 的返回句柄。
///
/// `metal_texture: Option<MTLTexture>` 属 IOSurface 零拷贝路径，
/// 未铺开前窗口侧纹理由渲染层（render.rs）自管，此处只保留逻辑 id。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DisplayHandle {
    pub id: u32,
}

/// 显示器管理 + 单帧快照（gRPC / VNC 共同接口）。
pub trait DisplayBackend: Send {
    /// 创建（或改配）指定显示器——upsert 语义，同 id 重复调用即缩放。
    fn create_display(&mut self, config: DisplayConfig) -> Result<DisplayHandle>;

    /// 移除显示器（主屏 0 通常不可移除，由后端返回错误）。
    fn remove_display(&mut self, display_id: u32) -> Result<()>;

    /// 列出当前全部显示器。
    fn list_displays(&mut self) -> Result<Vec<DisplayInfo>>;

    /// 单帧快照（阻塞，走后端最便宜的单帧路径）。
    fn get_framebuffer(&mut self, display_id: u32) -> Result<FrameBuffer>;
}

/// 连续帧流：阻塞取下一帧（gRPC 流=变化驱动；轮询模式=按目标 fps 采样）。
pub trait FrameSource: Send {
    fn next_frame(&mut self) -> Result<FrameBuffer>;
}

/// 生产者/消费者之间的"最新帧"信箱：新帧覆盖旧帧（慢渲染不积压，
/// 16MB/帧的 RGBA 流必须丢帧而非排队）。
#[derive(Clone, Default)]
pub struct LatestFrames {
    cell: Arc<Mutex<Option<FrameBuffer>>>,
    /// 累计丢弃帧数（背压观测）。
    dropped: Arc<Mutex<u64>>,
}

impl LatestFrames {
    pub fn new() -> Self {
        Self::default()
    }

    /// 压入最新帧，返回被覆盖（丢弃）的帧数（0 或 1）。
    pub fn push(&self, frame: FrameBuffer) {
        let mut cell = self.cell.lock().unwrap();
        if cell.is_some() {
            *self.dropped.lock().unwrap() += 1;
        }
        *cell = Some(frame);
    }

    /// 取走当前最新帧（若无新帧返回 None，消费幂等）。
    pub fn take(&self) -> Option<FrameBuffer> {
        self.cell.lock().unwrap().take()
    }

    /// 只看帧序号而不取走（去重用）。
    pub fn peek_stamp(&self) -> Option<(u64, u64)> {
        self.cell
            .lock()
            .unwrap()
            .as_ref()
            .map(|f| (f.seq, f.timestamp_us))
    }

    pub fn dropped_count(&self) -> u64 {
        *self.dropped.lock().unwrap()
    }
}

/// 统一错误（thiserror）。
#[derive(Debug, thiserror::Error)]
pub enum DisplayError {
    #[error("gRPC 连接失败 {endpoint}: {detail}")]
    GrpcConnection { endpoint: String, detail: String },
    #[error("gRPC {method} 调用失败: {status}")]
    GrpcStatus { method: &'static str, status: String },
    #[error("未找到 gRPC 端点（尝试端口 {tried:?} 被拒）；emulator 需带 -grpc <port> 启动")]
    EndpointNotFound { tried: Vec<u16> },
    #[error("不支持的像素格式: {0:?}")]
    UnsupportedFormat(PixelFormat),
    #[error("帧数据长度不符: len={len} 期望={expected}（{width}x{height}）")]
    FrameSizeMismatch {
        len: usize,
        expected: usize,
        width: u32,
        height: u32,
    },
    #[error("PNG 截图解码失败: {0}")]
    DecodePng(String),
    #[error("渲染初始化/绘制失败: {0}")]
    Render(String),
    #[error("VNC 协议错误: {0}")]
    VncProtocol(String),
    #[error("VNC 要求认证（服务端安全类型 {0:?}），本客户端仅支持无认证直连")]
    VncAuthUnsupported(Vec<u8>),
    #[error("VNC 不支持的矩形编码 {0}")]
    VncUnsupportedEncoding(i32),
    #[error("显示器 {0} 不存在")]
    NoSuchDisplay(u32),
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
}

pub type Result<T> = std::result::Result<T, DisplayError>;

/// 等比缩放的信箱视口（渲染层用；纯函数便于单测）。
/// 返回窗口坐标系内的 (x, y, w, h)，保宽高比、居中、不放大超过 1:1。
#[inline]
pub fn letterbox(
    win_w: u32,
    win_h: u32,
    img_w: u32,
    img_h: u32,
    allow_upscale: bool,
) -> (i32, i32, u32, u32) {
    if win_w == 0 || win_h == 0 || img_w == 0 || img_h == 0 {
        return (0, 0, 0, 0);
    }
    let scale = (win_w as f64 / img_w as f64).min(win_h as f64 / img_h as f64);
    let scale = if allow_upscale { scale } else { scale.min(1.0) };
    let w = (img_w as f64 * scale).round().max(1.0) as u32;
    let h = (img_h as f64 * scale).round().max(1.0) as u32;
    let x = (win_w.saturating_sub(w)) / 2;
    let y = (win_h.saturating_sub(h)) / 2;
    (x as i32, y as i32, w, h)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(format: PixelFormat) -> FrameBuffer {
        FrameBuffer::new(2, 2, format, vec![0u8; 16], 0, 0).unwrap()
    }

    #[test]
    fn framebuffer_length_validation() {
        assert!(FrameBuffer::new(2, 2, PixelFormat::Rgba, vec![0; 15], 0, 0).is_err());
        assert!(FrameBuffer::new(2, 2, PixelFormat::Rgba, vec![0; 16], 0, 0).is_ok());
        // 防溢出路径：u32::MAX²×4 超 usize 上限，应 panic 而非回绕
        let boom = std::panic::catch_unwind(|| FrameBuffer::expected_len(u32::MAX, u32::MAX));
        assert!(boom.is_err(), "expected_len 应在尺寸溢出时 panic");
    }

    #[test]
    fn bgra_to_rgba_swaps_channels() {
        let mut f = frame(PixelFormat::Bgra);
        f.data = vec![1, 2, 3, 255, 4, 5, 6, 255];
        f.to_rgba_inplace();
        assert_eq!(f.data, vec![3, 2, 1, 255, 6, 5, 4, 255]);
        assert_eq!(f.format, PixelFormat::Rgba);
        // RGBA 帧幂等
        let before = f.data.clone();
        f.to_rgba_inplace();
        assert_eq!(f.data, before);
    }

    #[test]
    fn latest_frames_overwrites_and_counts_drops() {
        let q = LatestFrames::new();
        assert!(q.take().is_none());
        q.push(FrameBuffer::new(1, 1, PixelFormat::Rgba, vec![7; 4], 1, 1).unwrap());
        q.push(FrameBuffer::new(1, 1, PixelFormat::Rgba, vec![8; 4], 2, 2).unwrap());
        assert_eq!(q.dropped_count(), 1);
        let f = q.take().unwrap();
        assert_eq!(f.seq, 2);
        assert!(q.take().is_none());
        assert_eq!(q.dropped_count(), 1);
    }

    #[test]
    fn letterbox_keeps_aspect_and_centers() {
        // 竖屏图放进横窗口：高度受限
        let (x, y, w, h) = letterbox(1000, 500, 1080, 2400, true);
        assert_eq!((w, h), (225, 500));
        assert_eq!(x, (1000 - 225) / 2);
        assert_eq!(y, 0);
        // 禁止放大
        let (_, _, w, h) = letterbox(4000, 4000, 1080, 2400, false);
        assert_eq!((w, h), (1080, 2400));
        // 退化输入
        assert_eq!(letterbox(0, 100, 10, 10, true), (0, 0, 0, 0));
    }
}
