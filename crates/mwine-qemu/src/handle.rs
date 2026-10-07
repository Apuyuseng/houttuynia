//! 运行中的 qemu 实例句柄：console 命令、boot 等待、优雅关停。

use std::process::Child;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use mwine_adb::AdbClient;

use crate::console::{console_command, console_token, is_ko};
use crate::{Error, Result};

/// 运行中的 qemu 直启实例。
///
/// `serial` 约定为 `emulator-<console_port>`（与 qemu `-ports c,c+1` 布局一致）。
/// Child 藏在 Mutex 里以维持 `&self` 语义的 `is_running`/`send_console`；
/// 句柄 Drop **不会**杀进程——守护进程场景要求 qemu 超越本进程生命周期。
pub struct QemuHandle {
    pub pid: u32,
    pub console_port: u16,
    pub adb_serial: String,
    pub grpc_port: u16,
    child: Mutex<Option<Child>>,
    token: String,
    adb: AdbClient,
}

impl std::fmt::Debug for QemuHandle {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("QemuHandle")
            .field("pid", &self.pid)
            .field("console_port", &self.console_port)
            .field("adb_serial", &self.adb_serial)
            .field("grpc_port", &self.grpc_port)
            .field("alive", &self.is_running())
            .finish()
    }
}

impl QemuHandle {
    pub(super) fn new(
        child: Child,
        console_port: u16,
        grpc_port: u16,
    ) -> Self {
        let pid = child.id();
        Self {
            pid,
            console_port,
            adb_serial: format!("emulator-{console_port}"),
            grpc_port,
            child: Mutex::new(Some(child)),
            token: console_token(),
            adb: AdbClient::new(),
        }
    }

    /// 无进程句柄（mock / 重连已存在实例用）。pid 置 0 表示非真实进程。
    pub fn detached(console_port: u16, grpc_port: u16) -> Self {
        Self {
            pid: 0,
            console_port,
            adb_serial: format!("emulator-{console_port}"),
            grpc_port,
            child: Mutex::new(None),
            token: console_token(),
            adb: AdbClient::new(),
        }
    }

    /// 执行一条 emulator console 命令（kill/multidisplay/redir/avd/...），
    /// 返回原始应答文本。
    pub fn send_console(&self, cmd: &str) -> Result<String> {
        console_command(self.console_port, &self.token, cmd)
    }

    pub fn is_running(&self) -> bool {
        let mut guard = self.child.lock().expect("qemu child lock");
        match guard.as_mut() {
            Some(child) => child
                .try_wait()
                .map(|s| s.is_none())
                .unwrap_or(false),
            // detached 句柄无从判断，保守视为不在运行
            None => false,
        }
    }

    /// 等待实例出现在 adb 设备列表且状态为 device（M1 验收：adb 可连）。
    /// 自建镜像冷启 guest adbd 上线可能要 1-2 分钟。
    pub fn wait_adb(&self, timeout: Duration) -> Result<()> {
        self.adb.ensure_server(None).ok();
        let deadline = Instant::now() + timeout;
        loop {
            if let Ok(devices) = self.adb.devices() {
                if devices
                    .iter()
                    .any(|d| d.serial == self.adb_serial && d.state == "device")
                {
                    return Ok(());
                }
            }
            if Instant::now() > deadline {
                return Err(Error::SerialTimeout(self.adb_serial.clone()));
            }
            std::thread::sleep(Duration::from_millis(800));
        }
    }

    /// 等待系统完成引导（sys.boot_completed=1 且 bootanim stopped）。
    /// 内部先等 adb 可连（共享超时预算）。
    pub fn wait_boot(&self, timeout: Duration) -> Result<()> {
        let start = Instant::now();
        self.wait_adb(timeout)?;
        let remain = timeout.checked_sub(start.elapsed()).unwrap_or(Duration::from_secs(1));
        let deadline = Instant::now() + remain;
        loop {
            if let Ok(out) =
                self.adb
                    .shell_blocking(&self.adb_serial, "getprop sys.boot_completed", Duration::from_secs(5))
            {
                if out.stdout_string().trim() == "1" {
                    let anim = self.adb.shell_blocking(
                        &self.adb_serial,
                        "getprop init.svc.bootanim",
                        Duration::from_secs(5),
                    );
                    let stopped = anim
                        .map(|a| a.stdout_string().trim() == "stopped")
                        .unwrap_or(true);
                    if stopped {
                        return Ok(());
                    }
                }
            }
            if Instant::now() > deadline {
                return Err(Error::BootTimeout);
            }
            std::thread::sleep(Duration::from_millis(1000));
        }
    }

    /// 为应用创建独立显示器（每显示器 = 一个宿主渲染目标）。
    /// `multidisplay add` 是 upsert 语义——同 id 重复调用即缩放。
    pub fn multidisplay_add(&self, id: u32, w: u32, h: u32, dpi: u32) -> Result<()> {
        let resp = self.send_console(&format!("multidisplay add {id} {w} {h} {dpi} 0"))?;
        if is_ko(&resp) {
            return Err(Error::Console(format!("multidisplay add 被拒: {}", resp.trim())));
        }
        Ok(())
    }

    /// 移除显示器。
    pub fn multidisplay_del(&self, id: u32) -> Result<()> {
        let resp = self.send_console(&format!("multidisplay del {id}"))?;
        if is_ko(&resp) {
            return Err(Error::Console(format!("multidisplay del 被拒: {}", resp.trim())));
        }
        Ok(())
    }

    /// 优雅关停：console kill → 20s 等待退出 → SIGKILL 兜底。
    pub fn shutdown(&self) -> Result<()> {
        let _ = self.send_console("kill");
        let deadline = Instant::now() + Duration::from_secs(20);
        while Instant::now() < deadline {
            {
                let mut guard = self.child.lock().expect("qemu child lock");
                if let Some(child) = guard.as_mut() {
                    if child.try_wait()?.is_some() {
                        return Ok(());
                    }
                } else {
                    return Ok(());
                }
            }
            std::thread::sleep(Duration::from_millis(500));
        }
        let mut guard = self.child.lock().expect("qemu child lock");
        if let Some(child) = guard.as_mut() {
            child.kill()?;
            let _ = child.wait();
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serial_derives_from_console_port() {
        let h = QemuHandle::detached(5556, 8556);
        assert_eq!(h.adb_serial, "emulator-5556");
        assert_eq!(h.grpc_port, 8556);
    }

    #[test]
    fn detached_handle_reports_not_running() {
        let h = QemuHandle::detached(5554, 8554);
        assert!(!h.is_running());
    }

    #[test]
    fn shutdown_on_detached_handle_is_noop_ok() {
        let h = QemuHandle::detached(5554, 8554);
        h.shutdown().expect("无进程句柄关停应为立即成功");
    }
}
