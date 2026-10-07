//! gRPC 后端（首选路径）：emulator 内置 gRPC（`emulator -grpc <port>`）。
//!
//! 实测结论（emulator 36.5.11 / qemu-system-aarch64-headless）：
//! - `getScreenshot` RGBA8888：1600x2560 → 精确 w*h*4 字节，字节序 RGBA，
//!   alpha 恒 255；PNG 格式亦可。
//! - `streamScreenshot`：变化驱动——guest 静止时近零推送，动画/滚动时按
//!   帧率推送（vsync 60Hz 上限）。因此慢内容不是缺陷，而是带宽特性；
//!   需要恒定帧率时用 [`StreamMode::Poll`]。
//! - `getDisplayConfigurations`/`setDisplayConfigurations`：multidisplay 的
//!   gRPC 等价物（与 console `multidisplay add/del` 同源），set 为整表替换。
//! - 端口：`-grpc` 显式指定（默认 8554，本仓库脚本惯用 8555）。
//!   连接时先 TCP 探测候选端口再拨号。

use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use tokio::runtime::Runtime;
use tonic::transport::{Channel, Endpoint};
use tonic::Streaming;

use crate::{
    DisplayBackend, DisplayConfig, DisplayError, DisplayHandle, DisplayInfo, FrameBuffer,
    FrameSource, LatestFrames, PixelFormat, Result,
};

/// 生成的 protobuf 客户端类型。
pub mod pb {
    // prost 对单文件包生成平铺结构：类型直接位于本模块
    // （pb::Image / pb::image_format::ImgFormat / pb::emulator_controller_client::…）
    tonic::include_proto!("android.emulation.control");
}

/// 默认探测端口：8554（官方默认）在先，8555（本仓库脚本惯例）在后。
pub const PROBE_PORTS: [u16; 4] = [8554, 8555, 8556, 8558];

/// `emulator-5554` → 8554（console 端口 + 3000）。解析失败返回 None。
pub fn grpc_port_from_serial(serial: &str) -> Option<u16> {
    let n: u16 = serial
        .strip_prefix("emulator-")
        .and_then(|n| n.parse().ok())?;
    n.checked_add(3000)
}

/// 从 emulator 服务发现注册表（~/Library/Caches/TemporaryItems/avd/running/
/// pid_*.ini）读取指定 gRPC 端口的认证 token（`-grpc-use-jwt` 启动的实例
/// 需要 `authorization: Bearer <token>`）。无注册表或无 token 返回 None。
pub fn discovery_token(port: u16) -> Option<String> {
    let home = std::env::var_os("HOME")?;
    let dir = std::path::Path::new(&home).join("Library/Caches/TemporaryItems/avd/running");
    discovery_token_in_dir(&dir, port)
}

/// [`discovery_token`] 的可测版本：在指定注册表目录内查找。
pub fn discovery_token_in_dir(dir: &std::path::Path, port: u16) -> Option<String> {
    for entry in std::fs::read_dir(dir).ok()?.flatten() {
        let text = std::fs::read_to_string(entry.path()).ok()?;
        let grpc_port = text
            .lines()
            .find(|l| l.starts_with("grpc.port="))
            .and_then(|l| l["grpc.port=".len()..].trim().parse::<u16>().ok());
        if grpc_port == Some(port) {
            return text
                .lines()
                .find(|l| l.starts_with("grpc.token="))
                .map(|l| l["grpc.token=".len()..].trim().to_string())
                .filter(|t| !t.is_empty());
        }
    }
    None
}

/// TCP 探测候选端口，返回第一个可连通的（300ms 超时/端口）。
pub fn probe_port(host: &str, ports: &[u16]) -> Option<u16> {
    ports.iter().copied().find(|&p| {
        TcpStream::connect((host, p))
            .ok()
            .and_then(|s| s.set_read_timeout(Some(Duration::from_millis(300))).ok())
            .is_some()
    })
}

// ---------------------------------------------------------------------------
// Image → FrameBuffer 转换（纯函数，单测覆盖）
// ---------------------------------------------------------------------------

#[allow(deprecated)] // 顶层 width/height 为协议废弃字段，仅作 format 缺失时的兜底
fn image_dims(img: &pb::Image) -> (u32, u32) {
    // 服务端把实际尺寸写进回显的 format（实测），顶层 width/height 已废弃。
    if let Some(f) = img.format.as_ref() {
        if f.width > 0 && f.height > 0 {
            return (f.width, f.height);
        }
    }
    (img.width, img.height)
}

