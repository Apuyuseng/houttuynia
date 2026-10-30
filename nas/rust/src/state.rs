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
    /// 最后一次视频帧到达时间（毫秒时间戳）——空闲回收看门狗用
    pub last_frame_ms: AtomicU64,
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

/// 手动连接过的无线设备（回连记忆；nas.json "known_devices"）
pub fn known_devices() -> Vec<String> {
    gwcfg_load()
        .get("known_devices")
        .and_then(|v| v.as_array())
        .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect())
        .unwrap_or_default()
}

/// 从历史连接记忆中移除（用户主动遗忘）
pub fn forget_device(serial: &str) {
    let mut v = gwcfg_load();
    let list: Vec<String> = v
        .get("known_devices")
        .and_then(|x| x.as_array())
        .map(|a| a.iter().filter_map(|s| s.as_str().map(String::from)).collect())
        .unwrap_or_default();
    let kept: Vec<String> = list.into_iter().filter(|s| s != serial).collect();
    if let Some(o) = v.as_object_mut() {
        o.insert("known_devices".into(), json!(kept));
    }
    let _ = gwcfg_save(&v);
}

pub fn remember_device(serial: &str) {
    if serial.is_empty() {
        return;
    }
    let mut v = gwcfg_load();
    let mut list: Vec<String> = v
        .get("known_devices")
        .and_then(|x| x.as_array())
        .map(|a| a.iter().filter_map(|s| s.as_str().map(String::from)).collect())
        .unwrap_or_default();
    if !list.iter().any(|s| s == serial) {
        list.push(serial.to_string());
        if let Some(o) = v.as_object_mut() {
            o.insert("known_devices".into(), json!(list));
        }
        let _ = gwcfg_save(&v);
    }
}

/// 用户选定的管理目标设备（真机模式多设备在线时用；nas.json "device_serial"）
pub fn selected_serial() -> Option<String> {
    gwcfg_load()
        .get("device_serial")
        .and_then(|v| v.as_str())
        .filter(|s| !s.is_empty())
        .map(String::from)
}

pub fn set_selected_serial(serial: &str) {
    let mut v = gwcfg_load();
    v.as_object_mut()
        .map(|o| o.insert("device_serial".into(), json!(serial)));
    let _ = gwcfg_save(&v);
}

/// 在线真机列表（非模拟器、state=device）
pub fn online_real_devices() -> Vec<String> {
    crate::adb::devices()
        .map(|ds| {
            ds.iter()
                .filter(|d| d.state == "device" && !d.serial.starts_with("emulator-"))
                .map(|d| d.serial.clone())
                .collect()
        })
        .unwrap_or_default()
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

    /// 管理目标设备：选定设备在线 → 用之；否则第一台在线真机兜底（前端可切换）
    pub fn current_serial(&self) -> Option<String> {
        let reals = online_real_devices();
        if let Some(sel) = selected_serial() {
            if reals.iter().any(|s| s == &sel) {
                return Some(sel);
            }
        }
        reals.into_iter().next()
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

/// 第一台在线真机（device_op 兜底用）
pub fn online_real_devices_first() -> Option<String> {
    online_real_devices().into_iter().next()
}
