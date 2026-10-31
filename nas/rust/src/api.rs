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
    // NAS 版纯真机网关：无虚拟机/运行时概念
    let device = st.current_serial();
    Json(json!({
        "error": "",
        "version": "0.1.0-rust",
        "backend": "rust",
        "mode": "device",
        "device_serial": device,
        "target_serial": device,
    }))
}

// ===================== 性能 =====================

/// 单设备性能+信息采样（SSE 顶栏与 /api/perf 共用）。
/// 一次 shell 取 /proc 数据；电池/机型额外两发（机型按 serial 缓存）。
pub fn dev_snapshot(st: &AppState, serial: &str) -> Value {
    let mut out = json!({ "serial": serial });
    if let Ok(o) = adb::shell_blocking(
        serial,
        "head -1 /proc/stat; grep -E 'MemTotal|MemAvailable' /proc/meminfo; cat /proc/uptime",
        5000,
    ) {
        let text = String::from_utf8_lossy(&o);
        let lines: Vec<&str> = text.lines().collect();
        if lines.len() >= 4 {
            // 按键名解析（kB→MB），不依赖行序
            let mem_kb = |key: &str| -> u64 {
                lines
                    .iter()
                    .find(|l| l.contains(key))
                    .and_then(|l| l.split_whitespace().nth(1))
                    .and_then(|t| t.parse().ok())
                    .unwrap_or(0)
                    / 1024
            };
            let fields: Vec<u64> = lines[0]
                .split_whitespace()
                .filter_map(|t| t.parse::<u64>().ok())
                .collect();
            if fields.len() >= 4 {
                let total: u64 = fields.iter().sum();
                let idle = fields[3] + fields.get(4).copied().unwrap_or(0);
                let busy = total - idle;
                let mem_total_mb = mem_kb("MemTotal");
                let avail_mb = mem_kb("MemAvailable");
                let uptime: f64 = lines[3]
                    .split_whitespace()
                    .next()
                    .and_then(|t| t.parse().ok())
                    .unwrap_or(0.0);
                // cpu% 差分（与全局 perf_last 状态共享）
                let cpu_pct = {
                    let mut last = st.perf_last.lock().unwrap();
                    match *last {
                        Some((lb, lt)) if total > lt && busy >= lb => {
                            (busy - lb) as f64 * 100.0 / (total - lt) as f64
                        }
                        _ => 0.0,
                    }
                };
                {
                    let mut last = st.perf_last.lock().unwrap();
                    *last = Some((busy, total));
                }
                out["cpu_pct"] = json!((cpu_pct * 10.0).round() / 10.0);
                out["mem"] = json!({
                    "total_mb": mem_total_mb,
                    "available_mb": avail_mb,
                });
                out["uptime_s"] = json!(uptime as u64);
            }
        }
    }
    // 电池/温度（复用 device_info 的解析逻辑，单设备版）
    if let Some(batt) = battery_of(serial) {
        out["battery"] = batt;
    }
    // 机型（缓存——getprop 每次都查太浪费）
    out["model"] = json!(model_of(serial));
    out
}

/// 单设备电池信息（device_info 的单设备版）
fn battery_of(serial: &str) -> Option<Value> {
    let out = adb::shell_blocking(serial, "dumpsys battery", 6000).ok()?;
    let text = String::from_utf8_lossy(&out);
    let g = |k: &str| -> Option<i64> {
        text.lines().find_map(|l| {
            let l = l.trim();
            l.starts_with(k).then(|| l[k.len()..].trim().parse().ok())?
        })
    };
    let status_code = g("status:")?;
    let status = match status_code {
        2 => "charging",
        3 => "discharging",
        4 => "not_charging",
        5 => "full",
        _ => "unknown",
    };
    Some(json!({
        "present": text.contains("present: true"),
        "level": g("level:"),
        "status": status,
        "temp_c": g("temperature:").map(|t| t as f64 / 10.0),
        "usb_powered": text.contains("USB powered: true"),
    }))
}

