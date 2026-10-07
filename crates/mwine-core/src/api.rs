use std::time::Instant;

use super::config::VmConfig;
use super::daemon::AppState;
use std::sync::Arc;
use serde_json::json;

type Resp = (u16, String);

pub fn route_with_body(method: &str, url: &str, body: &str, state: &Arc<AppState>) -> Resp {
    // POST /api/config 的载荷在 body
    if method == "POST" && url == "/api/image/import" {
        return image_import(state, body);
    }
    if method == "POST" && url == "/api/image/official-pull" {
        return image_official_pull(state, body);
    }
    if method == "GET" && url.starts_with("/api/image/official") {
        return image_official_list(url);
    }
    if method == "POST" && url == "/api/image/delete" {
        return image_delete(state, body);
    }
    if method == "POST" && url == "/api/image/use" {
        return image_use(state, body);
    }
    if method == "POST" && url == "/api/image/cancel" {
        return image_cancel(state);
    }
    if method == "POST" && url == "/api/config" {
        return config_set(state, body);
    }
    if let Some(pkg) = url.strip_prefix("/api/keys/") {
        if method == "PUT" && !pkg.contains('/') {
            return keys_set(pkg, body);
        }
    }
    route(method, url, state)
}

pub fn route(method: &str, url: &str, state: &Arc<AppState>) -> Resp {
    match (method, url) {
        ("GET", "/api/status") => status(state),
        ("POST", "/api/vm/start") => vm_start(state),
        ("POST", "/api/vm/stop") => vm_stop(state),
        ("GET", "/api/apps") => apps_list(state),
        ("GET", "/api/config") => config_get(),
        ("GET", "/api/perf") => perf(state),
        ("POST", "/api/identity/reset") => identity_reset(state),
        ("POST", "/api/screenshot") => screenshot_save(state),
        ("POST", "/api/vm/reset") => vm_reset(state),
        ("GET", "/api/image/list") => image_list(state),
        ("GET", "/api/image/task") => image_task_get(state),
        ("POST", "/api/config") => config_set(state, url),

        ("GET", "/health") => (200, json!({"ok": true}).to_string()),
        _ => {
            // 动态路由：POST /api/apps/<pkg>/run
            if method == "POST" && url.starts_with("/api/apps/") && url.ends_with("/run") {
                let pkg = url
                    .trim_start_matches("/api/apps/")
                    .trim_end_matches("/run");
                return app_run(state, pkg);
            }
            // 动态路由：keys
            //   GET  /api/keys/<pkg>            → 键位映射
            //   PUT  /api/keys/<pkg>            → 保存（body JSON）
            //   POST /api/keys/<pkg>/shot       → 截图（编辑器背景）
            //   GET  /api/keys/<pkg>/shot       → 截图文件路径
            (404, json!({"error": "not found"}).to_string())
        }
    }
}

/// 性能快照：guest CPU%（两次采样差分）/ 内存 / uptime + 宿主 qemu 开销。
fn perf(state: &Arc<AppState>) -> Resp {
    use std::sync::Mutex;
    static LAST: Mutex<Option<(Instant, u64, u64)>> = Mutex::new(None); // (t, busy, total)

    let serial = state
        .adb
        .devices()
        .ok()
        .and_then(|ds| {
            ds.into_iter()
                .find(|d| d.state == "device" && d.serial.starts_with("emulator-"))
                .map(|d| d.serial)
        });
    let Some(serial) = serial else {
        return (200, json!({"running": false}).to_string());
    };

    let read_cpu = || -> Option<(u64, u64, serde_json::Value)> {
        let out = state
            .adb
            .shell_blocking(&serial, "head -1 /proc/stat; head -2 /proc/meminfo; cat /proc/uptime", std::time::Duration::from_secs(5))
            .ok()?;
        let text = String::from_utf8_lossy(&out.stdout).to_string();
        let mut lines = text.lines();
        let cpu_line = lines.next()?.to_string();
        let mut fields = cpu_line.split_whitespace().skip(1).filter_map(|v| v.parse::<u64>().ok()).collect::<Vec<_>>();
        if fields.len() < 4 { return None; }
        // user nice system idle iowait irq softirq → busy = total - idle - iowait
        let total: u64 = fields.iter().sum();
        let idle = fields[3] + fields.get(4).copied().unwrap_or(0);
        let mem_total = lines.next()?.split_whitespace().nth(1)?.parse::<u64>().ok()?;
        let _mem_free = lines.next();
        let uptime = lines.next()?.split_whitespace().next()?.parse::<f64>().ok()?;
        let mem = json!({
            "total_mb": mem_total / 1024,
            "available_mb": state.adb.shell_blocking(&serial, "awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo", std::time::Duration::from_secs(3))
                .ok().and_then(|o| o.stdout_string().trim().parse::<u64>().ok()).unwrap_or(0),
        });
        Some((total - idle, total, json!({"uptime_s": uptime as u64, "mem": mem})))
    };

    let (busy, total, mut info) = match read_cpu() {
        Some(v) => v,
        None => return (200, json!({"running": true, "error": "采样失败"}).to_string()),
    };
    let mut cpu_pct = 0u64;
    {
        let mut last = LAST.lock().unwrap_or_else(|e| e.into_inner());
        if let Some((t0, b0, t_0)) = *last {
            let dt = Instant::now().duration_since(t0).as_millis().max(1) as u64;
            let db = busy.saturating_sub(b0);
            let dtt = total.saturating_sub(t_0);
            if dtt > 0 {
                cpu_pct = db * 100 / dtt;
            }
            let _ = dt;
        }
        *last = Some((Instant::now(), busy, total));
    }

    // 宿主 qemu 进程 CPU%（ps 两次采样太贵，用单次 CPU% 列）
    let host = std::process::Command::new("ps")
        .arg("-o")
        .arg("pcpu,rss,comm")
        .args(["-p", &host_qemu_pid().map(|p| p.to_string()).unwrap_or_default()])
        .output()
        .ok()
        .and_then(|o| String::from_utf8_lossy(&o.stdout).lines().nth(1).map(|l| l.trim().to_string()))
        .unwrap_or_default();
    let mut host_cpu = 0.0f64;
    let mut host_mem_mb = 0u64;
    if !host.is_empty() {
        let mut it = host.split_whitespace();
        host_cpu = it.next().and_then(|v| v.parse().ok()).unwrap_or(0.0);
        host_mem_mb = it.next().and_then(|v: &str| v.parse::<f64>().ok()).map(|kb| (kb / 1024.0) as u64).unwrap_or(0);
    }

    info["cpu_pct"] = json!(cpu_pct);
    info["running"] = json!(true);
    info["host"] = json!({"qemu_cpu_pct": host_cpu, "qemu_mem_mb": host_mem_mb});
    (200, info.to_string())
}

fn home_dot_mwine() -> std::path::PathBuf {
    let home = std::env::var("HOME").unwrap_or_default();
    std::path::PathBuf::from(home).join(".mwine")
}

