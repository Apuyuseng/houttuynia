//! scrcpy v4.1 会话：部署 server → 端口转发 → 启动 → 双 socket（video+control）。
//! 与 cpp/modules/scrcpy 同协议，按 Web 投屏需要的最子集实现。

use std::io::Read;
use std::net::TcpStream;
use std::time::{Duration, Instant};

use crate::adb::{self, AdbResult, ShellProcess};

pub const SERVER_VERSION: &str = "4.1";
pub const SERVER_CLASS: &str = "com.genymobile.scrcpy.Server";

const FLAG_SESSION: u64 = 1 << 63;
const FLAG_CONFIG: u64 = 1 << 62;
const FLAG_KEYFRAME: u64 = 1 << 61;

#[derive(Debug, Clone)]
pub struct VideoPacket {
    pub pts: u64,
    pub config: bool,
    pub key_frame: bool,
    pub data: Vec<u8>,
}

/// 音频包（config 包 = 编解码器特定配置，如 AAC 的 AudioSpecificConfig）
#[derive(Debug, Clone)]
pub struct AudioPacket {
    pub pts: u64,
    pub config: bool,
    pub data: Vec<u8>,
}

fn err<T>(msg: impl Into<String>) -> AdbResult<T> {
    Err(adb::AdbError(msg.into()))
}

/// server jar 的稳定远端路径（.jar 后缀必须：API 21 ART 拒收其他扩展名）。
pub fn remote_jar_path() -> String {
    format!("/data/local/tmp/scrcpy-server-v{SERVER_VERSION}.jar")
}

/// 本机 server jar 查找（与 C++ findServerJar 同序）。
pub fn find_server_jar() -> Option<String> {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/root".into());
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()));
    let mut cands: Vec<String> = vec![
        format!("{home}/.mwine/bin/scrcpy-server-v4.1"),
        format!("{home}/.mwine/bin/scrcpy-server-v4.0"),
        // 开发树直跑（CWD=仓库根）：dist 内置 jar 优先于 homebrew 装的
        // scrcpy-server（后者可能是旧版本——server/client 版本不匹配会
        // 直接 IllegalArgumentException，曾致 Mac 本地开流恒超时）
        "dist/Houttuynia.app/Contents/Resources/bin/scrcpy-server-v4.1".into(),
        "/opt/mwine/bin/scrcpy-server-v4.1".into(), // NAS Docker 镜像内置
        "/opt/homebrew/share/scrcpy/scrcpy-server".into(),
        "/usr/local/share/scrcpy/scrcpy-server".into(),
    ];
    if let Some(d) = exe_dir {
        cands.push(format!("{}/../Resources/bin/scrcpy-server-v4.1", d.display()));
        cands.push(format!("{}/scrcpy-server-v4.1", d.display()));
    }
    cands.into_iter().find(|p| std::path::Path::new(p).is_file())
}

/// 本机空闲端口。
fn free_port() -> AdbResult<u16> {
    let l = std::net::TcpListener::bind(("127.0.0.1", 0))
        .map_err(|e| adb::AdbError(format!("分配空闲端口失败: {e}")))?;
    Ok(l.local_addr().map(|a| a.port()).unwrap_or(0))
}

pub struct Session {
    pub serial: String,
    pub device_name: String,
    pub codec: String,
    pub video: TcpStream,
    pub control: Option<TcpStream>,
    /// 音频 socket（aac 等；设备不支持音频采集时 None——server 下发 codec id 0）
    pub audio: Option<TcpStream>,
    pub audio_codec: String,
    pub width: u32,
    pub height: u32,
    pub forward_port: u16,
    server: ShellProcess,
}

impl Session {
    pub fn stop(&mut self) {
        self.server.shutdown();
        let _ = self.video.shutdown(std::net::Shutdown::Both);
        if let Some(c) = &self.control {
            let _ = c.shutdown(std::net::Shutdown::Both);
        }
        let _ = adb::remove_forward(&self.serial, self.forward_port);
    }

