//! 集成 ADB 传输层（自研 adb server 核心）：直接与 adbd 建立 CNXN 会话
//! （USB usbfs/IOKit 直驱、TCP 无线明文），零外部 adb server 依赖。
//! 协议参照 AOSP adb/transport.cpp：
//!   A_CNXN → A_AUTH(TOKEN→SIGN→TOKEN→PUBKEY) → A_CNXN → A_OPEN/A_OKAY/A_WRTE/A_CLSE
//! 一条传输多路复用 local_id 流（scrcpy 双流/终端/文件传输并发）。
//!
//! 结构要点（踩过的坑）：
//! - 读写半分离：reader 线程独占读半，写侧持写半。若共用一把锁，空闲期
//!   reader 持锁阻塞读会把所有写操作卡死（写要等设备先来数据）。
//! - A_OKAY 双语义：应答 OPEN（流就绪）与确认 WRTE（流控）。设备发来的
//!   每个 WRTE 都必须回 OKAY，否则设备侧发送窗耗尽后 shell 输出停流。
//! - CNXN 的设备最大负载在 arg1（不在 payload——那是 ASCII 身份串）。
//! - 协议字序：消息 arg0=发送方自身流 id、arg1=目标流 id（OKAY/WRTE/CLSE）。

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{mpsc, Arc, Condvar, LazyLock, Mutex};
use std::time::{Duration, Instant};

use crate::usb;

pub const A_CNXN: u32 = 0x4e584e43; // "CNXN" 小端
pub const A_AUTH: u32 = 0x48545541; // "AUTH"
pub const A_OPEN: u32 = 0x4e45504f; // "OPEN"
pub const A_OKAY: u32 = 0x59414b4f; // "OKAY"
pub const A_CLSE: u32 = 0x45534c43; // "CLSE"
pub const A_WRTE: u32 = 0x45545257; // "WRTE"
pub const A_VERSION: u32 = 0x01000001;
pub const MAX_PAYLOAD: u32 = 1024 * 1024; // AOSP MAX_PAYLOAD_SIZE（现代 adb）
/// 每流未确认 WRTE 上限（协议流控：发送方等 OKAY 再发）。
/// 实测 NAS↔魅族17Pro Wi-Fi：设备 OKAY 应答 0.8~5s/个（随流内消费节奏）。
/// 窗 2~32 时 700KB 推送 60s+；窗 512 时推送 ≈ 网络本身（adb push 同级）。
/// adbd 不按字节计窗，只按 WRTE 条数——大窗只多占少量内核缓冲。
const SEND_WINDOW: u32 = 512;

fn checksum(data: &[u8]) -> u32 {
    data.iter().map(|b| *b as u32).sum()
}

#[derive(Debug, Clone)]
pub struct Message {
    pub cmd: u32,
    pub arg0: u32,
    pub arg1: u32,
    pub data: Vec<u8>,
}

impl Message {
    fn encode(&self) -> Vec<u8> {
        let mut h = Vec::with_capacity(24 + self.data.len());
        h.extend_from_slice(&self.cmd.to_le_bytes());
        h.extend_from_slice(&self.arg0.to_le_bytes());
        h.extend_from_slice(&self.arg1.to_le_bytes());
        h.extend_from_slice(&(self.data.len() as u32).to_le_bytes());
        h.extend_from_slice(&checksum(&self.data).to_le_bytes());
        h.extend_from_slice(&(self.cmd ^ 0xffff_ffff).to_le_bytes());
        h.extend_from_slice(&self.data);
        h
    }
}

// ---------------- 链路半分离 ----------------

enum ReadHalf {
    Tcp(TcpStream),
    Usb(usb::UsbHandle),
    Tls { conn: Arc<std::sync::Mutex<rustls::ClientConnection>>, stream: TcpStream },
}

enum WriteHalf {
    Tcp(TcpStream),
    Usb(usb::UsbWrite),
    Tls { conn: Arc<std::sync::Mutex<rustls::ClientConnection>>, stream: TcpStream },
}

impl ReadHalf {
    /// 读一帧内的数据；TimedOut/WouldBlock 供上层判 deadline
    fn read_some(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self {
            ReadHalf::Tcp(s) => s.read(buf),
            ReadHalf::Usb(h) => h.read(buf),
            ReadHalf::Tls { conn, stream } => {
                let mut conn = conn.lock().unwrap();
                // rustls 需要先喂 socket 数据再读
                match conn.read_tls(stream)? {
                    0 => return Err(std::io::Error::new(std::io::ErrorKind::UnexpectedEof, "TLS 对端关闭")),
                    _ => {}
                }
                conn.process_new_packets().map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
                conn.reader().read(buf)
            }
        }
    }
    /// TCP 读超时（USB 自带 20s 超时或阻塞至断开）
    fn set_read_timeout(&mut self, d: Option<Duration>) {
        match self {
            ReadHalf::Tcp(s) => { s.set_read_timeout(d).ok(); }
            ReadHalf::Tls { stream, .. } => { stream.set_read_timeout(d).ok(); }
            _ => {}
        }
    }
}

impl WriteHalf {
    fn write_all_bytes(&mut self, data: &[u8]) -> std::io::Result<()> {
        match self {
            WriteHalf::Tcp(s) => s.write_all(data),
            WriteHalf::Usb(h) => h.write_all(data),
            WriteHalf::Tls { conn, stream } => {
                let mut conn = conn.lock().unwrap();
                conn.writer().write_all(data)?;
                conn.write_tls(stream)?;
                Ok(())
            }
        }
    }
}

// ---------------- 流状态（分发目标） ----------------

enum OpenState {
    Pending,
    Open,
    Rejected(String),
}

