//! mwine-app：Android 应用管理。
//!
//! 职责：APK 安装/卸载/启动/列表/图标与元数据提取。
//! 基于 `mwine-adb` 原语组合；APK 本地解析（包名/版本/标签/图标）为
//! 纯 Rust 实现（zip + 二进制 AndroidManifest），**不依赖 Android SDK**——
//! 方案 A 产品形态（~150MB dmg，无 SDK）下 aapt2 不可用，本模块零 SDK
//! 也能工作；aapt2 存在时作为可选增强路径（标签/自适应图标更准）。
//!
//! 已知边界（v1）：`android:label`/`icon` 为资源引用（@string/@mipmap）时，
//! 解析需要 resources.arsc 完整解析，v1 未实现——此时 name=None、icon=None，
//! 由 UI 用包名/默认图标兜底；后续版本补最小 arsc 解析或 companion 设备侧渲染。

pub mod apk;
pub mod badging;
pub mod dumpsys;
pub mod manager;
pub mod manifest;

#[cfg(any(test, feature = "mock"))]
pub mod mock;

pub use apk::ZipArchive;
pub use badging::{find_aapt2, parse_badging, pick_raster_fallback, ApkBadging};
pub use dumpsys::{parse_dumpsys_all, parse_dumpsys_package};
pub use manager::{AdbAppManager, AdbShell, DeviceShell, ShellReply};
pub use manifest::{parse_manifest, ManifestInfo};

/// 应用图标：原始位图字节 + MIME（PNG 或 WebP）。
/// SwiftUI 侧直接 `NSImage(data:)`；不做宿主侧格式转换。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppIcon {
    pub data: Vec<u8>,
    pub mime: IconMime,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IconMime {
    Png,
    Webp,
}

impl IconMime {
    pub fn as_str(self) -> &'static str {
        match self {
            IconMime::Png => "image/png",
            IconMime::Webp => "image/webp",
        }
    }

    /// 从文件名或魔数判断格式（未知返回 None）。
    pub fn detect(path: &str, bytes: &[u8]) -> Option<IconMime> {
        let lower = path.to_ascii_lowercase();
        if lower.ends_with(".png") || bytes.starts_with(&[0x89, b'P', b'N', b'G']) {
            return Some(IconMime::Png);
        }
        if lower.ends_with(".webp")
            || (bytes.len() > 12 && &bytes[0..4] == b"RIFF" && &bytes[8..12] == b"WEBP")
        {
            return Some(IconMime::Webp);
        }
        None
    }
}

/// 单个已安装应用的元数据。
///
/// name 用 `Option<String>`：无 SDK 时 `@string` 标签
/// 暂不可解析，语义上“标签未知”比“标签=包名”更诚实，UI 用
/// [`AppInfo::display_name`] 取展示名。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AppInfo {
    pub package: String,
    /// 显示标签（APK android:label 字面量或 aapt2 解析）
    pub name: Option<String>,
    pub version: Option<String>,
    pub version_code: Option<u64>,
    pub icon: Option<AppIcon>,
    pub is_running: bool,
    /// guest 侧 base APK 路径（`pm path` 首行）
    pub apk_path: Option<String>,
    /// 启动入口 Activity（`pkg/.MainActivity` 形式，懒解析——list 不填，launch/get 填）
    pub launcher_activity: Option<String>,
    pub installed_at: Option<String>,
    pub updated_at: Option<String>,
}

impl AppInfo {
    /// UI 展示名：标签缺失时退化为包名末段（com.foo.bar → bar）。
    pub fn display_name(&self) -> &str {
        if let Some(n) = self.name.as_deref().filter(|n| !n.trim().is_empty()) {
            return n;
        }
        self.package.rsplit('.').next().unwrap_or(&self.package)
    }
}

/// 应用管理接口。
///
/// 实现约定：所有方法在设备离线时返回 [`Error::Adb`]；对不存在的包
/// 返回 [`Error::NotInstalled`]；shell 输出异常返回 [`Error::Shell`]。
pub trait AppManager {
    /// 安装 APK（`pm install -r -t`），安装后补授运行时权限并返回完整元数据。
    /// APK 文件本身会被解析（纯 Rust）以获取包名——不依赖安装输出。
    fn install(&mut self, apk_path: &Path) -> Result<AppInfo>;

    /// 卸载应用。
    fn uninstall(&mut self, package: &str) -> Result<()>;

    /// 启动应用的 launcher 入口。
    /// `display_id` 指定目标显示器（multidisplay 场景，`am start --display`）；
    /// None 启动到当前默认显示器。
    /// 优先 `cmd package resolve-activity` + `am start -n`（真实 intent 语义），
    /// 解析失败时回退 `monkey`。
    fn launch(&mut self, package: &str, display_id: Option<u32>) -> Result<()>;

    /// 列出第三方应用。`options.enrich` 控制元数据填充深度（见 [`ListOptions`]）。
    fn list(&mut self, options: &ListOptions) -> Result<Vec<AppInfo>>;

    /// 查询单个应用的完整元数据（含 launcher 入口解析）。
    /// 未安装返回 [`Error::NotInstalled`]。
    fn get(&mut self, package: &str) -> Result<AppInfo>;

    /// 强制停止应用（`am force-stop`）。
    fn stop(&mut self, package: &str) -> Result<()>;

    /// 是否已安装（仅查包列表，一次 shell 往返）。
    fn is_installed(&mut self, package: &str) -> Result<bool>;
}

/// list() 元数据填充策略：档位递增、成本递增。
///
/// Fast：3 次 shell 往返（包列表 + 全量 dumpsys + ps），拿到
/// 版本/安装时间/运行状态；标签与图标需要解析 APK 资源，不在快档提供。
#[derive(Debug, Clone, Default)]
pub struct ListOptions {
    /// 填充 version/installed_at/is_running（dumpsys + ps 批量，各一次往返）
    pub enrich: bool,
    /// 为每个应用拉取 base APK 并本地解析标签（字面量）/图标。
    /// 代价：每应用一次 pull（数 MB），仅小列表或用户主动刷新时开启。
    pub pull_metadata: bool,
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("adb: {0}")]
    Adb(#[from] mwine_adb::Error),
    #[error("shell 命令失败（{cmd}）: {output}")]
    Shell { cmd: String, output: String },
    #[error("包 {0} 未安装")]
    NotInstalled(String),
    #[error("包 {0} 没有可启动的 launcher 入口")]
    NoLauncher(String),
    #[error("APK 解析失败（{path}）: {reason}")]
    ApkParse { path: String, reason: String },
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

use std::path::Path;
