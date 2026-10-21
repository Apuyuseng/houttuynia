//! REST 路由：与 C++ 守护进程同契约（web 前端双后端通用）。

use std::io::Write;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};

use axum::body::Bytes;
use axum::extract::{Path, Query, State};
use axum::http::StatusCode;
use axum::Json;
use serde_json::{json, Value};

use crate::adb;
use crate::console;
use crate::scrcpy;
use crate::state::{self, AppState, SharedState, StreamSession, TermSession};

pub fn err(status: u16, msg: impl Into<String>) -> (StatusCode, Json<Value>) {
    (
        StatusCode::from_u16(status).unwrap_or(StatusCode::INTERNAL_SERVER_ERROR),
        Json(json!({ "error": msg.into() })),
    )
}

fn ok(v: Value) -> (StatusCode, Json<Value>) {
    (StatusCode::OK, Json(v))
}

fn not_impl(name: &str) -> (StatusCode, Json<Value>) {
    err(
        501,
        format!("Rust 网关暂未实现「{name}」——请用桌面客户端或等待后续版本"),
    )
}

fn require_vm(state: &AppState) -> Result<String, (StatusCode, Json<Value>)> {
    state
        .current_serial()
        .ok_or_else(|| err(503, "Android 未运行"))
}

// ===================== 基础 =====================

pub async fn status(State(st): State<SharedState>) -> Json<Value> {
    let serial = st.current_serial();
    Json(json!({
        "vm": { "running": serial.is_some(), "serial": serial },
        "error": "",
        "version": "0.1.0-rust",
        "backend": "rust",
    }))
}

pub async fn vm_start(State(st): State<SharedState>) -> (StatusCode, Json<Value>) {
    if st.current_serial().is_some() {
        return ok(json!({ "ok": true, "already": true }));
    }
    let r = tokio::task::spawn_blocking(move || -> Result<(), String> {
        let sdk = state::sdk_root().ok_or("SDK 探测失败（未找到 emulator）")?;
        let exe = format!("{sdk}/emulator/emulator");
        let avd = state::avd_name();
        let cfg = state::config_load();
        let mut args: Vec<String> = vec!["-avd".into(), avd.clone()];
        // 无头判定：显式环境变量或 Linux 无显示服务器
        let headless = std::env::var("MWINE_HEADLESS").is_ok()
            || (cfg!(target_os = "linux")
                && std::env::var("DISPLAY").is_err()
                && std::env::var("WAYLAND_DISPLAY").is_err());
        if headless {
            args.push("-no-window".into());
        }
        args.push("-no-boot-anim".into());
        if let Some(gpu) = cfg.get("gpu_mode").and_then(|v| v.as_str()) {
            if !gpu.is_empty() {
                args.push("-gpu".into());
                args.push(gpu.to_string());
            }
        }
        if let Some(mem) = cfg.get("memory_mb").and_then(|v| v.as_u64()) {
            if mem > 0 {
                args.push("-memory".into());
                args.push(mem.to_string());
            }
        }
        if let Some(proxy) = cfg.get("proxy").and_then(|v| v.as_str()) {
            if !proxy.is_empty() {
                args.push("-http-proxy".into());
                args.push(proxy.to_string());
            }
        }
        // 日志落文件，脱离父进程生命周期
        let home = std::env::var("HOME").unwrap_or_default();
        let logdir = format!("{home}/.mwine/logs");
        let _ = std::fs::create_dir_all(&logdir);
        let logf = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(format!("{logdir}/vm-rust.log"))
            .map_err(|e| format!("日志文件: {e}"))?;
        let logf2 = logf.try_clone().map_err(|e| format!("日志文件: {e}"))?;
        let mut cmd = std::process::Command::new(exe);
        cmd.args(&args)
            .stdout(logf)
            .stderr(logf2)
            // AVD 可能在外置数据目录（与 C++ resolveActiveAvd 同口径）
            .env("ANDROID_AVD_HOME", state::avd_home());
        cmd.spawn().map_err(|e| format!("emulator 启动失败: {e}"))?;
        // 等 adb 上线（冷启可达分钟级）
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(180);
        while std::time::Instant::now() < deadline {
            let _ = adb::ensure_server();
            if adb::online_emulator().is_some() {
                return Ok(());
            }
            std::thread::sleep(std::time::Duration::from_secs(2));
        }
        Err("等待虚拟机上线超时（180s）".into())
    })
    .await;
    match r {
        Ok(Ok(())) => ok(json!({ "ok": true })),
        Ok(Err(e)) => err(500, e),
        Err(e) => err(500, format!("启动任务失败: {e}")),
    }
}

pub async fn vm_stop(State(st): State<SharedState>) -> (StatusCode, Json<Value>) {
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return ok(json!({ "ok": true, "already": false, "note": "未在运行" })),
    };
    let r = tokio::task::spawn_blocking(move || -> Result<(), String> {
        // 存档 → kill（与桌面版 vm stop 同语义）
        let _ = console::cmd(&serial, "avd snapshot save mwine_default", true);
        let _ = console::cmd(&serial, "kill", false);
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(60);
        while std::time::Instant::now() < deadline {
            if adb::online_emulator().is_none() {
                return Ok(());
            }
            std::thread::sleep(std::time::Duration::from_secs(1));
        }
        Err("等待虚拟机退出超时".into())
    })
    .await;
    match r {
        Ok(Ok(())) => ok(json!({ "ok": true })),
        Ok(Err(e)) => err(500, e),
        Err(e) => err(500, format!("停止任务失败: {e}")),
    }
}

// ===================== 性能 =====================