fn keys_get(pkg: &str) -> Resp {
    let path = home_dot_mwine().join("keymaps").join(format!("{pkg}.json"));
    let body = std::fs::read_to_string(&path).unwrap_or_else(|_| "[]".into());
    (200, json!({"bindings": serde_json::from_str::<serde_json::Value>(&body).unwrap_or(serde_json::json!([]))}).to_string())
}

fn keys_set(pkg: &str, body: &str) -> Resp {
    let parsed: serde_json::Value = match serde_json::from_str(body) {
        Ok(v) => v,
        Err(e) => return (400, json!({"error": format!("JSON 错误: {e}")}).to_string()),
    };
    if !parsed.is_array() {
        return (400, json!({"error": "需为绑定数组"}).to_string());
    }
    let dir = home_dot_mwine().join("keymaps");
    std::fs::create_dir_all(&dir).ok();
    match std::fs::write(dir.join(format!("{pkg}.json")), body) {
        Ok(_) => (200, json!({"ok": true}).to_string()),
        Err(e) => (500, json!({"error": e.to_string()}).to_string()),
    }
}

fn keys_shot(state: &Arc<AppState>, pkg: &str) -> Resp {
    let Some(serial) = state.adb.devices().ok().and_then(|ds| {
        ds.into_iter().find(|d| d.state == "device" && d.serial.starts_with("emulator-")).map(|d| d.serial)
    }) else {
        return (503, json!({"error": "Android 未运行"}).to_string());
    };
    let out = match state.adb.shell_blocking(&serial, "screencap -p", std::time::Duration::from_secs(10)) {
        Ok(o) => o,
        Err(e) => return (500, json!({"error": e.to_string()}).to_string()),
    };
    if !(out.stdout.len() > 8 && out.stdout.starts_with(&[0x89, b'P', b'N', b'G'])) {
        return (500, json!({"error": "截图失败"}).to_string());
    }
    let dir = home_dot_mwine().join("keyshots");
    std::fs::create_dir_all(&dir).ok();
    let path = dir.join(format!("{pkg}.png"));
    if let Err(e) = std::fs::write(&path, &out.stdout) {
        return (500, json!({"error": e.to_string()}).to_string());
    }
    (200, json!({"path": path.display().to_string()}).to_string())
}

fn keys_shot_path(pkg: &str) -> Resp {
    let path = home_dot_mwine().join("keyshots").join(format!("{pkg}.png"));
    (200, json!({"path": path.display().to_string(), "exists": path.is_file()}).to_string())
}

const IDLE_MINUTES: u64 = 30;

/// 空闲看护：前台一直是 launcher（无人使用）→ 到时存档关停。
/// 任一前台应用切换即重置；emulator 消失则退出线程。
fn idle_watchdog(adb: mwine_adb::AdbClient, serial: String) {
    let mut idle_secs = 0u64;
    loop {
        std::thread::sleep(std::time::Duration::from_secs(60));
        // emulator 还在吗
        let alive = adb
            .devices()
            .map(|ds| ds.iter().any(|d| d.serial == serial && d.state == "device"))
            .unwrap_or(false);
        if !alive {
            return;
        }
        // 前台应用（launcher = 空闲）
        let fg = adb
            .shell_blocking(
                &serial,
                "dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity | grep -oE '[a-z0-9._]+/[a-zA-Z0-9._$]+' | cut -d/ -f1",
                std::time::Duration::from_secs(8),
            )
            .ok()
            .map(|o| o.stdout_string().trim().to_string())
            .unwrap_or_default();
        if fg.contains("launcher") || fg.is_empty() {
            idle_secs += 60;
        } else {
            idle_secs = 0;
        }
        if idle_secs >= IDLE_MINUTES * 60 {
            println!("[idle] 连续 {IDLE_MINUTES} 分钟无人使用，自动存档关停");
            let _ = mwine_supervisor::save_snapshot_blocking(
                &serial,
                "mwine_default",
                std::time::Duration::from_secs(20),
            );
            let _ = mwine_supervisor::emu_kill_wait(&adb, &serial, std::time::Duration::from_secs(35));
            return;
        }
    }
}

/// 设备身份重置：随机化 android_id / 广告 ID（应用看到的是一台新设备）。
fn identity_reset(state: &Arc<AppState>) -> Resp {
    let Some(serial) = state.adb.devices().ok().and_then(|ds| {
        ds.into_iter().find(|d| d.state == "device" && d.serial.starts_with("emulator-")).map(|d| d.serial)
    }) else {
        return (503, json!({"error": "Android 未运行"}).to_string());
    };
    let t = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let mut x = (t.as_nanos() as u64) ^ (std::process::id() as u64) << 32 | 1;
    let mut hex = String::new();
    for _ in 0..4 {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        hex.push_str(&format!("{x:016x}"));
    }
    let aid: String = hex.chars().take(16).collect();
    let h: Vec<char> = hex.chars().collect();
    let adid = format!(
        "{}-{}-{}-{}-{}",
        h[0..8].iter().collect::<String>(), h[8..12].iter().collect::<String>(),
        h[12..16].iter().collect::<String>(), h[16..20].iter().collect::<String>(),
        h[20..32].iter().collect::<String>()
    );
    let mut ok = 0;
    for (k, v) in [("android_id", aid.as_str()), ("advertising_id", adid.as_str())] {
        if state.adb.shell_blocking(
            &serial, &format!("settings put secure {k} {v}"), std::time::Duration::from_secs(5),
        ).is_ok() { ok += 1; }
    }
    (200, json!({"ok": ok == 2, "android_id": aid}).to_string())
}

/// 重置虚拟机：关停 → 清 userdata 与快照 → 下次启动全新（救急/转卖前清数据）。
fn vm_reset(state: &Arc<AppState>) -> Resp {
    let serial = state
        .adb
        .devices()
        .ok()
        .and_then(|ds| {
            ds.into_iter()
                .find(|d| d.serial.starts_with("emulator-"))
                .map(|d| d.serial)
        })
        .unwrap_or_else(|| "emulator-5554".to_string());
    // 关停（尽力存档无意义但要退净）
    let _ = mwine_supervisor::emu_kill_wait(&state.adb, &serial, std::time::Duration::from_secs(35));
    let home = std::env::var("HOME").unwrap_or_default();
    let avd_home = format!("{home}/.android/avd");
    let mut wiped = 0;
    for entry in std::fs::read_dir(&avd_home).ok().into_iter().flatten().flatten() {
        let avd = entry.path();
        let name = entry.file_name().to_string_lossy().trim_end_matches(".avd").to_string();
        if name != "MWine4" {
            continue;
        }
        // 内嵌模式 AVD 在 Application Support（两处都清）
        for base in [
            std::path::PathBuf::from(&avd_home),
            std::path::PathBuf::from(&home).join("Library/Application Support/MWine/avd"),
        ] {
            let dir = base.join(format!("{name}.avd"));
            for sub in ["snapshots", "cache.img.qcow2", "userdata-qemu.img.qcow2", "userdata.img"] {
                let p = dir.join(sub);
                if p.exists() {
                    let ok = if p.is_dir() {
                        std::fs::remove_dir_all(&p).is_ok()
                    } else {
                        std::fs::remove_file(&p).is_ok()
                    };
                    if ok {
                        wiped += 1;
                    }
                }
            }
        }
    }
    *state.qemu_pid.lock().unwrap_or_else(|e| e.into_inner()) = None;
    (200, json!({"ok": true, "wiped": wiped,
        "note": "已重置，下次启动为全新系统（应用与数据已清空）"}).to_string())
}

