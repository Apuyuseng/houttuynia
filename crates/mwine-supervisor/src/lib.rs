//! mwine-supervisor：Android SDK / AVD / emulator 实例的生命周期管理。
//!
//! 当前以 Android SDK 的 emulator（QEMU + HVF 封装）为 VM 底座；
//! 后续替换为自建精简 AOSP 镜像 + 裸 QEMU 时，接口保持不变。

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

pub mod cloud;
pub use cloud::{import_image, official_list, official_pull};

use mwine_adb::AdbClient;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("Android SDK not found (tried ANDROID_HOME, ANDROID_SDK_ROOT, ~/Library/Android/sdk)")]
    SdkNotFound,
    #[error("emulator binary not found: {0}")]
    EmulatorBinaryNotFound(String),
    #[error("no AVD named {0} (available: {1})")]
    AvdNotFound(String, String),
    #[error("emulator launch failed: {0}")]
    LaunchFailed(String),
    #[error("emulator did not appear on adb within timeout")]
    SerialTimeout,
    #[error("boot timeout (sys.boot_completed never reached 1)")]
    BootTimeout,
    #[error("adb error: {0}")]
    Adb(#[from] mwine_adb::Error),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

/// 探测到的 Android SDK。
#[derive(Debug, Clone)]
pub struct Sdk {
    pub root: PathBuf,
}

fn home_dir() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/Users/Shared"))
}

impl Sdk {
    pub fn detect() -> Result<Self> {
        // 优先级：显式环境变量 > .app 内嵌运行时（零依赖安装）> 用户 SDK
        // 内嵌运行时布局：<MWine.app>/Contents/Resources/runtime/{emulator,platform-tools,system-images}
        // exe = <MWine.app>/Contents/MacOS/<bin>；ancestors[2] = Contents
        let bundled = std::env::current_exe()
            .ok()
            .and_then(|p| p.ancestors().nth(2).map(|a| a.to_path_buf()))
            .map(|a| a.join("Resources/runtime"))
            .filter(|r| r.join("emulator").is_dir());
        let candidates: Vec<PathBuf> = [
            std::env::var_os("MWINE_SDK_ROOT"),
            std::env::var_os("ANDROID_HOME"),
            std::env::var_os("ANDROID_SDK_ROOT"),
        ]
        .into_iter()
        .flatten()
        .map(PathBuf::from)
        .chain(bundled)
        .chain(std::iter::once(home_dir().join("Library/Android/sdk")))
        .collect();
        for c in candidates {
            if c.join("emulator").is_dir() {
                return Ok(Self { root: c });
            }
        }
        Err(Error::SdkNotFound)
    }

    pub fn emulator_bin(&self) -> PathBuf {
        self.root.join("emulator/emulator")
    }

    pub fn adb_bin(&self) -> PathBuf {
        self.root.join("platform-tools/adb")
    }

    pub fn avd_home(&self) -> PathBuf {
        if let Some(p) = std::env::var_os("ANDROID_AVD_HOME") {
            return PathBuf::from(p);
        }
        if self.is_bundled() {
            // 零依赖模式：AVD 放 Application Support（首启从 bundle 模板种子）
            let d = home_dir().join("Library/Application Support/MWine/avd");
            std::fs::create_dir_all(&d).ok();
            d
        } else {
            home_dir().join(".android/avd")
        }
    }

    /// 是否使用 .app 内嵌运行时（零依赖安装模式）。
    pub fn is_bundled(&self) -> bool {
        // detect() 的候选链里 bundle 路径的父级含 ".app/Contents"
        self.root
            .ancestors()
            .any(|a| a.to_string_lossy().contains(".app/Contents"))
    }

    /// 零依赖模式首启：从 bundle 的 avd-template 种子出 AVD（幂等）。
    pub fn seed_avd_if_needed(&self, name: &str) {
        if !self.is_bundled() {
            return;
        }
        let avd_home = self.avd_home();
        let ini = avd_home.join(format!("{name}.ini"));
        let dir = avd_home.join(format!("{name}.avd"));
        if ini.is_file() && dir.is_dir() {
            return;
        }
        let template = self.root.join("avd-template");
        if !template.is_dir() {
            eprintln!("[avd] bundle 内无 avd-template，无法种子 {name}");
            return;
        }
        let _ = std::fs::create_dir_all(&dir);
        let _ = std::fs::copy(template.join("config.ini"), dir.join("config.ini"));
        let ini_content = format!("avd.ini.encoding=UTF-8\npath={}\ntarget=android-33\n", dir.display());
        let _ = std::fs::write(&ini, ini_content);
        println!("[avd] 已从 bundle 模板种子 {name} → {}", dir.display());
    }

    /// 在 build-tools 各版本目录中按版本降序查找工具二进制（如 aapt2）。
    pub fn find_build_tool(&self, name: &str) -> Option<PathBuf> {
        let bt = self.root.join("build-tools");
        let mut versions: Vec<PathBuf> = std::fs::read_dir(&bt)
            .ok()?
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| p.is_dir())
            .collect();
        versions.sort();
        for dir in versions.iter().rev() {
            let candidate = dir.join(name);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
        None
    }
}

/// 一个 AVD 的描述信息（来自 <avd>.ini 与 <avd>.avd/config.ini）。
#[derive(Debug, Clone)]
pub struct AvdInfo {
    pub name: String,
    pub path: PathBuf,
    pub config: BTreeMap<String, String>,
}

