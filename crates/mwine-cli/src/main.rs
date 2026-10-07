//! mwine CLI：命令行工具集。
//! doctor / vm / devices / install / apps / mirror（虚拟显示器视频流）

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{anyhow, Context, Result};
use clap::{Parser, Subcommand};
use mwine_adb::{AdbClient, InstallProgress};
use mwine_scrcpy::{ControlMessage, NewDisplay, ScrcpyClient, ServerOptions};
use mwine_supervisor::{EmulatorHandle, LaunchOptions, Sdk};

#[derive(Parser)]
#[command(
    name = "mwine",
    version,
    about = "在 macOS 上运行 Android 应用的工具链"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// 环境体检：HVF / SDK / AVD / QEMU / scrcpy-server / adb
    Doctor,
    /// 虚拟机（AVD 实例）管理
    Vm {
        #[command(subcommand)]
        action: VmAction,
    },
    /// 列出 adb 设备
    Devices,
    /// 安装 APK
    Install {
        /// APK 文件路径
        apk: PathBuf,
        /// 目标设备 serial（默认自动选择）
        #[arg(long)]
        serial: Option<String>,
        /// 内部：进度状态文件（安装器进度窗口轮询用，<pct> <label> 单行）
        #[arg(long, hide = true)]
        progress_file: Option<PathBuf>,
    },
    /// 应用管理
    Apps {
        #[command(subcommand)]
        action: AppsAction,
    },
    /// 文件互通：macOS <-> 设备传输（默认设备目录 /sdcard/Download）
    Files {
        #[command(subcommand)]
        action: FilesAction,
    },
    /// 停止 mwine 窗口/桥接进程（安全：只精确匹配 mwine 自己，绝不误伤系统）
    Kill {
        /// 同时停止虚拟机（vm stop）
        #[arg(long)]
        vm: bool,
    },
    /// 后台资源监护：模拟器 CPU/内存异常时提醒/限流，闲置可自动关停
    Monitor {
        /// CPU 提醒阈值（%，模拟器进程瞬时值）
        #[arg(long, default_value = "500")]
        cpu: f64,
        /// 内存提醒阈值（MB）
        #[arg(long, default_value = "8000", alias = "mem_mb")]
        mem_mb: u64,
        /// 限流目标 CPU（%），持续超限时节流模拟器；0=不限流
        #[arg(long, default_value = "0")]
        limit: f64,
        /// 无应用窗口且低负载持续 N 分钟后自动关停虚拟机；0=不启用
        #[arg(long, default_value = "0", alias = "idle_stop")]
        idle_stop: u64,
        /// 安装开机自启（LaunchAgent，当前参数写入），并立即启动一次
        #[arg(long)]
        auto_start: bool,
        /// 卸载开机自启
        #[arg(long)]
        remove_auto_start: bool,
    },
    /// 摄像头注入：把本地图片喂给应用摄像头（扫二维码等场景）
    Camera {
        #[command(subcommand)]
        action: CameraAction,
    },
    /// 通知桥：设备通知 -> macOS 通知中心（companion APK + adb reverse）
    /// 键盘→屏幕映射（游戏按键 DIY）
    Keys {
        #[command(subcommand)]
        action: KeysAction,
    },
    Notify {
        /// 目标设备 serial（默认自动选择）
        #[arg(long)]
        serial: Option<String>,
        /// 仅安装/授权，不进入监听循环
        #[arg(long)]
        setup_only: bool,
    },
    /// .app 壳 / APK 双击安装器（Finder、Dock、Launchpad 集成）
    App {
        #[command(subcommand)]
        action: AppAction,
    },
    /// 在独立 macOS 窗口中运行一个 Android 应用
    Run {
        /// 应用包名
        package: String,
        /// 目标设备 serial（默认自动选择）
        #[arg(long)]
        serial: Option<String>,
        /// 若设备不在线，启动此 AVD（默认自动选择：MWINE_AVD > 上次使用 > 唯一 arm64）
        #[arg(long)]
        avd: Option<String>,
        /// 虚拟显示器尺寸 WxH/DPI。默认 970x2048/430（竖屏手机、360dp 宽、
        /// 视网膜级清晰度；模拟器虚拟显示器上限 1820x2048）。
        /// 横屏应用可用 1820x1024/320。
        #[arg(long, default_value = "970x2048/430")]
        display: String,
        /// 窗口标题（默认用包名）
        #[arg(long)]
        title: Option<String>,
        /// 关闭音频桥
        #[arg(long)]
        no_audio: bool,
        /// 视频帧率上限（默认 30）
        #[arg(long)]
        max_fps: Option<f32>,
        /// 启动后立即进入全屏
        #[arg(long)]
        fullscreen: bool,
    },
    /// 启动虚拟显示器并获取视频流
    Mirror {
        /// 目标设备 serial（默认自动选择）
        #[arg(long)]
        serial: Option<String>,
        /// 若设备不在线，启动此 AVD（结束后自动关闭）
        #[arg(long)]
        avd: Option<String>,
        /// 虚拟显示器尺寸，如 1280x720/160（默认主显示器镜像模式）
        #[arg(long)]
        new_display: Option<String>,
        /// 启用 flex display（可动态调整尺寸，scrcpy 4.0）
        #[arg(long)]
        flex: bool,
        /// 启动会话后在虚拟显示器上启动此应用
        #[arg(long)]
        start_app: Option<String>,
        /// 录制原始 H.264 流到文件
        #[arg(long)]
        record: Option<PathBuf>,
        /// 会话时长（秒），0 表示直到流结束
        #[arg(long, default_value = "10")]
        duration: u64,
        /// 视频码率
        #[arg(long, default_value = "8000000")]
        bitrate: u32,
        /// 启用音频流（统计音频包）
        #[arg(long)]
        audio: bool,
    },
    /// 镜像管理：导入云分发包 / 拉取 Google 官方镜像
    Image {
        #[command(subcommand)]
        action: ImageAction,
    },
}

#[derive(Subcommand)]
enum ImageAction {
    /// 导入云分发包 tar.zst（校验 → 解压 → 稀疏化 → 装进 SDK）
    Import {
        /// 分发包路径（如 ~/Downloads/houttuynia-image-v1.0.1-android13-arm64.tar.zst）
        path: PathBuf,
        /// 整包 sha256（可选；包内 manifest 的逐文件校验始终执行）
        #[arg(long)]
        sha256: Option<String>,
        /// 安装到 system-images 下的目录名（默认 houttuynia）
        #[arg(long, default_value = "houttuynia")]
        name: String,
    },
    /// 列出 Google 官方仓库可拉的 system image（可加过滤词如 android-36）
    OfficialList {
        #[arg(long)]
        filter: Option<String>,
        /// HTTP 代理（如 http://127.0.0.1:7890）
        #[arg(long)]
        proxy: Option<String>,
    },
    /// 拉取并安装 Google 官方镜像（如 android-36 或完整 android-36;android-tv;arm64-v8a）
    OfficialPull {
        target: String,
        /// HTTP 代理（如 http://127.0.0.1:7890）
        #[arg(long)]
        proxy: Option<String>,
    },
}

#[derive(Subcommand)]
enum KeysAction {
    /// 抓当前屏幕截图（键位编辑器背景）
    Shot {
        package: String,
        #[arg(long)]
        serial: Option<String>,
    },
    /// 读取键位映射（JSON）
    Get { package: String },
    /// 保存键位映射（JSON 字符串经 stdin 或文件）
    Set {
        package: String,
        /// JSON 文件路径（缺省读 stdin）
        file: Option<PathBuf>,
    },
    /// 启动注入器（前台运行；监听键盘→注入屏幕触点）
    Watch { package: String },
}

#[derive(Subcommand)]
enum AppAction {
    /// 创建 .app 壳：mwine app create <包名> 或 mwine app create --apk <apk路径>
    Create {
        /// 应用包名（与 --apk 二选一）
        package: Option<String>,
        /// 从本地 APK 创建：解析包名/显示名/图标（aapt2），并安装到在线设备
        #[arg(long)]
        apk: Option<PathBuf>,
        /// 显示名（默认取 APK label 或包名）
        #[arg(long)]
        name: Option<String>,
        /// 输出目录（默认 ~/Applications/Houttuynia）
        #[arg(long)]
        out: Option<PathBuf>,
        /// 把 AVD 名烘焙进启动器（无在线设备时自动启动该 AVD）
        #[arg(long)]
        avd: Option<String>,
        /// --apk 模式下跳过安装到设备
        #[arg(long)]
        no_install: bool,
        /// 只在最后一行输出 .app 路径（供安装器脚本消费）
        #[arg(long)]
        print_path: bool,
    },
    /// 生成 APK 双击安装器 .app（注册 .apk 文件关联：双击即安装并生成应用壳）
    Installer {
        /// 输出目录（默认 ~/Applications/Houttuynia）
        #[arg(long)]
        out: Option<PathBuf>,
    },
}

#[derive(Subcommand)]
enum CameraAction {
    /// 把本地图片注入虚拟摄像头（应用到"扫一扫"看到的就是这张图）
    /// 原理：摄像头切 virtualscene 模式，图片贴到场景内的屏幕上
    Image {
        /// 本地图片路径（QR 码/条码/任意图片）
        path: PathBuf,
        #[arg(long)]
        serial: Option<String>,
        /// 注入前置摄像头（默认后置）
        #[arg(long)]
        front: bool,
    },
    /// 恢复为普通模拟摄像头
    Reset {
        #[arg(long)]
        serial: Option<String>,
        #[arg(long)]
        front: bool,
    },
}

#[derive(Subcommand)]
enum VmAction {
    /// 列出本机 AVD
    List,
    /// headless 启动 AVD 或自建镜像目录并等待引导完成
    Start {
        /// AVD 名（使用 --image 时仅为记录名）
        avd: String,
        /// 自建镜像目录（含 system-qemu.img 等），忽略 AVD
        #[arg(long)]
        image: Option<PathBuf>,
        /// 设备模式：phone 手机（1080x2400@440） / tablet 平板（1600x2560@320）
        #[arg(long)]
        mode: Option<String>,
        /// 加载指定快照启动；缺省时自动探测 AVD 已有快照
        #[arg(long)]
        snapshot: Option<String>,
        /// 冷启动（不加载快照）
        #[arg(long)]
        cold: bool,
        /// guest 内存 MB（qemu 层直传，绕过 2GB 下限；1.5GB 甜点/1.25GB 底线）
        #[arg(long)]
        memory: Option<u32>,
        /// HTTP 代理（如 http://127.0.0.1:7890）
        #[arg(long)]
        proxy: Option<String>,
        /// 用户数据目录（持久化应用数据，升级/重装不丢）
        #[arg(long)]
        data_dir: Option<PathBuf>,
        /// 额外 emulator 参数
        #[arg(last = true)]
        extra: Vec<String>,
    },
    /// 停止设备（存档快照后 adb emu kill）
    Stop { serial: String },
    /// 设备身份重置：随机化 android_id / 广告 ID，应用看到的你就是一台新设备
    ResetId {
        #[arg(long)]
        serial: Option<String>,
        /// 同时清除已登录应用的设备标识缓存（Google 广告 ID 重置）
        #[arg(long)]
        deep: bool,
    },
}

#[derive(Subcommand)]
enum AppsAction {
    /// 列出第三方应用
    List {
        #[arg(long)]
        serial: Option<String>,
    },
    /// 启动应用
    Launch {
        package: String,
        #[arg(long)]
        serial: Option<String>,
    },
    /// 卸载应用（设备侧 pm uninstall）
    Uninstall {
        package: String,
        #[arg(long)]
        serial: Option<String>,
    },
}

#[derive(Subcommand)]
enum FilesAction {
    /// 推送文件/目录到设备（目录递归）
    Push {
        /// 本地文件或目录（可多个）
        #[arg(required = true)]
        local: Vec<PathBuf>,
        /// 设备目标目录
        #[arg(long, default_value = "/sdcard/Download")]
        to: String,
        #[arg(long)]
        serial: Option<String>,
    },
    /// 拉取设备文件到本机
    Pull {
        /// 设备文件路径
        remote: String,
        /// 本地目标目录
        #[arg(long)]
        to: Option<PathBuf>,
        #[arg(long)]
        serial: Option<String>,
    },
    /// 列出设备目录内容
    Ls {
        /// 设备目录
        #[arg(default_value = "/sdcard/Download")]
        dir: String,
        #[arg(long)]
        serial: Option<String>,
    },
}

/// bundle 内嵌配置（app.conf: key=value）。
struct BundledConfig {
    package: String,
    title: Option<String>,
    avd: Option<String>,
    display: String,
    fps: Option<f32>,
    fullscreen: bool,
}

/// 检测当前可执行文件是否位于 <X>.app/Contents/MacOS/ 内，是则读取配置。
fn bundled_app_config() -> Option<BundledConfig> {
    let exe = std::env::current_exe().ok()?;
    let macos = exe.parent()?;
    if macos.file_name()?.to_str()? != "MacOS" {
        return None;
    }
    let contents = macos.parent()?;
    let app_dir = contents.parent()?;
    if !app_dir
        .file_name()?
        .to_str()?
        .ends_with(".app") {
        return None;
    }
    let conf = contents.join("Resources/app.conf");
    let text = std::fs::read_to_string(conf).ok()?;
    let mut package = None;
    let mut title = None;
    let mut avd = None;
    let mut display = "970x2048/430".to_string();
    let mut fps: Option<f32> = None;
    let mut fullscreen = false;
    for line in text.lines() {
        if let Some((k, v)) = line.split_once('=') {
            match k.trim() {
                "package" => package = Some(v.trim().to_string()),
                "title" => title = Some(v.trim().to_string()),
                "avd" => avd = Some(v.trim().to_string()).filter(|s| !s.is_empty()),
                "display" => display = v.trim().to_string(),
                "fps" => fps = v.trim().parse().ok(),
                "fullscreen" => fullscreen = v.trim() == "1",
                _ => {}
            }
        }
    }
    Some(BundledConfig {
        package: package?,
        title,
        avd,
        display,
        fps,
        fullscreen,
    })
}

/// bundle 模式入口：日志落盘 + 启动通知 + 直接运行应用窗口。
fn bundled_main(cfg: BundledConfig) -> Result<()> {
    // 日志：~/Library/Logs/mwine-apps/<app名>.log（stdout/stderr 重定向）
    #[cfg(target_os = "macos")]
    {
        let exe = std::env::current_exe().unwrap_or_default();
        let name = exe
            .file_stem()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_else(|| "app".into());
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_default();
        let dir = home.join("Library/Logs/mwine-apps");
        std::fs::create_dir_all(&dir).ok();
        if let Ok(f) = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(dir.join(format!("{name}.log")))
        {
            use std::os::unix::io::AsRawFd;
            let fd = f.as_raw_fd();
            unsafe {
                libc::dup2(fd, 1);
                libc::dup2(fd, 2);
            }
        }
        let _ = std::process::Command::new("osascript")
            .arg("-e")
            .arg(format!(
                "display notification \"正在启动 {}…\" with title \"mwine\"",
                cfg.title.as_deref().unwrap_or(&name)
            ))
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    }
    println!("[{}] bundle 启动: {}", chrono_like_now(), cfg.package);
    cmd_run(
        cfg.package, None, cfg.avd, cfg.display, cfg.title, false, cfg.fps, cfg.fullscreen,
    )
}

fn chrono_like_now() -> String {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| format!("[{}s]", d.as_secs()))
        .unwrap_or_default()
}

fn adb() -> AdbClient {
    AdbClient::new()
}

/// 安全停止：只杀两类进程——可执行文件位于本仓库的 mwine，或位于
/// ~/Applications/Houttuynia/ 内嵌的 mwine。绝不按模糊模式匹配（曾因
/// pkill -f ".app/Contents/MacOS" 误杀系统全部 GUI 应用）。
fn cmd_kill(vm: bool) -> Result<()> {
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_default();
    let wine_apps = home.join("Applications/Mwine");
    let me = std::env::current_exe().unwrap_or_default();
    let repo_root = me
        .ancestors()
        .find(|p| p.join("Cargo.toml").is_file() && p.join("crates").is_dir())
        .map(|p| p.to_path_buf());

    let my_pid = std::process::id();
    let mut killed = 0usize;
    // 两个专属路径模式；命中后仍用 ps comm= 校验完整可执行路径
    for pat in ["target/debug/mwine", "Applications/Mwine/"] {
        let Ok(out) = std::process::Command::new("pgrep")
            .args(["-f", pat])
            .output()
        else {
            continue;
        };
        for pid in String::from_utf8_lossy(&out.stdout)
            .split_whitespace()
            .filter_map(|p| p.parse::<u32>().ok())
            .filter(|p| *p != my_pid)
        {
            let Ok(comm) = std::process::Command::new("ps")
                .args(["-p", &pid.to_string(), "-o", "comm="])
                .output()
            else {
                continue;
            };
            let exe = PathBuf::from(String::from_utf8_lossy(&comm.stdout).trim());
            let is_mwine = exe.starts_with(&wine_apps)
                || repo_root.as_ref().map(|r| exe.starts_with(r)).unwrap_or(false);
            if !is_mwine {
                continue;
            }
            unsafe { libc::kill(pid as i32, libc::SIGTERM) };
            println!("  已停止 {pid}（{}）", exe.display());
            killed += 1;
        }
    }
    println!("✓ 已停止 {killed} 个 mwine 进程");
    if vm {
        let client = adb();
        if let Ok(devs) = client.devices() {
            for d in devs.iter().filter(|d| d.serial.starts_with("emulator-")) {
                let _ = mwine_supervisor::emu_console_kill(&d.serial);
                println!("✓ 已关停虚拟机 {}", d.serial);
            }
        }
    }
    Ok(())
}


fn mwine_config_dir() -> PathBuf {
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_default();
    home.join(".config/mwine")
}

fn record_last_avd(name: &str) {
    let dir = mwine_config_dir();
    std::fs::create_dir_all(&dir).ok();
    std::fs::write(dir.join("last-avd"), name).ok();
}

/// 默认 AVD：环境变量 MWINE_AVD > 上次使用过的 > 唯一的 arm64 AVD > 唯一 AVD。
fn pick_default_avd() -> Option<String> {
    if let Ok(v) = std::env::var("MWINE_AVD") {
        if !v.trim().is_empty() {
            return Some(v);
        }
    }
    let sdk = Sdk::detect().ok()?;
    let avds = sdk.list_avds();
    if let Ok(last) = std::fs::read_to_string(mwine_config_dir().join("last-avd")) {
        let last = last.trim();
        if avds.iter().any(|a| a.name == last) {
            return Some(last.to_string());
        }
    }
    let arm64: Vec<_> = avds.iter().filter(|a| a.is_arm64()).collect();
    if arm64.len() == 1 {
        return Some(arm64[0].name.clone());
    }
    if avds.len() == 1 {
        return Some(avds[0].name.clone());
    }
    None
}

fn resolve_serial(explicit: Option<&str>) -> Result<String> {
    if let Some(s) = explicit {
        return Ok(s.to_string());
    }
    let client = adb();
    let dev = client
        .find_online_device()
        .context("没有在线的 adb 设备；用 `mwine vm start <avd>` 启动一个")?;
    Ok(dev.serial)
}

fn print_doctor() -> Result<()> {
    println!("mwine doctor");
    for item in mwine_supervisor::doctor() {
        println!(
            "  [{}] {}\n        {}",
            if item.ok { "✓" } else { "✗" },
            item.name,
            item.detail
        );
    }
    let jar = mwine_scrcpy::find_server_jar();
    println!(
        "  [{}] scrcpy-server\n        {}",
        if jar.is_some() { "✓" } else { "✗" },
        jar.map(|p| p.display().to_string())
            .unwrap_or_else(|| "未找到（brew install scrcpy）".into())
    );
    let companion = find_companion_apk();
    println!(
        "  [{}] 通知桥 companion APK\n        {}",
        if companion.is_some() { "✓" } else { "✗" },
        companion
            .map(|p| p.display().to_string())
            .unwrap_or_else(|| "未找到（scripts/build_companion.sh）".into())
    );
    let aapt2 = find_aapt2();
    println!(
        "  [{}] aapt2（.app 图标提取）\n        {}",
        if aapt2.is_some() { "✓" } else { "✗" },
        aapt2
            .map(|p| p.display().to_string())
            .unwrap_or_else(|| "未找到（SDK build-tools）".into())
    );
    let client = adb();
    match client.devices() {
        Ok(devs) if !devs.is_empty() => {
            println!("  [✓] adb 设备");
            for d in devs {
                println!("        {} ({})", d.serial, d.state);
            }
        }
        Ok(_) => println!("  [-] adb 设备\n        无在线设备"),
        Err(_) => println!("  [✗] adb server 未运行"),
    }
    Ok(())
}

fn cmd_vm_list() -> Result<()> {
    let sdk = Sdk::detect().context("Android SDK 未找到")?;
    let running: Vec<String> = adb()
        .devices()
        .unwrap_or_default()
        .into_iter()
        .filter(|d| d.state == "device")
        .map(|d| d.serial)
        .collect();
    println!("{:<24} {:<10} 状态", "AVD", "ABI");
    for avd in sdk.list_avds() {
        let avd_name_underscore = avd.name.replace('.', "_");
        let state = if running
            .iter()
            .any(|s| s.contains(avd_name_underscore.as_str()))
        {
            "运行中*"
        } else {
            ""
        };
        println!(
            "{:<24} {:<10} {}",
            avd.name,
            avd.abi().unwrap_or("?"),
            state
        );
    }
    Ok(())
}

fn cmd_vm_start(
    avd: String,
    image: Option<PathBuf>,
    mode: Option<String>,
    snapshot: Option<String>,
    cold: bool,
    memory: Option<u32>,
    proxy: Option<String>,
    data_dir: Option<PathBuf>,
    extra: Vec<String>,
) -> Result<()> {
    let device_mode = match mode.as_deref() {
        None => None,
        Some("phone") => Some(mwine_supervisor::DeviceMode::Phone),
        Some("tablet") => Some(mwine_supervisor::DeviceMode::Tablet),
        Some(other) => {
            return Err(anyhow!("未知设备模式 {other:?}；支持 phone / tablet"));
        }
    };
    let sdk = Sdk::detect().context("Android SDK 未找到")?;
    let client = adb();
    client.ensure_server(Some(&sdk.adb_bin()))?;
    if image.is_some() {
        println!("启动自建镜像 {avd}（headless，{}）...", image.as_ref().unwrap().display());
    } else {
        println!("启动 {avd}（headless）...");
    }
    let opts = LaunchOptions {
        extra_args: extra,
        image_dir: image,
        snapshot,
        no_snapshot: cold,
        memory_mb: memory,
        device_mode,
        proxy,
        data_dir,
        ..Default::default()
    };
    let handle = EmulatorHandle::launch_headless(&sdk, &avd, &opts, &client).context("启动失败")?;
    println!("serial: {}，等待引导完成...", handle.serial);
    handle
        .wait_boot_completed(&client, Duration::from_secs(300))
        .context("引导超时")?;
    // 快照加载结果核对：emulator 把加载失败原因写进 ~/.mwine/logs/emu-<avd>.log
    // （配置不匹配时静默退回冷启动，不查日志用户无从得知）。
    // 注意只核对 mwine_default：emulator 对任何 AVD 都会自动尝试 default_boot，
    // 首次启动的失败属于正常现象，不能误报。
    if !cold {
        let log = std::env::var_os("HOME")
            .map(|h| {
                let mut p = std::path::PathBuf::from(h);
                p.push(".mwine/logs");
                p.push(format!("emu-{avd}.log"));
                p
            })
            .unwrap_or_default();
        let since = std::fs::metadata(&log)
            .and_then(|m| m.modified())
            .map(|t| {
                t.duration_since(std::time::UNIX_EPOCH)
                    .map(|d| d.as_secs() as i64)
                    .unwrap_or(0)
            })
            .unwrap_or(0);
        let launched_at = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0);
        let fresh_window = (launched_at - since) < 120;
        if fresh_window {
            if let Ok(body) = std::fs::read_to_string(&log) {
                if body.contains("Failed to load snapshot 'mwine_default'") {
                    println!("⚠ 快照 mwine_default 加载失败（配置与存档时不一致），本次为冷启动");
                    println!("  恢复方法：mwine vm stop（会以当前配置重存 mwine_default）后再次启动");
                } else if body.contains("Successfully loaded snapshot 'mwine_default'") {
                    println!("✓ 快照恢复启动");
                }
            }
        }
    }
    // 自建镜像的 qemu 管道 adb 可能失联（TP1A 世代已知问题），
    // 就绪后立即确保可用通道：优先管道，必要时挂 TCP 兜底（console redir）
    let (control_serial, chan_note) =
        match mwine_supervisor::ensure_control_channel(&client, &handle.serial) {
            Ok(s) if s == handle.serial => (
                handle.serial.clone(),
                format!("✓ {avd} 已就绪（{}）", handle.serial),
            ),
            Ok(s) => (
                s.clone(),
                format!("✓ {avd} 已就绪（adb 通道：{s}，管道失联已自动切换 TCP）"),
            ),
            Err(e) => (
                handle.serial.clone(),
                format!("✓ {avd} 已就绪（{}）⚠ 备用通道建立失败：{e}", handle.serial),
            ),
        };
    println!("{chan_note}");
    // 设备模式：运行时 wm size/density（resizable 镜像的显示权威源）
    if let Some(mode) = device_mode {
        match mwine_supervisor::apply_device_mode_runtime(&client, &control_serial, mode) {
            Ok(()) => {
                let (w, h, dpi) = mode.lcd();
                println!("✓ 设备模式：{}（{w}x{h}@{dpi}）", mode.device_name());
            }
            Err(e) => println!("⚠ 设备模式设置失败：{e}"),
        }
    }
    record_last_avd(&avd);
    // detach：父进程退出不影响 emulator（emulator 自身为独立进程组）
    std::mem::forget(handle);
    Ok(())
}