    /// 阻塞读下一个视频包（流结束 Ok(None)；session meta 被吸收并更新尺寸）。
    pub fn next_packet(&mut self) -> AdbResult<Option<VideoPacket>> {
        loop {
            let mut h8 = [0u8; 8];
            match read_exact(&mut self.video, &mut h8, 0) {
                Ok(true) => {}
                Ok(false) => return Ok(None), // 干净结束
                Err(e) => return Err(e),
            }
            let pts_flags = u64::from_be_bytes(h8);
            if pts_flags & FLAG_SESSION != 0 {
                let w = (pts_flags & 0xFFFF_FFFF) as u32;
                let mut h4 = [0u8; 4];
                match read_exact(&mut self.video, &mut h4, 0)? {
                    true => {
                        self.width = w;
                        self.height = u32::from_be_bytes(h4);
                        continue;
                    }
                    false => return err("session meta 中途 EOF"),
                }
            }
            let config = pts_flags & FLAG_CONFIG != 0;
            let key_frame = pts_flags & FLAG_KEYFRAME != 0;
            let pts = pts_flags & !(FLAG_CONFIG | FLAG_KEYFRAME);
            let mut l4 = [0u8; 4];
            match read_exact(&mut self.video, &mut l4, 0)? {
                true => {}
                false => return err("视频包长度前缀前 EOF"),
            }
            let len = u32::from_be_bytes(l4) as usize;
            if len == 0 {
                return Ok(Some(VideoPacket { pts, config, key_frame, data: Vec::new() }));
            }
            let mut data = vec![0u8; len];
            match read_exact(&mut self.video, &mut data, 0)? {
                true => {}
                false => return err("视频包中途 EOF"),
            }
            return Ok(Some(VideoPacket { pts, config, key_frame, data }));
        }
    }
}

/// 读满或 EOF；Ok(true)=读满 Ok(false)=起始即 EOF。timeout_ms=0 阻塞。
fn read_exact(s: &mut TcpStream, buf: &mut [u8], timeout_ms: u64) -> AdbResult<bool> {
    let mut filled = 0;
    let deadline = if timeout_ms > 0 {
        Some(Instant::now() + Duration::from_millis(timeout_ms))
    } else {
        None
    };
    while filled < buf.len() {
        if let Some(d) = deadline {
            let remain = d.saturating_duration_since(Instant::now());
            if remain.is_zero() {
                return err("read timeout");
            }
            s.set_read_timeout(Some(remain.min(Duration::from_millis(500)))).ok();
        } else {
            s.set_read_timeout(None).ok();
        }
        match s.read(&mut buf[filled..]) {
            Ok(0) => {
                if filled == 0 {
                    return Ok(false);
                }
                return err("eof in the middle of packet");
            }
            Ok(k) => filled += k,
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                continue; // deadline 已在循环头判
            }
            Err(e) => return err(format!("read: {e}")),
        }
    }
    Ok(true)
}

#[derive(Clone)]
pub struct StartParams {
    pub max_size: u32,
    pub bit_rate: u32,
    pub fps: u32,
    /// 请求音频流（浏览器 WebCodecs 播放；设备不支持时自动回退纯视频）
    pub audio: bool,
    /// 创建虚拟副屏（W, H, DPI）：应用独立窗口的基石（与 qemu 窗口模式无关，
    /// 无头下帧尺寸也正确——桌面版的 qemu multidisplay 在无头模式反而会信箱化）
    pub new_display: Option<(u32, u32, u32)>,
    /// 会话建立后在该副屏启动的应用包名
    pub start_app: Option<String>,
}

