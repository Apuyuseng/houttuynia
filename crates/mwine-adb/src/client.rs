//! 高层 ADB 客户端：设备发现、shell、推送、安装、端口转发。

use std::io::Read;
use std::net::{TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

use crate::connection::SmartConnection;
use crate::error::{Error, Result};
use crate::shell::{self, ShellOutput};
use crate::sync::SyncChannel;

pub const DEFAULT_ADB_PORT: u16 = 5037;

/// APK 安装进度。
#[derive(Debug, Clone, Copy)]
pub enum InstallProgress {
    /// APK 传输中（已传/总量字节）
    Push { sent: u64, total: u64 },
    /// 传输完成，设备侧安装中（pm install / dexopt，时长不可预估）
    Installing,
}

#[derive(Debug, Clone)]
pub struct AdbDevice {
    pub serial: String,
    pub state: String,
}

#[derive(Debug, Clone)]
pub struct AdbClient {
    pub host: String,
    pub port: u16,
}

impl Default for AdbClient {
    fn default() -> Self {
        Self {
            host: "127.0.0.1".into(),
            port: DEFAULT_ADB_PORT,
        }
    }
}

impl AdbClient {
    pub fn new() -> Self {
        Self::default()
    }

    fn addr(&self) -> String {
        format!("{}:{}", self.host, self.port)
    }

    fn connect(&self, timeout: Duration) -> Result<SmartConnection> {
        let addr = self
            .addr()
            .to_socket_addrs()
            .map_err(|e| Error::ServerUnreachable {
                addr: self.addr(),
                source: e,
            })?
            .next()
            .ok_or_else(|| Error::ServerUnreachable {
                addr: self.addr(),
                source: std::io::Error::new(std::io::ErrorKind::NotFound, "no address"),
            })?;
        let stream =
            TcpStream::connect_timeout(&addr, timeout).map_err(|e| Error::ServerUnreachable {
                addr: self.addr(),
                source: e,
            })?;
        stream.set_read_timeout(Some(timeout)).ok();
        stream.set_write_timeout(Some(timeout)).ok();
        stream.set_nodelay(true).ok();
        Ok(SmartConnection::from_stream(stream))
    }

    /// adb server 不在线时尝试用 adb 二进制拉起，然后重试连接。
    pub fn ensure_server(&self, adb_bin: Option<&Path>) -> Result<()> {
        if self.version().is_ok() {
            return Ok(());
        }
        let bin = adb_bin
            .map(|p| p.to_path_buf())
            .or_else(find_adb_on_path)
            .ok_or_else(|| {
                Error::AdbBinaryNotFound(
                    "adb not on PATH; pass sdk platform-tools path explicitly".into(),
                )
            })?;
        let status = Command::new(&bin).arg("start-server").status()?;
        if !status.success() {
            return Err(Error::ServiceFailed("adb start-server failed".into()));
        }
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            if self.version().is_ok() {
                return Ok(());
            }
            if Instant::now() > deadline {
                return Err(Error::ServerUnreachable {
                    addr: self.addr(),
                    source: std::io::Error::new(
                        std::io::ErrorKind::TimedOut,
                        "adb server did not come up after start-server",
                    ),
                });
            }
            std::thread::sleep(Duration::from_millis(300));
        }
    }

    pub fn version(&self) -> Result<String> {
        let mut conn = self.connect(Duration::from_secs(5))?;
        conn.send_service("host:version")?;
        let payload = conn.read_payload()?;
        Ok(String::from_utf8_lossy(&payload).trim().to_string())
    }

    pub fn devices(&self) -> Result<Vec<AdbDevice>> {
        let mut conn = self.connect(Duration::from_secs(5))?;
        conn.send_service("host:devices")?;
        let payload = conn.read_payload()?;
        let text = String::from_utf8_lossy(&payload);
        let mut devices = Vec::new();
        for line in text.lines() {
            if line.is_empty() {
                continue;
            }
            let mut parts = line.split('\t');
            if let (Some(serial), Some(state)) = (parts.next(), parts.next()) {
                devices.push(AdbDevice {
                    serial: serial.to_string(),
                    state: state.to_string(),
                });
            }
        }
        Ok(devices)
    }

    pub fn find_online_device(&self) -> Result<AdbDevice> {
        let devices = self.devices()?;
        devices
            .into_iter()
            .find(|d| d.state == "device")
            .ok_or_else(|| Error::DeviceNotFound("no online device".into()))
    }

    /// 通过 adb server 建立 TCP 设备连接（host:connect）。
    /// 用于 emulator 管道 adb 失联时的备用通道（console redir + adbd tcp 5555），
    /// 与命令行 `adb connect <addr>` 等价。已连接时 adb server 幂等返回。
    pub fn connect_network(&self, addr: &str) -> Result<String> {
        let mut conn = self.connect(Duration::from_secs(5))?;
        conn.send_service(&format!("host:connect {addr}"))?;
        let payload = conn.read_payload()?;
        Ok(String::from_utf8_lossy(&payload).trim().to_string())
    }

    /// 等待指定 serial 上线并处于 device 状态。
    pub fn wait_for_device(&self, serial: &str, timeout: Duration) -> Result<()> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Ok(devices) = self.devices() {
                if devices
                    .iter()
                    .any(|d| d.serial == serial && d.state == "device")
                {
                    return Ok(());
                }
            }
            if Instant::now() > deadline {
                return Err(Error::DeviceNotFound(format!(
                    "timeout waiting for {serial} to be online"
                )));
            }
            std::thread::sleep(Duration::from_millis(500));
        }
    }

    fn transport(&self, serial: &str) -> Result<SmartConnection> {
        let mut conn = self.connect(Duration::from_secs(10))?;
        conn.send_service(&format!("host:transport:{serial}"))?;
        Ok(conn)
    }

    /// adb root：让 adbd 以 root 运行（userdebug/eng 镜像可用）。
    /// 重启 adbd 会短暂掉线，返回后调用方通常需 wait_for_device。
    pub fn root(&self, serial: &str) -> Result<String> {
        let mut conn = self.transport(serial)?;
        conn.send_service("root:")?;
        let payload = conn.read_payload()?;
        Ok(String::from_utf8_lossy(&payload).trim().to_string())
    }

    /// 执行 shell 命令并等待结束（shell v2，分离 stdout/stderr/exit code）。
    pub fn shell_blocking(
        &self,
        serial: &str,
        cmd: &str,
        timeout: Duration,
    ) -> Result<ShellOutput> {
        let mut conn = self.transport(serial)?;
        conn.stream().set_read_timeout(Some(timeout)).ok();
        if conn.send_service(&shell::v2_service(cmd)).is_err() {
            // 老设备可能不支持 v2，回退 v1（混流、无退出码）
            let mut conn = self.transport(serial)?;
            conn.stream().set_read_timeout(Some(timeout)).ok();
            conn.send_service(&shell::v1_service(cmd))?;
            let data = shell::read_v1_to_end(conn.stream_mut())?;
            return Ok(ShellOutput {
                stdout: data,
                stderr: Vec::new(),
                exit_code: None,
            });
        }
        let reader = shell::ShellV2Reader::new(conn.stream_mut());
        reader.collect()
    }

    /// 启动长驻 shell 进程，返回持有连接的流。
    /// 连接保持打开期间远端进程存活；Drop 即断开（远端进程随之终止）。
    pub fn shell_stream(&self, serial: &str, cmd: &str) -> Result<ShellStream> {
        let mut conn = self.transport(serial)?;
        conn.stream().set_read_timeout(None).ok();
        conn.send_service(&shell::v2_service(cmd))?;
        Ok(ShellStream {
            stream: conn.into_stream(),
        })
    }

    /// 推送内存数据到设备（sync SEND）。
    pub fn push_bytes(&self, serial: &str, data: &[u8], remote_path: &str) -> Result<()> {
        let mut conn = self.transport(serial)?;
        conn.stream()
            .set_read_timeout(Some(Duration::from_secs(60)))
            .ok();
        conn.send_service("sync:")?;
        {
            let mut sync = SyncChannel::new(conn.stream_mut());
            sync.push_bytes(data, remote_path)?;
            sync.quit()?;
        }
        Ok(())
    }

    /// 推送本地文件到设备。
    pub fn push_file(&self, serial: &str, local: &Path, remote_path: &str) -> Result<()> {
        let data = std::fs::read(local)?;
        self.push_bytes(serial, &data, remote_path)
    }

    /// 推送本地文件或目录到设备目录（目录递归重建，返回全部推送的远端路径）。
    pub fn push_path(&self, serial: &str, local: &Path, remote_dir: &str) -> Result<Vec<String>> {
        let mut pushed = Vec::new();
        let name = local
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default();
        if name.is_empty() {
            return Err(Error::ServiceFailed(format!(
                "无法确定 {} 的文件名",
                local.display()
            )));
        }
        if local.is_file() {
            let remote = format!("{remote_dir}/{name}");
            self.push_file(serial, local, &remote)?;
            pushed.push(remote);
        } else if local.is_dir() {
            let base = format!("{remote_dir}/{name}");
            let root = local.to_path_buf();
            let mut stack = vec![(
                root.clone(),
                base.clone(),
            )];
            while let Some((l_dir, r_dir)) = stack.pop() {
                self.shell_blocking(
                    serial,
                    &format!("mkdir -p '{}'", r_dir.replace('\'', "'\\''")),
                    Duration::from_secs(15),
                )?;
                for entry in std::fs::read_dir(&l_dir)? {
                    let entry = entry?;
                    let path = entry.path();
                    if path.is_dir() {
                        let sub_name = entry.file_name().to_string_lossy().to_string();
                        stack.push((path, format!("{r_dir}/{sub_name}")));
                    } else {
                        let file_name = entry.file_name().to_string_lossy().to_string();
                        let remote = format!("{r_dir}/{file_name}");
                        self.push_file(serial, &path, &remote)?;
                        pushed.push(remote);
                    }
                }
            }
        } else {
            return Err(Error::ServiceFailed(format!(
                "{} 不存在",
                local.display()
            )));
        }
        Ok(pushed)
    }

    /// 从设备拉取文件（sync STAT + RECV），返回全部内容。
    pub fn pull_file(&self, serial: &str, remote_path: &str) -> Result<Vec<u8>> {
        let mut conn = self.transport(serial)?;
        conn.stream()
            .set_read_timeout(Some(Duration::from_secs(180)))
            .ok();
        conn.send_service("sync:")?;
        let data = {
            let mut sync = SyncChannel::new(conn.stream_mut());
            let data = sync.pull_file(remote_path)?;
            sync.quit()?;
            data
        };
        Ok(data)
    }

    /// 查询包的 base APK 在设备上的路径（`pm path` 首行）。
    pub fn package_apk_path(&self, serial: &str, package: &str) -> Result<String> {
        let out = self.shell_blocking(
            serial,
            &format!("pm path {package}"),
            Duration::from_secs(15),
        )?;
        out.stdout_string()
            .lines()
            .find_map(|l| l.trim().strip_prefix("package:").map(|s| s.to_string()))
            .ok_or_else(|| Error::ServiceFailed(format!("pm path returned nothing for {package}")))
    }

    /// 推送本地文件到设备（流式 + 进度回调，报告已传/总字节）。
    pub fn push_file_progress(
        &self,
        serial: &str,
        local: &Path,
        remote_path: &str,
        mut on_progress: impl FnMut(u64, u64),
    ) -> Result<()> {
        let total = std::fs::metadata(local)?.len();
        let mut file = std::fs::File::open(local)?;
        let mut conn = self.transport(serial)?;
        conn.stream()
            .set_read_timeout(Some(Duration::from_secs(180)))
            .ok();
        conn.send_service("sync:")?;
        {
            let mut sync = SyncChannel::new(conn.stream_mut());
            sync.push_stream(&mut file, remote_path, |sent| on_progress(sent, total))?;
            sync.quit()?;
        }
        Ok(())
    }

    /// 安装 APK：push 到临时路径 + `pm install -r`（带进度回调）。
    pub fn install_apk_with_progress(
        &self,
        serial: &str,
        apk: &Path,
        timeout: Duration,
        mut on_progress: impl FnMut(InstallProgress),
    ) -> Result<String> {
        let file_name = apk
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("app.apk");
        let remote = format!("/data/local/tmp/mwine_install_{file_name}");
        self.push_file_progress(serial, apk, &remote, |sent, total| {
            on_progress(InstallProgress::Push { sent, total });
        })?;
        on_progress(InstallProgress::Installing);
        let out =
            self.shell_blocking(serial, &format!("pm install -r -t \"{remote}\""), timeout)?;
        let mut text = out.stdout_string();
        if !out.stderr.is_empty() {
            text.push_str(&String::from_utf8_lossy(&out.stderr));
        }
        // 清理临时文件（尽力而为）
        let _ = self.shell_blocking(
            serial,
            &format!("rm -f \"{remote}\""),
            Duration::from_secs(10),
        );
        if !text.contains("Success") {
            return Err(Error::ServiceFailed(format!(
                "pm install failed: {}",
                text.trim()
            )));
        }
        Ok(text.trim().to_string())
    }

    /// 安装 APK：push 到临时路径 + `pm install -r`。
    /// 返回 pm 的输出文本；失败时文本中含错误原因。
    pub fn install_apk(&self, serial: &str, apk: &Path, timeout: Duration) -> Result<String> {
        self.install_apk_with_progress(serial, apk, timeout, |_| {})
    }

    /// 授予应用已申请但未授予的运行时权限（模拟器场景标准做法：
    /// 新装应用的 CAMERA/存储等权限默认拒绝，扫码/保存文件等功能静默失效）。
    /// 返回成功授予的数量（个别权限类型不支持 pm grant，失败跳过）。
    pub fn grant_runtime_permissions(&self, serial: &str, package: &str) -> Result<usize> {
        let dump = self.shell_blocking(
            serial,
            &format!("dumpsys package {package}"),
            Duration::from_secs(20),
        )?;
        // runtime permissions 段落形如：`    android.permission.CAMERA: granted=false, flags=...`
        let mut granted = 0usize;
        for line in dump.stdout_string().lines() {
            let trimmed = line.trim();
            let Some(rest) = trimmed.strip_prefix("android.permission.") else {
                continue;
            };
            let Some((perm, state)) = rest.split_once(": ") else {
                continue;
            };
            if !state.starts_with("granted=false") {
                continue;
            }
            let perm = format!("android.permission.{perm}");
            let out = self.shell_blocking(
                serial,
                &format!("pm grant {package} {perm}"),
                Duration::from_secs(10),
            );
            if out.map(|o| o.ok()).unwrap_or(false) {
                granted += 1;
            }
        }
        Ok(granted)
    }

    /// 列出已安装的第三方应用（包名）。
    pub fn list_packages(&self, serial: &str) -> Result<Vec<String>> {
        let out = self.shell_blocking(serial, "pm list packages -3", Duration::from_secs(30))?;
        Ok(out
            .stdout_string()
            .lines()
            .filter_map(|l| l.strip_prefix("package:").map(|s| s.trim().to_string()))
            .collect())
    }

    /// 通过 monkey 启动应用的 launcher 入口（无需知道 Activity 名）。
    pub fn launch_package(&self, serial: &str, package: &str) -> Result<()> {
        let out = self.shell_blocking(
            serial,
            &format!("monkey -p {package} -c android.intent.category.LAUNCHER 1"),
            Duration::from_secs(30),
        )?;
        if out.stdout_string().contains("aborted") || !out.ok() {
            return Err(Error::ServiceFailed(format!(
                "failed to launch {package}: {}{}",
                out.stdout_string(),
                String::from_utf8_lossy(&out.stderr)
            )));
        }
        Ok(())
    }

    /// 建立反向连接（transport 级服务 reverse:forward:）：
    /// 设备 localhost:device_port -> 宿主 tcp:host_port。
    pub fn reverse_tcp(&self, serial: &str, device_port: u16, host_port: u16) -> Result<()> {
        let mut conn = self.transport(serial)?;
        conn.send_service(&format!(
            "reverse:forward:tcp:{device_port};tcp:{host_port}"
        ))
    }

    /// 移除反向连接（transport 级服务）。
    pub fn remove_reverse(&self, serial: &str, device_port: u16) -> Result<()> {
        let mut conn = self.transport(serial)?;
        conn.send_service(&format!("reverse:killforward:tcp:{device_port}"))
    }

    /// 建立端口转发：宿主 tcp:port -> 设备 localabstract:name。
    /// host-serial 形式（显式指定目标设备）：多设备并存（含离线）时裸
    /// host:forward 会以空消息 FAIL（实测两台 emulator 并存时必现）。
    pub fn forward_abstract(
        &self,
        serial: &str,
        tcp_port: u16,
        abstract_name: &str,
    ) -> Result<()> {
        let mut conn = self.connect(Duration::from_secs(10))?;
        conn.send_service(&format!(
            "host-serial:{serial}:forward:tcp:{tcp_port};localabstract:{abstract_name}"
        ))
    }

    /// 建立端口转发：宿主 tcp:host_port -> 设备 tcp:device_port（127.0.0.1）。
    pub fn forward_tcp(&self, serial: &str, host_port: u16, device_port: u16) -> Result<()> {
        let mut conn = self.connect(Duration::from_secs(10))?;
        conn.send_service(&format!(
            "host-serial:{serial}:forward:tcp:{host_port};tcp:{device_port}"
        ))
    }

    /// 删除宿主端口转发（host-serial 形式）。
    pub fn remove_forward(&self, serial: &str, tcp_port: u16) -> Result<()> {
        let mut conn = self.connect(Duration::from_secs(10))?;
        conn.send_service(&format!("host-serial:{serial}:remove-forward:tcp:{tcp_port}"))
    }

    pub fn kill_server(&self) -> Result<()> {
        let mut conn = self.connect(Duration::from_secs(5))?;
        conn.send_service("host:kill-server")
    }
}