fn cmd_vm_stop(serial: String) -> Result<()> {
    // emu kill 不会自动回存 -snapshot 指定的档：关停前显式存档到 mwine_default，
    // 下次 vm start 即可 5s 内恢复到当前状态
    match mwine_supervisor::save_snapshot_blocking(&serial, "mwine_default", Duration::from_secs(20)) {
        Ok(()) => println!("✓ 状态已存档（mwine_default）"),
        Err(e) => println!("⚠ 存档失败（{e}），直接关停"),
    }
    mwine_supervisor::emu_console_kill(&serial).context("emu kill 失败")?;
    println!("✓ 已向 {serial} 发送关停指令");
    Ok(())
}

/// 设备身份重置：应用层可见的标识全部随机化。
/// android_id 是绝大多数应用/SDK 的设备指纹主键，settings 即时生效无需重启。
fn cmd_vm_reset_id(serial: Option<String>, deep: bool) -> Result<()> {
    let client = adb();
    client.ensure_server(None).ok();
    let serial = resolve_serial(serial.as_deref())?;

    let old_android_id = client
        .shell_blocking(&serial, "settings get secure android_id", Duration::from_secs(5))
        .map(|o| o.stdout_string().trim().to_string())
        .unwrap_or_default();
    println!("当前 android_id: {old_android_id}");

    // 16 位十六进制新 ID（标准格式）
    let t = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let seed = t.as_nanos() as u64 ^ (std::process::id() as u64) << 32;
    let mut x = seed | 1;
    let mut hex = String::new();
    for _ in 0..4 {
        // xorshift64
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        hex.push_str(&format!("{:016x}", x));
    }
    let new_android_id: String = hex.chars().take(16).collect();
    // UUID v4 形态的广告 ID
    let h: Vec<char> = hex.chars().collect();
    let new_advertising_id = format!(
        "{}-{}-{}-{}-{}",
        h[0..8].iter().collect::<String>(),
        h[8..12].iter().collect::<String>(),
        h[12..16].iter().collect::<String>(),
        h[16..20].iter().collect::<String>(),
        h[20..32].iter().collect::<String>()
    );

    let mut changed = 0;
    for (key, val) in [
        ("android_id", new_android_id.as_str()),
        // 广告 ID：UAID 是绝大多数广告 SDK 的主标识
        ("advertising_id", new_advertising_id.as_str()),
    ] {
        let cmd = format!("settings put secure {key} {val}");
        if client
            .shell_blocking(&serial, &cmd, Duration::from_secs(5))
            .is_ok()
        {
            changed += 1;
        } else {
            println!("⚠ 写入 {key} 失败");
        }
    }

    if deep {
        // 深度清理：应用缓存里的设备指纹（需要应用未运行；清的是系统广告ID缓存与剪贴板）
        for cmd in [
            "settings put secure limit_ad_tracking 1",
            "pm trim-caches 999999999999",
        ] {
            let _ = client.shell_blocking(&serial, cmd, Duration::from_secs(10));
        }
        println!("✓ 深度清理：广告跟踪已限制 + 应用缓存已裁剪");
    }

    println!("✓ 新 android_id: {new_android_id}（{changed}/2 项已写入，即时生效）");
    if old_android_id == new_android_id {
        println!("⚠ 新旧 ID 相同（极小概率），可重跑一次");
    }
    Ok(())
}

fn cmd_devices() -> Result<()> {
    for d in adb().devices().context("adb server 未运行")? {
        println!("{}  {}", d.serial, d.state);
    }
    Ok(())
}

fn cmd_image_import(path: PathBuf, sha256: Option<String>, name: String) -> Result<()> {
    let sdk = Sdk::detect().context("未找到 Android SDK")?;
    let dest = {
            static NEVER: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
            mwine_supervisor::import_image(&path, sha256.as_deref(), &name, &sdk, &|l: &str| println!("{l}"), &NEVER,
                &|phase, done, total| {
                    if total > 0 { println!("  [{phase}] {} / {} MiB", done >> 20, total >> 20); }
                })
        }
        .context("镜像导入失败")?;
    print_next_step(&sdk, &dest);
    Ok(())
}

fn cmd_image_official_list(filter: Option<String>, proxy: Option<String>) -> Result<()> {
    let list = mwine_supervisor::official_list(filter.as_deref(), proxy.as_deref())
        .context("拉取官方仓库清单失败（dl.google.com 不可达时请配置 https_proxy）")?;
    if list.is_empty() {
        println!("没有匹配的官方镜像");
        return Ok(());
    }
    for img in list.iter().take(40) {
        println!("{:<52} {:>7} MiB", img.path, img.size / 1048576);
    }
    if list.len() > 40 {
        println!("… 共 {} 项（用 --filter 过滤）", list.len());
    }
    Ok(())
}

fn cmd_image_official_pull(target: String, proxy: Option<String>) -> Result<()> {
    let sdk = Sdk::detect().context("未找到 Android SDK")?;
    let dest = {
            static NEVER: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
            mwine_supervisor::official_pull(&target, &sdk, &|l: &str| println!("{l}"), proxy.as_deref(), &NEVER,
                &|phase, done, total| {
                    if total > 0 { println!("  [{phase}] {} / {} MiB", done >> 20, total >> 20); }
                })
        }
        .context("官方镜像拉取失败")?;
    print_next_step(&sdk, &dest);
    Ok(())
}

fn print_next_step(sdk: &Sdk, dest: &Path) {
    println!();
    println!("下一步：把 AVD 指向新镜像（image.sysdir.1 = …）或");
    let rel = dest
        .strip_prefix(sdk.root.clone())
        .unwrap_or(dest);
    let _ = rel;
    println!("      mwine vm start <avd> --image {}", dest.display());
}

fn stdout_is_tty() -> bool {
    #[cfg(target_os = "macos")]
    {
        unsafe { libc::isatty(1) == 1 }
    }
    #[cfg(not(target_os = "macos"))]
    {
        false
    }
}

/// 安装进度条：TTY 下单行 \r 刷新（传输 % + 安装阶段转圈计时）；
/// 非 TTY（日志/管道）按 25% 阶梯打印纯文本行。
struct InstallBar {
    tty: bool,
    last_pct_step: u32,
    install_stop: Option<Arc<std::sync::atomic::AtomicBool>>,
}

impl InstallBar {
    fn new() -> Self {
        Self {
            tty: stdout_is_tty(),
            last_pct_step: 0,
            install_stop: None,
        }
    }

    fn update(&mut self, p: InstallProgress) {
        use std::io::Write as _;
        use std::sync::atomic::{AtomicBool, Ordering};
        match p {
            InstallProgress::Push { sent, total } => {
                if total == 0 {
                    return;
                }
                let pct = ((sent as f64 / total as f64) * 100.0) as u32;
                if self.tty {
                    eprint!(
                        "\r  传输 APK {:>3}%（{:.1}/{:.1} MB）",
                        pct,
                        sent as f64 / 1e6,
                        total as f64 / 1e6
                    );
                    let _ = std::io::stderr().flush();
                } else {
                    let step = pct / 25;
                    if step > self.last_pct_step {
                        self.last_pct_step = step;
                        println!("  传输 APK {}%", step * 25);
                    }
                }
            }
            InstallProgress::Installing => {
                if self.tty {
                    eprint!("\r\x1b[K  传输完成，设备安装中（dexopt 视包大小 10-60s）…");
                    let _ = std::io::stderr().flush();
                    let stop = Arc::new(AtomicBool::new(false));
                    let stop2 = stop.clone();
                    std::thread::spawn(move || {
                        let start = std::time::Instant::now();
                        let frames = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];
                        let mut i = 0usize;
                        while !stop2.load(Ordering::Relaxed) {
                            eprint!(
                                "\r  设备安装中… {} {:>2}s",
                                frames[i % frames.len()],
                                start.elapsed().as_secs()
                            );
                            let _ = std::io::stderr().flush();
                            i += 1;
                            std::thread::sleep(Duration::from_millis(200));
                        }
                    });
                    self.install_stop = Some(stop);
                } else {
                    println!("  传输完成，设备安装中…");
                }
            }
        }
    }

    fn finish(&mut self) {
        use std::io::Write as _;
        use std::sync::atomic::Ordering;
        if let Some(stop) = self.install_stop.take() {
            stop.store(true, Ordering::Relaxed);
        }
        if self.tty {
            eprint!("\r\x1b[K");
            let _ = std::io::stderr().flush();
        }
    }
}

/// 安装进度状态文件写入（<pct> <label> 单行，供 JXA 进度窗口轮询）；
/// 节流：百分比变化或每 400ms 最多写一次。
struct ProgressFileWriter {
    path: Option<PathBuf>,
    name: String,
    last_pct: u32,
    last_write: Instant,
}

impl ProgressFileWriter {
    fn new(path: Option<PathBuf>, name: String) -> Self {
        Self {
            path,
            name,
            last_pct: 0,
            last_write: Instant::now(),
        }
    }

    fn update(&mut self, p: InstallProgress) {
        let Some(path) = &self.path else { return };
        let (pct, label) = match p {
            InstallProgress::Push { sent, total } => {
                if total == 0 {
                    return;
                }
                (
                    ((sent as f64 / total as f64) * 100.0) as u32,
                    format!("已传送 {:.1}/{:.1} MB", sent as f64 / 1e6, total as f64 / 1e6),
                )
            }
            InstallProgress::Installing => {
                // S2 = 设备侧安装（dexopt），窗口显示已进行秒数
                self.last_pct = 100;
                let _ = std::fs::write(path, "S2 IND\n");
                return;
            }
        };
        let changed = pct != self.last_pct;
        if !changed && self.last_write.elapsed() < Duration::from_millis(400) {
            return;
        }
        self.last_pct = pct;
        self.last_write = Instant::now();
        let _ = std::fs::write(path, format!("{pct} {label}\n"));
    }
}

/// 多设备在线时的安装目标解析：
/// 1) 显式 --serial；2) 守护进程（管理器）拉起的 VM（/api/status.serial 且在线）；
/// 3) 唯一在线设备；4) 多台且都不是管理器的 → 报错并列出（宁可拒绝也不装错机器）。
/// Ok(None) = 无在线设备（调用方走自动启动默认 AVD 的分支）。
fn resolve_install_serial(
    client: &AdbClient,
    explicit: Option<String>,
) -> Result<Option<String>> {
    if let Some(s) = explicit {
        return Ok(Some(s));
    }
    let online: Vec<String> = client
        .devices()
        .unwrap_or_default()
        .into_iter()
        .filter(|d| d.state == "device" && d.serial.starts_with("emulator-"))
        .map(|d| d.serial)
        .collect();
    if online.is_empty() {
        return Ok(None);
    }
    // 守护进程的 VM 优先（双击安装进的是"管理器的虚拟机"，不是碰巧排第一台的）
    if let Ok(body) = ureq::AgentBuilder::new()
        .timeout_connect(std::time::Duration::from_secs(2))
        .build()
        .get("http://127.0.0.1:7788/api/status")
        .timeout(std::time::Duration::from_secs(3))
        .call()
    {
        if let Ok(s) = body.into_string() {
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(&s) {
                if let Some(serial) = v["vm"]["serial"].as_str() {
                    if online.iter().any(|s| s == serial) {
                        println!("（多设备环境，目标：管理器虚拟机 {serial}）");
                        return Ok(Some(serial.to_string()));
                    }
                }
            }
        }
    }
    if online.len() == 1 {
        return Ok(Some(online[0].clone()));
    }
    anyhow::bail!(
        "检测到多台设备在线：{}。请用 --serial 指定目标（或关掉其他模拟器）",
        online.join(", ")
    )
}

fn cmd_install(apk: PathBuf, serial: Option<String>, progress_file: Option<PathBuf>) -> Result<()> {
    let client = adb();
    client.ensure_server(None).ok();
    // 无在线设备：自动启动默认 AVD（与 run/mirror 同规则），进度窗口显示 VM 阶段
    let serial = match resolve_install_serial(&client, serial)? {
        Some(s) => s,
        None => {
                let avd = pick_default_avd().ok_or_else(|| {
                    anyhow!("无在线设备且无法确定默认 AVD；先运行 `mwine vm start <avd>` 或设置 MWINE_AVD")
                })?;
                let vm_line = |text: &str| {
                    if let Some(pf) = &progress_file {
                        let _ = std::fs::write(pf, format!("VM IND {text}\n"));
                    }
                };
                vm_line("正在启动虚拟机…");
                println!("(无在线设备，启动 {avd} ...)");
                let sdk = Sdk::detect().context("Android SDK 未找到")?;
                let handle = EmulatorHandle::launch_headless(
                    &sdk,
                    &avd,
                    &LaunchOptions::default(),
                    &client,
                )?;
                println!("serial: {}，等待引导 ...", handle.serial);
                vm_line("正在启动虚拟机…（冷启动最长约 2 分钟）");
                handle
                    .wait_boot_completed(&client, Duration::from_secs(300))
                    .context("虚拟机引导超时")?;
                record_last_avd(&avd);
                vm_line("虚拟机已就绪");
                handle.serial.clone()
        }
    };
    let apk = apk.canonicalize().context("APK 路径无效")?;
    println!("安装 {} 到 {serial} ...", apk.display());
    let client = adb();
    let name = apk
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| "APK".into());
    let mut bar = InstallBar::new();
    let mut pw = ProgressFileWriter::new(progress_file, name);
    let result =
        client.install_apk_with_progress(&serial, &apk, Duration::from_secs(600), |p| {
            bar.update(p);
            pw.update(p);
        });
    bar.finish();
    let out = result?;
    println!("✓ {out}");
    // 授予运行时权限（扫码/存储等）
    let pkg = out_after_package(&apk).unwrap_or_default();
    if !pkg.is_empty() {
        match client.grant_runtime_permissions(&serial, &pkg) {
            Ok(n) if n > 0 => println!("✓ 已授予 {n} 项运行时权限"),
            _ => {}
        }
    }
    Ok(())
}

/// 从 APK 提取包名（aapt2 badging；失败返回 None，权限授予跳过）。
fn out_after_package(apk: &Path) -> Option<String> {
    let aapt2 = find_aapt2()?;
    let text = run_tool(
        std::process::Command::new(&aapt2)
            .args(["dump", "badging"])
            .arg(apk),
    )
    .ok()?;
    parse_badging(&text).package.into()
}

fn cmd_apps_list(serial: Option<String>) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    for p in adb().list_packages(&serial)? {
        println!("{p}");
    }
    Ok(())
}

fn cmd_apps_uninstall(package: String, serial: Option<String>) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    adb().shell_blocking(
        &serial,
        &format!("pm uninstall -k {package}"),
        Duration::from_secs(30),
    )?;
    println!("✓ 已卸载 {package}");
    Ok(())
}

fn cmd_apps_launch(package: String, serial: Option<String>) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    adb().launch_package(&serial, &package)?;
    println!("✓ 已启动 {package}");
    Ok(())
}

// ---------------- 文件互通 ----------------

fn cmd_files_push(local: Vec<PathBuf>, to: String, serial: Option<String>) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    let client = adb();
    for path in &local {
        let path = path.canonicalize().context(format!("路径无效：{}", path.display()))?;
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default();
        println!("推送 {} -> {serial}:{to}/ ...", path.display());
        match client.push_path(&serial, &path, &to) {
            Ok(pushed) => {
                for remote in pushed {
                    println!("  ✓ {remote}");
                }
            }
            Err(e) => {
                // 本镜像已知 bug：MediaProvider 启动期 codec 探测 ANR → FUSE 存储未挂载，
                // /sdcard 不可写。降级：root 直写 MediaProvider 真实根（/data/media/0/...）
                // + MediaStore 注册 —— 现代 app（SAF/MediaStore，如微信/TG 选文件）照常可见。
                println!("  ⚠ 直接推送失败（{e}），尝试 root 直写媒体库 ...");
                push_via_media_root(&client, &serial, &path, &to, &name)?;
            }
        }
        println!("✓ {name} 完成");
    }
    Ok(())
}

/// /sdcard 不可用时的降级通道：/data/local/tmp 中转 → root 复制到
/// /data/media/0/<目标> → chown media_rw → MediaStore 插行（content insert）。
fn push_via_media_root(
    client: &mwine_adb::AdbClient,
    serial: &str,
    local: &Path,
    to: &str,
    name: &str,
) -> Result<()> {
    let remote_tmp = format!("/data/local/tmp/mwine_{name}");
    client.push_path(serial, local, "/data/local/tmp")?;
    // 需要 root：确保 adbd 以 root 运行（userdebug 镜像；重启 adbd 会短暂掉线，
    // 重连后的首个 transport 可能仍 offline，脚本执行需带重试）
    let _ = client.root(serial);
    let _ = client.wait_for_device(serial, Duration::from_secs(30));
    std::thread::sleep(Duration::from_millis(1500));
    // to 形如 /sdcard/Download → 真实根映射为 /data/media/0/Download
    let sub = to
        .trim_start_matches("/sdcard")
        .trim_start_matches("/storage/emulated/0")
        .trim_matches('/')
        .to_string();
    let real_dir = format!("/data/media/0/{sub}");
    let real_path = format!("{real_dir}/{name}");
    let esc = |s: &str| s.replace(' ', "\\ ");
    let script = format!(
        "mkdir -p {d} && cp {t} {r} && chown media_rw:media_rw {r} && chmod 660 {r} && echo MWINE_OK",
        d = esc(&real_dir),
        t = esc(&remote_tmp),
        r = esc(&real_path),
    );
    let mut out = None;
    let mut last_err = String::new();
    for attempt in 0..3 {
        match client.shell_blocking(serial, &script, Duration::from_secs(30)) {
            Ok(o) => {
                out = Some(o);
                break;
            }
            Err(e) => {
                last_err = e.to_string();
                std::thread::sleep(Duration::from_millis(2000 * (attempt + 1)));
            }
        }
    }
    let out = out.context(format!("root 写入失败（{last_err}）"))?;
    if !out.stdout_string().contains("MWINE_OK") {
        anyhow::bail!(
            "写入 {} 失败：{}",
            real_path,
            out.stdout_string().trim()
        );
    }
    // MediaStore 注册（应用立即可见）
    let mime = match local.extension().and_then(|e| e.to_str()) {
        Some("png") => "image/png",
        Some("jpg") | Some("jpeg") => "image/jpeg",
        Some("gif") => "image/gif",
        Some("mp4") => "video/mp4",
        Some("mp3") => "audio/mpeg",
        Some("txt") => "text/plain",
        Some("pdf") => "application/pdf",
        Some("apk") => "application/vnd.android.package-archive",
        _ => "application/octet-stream",
    };
    let visible_path = format!("/storage/emulated/0/{sub}/{name}");
    let insert = format!(
        "content insert --uri content://media/external/file --bind _data:s:{p} --bind mime_type:s:{m}",
        p = esc(&visible_path),
        m = mime,
    );
    match client.shell_blocking(serial, &insert, Duration::from_secs(15)) {
        Ok(o) if !o.stdout_string().contains("Error") => {
            println!("  ✓ 已注册媒体库（{visible_path}）");
        }
        _ => println!("  ⚠ 媒体库注册未生效（文件已在 {real_path}，稍后系统扫描会收录）"),
    }
    println!("  ✓ {real_path}");
    Ok(())
}

fn cmd_files_pull(remote: String, to: Option<PathBuf>, serial: Option<String>) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    let name = remote
        .rsplit('/')
        .next()
        .filter(|s| !s.is_empty())
        .unwrap_or("file")
        .to_string();
    let out_dir = to.unwrap_or_else(|| {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_default();
        home.join("Downloads")
    });
    std::fs::create_dir_all(&out_dir)?;
    let dest = out_dir.join(&name);
    println!("拉取 {serial}:{remote} -> {} ...", dest.display());
    let data = adb().pull_file(&serial, &remote)?;
    std::fs::write(&dest, &data)?;
    println!("✓ {}（{} 字节）", dest.display(), data.len());
    Ok(())
}

fn cmd_files_ls(dir: String, serial: Option<String>) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    let out = adb().shell_blocking(
        &serial,
        &format!("ls -lh '{}'", dir.replace('\'', "'\\''")),
        Duration::from_secs(15),
    )?;
    if !out.ok() {
        return Err(anyhow!(
            "ls 失败：{}{}",
            out.stdout_string(),
            String::from_utf8_lossy(&out.stderr)
        ));
    }
    println!("{dir}:");
    print!("{}", out.stdout_string());
    Ok(())
}

// ---------------- 摄像头注入 ----------------

/// emulator 控制台简封：打印应答，KO 视为错误
fn emu_cmd(serial: &str, cmd: &str) -> Result<String> {
    let resp = mwine_supervisor::emu_console_cmd(serial, cmd)
        .with_context(|| format!("控制台命令失败: {cmd}"))?;
    let text = resp.trim().to_string();
    if text.contains("KO") {
        return Err(anyhow!("模拟器拒绝 {cmd}: {text}"));
    }
    Ok(text)
}

fn cmd_camera_image(path: PathBuf, serial: Option<String>, front: bool) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    let path = path.canonicalize().context("图片路径无效")?;
    let surface = if front { "table" } else { "wall" };
    println!("注入图片到虚拟场景{surface}面 {} ...", path.display());
    // emulator 控制台：virtualscene-image <wall|table> <path>
    // （emulated 摄像头默认即虚拟房间场景，无需切模式）
    emu_cmd(&serial, &format!("virtualscene-image {surface} {}", path.display()))?;
    println!("✓ 完成。在应用里打开『扫一扫』，摄像头看到的就是这张图");
    println!("  （恢复普通模拟摄像头：mwine camera reset）");
    Ok(())
}

