//! scrcpy 客户端编排：部署 server -> 端口转发 -> 启动 -> 连接三 socket。

use std::net::TcpStream;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use mwine_adb::{AdbClient, ShellStream};

use crate::audio::AudioStream;
use crate::control::ControlMessage;
use crate::device_msg::{read_device_message, DeviceMessage};
use crate::error::{Error, Result};
use crate::options::{server_command, session_remote_path, ServerOptions};
use crate::video::{read_device_name, VideoStream};

/// 在本机分配一个空闲 TCP 端口（bind :0 后立即释放）。
fn free_port() -> std::io::Result<u16> {
    let listener = std::net::TcpListener::bind(("127.0.0.1", 0))?;
    Ok(listener.local_addr()?.port())
}

#[derive(Debug, Clone)]
pub struct ScrcpyClient {
    pub adb: AdbClient,
    pub serial: String,
    pub server_jar: PathBuf,
    pub options: ServerOptions,
}

impl ScrcpyClient {
    pub fn new(adb: AdbClient, serial: impl Into<String>, server_jar: impl Into<PathBuf>) -> Self {
        Self {
            adb,
            serial: serial.into(),
            server_jar: server_jar.into(),
            options: ServerOptions::default(),
        }
    }

    /// 启动完整会话。
    pub fn start(self) -> Result<Session> {
        self.options.validate()?;
        if !self.server_jar.is_file() {
            return Err(Error::ServerJarNotFound(
                self.server_jar.display().to_string(),
            ));
        }

        // 1. 推送 server jar（每会话独立路径，防并发会话互相覆盖写坏字节码）
        //    scid 必须回写进 options：server 的 socket 名与 forward 目标都由它派生，
        //    不回写时双方都退化为裸名 "scrcpy"，任何残留进程都会劫持新会话。
        let session = self.options.scid.unwrap_or_else(ServerOptions::random_scid);
        let mut options = self.options;
        options.scid = Some(session);
        let remote_jar = session_remote_path(session);
        let jar = std::fs::read(&self.server_jar)?;
        self.adb
            .push_bytes(&self.serial, &jar, &remote_jar)
            .map_err(|e| Error::LaunchFailed(format!("push server jar 失败: {e}")))?;

        // 2. 分配转发端口（socket 名由 scid 决定，多会话天然隔离）
        let port = free_port()?;
        self.adb
            .forward_abstract(&self.serial, port, &options.socket_name())
            .map_err(|e| Error::LaunchFailed(format!("forward 失败: {e}")))?;

        // 3. 启动 server 进程（长驻 shell；持有句柄即保持存活）
        let args = options.to_args();
        let cmd = server_command(&remote_jar, &args);
        let server_process = self
            .adb
            .shell_stream(&self.serial, &cmd)
            .map_err(|e| Error::LaunchFailed(format!("启动 server 进程失败: {e}")))?;

        // 4. 连接 video socket 并读取 dummy 字节。
        // adb forward 在 abstract socket 尚未创建时也会接受 TCP 连接后立刻关闭，
        // 因此必须把"读到 dummy 字节"作为 server 就绪判据，失败即重连。
        let deadline = Instant::now() + Duration::from_secs(15);
        let video_tcp = loop {
            let stream = match TcpStream::connect(("127.0.0.1", port)) {
                Ok(s) => s,
                Err(_) if Instant::now() < deadline => {
                    std::thread::sleep(Duration::from_millis(200));
                    continue;
                }
                Err(_) => return Err(Error::ConnectTimeout),
            };
            stream.set_nodelay(true).ok();
            stream.set_read_timeout(Some(Duration::from_secs(2))).ok();
            let mut dummy = [0u8; 1];
            use std::io::Read;
            match (&stream).read_exact(&mut dummy) {
                Ok(()) => {
                    stream.set_read_timeout(None).ok();
                    break stream;
                }
                Err(_) if Instant::now() < deadline => {
                    // server 尚未监听（或刚启动即退出），重试
                    std::thread::sleep(Duration::from_millis(200));
                    continue;
                }
                Err(_) => return Err(Error::ConnectTimeout),
            }
        };

        // 5. 连接 audio socket（若启用；server 在 video 后按序 accept 它），
        //    再连接 control socket——全部 accept 完成后才发送 64 字节设备名
        let audio_tcp = if options.audio {
            Some(connect_with_retry(port, Duration::from_secs(15))?)
        } else {
            None
        };
        // server 只 accept 启用的 socket：control=false 时连了也永远无人 accept
        //（客户端会 15s 超时并连带泄漏 forward 与 server 进程）
        let control_tcp = if options.control {
            Some(connect_with_retry(port, Duration::from_secs(15))?)
        } else {
            None
        };
        let device_name = read_device_name(&mut &video_tcp)?;

        // 6. 流（codec id 紧跟设备名之后）
        let video = VideoStream::new(video_tcp)?;
        let audio = match audio_tcp {
            Some(tcp) => Some(AudioStream::new(tcp)?),
            None => None,
        };

        Ok(Session {
            adb: self.adb,
            serial: self.serial,
            device_name,
            video,
            audio,
            control: control_tcp.map(ControlChannel::new),
            server_process: Some(server_process),
            forward_port: port,
            remote_jar,
        })
        // 注意：server_process 一旦 Drop（Session 销毁），远端进程随之退出
    }
}

