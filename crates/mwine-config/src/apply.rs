//! 配置生效：计划（纯函数，可测）+ 执行（adb shell）。
//!
//! 热生效分档：
//! - 热应用：`settings put`（代理 / android_id）与 `wm`（分辨率 / density）
//! - 需重启：qemu 启动参数（内存 / 核数）、ramdisk 属性注入（品牌 / 型号 /
//!   序列号，ro.* 运行期只读）、data 分区与 GMS 预装

use std::time::Duration;

use mwine_adb::AdbClient;

use crate::identity::derive_identity;
use crate::DeviceConfig;

/// 单项配置的生效路径。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ApplyItem {
    /// 可热应用：key + 依次执行的 shell 命令（任一失败则该项记入 failed）。
    Hot {
        key: &'static str,
        commands: Vec<String>,
    },
    /// 需重启 VM 才生效：key + 原因（供 UI 提示）。
    Restart {
        key: &'static str,
        reason: &'static str,
    },
}

/// 生效计划（`plan_apply` 产出，纯数据）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApplyPlan {
    pub items: Vec<ApplyItem>,
}

/// 执行结果摘要。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ApplyReport {
    /// 已热应用成功的配置项 key。
    pub applied: Vec<&'static str>,
    /// 需重启后生效的配置项 key（执行器不做处理，报告给调用方）。
    pub requires_restart: Vec<&'static str>,
    /// 热应用失败的项。
    pub failed: Vec<FailedItem>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FailedItem {
    pub key: &'static str,
    pub error: String,
}

/// 生成生效计划（纯函数）。
///
/// 命令内插值均来自已校验的配置字段（代理 host:port、十六进制 android_id、
/// 数字分辨率），无 shell 元字符可达。
pub fn plan_apply(config: &DeviceConfig) -> ApplyPlan {
    let mut items = Vec::new();

    // 代理：设置 / 清除（":0" 为 Android 清除 http_proxy 的惯用写法）
    let proxy_cmd = match &config.proxy {
        Some(p) => format!("settings put global http_proxy {p}"),
        None => "settings put global http_proxy :0".to_string(),
    };
    items.push(ApplyItem::Hot {
        key: "proxy",
        commands: vec![proxy_cmd],
    });

    // 模式：分辨率 + density
    let (w, h, dpi) = config.mode.preset();
    items.push(ApplyItem::Hot {
        key: "display_mode",
        commands: vec![
            format!("wm size {w}x{h}"),
            format!("wm density {dpi}"),
        ],
    });

    // 设备伪装：android_id 可热写；序列号/IMEI 是 ro.*/nvram 语义，随下次启动注入
    if let Some(seed) = config.device_id_seed {
        let id = derive_identity(seed);
        items.push(ApplyItem::Hot {
            key: "android_id",
            commands: vec![format!("settings put secure android_id {}", id.android_id)],
        });
        items.push(ApplyItem::Restart {
            key: "device_serial",
            reason: "序列号/IMEI 为只读属性，经 ramdisk 注入于下次启动生效",
        });
    }

    // 启动期参数
    items.push(ApplyItem::Restart {
        key: "memory",
        reason: "guest 内存为 qemu 启动参数",
    });
    items.push(ApplyItem::Restart {
        key: "cores",
        reason: "vCPU 数为 qemu 启动参数",
    });
    items.push(ApplyItem::Restart {
        key: "brand_model",
        reason: "品牌/型号为只读属性，经 ramdisk 注入于下次启动生效",
    });
    if config.data_dir.is_some() {
        items.push(ApplyItem::Restart {
            key: "data_dir",
            reason: "数据目录决定 data 分区挂载",
        });
    }
    if config.google_services {
        items.push(ApplyItem::Restart {
            key: "google_services",
            reason: "GMS 预装属镜像/system 侧变更",
        });
    }

    ApplyPlan { items }
}

