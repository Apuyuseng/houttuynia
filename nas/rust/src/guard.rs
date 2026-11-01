//! 家长管控（防沉迷）：屏幕状态检测 + 非待机时长/时段管控 + 应用黑名单 + 外网封禁。
//!
//! 检测（每 5s 一轮，单次 shell 合并取证，减少真机交互）：
//!   dumpsys power                → mWakefulness=Awake/Asleep/Dozing/Dreaming
//!   dumpsys activity activities  → mResumedActivity/topResumedActivity 前台包名
//! 执行（全部经 adb shell，非 root 可用，TCL TV 实测）：
//!   强制待机 input keyevent 223（唤醒 224）
//!   踢杀黑名单应用 am force-stop
//!   提示 cmd notification post（shell 身份发真实系统通知）
//! 外网封禁（可逆、不动 WiFi——wifi 一断 adb 通道就没了）：
//!   全局 http_proxy 指向黑洞端口 + private_dns 严格模式指向不可达主机；
//!   解除时两者恢复 null。原生 socket 流量不受代理约束（非 root 方案边界）。
//! 时区：日期/时段按本地时间计算；容器无 zoneinfo，默认 UTC+8，
//! 可用 MWINE_TZ_OFFSET_MIN 覆盖。

use std::collections::HashMap;
use std::sync::Mutex;

use crate::adb::{self, AdbResult};
use serde::{Deserialize, Serialize};
use serde_json::json;

#[derive(Serialize, Deserialize, Clone, Default)]
pub struct GuardEvent {
    pub time: i64, // ms
    pub kind: String, // sleep_forced / wake_detected / app_blocked / internet_on / internet_off / config
    pub detail: String,
}

#[derive(Serialize, Deserialize, Clone)]
pub struct GuardCfg {
    pub enabled: bool,
    pub windows: Vec<String>, // "HH:MM-HH:MM"，支持跨零点（20:00-06:30）；空 = 全天允许
    pub daily_limit_min: u32, // 0 = 不限
    pub blocked_apps: Vec<String>,
    pub block_internet: bool,
}

impl Default for GuardCfg {
    fn default() -> Self {
        GuardCfg {
            enabled: false,
            windows: Vec::new(),
            daily_limit_min: 0,
            blocked_apps: Vec::new(),
            block_internet: false,
        }
    }
}

// ---------------- SQLite 存储（配置/用量/事件；WAL 事务化防崩溃损坏） ----------------

use rusqlite::Connection;

fn db() -> &'static Mutex<Connection> {
    static DB: std::sync::LazyLock<Mutex<Connection>> = std::sync::LazyLock::new(|| {
        let home = std::env::var("HOME").unwrap_or_default();
        let path = format!("{home}/.mwine/guard.db");
        let conn = match Connection::open(&path) {
            Ok(c) => c,
            Err(e) => {
                // 打不开（卷只读等）：内存库降级——服务可用，数据不落盘
                tracing::error!("guard.db 打开失败（降级内存库，数据不持久）: {e}");
                Connection::open_in_memory().expect("内存库")
            }
        };
        let _ = conn.pragma_update(None, "journal_mode", "WAL");
        let _ = conn.pragma_update(None, "synchronous", "NORMAL");
        let _ = conn.execute_batch(
            "CREATE TABLE IF NOT EXISTS guard_config (
               serial TEXT PRIMARY KEY,
               enabled INTEGER NOT NULL DEFAULT 0,
               windows TEXT NOT NULL DEFAULT '[]',
               daily_limit_min INTEGER NOT NULL DEFAULT 0,
               blocked_apps TEXT NOT NULL DEFAULT '[]',
               block_internet INTEGER NOT NULL DEFAULT 0);
             CREATE TABLE IF NOT EXISTS guard_usage (
               serial TEXT NOT NULL,
               date TEXT NOT NULL,
               secs INTEGER NOT NULL DEFAULT 0,
               PRIMARY KEY (serial, date));
             CREATE TABLE IF NOT EXISTS guard_events (
               id INTEGER PRIMARY KEY AUTOINCREMENT,
               serial TEXT NOT NULL,
               time INTEGER NOT NULL,
               kind TEXT NOT NULL,
               detail TEXT NOT NULL);
             CREATE INDEX IF NOT EXISTS idx_guard_events_serial
               ON guard_events (serial, id DESC);",
        );
        migrate_json(&conn);
        Mutex::new(conn)
    });
    &DB
}

