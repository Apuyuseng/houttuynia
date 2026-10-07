//! [`AppManager`] 的 ADB 组合实现：包装 `mwine-adb` 原语 + 设备 shell 输出解析。
//!
//! shell 访问经 [`DeviceShell`] 抽象（真实实现走 AdbClient；测试注入脚本化
//! mock），使 list/launch/uninstall 的全部决策逻辑可离线单测。

use std::collections::HashSet;
use std::path::Path;
use std::time::Duration;

use mwine_adb::AdbClient;

use crate::apk::ZipArchive;
use crate::badging::{self, ApkBadging};
use crate::dumpsys::{self, package_running, parse_process_names, parse_resolve_activity};
use crate::manifest::parse_manifest;
use crate::{
    AppIcon, AppInfo, AppManager, Error, IconMime, ListOptions, Result,
};

/// shell 执行结果（stdout/stderr 合流 + 退出码是否为 0）。
pub struct ShellReply {
    pub out: String,
    pub ok: bool,
}

/// 设备 shell 抽象：生产为 ADB shell；测试为脚本回放。
pub trait DeviceShell: Send {
    fn run(&mut self, cmd: &str, timeout: Duration) -> Result<ShellReply>;
}

/// 生产实现：经 AdbClient shell_blocking（v2 协议）。
pub struct AdbShell {
    pub client: AdbClient,
    pub serial: String,
}

impl DeviceShell for AdbShell {
    fn run(&mut self, cmd: &str, timeout: Duration) -> Result<ShellReply> {
        let out = self.client.shell_blocking(&self.serial, cmd, timeout)?;
        let mut text = out.stdout_string();
        if !out.stderr.is_empty() {
            if !text.is_empty() && !text.ends_with('\n') {
                text.push('\n');
            }
            text.push_str(&String::from_utf8_lossy(&out.stderr));
        }
        Ok(ShellReply {
            out: text,
            ok: out.ok(),
        })
    }
}

/// ADB 组合实现。
///
/// `AdbAppManager::new(client, serial)` 绑定一台设备；`with_shell` 供测试注入。
pub struct AdbAppManager {
    client: AdbClient,
    pub serial: String,
    shell: Box<dyn DeviceShell>,
}

impl AdbAppManager {
    pub fn new(client: AdbClient, serial: impl Into<String>) -> Self {
        let serial = serial.into();
        Self {
            client: client.clone(),
            serial: serial.clone(),
            shell: Box::new(AdbShell { client, serial }),
        }
    }

    /// 测试入口：注入脚本化 shell。生产代码不要用。
    #[doc(hidden)]
    pub fn with_shell(client: AdbClient, serial: impl Into<String>, shell: Box<dyn DeviceShell>) -> Self {
        Self {
            client,
            serial: serial.into(),
            shell,
        }
    }

    fn shell(&mut self, cmd: &str, timeout: Duration) -> Result<ShellReply> {
        self.shell.run(cmd, timeout)
    }

    fn shell_ok(&mut self, cmd: &str, timeout: Duration) -> Result<String> {
        let reply = self.shell(cmd, timeout)?;
        if !reply.ok {
            return Err(Error::Shell {
                cmd: cmd.to_string(),
                output: reply.out.trim().to_string(),
            });
        }
        Ok(reply.out)
    }

    /// 解析本地 APK 元数据：纯 Rust 路径（zip + AXML）为主，
    /// aapt2 存在时叠加 badging（标签引用/自适应图标更准）。
    pub fn read_local_apk(apk_path: &Path) -> Result<(crate::manifest::ManifestInfo, Option<ApkBadging>)> {
        let label = apk_path.display().to_string();
        let zip = ZipArchive::open(apk_path)?;
        let axml = zip
            .read_entry("AndroidManifest.xml")?
            .ok_or_else(|| Error::ApkParse {
                reason: "缺少 AndroidManifest.xml".into(),
                path: label.clone(),
            })?;
        let info = parse_manifest(&axml, &label)?;
        let badging = badging::find_aapt2().and_then(|tool| badging::aapt2_badging(&tool, apk_path));
        Ok((info, badging))
    }