/// 截屏到 ~/Desktop（screencap 走 SurfaceFlinger，不经坏掉的编码器）。
fn screenshot_save(state: &Arc<AppState>) -> Resp {
    let Some(serial) = state.adb.devices().ok().and_then(|ds| {
        ds.into_iter().find(|d| d.state == "device" && d.serial.starts_with("emulator-")).map(|d| d.serial)
    }) else {
        return (503, json!({"error": "Android 未运行"}).to_string());
    };
    let out = match state.adb.shell_blocking(&serial, "screencap -p", std::time::Duration::from_secs(10)) {
        Ok(o) => o.stdout,
        Err(e) => return (500, json!({"error": e.to_string()}).to_string()),
    };
    if !(out.len() > 8 && out.starts_with(&[0x89, b'P', b'N', b'G'])) {
        return (500, json!({"error": "截图失败"}).to_string());
    }
    let home = std::env::var("HOME").unwrap_or_default();
    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let path = format!("{home}/Desktop/MWine-{ts}.png");
    match std::fs::write(&path, &out) {
        Ok(_) => (200, json!({"ok": true, "path": path}).to_string()),
        Err(e) => (500, json!({"error": e.to_string()}).to_string()),
    }
}

fn host_qemu_pid() -> Option<u32> {
    let out = std::process::Command::new("pgrep").arg("-f").arg("qemu-system-aarch64").output().ok()?;
    String::from_utf8_lossy(&out.stdout).lines().next()?.trim().parse().ok()
}

/// 配置读取（UI 设置页初始化）。
fn config_get() -> Resp {
    let cfg = VmConfig::load();
    (200, serde_json::to_string(&cfg).unwrap_or_default())
}

/// 配置保存（body 为 VmConfig JSON）。写 AVD 配置（下次启动生效），
/// VM 运行中则同步做运行时 wm size/density 立即生效。
fn config_set(state: &Arc<AppState>, body: &str) -> Resp {
    let mut cfg: VmConfig = match serde_json::from_str(body) {
        Ok(c) => c,
        Err(e) => return (400, json!({"error": format!("配置格式错误: {e}")}).to_string()),
    };
    // 保留 daemon 侧的 avd 名：UI 加载的可能是旧值（镜像切换→relabel 后 UI 不知道新名）
    if let Some((_, _, real_avd)) = resolve_active_avd() {
        cfg.avd = real_avd;
    }
    if let Err(e) = cfg.save() {
        return (500, json!({"error": e.to_string()}).to_string());
    }
    apply_cfg_to_avd(&cfg);
    // 快照记录着保存时的屏幕配置，恢复会盖掉新尺寸 → 尺寸变化时作废快照
    // （下次启动冷启一次，之后再恢复 5s 快启）
    let lcd = cfg.lcd();
    if let Some(snap) = snapshot_hw_lcd(&cfg.avd) {
        if snap != lcd {
            let _ = remove_snapshot(&cfg.avd, "mwine_default");
        }
    }
    let (w, h, dpi) = cfg.lcd();
    if let Ok(d) = state.adb.devices() {
        if let Some(dev) = d.iter().find(|d| d.state == "device" && d.serial.starts_with("emulator-")) {
            let _ = mwine_supervisor::set_lcd_runtime(&state.adb, &dev.serial, w, h, dpi);
        }
    }
    (200, json!({"ok": true}).to_string())
}

/// 读快照里记录的 (宽, 高, dpi)（无快照返回 None）。
fn snapshot_hw_lcd(avd: &str) -> Option<(u32, u32, u32)> {
    let home = std::env::var_os("HOME")?;
    let hw = std::path::Path::new(&home)
        .join(".android/avd")
        .join(format!("{avd}.avd/snapshots/mwine_default/hardware.ini"));
    let text = std::fs::read_to_string(hw).ok()?;
    let get = |k: &str| -> Option<u32> {
        text.lines()
            .find(|l| l.trim_start().starts_with(k))
            .and_then(|l| l.split('=').nth(1))
            .and_then(|v| v.trim().parse().ok())
    };
    Some((get("hw.lcd.width")?, get("hw.lcd.height")?, get("hw.lcd.density")?))
}

/// 删除指定快照（作废 = 下次冷启用新配置）。
fn remove_snapshot(avd: &str, name: &str) -> std::io::Result<()> {
    let home = std::env::var("HOME").unwrap_or_default();
    let dir = std::path::Path::new(&home)
        .join(".android/avd")
        .join(format!("{avd}.avd/snapshots/{name}"));
    if dir.is_dir() {
        std::fs::remove_dir_all(dir)?;
    }
    Ok(())
}

fn apply_cfg_to_avd(cfg: &VmConfig) {
    let Some((avd_dir, _, _)) = resolve_active_avd() else { return };
    let (w, h, dpi) = cfg.lcd();
    let cfg_p = avd_dir.join("config.ini");
    let text = std::fs::read_to_string(&cfg_p).unwrap_or_default();
    let new_text: String = text.lines().map(|l| {
        let key = l.split('=').next().unwrap_or("").trim();
        match key {
            "hw.lcd.width" => format!("hw.lcd.width = {w}"),
            "hw.lcd.height" => format!("hw.lcd.height = {h}"),
            "hw.lcd.density" => format!("hw.lcd.density = {dpi}"),
            "hw.initialOrientation" => format!(
                "hw.initialOrientation = {}",
                if w > h { "landscape" } else { "portrait" }
            ),
            _ => l.to_string(),
        }
    }).chain(std::iter::once(String::new())).collect::<Vec<_>>().join("\n");
    let _ = std::fs::write(&cfg_p, new_text);
    let _ = std::fs::remove_file(avd_dir.join("hardware-qemu.ini")); // 防旧值钉住
}