/// 旧版 guard.json 一次性迁移（成功后改名 .migrated 保留为备份）
fn migrate_json(conn: &Connection) {
    let home = std::env::var("HOME").unwrap_or_default();
    let path = format!("{home}/.mwine/guard.json");
    let Ok(text) = std::fs::read_to_string(&path) else { return };
    let Ok(v) = serde_json::from_str::<serde_json::Value>(&text) else { return };
    let Some(map) = v.as_object() else { return };
    for (serial, o) in map {
        let g = |k: &str| o.get(k);
        let cfg = GuardCfg {
            enabled: g("enabled").and_then(|x| x.as_bool()).unwrap_or(false),
            windows: serde_json::from_value(g("windows").cloned().unwrap_or(json!([]))).unwrap_or_default(),
            daily_limit_min: g("daily_limit_min").and_then(|x| x.as_u64()).unwrap_or(0) as u32,
            blocked_apps: serde_json::from_value(g("blocked_apps").cloned().unwrap_or(json!([]))).unwrap_or_default(),
            block_internet: g("block_internet").and_then(|x| x.as_bool()).unwrap_or(false),
        };
        let _ = conn.execute(
            "INSERT OR REPLACE INTO guard_config VALUES (?1,?2,?3,?4,?5,?6)",
            rusqlite::params![
                serial, cfg.enabled as i32,
                serde_json::to_string(&cfg.windows).unwrap_or_default(),
                cfg.daily_limit_min,
                serde_json::to_string(&cfg.blocked_apps).unwrap_or_default(),
                cfg.block_internet as i32
            ],
        );
        if let Some(usage) = g("usage").and_then(|x| x.as_object()) {
            for (date, secs) in usage {
                if let Some(secs) = secs.as_u64() {
                    let _ = conn.execute(
                        "INSERT OR REPLACE INTO guard_usage VALUES (?1,?2,?3)",
                        rusqlite::params![serial, date, secs as i64],
                    );
                }
            }
        }
        if let Some(events) = g("events").and_then(|x| x.as_array()) {
            for e in events {
                let _ = conn.execute(
                    "INSERT INTO guard_events (serial,time,kind,detail) VALUES (?1,?2,?3,?4)",
                    rusqlite::params![
                        serial,
                        e.get("time").and_then(|x| x.as_i64()).unwrap_or(0),
                        e.get("kind").and_then(|x| x.as_str()).unwrap_or(""),
                        e.get("detail").and_then(|x| x.as_str()).unwrap_or("")
                    ],
                );
            }
        }
    }
    let _ = std::fs::rename(&path, format!("{path}.migrated"));
    tracing::info!("guard.json 已迁移至 SQLite（备份为 guard.json.migrated）");
}

pub fn cfg_get(serial: &str) -> GuardCfg {
    let db = db().lock().unwrap();
    let row = conn_row(&db, serial);
    match row {
        Some((enabled, windows, limit, blocked, net)) => GuardCfg {
            enabled,
            windows: serde_json::from_str(&windows).unwrap_or_default(),
            daily_limit_min: limit as u32,
            blocked_apps: serde_json::from_str(&blocked).unwrap_or_default(),
            block_internet: net,
        },
        None => GuardCfg::default(),
    }
}