fn cmd_camera_reset(serial: Option<String>, front: bool) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    let surface = if front { "table" } else { "wall" };
    // 空 path 恢复场景默认图
    emu_cmd(&serial, &format!("virtualscene-image {surface}"))?;
    println!("✓ 摄像头已恢复默认虚拟场景");
    Ok(())
}

fn parse_new_display(s: &str) -> Result<NewDisplay> {
    // 支持 "WxH/DPI" / "WxH" / "/DPI" / ""
    let (size_part, dpi_part) = match s.split_once('/') {
        Some((a, b)) => (a, Some(b)),
        None => (s, None),
    };
    let (w, h) = if size_part.is_empty() {
        (None, None)
    } else {
        let (w, h) = size_part
            .split_once('x')
            .ok_or_else(|| anyhow!("尺寸格式应为 WxH，如 1280x720"))?;
        (
            Some(w.parse().context("宽度无效")?),
            Some(h.parse().context("高度无效")?),
        )
    };
    let dpi = match dpi_part {
        Some(d) if !d.is_empty() => Some(d.parse().context("DPI 无效")?),
        _ => None,
    };
    Ok(NewDisplay {
        width: w,
        height: h,
        dpi,
    })
}

fn cmd_run(
    package: String,
    serial: Option<String>,
    avd: Option<String>,
    display: String,
    title: Option<String>,
    no_audio: bool,
    max_fps: Option<f32>,
    fullscreen: bool,
) -> Result<()> {
    let nd = parse_new_display(&display)?;
    let client = adb();
    client.ensure_server(None).ok();
    let serial = match resolve_install_serial(&client, serial)? {
        Some(s) => s,
        None => {
                let avd = avd
                    .or_else(pick_default_avd)
                    .ok_or_else(|| anyhow!("无在线设备且无法确定默认 AVD；先运行 `mwine vm start <avd>` 或设置 MWINE_AVD"))?;
                let sdk = Sdk::detect().context("Android SDK 未找到")?;
                println!("(无在线设备，启动 {avd} ...)");
                // windowed：multidisplay 宿主窗口必须由 emulator GUI 进程创建，
                // headless 下无 NSWindow 可用（这是显示直通架构的硬前提）
                let handle = EmulatorHandle::launch_headless(
                    &sdk,
                    &avd,
                    &LaunchOptions {
                        windowed: true,
                        ..Default::default()
                    },
                    &client,
                )?;
                println!("serial: {}，等待引导 ...", handle.serial);
                handle.wait_boot_completed(&client, std::time::Duration::from_secs(300))?;
                record_last_avd(&avd);
                handle.serial.clone()
            }
    };
    // 渲染模式按 app 选择（~/.mwine/appmodes.json）：
    // - window：scrcpy 独立窗口（macOS 原生融合，适合工具/聊天类）
    // - emulator（默认）：主屏 + emulator 窗口置前台（gfxstream/Metal 直渲染，适合游戏）
    // 运行时权限补授（已装应用也会是 granted=false）
    let _ = client
        .grant_runtime_permissions(&serial, &package)
        .map(|n| {
            if n > 0 {
                println!("✓ 已授予 {n} 项运行时权限");
            }
        });


    // 解析入口并启动（主显示器）
    let comp = client
        .shell_blocking(
            &serial,
            &format!("cmd package resolve-activity --brief {package} | tail -1"),
            std::time::Duration::from_secs(10),
        )
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| s.contains('/'))
        .ok_or_else(|| {
            // GUI 场景（点壳）用户看不到 stderr：弹 macOS 通知指路
            let _ = std::process::Command::new("osascript")
                .arg("-e")
                .arg(format!(
                    "display notification \"{package} 未安装，请把 APK 拖到 MWine 窗口重新安装\" with title \"Houttuynia\""
                ))
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status();
            anyhow!("无法解析 {package} 的启动 Activity（未安装？）")
        })?;
    client
        .shell_blocking(
            &serial,
            &format!("am force-stop {package}; am start -n '{comp}'"),
            std::time::Duration::from_secs(15),
        )
        .map_err(|e| anyhow!("am start 失败: {e}"))?;

    // 等 app 真正到前台（否则窗口置前了用户看到的还是桌面/上个应用）
    for _ in 0..12 {
        std::thread::sleep(Duration::from_millis(500));
        if let Ok(o) = client.shell_blocking(
            &serial,
            "dumpsys activity activities 2>/dev/null | grep -m1 topResumedActivity",
            Duration::from_secs(3),
        ) {
            if o.stdout_string().contains(&package) {
                break;
            }
        }
    }

    // emulator 窗口置前（双保险：osascript + 首次运行会请求一次自动化授权）
    let _ = std::process::Command::new("osascript")
        .arg("-e")
        .arg(
            "tell application \"System Events\" to set frontmost of (first process whose name begins with \"qemu-system\") to true",
        )
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();

    println!("✓ 已启动 {package}（emulator 窗口已置前台）");
    Ok(())
}

fn cmd_mirror(args: MirrorArgs) -> Result<()> {
    let jar =
        mwine_scrcpy::find_server_jar().context("scrcpy-server 未找到（brew install scrcpy）")?;
    let client = adb();
    client.ensure_server(None).ok();

    // 设备在线性：显式 serial > 在线设备 > 自动启动 AVD（默认选择规则同 run）
    let serial = match args.serial.clone() {
        Some(s) => s,
        None => match client.find_online_device() {
            Ok(d) => d.serial,
            Err(_) => {
                let avd = args
                    .avd
                    .clone()
                    .or_else(pick_default_avd)
                    .ok_or_else(|| {
                        anyhow!("无在线设备且无法确定默认 AVD；先运行 `mwine vm start <avd>` 或设置 MWINE_AVD")
                    })?;
                let sdk = Sdk::detect().context("Android SDK 未找到")?;
                println!("(无在线设备，启动 {avd} ...)");
                let handle = EmulatorHandle::launch_headless(
                    &sdk,
                    &avd,
                    &LaunchOptions::default(),
                    &client,
                )?;
                println!("serial: {}，等待引导 ...", handle.serial);
                handle.wait_boot_completed(&client, Duration::from_secs(300))?;
                record_last_avd(&avd);
                handle.serial.clone()
            }
        },
    };

    let mut options = ServerOptions {
        video_bit_rate: args.bitrate,
        audio: args.audio,
        keep_active: args.start_app.is_some(),
        ..Default::default()
    };
    if let Some(nd) = &args.new_display {
        options.new_display = Some(parse_new_display(nd)?);
        options.flex_display = args.flex;
    }

    println!(
        "连接 {serial}（{}server jar: {}）",
        if options.new_display.is_some() {
            "虚拟显示器模式，"
        } else {
            ""
        },
        jar.display()
    );

    let scrcpy = ScrcpyClient::new(client.clone(), &serial, &jar);
    let scrcpy = ScrcpyClient { options, ..scrcpy };
    let mut session = scrcpy
        .start()
        .map_err(|e| anyhow!("scrcpy 会话建立失败: {e}"))?;
    println!(
        "✓ 会话已建立：设备 {:?}，codec {}，初始分辨率 {}",
        session.device_name,
        session.video.codec().name(),
        {
            let (w, h) = session.video.size();
            format!("{w}x{h}")
        }
    );

    if let Some(app) = &args.start_app {
        println!("启动应用 {app} ...");
        session
            .control
            .as_mut()
            .ok_or_else(|| anyhow!("control=false，无法启动应用"))?
            .send(&ControlMessage::StartApp { name: app.clone() })
            .map_err(|e| anyhow!("发送 StartApp 失败: {e}"))?;
    }

    // 音频统计线程（mirror 为诊断工具，不接输出设备）
    if let Some(audio) = session.audio.take() {
        println!("✓ 音频流：{}", audio.codec().name());
        std::thread::spawn(move || {
            let mut audio = audio;
            let (mut packets, mut bytes) = (0u64, 0u64);
            let mut last = Instant::now();
            loop {
                match audio.next_packet() {
                    Ok(Some(p)) => {
                        packets += 1;
                        bytes += p.data.len() as u64;
                        if last.elapsed() >= Duration::from_secs(2) {
                            println!("  [audio] packets={packets} bytes={bytes}");
                            last = Instant::now();
                        }
                    }
                    Ok(None) => {
                        println!("  [audio] 流结束（{packets} 包，{bytes} 字节）");
                        break;
                    }
                    Err(e) => {
                        eprintln!("  [audio] 流错误: {e}");
                        break;
                    }
                }
            }
        });
    }

    let mut record_file = match &args.record {
        Some(path) => Some(std::fs::File::create(path).context("无法创建录制文件")?),
        None => None,
    };

    let deadline = if args.duration > 0 {
        Some(Instant::now() + Duration::from_secs(args.duration))
    } else {
        None
    };

    let start = Instant::now();
    let mut packets = 0u64;
    let mut bytes = 0u64;
    let mut config_packets = 0u64;
    let mut last_report = Instant::now();
    let mut last_pts;
    let mut max_res = session.video.size();

    println!("抓流中（时长 {}s，0 表示直到流结束）...", args.duration);
    // 读超时让 duration 在"虚拟显示器无内容、编码器不产帧"时也能生效。
    // 已知硬化点：单个包跨超时窗口会被截断（本地回环 + 小包场景风险低），
    // 正式版应改为 poll() 就绪检测后再读。
    session
        .video
        .stream_mut()
        .set_read_timeout(Some(Duration::from_millis(500)))
        .ok();
    loop {
        if let Some(dl) = deadline {
            if Instant::now() > dl {
                break;
            }
        }
        match session.video.next_packet() {
            Err(mwine_scrcpy::Error::Io(ref e))
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                continue; // 读超时：无新帧，回到 deadline 检查
            }
            Ok(Some(pkt)) => {
                packets += 1;
                bytes += pkt.data.len() as u64;
                if pkt.config {
                    config_packets += 1;
                }
                last_pts = pkt.pts;
                let res = session.video.size();
                if res.0 * res.1 > max_res.0 * max_res.1 {
                    max_res = res;
                }
                if let Some(f) = record_file.as_mut() {
                    use std::io::Write;
                    f.write_all(&pkt.data)?;
                }
                if last_report.elapsed() >= Duration::from_secs(2) {
                    let (w, h) = session.video.size();
                    println!(
                        "  [{}s] packets={packets} bytes={bytes} codec包={config_packets} 分辨率={w}x{h} 最新PTS={last_pts}",
                        start.elapsed().as_secs()
                    );
                    last_report = Instant::now();
                }
            }
            Ok(None) => {
                println!("视频流结束");
                break;
            }
            Err(e) => return Err(anyhow!("视频流错误: {e}")),
        }
    }

    if let Some(path) = &args.record {
        println!("✓ 已录制到 {}", path.display());
    }
    println!(
        "统计：{packets} 包，{bytes} 字节，其中配置包 {config_packets} 个，最大分辨率 {}x{}，持续 {:.1}s",
        max_res.0,
        max_res.1,
        start.elapsed().as_secs_f32()
    );

    session.stop();
    Ok(())
}

/// 提取 Mirror 子命令参数（避免大结构体到处传）。
struct MirrorArgs {
    serial: Option<String>,
    avd: Option<String>,
    new_display: Option<String>,
    flex: bool,
    start_app: Option<String>,
    record: Option<PathBuf>,
    duration: u64,
    bitrate: u32,
    audio: bool,
}

fn main() -> Result<()> {
    // bundle 模式：本二进制作为 .app 的 CFBundleExecutable 被双击启动
    // （正规 bundle 进程：macOS 菜单栏/Dock 图标/Cmd+Tab 全部生效）。
    // 读 Contents/Resources/app.conf 得到要运行的应用。
    if let Some(cfg) = bundled_app_config() {
        return bundled_main(cfg);
    }
    let cli = Cli::parse();
    match cli.command {
        Command::Doctor => print_doctor(),
        Command::Vm { action } => match action {
            VmAction::List => cmd_vm_list(),
            VmAction::Start { avd, image, mode, snapshot, cold, memory, proxy, data_dir, extra } => {
                cmd_vm_start(avd, image, mode, snapshot, cold, memory, proxy, data_dir, extra)
            }
            VmAction::Stop { serial } => cmd_vm_stop(serial),
            VmAction::ResetId { serial, deep } => cmd_vm_reset_id(serial, deep),
        },
        Command::Devices => cmd_devices(),
        Command::Image { action } => match action {
            ImageAction::Import { path, sha256, name } => cmd_image_import(path, sha256, name),
            ImageAction::OfficialList { filter, proxy } => cmd_image_official_list(filter, proxy),
            ImageAction::OfficialPull { target, proxy } => cmd_image_official_pull(target, proxy),
        },
        Command::Install { apk, serial, progress_file } => cmd_install(apk, serial, progress_file),
        Command::Apps { action } => match action {
            AppsAction::List { serial } => cmd_apps_list(serial),
            AppsAction::Launch { package, serial } => cmd_apps_launch(package, serial),
                AppsAction::Uninstall { package, serial } => cmd_apps_uninstall(package, serial),
        },
        Command::Files { action } => match action {
            FilesAction::Push { local, to, serial } => cmd_files_push(local, to, serial),
            FilesAction::Pull { remote, to, serial } => cmd_files_pull(remote, to, serial),
            FilesAction::Ls { dir, serial } => cmd_files_ls(dir, serial),
        },
        Command::Kill { vm } => cmd_kill(vm),
        Command::Monitor {
            cpu,
            mem_mb,
            limit,
            idle_stop,
            auto_start,
            remove_auto_start,
        } => {
            if remove_auto_start {
                return uninstall_monitor_agent();
            }
            if auto_start {
                // 安装即由 launchd 拉起实例，本进程不再前台运行（避免双开）
                return install_monitor_agent(cpu, mem_mb, limit, idle_stop);
            }
            cmd_monitor(cpu, mem_mb, limit, idle_stop)
        }
        Command::Camera { action } => match action {
            CameraAction::Image { path, serial, front } => cmd_camera_image(path, serial, front),
            CameraAction::Reset { serial, front } => cmd_camera_reset(serial, front),
        },
        Command::Keys { action } => match action {
            KeysAction::Shot { package, serial } => cmd_keys_shot(package, serial),
            KeysAction::Get { package } => cmd_keys_get(package),
            KeysAction::Set { package, file } => cmd_keys_set(package, file),
            KeysAction::Watch { package } => cmd_keys_watch(package),
        },
        Command::Notify { serial, setup_only } => cmd_notify(serial, setup_only),
        Command::App { action } => match action {
            AppAction::Create {
                package,
                apk,
                name,
                out,
                avd,
                no_install,
                print_path,
            } => cmd_app_create(package, apk, name, out, avd, no_install, print_path),
            AppAction::Installer { out } => cmd_app_installer(out),
        },
        Command::Run {
            package,
            serial,
            avd,
            display,
            title,
            no_audio,
            max_fps,
            fullscreen,
        } => cmd_run(package, serial, avd, display, title, no_audio, max_fps, fullscreen),
        Command::Mirror {
            serial,
            avd,
            new_display,
            flex,
            start_app,
            record,
            duration,
            bitrate,
            audio,
        } => cmd_mirror(MirrorArgs {
            serial,
            avd,
            new_display,
            flex,
            start_app,
            record,
            duration,
            bitrate,
            audio,
        }),
    }
}

// ---------------- 通知桥 ----------------

/// forward 模式端口：companion 在设备 127.0.0.1:BRIDGE_DEVICE_PORT 起 TCP server，
/// 宿主 forward（本地 BRIDGE_HOST_PORT）后连入读取（与 scrcpy 同向，不依赖 adb reverse）
const BRIDGE_HOST_PORT: u16 = 27186;
const BRIDGE_DEVICE_PORT: u16 = 27185;
const BRIDGE_COMPONENT: &str = "com.mwine.bridge/.BridgeService";
const BRIDGE_PACKAGE: &str = "com.mwine.bridge";

fn find_companion_apk() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("MWINE_BRIDGE_APK") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Some(p);
        }
    }
    // 从可执行文件向上找仓库的 assets/companion/dist/mwine-bridge.apk
    let mut dir = std::env::current_exe().ok()?;
    while dir.pop() {
        let candidate = dir.join("assets/companion/dist/mwine-bridge.apk");
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

fn shell_quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', "'\\''"))
}

fn post_macos_notification(title: &str, subtitle: &str, body: &str) {
    let script = format!(
        "display notification {} with title {} subtitle {}",
        shell_quote(body),
        shell_quote(title),
        shell_quote(subtitle)
    );
    let _ = std::process::Command::new("osascript")
        .arg("-e")
        .arg(&script)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();
}

fn listener_enabled(client: &AdbClient, serial: &str) -> Result<bool> {
    let out = client.shell_blocking(
        serial,
        "settings get secure enabled_notification_listeners",
        Duration::from_secs(15),
    )?;
    Ok(out.stdout_string().contains("com.mwine.bridge"))
}

/// 授权 NotificationListener 并强制重绑（触发 companion 的 onListenerConnected -> hello）。
/// 首选 API 30+ 的 `cmd notification`（立即重绑）；老设备走 settings 双写触发观察者。
fn grant_and_rebind(client: &AdbClient, serial: &str) -> Result<()> {
    // Android 13 实测：包刚装完 PackageManager 未就绪时 allow_listener 返回成功但
    // 不生效（静默无效），需重试到 enabled_notification_listeners 真正出现为止。
    let allow_once = |comp: &str| {
        client
            .shell_blocking(
                serial,
                &format!("cmd notification allow_listener {comp}"),
                Duration::from_secs(15),
            )
            .map(|o| o.ok())
            .unwrap_or(false)
    };
    if allow_once(BRIDGE_COMPONENT) {
        // allow 对已授权组件是幂等操作、不会重绑；disallow -> allow 强制一次全新绑定
        let _ = client.shell_blocking(
            serial,
            &format!("cmd notification disallow_listener {BRIDGE_COMPONENT}"),
            Duration::from_secs(15),
        );
        if allow_once(BRIDGE_COMPONENT) {
            for attempt in 0..6 {
                if listener_enabled(client, serial)? {
                    return Ok(());
                }
                if attempt < 5 {
                    std::thread::sleep(Duration::from_secs(1));
                    allow_once(BRIDGE_COMPONENT);
                }
            }
            return Err(anyhow!(
                "listener 授权后未生效（enabled_notification_listeners 未包含 mwine）"
            ));
        }
    }
    // 回退：settings 追加（保留其他 listener），空值->目标值两次写入触发观察者重绑
    let cur = client
        .shell_blocking(
            serial,
            "settings get secure enabled_notification_listeners",
            Duration::from_secs(15),
        )
        .map(|o| o.stdout_string().trim().to_string())
        .unwrap_or_default();
    let others: Vec<&str> = cur
        .split(':')
        .filter(|s| !s.is_empty() && *s != "null" && !s.contains("com.mwine.bridge"))
        .collect();
    let mut target = others.join(":");
    if !target.is_empty() {
        target.push(':');
    }
    target.push_str(BRIDGE_COMPONENT);
    let put = |v: &str| {
        client.shell_blocking(
            serial,
            &format!("settings put secure enabled_notification_listeners '{v}'"),
            Duration::from_secs(15),
        )
    };
    let _ = put("");
    put(&target).map_err(|e| anyhow!("settings 授权失败: {e}"))?;
    if listener_enabled(client, serial)? {
        Ok(())
    } else {
        Err(anyhow!(
            "listener 授权未生效；可在设备 设置->通知访问权 中手动开启 com.mwine.bridge"
        ))
    }
}

/// 媒体栈可用性（scrcpy 视频的前提 = guest 能创建编码器）。
/// 用 screenrecord 1 秒采样，结果按 qemu pid 缓存。
pub fn media_ok(client: &mwine_adb::AdbClient, serial: &str) -> bool {
    let home = std::env::var("HOME").unwrap_or_default();
    let cache = PathBuf::from(&home).join(".mwine/media.state");
    let qemu_pid = std::process::Command::new("pgrep")
        .arg("-f").arg("qemu-system-aarch64")
        .output()
        .ok()
        .and_then(|o| String::from_utf8_lossy(&o.stdout).lines().next().and_then(|l| l.trim().parse::<u32>().ok()));
    let Some(pid) = qemu_pid else { return false };
    if let Ok(text) = std::fs::read_to_string(&cache) {
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&text) {
            if v["pid"].as_u64() == Some(pid as u64) {
                return v["ok"].as_bool().unwrap_or(false);
            }
        }
    }
    // 现场探测：1 秒录屏，产物非空 = 编码器可建
    let ok = client
        .shell_blocking(
            serial,
            "rm -f /data/local/tmp/mwine-probe.mp4; timeout 6 screenrecord --time-limit 1 /data/local/tmp/mwine-probe.mp4 >/dev/null 2>&1; test -s /data/local/tmp/mwine-probe.mp4 && echo YES || echo NO",
            Duration::from_secs(15),
        )
        .ok()
        .map(|o| o.stdout_string().trim().contains("YES"))
        .unwrap_or(false);
    let _ = std::fs::write(&cache, format!("{{\"pid\":{pid},\"ok\":{ok}}}"));
    ok
}

fn keys_dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_default();
    let d = PathBuf::from(home).join(".mwine/keymaps");
    std::fs::create_dir_all(&d).ok();
    d
}

fn cmd_keys_shot(package: String, serial: Option<String>) -> Result<()> {
    let serial = resolve_serial(serial.as_deref())?;
    let client = adb();
    let out = client
        .shell_blocking(&serial, "screencap -p", Duration::from_secs(10))
        .context("截图失败")?;
    // shell v2 协议分离 stdout，无 tty 的 \n→\r\n 污染，PNG 直出
    let mut data = out.stdout;
    if data.first() == Some(&0x0a) { /* skip */ }
    // PNG 魔数校验
    anyhow::ensure!(data.len() > 8 && data.starts_with(&[0x89, b'P', b'N', b'G']), "非 PNG 输出");
    let dir = PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".mwine/keyshots");
    std::fs::create_dir_all(&dir).ok();
    let path = dir.join(format!("{package}.png"));
    std::fs::write(&path, &data)?;
    println!("{}", path.display());
    Ok(())
}

fn cmd_keys_get(package: String) -> Result<()> {
    let path = keys_dir().join(format!("{package}.json"));
    if path.is_file() {
        println!("{}", std::fs::read_to_string(&path)?);
    } else {
        println!("[]");
    }
    Ok(())
}

fn cmd_keys_set(package: String, file: Option<PathBuf>) -> Result<()> {
    let json = match file {
        Some(f) => std::fs::read_to_string(&f)?,
        None => {
            use std::io::Read;
            let mut s = String::new();
            std::io::stdin().read_to_string(&mut s)?;
            s
        }
    };
    // 校验
    let parsed: serde_json::Value = serde_json::from_str(&json).context("JSON 格式错误")?;
    anyhow::ensure!(parsed.is_array(), "需为绑定数组");
    std::fs::write(keys_dir().join(format!("{package}.json")), json)?;
    println!("✓ 已保存 {} 的键位映射", parsed.as_array().map(|a| a.len()).unwrap_or(0));
    Ok(())
}

