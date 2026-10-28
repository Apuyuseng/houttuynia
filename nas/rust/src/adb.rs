//! adb 客户端门面：全部设备操作走集成传输层（transport.rs，直连 adbd，
//! 零外部 adb server 依赖）。函数签名与旧「smart-socket 5037 客户端」时期
//! 保持一致，api.rs / scrcpy.rs / filemgr.rs / icons.rs / guard.rs 零改动。
//!
//! 长驻流（终端 pty / scrcpy server / 端口转发）经 transport::Stream 的
//! 环回 TCP 桥暴露 TcpStream，复用原有读 写/超时/分帧代码。
//!
//! 仍保留的能力：mDNS DNS-SD 发现（组播+广播双通道）、无线配对
//! （pairing.rs 纯 Rust SPAKE2+TLS）、外部 adb server 劝退（host:kill，
//! 释放 USB 接口占用）。

use std::io::{Read, Write};
use std::net::{Shutdown, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, LazyLock, Mutex};
use std::time::{Duration, Instant};

use crate::transport::{self, DeviceConn, Stream};

#[derive(Debug)]
pub struct AdbError(pub String);

impl std::fmt::Display for AdbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}
impl std::error::Error for AdbError {}

pub type AdbResult<T> = Result<T, AdbError>;

fn err<T>(msg: impl Into<String>) -> AdbResult<T> {
    Err(AdbError(msg.into()))
}

fn now_deadline(timeout_ms: u64) -> Instant {
    Instant::now() + Duration::from_millis(timeout_ms)
}

// ---------------- 连接获取 ----------------

/// serial → 存活连接（复用/重连）。含 ":" 的 serial 视为无线 TCP 地址。
fn conn(serial: &str) -> AdbResult<Arc<DeviceConn>> {
    let tcp = if serial.contains(':') { Some(serial.to_string()) } else { None };
    transport::get_conn(serial, tcp.as_deref()).map_err(AdbError)
}

// ---------------- 设备枚举 ----------------

/// 设备列表项。
#[derive(Debug, Clone, serde::Serialize)]
pub struct Device {
    pub serial: String,
    pub state: String,
}

/// USB 扫描 + 无线注册表 → 设备列表。
/// 注：USB 在线即报 device（授权状态在握手时才可知：未授权设备首次
/// shell 会在手机弹授权框，等待用户允许）。
pub fn devices() -> AdbResult<Vec<Device>> {
    let mut out: Vec<Device> = Vec::new();
    // 官方 adb server 的 host:devices 是 USB 设备的权威来源（server 桥模式）
    for (serial, state) in transport::server_devices() {
        if serial.starts_with("emulator-") {
            continue;
        }
        if out.iter().any(|d| d.serial == serial) {
            continue;
        }
        out.push(Device { serial, state });
    }
    // 无线设备（我们注册表里的直连/桥接会话）
    for (serial, _kind) in transport::list_connected() {
        if out.iter().any(|d| d.serial == serial) {
            continue;
        }
        out.push(Device { serial, state: "device".into() });
    }
    // 无 server 环境兜底：本地 USB 扫描（MWINE_NATIVE_USB 模式）
    if out.is_empty() && std::env::var("MWINE_NATIVE_USB").ok().as_deref() == Some("1") {
        for d in crate::usb::scan() {
            out.push(Device { serial: d.serial, state: "device".into() });
        }
    }
    Ok(out)
}

/// 找第一个在线模拟器 serial（遗留；emulator-N 经 TCP 回环接入）。
pub fn online_emulator() -> Option<String> {
    devices().ok()?.into_iter().find_map(|d| {
        if d.state == "device" && d.serial.starts_with("emulator-") {
            Some(d.serial)
        } else {
            None
        }
    })
}

// ---------------- shell（一次性命令） ----------------

/// 执行 shell 命令收全部输出（v1 通道：FlymeOS 上 v2+app_process 有崩溃怪癖，
/// 且一次性命令无需 v2 的退出码帧）。
pub fn shell_blocking(serial: &str, cmd: &str, timeout_ms: u64) -> AdbResult<Vec<u8>> {
    let c = conn(serial)?;
    let mut st = c.open_stream(&format!("shell:{cmd}")).map_err(AdbError)?;
    let deadline = now_deadline(timeout_ms.max(1000));
    let mut out = Vec::new();
    loop {
        let remain = deadline.saturating_duration_since(Instant::now());
        if remain.is_zero() {
            return err("shell 读取超时");
        }
        match st.read_timeout(remain) {
            Ok(Some(d)) => out.extend_from_slice(&d),
            Ok(None) => break,
            Err(()) => return err("shell 读取超时"),
        }
    }
    Ok(out)
}

/// USB 真机切 tcpip 模式（之后可无线 adb_connect ip:5555）。
pub fn adb_tcpip(serial: &str, port: u16) -> AdbResult<String> {
    let c = conn(serial)?;
    let mut st = c
        .open_stream(&format!("tcpip:{port}"))
        .map_err(|e| AdbError(format!("tcpip 切换失败: {e}")))?;
    // adbd 复位监听前会 CLSE；读到流结束即认为已受理
    let _ = st.read_timeout(Duration::from_secs(3));
    Ok(format!("tcpip {port} 已受理（adbd 即将重启无线监听）"))
}

