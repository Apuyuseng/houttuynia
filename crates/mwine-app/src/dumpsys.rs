//! 设备侧输出解析（纯函数，便于离线单测）：
//! - `dumpsys package`（单包 / 全量）→ 版本与安装时间
//! - `ps -A -o NAME` → 运行中进程集合
//! - `cmd package resolve-activity --brief` → 启动入口组件

use std::collections::{BTreeMap, HashSet};

/// dumpsys 中我们关心的包级字段。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PackageDump {
    pub version_name: Option<String>,
    pub version_code: Option<u64>,
    pub first_install_time: Option<String>,
    pub last_update_time: Option<String>,
}

/// 解析 `dumpsys package <pkg>`（单包）输出。
pub fn parse_dumpsys_package(text: &str) -> PackageDump {
    parse_dumpsys_all(text)
        .remove(&section_key(text))
        .unwrap_or_default()
}

/// 解析 `dumpsys package`（全量）输出：包名 → 字段。
/// 以 `Package [name]` 行分段；非 package 段落（Providers 等）天然跳过。
pub fn parse_dumpsys_all(text: &str) -> BTreeMap<String, PackageDump> {
    let mut map = BTreeMap::new();
    let mut current: Option<String> = None;
    let mut dump = PackageDump::default();
    for line in text.lines() {
        if let Some(rest) = line.trim_start().strip_prefix("Package [") {
            if let Some(name) = rest.split(']').next() {
                if let Some(prev) = current.take() {
                    map.insert(prev, std::mem::take(&mut dump));
                }
                current = Some(name.to_string());
                continue;
            }
        }
        if current.is_none() {
            continue;
        }
        let t = line.trim();
        if dump.version_name.is_none() {
            if let Some(v) = t.strip_prefix("versionName=") {
                dump.version_name = non_empty(v);
            }
        }
        if dump.version_code.is_none() {
            // 形如 `versionCode=42 minSdk=24 targetSdk=34`
            if let Some(rest) = t.strip_prefix("versionCode=") {
                dump.version_code = rest
                    .split_whitespace()
                    .next()
                    .and_then(|v| v.parse().ok());
            }
        }
        if dump.first_install_time.is_none() {
            if let Some(v) = t.strip_prefix("firstInstallTime=") {
                dump.first_install_time = non_empty(v);
            }
        }
        if dump.last_update_time.is_none() {
            if let Some(v) = t.strip_prefix("lastUpdateTime=") {
                dump.last_update_time = non_empty(v);
            }
        }
    }
    if let Some(prev) = current {
        map.insert(prev, dump);
    }
    map
}

fn non_empty(v: &str) -> Option<String> {
    let v = v.trim();
    (!v.is_empty() && v != "null").then(|| v.to_string())
}

/// 从 dumpsys 文本取“当前段”包名（供单包解析复用分段键）。
fn section_key(text: &str) -> String {
    text.lines()
        .find_map(|l| {
            l.trim_start()
                .strip_prefix("Package [")
                .and_then(|r| r.split(']').next())
                .map(|s| s.to_string())
        })
        .unwrap_or_default()
}

/// 解析 `ps -A -o NAME` 输出为进程名集合（忽略表头）。
pub fn parse_process_names(text: &str) -> HashSet<String> {
    text.lines()
        .skip(1) // NAME 表头
        .map(|l| l.trim())
        .filter(|l| !l.is_empty())
        .map(|l| l.split_whitespace().next().unwrap_or(l).to_string())
        .collect()
}

/// 判断包是否有进程在运行：进程名等于包名或以 `包名:` / `包名.` 开头
/// （多进程应用形如 `pkg:service`）。
pub fn package_running(procs: &HashSet<String>, package: &str) -> bool {
    procs.iter().any(|p| {
        p == package
            || p.strip_prefix(package)
                .is_some_and(|rest| rest.starts_with(':') || rest.starts_with('.'))
    })
}

/// 从 `cmd package resolve-activity --brief <pkg>` 输出提取入口组件
///（最后一行含 '/' 的行；形如 `com.pkg/.MainActivity`）。
pub fn parse_resolve_activity(text: &str) -> Option<String> {
    text.lines()
        .map(|l| l.trim())
        .rfind(|l| l.contains('/') && !l.contains(' '))
        .map(|l| l.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    const SINGLE: &str = r#"Package [com.example.app] (0x123 /data/app/~~abc==/com.example.app-xyz==/base.apk)
  userId=10123
  pkg=Package{... com.example.app}
  versionCode=42 minSdk=24 targetSdk=34
  versionName=1.2.3
  firstInstallTime=2026-10-01 12:00:00
  lastUpdateTime=2026-10-02 13:00:00
  installerPackageName=null
  signatures=..."#;

    #[test]
    fn parse_single_package() {
        let d = parse_dumpsys_package(SINGLE);
        assert_eq!(d.version_name.as_deref(), Some("1.2.3"));
        assert_eq!(d.version_code, Some(42));
        assert_eq!(d.first_install_time.as_deref(), Some("2026-10-01 12:00:00"));
        assert_eq!(d.last_update_time.as_deref(), Some("2026-10-02 13:00:00"));
    }

    #[test]
    fn parse_all_packages_sections() {
        let all = format!("{SINGLE}\n{other}", other = SINGLE.replace("com.example.app", "com.other.app").replace("1.2.3", "9.9"));
        let map = parse_dumpsys_all(&all);
        assert_eq!(map.len(), 2);
        assert_eq!(map["com.example.app"].version_name.as_deref(), Some("1.2.3"));
        assert_eq!(map["com.other.app"].version_name.as_deref(), Some("9.9"));
    }

    #[test]
    fn process_names_and_running() {
        let ps = "NAME\nsystem\ncom.example.app\ncom.example.app:service\ncom.other\n";
        let procs = parse_process_names(ps);
        assert!(package_running(&procs, "com.example.app"));
        assert!(!package_running(&procs, "com.notinstalled"));
        // 前缀相同但不同包（com.example.appx）不算运行
        let procs2 = parse_process_names("NAME\ncom.example.appx\n");
        assert!(!package_running(&procs2, "com.example.app"));
    }

    #[test]
    fn resolve_activity_picks_component_line() {
        let out = "priority=0 preferredOrder=0 match=0x00108000 specificIndex=-1 isDefault=true\ncom.example.app/.MainActivity\n";
        assert_eq!(
            parse_resolve_activity(out).as_deref(),
            Some("com.example.app/.MainActivity")
        );
        // 无入口：全部行都含空格（如 "No activity found"）
        assert_eq!(parse_resolve_activity("No activity found\n"), None);
    }
}