impl AvdInfo {
    pub fn abi(&self) -> Option<&str> {
        self.config.get("abi.type").map(|s| s.as_str())
    }

    pub fn is_arm64(&self) -> bool {
        self.abi().is_some_and(|a| a.contains("arm64"))
    }
}

/// 解析 Java properties 格式文本。
pub fn parse_properties(text: &str) -> BTreeMap<String, String> {
    let mut map = BTreeMap::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with('!') {
            continue;
        }
        if let Some(idx) = line.find('=') {
            map.insert(
                line[..idx].trim().to_string(),
                line[idx + 1..].trim().to_string(),
            );
        }
    }
    map
}

impl Sdk {
    /// 列出本机全部 AVD。
    pub fn list_avds(&self) -> Vec<AvdInfo> {
        let mut avds = Vec::new();
        let home = self.avd_home();
        let Ok(entries) = std::fs::read_dir(&home) else {
            return avds;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("ini") {
                continue;
            }
            let Some(name) = path.file_stem().and_then(|n| n.to_str()) else {
                continue;
            };
            let Ok(text) = std::fs::read_to_string(&path) else {
                continue;
            };
            let meta = parse_properties(&text);
            let avd_dir = meta
                .get("path")
                .cloned()
                .map(PathBuf::from)
                .unwrap_or_else(|| home.join(format!("{name}.avd")));
            let config = std::fs::read_to_string(avd_dir.join("config.ini"))
                .map(|t| parse_properties(&t))
                .unwrap_or_default();
            avds.push(AvdInfo {
                name: name.to_string(),
                path: avd_dir,
                config,
            });
        }
        avds.sort_by(|a, b| a.name.cmp(&b.name));
        avds
    }

    pub fn find_avd(&self, name: &str) -> Result<AvdInfo> {
        let names = || -> String {
            self.list_avds()
                .iter()
                .map(|a| a.name.clone())
                .collect::<Vec<_>>()
                .join(", ")
        };
        self.list_avds()
            .into_iter()
            .find(|a| a.name == name)
            .ok_or_else(|| Error::AvdNotFound(name.into(), names()))
    }
}

/// emulator 启动选项。
#[derive(Debug, Default, Clone)]
pub struct LaunchOptions {
    /// AVD 主目录覆盖（child 的 ANDROID_AVD_HOME）。AVD 实体可能在另一台主目录
    /// （bundled=Application Support / 开发态=~/.android/avd），按实际位置传给 emulator
    pub avd_home: Option<PathBuf>,
    /// 附加的原始 emulator 参数（如 -memory / -cores）。
    pub extra_args: Vec<String>,
    /// 冷启动（禁用快照加载/保存）。
    pub no_snapshot: bool,
    /// 启动时加载的快照名（如 quick1）；None 且非冷启时自动探测 AVD 已有快照。
    pub snapshot: Option<String>,
    /// 自定义 sdcard 镜像路径（emulator -sdcard；空 = AVD 默认）。
    pub sdcard: Option<std::path::PathBuf>,
    /// 跳过启动动画。
    pub no_boot_anim: bool,
    /// 设备模式（手机/平板）：改写 AVD 屏幕尺寸与密度。
    pub device_mode: Option<DeviceMode>,
    /// 自建镜像目录（含 system-qemu.img / vendor-qemu.img / ramdisk-qemu.img /
    /// kernel-ranchu）。设置后忽略 AVD 名，用 emulator -sysdir 直接加载，
    /// 无需 AVD。ranchu 内核依赖 emulator QEMU 分支的 goldfish 设备模型，
    /// 因此启动器仍用 SDK emulator，仅替换镜像集。
    pub image_dir: Option<PathBuf>,
    /// HTTP 代理（如 http://127.0.0.1:7890），guest 全部流量经宿主代理。
    pub proxy: Option<String>,
    /// 用户数据目录（userdata-qemu.img 等所在）。指向宿主持久路径后，
    /// mwine 升级/重装不影响应用数据。
    pub data_dir: Option<PathBuf>,
    /// guest 内存（MB）。绕过 emulator 对 API33 镜像的 2GB 下限钳制
    /// （qemu 层 -m 直传；1.5GB 为实测甜点，1.25GB 为底线）。
    pub memory_mb: Option<u32>,
    /// 有窗口模式（渲染直通载体：主显示器+multidisplay 副屏均为原生宿主窗口，
    /// gfxstream 直接 Metal 渲染）。默认 headless。
    pub windowed: bool,
}

/// 设备模式：决定屏幕尺寸 / 密度 / 设备名（手机 or 平板形态）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceMode {
    /// 手机：1080x2400 @440dpi（约 392dp 宽，手机布局）。
    Phone,
    /// 平板：2560x1600 @320dpi 横屏（约 800dp 宽，smallestWidth≥600 触发平板 UI）。
    Tablet,
}

impl DeviceMode {
    /// 屏幕 (宽, 高, dpi)。
    pub fn lcd(&self) -> (u32, u32, u32) {
        match self {
            DeviceMode::Phone => (1080, 2400, 440),
            DeviceMode::Tablet => (2560, 1600, 320),
        }
    }

    /// AVD 的 hw.device.name（宿主侧元数据，标识形态）。
    pub fn device_name(&self) -> &'static str {
        match self {
            DeviceMode::Phone => "mwine_phone",
            DeviceMode::Tablet => "mwine_tablet",
        }
    }
}

