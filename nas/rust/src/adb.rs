//! 极简 adb 客户端：smart-socket（adb server 5037）协议。
//!
//! 关键坑（2026-10-20 用 Wireshark 级排查换来的）：
//! - 新版 adb server（34+）必须用 `host:tport:<serial>` 切换传输：
//!   旧式 `host:transport:` 之后一切 shell v2 服务必现 FAIL closed。
//! - `host:tport:` OKAY 之后 server 立即推送 8 字节 transport_id（LE u64），
//!   不消费它，下一个服务的状态字节会错位读出。
//! - 老 server 无 tport 服务（FAIL）→ 同一连接回落 legacy host:transport。
//! - 设备侧不认 shell v2（老 adbd / 老 server 转发缺陷）→ 回落 v1 `shell:`。

use std::io::{Read, Write};
use std::net::{Shutdown, TcpStream};
use std::time::{Duration, Instant};

const ADB_SERVER: &str = "127.0.0.1:5037";

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

/// 一条与 adb server 的连接（smart-socket 帧：4 位小写十六进制长度 + 载荷）。
pub struct SmartConn {
    pub stream: TcpStream,
}

impl SmartConn {
    pub fn connect(timeout_ms: u64) -> AdbResult<Self> {
        let stream = TcpStream::connect(ADB_SERVER)
            .map_err(|e| AdbError(format!("adb server 不可达 ({ADB_SERVER}): {e}")))?;
        stream
            .set_nodelay(true)
            .map_err(|e| AdbError(format!("nodelay: {e}")))?;
        let c = Self { stream };
        c.stream
            .set_read_timeout(Some(Duration::from_millis(timeout_ms.max(1000))))
            .ok();
        Ok(c)
    }

    fn write_all(&mut self, data: &[u8]) -> AdbResult<()> {
        self.stream
            .write_all(data)
            .map_err(|e| AdbError(format!("写入 adb server 失败: {e}")))
    }

    /// 发服务串并读 OKAY/FAIL。
    pub fn service(&mut self, svc: &str) -> AdbResult<()> {
        let payload = svc.as_bytes();
        if payload.len() > 0xFFFF {
            return err("service string too long");
        }
        let framed = format!("{:04x}", payload.len());
        self.write_all(framed.as_bytes())?;
        self.write_all(payload)?;
        self.read_status()
    }

    fn read_exact_timeout(&mut self, n: usize, timeout_ms: u64) -> AdbResult<Vec<u8>> {
        let mut buf = vec![0u8; n];
        let deadline = now_deadline(timeout_ms);
        let mut filled = 0;
        while filled < n {
            match self.stream.read(&mut buf[filled..]) {
                Ok(0) => return err("eof in the middle of packet"),
                Ok(k) => filled += k,
                Err(e)
                    if e.kind() == std::io::ErrorKind::WouldBlock
                        || e.kind() == std::io::ErrorKind::TimedOut =>
                {
                    if Instant::now() >= deadline {
                        return err("read timeout");
                    }
                }
                Err(e) => return err(format!("read: {e}")),
            }
        }
        Ok(buf)
    }

    fn read_status(&mut self) -> AdbResult<()> {
        let st = self.read_exact_timeout(4, 30000)?;
        match &st[..] {
            b"OKAY" => Ok(()),
            b"FAIL" => {
                let lenb = self.read_exact_timeout(4, 10000)?;
                let len = usize::from_str_radix(std::str::from_utf8(&lenb).unwrap_or("0"), 16)
                    .unwrap_or(0);
                let msg = self.read_exact_timeout(len.min(4096), 10000)?;
                err(format!(
                    "adb service request failed: {}",
                    String::from_utf8_lossy(&msg).trim()
                ))
            }
            other => err(format!(
                "protocol error: unexpected status bytes: {}",
                String::from_utf8_lossy(other)
            )),
        }
    }

