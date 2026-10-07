//! 测试与演示用 mock 启动器（测试用 mock 实现）。
//!
//! 供 mwine-display（M2）/ mwine-input（M3）等下游在无真机时开发联调：
//! 返回预设端口的 detached 句柄，console 命令由闭包应答。

use crate::config::QemuConfig;
use crate::handle::QemuHandle;
use crate::{Error, QemuLauncher, Result};

/// mock 启动器：不 spawn 任何进程。
pub struct MockLauncher {
    /// 预设返回的 (console_port, grpc_port)。
    pub ports: (u16, u16),
    /// 捕获到的最近一次启动配置（断言用）。
    pub last_config: std::sync::Mutex<Option<QemuConfig>>,
    /// console 命令应答函数（默认回 OK）。
    pub console_responder: Box<dyn Fn(&str) -> String + Send + Sync>,
}

impl Default for MockLauncher {
    fn default() -> Self {
        Self {
            ports: (5554, 8554),
            last_config: std::sync::Mutex::new(None),
            console_responder: Box::new(|_| "OK\r\n".into()),
        }
    }
}

impl MockLauncher {
    pub fn with_ports(console: u16, grpc: u16) -> Self {
        Self {
            ports: (console, grpc),
            ..Default::default()
        }
    }
}

impl QemuLauncher for MockLauncher {
    fn launch(&self, config: &QemuConfig) -> Result<QemuHandle> {
        if config.image_dir.is_none() && config.avd_name.is_empty() {
            return Err(Error::NoTarget);
        }
        *self.last_config.lock().unwrap() = Some(config.clone());
        Ok(QemuHandle::detached(self.ports.0, self.ports.1))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mock_launch_returns_preset_ports() {
        let m = MockLauncher::with_ports(5558, 8558);
        let h = m
            .launch(&QemuConfig {
                avd_name: "T".into(),
                ..Default::default()
            })
            .expect("mock 启动应成功");
        assert_eq!(h.console_port, 5558);
        assert_eq!(h.grpc_port, 8558);
        assert_eq!(h.adb_serial, "emulator-5558");
        assert!(!h.is_running(), "mock 句柄无进程");
    }

    #[test]
    fn mock_captures_config_and_rejects_empty_target() {
        let m = MockLauncher::default();
        assert!(matches!(m.launch(&QemuConfig::default()), Err(Error::NoTarget)));
        let cfg = QemuConfig {
            avd_name: "Cap".into(),
            memory_mb: 2048,
            ..Default::default()
        };
        m.launch(&cfg).unwrap();
        let captured = m.last_config.lock().unwrap().clone().unwrap();
        assert_eq!(captured.avd_name, "Cap");
        assert_eq!(captured.memory_mb, 2048);
    }
}