/// 运行时应用设备模式（guest 侧 wm size/density 覆盖）。
/// 现代 resizable 镜像的显示由 guest 的 wm 设置决定（config.ini 的
/// hw.lcd 不生效），且该覆盖存于 /data——快照恢复后依然生效。
pub fn apply_device_mode_runtime(adb: &AdbClient, serial: &str, mode: DeviceMode) -> Result<()> {
    let (w, h, dpi) = mode.lcd();
    set_lcd_runtime(adb, serial, w, h, dpi)
}

/// 运行时设置任意尺寸/密度（wm size/density，立即生效）。
pub fn set_lcd_runtime(adb: &AdbClient, serial: &str, w: u32, h: u32, dpi: u32) -> Result<()> {
    let cmd = format!("wm size {w}x{h} && wm density {dpi}");
    adb.shell_blocking(serial, &cmd, Duration::from_secs(10))
        .map_err(|e| Error::LaunchFailed(format!("设置屏幕尺寸失败: {e}")))?;
    Ok(())
}

/// 把 AVD 屏幕配置改写为任意自定义尺寸（config.ini 的 hw.lcd.*）。
/// 尺寸与已存快照不一致时下次启动会冷启动一次（快照配置校验）。
pub fn set_lcd(avd: &AvdInfo, w: u32, h: u32, dpi: u32) -> Result<()> {
    let cfg = avd.path.join("config.ini");
    let text = std::fs::read_to_string(&cfg)
        .map_err(|e| Error::LaunchFailed(format!("读取 {}: {e}", cfg.display())))?;
    let mut out = String::with_capacity(text.len());
    let mut seen = std::collections::BTreeMap::new();
    for line in text.lines() {
        let key = line.split('=').next().unwrap_or("").trim();
        let new = match key {
            "hw.lcd.width" => Some(format!("hw.lcd.width = {w}")),
            "hw.lcd.height" => Some(format!("hw.lcd.height = {h}")),
            "hw.lcd.density" => Some(format!("hw.lcd.density = {dpi}")),
            "hw.initialOrientation" => Some(format!(
                "hw.initialOrientation = {}",
                if w > h { "landscape" } else { "portrait" }
            )),
            _ => None,
        };
        if let Some(n) = new {
            seen.insert(key.to_string(), true);
            out.push_str(&n);
        } else {
            out.push_str(line);
        }
        out.push('\n');
    }
    for (k, v) in [
        ("hw.lcd.width", w.to_string()),
        ("hw.lcd.height", h.to_string()),
        ("hw.lcd.density", dpi.to_string()),
        (
            "hw.initialOrientation",
            if w > h { "landscape".into() } else { "portrait".into() },
        ),
    ] {
        if !seen.contains_key(k) {
            out.push_str(&format!("{k} = {v}\n"));
        }
    }
    std::fs::write(&cfg, out)
        .map_err(|e| Error::LaunchFailed(format!("写入 {}: {e}", cfg.display())))?;
    // emulator 运行时会写回 hardware-qemu.ini 且启动时优先生效——必须同步覆盖，
    // 否则新 lcd 只改 config.ini 会被旧 hardware-qemu.ini 压住（表现为"设置不生效"）
    let hwq = avd.path.join("hardware-qemu.ini");
    if hwq.is_file() {
        if let Ok(text) = std::fs::read_to_string(&hwq) {
            let mut out2 = String::with_capacity(text.len());
            for line in text.lines() {
                let key = line.split('=').next().unwrap_or("").trim();
                let new = match key {
                    "hw.lcd.width" => Some(format!("hw.lcd.width = {w}")),
                    "hw.lcd.height" => Some(format!("hw.lcd.height = {h}")),
                    "hw.lcd.density" => Some(format!("hw.lcd.density = {dpi}")),
                    _ => None,
                };
                out2.push_str(new.as_deref().unwrap_or(line));
                out2.push('\n');
            }
            let _ = std::fs::write(&hwq, out2);
        }
    }
    Ok(())
}

/// 把 AVD 屏幕配置改写为指定设备模式（config.ini 的 hw.lcd.* 与 hw.device.name）。
/// guest 的 UI 布局（手机/平板）由 dp 宽度自动判定，无需镜像侧改动。
pub fn apply_device_mode(avd: &AvdInfo, mode: DeviceMode) -> Result<()> {
    let (w, h, dpi) = mode.lcd();
    let cfg = avd.path.join("config.ini");
    let text = std::fs::read_to_string(&cfg)
        .map_err(|e| Error::LaunchFailed(format!("读取 {}: {e}", cfg.display())))?;
    let mut out = String::with_capacity(text.len());
    let mut seen = std::collections::BTreeMap::new();
    for line in text.lines() {
        let key = line.split('=').next().unwrap_or("").trim();
        let new = match key {
            "hw.lcd.width" => Some(format!("hw.lcd.width = {w}")),
            "hw.lcd.height" => Some(format!("hw.lcd.height = {h}")),
            "hw.lcd.density" => Some(format!("hw.lcd.density = {dpi}")),
            "hw.device.name" => Some(format!("hw.device.name = {}", mode.device_name())),
            _ => None,
        };
        if let Some(n) = new {
            seen.insert(key.to_string(), true);
            out.push_str(&n);
        } else {
            out.push_str(line);
        }
        out.push('\n');
    }
    // config.ini 缺键时补齐（AVD 由脚本生成时可能缺）
    for (k, v) in [
        ("hw.lcd.width", w.to_string()),
        ("hw.lcd.height", h.to_string()),
        ("hw.lcd.density", dpi.to_string()),
        ("hw.device.name", mode.device_name().to_string()),
    ] {
        if !seen.contains_key(k) {
            out.push_str(&format!("{k} = {v}\n"));
        }
    }
    std::fs::write(&cfg, out)
        .map_err(|e| Error::LaunchFailed(format!("写入 {}: {e}", cfg.display())))?;
    // emulator 运行时会写回 hardware-qemu.ini 且启动时优先生效——必须同步覆盖，
    // 否则新 lcd 只改 config.ini 会被旧 hardware-qemu.ini 压住（表现为"设置不生效"）
    let hwq = avd.path.join("hardware-qemu.ini");
    if hwq.is_file() {
        if let Ok(text) = std::fs::read_to_string(&hwq) {
            let mut out2 = String::with_capacity(text.len());
            for line in text.lines() {
                let key = line.split('=').next().unwrap_or("").trim();
                let new = match key {
                    "hw.lcd.width" => Some(format!("hw.lcd.width = {w}")),
                    "hw.lcd.height" => Some(format!("hw.lcd.height = {h}")),
                    "hw.lcd.density" => Some(format!("hw.lcd.density = {dpi}")),
                    _ => None,
                };
                out2.push_str(new.as_deref().unwrap_or(line));
                out2.push('\n');
            }
            let _ = std::fs::write(&hwq, out2);
        }
    }
    Ok(())
}