struct StreamState {
    /// 数据帧出口（CLSE/断连时 drop → 消费端 recv 返回 Err(Disconnected)）
    tx: mpsc::Sender<Vec<u8>>,
    /// OPEN 应答等待（Pending→Open/Rejected + Condvar）
    open: Arc<(Mutex<OpenState>, Condvar)>,
    /// 未确认 WRTE 计数（流控窗口）
    window: Arc<(Mutex<u32>, Condvar)>,
}

// ---------------- 连接 ----------------

struct ConnInner {
    write: Mutex<WriteHalf>,
    streams: Mutex<HashMap<u32, StreamState>>,
    alive: AtomicBool,
    dead_reason: Mutex<String>,
    max_payload: u32,
}

pub struct DeviceConn {
    pub serial: String,
    pub kind: &'static str, // usb | tcp | server
    inner: Arc<ConnInner>,
    next_local: AtomicU32,
    /// adb server 桥模式（macOS 开发机回退路径）：每路流一条 smart-socket
    /// 连接（host:tport:serial + service），CNXN/AUTH 由系统 adb server 完成。
    /// Linux 生产路径恒 false（纯 Rust usbfs 直驱）。
    pub server_backed: bool,
}

/// 流写端：直连（WRTE 分片）或 adb-server 桥（裸 socket 写）
enum StreamWriter {
    Link(Arc<DeviceConn>, u32),
    Socket(TcpStream),
}

/// 一路打开的流：读（阻塞 channel）+ 写 + 关闭。
/// 字段用 Option 承载：into_tcp_bridge 可 take 出部件而不与 Drop 冲突。
pub struct Stream {
    pub local: u32,
    conn: Option<Arc<DeviceConn>>,
    rx: Option<mpsc::Receiver<Vec<u8>>>,
    writer: Option<StreamWriter>,
}

impl Stream {
    /// 阻塞读下一块数据；None = 流被设备关闭（CLSE）或连接断开
    pub fn read(&mut self) -> Option<Vec<u8>> {
        self.rx.as_mut()?.recv().ok()
    }

    /// 限时读：Ok(Some)=数据、Ok(None)=关闭、Err=超时
    pub fn read_timeout(&mut self, d: Duration) -> Result<Option<Vec<u8>>, ()> {
        match self.rx.as_mut().unwrap().recv_timeout(d) {
            Ok(data) => Ok(Some(data)),
            Err(mpsc::RecvTimeoutError::Disconnected) => Ok(None),
            Err(mpsc::RecvTimeoutError::Timeout) => Err(()),
        }
    }

    /// 写流数据（直连：分片 ≤ max_payload + 发送窗；server 桥：裸写 socket）
    pub fn write(&self, data: &[u8]) -> Result<(), String> {
        match self.writer.as_ref().unwrap() {
            StreamWriter::Link(conn, local) => conn.write_stream(*local, data),
            StreamWriter::Socket(s) => {
                use std::io::Write as _;
                let mut s = s.try_clone().map_err(|e| format!("server 桥克隆失败: {e}"))?;
                s.write_all(data).map_err(|e| format!("server 桥写入失败: {e}"))
            }
        }
    }

    /// 由裸 socket 组装流（adb-server 桥路径）：读泵线程 sock→channel
    pub(crate) fn from_tcp(sock: TcpStream) -> Stream {
        let (tx, rx) = mpsc::channel();
        let writer = sock.try_clone();
        std::thread::spawn(move || {
            let mut sock = sock;
            let mut buf = [0u8; 16 * 1024];
            use std::io::Read as _;
            eprintln!("[from_tcp] pump 启动");
            loop {
                match sock.read(&mut buf) {
                    Ok(0) => { eprintln!("[from_tcp] EOF"); break; }
                    Ok(n) => {
                        eprintln!("[from_tcp] 读到 {n}B");
                        if tx.send(buf[..n].to_vec()).is_err() {
                            eprintln!("[from_tcp] rx 已关"); break;
                        }
                    }
                    Err(e) => { eprintln!("[from_tcp] err {e}"); break; }
                }
            }
        });
        Stream {
            local: 0,
            conn: None,
            rx: Some(rx),
            writer: writer.ok().map(StreamWriter::Socket),
        }
    }