fn app_run(state: &Arc<AppState>, package: &str) -> Resp {
    let serial = state.adb
        .find_online_device()
        .map(|d| d.serial)
        .unwrap_or_else(|_| "emulator-5554".to_string());

    // 解析入口 Activity 并启动
    let resolve = state.adb.shell_blocking(
        &serial,
        &format!("cmd package resolve-activity --brief {package} | tail -1"),
        std::time::Duration::from_secs(10),
    );
    let comp = resolve.ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| s.contains('/'));

    match comp {
        Some(c) => {
            let result = state.adb.shell_blocking(
                &serial,
                &format!("am start -n '{c}'"),
                std::time::Duration::from_secs(10),
            );
            match result {
                Ok(_) => (200, json!({"ok": true, "package": package}).to_string()),
                Err(e) => (500, json!({"error": e.to_string()}).to_string()),
            }
        }
        None => (404, json!({"error": "找不到启动入口"}).to_string()),
    }
}

fn status(state: &Arc<AppState>) -> Resp {
    // 以 adb 在线设备为准：无论 VM 是谁拉起的（CLI / 守护进程 / 开机自启），
    // 只要有 emulator 设备在线就视为运行中。守护进程自身 spawn 的 pid 仅作兜底。
    let running = state
        .adb
        .devices()
        .map(|ds| ds.iter().any(|d| d.state == "device" && d.serial.starts_with("emulator-")))
        .unwrap_or_else(|_| {
            let pid = state.qemu_pid.lock().unwrap_or_else(|e| e.into_inner());
            match *pid {
                Some(p) => unsafe { libc::kill(p as i32, 0) == 0 },
                None => false,
            }
        });
    let err = state.vm_error.lock().unwrap_or_else(|e| e.into_inner()).clone();
    let serial = state.vm_serial.lock().unwrap_or_else(|e| e.into_inner()).clone();
    (
        200,
        json!({"vm": {"running": running, "serial": serial}, "error": err, "version": "0.1.0"}).to_string(),
    )
}

fn vm_start(state: &Arc<AppState>) -> Resp {
    // 唯一权威判据 = adb 在线设备（pid 判定会被僵尸进程欺骗：forget 不收尸 → kill(pid,0) 恒真
    // → 永远 already 拒启，表现为「点启动没反应」）
    if let Ok(ds) = state.adb.devices() {
        let online: Vec<String> = ds
            .iter()
            .filter(|d| d.state == "device" && d.serial.starts_with("emulator-"))
            .map(|d| d.serial.clone())
            .collect();
        if !online.is_empty() {
            // 收养：daemon 重启后遇到"本产品唯一在线 VM"时认领之（多台无法甄别归属，留空
            // 由安装侧按"多设备"规则处理），保证 /api/status.serial 尽早可用
            if online.len() == 1 {
                *state.vm_serial.lock().unwrap_or_else(|e| e.into_inner()) = Some(online[0].clone());
            }
            return (200, json!({"ok": true, "already": true}).to_string());
        }
    }

    // AVD 名跟随镜像版本（=模拟器窗口标题）；放在 already 检查后，只在即将启动时做
    if let Ok(sdk) = mwine_supervisor::Sdk::detect() {
        if let Some(sysdir) = current_avd_sysdir() {
            relabel_avd(&sdk, &sysdir);
        }
    }
    // 走 supervisor 的统一启动路径：按用户配置（尺寸/内存/sdcard）启动 windowed VM，
    // emulator 自己的窗口就是显示界面；快照自动恢复（5s 级，配置变更后首次为冷启）
    let cfg = VmConfig::load();
    *state.vm_error.lock().unwrap_or_else(|e| e.into_inner()) = String::new();
    let state2 = state.clone();
    std::thread::spawn(move || {
        // 僵尸清场：qemu 进程还在但 adb 无 device（上次启动失败/关停未净）→
        // 端口被占，新实例会静默失败（用户点启动"没反应"的根因）
        let healthy = state2
            .adb
            .devices()
            .map(|ds| ds.iter().any(|d| d.state == "device" && d.serial.starts_with("emulator-")))
            .unwrap_or(false);
        if !healthy {
            let zombie = std::process::Command::new("pgrep")
                .arg("-f")
                .arg("qemu-system-aarch64")
                .output()
                .map(|o| !o.stdout.is_empty())
                .unwrap_or(false);
            if zombie {
                println!("[vm] 发现僵尸 qemu，清理后重启");
                let _ = std::process::Command::new("pkill")
                    .arg("-9")
                    .arg("-f")
                    .arg("qemu-system-aarch64")
                    .status();
                std::thread::sleep(std::time::Duration::from_secs(3));
            }
        }

        let sdk = match mwine_supervisor::Sdk::detect() {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[vm] SDK 探测失败: {e}");
                *state2.vm_error.lock().unwrap_or_else(|e| e.into_inner()) = format!("SDK 探测失败: {e}");
                return;
            }
        };
        // 镜像预检：分发包未导入时给出明确引导（而不是 emulator 静默失败）
        if let Some(sysdir) = current_avd_sysdir() {
            let img_dir = sdk_root().join(&sysdir);
            if !img_dir.join("system.img").symlink_metadata().is_ok()
                && !img_dir.join("system-qemu.img").is_file()
            {
                eprintln!("[vm] 镜像缺失: {}", img_dir.display());
                *state2.vm_error.lock().unwrap_or_else(|e| e.into_inner()) =
                    "尚未安装系统镜像：请到「镜像」页导入 houttuynia-image-*.tar.zst 分发包（或拉取官方镜像）".into();
                return;
            }
        }
        // 9p 宿主共享目录（qemu -virtfs；emulator 参数须在 -qemu 透传块前）
        let mut extra = Vec::new();
        if !cfg.share_dir.is_empty() {
            extra.push("-virtfs".to_string());
            extra.push(format!(
                "local,path={},mount_tag=hostshare,security_model=none,id=hostshare",
                cfg.share_dir
            ));
        }
        // AVD 实体可能在另一台主目录（历史分裂）：按实际位置覆盖 child 的 AVD home
        let avd_home_override = resolve_active_avd()
            .map(|(_, h, _)| h)
            .filter(|h| Some(h) != sdk.avd_home().parent().map(|_| h).map(|_| h) && {
                let canonical = |p: &std::path::Path| p.canonicalize().unwrap_or_else(|_| p.to_path_buf());
                canonical(h) != canonical(&sdk.avd_home())
            });
        // 专业参数：QEMU 附加参数 + GPU 后端 + 网络模拟 + 启动行为
        if !cfg.qemu_extra.is_empty() {
            for a in cfg.qemu_extra.split_whitespace() {
                extra.push(a.to_string());
            }
        }
        if !cfg.gpu_mode.is_empty() {
            extra.push("-gpu".to_string());
            extra.push(cfg.gpu_mode.clone());
        }
        if !cfg.netspeed.is_empty() {
            extra.push("-netspeed".to_string());
            extra.push(cfg.netspeed.clone());
        }
        if !cfg.netdelay.is_empty() {
            extra.push("-netdelay".to_string());
            extra.push(cfg.netdelay.clone());
        }
        // CPU 核数写入 AVD
        if cfg.cpu_cores > 0 {
            if let Some((dir, _, _)) = resolve_active_avd() {
                let cfg_p = dir.join("config.ini");
                if let Ok(txt) = std::fs::read_to_string(&cfg_p) {
                    let new_txt: String = txt.lines().map(|l| {
                        if l.trim_start().starts_with("hw.cpu.ncore") {
                            format!("hw.cpu.ncore = {}", cfg.cpu_cores)
                        } else { l.to_string() }
                    }).chain(std::iter::once(String::new())).collect::<Vec<_>>().join("\n");
                    let _ = std::fs::write(&cfg_p, new_txt);
                }
            }
        }
        let opts = mwine_supervisor::LaunchOptions {
            windowed: true,
            avd_home: avd_home_override,
            no_snapshot: cfg.no_snapshot,
            no_boot_anim: cfg.no_boot_anim,
            proxy: if cfg.proxy.is_empty() { None } else { Some(cfg.proxy.clone()) },
            memory_mb: if cfg.memory_mb > 0 { Some(cfg.memory_mb) } else { None },
            sdcard: if cfg.sdcard_path.is_empty() {
                None
            } else {
                Some(std::path::PathBuf::from(&cfg.sdcard_path))
            },
            extra_args: extra,
            ..Default::default()
        };
        match mwine_supervisor::EmulatorHandle::launch_headless(
            &sdk,
            &cfg.avd,
            &opts,
            &state2.adb,
        ) {
            Ok(mut handle) => {
                let pid = handle.child_id();
                let serial = handle.serial.clone();
                println!("[vm] emulator pid={pid} serial={serial}");
                *state2.qemu_pid.lock().unwrap_or_else(|e| e.into_inner()) = Some(pid);
                *state2.vm_serial.lock().unwrap_or_else(|e| e.into_inner()) = Some(serial.clone());
                // TP1A 已知：管道 adb 可能失联（offline 卡死）——
                // 就绪后确保备用 TCP 通道（console redir 5055）
                if let Err(e) = mwine_supervisor::ensure_control_channel(&state2.adb, &serial) {
                    eprintln!("[vm] 备用通道建立失败: {e}");
                }
                // guest 内代理（10.0.2.2=宿主；settings 即时生效）
                if !cfg.proxy.is_empty() {
                    if let Some(host_port) = cfg.proxy.rsplit(':').next().map(|p| p.to_string()) {
                        let _ = state2.adb.shell_blocking(
                            &serial,
                            &format!("settings put global http_proxy 10.0.2.2:{host_port}"),
                            std::time::Duration::from_secs(8),
                        );
                    }
                }
                // 不 forget：后台线程 wait() 收尸（防僵尸），emulator 仍独立运行
                let st3 = state2.clone();
                std::thread::spawn(move || {
                    let code = handle.child.wait();
                    println!("[vm] emulator 退出: {code:?}");
                    *st3.qemu_pid.lock().unwrap_or_else(|e| e.into_inner()) = None;
                    *st3.vm_serial.lock().unwrap_or_else(|e| e.into_inner()) = None;
                });
                // 空闲看护：无前台应用持续 IDLE_MINUTES → 存档自动关停
                let adb2 = state2.adb.clone();
                std::thread::spawn(move || idle_watchdog(adb2, serial));
            }
            Err(e) => {
                eprintln!("[vm] 启动失败: {e}");
                *state2.vm_error.lock().unwrap_or_else(|e| e.into_inner()) = format!("启动失败: {e}");
            }
        }
    });

    (202, json!({"ok": true, "message": "VM 启动中"}).to_string())
}