/// 探测 AVD 已保存的快照名：优先产品基准档 `mwine_default`（由 vm stop 存档、
/// vm start 加载的固定闭环），否则取最新修改的一个。
pub fn latest_snapshot(avd: &AvdInfo) -> Option<String> {
    let dir = avd.path.join("snapshots");
    if dir.join("mwine_default").join("snapshot.pb").is_file() {
        return Some("mwine_default".to_string());
    }
    let mut best: Option<(std::time::SystemTime, String)> = None;
    for entry in std::fs::read_dir(&dir).ok()?.flatten() {
        if entry.file_type().ok().map_or(false, |t| t.is_dir()) {
            if let Ok(modified) = entry.metadata().and_then(|m| m.modified()) {
                let name = entry.file_name().to_string_lossy().to_string();
                if best.as_ref().map_or(true, |(t, _)| modified > *t) {
                    best = Some((modified, name));
                }
            }
        }
    }
    best.map(|(_, n)| n)
}

/// 关停前显式存档到 `mwine_default`（emu kill 不会自动回存 -snapshot 指定的档）。
/// 保存是异步的：等待快照目录 mtime 前进或超时。
pub fn save_snapshot_blocking(serial: &str, name: &str, timeout: Duration) -> Result<()> {
    let home = std::env::var_os("HOME")
        .map(std::path::PathBuf::from)
        .ok_or_else(|| Error::LaunchFailed("无 HOME".into()))?;
    let avd_home = home.join(".android/avd");
    // 从 serial 反查 AVD 目录：~/.android/avd/*.avd/snapshots/<name> 存在者
    let mut snap_dir = None;
    for entry in std::fs::read_dir(&avd_home).ok().into_iter().flatten().flatten() {
        let p = entry.path().join("snapshots").join(name);
        if p.is_dir() {
            snap_dir = Some(p);
            break;
        }
    }
    let snap_dir = snap_dir.ok_or_else(|| Error::LaunchFailed(format!("快照 {name} 不存在")))?;
    // 保存过程就地重写 ram.bin/snapshot.pb 等，目录 mtime 不动 → 盯文件
    let mark = snap_dir.join("snapshot.pb");
    let before = std::fs::metadata(&mark).and_then(|m| m.modified()).ok();

    let mut token_path = home.clone();
    token_path.push(".emulator_console_auth_token");
    let token = std::fs::read_to_string(token_path).unwrap_or_default();
    // emulator-5554：console 在 5554（与 serial 同号），adb 才是 5555
    let port: u16 = serial
        .rsplit('-')
        .next()
        .and_then(|s| s.parse().ok())
        .unwrap_or(5554);

    use std::io::{BufRead, BufReader, Write};
    // stderr 日志里已出现过的保存完成行数（本次保存 = 新增一行）
    let mut saved_marker = 0usize;
    if let Some(avd) = find_avd_by_snapshot(home.as_path(), name) {
        let log = home.join(".mwine/logs").join(format!("emu-{avd}.log"));
        if let Ok(body) = std::fs::read_to_string(&log) {
            saved_marker = body
                .lines()
                .filter(|l| l.contains(&format!("Saving snapshot '{name}' using")))
                .count();
        }
    }
    let stream = std::net::TcpStream::connect(("127.0.0.1", port))
        .map_err(|e| Error::LaunchFailed(format!("console {port}: {e}")))?;
    stream.set_read_timeout(Some(Duration::from_secs(3))).ok();
    let mut reader = BufReader::new(stream.try_clone().map_err(|e| Error::LaunchFailed(e.to_string()))?);
    let mut line = String::new();
    let _ = reader.read_line(&mut line); // banner
    let mut w = stream;
    let _ = writeln!(w, "auth {token}");
    let _ = writeln!(w, "avd snapshot save {name}");
    let _ = w.flush();

    let deadline = Instant::now() + timeout;
    let mut last_mtime = before;
    let mut stable_rounds = 0;
    while Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(500));
        // 确认信号（按可靠性排序）：
        // 1) emulator stderr 日志出现本次保存的 "Saving snapshot '<name>' using N ms"
        //    （supervisor 已把 stderr 落盘 ~/.mwine/logs/emu-<serial 对应 avd>.log）
        // 2) snapshot.pb mtime 前进后且连续 2 轮稳定（写完不再变动）
        if let Some(avd) = find_avd_by_snapshot(home.as_path(), name) {
            let log = home.join(".mwine/logs").join(format!("emu-{avd}.log"));
            if let Ok(body) = std::fs::read_to_string(&log) {
                let hits = body
                    .lines()
                    .filter(|l| l.contains(&format!("Saving snapshot '{name}' using")))
                    .count();
                if hits > saved_marker {
                    saved_marker = hits;
                    return Ok(());
                }
            }
        }
        if let Ok(m) = std::fs::metadata(&mark).and_then(|m| m.modified()) {
            let advanced = last_mtime.map_or(true, |b| m > b);
            if advanced {
                last_mtime = Some(m);
                stable_rounds = 0;
            } else if last_mtime.is_some() && before.map_or(true, |b| m > b) {
                stable_rounds += 1;
                if stable_rounds >= 2 {
                    return Ok(());
                }
            }
        }
    }
    Err(Error::LaunchFailed(format!("快照 {name} 保存等待超时")))
}

