//! mwine-qemu：qemu headless 进程管理（替代 emulator launcher）
//!
//! 职责：
//! 1. 设置环境变量（ANDROID_SDK_ROOT/ANDROID_AVD_HOME/DYLD_LIBRARY_PATH）
//! 2. 以 -grpc <port> 启动 qemu（免认证 gRPC）
//! 3. 进程生命周期管理（启动/等待引导/停止/健康检查）

use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Debug, Clone)]
pub struct QemuConfig {
    pub avd_name: String,
    pub runtime_dir: PathBuf,
    pub avd_home: PathBuf,
    pub memory_mb: u32,
    pub cores: u32,
    pub grpc_port: u16,
    pub snapshot: Option<String>,
    pub extra_args: Vec<String>,
}

impl Default for QemuConfig {
    fn default() -> Self {
        Self {
            avd_name: "MWine".into(),
            runtime_dir: PathBuf::from("/tmp/mwine-runtime"),
            avd_home: PathBuf::from("/tmp/mwine-avd"),
            memory_mb: 1536,
            cores: 4,
            grpc_port: 8555,
            snapshot: None,
            extra_args: vec![],
        }
    }
}

pub struct QemuHandle {
    pub pid: u32,
    pub grpc_port: u16,
    pub adb_serial: String,
    child: Child,
}

#[derive(Debug, thiserror::Error)]
pub enum QemuError {
    #[error("qemu 二进制不存在: {0}")]
    BinaryNotFound(PathBuf),
    #[error("镜像目录缺失: {0}")]
    ImageDirNotFound(PathBuf),
    #[error("AVD 不存在: {0}")]
    AvdNotFound(String),
    #[error("启动失败: {0}")]
    LaunchFailed(String),
    #[error("引导超时（{timeout}s）")]
    BootTimeout { timeout: u64 },
    #[error("进程异常退出")]
    ProcessExited,
}

impl QemuHandle {
    pub fn wait_boot(&self, adb: &mwine_adb::AdbClient, timeout: Duration) -> Result<(), QemuError> {
        let deadline = Instant::now() + timeout;
        loop {
            if !self.is_running() {
                return Err(QemuError::ProcessExited);
            }
            if let Ok(out) = adb.shell_blocking(
                &self.adb_serial,
                "getprop sys.boot_completed",
                Duration::from_secs(5),
            ) {
                if String::from_utf8_lossy(&out.stdout).trim() == "1" {
                    return Ok(());
                }
            }
            if Instant::now() > deadline {
                return Err(QemuError::BootTimeout { timeout: timeout.as_secs() });
            }
            std::thread::sleep(Duration::from_secs(2));
        }
    }

    pub fn is_running(&self) -> bool {
        // kill(pid, 0) 不发信号，仅检查进程存在
        unsafe { libc::kill(self.pid as i32, 0) == 0 }
    }

    pub fn shutdown(&mut self) -> Result<(), QemuError> {
        unsafe { libc::kill(self.pid as i32, libc::SIGTERM); }
        std::thread::sleep(Duration::from_secs(2));
        unsafe { libc::kill(self.pid as i32, libc::SIGKILL); }
        let _ = self.child.wait();
        Ok(())
    }
}

pub fn launch(config: &QemuConfig, adb: &mwine_adb::AdbClient) -> Result<QemuHandle, QemuError> {
    let qemu_bin = config.runtime_dir
        .join("emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless");
    if !qemu_bin.is_file() {
        return Err(QemuError::BinaryNotFound(qemu_bin));
    }

    let avd_ini = config.avd_home.join(format!("{}.ini", config.avd_name));
    if !avd_ini.is_file() {
        return Err(QemuError::AvdNotFound(config.avd_name.clone()));
    }

    let before: Vec<String> = adb.devices()
        .unwrap_or_default()
        .into_iter()
        .map(|d| d.serial)
        .collect();

    let emu_bin = config.runtime_dir.join("emulator/emulator");
    let mut cmd = Command::new(&emu_bin);
    cmd.env("ANDROID_SDK_ROOT", &config.runtime_dir)
        .env("ANDROID_AVD_HOME", &config.avd_home)
        .arg("-avd").arg(&config.avd_name)
        .arg("-no-window")
        .arg("-no-audio")
        .arg("-grpc").arg(config.grpc_port.to_string());
    // NOTE: -memory 和 -cores 由 emulator launcher 自行处理

    match &config.snapshot {
        Some(snap) => { cmd.arg("-snapshot").arg(snap); }
        None => { cmd.arg("-no-snapshot"); }
    }

    for arg in &config.extra_args {
        cmd.arg(arg);
    }

    cmd.stdout(Stdio::null()).stderr(Stdio::piped());

    let child = cmd.spawn()
        .map_err(|e| QemuError::LaunchFailed(format!("{}: {e}", qemu_bin.display())))?;

    let pid = child.id();

    let deadline = Instant::now() + Duration::from_secs(120);
    let serial = loop {
        if let Ok(devices) = adb.devices() {
            if let Some(new) = devices.iter().find(|d| !before.contains(&d.serial)) {
                break new.serial.clone();
            }
        }
        if Instant::now() > deadline {
            return Err(QemuError::LaunchFailed("120s 内未出现 adb 设备".into()));
        }
        std::thread::sleep(Duration::from_millis(500));
    };

    Ok(QemuHandle {
        pid,
        grpc_port: config.grpc_port,
        adb_serial: serial,
        child,
    })
}

pub fn first_run_setup(runtime_dir: &Path, avd_name: &str) -> Result<(), QemuError> {
    let avd_home = runtime_dir.join("avd");
    let avd_dir = avd_home.join(format!("{}.avd", avd_name));
    std::fs::create_dir_all(&avd_dir)
        .map_err(|e| QemuError::LaunchFailed(format!("创建 AVD 目录: {e}")))?;

    let config = format!(
        "avd.ini.encoding=UTF-8\n\
         image.sysdir.1 = system-images/android-33/mwine/arm64-v8a/\n\
         tag.id = mwine\n\
         abi.type = arm64-v8a\n\
         hw.cpu.arch = arm64\n\
         hw.cpu.ncore = 4\n\
         hw.ramSize = 1536\n\
         hw.lcd.density = 420\n\
         hw.lcd.width = 720\n\
         hw.lcd.height = 1280\n\
         hw.gpu.enabled = yes\n\
         hw.gpu.mode = auto\n\
         disk.dataPartition.size = 6442450944\n"
    );
    std::fs::write(avd_dir.join("config.ini"), config)
        .map_err(|e| QemuError::LaunchFailed(format!("写 config.ini: {e}")))?;

    let ini = format!(
        "avd.ini.encoding=UTF-8\npath={}\ntarget=android-33\n",
        avd_dir.display()
    );
    std::fs::write(avd_home.join(format!("{}.ini", avd_name)), ini)
        .map_err(|e| QemuError::LaunchFailed(format!("写 AVD ini: {e}")))?;

    Ok(())
}