fn vm_stop(state: &Arc<AppState>) -> Resp {
    // 先存档再关停，并等 emulator 完全退出（优雅关停 ~20s，
    // 不等完就重启会因端口冲突把新实例一起带死）
    let serial = state
        .adb
        .devices()
        .ok()
        .and_then(|ds| {
            ds.into_iter()
                .find(|d| d.state == "device" && d.serial.starts_with("emulator-"))
                .map(|d| d.serial)
        })
        .unwrap_or_else(|| "emulator-5554".to_string());
    let _ = mwine_supervisor::save_snapshot_blocking(
        &serial,
        "mwine_default",
        std::time::Duration::from_secs(20),
    );
    match mwine_supervisor::emu_kill_wait(&state.adb, &serial, std::time::Duration::from_secs(35)) {
        Ok(()) => {
            *state.qemu_pid.lock().unwrap_or_else(|e| e.into_inner()) = None;
            *state.vm_serial.lock().unwrap_or_else(|e| e.into_inner()) = None;
            (200, json!({"ok": true}).to_string())
        }
        Err(e) => (500, json!({"error": e.to_string()}).to_string()),
    }
}

// ===== 应用列表端点 =====
pub fn apps_list(state: &Arc<AppState>) -> Resp {
    // 找到在线设备
    let serial = state.adb
        .find_online_device()
        .map(|d| d.serial)
        .unwrap_or_else(|_| "emulator-5554".to_string());

    // 列出有启动入口的应用（用户可见的，含系统设置等）
    let out = state.adb.shell_blocking(
        &serial,
        "pm query-activities --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER 2>/dev/null | grep '/' | sed 's|.* ||;s|/.*||' | sort -u",
        std::time::Duration::from_secs(10),
    );

    match out {
        Ok(o) => {
            let pkgs: Vec<String> = String::from_utf8_lossy(&o.stdout)
                .lines()
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
                .collect();

            let apps: Vec<serde_json::Value> = pkgs.iter().map(|p| {
                json!({
                    "package": p,
                    "name": p.split('.').last().unwrap_or(p),
                    "icon": null,
                    "running": false,
                })
            }).collect();

            (200, json!({"apps": apps}).to_string())
        }
        Err(e) => (500, json!({"error": e.to_string()}).to_string()),
    }
}




// ================= 镜像管理（导入 + 官方拉取） =================

#[derive(serde::Serialize, Clone, Default)]
pub struct ImageTask {
    pub running: bool,
    pub done: bool,
    pub lines: Vec<String>,
    pub error: String,
    pub result: String,
    /// 结构化进度：阶段名 + 已完成字节 + 总字节（0=不定态）
    pub phase: String,
    pub progress: u64,
    pub progress_total: u64,
}

fn image_task_get(state: &Arc<AppState>) -> Resp {
    let t = state.image_task.lock().unwrap_or_else(|e| e.into_inner());
    (200, serde_json::to_string(&*t).unwrap_or_default())
}