fn conn_row(db: &Connection, serial: &str) -> Option<(bool, String, i64, String, bool)> {
    db.query_row(
        "SELECT enabled, windows, daily_limit_min, blocked_apps, block_internet          FROM guard_config WHERE serial = ?1",
        [serial],
        |r| {
            Ok((
                r.get::<_, i32>(0)? != 0,
                r.get::<_, String>(1)?,
                r.get::<_, i64>(2)?,
                r.get::<_, String>(3)?,
                r.get::<_, i32>(4)? != 0,
            ))
        },
    )
    .ok()
}

/// 更新规则；外网封禁状态迁移即时生效（设备离线仅保存——上线后保活逻辑补执行）
pub fn cfg_set(serial: &str, incoming: &GuardCfg, online: bool) -> GuardCfg {
    {
        let mut db = db().lock().unwrap();
        let _ = db.execute(
            "INSERT OR REPLACE INTO guard_config VALUES (?1,?2,?3,?4,?5,?6)",
            rusqlite::params![
                serial,
                incoming.enabled as i32,
                serde_json::to_string(&incoming.windows).unwrap_or_default(),
                incoming.daily_limit_min,
                serde_json::to_string(&incoming.blocked_apps).unwrap_or_default(),
                incoming.block_internet as i32
            ],
        );
    }
    let was_block = cfg_get(serial).block_internet;
    if was_block != incoming.block_internet && online {
        let r = if incoming.block_internet {
            internet_block(serial)
        } else {
            internet_unblock(serial)
        };
        let detail = format!("外网封禁{}：{}", if incoming.block_internet { "开启" } else { "关闭" }, if r.is_ok() { "成功".into() } else { r.err().map(|e| e.to_string()).unwrap_or_default() });
        push_event(serial, if incoming.block_internet { "internet_on" } else { "internet_off" }, &detail);
    }
    push_event(serial, "config", "管控配置已更新");
    incoming.clone()
}

fn push_event(serial: &str, kind: &str, detail: &str) {
    let mut db = db().lock().unwrap();
    let _ = db.execute(
        "INSERT INTO guard_events (serial,time,kind,detail) VALUES (?1,?2,?3,?4)",
        rusqlite::params![serial, now_ms(), kind, detail],
    );
    // 每设备只留最近 200 条
    let _ = db.execute(
        "DELETE FROM guard_events WHERE serial = ?1 AND id NOT IN          (SELECT id FROM guard_events WHERE serial = ?1 ORDER BY id DESC LIMIT 200)",
        [serial],
    );
}

/// 最近 n 条事件（新→旧）
pub fn events_recent(serial: &str, n: i64) -> Vec<GuardEvent> {
    let db = db().lock().unwrap();
    let mut stmt = match db.prepare(
        "SELECT time, kind, detail FROM guard_events WHERE serial = ?1 ORDER BY id DESC LIMIT ?2",
    ) {
        Ok(s) => s,
        Err(_) => return Vec::new(),
    };
    let rows = stmt.query_map([serial, n.to_string().as_str()], |r| {
        Ok(GuardEvent { time: r.get(0)?, kind: r.get(1)?, detail: r.get(2)? })
    });
    match rows {
        Ok(rs) => rs.filter_map(|x| x.ok()).collect(),
        Err(_) => Vec::new(),
    }
}

/// 累计某设备某日亮屏秒数（UPSERT 原子加）
fn usage_add(serial: &str, date: &str, secs: i64) {
    let mut db = db().lock().unwrap();
    let _ = db.execute(
        "INSERT INTO guard_usage VALUES (?1, ?2, ?3)
         ON CONFLICT(serial, date) DO UPDATE SET secs = secs + ?3",
        rusqlite::params![serial, date, secs],
    );
}

/// 某设备某日已用秒数
pub fn usage_secs(serial: &str, date: &str) -> i64 {
    let db = db().lock().unwrap();
    db.query_row(
        "SELECT secs FROM guard_usage WHERE serial = ?1 AND date = ?2",
        [serial, date],
        |r| r.get::<_, i64>(0),
    )
    .unwrap_or(0)
}