fn image_to_frame(img: pb::Image) -> Result<FrameBuffer> {
    let (w, h) = image_dims(&img);
    use pb::image_format::ImgFormat;
    let fmt = img
        .format
        .as_ref()
        .map(|f| f.format())
        .unwrap_or(ImgFormat::Png);
    match fmt {
        ImgFormat::Rgba8888 => FrameBuffer::new(
            w,
            h,
            PixelFormat::Rgba,
            img.image,
            img.timestamp_us,
            img.seq as u64,
        ),
        ImgFormat::Png => decode_png_to_rgba(&img.image, img.timestamp_us, img.seq as u64),
        ImgFormat::Rgb888 => rgb888_to_rgba(w, h, img.image, img.timestamp_us, img.seq as u64),
    }
}

/// RGB888（每像素 3 字节）→ RGBA。
fn rgb888_to_rgba(w: u32, h: u32, data: Vec<u8>, timestamp_us: u64, seq: u64) -> Result<FrameBuffer> {
    if data.len() != w as usize * h as usize * 3 {
        return Err(DisplayError::FrameSizeMismatch {
            len: data.len(),
            expected: w as usize * h as usize * 3,
            width: w,
            height: h,
        });
    }
    let mut out = Vec::with_capacity(w as usize * h as usize * 4);
    for px in data.chunks_exact(3) {
        out.extend_from_slice(&[px[0], px[1], px[2], 255]);
    }
    FrameBuffer::new(w, h, PixelFormat::Rgba, out, timestamp_us, seq)
}

/// PNG 截图 → RGBA FrameBuffer。
pub fn decode_png_to_rgba(data: &[u8], timestamp_us: u64, seq: u64) -> Result<FrameBuffer> {
    let decoder = png::Decoder::new(std::io::Cursor::new(data));
    let mut reader = decoder
        .read_info()
        .map_err(|e| DisplayError::DecodePng(e.to_string()))?;
    let mut buf = vec![0u8; reader.output_buffer_size()];
    let info = reader
        .next_frame(&mut buf)
        .map_err(|e| DisplayError::DecodePng(e.to_string()))?;
    buf.truncate(info.buffer_size());
    // png crate 输出统一大端 RGBA8
    FrameBuffer::new(
        info.width,
        info.height,
        PixelFormat::Rgba,
        buf,
        timestamp_us,
        seq,
    )
}

// ---------------------------------------------------------------------------
// 显示器配置合并（纯函数，单测覆盖）
// ---------------------------------------------------------------------------

/// 整表替换语义下的 upsert：找到同 id 改尺寸，否则追加。
/// 注意：list 只含副屏条目——setDisplayConfigurations 不接受主屏 0
/// （实测 36.x：带主屏条目即报 "Display id has to be in the set"）。
pub fn merge_display_configs(
    mut list: Vec<pb::DisplayConfiguration>,
    cfg: &DisplayConfig,
) -> Vec<pb::DisplayConfiguration> {
    if let Some(entry) = list
        .iter_mut()
        .find(|d| d.display == cfg.display_id)
    {
        entry.width = cfg.width;
        entry.height = cfg.height;
        entry.dpi = cfg.dpi;
    } else {
        list.push(pb::DisplayConfiguration {
            width: cfg.width,
            height: cfg.height,
            dpi: cfg.dpi,
            flags: 0,
            display: cfg.display_id,
        });
    }
    list
}

/// 整表替换语义下的删除：目标不存在返回 None。
pub fn remove_display_config(
    list: Vec<pb::DisplayConfiguration>,
    id: u32,
) -> Option<Vec<pb::DisplayConfiguration>> {
    if !list.iter().any(|d| d.display == id) {
        return None;
    }
    Some(list.into_iter().filter(|d| d.display != id).collect())
}

/// 构造截图/流请求。max_width=Some(w) 时请求 (w, 10000) 框，服务端按宽
/// 内缩（实测 1600x2560 → 800x1280），用于控制带宽。
pub fn screenshot_request(display_id: u32, max_width: Option<u32>) -> pb::ImageFormat {
    let (w, h) = match max_width {
        Some(w) => (w, 10_000),
        None => (0, 0),
    };
    pb::ImageFormat {
        format: pb::image_format::ImgFormat::Rgba8888 as i32,
        display: display_id,
        width: w,
        height: h,
        ..Default::default()
    }
}