    /// 环回 TCP 桥：把流暴露为 TcpStream，供既有 TcpStream 消费方
    /// （终端/scrcpy/转发）零改动复用。断链语义：任一侧 EOF → 设备流关闭。
    pub fn into_tcp_bridge(mut self) -> std::io::Result<TcpStream> {
        use std::net::TcpListener;
        let listener = TcpListener::bind(("127.0.0.1", 0))?;
        let addr = listener.local_addr()?;
        let client = TcpStream::connect(addr)?;
        let (peer, _) = listener.accept()?;
        drop(listener);
        client.set_nodelay(true).ok();
        peer.set_nodelay(true).ok();
        // 部件移交桥线程；local=0 抑制 Drop 再关流
        let local = self.local;
        self.local = 0;
        let conn = self.conn.take();
        let rx = self.rx.take().unwrap();
        let writer_sock = match self.writer.take() {
            Some(StreamWriter::Socket(s)) => Some(s),
            Some(StreamWriter::Link(c, l)) => {
                // 直连流的写端保留在 conn 上（桥线程经 write_stream）
                let _ = (c, l);
                None
            }
            None => None,
        };
        // peer 侧双 clone：读=调用方写来的命令，写=设备响应。loopback 是
        // 全双工（client.write → peer.read；peer.write → client.read）——
        // 调用方→设备的读端必须在 peer 上，用 client 的 clone 读会抢走
        // 本应给调用方的响应数据（曾致终端收不到任何回显）。
        let mut peer_write = peer.try_clone()?;
        let mut peer_read = peer.try_clone()?;
        let conn_r = conn.clone();
        let sock_r = writer_sock.as_ref().and_then(|s| s.try_clone().ok());
        // 设备 → 调用方
        std::thread::spawn(move || {
            let mut rx = rx;
            while let Ok(data) = rx.recv() {
                if std::io::Write::write_all(&mut peer_write, &data).is_err() {
                    break;
                }
            }
            // 设备侧流结束：关 peer 让调用方读到 EOF
            let _ = peer_write.shutdown(std::net::Shutdown::Both);
            if let Some(c) = &conn_r {
                c.close_stream(local);
            }
            if let Some(s) = &sock_r {
                let _ = s.shutdown(std::net::Shutdown::Both);
            }
        });
        // 调用方 → 设备（直连：write_stream；server 桥：裸写 socket）
        let conn_w = conn.clone();
        let mut sock_w = writer_sock;
        std::thread::spawn(move || {
            let mut buf = [0u8; 8192];
            loop {
                match peer_read.read(&mut buf) {
                    Ok(0) => break,
                    Ok(n) => {
                        let ok = match (&conn_w, &mut sock_w) {
                            (Some(c), _) => c.write_stream(local, &buf[..n]).is_ok(),
                            (None, Some(s)) => {
                                use std::io::Write as _;
                                s.write_all(&buf[..n]).is_ok()
                            }
                            (None, None) => false,
                        };
                        if !ok {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
            let _ = peer_read.shutdown(std::net::Shutdown::Both);
            if let Some(c) = &conn_w {
                c.close_stream(local);
            }
        });
        drop(peer);
        Ok(client)
    }

    pub fn close(mut self) {
        self.close_inner();
    }

    fn close_inner(&mut self) {
        let local = self.local;
        self.local = 0; // 防止 Drop 再次关闭
        match self.writer.as_ref() {
            Some(StreamWriter::Link(c, l)) => c.close_stream(*l),
            Some(StreamWriter::Socket(s)) => {
                let _ = s.shutdown(std::net::Shutdown::Both);
            }
            None => {
                if local != 0 {
                    if let Some(c) = self.conn.as_ref() {
                        c.close_stream(local);
                    }
                }
            }
        }
    }
}

impl Drop for Stream {
    fn drop(&mut self) {
        if self.local != 0 || self.writer.is_some() {
            self.close_inner();
        }
    }
}

impl DeviceConn {
    /// 连接：USB（直驱）或 TCP（无线明文 5555 / tcpip 模式）。
    /// 完成 CNXN/AUTH 握手并启动 reader 线程；失败给出人话原因。
    pub fn connect(serial: &str, tcp_addr: Option<&str>) -> Result<Arc<DeviceConn>, String> {
        // emulator-N → TCP 127.0.0.1:(N-1)（adb 端口 = console 端口 - 1；遗留兼容）
        let tcp_owned;
        let tcp_addr = match tcp_addr {
            Some(a) => Some(a.to_string()),
            None => match serial.strip_prefix("emulator-").and_then(|p| p.parse::<u16>().ok()) {
                Some(p) => {
                    tcp_owned = format!("127.0.0.1:{}", p - 1);
                    Some(tcp_owned)
                }
                None => None,
            },
        };
        match tcp_addr {
            Some(addr) => Self::connect_tcp(serial, &addr),
            None => Self::connect_usb(serial),
        }
    }

    /// TCP 连接：先试 TLS（无线调试标准 _adb._tcp，授权持久化免弹窗）；
    /// TLS 不可达/失败则回落明文（5555 tcpip 模式，需每次授权）。
    fn connect_tcp(serial: &str, addr: &str) -> Result<Arc<DeviceConn>, String> {
        // TLS 端口探测：无线调试默认监听 5555 之外的 TLS 端口，
        // mDNS 会通告真实端口；这里先试 adb 惯例 5555，失败由上层 fallback。
        match crate::pairing::tls_adb_connect(addr) {
            Ok((tls, stream)) => {
                tracing::info!("TLS 无线调试通道建立: {addr}");
                let w = stream.try_clone().map_err(|e| format!("TLS 半分离失败: {e}"))?;
                let tls = Arc::new(std::sync::Mutex::new(tls));
                let mut rh = ReadHalf::Tls { conn: Arc::clone(&tls), stream };
                let mut wh = WriteHalf::Tls { conn: tls, stream: w };
                let max_payload = handshake(&mut rh, &mut wh, serial)?;
                return Ok(Self::finalize(serial, "tcp", rh, wh, max_payload));
            }
            Err(e) => {
                tracing::debug!("TLS 无线调试不可达（{e}），回落明文 TCP");
            }
        }
        let s = TcpStream::connect(addr).map_err(|e| format!("TCP 连接 {addr} 失败: {e}"))?;
        s.set_nodelay(true).ok();
        let w = s.try_clone().map_err(|e| format!("TCP 半分离失败: {e}"))?;
        let mut rh = ReadHalf::Tcp(s);
        let mut wh = WriteHalf::Tcp(w);
        let max_payload = handshake(&mut rh, &mut wh, serial)?;
        Ok(Self::finalize(serial, "tcp", rh, wh, max_payload))
    }

    /// USB 直驱。两类坑及其解法：
    /// 1) 上一会话异常退出 → adbd 仍认为旧连接在线：收到我们的 CNXN 会
    ///    kick 旧传输并**重枚举整个 USB gadget**（本机句柄随即 NO_DEVICE/
    ///    STALL，现象为写成功读立刻报错）。设备 1~2s 后以全新状态回归，
    ///    重开句柄重发 CNXN 即拿到 AUTH——真 adb server 靠热插拔自动扛过
    ///    这一下，我们用耐心重试等效（实测魅族 FlymeOS Android 16）。
    /// 2) Linux 上端口复位可自愈传输层翻转错位（usbfs 专属），作为兜底。
    fn connect_usb(serial: &str) -> Result<Arc<DeviceConn>, String> {
        let mut last_err = String::new();
        for attempt in 1..=6u32 {
            let Some(d) = usb::scan().into_iter().find(|d| d.serial == serial) else {
                // kick 重枚举进行中：等它回来
                if attempt < 4 {
                    tracing::debug!("USB 设备暂不在总线（可能正在重枚举），1.5s 后重试");
                    std::thread::sleep(Duration::from_millis(1500));
                    continue;
                }
                return Err(format!("USB 设备不在总线上: {serial}"));
            };
            match usb::UsbHandle::open(&d) {
                Ok(h) => match h.split() {
                    Ok(w) => {
                        let mut rh = ReadHalf::Usb(h);
                        let mut wh = WriteHalf::Usb(w);
                        match handshake(&mut rh, &mut wh, serial) {
                            Ok(max_payload) => {
                                if attempt > 1 {
                                    tracing::info!("USB 第 {attempt} 轮连接成功（kick 自愈）");
                                }
                                return Ok(Self::finalize(serial, "usb", rh, wh, max_payload));
                            }
                            Err(e) => last_err = e,
                        }
                    }
                    Err(e) => last_err = format!("USB 写半分离失败: {e}"),
                },
                Err(e) if e.raw_os_error() == Some(16) => {
                    return Err(format!("USB 接口被占用（外部 adb server 未退出?）: {e}"));
                }
                Err(e) => {
                    // EPROTO(71)/ENODEV(19)/ENOENT(2) 都是可自愈的传输层状态——走 reset 重试
                    last_err = format!("USB 接口打开失败: {e}");
                    tracing::debug!("USB 打开失败（{e}），进入重试");
                }
            }
            if attempt < 6 {
                // kick 重枚举（荣耀/Flyme 实测）：devnum 漂移期间句柄全部 ENODEV，
                // 等 3s 让总线尘埃落定再战；第 3 轮起加端口复位兜底翻转错位。
                if attempt >= 3 {
                    #[cfg(target_os = "linux")]
                    if let Some(d) = usb::scan().into_iter().find(|d| d.serial == serial) {
                        if let Ok(h) = usb::UsbHandle::open(&d) {
                            match h.reset() {
                                Ok(()) => tracing::info!("Linux USB 端口复位已执行"),
                                Err(e) => tracing::debug!("USB 端口复位失败: {e}"),
                            }
                        }
                    }
                }
                tracing::debug!("USB 第 {attempt} 轮失败（{last_err}），3s 后重试（等重枚举稳定）");
                std::thread::sleep(Duration::from_secs(3));
            }
        }
        Err(format!("USB 连接失败（已重试 6 轮）: {last_err}"))
    }

    /// 握手完成后组装连接并启动 reader 线程
    fn finalize(
        serial: &str,
        kind: &'static str,
        rh: ReadHalf,
        wh: WriteHalf,
        max_payload: u32,
    ) -> Arc<DeviceConn> {
        let conn = Arc::new(DeviceConn {
            serial: serial.to_string(),
            kind,
            inner: Arc::new(ConnInner {
                write: Mutex::new(wh),
                streams: Mutex::new(HashMap::new()),
                alive: AtomicBool::new(true),
                dead_reason: Mutex::new(String::new()),
                max_payload,
            }),
            next_local: AtomicU32::new(1),
            server_backed: false,
        });
        conn.spawn_reader(rh);
        conn
    }

    /// adb-server 桥连接（macOS 开发机回退路径）：CNXN/AUTH 由系统 adb
    /// server 完成，这里只持 serial；每路流一条独立 smart-socket 连接。
    fn connect_server(serial: &str) -> Result<Arc<DeviceConn>, String> {
        // 探活：server 可达且设备在线
        let mut s = TcpStream::connect("127.0.0.1:5037")
            .map_err(|e| format!("adb server 不可达（127.0.0.1:5037）: {e}"))?;
        s.set_read_timeout(Some(Duration::from_secs(3))).ok();
        smart_service(&mut s, "host:devices")?;
        let list = smart_payload(&mut s)?;
        let online = String::from_utf8_lossy(&list)
            .lines()
            .any(|l| l.split_whitespace().next() == Some(serial));
        if !online {
            return Err(format!("adb server 未见到设备 {serial}"));
        }
        Ok(Arc::new(DeviceConn {
            serial: serial.to_string(),
            kind: "server",
            inner: Arc::new(ConnInner {
                write: Mutex::new(WriteHalf::Tcp(placeholder_stream())), // 占位，不使用
                streams: Mutex::new(HashMap::new()),
                alive: AtomicBool::new(true),
                dead_reason: Mutex::new(String::new()),
                max_payload: 1024 * 1024,
            }),
            next_local: AtomicU32::new(1),
            server_backed: true,
        }))
    }

    fn send(&self, msg: &Message) -> Result<(), String> {
        let mut w = self.inner.write.lock().unwrap();
        w.write_all_bytes(&msg.encode())
            .map_err(|e| {
                self.mark_dead(&e.to_string());
                format!("发送失败: {e}")
            })
    }

    fn mark_dead(&self, reason: &str) {
        self.inner.alive.store(false, Ordering::Relaxed);
        *self.inner.dead_reason.lock().unwrap() = reason.to_string();
    }

    /// 打开一路流（服务如 shell:v2:... / sync: / localabstract:x）。
    /// 阻塞至设备 OKAY（或拒绝/超时）。
    pub fn open_stream(self: &Arc<Self>, service: &str) -> Result<Stream, String> {
        // adb-server 桥：每路流一条 smart-socket 连接。
        // transport 切换：host:tport:<serial>（server 34~36）失败则回落
        // legacy host:transport:<serial>（adb 37 起移除 tport，实测 37.0.1
        // FAIL "more than one device/emulator"——legacy 各版本均支持）。
        if self.server_backed {
            let mut s = TcpStream::connect("127.0.0.1:5037")
                .map_err(|e| format!("adb server 不可达: {e}"))?;
            s.set_nodelay(true).ok();
            match smart_service(&mut s, &format!("host:tport:{}", self.serial)) {
                Ok(()) => {
                    // tport OKAY 后 server 推 8 字节 transport_id（LE u64），必须消费
                    let mut tid = [0u8; 8];
                    read_exact_sock(&mut s, &mut tid)?;
                }
                Err(_) => {
                    // 37+：换新连接回落 legacy host:transport（tport 的 FAIL
                    // 会把当前连接置为 closed，任何后续 service 无应答）
                    let mut s2 = TcpStream::connect("127.0.0.1:5037")
                        .map_err(|e| format!("adb server 不可达: {e}"))?;
                    s2.set_nodelay(true).ok();
                    smart_service(&mut s2, &format!("host:transport:{}", self.serial))?;
                    s = s2;
                }
            }
            smart_service(&mut s, service)?;
            return Ok(Stream::from_tcp(s));
        }
        if !self.inner.alive.load(Ordering::Relaxed) {
            return Err(format!("连接已断开: {}", self.inner.dead_reason.lock().unwrap()));
        }
        let local = self.next_local.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = mpsc::channel();
        let open = Arc::new((Mutex::new(OpenState::Pending), Condvar::new()));
        let window = Arc::new((Mutex::new(0u32), Condvar::new()));
        self.inner.streams.lock().unwrap().insert(
            local,
            StreamState { tx, open: Arc::clone(&open), window },
        );

        let data = format!("{service}\0").into_bytes();
        self.send(&Message { cmd: A_OPEN, arg0: local, arg1: 0, data })?;

        // 等 OKAY（Pending→Open/Rejected；reader 线程经 dispatch 推进）
        let (om, ocv) = &*open;
        let mut st = om.lock().unwrap();
        let deadline = Instant::now() + Duration::from_secs(15);
        loop {
            match &*st {
                OpenState::Open => {
                    drop(st);
                    return Ok(Stream {
                        local,
                        conn: Some(Arc::clone(self)),
                        rx: Some(rx),
                        writer: Some(StreamWriter::Link(Arc::clone(self), local)),
                    });
                }
                OpenState::Rejected(e) => {
                    let e = e.clone();
                    drop(st);
                    self.inner.streams.lock().unwrap().remove(&local);
                    return Err(format!("设备拒绝流: {e}"));
                }
                OpenState::Pending => {}
            }
            let now = Instant::now();
            if now >= deadline {
                drop(st);
                self.inner.streams.lock().unwrap().remove(&local);
                return Err("OPEN 应答超时".into());
            }
            let (guard, timeout) = ocv.wait_timeout(st, deadline - now).unwrap();
            st = guard;
            if timeout.timed_out() {
                continue; // 循环头再判 deadline（防虚假唤醒超误报）
            }
        }
    }

    /// 写流数据（≤max_payload 分片；每片等发送窗）
    pub fn write_stream(&self, local: u32, mut data: &[u8]) -> Result<(), String> {
        let window = {
            let streams = self.inner.streams.lock().unwrap();
            streams.get(&local).map(|s| Arc::clone(&s.window))
        };
        let Some(window) = window else {
            return Err("流已关闭".into());
        };
        let max = self.inner.max_payload as usize;
        while !data.is_empty() {
            let n = data.len().min(max);
            // 流控：未确认 WRTE ≥ 窗口时等 OKAY
            {
                let (wm, wcv) = &*window;
                let mut unacked = wm.lock().unwrap();
                let deadline = Instant::now() + Duration::from_secs(15);
                while *unacked >= SEND_WINDOW {
                    let now = Instant::now();
                    if now >= deadline {
                        return Err("发送窗等待超时（设备未确认）".into());
                    }
                    let (g, t) = wcv.wait_timeout(unacked, deadline - now).unwrap();
                    unacked = g;
                    if t.timed_out() {
                        break;
                    }
                }
                if *unacked >= SEND_WINDOW {
                    return Err("发送窗等待超时（设备未确认）".into());
                }
                *unacked += 1;
            }
            let _tw = Instant::now();
            self.send(&Message {
                cmd: A_WRTE,
                arg0: local,
                arg1: 0,
                data: data[..n].to_vec(),
            })?;
            let _dt = _tw.elapsed().as_millis();
            if _dt > 50 {
                tracing::info!("[tx] WRTE local={local} len={n} 耗时{}ms", _dt);
            }
            data = &data[n..];
        }
        Ok(())
    }

    pub fn close_stream(&self, local: u32) {
        if local == 0 {
            return;
        }
        let removed = self.inner.streams.lock().unwrap().remove(&local);
        if removed.is_some() {
            let _ = self.send(&Message { cmd: A_CLSE, arg0: local, arg1: 0, data: Vec::new() });
        }
    }

    pub fn alive(&self) -> bool {
        self.inner.alive.load(Ordering::Relaxed)
    }

    /// reader 线程：持续收帧、分发到流队列；断开时收割全部流
    fn spawn_reader(self: &Arc<Self>, rh: ReadHalf) {
        let this = Arc::clone(self);
        std::thread::spawn(move || {
            let mut rh = rh;
            loop {
                match read_message(&mut rh) {
                    Ok(Some(msg)) => this.dispatch(msg),
                    Ok(None) => {
                        this.mark_dead("设备关闭连接");
                        break;
                    }
                    Err(e) => {
                        this.mark_dead(&e);
                        break;
                    }
                }
            }
            // 全部流置为拒绝（唤醒 open 等待者）并 drop 发送端（消费端收 Disconnected）
            let streams: Vec<StreamState> =
                this.inner.streams.lock().unwrap().drain().map(|(_, v)| v).collect();
            for s in streams {
                let (om, ocv) = &*s.open;
                let mut st = om.lock().unwrap();
                if matches!(&*st, OpenState::Pending) {
                    *st = OpenState::Rejected("连接断开".into());
                }
                ocv.notify_all();
                drop(st);
                drop(s.tx);
            }
        });
    }

    fn dispatch(self: &Arc<Self>, msg: Message) {
        match msg.cmd {
            A_OKAY => {
                let st = {
                    let streams = self.inner.streams.lock().unwrap();
                    streams.get(&msg.arg1).map(|s| (Arc::clone(&s.open), Arc::clone(&s.window)))
                };
                let Some((open, window)) = st else { return };
                // Pending→Open（OPEN 应答）；已 Open→WRTE 确认（窗口-1）
                let (om, ocv) = &*open;
                let mut ost = om.lock().unwrap();
                let just_opened = matches!(&*ost, OpenState::Pending);
                if just_opened {
                    *ost = OpenState::Open;
                }
                drop(ost);
                ocv.notify_all();
                if !just_opened {
                    let (wm, wcv) = &*window;
                    let mut unacked = wm.lock().unwrap();
                    *unacked = unacked.saturating_sub(1);
                    wcv.notify_all();
                }
            }
            A_WRTE => {
                let tx = {
                    let streams = self.inner.streams.lock().unwrap();
                    streams.get(&msg.arg1).map(|s| s.tx.clone())
                };
                if let Some(tx) = tx {
                    let _ = tx.send(msg.data);
                    // 流控：确认设备 WRTE（arg0=设备侧流 id）
                    let _ = self.send(&Message {
                        cmd: A_OKAY,
                        arg0: msg.arg1,
                        arg1: msg.arg0,
                        data: Vec::new(),
                    });
                }
            }
            A_CLSE => {
                let removed = self.inner.streams.lock().unwrap().remove(&msg.arg1);
                if let Some(st) = removed {
                    let (om, ocv) = &*st.open;
                    let mut ost = om.lock().unwrap();
                    if matches!(&*ost, OpenState::Pending) {
                        *ost = OpenState::Rejected("closed".into());
                    }
                    drop(ost);
                    ocv.notify_all();
                    drop(st.tx);
                }
            }
            _ => {}
        }
    }
}

// ---------------- 帧读取 ----------------

/// 阻塞读一帧（deadline 兜底；TimedOut/WouldBlock 时循环重试）
fn read_message(rh: &mut ReadHalf) -> Result<Option<Message>, String> {
    let t0 = Instant::now();
    let r = read_message_with(rh, None);
    if let Ok(Some(m)) = &r {
        let dt = t0.elapsed().as_millis();
        if dt > 200 {
            tracing::debug!("[read] {:?} 帧耗时 {}ms len={}", m.cmd.to_le_bytes().map(|b| b as char).iter().collect::<String>(), dt, m.data.len());
        }
    }
    r
}

fn read_message_with(rh: &mut ReadHalf, deadline: Option<Instant>) -> Result<Option<Message>, String> {
    let _t0 = Instant::now();
    let mut hdr = [0u8; 24];
    let mut got = 0;
    while got < 24 {
        // TCP：大超时一次到位（120s 由调用方 deadline 兜底）；USB：usbfs ioctl 自带 20s。
        if let ReadHalf::Tcp(_) = rh { rh.set_read_timeout(Some(Duration::from_secs(120))); }
        match rh.read_some(&mut hdr[got..]) {
            Ok(0) => return Ok(None),
            Ok(n) => got += n,
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                if let Some(d) = deadline {
                    if Instant::now() >= d {
                        return Err("读取超时".into());
                    }
                } else {
                    return Err("读取超时（设备无应答）".into());
                }
            }
            Err(e) => return Err(format!("读取: {e}")),
        }
    }
    let cmd = u32::from_le_bytes([hdr[0], hdr[1], hdr[2], hdr[3]]);
    let len = u32::from_le_bytes([hdr[12], hdr[13], hdr[14], hdr[15]]) as usize;
    if len > 8 * 1024 * 1024 {
        return Err(format!("帧负载异常: {len}"));
    }
    let mut data = vec![0u8; len];
    let mut got = 0;
    while got < data.len() {
        if let ReadHalf::Tcp(_) = rh { rh.set_read_timeout(Some(Duration::from_secs(120))); }
        match rh.read_some(&mut data[got..]) {
            Ok(0) => return Ok(None),
            Ok(n) => got += n,
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                if let Some(d) = deadline {
                    if Instant::now() >= d {
                        return Err("读取负载超时".into());
                    }
                } else {
                    return Err("读取负载超时（设备无应答）".into());
                }
            }
            Err(e) => return Err(format!("读取负载: {e}")),
        }
    }
    let dt = _t0.elapsed().as_millis();
    if dt > 50 {
        let tag = match cmd { 0x4e584e43 => "CNXN", 0x48545541 => "AUTH", 0x4e45504f => "OPEN", 0x59414b4f => "OKAY", 0x45534c43 => "CLSE", 0x45545257 => "WRTE", _ => "?" };
        tracing::info!("[rx] {tag} arg0={} arg1={} len={} 耗时{}ms",
            u32::from_le_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]),
            u32::from_le_bytes([hdr[8], hdr[9], hdr[10], hdr[11]]),
            len, dt);
    }
    Ok(Some(Message {
        cmd,
        arg0: u32::from_le_bytes([hdr[4], hdr[5], hdr[6], hdr[7]]),
        arg1: u32::from_le_bytes([hdr[8], hdr[9], hdr[10], hdr[11]]),
        data,
    }))
}