    /// 切到设备传输：优先 host:tport（server 34+），失败回落 legacy。
    ///
    /// 注意：tport 失败后**必须换新连接**再回落 host:transport —— tport 的 FAIL
    /// 应答会把当前连接置于 closed 状态，任何后续 service 都收不到应答（实测
    /// server 34/37 上直接 EOF），这是 adb server 的既定行为而非异常。
    pub fn transport(&mut self, serial: &str) -> AdbResult<()> {
        match self.service(&format!("host:tport:{serial}")) {
            Ok(()) => {
                let id = self.read_exact_timeout(8, 10000)?;
                if id.len() != 8 {
                    return err("tport 后读取 transport_id 失败");
                }
                Ok(())
            }
            Err(_) => {
                // 换新连接回落 legacy（旧 server 无 tport 服务）
                let mut c = SmartConn::connect(5000)?;
                c.service(&format!("host:transport:{serial}"))?;
                // 接管底层连接供调用方继续用
                self.stream = c.stream;
                Ok(())
            }
        }
    }

    /// 裸服务（不经 transport，如 host:version / host:devices / host:track-transports）。
    pub fn host_service_payload(&mut self, svc: &str) -> AdbResult<Vec<u8>> {
        self.service(svc)?;
        let lenb = self.read_exact_timeout(4, 10000)?;
        let len =
            usize::from_str_radix(std::str::from_utf8(&lenb).unwrap_or("0"), 16).unwrap_or(0);
        self.read_exact_timeout(len, 30000)
    }
}

/// 设备列表项。
#[derive(Debug, Clone, serde::Serialize)]
pub struct Device {
    pub serial: String,
    pub state: String,
}

pub fn devices() -> AdbResult<Vec<Device>> {
    let mut c = SmartConn::connect(5000)?;
    let payload = c.host_service_payload("host:devices")?;
    let text = String::from_utf8_lossy(&payload);
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let mut parts = line.split_whitespace();
        let serial = parts.next().unwrap_or("").to_string();
        let state = parts.next().unwrap_or("").to_string();
        if !serial.is_empty() {
            out.push(Device { serial, state });
        }
    }
    Ok(out)
}

/// 找第一个在线模拟器 serial。
pub fn online_emulator() -> Option<String> {
    devices().ok()?.into_iter().find_map(|d| {
        if d.state == "device" && d.serial.starts_with("emulator-") {
            Some(d.serial)
        } else {
            None
        }
    })
}

/// 执行 shell 命令收全部输出（v2 优先，失败回落 v1）。
pub fn shell_blocking(serial: &str, cmd: &str, timeout_ms: u64) -> AdbResult<Vec<u8>> {
    // v2 raw：分帧输出（id u8 + len u32 LE + payload；id=1 stdout / 2 stderr / 3 exit）
    if let Ok(out) = shell_v2(serial, cmd, timeout_ms, false) {
        return Ok(out);
    }
    shell_v1(serial, cmd, timeout_ms)
}

fn shell_v1(serial: &str, cmd: &str, timeout_ms: u64) -> AdbResult<Vec<u8>> {
    let mut c = SmartConn::connect(5000)?;
    c.transport(serial)?;
    c.service(&format!("shell:{cmd}"))?;
    let mut out = Vec::new();
    let deadline = now_deadline(timeout_ms);
    let mut buf = [0u8; 65536];
    loop {
        match c.stream.read(&mut buf) {
            Ok(0) => break,
            Ok(k) => out.extend_from_slice(&buf[..k]),
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                if Instant::now() >= deadline {
                    return err("shell 读取超时");
                }
            }
            Err(e) => return err(format!("shell 读取失败: {e}")),
        }
    }
    Ok(out)
}

fn shell_v2(serial: &str, cmd: &str, timeout_ms: u64, pty: bool) -> AdbResult<Vec<u8>> {
    let mut c = SmartConn::connect(5000)?;
    c.transport(serial)?;
    let svc = if pty {
        format!("shell,v2,pty:{cmd}")
    } else {
        format!("shell,v2,raw:{cmd}")
    };
    c.service(&svc)?;
    let mut out = Vec::new();
    let deadline = now_deadline(timeout_ms);
    loop {
        // 逐字节读帧头与帧体，避免半帧/半载荷超时把已到达数据丢掉
        // （此前 read_exact_timeout 超时放弃已读字节 → 下一帧错位解析 → EOF）
        let read_bytes = |c: &mut SmartConn, n: usize| -> AdbResult<Vec<u8>> {
            let mut out = Vec::with_capacity(n);
            while out.len() < n {
                let mut b = [0u8; 1];
                match c.stream.read(&mut b) {
                    Ok(0) => return err("eof in the middle of packet"),
                    Ok(_) => out.push(b[0]),
                    Err(e)
                        if e.kind() == std::io::ErrorKind::WouldBlock
                            || e.kind() == std::io::ErrorKind::TimedOut =>
                    {
                        if Instant::now() >= deadline {
                            return err("shell v2 读取超时");
                        }
                    }
                    Err(e) => return err(format!("shell v2 读取失败: {e}")),
                }
            }
            Ok(out)
        };
        let hdr = read_bytes(&mut c, 5)?;
        let id = hdr[0];
        let len = u32::from_le_bytes([hdr[1], hdr[2], hdr[3], hdr[4]]) as usize;
        let payload = read_bytes(&mut c, len)?;
        match id {
            1 | 2 => out.extend_from_slice(&payload), // stdout/stderr 合并
            3 => break,                               // exit
            _ => {}
        }
    }
    Ok(out)
}