// ---------------------------------------------------------------------------
// 后端
// ---------------------------------------------------------------------------

/// 从 setDisplayConfigurations 的拒绝信息里解析合法副屏 id 集合：
/// "Display id has to be in the set: [1, 3]." → [1, 3]。
/// emulator 36.x 的副屏 id 由 qemu 内部分配（不是 2+ 的固定惯例）。
pub fn parse_allowed_display_ids(err_msg: &str) -> Option<Vec<u32>> {
    let start = err_msg.find('[')?;
    let end = err_msg.rfind(']')?;
    let ids = err_msg[start + 1..end]
        .split(',')
        .filter_map(|t| t.trim().parse().ok())
        .collect::<Vec<u32>>();
    if ids.is_empty() { None } else { Some(ids) }
}

/// 从合法集合中选一个未被现有显示器占用的最小 id；全占用返回 None。
fn pick_free_display_id(allowed: &[u32], used: &[u32]) -> Option<u32> {
    allowed.iter().copied().find(|id| !used.contains(id))
}

/// gRPC 显示后端（阻塞 API，内部独占 tokio current-thread runtime）。
pub struct GrpcDisplayBackend {
    endpoint: String,
    port: u16,
    rt: Arc<Runtime>,
    client: AuthClient,
}

/// Bearer 认证拦截器（token 为 None 时透传）。
#[derive(Clone)]
struct BearerInterceptor(Option<tonic::metadata::MetadataValue<tonic::metadata::Ascii>>);

impl tonic::service::Interceptor for BearerInterceptor {
    fn call(&mut self, mut req: tonic::Request<()>)
        -> std::result::Result<tonic::Request<()>, tonic::Status> {
        if let Some(v) = &self.0 {
            req.metadata_mut().insert("authorization", v.clone());
        }
        Ok(req)
    }
}

/// 带（可选）认证的客户端类型。
pub type AuthClient = pb::emulator_controller_client::EmulatorControllerClient<
    tonic::service::interceptor::InterceptedService<Channel, BearerInterceptor>,
>;

/// 拨号并按需附 Bearer token（token 存在时每个请求注入 authorization 头）。
async fn dial_with_token(
    endpoint: &str,
    token: Option<&str>,
    timeout: Duration,
) -> std::result::Result<AuthClient, tonic::transport::Error> {
    let ep = Endpoint::from_shared(endpoint.to_string())?;
    let chan = ep.connect_timeout(timeout).connect().await?;
    let header = token
        .filter(|t| !t.is_empty())
        .map(|t| format!("Bearer {t}"))
        .and_then(|h| h.parse().ok());
    let interceptor = BearerInterceptor(header);
    let client =
        pb::emulator_controller_client::EmulatorControllerClient::with_interceptor(
            chan,
            interceptor,
        )
        .max_decoding_message_size(64 * 1024 * 1024);
    Ok(client)
}

impl GrpcDisplayBackend {
    /// 连接指定端口。若该端口的 emulator 启用了 JWT（服务发现注册表里有
    /// grpc.token），自动附 Bearer 认证。
    pub fn connect(host: &str, port: u16) -> Result<Self> {
        let endpoint = format!("http://{host}:{port}");
        let token = discovery_token(port);
        let rt = Arc::new(
            tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .map_err(|e| DisplayError::GrpcConnection {
                    endpoint: endpoint.clone(),
                    detail: e.to_string(),
                })?,
        );
        let client = rt
            .block_on(dial_with_token(&endpoint, token.as_deref(), Duration::from_secs(3)))
            .map_err(|e| DisplayError::GrpcConnection {
                endpoint: endpoint.clone(),
                detail: e.to_string(),
            })?;
        Ok(Self {
            endpoint,
            port,
            rt,
            client,
        })
    }

    /// 按 serial/候选端口探测连接。显式端口优先，否则 serial 推断 + 常用端口探测。
    pub fn connect_serial(serial: &str, explicit_port: Option<u16>) -> Result<Self> {
        let mut candidates: Vec<u16> = Vec::new();
        if let Some(p) = explicit_port {
            candidates.push(p);
        }
        if let Some(p) = grpc_port_from_serial(serial) {
            if !candidates.contains(&p) {
                candidates.push(p);
            }
        }
        for p in PROBE_PORTS {
            if !candidates.contains(&p) {
                candidates.push(p);
            }
        }
        let port = probe_port("127.0.0.1", &candidates).ok_or_else(|| {
            DisplayError::EndpointNotFound {
                tried: candidates.clone(),
            }
        })?;
        Self::connect("127.0.0.1", port)
    }