// ---------------- 长驻 shell（环回 TCP 桥） ----------------

pub struct ShellProcess {
    pub stream: TcpStream,
}

impl ShellProcess {
    pub fn shutdown(&self) {
        let _ = self.stream.shutdown(Shutdown::Both);
    }
}

fn bridged(stream: Stream, what: &str) -> AdbResult<ShellProcess> {
    let stream_tcp = stream
        .into_tcp_bridge()
        .map_err(|e| AdbError(format!("{what} 环回桥失败: {e}")))?;
    Ok(ShellProcess { stream: stream_tcp })
}

/// 以 shell v1（`shell:<cmd>`）打开命令进程（流式）。
pub fn shell_v1_stream(serial: &str, cmd: &str) -> AdbResult<ShellProcess> {
    let c = conn(serial)?;
    let st = c.open_stream(&format!("shell:{cmd}")).map_err(AdbError)?;
    bridged(st, "shell v1")
}

/// 长驻 shell：v2 raw 优先，失败回落 v1。
pub fn shell_stream(serial: &str, cmd: &str) -> AdbResult<ShellProcess> {
    let c = conn(serial)?;
    match c.open_stream(&format!("shell,v2,raw:{cmd}")) {
        Ok(st) => bridged(st, "shell v2"),
        Err(_) => {
            let st = c.open_stream(&format!("shell:{cmd}")).map_err(AdbError)?;
            bridged(st, "shell v1")
        }
    }
}

/// scrcpy server 拉起通道：先 v1 后 v2。
///
/// 背景：魅族 FlymeOS（Android 16）固件的 `register_android_util_SeempLog`
/// 会在 v2（pty）通道下让 app_process 崩溃（SeempLog 类缺失 → ART abort），
/// 但 v1 通道正常。官方 scrcpy 桌面版始终走 v1 也是同样原因的历史选择。
pub fn shell_stream_scrcpy(serial: &str, cmd: &str) -> AdbResult<ShellProcess> {
    match shell_v1_stream(serial, cmd) {
        Ok(p) => Ok(p),
        Err(e) => {
            tracing::debug!("shell v1 失败，回落 v2: {e}");
            shell_stream(serial, cmd)
        }
    }
}

/// 交互式终端会话：v2 pty 优先（提示符/回显完整），不支持则 v1 交互 shell。
pub fn shell_pty(serial: &str) -> AdbResult<(ShellProcess, bool)> {
    let c = conn(serial)?;
    match c.open_stream("shell,v2,pty:") {
        Ok(st) => Ok((bridged(st, "pty v2")?, true)),
        Err(_) => {
            let st = c.open_stream("shell:").map_err(AdbError)?;
            Ok((bridged(st, "pty v1")?, false))
        }
    }
}

pub fn shell_pty_full(serial: &str) -> AdbResult<(ShellProcess, bool)> {
    shell_pty(serial)
}

// ===================== shell v2 双工（WS 桥用；桥上的 TcpStream） =====================

/// shell v2 帧 id
pub const V2_STDIN: u8 = 0;
pub const V2_STDOUT: u8 = 1;
pub const V2_STDERR: u8 = 2;
pub const V2_EXIT: u8 = 3;
pub const V2_CLOSE_STDIN: u8 = 4;
pub const V2_WINSZ: u8 = 5;

/// 编码一帧：id u8 + len u32 LE + payload
pub fn v2_frame(id: u8, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(5 + payload.len());
    out.push(id);
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(payload);
    out
}

/// 窗口尺寸变化帧（"rows:cols" 文本载荷）
pub fn v2_winsz(cols: u16, rows: u16) -> Vec<u8> {
    v2_frame(V2_WINSZ, format!("{rows}:{cols}").as_bytes())
}

/// 从 v2 流读一帧：(id, payload)；EOF → Ok(None)
pub fn v2_read_frame(s: &mut TcpStream) -> AdbResult<Option<(u8, Vec<u8>)>> {
    let mut hdr = [0u8; 5];
    match read_frame_exact(s, &mut hdr) {
        Ok(true) => {}
        Ok(false) => return Ok(None),
        Err(e) => return Err(e),
    }
    let id = hdr[0];
    let len = u32::from_le_bytes([hdr[1], hdr[2], hdr[3], hdr[4]]) as usize;
    let mut payload = vec![0u8; len];
    match read_frame_exact(s, &mut payload) {
        Ok(true) => Ok(Some((id, payload))),
        Ok(false) => err("v2 帧载荷 EOF"),
        Err(e) => Err(e),
    }
}

fn read_frame_exact(s: &mut TcpStream, buf: &mut [u8]) -> AdbResult<bool> {
    let mut filled = 0;
    while filled < buf.len() {
        match s.read(&mut buf[filled..]) {
            Ok(0) => {
                if filled == 0 {
                    return Ok(false);
                }
                return err("eof in the middle of frame");
            }
            Ok(k) => filled += k,
            Err(e) => return err(format!("read: {e}")),
        }
    }
    Ok(true)
}

