//! 启动器：环境组装 + spawn（替代 SDK emulator launcher 的核心职责）。

use std::path::PathBuf;
use std::process::{Command, Stdio};

use crate::config::{QemuConfig, QemuRuntime};
use crate::handle::QemuHandle;
use crate::ports::{pick_port, pick_port_pair};
use crate::{Error, Result};

/// qemu 启动器接口（mock 实现见 [`crate::mock`]）。
pub trait QemuLauncher {
    fn launch(&self, config: &QemuConfig) -> Result<QemuHandle>;
}

/// 直启实现：用实测最小环境集 exec qemu-system-*-headless。
pub struct DirectLauncher {
    pub runtime: QemuRuntime,
    /// qemu stdout/stderr 落盘路径（支持排障）；None 时丢弃。
    pub log_file: Option<PathBuf>,
}

impl DirectLauncher {
    pub fn new(runtime: QemuRuntime) -> Self {
        Self { runtime, log_file: None }
    }

    /// 从本机 Android SDK 布局探测（开发机/CLI 路径）。
    pub fn detect() -> Result<Self> {
        let runtime = QemuRuntime::detect_from_sdk()
            .ok_or_else(|| Error::BinaryNotFound(PathBuf::from("(SDK 未找到 qemu)")))?;
        Ok(Self::new(runtime))
    }
}

impl QemuLauncher for DirectLauncher {
    fn launch(&self, config: &QemuConfig) -> Result<QemuHandle> {
        if config.image_dir.is_none() && config.avd_name.is_empty() {
            return Err(Error::NoTarget);
        }
        self.runtime.validate()?;
        config.validate_image_dir()?;

        let (console_port, adb_port) = match config.ports {
            Some(p) => p,
            None => pick_port_pair(5554).ok_or(Error::PortExhausted)?,
        };
        let grpc_port = match config.grpc_port {
            Some(p) => p,
            None => pick_port(8554).ok_or(Error::PortExhausted)?,
        };

        let mut cmd = Command::new(&self.runtime.qemu_bin);
        cmd.args(config.build_args(console_port, adb_port, grpc_port));
        // 环境集是直启的命门，缺失对应症状见 lib.rs 文档
        cmd.env("DYLD_LIBRARY_PATH", self.runtime.dyld_library_path());
        cmd.env("ANDROID_EMULATOR_LAUNCHER_DIR", &self.runtime.launcher_dir);
        cmd.env("ANDROID_EMULATOR_WRAPPER_PID", std::process::id().to_string());
        if let Some(sdk) = &self.runtime.sdk_root {
            cmd.env("ANDROID_SDK_ROOT", sdk);
        }
        if let Some(avd) = &self.runtime.avd_home {
            cmd.env("ANDROID_AVD_HOME", avd);
        }
        match &self.log_file {
            Some(path) => {
                if let Some(parent) = path.parent() {
                    std::fs::create_dir_all(parent).ok();
                }
                let out = std::fs::File::create(path)
                    .map_err(|e| Error::LaunchFailed(format!("日志文件创建失败: {e}")))?;
                cmd.stdout(out.try_clone().map_err(Error::Io)?).stderr(out);
            }
            None => {
                cmd.stdout(Stdio::null()).stderr(Stdio::null());
            }
        }

        let child = cmd
            .spawn()
            .map_err(|e| Error::LaunchFailed(format!("{}: {e}", self.runtime.qemu_bin.display())))?;

        Ok(QemuHandle::new(child, console_port, grpc_port))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn launch_requires_target() {
        let l = DirectLauncher::new(QemuRuntime::bundle(
            "/nonexistent/qemu".into(),
            "/nonexistent".into(),
            "/nonexistent/lib64".into(),
        ));
        let err = l.launch(&QemuConfig::default()).unwrap_err();
        assert!(matches!(err, Error::NoTarget));
    }

    #[test]
    fn launch_validates_binary_presence_first() {
        let l = DirectLauncher::new(QemuRuntime::bundle(
            "/nonexistent/qemu".into(),
            "/nonexistent".into(),
            "/nonexistent/lib64".into(),
        ));
        let cfg = QemuConfig {
            avd_name: "x".into(),
            ..Default::default()
        };
        let err = l.launch(&cfg).unwrap_err();
        assert!(matches!(err, Error::BinaryNotFound(_)));
    }
}