/// 长驻 shell 进程句柄：保持连接打开即保持远端进程存活。
pub struct ShellStream {
    stream: TcpStream,
}

impl ShellStream {
    /// 非阻塞地丢弃远端输出（减少缓冲堆积）。
    pub fn drain_available(&mut self) -> Result<usize> {
        let mut buf = [0u8; 4096];
        let mut total = 0;
        self.stream.set_nonblocking(true)?;
        loop {
            match self.stream.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => total += n,
                Err(ref e)
                    if e.kind() == std::io::ErrorKind::WouldBlock
                        || e.kind() == std::io::ErrorKind::Interrupted =>
                {
                    break
                }
                Err(e) => {
                    self.stream.set_nonblocking(false).ok();
                    return Err(e.into());
                }
            }
        }
        self.stream.set_nonblocking(false)?;
        Ok(total)
    }

    /// 主动断开（远端进程将收到 EOF 并退出）。
    pub fn shutdown(&mut self) {
        use std::net::Shutdown;
        self.stream.shutdown(Shutdown::Both).ok();
    }
}

fn find_adb_on_path() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("PATH") {
        for dir in std::env::split_paths(&path) {
            let candidate = dir.join("adb");
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    // 零依赖模式：.app 内嵌运行时的 platform-tools/adb
    if let Ok(exe) = std::env::current_exe() {
        for anc in exe.ancestors().skip(1) {
            let cand = anc.join("Resources/runtime/platform-tools/adb");
            if cand.is_file() {
                return Some(cand);
            }
        }
    }
    None
}