// ===================== sync 协议（STAT/SEND/RECV/LIST） =====================

/// 一条 sync: 流上的请求-应答会话（带接收缓冲）。
struct Sync {
    stream: Stream,
    buf: Vec<u8>,
}

impl Sync {
    fn open(serial: &str) -> AdbResult<Sync> {
        let c = conn(serial)?;
        let stream = c.open_stream("sync:").map_err(AdbError)?;
        Ok(Sync { stream, buf: Vec::new() })
    }

    fn req(&mut self, cmd: &[u8; 4], payload: &[u8]) -> AdbResult<()> {
        let mut m = Vec::with_capacity(8 + payload.len());
        m.extend_from_slice(cmd);
        m.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        m.extend_from_slice(payload);
        self.stream.write(&m).map_err(AdbError)
    }

    /// 从流读满 n 字节
    fn fill(&mut self, n: usize) -> AdbResult<()> {
        self.fill_long(n, 30_000)
    }

    fn fill_long(&mut self, n: usize, timeout_ms: u64) -> AdbResult<()> {
        if std::env::var("MWINE_SYNC_DEBUG").is_ok() {
            eprintln!("[sync] fill 需 {n}B，已有 {}B", self.buf.len());
        }
        let deadline = now_deadline(timeout_ms);
        while self.buf.len() < n {
            let remain = deadline.saturating_duration_since(Instant::now());
            if remain.is_zero() {
                return err("sync 读取超时");
            }
            match self.stream.read_timeout(remain) {
                Ok(Some(d)) => {
                    if std::env::var("MWINE_SYNC_DEBUG").is_ok() {
                        eprintln!("[sync] 收 {}B", d.len());
                    }
                    self.buf.extend_from_slice(&d);
                }
                Ok(None) => return err("sync 流被设备关闭"),
                Err(()) => {
                    if std::env::var("MWINE_SYNC_DEBUG").is_ok() {
                        eprintln!("[sync] 超时，已收 {}B", self.buf.len());
                    }
                    return err("sync 读取超时");
                }
            }
        }
        Ok(())
    }

    fn take(&mut self, n: usize) -> Vec<u8> {
        let out: Vec<u8> = self.buf.drain(..n).collect();
        out
    }

    /// 读一个应答包：id(4) + len(4) + 载荷(len)
    fn read_packet(&mut self) -> AdbResult<([u8; 4], Vec<u8>)> {
        self.read_packet_long(30_000)
    }

    fn read_packet_long(&mut self, timeout_ms: u64) -> AdbResult<([u8; 4], Vec<u8>)> {
        self.fill_long(8, timeout_ms)?;
        let head = self.take(8);
        let mut id = [0u8; 4];
        id.copy_from_slice(&head[..4]);
        let len = u32::from_le_bytes([head[4], head[5], head[6], head[7]]) as usize;
        if len > 64 * 1024 * 1024 {
            return err(format!("sync 应答长度异常: {len}"));
        }
        if len > 0 {
            self.fill_long(len, timeout_ms)?;
        }
        let data = if len > 0 { self.take(len) } else { Vec::new() };
        Ok((id, data))
    }

    /// STAT：应答为定长 [id=STAT][mode][size][mtime]（16B，无长度字段——
    /// 与 DATA/OKAY/FAIL 的 [id][len][payload] 变长格式不同，勿用 read_packet）
    fn stat(&mut self, path: &str) -> AdbResult<(u32, u32, u32)> {
        self.req(b"STAT", path.as_bytes())?;
        self.fill(16)?;
        let head = self.take(16);
        if &head[..4] != b"STAT" {
            return err(format!("STAT 应答异常: {}", String::from_utf8_lossy(&head[..4])));
        }
        let mode = u32::from_le_bytes([head[4], head[5], head[6], head[7]]);
        let size = u32::from_le_bytes([head[8], head[9], head[10], head[11]]);
        let mtime = u32::from_le_bytes([head[12], head[13], head[14], head[15]]);
        Ok((mode, size, mtime))
    }

    /// RECV：逐块回调（不整包缓冲）
    fn recv(&mut self, path: &str, mut on_data: impl FnMut(&[u8]) -> AdbResult<()>) -> AdbResult<()> {
        self.req(b"RECV", path.as_bytes())?;
        loop {
            let (id, data) = self.read_packet()?;
            match &id[..] {
                b"DATA" => on_data(&data)?,
                b"DONE" => return Ok(()),
                b"FAIL" => {
                    return err(format!("pull FAIL: {}", String::from_utf8_lossy(&data).trim()))
                }
                other => return err(format!("sync RECV 异常: {}", String::from_utf8_lossy(other))),
            }
        }
    }
}

/// 设备 stat：读文件大小（缺 stat -c 的老 toolbox 用 ls 兜底）。
pub fn remote_file_size(serial: &str, path: &str) -> Option<u64> {
    let out = shell_blocking(
        serial,
        &format!("stat -c %s '{path}' 2>/dev/null || ls -l '{path}' 2>/dev/null"),
        5000,
    )
    .ok()?;
    let text = String::from_utf8_lossy(&out);
    let t = text.trim();
    if let Ok(n) = t.parse::<u64>() {
        return Some(n);
    }
    // ls -l: "-rw-rw-rw- root root 733706 2026-10-20 17:17 path"
    t.split_whitespace().nth(4)?.parse::<u64>().ok()
}