pub async fn perf(State(st): State<SharedState>) -> (StatusCode, Json<Value>) {
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return ok(json!({ "running": false })),
    };
    let r = tokio::task::spawn_blocking(move || -> Result<Value, String> {
        let out = adb::shell_blocking(
            &serial,
            "head -1 /proc/stat; head -2 /proc/meminfo; cat /proc/uptime",
            5000,
        )
        .map_err(|e| e.to_string())?;
        let text = String::from_utf8_lossy(&out);
        let lines: Vec<&str> = text.lines().collect();
        if lines.len() < 4 {
            return Err("性能数据不完整".into());
        }
        let fields: Vec<u64> = lines[0]
            .split_whitespace()
            .filter_map(|t| t.parse::<u64>().ok())
            .collect();
        if fields.len() < 4 {
            return Err("cpu 行解析失败".into());
        }
        let total: u64 = fields.iter().sum();
        let idle = fields[3] + fields.get(4).copied().unwrap_or(0);
        let busy = total - idle;
        let mem_total_kb: u64 = lines[1]
            .split_whitespace()
            .nth(1)
            .and_then(|t| t.parse().ok())
            .unwrap_or(0);
        // MemAvailable 单独查（老镜像第三行不一定是 MemFree/Available 次序一致）
        let avail_out = adb::shell_blocking(
            &serial,
            "awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo",
            3000,
        )
        .map_err(|e| e.to_string())?;
        let avail_mb: u64 = String::from_utf8_lossy(&avail_out)
            .trim()
            .parse()
            .unwrap_or(0);
        let uptime: f64 = lines[3]
            .split_whitespace()
            .next()
            .and_then(|t| t.parse().ok())
            .unwrap_or(0.0);
        Ok(json!({
            "busy": busy, "total": total,
            "mem": { "total_mb": mem_total_kb / 1024, "available_mb": avail_mb },
            "uptime_s": uptime as u64,
        }))
    })
    .await;
    match r {
        Ok(Ok(v)) => {
            // cpu% 两次采样差分
            let busy = v.get("busy").and_then(|x| x.as_u64()).unwrap_or(0);
            let total = v.get("total").and_then(|x| x.as_u64()).unwrap_or(0);
            let mut last = st.perf_last.lock().unwrap();
            let cpu_pct = match *last {
                Some((lb, lt)) if total > lt && busy >= lb => {
                    (busy - lb) as f64 * 100.0 / (total - lt) as f64
                }
                _ => 0.0,
            };
            *last = Some((busy, total));
            let mut out = v;
            out.as_object_mut().map(|o| {
                o.remove("busy");
                o.remove("total");
                o.insert("cpu_pct".into(), json!(cpu_pct));
                o.insert("running".into(), json!(true));
            });
            ok(out)
        }
        Ok(Err(e)) => err(500, e),
        Err(e) => err(500, format!("perf 任务失败: {e}")),
    }
}

// ===================== 应用 =====================

pub async fn apps(State(st): State<SharedState>) -> (StatusCode, Json<Value>) {
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let r = tokio::task::spawn_blocking(move || -> Result<Value, String> {
        let out = adb::shell_blocking(
            &serial,
            "pm query-activities --brief -a android.intent.action.MAIN \
             -c android.intent.category.LAUNCHER 2>/dev/null | grep '/' | \
             sed 's|.* ||;s|/.*||' | sort -u",
            10000,
        )
        .map_err(|e| e.to_string())?;
        let text = String::from_utf8_lossy(&out);
        let apps: Vec<Value> = text
            .lines()
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(|pkg| {
                json!({
                    "package": pkg,
                    "name": pkg.rsplit('.').next().unwrap_or(pkg),
                    "icon": Value::Null,
                    "running": false,
                    "mode": "emulator",
                })
            })
            .collect();
        Ok(json!({ "apps": apps }))
    })
    .await;
    match r {
        Ok(Ok(v)) => ok(v),
        Ok(Err(e)) => err(500, e),
        Err(e) => err(500, format!("应用列表失败: {e}")),
    }
}

pub async fn app_run(
    State(st): State<SharedState>,
    Path(pkg): Path<String>,
) -> (StatusCode, Json<Value>) {
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let r = tokio::task::spawn_blocking(move || -> Result<(), String> {
        adb::shell_blocking(
            &serial,
            &format!("monkey --pct-syskeys 0 -p {pkg} -c android.intent.category.LAUNCHER 1"),
            15000,
        )
        .map(|_| ())
        .map_err(|e| e.to_string())
    })
    .await;
    match r {
        Ok(Ok(())) => ok(json!({ "ok": true })),
        Ok(Err(e)) => err(500, e),
        Err(e) => err(500, format!("启动失败: {e}")),
    }
}

pub async fn app_uninstall(
    State(st): State<SharedState>,
    Path(pkg): Path<String>,
) -> (StatusCode, Json<Value>) {
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let r = tokio::task::spawn_blocking(move || -> Result<Value, String> {
        let out = adb::shell_blocking(&serial, &format!("pm uninstall -k {pkg}"), 30000)
            .map_err(|e| e.to_string())?;
        let text = String::from_utf8_lossy(&out).trim().to_string();
        Ok(json!({ "ok": text.contains("Success"), "output": text, "package": pkg }))
    })
    .await;
    match r {
        Ok(Ok(v)) => ok(v),
        Ok(Err(e)) => err(500, e),
        Err(e) => err(500, format!("卸载失败: {e}")),
    }
}