/// 单任务模型：同一时间只允许一个导入/拉取（互斥足够，队列属于过度设计）。
fn spawn_image_task(state: &Arc<AppState>, title: &str, f: impl FnOnce(&dyn Fn(&str), &std::sync::atomic::AtomicBool, mwine_supervisor::cloud::ProgressFn) -> Result<std::path::PathBuf, mwine_supervisor::Error> + Send + 'static) -> Resp {
    {
        let mut t = state.image_task.lock().unwrap_or_else(|e| e.into_inner());
        if t.running {
            return (409, json!({"ok": false, "error": "已有镜像任务在进行中"}).to_string());
        }
        *t = ImageTask { running: true, lines: vec![title.to_string()], ..Default::default() };
    }
    let flag = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    *state.image_cancel.lock().unwrap_or_else(|e| e.into_inner()) = flag.clone();
    let st = state.clone();
    std::thread::spawn(move || {
        let st2 = st.clone();
        let log = move |line: &str| {
            println!("[image] {line}");
            let mut t = st2.image_task.lock().unwrap_or_else(|e| e.into_inner());
            t.lines.push(line.to_string());
            if t.lines.len() > 200 {
                let excess = t.lines.len() - 200;
                t.lines.drain(0..excess);
            }
        };
        let st3 = st.clone();
        let prog = move |phase: &str, done: u64, total: u64| {
            let mut t = st3.image_task.lock().unwrap_or_else(|e| e.into_inner());
            t.phase = phase.to_string();
            t.progress = done;
            t.progress_total = total;
        };
        match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(&log, &flag, &prog))) {
            Ok(Ok(dest)) => {
                let mut t = st.image_task.lock().unwrap_or_else(|e| e.into_inner());
                t.running = false;
                t.done = true;
                t.result = dest.display().to_string();
            }
            Ok(Err(e)) => {
                let mut t = st.image_task.lock().unwrap_or_else(|e| e.into_inner());
                t.running = false;
                // LaunchFailed 的 Display 前缀对用户无意义，剥掉
                t.error = e
                    .to_string()
                    .trim_start_matches("emulator launch failed: ")
                    .to_string();
            }
            Err(_) => {
                let mut t = st.image_task.lock().unwrap_or_else(|e| e.into_inner());
                t.running = false;
                t.error = "任务内部崩溃".into();
            }
        }
    });
    (200, json!({"ok": true}).to_string())
}

fn image_import(state: &Arc<AppState>, body: &str) -> Resp {
    let v: serde_json::Value = match serde_json::from_str(body) {
        Ok(v) => v,
        Err(e) => return (400, json!({"error": format!("JSON 错误: {e}")}).to_string()),
    };
    let path = v["path"].as_str().unwrap_or_default().to_string();
    if path.is_empty() {
        return (400, json!({"error": "缺少 path"}).to_string());
    }
    let sha256 = v["sha256"].as_str().map(String::from);
    let name = v["name"].as_str().unwrap_or("houttuynia").to_string();
    let sdk = match mwine_supervisor::Sdk::detect() {
        Ok(s) => s,
        Err(e) => return (500, json!({"error": format!("SDK 探测失败: {e}")}).to_string()),
    };
    let title = format!("导入 {}", std::path::Path::new(&path).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default());
    spawn_image_task(state, &title, move |log, cancel, prog| {
        mwine_supervisor::import_image(std::path::Path::new(&path), sha256.as_deref(), &name, &sdk, log, cancel, prog)
    })
}

fn image_official_pull(state: &Arc<AppState>, body: &str) -> Resp {
    let v: serde_json::Value = match serde_json::from_str(body) {
        Ok(v) => v,
        Err(e) => return (400, json!({"error": format!("JSON 错误: {e}")}).to_string()),
    };
    let target = v["target"].as_str().unwrap_or_default().to_string();
    if target.is_empty() {
        return (400, json!({"error": "缺少 target"}).to_string());
    }
    let sdk = match mwine_supervisor::Sdk::detect() {
        Ok(s) => s,
        Err(e) => return (500, json!({"error": format!("SDK 探测失败: {e}")}).to_string()),
    };
    let proxy = v["proxy"].as_str().unwrap_or_default().to_string();
    let title = format!("拉取官方镜像 {target}");
    spawn_image_task(state, &title, move |log, cancel, prog| {
        mwine_supervisor::official_pull(&target, &sdk, log, if proxy.is_empty() { None } else { Some(proxy.as_str()) }, cancel, prog)
    })
}

fn image_official_list(url: &str) -> Resp {
    let q = url.split_once('?').map(|(_, q)| q).unwrap_or("");
    let getq = |k: &str| -> Option<String> {
        let key = format!("{k}=");
        q.split('&').find_map(|p| p.strip_prefix(&key).map(|v| v.to_string()))
    };
    let filter = getq("filter");
    let proxy = getq("proxy").unwrap_or_default();
    match mwine_supervisor::official_list(filter.as_deref(), if proxy.is_empty() { None } else { Some(proxy.as_str()) }) {
        Ok(list) => {
            let items: Vec<serde_json::Value> = list
                .iter()
                .map(|i| json!({
                    "path": i.path,
                    "display": i.path.split(';').nth(1)
                        .and_then(|a| mwine_supervisor::cloud::android_version_name(a)),
                    "size": i.size,
                    "url": i.url,
                }))
                .collect();
            (200, json!({"images": items}).to_string())
        }
        Err(e) => (502, json!({"error": format!("官方仓库不可达: {e}（可配 https_proxy）")}).to_string()),
    }
}

/// AVD 双主目录查找：AVD 实体可能在 bundled 主目录（Application Support）或
/// 开发态主目录（~/.android/avd）——由 daemon 的启动环境决定，历史上分裂过。
/// 返回 (avd 目录, 所属主目录)。
fn find_avd_dir(name: &str) -> Option<(std::path::PathBuf, std::path::PathBuf)> {
    let home = std::env::var_os("HOME").map(std::path::PathBuf::from).unwrap_or_default();
    let mut homes: Vec<std::path::PathBuf> = Vec::new();
    if let Ok(sdk) = mwine_supervisor::Sdk::detect() {
        homes.push(sdk.avd_home());
    }
    homes.push(home.join(".android/avd"));
    homes.push(home.join("Library/Application Support/MWine/avd"));
    for h in homes {
        let d = h.join(format!("{name}.avd"));
        if d.join("config.ini").is_file() {
            return Some((d, h));
        }
    }
    None
}

/// 解析"当前生效的 AVD"（带自愈）：
/// 1) config.avd 指名的；2) 产品种子名 MWine4；3) 任一台主目录里带 sysdir 的首个 AVD。
/// 情形 2/3 会把 config.avd 回写为实际名（历史上改名漂移产生过幽灵名）。
fn resolve_active_avd() -> Option<(std::path::PathBuf, std::path::PathBuf, String)> {
    let named = VmConfig::load().avd;
    if let Some((d, h)) = find_avd_dir(&named) {
        return Some((d, h, named));
    }
    if let Some((d, h)) = find_avd_dir("MWine4") {
        let mut cfg = VmConfig::load();
        cfg.avd = "MWine4".to_string();
        let _ = cfg.save();
        println!("[avd] 自愈：幽灵名 {named} → MWine4");
        return Some((d, h, "MWine4".to_string()));
    }
    // 兜底扫描：两台主目录中任一含 image.sysdir 的 AVD
    let home = std::env::var_os("HOME").map(std::path::PathBuf::from).unwrap_or_default();
    for h in [home.join(".android/avd"), home.join("Library/Application Support/MWine/avd")] {
        let Ok(rd) = std::fs::read_dir(&h) else { continue };
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().trim_end_matches(".avd").to_string();
            let cfg_p = e.path().join("config.ini");
            if e.path().is_dir()
                && std::fs::read_to_string(&cfg_p)
                    .map(|t| t.contains("image.sysdir.1"))
                    .unwrap_or(false)
            {
                let mut cfg = VmConfig::load();
                cfg.avd = name.clone();
                let _ = cfg.save();
                println!("[avd] 自愈：幽灵名 {named} → {name}");
                return Some((e.path(), h, name));
            }
        }
    }
    None
}