/// 由快照名反查 AVD 名（快照所在 avd 目录名去掉 .avd 后缀）。
fn find_avd_by_snapshot(home: &std::path::Path, name: &str) -> Option<String> {
    let avd_home = home.join(".android/avd");
    for entry in std::fs::read_dir(&avd_home).ok()?.flatten() {
        if entry.path().join("snapshots").join(name).is_dir() {
            let n = entry.file_name().to_string_lossy().to_string();
            return Some(n.trim_end_matches(".avd").to_string());
        }
    }
    None
}

/// 关停并等待 emulator 完全退出（优雅关停最长 ~20s；不等完就重启会端口冲突双杀）。
pub fn emu_kill_wait(adb: &AdbClient, serial: &str, timeout: Duration) -> Result<()> {
    emu_console_kill(serial)?;
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(500));
        let gone = adb
            .devices()
            .map(|ds| !ds.iter().any(|d| d.serial == serial))
            .unwrap_or(true);
        if gone {
            return Ok(());
        }
    }
    Err(Error::LaunchFailed(format!("{serial} 关停等待超时")))
}

/// 运行中的 emulator 实例。
pub struct EmulatorHandle {
    pub avd: String,
    pub serial: String,
    pub child: Child,
}

impl EmulatorHandle {
    /// emulator 子进程 pid（守护进程跟踪存活用）
    pub fn child_id(&self) -> u32 {
        self.child.id()
    }