// ---------------- 本地时间（容器无 zoneinfo：默认 UTC+8，可覆盖） ----------------

fn tz_offset_min() -> i64 {
    std::env::var("MWINE_TZ_OFFSET_MIN")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(480)
}

/// (日期 "YYYY-MM-DD", 当日分钟数)
fn local_date_minutes() -> (String, i64) {    let ms = now_ms();
    let shifted = ms + tz_offset_min() * 60_000;
    let days = shifted / 86_400_000;
    let rem = shifted % 86_400_000;
    // 1970-01-01 起的 civil 日期换算（Howard Hinnant 算法）
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    (
        format!("{y:04}-{m:02}-{d:02}"),
        rem / 60_000,
    )
}

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

// ---------------- 时段窗口 ----------------

/// "HH:MM-HH:MM" 是否包含当日 minute（支持跨零点：20:00-06:30）
fn window_contains(w: &str, minute: i64) -> bool {
    let Some((a, b)) = w.split_once('-') else { return false };
    let parse = |s: &str| -> Option<i64> {
        let (h, m) = s.trim().split_once(':')?;
        let h: i64 = h.trim().parse().ok()?;
        let m: i64 = m.trim().parse().ok()?;
        Some(h * 60 + m)
    };
    let (Some(s), Some(e)) = (parse(a), parse(b)) else { return false };
    if s <= e {
        minute >= s && minute < e
    } else {
        minute >= s || minute < e // 跨零点
    }
}

fn in_any_window(windows: &[String], minute: i64) -> bool {
    if windows.is_empty() {
        return true; // 未配时段 = 全天允许
    }
    windows.iter().any(|w| window_contains(w, minute))
}

// ---------------- 设备侧操作 ----------------

pub struct DeviceProbe {
    pub wakefulness: String,
    pub screen_on: bool,    // Awake | Dreaming（屏保亮屏也算在用）
    pub foreground: String, // 前台包名（空 = 未知/无）
}

/// 一次 shell 合并取证屏幕状态 + 前台应用
pub fn probe(serial: &str) -> AdbResult<DeviceProbe> {
    let out = adb::shell_blocking(
        serial,
        "dumpsys power | grep -m1 'mWakefulness='; \
         dumpsys activity activities | grep -E 'mResumedActivity|topResumedActivity' | head -1",
        8000,
    )?;
    let text = String::from_utf8_lossy(&out);
    let wakefulness = text
        .lines()
        .find_map(|l| l.split("mWakefulness=").nth(1))
        .map(|s| s.trim().to_string())
        .unwrap_or_default();
    let foreground = text
        .lines()
        .find_map(|l| {
            // mResumedActivity: ActivityRecord{hash u0 com.pkg/.Act t123}
            let i = l.find(" u0 ")?;
            let rest = &l[i + 4..];
            let pkg = rest.split('/').next()?;
            Some(pkg.to_string())
        })
        .unwrap_or_default();
    let screen_on = wakefulness == "Awake" || wakefulness == "Dreaming";
    Ok(DeviceProbe { wakefulness, screen_on, foreground })
}

fn shell(serial: &str, cmd: &str, timeout_ms: u64) {
    let _ = adb::shell_blocking(serial, cmd, timeout_ms);
}

fn notify(serial: &str, msg: &str) {
    // 文案不带空格（shell 引号层太多，稳妥起见用下划线）
    shell(serial, &format!("cmd notification post -t 家长管控 mwine_guard {msg}"), 5000);
}

fn force_sleep(serial: &str) {
    shell(serial, "input keyevent 223", 5000);
}

/// 外网封禁：代理黑洞 + 严格私有 DNS 指向不可达主机（不动 WiFi，adb 通道保命）
pub fn internet_block(serial: &str) -> AdbResult<()> {
    adb::shell_blocking(
        serial,
        "settings put global http_proxy 127.0.0.1:9; \
         settings put global private_dns_mode hostname; \
         settings put global private_dns_specifier dns.invalid",
        8000,
    )
    .map(|_| ())
}

