//! 真机连接：扫码配对（QR + mDNS 发现 + adb pair/connect）、配对码、手动、断开。
//! 流程对齐桌面版 devmgr.cpp hostpair：
//!   出码 WIFI:T:ADB;S:<service>;P:<password>;; → 手机扫码后以该服务名通告
//!   _adb-tls-pairing._tcp → 发现后 adb pair（手机是配对服务端）→
//!   等新的 _adb._tcp 出现 → adb connect。

use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};

use serde_json::{json, Value};

use crate::adb::{self, AdbResult};

fn err<T>(msg: impl Into<String>) -> AdbResult<T> {
    Err(adb::AdbError(msg.into()))
}

pub struct HostPairState {
    pub state: Mutex<Value>,   // 完整状态 JSON（前端轮询）
    pub cancel: Arc<AtomicBool>,
}

pub struct HostPairManager {
    pub current: Mutex<Option<Arc<HostPairState>>>,
}

impl HostPairManager {
    pub fn new() -> Self {
        Self { current: Mutex::new(None) }
    }
}

fn random_token() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let t = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.subsec_nanos() as u64 ^ (d.as_secs() << 20))
        .unwrap_or(0);
    let mut v = t ^ (std::process::id() as u64) ^ 0x9e3779b97f4a7c15;
    v ^= v >> 29;
    v = v.wrapping_mul(0xbf58476d1ce4e5b9);
    format!("{:06x}", (v & 0xFF_FFFF) as u32)
}

/// QR 载荷矩阵（qrcode crate 生成 '0'/'1' 行，与 C++ qrMatrixRows 同形）
fn qr_rows(payload: &str) -> Vec<String> {
    let code = match qrcode::QrCode::new(payload.as_bytes()) {
        Ok(c) => c,
        Err(_) => return Vec::new(),
    };
    let w = code.width();
    let data = code.into_colors();
    (0..w)
        .map(|y| {
            (0..w)
                .map(|x| if data[y * w + x] == qrcode::Color::Dark { '1' } else { '0' })
                .collect::<String>()
        })
        .collect()
}

/// 开始扫码配对会话：返回初始状态（含 qr 矩阵行）
pub fn hostpair_begin(mgr: &HostPairManager, wait_ms: u64) -> AdbResult<Value> {
    // 已有等待中的会话则直接复用
    {
        let cur = mgr.current.lock().unwrap();
        if let Some(s) = cur.as_ref() {
            let st = s.state.lock().unwrap().clone();
            if st.get("state").and_then(|v| v.as_str()) == Some("waiting") {
                return Ok(st);
            }
        }
    }
    let service = format!("studio-{}", random_token());
    let password = random_token();
    let qr_payload = format!("WIFI:T:ADB;S:{service};P:{password};;");
    let rows = qr_rows(&qr_payload);

    let st = json!({
        "service": service,
        "qr": qr_payload,
        "rows": rows,
        "state": "waiting",
        "message": "等待手机扫码…（手机「无线调试 → 使用二维码配对设备」扫此码）",
    });
    let hs = Arc::new(HostPairState {
        state: Mutex::new(st.clone()),
        cancel: Arc::new(AtomicBool::new(false)),
    });
    *mgr.current.lock().unwrap() = Some(hs.clone());

    std::thread::spawn(move || hostpair_worker(hs, service, password, wait_ms));
    Ok(st)
}

pub fn hostpair_status(mgr: &HostPairManager) -> Value {
    mgr.current
        .lock()
        .unwrap()
        .as_ref()
        .map(|s| s.state.lock().unwrap().clone())
        .unwrap_or_else(|| json!({ "state": "idle" }))
}

pub fn hostpair_cancel(mgr: &HostPairManager) -> bool {
    let cur = mgr.current.lock().unwrap();
    if let Some(s) = cur.as_ref() {
        if s.state.lock().unwrap().get("state").and_then(|v| v.as_str()) == Some("waiting") {
            s.cancel.store(true, Ordering::SeqCst);
            return true;
        }
    }
    false
}

fn set_state(hs: &HostPairState, state: &str, message: &str, extra: &[(&str, Value)]) {
    let mut st = hs.state.lock().unwrap();
    st.as_object_mut().map(|o| {
        o.insert("state".into(), json!(state));
        o.insert("message".into(), json!(message));
        for (k, v) in extra {
            o.insert((*k).into(), v.clone());
        }
    });
}

fn hostpair_worker(hs: Arc<HostPairState>, service: String, password: String, wait_ms: u64) {
    let t0 = std::time::Instant::now();
    let deadline = t0 + std::time::Duration::from_millis(wait_ms);

    // 配对前已在的 _adb._tcp 地址（配对后新出现的才是本机手机）
    let before: Vec<String> = adb::mdns_services()
        .into_iter()
        .filter(|s| s.svc_type.starts_with("_adb._tcp"))
        .map(|s| s.addr)
        .collect();

    // 1) 等手机扫码
    let mut pair_addr: Option<String> = None;
    while std::time::Instant::now() < deadline {
        if hs.cancel.load(Ordering::SeqCst) {
            set_state(&hs, "cancelled", "已取消", &[]);
            return;
        }
        for svc in adb::mdns_services() {
            if svc.svc_type.contains("adb-tls-pairing") && svc.name == service {
                pair_addr = Some(svc.addr.clone());
                break;
            }
        }
        if pair_addr.is_some() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(1200));
    }
    let Some(addr) = pair_addr else {
        set_state(&hs, "failed", "等待手机扫码超时（未发现配对服务）", &[]);
        return;
    };

    // 2) adb pair（手机是配对服务端，密码即 QR 的 P 字段）
    set_state(&hs, "pairing", &format!("发现配对服务 {addr}，配对中…"), &[]);
    if let Err(e) = adb::adb_pair(&addr, &password) {
        set_state(&hs, "failed", &e.to_string(), &[]);
        return;
    }

    // 3) 等 _adb._tcp 新地址 → connect
    set_state(&hs, "connecting", "配对成功，连接设备…", &[]);
    let phone_ip = addr.split(':').next().unwrap_or("").to_string();
    let cdeadline = std::time::Instant::now() + std::time::Duration::from_secs(25);
    while std::time::Instant::now() < cdeadline {
        for svc in adb::mdns_services() {
            if !svc.svc_type.starts_with("_adb._tcp") || before.contains(&svc.addr) {
                continue;
            }
            let host = svc.addr.split(':').next().unwrap_or("");
            if host != phone_ip {
                continue;
            }
            match adb::adb_connect(&svc.addr) {
                Ok(_) => {
                    set_state(
                        &hs,
                        "connected",
                        &format!("已连接 {}", svc.addr),
                        &[("device", json!(svc.addr))],
                    );
                    return;
                }
                Err(_) => std::thread::sleep(std::time::Duration::from_millis(1500)),
            }
        }
        // 兜底：同 IP 5555
        if let Ok(_) = adb::adb_connect(&format!("{phone_ip}:5555")) {
            set_state(
                &hs,
                "connected",
                &format!("已连接 {phone_ip}:5555"),
                &[("device", json!(format!("{phone_ip}:5555")))],
            );
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(1200));
    }
    set_state(&hs, "failed", "配对成功但连接超时（在手机上打开「无线调试」主界面看端口后用手动连接）", &[]);
}