/// 从设备拉取文件（sync STAT + RECV，整包 ≤max_bytes）。
pub fn pull_file(serial: &str, remote: &str, max_bytes: usize) -> AdbResult<Vec<u8>> {
    let mut s = Sync::open(serial)?;
    let (_mode, size, _mtime) = s.stat(remote)?;
    if size as usize > max_bytes {
        return err(format!("文件超过上限: {size} > {max_bytes}"));
    }
    let mut out = Vec::with_capacity(size as usize);
    s.recv(remote, |chunk| {
        out.extend_from_slice(chunk);
        Ok(())
    })?;
    Ok(out)
}

/// 流式拉取：sync STAT 取尺寸后 RECV 循环，DATA 块经 channel 推给消费方
/// （不整包缓冲——大文件下载内存恒定）。返回文件尺寸（Content-Length 用）。
/// 由 spawn_blocking 线程调用（内部用 blocking_send）。
pub fn pull_stream(
    serial: &str,
    remote: &str,
    tx: tokio::sync::mpsc::Sender<Vec<u8>>,
) -> AdbResult<u64> {
    let mut s = Sync::open(serial)?;
    let (_mode, size, _mtime) = s.stat(remote)?;
    let mut got: u64 = 0;
    s.recv(remote, |chunk| {
        got += chunk.len() as u64;
        if tx.blocking_send(chunk.to_vec()).is_err() {
            return err("下载客户端已断开");
        }
        Ok(())
    })?;
    if size == 0 {
        Ok(got) // 空文件 STAT 报 0，以实际收到的为准
    } else {
        Ok(size as u64)
    }
}

/// 推送字节到设备路径（sync SEND/DATA/DONE）。
pub fn push_bytes(serial: &str, data: &[u8], remote: &str) -> AdbResult<()> {
    let mut push = SyncPush::start(serial, remote)?;
    push.write(data)?;
    push.finish()
}

/// 流式推送（sync SEND/DATA/DONE 的增量版——不整包缓冲，上传内存恒定）。
pub struct SyncPush {
    s: Sync,
    buf: Vec<u8>,
}

impl SyncPush {
    pub fn start(serial: &str, remote: &str) -> AdbResult<Self> {
        let mut s = Sync::open(serial)?;
        // SEND 载荷 "path,mode"（模式 0644）
        let path_mode = format!("{remote},420");
        s.req(b"SEND", path_mode.as_bytes())?;
        Ok(SyncPush { s, buf: Vec::with_capacity(65536) })
    }

    /// 追加数据（自动按 ≤64KB DATA 帧发出，保留尾巴等后续拼帧）
    pub fn write(&mut self, data: &[u8]) -> AdbResult<()> {
        self.buf.extend_from_slice(data);
        while self.buf.len() >= 65536 {
            let chunk: Vec<u8> = self.buf.drain(..65536).collect();
            self.s.req(b"DATA", &chunk)?;
        }
        Ok(())
    }

    /// 收尾：剩余数据 + DONE + 等待设备侧 OKAY（大文件写盘较慢，放宽到 5 分钟）
    pub fn finish(mut self) -> AdbResult<()> {
        if !self.buf.is_empty() {
            let chunk = std::mem::take(&mut self.buf);
            self.s.req(b"DATA", &chunk)?;
        }
        let mtime = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as u32)
            .unwrap_or(0);
        self.s.req(b"DONE", &mtime.to_le_bytes())?;
        let (id, data) = self.s.read_packet_long(300_000)?;
        match &id[..] {
            b"OKAY" => Ok(()),
            b"FAIL" => err(format!("push FAIL: {}", String::from_utf8_lossy(&data).trim())),
            other => err(format!("sync 应答异常: {}", String::from_utf8_lossy(other))),
        }
    }
}

/// 远程文件区间读取（dd 分块）：真机上图标提取只读尾部目录+图标切片，
/// 避免整包拉取（APK 动辄几十 MB）。
pub fn pull_range(serial: &str, path: &str, offset: u64, len: u64) -> AdbResult<Vec<u8>> {
    if len == 0 {
        return Ok(Vec::new());
    }
    const BS: u64 = 4096;
    let skip = offset / BS;
    let end = offset + len;
    let count = (end + BS - 1) / BS - skip;
    let out = shell_blocking(
        serial,
        &format!("dd if='{path}' bs={BS} skip={skip} count={count} 2>/dev/null"),
        60000,
    )?;
    let start = (offset - skip * BS) as usize;
    let want = len as usize;
    if out.len() < start + want {
        return err(format!("dd 区间读取不足: {} < {}", out.len(), start + want));
    }
    Ok(out[start..start + want].to_vec())
}

// ===================== 端口转发（自托管监听桥） =====================

struct ForwardEntry {
    stop: Arc<AtomicBool>,
}