    pub fn endpoint(&self) -> &str {
        &self.endpoint
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    /// 单帧 RGBA 快照（max_width 服务端内缩，None=原始分辨率）。
    pub fn snapshot_rgba(&self, display_id: u32, max_width: Option<u32>) -> Result<FrameBuffer> {
        let mut client = self.client.clone();
        let req = screenshot_request(display_id, max_width);
        let resp = self
            .rt
            .block_on(async { client.get_screenshot(req).await })
            .map_err(status_err("getScreenshot"))?;
        image_to_frame(resp.into_inner())
    }

    /// 单帧 PNG 快照（服务端编码 PNG，客户端解码回 RGBA）。
    pub fn snapshot_png(&self, display_id: u32, max_width: Option<u32>) -> Result<FrameBuffer> {
        let mut client = self.client.clone();
        let mut req = screenshot_request(display_id, max_width);
        req.format = pb::image_format::ImgFormat::Png as i32;
        let resp = self
            .rt
            .block_on(async { client.get_screenshot(req).await })
            .map_err(status_err("getScreenshot"))?;
        image_to_frame(resp.into_inner())
    }

    /// 变化驱动帧流（gRPC server-streaming）。
    pub fn stream(&self, display_id: u32, max_width: Option<u32>) -> Result<GrpcFrameStream> {
        let mut client = self.client.clone();
        let req = screenshot_request(display_id, max_width);
        let streaming = self
            .rt
            .block_on(async { client.stream_screenshot(req).await })
            .map_err(status_err("streamScreenshot"))?
            .into_inner();
        Ok(GrpcFrameStream {
            rt: Arc::clone(&self.rt),
            stream: streaming,
        })
    }

    /// 恒定帧率轮询源（getScreenshot 周期采样）。
    pub fn poll(&self, display_id: u32, fps: u32, max_width: Option<u32>) -> PollFrameSource {
        PollFrameSource {
            rt: Arc::clone(&self.rt),
            client: self.client.clone(),
            display_id,
            max_width,
            interval: Duration::from_millis(1000 / (fps.max(1) as u64)),
            last: None,
        }
    }
}

fn status_err(method: &'static str) -> impl Fn(tonic::Status) -> DisplayError {
    move |s: tonic::Status| DisplayError::GrpcStatus {
        method,
        status: s.message().to_string(),
    }
}

async fn dial_with_timeout(
    endpoint: &str,
    token: Option<&str>,
    timeout: Duration,
) -> std::result::Result<AuthClient, tonic::transport::Error> {
    dial_with_token(endpoint, token, timeout).await
}

impl DisplayBackend for GrpcDisplayBackend {
    fn create_display(&mut self, config: DisplayConfig) -> Result<DisplayHandle> {
        let mut client = self.client.clone();
        let current = self
            .rt
            .block_on(async {
                client
                    .get_display_configurations(pb::Empty {})
                    .await
            })
            .map_err(status_err("getDisplayConfigurations"))?
            .into_inner();
        if config.display_id == 0 {
            return Err(DisplayError::GrpcStatus {
                method: "setDisplayConfigurations",
                status: "主屏（display 0）不可通过 gRPC setDisplayConfigurations 修改；                         请改用 AVD 配置或 console"
                    .into(),
            });
        }
        let used: Vec<u32> = current.displays.iter().map(|d| d.display).collect();
        // 只发副屏条目（主屏由 qemu 自管）；base 保留原始表供 id 被拒后重建
        let secondary: Vec<_> = current
            .displays
            .iter()
            .filter(|d| d.display != 0)
            .cloned()
            .collect();
        let merged = merge_display_configs(secondary.clone(), &config);
        let applied = self
            .rt
            .block_on(async {
                client
                    .set_display_configurations(pb::DisplayConfigurations {
                        displays: merged.clone(),
                        ..Default::default()
                    })
                    .await
            });
        // 请求 id 不在 qemu 分配的合法集合时，解析拒绝信息自动改选空闲 id。
        // （emulator 36.x 副屏 id 由 qemu 内部分配，上层不应假设 2+ 连续可用）
        let applied = match applied {
            Ok(resp) => resp.into_inner(),
            Err(status) if parse_allowed_display_ids(status.message()).is_some() => {
                let msg = status.message().to_string();
                let Some(allowed) = parse_allowed_display_ids(&msg) else {
                    return Err(DisplayError::GrpcStatus {
                        method: "setDisplayConfigurations",
                        status: msg,
                    });
                };
                let Some(free_id) = pick_free_display_id(&allowed, &used) else {
                    return Err(DisplayError::GrpcStatus {
                        method: "setDisplayConfigurations",
                        status: format!("合法显示器 id {allowed:?} 均已占用"),
                    });
                };
                let retry = merge_display_configs(secondary, &DisplayConfig { display_id: free_id, ..config });
                self.rt
                    .block_on(async {
                        client
                            .set_display_configurations(pb::DisplayConfigurations {
                                displays: retry,
                                ..Default::default()
                            })
                            .await
                    })
                    .map_err(status_err("setDisplayConfigurations"))?
                    .into_inner()
            }
            Err(status) => {
                return Err(DisplayError::GrpcStatus {
                    method: "setDisplayConfigurations",
                    status: status.message().to_string(),
                })
            }
        };
        // 回读校验：按参数出现在应用结果里的显示器（取实际生效 id）
        applied
            .displays
            .iter()
            .find(|d| d.width == config.width && d.height == config.height && d.dpi == config.dpi)
            .map(|d| DisplayHandle { id: d.display })
            .ok_or(DisplayError::GrpcStatus {
                method: "setDisplayConfigurations",
                status: format!(
                    "应用后未出现显示器 {}（应用结果 {:?}）",
                    config.display_id, applied.displays
                ),
            })
    }

