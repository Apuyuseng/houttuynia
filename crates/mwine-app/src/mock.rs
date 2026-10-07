//! [`AppManager`] 的可编程 mock：供 mwine-core / MWineUI 并行开发与测试。
//!
//! 启用方式：`mwine-app = { path = "...", features = ["mock"] }`（本 crate
//! 单元测试经 cfg(test) 直接可用）。状态驱动——预置 installed/running 集合，
//! 操作改动集合并记录调用日志，供断言。

use std::collections::BTreeMap;
use std::path::Path;

use crate::{AppInfo, AppManager, Error, ListOptions, Result};

#[derive(Default)]
pub struct MockAppManager {
    /// package -> AppInfo（install/get/list 的数据源）
    pub apps: BTreeMap<String, AppInfo>,
    /// 模拟运行中的包
    pub running: std::collections::BTreeSet<String>,
    /// install() 接下来要"装"的包（默认按文件名推测 com.mock.<stem>）
    pub next_install_package: Option<String>,
    /// 调用日志：launch 的 (package, display_id)
    pub launch_log: Vec<(String, Option<u32>)>,
    /// uninstall 记录
    pub uninstalled_log: Vec<String>,
    /// stop 记录
    pub stopped_log: Vec<String>,
    /// 对匹配包名的操作强制失败（模拟设备错误）
    pub fail_on: Option<String>,
}

impl MockAppManager {
    pub fn new() -> Self {
        Self::default()
    }

    /// 预置一个应用。
    pub fn with_app(mut self, package: &str, name: Option<&str>) -> Self {
        self.apps.insert(
            package.to_string(),
            AppInfo {
                package: package.to_string(),
                name: name.map(|n| n.to_string()),
                version: Some("1.0.0".into()),
                ..Default::default()
            },
        );
        self
    }

    fn check_fail(&self, package: &str) -> Result<()> {
        if self.fail_on.as_deref() == Some(package) {
            return Err(Error::Shell {
                cmd: "mock failure".into(),
                output: format!("mock: 拒绝对 {package} 的操作"),
            });
        }
        Ok(())
    }
}

impl AppManager for MockAppManager {
    fn install(&mut self, apk_path: &Path) -> Result<AppInfo> {
        let package = self
            .next_install_package
            .clone()
            .unwrap_or_else(|| {
                let stem = apk_path
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .unwrap_or("app");
                format!("com.mock.{stem}")
            });
        self.check_fail(&package)?;
        let info = AppInfo {
            package: package.clone(),
            name: Some(package.clone()),
            version: Some("1.0.0-mock".into()),
            ..Default::default()
        };
        self.apps.insert(package.clone(), info.clone());
        Ok(info)
    }

    fn uninstall(&mut self, package: &str) -> Result<()> {
        self.check_fail(package)?;
        self.apps
            .remove(package)
            .ok_or_else(|| Error::NotInstalled(package.to_string()))?;
        self.running.remove(package);
        self.uninstalled_log.push(package.to_string());
        Ok(())
    }

    fn launch(&mut self, package: &str, display_id: Option<u32>) -> Result<()> {
        self.check_fail(package)?;
        if !self.apps.contains_key(package) {
            return Err(Error::NotInstalled(package.to_string()));
        }
        self.running.insert(package.to_string());
        self.launch_log.push((package.to_string(), display_id));
        Ok(())
    }

    fn list(&mut self, options: &ListOptions) -> Result<Vec<AppInfo>> {
        Ok(self
            .apps
            .values()
            .map(|a| {
                let mut a = a.clone();
                a.is_running = options.enrich && self.running.contains(&a.package);
                a
            })
            .collect())
    }

    fn get(&mut self, package: &str) -> Result<AppInfo> {
        self.apps
            .get(package)
            .cloned()
            .ok_or_else(|| Error::NotInstalled(package.to_string()))
    }

    fn stop(&mut self, package: &str) -> Result<()> {
        self.check_fail(package)?;
        self.running.remove(package);
        self.stopped_log.push(package.to_string());
        Ok(())
    }

    fn is_installed(&mut self, package: &str) -> Result<bool> {
        Ok(self.apps.contains_key(package))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mock_install_launch_uninstall_cycle() {
        let mut m = MockAppManager::new();
        let apk = Path::new("/tmp/demo.apk");
        let info = m.install(apk).unwrap();
        assert_eq!(info.package, "com.mock.demo");
        assert!(m.is_installed("com.mock.demo").unwrap());

        m.launch("com.mock.demo", Some(2)).unwrap();
        assert_eq!(m.launch_log, vec![("com.mock.demo".into(), Some(2))]);
        let apps = m.list(&ListOptions { enrich: true, pull_metadata: false }).unwrap();
        assert!(apps[0].is_running);

        m.stop("com.mock.demo").unwrap();
        m.uninstall("com.mock.demo").unwrap();
        assert!(!m.is_installed("com.mock.demo").unwrap());
        assert!(m.get("com.mock.demo").is_err());
    }

    #[test]
    fn mock_missing_package_not_installed() {
        let mut m = MockAppManager::new();
        assert!(matches!(
            m.launch("com.nothere", None),
            Err(Error::NotInstalled(_))
        ));
        assert!(matches!(
            m.uninstall("com.nothere"),
            Err(Error::NotInstalled(_))
        ));
    }

    #[test]
    fn mock_fail_on_simulation() {
        let mut m = MockAppManager::new().with_app("com.a.app", Some("A"));
        m.fail_on = Some("com.a.app".into());
        assert!(m.launch("com.a.app", None).is_err());
        assert!(m.uninstall("com.a.app").is_err());
    }
}