// ---------------- CNXN/AUTH 握手 ----------------

/// CNXN/AUTH 握手。返回设备侧最大负载（arg1）。
/// 已授权密钥：TOKEN→SIGN→CNXN 秒过；新密钥：TOKEN→SIGN→TOKEN→PUBKEY→
/// （手机弹「允许 USB 调试」，最长等 120s）→CNXN；拒绝→连接被设备关闭。
fn handshake(rh: &mut ReadHalf, wh: &mut WriteHalf, serial: &str) -> Result<u32, String> {
    let cnxn = Message {
        cmd: A_CNXN,
        arg0: A_VERSION,
        arg1: MAX_PAYLOAD,
        data: b"host::features=shell_v2,cmd,stat_v2,ls_v2,fixed_push_mkdir,apex,abb_exec,sendrecv_v2,track_app,sendrecv_v2_brotli,openscreen_mdns,remount_shell,track_app,abb,fixed_push_symlink_timestamp,abb_exec,apex,can_reboot_bootloader,fixed_push_mkdir".to_vec(),
    };
    wh.write_all_bytes(&cnxn.encode())
        .map_err(|e| format!("CNXN 发送失败: {e}"))?;

    let key = crate::adbkeys::load_or_create();
    // 未授权设备等用户在手机上点「允许 USB 调试」——弹窗可能被忽略/熄屏，
    // 给 5 分钟而不是 2 分钟（实测 FlymeOS 用户平均反应 1~3 分钟）。
    let deadline = Instant::now() + Duration::from_secs(300);
    let mut tokens = 0u32;
    let mut auth_announced = false;
    loop {
        let msg = read_message_with(rh, Some(deadline))?.ok_or("设备在握手期间断开")?;
        match msg.cmd {
            A_CNXN => {
                // 设备最大负载在 arg1（payload 是 ASCII 身份串）；过老的
                // adbd（arg1<4096）按 4K 保守值
                let dev_max = msg.arg1.clamp(4096, MAX_PAYLOAD);
                tracing::debug!(
                    "CNXN 完成 serial={serial} dev_max={dev_max} identity={}",
                    String::from_utf8_lossy(&msg.data)
                );
                return Ok(dev_max);
            }
            A_AUTH => match msg.arg0 {
                1 => {
                    tokens += 1;
                    match tokens {
                        1 => {
                            // TOKEN → 签名应答（AOSP adb.cpp send_auth_response：
                            // RSA_sign(NID_sha1)；密钥已被设备授权时直接回 CNXN）
                            let sig = crate::adbkeys::sign_token(&key, &msg.data);
                            wh.write_all_bytes(
                                &Message { cmd: A_AUTH, arg0: 2, arg1: 0, data: sig }.encode(),
                            )
                            .map_err(|e| format!("AUTH 签名发送失败: {e}"))?;
                        }
                        2 => {
                            // AOSP send_auth_pubkey：密钥未被认（第二个 TOKEN）
                            // → 发公钥触发手机授权弹窗，用户点允许后 CNXN
                            if !auth_announced {
                                tracing::info!(
                                    "等待 {serial} 上「允许 USB 调试」弹窗确认（勾选一律允许）"
                                );
                                auth_announced = true;
                            }
                            let pk = crate::adbkeys::android_pubkey(&key);
                            let payload = format!("{pk}\0mwine@nas");
                            wh.write_all_bytes(
                                &Message {
                                    cmd: A_AUTH,
                                    arg0: 3,
                                    arg1: 0,
                                    data: payload.into_bytes(),
                                }
                                .encode(),
                            )
                            .map_err(|e| format!("AUTH 公钥发送失败: {e}"))?;
                        }
                        _ => {
                            // 授权后个别固件重发 TOKEN 要求再签名
                            let sig = crate::adbkeys::sign_token(&key, &msg.data);
                            wh.write_all_bytes(
                                &Message { cmd: A_AUTH, arg0: 2, arg1: 0, data: sig }.encode(),
                            )
                            .map_err(|e| format!("AUTH 复签发送失败: {e}"))?;
                        }
                    }
                }
                2 => return Err(format!("设备拒绝认证（{serial}）")),
                other => return Err(format!("AUTH 未知子类型 {other}")),
            },
            other => return Err(format!("握手收到异常帧 {other:#010x}")),
        }
    }
}