    fn remove_display(&mut self, display_id: u32) -> Result<()> {
        if display_id == 0 {
            return Err(DisplayError::GrpcStatus {
                method: "setDisplayConfigurations",
                status: "主屏（display 0）不可移除".into(),
            });
        }
        let mut client = self.client.clone();
        let current = self
            .rt
            .block_on(async {
                client
                    .get_display_configurations(pb::Empty {})
                    .await
            })
            .map_err(status_err("getDisplayConfigurations"))?
            .into_inner();
        // 同 create：set 只接受副屏条目，主屏剔除后再过滤目标
        let secondary: Vec<_> = current
            .displays
            .into_iter()
            .filter(|d| d.display != 0)
            .collect();
        let next = remove_display_config(secondary, display_id)
            .ok_or(DisplayError::NoSuchDisplay(display_id))?;
        self.rt
            .block_on(async {
                client
                    .set_display_configurations(pb::DisplayConfigurations {
                        displays: next,
                        ..Default::default()
                    })
                    .await
            })
            .map_err(status_err("setDisplayConfigurations"))?;
        Ok(())
    }

    fn list_displays(&mut self) -> Result<Vec<DisplayInfo>> {
        let mut client = self.client.clone();
        let cfgs = self
            .rt
            .block_on(async {
                client
                    .get_display_configurations(pb::Empty {})
                    .await
            })
            .map_err(status_err("getDisplayConfigurations"))?
            .into_inner();
        Ok(cfgs
            .displays
            .into_iter()
            .map(|d| DisplayInfo {
                display_id: d.display,
                width: d.width,
                height: d.height,
                dpi: d.dpi,
            })
            .collect())
    }