static FORWARDS: LazyLock<Mutex<std::collections::HashMap<(String, u16), Arc<ForwardEntry>>>> =
    LazyLock::new(|| Mutex::new(std::collections::HashMap::new()));

/// 注册端口转发：127.0.0.1:tcp_port → 设备 abstract socket。
/// 自托管监听（原 adb server 的 host:forward 等价物）：每个进来的 TCP 连接
/// 对应一路 `localabstract:` 流双向泵接。
pub fn forward_abstract(serial: &str, tcp_port: u16, abstract_name: &str) -> AdbResult<()> {
    // 同端口旧转发先撤
    let _ = remove_forward(serial, tcp_port);
    let stop = Arc::new(AtomicBool::new(false));
    FORWARDS
        .lock()
        .unwrap()
        .insert((serial.to_string(), tcp_port), Arc::new(ForwardEntry { stop: Arc::clone(&stop) }));
    let serial_c = serial.to_string();
    let svc = format!("localabstract:{abstract_name}");
    std::thread::spawn(move || {
        let Ok(listener) = std::net::TcpListener::bind(("127.0.0.1", tcp_port)) else {
            return;
        };
        listener.set_nonblocking(true).ok();
        loop {
            if stop.load(Ordering::Relaxed) {
                break;
            }
            match listener.accept() {
                Ok((sock, _)) => {
                    sock.set_nonblocking(false).ok();
                    let (s2, svc2) = (serial_c.clone(), svc.clone());
                    std::thread::spawn(move || pump_forward(&s2, &svc2, sock));
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(150));
                }
                Err(_) => break,
            }
        }
    });
    Ok(())
}

fn pump_forward(serial: &str, svc: &str, mut sock: TcpStream) {
    let Ok(c) = transport::get_conn(serial, None) else { return };
    let Ok(stream) = c.open_stream(svc) else { return };
    let Ok(mut bridged) = stream.into_tcp_bridge() else { return };
    // sock ↔ bridged 双向拼接
    let Ok(mut sock_r) = sock.try_clone() else { return };
    let Ok(mut bridged_r) = bridged.try_clone() else { return };
    let t1 = std::thread::spawn(move || {
        let _ = std::io::copy(&mut sock_r, &mut bridged);
    });
    let _t2 = std::thread::spawn(move || {
        let _ = std::io::copy(&mut bridged_r, &mut sock);
    });
    let _ = t1.join();
}

/// 撤销端口转发。
pub fn remove_forward(serial: &str, tcp_port: u16) -> AdbResult<()> {
    if let Some(e) = FORWARDS.lock().unwrap().remove(&(serial.to_string(), tcp_port)) {
        e.stop.store(true, Ordering::Relaxed);
    }
    Ok(())
}

// ===================== 无线：配对 / 连接 / 断开 / mDNS =====================

/// 无线配对：纯 Rust SPAKE2+TLS 实现（pairing.rs）。
/// 配对成功后手机信任本机 adbkey，后续 adb_connect 即可建立连接。
pub fn adb_pair(addr: &str, code: &str) -> AdbResult<String> {
    crate::adbkeys::load_or_create(); // 确保 adbkey 存在（配对要发给手机）
    crate::pairing::pair(addr, code).map_err(AdbError)
}

/// 连接无线设备（明文 adbd：tcpip 5555 模式；TLS 无线调试暂不支持，
/// 走 USB adb_tcpip 切换后连接）。
pub fn adb_connect(addr: &str) -> AdbResult<String> {
    transport::get_conn(addr, Some(addr))
        .map(|_| format!("connected to {addr}"))
        .map_err(|e| AdbError(format!("failed to connect to {addr}: {e}")))
}

/// 断开无线设备。
pub fn adb_disconnect(serial: &str) -> AdbResult<String> {
    transport::drop_conn(serial);
    Ok(format!("disconnected {serial}"))
}

/// mDNS 发现的 _adb._tcp / _adb-tls-pairing._tcp 服务（adb mdns services 输出）
#[derive(Debug, Clone)]
pub struct MdnsService {
    pub name: String,
    pub svc_type: String,
    pub addr: String,
}

pub fn mdns_services() -> Vec<MdnsService> {
    // 组播 + 广播双通道（多接口主机上组播常因默认路由黑洞）
    let mut list = mdns_services_broadcast();
    for (svc_fqdn, kind) in [
        ("_adb._tcp.local", "_adb._tcp"),
        ("_adb-tls-pairing._tcp.local", "_adb-tls-pairing._tcp"),
        ("_adb-tls-connect._tcp.local", "_adb-tls-connect._tcp"),
    ] {
        for (name, host, port) in mdns_discover(svc_fqdn) {
            let ip = mdns_resolve_a(&host).unwrap_or_else(|| host.clone());
            list.push(MdnsService {
                name,
                svc_type: kind.to_string(),
                addr: format!("{ip}:{port}"),
            });
        }
    }
    list
}