// ---------------- 注册表（serial → 连接） ----------------

pub struct RegistryEntry {
    pub kind: &'static str,
    /// TCP 设备的回连地址（USB 为 None）
    pub tcp_addr: Option<String>,
    pub conn: Arc<DeviceConn>,
}

pub static REGISTRY: LazyLock<Mutex<HashMap<String, RegistryEntry>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

/// 获取/建立某设备的连接（复用存活连接；断开则重连）
pub fn get_conn(serial: &str, tcp_addr: Option<&str>) -> Result<Arc<DeviceConn>, String> {
    {
        let reg = REGISTRY.lock().unwrap();
        if let Some(e) = reg.get(serial) {
            if e.conn.alive() {
                return Ok(Arc::clone(&e.conn));
            }
        }
    }
    // 回连地址：优先入参 → 注册表记忆（无线设备）
    let tcp_addr = match tcp_addr {
        Some(a) => Some(a.to_string()),
        None => REGISTRY.lock().unwrap().get(serial).and_then(|e| e.tcp_addr.clone()),
    };
    // USB serial：默认走官方 adb server 桥（荣耀 MagicOS / 魅族 Flyme 等
    // 固件对非官方客户端的 CNXN 会直接踢线重枚举，自研 usbfs 直驱在
    // 这类国产固件上不可用——官方 server 的 libusb 栈有热插拔自愈，
    // 我们已在 usbmon 抓包确认字节级一致仍被踢，故此路径弃用）。
    // 设 MWINE_NATIVE_USB=1 可强制尝试自研直驱（实验）。
    let native_usb = std::env::var("MWINE_NATIVE_USB").ok().as_deref() == Some("1");
    let conn = if tcp_addr.is_none() && !native_usb {
        match DeviceConn::connect_server(serial) {
            Ok(c) => c,
            Err(e) => {
                // server 桥失败再试一次直驱（无 server 环境兜底）
                match DeviceConn::connect(serial, None) {
                    Ok(c) => c,
                    Err(e2) => return Err(format!("{e}；USB 直驱也失败: {e2}")),
                }
            }
        }
    } else {
        match DeviceConn::connect(serial, tcp_addr.as_deref()) {
            Ok(c) => c,
            Err(e) => {
                // TCP/TLS 直连失败：无线设备也可走 server 桥兜底
                if DeviceConn::connect_server(serial).is_ok() {
                    DeviceConn::connect_server(serial)?
                } else {
                    return Err(e);
                }
            }
        }
    };
    let kind = conn.kind;
    REGISTRY.lock().unwrap().insert(
        serial.to_string(),
        RegistryEntry {
            kind,
            tcp_addr,
            conn: Arc::clone(&conn),
        },
    );
    Ok(conn)
}