    /// 从 APK zip 内挑启动图标：优先 badging 给出的 entry（xml 则回退同名位图），
    /// 否则密度目录启发式扫描（无 arsc 时的尽力而为）。
    fn icon_from_zip(zip: &ZipArchive, badging: Option<&ApkBadging>) -> Option<AppIcon> {
        let entry = badging
            .and_then(|b| b.icon.clone())
            .map(|icon| {
                if icon.ends_with(".xml") {
                    let entries: Vec<&str> = zip.entry_names().collect();
                    badging::pick_raster_fallback(&entries, &icon).unwrap_or(icon)
                } else {
                    icon
                }
            })
            .or_else(|| {
                let entries: Vec<&str> = zip.entry_names().collect();
                heuristic_icon(&entries)
            })?;
        let bytes = zip.read_entry(&entry).ok().flatten()?;
        let mime = IconMime::detect(&entry, &bytes)?;
        Some(AppIcon {
            data: bytes,
            mime,
        })
    }

    fn launcher_of(&mut self, package: &str) -> Result<Option<String>> {
        let out = self.shell(
            &format!("cmd package resolve-activity --brief {package} | tail -1"),
            Duration::from_secs(10),
        )?;
        Ok(parse_resolve_activity(&out.out))
    }

    fn base_apk_path(&mut self, package: &str) -> Result<Option<String>> {
        let out = self.shell(&format!("pm path {package}"), Duration::from_secs(15))?;
        Ok(out
            .out
            .lines()
            .find_map(|l| l.trim().strip_prefix("package:").map(|s| s.to_string())))
    }

    /// 拉取 base APK 并本地解析标签/图标（list 的 pull_metadata 档、get 的富化）。
    fn pull_metadata(&mut self, info: &mut AppInfo) {
        let Some(remote) = self.base_apk_path(&info.package).ok().flatten() else {
            return;
        };
        let bytes = match self.client.pull_file(&self.serial, &remote) {
            Ok(b) => b,
            Err(_) => return,
        };
        let zip = match ZipArchive::from_bytes(bytes, info.package.clone()) {
            Ok(z) => z,
            Err(_) => return,
        };
        let Some(axml) = zip.read_entry("AndroidManifest.xml").ok().flatten() else {
            return;
        };
        if let Ok(manifest) = parse_manifest(&axml, &info.package) {
            if info.name.is_none() {
                info.name = manifest.label.clone();
            }
            if info.version.is_none() {
                info.version = manifest.version_name.clone();
            }
            if info.version_code.is_none() {
                info.version_code = manifest.version_code;
            }
        }
        if info.icon.is_none() {
            info.icon = Self::icon_from_zip(&zip, None);
        }
    }

    fn get_fast(&mut self, package: &str) -> Result<AppInfo> {
        let mut info = AppInfo {
            package: package.to_string(),
            ..Default::default()
        };
        let out = self.shell_ok(&format!("dumpsys package {package}"), Duration::from_secs(30))?;
        let dump = dumpsys::parse_dumpsys_package(&out);
        info.version = dump.version_name;
        info.version_code = dump.version_code;
        info.installed_at = dump.first_install_time;
        info.updated_at = dump.last_update_time;
        let ps = self.shell("ps -A -o NAME", Duration::from_secs(15))?;
        info.is_running = package_running(&parse_process_names(&ps.out), package);
        info.apk_path = self.base_apk_path(package)?;
        info.launcher_activity = self.launcher_of(package)?;
        if info.launcher_activity.is_none() {
            // 已安装但无 launcher 入口（纯服务应用）不报错，交由调用方判断
            let installed = self.is_installed(package)?;
            if !installed {
                return Err(Error::NotInstalled(package.to_string()));
            }
        }
        Ok(info)
    }
}