/// POST /api/apps/install-upload?name=x.apk —— 原始字节上传安装
pub async fn install_upload(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
    body: Bytes,
) -> (StatusCode, Json<Value>) {
    if body.len() > 512 * 1024 * 1024 {
        return err(413, "APK 超过 512MB 上限");
    }
    if body.len() < 4 || &body[..4] != b"PK\x03\x04" {
        return err(400, "不是有效的 APK（zip）文件");
    }
    let mut name = q.get("name").cloned().unwrap_or_else(|| "upload.apk".into());
    name = name.rsplit(['/', '\\']).next().unwrap_or("upload.apk").to_string();
    if !name.to_lowercase().ends_with(".apk") {
        name.push_str(".apk");
    }
    if !st.install_task.try_begin(&format!("安装 {name}")) {
        return err(409, "已有安装任务在进行中");
    }
    let home = std::env::var("HOME").unwrap_or_default();
    let dir = format!("{home}/.mwine/uploads");
    let _ = std::fs::create_dir_all(&dir);
    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let path = format!("{dir}/{ts}-{name}");
    if let Err(e) = std::fs::write(&path, &body) {
        st.install_task.finish_err(&format!("落盘失败: {e}"));
        return err(500, format!("落盘失败: {e}"));
    }
    let _task = &st.install_task;
    let total = body.len() as u64;
    let st2 = st.clone();
    let result = tokio::task::spawn_blocking(move || -> Result<(), String> {
        let task = &st2.install_task;
        let serial = adb::online_emulator().ok_or("虚拟机未运行")?;
        task.set_progress("传输 APK", 0, total);
        let remote = format!("/data/local/tmp/mwine-upload-{ts}.apk");
        adb::push_bytes(&serial, &std::fs::read(&path).map_err(|e| e.to_string())?, &remote)
            .map_err(|e| e.to_string())?;
        task.set_progress("设备安装中", total, total);
        let out = adb::shell_blocking(&serial, &format!("pm install -r '{remote}'"), 600000)
            .map_err(|e| e.to_string())?;
        let _ = adb::shell_blocking(&serial, &format!("rm -f '{remote}'"), 10000);
        let _ = std::fs::remove_file(&path);
        let text = String::from_utf8_lossy(&out);
        if text.contains("Success") {
            Ok(())
        } else {
            Err(format!("安装失败: {}", text.trim()))
        }
    })
    .await;
    let task = &st.install_task;
    match result {
        Ok(Ok(())) => {
            task.finish_ok(&name);
            ok(json!({ "ok": true }))
        }
        Ok(Err(e)) => {
            task.finish_err(&e);
            err(500, e)
        }
        Err(e) => {
            task.finish_err(&e.to_string());
            err(500, format!("安装任务失败: {e}"))
        }
    }
}

pub async fn install_task(State(st): State<SharedState>) -> Json<Value> {
    Json(st.install_task.to_json())
}

pub async fn wallpaper_png(State(st): State<SharedState>) -> axum::response::Response {
    use axum::response::IntoResponse;
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行").into_response(),
    };
    let r = tokio::task::spawn_blocking(move || crate::icons::wallpaper(&serial)).await;
    match r {
        Ok(Ok(bytes)) => (
            [
                (axum::http::header::CONTENT_TYPE, "image/png"),
                (axum::http::header::CACHE_CONTROL, "public, max-age=60"),
            ],
            bytes,
        )
            .into_response(),
        Ok(Err(e)) => err(404, e.to_string()).into_response(),
        Err(e) => err(500, format!("壁纸任务失败: {e}")).into_response(),
    }
}

pub async fn app_icon(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
) -> axum::response::Response {
    use axum::response::IntoResponse;
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行").into_response(),
    };
    let pkg = q.get("pkg").cloned().unwrap_or_default();
    if pkg.is_empty() || !pkg.chars().all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '_') {
        return err(400, "包名非法").into_response();
    }
    let r = tokio::task::spawn_blocking(move || crate::icons::app_icon(&serial, &pkg)).await;
    match r {
        Ok(Ok((bytes, mime))) => (
            [
                (axum::http::header::CONTENT_TYPE, mime),
                (axum::http::header::CACHE_CONTROL, "public, max-age=300"),
            ],
            bytes,
        )
            .into_response(),
        Ok(Err(e)) => err(404, e.to_string()).into_response(),
        Err(e) => err(500, format!("图标任务失败: {e}")).into_response(),
    }
}

// ===================== 截图 =====================

pub async fn screenshot_png(State(st): State<SharedState>) -> axum::response::Response {
    use axum::response::IntoResponse;
    let serial = match st.current_serial() {
        Some(s) => s,
        None => {
            return (
                StatusCode::SERVICE_UNAVAILABLE,
                Json(json!({ "error": "Android 未运行" })),
            )
                .into_response()
        }
    };
    let r = tokio::task::spawn_blocking(move || adb::shell_blocking(&serial, "screencap -p", 15000))
        .await;
    match r {
        Ok(Ok(bytes)) if bytes.len() > 8 && &bytes[..4] == b"\x89PNG" => (
            [
                (axum::http::header::CONTENT_TYPE, "image/png"),
                (axum::http::header::CACHE_CONTROL, "no-store"),
            ],
            bytes,
        )
            .into_response(),
        Ok(Ok(_)) => err(500, "截图失败（非 PNG）").into_response(),
        Ok(Err(e)) => err(500, e.to_string()).into_response(),
        Err(e) => err(500, format!("截图任务失败: {e}")).into_response(),
    }
}

// ===================== 配置 =====================

pub async fn config_get() -> Json<Value> {
    Json(state::config_load())
}