    fn get_framebuffer(&mut self, display_id: u32) -> Result<FrameBuffer> {
        self.snapshot_rgba(display_id, None)
    }
}

/// 变化驱动帧流句柄。
pub struct GrpcFrameStream {
    rt: Arc<Runtime>,
    stream: Streaming<pb::Image>,
}

impl FrameSource for GrpcFrameStream {
    fn next_frame(&mut self) -> Result<FrameBuffer> {
        match self.rt.block_on(self.stream.message()) {
            Ok(Some(img)) => image_to_frame(img),
            Ok(None) => Err(DisplayError::GrpcStatus {
                method: "streamScreenshot",
                status: "服务端关闭了帧流".into(),
            }),
            Err(s) => Err(DisplayError::GrpcStatus {
                method: "streamScreenshot",
                status: s.message().to_string(),
            }),
        }
    }
}

/// 恒定帧率轮询源。
pub struct PollFrameSource {
    rt: Arc<Runtime>,
    client: AuthClient,
    display_id: u32,
    max_width: Option<u32>,
    interval: Duration,
    last: Option<Instant>,
}

impl FrameSource for PollFrameSource {
    fn next_frame(&mut self) -> Result<FrameBuffer> {
        // 控频：距上次采样不足一个周期就先睡
        if let Some(last) = self.last {
            let elapsed = last.elapsed();
            if elapsed < self.interval {
                std::thread::sleep(self.interval - elapsed);
            }
        }
        let mut client = self.client.clone();
        let req = screenshot_request(self.display_id, self.max_width);
        let resp = self
            .rt
            .block_on(async { client.get_screenshot(req).await })
            .map_err(status_err("getScreenshot"))?;
        self.last = Some(Instant::now());
        image_to_frame(resp.into_inner())
    }
}

// ---------------------------------------------------------------------------
// 后台帧泵（渲染线程解耦）
// ---------------------------------------------------------------------------

/// 帧泵模式。
#[derive(Debug, Clone, Copy)]
pub enum StreamMode {
    /// gRPC server-streaming，变化驱动。
    Stream,
    /// getScreenshot 恒定帧率轮询（fps）。
    Poll(u32),
}

/// 帧泵运行统计。
#[derive(Debug, Default)]
pub struct StreamStats {
    pub frames: u64,
    pub errors: u64,
    pub last_error: Option<String>,
    pub last_frame_at: Option<Instant>,
}

/// 后台帧泵：独立线程 + 独立 runtime 拉流，最新帧写入 [`LatestFrames`]。
/// 渲染循环只消费信箱，永不阻塞在网络上。断线自动重连（1s 退避）。
pub struct FramePump {
    frames: LatestFrames,
    stats: Arc<Mutex<StreamStats>>,
    cancel: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
    started: Instant,
}

impl FramePump {
    /// 启动帧泵。线程内自行连接（emulator 未就绪时重试至 120s）。
    /// max_width=Some(w) 时服务端按宽内缩（带宽控制）。
    /// token=Some 时每个请求注入 Bearer 认证（`-grpc-use-jwt` 实例）。
    pub fn spawn(
        endpoint: String,
        display_id: u32,
        mode: StreamMode,
        max_width: Option<u32>,
        token: Option<String>,
    ) -> Self {
        let frames = LatestFrames::new();
        let stats = Arc::new(Mutex::new(StreamStats::default()));
        let cancel = Arc::new(AtomicBool::new(false));
        let handle = std::thread::Builder::new()
            .name("mwine-display-pump".into())
            .spawn({
                let frames = frames.clone();
                let stats = Arc::clone(&stats);
                let cancel = Arc::clone(&cancel);
                move || {
                    pump_loop(endpoint, token, display_id, mode, max_width, frames, stats, cancel)
                }
            })
            .expect("spawn display pump");
        Self {
            frames,
            stats,
            cancel,
            handle: Some(handle),
            started: Instant::now(),
        }
    }

    pub fn frames(&self) -> LatestFrames {
        self.frames.clone()
    }

    pub fn stats_snapshot(&self) -> (u64, u64, Option<String>) {
        let s = self.stats.lock().unwrap();
        (s.frames, s.errors, s.last_error.clone())
    }

    /// 泵启动以来的实测平均帧率。
    pub fn measured_fps(&self) -> f64 {
        let frames = self.stats.lock().unwrap().frames;
        frames as f64 / self.started.elapsed().as_secs_f64().max(0.001)
    }

