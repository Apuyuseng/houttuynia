//! ConfigStore 的 mock 实现（编码标准：每个 trait 的 mock 放 tests/mock.rs）
//! 及跨模块语义集成测试。

use mwine_adb::AdbClient;
use mwine_config::{
    default_config_path, ApplyItem, ApplyReport, ConfigError, ConfigStore, DeviceConfig,
    DeviceMode, FileConfigStore, MemoryConfigStore,
};

/// 记录调用次数的 mock：下游模块（mwine-core/UI）测试时可替换真实存储。
#[derive(Debug, Clone, Default)]
struct MockConfigStore {
    inner: MemoryConfigStore,
    loads: std::cell::Cell<usize>,
    saves: std::cell::Cell<usize>,
}

impl MockConfigStore {
    fn new() -> Self {
        Self::default()
    }
}

impl ConfigStore for MockConfigStore {
    fn load(&self) -> mwine_config::Result<DeviceConfig> {
        self.loads.set(self.loads.get() + 1);
        self.inner.load()
    }

    fn save(&self, config: &DeviceConfig) -> mwine_config::Result<()> {
        self.saves.set(self.saves.get() + 1);
        self.inner.save(config)
    }

    // apply_runtime 用 trait 默认实现（计划 → 定位设备 → 执行）
}

#[test]
fn mock_store_roundtrip_and_call_recording() {
    let mock = MockConfigStore::new();
    assert_eq!(mock.load().unwrap(), DeviceConfig::default());

    let cfg = DeviceConfig {
        model: "MWine-2".into(),
        mode: DeviceMode::Tablet,
        ..DeviceConfig::default()
    };
    mock.save(&cfg).unwrap();
    assert_eq!(mock.load().unwrap(), cfg);
    assert_eq!(mock.saves.get(), 1);
    assert_eq!(mock.loads.get(), 2);

    // mock 的 apply_runtime：无效配置在校验阶段即被拒绝
    let mut bad = cfg.clone();
    bad.cores = 99;
    let adb = AdbClient::new();
    assert!(matches!(
        mock.apply_runtime(&bad, &adb),
        Err(ConfigError::Validation(_))
    ));
}

#[test]
fn apply_runtime_maps_unreachable_adb_to_config_error() {
    // 指向必然拒绝连接的端口：find_online_device 确定性失败
    let adb = AdbClient {
        port: 1,
        ..AdbClient::default()
    };
    let mock = MockConfigStore::new();
    let err = mock
        .apply_runtime(&DeviceConfig::default(), &adb)
        .unwrap_err();
    assert!(
        matches!(&err, ConfigError::Adb { op, .. } if *op == "find_online_device"),
        "期望 Adb 错误，实际 {err:?}"
    );
}

#[test]
fn report_shape_matches_plan_semantics() {
    // 汇总结构与计划分档一致（applied/requires_restart/failed 三桶）
    let report = ApplyReport::default();
    assert!(report.applied.is_empty() && report.requires_restart.is_empty() && report.failed.is_empty());

    let plan = mwine_config::plan_apply(&DeviceConfig::default());
    let restart_keys: Vec<_> = plan
        .items
        .iter()
        .filter_map(|i| match i {
            ApplyItem::Restart { key, .. } => Some(*key),
            ApplyItem::Hot { .. } => None,
        })
        .collect();
    assert_eq!(restart_keys, vec!["memory", "cores", "brand_model"]);
}

#[test]
fn file_store_validates_before_write() {
    // 无效配置不允许落盘（即使绕过类型系统构造的路径字段）
    let store = FileConfigStore::new("/tmp/mwine-config-mock-should-not-exist.json");
    let bad = DeviceConfig {
        brand: "bad brand".into(),
        ..DeviceConfig::default()
    };
    assert!(store.save(&bad).is_err());
    assert!(!std::path::Path::new("/tmp/mwine-config-mock-should-not-exist.json").exists());
}

#[test]
fn default_path_prefers_env_override() {
    // 注意：环境变量进程级生效，此处只验证函数契约（存在即返回）
    let p = default_config_path();
    match std::env::var("MWINE_CONFIG_PATH") {
        Ok(v) => assert_eq!(p, std::path::PathBuf::from(v)),
        Err(_) => assert!(p.ends_with("Library/Application Support/MWine/config.json")),
    }
}