/// 读当前 AVD 的 image.sysdir.1（无 AVD/无配置返回 None）。
fn current_avd_sysdir() -> Option<String> {
    let (dir, _, _) = resolve_active_avd()?;
    let text = std::fs::read_to_string(dir.join("config.ini")).ok()?;
    text.lines()
        .find(|l| l.trim_start().starts_with("image.sysdir.1"))
        .and_then(|l| l.split('=').nth(1))
        .map(|v| v.trim().trim_end_matches('/').to_string())
}

/// AVD 名 = 模拟器窗口标题的唯一来源（本版本 displayname/-name 均无效）。
/// 把 AVD 重命名为版本化标签（如 Android13-Houttuynia / Android16-AOSP），
/// 标题随之显示 "Android Emulator - Android13-Houttuynia:5554"。
/// 幂等；必须在 VM 关停状态下调用。返回（可能不变的）AVD 名。
fn relabel_avd(sdk: &mwine_supervisor::Sdk, sysdir_rel: &str) -> Option<String> {
    let mut it = sysdir_rel.trim_matches('/').split('/');
    let _ = it.next()?; // system-images
    let api_seg = it.next()?; // android-33
    let tag = it.next().unwrap_or("default");
    let ver = api_seg.trim_start_matches("android-").split('-').next()?;
    let suffix = if tag.contains("houttuynia") {
        "Houttuynia".to_string()
    } else if tag == "default" {
        "AOSP".to_string()
    } else {
        tag.replace(['-', '_'], "")
    };
    let new_name = format!("Android{ver}-{suffix}");
    let Some((from_dir, home, cur)) = resolve_active_avd() else {
        return None; // 无 AVD：跳过改名
    };
    if cur == new_name {
        return Some(new_name); // 已是目标名
    }
    let mut cfg = VmConfig::load();
    cfg.avd = cur.clone();
    let to_dir = home.join(format!("{new_name}.avd"));
    let from_ini = home.join(format!("{cur}.ini"));
    let to_ini = home.join(format!("{new_name}.ini"));
    // 旧名字可能钉住的运行时文件一并清掉
    let _ = std::fs::remove_file(from_dir.join("hardware-qemu.ini"));
    if std::fs::rename(&from_dir, &to_dir).is_err() {
        return None;
    }
    if from_ini.is_file() {
        let _ = std::fs::rename(&from_ini, &to_ini);
        if let Ok(txt) = std::fs::read_to_string(&to_ini) {
            let new_txt = txt.replace(&format!("{cur}.avd"), &format!("{new_name}.avd"));
            let _ = std::fs::write(&to_ini, new_txt);
        }
    }
    cfg.avd = new_name.clone();
    let _ = cfg.save();
    println!("[avd] 重命名 {cur} → {new_name}（窗口标题随镜像版本）");
    Some(new_name)
}

fn sdk_root() -> std::path::PathBuf {
    mwine_supervisor::Sdk::detect()
        .map(|s| s.root)
        .unwrap_or_else(|_| {
            std::env::var_os("HOME")
                .map(std::path::PathBuf::from)
                .unwrap_or_default()
                .join("Library/Android/sdk")
        })
}

fn image_list(state: &Arc<AppState>) -> Resp {
    let _ = state;
    let sdk = match mwine_supervisor::Sdk::detect() {
        Ok(s) => s,
        Err(e) => return (500, json!({"error": format!("SDK 探测失败: {e}")}).to_string()),
    };
    let current_sysdir = current_avd_sysdir();
    let mut out = Vec::new();
    let sysdir = sdk.root.join("system-images");
    let _ = std::fs::create_dir_all(&sysdir);
    if let Ok(apis) = std::fs::read_dir(&sysdir) {
        for api in apis.filter_map(|e| e.ok()) {
            let api_dir = api.path();
            let api_name = api.file_name().to_string_lossy().into_owned();
            if let Ok(tags) = std::fs::read_dir(&api_dir) {
                for tag in tags.filter_map(|e| e.ok()) {
                    let abi = tag.path().join("arm64-v8a");
                    if !abi.join("system-qemu.img").is_file() && !abi.join("system.img").is_file() {
                        continue;
                    }
                    let mut size = 0u64;
                    if let Ok(files) = std::fs::read_dir(&abi) {
                        for f in files.filter_map(|e| e.ok()) {
                            if let Ok(m) = f.metadata() {
                                size += m.len();
                            }
                        }
                    }
                    // du 语义的实占更接近用户感知，但 std 无 statvfs 磀粒度；给逻辑大小即可
                    let rel = format!(
                        "system-images/{}/{}/arm64-v8a",
                        api_name,
                        tag.file_name().to_string_lossy()
                    );
                    out.push(json!({
                        "api": api_name,
                        "display": mwine_supervisor::cloud::android_version_name(&api_name),
                        "name": tag.file_name().to_string_lossy().into_owned(),
                        "path": abi.display().to_string(),
                        "rel": rel,
                        "in_use": current_sysdir.as_deref() == Some(rel.as_str()),
                        "size_mb": size / 1048576,
                    }));
                }
            }
        }
    }
    (200, json!({"images": out}).to_string())
}