/// 无 arsc 时的图标启发式：res/ 下名字含 launcher/icon 的最高密度位图。
fn heuristic_icon(entries: &[&str]) -> Option<String> {
    let density_of = |path: &str| -> u32 {
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
        }
        0
    };
    let mut best: Option<(u32, &str)> = None;
    for e in entries {
        let lower = e.to_ascii_lowercase();
        if !e.starts_with("res/")
            || !(lower.contains("launcher") || lower.contains("/icon"))
            || !(lower.ends_with(".png") || lower.ends_with(".webp"))
        {
            continue;
        }
        let d = density_of(e);
        if best.as_ref().is_none_or(|(bd, _)| d > *bd) {
            best = Some((d, e));
        }
    }
    best.map(|(_, e)| e.to_string())
}

impl AppManager for AdbAppManager {
    fn install(&mut self, apk_path: &Path) -> Result<AppInfo> {
        self.install_with_progress(apk_path, |_| {})
    }

    fn uninstall(&mut self, package: &str) -> Result<()> {
        if !self.is_installed(package)? {
            return Err(Error::NotInstalled(package.to_string()));
        }
        let out = self.shell(
            &format!("pm uninstall {package}"),
            Duration::from_secs(60),
        )?;
        if !out.out.contains("Success") {
            return Err(Error::Shell {
                cmd: format!("pm uninstall {package}"),
                output: out.out.trim().to_string(),
            });
        }
        Ok(())
    }

    fn launch(&mut self, package: &str, display_id: Option<u32>) -> Result<()> {
        if !self.is_installed(package)? {
            return Err(Error::NotInstalled(package.to_string()));
        }
        let comp = self.launcher_of(package)?;
        if let Some(comp) = comp {
            let display = display_id
                .map(|id| format!("--display {id} "))
                .unwrap_or_default();
            let out = self.shell(
                &format!("am start {display}-n '{comp}'"),
                Duration::from_secs(20),
            )?;
            // am 对已存在的 Error 仍可能退出 0：按输出兜底判定
            if out.ok && !out.out.contains("Error") {
                return Ok(());
            }
        }
        // 回退：monkey（不依赖入口解析；--display 不适用则丢弃定向）
        let _ = display_id;
        let out = self.shell(
            &format!("monkey -p {package} -c android.intent.category.LAUNCHER 1"),
            Duration::from_secs(30),
        )?;
        if out.out.contains("aborted") || !out.ok {
            return Err(Error::NoLauncher(package.to_string()));
        }
        Ok(())
    }

    fn list(&mut self, options: &ListOptions) -> Result<Vec<AppInfo>> {
        let out = self.shell_ok("pm list packages -3", Duration::from_secs(30))?;
        let packages: Vec<String> = out
            .lines()
            .filter_map(|l| l.trim().strip_prefix("package:").map(|s| s.to_string()))
            .collect();

        let mut dumps = std::collections::BTreeMap::new();
        let mut procs = HashSet::new();
        if options.enrich || options.pull_metadata {
            if let Ok(all) =
                self.shell("dumpsys package", Duration::from_secs(90))
            {
                dumps = dumpsys::parse_dumpsys_all(&all.out);
            }
        }
        if options.enrich {
            if let Ok(ps) = self.shell("ps -A -o NAME", Duration::from_secs(15)) {
                procs = parse_process_names(&ps.out);
            }
        }

        let mut result = Vec::with_capacity(packages.len());
        for pkg in packages {
            let dump = dumps.get(&pkg);
            let mut info = AppInfo {
                package: pkg.clone(),
                version: dump.and_then(|d| d.version_name.clone()),
                version_code: dump.and_then(|d| d.version_code),
                installed_at: dump.and_then(|d| d.first_install_time.clone()),
                updated_at: dump.and_then(|d| d.last_update_time.clone()),
                is_running: package_running(&procs, &pkg),
                ..Default::default()
            };
            if options.pull_metadata {
                self.pull_metadata(&mut info);
            }
            result.push(info);
        }
        Ok(result)
    }