fn cmd_keys_watch(package: String) -> Result<()> {
    // 注入器是 Swift 小工具（CGEventTap 原生支持）
    let exe = std::env::current_exe()?;
    let candidates = [
        // 1) .app bundle 内
        exe.parent()
            .and_then(|p| p.parent())
            .map(|c| c.join("Resources/bin/MWineKeys")),
        // 2) 仓库构建产物
        exe.ancestors()
            .find(|a| a.ends_with("target/release"))
            .map(|t| t.parent().unwrap().join("app/KeyTap/.build/release/MWineKeys")),
        // 3) ~/.mwine/bin
        std::env::var("HOME").ok().map(|h| PathBuf::from(h).join(".mwine/bin/MWineKeys")),
    ];
    let bin = candidates
        .into_iter()
        .flatten()
        .find(|p| p.is_file())
        .context("MWineKeys 注入器未找到（app/KeyTap 未构建？）")?;
    let keymap = keys_dir().join(format!("{package}.json"));
    anyhow::ensure!(keymap.is_file(), "{package} 没有键位映射，先用 `mwine keys set` 创建");
    // adb 路径（bundle 或 SDK）
    let adb = std::env::var("HOME")
        .ok()
        .map(|h| PathBuf::from(h).join("Library/Android/sdk/platform-tools/adb"))
        .filter(|p| p.is_file())
        .unwrap_or_else(|| PathBuf::from("adb"));
    println!("按键映射已启用：{package}（按住映射键触发，Ctrl-C 退出）");
    let status = std::process::Command::new(&bin)
        .arg(&keymap)
        .arg(&adb)
        .status()
        .with_context(|| format!("运行 {:?} 失败", bin))?;
    std::process::exit(status.code().unwrap_or(0));
}

fn cmd_notify(serial: Option<String>, setup_only: bool) -> Result<()> {
    let client = adb();
    client.ensure_server(None).ok();
    let serial = resolve_serial(serial.as_deref())?;
    let apk = find_companion_apk().context(
        "companion APK 未找到：先运行 scripts/build_companion.sh，或设置 MWINE_BRIDGE_APK",
    )?;

    // 安装（已装则 -r 覆盖，保持幂等）
    println!("安装通知桥 APK（{}）...", apk.display());
    client.install_apk(&serial, &apk, Duration::from_secs(120))?;
    println!("✓ 已安装 com.mwine.bridge");

    // Android 13+：POST_NOTIFICATIONS 是运行时权限且默认拒绝，
    // companion targetSdk 33，不授权则测试通知/状态通知全部静默丢失
    let granted = client
        .grant_runtime_permissions(&serial, BRIDGE_PACKAGE)
        .unwrap_or(0);
    if granted > 0 {
        println!("✓ 已授予 {granted} 项运行时权限（含 POST_NOTIFICATIONS）");
    }
    // 包刚装完 PackageManager 可能未就绪，listener 授权前稍等
    std::thread::sleep(Duration::from_secs(1));

    // 授权 + 强制重绑（重装后 companion 进程被杀，重绑保证 server 端口起好）
    grant_and_rebind(&client, &serial)?;
    println!("✓ listener 已授权并重绑");

    // forward 模式（与 scrcpy 同向，不依赖 adb reverse）：
    // companion 在设备 127.0.0.1:BRIDGE_DEVICE_PORT 起 TCP server，宿主连接本机转发端口读取
    let _ = client.remove_reverse(&serial, BRIDGE_HOST_PORT);
    client.forward_tcp(&serial, BRIDGE_HOST_PORT, BRIDGE_DEVICE_PORT)?;
    println!(
        "✓ forward 已建立（宿主 :{BRIDGE_HOST_PORT} -> 设备 :{BRIDGE_DEVICE_PORT}）"
    );

    if setup_only {
        println!("✓ 安装完成。运行 `mwine notify` 开始监听");
        return Ok(());
    }

    let seen: Arc<Mutex<HashMap<String, i64>>> = Arc::new(Mutex::new(HashMap::new()));
    println!("监听中（Ctrl-C 退出）...");
    loop {
        match std::net::TcpStream::connect(("127.0.0.1", BRIDGE_HOST_PORT)) {
            Ok(stream) => {
                println!("✓ 通知桥已连接（com.mwine.bridge）");
                if let Err(e) = handle_bridge_connection(stream, seen.clone()) {
                    eprintln!("[mwine] 通知桥读取错误: {e}");
                }
                println!("～ 连接断开，2s 后重连 ...");
            }
            Err(e) => {
                eprintln!("[mwine] 连接失败（{e}），重建 forward 后 2s 重试 ...");
                // adb server 重启等场景 forward 会丢，重建后重试
                let _ = client.remove_reverse(&serial, BRIDGE_HOST_PORT);
                let _ = client.forward_tcp(&serial, BRIDGE_HOST_PORT, BRIDGE_DEVICE_PORT);
            }
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}

fn handle_bridge_connection(
    stream: std::net::TcpStream,
    seen: Arc<Mutex<HashMap<String, i64>>>,
) -> Result<()> {
    use std::io::BufRead;
    let reader = std::io::BufReader::new(stream);
    for line in reader.lines() {
        let Ok(line) = line else { break };
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let Ok(v) = serde_json::from_str::<serde_json::Value>(line) else {
            continue;
        };
        if v.get("event").and_then(|e| e.as_str()) == Some("hello") {
            continue; // 连接成功时已打印
        }
        let title = v
            .get("title")
            .and_then(|t| t.as_str())
            .filter(|t| !t.is_empty())
            .unwrap_or("mwine");
        let pkg = v.get("pkg").and_then(|p| p.as_str()).unwrap_or("");
        let text = v.get("text").and_then(|t| t.as_str()).unwrap_or("");
        let key = v.get("key").and_then(|k| k.as_str()).unwrap_or("");
        let post_time = v.get("postTime").and_then(|t| t.as_i64()).unwrap_or(0);

        // 去重：同一通知的 (key, postTime) 只放行一次
        {
            let mut map = seen.lock().unwrap();
            if map.len() > 4096 {
                map.clear();
            }
            if map.get(key).is_some_and(|t| *t == post_time) {
                continue;
            }
            map.insert(key.to_string(), post_time);
        }

        println!("[通知] {pkg}: {title} - {text}");
        post_macos_notification(title, pkg, text);
    }
    Ok(())
}

// ---------------- .app 壳 / APK 安装器 ----------------

fn find_aapt2() -> Option<PathBuf> {
    Sdk::detect()
        .ok()
        .and_then(|sdk| sdk.find_build_tool("aapt2"))
}

/// 运行外部工具，非零退出时把 stderr 摘要带进错误。
fn run_tool(cmd: &mut std::process::Command) -> Result<String> {
    let out = cmd.output().with_context(|| format!("无法执行 {:?}", cmd))?;
    if !out.status.success() {
        return Err(anyhow!(
            "{:?} 失败（{}）：{}",
            cmd,
            out.status,
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    Ok(String::from_utf8_lossy(&out.stdout).to_string())
}

/// aapt2 dump badging 的解析结果。
struct ApkBadging {
    package: String,
    label: Option<String>,
    /// 最高密度图标在 APK 内的 zip entry（可能是 adaptive 的 .xml）
    icon: Option<String>,
}

fn unquote(s: &str) -> String {
    let s = s.trim();
    s.strip_prefix('\'')
        .and_then(|s| s.strip_suffix('\''))
        .unwrap_or(s)
        .to_string()
}

fn is_raster_icon(path: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    [".png", ".webp", ".jpg", ".jpeg"]
        .iter()
        .any(|e| lower.ends_with(e))
}

fn parse_badging(text: &str) -> ApkBadging {
    let mut package = None;
    let mut label = None;
    let mut icons: Vec<(u32, String)> = Vec::new();
    for line in text.lines() {
        if package.is_none() {
            if let Some(rest) = line.strip_prefix("package:") {
                if let Some(idx) = rest.find("name=") {
                    let v = rest[idx + "name=".len()..]
                        .split_whitespace()
                        .next()
                        .unwrap_or("");
                    let v = unquote(v);
                    if !v.is_empty() {
                        package = Some(v);
                    }
                }
            }
        }
        if label.is_none() {
            if let Some(rest) = line.strip_prefix("application-label:") {
                let v = unquote(rest);
                if !v.is_empty() {
                    label = Some(v);
                }
            }
        }
        if let Some(rest) = line.strip_prefix("application-icon-") {
            if let Some((dpi, path)) = rest.split_once(':') {
                if let Ok(dpi) = dpi.trim().parse::<u32>() {
                    let path = unquote(path);
                    if !path.is_empty() {
                        icons.push((dpi, path));
                    }
                }
            }
        }
    }
    // 优先位图资源；全是 adaptive xml 时先保留，后面再回退扫描
    let pool: Vec<&(u32, String)> = {
        let raster: Vec<&(u32, String)> =
            icons.iter().filter(|(_, p)| is_raster_icon(p)).collect();
        if raster.is_empty() {
            icons.iter().collect()
        } else {
            raster
        }
    };
    let icon = pool
        .iter()
        .max_by_key(|(dpi, _)| *dpi)
        .map(|(_, p)| p.clone());
    ApkBadging {
        package: package.unwrap_or_default(),
        label,
        icon,
    }
}

/// 从目录限定符估计资源密度（mipmap-xxxhdpi-v4 等）。
fn density_of(path: &str) -> u32 {
    const NAMES: [(&str, u32); 5] = [
        ("xxxhdpi", 640),
        ("xxhdpi", 480),
        ("xhdpi", 320),
        ("hdpi", 240),
        ("mdpi", 160),
    ];
    for seg in path.split('/') {
        for (name, dpi) in NAMES {
            if seg.contains(name) {
                return dpi;
            }
        }
        if let Some(dpi) = seg
            .split('-')
            .filter_map(|p| p.strip_suffix("dpi"))
            .filter_map(|p| p.parse::<u32>().ok())
            .max()
        {
            return dpi;
        }
    }
    0
}

/// adaptive icon（.xml）在 APK 里通常伴随同名位图；扫描 zip 目录找最高密度位图。
fn pick_raster_fallback(entries: &[String], xml_icon: &str) -> Option<String> {
    let stem = xml_icon
        .rsplit('/')
        .next()
        .unwrap_or("")
        .split('.')
        .next()
        .unwrap_or("");
    if stem.is_empty() {
        return None;
    }
    let mut best: Option<(u32, &String)> = None;
    for e in entries {
        if !is_raster_icon(e) || !e.starts_with("res/") {
            continue;
        }
        let e_stem = e
            .rsplit('/')
            .next()
            .unwrap_or("")
            .split('.')
            .next()
            .unwrap_or("");
        if e_stem == stem {
            let d = density_of(e);
            if best.as_ref().is_none_or(|(bd, _)| d > *bd) {
                best = Some((d, e));
            }
        }
    }
    best.map(|(_, e)| e.clone())
}

/// 从 APK 提取指定 zip entry 到本地文件（unzip -p）。
fn apk_extract_entry(apk: &Path, entry: &str, dest: &Path) -> Result<()> {
    let out = std::process::Command::new("unzip")
        .arg("-p")
        .arg(apk)
        .arg(entry)
        .stderr(std::process::Stdio::piped())
        .output()
        .with_context(|| format!("unzip 执行失败（{}）", apk.display()))?;
    if !out.status.success() {
        return Err(anyhow!(
            "提取 {} 失败：{}",
            entry,
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    std::fs::write(dest, &out.stdout)?;
    Ok(())
}

/// 位图 -> AppIcon.icns（sips 生成 iconset + iconutil 打包）。
fn make_icns(source: &Path, dest_icns: &Path) -> Result<()> {
    let iconset = dest_icns.with_extension("iconset");
    std::fs::create_dir_all(&iconset)?;
    let master = iconset.join("master.png");
    run_tool(
        std::process::Command::new("sips")
            .args(["-s", "format", "png"])
            .arg(source)
            .arg("--out")
            .arg(&master),
    )
    .context("sips 转换图标失败（webp/png 应支持）")?;
    const SIZES: [(&str, u32); 10] = [
        ("icon_16x16.png", 16),
        ("icon_16x16@2x.png", 32),
        ("icon_32x32.png", 32),
        ("icon_32x32@2x.png", 64),
        ("icon_128x128.png", 128),
        ("icon_128x128@2x.png", 256),
        ("icon_256x256.png", 256),
        ("icon_256x256@2x.png", 512),
        ("icon_512x512.png", 512),
        ("icon_512x512@2x.png", 1024),
    ];
    for (name, size) in SIZES {
        run_tool(
            std::process::Command::new("sips")
                .arg("-z")
                .arg(size.to_string())
                .arg(size.to_string())
                .arg(&master)
                .arg("--out")
                .arg(iconset.join(name)),
        )?;
    }
    run_tool(
        std::process::Command::new("iconutil")
            .args(["-c", "icns"])
            .arg(&iconset)
            .arg("-o")
            .arg(dest_icns),
    )?;
    std::fs::remove_dir_all(&iconset).ok();
    Ok(())
}

/// 设备侧渲染应用图标：companion IconDump（app_process 模式，shell uid 即可）
/// -> PNG -> icns。覆盖 adaptive icon / 矢量 / 资源混淆等宿主侧无法处理的情况。
fn render_icon_on_device(
    client: &AdbClient,
    serial: &str,
    package: &str,
    resources: &Path,
) -> Option<PathBuf> {
    // companion 已装则复用，未装则安装（幂等）
    if !client
        .shell_blocking(serial, "pm path com.mwine.bridge", Duration::from_secs(10))
        .map(|o| o.stdout_string().contains("package:"))
        .unwrap_or(false)
    {
        let apk = find_companion_apk()?;
        client
            .install_apk(serial, &apk, Duration::from_secs(120))
            .ok()?;
    }
    let cap = client
        .shell_blocking(serial, "pm path com.mwine.bridge", Duration::from_secs(10))
        .ok()?
        .stdout_string()
        .lines()
        .find_map(|l| l.trim().strip_prefix("package:").map(|s| s.trim().to_string()))?;

    let remote_png = format!("/data/local/tmp/mwine-icon-{}.png", std::process::id());
    let out = client.shell_blocking(
        serial,
        &format!(
            "CLASSPATH={cap} app_process / com.mwine.bridge.IconDump {package} {remote_png} 512"
        ),
        Duration::from_secs(30),
    );
    if !out.as_ref().map(|o| o.stdout_string().contains("ok")).unwrap_or(false) {
        eprintln!(
            "    设备渲染失败：{}",
            out.map(|o| o.stdout_string().trim().to_string())
                .unwrap_or_else(|e| e.to_string())
        );
        return None;
    }
    let png = match client.pull_file(serial, &remote_png) {
        Ok(d) if d.len() > 1000 => d,
        _ => return None,
    };
    let _ = client.shell_blocking(serial, &format!("rm -f '{remote_png}'"), Duration::from_secs(10));
    let tmp = std::env::temp_dir().join(format!("mwine-icon-{}.png", std::process::id()));
    std::fs::write(&tmp, png).ok()?;
    let icns = resources.join("AppIcon.icns");
    if make_icns(&tmp, &icns).is_err() {
        std::fs::remove_file(&icns).ok();
        std::fs::remove_file(&tmp).ok();
        return None;
    }
    std::fs::remove_file(&tmp).ok();
    Some(icns)
}

/// 尽力从 APK 生成 Resources/AppIcon.icns；失败返回 None（壳仍可用，无图标）。
fn extract_app_icon(apk: &Path, badging: &ApkBadging, resources: &Path) -> Option<PathBuf> {
    let entry = badging.icon.as_ref()?;
    let entry = if !is_raster_icon(entry) {
        let listing = std::process::Command::new("unzip")
            .arg("-Z1")
            .arg(apk)
            .output()
            .ok()?;
        let entries: Vec<String> = String::from_utf8_lossy(&listing.stdout)
            .lines()
            .map(|s| s.to_string())
            .collect();
        pick_raster_fallback(&entries, entry)?
    } else {
        entry.clone()
    };
    let tmp = std::env::temp_dir().join(format!("mwine-icon-{}", std::process::id()));
    apk_extract_entry(apk, &entry, &tmp).ok()?;
    let icns = resources.join("AppIcon.icns");
    if make_icns(&tmp, &icns).is_err() {
        std::fs::remove_file(&icns).ok();
        return None;
    }
    std::fs::remove_file(&tmp).ok();
    Some(icns)
}

fn xml_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}

/// 作为文件名/CFBundleExecutable 使用的安全化（保留中文等合法字符）。
fn sanitize_file_name(s: &str) -> String {
    s.chars()
        .map(|c| match c {
            '/' | ':' | '\\' | '"' | '\'' | '$' | '`' | '\0' => '_',
            _ => c,
        })
        .collect::<String>()
        .trim()
        .to_string()
}

fn mwine_bin() -> PathBuf {
    // 启动脚本里优先 PATH 中的 mwine；这里只提供生成时刻的回退路径
    std::env::current_exe().unwrap_or_else(|_| PathBuf::from("mwine"))
}

/// 让 LaunchServices 立即收录（Launchpad/Dock 可见），不必等系统扫描。
fn lsregister(app_dir: &Path) -> Result<()> {
    let ls = PathBuf::from(
        "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister",
    );
    run_tool(std::process::Command::new(&ls).arg("-f").arg(app_dir))
        .context("lsregister 注册失败")?;
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn cmd_app_create(
    package: Option<String>,
    apk: Option<PathBuf>,
    name: Option<String>,
    out: Option<PathBuf>,
    avd: Option<String>,
    no_install: bool,
    print_path: bool,
) -> Result<()> {
    let client = adb();
    client.ensure_server(None).ok();
    let online_serial = client.find_online_device().ok().map(|d| d.serial);

    // 解析包名 / 显示名 / 图标来源
    let (package, apk_info) = match apk {
        Some(apk) => {
            let apk = apk.canonicalize().context("APK 路径无效")?;
            let aapt2 = find_aapt2().context(
                "aapt2 未找到（.app 图标提取需要 Android SDK build-tools；mwine doctor 检查）",
            )?;
            let text = run_tool(
                std::process::Command::new(&aapt2)
                    .args(["dump", "badging"])
                    .arg(&apk),
            )
            .context("aapt2 dump badging 失败（APK 损坏？）")?;
            let badging = parse_badging(&text);
            if badging.package.is_empty() {
                return Err(anyhow!("badging 里没有包名：{}", apk.display()));
            }
            if !no_install {
                match &online_serial {
                    Some(serial) => {
                        println!("安装 {} 到 {serial} ...", apk.display());
                        let mut bar = InstallBar::new();
                        let r = client.install_apk_with_progress(
                            serial,
                            &apk,
                            Duration::from_secs(600),
                            |p| bar.update(p),
                        );
                        bar.finish();
                        r?;
                        println!("✓ 已安装 {}", badging.package);
                    }
                    None => println!(
                        "～ 无在线设备，跳过安装（mwine vm start <avd> 后重跑或 mwine install）"
                    ),
                }
            }
            (badging.package.clone(), Some((apk, badging)))
        }
        None => {
            let package = package.context("请提供包名，或用 --apk <路径> 从 APK 文件创建")?;
            (package, None)
        }
    };

    let display = name
        .or_else(|| apk_info.as_ref().and_then(|(_, b)| b.label.clone()))
        .unwrap_or_else(|| package.clone());
    let exe_name = sanitize_file_name(&display);
    if exe_name.is_empty() {
        return Err(anyhow!("显示名无效"));
    }

    let out_dir = out.unwrap_or_else(|| {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_default();
        home.join("Applications/Mwine")
    });
    ensure_reswatch_bin(&out_dir);
    ensure_appmgr_bin(&out_dir);
    ensure_settings_bin(&out_dir);
    let app_dir = out_dir.join(format!("{exe_name}.app"));
    let contents = app_dir.join("Contents");
    let macos_dir = contents.join("MacOS");
    let resources = contents.join("Resources");
    // 全量重建：清掉旧壳里的残留（例如上次的图标）
    std::fs::remove_dir_all(&app_dir).ok();
    std::fs::create_dir_all(&macos_dir)?;
    std::fs::create_dir_all(&resources)?;

    // 图标：本地 aapt2 提取位图 -> 失败则设备侧渲染（companion IconDump，
    // 经 Android 自身管线渲染 adaptive/矢量/混淆资源，宿主侧无解的图标全靠它）
    let mut icon_rel = None;
    if let Some((apk, badging)) = &apk_info {
        if extract_app_icon(apk, badging, &resources).is_some() {
            icon_rel = Some("AppIcon");
            println!("✓ 图标已从 APK 提取");
        } else if let Some(serial) = &online_serial {
            println!("～ APK 内无位图图标，改用设备侧渲染 ...");
            if render_icon_on_device(&client, serial, &package, &resources).is_some() {
                icon_rel = Some("AppIcon");
                println!("✓ 图标已由设备渲染");
            }
        }
        if icon_rel.is_none() {
            println!("～ 图标提取失败（设备不在线且 APK 无位图图标），使用默认图标");
        }
    } else if let Some(serial) = &online_serial {
        // 包名模式：设备侧渲染最准（adaptive/矢量通吃），失败再回退拉 APK 解析
        if render_icon_on_device(&client, serial, &package, &resources).is_some() {
            icon_rel = Some("AppIcon");
            println!("✓ 图标已由设备渲染");
        } else {
            println!("拉取 {package} 的 APK 以提取图标 ...");
            match client
                .package_apk_path(serial, &package)
                .and_then(|remote| client.pull_file(serial, &remote))
            {
                Ok(bytes) => {
                    let tmp = std::env::temp_dir().join(format!("mwine-icon-{package}.apk"));
                    std::fs::write(&tmp, &bytes)?;
                    if let Some(aapt2) = find_aapt2() {
                        if let Ok(text) = run_tool(
                            std::process::Command::new(&aapt2)
                                .args(["dump", "badging"])
                                .arg(&tmp),
                        ) {
                            let badging = parse_badging(&text);
                            if extract_app_icon(&tmp, &badging, &resources).is_some() {
                                icon_rel = Some("AppIcon");
                                println!("✓ 图标已从设备 APK 提取");
                            }
                        }
                    }
                    std::fs::remove_file(&tmp).ok();
                }
                Err(_) => println!("～ 设备上未安装 {package}（或拉取失败），图标留空"),
            }
        }
    }

    // 嵌入式启动器：mwine 二进制本体拷入 .app 作为 CFBundleExecutable。
    // 正规 bundle 进程：macOS 系统菜单栏显示我们的菜单（摄像头/文件）、
    // Dock/Cmd+Tab 显示真实应用图标；二进制自检 bundle 环境读 app.conf 运行。
    let exe = mwine_bin();
    let launcher = macos_dir.join(&exe_name);
    std::fs::copy(&exe, &launcher).context(format!(
        "拷贝 mwine 二进制到 {} 失败",
        launcher.display()
    ))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut perms = std::fs::metadata(&launcher)?.permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&launcher, perms)?;
    }
    let conf = format!(
        "package={package}\ntitle={display}\ndisplay=970x2048/430\nfps=30\nfullscreen=0\navd={avd}\n",
        package = package,
        display = display,
        avd = avd.as_deref().unwrap_or(""),
    );
    std::fs::write(resources.join("app.conf"), conf)?;

    let icon_key = match icon_rel {
        Some(icon) => format!("  <key>CFBundleIconFile</key><string>{icon}</string>\n"),
        None => String::new(),
    };
    let plist = format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n  <key>CFBundleExecutable</key><string>{}</string>\n  <key>CFBundleIdentifier</key><string>{}</string>\n  <key>CFBundleName</key><string>{}</string>\n  <key>CFBundleDisplayName</key><string>{}</string>\n  <key>CFBundlePackageType</key><string>APPL</string>\n  <key>CFBundleShortVersionString</key><string>0.1</string>\n  <key>CFBundleVersion</key><string>1</string>\n  <key>LSMinimumSystemVersion</key><string>11.0</string>\n  <key>NSHighResolutionCapable</key><true/>\n{icon_key}</dict>\n</plist>\n",
        xml_escape(&exe_name),
        xml_escape(&format!("wine.mwine.{package}")),
        xml_escape(&display),
        xml_escape(&display),
    );
    std::fs::write(contents.join("Info.plist"), plist)?;

    // macOS 26 对 GUI 上下文中拉起的未签名二进制会在 dyld 阶段阻塞（Gatekeeper 校验），
    // 生成的壳与 mwine 本体都做 ad-hoc 签名（失败仅告警，终端直接运行不受影响）
    codesign_best_effort(&app_dir);
    if !codesign_best_effort(&exe) {
        println!("～ mwine 二进制未签名（Finder/.app 启动可能被 Gatekeeper 阻塞）");
        println!("  修复：codesign -f -s - \"{}\"", exe.display());
    }

    lsregister(&app_dir)?;
    // 重建同名壳时 Finder 图标缓存不会自动刷新，touch 触发重扫
    std::fs::File::open(&app_dir)
        .and_then(|f| f.set_modified(std::time::SystemTime::now()))
        .ok();
    if print_path {
        println!("{}", app_dir.display());
    } else {
        println!("✓ 已生成 {}", app_dir.display());
        println!("  Finder 双击即启动应用窗口；Launchpad 稍候收录（也可拖入 Dock）");
    }
    Ok(())
}

/// 生成 APK 双击安装器 .app 并注册为 .apk 默认打开方式。
///
/// 技术要点（macOS 26 实测）：
/// - 双击文件走 Apple Events（odoc），bash 壳收不到路径 → 用 osacompile
///   生成 AppleScript applet（droplet），`on open` 原生接收文件
/// - 未签名 .app 不被 LaunchServices 绑定评估接受 → ad-hoc codesign
/// - 评估器仍可能不选我们 → 经 CoreServices FFI 显式
///   LSSetDefaultRoleHandlerForContentType 写入默认处理器
/// - GUI 上下文拉起未签名 mwine 二进制会阻塞在 dyld → 顺手给本体补签名
fn cmd_app_installer(out: Option<PathBuf>) -> Result<()> {
    let out_dir = out.unwrap_or_else(|| {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_default();
        home.join("Applications/Mwine")
    });
    let app_dir = out_dir.join("Houttuynia Installer.app");
    std::fs::create_dir_all(&out_dir)?;
    std::fs::remove_dir_all(&app_dir).ok();

    // 1. osacompile 生成 applet（可执行名为 droplet）
    let applescript = r#"on open theFiles
	set appPath to POSIX path of (path to me)
	set shPath to quoted form of (appPath & "/Contents/Resources/install.sh")
	set args to ""
	repeat with f in theFiles
		set args to args & " " & quoted form of (POSIX path of f)
	end repeat
	try
		do shell script "/bin/bash " & shPath & args
	on error errMsg
		display notification ("安装器错误: " & errMsg) with title "Houttuynia 安装器"
	end try
end open

on run
	display notification "在 Finder 中双击任意 .apk 文件即可安装到 mwine" with title "Houttuynia 安装器"
end run"#;
    let script_path = std::env::temp_dir().join(format!(
        "mwine-installer-{}.applescript",
        std::process::id()
    ));
    std::fs::write(&script_path, applescript)?;
    run_tool(
        std::process::Command::new("osacompile")
            .arg("-o")
            .arg(&app_dir)
            .arg(&script_path),
    )
    .context("osacompile 生成 applet 失败")?;
    std::fs::remove_file(&script_path).ok();
    let contents = app_dir.join("Contents");

    // 2. 安装逻辑放 Resources/install.sh（AppleScript 只做文件转发）
    let exe = mwine_bin();
    let install_sh = format!(
        r#"#!/bin/bash
# 由 mwine app installer 生成：接收 .apk 路径 -> 安装 -> 生成 .app 壳 -> 启动
LOG="$HOME/Library/Logs/mwine-installer.log"
mkdir -p "$(dirname "$LOG")"
exec >>"$LOG" 2>&1

MWINE="mwine"
if ! command -v mwine >/dev/null 2>&1; then
  MWINE="{exe}"
fi

notify() {{ osascript -e "display notification \\"$1\\" with title \\"Houttuynia 安装器\\" subtitle \\"$2\\"" >/dev/null 2>&1; }}

ok=0; fail=0; last_app=""
N=$#
i=0
PROG="$(mktemp /tmp/mwine-progress.XXXXXX)"
echo "S1 0 准备安装…" > "$PROG"
APP_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
# 悬浮进度窗口（预编译 Swift）；失败不影响安装
if [ -x "$APP_DIR/Contents/Resources/progress-bin" ]; then
  "$APP_DIR/Contents/Resources/progress-bin" "$PROG" >>"$LOG" 2>&1 &
fi

for f in "$@"; do
  case "$f" in
    *.[Aa][Pp][Kk]) ;;
    *) continue ;;
  esac
  i=$((i+1))
  name=$(basename "$f" .apk); name=${{name%.[Aa][Pp][Kk]}}
  echo "[$(date '+%F %T')] install $f"
  echo "APP ${{name}}" > "$PROG"
  echo "S1 0 准备传送安装包（${{i}}/${{N}}）" > "$PROG"
  notify "正在安装 ${{name}}…（${{i}}/${{N}}）" "$f"
  err=$( {{ "$MWINE" install --progress-file "$PROG" "$f"; }} 2>&1 ) || {{
    fail=$((fail+1))
    echo "install failed: $err"
    reason=$(echo "$err" | tail -n 1 | cut -c1-150)
    case "$err" in
      *NO_MATCHING_ABIS*)
        reason="该 APK 不含 arm64 原生库（x86/32 位包），arm64 模拟器装不了。请换 arm64 版本 APK" ;;
      *offline*|*'no online'*|*DeviceNotFound*|*ServerUnreachable*)
        reason="设备未能自动启动。终端执行 mwine vm start <avd> 后重试" ;;
      *INSUFFICIENT_STORAGE*)
        reason="设备存储不足。卸载部分应用后重试" ;;
    esac
    notify "❌ ${{name}}：${{reason}}" "$f"
    echo "ERROR 安装 ${{name}} 失败：${{reason}}" > "$PROG"
    break
  }}
  echo "S3 IND" > "$PROG"
  notify "已安装 ${{name}}，正在生成应用图标…" "$f"
  if app=$( {{ "$MWINE" app create --apk "$f" --no-install --print-path; }} 2>>"$LOG" | tail -n 1 ); then
    ok=$((ok+1)); last_app="$app"
    echo "created $app"
    notify "✓ ${{name}} 已就绪（${{app/#$HOME/\\~}}），启动中…" "$f"
  else
    fail=$((fail+1))
    notify "❌ ${{name}} 安装成功但生成 .app 失败（详见 mwine-installer.log）" "$f"
  fi
done

if [ "$fail" -eq 0 ] && [ "$ok" -gt 0 ]; then
  echo "DONE 安装完成：成功 ${{ok}} 个应用" > "$PROG"
  notify "✓ 本次共安装 ${{ok}} 个应用" "完成"
  [ -n "$last_app" ] && open "$last_app"
fi
( sleep 4; rm -f "$PROG" ) &
"#,
        exe = exe.display(),
    );

    let sh_path = contents.join("Resources/install.sh");
    std::fs::write(&sh_path, install_sh)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut perms = std::fs::metadata(&sh_path)?.permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&sh_path, perms)?;
    }

    // 2.5 进度窗口 v6：加载动画版——Q 版 Android 机器人蹦跳 + 步骤文案轮播
    // （"美好的事情即将发生"式），完成撒彩纸，失败机器人变蔫+原因。
    // 进度条已移除：唯一有真实进度的是安装包上传（以 MB 文本呈现）。
    let progress_swift = r#"import Cocoa