/// 推送字节到设备路径（sync: 服务 + SEND/DATA/DONE）。
pub fn push_bytes(serial: &str, data: &[u8], remote: &str) -> AdbResult<()> {
    let mut c = SmartConn::connect(5000)?;
    c.transport(serial)?;
    c.service("sync:")?;
    // SEND <len> <path>,<mode>（模式 0644）
    let path_mode = format!("{remote},420");
    let mut hdr = Vec::with_capacity(8 + path_mode.len());
    hdr.extend_from_slice(b"SEND");
    hdr.extend_from_slice(&(path_mode.len() as u32).to_le_bytes());
    c.write_all(&hdr)?;
    c.write_all(path_mode.as_bytes())?;
    // DATA 分块（≤64KB）
    for chunk in data.chunks(65536) {
        let mut dh = Vec::with_capacity(8);
        dh.extend_from_slice(b"DATA");
        dh.extend_from_slice(&(chunk.len() as u32).to_le_bytes());
        c.write_all(&dh)?;
        c.write_all(chunk)?;
    }
    // DONE + mtime
    let mtime = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as u32)
        .unwrap_or(0);
    let mut done = Vec::with_capacity(8);
    done.extend_from_slice(b"DONE");
    done.extend_from_slice(&mtime.to_le_bytes());
    c.write_all(&done)?;
    // OKAY / FAIL <len> <msg>
    let st = c.read_exact_timeout(4, 30000)?;
    match &st[..] {
        b"OKAY" => Ok(()),
        b"FAIL" => {
            let lenb = c.read_exact_timeout(4, 10000)?;
            let len = u32::from_le_bytes([lenb[0], lenb[1], lenb[2], lenb[3]]) as usize;
            let msg = c.read_exact_timeout(len.min(4096), 10000)?;
            err(format!(
                "push FAIL: {}",
                String::from_utf8_lossy(&msg).trim()
            ))
        }
        other => err(format!(
            "sync 应答异常: {}",
            String::from_utf8_lossy(other)
        )),
    }
}

/// 注册端口转发：127.0.0.1:tcp_port → 设备 abstract socket。
pub fn forward_abstract(serial: &str, tcp_port: u16, abstract_name: &str) -> AdbResult<()> {
    let mut c = SmartConn::connect(5000)?;
    c.service(&format!(
        "host-serial:{serial}:forward:tcp:{tcp_port};localabstract:{abstract_name}"
    ))
}

/// 撤销端口转发。
pub fn remove_forward(serial: &str, tcp_port: u16) -> AdbResult<()> {
    let mut c = SmartConn::connect(5000)?;
    c.service(&format!(
        "host-serial:{serial}:killforward:tcp:{tcp_port}"
    ))
}

/// 启动长驻 shell 进程（v2 raw 优先、v1 回落）。返回持有连接：
/// 进程随连接断开而被杀（SIGHUP），调用方持有即保活。
pub struct ShellProcess {
    pub stream: TcpStream,
}

impl ShellProcess {
    pub fn shutdown(&self) {
        let _ = self.stream.shutdown(Shutdown::Both);
    }
}

pub fn shell_stream(serial: &str, cmd: &str) -> AdbResult<ShellProcess> {
    // v2 raw
    {
        let mut c = SmartConn::connect(5000)?;
        c.transport(serial)?;
        match c.service(&format!("shell,v2,raw:{cmd}")) {
            Ok(()) => return Ok(ShellProcess { stream: c.stream }),
            Err(_) => { /* 落 v1 */ }
        }
    }
    let mut c = SmartConn::connect(5000)?;
    c.transport(serial)?;
    c.service(&format!("shell:{cmd}"))?;
    Ok(ShellProcess { stream: c.stream })
}