/// 启动完整会话（阻塞；冷启 server + dexopt 可达十秒级）。
pub fn start(serial: &str, jar: &str, p: &StartParams) -> AdbResult<Session> {
    let remote = remote_jar_path();
    let local = std::fs::read(jar).map_err(|e| adb::AdbError(format!("读取 server jar: {e}")))?;
    // 魅族 FlymeOS / 部分 TV 会清理 /data/local/tmp 的 jar——用「远端 stat
    // 尺寸一致则跳过」代替每次全量推：被清理时 stat 失败/尺寸不符照常重推，
    // 而省电劣化链路（实测 RTT ~1s 的无线真机）上 733KB 推送是开流时延大头。
    let remote_size = adb::shell_blocking(serial, &format!("stat -c %s {remote} 2>/dev/null"), 8000)
        .ok()
        .and_then(|o| String::from_utf8_lossy(&o).trim().parse::<u64>().ok());
    if remote_size != Some(local.len() as u64) {
        tracing::info!("推送 scrcpy server jar（{}B，远端缺失/不符）", local.len());
        adb::push_bytes(serial, &local, &remote)?;
        // sync 流 → shell 流快速连续建链时，server 桥上偶发假流（新链读到
        // 上一链的 FIN 时序残留，实测 ~30% 概率）。推完探测确认落盘（荣耀
        // MagicOS 会即时清外来 jar，探测也兜住"推了即被清"的极端时序），
        // 最多 3 次；每次探测间隔 300ms 让前链完全收尾。
        for _ in 0..3 {
            std::thread::sleep(Duration::from_millis(300));
            let ok = adb::shell_blocking(
                serial,
                &format!("ls {remote} >/dev/null 2>&1 && echo Y"),
                5000,
            )
            .map(|o| String::from_utf8_lossy(&o).contains('Y'))
            .unwrap_or(false);
            if ok {
                break;
            }
            tracing::debug!("jar 落盘探测未过，重推");
            adb::push_bytes(serial, &local, &remote)?;
        }
    }

    let scid: u32 = rand_scid();
    let socket_name = format!("scrcpy_{scid:08x}");
    let port = free_port()?;
    if port == 0 {
        return err("分配空闲端口失败");
    }
    adb::forward_abstract(serial, port, &socket_name)?;

    // 组装 server 命令行（与 toArgs 逐字段对齐；scid 双发是 fork 的既定怪癖）
    // 音频编码：本产品镜像/常见 TV 的可用集里 AAC 浏览器兼容最好
    //（WebCodecs mp4a.40.2 通用；opus 在部分 TV 缺编码器——TCL 实测只有
    // aac/flac hybrid）。设备不支持音频采集时 server 会下发 disable 码。
    let audio_args = if p.audio {
        "audio=true audio_codec=aac audio_bit_rate=128000"
    } else {
        "audio=false"
    };
    // 编码参数（质量+低延迟）：
    //   i-frame-interval=2s —— 关键帧更密，卡顿/丢帧后画面恢复更快（默认 10s）
    //   repeat-previous-sps —— 静止画面省带宽，动起来立刻有 SPS 可解码
    //   android:video_bit_rate 由调用方按链路给（USB 20M）
    let mut args = format!(
        "scid={scid:08x} log_level=info video=true control=true {audio_args} \
         video_codec=h264 video_bit_rate={} max_size={} scid={scid:x} max_fps={} \
         stay_awake=true power_off_on_close=false keep_active=false show_touches=false \
         tunnel_forward=true clipboard_autosync=true",
        p.bit_rate, p.max_size, p.fps
    );
    if let Some((w, h, dpi)) = p.new_display {
        args.push_str(&format!(
            " new_display={w}x{h}/{dpi} vd_destroy_content=true vd_system_decorations=true"
        ));
    }
    let cmd = format!(
        "CLASSPATH={remote} app_process / {SERVER_CLASS} {SERVER_VERSION} {args}"
    );
    let server = adb::shell_stream_scrcpy(serial, &cmd)?;

    // 连 video socket：forward 在 server 未监听时也会接受 TCP 后立即关闭，
    // 以「读到 dummy 字节」为就绪判据；首探放宽（server 冷启 + dexopt 数秒）。
    // 去掉固定 400ms sleep——connect 失败/立即关闭本身就有 200ms 重试节奏。
    let deadline = Instant::now() + Duration::from_secs(20);
    let mut video: Option<TcpStream> = None;
    let mut first = true;
    while Instant::now() < deadline {
        match TcpStream::connect(("127.0.0.1", port)) {
            Ok(mut conn) => {
                conn.set_nodelay(true).ok();
                let mut b = [0u8; 1];
                let wait = if first { 12000 } else { 2000 };
                match read_exact(&mut conn, &mut b, wait) {
                    Ok(true) => {
                        video = Some(conn);
                        break;
                    }
                    _ => {
                        first = false;
                        std::thread::sleep(Duration::from_millis(200));
                    }
                }
            }
            Err(_) => std::thread::sleep(Duration::from_millis(200)),
        }
    }
    let mut video = match video {
        Some(v) => v,
        None => {
            server.shutdown();
            let _ = adb::remove_forward(serial, port);
            return err("connection to scrcpy server timed out");
        }
    };

    // audio socket（启用音频时；server 在 video 后按序 accept 它，再 control）
    let audio = if p.audio {
        let adeadline = Instant::now() + Duration::from_secs(15);
        let mut a = None;
        while Instant::now() < adeadline {
            if let Ok(conn) = TcpStream::connect(("127.0.0.1", port)) {
                conn.set_nodelay(true).ok();
                a = Some(conn);
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        Some(a.ok_or_else(|| adb::AdbError("audio socket 连接超时".into()))?)
    } else {
        None
    };

    // control socket（video/audio 之后；全部 accept 完成才发设备名）
    let control = {
        let cdeadline = Instant::now() + Duration::from_secs(15);
        let mut c = None;
        while Instant::now() < cdeadline {
            if let Ok(conn) = TcpStream::connect(("127.0.0.1", port)) {
                conn.set_nodelay(true).ok();
                c = Some(conn);
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        c.ok_or_else(|| adb::AdbError("control socket 连接超时".into()))?
    };

    // 设备名（64 字节）+ codec id（4 字节）
    let mut name = [0u8; 64];
    match read_exact(&mut video, &mut name, 15000)? {
        true => {}
        false => return err("读设备名 EOF"),
    }
    let device_name = String::from_utf8_lossy(&name)
        .trim_end_matches('\0')
        .to_string();
    let mut codec = [0u8; 4];
    match read_exact(&mut video, &mut codec, 15000)? {
        true => {}
        false => return err("读 codec id EOF"),
    }
    let codec = match &codec {
        b"h264" => "h264",
        b"h265" => "h265",
        b"av1\x00" => "av1",
        other => return err(format!("未知 codec id: {}", String::from_utf8_lossy(other))),
    }
    .to_string();

    // 音频流 meta：仅 4 字节 codec id（无 dummy/设备名——那些只在首个 socket）。
    // id=0 设备禁用音频（静默回退纯视频）；id=1 配置错误（同样降级，不杀视频）
    let mut audio = audio;
    let mut audio_codec = String::new();
    if let Some(mut a) = audio.take() {
        let mut cid = [0u8; 4];
        match read_exact(&mut a, &mut cid, 10000) {
            Ok(true) => {
                let id = u32::from_be_bytes(cid);
                match id {
                    0 | 1 => {
                        tracing::warn!("设备侧音频不可用（disable 码 {id}），回退纯视频");
                        let _ = a.shutdown(std::net::Shutdown::Both);
                    }
                    _ => {
                        audio_codec = match &cid {
                            b"\0aac" => "aac".into(),
                            b"opus" => "opus".into(),
                            b"flac" => "flac".into(),
                            other => format!("unknown:{}", String::from_utf8_lossy(other)),
                        };
                        if audio_codec.starts_with("unknown") {
                            tracing::warn!("未知音频 codec id {id:#x}，忽略音频流");
                            let _ = a.shutdown(std::net::Shutdown::Both);
                        } else {
                            audio = Some(a);
                        }
                    }
                }
            }
            _ => {
                tracing::warn!("音频 codec id 读取失败，回退纯视频");
                let _ = a.shutdown(std::net::Shutdown::Both);
            }
        }
    }

    // 副屏会话：建立后即下发 StartApp（应用直接落在该副屏）
    if let Some(app) = &p.start_app {
        if let Some(msg) = control::start_app(app) {
            let mut c = control.try_clone().ok();
            if let Some(cc) = c.as_mut() {
                use std::io::Write as _;
                let _ = cc.write_all(&msg);
            }
        }
    }

    Ok(Session {
        serial: serial.to_string(),
        device_name,
        codec,
        video,
        control: Some(control),
        audio,
        audio_codec,
        width: 0,
        height: 0,
        forward_port: port,
        server,
    })
}

fn rand_scid() -> u32 {
    use std::time::{SystemTime, UNIX_EPOCH};
    let t = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.subsec_nanos() as u64 ^ d.as_secs())
        .unwrap_or(0);
    let pid = std::process::id() as u64;
    let mut v = t ^ (pid << 16) ^ 0x9e3779b97f4a7c15u64;
    v ^= v >> 33;
    v = v.wrapping_mul(0xff51afd7ed558ccd);
    (v as u32) & 0x7FFF_FFFF
}

// ===================== 控制消息编码（与 control.cpp 逐字节对齐）=====================

pub mod control {
    pub const TYPE_INJECT_KEYCODE: u8 = 0;
    pub const TYPE_INJECT_TEXT: u8 = 1;
    pub const TYPE_INJECT_TOUCH: u8 = 2;
    pub const TYPE_INJECT_SCROLL: u8 = 3;
    pub const TYPE_RESET_VIDEO: u8 = 17;

    pub const ACTION_DOWN: u8 = 0;
    pub const ACTION_UP: u8 = 1;
    pub const ACTION_MOVE: u8 = 2;

    pub const POINTER_ID_MOUSE: u64 = u64::MAX;

    fn push_u16be(b: &mut Vec<u8>, v: u16) {
        b.extend_from_slice(&v.to_be_bytes());
    }
    fn push_u32be(b: &mut Vec<u8>, v: u32) {
        b.extend_from_slice(&v.to_be_bytes());
    }
    fn push_u64be(b: &mut Vec<u8>, v: u64) {
        b.extend_from_slice(&v.to_be_bytes());
    }
    fn float_to_u16fp(v: f32) -> u16 {
        if v >= 1.0 {
            return 0xffff;
        }
        if v <= 0.0 {
            return 0;
        }
        (v * 65536.0) as u16
    }
    fn float_to_i16fp(v: f32) -> i16 {
        (v.clamp(-1.0, 1.0) * 32767.0) as i16
    }

    pub fn inject_keycode(down: bool, keycode: i32) -> Vec<u8> {
        let mut b = Vec::with_capacity(14);
        b.push(TYPE_INJECT_KEYCODE);
        b.push(if down { 0 } else { 1 });
        push_u32be(&mut b, keycode as u32);
        push_u32be(&mut b, 0); // repeat
        push_u32be(&mut b, 0); // metaState
        b
    }

    pub fn inject_text(text: &str) -> Option<Vec<u8>> {
        let utf8 = text.as_bytes();
        if utf8.len() > 300 {
            return None;
        }
        let mut b = Vec::with_capacity(5 + utf8.len());
        b.push(TYPE_INJECT_TEXT);
        push_u32be(&mut b, utf8.len() as u32);
        b.extend_from_slice(utf8);
        Some(b)
    }

    #[allow(clippy::too_many_arguments)]
    pub fn inject_touch(
        action: u8,
        pointer_id: u64,
        x: i32,
        y: i32,
        screen_w: u16,
        screen_h: u16,
        pressure: f32,
        action_button: i32,
        buttons: i32,
    ) -> Vec<u8> {
        let mut b = Vec::with_capacity(28);
        b.push(TYPE_INJECT_TOUCH);
        b.push(action);
        push_u64be(&mut b, pointer_id);
        push_u32be(&mut b, x as u32);
        push_u32be(&mut b, y as u32);
        push_u16be(&mut b, screen_w);
        push_u16be(&mut b, screen_h);
        push_u16be(&mut b, float_to_u16fp(pressure));
        push_u32be(&mut b, action_button as u32);
        push_u32be(&mut b, buttons as u32);
        b
    }

    pub fn inject_scroll(x: i32, y: i32, w: u16, h: u16, hs: f32, vs: f32) -> Vec<u8> {
        let mut b = Vec::with_capacity(24);
        b.push(TYPE_INJECT_SCROLL);
        push_u32be(&mut b, x as u32);
        push_u32be(&mut b, y as u32);
        push_u16be(&mut b, w);
        push_u16be(&mut b, h);
        push_u16be(&mut b, float_to_i16fp(hs) as u16);
        push_u16be(&mut b, float_to_i16fp(vs) as u16);
        push_u32be(&mut b, 0);
        b
    }

    pub fn reset_video() -> Vec<u8> {
        vec![TYPE_RESET_VIDEO]
    }

    pub const TYPE_START_APP: u8 = 16;
    pub const TYPE_SET_CLIPBOARD: u8 = 9;

    /// 设置设备剪贴板（sequence 回执用随机值；paste=false 仅同步不触发粘贴动作）
    pub fn set_clipboard(sequence: u64, paste: bool, text: &str) -> Option<Vec<u8>> {
        let utf8 = text.as_bytes();
        if utf8.len() > 1_000_000 {
            return None;
        }
        let mut b = Vec::with_capacity(14 + utf8.len());
        b.push(TYPE_SET_CLIPBOARD);
        push_u64be(&mut b, sequence);
        b.push(if paste { 1 } else { 0 });
        push_u32be(&mut b, utf8.len() as u32);
        b.extend_from_slice(utf8);
        Some(b)
    }

    /// 在会话副屏上启动应用（包名；上游兼容 "?action=..." 等修饰，这里只用包名）
    pub fn start_app(name: &str) -> Option<Vec<u8>> {
        let b = name.as_bytes();
        if b.is_empty() || b.len() > 255 {
            return None;
        }
        let mut out = Vec::with_capacity(2 + b.len());
        out.push(TYPE_START_APP);
        out.push(b.len() as u8);
        out.extend_from_slice(b);
        Some(out)
    }
}


// ===================== 音频流解复用（独立线程持有 socket） =====================

/// 音频 socket 上的包读取器。协议（上游 Streamer.writePacket，与视频同构）：
/// [u64 pts+flags（bit62=CONFIG）][u32 len][data]；流干净结束返回 Ok(None)。
pub struct AudioDemuxer {
    sock: TcpStream,
}

impl AudioDemuxer {
    pub fn new(sock: TcpStream) -> Self {
        AudioDemuxer { sock }
    }

    pub fn next_packet(&mut self) -> AdbResult<Option<AudioPacket>> {
        let mut h8 = [0u8; 8];
        match read_exact(&mut self.sock, &mut h8, 0) {
            Ok(true) => {}
            Ok(false) => return Ok(None),
            Err(e) => return Err(e),
        }
        let pts_flags = u64::from_be_bytes(h8);
        let config = pts_flags & FLAG_CONFIG != 0;
        let pts = pts_flags & !(FLAG_CONFIG | FLAG_KEYFRAME);
        let mut l4 = [0u8; 4];
        match read_exact(&mut self.sock, &mut l4, 0)? {
            true => {}
            false => return err("音频包长度前缀前 EOF"),
        }
        let len = u32::from_be_bytes(l4) as usize;
        let mut data = vec![0u8; len];
        if len > 0 {
            match read_exact(&mut self.sock, &mut data, 0)? {
                true => {}
                false => return err("音频包中途 EOF"),
            }
        }
        Ok(Some(AudioPacket { pts, config, data }))
    }
}