// Q 版 Android 机器人（矢量绘制）：弹跳、眨眼；sad 模式变蔫
final class RobotView: NSView {
    var root = CALayer()
    var headLayer: CAShapeLayer!
    var bodyLayer: CAShapeLayer!
    var eyeL = CALayer(), eyeR = CALayer()
    var antennaL = CAShapeLayer(), antennaR = CAShapeLayer()
    var armL: CAShapeLayer!
    var armR: CAShapeLayer!
    var legL: CAShapeLayer!
    var legR: CAShapeLayer!
    var mouth: CAShapeLayer?
    var sad = false

    private let green = NSColor(srgbRed: 0.26, green: 0.87, blue: 0.50, alpha: 1)
    private let gray = NSColor(srgbRed: 0.66, green: 0.70, blue: 0.68, alpha: 1)

    // quartz 坐标（y=0 在底部），从下往上：腿→身体→胳膊→头→眼→天线
    func build() {
        wantsLayer = true
        layer?.addSublayer(root)
        root.frame = bounds
        let g = sad ? gray : green

        func shape(_ path: CGPath, _ color: NSColor) -> CAShapeLayer {
            let l = CAShapeLayer()
            l.path = path
            l.fillColor = color.cgColor
            root.addSublayer(l)
            return l
        }
        func rounded(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat, _ r: CGFloat) -> CGPath {
            CGPath(roundedRect: NSRect(x: x, y: y, width: w, height: h),
                   cornerWidth: r, cornerHeight: r, transform: nil)
        }
        func line(_ x1: CGFloat, _ y1: CGFloat, _ x2: CGFloat, _ y2: CGFloat) -> CGPath {
            let p = CGMutablePath()
            p.move(to: CGPoint(x: x1, y: y1))
            p.addLine(to: CGPoint(x: x2, y: y2))
            return p
        }

        // 腿（y 2..16）
        legL = shape(rounded(42, 2, 8, 16, 4), g)
        legR = shape(rounded(70, 2, 8, 16, 4), g)
        // 身体（y 14..56，圆顶矩形）
        bodyLayer = shape(rounded(26, 14, 68, 42, 12), g)
        // 胳膊（两侧胶囊，y 24..46）
        armL = shape(rounded(12, 24, 10, 22, 5), g)
        armR = shape(rounded(98, 24, 10, 22, 5), g)
        // 头（y 56..98，上圆下方）
        headLayer = shape(rounded(20, 56, 80, 42, 18), g)
        // 眼睛（头部上半，y≈78）
        eyeL.backgroundColor = NSColor.white.cgColor
        eyeL.frame = NSRect(x: 42, y: 76, width: 10, height: 10)
        eyeL.cornerRadius = 5
        root.addSublayer(eyeL)
        eyeR.backgroundColor = NSColor.white.cgColor
        eyeR.frame = NSRect(x: 68, y: 76, width: 10, height: 10)
        eyeR.cornerRadius = 5
        root.addSublayer(eyeR)
        // 天线（从头顶向上伸展）
        antennaL.strokeColor = g.cgColor
        antennaL.lineWidth = 4
        antennaL.lineCap = .round
        antennaL.fillColor = NSColor.clear.cgColor
        antennaL.path = line(44, 96, 32, 112)
        root.addSublayer(antennaL)
        antennaR.strokeColor = g.cgColor
        antennaR.lineWidth = 4
        antennaR.lineCap = .round
        antennaR.fillColor = NSColor.clear.cgColor
        antennaR.path = line(76, 96, 88, 112)
        root.addSublayer(antennaR)

        startBounce()
        startBlink()
    }

    func startBounce() {
        let b = CAKeyframeAnimation(keyPath: "transform.translation.y")
        b.values = [0, -14, 0]
        b.keyTimes = [0, 0.4, 1]
        b.duration = 0.6
        b.repeatCount = .infinity
        b.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
        root.add(b, forKey: "bounce")
        // 胳膊随弹跳摆动
        let aw = CAKeyframeAnimation(keyPath: "transform.rotation.z")
        aw.values = [-0.18, 0.22, -0.18]
        aw.keyTimes = [0, 0.4, 1]
        aw.duration = 0.6
        aw.repeatCount = .infinity
        armL.add(aw, forKey: "waveL")
        armR.add(aw, forKey: "waveR")
    }

    func startBlink() {
        let k = CAKeyframeAnimation(keyPath: "transform.scale.y")
        k.values = [1, 1, 0.06, 1, 1]
        k.keyTimes = [0, 0.86, 0.9, 0.94, 1]
        k.duration = 3.6
        k.repeatCount = .infinity
        eyeL.add(k, forKey: "blink")
        eyeR.add(k, forKey: "blink")
    }

    func makeSad() {
        sad = true
        root.removeAnimation(forKey: "bounce")
        armL.removeAnimation(forKey: "waveL")
        armR.removeAnimation(forKey: "waveR")
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        root.transform = CATransform3DMakeRotation(0.08, 0, 0, 1)
        let g2 = gray.cgColor
        headLayer.fillColor = g2
        bodyLayer.fillColor = g2
        armL.fillColor = g2
        armR.fillColor = g2
        legL.fillColor = g2
        legR.fillColor = g2
        antennaL.strokeColor = g2
        antennaR.strokeColor = g2
        // 眼睛变 ><（缩成小点）+ 苦脸嘴
        eyeL.frame = NSRect(x: 43, y: 77, width: 7, height: 7)
        eyeR.frame = NSRect(x: 69, y: 77, width: 7, height: 7)
        let m = CAShapeLayer()
        let p = CGMutablePath()
        p.move(to: CGPoint(x: 46, y: 68))
        p.addQuadCurve(to: CGPoint(x: 74, y: 68), control: CGPoint(x: 60, y: 62))
        m.path = p
        m.strokeColor = NSColor.white.cgColor
        m.lineWidth = 3.5
        m.lineCap = .round
        m.fillColor = NSColor.clear.cgColor
        root.addSublayer(m)
        mouth = m
        CATransaction.commit()
    }

    func celebrate() {
        root.removeAnimation(forKey: "bounce")
        let j = CAKeyframeAnimation(keyPath: "transform.translation.y")
        j.values = [0, -30, 0, -18, 0]
        j.keyTimes = [0, 0.28, 0.55, 0.78, 1]
        j.duration = 0.9
        j.repeatCount = 3
        root.add(j, forKey: "jump")
    }
}

final class Delegate: NSObject, NSApplicationDelegate {
    let progPath: String
    var win: NSWindow!
    var header: NSTextField!
    var status: NSTextField!
    var sub: NSTextField!
    var robot: RobotView!
    var closeBtn: NSButton!
    var stepDots: [NSTextField] = []
    var confetti: CAEmitterLayer?
    var target = 0.0, shown = 0.0
    var indet = false
    var doneAt: Date?
    var failed = false
    var lastRead = ""
    var lastLine = ""
    let startedAt = Date()
    var stepStart = Date()
    var phaseStart = Date()
    var msgIdx = 0
    var currentStep = 1
    var vmStarting = false
    var dotStep = 0

    // 等待时的暖心轮播（致敬 Windows 安装器）
    let calmLines = [
        "美好的事情即将发生…",
        "正在为应用铺好红毯…",
        "深呼吸，马上就好…",
        "正在把应用安置到新家…",
        "耐心是最好的等待…",
    ]

    init(path: String) { self.progPath = path; super.init() }

    func applicationDidFinishLaunching(_ note: Notification) {
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 420, height: 280),
                       styleMask: [.titled, .fullSizeContentView],
                       backing: .buffered, defer: false)
        win.title = "Houttuynia 安装器"
        win.titlebarAppearsTransparent = true
        win.titleVisibility = .hidden
        win.level = .floating
        win.isMovableByWindowBackground = true
        win.backgroundColor = .clear

        let fx = NSVisualEffectView(frame: NSRect(x: 0, y: 0, width: 420, height: 280))
        fx.material = .popover
        fx.state = .active
        fx.wantsLayer = true
        fx.layer?.cornerRadius = 14
        fx.layer?.masksToBounds = true
        win.contentView = fx

        robot = RobotView(frame: NSRect(x: 140, y: 96, width: 120, height: 120))
        robot.build()
        fx.addSubview(robot)

        header = NSTextField(labelWithString: "准备安装…")
        header.frame = NSRect(x: 20, y: 232, width: 380, height: 22)
        header.font = NSFont.systemFont(ofSize: 14, weight: .semibold)
        header.alignment = .center
        fx.addSubview(header)

        status = NSTextField(labelWithString: "马上开始…")
        status.frame = NSRect(x: 20, y: 66, width: 380, height: 18)
        status.font = NSFont.systemFont(ofSize: 13, weight: .medium)
        status.alignment = .center
        fx.addSubview(status)

        sub = NSTextField(labelWithString: "")
        sub.frame = NSRect(x: 20, y: 46, width: 380, height: 16)
        sub.font = NSFont.systemFont(ofSize: 11)
        sub.alignment = .center
        sub.textColor = .secondaryLabelColor
        fx.addSubview(sub)

        // 迷你步骤条：传送 · 安装 · 创建（小圆点+字）
        let names = ["传送", "安装", "创建"]
        for (i, n) in names.enumerated() {
            let d = NSTextField(labelWithString: "●  \(n)")
            d.frame = NSRect(x: 76 + CGFloat(i) * 96, y: 18, width: 90, height: 14)
            d.font = NSFont.systemFont(ofSize: 10)
            d.alignment = .center
            d.textColor = .tertiaryLabelColor
            fx.addSubview(d)
            stepDots.append(d)
        }

        closeBtn = NSButton(title: "关闭", target: self, action: #selector(closeClicked))
        closeBtn.frame = NSRect(x: 168, y: 118, width: 84, height: 32)
        closeBtn.bezelStyle = .rounded
        closeBtn.keyEquivalent = "\r"
        closeBtn.isHidden = true
        fx.addSubview(closeBtn)

        win.center()
        win.orderFrontRegardless()
        app.activate(ignoringOtherApps: true)
        setDot(1)
    }

    @objc func closeClicked() { NSApp.terminate(nil) }

    func setDot(_ n: Int) {
        dotStep = n
        let names = ["传送", "安装", "创建"]
        for i in 0..<3 {
            let idx = i + 1
            if failed && idx == n {
                stepDots[i].stringValue = "✕  \(names[i])"
                stepDots[i].textColor = .systemRed
            } else if idx < n || doneAt != nil {
                stepDots[i].stringValue = "✓  \(names[i])"
                stepDots[i].textColor = .systemGreen
            } else if idx == n {
                stepDots[i].stringValue = "◉  \(names[i])"
                stepDots[i].textColor = .controlAccentColor
            } else {
                stepDots[i].stringValue = "●  \(names[i])"
                stepDots[i].textColor = .tertiaryLabelColor
            }
        }
    }

    func showError(_ reason: String) {
        failed = true
        robot.makeSad()
        header.stringValue = "哎呀，安装失败了"
        status.stringValue = String(reason.prefix(90))
        status.textColor = .systemRed
        status.font = NSFont.systemFont(ofSize: 11)
        sub.stringValue = "修复上面的问题后，重新双击 APK 即可再试"
        closeBtn.isHidden = false
        closeBtn.frame.origin.y = 118
        setDot(currentStep)
    }

    func celebrate() {
        robot.celebrate()
        header.stringValue = "🎉 装好啦！"
        // 彩纸（中心向上喷洒）
        let e = CAEmitterLayer()
        e.emitterPosition = CGPoint(x: 210, y: 60)
        e.emitterShape = .point
        e.renderMode = .additive
        let colors: [NSColor] = [.systemPink, .systemYellow, .systemGreen, .systemBlue, .systemOrange, .systemPurple]
        var cells: [CAEmitterCell] = []
        for c in colors {
            let cell = CAEmitterCell()
            cell.birthRate = 14
            cell.lifetime = 2.2
            cell.velocity = 150
            cell.velocityRange = 90
            cell.emissionRange = -1.6
            cell.emissionLongitude = -.pi / 2
            cell.yAcceleration = 260
            cell.spin = 6
            cell.spinRange = 10
            cell.scale = 0.5
            cell.scaleRange = 0.4
            cell.color = c.cgColor
            let img = NSImage(size: NSSize(width: 8, height: 8), flipped: false) { r in
                NSColor.white.setFill(); r.fill()
                return true
            }
            cell.contents = img.cgImage(forProposedRect: nil, context: nil, hints: nil)
            cells.append(cell)
        }
        e.emitterCells = cells
        win.contentView?.layer?.addSublayer(e)
        confetti = e
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { e.birthRate = 0 }
    }

    func tick() {
        if failed { return }
        if doneAt == nil {
            if let s = try? String(contentsOfFile: progPath, encoding: .utf8) {
                let trimmed = s.trimmingCharacters(in: .whitespacesAndNewlines)
                if trimmed != lastRead {
                    lastRead = trimmed
                    let last = trimmed.split(separator: "\n").last.map(String.init) ?? ""
                    if last.hasPrefix("DONE") {
                        if currentStep <= 3 { currentStep = 4; setDot(4) }
                        celebrate()
                        status.stringValue = ""
                        sub.stringValue = "窗口即将自动关闭"
                        doneAt = Date()
                    } else if last.hasPrefix("ERROR ") {
                        showError(String(last.dropFirst(6)))
                    } else if last.hasPrefix("APP ") {
                        header.stringValue = "正在安装「\(last.dropFirst(4))」"
                    } else if last.hasPrefix("S2 IND") {
                        if currentStep < 2 { currentStep = 2; phaseStart = Date(); stepStart = Date() }
                        setDot(2)
                        indet = true
                    } else if last.hasPrefix("S3 IND") {
                        if currentStep < 3 { currentStep = 3; phaseStart = Date(); stepStart = Date() }
                        setDot(3)
                        indet = true
                    } else if last.hasPrefix("VM IND") {
                        vmStarting = true
                        setDot(1)
                        indet = true
                    } else if last.hasPrefix("S1 ") {
                        vmStarting = false
                        if currentStep != 1 { currentStep = 1; phaseStart = Date(); stepStart = Date() }
                        setDot(1)
                        let body = last.dropFirst(3)
                        let parts = body.split(separator: " ", maxSplits: 1)
                        if parts.count == 2 {
                            status.stringValue = "正在把安装包送往虚拟机…"
                            sub.stringValue = String(parts[1])
                        }
                    }
                }
            }
        }
        let elapsed = Int(Date().timeIntervalSince(stepStart))
        if vmStarting && !failed && doneAt == nil {
            status.stringValue = currentStep == 1 ? "正在唤醒虚拟机…" : status.stringValue
            sub.stringValue = "已进行 \(elapsed) 秒 · 虚拟机首次冷启动最长约 2 分钟，之后秒级唤醒"
            if Date().timeIntervalSince(phaseStart) > 4 {
                phaseStart = Date()
                msgIdx = (msgIdx + 1) % calmLines.count
            }
        } else if currentStep == 1 && !failed && doneAt == nil {
            // 传送：sub 由协议携带 MB 信息（无需计时）
        } else if currentStep == 2 && !failed && doneAt == nil {
            status.stringValue = calmLines[min(msgIdx, calmLines.count - 1)]
            sub.stringValue = "已进行 \(elapsed) 秒 · 系统正在安装并优化应用"
            if Date().timeIntervalSince(phaseStart) > 4 {
                phaseStart = Date()
                msgIdx = (msgIdx + 1) % calmLines.count
            }
        } else if currentStep == 3 && !failed && doneAt == nil {
            status.stringValue = "快好了！正在创建应用图标…"
            sub.stringValue = ""
        }
        if let d = doneAt, Date().timeIntervalSince(d) > 2.8 { NSApp.terminate(nil) }
        if !win.isVisible { NSApp.terminate(nil) }
        if Date().timeIntervalSince(startedAt) > 900 { NSApp.terminate(nil) }
    }
}

