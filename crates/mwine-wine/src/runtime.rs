//! Wine 运行时发现与命令构造。
//!
//! 发现顺序（首个命中生效）：
//! 1. `MWINE_WINE` 环境变量：指向 wine 可执行文件，或包含 bin/wine 的目录
//! 2. /Applications 下的已知 Wine .app（Wine Crossover / Wine Staging / Wine Devel / Wine Stable）
//! 3. Homebrew Caskroom 内的同名 .app（brew 卸载残留时兜底）
//! 4. PATH 中的 wine / wine64

use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{anyhow, Context, Result};

/// 所有 wine 进程统一注入的环境（裁剪/降噪的关键）：
/// - `mscoree,mshtml=` 禁用 Mono/Gecko → 新建前缀不下载安装这两个大家伙
///   （.NET 网页类应用需要时再 `winetricks` 定向补）
/// - `winemenubuilder.exe=d` 禁止 wine 在宿主生成菜单/启动项垃圾
/// - `WINEDEBUG=-all` 关闭调试通道（省开销、日志不刷屏）
pub const DLL_OVERRIDES: &str = "mscoree,mshtml=;winemenubuilder.exe=d";

#[derive(Debug, Clone)]
pub struct WineRuntime {
    /// wine 启动器（脚本或二进制），接受 `<程序> [参数...]`
    pub wine: PathBuf,
    /// 同目录 wineserver（前缀清理用）
    pub server: PathBuf,
    /// 发现来源描述（doctor 展示）
    pub source: String,
}

/// /Applications 与 Caskroom 里已知的 Wine .app 名与其 bin 子目录。
/// （均为「Resources/<dir>/bin」布局）
const KNOWN_APPS: &[(&str, &str)] = &[
    ("Wine Crossover.app", "wine"),
    ("Wine Staging.app", "wine"),
    ("Wine Devel.app", "wine"),
    ("Wine Stable.app", "wine"),
];

impl WineRuntime {
    /// 按顺序探测 Wine 运行时；找不到返回 None（调用方给出安装指引）。
    pub fn discover() -> Option<WineRuntime> {
        // 1. 环境变量
        if let Ok(v) = std::env::var("MWINE_WINE") {
            let p = PathBuf::from(&v);
            let candidates: Vec<PathBuf> = if p.is_dir() {
                vec![p.join("bin/wine"), p.join("wine")]
            } else {
                vec![p.clone()]
            };
            for c in candidates {
                if c.is_file() {
                    return Some(Self::from_bin(c, format!("MWINE_WINE={v}")));
                }
            }
        }
        // 2. /Applications
        for (app, sub) in KNOWN_APPS {
            if let Some(rt) = Self::from_app(&PathBuf::from("/Applications").join(app), sub) {
                return Some(rt);
            }
        }
        // 3. Caskroom 兜底（brew 已登记但 /Applications 被清掉的残留）
        for caskroom in ["/opt/homebrew/Caskroom", "/usr/local/Caskroom"] {
            let base = PathBuf::from(caskroom);
            let Ok(entries) = std::fs::read_dir(&base) else { continue };
            for (app, sub) in KNOWN_APPS {
                let app_name = app.trim_end_matches(".app");
                let Ok(vers) = std::fs::read_dir(base.join(app_name)) else { continue };
                for ver in vers.flatten() {
                    if let Some(rt) = Self::from_app(&ver.path().join(app), sub) {
                        return Some(rt);
                    }
                }
            }
        }
        // 4. PATH
        for name in ["wine", "wine64"] {
            if let Ok(out) = Command::new("which").arg(name).output() {
                if out.status.success() {
                    let p = PathBuf::from(String::from_utf8_lossy(&out.stdout).trim());
                    if p.is_file() {
                        return Some(Self::from_bin(p, "PATH".into()));
                    }
                }
            }
        }
        None
    }

    fn from_app(app: &Path, sub: &str) -> Option<WineRuntime> {
        let bin = app.join("Contents/Resources").join(sub).join("bin");
        Self::from_bin_dir(&bin).map(|rt| {
            let source = format!("{}", app.display());
            WineRuntime { source, ..rt }
        })
    }

    fn from_bin_dir(bin: &Path) -> Option<WineRuntime> {
        let wine = bin.join("wine");
        if !wine.is_file() {
            return None;
        }
        let server = bin.join("wineserver");
        Some(WineRuntime {
            wine,
            server,
            source: String::new(),
        })
    }

    fn from_bin(bin: PathBuf, source: String) -> WineRuntime {
        let server = bin
            .parent()
            .map(|p| p.join("wineserver"))
            .filter(|p| p.is_file())
            .unwrap_or_else(|| bin.clone());
        WineRuntime { wine: bin, server, source }
    }

    /// 构造一个已注入 WINEPREFIX 与裁剪环境的 wine 命令。
    pub fn command(&self, prefix: &Path) -> Command {
        let mut c = Command::new(&self.wine);
        c.env("WINEPREFIX", prefix)
            .env("WINEDLLOVERRIDES", DLL_OVERRIDES)
            .env("WINEDEBUG", "-all");
        c
    }

    /// 构造 wineserver 命令（同前缀环境）。
    pub fn server_command(&self, prefix: &Path) -> Command {
        let mut c = Command::new(&self.server);
        c.env("WINEPREFIX", prefix)
            .env("WINEDLLOVERRIDES", DLL_OVERRIDES)
            .env("WINEDEBUG", "-all");
        c
    }

    /// `wine --version`（探测开销小，doctor/安装前展示用）。
    pub fn version(&self) -> Result<String> {
        let out = Command::new(&self.wine)
            .arg("--version")
            .env("WINEDLLOVERRIDES", DLL_OVERRIDES)
            .env("WINEDEBUG", "-all")
            .output()
            .context("wine --version 执行失败")?;
        Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
    }

    pub fn require() -> Result<WineRuntime> {
        Self::discover().ok_or_else(|| {
            anyhow!(
                "未找到 Wine 运行时。安装方式之一：\n  \
                 brew install --cask --no-quarantine $(brew info --cask wine-stable 2>/dev/null \
                 || echo wine-crossover)\n  \
                 或从 https://github.com/Gcenx/macOS_Wine_builds/releases 下载 .app 拖入 /Applications\n  \
                 已装在别处？export MWINE_WINE=/path/to/wine"
            )
        })
    }
}
