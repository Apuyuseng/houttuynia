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

pub struct StartParams {
    pub max_size: u32,
    pub bit_rate: u32,
    pub fps: u32,
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
    // 同尺寸跳过推送（jar 常驻设备；升级内容变化时尺寸不同才重推）
    let skip = adb::remote_file_size(serial, &remote) == Some(local.len() as u64);
    if !skip {
        adb::push_bytes(serial, &local, &remote)?;
    }

    let scid: u32 = rand_scid();
    let socket_name = format!("scrcpy_{scid:08x}");
    let port = free_port()?;
    if port == 0 {
        return err("分配空闲端口失败");
    }
    adb::forward_abstract(serial, port, &socket_name)?;

    // 组装 server 命令行（与 toArgs 逐字段对齐；scid 双发是 fork 的既定怪癖）
    let mut args = format!(
        "scid={scid:08x} log_level=info video=true audio=false control=true \
         video_codec=h264 video_bit_rate={} max_size={} scid={scid:x} max_fps={} \
         stay_awake=true power_off_on_close=false keep_active=false show_touches=false \
         tunnel_forward=true",
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
    let server = adb::shell_stream(serial, &cmd)?;

    // 连 video socket：forward 在 server 未监听时也会接受 TCP 后立即关闭，
    // 以「读到 dummy 字节」为就绪判据；首探放宽（server 冷启 + dexopt 数秒）。
    std::thread::sleep(Duration::from_millis(400));
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

    // control socket（server 在 video 后按序 accept；双 accept 完成才发设备名）
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