let args = CommandLine.arguments
guard args.count >= 2 else { exit(2) }
let app = NSApplication.shared
let delegate = Delegate(path: args[1])
app.delegate = delegate
// 注意：不手动调用 applicationDidFinishLaunching——app.run() 会自动回调，
// 双重构建曾导致 UI 叠层 + 数组污染（步骤点永不更新）
let t = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { _ in delegate.tick() }
_ = t
app.run()
"#;
    let swift_path = contents.join("Resources/progress.swift");
    std::fs::write(&swift_path, progress_swift)?;
    let progress_bin = contents.join("Resources/progress-bin");
    let compiled = run_tool(
        std::process::Command::new("swiftc")
            .args(["-O", "-o"])
            .arg(&progress_bin)
            .arg(&swift_path)
            .arg("-framework")
            .arg("Cocoa"),
    );
    match compiled {
        Ok(_) => {
            std::fs::remove_file(&swift_path).ok();
            codesign_best_effort(&progress_bin);
        }
        Err(e) => {
            std::fs::remove_file(&swift_path).ok();
            println!("～ 进度窗口编译失败（{e}），降级为仅通知反馈");
        }
    }

        // 3. 覆盖 Info.plist
    // 3. 覆盖 Info.plist：文档类型声明 + UTI + 后台运行
    let plist = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>droplet</string>
  <key>CFBundleIdentifier</key><string>com.houttuynia.installer</string>
  <key>CFBundleName</key><string>Houttuynia Installer</string>
  <key>CFBundleDisplayName</key><string>mwine APK 安装器</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleIconFile</key><string>droplet</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <!-- 后台工具：不占 Dock -->
  <key>LSUIElement</key><true/>
  <key>UTImportedTypeDeclarations</key>
  <array>
    <dict>
      <key>UTTypeIdentifier</key><string>com.android.apk</string>
      <key>UTTypeDescription</key><string>Android 应用包</string>
      <key>UTTypeConformsTo</key>
      <array>
        <string>public.data</string>
        <string>public.archive.zip</string>
      </array>
      <key>UTTypeTagSpecification</key>
      <dict>
        <key>public.filename-extension</key><array><string>apk</string></array>
        <key>public.mime-type</key><array><string>application/vnd.android.package-archive</string></array>
      </dict>
    </dict>
  </array>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Android 应用包</string>
      <key>LSHandlerRank</key><string>Owner</string>
      <key>LSItemContentTypes</key><array><string>com.android.apk</string></array>
      <key>CFBundleTypeExtensions</key><array><string>apk</string></array>
      <key>LSIsAppleDefaultForType</key><true/>
    </dict>
  </array>
</dict>
</plist>
"#;
    std::fs::write(contents.join("Info.plist"), plist)?;

    // 4. ad-hoc 签名（绑定评估与 Gatekeeper 都需要）+ 注册 + 显式默认处理器
    if !codesign_best_effort(&app_dir) {
        println!("～ 安装器签名失败（macOS 可能拒绝绑定 .apk 双击）");
    }
    if !codesign_best_effort(&exe) {
        println!("～ mwine 二进制未签名（GUI 启动可能被 Gatekeeper 阻塞）");
        println!("  修复：codesign -f -s - \"{}\"", exe.display());
    }
    lsregister(&app_dir)?;
    // .apk 在用户机器上可能被 UC 浏览器等抢注为 com.ucweb.pkg（Finder 据此选默认 App），
    // 必须 UTI 全接管：自己的声明 + 抢注者 + 系统动态 UTI 三路都设
    for uti in ["com.android.apk", "com.ucweb.pkg", "com.android.package-archive"] {
        if let Err(e) = set_default_handler(uti, "com.houttuynia.installer") {
            println!("～ 设置 {uti} 处理器失败: {e}");
        }
    }

    println!("✓ 已生成 {}", app_dir.display());
    println!("  之后在 Finder 双击任意 .apk：自动安装 -> 生成应用 .app -> 自动启动");
    Ok(())
}

/// ad-hoc 签名（幂等）：返回签名后是否通过校验。
#[cfg(target_os = "macos")]
fn codesign_best_effort(path: &Path) -> bool {
    let sign = std::process::Command::new("codesign")
        .args(["-f", "-s", "-"])
        .arg(path)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();
    let verified = std::process::Command::new("codesign")
        .arg("--verify")
        .arg(path)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    sign.is_ok() && verified
}

#[cfg(not(target_os = "macos"))]
fn codesign_best_effort(_path: &Path) -> bool {
    true
}

/// 经 CoreServices FFI 显式写入某 UTI 的默认处理器
/// （评估器对新生成 app 的自动绑定不可靠；macOS 26 实测必须显式设置）。
#[cfg(target_os = "macos")]
fn set_default_handler(uti: &str, bundle_id: &str) -> Result<()> {
    use std::os::raw::{c_char, c_int};

    type CFStringRef = *const std::ffi::c_void;

    #[link(name = "CoreFoundation", kind = "framework")]
    unsafe extern "C" {
        fn CFStringCreateWithCString(
            alloc: *const std::ffi::c_void,
            c_str: *const c_char,
            encoding: u32,
        ) -> CFStringRef;
    }
    #[link(name = "CoreServices", kind = "framework")]
    unsafe extern "C" {
        fn LSSetDefaultRoleHandlerForContentType(
            uti: CFStringRef,
            roles: u32,
            handler: CFStringRef,
        ) -> c_int;
    }

    const K_CF_STRING_ENCODING_UTF8: u32 = 0x0800_0100;
    // kLSRolesAll = (UInt32)-1
    const LS_ROLES_ALL: u32 = u32::MAX;

    unsafe {
        // CString 保证 NUL 结尾（&str 直传会读到越界内存，得到垃圾 CFString）
        let c_uti = std::ffi::CString::new(uti).map_err(|_| anyhow!("UTI 含 NUL"))?;
        let c_bid = std::ffi::CString::new(bundle_id).map_err(|_| anyhow!("bundle id 含 NUL"))?;
        let uti_cf = CFStringCreateWithCString(
            std::ptr::null(),
            c_uti.as_ptr(),
            K_CF_STRING_ENCODING_UTF8,
        );
        let bid_cf = CFStringCreateWithCString(
            std::ptr::null(),
            c_bid.as_ptr(),
            K_CF_STRING_ENCODING_UTF8,
        );
        if uti_cf.is_null() || bid_cf.is_null() {
            return Err(anyhow!("CFString 创建失败"));
        }
        let status = LSSetDefaultRoleHandlerForContentType(uti_cf, LS_ROLES_ALL, bid_cf);
        if status != 0 {
            return Err(anyhow!("LSSetDefaultRoleHandlerForContentType -> {status}"));
        }
    }
    Ok(())
}

#[cfg(not(target_os = "macos"))]
fn set_default_handler(_uti: &str, _bundle_id: &str) -> Result<()> {
    Ok(())
}

// ---------------- 资源监护 ----------------

/// 模拟器 qemu 进程 pid（精确匹配 Android SDK emulator 二进制路径，
/// 绝不误伤 libvirt 等其他 qemu）
fn emulator_pid() -> Option<u32> {
    let out = std::process::Command::new("pgrep")
        .args(["-f", "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless"])
        .output()
        .ok()?;
    String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .filter_map(|p| p.parse().ok())
        .next()
}

/// 进程累计 CPU 时间（秒）
fn proc_cputime(pid: u32) -> Option<f64> {
    let out = std::process::Command::new("ps")
        .args(["-p", &pid.to_string(), "-o", "cputime="])
        .output()
        .ok()?;
    parse_cputime(String::from_utf8_lossy(&out.stdout).trim())
}

fn parse_cputime(text: &str) -> Option<f64> {
    // 格式 "MM:SS.ss" 或 "HH:MM:SS.ss"
    let parts: Vec<f64> = text
        .split(':')
        .filter_map(|p| p.trim().parse().ok())
        .collect();
    match parts.len() {
        2 => Some(parts[0] * 60.0 + parts[1]),
        3 => Some(parts[0] * 3600.0 + parts[1] * 60.0 + parts[2]),
        _ => None,
    }
}

fn proc_rss_mb(pid: u32) -> Option<f64> {
    let out = std::process::Command::new("ps")
        .args(["-p", &pid.to_string(), "-o", "rss="])
        .output()
        .ok()?;
    String::from_utf8_lossy(&out.stdout)
        .trim()
        .parse::<f64>()
        .ok()
        .map(|kb| kb / 1024.0)
}

fn running_window_count() -> usize {
    let mut n = 0;
    for pat in ["target/debug/mwine run", "Applications/Mwine/"] {
        if let Ok(out) = std::process::Command::new("pgrep").args(["-f", pat]).output() {
            n += String::from_utf8_lossy(&out.stdout)
                .split_whitespace()
                .count();
        }
    }
    n
}

fn monitor_notify(body: &str) {
    let body = body.replace('\'', "'").replace('"', "\\\"");
    let _ = std::process::Command::new("osascript")
        .arg("-e")
        .arg(format!(
            "display notification \"{body}\" with title \"mwine 资源监护\""
        ))
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();
}

#[allow(clippy::too_many_arguments)]
fn cmd_monitor(cpu_alert: f64, mem_alert_mb: u64, limit_cpu: f64, idle_stop_min: u64) -> Result<()> {
    ensure_single_monitor_instance()?;
    println!("mwine 资源监护启动（Ctrl-C 退出）");
    println!(
        "  提醒阈值：CPU {cpu_alert}% / 内存 {mem_alert_mb}MB；限流：{}；闲置关停：{}",
        if limit_cpu > 0.0 {
            format!("超限时节流至 {limit_cpu}%")
        } else {
            "关".to_string()
        },
        if idle_stop_min > 0 {
            format!("{idle_stop_min} 分钟")
        } else {
            "关".to_string()
        }
    );

    let mut last_cpu_time: Option<(f64, std::time::Instant)> = None;
    let mut cpu_now = 0f64;
    let mut high_since: Option<std::time::Instant> = None;
    let mut alerted_cpu = false;
    let mut alerted_mem = false;
    let mut throttling = false;
    let mut duty = 1.0f64;
    let mut throttle_started = std::time::Instant::now();
    let mut idle_since: Option<std::time::Instant> = None;
    // SIGINT 优雅退出：若正在节流要先 SIGCONT 恢复
    install_sigint_handler();

    while !MONITOR_STOP.load(std::sync::atomic::Ordering::SeqCst) {
        std::thread::sleep(Duration::from_millis(1000));
        let Some(pid) = emulator_pid() else {
            // 模拟器不在：清空状态，闲置计时也作废
            last_cpu_time = None;
            high_since = None;
            throttling = false;
            alerted_cpu = false;
            alerted_mem = false;
            idle_since = None;
            continue;
        };

        // 瞬时 CPU：cputime 差分
        if let Some(t) = proc_cputime(pid) {
            let now = std::time::Instant::now();
            if let Some((pt, pat)) = last_cpu_time {
                let dt = pat.elapsed().as_secs_f64().max(0.001);
                cpu_now = ((t - pt) / dt * 100.0).max(0.0);
            }
            last_cpu_time = Some((t, now));
        }
        let rss = proc_rss_mb(pid).unwrap_or(0.0);

        // --- CPU 异常提醒（持续 30s 超阈值才报，避免抖动）---
        if cpu_now > cpu_alert {
            let since = *high_since.get_or_insert_with(std::time::Instant::now);
            if since.elapsed() >= Duration::from_secs(30) && !alerted_cpu {
                alerted_cpu = true;
                monitor_notify(&format!(
                    "虚拟机 CPU 持续 {cpu_now:.0}%（阈值 {cpu_alert:.0}%）{}。可关闭不用的窗口，或 mwine kill --vm 全部关停",
                    if limit_cpu > 0.0 { "，已自动限流" } else { "" }
                ));
            }
        } else {
            high_since = None;
            alerted_cpu = false;
        }

        // --- 内存提醒（10 分钟冷却一次）---
        if rss > mem_alert_mb as f64 && !alerted_mem {
            alerted_mem = true;
            monitor_notify(&format!(
                "虚拟机内存 {rss:.1}GB（阈值 {mem_alert_mb}MB）。建议重启虚拟机：mwine kill --vm 后再打开应用（快照秒级唤醒）"
            ));
        } else if rss < mem_alert_mb as f64 * 0.8 {
            alerted_mem = false;
        }

        // --- 限流：已移除。SIGSTOP/SIGCONT 冻结多线程 qemu 会触发其内部
        // 看门狗 abort（实测 21:0x 连续 4 次崩溃）与永久 T 态冻结，弊大于利；
        // 异常处置改为：提醒 + 建议关窗口/关停虚拟机（idle-stop）。
        let _ = (limit_cpu, &mut throttling, &mut duty, &mut throttle_started);

        // --- 闲置关停（无窗口 + 低 CPU 持续 N 分钟）---
        if idle_stop_min > 0 {
            let idle = running_window_count() == 0 && cpu_now < 30.0;
            if idle {
                let since = *idle_since.get_or_insert_with(std::time::Instant::now);
                if since.elapsed() >= Duration::from_secs(idle_stop_min * 60) {
                    monitor_notify("虚拟机已闲置，自动关停（重新使用时会自动唤醒）");
                    match mwine_supervisor::emu_console_kill("emulator-5554") {
                        Ok(_) => println!("[mwine] 闲置关停虚拟机 ✓"),
                        Err(e) => println!("[mwine] 闲置关停失败: {e}"),
                    }
                    idle_since = None;
                }
            } else {
                idle_since = None;
            }
        }
    }
    // 退出前恢复可能的 SIGSTOP
    if let Some(pid) = emulator_pid() {
        unsafe { libc::kill(pid as i32, libc::SIGCONT) };
    }
    println!("监护退出");
    Ok(())
}

#[cfg(target_os = "macos")]
static MONITOR_STOP: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

#[cfg(target_os = "macos")]
extern "C" fn monitor_sigint(_: i32) {
    // 信号安全：仅原子置位
    MONITOR_STOP.store(true, std::sync::atomic::Ordering::SeqCst);
}

#[cfg(target_os = "macos")]
fn install_sigint_handler() {
    // SIGINT 与 SIGTERM 都置停止标志；退出兜底恢复 SIGCONT
    // （曾因只接管 SIGINT：monitor 被 kill 后模拟器永久冻结在 T 状态）
    unsafe {
        libc::signal(libc::SIGINT, monitor_sigint as usize);
        libc::signal(libc::SIGTERM, monitor_sigint as usize);
    }
}

#[cfg(not(target_os = "macos"))]
fn install_sigint_handler() {}

// ---------------- monitor 开机自启（LaunchAgent，用户自选） ----------------

const MONITOR_AGENT_LABEL: &str = "wine.mwine.monitor";

fn monitor_agent_plist() -> PathBuf {
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_default();
    home.join("Library/LaunchAgents/wine.mwine.monitor.plist")
}

#[allow(clippy::too_many_arguments)]
fn install_monitor_agent(cpu: f64, mem_mb: u64, limit: f64, idle_stop: u64) -> Result<()> {
    let exe = mwine_bin();
    let plist = monitor_agent_plist();
    std::fs::create_dir_all(plist.parent().unwrap())?;
    let args = format!(
        "<string>--cpu</string><string>{cpu}</string>\
         <string>--mem-mb</string><string>{mem_mb}</string>\
         <string>--limit</string><string>{limit}</string>\
         <string>--idle-stop</string><string>{idle_stop}</string>"
    );
    let xml = format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n  <key>Label</key><string>{MONITOR_AGENT_LABEL}</string>\n  <key>ProgramArguments</key>\n  <array>\n    <string>{}</string>\n    <string>monitor</string>\n    {}\n  </array>\n  <key>RunAtLoad</key><true/>\n  <key>KeepAlive</key><true/>\n</dict>\n</plist>\n",
        exe.display(),
        args
    );
    std::fs::write(&plist, xml)?;
    plutil_ok(&plist)?;
    // 卸载旧实例（若有）再加载，避免双开
    let _ = std::process::Command::new("launchctl")
        .args(["unload", "-w"])
        .arg(&plist)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();
    let out = std::process::Command::new("launchctl")
        .args(["load", "-w"])
        .arg(&plist)
        .output()
        .context("launchctl load 失败")?;
    if !out.status.success() {
        return Err(anyhow!(
            "launchctl load 失败：{}",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    println!("✓ 开机自启已安装并启动（{}）", plist.display());
    println!("  参数：CPU {cpu}% / 内存 {mem_mb}MB / 限流 {limit} / 闲置关停 {idle_stop} 分钟");
    println!("  卸载：mwine monitor --remove-auto-start");
    Ok(())
}

fn uninstall_monitor_agent() -> Result<()> {
    let plist = monitor_agent_plist();
    if !plist.exists() {
        println!("（未安装开机自启）");
        return Ok(());
    }
    let _ = std::process::Command::new("launchctl")
        .args(["unload", "-w"])
        .arg(&plist)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();
    std::fs::remove_file(&plist).context("删除 plist 失败")?;
    println!("✓ 开机自启已卸载");
    Ok(())
}

fn plutil_ok(plist: &Path) -> Result<()> {
    let out = std::process::Command::new("plutil")
        .arg("-lint")
        .arg(plist)
        .output()
        .context("plutil 执行失败")?;
    if !out.status.success() {
        return Err(anyhow!(
            "plist 无效：{}",
            String::from_utf8_lossy(&out.stdout).trim()
        ));
    }
    Ok(())
}

/// 单实例锁：已有一个存活的 monitor 时拒绝启动（LaunchAgent 与手动
/// 双开会重复限流/重复通知）。锁文件持有进程退出自动失效（存 pid 校验）。
fn ensure_single_monitor_instance() -> Result<()> {
    let lock = std::env::temp_dir().join("mwine-monitor.lock");
    if let Ok(text) = std::fs::read_to_string(&lock) {
        if let Ok(pid) = text.trim().parse::<u32>() {
            if pid != std::process::id()
                && std::process::Command::new("ps")
                    .args(["-p", &pid.to_string(), "-o", "comm="])
                    .output()
                    .map(|o| {
                        let comm = String::from_utf8_lossy(&o.stdout).trim().to_string();
                        comm.contains("mwine")
                    })
                    .unwrap_or(false)
            {
                return Err(anyhow!(
                    "monitor 已在运行（pid {pid}）；如需调整参数先结束旧实例或 mwine kill"
                ));
            }
        }
    }
    std::fs::write(&lock, std::process::id().to_string()).ok();
    Ok(())
}

/// 实时资源监视窗口（菜单「查看资源占用」打开）：共享一份放 Mwine 目录，
/// 缺失时 swiftc 编译。CPU cputime 差分 + 内存 + 窗口数 + 自启状态。
/// 运行时编译的 Swift 辅助二进制版本：源码更新后自增，触发重编，
/// 避免 ~/Applications/Houttuynia 下的旧二进制一直被复用。
const SWIFT_HELPER_VERSION: &str = "2";

/// bin 存在且版本一致时返回 true（跳过重编）。
fn swift_bin_fresh(bin: &std::path::Path) -> bool {
    if !bin.is_file() {
        return false;
    }
    let marker = bin.with_extension("version");
    std::fs::read_to_string(&marker).ok().as_deref() == Some(SWIFT_HELPER_VERSION)
}

fn mark_swift_bin_version(bin: &std::path::Path) {
    let _ = std::fs::write(bin.with_extension("version"), SWIFT_HELPER_VERSION);
}

fn ensure_reswatch_bin(wine_dir: &Path) {
    let bin = wine_dir.join("reswatch-bin");
    if swift_bin_fresh(&bin) {
        return;
    }
    let src = wine_dir.join(".reswatch.swift");
    let swift = r#"import Cocoa

// 单窗单列双区：上半 CPU（蓝，>500% 变红）/ 分隔线 / 下半内存（紫），
// 同一时间轴，纯真实数据。
final class LineChart: NSView {
    var samples: [CGFloat] = []
    let capacity: Int
    var strokeC: NSColor
    var maxValue: CGFloat
    /// 填充朝向共享轴侧：false=向下（上区），true=向上（下区，轴在其顶）
    var fillUp = false

    override var isFlipped: Bool { true }

    init(frame: NSRect, capacity: Int = 60, max: CGFloat, color: NSColor) {
        self.capacity = capacity
        self.maxValue = max
        self.strokeC = color
        super.init(frame: frame)
    }
    required init?(coder: NSCoder) { fatalError() }

    func push(_ v: CGFloat) {
        samples.append(min(max(v / maxValue, 0), 1))
        if samples.count > capacity { samples.removeFirst() }
        needsDisplay = true
    }

    override func draw(_ r: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext, samples.count > 1 else { return }
        let inset: CGFloat = 4
        let w = bounds.width - inset * 2, h = bounds.height - inset * 2
        ctx.setStrokeColor(NSColor.separatorColor.withAlphaComponent(0.3).cgColor)
        ctx.setLineWidth(0.5)
        for f in [0.5] {
            let y = inset + h * (1 - CGFloat(f))
            ctx.move(to: CGPoint(x: inset, y: y))
            ctx.addLine(to: CGPoint(x: inset + w, y: y))
            ctx.strokePath()
        }
        let n = samples.count
        let step = w / CGFloat(capacity - 1)
        let x0 = inset + w - CGFloat(n - 1) * step
        var path = CGMutablePath()
        var prev = CGPoint(x: x0, y: inset + h * (1 - samples[0]))
        path.move(to: prev)
        for i in 1..<n {
            let p = CGPoint(x: x0 + CGFloat(i) * step, y: inset + h * (1 - samples[i]))
            let mid = CGPoint(x: (prev.x + p.x) / 2, y: (prev.y + p.y) / 2)
            path.addQuadCurve(to: mid, control: prev)
            prev = p
        }
        path.addLine(to: prev)
        let fill = path.mutableCopy()!
        let baseY = fillUp ? inset : bounds.height - inset
        fill.addLine(to: CGPoint(x: prev.x, y: baseY))
        fill.addLine(to: CGPoint(x: x0, y: baseY))
        fill.closeSubpath()
        ctx.setFillColor(strokeC.withAlphaComponent(0.15).cgColor)
        ctx.addPath(fill)
        ctx.fillPath()
        ctx.setStrokeColor(strokeC.cgColor)
        ctx.setLineWidth(1.6)
        ctx.setLineJoin(.round); ctx.setLineCap(.round)
        ctx.addPath(path)
        ctx.strokePath()
        ctx.setFillColor(strokeC.cgColor)
        ctx.fillEllipse(in: CGRect(x: prev.x - 2.5, y: prev.y - 2.5, width: 5, height: 5))
    }
}

final class Pill: NSView {
    let label = NSTextField(labelWithString: "")
    init(_ text: String, _ color: NSColor) {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = 10
        layer?.backgroundColor = color.withAlphaComponent(0.12).cgColor
        label.stringValue = text
        label.font = .systemFont(ofSize: 11, weight: .medium)
        label.textColor = color
        label.sizeToFit()
        frame = NSRect(x: 0, y: 0, width: label.bounds.width + 20, height: 20)
        label.frame.origin = CGPoint(x: 10, y: 2)
        addSubview(label)
    }
    func update(_ text: String, _ color: NSColor) {
        label.stringValue = text
        label.textColor = color
        layer?.backgroundColor = color.withAlphaComponent(0.12).cgColor
        label.sizeToFit()
        setFrameSize(NSSize(width: label.bounds.width + 20, height: 20))
    }
    required init?(coder: NSCoder) { fatalError() }
}

final class Delegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var win: NSWindow!
    var cpuChart: LineChart!
    var memChart: LineChart!
    var cpuNum = NSTextField(labelWithString: "…")
    var memNum = NSTextField(labelWithString: "…")
    var winPill: Pill!
    var guardPill: Pill!
    var lastCpu: Double?
    var lastAt: Date?
    let green = NSColor(srgbRed: 0.2, green: 0.78, blue: 0.35, alpha: 1)

    func applicationDidFinishLaunching(_ n: Notification) {
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 360, height: 252),
                       styleMask: [.titled, .closable], backing: .buffered, defer: false)
        win.title = "资源占用"
        let content = win.contentView!

        func row(_ y: CGFloat, title: String, num: NSTextField) {
            let t = NSTextField(labelWithString: title)
            t.font = .systemFont(ofSize: 11, weight: .semibold)
            t.textColor = .secondaryLabelColor
            t.frame = NSRect(x: 20, y: y, width: 60, height: 14)
            content.addSubview(t)
            num.font = .monospacedDigitSystemFont(ofSize: 15, weight: .bold)
            num.alignment = .right
            num.frame = NSRect(x: 240, y: y - 3, width: 100, height: 20)
            content.addSubview(num)
        }
        row(232, title: "CPU", num: cpuNum)
        // 共用 x 轴：两区同宽/同容量/同刻度推进，分隔线即共享时间轴——
        // CPU 尖峰与内存变化在同一横向位置对齐
        cpuChart = LineChart(frame: NSRect(x: 20, y: 148, width: 320, height: 80),
                             max: 800, color: .controlAccentColor)
        content.addSubview(cpuChart)

        let sep = NSView(frame: NSRect(x: 20, y: 141, width: 320, height: 1))
        sep.wantsLayer = true
        sep.layer?.backgroundColor = NSColor.separatorColor.cgColor
        content.addSubview(sep)

        memChart = LineChart(frame: NSRect(x: 20, y: 55, width: 320, height: 80),
                             max: 8192,
                             color: NSColor(srgbRed: 0.55, green: 0.4, blue: 0.9, alpha: 1))
        memChart.fillUp = true  // 轴在图表顶（共享轴），填充向上
        content.addSubview(memChart)
        row(37, title: "内存", num: memNum)

        winPill = Pill("窗口 0", .secondaryLabelColor)
        winPill.frame.origin = CGPoint(x: 20, y: 8)
        content.addSubview(winPill)
        guardPill = Pill("监护 开", green)
        guardPill.frame.origin = CGPoint(x: 20 + winPill.bounds.width + 8, y: 8)
        content.addSubview(guardPill)

        win.delegate = self  // 关窗即退出（否则进程残留+单实例拦截=再点菜单无窗口）
        win.center()
        win.orderFrontRegardless()
        app.activate(ignoringOtherApps: true)
    }

    func windowWillClose(_ n: Notification) { NSApp.terminate(nil) }

    @objc func tick() {
        let pid = shell("/usr/bin/pgrep", ["-f", "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless"])
            .split(whereSeparator: { $0 == " " || $0 == "\n" }).first.flatMap { Int($0) }
        guard let pid else {
            cpuNum.stringValue = "—"; memNum.stringValue = "—"
            winPill.update("虚拟机未运行", .secondaryLabelColor)
            return
        }
        var cpuNow: CGFloat = 0
        let now = Date()
        let t = cpuTime(pid)
        if let t, let lt = lastCpu, let lat = lastAt {
            let dt = now.timeIntervalSince(lat)
            cpuNow = CGFloat(max(0, (t - lt) / dt * 100))
            cpuNum.stringValue = String(format: "%.0f%%", cpuNow)
        }
        lastCpu = t; lastAt = now
        cpuChart.strokeC = cpuNow > 500 ? .systemRed : .controlAccentColor
        cpuChart.push(cpuNow)

        var memMB: CGFloat = 0
        let rss = shell("/bin/ps", ["-p", String(pid), "-o", "rss="]).trimmingCharacters(in: .whitespacesAndNewlines)
        if let kb = Double(rss) {
            memMB = CGFloat(kb / 1024)
            memNum.stringValue = String(format: "%.1f GB", kb / 1024 / 1024)
        }
        memChart.push(memMB)

        let n1 = shell("/usr/bin/pgrep", ["-f", "target/debug/mwine run"]).split(whereSeparator: { $0 == " " || $0 == "\n" }).count
        let n2 = shell("/usr/bin/pgrep", ["-f", "Applications/Mwine/"]).split(whereSeparator: { $0 == " " || $0 == "\n" }).count
        winPill.update("窗口 \(n1 + n2)", .secondaryLabelColor)
        let home = FileManager.default.homeDirectoryForCurrentUser
        let on = FileManager.default.fileExists(atPath: home.appendingPathComponent("Library/LaunchAgents/wine.mwine.monitor.plist").path)
        guardPill.update("监护 \(on ? "开" : "关")", on ? green : .secondaryLabelColor)
    }

    func shell(_ b: String, _ a: [String]) -> String {
        guard let d = Process().run(bin: b, args: a) else { return "" }
        return String(data: d, encoding: .utf8) ?? ""
    }
    func cpuTime(_ pid: Int) -> Double? {
        let s = shell("/bin/ps", ["-p", String(pid), "-o", "cputime="]).trimmingCharacters(in: .whitespacesAndNewlines)
        let p = s.split(separator: ":").compactMap { Double($0) }
        switch p.count {
        case 2: return p[0] * 60 + p[1]
        case 3: return p[0] * 3600 + p[1] * 60 + p[2]
        default: return nil
        }
    }
}