/// server 桥模式的写半占位（ConnInner.write 不被该模式使用）
fn placeholder_stream() -> TcpStream {
    use std::net::TcpListener;
    let l = TcpListener::bind(("127.0.0.1", 0)).expect("回环监听");
    let addr = l.local_addr().expect("回环地址");
    let c = TcpStream::connect(addr).expect("回环连接");
    let _ = l.accept();
    c
}

// ---------------- smart-socket 辅助（adb server 5037 协议） ----------------

/// 发一个服务串并读 OKAY/FAIL
fn smart_service(s: &mut TcpStream, svc: &str) -> Result<(), String> {
    use std::io::Write as _;
    let payload = svc.as_bytes();
    s.write_all(format!("{:04x}", payload.len()).as_bytes())
        .map_err(|e| e.to_string())?;
    s.write_all(payload).map_err(|e| e.to_string())?;
    let mut st = [0u8; 4];
    read_exact_sock(s, &mut st)?;
    match &st {
        b"OKAY" => Ok(()),
        b"FAIL" => {
            let mut lb = [0u8; 4];
            read_exact_sock(s, &mut lb)?;
            let len = usize::from_str_radix(&String::from_utf8_lossy(&lb), 16).unwrap_or(0);
            let mut msg = vec![0u8; len.min(4096)];
            if len > 0 {
                read_exact_sock(s, &mut msg)?;
            }
            Err(String::from_utf8_lossy(&msg).trim().to_string())
        }
        other => Err(format!("smart-socket 状态异常: {}", String::from_utf8_lossy(other))),
    }
}