/// 解除封禁：恢复出厂默认（无代理 + 自动 DNS）
pub fn internet_unblock(serial: &str) -> AdbResult<()> {
    adb::shell_blocking(
        serial,
        "settings put global http_proxy :0; \
         settings delete global http_proxy; \
         settings put global private_dns_mode off; \
         settings delete global private_dns_specifier",
        8000,
    )
    .map(|_| ())
}

// ---------------- 主循环 ----------------

const POLL_SECS: u64 = 5;

/// 每设备的最近巡检时间戳（API 展示巡检活性）
pub static LAST_CHECK: std::sync::LazyLock<Mutex<HashMap<String, i64>>> =
    std::sync::LazyLock::new(|| Mutex::new(HashMap::new()));

/// 黑名单踢杀节流（(serial, pkg) → 上次事件戳；纯运行态，不落库）
static KILL_TS: std::sync::LazyLock<Mutex<HashMap<(String, String), i64>>> =
    std::sync::LazyLock::new(|| Mutex::new(HashMap::new()));

/// 近 N 天用量序列（缺的天补 0，图表用）
pub fn usage_history(serial: &str, days: usize) -> Vec<(String, u32)> {
    let mut rows: std::collections::HashMap<String, i64> = std::collections::HashMap::new();
    {
        let db = db().lock().unwrap();
        let prepared = db.prepare(
            "SELECT date, secs FROM guard_usage WHERE serial = ?1 AND date >= date('now','-40 days')",
        );
        match prepared {
            Ok(mut stmt) => {
                if let Ok(rs) = stmt.query_map([serial], |r| {
                    Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?))
                }) {
                    for x in rs.flatten() {
                        rows.insert(x.0, x.1);
                    }
                }
            }
            Err(_) => {}
        }
    }
    let (today, _) = local_date_minutes();
    let today_days = date_to_days(&today);
    (0..days)
        .rev()
        .map(|i| {
            let d = days_to_date(today_days.saturating_sub(i as i64));
            let min = (rows.get(&d).copied().unwrap_or(0) / 60) as u32;
            (d, min)
        })
        .collect()
}