    /// headless 启动一个 AVD 或自建镜像目录，并等待其出现在 adb 设备列表。
    pub fn launch_headless(
        sdk: &Sdk,
        avd_name: &str,
        opts: &LaunchOptions,
        adb: &AdbClient,
    ) -> Result<Self> {
        let bin = sdk.emulator_bin();
        if !bin.is_file() {
            return Err(Error::EmulatorBinaryNotFound(bin.display().to_string()));
        }

        let before: Vec<String> = adb
            .devices()
            .unwrap_or_default()
            .into_iter()
            .map(|d| d.serial)
            .collect();

        let mut cmd = Command::new(&bin);
        // emulator 需要 ANDROID_SDK_ROOT 解析系统路径（自建镜像模式 -sysdir 覆盖后依然校验）
        cmd.env("ANDROID_SDK_ROOT", &sdk.root);
        // 零依赖模式：AVD home 一致性（种子 + 子进程环境）
        if sdk.is_bundled() {
            sdk.seed_avd_if_needed(avd_name);
            cmd.env("ANDROID_AVD_HOME", sdk.avd_home());
            // emulator 内部用 ANDROID_SDK_ROOT 找 platform-tools/adb
            cmd.env("ANDROID_HOME", &sdk.root);
        }
        // AVD 实体在另一台主目录时显式覆盖（后设优先）
        if let Some(h) = &opts.avd_home {
            cmd.env("ANDROID_AVD_HOME", h);
        }
        if let Some(dir) = &opts.image_dir {
            // 自建镜像模式：校验关键文件齐全
            for f in ["system-qemu.img", "ramdisk-qemu.img", "kernel-ranchu"] {
                if !dir.join(f).is_file() {
                    return Err(Error::LaunchFailed(format!(
                        "镜像目录缺少 {f}: {}",
                        dir.display()
                    )));
                }
            }
            // ramdisk 完整性：曾出现构建机部分重编产物只有目录骨架、缺 init
            // （内核 panic "Unable to mount root fs"、adb 永久 offline）。
            // 合法 ramdisk 有两种形态：boot+vendor_ramdisk 拼接（~4.3MB，android-13
            // emu_img_zip 标准产物，含 fstab + 模块）或旧式含模块全量（18MB+）。
            // 真正的判据是文件头为 lz4 帧且解压后有 init，这里用体积下界 3MB 兜底。
            let ramdisk_len = std::fs::metadata(dir.join("ramdisk-qemu.img"))
                .map_err(|e| Error::LaunchFailed(format!("读取 ramdisk 元数据失败: {e}")))?
                .len();
            if ramdisk_len < 3 * 1024 * 1024 {
                return Err(Error::LaunchFailed(format!(
                    "ramdisk-qemu.img 仅 {}KB，构建不完整（缺 init，内核会 panic 循环）。\
                     请在构建机完整重编后重新拉取镜像目录: {}",
                    ramdisk_len / 1024,
                    dir.display()
                )));
            }
            cmd.arg("-sysdir").arg(dir);
            cmd.arg("-system").arg(dir.join("system-qemu.img"));
            let vendor = dir.join("vendor-qemu.img");
            if vendor.is_file() {
                cmd.arg("-vendor").arg(vendor);
            }
            cmd.arg("-ramdisk").arg(dir.join("ramdisk-qemu.img"));
            cmd.arg("-kernel").arg(dir.join("kernel-ranchu"));
            // 固定控制台/adb 端口：serial 可预测（emulator-5556），
            // 自建镜像冷启慢，不做差分发现
        } else {
            let avd_info = sdk.find_avd(avd_name)?;
            // 设备模式：改写屏幕尺寸/密度（手机/平板形态）
            if let Some(mode) = opts.device_mode {
                apply_device_mode(&avd_info, mode)?;
            }
            // 快照：显式指定 > 自动探测最新快照
            if !opts.no_snapshot {
                let snap = opts
                    .snapshot
                    .clone()
                    .or_else(|| latest_snapshot(&avd_info));
                if let Some(name) = snap {
                    cmd.arg("-snapshot").arg(&name);
                }
            }
            cmd.arg("-avd").arg(avd_name);
        }
        if !opts.windowed {
            cmd.arg("-no-window").arg("-no-audio");
        }
        // stdout/stderr 落盘（窗口与无头模式一致）：快照/渲染问题
        // （"gles_mode_selected"、GPU 回退原因）只打在这里，是远程排障弱机的唯一依据
        {
            let log_dir = std::env::var_os("HOME").map(|h| {
                let mut p = std::path::PathBuf::from(h);
                p.push(".mwine/logs");
                p
            });
            let handled = log_dir
                .and_then(|dir| {
                    std::fs::create_dir_all(&dir).ok()?;
                    let log = dir.join(format!("emu-{avd_name}.log"));
                    // 轮转：超 8MB 截断（append 模式会无限增长）
                    if std::fs::metadata(&log).map(|m| m.len() > 8 * 1024 * 1024).unwrap_or(false) {
                        let _ = std::fs::write(&log, "");
                    }
                    let f = std::fs::File::options().create(true).append(true).open(&log).ok()?;
                    let f2 = f.try_clone().ok();
                    cmd.stdout(Stdio::from(f));
                    if let Some(f2) = f2 {
                        cmd.stderr(Stdio::from(f2));
                    }
                    Some(())
                })
                .is_some();
            if !handled {
                cmd.stdout(Stdio::null()).stderr(Stdio::null());
            }
        }
        // GPU：不传 -gpu，走模拟器默认解析（与历史可见会话一致）。
        // 教训：强制 swiftshader_indirect 会把 Vulkan ICD 切到弃用的 SwiftShader
        // 后端（黑屏）；env -i 极简环境启动 daemon 也会让 Metal 呈现异常。
        if opts.no_boot_anim {
            cmd.arg("-no-boot-anim");
        }
        if opts.no_snapshot {
            cmd.arg("-no-snapshot");
        }
        if let Some(proxy) = &opts.proxy {
            cmd.arg("-http-proxy").arg(proxy);
        }
        if let Some(dd) = &opts.data_dir {
            std::fs::create_dir_all(dd).ok();
            cmd.arg("-datadir").arg(dd);
        } else if opts.image_dir.is_some() {
            cmd.arg("-datadir").arg("/tmp/mwine-custom-data");
        }
        // emulator 自身的参数必须全部位于 -qemu 透传块之前
        // （-qemu 之后的参数原样交给 QEMU，emulator 不再解析）
        if let Some(sd) = &opts.sdcard {
            cmd.arg("-sdcard").arg(sd);
        }
        for a in &opts.extra_args {
            cmd.arg(a);
        }
        // 内存下探：qemu 层直传（emulator 会把 hw.ramSize 钳到镜像下限，绕开之）
        if let Some(mb) = opts.memory_mb {
            cmd.arg("-qemu").arg("-m").arg(format!("{mb}M"));
        }
        let child = cmd
            .spawn()
            .map_err(|e| Error::LaunchFailed(format!("{}: {e}", bin.display())))?;

        // 通过 adb 设备列表差分发现新实例的 serial
        let deadline = Instant::now() + Duration::from_secs(120);
        let serial = loop {
            if let Ok(devices) = adb.devices() {
                if let Some(new) = devices
                    .into_iter()
                    .find(|d| d.state == "device" && !before.contains(&d.serial))
                {
                    break new.serial;
                }
            }
            if Instant::now() > deadline {
                return Err(Error::SerialTimeout);
            }
            std::thread::sleep(Duration::from_millis(800));
        };

        Ok(Self {
            avd: avd_name.to_string(),
            serial,
            child,
        })
    }

    /// 等待系统完成引导（sys.boot_completed=1 且 bootanim 停止）。
    pub fn wait_boot_completed(&self, adb: &AdbClient, timeout: Duration) -> Result<()> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Ok(out) = adb.shell_blocking(
                &self.serial,
                "getprop sys.boot_completed",
                Duration::from_secs(5),
            ) {
                if out.stdout_string().trim() == "1" {
                    let anim = adb.shell_blocking(
                        &self.serial,
                        "getprop init.svc.bootanim",
                        Duration::from_secs(5),
                    );
                    let stopped = anim
                        .map(|a| a.stdout_string().trim() == "stopped")
                        .unwrap_or(true);
                    if stopped {
                        return Ok(());
                    }
                }
            }
            if Instant::now() > deadline {
                return Err(Error::BootTimeout);
            }
            std::thread::sleep(Duration::from_millis(1000));
        }
    }

    /// 优雅关停（emulator 控制台 kill），超时后强杀进程。
    pub fn shutdown(&mut self, _adb: &AdbClient) -> Result<()> {
        let _ = emu_console_kill(&self.serial);
        let deadline = Instant::now() + Duration::from_secs(20);
        while Instant::now() < deadline {
            if self.child.try_wait()?.is_some() {
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(500));
        }
        self.child.kill()?;
        let _ = self.child.wait();
        Ok(())
    }

    pub fn is_running(&mut self) -> bool {
        self.child.try_wait().map(|s| s.is_none()).unwrap_or(false)
    }
}