/// 交互式终端会话：v2 pty 优先（提示符/回显完整），不支持则 v1 交互 shell。
pub fn shell_pty(serial: &str) -> AdbResult<(ShellProcess, bool)> {
    {
        let mut c = SmartConn::connect(5000)?;
        c.transport(serial)?;
        match c.service("shell,v2,pty:") {
            Ok(()) => return Ok((ShellProcess { stream: c.stream }, true)),
            Err(_) => {}
        }
    }
    let mut c = SmartConn::connect(5000)?;
    c.transport(serial)?;
    c.service("shell:")?;
    Ok((ShellProcess { stream: c.stream }, false))
}

/// 确保 adb server 在跑（未跑则用 PATH/常见路径的 adb 拉起）。
pub fn ensure_server() -> AdbResult<()> {
    if SmartConn::connect(1500).is_ok() {
        return Ok(());
    }
    let adb_bin = find_adb_binary().unwrap_or_else(|| "adb".to_string());
    std::process::Command::new(adb_bin)
        .arg("start-server")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map_err(|e| AdbError(format!("adb server 拉起失败: {e}")))?;
    SmartConn::connect(5000)?;
    Ok(())
}

pub fn find_adb_binary() -> Option<String> {
    let home = std::env::var("HOME").unwrap_or_default();
    let mut cands: Vec<String> = Vec::new();
    if let Ok(v) = std::env::var("MWINE_SDK_ROOT") {
        cands.push(format!("{v}/platform-tools/adb"));
    }
    for name in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
        if let Ok(v) = std::env::var(name) {
            cands.push(format!("{v}/platform-tools/adb"));
        }
    }
    #[cfg(target_os = "macos")]
    {
        cands.push(format!("{home}/Library/Android/sdk/platform-tools/adb"));
        cands.push(format!(
            "{home}/Library/Application Support/MWine/runtime/platform-tools/adb"
        ));
    }
    #[cfg(not(target_os = "macos"))]
    {
        cands.push(format!("{home}/Android/Sdk/platform-tools/adb"));
        cands.push("/opt/android-sdk/platform-tools/adb".into());
    }
    cands.push("/data/android-sdk/platform-tools/adb".into()); // NAS Docker 卷路径
    cands.into_iter().find(|p| std::path::Path::new(p).exists())
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

/// 从设备拉取文件（sync: 服务 + STAT 尺寸 + RECV 分块）。
pub fn pull_file(serial: &str, remote: &str, max_bytes: usize) -> AdbResult<Vec<u8>> {
    let mut c = SmartConn::connect(5000)?;
    c.transport(serial)?;
    c.service("sync:")?;
    // STAT 取尺寸
    let mut hdr = Vec::with_capacity(8 + remote.len());
    hdr.extend_from_slice(b"STAT");
    hdr.extend_from_slice(&(remote.len() as u32).to_le_bytes());
    c.write_all(&hdr)?;
    c.write_all(remote.as_bytes())?;
    let st = c.read_exact_timeout(4, 10000)?;
    if &st[..] != b"STAT" {
        return err(format!("STAT 应答异常: {}", String::from_utf8_lossy(&st)));
    }
    let stbuf = c.read_exact_timeout(12, 10000)?;
    let size = u32::from_le_bytes([stbuf[4], stbuf[5], stbuf[6], stbuf[7]]) as usize;
    if size == 0 || size > max_bytes {
        return err(format!("文件尺寸异常: {size}"));
    }
    // RECV 循环
    let mut rq = Vec::with_capacity(8 + remote.len());
    rq.extend_from_slice(b"RECV");
    rq.extend_from_slice(&(remote.len() as u32).to_le_bytes());
    c.write_all(&rq)?;
    c.write_all(remote.as_bytes())?;
    let mut out = Vec::with_capacity(size);
    loop {
        let id = c.read_exact_timeout(4, 30000)?;
        let lenb = c.read_exact_timeout(4, 10000)?;
        let len = u32::from_le_bytes([lenb[0], lenb[1], lenb[2], lenb[3]]) as usize;
        match &id[..] {
            b"DATA" => {
                let chunk = c.read_exact_timeout(len, 60000)?;
                out.extend_from_slice(&chunk);
            }
            b"DONE" => break,
            b"FAIL" => {
                let msg = c.read_exact_timeout(len.min(4096), 10000)?;
                return err(format!("pull FAIL: {}", String::from_utf8_lossy(&msg).trim()));
            }
            other => return err(format!("sync RECV 异常: {}", String::from_utf8_lossy(other))),
        }
    }
    Ok(out)
}