/// "YYYY-MM-DD" → 距 1970-01-01 天数（civil 换算）
fn date_to_days(d: &str) -> i64 {
    let mut it = d.split('-');
    let y: i64 = it.next().and_then(|v| v.parse().ok()).unwrap_or(1970);
    let m: i64 = it.next().and_then(|v| v.parse().ok()).unwrap_or(1);
    let dd: i64 = it.next().and_then(|v| v.parse().ok()).unwrap_or(1);
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y.rem_euclid(400);
    let mp = if m > 2 { m - 3 } else { m + 9 };
    let doy = (153 * mp + 2) / 5 + dd - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

fn days_to_date(z: i64) -> String {
    let z = z + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let dd = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!("{y:04}-{m:02}-{dd:02}")
}

/// 每 5s：遍历全部在线设备，各设备独立执行管控（与「当前浏览设备」无关）。
/// 设备离线跳过；管控关闭的设备只跳过执行（配置保留）。
pub fn run_loop(state: crate::state::SharedState) {
    // per-serial 屏幕亮灭跟踪（唤醒沿检测用）
    let mut screen_state: HashMap<String, bool> = HashMap::new();
    let mut poll_count: u64 = 0;
    loop {
        std::thread::sleep(std::time::Duration::from_secs(POLL_SECS));
        poll_count += 1;
        let _ = &state; // current_serial 语义已不用于管控（按设备独立），保留参数兼容
        for serial in crate::state::online_real_devices() {
            let cfg = cfg_get(&serial);
            if !cfg.enabled {
                continue;
            }
            let Ok(p) = probe(&serial) else { continue };
            LAST_CHECK.lock().unwrap().insert(serial.clone(), now_ms());
            let (today, minute) = local_date_minutes();

            // 屏亮时长累计（Dreaming 屏保也计入——屏幕确实亮着；UPSERT 原子加）
            if p.screen_on {
                usage_add(&serial, &today, POLL_SECS as i64);
                // 顺手清 40 天前的旧行（每日首次写入时自然触发）
                let cutoff = days_to_date(date_to_days(&today) - 40);
                let mut db = db().lock().unwrap();
                let _ = db.execute("DELETE FROM guard_usage WHERE date < ?1", [cutoff]);
            }

            // 亮屏沿检测（自然唤醒也记录——家长关心孩子何时开的电视）
            if screen_state.get(&serial) == Some(&false) && p.screen_on {
                push_event(&serial, "wake_detected", &format!("检测到亮屏（{}）", p.foreground));
            }
            screen_state.insert(serial.clone(), p.screen_on);

            // 黑名单：前台命中即踢杀（事件按包 60s 节流，踢杀本身每轮执行）
            if p.screen_on && !p.foreground.is_empty() && cfg.blocked_apps.iter().any(|a| a == &p.foreground) {
                let pkg = p.foreground.clone();
                shell(&serial, &format!("am force-stop {pkg}"), 8000);
                notify(&serial, &format!("应用_{pkg}_已被家长管控拦截"));
                let now = now_ms();
                let throttled = KILL_TS
                    .lock()
                    .unwrap()
                    .get(&(serial.clone(), pkg.clone()))
                    .map(|t| now - t < 60_000)
                    .unwrap_or(false);
                if !throttled {
                    KILL_TS.lock().unwrap().insert((serial.clone(), pkg.clone()), now);
                    push_event(&serial, "app_blocked", &format!("已拦截应用 {pkg}"));
                }
            }

            // 时段/上限：屏亮且违规 → 强制待机（每次唤醒尝试都记录，方便家长看到）
            let used_min: u32 = (usage_secs(&serial, &today) / 60) as u32;
            let over_limit = cfg.daily_limit_min > 0 && used_min >= cfg.daily_limit_min;
            let in_win = in_any_window(&cfg.windows, minute);
            if p.screen_on && (!in_win || over_limit) {
                let reason = if !in_win {
                    format!("非允许时段（{:02}:{:02}）", minute / 60, minute % 60)
                } else {
                    format!("今日已用 {used_min} 分钟，达到上限 {}", cfg.daily_limit_min)
                };
                force_sleep(&serial);
                notify(&serial, &reason.replace(' ', "_").replace('（', "_").replace('）', ""));
                push_event(&serial, "sleep_forced", &reason);
            }
            // 封禁保活：每 12 轮（1 分钟）复查一次，防止被设备侧改回
            if cfg.block_internet && poll_count % 12 == 0 {
                if let Ok(out) = adb::shell_blocking(&serial, "settings get global private_dns_mode", 5000) {
                    if !String::from_utf8_lossy(&out).contains("hostname") {
                        let _ = internet_block(&serial);
                    }
                }
            }
        }
    }
}

/// 对外：今日日期与当前分钟数（api 层组装响应用）
pub fn local_today_minute() -> (String, i64) {
    local_date_minutes()
}

/// 对外：当前时刻是否处于允许时段（api 响应用）
pub fn in_window_now(windows: &[String]) -> bool {
    let (_, m) = local_date_minutes();
    in_any_window(windows, m)
}

/// 配置库中出现过的全部设备序列号（回连候选）
pub fn known_serials() -> Vec<String> {
    let mut out = Vec::new();
    let db = db().lock().unwrap();
    if let Ok(mut stmt) = db.prepare("SELECT DISTINCT serial FROM guard_config") {
        if let Ok(rs) = stmt.query_map([], |r| r.get::<_, String>(0)) {
            out.extend(rs.flatten());
        }
    }
    out
}