    /// 停止泵并回收线程。
    pub fn stop(&mut self) {
        self.cancel.store(true, Ordering::Relaxed);
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

impl Drop for FramePump {
    fn drop(&mut self) {
        self.stop();
    }
}

fn pump_loop(
    endpoint: String,
    token: Option<String>,
    display_id: u32,
    mode: StreamMode,
    max_width: Option<u32>,
    frames: LatestFrames,
    stats: Arc<Mutex<StreamStats>>,
    cancel: Arc<AtomicBool>,
) {
    let rt = match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(rt) => rt,
        Err(e) => {
            *stats.lock().unwrap() = StreamStats {
                errors: 1,
                last_error: Some(format!("runtime 创建失败: {e}")),
                ..Default::default()
            };
            return;
        }
    };
    rt.block_on(async move {
        // 连接重试窗：覆盖 emulator 冷启动
        let deadline = Instant::now() + Duration::from_secs(120);
        loop {
            if cancel.load(Ordering::Relaxed) {
                return;
            }
            let mut client = match dial_with_timeout(&endpoint, token.as_deref(), Duration::from_secs(3)).await {
                Ok(c) => c,
                Err(err) => {
                    {
                        let mut s = stats.lock().unwrap();
                        s.errors += 1;
                        s.last_error = Some(err.to_string());
                    }
                    if Instant::now() > deadline {
                        return;
                    }
                    tokio::time::sleep(Duration::from_secs(1)).await;
                    continue;
                }
            };
            let make_req = || screenshot_request(display_id, max_width);
            match mode {
                StreamMode::Stream => {
                    let mut stream = match client.stream_screenshot(make_req()).await {
                        Ok(s) => s.into_inner(),
                        Err(e) => {
                            let mut s = stats.lock().unwrap();
                            s.errors += 1;
                            s.last_error = Some(e.message().to_string());
                            drop(s);
                            tokio::time::sleep(Duration::from_secs(1)).await;
                            continue;
                        }
                    };
                    loop {
                        if cancel.load(Ordering::Relaxed) {
                            return;
                        }
                        match stream.message().await {
                            Ok(Some(img)) => {
                                let mut s = stats.lock().unwrap();
                                s.frames += 1;
                                s.last_frame_at = Some(Instant::now());
                                drop(s);
                                if let Ok(f) = image_to_frame(img) {
                                    frames.push(f);
                                }
                            }
                            Ok(None) => break, // 服务端关流，外层重连
                            Err(e) => {
                                let mut s = stats.lock().unwrap();
                                s.errors += 1;
                                s.last_error = Some(e.message().to_string());
                                drop(s);
                                break;
                            }
                        }
                    }
                    tokio::time::sleep(Duration::from_millis(500)).await;
                }
                StreamMode::Poll(fps) => {
                    let interval = Duration::from_millis(1000 / (fps.max(1) as u64));
                    loop {
                        if cancel.load(Ordering::Relaxed) {
                            return;
                        }
                        match client.get_screenshot(make_req()).await {
                            Ok(resp) => {
                                let mut s = stats.lock().unwrap();
                                s.frames += 1;
                                s.last_frame_at = Some(Instant::now());
                                drop(s);
                                if let Ok(f) = image_to_frame(resp.into_inner()) {
                                    frames.push(f);
                                }
                            }
                            Err(e) => {
                                let mut s = stats.lock().unwrap();
                                s.errors += 1;
                                s.last_error = Some(e.message().to_string());
                                drop(s);
                                break; // 重连
                            }
                        }
                        tokio::time::sleep(interval).await;
                    }
                    tokio::time::sleep(Duration::from_millis(500)).await;
                }
            }
        }
    });
}

// ---------------------------------------------------------------------------
// 单元测试（无 emulator，纯函数）
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn screenshot_request_scaling_fields() {
        let full = screenshot_request(3, None);
        assert_eq!((full.display, full.width, full.height), (3, 0, 0));
        assert_eq!(full.format, pb::image_format::ImgFormat::Rgba8888 as i32);
        let scaled = screenshot_request(0, Some(800));
        assert_eq!((scaled.display, scaled.width, scaled.height), (0, 800, 10_000));
    }