fn connect_with_retry(port: u16, timeout: Duration) -> Result<TcpStream> {
    let deadline = Instant::now() + timeout;
    loop {
        match TcpStream::connect(("127.0.0.1", port)) {
            Ok(s) => {
                s.set_nodelay(true).ok();
                return Ok(s);
            }
            Err(_) if Instant::now() > deadline => return Err(Error::ConnectTimeout),
            Err(_) => std::thread::sleep(Duration::from_millis(100)),
        }
    }
}

/// 控制通道发送端。
pub struct ControlChannel {
    stream: TcpStream,
}

impl ControlChannel {
    fn new(stream: TcpStream) -> Self {
        Self { stream }
    }

    pub fn send(&mut self, msg: &ControlMessage) -> Result<()> {
        msg.write_to(&mut self.stream)
    }

    /// 非阻塞读取一条设备消息（剪贴板同步等）。
    /// 无数据到达时返回 None。
    /// 克隆底层流（供独立的设备消息读取线程使用；TcpStream 双工可分线程读写）。
    pub fn try_clone_stream(&self) -> std::io::Result<TcpStream> {
        self.stream.try_clone()
    }

    /// 阻塞读取一条设备消息（剪贴板同步等）。流结束返回 None。
    pub fn read_device_message_blocking(&mut self) -> Result<Option<DeviceMessage>> {
        read_device_message(&mut self.stream)
    }

    pub fn try_read_device_message(&mut self) -> Result<Option<DeviceMessage>> {
        self.stream.set_nonblocking(true)?;
        let result = read_device_message(&mut self.stream);
        self.stream.set_nonblocking(false)?;
        match result {
            Ok(msg) => Ok(msg),
            Err(Error::Io(e))
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::Interrupted =>
            {
                Ok(None)
            }
            Err(e) => Err(e),
        }
    }
}

/// 一次 scrcpy 会话。Drop 时终止远端 server 并清理端口转发。
pub struct Session {
    pub adb: AdbClient,
    pub serial: String,
    pub device_name: String,
    pub video: VideoStream,
    /// 音频流（ServerOptions::audio 开启时）
    pub audio: Option<AudioStream>,
    pub control: Option<ControlChannel>,
    server_process: Option<ShellStream>,
    pub forward_port: u16,
    /// 本会话的远端 jar（Drop 时清理）
    remote_jar: String,
}

impl Session {
    /// 显式停止会话。
    pub fn stop(&mut self) {
        if let Some(mut proc) = self.server_process.take() {
            proc.shutdown();
        }
        let _ = self.adb.remove_forward(&self.serial, self.forward_port);
        self.cleanup_jar();
    }

    fn cleanup_jar(&self) {
        let _ = self.adb.shell_blocking(
            &self.serial,
            &format!("rm -f '{}'", self.remote_jar.replace('\'', "'\\''")),
            std::time::Duration::from_secs(10),
        );
    }

    /// server 进程是否仍被本会话持有。
    pub fn server_alive(&self) -> bool {
        self.server_process.is_some()
    }

    /// 拆分为四部分，供多线程播放器使用：
    /// - VideoStream：视频 socket（通常移入独立读线程）
    /// - ControlChannel：控制通道
    /// - Option<AudioStream>：音频 socket（未启用为 None）
    /// - SessionGuard：持有 server 进程存活；Drop 时自动清理
    pub fn split(self) -> (VideoStream, Option<ControlChannel>, Option<AudioStream>, SessionGuard) {
        let this = std::mem::ManuallyDrop::new(self);
        // SAFETY: this 已被 ManuallyDrop 包裹（Drop 不会执行），
        // 各字段经 ptr::read 移出后由 SessionGuard 接管清理职责，
        // 不会出现双重释放。
        unsafe {
            let guard = SessionGuard {
                _device_name: std::ptr::read(&this.device_name),
                adb: std::ptr::read(&this.adb),
                serial: std::ptr::read(&this.serial),
                forward_port: this.forward_port,
                remote_jar: std::ptr::read(&this.remote_jar),
                server_process: std::ptr::read(&this.server_process),
            };
            let video = std::ptr::read(&this.video);
            let control = std::ptr::read(&this.control);
            let audio = std::ptr::read(&this.audio);
            (video, control, audio, guard)
        }
    }
}

/// 会话守卫：保持 scrcpy server 进程存活，Drop 时终止并清理转发。
pub struct SessionGuard {
    _device_name: String,
    adb: AdbClient,
    serial: String,
    forward_port: u16,
    remote_jar: String,
    server_process: Option<ShellStream>,
}

impl SessionGuard {
    pub fn stop(&mut self) {
        if let Some(mut proc) = self.server_process.take() {
            proc.shutdown();
        }
        let _ = self.adb.remove_forward(&self.serial, self.forward_port);
        let _ = self.adb.shell_blocking(
            &self.serial,
            &format!("rm -f '{}'", self.remote_jar.replace('\'', "'\\''")),
            std::time::Duration::from_secs(10),
        );
    }
}

impl Drop for SessionGuard {
    fn drop(&mut self) {
        self.stop();
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        self.stop();
    }
}