pub async fn config_set(Json(v): Json<Value>) -> (StatusCode, Json<Value>) {
    if !v.is_object() {
        return err(400, "配置须为 JSON 对象");
    }
    match state::config_save(&v) {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => err(500, format!("保存失败: {e}")),
    }
}

// ===================== 快照 =====================

fn snapshots_dir() -> Option<String> {
    let dir = format!("{}/snapshots", state::avd_dir());
    std::path::Path::new(&dir).is_dir().then_some(dir)
}

pub async fn snapshots() -> (StatusCode, Json<Value>) {
    let Some(dir) = snapshots_dir() else {
        return ok(json!({ "snapshots": [] }));
    };
    let mut arr = Vec::new();
    if let Ok(rd) = std::fs::read_dir(&dir) {
        for e in rd.flatten() {
            if e.path().is_dir() {
                let mtime = e
                    .metadata()
                    .and_then(|m| m.modified())
                    .ok()
                    .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                    .map(|d| d.as_millis() as u64)
                    .unwrap_or(0);
                arr.push(json!({
                    "name": e.file_name().to_string_lossy(),
                    "mtime_ms": mtime,
                }));
            }
        }
    }
    arr.sort_by_key(|v| v.get("mtime_ms").and_then(|x| x.as_u64()).unwrap_or(0));
    arr.reverse();
    ok(json!({ "snapshots": arr }))
}

fn snapshot_name_ok(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 48
        && name
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

pub async fn snapshot_save(
    State(st): State<SharedState>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let name = v.get("name").and_then(|x| x.as_str()).unwrap_or("").trim();
    if !snapshot_name_ok(name) {
        return err(400, "存档名须为 1–48 位字母/数字/_/-");
    }
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let cmd = format!("avd snapshot save {name}");
    let r = tokio::task::spawn_blocking(move || console::cmd(&serial, &cmd, true)).await;
    match r {
        Ok(Ok(_)) => ok(json!({ "ok": true })),
        Ok(Err(e)) => err(500, e.to_string()),
        Err(e) => err(500, format!("存档失败: {e}")),
    }
}

pub async fn snapshot_load(
    State(st): State<SharedState>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let name = v.get("name").and_then(|x| x.as_str()).unwrap_or("").trim();
    if !snapshot_name_ok(name) {
        return err(400, "存档名非法");
    }
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let cmd = format!("avd snapshot load {name}");
    let r = tokio::task::spawn_blocking(move || console::cmd(&serial, &cmd, true)).await;
    match r {
        Ok(Ok(_)) => ok(json!({ "ok": true })),
        Ok(Err(e)) => err(500, e.to_string()),
        Err(e) => err(500, format!("恢复失败: {e}")),
    }
}

pub async fn snapshot_delete(
    State(st): State<SharedState>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let name = v.get("name").and_then(|x| x.as_str()).unwrap_or("").trim();
    if !snapshot_name_ok(name) {
        return err(400, "存档名非法");
    }
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let cmd = format!("avd snapshot delete {name}");
    let r = tokio::task::spawn_blocking(move || console::cmd(&serial, &cmd, true)).await;
    match r {
        Ok(Ok(_)) => ok(json!({ "ok": true })),
        Ok(Err(e)) => err(500, e.to_string()),
        Err(e) => err(500, format!("删除失败: {e}")),
    }
}

// ===================== 镜像 =====================

fn android_version_name(api: &str) -> Option<&'static str> {
    Some(match api {
        "android-21" => "Android 5.0",
        "android-22" => "Android 5.1",
        "android-23" => "Android 6.0",
        "android-24" => "Android 7.0",
        "android-25" => "Android 7.1",
        "android-26" => "Android 8.0",
        "android-27" => "Android 8.1",
        "android-28" => "Android 9",
        "android-29" => "Android 10",
        "android-30" => "Android 11",
        "android-31" => "Android 12",
        "android-32" => "Android 12L",
        "android-33" => "Android 13",
        "android-34" => "Android 14",
        "android-35" => "Android 15",
        "android-36" => "Android 16",
        _ => return None,
    })
}

fn current_sysdir_rel() -> Option<String> {
    // AVD config.ini 的 image.sysdir.1（相对 SDK 根）
    let text = std::fs::read_to_string(format!("{}/config.ini", state::avd_dir())).ok()?;
    for line in text.lines() {
        if let Some(v) = line.strip_prefix("image.sysdir.1") {
            return Some(
                v.trim_start_matches([' ', '='])
                    .trim_end_matches('/')
                    .to_string(),
            );
        }
    }
    None
}

pub async fn image_list() -> (StatusCode, Json<Value>) {
    let Some(sdk) = state::sdk_root() else {
        return err(500, "SDK 探测失败（未找到 emulator）");
    };
    let sysdir = format!("{sdk}/system-images");
    let _ = std::fs::create_dir_all(&sysdir);
    let in_use = current_sysdir_rel();
    let mut arr = Vec::new();
    if let Ok(apis) = std::fs::read_dir(&sysdir) {
        for api in apis.flatten() {
            let api_name = api.file_name().to_string_lossy().to_string();
            if let Ok(tags) = std::fs::read_dir(api.path()) {
                for tag in tags.flatten() {
                    let abi = tag.path().join("arm64-v8a");
                    let has_system = abi.join("system-qemu.img").is_file()
                        || abi.join("system.img").is_file();
                    if !has_system {
                        continue;
                    }
                    let size: u64 = std::fs::read_dir(&abi)
                        .map(|rd| {
                            rd.flatten()
                                .filter_map(|f| f.metadata().ok().map(|m| m.len()))
                                .sum()
                        })
                        .unwrap_or(0);
                    let rel = format!(
                        "system-images/{}/{}/arm64-v8a",
                        api_name,
                        tag.file_name().to_string_lossy()
                    );
                    arr.push(json!({
                        "api": api_name,
                        "display": android_version_name(&api_name),
                        "name": tag.file_name().to_string_lossy(),
                        "path": abi.to_string_lossy(),
                        "rel": rel,
                        "in_use": in_use.as_deref() == Some(rel.as_str()),
                        "size_mb": size / 1_048_576,
                    }));
                }
            }
        }
    }
    ok(json!({ "images": arr }))
}

