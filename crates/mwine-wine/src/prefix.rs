//! Wine 前缀管理：精简模板 + APFS clonefile 克隆 + 安装后裁剪。
//!
//! 设计（「裁剪优化」的三个层面）：
//! 1. **模板瘦身**：模板前缀创建时禁用 Mono/Gecko（`WINEDLLOVERRIDES`，见
//!    [`crate::runtime::DLL_OVERRIDES`]），不做几百 MB 的运行时捆绑；禁用
//!    winemenubuilder，宿主不产生菜单/启动项垃圾。
//! 2. **克隆共享**：每个应用的前缀用 `cp -cR`（APFS clonefile，写时复制）
//!    从模板克隆——秒级创建、未修改的数据块在磁盘上与模板共享，N 个应用
//!    的磁盘占用 ≈ 模板 + 各自增量。
//! 3. **安装后清理**：安装器残留的 Temp/Logs/Prefetch/CrashDumps 定期回收
//!    （[`WineStore::trim`]）。

use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};

use crate::runtime::WineRuntime;

/// 一个已发现的可执行文件（drive_c 内相对路径）。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExeInfo {
    /// 相对 prefix/drive_c 的路径，如 `Program Files/Foo/foo.exe`
    pub path: String,
    /// 文件大小（字节）
    pub size: u64,
}

/// 应用索引条目（持久化到 `<base>/index.json`）。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppEntry {
    pub name: String,
    /// 安装器（.exe/.msi）的原始路径，仅记录来源
    pub installer: String,
    /// 安装时发现的 exe 清单
    pub exes: Vec<ExeInfo>,
    pub created: u64,
    pub last_run: u64,
    pub runs: u64,
}

#[derive(Debug, Default, Serialize, Deserialize)]
struct IndexFile {
    apps: Vec<AppEntry>,
}

/// 裁剪结果统计。
#[derive(Debug, Default, Clone, Copy)]
pub struct TrimReport {
    pub files: u64,
    pub bytes: u64,
}

impl TrimReport {
    pub fn add_bytes(&mut self, n: u64) {
        self.files += 1;
        self.bytes += n;
    }
}

/// Wine 存储区（模板 + 应用前缀 + 索引）。
pub struct WineStore {
    /// 根目录，默认 `~/Library/Application Support/mwine/wine`
    pub base: PathBuf,
}

fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// 名称白名单化：仅保留 [A-Za-z0-9._-]，其余替换为 '_'，不允许空/点开头。
pub fn sanitize_name(raw: &str) -> String {
    let mut s: String = raw
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | '-') { c } else { '_' })
        .collect();
    while s.starts_with('.') {
        s.remove(0);
    }
    s.truncate(48);
    if s.is_empty() {
        "app".into()
    } else {
        s
    }
}