/// DNS-SD 发现：组播 224.0.0.251:5353 PTR 查询，收 SRV 记录拿主机名+端口。
fn mdns_discover(service: &str) -> Vec<(String, String, u16)> {
    use std::net::UdpSocket;
    let mut out = Vec::new();
    let Ok(sock) = UdpSocket::bind("0.0.0.0:0") else { return out };
    sock.set_read_timeout(Some(Duration::from_millis(1200))).ok();
    // 组播 TTL=255（DNS-SD 规范）+ 回环（本机手机也在发）
    let s2 = socket2::SockRef::from(&sock);
    let _ = s2.set_ttl(255);
    #[cfg(unix)]
    unsafe {
        use std::os::unix::io::AsRawFd;
        let on: libc::c_int = 1;
        libc::setsockopt(
            sock.as_raw_fd(),
            libc::IPPROTO_IP,
            libc::IP_MULTICAST_LOOP,
            &on as *const libc::c_int as *const libc::c_void,
            4,
        );
    }
    let qname = dns_encode_name(service);
    let mut query = Vec::with_capacity(12 + qname.len() + 4);
    query.extend_from_slice(&0x1234u16.to_be_bytes());
    query.extend_from_slice(&0u16.to_be_bytes());
    query.extend_from_slice(&1u16.to_be_bytes());
    query.extend_from_slice(&0u16.to_be_bytes());
    query.extend_from_slice(&0u16.to_be_bytes());
    query.extend_from_slice(&0u16.to_be_bytes());
    query.extend_from_slice(&qname);
    query.extend_from_slice(&0x000Cu16.to_be_bytes());
    query.extend_from_slice(&1u16.to_be_bytes());
    let _ = sock.send_to(&query, ("224.0.0.251", 5353));

    let deadline = Instant::now() + Duration::from_millis(1500);
    let mut buf = [0u8; 9000];
    let mut srv_seen: Vec<(String, u16)> = Vec::new();
    while Instant::now() < deadline {
        let (n, _) = match sock.recv_from(&mut buf) {
            Ok(v) => v,
            Err(_) => break,
        };
        let pkt = &buf[..n];
        if n < 12 {
            continue;
        }
        let qd = u16::from_be_bytes([pkt[4], pkt[5]]) as usize;
        let mut off = 12;
        for _ in 0..qd {
            off = dns_skip_name(pkt, off);
            off += 4;
        }
        let total_rr = u16::from_be_bytes([pkt[6], pkt[7]]) as usize
            + u16::from_be_bytes([pkt[8], pkt[9]]) as usize
            + u16::from_be_bytes([pkt[10], pkt[11]]) as usize;
        for _ in 0..total_rr {
            if off + 10 > n {
                break;
            }
            let (name_end, _) = dns_read_name(pkt, off);
            off = name_end;
            let rtype = u16::from_be_bytes([pkt[off], pkt[off + 1]]);
            let rdlen = u16::from_be_bytes([pkt[off + 8], pkt[off + 9]]) as usize;
            let rdata_off = off + 10;
            if rtype == 33 && rdata_off + 6 <= n {
                let port = u16::from_be_bytes([pkt[rdata_off + 4], pkt[rdata_off + 5]]);
                let (_, target) = dns_read_name(pkt, rdata_off + 6);
                srv_seen.push((target, port));
            }
            off = rdata_off + rdlen;
        }
    }
    for (target, port) in srv_seen {
        let ip = mdns_resolve_a(&target)
            .or_else(|| {
                target
                    .trim_end_matches(".local")
                    .parse::<std::net::Ipv4Addr>()
                    .ok()
                    .map(|i| i.to_string())
            });
        if let Some(ip) = ip {
            let inst = target.trim_end_matches(service).trim_end_matches('.').to_string();
            out.push((inst, format!("{ip}"), port));
        }
    }
    out
}

fn mdns_resolve_a(host: &str) -> Option<String> {
    use std::net::UdpSocket;
    let sock = UdpSocket::bind("0.0.0.0:0").ok()?;
    sock.set_read_timeout(Some(Duration::from_millis(800))).ok();
    let qname = dns_encode_name(host);
    let mut q = Vec::with_capacity(12 + qname.len() + 4);
    q.extend_from_slice(&0x4321u16.to_be_bytes());
    q.extend_from_slice(&0u16.to_be_bytes());
    q.extend_from_slice(&1u16.to_be_bytes());
    q.extend_from_slice(&0u16.to_be_bytes());
    q.extend_from_slice(&0u16.to_be_bytes());
    q.extend_from_slice(&0u16.to_be_bytes());
    q.extend_from_slice(&qname);
    q.extend_from_slice(&1u16.to_be_bytes());
    q.extend_from_slice(&1u16.to_be_bytes());
    sock.send_to(&q, ("224.0.0.251", 5353)).ok()?;
    let deadline = Instant::now() + Duration::from_millis(900);
    let mut buf = [0u8; 4096];
    while Instant::now() < deadline {
        let (n, _) = sock.recv_from(&mut buf).ok()?;
        let pkt = &buf[..n];
        if n < 12 {
            continue;
        }
        let qd = u16::from_be_bytes([pkt[4], pkt[5]]) as usize;
        let mut off = 12;
        for _ in 0..qd {
            off = dns_skip_name(pkt, off);
            off += 4;
        }
        let an = u16::from_be_bytes([pkt[6], pkt[7]]) as usize;
        for _ in 0..an {
            if off + 10 > n {
                break;
            }
            let (ne, _) = dns_read_name(pkt, off);
            off = ne;
            let rtype = u16::from_be_bytes([pkt[off], pkt[off + 1]]);
            let rdlen = u16::from_be_bytes([pkt[off + 8], pkt[off + 9]]) as usize;
            let rdata_off = off + 10;
            if rtype == 1 && rdlen == 4 && rdata_off + 4 <= n {
                return Some(format!(
                    "{}.{}.{}.{}",
                    pkt[rdata_off], pkt[rdata_off + 1], pkt[rdata_off + 2], pkt[rdata_off + 3]
                ));
            }
            off = rdata_off + rdlen;
        }
    }
    None
}