pub async fn image_use(
    State(st): State<SharedState>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let path = v.get("path").and_then(|x| x.as_str()).unwrap_or("");
    let stop_vm = v.get("stop_vm").map(|x| x.as_bool().unwrap_or(false)).unwrap_or(false);
    let Some(sdk) = state::sdk_root() else {
        return err(500, "SDK 探测失败");
    };
    let sysroot = std::fs::canonicalize(format!("{sdk}/system-images")).unwrap_or_default();
    let canon = std::fs::canonicalize(path).unwrap_or_default();
    if !canon.starts_with(&sysroot)
        || !(canon.join("system-qemu.img").is_file() || canon.join("system.img").is_file())
    {
        return err(400, "非法的镜像目录");
    }
    // rel：system-images/<api>/<tag>/arm64-v8a
    let rel = canon
        .strip_prefix(&sysroot)
        .map(|r| format!("system-images{}", r.to_string_lossy()))
        .unwrap_or_default();
    let running = st.current_serial();
    if running.is_some() && !stop_vm {
        return err(409, "虚拟机运行中：需先停止（传 stop_vm=true 自动停止）");
    }
    if let Some(serial) = running {
        let _ = console::cmd(&serial, "kill", false);
        for _ in 0..30 {
            if adb::online_emulator().is_none() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_secs(1));
        }
    }
    // 改写 config.ini 的 image.sysdir.1
    let ini = format!("{}/config.ini", state::avd_dir());
    let text = std::fs::read_to_string(&ini).unwrap_or_default();
    let mut out = String::new();
    let mut found = false;
    for line in text.lines() {
        if line.starts_with("image.sysdir.1") {
            out.push_str(&format!("image.sysdir.1 = {rel}\n"));
            found = true;
        } else {
            out.push_str(line);
            out.push('\n');
        }
    }
    if !found {
        out.push_str(&format!("image.sysdir.1 = {rel}\n"));
    }
    if let Err(e) = std::fs::write(&ini, out) {
        return err(500, format!("写入 AVD 配置失败: {e}"));
    }
    ok(json!({ "ok": true, "restarted": false, "sysdir": rel }))
}

pub async fn image_delete(
    State(_st): State<SharedState>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let path = v.get("path").and_then(|x| x.as_str()).unwrap_or("");
    let Some(sdk) = state::sdk_root() else {
        return err(500, "SDK 探测失败");
    };
    let sysroot = std::fs::canonicalize(format!("{sdk}/system-images")).unwrap_or_default();
    let canon = std::fs::canonicalize(path).unwrap_or_default();
    if !canon.starts_with(&sysroot) {
        return err(400, "非法的镜像目录");
    }
    let rel = format!("system-images{}", canon.strip_prefix(&sysroot).map(|r| r.to_string_lossy()).unwrap_or_default());
    if current_sysdir_rel().as_deref() == Some(rel.as_str()) {
        return err(409, "在用镜像不许删除");
    }
    match std::fs::remove_dir_all(&canon) {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => err(500, format!("删除失败: {e}")),
    }
}

pub async fn image_task(State(st): State<SharedState>) -> Json<Value> {
    Json(st.image_task.to_json())
}

// ===================== 运行时 =====================

pub async fn runtime_status() -> Json<Value> {
    let sdk = state::sdk_root();
    let present = sdk.is_some();
    Json(json!({
        "present": present,
        "source": if present { "sdk" } else { "none" },
        "path": sdk,
    }))
}

pub async fn runtime_task(State(st): State<SharedState>) -> Json<Value> {
    Json(st.runtime_task.to_json())
}

// ===================== 终端 =====================