impl WineStore {
    /// 打开默认存储区（`MWINE_WINE_HOME` 可重定向，测试用）。
    pub fn open() -> WineStore {
        let base = std::env::var("MWINE_WINE_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|_| {
                dirs_home().join("Library/Application Support/mwine/wine")
            });
        WineStore { base }
    }

    pub fn template(&self) -> PathBuf {
        self.base.join("template")
    }
    pub fn apps_dir(&self) -> PathBuf {
        self.base.join("apps")
    }
    fn index_path(&self) -> PathBuf {
        self.base.join("index.json")
    }
    pub fn app_prefix(&self, name: &str) -> PathBuf {
        self.apps_dir().join(sanitize_name(name))
    }

    fn read_index(&self) -> IndexFile {
        std::fs::read_to_string(self.index_path())
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok())
            .unwrap_or_default()
    }
    fn write_index(&self, idx: &IndexFile) -> Result<()> {
        std::fs::create_dir_all(&self.base)?;
        std::fs::write(
            self.index_path(),
            serde_json::to_string_pretty(idx).context("索引序列化失败")?,
        )
        .context("写 index.json 失败")
    }

    /// 确保（并按需创建/修复）精简模板前缀。返回 true=本次新建。
    pub fn ensure_template(&self, rt: &WineRuntime) -> Result<bool> {
        let tpl = self.template();
        if tpl.join("system.reg").is_file() {
            return Ok(false);
        }
        // 残缺目录（无 system.reg）推倒重来
        if tpl.exists() {
            std::fs::remove_dir_all(&tpl).with_context(|| format!("清理残缺模板 {}", tpl.display()))?;
        }
        std::fs::create_dir_all(&tpl)?;
        println!("创建精简模板前缀（禁 Mono/Gecko，一次性，约 10-30s）...");
        let st = rt
            .command(&tpl)
            .arg("wineboot")
            .arg("-u")
            .status()
            .context("wineboot 执行失败")?;
        if !st.success() {
            bail!("模板前缀初始化失败（wineboot 退出码 {:?}）", st.code());
        }
        // wineboot 收尾后自身的临时产物也清掉，模板保持最小
        let _ = self.trim(&tpl);
        std::fs::write(tpl.join(".mwine-template"), format!("wine={}\n", rt.version().unwrap_or_default()))
            .ok();
        println!("✓ 模板就绪：{}（{}）", tpl.display(), human_bytes(dir_size(&tpl)));
        Ok(true)
    }

    /// 从模板克隆一个应用前缀。返回 (路径, 是否走 APFS 写时复制)。
    pub fn clone_from_template(&self, name: &str) -> Result<(PathBuf, bool)> {
        let src = self.template();
        if !src.join("system.reg").is_file() {
            bail!("模板前缀不存在，请先 mwine wine install（会自动创建）");
        }
        let dst = self.app_prefix(name);
        if dst.exists() {
            bail!("前缀已存在：{}（换 --name 或先 mwine wine uninstall {name}）", dst.display());
        }
        std::fs::create_dir_all(self.apps_dir())?;
        // 优先 clonefile（写时复制，磁盘块与模板共享）；不支持时退回普通拷贝
        let cow = Command::new("cp")
            .args(["-cR"])
            .arg(&src)
            .arg(&dst)
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        if !cow {
            let st = Command::new("cp")
                .args(["-R"])
                .arg(&src)
                .arg(&dst)
                .status()
                .context("cp -R 克隆失败")?;
            if !st.success() {
                bail!("克隆前缀失败");
            }
        }
        // 克隆体的身份文件重置，避免 wine 误判为同一实例
        let _ = std::fs::remove_file(dst.join(".update-timestamp"));
        Ok((dst, cow))
    }

    /// 扫描前缀内新安装的 exe（排除 windows 系统目录与用户目录）。
    pub fn discover_exes(prefix: &Path) -> Vec<ExeInfo> {
        let drive_c = prefix.join("drive_c");
        let mut out = Vec::new();
        if !drive_c.is_dir() {
            return out;
        }
        walk_exes(&drive_c, &drive_c, &mut out, 0);
        out.sort_by(|a, b| b.size.cmp(&a.size).then(a.path.cmp(&b.path)));
        out.truncate(64);
        out
    }

    /// 读取应用索引。
    pub fn apps(&self) -> Vec<AppEntry> {
        self.read_index().apps
    }

    pub fn app(&self, name: &str) -> Option<AppEntry> {
        self.apps().into_iter().find(|a| a.name == name)
    }

    /// 登记一个新装应用。
    pub fn register(&self, entry: AppEntry) -> Result<()> {
        let mut idx = self.read_index();
        idx.apps.retain(|a| a.name != entry.name);
        idx.apps.push(entry);
        self.write_index(&idx)
    }

    /// 更新运行统计（不存在则忽略）。
    pub fn touch_run(&self, name: &str) -> Result<()> {
        let mut idx = self.read_index();
        if let Some(a) = idx.apps.iter_mut().find(|a| a.name == name) {
            a.last_run = now_secs();
            a.runs += 1;
            self.write_index(&idx)?;
        }
        Ok(())
    }

    /// 删除应用：关停其 wine 进程 → 删目录 → 移除索引。
    pub fn uninstall(&self, rt: &WineRuntime, name: &str) -> Result<()> {
        let prefix = self.app_prefix(name);
        if !prefix.is_dir() {
            bail!("不存在应用前缀：{}", prefix.display());
        }
        let freed = dir_size(&prefix);
        let _ = rt.server_command(&prefix).arg("-k").status();
        std::fs::remove_dir_all(&prefix).with_context(|| format!("删除 {} 失败", prefix.display()))?;
        let mut idx = self.read_index();
        idx.apps.retain(|a| a.name != name);
        self.write_index(&idx)?;
        println!("✓ 已卸载 {name}（释放 {}）", human_bytes(freed));
        Ok(())
    }

    /// 关停所有前缀（模板 + 各应用）的 wine 进程。
    pub fn kill_all(&self, rt: &WineRuntime) -> usize {
        let mut targets = vec![self.template()];
        if let Ok(rd) = std::fs::read_dir(self.apps_dir()) {
            targets.extend(rd.flatten().map(|e| e.path()));
        }
        let mut killed = 0;
        for p in targets {
            if p.join("system.reg").is_file()
                && rt.server_command(&p).arg("-k").status().map(|s| s.success()).unwrap_or(false)
            {
                killed += 1;
            }
        }
        killed
    }

    /// 裁剪一个前缀：清空安装器/运行期垃圾（Temp、Logs、Prefetch、CrashDumps）。
    /// 只删这些目录的**内容**，不动目录本身与任何程序文件。
    pub fn trim(&self, prefix: &Path) -> Result<TrimReport> {
        let drive_c = prefix.join("drive_c");
        if !drive_c.is_dir() {
            bail!("不是有效前缀（缺 drive_c）：{}", prefix.display());
        }
        let mut report = TrimReport::default();
        // users/*\/Temp
        let users = drive_c.join("users");
        if let Ok(rd) = std::fs::read_dir(&users) {
            for u in rd.flatten() {
                clear_dir_contents(&u.path().join("Temp"), &mut report);
            }
        }
        let win = drive_c.join("windows");
        for sub in ["Temp", "Logs", "Prefetch", "CrashDumps"] {
            clear_dir_contents(&win.join(sub), &mut report);
        }
        Ok(report)
    }
}