/// 通过 emulator 控制台协议优雅关停（与 `adb emu kill` 同路径：
/// 直连控制台端口 + auth token）。serial 形如 emulator-5554。
pub fn emu_console_kill(serial: &str) -> Result<()> {
    let resp = emu_console_cmd(serial, "kill")?;
    if resp.contains("KO") {
        return Err(Error::LaunchFailed(format!(
            "emulator 控制台拒绝 kill: {}",
            resp.trim()
        )));
    }
    Ok(())
}

/// TCP adb 备用通道端口：guest 侧 adbd 监听 5555，宿主侧 console redir 用 5055
/// （避开 emulator 自占的宿主 5555）。
pub const ADB_TCP_HOST_PORT: u16 = 5055;
pub const ADB_TCP_GUEST_PORT: u16 = 5555;

/// 确保对该 emulator 实例存在可用的 adb 控制通道。
///
/// 自建镜像（TP1A 世代）的 qemu 管道 adb 会在快照恢复或长时间运行后失联
/// （guest 本身正常）。TCP 通道是产品级兜底：guest 侧持久监听
/// `persist.adb.tcp.port=5555`（幂等补设）+ 宿主 console redir。
/// 返回可用 serial：管道优先，失联时降级为 `localhost:5055`。
pub fn ensure_control_channel(adb: &AdbClient, serial: &str) -> Result<String> {
    // 管道健康则直接用，并幂等确保 guest 持久监听 TCP（防后续失联无解）
    if adb
        .shell_blocking(serial, "true", Duration::from_secs(5))
        .is_ok()
    {
        let _ = adb.shell_blocking(
            serial,
            "setprop persist.adb.tcp.port 5555",
            Duration::from_secs(5),
        );
        return Ok(serial.to_string());
    }

    // 管道失联：console 加 redir（已存在时报错可忽略），adb server 连 TCP
    let _ = emu_console_cmd(
        serial,
        &format!("redir add tcp:{ADB_TCP_HOST_PORT}:{ADB_TCP_GUEST_PORT}"),
    );
    let addr = format!("localhost:{ADB_TCP_HOST_PORT}");
    let resp = adb.connect_network(&addr)?;
    if resp.contains("cannot connect") || resp.contains("failed") {
        return Err(Error::LaunchFailed(format!(
            "TCP adb 通道建立失败（管道与 TCP 均不可用）: {resp}"
        )));
    }
    // 通道验证 + 幂等补设持久监听
    adb.shell_blocking(&addr, "setprop persist.adb.tcp.port 5555", Duration::from_secs(5))
        .map_err(|e| Error::LaunchFailed(format!("TCP adb 通道不可用: {e}")))?;
    Ok(addr)
}

/// 为应用创建独立直通显示器（每显示器 = 一个原生宿主窗口，gfxstream 直渲染）。
/// `multidisplay add` 是 upsert 语义——同 id 重复调用即窗口缩放。
pub fn multidisplay_add(serial: &str, id: u32, w: u32, h: u32, dpi: u32) -> Result<()> {
    let resp = emu_console_cmd(serial, &format!("multidisplay add {id} {w} {h} {dpi} 0"))?;
    if resp.contains("KO") {
        return Err(Error::LaunchFailed(format!("multidisplay add 被拒: {}", resp.trim())));
    }
    Ok(())
}

/// 移除直通显示器（对应宿主窗口随之关闭）。
pub fn multidisplay_del(serial: &str, id: u32) -> Result<()> {
    let resp = emu_console_cmd(serial, &format!("multidisplay del {id}"))?;
    if resp.contains("KO") {
        return Err(Error::LaunchFailed(format!("multidisplay del 被拒: {}", resp.trim())));
    }
    Ok(())
}

/// 执行任意 emulator 控制台命令（与 `adb emu <cmd>` 同路径：控制台端口 + auth），
/// 返回控制台原始应答文本。支持的命令族见 emulator-console 文档
/// （camera / virtualscene / sensors / rotate / ...）。
pub fn emu_console_cmd(serial: &str, cmd: &str) -> Result<String> {
    use std::io::{Read, Write};
    use std::net::TcpStream;

    let port: u16 = serial
        .strip_prefix("emulator-")
        .and_then(|n| n.parse().ok())
        .ok_or_else(|| Error::LaunchFailed(format!("无法从 serial 解析控制台端口: {serial}")))?;
    let token_path = home_dir().join(".emulator_console_auth_token");
    let token = std::fs::read_to_string(&token_path)
        .unwrap_or_default()
        .trim()
        .to_string();

    let mut stream = TcpStream::connect(("127.0.0.1", port))?;
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    // 读取控制台问候（读取到静默或超时均可，控制台行为随版本略有差异）
    let mut greeting = [0u8; 1024];
    let _ = stream.read(&mut greeting);
    if !token.is_empty() {
        writeln!(stream, "auth {token}")?;
        let mut resp = [0u8; 256];
        let _ = stream.read(&mut resp);
    }
    writeln!(stream, "{cmd}")?;
    let mut resp = [0u8; 1024];
    let n = stream.read(&mut resp).unwrap_or(0);
    Ok(String::from_utf8_lossy(&resp[..n]).to_string())
}

