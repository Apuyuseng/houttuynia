//! mwine-config：设备配置管理——持久化（JSON）+ 运行时热生效。
//!
//! 设计：
//! - DeviceConfig：品牌/型号/模式/内存/核数/代理/数据目录/设备种子/GMS
//! - ConfigStore trait：load / save / apply_runtime（trait 默认实现复用计划器）
//! - 热生效分两档：
//!   * 可热应用（settings put / wm 命令）：代理、分辨率与 DPI（模式）、Android ID
//!   * 需重启 VM：内存/核数（qemu 参数）、品牌/型号/序列号（ramdisk 注入管道）、
//!     数据目录、GMS 预装
//! - 设备伪装：device_id_seed 经确定性 PRNG（SplitMix64）派生 Android ID /
//!   序列号 / 伪 IMEI，同种子恒同值

pub mod apply;
pub mod identity;
pub mod store;

pub use apply::{execute_apply, plan_apply, ApplyItem, ApplyPlan, ApplyReport, FailedItem};
pub use identity::{derive_identity, DeviceIdentity};
pub use store::{default_config_path, FileConfigStore, MemoryConfigStore};

use std::path::PathBuf;

use mwine_adb::AdbClient;

#[derive(Debug, thiserror::Error)]
pub enum ConfigError {
    #[error("配置文件读写失败 {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("配置解析失败 {path}: {source}")]
    Parse {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("配置校验失败: {0}")]
    Validation(String),
    #[error("ADB 操作失败（{op}）: {source}")]
    Adb {
        op: &'static str,
        #[source]
        source: mwine_adb::Error,
    },
}

pub type Result<T> = std::result::Result<T, ConfigError>;

/// 设备形态（决定分辨率/DPI 预设）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DeviceMode {
    Phone,
    Tablet,
}

impl DeviceMode {
    /// 该形态的预设：(宽, 高, dpi)。经 `wm size` / `wm density` 热生效。
    pub fn preset(&self) -> (u32, u32, u32) {
        match self {
            DeviceMode::Phone => (1080, 2340, 420),
            DeviceMode::Tablet => (1600, 2560, 320),
        }
    }
}

/// 设备配置。所有字段带默认值，配置文件可缺省（前后版本兼容）。
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(default)]
pub struct DeviceConfig {
    pub brand: String,
    pub model: String,
    pub mode: DeviceMode,
    pub memory_mb: u32,
    pub cores: u32,
    /// HTTP 代理（host:port）。None = 直连。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub proxy: Option<String>,
    /// 用户数据持久化目录。None = 默认（~/Library/Application Support/MWine/）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub data_dir: Option<PathBuf>,
    /// 设备伪装随机种子（派生 Android ID/序列号/IMEI）。None = 不伪装。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub device_id_seed: Option<u64>,
    /// 是否预装 GMS。
    pub google_services: bool,
}

impl Default for DeviceConfig {
    fn default() -> Self {
        Self {
            brand: "MWine".into(),
            model: "MWine-1".into(),
            mode: DeviceMode::Phone,
            memory_mb: 1536,
            cores: 4,
            proxy: None,
            data_dir: None,
            device_id_seed: None,
            google_services: false,
        }
    }
}

impl DeviceConfig {
    /// 校验配置合法性（持久化与生效前调用）。
    ///
    /// brand/model 只允许 `[A-Za-z0-9._-]`——它们会进入 ramdisk 属性注入
    /// 管道与 shell 语境，禁空格与元字符可阻断注入。
    pub fn validate(&self) -> Result<()> {
        if !is_prop_safe(&self.brand) || !is_prop_safe(&self.model) {
            return Err(ConfigError::Validation(format!(
                "brand/model 仅允许字母数字与 . _ -（当前 brand={:?} model={:?}）",
                self.brand, self.model
            )));
        }
        if !(256..=8192).contains(&self.memory_mb) {
            return Err(ConfigError::Validation(format!(
                "memory_mb 需在 256..=8192（当前 {}）",
                self.memory_mb
            )));
        }
        if self.cores == 0 || self.cores > 16 {
            return Err(ConfigError::Validation(format!(
                "cores 需在 1..=16（当前 {}）",
                self.cores
            )));
        }
        if let Some(proxy) = &self.proxy {
            validate_proxy(proxy).map_err(ConfigError::Validation)?;
        }
        Ok(())
    }
}