    #[test]
    fn discovery_token_finds_matching_port() {
        let dir = std::env::temp_dir().join(format!("mwine_test_disc_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("pid_1.ini"),
            "avd.id=MWine1\ngrpc.port=8554\ngrpc.token=abc123==\n",
        )
        .unwrap();
        std::fs::write(
            dir.join("pid_2.ini"),
            "avd.id=MWine2\ngrpc.port=8556\n",
        )
        .unwrap();
        assert_eq!(discovery_token_in_dir(&dir, 8554).as_deref(), Some("abc123=="));
        assert_eq!(discovery_token_in_dir(&dir, 8556), None);
        assert_eq!(discovery_token_in_dir(&dir, 9999), None);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn port_inference_from_serial() {
        assert_eq!(grpc_port_from_serial("emulator-5554"), Some(8554));
        assert_eq!(grpc_port_from_serial("emulator-5556"), Some(8556));
        assert_eq!(grpc_port_from_serial("localhost:5055"), None);
        assert_eq!(grpc_port_from_serial("emulator-65500"), None); // +3000 溢出
    }

    #[test]
    fn merge_upserts_and_appends() {
        let list = vec![pb::DisplayConfiguration {
            width: 1600,
            height: 2560,
            dpi: 320,
            flags: 0,
            display: 0,
        }];
        // 改主屏
        let out = merge_display_configs(
            list.clone(),
            &DisplayConfig {
                display_id: 0,
                width: 1080,
                height: 2400,
                dpi: 440,
            },
        );
        assert_eq!(out.len(), 1);
        assert_eq!((out[0].width, out[0].height, out[0].dpi), (1080, 2400, 440));
        // 追加副屏
        let out = merge_display_configs(
            list,
            &DisplayConfig {
                display_id: 2,
                width: 1280,
                height: 800,
                dpi: 240,
            },
        );
        assert_eq!(out.len(), 2);
        assert_eq!(out[1].display, 2);
    }

    #[test]
    fn parse_allowed_ids_and_pick_free() {
        assert_eq!(
            parse_allowed_display_ids("Display id has to be in the set: [1, 3]."),
            Some(vec![1, 3])
        );
        assert_eq!(parse_allowed_display_ids("some other error"), None);
        assert_eq!(
            parse_allowed_display_ids("set: []"),
            None,
            "空集合视为无信息"
        );
        assert_eq!(pick_free_display_id(&[1, 3], &[0, 1]), Some(3));
        assert_eq!(pick_free_display_id(&[1, 3], &[0, 1, 3]), None);
    }

    #[test]
    fn remove_filters_and_reports_missing() {
        let list = vec![
            pb::DisplayConfiguration { display: 0, width: 1, height: 1, dpi: 1, flags: 0 },
            pb::DisplayConfiguration { display: 2, width: 2, height: 2, dpi: 2, flags: 0 },
        ];
        let out = remove_display_config(list.clone(), 2).unwrap();
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].display, 0);
        assert!(remove_display_config(list, 7).is_none());
    }

    #[test]
    fn image_to_frame_validates_and_converts() {
        // RGBA8888：format 回显尺寸
        let img = pb::Image {
            format: Some(pb::ImageFormat {
                format: pb::image_format::ImgFormat::Rgba8888 as i32,
                width: 4,
                height: 2,
                ..Default::default()
            }),
            image: vec![9u8; 4 * 2 * 4],
            seq: 3,
            timestamp_us: 42,
            ..Default::default()
        };
        let f = image_to_frame(img).unwrap();
        assert_eq!((f.width, f.height), (4, 2));
        assert_eq!(f.format, PixelFormat::Rgba);
        assert_eq!((f.seq, f.timestamp_us), (3, 42));

        // 长度不符报错
        let bad = pb::Image {
            format: Some(pb::ImageFormat {
                format: pb::image_format::ImgFormat::Rgba8888 as i32,
                width: 4,
                height: 2,
                ..Default::default()
            }),
            image: vec![0u8; 10],
            ..Default::default()
        };
        assert!(matches!(
            image_to_frame(bad),
            Err(DisplayError::FrameSizeMismatch { .. })
        ));

        // 尺寸缺失顶层兜底
        let legacy = pb::Image {
            width: 2,
            height: 1,
            format: Some(pb::ImageFormat {
                format: pb::image_format::ImgFormat::Rgba8888 as i32,
                ..Default::default()
            }),
            image: vec![0u8; 8],
            ..Default::default()
        };
        assert_eq!(image_to_frame(legacy).unwrap().width, 2);
    }

    #[test]
    fn png_decode_roundtrip() {
        // 编一个 2x1 红绿 PNG 再解码
        let mut png_data = Vec::new();
        {
            let mut enc = png::Encoder::new(&mut png_data, 2, 1);
            enc.set_color(png::ColorType::Rgba);
            enc.set_depth(png::BitDepth::Eight);
            let mut w = enc.write_header().unwrap();
            w.write_image_data(&[255, 0, 0, 255, 0, 255, 0, 255]).unwrap();
        }
        let f = decode_png_to_rgba(&png_data, 0, 0).unwrap();
        assert_eq!((f.width, f.height), (2, 1));
        assert_eq!(&f.data[..4], &[255, 0, 0, 255]);
    }
}