fn image_delete(state: &Arc<AppState>, body: &str) -> Resp {
    let v: serde_json::Value = match serde_json::from_str(body) {
        Ok(v) => v,
        Err(e) => return (400, json!({"error": format!("JSON 错误: {e}")}).to_string()),
    };
    let path = std::path::PathBuf::from(v["path"].as_str().unwrap_or_default());
    let sdk = match mwine_supervisor::Sdk::detect() {
        Ok(s) => s,
        Err(e) => return (500, json!({"error": format!("{e}")}).to_string()),
    };
    let sysroot = sdk.root.join("system-images");
    // 路径安全：必须在 system-images 下、是目录、确实是镜像目录（含 system*.img）
    let canon = path.canonicalize().unwrap_or(path.clone());
    if !canon.starts_with(&sysroot)
        || !canon.is_dir()
        || (!canon.join("system-qemu.img").is_file() && !canon.join("system.img").is_file())
    {
        return (400, json!({"error": "非法的镜像目录"}).to_string());
    }
    // 在用检测：当前 AVD 指向的镜像不许删
    // 与 config.ini 的 image.sysdir.1 同构：system-images/<api>/<name>/arm64-v8a（去尾斜杠）
    let rel = canon
        .strip_prefix(&sdk.root)
        .map(|r| r.display().to_string().trim_end_matches('/').to_string())
        .unwrap_or_default();
    if let Some(cur) = current_avd_sysdir() {
        if cur.trim_end_matches('/') == rel {
            return (409, json!({"error": "该镜像正在使用中，请先切换到其他镜像"}).to_string());
        }
    }
    // 运行中检测：VM 在跑时一律拒绝删除（qemu 会 mmap 镜像文件；ps 扫描命令行
    // 的启发式会被恰好含关键字的无关进程误伤——改用 adb 权威判据）
    if let Ok(ds) = state.adb.devices() {
        if ds.iter().any(|d| d.state == "device" && d.serial.starts_with("emulator-")) {
            return (409, json!({"error": "虚拟机运行中，请先关停再删除镜像"}).to_string());
        }
    }
    match std::fs::remove_dir_all(&canon) {
        Ok(_) => {
            // 清掉因此变空的父目录（…/android-21/default 这类空壳不留）
            let mut cur = canon.parent().map(|p| p.to_path_buf());
            while let Some(d) = cur {
                if d == sysroot || std::fs::read_dir(&d).map(|mut r| r.next().is_some()).unwrap_or(true) {
                    break;
                }
                std::fs::remove_dir(&d).ok();
                cur = d.parent().map(|p| p.to_path_buf());
            }
            (200, json!({"ok": true}).to_string())
        }
        Err(e) => (500, json!({"error": format!("删除失败: {e}")}).to_string()),
    }
}

fn canonical_display(p: &std::path::Path) -> String { p.display().to_string() }

/// 切换当前 AVD 使用的镜像。完整业务流：
/// 1. 先校验路径合法性（无效则不动虚拟机）
/// 2. VM 运行中且未确认 → 409 needs_confirm（UI 弹确认框）
/// 3. stop_vm=true：不存档直接关停（切换后快照本就作废）→ 切换 → 自动用新镜像重启
fn image_use(state: &Arc<AppState>, body: &str) -> Resp {
    let v: serde_json::Value = match serde_json::from_str(body) {
        Ok(v) => v,
        Err(e) => return (400, json!({"error": format!("JSON 错误: {e}")}).to_string()),
    };
    let path = std::path::PathBuf::from(v["path"].as_str().unwrap_or_default());
    let stop_vm = v["stop_vm"].as_bool().unwrap_or_else(|| v["stop_vm"].as_str() == Some("true"));
    let sdk = match mwine_supervisor::Sdk::detect() {
        Ok(s) => s,
        Err(e) => return (500, json!({"error": format!("{e}")}).to_string()),
    };
    // ---- 先校验，避免停了机才报路径错 ----
    let sysroot = sdk.root.join("system-images");
    let canon = path.canonicalize().unwrap_or(path.clone());
    if !canon.starts_with(&sysroot) || canon.join("system.img").symlink_metadata().is_err() {
        return (400, json!({"error": "非法的镜像目录"}).to_string());
    }
    let vm_running = state
        .adb
        .devices()
        .map(|ds| ds.iter().any(|d| d.state == "device" && d.serial.starts_with("emulator-")))
        .unwrap_or(false);
    if vm_running && !stop_vm {
        return (
            409,
            json!({"running": true, "needs_confirm": true, "error": "虚拟机运行中，切换需要先关停"}).to_string(),
        );
    }

    // ---- 关停（不存档：旧镜像的快照切换后即作废）----
    if vm_running {
        let serial = state
            .adb
            .devices()
            .ok()
            .and_then(|ds| {
                ds.into_iter()
                    .find(|d| d.state == "device" && d.serial.starts_with("emulator-"))
                    .map(|d| d.serial)
            })
            .unwrap_or_else(|| "emulator-5554".to_string());
        if let Err(e) =
            mwine_supervisor::emu_kill_wait(&state.adb, &serial, std::time::Duration::from_secs(35))
        {
            return (500, json!({"error": format!("关停虚拟机失败: {e}")}).to_string());
        }
        *state.qemu_pid.lock().unwrap_or_else(|e| e.into_inner()) = None;
    }

    // ---- 切换 sysdir + 清不兼容快照 ----
    let rel = canon
        .strip_prefix(&sdk.root)
        .map(|r| r.display().to_string())
        .unwrap_or_default();
    let Some((avd_dir, _, _)) = resolve_active_avd() else {
        return (
            500,
            json!({"error": "找不到可用的 AVD（两台主目录均无）。请先在概览页点一次启动完成初始化"}).to_string(),
        );
    };
    let cfg = avd_dir.join("config.ini");
    let text = std::fs::read_to_string(&cfg).unwrap_or_default();
    let new_text: String = text
        .lines()
        .map(|l| {
            if l.trim_start().starts_with("image.sysdir.1") {
                format!("image.sysdir.1 = {rel}/")
            } else {
                l.to_string()
            }
        })
        .chain(std::iter::once(String::new()))
        .collect::<Vec<_>>()
        .join("\n");
    if std::fs::write(&cfg, new_text).is_err() {
        return (500, json!({"error": "写入 AVD 配置失败"}).to_string());
    }
    for f in ["snapshots", "hardware-qemu.ini"] {
        let p = avd_dir.join(f);
        if p.is_dir() {
            let _ = std::fs::remove_dir_all(&p);
        } else if p.is_file() {
            let _ = std::fs::remove_file(&p);
        }
    }
    for e in std::fs::read_dir(&avd_dir).into_iter().flatten().filter_map(|e| e.ok()) {
        if e.file_name().to_string_lossy().ends_with(".qcow2") {
            let _ = std::fs::remove_file(e.path());
        }
    }

    // AVD 改名跟随新镜像版本（窗口标题）
    relabel_avd(&sdk, &rel);

    // ---- 原本在跑就自动用新镜像重启 ----
    if vm_running {
        vm_start(state);
    }
    (200, json!({"ok": true, "restarted": vm_running, "sysdir": rel}).to_string())
}

fn image_cancel(state: &Arc<AppState>) -> Resp {
    let flag = state.image_cancel.lock().unwrap_or_else(|e| e.into_inner()).clone();
    let was_running = state.image_task.lock().unwrap_or_else(|e| e.into_inner()).running;
    if !was_running {
        return (409, json!({"error": "当前没有进行中的镜像任务"}).to_string());
    }
    flag.store(true, std::sync::atomic::Ordering::Relaxed);
    {
        let mut t = state.image_task.lock().unwrap_or_else(|e| e.into_inner());
        t.lines.push("收到取消请求，正在中止…".into());
    }
    (200, json!({"ok": true}).to_string())
}
