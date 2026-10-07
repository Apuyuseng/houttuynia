//! 配置持久化实现：JSON 文件（默认，原子写）与内存（测试/下游 mock）。

use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use crate::{ConfigError, ConfigStore, DeviceConfig, Result};

/// 默认配置文件路径。
///
/// 优先取 `MWINE_CONFIG_PATH` 环境变量（守护进程/测试注入用），
/// 否则 `~/Library/Application Support/MWine/config.json`。
pub fn default_config_path() -> PathBuf {
    if let Ok(p) = std::env::var("MWINE_CONFIG_PATH") {
        return PathBuf::from(p);
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    PathBuf::from(home)
        .join("Library/Application Support/MWine/config.json")
}

/// JSON 文件存储。
///
/// - load：文件不存在 → 返回默认配置（首跑语义）；存在则解析 + 校验
/// - save：先校验，写同目录临时文件后原子 rename（避免半写损坏）
#[derive(Debug, Clone)]
pub struct FileConfigStore {
    path: PathBuf,
}

impl FileConfigStore {
    pub fn new(path: impl Into<PathBuf>) -> Self {
        Self { path: path.into() }
    }

    pub fn path(&self) -> &std::path::Path {
        &self.path
    }
}

impl ConfigStore for FileConfigStore {
    fn load(&self) -> Result<DeviceConfig> {
        match std::fs::read_to_string(&self.path) {
            Ok(text) => {
                let cfg: DeviceConfig = serde_json::from_str(&text).map_err(|source| {
                    ConfigError::Parse {
                        path: self.path.clone(),
                        source,
                    }
                })?;
                cfg.validate()?;
                Ok(cfg)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                Ok(DeviceConfig::default())
            }
            Err(source) => Err(ConfigError::Io {
                path: self.path.clone(),
                source,
            }),
        }
    }

    fn save(&self, config: &DeviceConfig) -> Result<()> {
        config.validate()?;
        if let Some(dir) = self.path.parent() {
            std::fs::create_dir_all(dir).map_err(|source| ConfigError::Io {
                path: dir.to_path_buf(),
                source,
            })?;
        }
        let text = serde_json::to_string_pretty(config)
            .map_err(|source| ConfigError::Parse {
                path: self.path.clone(),
                source,
            })?;
        let tmp = self.path.with_extension("json.tmp");
        std::fs::write(&tmp, text).map_err(|source| ConfigError::Io {
            path: tmp.clone(),
            source,
        })?;
        std::fs::rename(&tmp, &self.path).map_err(|source| ConfigError::Io {
            path: self.path.clone(),
            source,
        })?;
        Ok(())
    }
}

/// 内存存储：测试与下游模块 mock 用（也是本模块标准的参考 mock 实现）。
#[derive(Debug, Clone, Default)]
pub struct MemoryConfigStore {
    state: Arc<Mutex<Option<DeviceConfig>>>,
}

impl MemoryConfigStore {
    pub fn new() -> Self {
        Self::default()
    }
}

impl ConfigStore for MemoryConfigStore {
    fn load(&self) -> Result<DeviceConfig> {
        let guard = self.state.lock().expect("MemoryConfigStore 锁中毒");
        Ok(guard.clone().unwrap_or_default())
    }

    fn save(&self, config: &DeviceConfig) -> Result<()> {
        config.validate()?;
        let mut guard = self.state.lock().expect("MemoryConfigStore 锁中毒");
        *guard = Some(config.clone());
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 轻量临时目录（避免引 tempfile 依赖）：temp/ + 进程号 + 计数器。
    struct TempDir(PathBuf);
    static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    impl TempDir {
        fn new() -> Self {
            let n = COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            let p = std::env::temp_dir().join(format!(
                "mwine-config-test-{}-{n}",
                std::process::id()
            ));
            std::fs::create_dir_all(&p).unwrap();
            Self(p)
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn file_store_roundtrip_and_first_run_default() {
        let dir = TempDir::new();
        let store = FileConfigStore::new(dir.0.join("nested/config.json"));

        // 首跑（文件不存在）→ 默认配置
        assert_eq!(store.load().unwrap(), DeviceConfig::default());

        let cfg = DeviceConfig {
            proxy: Some("10.0.0.8:8888".into()),
            mode: crate::DeviceMode::Tablet,
            memory_mb: 4096,
            ..DeviceConfig::default()
        };
        store.save(&cfg).unwrap();
        assert_eq!(store.load().unwrap(), cfg);

        // save 不留临时文件
        assert!(!dir.0.join("nested/config.json.tmp").exists());
    }

    #[test]
    fn file_store_rejects_invalid_on_save_and_tampered_on_load() {
        let dir = TempDir::new();
        let store = FileConfigStore::new(dir.0.join("config.json"));

        let mut bad = DeviceConfig::default();
        bad.memory_mb = 999_999;
        assert!(store.save(&bad).is_err());

        // 篡改文件（越界内存）→ load 校验拒绝
        std::fs::write(
            store.path(),
            r#"{"memory_mb": 999999, "brand": "MWine", "model": "MWine-1"}"#,
        )
        .unwrap();
        assert!(store.load().is_err());

        // 损坏 JSON → Parse 错误
        std::fs::write(store.path(), "{not json").unwrap();
        assert!(matches!(store.load(), Err(ConfigError::Parse { .. })));
    }

    #[test]
    fn memory_store_isolated_clones_share_state() {
        let store = MemoryConfigStore::new();
        assert_eq!(store.load().unwrap(), DeviceConfig::default());
        let cfg = DeviceConfig {
            device_id_seed: Some(123),
            ..DeviceConfig::default()
        };
        store.save(&cfg).unwrap();
        let clone = store.clone();
        assert_eq!(clone.load().unwrap(), cfg);
    }
}