// ===================== shell v2 双工（WS 桥用） =====================

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

/// pty 会话（v2 优先）：返回 (流, 是否 v2-pty)。v1 交互回落为 raw 流。
pub fn shell_pty_full(serial: &str) -> AdbResult<(ShellProcess, bool)> {
    shell_pty(serial)
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

// ===================== adb 二进制子进程（配对/连接/mDNS 发现） =====================

/// 以 shell v1（`shell:<cmd>`）打开命令进程（流式，不等待输出）。
pub fn shell_v1_stream(serial: &str, cmd: &str) -> AdbResult<ShellProcess> {
    let mut c = SmartConn::connect(30000)?;
    c.transport(serial)?;
    c.service(&format!("shell:{cmd}"))?;
    Ok(ShellProcess { stream: c.stream })
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

// ===== 自研 adb server 会话命令（pair/connect/disconnect —— 不再依赖外部 adb 二进制）=====

/// 读 OKAY 之后的长度前缀文本载荷（4 位十六进制长度 + 内容）。
fn read_len_prefixed(c: &mut SmartConn, timeout_ms: u64) -> AdbResult<String> {
    let lenb = c.read_exact_timeout(4, timeout_ms)?;
    let len = usize::from_str_radix(std::str::from_utf8(&lenb).unwrap_or("0"), 16).unwrap_or(0);
    let msg = c.read_exact_timeout(len.min(4096), timeout_ms)?;
    Ok(String::from_utf8_lossy(&msg).trim().to_string())
}

/// host:pair —— 无线配对：纯 Rust SPAKE2+TLS 实现（见 pairing.rs，零外部 adb 依赖）。
/// 配对成功后手机信任本机 adbkey，后续 host:connect 即可建立加密连接。
pub fn adb_pair(addr: &str, code: &str) -> AdbResult<String> {
    // adbkey 由 adb server 首次启动生成；配对要把它发给手机
    ensure_server()?;
    crate::pairing::pair(addr, code).map_err(AdbError)
}

/// host:connect:<addr>
///
/// 注意：server 对 host:connect 永远先回 OKAY，真实结果在 OKAY 后的
/// 长度前缀载荷里（"connected to ..." / "failed to connect ..."）——
/// 必须读载荷并判定，否则失败也被报成 ok。
pub fn adb_connect(addr: &str) -> AdbResult<String> {
    let mut c = SmartConn::connect(5000)?;
    c.service(&format!("host:connect:{addr}"))
        .map_err(|e| AdbError(format!("连接失败: {}", e.to_string().replace("adb service request failed: ", ""))))?;
    match read_len_prefixed(&mut c, 15000) {
        Ok(msg) if msg.starts_with("failed") || msg.starts_with("cannot") => {
            err(format!("连接失败: {msg}"))
        }
        Ok(msg) => Ok(msg),
        Err(_) => Ok("connected".into()), // 老 server 不回载荷：保持原有宽松语义
    }
}

/// host:disconnect:<serial>
pub fn adb_disconnect(serial: &str) -> AdbResult<String> {
    let mut c = SmartConn::connect(5000)?;
    c.service(&format!("host:disconnect:{serial}"))
        .map_err(|e| AdbError(format!("断开失败: {}", e.to_string().replace("adb service request failed: ", ""))))?;
    match read_len_prefixed(&mut c, 10000) {
        Ok(msg) if msg.starts_with("failed") || msg.starts_with("cannot") => {
            err(format!("断开失败: {msg}"))
        }
        Ok(msg) => Ok(msg),
        Err(_) => Ok("disconnected".into()),
    }
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
    use std::time::{Duration, Instant};
    let mut out = Vec::new();
    let sock = match UdpSocket::bind("0.0.0.0:0") {
        Ok(s) => s,
        Err(_) => return out,
    };
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
        if n < 12 { continue; }
        let pkt = &buf[..n];
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
            if off + 10 > n { break; }
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
            .or_else(|| target.trim_end_matches(".local").parse::<std::net::Ipv4Addr>().ok().map(|i| i.to_string()));
        if let Some(ip) = ip {
            let inst = target
                .trim_end_matches(service)
                .trim_end_matches('.')
                .to_string();
            out.push((inst, format!("{ip}"), port));
        }
    }
    out
}

fn mdns_resolve_a(host: &str) -> Option<String> {
    use std::net::UdpSocket;
    use std::time::{Duration, Instant};
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
        if n < 12 { continue; }
        let qd = u16::from_be_bytes([buf[4], buf[5]]) as usize;
        let mut off = 12;
        for _ in 0..qd {
            off = dns_skip_name(&buf, off);
            off += 4;
        }
        let an = u16::from_be_bytes([buf[6], buf[7]]) as usize;
        for _ in 0..an {
            if off + 10 > n { break; }
            let (ne, _) = dns_read_name(&buf, off);
            off = ne;
            let rtype = u16::from_be_bytes([buf[off], buf[off + 1]]);
            let rdlen = u16::from_be_bytes([buf[off + 8], buf[off + 9]]) as usize;
            let rdata_off = off + 10;
            if rtype == 1 && rdlen == 4 && rdata_off + 4 <= n {
                return Some(format!("{}.{}.{}.{}",
                    buf[rdata_off], buf[rdata_off + 1], buf[rdata_off + 2], buf[rdata_off + 3]));
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
        if off >= pkt.len() { return off; }
        let len = pkt[off] as usize;
        if len == 0 { return off + 1; }
        if len & 0xC0 == 0xC0 { return off + 2; }
        off += 1 + len;
    }
}

fn dns_read_name(pkt: &[u8], mut off: usize) -> (usize, String) {
    let mut name = String::new();
    let mut jumped = false;
    let mut end_off = off;
    let mut hops = 0;
    loop {
        if off >= pkt.len() || hops > 20 { break; }
        let len = pkt[off] as usize;
        if len == 0 {
            if !jumped { end_off = off + 1; }
            break;
        }
        if len & 0xC0 == 0xC0 {
            if off + 1 >= pkt.len() { break; }
            let ptr = ((len & 0x3F) << 8) | pkt[off + 1] as usize;
            if !jumped { end_off = off + 2; }
            jumped = true;
            off = ptr;
            hops += 1;
            continue;
        }
        off += 1;
        if off + len > pkt.len() { break; }
        if !name.is_empty() { name.push('.'); }
        name.push_str(&String::from_utf8_lossy(&pkt[off..off + len]));
        off += len;
        if !jumped { end_off = off; }
    }
    (end_off, name)
}

/// 远程文件区间读取（dd 分块）：无线真机上图标提取只读尾部目录+图标切片，
/// 避免整包拉取（APK 动辄几十 MB，Wi-Fi 繁忙时整拉是灾难）。
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

/// 组播发现：用广播 255.255.255.255 + 常见网段定向，补偿多接口默认路由下
/// 224.0.0.251 出不出的问题。同样收 SRV。
pub fn mdns_services_broadcast() -> Vec<MdnsService> {
    let mut list = Vec::new();
    for (svc_fqdn, kind) in [
        ("_adb._tcp.local", "_adb._tcp"),
        ("_adb-tls-pairing._tcp.local", "_adb-tls-pairing._tcp"),
        ("_adb-tls-connect._tcp.local", "_adb-tls-connect._tcp"),
    ] {
        for (name, host, port) in mdns_broadcast_query(svc_fqdn) {
            let ip = mdns_resolve_a(&host)
                .or_else(|| host.trim_end_matches(".local").parse::<std::net::Ipv4Addr>().ok().map(|i| i.to_string()))
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
    use std::time::{Duration, Instant};
    let mut out = Vec::new();
    let sock = match UdpSocket::bind("0.0.0.0:0") {
        Ok(s) => s,
        Err(_) => return out,
    };
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
        if n < 12 { continue; }
        let pkt = &buf[..n];
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
            if off + 10 > n { break; }
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
            .or_else(|| target.trim_end_matches(".local").parse::<std::net::Ipv4Addr>().ok().map(|i| i.to_string()));
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