/// 机型名缓存（"HONOR VNE-AN00" 式：manufacturer model）
fn model_of(serial: &str) -> String {
    static MODELS: std::sync::LazyLock<Mutex<std::collections::HashMap<String, String>>> =
        std::sync::LazyLock::new(|| Mutex::new(std::collections::HashMap::new()));
    if let Some(m) = MODELS.lock().unwrap().get(serial) {
        return m.clone();
    }
    let m = adb::shell_blocking(
        serial,
        "getprop ro.product.manufacturer; getprop ro.product.model",
        5000,
    )
    .ok()
    .map(|o| {
        let text = String::from_utf8_lossy(&o).to_string();
        // 两行各取非空：manufacturer 可为空（TCL TV 实测只有 model）
        let parts: Vec<&str> = text.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
        match parts.as_slice() {
            [a, b] => format!("{a} {b}"),
            [a] => a.to_string(),
            _ => serial.to_string(),
        }
    })
    .unwrap_or_else(|| serial.to_string());
    MODELS.lock().unwrap().insert(serial.to_string(), m.clone());
    m
}

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
            30000,
        )
        .map_err(|e| e.to_string())?;
        let text = String::from_utf8_lossy(&out);
        let pkgs: Vec<&str> = text
            .lines()
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .collect();
        let serial_bg = serial.clone();
        // 标签缺失的后台补抓（拉 APK + AXML/arsc 解析较慢，不阻塞列表响应）
        {
            let missing: Vec<String> = pkgs
                .iter()
                .map(|p| p.to_string())
                .filter(|p| crate::icons::app_label_cached_only(p).is_none())
                .collect();
            if !missing.is_empty() {
                std::thread::spawn(move || {
                    for pkg in missing {
                        let _ = crate::icons::app_label(&serial_bg, &pkg);
                    }
                });
            }
        }
        // 图标后台预热：图标提取只由图标接口懒触发，而前端只对 has_icon=true
        // 的包渲染 img——全新环境（缓存为空）会死锁成全字母块。这里主动补齐：
        // 分 3 路打满提取槽；负缓存（.noicon）在 app_icon 内部快速返回不重复劳动；
        // 落盘后 cache_stamp 变化 → SSE /api/events/apps 推新列表，前端图标渐出。
        {
            let missing: Vec<String> = pkgs
                .iter()
                .map(|p| p.to_string())
                .filter(|p| !crate::icons::icon_cached_or_inflight(p))
                .collect();
            if !missing.is_empty() {
                for lane in 0..3usize {
                    let serial_ic = serial.clone();
                    let chunk: Vec<String> = missing
                        .iter()
                        .skip(lane)
                        .step_by(3)
                        .cloned()
                        .collect();
                    if chunk.is_empty() {
                        continue;
                    }
                    std::thread::spawn(move || {
                        for pkg in chunk {
                            let _ = crate::icons::app_icon(&serial_ic, &pkg);
                        }
                    });
                }
            }
        }
        let apps: Vec<Value> = pkgs
            .iter()
            .map(|pkg| {
                let label = crate::icons::app_label_cached_only(pkg);
                json!({
                    "package": pkg,
                    "name": label.unwrap_or_else(|| pkg.rsplit('.').next().unwrap_or(pkg).to_string()),
                    "icon": Value::Null,
                    "running": false,
                    "run_on": "main", // 应用运行位置（与全局真机/虚拟机模式无关）
                    "has_icon": crate::icons::icon_cached(pkg),
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
        Ok(Ok((bytes, source))) => (
            [
                (axum::http::header::CONTENT_TYPE, "image/png"),
                (axum::http::header::CACHE_CONTROL, "public, max-age=60"),
                (axum::http::header::HeaderName::from_static("x-wallpaper-source"), source),
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
    let r = tokio::task::spawn_blocking(move || adb::shell_blocking(&serial, "screencap -p", 30000))
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

// ===================== 镜像 =====================

// ===================== 运行时 =====================

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
pub async fn not_impl_misc() -> (StatusCode, Json<Value>) {
    not_impl("该功能")
}

// ===================== 投屏 =====================

pub async fn stream_open(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
) -> (StatusCode, Json<Value>) {
    // 显式 serial 优先（前端指定设备），缺省回落 run_mode 自动选
    let serial = q
        .get("serial")
        .filter(|s| !s.is_empty())
        .cloned()
        .or_else(|| st.current_serial());
    let serial = match serial {
        Some(s) => s,
        None => return err(503, "Android 未运行"),
    };
    let jar = match scrcpy::find_server_jar() {
        Some(j) => j,
        None => return err(500, "找不到 scrcpy-server（运行时未就绪）"),
    };
    let max_size: u32 = q.get("max_size").and_then(|s| s.parse().ok()).unwrap_or(1280);
    let bit_rate: u32 = q.get("bit_rate").and_then(|s| s.parse().ok()).unwrap_or(8_000_000);
    let fps: u32 = q.get("fps").and_then(|s| s.parse().ok()).unwrap_or(60);
    // 应用独立窗口：app= 包名 + 可选 w/h/dpi 副屏尺寸（默认 720×1280@320 竖屏手机）
    let start_app = q.get("app").cloned().filter(|s| !s.is_empty());
    let audio = q.get("audio").and_then(|s| s.parse().ok()).unwrap_or(true);
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
    // 音频始终请求：设备不支持时 server 下发 disable 码自动回退纯视频
    let params = scrcpy::StartParams { max_size, bit_rate, fps, audio, new_display, start_app };

    // TV/大屏设备待机（Dreaming）时 scrcpy 抓的是黑缓冲——开流前先唤醒
    // （不影响手机：已亮屏的设备 wake_up 是 no-op；scrcpy stay_awake=true 接管保活）
    // 唤醒：先查屏幕状态，仅在熄屏时才 wake+等管线（多数时候已亮屏，省 800ms+一次 shell 往返）
    let serial_wake = serial.clone();
    let woke = tokio::task::spawn_blocking(move || {
        let awake = adb::shell_blocking(&serial_wake, "dumpsys power | grep -q 'mWakefulness=Awake' && echo Y", 5000)
            .map(|o| String::from_utf8_lossy(&o).contains('Y'))
            .unwrap_or(true); // 查询失败按已亮屏处理（不白等）
        if awake {
            false
        } else {
            let _ = adb::shell_blocking(&serial_wake, "input keyevent KEYCODE_WAKEUP", 5000);
            true
        }
    })
    .await
    .unwrap_or(false);
    if woke {
        // 唤醒后给 SurfaceFlinger 一点时间建立新帧（TV 的管线启动比手机慢）
        tokio::time::sleep(std::time::Duration::from_millis(800)).await;
    }

    // 会话空闲回收看门狗（独立线程，每 15s 扫描）：
    //   A) 从未被观看（!attached）超 90s —— 页面打开失败/忘记关
    //   B) 已结束（ended）超 30s —— 流断开后残留
    //   C) 帧停滞超 180s（页面关闭后 video pump 断开 → feeder 发送失败自退 → ended，B 已覆盖；
    //      此项兜底 pump 存活但无人消费的极端态）
    {
        let st_w = st.clone();
        std::thread::spawn(move || loop {
            std::thread::sleep(std::time::Duration::from_secs(15));
            let now_ms = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_millis() as u64)
                .unwrap_or(0);
            let created_ms = now_ms; // 会话创建即开始计时（attached=false 判定用）
            let victims: Vec<Arc<StreamSession>> = {
                let mut m = st_w.streams.lock().unwrap();
                let mut kill = Vec::new();
                m.retain(|_, s| {
                    let attached = s.attached.load(Ordering::SeqCst);
                    let ended = s.ended.load(Ordering::SeqCst);
                    let last = s.last_frame_ms.load(Ordering::Relaxed);
                    let idle_ms = if last > 0 { now_ms.saturating_sub(last) } else { 0 };
                    let dead = (!attached && now_ms.saturating_sub(created_ms) > 90_000)
                        || (ended && idle_ms > 30_000)
                        || (!ended && last > 0 && idle_ms > 180_000);
                    if dead {
                        kill.push(s.clone());
                        false // retain 移除
                    } else {
                        true
                    }
                });
                kill
            };
            for s in victims {
                tracing::info!("投屏会话空闲回收 id={} serial={}", s.id, s.serial);
                s.stop();
            }
        });
    }

    // 开流（含一次整体重试）：server 桥上「推 jar（sync 流）→ 启动 server
    // （shell 流）」快速连续建链偶发假流（HTTP200 但 0 字节，实测 ~30% 概率
    // 连续开关时出现）。重试路径 jar 已在（stat 命中快路径），第二轮稳定。
    let mut attempt = 0;
    let session = loop {
        attempt += 1;
        let serial_c = serial.clone();
        let jar_c = jar.clone();
        let params_c = params.clone();
        let r = tokio::task::spawn_blocking(move || scrcpy::start(&serial_c, &jar_c, &params_c)).await;
        match r {
            Ok(Ok(s)) => break s,
            Ok(Err(e)) => {
                if attempt >= 2 {
                    return err(500, format!("投屏会话启动失败: {e}"));
                }
                tracing::warn!("投屏开流第 {attempt} 次失败（{e}），重试");
                tokio::time::sleep(std::time::Duration::from_millis(500)).await;
            }
            Err(e) => return err(500, format!("投屏任务失败: {e}")),
        }
    };
    let mut session = session;

    let id = st.alloc_id();
    let (tx, rx) = tokio::sync::mpsc::channel::<Vec<u8>>(64);
    let codec = session.codec.clone();
    let serial = session.serial.clone();

    // 音频 socket 交给专用喂包线程（与视频线程并行读）
    if let Some(audio_sock) = session.audio.take() {
        let tx_audio = tx.clone();
        std::thread::spawn(move || crate::scrcpy_audio_feeder(audio_sock, tx_audio));
    }

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
        last_frame_ms: std::sync::atomic::AtomicU64::new(0),
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

// ---- 控制通道 WebSocket：一条连接即发即走 ----
// 原 POST /touch|key|text|scroll 每事件一个 HTTP 请求，连续拖动时事件
// 在浏览器同源连接池里排队（并发 6 限制），输入延迟明显劣于原生 scrcpy
// 的单 socket。WS 化后与 scrcpy 相同的即时注入路径。

/// 按消息类型把 JSON 载荷编译成 scrcpy 控制帧并发送（touch/key/text/scroll/reset 复用各端点逻辑）
fn ctl_dispatch(s: &Arc<StreamSession>, v: &Value) -> Result<(), String> {
    let t = v.get("t").and_then(|x| x.as_str()).unwrap_or("");
    match t {
        "touch" => {
            let action = v.get("action").and_then(|x| x.as_i64()).unwrap_or(-1);
            if !(0..=2).contains(&action) {
                return Err("action 须为 0(down)/1(up)/2(move)".into());
            }
            let w = s.width.load(Ordering::SeqCst) as u16;
            let h = s.height.load(Ordering::SeqCst) as u16;
            if w == 0 || h == 0 {
                return Err("画面尺寸尚未就绪".into());
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
            stream_control_send(s, &bytes).map_err(|(_, j)| j.get("error").and_then(|x| x.as_str()).unwrap_or("控制发送失败").to_string())
        }
        "key" => {
            let keycode = v.get("keycode").and_then(|x| x.as_i64()).unwrap_or(-1) as i32;
            if keycode < 0 {
                return Err("缺少 keycode".into());
            }
            let action = v.get("action").and_then(|x| x.as_str()).unwrap_or("tap");
            let send = |down: bool| stream_control_send(s, &scrcpy::control::inject_keycode(down, keycode));
            match action {
                "down" => send(true),
                "up" => send(false),
                _ => send(true).and_then(|_| send(false)),
            }
            .map_err(|(_, j)| j.get("error").and_then(|x| x.as_str()).unwrap_or("控制发送失败").to_string())
        }
        "text" => {
            let text = v.get("text").and_then(|x| x.as_str()).unwrap_or("");
            if text.is_empty() || text.len() > 300 {
                return Err("text 为空或超长".into());
            }
            let ascii = text.bytes().all(|b| (32..=127).contains(&b));
            let bytes = if ascii {
                scrcpy::control::inject_text(text).ok_or("文本超长")?
            } else {
                scrcpy::control::set_clipboard(0, true, text).ok_or("文本超长")?
            };
            stream_control_send(s, &bytes).map_err(|(_, j)| j.get("error").and_then(|x| x.as_str()).unwrap_or("控制发送失败").to_string())
        }
        "scroll" => {
            let w = s.width.load(Ordering::SeqCst) as u16;
            let h = s.height.load(Ordering::SeqCst) as u16;
            if w == 0 || h == 0 {
                return Err("画面尺寸尚未就绪".into());
            }
            let x = v.get("x").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
            let y = v.get("y").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
            let hs = v.get("hs").and_then(|x| x.as_f64()).unwrap_or(0.0) as f32;
            let vs = v.get("vs").and_then(|x| x.as_f64()).unwrap_or(0.0) as f32;
            let bytes = scrcpy::control::inject_scroll(x, y, w, h, hs, vs);
            stream_control_send(s, &bytes).map_err(|(_, j)| j.get("error").and_then(|x| x.as_str()).unwrap_or("控制发送失败").to_string())
        }
        "reset" => stream_control_send(s, &scrcpy::control::reset_video()).map_err(|(_, j)| j.get("error").and_then(|x| x.as_str()).unwrap_or("控制发送失败").to_string()),
        _ => Err(format!("未知控制类型 {t}")),
    }
}

/// GET /api/stream/<id>/ctl —— 控制通道（WS 升级）。
pub async fn stream_control_ws(
    State(st): State<SharedState>,
    Path(id): Path<u64>,
    ws: WebSocketUpgrade,
) -> axum::response::Response {
    use axum::response::IntoResponse;
    let s = match stream_get(&st, id) {
        Ok(s) => s,
        Err(e) => return e.into_response(),
    };
    ws.on_upgrade(move |mut socket| async move {
        while let Some(Ok(msg)) = socket.recv().await {
            let text = match msg {
                Message::Text(t) => t.to_string(),
                Message::Binary(b) => match String::from_utf8(b.to_vec()) {
                    Ok(t) => t,
                    Err(_) => continue,
                },
                Message::Close(_) => break,
                _ => continue,
            };
            let reply = match serde_json::from_str::<Value>(&text) {
                Ok(v) => match ctl_dispatch(&s, &v) {
                    Ok(()) => json!({ "ok": true }),
                    Err(e) => json!({ "error": e }),
                },
                Err(e) => json!({ "error": format!("JSON 解析失败: {e}") }),
            };
            // 回包尽力而为（客户端主要靠发即忘，不等待响应）
            if socket.send(Message::Text(reply.to_string().into())).await.is_err() {
                break;
            }
        }
    })
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

// ===================== WebSocket 终端（真实 pty 桥） =====================

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use futures_util::{SinkExt, StreamExt};

/// GET /api/term/ws?cols=&rows= —— 升级为 WS，桥接设备 pty
pub async fn term_ws(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
    ws: WebSocketUpgrade,
) -> axum::response::Response {
    let cols: u16 = q.get("cols").and_then(|s| s.parse().ok()).unwrap_or(80);
    let rows: u16 = q.get("rows").and_then(|s| s.parse().ok()).unwrap_or(24);
    ws.on_upgrade(move |socket| term_ws_loop(socket, st, cols, rows))
}

async fn term_ws_loop(mut socket: WebSocket, st: SharedState, cols: u16, rows: u16) {
    let Some(serial) = st.current_serial() else {
        let _ = socket
            .send(Message::Text("\x1b[31mAndroid 未运行\x1b[0m\r\n".into()))
            .await;
        return;
    };
    let opened = tokio::task::spawn_blocking(move || adb::shell_pty_full(&serial)).await;
    let (proc, pty) = match opened {
        Ok(Ok(v)) => v,
        Ok(Err(e)) => {
            let _ = socket
                .send(Message::Text(format!("\x1b[31m终端开启失败: {e}\x1b[0m\r\n").into()))
                .await;
            return;
        }
        Err(_) => return,
    };
    let mut dev_read = match proc.stream.try_clone() {
        Ok(r) => r,
        Err(_) => return,
    };
    let mut dev_write = proc.stream;

    // v2 pty：先同步窗口尺寸
    if pty {
        let _ = std::io::Write::write_all(&mut dev_write, &adb::v2_winsz(cols, rows));
    }

    // 设备 → WS：读线程去帧后入 mpsc
    let (tx, mut rx) = tokio::sync::mpsc::channel::<Vec<u8>>(64);
    std::thread::spawn(move || {
        if pty {
            loop {
                match adb::v2_read_frame(&mut dev_read) {
                    Ok(Some((id, payload))) => {
                        if id == adb::V2_EXIT {
                            break;
                        }
                        if id == adb::V2_STDOUT || id == adb::V2_STDERR {
                            if tx.blocking_send(payload).is_err() {
                                break;
                            }
                        }
                    }
                    _ => break,
                }
            }
        } else {
            let mut buf = [0u8; 8192];
            loop {
                match std::io::Read::read(&mut dev_read, &mut buf) {
                    Ok(0) => break,
                    Ok(n) => {
                        if tx.blocking_send(buf[..n].to_vec()).is_err() {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
        }
    });

    // WS → 设备 + 设备 → WS 双工
    loop {
        tokio::select! {
            out = rx.recv() => {
                match out {
                    Some(data) => {
                        if socket.send(Message::Binary(data.into())).await.is_err() {
                            break;
                        }
                    }
                    None => break, // 设备侧结束
                }
            }
            msg = socket.next() => {
                match msg {
                    Some(Ok(Message::Text(t))) => {
                        // 控制消息：{"resize":[cols,rows]}
                        if let Ok(v) = serde_json::from_str::<Value>(&t) {
                            if let Some(arr) = v.get("resize").and_then(|a| a.as_array()) {
                                let c = arr.first().and_then(|x| x.as_u64()).unwrap_or(80) as u16;
                                let r = arr.get(1).and_then(|x| x.as_u64()).unwrap_or(24) as u16;
                                if pty {
                                    let _ = std::io::Write::write_all(
                                        &mut dev_write, &adb::v2_winsz(c, r));
                                }
                                continue;
                            }
                        }
                        let bytes = t.as_bytes();
                        let w = if pty { adb::v2_frame(adb::V2_STDIN, bytes) } else { bytes.to_vec() };
                        if std::io::Write::write_all(&mut dev_write, &w).is_err() { break; }
                    }
                    Some(Ok(Message::Binary(b))) => {
                        let w = if pty { adb::v2_frame(adb::V2_STDIN, &b) } else { b.to_vec() };
                        if std::io::Write::write_all(&mut dev_write, &w).is_err() { break; }
                    }
                    Some(Ok(Message::Close(_))) | None => break,
                    _ => {}
                }
            }
        }
    }
    let _ = dev_write.shutdown(std::net::Shutdown::Both);
    let _ = socket.close().await;
}

// ===================== 运行模式 =====================

// ===================== 真机连接 =====================

pub async fn devices_list(State(st): State<SharedState>) -> (StatusCode, Json<Value>) {
    match tokio::task::spawn_blocking(adb::devices).await {
        Ok(Ok(ds)) => {
            let current = st.current_serial();
            let selected = crate::state::selected_serial();
            let arr: Vec<Value> = ds
                .iter()
                .map(|d| {
                    json!({
                        "serial": d.serial,
                        "state": d.state,
                        "kind": if d.serial.starts_with("emulator-") { "emulator" }
                                else if d.serial.contains(':') { "wifi" } else { "usb" },
                        "current": current.as_deref() == Some(d.serial.as_str()),
                        "selected": selected.as_deref() == Some(d.serial.as_str()),
                    })
                })
                .collect();
            ok(json!({ "devices": arr, "known": crate::state::known_devices() }))
        }
        Ok(Err(e)) => err(500, e.to_string()),
        Err(e) => err(500, format!("设备列表失败: {e}")),
    }
}

/// POST /api/devices/known/remove {serial} —— 从历史连接记忆中遗忘
pub async fn devices_known_remove(Json(v): Json<Value>) -> (StatusCode, Json<Value>) {
    let serial = v.get("serial").and_then(|x| x.as_str()).unwrap_or("").trim().to_string();
    if serial.is_empty() {
        return err(400, "缺少 serial");
    }
    crate::state::forget_device(&serial);
    ok(json!({ "ok": true }))
}

/// POST /api/devices/select {serial} —— 多设备在线时选定管理目标（持久化到 nas.json）
/// 设备运维操作（设置页开关）：action ∈
///   restart_adb    重启本容器 adb server（USB 自愈/会话清零）
///   kill_foreground 杀当前前台应用（模拟"关闭应用"）
///   wake           唤醒屏幕
///   sleep          熄屏
///   home           回主页
///   force_stop_all 强制停止全部第三方应用（清理后台）
///   grant_all_perm 给网关桥 APK 授权（如已安装）
pub async fn device_op(Json(v): Json<Value>) -> (StatusCode, Json<Value>) {
    let action = v.get("action").and_then(|x| x.as_str()).unwrap_or("");
    let Some(serial) = state::selected_serial().or_else(state::online_real_devices_first) else {
        return err(503, "设备未连接");
    };
    let (cmd, ok_msg) = match action {
        "restart_adb" => {
            // 重启 adb server：杀设备侧残留 server 进程 + host:kill 拉起重扫
            let _ = adb::shell_blocking(&serial, "pkill -f app_process", 5000);
            match adb::restart_server() {
                Ok(()) => (None, "adb server 已重启（USB 重扫完成）"),
                Err(e) => return err(500, format!("重启失败: {e}")),
            }
        }
        "kill_foreground" => (
            Some("am force-stop $(dumpsys window | grep mCurrentFocus | grep -oE 'com[[:alnum:]._]+') | head -1".to_string()),
            "已停止前台应用",
        ),
        "wake" => (Some("input keyevent KEYCODE_WAKEUP".into()), "已唤醒"),
        "sleep" => (Some("input keyevent KEYCODE_SLEEP".into()), "已熄屏"),
        "home" => (Some("input keyevent KEYCODE_HOME".into()), "已回主页"),
        "force_stop_all" => (
            Some("for p in $(pm list packages -3 | sed 's/package://'); do am force-stop $p; done".into()),
            "已停止全部第三方应用",
        ),
        _ => return err(400, "未知操作"),
    };
    if let Some(c) = cmd {
        let s = serial.clone();
        let r = tokio::task::spawn_blocking(move || adb::shell_blocking(&s, &c, 30000)).await;
        if let Ok(Err(e)) = r {
            return err(500, format!("操作失败: {e}"));
        }
    }
    ok(json!({ "ok": true, "message": ok_msg }))
}

pub async fn devices_select(Json(v): Json<Value>) -> (StatusCode, Json<Value>) {
    let serial = v
        .get("serial")
        .and_then(|x| x.as_str())
        .unwrap_or("")
        .trim()
        .to_string();
    if serial.is_empty() {
        return err(400, "缺少 serial");
    }
    let online = crate::state::online_real_devices();
    if !online.iter().any(|s| s == &serial) {
        return err(404, format!("设备不在线: {serial}"));
    }
    crate::state::set_selected_serial(&serial);
    ok(json!({ "ok": true, "serial": serial }))
}

// ===================== 设备信息（电量/温度） =====================

/// GET /api/device/info —— 全部在线设备的电量/充电状态/电池温度/温区
/// （TV 等无电池设备返回 present=false，前端显示供电状态）
pub async fn device_info() -> (StatusCode, Json<Value>) {
    let serials = state::online_real_devices();
    let r = tokio::task::spawn_blocking(move || -> Value {
        let mut map = serde_json::Map::new();
        for serial in serials {
            let out = adb::shell_blocking(
                &serial,
                "dumpsys battery; echo __TH__; \
                 for z in /sys/class/thermal/thermal_zone*; do \
                   t=$(cat $z/type 2>/dev/null); v=$(cat $z/temp 2>/dev/null); \
                   [ -n \"$v\" ] && echo \"$t $v\"; done 2>/dev/null | head -6",
                8000,
            );
            let text = match out {
                Ok(o) => String::from_utf8_lossy(&o).to_string(),
                Err(_) => continue,
            };
            let g = |k: &str| -> Option<i64> {
                text.lines().find_map(|l| {
                    let l = l.trim();
                    l.starts_with(k).then(|| l[k.len()..].trim().parse().ok())?
                })
            };
            let status_code = g("status:").unwrap_or(0);
            let status = match status_code {
                2 => "charging",
                3 => "discharging",
                4 => "not_charging",
                5 => "full",
                _ => "unknown",
            };
            let thermal: Vec<Value> = text
                .split("__TH__")
                .nth(1)
                .map(|th| {
                    th.lines()
                        .filter_map(|l| {
                            let mut it = l.trim().split(' ');
                            let t = it.next()?.to_string();
                            let v: i64 = it.next()?.parse().ok()?;
                            Some(json!({ "type": t, "temp_c": v as f64 / 1000.0 }))
                        })
                        .collect()
                })
                .unwrap_or_default();
            map.insert(
                serial,
                json!({
                    // 电池温度单位 0.1°C
                    "battery": {
                        "present": text.contains("present: true"),
                        "level": g("level:"),
                        "status": status,
                        "voltage_mv": g("voltage:"),
                        "temp_c": g("temperature:").map(|t| t as f64 / 10.0),
                        "usb_powered": text.contains("USB powered: true"),
                        "ac_powered": text.contains("AC powered: true"),
                    },
                    "thermal": thermal,
                }),
            );
        }
        Value::Object(map)
    })
    .await;
    match r {
        Ok(v) => ok(v),
        Err(e) => err(500, format!("设备信息失败: {e}")),
    }
}

// ===================== 家长管控（防沉迷） =====================

/// GET /api/guard?serial= —— 指定设备的管控配置 + 实时屏幕 + 今日用量 + 近 30 天历史 + 事件；
/// 附全部在线设备清单（各设备独立管控，供前端切换）
pub async fn guard_get(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
) -> (StatusCode, Json<Value>) {
    let serial = match q.get("serial").filter(|s| !s.is_empty()) {
        Some(s) => s.clone(),
        None => match st.current_serial() {
            Some(s) => s,
            None => return err(503, "Android 未连接"),
        },
    };
    let cfg = crate::guard::cfg_get(&serial);
    let probe_serial = serial.clone();
    let probe = tokio::task::spawn_blocking(move || crate::guard::probe(&probe_serial)).await;
    let (today, _minute) = crate::guard::local_today_minute();
    let screen = match probe {
        Ok(Ok(p)) => json!({ "on": p.screen_on, "wakefulness": p.wakefulness, "foreground": p.foreground }),
        _ => json!(null),
    };
    let history: Vec<Value> = crate::guard::usage_history(&serial, 30)
        .into_iter()
        .map(|(d, m)| json!({ "date": d, "min": m }))
        .collect();
    // 各在线设备的管控状态一览（不在线的设备配置仍在，但不执行）
    let devices: Vec<Value> = crate::state::online_real_devices()
        .into_iter()
        .map(|s| {
            let c = crate::guard::cfg_get(&s);
            json!({ "serial": s, "enabled": c.enabled, "current": s == serial })
        })
        .collect();
    let last_check = crate::guard::LAST_CHECK
        .lock()
        .unwrap()
        .get(&serial)
        .copied();
    ok(json!({
        "serial": serial,
        "enabled": cfg.enabled,
        "windows": cfg.windows,
        "daily_limit_min": cfg.daily_limit_min,
        "blocked_apps": cfg.blocked_apps,
        "block_internet": cfg.block_internet,
        "today_used_min": crate::guard::usage_secs(&serial, &today) / 60,
        "in_window": crate::guard::in_window_now(&cfg.windows),
        "screen": screen,
        "last_check_ms": last_check,
        "usage_history": history,
        "devices": devices,
        "events": crate::guard::events_recent(&serial, 50),
    }))
}

#[derive(serde::Deserialize)]
pub struct GuardSetReq {
    #[serde(default)]
    pub serial: Option<String>,
    #[serde(flatten)]
    pub cfg: crate::guard::GuardCfg,
}

/// POST /api/guard —— 更新指定设备（缺省当前设备）的管控配置；
/// 设备离线时仅保存（上线后巡检会补执行外网封禁）
pub async fn guard_set(
    State(st): State<SharedState>,
    Json(v): Json<GuardSetReq>,
) -> (StatusCode, Json<Value>) {
    let serial = match v.serial.clone().filter(|s| !s.is_empty()) {
        Some(s) => s,
        None => match st.current_serial() {
            Some(s) => s,
            None => return err(503, "Android 未连接"),
        },
    };
    if !serial.chars().all(|c| c.is_ascii_alphanumeric() || ".:-_".contains(c)) {
        return err(400, "serial 非法");
    }
    for w in &v.cfg.windows {
        if !w.contains('-') || w.len() != 11 {
            return err(400, format!("时段格式应为 HH:MM-HH:MM：{w}"));
        }
    }
    let online = crate::state::online_real_devices();
    let is_online = online.iter().any(|s| s == &serial);
    let cfg = crate::guard::cfg_set(&serial, &v.cfg, is_online);
    ok(json!({
        "ok": true,
        "serial": serial,
        "enabled": cfg.enabled,
        "windows": cfg.windows,
        "daily_limit_min": cfg.daily_limit_min,
        "blocked_apps": cfg.blocked_apps,
        "block_internet": cfg.block_internet,
    }))
}

/// POST /api/devices/pair {host, port, code, connect_port?}
pub async fn devices_pair(Json(v): Json<Value>) -> (StatusCode, Json<Value>) {
    let host = v.get("host").and_then(|x| x.as_str()).unwrap_or("").trim().to_string();
    // port 兼容数字与字符串（表单控件可能给字符串数字）
    let num = |x: Option<&Value>| -> Option<u64> {
        x.and_then(|v| {
            v.as_u64()
                .or_else(|| v.as_str().and_then(|s| s.trim().parse::<u64>().ok()))
        })
    };
    let port = num(v.get("port")).unwrap_or(0) as u16;
    let code = v.get("code").and_then(|x| x.as_str()).unwrap_or("").trim().to_string();
    let connect_port = num(v.get("connect_port")).map(|p| p as u16);
    if host.is_empty() || port == 0 || code.is_empty() {
        return err(400, "缺少 host/port/code");
    }
    let r = tokio::task::spawn_blocking(move || -> Result<String, String> {
        let pair_addr = format!("{host}:{port}");
        // 配对返回 paired:<GUID>，用于跨网段匹配连接服务
        let paired = adb::adb_pair(&pair_addr, &code).map_err(|e| e.to_string())?;
        let guid = paired.strip_prefix("paired:").map(|s| s.to_string());
        // 连接：显式 connect_port > mDNS 候选（TLS connect > _adb._tcp > 5555）。
        // 配对后 adbd 要几秒才重新通告 connect 服务，重试等待
        if let Some(cp) = connect_port {
            let t = format!("{host}:{cp}");
            return match adb::adb_connect(&t) {
                Ok(_) => Ok(t),
                Err(e) => Err(format!("配对成功但连接失败（{t}: {e}）")),
            };
        }
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(18);
        let mut last_err = String::new();
        loop {
            for t in adb::wireless_connect_candidates(&host, &[], guid.as_deref()) {
                match adb::adb_connect(&t) {
                    Ok(_) => return Ok(t),
                    Err(e) => last_err = format!("{t}: {e}"),
                }
            }
            if std::time::Instant::now() >= deadline {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(1500));
        }
        Err(format!("配对成功但连接失败（{last_err}）"))
    })
    .await;
    match r {
        Ok(Ok(target)) => ok(json!({ "ok": true, "device": target })),
        Ok(Err(e)) => err(500, e),
        Err(e) => err(500, format!("配对任务失败: {e}")),
    }
}

/// POST /api/devices/connect {addr: "192.168.1.5:5555"}
pub async fn devices_connect(Json(v): Json<Value>) -> (StatusCode, Json<Value>) {
    let addr = v.get("addr").and_then(|x| x.as_str()).unwrap_or("").trim().to_string();
    if addr.is_empty() || !addr.contains(':') {
        return err(400, "addr 须为 IP:端口");
    }
    let addr_for_memory = addr.clone();
    let r = tokio::task::spawn_blocking(move || adb::adb_connect(&addr)).await;
    match r {
        Ok(Ok(_)) => {
            state::remember_device(&addr_for_memory); // 回连记忆（容器内 adb 重启后自动恢复）
            ok(json!({ "ok": true }))
        }
        Ok(Err(e)) => err(500, e.to_string()),
        Err(e) => err(500, format!("连接失败: {e}")),
    }
}

/// POST /api/devices/disconnect {serial}
pub async fn devices_disconnect(Json(v): Json<Value>) -> (StatusCode, Json<Value>) {
    let serial = v.get("serial").and_then(|x| x.as_str()).unwrap_or("").to_string();
    if serial.is_empty() {
        return err(400, "缺少 serial");
    }
    let r = tokio::task::spawn_blocking(move || adb::adb_disconnect(&serial)).await;
    match r {
        Ok(Ok(_)) => ok(json!({ "ok": true })),
        Ok(Err(e)) => err(500, e.to_string()),
        Err(e) => err(500, format!("断开失败: {e}")),
    }
}

/// GET /api/devices/discovered → mDNS 发现的服务列表
pub async fn devices_discovered() -> (StatusCode, Json<Value>) {
    let r = tokio::task::spawn_blocking(adb::mdns_services).await;
    match r {
        Ok(list) => ok(json!({
            "services": list.iter().map(|s| json!({
                "name": s.name, "type": s.svc_type, "addr": s.addr,
            })).collect::<Vec<_>>()
        })),
        Err(e) => err(500, format!("发现失败: {e}")),
    }
}

pub async fn hostpair_begin(State(st): State<SharedState>) -> (StatusCode, Json<Value>) {
    match crate::devices::hostpair_begin(&st.hostpair, 180_000) {
        Ok(st) => ok(st),
        Err(e) => err(500, e.to_string()),
    }
}

pub async fn hostpair_get(State(st): State<SharedState>) -> Json<Value> {
    Json(crate::devices::hostpair_status(&st.hostpair))
}

pub async fn hostpair_cancel(State(st): State<SharedState>) -> Json<Value> {
    Json(json!({ "ok": crate::devices::hostpair_cancel(&st.hostpair) }))
}

// ===================== 运行时下载 =====================

// ===================== SSE 事件流（替代轮询） =====================

use axum::response::sse::{Event as SseEvent, KeepAlive, Sse};
use std::convert::Infallible;

/// GET /api/events/status —— 状态变化即推（2s 巡检），15s 心跳保活
pub async fn events_status(
    State(st): State<SharedState>,
) -> Sse<impl futures_util::Stream<Item = Result<SseEvent, Infallible>>> {
    let (tx, rx) = tokio::sync::mpsc::channel::<Result<SseEvent, Infallible>>(8);
    tokio::spawn(async move {
        let mut last = String::new();
        loop {
            // 基础状态 + 当前设备快照（机型/CPU/内存/电量/温度/运行时长）。
            // cpu 每轮几乎必变 → 实际按巡检周期（3s）推新；无设备时退化为纯状态。
            let mut body = status(State(st.clone())).await.0;
            if let Some(serial) = st.current_serial() {
                let st2 = st.clone();
                let snap = tokio::task::spawn_blocking(move || dev_snapshot(&st2, &serial)).await;
                if let Ok(v) = snap {
                    if let (Some(o), Some(v2)) = (body.as_object_mut(), v.as_object()) {
                        for (k, val) in v2 {
                            o.insert(k.clone(), val.clone());
                        }
                    }
                }
            }
            let body = body.to_string();
            if body != last {
                last = body.clone();
                if tx.send(Ok(SseEvent::default().data(body))).await.is_err() {
                    return;
                }
            }
            tokio::time::sleep(std::time::Duration::from_secs(3)).await;
        }
    });
    Sse::new(tokio_stream::wrappers::ReceiverStream::new(rx))
        .keep_alive(KeepAlive::new().interval(std::time::Duration::from_secs(15)))
}

/// GET /api/events/device —— 设备事件合流 SSE：
///   {type:"notification", pkg,title,text,key,postTime}  桥 APK 实时推送
///   {type:"clipboard", text}                             活跃投屏会话的剪贴板（scrcpy 自动同步）
/// 剪贴板每 2s 采样比对（有投屏会话才有源）；通知为纯事件驱动零轮询。
/// GET /api/sms/list?box=inbox|sent|draft|all&limit= —— 设备短信（content://sms 经 shell content query）。
/// 输出 JSON 行解析：Row: address=..., body=..., date=..., type=...
pub async fn sms_list(Query(q): Query<std::collections::HashMap<String, String>>) -> (StatusCode, Json<Value>) {
    let box_ = q.get("box").cloned().unwrap_or_else(|| "all".into());
    let limit = q.get("limit").and_then(|s| s.parse::<u32>().ok()).unwrap_or(200).min(500);
    let uri = match box_.as_str() {
        "inbox" => "content://sms/inbox",
        "sent" => "content://sms/sent",
        "draft" => "content://sms/draft",
        _ => "content://sms",
    };
    let serial = match state::selected_serial().or_else(state::online_real_devices_first) {
        Some(s) => s,
        None => return err(503, "设备未连接"),
    };
    let cmd = format!(
        "content query --uri {uri} --projection _id:thread_id:address:person:body:date:type:read --sort date DESC 2>/dev/null | head -{limit}"
    );
    let r = tokio::task::spawn_blocking(move || adb::shell_blocking(&serial, &cmd, 20000)).await;
    let out = match r {
        Ok(Ok(o)) => o,
        Ok(Err(e)) => return err(500, format!("读取短信失败: {e}")),
        Err(e) => return err(500, format!("任务失败: {e}")),
    };
    let text = String::from_utf8_lossy(&out);
    let mut msgs = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if !line.starts_with("Row:") {
            continue;
        }
        let mut m = serde_json::Map::new();
        for kv in line[4..].split(", ") {
            if let Some((k, v)) = kv.split_once('=') {
                m.insert(k.trim().to_string(), json!(v));
            }
        }
        if m.contains_key("_id") {
            msgs.push(Value::Object(m));
        }
    }
    ok(json!({ "msgs": msgs, "box": box_ }))
}

/// POST /api/sms/send {to, body} —— 发送短信：调起设备短信 App 预填（smsto: 意图），
/// 用户在设备上点发送键确认（adb 无法跨过系统发送确认，属安全设计）。
pub async fn sms_send(Json(v): Json<Value>) -> (StatusCode, Json<Value>) {
    let to = v.get("to").and_then(|x| x.as_str()).unwrap_or("").to_string();
    let body = v.get("body").and_then(|x| x.as_str()).unwrap_or("").to_string();
    if to.is_empty() {
        return err(400, "缺少收件人");
    }
    let serial = match state::selected_serial().or_else(state::online_real_devices_first) {
        Some(s) => s,
        None => return err(503, "设备未连接"),
    };
    // shell 转义（单引号包裹，内部单引号成对）
    let esc = |s: &str| s.replace('\'', "'\''");
    let cmd = format!(
        "am start -a android.intent.action.SENDTO -d 'smsto:{}' --es sms_body '{}'",
        esc(&to),
        esc(&body)
    );
    let r = tokio::task::spawn_blocking(move || adb::shell_blocking(&serial, &cmd, 10000)).await;
    match r {
        Ok(Ok(_)) => ok(json!({ "ok": true, "message": "已在设备上打开短信编辑（请在设备上点发送确认）" })),
        Ok(Err(e)) => err(500, format!("拉起短信失败: {e}")),
        Err(e) => err(500, format!("任务失败: {e}")),
    }
}

/// GET /api/about —— 关于信息：版本/项目地址/开源组件清单（编译期从
/// Cargo.lock 嵌入，随镜像走，无需运行时读文件）
pub async fn about() -> Json<Value> {
    json!({
        "version": env!("CARGO_PKG_VERSION"),
        "project": "https://github.com/Apuyuseng/houttuynia",
        "license": "MIT",
        "components": ABOUT_COMPONENTS,
    }).into()
}

/// 编译期嵌入的开源组件清单（name version license）
static ABOUT_COMPONENTS: &[(&str, &str, &str)] = &[
    ("axum", "0.8", "MIT"),
    ("tokio", "1", "MIT"),
    ("tower-http", "0.6", "MIT"),
    ("rustls", "0.23", "Apache-2.0/ISC/MIT"),
    ("rusqlite", "0.31", "MIT"),
    ("rsa", "0.9", "MIT/Apache-2.0"),
    ("nusb (macOS)", "0.1", "MIT/Apache-2.0"),
    ("scrcpy server", "4.1", "Apache-2.0"),
    ("xterm.js", "5", "MIT"),
    ("vue", "3", "MIT"),
    ("element-plus", "2", "MIT"),
    ("material-design-icons", "-", "Apache-2.0"),
];

pub async fn events_device(
    State(st): State<SharedState>,
) -> Sse<impl futures_util::Stream<Item = Result<SseEvent, Infallible>>> {
    tracing::info!("[events_device] handler 进入");
    crate::bridge::ensure_started();
    let (tx, rx) = tokio::sync::mpsc::channel::<Result<SseEvent, Infallible>>(16);
    let mut bus_rx = crate::bridge::BUS.subscribe();
    let tx_n = tx.clone();
    let st2 = st.clone();
    // 通知转发
    tokio::spawn(async move {
        let tx = tx_n;
        loop {
            match bus_rx.recv().await {
                Ok(line) => {
                    let mut v: Value = serde_json::from_str(&line).unwrap_or(Value::Null);
                    if v.is_object() {
                        v["type"] = json!("notification");
                        if tx.send(Ok(SseEvent::default().data(v.to_string()))).await.is_err() {
                            return;
                        }
                    }
                }
                Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => continue,
                Err(_) => return,
            }
        }
    });
    // 剪贴板采样（hello 作为该任务首帧下行）
    let tx_c = tx.clone();
    tokio::spawn(async move {
        let tx = tx_c;
        tx.send(Ok(SseEvent::default().data(
            json!({ "type": "hello" }).to_string(),
        )))
        .await
        .ok();
        let mut last = String::new();
        loop {
            tokio::time::sleep(std::time::Duration::from_secs(2)).await;
            let mut text = String::new();
            if let Ok(streams) = st2.streams.try_lock() {
                for s in streams.values() {
                    if let Ok(c) = s.clipboard.try_lock() {
                        if !c.is_empty() {
                            text = c.clone();
                            break;
                        }
                    }
                }
            }
            if !text.is_empty() && text != last {
                last = text.clone();
                let body = json!({ "type": "clipboard", "text": text }).to_string();
                if tx.send(Ok(SseEvent::default().data(body))).await.is_err() {
                    return;
                }
            }
        }
    });
    // hello 在剪贴板任务里作为首帧下行（handler 返回后 axum 才开始消费流——
    // handler 同步段 send 的帧会丢失，教训：SSE 首帧必须在 spawn 内发）
    Sse::new(tokio_stream::wrappers::ReceiverStream::new(rx))
        .keep_alive(KeepAlive::new().interval(std::time::Duration::from_secs(15)))
}

/// GET /api/events/apps —— 应用列表：首次全量 + 缓存（标签/图标）变化即推；
/// 设备侧包集合每 30s 慢速复查（真机 Wi-Fi 上不为列表狂刷 pm）
pub async fn events_apps(
    State(st): State<SharedState>,
) -> Sse<impl futures_util::Stream<Item = Result<SseEvent, Infallible>>> {
    let (tx, rx) = tokio::sync::mpsc::channel::<Result<SseEvent, Infallible>>(8);
    tokio::spawn(async move {
        tracing::info!("events_apps: 任务启动");
        let mut last_body = String::new();
        let mut last_device_check = std::time::Instant::now() - std::time::Duration::from_secs(60);
        let mut last_cache_stamp = String::new();
        loop {
            let need_device = last_device_check.elapsed() > std::time::Duration::from_secs(30);
            let body = if need_device || last_body.is_empty() {
                last_device_check = std::time::Instant::now();
                match apps(State(st.clone())).await {
                    (_, Json(v)) => v.to_string(),
                }
            } else {
                // 只复查本地缓存（标签/图标落盘后列表内容变化）
                let stamp = cache_stamp();
                if stamp != last_cache_stamp {
                    // 缓存变了但设备没查：用缓存标签重组上一次的包集合
                    if let Ok(mut v) = serde_json::from_str::<Value>(&last_body) {
                        if let Some(arr) = v.get_mut("apps").and_then(|a| a.as_array_mut()) {
                            for a in arr.iter_mut() {
                                let pkg = a
                                    .get("package")
                                    .and_then(|p| p.as_str())
                                    .map(String::from);
                                if let Some(pkg) = pkg {
                                    let label = crate::icons::app_label_cached_only(&pkg);
                                    let has_icon = crate::icons::icon_cached(&pkg);
                                    if let Some(o) = a.as_object_mut() {
                                        if let Some(l) = label {
                                            o.insert("name".into(), json!(l));
                                        }
                                        o.insert("has_icon".into(), json!(has_icon));
                                    }
                                }
                            }
                        }
                        v.to_string()
                    } else {
                        tokio::time::sleep(std::time::Duration::from_secs(3)).await;
                        continue;
                    }
                } else {
                    tokio::time::sleep(std::time::Duration::from_secs(3)).await;
                    continue;
                }
            };
            last_cache_stamp = cache_stamp();
            if body != last_body {
                tracing::info!("events_apps: 推送 {} 字节", body.len());
                last_body = body.clone();
                if tx.send(Ok(SseEvent::default().data(body))).await.is_err() {
                    return;
                }
            } else {
                tokio::time::sleep(std::time::Duration::from_secs(3)).await;
            }
        }
    });
    Sse::new(tokio_stream::wrappers::ReceiverStream::new(rx))
        .keep_alive(KeepAlive::new().interval(std::time::Duration::from_secs(15)))
}

/// 本地标签/图标缓存戳（labels.json mtime + icons 目录最大 mtime）
fn cache_stamp() -> String {
    let home = std::env::var("HOME").unwrap_or_default();
    let base = format!("{home}/.mwine/appcache/rust");
    let mut stamp = String::new();
    for p in [format!("{base}/labels.json")] {
        if let Ok(m) = std::fs::metadata(&p).and_then(|m| m.modified()) {
            stamp += &format!("{:?};", m);
        }
    }
    if let Ok(rd) = std::fs::read_dir(format!("{base}/icons")) {
        for e in rd.flatten() {
            if let Ok(t) = e.metadata().and_then(|m| m.modified()) {
                stamp += &format!("{:?},", t);
            }
        }
    }
    stamp
}
