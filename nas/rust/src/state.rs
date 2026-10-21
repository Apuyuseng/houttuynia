//! 共享状态：投屏会话 / 终端会话 / 任务进度 / 配置路径。

use std::collections::HashMap;
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};

/// 任务进度（与 C++ TaskState JSON 同构，前端轮询契约不变）。
#[derive(Debug, Default)]
pub struct TaskState {
    pub running: AtomicBool,
    pub done: AtomicBool,
    pub error: Mutex<String>,
    pub result: Mutex<String>,
    pub phase: Mutex<String>,
    pub progress: AtomicU64,
    pub progress_total: AtomicU64,
    pub lines: Mutex<Vec<String>>,
    pub package: Mutex<String>,
    pub label: Mutex<String>,
    pub version: Mutex<String>,
}

impl TaskState {
    pub fn try_begin(&self, title: &str) -> bool {
        if self.running.swap(true, Ordering::SeqCst) {
            return false;
        }
        self.done.store(false, Ordering::SeqCst);
        self.progress.store(0, Ordering::SeqCst);
        self.progress_total.store(0, Ordering::SeqCst);
        *self.error.lock().unwrap() = String::new();
        *self.result.lock().unwrap() = String::new();
        *self.phase.lock().unwrap() = title.to_string();
        *self.lines.lock().unwrap() = vec![title.to_string()];
        true
    }
    pub fn set_progress(&self, phase: &str, done: u64, total: u64) {
        *self.phase.lock().unwrap() = phase.to_string();
        self.progress.store(done, Ordering::SeqCst);
        self.progress_total.store(total, Ordering::SeqCst);
    }
    pub fn finish_ok(&self, result: &str) {
        *self.result.lock().unwrap() = result.to_string();
        *self.phase.lock().unwrap() = "完成".into();
        self.running.store(false, Ordering::SeqCst);
        self.done.store(true, Ordering::SeqCst);
    }
    pub fn finish_err(&self, e: &str) {
        *self.error.lock().unwrap() = e.to_string();
        self.running.store(false, Ordering::SeqCst);
    }
    pub fn to_json(&self) -> Value {
        json!({
            "running": self.running.load(Ordering::SeqCst),
            "done": self.done.load(Ordering::SeqCst),
            "error": *self.error.lock().unwrap(),
            "result": *self.result.lock().unwrap(),
            "phase": *self.phase.lock().unwrap(),
            "progress": self.progress.load(Ordering::SeqCst),
            "progress_total": self.progress_total.load(Ordering::SeqCst),
            "lines": *self.lines.lock().unwrap(),
            "package": *self.package.lock().unwrap(),
            "label": *self.label.lock().unwrap(),
            "version": *self.version.lock().unwrap(),
        })
    }
}

/// 投屏会话（scrcpy）：视频由泵线程消费，控制通道共享给注入路由。
pub struct StreamSession {
    pub id: u64,
    pub serial: String,
    pub codec: String,
    pub width: AtomicU64,
    pub height: AtomicU64,
    pub control: Option<Arc<Mutex<TcpStream>>>,
    pub ended: AtomicBool,
    pub err: Mutex<String>,
    /// 视频泵就绪帧队列（receive 端；见 scrcpy::Session 消费线程）
    pub frame_rx: Mutex<Option<tokio::sync::mpsc::Receiver<crate::FrameMsg>>>,
    pub attached: AtomicBool,
    /// 会话存活守卫：析构清理（stop 由持有方共享调用）
    pub stop_fn: Mutex<Option<Box<dyn FnMut() + Send>>>,
    /// 设备剪贴板最新内容（控制通道设备消息读取线程更新）
    pub clipboard: Mutex<String>,
}

impl StreamSession {
    pub fn stop(&self) {
        self.ended.store(true, Ordering::SeqCst);
        if let Some(mut f) = self.stop_fn.lock().unwrap().take() {
            f();
        }
    }
}

/// 终端会话。
pub struct TermSession {
    pub id: u64,
    pub serial: String,
    pub pty: bool,
    pub alive: AtomicBool,
    pub buf: Mutex<Vec<u8>>,
    pub writer: Mutex<TcpStream>,
}

// ===================== 网关配置（~/.mwine/nas.json，与桌面 config.json 分离） =====================

pub fn gwcfg_path() -> String {
    let home = std::env::var("HOME").unwrap_or_default();
    format!("{home}/.mwine/nas.json")
}

pub fn gwcfg_load() -> Value {
    std::fs::read_to_string(gwcfg_path())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_else(|| json!({}))
}

pub fn gwcfg_save(v: &Value) -> std::io::Result<()> {
    let path = gwcfg_path();
    if let Some(dir) = std::path::Path::new(&path).parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, serde_json::to_string_pretty(v)?)
}

/// 运行模式：device（真机，默认）| vm（虚拟机）
pub fn run_mode() -> String {
    gwcfg_load()
        .get("run_mode")
        .and_then(|v| v.as_str())
        .unwrap_or("device")
        .to_string()
}

pub fn set_run_mode(mode: &str) {
    let mut v = gwcfg_load();
    v.as_object_mut()
        .map(|o| o.insert("run_mode".into(), json!(mode)));
    let _ = gwcfg_save(&v);
}