pub async fn term_open(State(st): State<SharedState>) -> (StatusCode, Json<Value>) {
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let r = tokio::task::spawn_blocking(move || adb::shell_pty(&serial)).await;
    match r {
        Ok(Ok((proc, pty))) => {
            let id = st.alloc_id();
            let serial = st.current_serial().unwrap_or_default();
            let mut reader = match proc.stream.try_clone() {
                Ok(r) => r,
                Err(e) => return err(500, format!("终端连接复制失败: {e}")),
            };
            let term = Arc::new(TermSession {
                id,
                serial: serial.clone(),
                pty,
                alive: std::sync::atomic::AtomicBool::new(true),
                buf: Mutex::new(Vec::new()),
                writer: Mutex::new(proc.stream),
            });
            let t2 = term.clone();
            std::thread::spawn(move || {
                let mut buf = [0u8; 8192];
                // v2 pty 分帧暂存（跨 read 的半帧要拼）
                let mut framebuf: Vec<u8> = Vec::new();
                loop {
                    match std::io::Read::read(&mut reader, &mut buf) {
                        Ok(0) => {
                            t2.alive.store(false, Ordering::SeqCst);
                            break;
                        }
                        Ok(n) => {
                            if t2.pty {
                                // v2 去帧头：id u8 + len u32 LE + payload；id 1/2 进缓冲，3=退出
                                framebuf.extend_from_slice(&buf[..n]);
                                loop {
                                    if framebuf.len() < 5 {
                                        break;
                                    }
                                    let fid = framebuf[0];
                                    let flen = u32::from_le_bytes([
                                        framebuf[1], framebuf[2], framebuf[3], framebuf[4],
                                    ])
                                        as usize;
                                    if framebuf.len() < 5 + flen {
                                        break;
                                    }
                                    let payload = framebuf[5..5 + flen].to_vec();
                                    framebuf.drain(..5 + flen);
                                    if fid == 1 || fid == 2 {
                                        t2.buf.lock().unwrap().extend_from_slice(&payload);
                                    } else if fid == 3 {
                                        t2.alive.store(false, Ordering::SeqCst);
                                        return;
                                    }
                                }
                            } else {
                                t2.buf.lock().unwrap().extend_from_slice(&buf[..n]);
                            }
                        }
                        Err(_) => {
                            t2.alive.store(false, Ordering::SeqCst);
                            break;
                        }
                    }
                }
            });
            st.terminals.lock().unwrap().insert(id, term);
            ok(json!({ "ok": true, "id": id, "serial": serial, "pty": pty }))
        }
        Ok(Err(e)) => err(500, e.to_string()),
        Err(e) => err(500, format!("终端开启失败: {e}")),
    }
}

pub async fn term_read(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
    Query(q): Query<std::collections::HashMap<String, String>>,
) -> (StatusCode, Json<Value>) {
    let term = match st.terminals.lock().unwrap().get(&id) {
        Some(t) => t.clone(),
        None => return err(404, "会话不存在"),
    };
    let cursor: usize = q
        .get("cursor")
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let buf = term.buf.lock().unwrap();
    let data = if cursor < buf.len() {
        String::from_utf8_lossy(&buf[cursor..]).to_string()
    } else {
        String::new()
    };
    let new_cursor = buf.len();
    drop(buf);
    ok(json!({
        "id": id,
        "cursor": new_cursor,
        "data": data,
        "alive": term.alive.load(Ordering::SeqCst),
        "pty": term.pty,
    }))
}

pub async fn term_write(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let term = match st.terminals.lock().unwrap().get(&id) {
        Some(t) => t.clone(),
        None => return err(404, "会话不存在"),
    };
    let data = v.get("data").and_then(|x| x.as_str()).unwrap_or("");
    let r = term.writer.lock().unwrap().write_all(data.as_bytes());
    match r {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => err(500, format!("写入失败: {e}")),
    }
}

pub async fn term_close(State(st): State<SharedState>, Path(id): Path<u64>) -> Json<Value> {
    if let Some(t) = st.terminals.lock().unwrap().remove(&id) {
        t.alive.store(false, Ordering::SeqCst);
        let _ = t.writer.lock().unwrap().shutdown(std::net::Shutdown::Both);
    }
    Json(json!({ "ok": true }))
}

// ===================== 设备（真机列出只读） =====================

pub async fn devices() -> (StatusCode, Json<Value>) {
    match tokio::task::spawn_blocking(adb::devices).await {
        Ok(Ok(ds)) => ok(json!({ "devices": ds })),
        Ok(Err(e)) => err(500, e.to_string()),
        Err(e) => err(500, format!("设备列表失败: {e}")),
    }
}

// ===================== 501 占位 =====================

pub async fn not_impl_store() -> (StatusCode, Json<Value>) {
    not_impl("在线商店")
}
pub async fn not_impl_capture() -> (StatusCode, Json<Value>) {
    not_impl("流量分析")
}
pub async fn not_impl_pair() -> (StatusCode, Json<Value>) {
    not_impl("真机配对")
}
pub async fn not_impl_runtime_dl() -> (StatusCode, Json<Value>) {
    not_impl("运行时在线下载")
}
pub async fn not_impl_image_pull() -> (StatusCode, Json<Value>) {
    not_impl("镜像在线拉取")
}
pub async fn not_impl_misc() -> (StatusCode, Json<Value>) {
    not_impl("该功能")
}

// ===================== 投屏 =====================