fn dns_encode_name(name: &str) -> Vec<u8> {
    let mut out = Vec::new();
    for part in name.split('.') {
        out.push(part.len() as u8);
        out.extend_from_slice(part.as_bytes());
    }
    out.push(0);
    out
}

fn dns_skip_name(pkt: &[u8], mut off: usize) -> usize {
    loop {
        if off >= pkt.len() {
            return off;
        }
        let len = pkt[off] as usize;
        if len == 0 {
            return off + 1;
        }
        if len & 0xC0 == 0xC0 {
            return off + 2;
        }
        off += 1 + len;
    }
}

fn dns_read_name(pkt: &[u8], mut off: usize) -> (usize, String) {
    let mut name = String::new();
    let mut jumped = false;
    let mut end_off = off;
    let mut hops = 0;
    loop {
        if off >= pkt.len() || hops > 20 {
            break;
        }
        let len = pkt[off] as usize;
        if len == 0 {
            if !jumped {
                end_off = off + 1;
            }
            break;
        }
        if len & 0xC0 == 0xC0 {
            if off + 1 >= pkt.len() {
                break;
            }
            let ptr = ((len & 0x3F) << 8) | pkt[off + 1] as usize;
            if !jumped {
                end_off = off + 2;
            }
            jumped = true;
            off = ptr;
            hops += 1;
            continue;
        }
        off += 1;
        if off + len > pkt.len() {
            break;
        }
        if !name.is_empty() {
            name.push('.');
        }
        name.push_str(&String::from_utf8_lossy(&pkt[off..off + len]));
        off += len;
        if !jumped {
            end_off = off;
        }
    }
    (end_off, name)
}

/// 组播发现：用广播 255.255.255.255 + 常见网段定向，补偿多接口默认路由下
/// 224.0.0.251 出不去的问题。同样收 SRV。
pub fn mdns_services_broadcast() -> Vec<MdnsService> {
    let mut list = Vec::new();
    for (svc_fqdn, kind) in [
        ("_adb._tcp.local", "_adb._tcp"),
        ("_adb-tls-pairing._tcp.local", "_adb-tls-pairing._tcp"),
        ("_adb-tls-connect._tcp.local", "_adb-tls-connect._tcp"),
    ] {
        for (name, host, port) in mdns_broadcast_query(svc_fqdn) {
            let ip = mdns_resolve_a(&host)
                .or_else(|| {
                    host.trim_end_matches(".local")
                        .parse::<std::net::Ipv4Addr>()
                        .ok()
                        .map(|i| i.to_string())
                })
                .unwrap_or(host.clone());
            list.push(MdnsService {
                name,
                svc_type: kind.to_string(),
                addr: format!("{ip}:{port}"),
            });
        }
    }
    list
}