func shellPids(_ bin: String, _ args: [String]) -> [pid_t] {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: bin)
    p.arguments = args
    p.standardOutput = Pipe(); p.standardError = Pipe()
    guard (try? p.run()) != nil else { return [] }
    p.waitUntilExit()
    let out = String(data: (p.standardOutput as? Pipe)?.fileHandleForReading.readDataToEndOfFile() ?? Data(),
                     encoding: .utf8) ?? ""
    return out.split(whereSeparator: { $0 == "\n" || $0 == " " }).compactMap { pid_t($0) }
}

extension Process {
    func run(bin: String, args: [String]) -> Data? {
        executableURL = URL(fileURLWithPath: bin)
        arguments = args
        standardOutput = Pipe(); standardError = Pipe()
        do { try run(); waitUntilExit() } catch { return nil }
        return (standardOutput as? Pipe)?.fileHandleForReading.readDataToEndOfFile()
    }
}

// 单实例：已有 reswatch 在跑则本实例静默退出（曾每次点菜单累积一个进程，
// 实测堆了 8 个各持 4 个/秒的子进程轮询）
let others = shellPids("/usr/bin/pgrep", ["-x", "reswatch-bin"])
    .filter { $0 != getpid() }
if !others.isEmpty { exit(0) }

let app = NSApplication.shared
let d = Delegate()
app.delegate = d
d.applicationDidFinishLaunching(Notification(name: .init("l")))
Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { _ in d.tick() }
// 生命周期：虚拟机消失 90s 后自动退出（分享/窗口关闭后不残留）
var goneSince: Date? = nil
Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { _ in
    let emu = shellPids("/usr/bin/pgrep", ["-f", "qemu-system-aarch64-headless"])
    if emu.isEmpty {
        if goneSince == nil { goneSince = Date() }
        if Date().timeIntervalSince(goneSince!) > 90 { NSApp.terminate(nil) }
    } else { goneSince = nil }
}
d.tick()
app.run()
"#;
    let _ = std::fs::write(&src, swift);
    let compiled = run_tool(
        std::process::Command::new("swiftc")
            .args(["-O", "-o"])
            .arg(&bin)
            .arg(&src)
            .arg("-framework")
            .arg("Cocoa"),
    );
    match compiled {
        Ok(_) => {
            std::fs::remove_file(&src).ok();
            mark_swift_bin_version(&bin);
            codesign_best_effort(&bin);
        }
        Err(e) => {
            std::fs::remove_file(&src).ok();
            println!("～ 资源监视窗口编译失败（{e}），菜单将退化为通知显示");
        }
    }
}


/// 窗口管理器（菜单「管理打开的窗口」）：共享一份放 Mwine 目录。
fn ensure_appmgr_bin(wine_dir: &Path) {
    let bin = wine_dir.join("appmgr-bin");
    if swift_bin_fresh(&bin) {
        return;
    }
    let src = wine_dir.join(".appmgr.swift");
    let swift = r##"import Cocoa

// mwine 设置面板：分页导航（虚拟机/显示/网络/设备伪装），改动即时落盘
// （AVD config.ini / mwine 设置库），标注哪些需要重启虚拟机生效。
final class NavItem: NSView {
    var label: NSTextField!
    var icon: NSImageView!
    var iconBox: NSView!
    var page: Int
    init(_ title: String, sysIcon: String, page: Int, tint: NSColor) {
        self.page = page
        super.init(frame: NSRect(x: 0, y: 0, width: 172, height: 48))
        wantsLayer = true
        layer?.cornerRadius = 10
        // 彩色圆角图标块（iOS 设置风格）
        iconBox = NSView(frame: NSRect(x: 10, y: 10, width: 28, height: 28))
        iconBox.wantsLayer = true
        iconBox.layer?.cornerRadius = 7
        iconBox.layer?.backgroundColor = tint.cgColor
        addSubview(iconBox)
        icon = NSImageView(frame: NSRect(x: 5, y: 5, width: 18, height: 18))
        icon.image = NSImage(systemSymbolName: sysIcon, accessibilityDescription: nil)
        icon.contentTintColor = .white
        iconBox.addSubview(icon)
        label = NSTextField(labelWithString: title)
        label.font = .systemFont(ofSize: 13, weight: .medium)
        label.frame = NSRect(x: 46, y: 16, width: 122, height: 16)
        addSubview(label)
    }
    func setSelected(_ on: Bool) {
        layer?.backgroundColor = on
            ? NSColor.controlAccentColor.withAlphaComponent(0.15).cgColor
            : NSColor.clear.cgColor
        label.textColor = on ? .controlAccentColor : .labelColor
        icon.contentTintColor = on ? .controlAccentColor : .secondaryLabelColor
    }
    required init?(coder: NSCoder) { fatalError() }
}

final class Row: NSView {
    let title = NSTextField(labelWithString: "")
    let value = NSTextField(labelWithString: "")
    var control: NSControl? = nil
    var keyPath: String = ""
    var needsRestart = false
    init(title: String, value: String = "", width: CGFloat = 396) {
        super.init(frame: NSRect(x: 0, y: 0, width: width, height: 30))
        self.title.stringValue = title
        self.title.font = .systemFont(ofSize: 12)
        self.title.frame = NSRect(x: 0, y: 7, width: 170, height: 16)
        addSubview(self.title)
        self.value.stringValue = value
        self.value.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        self.value.textColor = .secondaryLabelColor
        self.value.alignment = .right
        self.value.frame = NSRect(x: width - 160, y: 8, width: 160, height: 14)
        addSubview(self.value)
    }
    /// 右侧换成可交互控件
    func setControl(_ c: NSControl) {
        control.map { $0.removeFromSuperview() }
        value.isHidden = true
        c.frame = NSRect(x: frame.width - 194, y: 8, width: 180, height: 28)
        addSubview(c)
        control = c
    }
    func setNeedsRestart(_ note: String = "重启虚拟机后生效") {
        needsRestart = true
        value.stringValue = value.stringValue.isEmpty ? note : value.stringValue
    }
    required init?(coder: NSCoder) { fatalError() }
}

final class Delegate: NSObject, NSApplicationDelegate, NSComboBoxDataSource, NSComboBoxDelegate,
                      NSTextFieldDelegate {
    var win: NSWindow!
    var navItems: [NavItem] = []
    var pages: [NSView] = []
    var currentPage = 0
    // VM
    var ramCombo: NSComboBox!
    var cpuCombo: NSComboBox!
    // 显示
    var resCombo: NSComboBox!
    var fpsField: NSTextField!
    // 网络
    var proxyField: NSTextField!
    var dnsCombo: NSComboBox!
    // 设备
    var modelField: NSTextField!
    var brandField: NSTextField!
    var androidCombo: NSComboBox!

    // 活动 AVD：mwine 上次使用 > mwine 自建镜像 AVD > 其他 arm64 > RootAPI30。
    // 自建 AOSP 镜像（mwine-v2/android-13）与 Google 镜像并存，不能写死 RootAPI30。
    let avdIni: String = {
        let fm = FileManager.default
        let home = fm.homeDirectoryForCurrentUser
        let avdRoot = home.appendingPathComponent(".android/avd")
        func iniPath(_ name: String) -> String {
            avdRoot.appendingPathComponent("\(name).avd/config.ini").path
        }
        var candidates: [String] = []
        if let last = try? String(contentsOfFile: home.appendingPathComponent(".config/mwine/last-avd").path, encoding: .utf8) {
            let t = last.trimmingCharacters(in: .whitespacesAndNewlines)
            if !t.isEmpty { candidates.append(t) }
        }
        var mwineAvds: [String] = []
        var arm64Avds: [String] = []
        if let names = try? fm.contentsOfDirectory(atPath: avdRoot.path) {
            for d in names.sorted() where d.hasSuffix(".avd") {
                let name = String(d.dropLast(4))
                let cfg = (try? String(contentsOfFile: iniPath(name), encoding: .utf8)) ?? ""
                if cfg.contains("mwine") { mwineAvds.append(name) }
                else if cfg.contains("arm64") { arm64Avds.append(name) }
            }
        }
        candidates += mwineAvds + arm64Avds + ["RootAPI30"]
        for c in candidates where fm.fileExists(atPath: iniPath(c)) {
            return iniPath(c)
        }
        return iniPath("RootAPI30")
    }()

    func iniGet(_ key: String) -> String? {
        guard let t = try? String(contentsOfFile: avdIni, encoding: .utf8) else { return nil }
        for l in t.split(separator: "\n") {
            if l.hasPrefix(key + " =") { return String(l.dropFirst(key.count + 3)).trimmingCharacters(in: .whitespaces) }
        }
        return nil
    }
    @discardableResult
    func iniSet(_ key: String, _ value: String) -> Bool {
        guard var t = try? String(contentsOfFile: avdIni, encoding: .utf8) else { return false }
        var lines = t.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        var found = false
        for i in lines.indices where lines[i].hasPrefix(key + " =") {
            lines[i] = "\(key) = \(value)"; found = true
        }
        if !found { lines.append("\(key) = \(value)") }
        t = lines.joined(separator: "\n")
        do { try t.write(toFile: avdIni, atomically: true, encoding: .utf8); return true }
        catch { return false }
    }

    func applicationDidFinishLaunching(_ n: Notification) {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 480),
                       styleMask: [.titled, .closable], backing: .buffered, defer: false)
        win.title = "mwine 设置"
        let content = win.contentView!
        content.wantsLayer = true

        // 左侧导航
        let specs: [(String, String, NSColor)] = [
            ("虚拟机", "cpu", NSColor.systemBlue),
            ("显示", "rectangle.on.rectangle", NSColor.systemPurple),
            ("网络与代理", "network", NSColor.systemGreen),
            ("设备伪装", "iphone", NSColor.systemOrange),
        ]
        for (i, (t, ic, tint)) in specs.enumerated() {
            let item = NavItem(t, sysIcon: ic, page: i, tint: tint)
            item.frame.origin = CGPoint(x: 12, y: 414 - CGFloat(i) * 54)
            item.addTracking()
            navItems.append(item)
            content.addSubview(item)
        }

        buildVMPage(); buildDisplayPage(); buildNetworkPage(); buildDevicePage()
        for (i, p) in pages.enumerated() {
            p.frame = NSRect(x: 200, y: 52, width: 424, height: 400)
            p.isHidden = i != 0
            content.addSubview(p)
        }
        navItems.first?.setSelected(true)

        let done = NSButton(title: "完成", target: self, action: #selector(doneMe))
        done.bezelStyle = .rounded
        done.keyEquivalent = "\r"
        done.controlSize = .large
        done.frame = NSRect(x: 506, y: 8, width: 98, height: 36)
        content.addSubview(done)
        let hint = NSTextField(labelWithString: "标注 ⚠ 的改动需重启虚拟机生效（菜单 mwine → 重启虚拟机）")
        hint.font = .systemFont(ofSize: 10)
        hint.textColor = .secondaryLabelColor
        hint.frame = NSRect(x: 204, y: 18, width: 300, height: 12)
        content.addSubview(hint)

        win.center()
        win.orderFrontRegardless()
        app.activate(ignoringOtherApps: true)
    }
    @objc func doneMe() { NSApp.terminate(nil) }

    // MARK: - 页面
    func pageHeader(_ p: NSView, _ title: String, y: CGFloat) {
        let h = NSTextField(labelWithString: title)
        h.font = .systemFont(ofSize: 16, weight: .bold)
        h.frame = NSRect(x: 4, y: y, width: 300, height: 20)
        p.addSubview(h)
        _ = h.frame
        let line = NSView(frame: NSRect(x: 4, y: y - 8, width: 416, height: 1))
        line.wantsLayer = true
        line.layer?.backgroundColor = NSColor.separatorColor.withAlphaComponent(0.5).cgColor
        p.addSubview(line)
    }

    func buildVMPage() {
        let p = NSView()
        pageHeader(p, "虚拟机", y: 350)
        // 内存
        let ram = Row(title: "内存 ⚠", value: (iniGet("hw.ramSize") ?? "?") + " MB", width: 396)
        ram.frame.origin = CGPoint(x: 4, y: 300)
        ramCombo = combo(items: ["2048", "3072", "4096", "6144", "8192"], sel: iniGet("hw.ramSize") ?? "6144")
        ramCombo.delegate = self
        ramCombo.tag = 100
        ram.setControl(ramCombo)
        p.addSubview(ram)
        // CPU
        let cpu = Row(title: "CPU 核心 ⚠", value: (iniGet("hw.cpu.ncore") ?? "4") + " 核", width: 396)
        cpu.frame.origin = CGPoint(x: 4, y: 246)
        cpuCombo = combo(items: ["2", "4", "6", "8"], sel: iniGet("hw.cpu.ncore") ?? "4")
        cpuCombo.tag = 101
        cpu.setControl(cpuCombo)
        p.addSubview(cpu)
        // 存储
        let disk = Row(title: "磁盘镜像", value: (iniGet("disk.dataPartition.size") ?? "-"), width: 396)
        disk.frame.origin = CGPoint(x: 4, y: 192)
        p.addSubview(disk)
        // AVD 信息
        let info = NSTextField(wrappingLabelWithString:
            "AVD: RootAPI30 · arm64 · Android 11\n设置写入 ~/.android/avd/RootAPI30.avd/config.ini")
        info.font = .systemFont(ofSize: 10)
        info.textColor = .secondaryLabelColor
        info.frame = NSRect(x: 4, y: 10, width: 396, height: 40)
        p.addSubview(info)
        pages.append(p)
    }

    func buildDisplayPage() {
        let p = NSView()
        pageHeader(p, "显示", y: 350)
        let res = Row(title: "窗口分辨率", value: "970x2048/430", width: 396)
        res.frame.origin = CGPoint(x: 4, y: 300)
        resCombo = combo(items: ["720x1520/320", "970x2048/430", "1080x2280/420"], sel: "970x2048/430")
        resCombo.tag = 200
        res.setControl(resCombo)
        p.addSubview(res)
        let fps = Row(title: "帧率上限", value: "30", width: 396)
        fps.frame.origin = CGPoint(x: 4, y: 246)
        fpsField = NSTextField()
        fpsField.stringValue = "30"
        fpsField.placeholderString = "10-60"
        fpsField.tag = 201
        fpsField.delegate = self
        fps.setControl(fpsField)
        p.addSubview(fps)
        let gpu = Row(title: "GPU 渲染", value: "原生（Metal）", width: 396)
        gpu.frame.origin = CGPoint(x: 4, y: 192)
        p.addSubview(gpu)
        let note = NSTextField(wrappingLabelWithString:
            "分辨率/帧率作用于新生成的 .app 壳（mwine app create）；已有应用用窗口菜单「切换 清晰/流畅模式」")
        note.font = .systemFont(ofSize: 10)
        note.textColor = .secondaryLabelColor
        note.frame = NSRect(x: 4, y: 10, width: 396, height: 40)
        p.addSubview(note)
        pages.append(p)
    }

    func buildNetworkPage() {
        let p = NSView()
        pageHeader(p, "网络与代理", y: 350)
        let proxy = Row(title: "HTTP 代理", value: "未设置", width: 396)
        proxy.frame.origin = CGPoint(x: 4, y: 300)
        proxyField = NSTextField()
        proxyField.placeholderString = "host:port（如 127.0.0.1:7890）"
        // 读取现有（-global http.proxy）
        let cur = shell("/Users/yusengapu/Library/Android/sdk/platform-tools/adb",
                        ["-s", "emulator-5554", "shell", "settings get global http_proxy"])
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if cur != ":0" && !cur.contains("null") && !cur.isEmpty {
            proxyField.stringValue = cur
        }
        proxyField.tag = 300
        proxy.setControl(proxyField)
        p.addSubview(proxy)
        let dns = Row(title: "DNS ⚠", value: "默认 10.0.2.3", width: 396)
        dns.frame.origin = CGPoint(x: 4, y: 246)
        dnsCombo = combo(items: ["默认", "8.8.8.8", "1.1.1.1", "223.5.5.5"], sel: "默认")
        dns.setControl(dnsCombo)
        p.addSubview(dns)
        let netinfo = Row(title: "设备网络", value: "NAT（10.0.2.x）", width: 396)
        netinfo.frame.origin = CGPoint(x: 4, y: 192)
        p.addSubview(netinfo)
        let note = NSTextField(wrappingLabelWithString:
            "代理立即生效（adb settings put global http_proxy，留空清除）；DNS 写入 AVD 启动参数")
        note.font = .systemFont(ofSize: 10)
        note.textColor = .secondaryLabelColor
        note.frame = NSRect(x: 4, y: 10, width: 396, height: 40)
        p.addSubview(note)
        pages.append(p)
    }

    func buildDevicePage() {
        let p = NSView()
        pageHeader(p, "设备伪装", y: 350)
        let model = Row(title: "型号 ⚠", value: sysProp("ro.product.model"), width: 396)
        model.frame.origin = CGPoint(x: 4, y: 300)
        modelField = NSTextField()
        modelField.stringValue = sysProp("ro.product.model")
        modelField.tag = 400
        model.setControl(modelField)
        p.addSubview(model)
        let brand = Row(title: "品牌 ⚠", value: sysProp("ro.product.brand"), width: 396)
        brand.frame.origin = CGPoint(x: 4, y: 246)
        brandField = NSTextField()
        brandField.stringValue = sysProp("ro.product.brand")
        brandField.tag = 401
        brand.setControl(brandField)
        p.addSubview(brand)
        let ver = Row(title: "Android 版本 ⚠", value: sysProp("ro.build.version.release"), width: 396)
        ver.frame.origin = CGPoint(x: 4, y: 192)
        androidCombo = combo(items: ["跟随镜像"], sel: "跟随镜像")
        androidCombo.isEnabled = false
        ver.setControl(androidCombo)
        p.addSubview(ver)
        let note = NSTextField(wrappingLabelWithString:
            "型号/品牌写入 AVD config.ini（hw.device.*）；运行中设备可临时改 system build.prop（需 root 镜像）")
        note.font = .systemFont(ofSize: 10)
        note.textColor = .secondaryLabelColor
        note.frame = NSRect(x: 4, y: 10, width: 396, height: 40)
        p.addSubview(note)
        pages.append(p)
    }

    // MARK: - 交互
    func combo(items: [String], sel: String) -> NSComboBox {
        let c = NSComboBox()
        c.addItems(withObjectValues: items)
        c.stringValue = sel
        c.usesDataSource = false
        c.isBezeled = true
        c.completes = false
        return c
    }
    func comboBoxSelectionDidChange(_ n: Notification) {
        guard let c = n.object as? NSComboBox else { return }
        switch c.tag {
        case 100: iniSet("hw.ramSize", c.stringValue); toast("内存 = \(c.stringValue) MB（重启虚拟机生效）")
        case 101: iniSet("hw.cpu.ncore", c.stringValue); toast("CPU = \(c.stringValue) 核（重启虚拟机生效）")
        case 200: toast("分辨率 \(c.stringValue) 将用于新生成的应用壳")
        default: break
        }
    }
    func controlTextDidEndEditing(_ n: Notification) {
        guard let f = n.object as? NSTextField else { return }
        switch f.tag {
        case 201:
            let v = Int(f.stringValue) ?? 30
            toast("帧率上限 = \(v)（新生成应用壳生效）")
        case 300:
            let v = f.stringValue.trimmingCharacters(in: .whitespaces)
            let adbBin = "/Users/yusengapu/Library/Android/sdk/platform-tools/adb"
            if v.isEmpty {
                _ = shell(adbBin, ["-s", "emulator-5554", "shell", "settings put global http_proxy :0"])
                toast("代理已清除")
            } else {
                _ = shell(adbBin, ["-s", "emulator-5554", "shell", "settings put global http_proxy \(v)"])
                toast("代理 = \(v)（立即生效）")
            }
        case 400: iniSet("hw.device.name", f.stringValue); toast("型号 = \(f.stringValue)（重启生效）")
        case 401: iniSet("hw.device.manufacturer", f.stringValue); toast("品牌 = \(f.stringValue)（重启生效）")
        default: break
        }
    }
    func comboBox(_ c: NSComboBox, objectValueForItemAt idx: Int) -> Any? {
        (c.objectValues as? [String])?[idx] ?? ""
    }
    func numberOfItems(in c: NSComboBox) -> Int { 0 }

    func toast(_ msg: String) {
        let _ = try? Process().run(bin: "/usr/bin/osascript", args: [
            "-e", "display notification \"\(msg.replacingOccurrences(of: "\"", with: "'"))\" with title \"mwine 设置\""
        ])
    }
    func sysProp(_ k: String) -> String {
        shell("/Users/yusengapu/Library/Android/sdk/platform-tools/adb",
              ["-s", "emulator-5554", "shell", "getprop \(k)"]).trimmingCharacters(in: .whitespacesAndNewlines)
    }
    func shell(_ b: String, _ a: [String]) -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: b)
        p.arguments = a
        p.standardOutput = Pipe(); p.standardError = Pipe()
        guard (try? p.run()) != nil else { return "" }
        p.waitUntilExit()
        return String(data: (p.standardOutput as? Pipe)?.fileHandleForReading.readDataToEndOfFile() ?? Data(), encoding: .utf8) ?? ""
    }

    func selectPage(_ i: Int) {
        navItems.enumerated().forEach { $0.element.setSelected($0.offset == i) }
        pages.enumerated().forEach { $0.element.isHidden = $0.offset != i }
        currentPage = i
    }
}