/// doctor：环境体检结果条目。#[derive(Debug, Clone)]
pub struct DoctorItem {
    pub name: String,
    pub ok: bool,
    pub detail: String,
}

pub fn doctor() -> Vec<DoctorItem> {
    let mut items = Vec::new();

    let hv = std::process::Command::new("sysctl")
        .arg("kern.hv_support")
        .output();
    let hv_ok = hv
        .as_ref()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().ends_with("1"))
        .unwrap_or(false);
    items.push(DoctorItem {
        name: "Hypervisor 硬件虚拟化 (kern.hv_support)".into(),
        ok: hv_ok,
        detail: hv
            .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
            .unwrap_or_else(|e| e.to_string()),
    });

    match Sdk::detect() {
        Ok(sdk) => {
            let avds = sdk.list_avds();
            let arm64 = avds.iter().filter(|a| a.is_arm64()).count();
            items.push(DoctorItem {
                name: "Android SDK".into(),
                ok: true,
                detail: format!(
                    "{}（{} 个 AVD，其中 {} 个 arm64）",
                    sdk.root.display(),
                    avds.len(),
                    arm64
                ),
            });
        }
        Err(_) => items.push(DoctorItem {
            name: "Android SDK".into(),
            ok: false,
            detail: "未找到（ANDROID_HOME / ~/Library/Android/sdk）".into(),
        }),
    }

    let qemu = std::process::Command::new("qemu-system-aarch64")
        .arg("--version")
        .output();
    items.push(DoctorItem {
        name: "QEMU".into(),
        ok: qemu.is_ok(),
        detail: qemu
            .map(|o| {
                String::from_utf8_lossy(&o.stdout)
                    .lines()
                    .next()
                    .unwrap_or("")
                    .to_string()
            })
            .unwrap_or_else(|e| format!("未安装: {e}")),
    });

    items
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_properties_basic() {
        let text = "# comment\nabi.type=arm64-v8a\nimage.sysdir.1=system-images/android-30/default/arm64-v8a/\nbroken line without equals\n";
        let map = parse_properties(text);
        assert_eq!(map.get("abi.type").unwrap(), "arm64-v8a");
        assert!(map.get("image.sysdir.1").unwrap().contains("android-30"));
        assert_eq!(map.len(), 2);
    }

    #[test]
    fn sdk_detect_on_macos_dev_machine() {
        // 本机存在 SDK；CI 或无 SDK 环境跳过
        if PathBuf::from("/opt/homebrew/bin/brew").exists() {
            let sdk = Sdk::detect();
            assert!(sdk.is_ok(), "dev machine should have Android SDK");
        }
    }
}

#[cfg(test)]
mod device_mode_tests {
    use super::*;

    fn write_cfg(dir: &std::path::Path, body: &str) -> AvdInfo {
        std::fs::create_dir_all(dir).unwrap();
        std::fs::write(dir.join("config.ini"), body).unwrap();
        AvdInfo {
            name: "t".into(),
            path: dir.to_path_buf(),
            config: BTreeMap::new(),
        }
    }

    #[test]
    fn apply_mode_rewrites_lcd() {
        let d = std::env::temp_dir().join(format!("mwine_test_mode_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let avd = write_cfg(
            &d,
            "hw.lcd.width = 1080\nhw.lcd.height = 2340\nhw.lcd.density = 420\nhw.device.name = pixel\n",
        );
        apply_device_mode(&avd, DeviceMode::Tablet).unwrap();
        let out = std::fs::read_to_string(d.join("config.ini")).unwrap();
        assert!(out.contains("hw.lcd.width = 1600"));
        assert!(out.contains("hw.lcd.height = 2560"));
        assert!(out.contains("hw.lcd.density = 320"));
        assert!(out.contains("hw.device.name = mwine_tablet"));

        apply_device_mode(&avd, DeviceMode::Phone).unwrap();
        let out = std::fs::read_to_string(d.join("config.ini")).unwrap();
        assert!(out.contains("hw.lcd.width = 1080"));
        assert!(out.contains("hw.lcd.density = 440"));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn apply_mode_appends_missing_keys() {
        let d = std::env::temp_dir().join(format!("mwine_test_mode2_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let avd = write_cfg(&d, "abi.type = arm64-v8a\n");
        apply_device_mode(&avd, DeviceMode::Phone).unwrap();
        let out = std::fs::read_to_string(d.join("config.ini")).unwrap();
        assert!(out.contains("abi.type = arm64-v8a"));
        assert!(out.contains("hw.lcd.width = 1080"));
        assert!(out.contains("hw.device.name = mwine_phone"));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn lcd_profiles_are_sane() {
        let (pw, ph, pd) = DeviceMode::Phone.lcd();
        assert!(ph > pw, "手机应为竖屏");
        let (tw, th, td) = DeviceMode::Tablet.lcd();
        assert!(th > tw, "平板为竖屏大屏");
        // dp 宽度：平板 ≥600dp 触发 tablet UI，手机 <600dp
        assert!((tw as u32 * 160 / td) >= 600);
        assert!((pw as u32 * 160 / pd) < 600);
    }
}