/// 当前在线真机（非模拟器；唯一在线时取之）
pub fn online_real_device() -> Option<String> {
    let ds = crate::adb::devices().ok()?;
    let mut reals: Vec<&crate::adb::Device> = ds
        .iter()
        .filter(|d| d.state == "device" && !d.serial.starts_with("emulator-"))
        .collect();
    // USB 设备 serial 形如 "582QUGHG2277L"；无线 "192.168.x.x:5555"
    if reals.len() == 1 {
        return Some(reals.remove(0).serial.clone());
    }
    None
}

pub struct AppState {
    pub streams: Mutex<HashMap<u64, Arc<StreamSession>>>,
    pub terminals: Mutex<HashMap<u64, Arc<TermSession>>>,
    pub next_id: AtomicU64,
    pub install_task: TaskState,
    pub store_task: TaskState,
    pub image_task: TaskState,
    pub runtime_task: TaskState,
    /// perf 差分采样（guest /proc/stat）
    pub perf_last: Mutex<Option<(u64, u64)>>,
    /// 扫码配对会话
    pub hostpair: crate::devices::HostPairManager,
}

pub type SharedState = Arc<AppState>;

impl AppState {
    pub fn new() -> SharedState {
        Arc::new(AppState {
            streams: Mutex::new(HashMap::new()),
            terminals: Mutex::new(HashMap::new()),
            next_id: AtomicU64::new(1),
            install_task: TaskState::default(),
            store_task: TaskState::default(),
            image_task: TaskState::default(),
            runtime_task: TaskState::default(),
            perf_last: Mutex::new(None),
            hostpair: crate::devices::HostPairManager::new(),
        })
    }

    pub fn alloc_id(&self) -> u64 {
        self.next_id.fetch_add(1, Ordering::SeqCst)
    }

    /// 按运行模式取管理目标：真机模式 → 唯一在线真机；虚拟机模式 → 在线模拟器
    pub fn current_serial(&self) -> Option<String> {
        if run_mode() == "device" {
            online_real_device()
        } else {
            crate::adb::online_emulator()
        }
    }

    pub fn vm_running(&self) -> bool {
        crate::adb::online_emulator().is_some()
    }
}

// ---------------- 配置（~/.mwine/config.json 透传，与桌面版同文件） ----------------

pub fn config_path() -> String {
    let home = std::env::var("HOME").unwrap_or_default();
    format!("{home}/.mwine/config.json")
}

pub fn config_load() -> Value {
    std::fs::read_to_string(config_path())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_else(|| json!({}))
}

pub fn config_save(v: &Value) -> std::io::Result<()> {
    let path = config_path();
    if let Some(dir) = std::path::Path::new(&path).parent() {
        std::fs::create_dir_all(dir)?;
    }
    std::fs::write(path, serde_json::to_string_pretty(v)?)
}

pub fn config_get_str(key: &str) -> Option<String> {
    config_load().get(key)?.as_str().map(String::from)
}

/// AVD 名（默认 MWine4）
pub fn avd_name() -> String {
    config_get_str("avd").unwrap_or_else(|| "MWine4".into())
}

/// SDK 根目录探测（MWINE_SDK_ROOT > ANDROID_HOME > 平台默认路径）。
pub fn sdk_root() -> Option<String> {
    let home = std::env::var("HOME").unwrap_or_default();
    let mut cands: Vec<String> = Vec::new();
    for var in ["MWINE_SDK_ROOT", "ANDROID_HOME", "ANDROID_SDK_ROOT"] {
        if let Ok(v) = std::env::var(var) {
            if !v.is_empty() {
                cands.push(v);
            }
        }
    }
    #[cfg(target_os = "macos")]
    {
        cands.push(format!("{home}/Library/Application Support/MWine/runtime"));
        cands.push(format!("{home}/Library/Android/sdk"));
    }
    #[cfg(not(target_os = "macos"))]
    {
        cands.push(format!("{home}/.local/share/MWine/runtime"));
        cands.push(format!("{home}/Android/Sdk"));
        cands.push("/opt/android-sdk".into());
    }
    cands.into_iter().find(|c| {
        std::path::Path::new(&format!("{c}/emulator/emulator")).exists()
            || std::path::Path::new(&format!("{c}/emulator")).is_dir()
    })
}

/// AVD 目录：ANDROID_AVD_HOME 优先；否则按 <name>.ini 实际所在判定
///（产品 AVD 常在外置数据目录，双主目录历史分裂见 C++ findAvdDir）。
pub fn avd_home() -> String {
    if let Ok(p) = std::env::var("ANDROID_AVD_HOME") {
        if !p.is_empty() {
            return p;
        }
    }
    let home = std::env::var("HOME").unwrap_or_default();
    let name = avd_name();
    #[cfg(target_os = "macos")]
    let bundled = format!("{home}/Library/Application Support/MWine/avd");
    #[cfg(not(target_os = "macos"))]
    let bundled = format!("{home}/.local/share/MWine/avd");
    if std::path::Path::new(&format!("{bundled}/{name}.ini")).exists() {
        return bundled;
    }
    format!("{home}/.android/avd")
}

/// AVD 实体目录（<home>/<name>.avd）。
pub fn avd_dir() -> String {
    format!("{}/{}.avd", avd_home(), avd_name())
}