/// 递归收集 exe（不跟随符号链接；跳过 drive_c 直下的 windows/users）。
fn walk_exes(root: &Path, dir: &Path, out: &mut Vec<ExeInfo>, depth: usize) {
    if depth > 8 {
        return;
    }
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        let Ok(ft) = e.file_type() else { continue };
        if ft.is_symlink() {
            continue;
        }
        if ft.is_dir() {
            if depth == 0 {
                let n = e.file_name().to_string_lossy().to_ascii_lowercase();
                if n == "windows" || n == "users" {
                    continue;
                }
            }
            walk_exes(root, &p, out, depth + 1);
        } else if ft.is_file() {
            let name = e.file_name().to_string_lossy().to_ascii_lowercase();
            if name.ends_with(".exe") {
                let size = e.metadata().map(|m| m.len()).unwrap_or(0);
                let rel = p
                    .strip_prefix(root)
                    .map(|r| r.to_string_lossy().to_string())
                    .unwrap_or_default();
                out.push(ExeInfo { path: rel, size });
            }
        }
    }
}

/// 清空目录内容（保留目录本身）。符号链接直接删除（计入 0 字节，防跟随）。
fn clear_dir_contents(dir: &Path, report: &mut TrimReport) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        let Ok(ft) = e.file_type() else { continue };
        if ft.is_symlink() {
            let _ = std::fs::remove_file(&p);
            continue;
        }
        if ft.is_dir() {
            let sz = dir_size(&p);
            if std::fs::remove_dir_all(&p).is_ok() {
                report.bytes += sz;
            }
        } else {
            let sz = e.metadata().map(|m| m.len()).unwrap_or(0);
            if std::fs::remove_file(&p).is_ok() {
                report.add_bytes(sz);
            }
        }
    }
}

/// 目录逻辑大小（不跟随符号链接）。
pub fn dir_size(p: &Path) -> u64 {
    let Ok(md) = std::fs::symlink_metadata(p) else { return 0 };
    if md.is_file() {
        return md.len();
    }
    if !md.is_dir() {
        return 0;
    }
    let Ok(rd) = std::fs::read_dir(p) else { return 0 };
    rd.flatten().map(|e| dir_size(&e.path())).sum()
}

pub fn human_bytes(n: u64) -> String {
    let units = ["B", "KB", "MB", "GB", "TB"];
    let mut v = n as f64;
    let mut i = 0;
    while v >= 1024.0 && i < units.len() - 1 {
        v /= 1024.0;
        i += 1;
    }
    if i == 0 {
        format!("{n} B")
    } else {
        format!("{v:.1} {}", units[i])
    }
}

/// unix 秒 → "YYYY-MM-DD HH:MM (UTC)"（无外部依赖的民用历换算）。
pub fn fmt_ts(secs: u64) -> String {
    let days = (secs / 86400) as i64;
    let rem = secs % 86400;
    let (y, m, d) = civil_from_days(days);
    format!(
        "{y:04}-{m:02}-{d:02} {:02}:{:02} UTC",
        rem / 3600,
        (rem % 3600) / 60
    )
}

/// Howard Hinnant 的 epoch→公历算法（无时区，UTC）。
pub fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

fn dirs_home() -> PathBuf {
    std::env::var("HOME").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from("/"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sanitize() {
        // 非法字符替换、前导点剥离、长度截断
        assert_eq!(sanitize_name("Notepad++ v8.6"), "Notepad___v8.6");
        assert_eq!(sanitize_name(".hidden"), "hidden");
        assert_eq!(sanitize_name("///"), "___");
        assert!(sanitize_name("a-very-long-name").len() <= 48);
    }

    #[test]
    fn civil() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        assert_eq!(civil_from_days(19_000), (2022, 1, 8));
    }
}