extension NavItem {
    func addTracking() {
        let t = NSTrackingArea(rect: bounds, options: [.activeAlways, .mouseEnteredAndExited], owner: self)
        addTrackingArea(t)
    }
    override func mouseDown(with e: NSEvent) {
        (NSApp.delegate as? Delegate)?.selectPage(page)
    }
    override func mouseEntered(with e: NSEvent) {
        layer?.backgroundColor = NSColor.quaternaryLabelColor.withAlphaComponent(0.15).cgColor
    }
    override func mouseExited(with e: NSEvent) {
        if label.textColor != .controlAccentColor {
            layer?.backgroundColor = NSColor.clear.cgColor
        }
    }
}
extension Process {
    func run(bin: String, args: [String]) throws {
        executableURL = URL(fileURLWithPath: bin)
        arguments = args
        standardOutput = Pipe(); standardError = Pipe()
        try run(); waitUntilExit()
    }
}

let app = NSApplication.shared
let d = Delegate()
app.delegate = d
d.applicationDidFinishLaunching(Notification(name: .init("l")))
app.run()
"##;;;;;
    let _ = std::fs::write(&src, swift);
    let compiled = run_tool(
        std::process::Command::new("swiftc")
            .args(["-O", "-o"])
            .arg(&bin)
            .arg(&src)
            .arg("-framework")
            .arg("Cocoa"),
    );
    match compiled {
        Ok(_) => {
            std::fs::remove_file(&src).ok();
            mark_swift_bin_version(&bin);
            codesign_best_effort(&bin);
        }
        Err(e) => {
            std::fs::remove_file(&src).ok();
            println!("～ 窗口管理器编译失败（{e}），菜单将退化为提示");
        }
    }
}


/// 设置面板（菜单「设置…」）：虚拟机/显示/网络代理/设备伪装 四页。
fn ensure_settings_bin(wine_dir: &Path) {
    let bin = wine_dir.join("settings-bin");
    if swift_bin_fresh(&bin) {
        return;
    }
    let src = wine_dir.join(".settings.swift");
    let swift = r##"import Cocoa

// mwine 设置面板：分页导航（虚拟机/显示/网络/设备伪装），改动即时落盘
// （AVD config.ini / mwine 设置库），标注哪些需要重启虚拟机生效。
final class NavItem: NSView {
    var label: NSTextField!
    var icon: NSImageView!
    var page: Int
    init(_ title: String, sysIcon: String, page: Int) {
        self.page = page
        super.init(frame: NSRect(x: 0, y: 0, width: 168, height: 40))
        wantsLayer = true
        layer?.cornerRadius = 8
        label = NSTextField(labelWithString: title)
        label.font = .systemFont(ofSize: 13, weight: .medium)
        label.frame = NSRect(x: 40, y: 12, width: 120, height: 16)
        addSubview(label)
        icon = NSImageView(frame: NSRect(x: 10, y: 10, width: 20, height: 20))
        icon.image = NSImage(systemSymbolName: sysIcon, accessibilityDescription: nil)
        icon.contentTintColor = .secondaryLabelColor
        addSubview(icon)
    }
    func setSelected(_ on: Bool) {
        layer?.backgroundColor = on
            ? NSColor.controlAccentColor.withAlphaComponent(0.15).cgColor
            : NSColor.clear.cgColor
        label.textColor = on ? .controlAccentColor : .labelColor
        icon.contentTintColor = on ? .controlAccentColor : .secondaryLabelColor
    }
    required init?(coder: NSCoder) { fatalError() }
}

final class Row: NSView {
    let title = NSTextField(labelWithString: "")
    let value = NSTextField(labelWithString: "")
    var control: NSControl? = nil
    var keyPath: String = ""
    var needsRestart = false
    init(title: String, value: String = "", width: CGFloat = 396) {
        super.init(frame: NSRect(x: 0, y: 0, width: width, height: 30))
        self.title.stringValue = title
        self.title.font = .systemFont(ofSize: 12)
        self.title.frame = NSRect(x: 0, y: 7, width: 170, height: 16)
        addSubview(self.title)
        self.value.stringValue = value
        self.value.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        self.value.textColor = .secondaryLabelColor
        self.value.alignment = .right
        self.value.frame = NSRect(x: width - 160, y: 8, width: 160, height: 14)
        addSubview(self.value)
    }
    /// 右侧换成可交互控件
    func setControl(_ c: NSControl) {
        control.map { $0.removeFromSuperview() }
        value.isHidden = true
        c.frame = NSRect(x: frame.width - 170, y: 2, width: 170, height: 26)
        addSubview(c)
        control = c
    }
    func setNeedsRestart(_ note: String = "重启虚拟机后生效") {
        needsRestart = true
        value.stringValue = value.stringValue.isEmpty ? note : value.stringValue
    }
    required init?(coder: NSCoder) { fatalError() }
}

final class Delegate: NSObject, NSApplicationDelegate, NSComboBoxDataSource, NSComboBoxDelegate,
                      NSTextFieldDelegate {
    var win: NSWindow!
    var navItems: [NavItem] = []
    var pages: [NSView] = []
    var currentPage = 0
    // VM
    var ramCombo: NSComboBox!
    var cpuCombo: NSComboBox!
    // 显示
    var resCombo: NSComboBox!
    var fpsField: NSTextField!
    // 网络
    var proxyField: NSTextField!
    var dnsCombo: NSComboBox!
    // 设备
    var modelField: NSTextField!
    var brandField: NSTextField!
    var androidCombo: NSComboBox!

    // 活动 AVD：mwine 上次使用 > mwine 自建镜像 AVD > 其他 arm64 > RootAPI30。
    // 自建 AOSP 镜像（mwine-v2/android-13）与 Google 镜像并存，不能写死 RootAPI30。
    let avdIni: String = {
        let fm = FileManager.default
        let home = fm.homeDirectoryForCurrentUser
        let avdRoot = home.appendingPathComponent(".android/avd")
        func iniPath(_ name: String) -> String {
            avdRoot.appendingPathComponent("\(name).avd/config.ini").path
        }
        var candidates: [String] = []
        if let last = try? String(contentsOfFile: home.appendingPathComponent(".config/mwine/last-avd").path, encoding: .utf8) {
            let t = last.trimmingCharacters(in: .whitespacesAndNewlines)
            if !t.isEmpty { candidates.append(t) }
        }
        var mwineAvds: [String] = []
        var arm64Avds: [String] = []
        if let names = try? fm.contentsOfDirectory(atPath: avdRoot.path) {
            for d in names.sorted() where d.hasSuffix(".avd") {
                let name = String(d.dropLast(4))
                let cfg = (try? String(contentsOfFile: iniPath(name), encoding: .utf8)) ?? ""
                if cfg.contains("mwine") { mwineAvds.append(name) }
                else if cfg.contains("arm64") { arm64Avds.append(name) }
            }
        }
        candidates += mwineAvds + arm64Avds + ["RootAPI30"]
        for c in candidates where fm.fileExists(atPath: iniPath(c)) {
            return iniPath(c)
        }
        return iniPath("RootAPI30")
    }()

    func iniGet(_ key: String) -> String? {
        guard let t = try? String(contentsOfFile: avdIni, encoding: .utf8) else { return nil }
        for l in t.split(separator: "\n") {
            if l.hasPrefix(key + " =") { return String(l.dropFirst(key.count + 3)).trimmingCharacters(in: .whitespaces) }
        }
        return nil
    }
    @discardableResult
    func iniSet(_ key: String, _ value: String) -> Bool {
        guard var t = try? String(contentsOfFile: avdIni, encoding: .utf8) else { return false }
        var lines = t.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        var found = false
        for i in lines.indices where lines[i].hasPrefix(key + " =") {
            lines[i] = "\(key) = \(value)"; found = true
        }
        if !found { lines.append("\(key) = \(value)") }
        t = lines.joined(separator: "\n")
        do { try t.write(toFile: avdIni, atomically: true, encoding: .utf8); return true }
        catch { return false }
    }

    func applicationDidFinishLaunching(_ n: Notification) {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 620, height: 460),
                       styleMask: [.titled, .closable], backing: .buffered, defer: false)
        win.title = "mwine 设置"
        let content = win.contentView!
        content.wantsLayer = true

        // 左侧导航
        let specs: [(String, String)] = [
            ("虚拟机", "cpu"), ("显示", "rectangle.on.rectangle"),
            ("网络与代理", "network"), ("设备伪装", "iphone"),
        ]
        for (i, (t, ic)) in specs.enumerated() {
            let item = NavItem(t, sysIcon: ic, page: i)
            item.frame.origin = CGPoint(x: 12, y: 420 - CGFloat(i) * 46)
            item.addTracking()
            navItems.append(item)
            content.addSubview(item)
        }

        buildVMPage(); buildDisplayPage(); buildNetworkPage(); buildDevicePage()
        for (i, p) in pages.enumerated() {
            p.frame = NSRect(x: 196, y: 46, width: 404, height: 380)
            p.isHidden = i != 0
            content.addSubview(p)
        }
        navItems.first?.setSelected(true)

        let done = NSButton(title: "完成", target: self, action: #selector(doneMe))
        done.bezelStyle = .rounded
        done.keyEquivalent = "\r"
        done.frame = NSRect(x: 524, y: 10, width: 80, height: 30)
        content.addSubview(done)
        let hint = NSTextField(labelWithString: "标注 ⚠ 的改动需重启虚拟机生效（菜单 mwine → 重启虚拟机）")
        hint.font = .systemFont(ofSize: 10)
        hint.textColor = .secondaryLabelColor
        hint.frame = NSRect(x: 200, y: 16, width: 320, height: 12)
        content.addSubview(hint)

        win.center()
        win.orderFrontRegardless()
        app.activate(ignoringOtherApps: true)
    }
    @objc func doneMe() { NSApp.terminate(nil) }

    // MARK: - 页面
    func pageHeader(_ p: NSView, _ title: String, y: CGFloat) {
        let h = NSTextField(labelWithString: title)
        h.font = .systemFont(ofSize: 15, weight: .semibold)
        h.frame = NSRect(x: 4, y: y, width: 300, height: 20)
        p.addSubview(h)
        _ = h.frame
        let line = NSView(frame: NSRect(x: 4, y: y - 6, width: 396, height: 1))
        line.wantsLayer = true
        line.layer?.backgroundColor = NSColor.separatorColor.withAlphaComponent(0.5).cgColor
        p.addSubview(line)
    }

    func buildVMPage() {
        let p = NSView()
        pageHeader(p, "虚拟机", y: 350)
        // 内存
        let ram = Row(title: "内存 ⚠", value: (iniGet("hw.ramSize") ?? "?") + " MB", width: 396)
        ram.frame.origin = CGPoint(x: 4, y: 308)
        ramCombo = combo(items: ["2048", "3072", "4096", "6144", "8192"], sel: iniGet("hw.ramSize") ?? "6144")
        ramCombo.delegate = self
        ramCombo.tag = 100
        ram.setControl(ramCombo)
        p.addSubview(ram)
        // CPU
        let cpu = Row(title: "CPU 核心 ⚠", value: (iniGet("hw.cpu.ncore") ?? "4") + " 核", width: 396)
        cpu.frame.origin = CGPoint(x: 4, y: 272)
        cpuCombo = combo(items: ["2", "4", "6", "8"], sel: iniGet("hw.cpu.ncore") ?? "4")
        cpuCombo.tag = 101
        cpu.setControl(cpuCombo)
        p.addSubview(cpu)
        // 存储
        let disk = Row(title: "磁盘镜像", value: (iniGet("disk.dataPartition.size") ?? "-"), width: 396)
        disk.frame.origin = CGPoint(x: 4, y: 236)
        p.addSubview(disk)
        // AVD 信息
        let info = NSTextField(wrappingLabelWithString:
            "AVD: RootAPI30 · arm64 · Android 11\n设置写入 ~/.android/avd/RootAPI30.avd/config.ini")
        info.font = .systemFont(ofSize: 10)
        info.textColor = .secondaryLabelColor
        info.frame = NSRect(x: 4, y: 10, width: 396, height: 40)
        p.addSubview(info)
        pages.append(p)
    }

    func buildDisplayPage() {
        let p = NSView()
        pageHeader(p, "显示", y: 350)
        let res = Row(title: "窗口分辨率", value: "970x2048/430", width: 396)
        res.frame.origin = CGPoint(x: 4, y: 308)
        resCombo = combo(items: ["720x1520/320", "970x2048/430", "1080x2280/420"], sel: "970x2048/430")
        resCombo.tag = 200
        res.setControl(resCombo)
        p.addSubview(res)
        let fps = Row(title: "帧率上限", value: "30", width: 396)
        fps.frame.origin = CGPoint(x: 4, y: 272)
        fpsField = NSTextField()
        fpsField.stringValue = "30"
        fpsField.placeholderString = "10-60"
        fpsField.tag = 201
        fpsField.delegate = self
        fps.setControl(fpsField)
        p.addSubview(fps)
        let gpu = Row(title: "GPU 渲染", value: "原生（Metal）", width: 396)
        gpu.frame.origin = CGPoint(x: 4, y: 236)
        p.addSubview(gpu)
        let note = NSTextField(wrappingLabelWithString:
            "分辨率/帧率作用于新生成的 .app 壳（mwine app create）；已有应用用窗口菜单「切换 清晰/流畅模式」")
        note.font = .systemFont(ofSize: 10)
        note.textColor = .secondaryLabelColor
        note.frame = NSRect(x: 4, y: 10, width: 396, height: 40)
        p.addSubview(note)
        pages.append(p)
    }

    func buildNetworkPage() {
        let p = NSView()
        pageHeader(p, "网络与代理", y: 350)
        let proxy = Row(title: "HTTP 代理", value: "未设置", width: 396)
        proxy.frame.origin = CGPoint(x: 4, y: 308)
        proxyField = NSTextField()
        proxyField.placeholderString = "host:port（如 127.0.0.1:7890）"
        // 读取现有（-global http.proxy）
        let cur = shell("/Users/yusengapu/Library/Android/sdk/platform-tools/adb",
                        ["-s", "emulator-5554", "shell", "settings get global http_proxy"])
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if cur != ":0" && !cur.contains("null") && !cur.isEmpty {
            proxyField.stringValue = cur
        }
        proxyField.tag = 300
        proxy.setControl(proxyField)
        p.addSubview(proxy)
        let dns = Row(title: "DNS ⚠", value: "默认 10.0.2.3", width: 396)
        dns.frame.origin = CGPoint(x: 4, y: 272)
        dnsCombo = combo(items: ["默认", "8.8.8.8", "1.1.1.1", "223.5.5.5"], sel: "默认")
        dns.setControl(dnsCombo)
        p.addSubview(dns)
        let netinfo = Row(title: "设备网络", value: "NAT（10.0.2.x）", width: 396)
        netinfo.frame.origin = CGPoint(x: 4, y: 236)
        p.addSubview(netinfo)
        let note = NSTextField(wrappingLabelWithString:
            "代理立即生效（adb settings put global http_proxy，留空清除）；DNS 写入 AVD 启动参数")
        note.font = .systemFont(ofSize: 10)
        note.textColor = .secondaryLabelColor
        note.frame = NSRect(x: 4, y: 10, width: 396, height: 40)
        p.addSubview(note)
        pages.append(p)
    }

    func buildDevicePage() {
        let p = NSView()
        pageHeader(p, "设备伪装", y: 350)
        let model = Row(title: "型号 ⚠", value: sysProp("ro.product.model"), width: 396)
        model.frame.origin = CGPoint(x: 4, y: 308)
        modelField = NSTextField()
        modelField.stringValue = sysProp("ro.product.model")
        modelField.tag = 400
        model.setControl(modelField)
        p.addSubview(model)
        let brand = Row(title: "品牌 ⚠", value: sysProp("ro.product.brand"), width: 396)
        brand.frame.origin = CGPoint(x: 4, y: 272)
        brandField = NSTextField()
        brandField.stringValue = sysProp("ro.product.brand")
        brandField.tag = 401
        brand.setControl(brandField)
        p.addSubview(brand)
        let ver = Row(title: "Android 版本 ⚠", value: sysProp("ro.build.version.release"), width: 396)
        ver.frame.origin = CGPoint(x: 4, y: 236)
        androidCombo = combo(items: ["跟随镜像"], sel: "跟随镜像")
        androidCombo.isEnabled = false
        ver.setControl(androidCombo)
        p.addSubview(ver)
        let note = NSTextField(wrappingLabelWithString:
            "型号/品牌写入 AVD config.ini（hw.device.*）；运行中设备可临时改 system build.prop（需 root 镜像）")
        note.font = .systemFont(ofSize: 10)
        note.textColor = .secondaryLabelColor
        note.frame = NSRect(x: 4, y: 10, width: 396, height: 40)
        p.addSubview(note)
        pages.append(p)
    }

    // MARK: - 交互
    func combo(items: [String], sel: String) -> NSComboBox {
        let c = NSComboBox()
        c.addItems(withObjectValues: items)
        c.stringValue = sel
        c.usesDataSource = false
        c.isBezeled = true
        c.completes = false
        return c
    }
    func comboBoxSelectionDidChange(_ n: Notification) {
        guard let c = n.object as? NSComboBox else { return }
        switch c.tag {
        case 100: iniSet("hw.ramSize", c.stringValue); toast("内存 = \(c.stringValue) MB（重启虚拟机生效）")
        case 101: iniSet("hw.cpu.ncore", c.stringValue); toast("CPU = \(c.stringValue) 核（重启虚拟机生效）")
        case 200: toast("分辨率 \(c.stringValue) 将用于新生成的应用壳")
        default: break
        }
    }
    func controlTextDidEndEditing(_ n: Notification) {
        guard let f = n.object as? NSTextField else { return }
        switch f.tag {
        case 201:
            let v = Int(f.stringValue) ?? 30
            toast("帧率上限 = \(v)（新生成应用壳生效）")
        case 300:
            let v = f.stringValue.trimmingCharacters(in: .whitespaces)
            let adbBin = "/Users/yusengapu/Library/Android/sdk/platform-tools/adb"
            if v.isEmpty {
                _ = shell(adbBin, ["-s", "emulator-5554", "shell", "settings put global http_proxy :0"])
                toast("代理已清除")
            } else {
                _ = shell(adbBin, ["-s", "emulator-5554", "shell", "settings put global http_proxy \(v)"])
                toast("代理 = \(v)（立即生效）")
            }
        case 400: iniSet("hw.device.name", f.stringValue); toast("型号 = \(f.stringValue)（重启生效）")
        case 401: iniSet("hw.device.manufacturer", f.stringValue); toast("品牌 = \(f.stringValue)（重启生效）")
        default: break
        }
    }
    func comboBox(_ c: NSComboBox, objectValueForItemAt idx: Int) -> Any? {
        (c.objectValues as? [String])?[idx] ?? ""
    }
    func numberOfItems(in c: NSComboBox) -> Int { 0 }

    func toast(_ msg: String) {
        let _ = try? Process().run(bin: "/usr/bin/osascript", args: [
            "-e", "display notification \"\(msg.replacingOccurrences(of: "\"", with: "'"))\" with title \"mwine 设置\""
        ])
    }
    func sysProp(_ k: String) -> String {
        shell("/Users/yusengapu/Library/Android/sdk/platform-tools/adb",
              ["-s", "emulator-5554", "shell", "getprop \(k)"]).trimmingCharacters(in: .whitespacesAndNewlines)
    }
    func shell(_ b: String, _ a: [String]) -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: b)
        p.arguments = a
        p.standardOutput = Pipe(); p.standardError = Pipe()
        guard (try? p.run()) != nil else { return "" }
        p.waitUntilExit()
        return String(data: (p.standardOutput as? Pipe)?.fileHandleForReading.readDataToEndOfFile() ?? Data(), encoding: .utf8) ?? ""
    }

    func selectPage(_ i: Int) {
        navItems.enumerated().forEach { $0.element.setSelected($0.offset == i) }
        pages.enumerated().forEach { $0.element.isHidden = $0.offset != i }
        currentPage = i
    }
}

extension NavItem {
    func addTracking() {
        let t = NSTrackingArea(rect: bounds, options: [.activeAlways, .mouseEnteredAndExited], owner: self)
        addTrackingArea(t)
    }
    override func mouseDown(with e: NSEvent) {
        (NSApp.delegate as? Delegate)?.selectPage(page)
    }
    override func mouseEntered(with e: NSEvent) {
        layer?.backgroundColor = NSColor.quaternaryLabelColor.withAlphaComponent(0.15).cgColor
    }
    override func mouseExited(with e: NSEvent) {
        if label.textColor != .controlAccentColor {
            layer?.backgroundColor = NSColor.clear.cgColor
        }
    }
}
extension Process {
    func run(bin: String, args: [String]) throws {
        executableURL = URL(fileURLWithPath: bin)
        arguments = args
        standardOutput = Pipe(); standardError = Pipe()
        try run(); waitUntilExit()
    }
}

let app = NSApplication.shared
let d = Delegate()
app.delegate = d
d.applicationDidFinishLaunching(Notification(name: .init("l")))
app.run()
"##;
    let _ = std::fs::write(&src, swift);
    let compiled = run_tool(
        std::process::Command::new("swiftc")
            .args(["-O", "-o"])
            .arg(&bin)
            .arg(&src)
            .arg("-framework")
            .arg("Cocoa"),
    );
    match compiled {
        Ok(_) => {
            std::fs::remove_file(&src).ok();
            mark_swift_bin_version(&bin);
            codesign_best_effort(&bin);
        }
        Err(e) => {
            std::fs::remove_file(&src).ok();
            println!("～ 设置面板编译失败（{e}）");
        }
    }
}


fn dirs_or_create() -> Result<std::path::PathBuf> {
    let d = std::path::PathBuf::from(
        std::env::var("HOME").unwrap_or_else(|_| "/tmp".into()),
    )
    .join(".mwine");
    std::fs::create_dir_all(&d).ok();
    Ok(d)
}