    fn get(&mut self, package: &str) -> Result<AppInfo> {
        let mut info = self.get_fast(package)?;
        // 标签/图标不常变，get() 全量补齐（每包一次 pull，量级可控）
        if info.name.is_none() || info.icon.is_none() {
            self.pull_metadata(&mut info);
        }
        Ok(info)
    }

    fn stop(&mut self, package: &str) -> Result<()> {
        let out = self.shell(
            &format!("am force-stop {package}"),
            Duration::from_secs(15),
        )?;
        if !out.ok {
            return Err(Error::Shell {
                cmd: format!("am force-stop {package}"),
                output: out.out.trim().to_string(),
            });
        }
        Ok(())
    }

    fn is_installed(&mut self, package: &str) -> Result<bool> {
        let out = self.shell(
            &format!("pm list packages {package}"),
            Duration::from_secs(20),
        )?;
        Ok(out
            .out
            .lines()
            .any(|l| l.trim() == format!("package:{package}")))
    }
}

impl AdbAppManager {
    /// install 的进度版：进度回调 + 安装后补授运行时权限。
    pub fn install_with_progress(
        &mut self,
        apk_path: &Path,
        on_progress: impl FnMut(mwine_adb::InstallProgress),
    ) -> Result<AppInfo> {
        let (manifest, badging) = Self::read_local_apk(apk_path)?;
        let package = manifest.package.clone();

        self.client.install_apk_with_progress(
            &self.serial,
            apk_path,
            Duration::from_secs(600),
            on_progress,
        )?;
        // 扫码/存储等运行时权限默认拒绝，模拟器场景统一补授
        let _ = self.client.grant_runtime_permissions(&self.serial, &package);

        let mut info = self.get_fast(&package)?;
        if info.name.is_none() {
            info.name = manifest
                .label
                .clone()
                .or_else(|| badging.as_ref().and_then(|b| b.label.clone()));
        }
        if info.icon.is_none() {
            if let Ok(zip) = ZipArchive::open(apk_path) {
                info.icon = Self::icon_from_zip(&zip, badging.as_ref());
            }
        }
        Ok(info)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    /// 脚本化 shell：按顺序弹出预设回复；耗尽后返回错误。
    struct ScriptedShell {
        replies: VecDeque<Result<ShellReply>>,
    }

    impl ScriptedShell {
        fn new(replies: Vec<Result<ShellReply>>) -> Self {
            Self {
                replies: replies.into(),
            }
        }

        fn ok(out: &str) -> Result<ShellReply> {
            Ok(ShellReply {
                out: out.to_string(),
                ok: true,
            })
        }
    }

    impl DeviceShell for ScriptedShell {
        fn run(&mut self, _cmd: &str, _timeout: Duration) -> Result<ShellReply> {
            self.replies
                .pop_front()
                .unwrap_or_else(|| Err(Error::Shell {
                    cmd: "script exhausted".into(),
                    output: String::new(),
                }))
        }
    }

    fn manager_with(replies: Vec<Result<ShellReply>>) -> AdbAppManager {
        AdbAppManager::with_shell(
            AdbClient::new(),
            "emulator-test",
            Box::new(ScriptedShell::new(replies)),
        )
    }

    #[test]
    fn list_fast_enriches_from_dumpsys_and_ps() {
        let mut m = manager_with(vec![
            // pm list packages -3
            ScriptedShell::ok("package:com.a.app\npackage:com.b.app\n"),
            // dumpsys package（全量）
            ScriptedShell::ok(
                "Package [com.a.app]\n  versionCode=10 minSdk=24\n  versionName=1.0\n  firstInstallTime=2026-10-01 10:00:00\n  lastUpdateTime=2026-10-01 10:00:00\n\nPackage [com.b.app]\n  versionName=2.0\n",
            ),
            // ps -A -o NAME
            ScriptedShell::ok("NAME\ncom.a.app\ncom.a.app:svc\n"),
        ]);
        let apps = m.list(&ListOptions { enrich: true, pull_metadata: false }).unwrap();
        assert_eq!(apps.len(), 2);
        let a = &apps[0];
        assert_eq!(a.package, "com.a.app");
        assert_eq!(a.version.as_deref(), Some("1.0"));
        assert_eq!(a.version_code, Some(10));
        assert!(a.is_running);
        assert_eq!(a.installed_at.as_deref(), Some("2026-10-01 10:00:00"));
        assert!(!apps[1].is_running);
        assert_eq!(apps[1].version.as_deref(), Some("2.0"));
    }

    #[test]
    fn launch_resolves_activity_then_am_start() {
        let mut m = manager_with(vec![
            // is_installed
            ScriptedShell::ok("package:com.a.app\n"),
            // resolve-activity
            ScriptedShell::ok("com.a.app/.MainActivity\n"),
            // am start（ok + Starting）
            ScriptedShell::ok("Starting: Intent { cmp=com.a.app/.MainActivity }\n"),
        ]);
        m.launch("com.a.app", Some(3)).unwrap();
    }

    #[test]
    fn launch_falls_back_to_monkey_when_resolve_fails() {
        let mut m = manager_with(vec![
            ScriptedShell::ok("package:com.a.app\n"), // is_installed
            ScriptedShell::ok("No activity found\n"), // resolve-activity
            ScriptedShell::ok(
                "Events injected: 1\n## Network stats: elapsed=...\n",
            ), // monkey 成功
        ]);
        m.launch("com.a.app", None).unwrap();
    }

    #[test]
    fn launch_missing_package_errors() {
        let mut m = manager_with(vec![
            ScriptedShell::ok(""), // pm list packages：空
        ]);
        let err = m.launch("com.nothere", None).unwrap_err();
        assert!(matches!(err, Error::NotInstalled(p) if p == "com.nothere"));
    }

    #[test]
    fn uninstall_checks_output_success() {
        let mut m = manager_with(vec![
            ScriptedShell::ok("package:com.a.app\n"), // is_installed
            ScriptedShell::ok("Success\n"),           // pm uninstall
        ]);
        m.uninstall("com.a.app").unwrap();

        let mut m2 = manager_with(vec![
            ScriptedShell::ok("package:com.a.app\n"),
            ScriptedShell::ok("Failure [DELETE_FAILED_INTERNAL_ERROR]\n"),
        ]);
        assert!(m2.uninstall("com.a.app").is_err());
    }

    #[test]
    fn heuristic_icon_picks_densest_launcher_bitmap() {
        let entries = vec![
            "res/mipmap-mdpi-v4/ic_launcher.png",
            "res/mipmap-xxhdpi-v4/ic_launcher.png",
            "res/drawable/splash.png",
            "res/mipmap-anydpi-v26/ic_launcher.xml",
        ];
        assert_eq!(
            heuristic_icon(&entries).as_deref(),
            Some("res/mipmap-xxhdpi-v4/ic_launcher.png")
        );
        assert_eq!(heuristic_icon(&[]), None);
    }

    #[test]
    fn is_installed_exact_match_only() {
        let mut m = manager_with(vec![
            ScriptedShell::ok("package:com.a.app\npackage:com.a.appx\n"), // 前缀包并存
        ]);
        assert!(m.is_installed("com.a.appx").unwrap());
        // com.a.app 有独立条目 → true（前缀包不影响）
        let mut m2 = manager_with(vec![
            ScriptedShell::ok("package:com.a.appx\n"), // 只有更长包名
        ]);
        assert!(!m2.is_installed("com.a.app").unwrap());
    }
}