/// 读 OKAY 后的长度前缀载荷（host:devices 等）
fn smart_payload(s: &mut TcpStream) -> Result<Vec<u8>, String> {
    let mut lb = [0u8; 4];
    read_exact_sock(s, &mut lb)?;
    let len = usize::from_str_radix(&String::from_utf8_lossy(&lb), 16).unwrap_or(0);
    let mut out = vec![0u8; len.min(64 * 1024)];
    if len > 0 {
        read_exact_sock(s, &mut out)?;
    }
    Ok(out)
}

fn read_exact_sock(s: &mut TcpStream, buf: &mut [u8]) -> Result<(), String> {
    use std::io::Read as _;
    let mut got = 0;
    while got < buf.len() {
        match s.read(&mut buf[got..]) {
            Ok(0) => return Err("adb server 连接中断".into()),
            Ok(n) => got += n,
            Err(e) => return Err(format!("adb server 读取: {e}")),
        }
    }
    Ok(())
}

pub fn drop_conn(serial: &str) {
    // 摘除注册项；连接的流随 Arc 归零（reader 线程退出时）一并收割
    if let Some(e) = REGISTRY.lock().unwrap().remove(serial) {
        e.conn.inner.alive.store(false, Ordering::Relaxed);
    }
}

/// 官方 adb server 的权威设备列表（serial, state）——USB 设备以此为准
pub fn server_devices() -> Vec<(String, String)> {
    let mut out = Vec::new();
    let Ok(mut s) = TcpStream::connect("127.0.0.1:5037") else { return out };
    s.set_read_timeout(Some(Duration::from_secs(3))).ok();
    if smart_service(&mut s, "host:devices").is_err() {
        return out;
    }
    let Ok(list) = smart_payload(&mut s) else { return out };
    for line in String::from_utf8_lossy(&list).lines() {
        let mut it = line.split_whitespace();
        let (Some(serial), Some(state)) = (it.next(), it.next()) else { continue };
        if serial.is_empty() {
            continue;
        }
        out.push((serial.to_string(), state.to_string()));
    }
    out
}

pub fn list_connected() -> Vec<(String, &'static str)> {
    REGISTRY
        .lock()
        .unwrap()
        .iter()
        .filter(|(_, e)| e.conn.alive())
        .map(|(k, e)| (k.clone(), e.kind))
        .collect()
}