/// 属性/文件语境安全字符集：字母数字与 . _ -，非空。
fn is_prop_safe(s: &str) -> bool {
    !s.is_empty()
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

/// 代理格式：host:port（host 可为域名/IPv4/[IPv6]），port 1..=65535。
fn validate_proxy(proxy: &str) -> std::result::Result<(), String> {
    let (host, port) = proxy
        .rsplit_once(':')
        .ok_or_else(|| format!("代理需为 host:port（当前 {proxy:?}）"))?;
    let port: u32 = port
        .parse()
        .map_err(|_| format!("代理端口非法（当前 {proxy:?}）"))?;
    if host.is_empty() || port == 0 || port > 65535 {
        return Err(format!("代理地址非法（当前 {proxy:?}）"));
    }
    Ok(())
}

/// 配置存取与热生效抽象。
///
/// `apply_runtime` 提供 trait 默认实现：校验 → 生成生效计划 → 定位在线设备
/// → 执行热应用项。返回 [`ApplyReport`]（额外给出明细：哪些已生效、
/// 哪些需重启后生效），不中断在失败项上。
pub trait ConfigStore {
    fn load(&self) -> Result<DeviceConfig>;
    fn save(&self, config: &DeviceConfig) -> Result<()>;
    fn apply_runtime(&self, config: &DeviceConfig, adb: &AdbClient) -> Result<ApplyReport> {
        config.validate()?;
        let plan = plan_apply(config);
        let device = adb.find_online_device().map_err(|source| ConfigError::Adb {
            op: "find_online_device",
            source,
        })?;
        Ok(execute_apply(adb, &device.serial, &plan))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_valid() {
        assert!(DeviceConfig::default().validate().is_ok());
    }

    #[test]
    fn validation_rejects_bad_values() {
        let mut cfg = DeviceConfig::default();

        cfg.memory_mb = 128;
        assert!(matches!(cfg.validate(), Err(ConfigError::Validation(_))));

        cfg.memory_mb = 9999;
        assert!(cfg.validate().is_err());

        cfg.memory_mb = 1536;
        cfg.cores = 0;
        assert!(cfg.validate().is_err());
        cfg.cores = 4;

        // brand 含空格/元字符 → 拒绝（ramdisk 注入防护）
        cfg.brand = "MWine Pro;rm -rf".into();
        assert!(cfg.validate().is_err());
        cfg.brand = "MWine_Pro-2".into();
        assert!(cfg.validate().is_ok());

        cfg.proxy = Some("127.0.0.1".into()); // 缺端口
        assert!(cfg.validate().is_err());
        cfg.proxy = Some("127.0.0.1:7890".into());
        assert!(cfg.validate().is_ok());
        cfg.proxy = Some("127.0.0.1:99999".into());
        assert!(cfg.validate().is_err());
    }

    #[test]
    fn json_roundtrip_keeps_defaults_and_omits_none() {
        let cfg = DeviceConfig::default();
        let json = serde_json::to_string(&cfg).unwrap();
        assert!(!json.contains("proxy"), "None 字段不应序列化: {json}");
        let back: DeviceConfig = serde_json::from_str(&json).unwrap();
        assert_eq!(cfg, back);

        // 空对象 → 全默认（版本兼容）
        let empty: DeviceConfig = serde_json::from_str("{}").unwrap();
        assert_eq!(empty, DeviceConfig::default());

        // 未知字段忽略（向前兼容），缺省字段取默认
        let fwd: DeviceConfig =
            serde_json::from_str(r#"{"brand":"X","future_field":1}"#).unwrap();
        assert_eq!(fwd.brand, "X");
        assert_eq!(fwd.memory_mb, DeviceConfig::default().memory_mb);
    }

    #[test]
    fn mode_presets() {
        assert_eq!(DeviceMode::Phone.preset(), (1080, 2340, 420));
        assert_eq!(DeviceMode::Tablet.preset(), (1600, 2560, 320));
    }
}