pub async fn stream_open(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
) -> (StatusCode, Json<Value>) {
    let serial = match st.current_serial() {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let jar = match scrcpy::find_server_jar() {
        Some(j) => j,
        None => return err(500, "找不到 scrcpy-server（运行时未就绪）"),
    };
    let max_size: u32 = q.get("max_size").and_then(|s| s.parse().ok()).unwrap_or(1280);
    let bit_rate: u32 = q.get("bit_rate").and_then(|s| s.parse().ok()).unwrap_or(4_000_000);
    let fps: u32 = q.get("fps").and_then(|s| s.parse().ok()).unwrap_or(30);
    // 应用独立窗口：app= 包名 + 可选 w/h/dpi 副屏尺寸（默认 720×1280@320 竖屏手机）
    let start_app = q.get("app").cloned().filter(|s| !s.is_empty());
    let new_display = if start_app.is_some() {
        let w: u32 = q.get("w").and_then(|s| s.parse().ok()).unwrap_or(720);
        let h: u32 = q.get("h").and_then(|s| s.parse().ok()).unwrap_or(1280);
        let dpi: u32 = q.get("dpi").and_then(|s| s.parse().ok()).unwrap_or(320);
        Some((w, h, dpi))
    } else {
        None
    };
    // 副屏会话的视频流上限取副屏长边（max_size 约束只到视频编码器）
    let max_size = if let Some((w, h, _)) = new_display {
        w.max(h)
    } else {
        max_size
    };
    let params = scrcpy::StartParams { max_size, bit_rate, fps, new_display, start_app };

    let r = tokio::task::spawn_blocking(move || scrcpy::start(&serial, &jar, &params)).await;
    let mut session = match r {
        Ok(Ok(s)) => s,
        Ok(Err(e)) => return err(500, format!("投屏会话启动失败: {e}")),
        Err(e) => return err(500, format!("投屏任务失败: {e}")),
    };

    let id = st.alloc_id();
    let (tx, rx) = tokio::sync::mpsc::channel::<Vec<u8>>(64);
    let codec = session.codec.clone();
    let serial = session.serial.clone();

    // 控制通道克隆用于 stop_fn
    let control = session.control.take().map(|c| Arc::new(Mutex::new(c)));
    let control_for_stop = control.clone();
    let serial_for_stop = serial.clone();
    let fwd_port = session.forward_port;

    let stream = Arc::new(StreamSession {
        id,
        serial: serial.clone(),
        codec: codec.clone(),
        width: std::sync::atomic::AtomicU64::new(session.width as u64),
        height: std::sync::atomic::AtomicU64::new(session.height as u64),
        control,
        ended: std::sync::atomic::AtomicBool::new(false),
        err: Mutex::new(String::new()),
        frame_rx: Mutex::new(Some(rx)),
        attached: std::sync::atomic::AtomicBool::new(false),
        clipboard: Mutex::new(String::new()),
        stop_fn: Mutex::new(Some(Box::new(move || {
            if let Some(c) = &control_for_stop {
                let _ = c.lock().unwrap().shutdown(std::net::Shutdown::Both);
            }
            let _ = adb::remove_forward(&serial_for_stop, fwd_port);
        }))),
    });
    // 设备消息读取线程：clipboard_autosync 的剪贴板推送经控制通道上行
    if let Some(ctrl) = &stream.control {
        if let Ok(mut rd) = ctrl.lock().unwrap().try_clone() {
            let s3 = stream.clone();
            std::thread::spawn(move || {
                use std::io::Read as _;
                loop {
                    let mut t = [0u8; 1];
                    if rd.read_exact(&mut t).is_err() { break; }
                    if t[0] == 0 {
                        // CLIPBOARD：lp32 字符串
                        let mut l4 = [0u8; 4];
                        if rd.read_exact(&mut l4).is_err() { break; }
                        let n = u32::from_be_bytes(l4) as usize;
                        let mut payload = vec![0u8; n.min(1_000_000)];
                        if n > 0 && rd.read_exact(&mut payload).is_err() { break; }
                        *s3.clipboard.lock().unwrap() = String::from_utf8_lossy(&payload).to_string();
                    } else {
                        // 其余消息（ack/uhid）本用例不关心——但无法预知长度，
                        // 协议上 ack 是 8 字节序列号，跳过；未知类型终止读取
                        if t[0] == 1 {
                            let mut skip = [0u8; 8];
                            if rd.read_exact(&mut skip).is_err() { break; }
                        } else {
                            break;
                        }
                    }
                }
            });
        }
    }
    let s2 = stream.clone();
    let st2 = st.clone();
    std::thread::spawn(move || {
        crate::scrcpy_feeder(session, tx, s2);
        st2.streams.lock().unwrap().remove(&id);
    });
    st.streams.lock().unwrap().insert(id, stream.clone());
    // 等首个 meta（编码器冷启数秒）
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(8);
    while std::time::Instant::now() < deadline {
        if stream.width.load(Ordering::SeqCst) > 0 || stream.ended.load(Ordering::SeqCst) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    if stream.ended.load(Ordering::SeqCst) && stream.width.load(Ordering::SeqCst) == 0 {
        let detail = stream.err.lock().unwrap().clone();
        return err(
            500,
            format!(
                "设备未产出视频流（该镜像可能不支持投屏编码）{}",
                if detail.is_empty() { String::new() } else { format!(": {detail}") }
            ),
        );
    }
    ok(json!({
        "id": id, "serial": serial, "codec": codec,
        "width": stream.width.load(Ordering::SeqCst),
        "height": stream.height.load(Ordering::SeqCst),
    }))
}

pub async fn stream_video(State(st): State<SharedState>, Path(id): Path<u64>) -> axum::response::Response {
    use axum::response::IntoResponse;
    let stream = match st.streams.lock().unwrap().get(&id) {
        Some(s) => s.clone(),
        None => return err(404, "会话不存在").into_response(),
    };
    if stream.attached.swap(true, Ordering::SeqCst) {
        return err(409, "该会话的视频已被领取（每会话一个观众）").into_response();
    }
    let rx = match stream.frame_rx.lock().unwrap().take() {
        Some(r) => r,
        None => return err(409, "视频流已被领取").into_response(),
    };
    let recv_stream = tokio_stream::wrappers::ReceiverStream::new(rx);
    let body = axum::body::Body::from_stream(
        tokio_stream::StreamExt::map(recv_stream, |v| Ok::<bytes::Bytes, std::convert::Infallible>(bytes::Bytes::from(v))),
    );
    (
        [
            (axum::http::header::CONTENT_TYPE, "application/octet-stream"),
            (axum::http::header::CACHE_CONTROL, "no-store"),
            (axum::http::header::HeaderName::from_static("x-accel-buffering"), "no"),
        ],
        body,
    )
        .into_response()
}

fn stream_get(st: &AppState, id: u64) -> Result<Arc<StreamSession>, (StatusCode, Json<Value>)> {
    st.streams
        .lock()
        .unwrap()
        .get(&id)
        .cloned()
        .ok_or_else(|| err(404, "会话不存在"))
}

fn stream_control_send(
    s: &StreamSession,
    bytes: &[u8],
) -> Result<(), (StatusCode, Json<Value>)> {
    let Some(ctrl) = &s.control else {
        return Err(err(500, "控制通道不可用"));
    };
    ctrl.lock()
        .unwrap()
        .write_all(bytes)
        .map_err(|e| err(500, format!("控制通道写入失败: {e}")))
}

pub async fn stream_touch(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let s = match stream_get(&st, id) { Ok(s) => s, Err(e) => return e };
    let action = v.get("action").and_then(|x| x.as_i64()).unwrap_or(-1);
    if !(0..=2).contains(&action) {
        return err(400, "action 须为 0(down)/1(up)/2(move)");
    }
    let w = s.width.load(Ordering::SeqCst) as u16;
    let h = s.height.load(Ordering::SeqCst) as u16;
    if w == 0 || h == 0 {
        return err(409, "画面尺寸尚未就绪");
    }
    let x = v.get("x").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
    let y = v.get("y").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
    let up = action == 1;
    let bytes = scrcpy::control::inject_touch(
        action as u8,
        scrcpy::control::POINTER_ID_MOUSE,
        x, y, w, h,
        if up { 0.0 } else { 1.0 },
        1,
        if up { 0 } else { 1 },
    );
    match stream_control_send(&s, &bytes) {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => e,
    }
}

pub async fn stream_key(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let s = match stream_get(&st, id) { Ok(s) => s, Err(e) => return e };
    let keycode = v.get("keycode").and_then(|x| x.as_i64()).unwrap_or(-1) as i32;
    if keycode < 0 {
        return err(400, "缺少 keycode");
    }
    let action = v.get("action").and_then(|x| x.as_str()).unwrap_or("tap");
    let send = |down: bool| stream_control_send(&s, &scrcpy::control::inject_keycode(down, keycode));
    let r = match action {
        "down" => send(true),
        "up" => send(false),
        _ => send(true).and_then(|_| send(false)),
    };
    match r {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => e,
    }
}

pub async fn stream_text(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let s = match stream_get(&st, id) { Ok(s) => s, Err(e) => return e };
    let text = v.get("text").and_then(|x| x.as_str()).unwrap_or("");
    if text.is_empty() || text.len() > 300 {
        return err(400, "text 为空或超长");
    }
    // 与桌面版 injectTextSmart 同策略：ASCII 走 INJECT_TEXT；非 ASCII
    //（CJK 等无按键映射字符会被 server 静默丢弃）走 SET_CLIPBOARD(paste=true)
    //——server 置剪贴板后立即触发 KEYCODE_PASTE，焦点字段即得全文。
    let ascii = text.bytes().all(|b| (32..=127).contains(&b));
    let bytes = if ascii {
        match scrcpy::control::inject_text(text) {
            Some(b) => b,
            None => return err(400, "文本超长"),
        }
    } else {
        match scrcpy::control::set_clipboard(0, true, text) {
            Some(b) => b,
            None => return err(400, "文本超长"),
        }
    };
    match stream_control_send(&s, &bytes) {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => e,
    }
}

pub async fn stream_scroll(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let s = match stream_get(&st, id) { Ok(s) => s, Err(e) => return e };
    let w = s.width.load(Ordering::SeqCst) as u16;
    let h = s.height.load(Ordering::SeqCst) as u16;
    if w == 0 || h == 0 {
        return err(409, "画面尺寸尚未就绪");
    }
    let x = v.get("x").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
    let y = v.get("y").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
    let hs = v.get("hs").and_then(|x| x.as_f64()).unwrap_or(0.0) as f32;
    let vs = v.get("vs").and_then(|x| x.as_f64()).unwrap_or(0.0) as f32;
    let bytes = scrcpy::control::inject_scroll(x, y, w, h, hs, vs);
    match stream_control_send(&s, &bytes) {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => e,
    }
}

pub async fn stream_reset(State(st): State<SharedState>, Path(id): Path<u64>) -> (StatusCode, Json<Value>) {
    let s = match stream_get(&st, id) { Ok(s) => s, Err(e) => return e };
    match stream_control_send(&s, &scrcpy::control::reset_video()) {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => e,
    }
}

pub async fn stream_close(State(st): State<SharedState>, Path(id): Path<u64>) -> Json<Value> {
    if let Some(s) = st.streams.lock().unwrap().remove(&id) {
        s.stop();
    }
    Json(json!({ "ok": true }))
}

// GET /api/stream/<id>/clipboard —— 读设备剪贴板（autosync 推送的最新值）
pub async fn stream_clipboard_get(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
) -> (StatusCode, Json<Value>) {
    let s = match stream_get(&st, id) { Ok(s) => s, Err(e) => return e };
    ok(json!({ "text": s.clipboard.lock().unwrap().clone() }))
}

// POST /api/stream/<id>/clipboard {text} —— 宿主剪贴板同步进设备（不触发粘贴）
pub async fn stream_clipboard_set(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let s = match stream_get(&st, id) { Ok(s) => s, Err(e) => return e };
    let text = v.get("text").and_then(|x| x.as_str()).unwrap_or("");
    let paste = v.get("paste").and_then(|x| x.as_bool()).unwrap_or(false);
    let Some(bytes) = scrcpy::control::set_clipboard(0, paste, text) else {
        return err(400, "剪贴板内容超长");
    };
    match stream_control_send(&s, &bytes) {
        Ok(()) => ok(json!({ "ok": true })),
        Err(e) => e,
    }
}