fn mdns_broadcast_query(service: &str) -> Vec<(String, String, u16)> {
    use std::net::UdpSocket;
    let mut out = Vec::new();
    let Ok(sock) = UdpSocket::bind("0.0.0.0:0") else { return out };
    sock.set_read_timeout(Some(Duration::from_millis(1200))).ok();
    sock.set_broadcast(true).ok();
    let qname = dns_encode_name(service);
    let mut query = Vec::with_capacity(12 + qname.len() + 4);
    query.extend_from_slice(&0x5678u16.to_be_bytes());
    query.extend_from_slice(&0u16.to_be_bytes());
    query.extend_from_slice(&1u16.to_be_bytes());
    query.extend_from_slice(&0u16.to_be_bytes());
    query.extend_from_slice(&0u16.to_be_bytes());
    query.extend_from_slice(&0u16.to_be_bytes());
    query.extend_from_slice(&qname);
    query.extend_from_slice(&0x000Cu16.to_be_bytes());
    query.extend_from_slice(&1u16.to_be_bytes());
    // 广播到 5353 与本网段广播
    let _ = sock.send_to(&query, ("255.255.255.255", 5353));
    if let Ok(ip) = local_ip() {
        if let Some(bcast) = broadcast_of(&ip) {
            let _ = sock.send_to(&query, (bcast.as_str(), 5353));
        }
    }
    let deadline = Instant::now() + Duration::from_millis(1500);
    let mut buf = [0u8; 9000];
    let mut seen: Vec<(String, u16)> = Vec::new();
    while Instant::now() < deadline {
        let (n, _) = match sock.recv_from(&mut buf) {
            Ok(v) => v,
            Err(_) => break,
        };
        let pkt = &buf[..n];
        if n < 12 {
            continue;
        }
        let qd = u16::from_be_bytes([pkt[4], pkt[5]]) as usize;
        let mut off = 12;
        for _ in 0..qd {
            off = dns_skip_name(pkt, off);
            off += 4;
        }
        let total_rr = u16::from_be_bytes([pkt[6], pkt[7]]) as usize
            + u16::from_be_bytes([pkt[8], pkt[9]]) as usize
            + u16::from_be_bytes([pkt[10], pkt[11]]) as usize;
        for _ in 0..total_rr {
            if off + 10 > n {
                break;
            }
            let (ne, _) = dns_read_name(pkt, off);
            off = ne;
            let rtype = u16::from_be_bytes([pkt[off], pkt[off + 1]]);
            let rdlen = u16::from_be_bytes([pkt[off + 8], pkt[off + 9]]) as usize;
            let rdata_off = off + 10;
            if rtype == 33 && rdata_off + 6 <= n {
                let port = u16::from_be_bytes([pkt[rdata_off + 4], pkt[rdata_off + 5]]);
                let (_, target) = dns_read_name(pkt, rdata_off + 6);
                seen.push((target, port));
            }
            off = rdata_off + rdlen;
        }
    }
    for (target, port) in seen {
        let ip = mdns_resolve_a(&target)
            .or_else(|| {
                target
                    .trim_end_matches(".local")
                    .parse::<std::net::Ipv4Addr>()
                    .ok()
                    .map(|i| i.to_string())
            });
        if let Some(ip) = ip {
            let inst = target.trim_end_matches(service).trim_end_matches('.').to_string();
            out.push((inst, format!("{ip}"), port));
        }
    }
    out
}

fn local_ip() -> std::io::Result<String> {
    // UDP 连接一个公网地址让内核选出本机出口 IP（不实际发包）
    let s = std::net::UdpSocket::bind("0.0.0.0:0")?;
    s.connect(("8.8.8.8", 80))?;
    Ok(s.local_addr()?.ip().to_string())
}

fn broadcast_of(ip: &str) -> Option<String> {
    ip.rsplitn(2, '.').nth(1).map(|p| format!("{p}.255"))
}

// ===================== 外部 adb server 劝退 / USB 自愈 =====================


pub fn ensure_server() -> AdbResult<()> {
    // server 桥模式（默认）：保证官方 adb server 存活（USB 的 libusb 栈
    // 全权交给官方 server；我们经 5037 smart-socket 走 host:tport 多路复用）
    if TcpStream::connect(("127.0.0.1", 5037)).is_err() {
        if let Some(adb) = find_adb_binary() {
            let _ = std::process::Command::new(adb)
                .arg("start-server")
                .env("HOME", std::env::var("HOME").unwrap_or_else(|_| "/data".into()))
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status();
            std::thread::sleep(Duration::from_millis(800));
        }
    }
    Ok(())
}

pub fn restart_server() -> AdbResult<()> {
    // 官方协议重启：host:kill 让 server 自行退出，再拉起（热插拔/自愈路径）
    if let Ok(mut c) = TcpStream::connect(("127.0.0.1", 5037)) {
        let payload = b"host:kill";
        let _ = c.write_all(format!("{:04x}", payload.len()).as_bytes());
        let _ = c.write_all(payload);
        std::thread::sleep(Duration::from_millis(500));
    }
    ensure_server()
}

/// 官方 adb 二进制定位（server 桥模式用；容器内置 /opt/mwine/adb）
pub fn find_adb_binary() -> Option<String> {
    if let Ok(v) = std::env::var("MWINE_ADB_BIN") {
        if std::path::Path::new(&v).exists() {
            return Some(v);
        }
    }
    let home = std::env::var("HOME").unwrap_or_default();
    #[cfg(target_os = "macos")]
    let cands = [
        format!("{home}/Library/Android/sdk/platform-tools/adb"),
        "/opt/homebrew/bin/adb".to_string(),
        "/usr/local/bin/adb".to_string(),
    ];
    #[cfg(not(target_os = "macos"))]
    let cands = [
        "/opt/mwine/adb".to_string(),
        "/usr/local/bin/adb".to_string(),
        "/usr/bin/adb".to_string(),
    ];
    cands.into_iter().find(|p| std::path::Path::new(p).exists())
}

/// /sys 总线上是否存在 ADB 接口（ff/42/01）——桥模式下的自愈探测信号
pub fn usb_adb_iface_present() -> bool {
    let rd = match std::fs::read_dir("/sys/bus/usb/devices") {
        Ok(r) => r,
        Err(_) => return false,
    };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if !name.contains(':') {
            continue;
        }
        let base = format!("{}/", e.path().display());
        let sc = std::fs::read_to_string(format!("{base}bInterfaceSubClass")).unwrap_or_default();
        let pp = std::fs::read_to_string(format!("{base}bInterfaceProtocol")).unwrap_or_default();
        if sc.trim() == "42" && pp.trim() == "01" {
            return true;
        }
    }
    false
}