/// 按计划经 adb 执行热应用项。
///
/// 逐项执行、失败不中断；全部结果汇总进 [`ApplyReport`]。
/// `exit_code == None`（老设备 v1 shell 无退出码）视为成功（尽力而为语义）。
pub fn execute_apply(adb: &AdbClient, serial: &str, plan: &ApplyPlan) -> ApplyReport {
    let mut report = ApplyReport::default();
    for item in &plan.items {
        match item {
            ApplyItem::Hot { key, commands } => {
                let mut last_err = None;
                for cmd in commands {
                    match adb.shell_blocking(serial, cmd, Duration::from_secs(10)) {
                        Ok(out) => {
                            let failed = matches!(out.exit_code, Some(code) if code != 0);
                            if failed {
                                let stderr = String::from_utf8_lossy(&out.stderr);
                                let stdout = String::from_utf8_lossy(&out.stdout);
                                last_err = Some(format!(
                                    "`{cmd}` 退出码 {:?}：{}{}",
                                    out.exit_code,
                                    stderr.trim(),
                                    stdout.trim()
                                ));
                            }
                        }
                        Err(e) => last_err = Some(format!("`{cmd}` 通信失败: {e}")),
                    }
                }
                match last_err {
                    Some(error) => report.failed.push(FailedItem { key, error }),
                    None => report.applied.push(key),
                }
            }
            ApplyItem::Restart { key, .. } => report.requires_restart.push(key),
        }
    }
    report
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::DeviceMode;

    fn keys(plan: &ApplyPlan) -> Vec<&'static str> {
        plan.items
            .iter()
            .map(|i| match i {
                ApplyItem::Hot { key, .. } => *key,
                ApplyItem::Restart { key, .. } => *key,
            })
            .collect()
    }

    #[test]
    fn default_config_hot_items_and_restart_items() {
        let plan = plan_apply(&DeviceConfig::default());
        assert_eq!(keys(&plan), vec!["proxy", "display_mode", "memory", "cores", "brand_model"]);

        // 默认无代理 → 清除命令
        assert!(matches!(
            &plan.items[0],
            ApplyItem::Hot { commands, .. } if commands == &vec!["settings put global http_proxy :0".to_string()]
        ));

        // 模式 → wm size + wm density
        let (w, h, dpi) = DeviceMode::Phone.preset();
        assert!(matches!(
            &plan.items[1],
            ApplyItem::Hot { key: "display_mode", commands }
                if commands == &vec![format!("wm size {w}x{h}"), format!("wm density {dpi}")]
        ));
    }

    #[test]
    fn proxy_some_produces_set_command() {
        let cfg = DeviceConfig {
            proxy: Some("127.0.0.1:7890".into()),
            ..DeviceConfig::default()
        };
        cfg.validate().unwrap();
        let plan = plan_apply(&cfg);
        assert!(matches!(
            &plan.items[0],
            ApplyItem::Hot { key: "proxy", commands }
                if commands == &vec!["settings put global http_proxy 127.0.0.1:7890".to_string()]
        ));
    }

    #[test]
    fn seed_adds_android_id_hot_and_serial_restart() {
        let cfg = DeviceConfig {
            device_id_seed: Some(7),
            data_dir: Some("/data/foo".into()),
            google_services: true,
            ..DeviceConfig::default()
        };
        let plan = plan_apply(&cfg);
        let ks = keys(&plan);
        assert!(ks.contains(&"android_id") && ks.contains(&"device_serial"));
        assert!(ks.contains(&"data_dir") && ks.contains(&"google_services"));

        // android_id 命令携带确定性派生值
        let id = crate::identity::derive_identity(7);
        assert!(plan.items.iter().any(|i| matches!(
            i,
            ApplyItem::Hot { key: "android_id", commands }
                if commands == &vec![format!("settings put secure android_id {}", id.android_id)]
        )));
    }

    #[test]
    fn plan_is_pure_and_deterministic() {
        let cfg = DeviceConfig::default();
        assert_eq!(plan_apply(&cfg), plan_apply(&cfg));
    }
}
