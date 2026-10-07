//! 镜像安装：本地导入 + Google 官方镜像拉取。
//!
//! 设计：
//! - 「导入」：安装本地的云分发包 tar.zst（打包者经任意渠道分享，使用者自行获取）。
//! - 「官方拉取」：从 Google SDK 仓库（dl.google.com/sys-img）下载官方 system image。
//! - **零外部工具依赖**：HTTP 下载（断点续传+代理）/zstd 解压/tar 解包/sha256/zip 解压/
//!   稀疏化全部为 Rust 内部实现（ureq/zstd/tar/sha2/zip crate），不假设用户环境有 curl/zstd 等。
//!
//! 分发包格式：
//!   houttuynia-image-<version>.tar.zst
//!     └─ houttuynia-image/            ← arm64-v8a 目录原样（symlink 保留）
//!         ├─ manifest.json            ← 每文件 sha256 + 版本信息（内嵌校验）
//!         └─ system-qemu.img / vendor-qemu.img / ramdisk-qemu.img / kernel-ranchu …

use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::{Error, Result, Sdk};

#[derive(Deserialize)]
struct Manifest {
    version: String,
    #[serde(default = "default_api")]
    android_api: u32,
    files: HashMap<String, String>,
}

fn default_api() -> u32 {
    33
}

/// 取消令牌：daemon 的 /api/image/cancel 置 true，管线在各检查点轮询。
pub type CancelFlag = std::sync::Arc<std::sync::atomic::AtomicBool>;

/// 进度回调：(阶段, 已完成字节, 总字节)。总字节=0 表示该阶段不定态（转圈）。
pub type ProgressFn<'a> = &'a dyn Fn(&str, u64, u64);

fn check_cancel(c: &std::sync::atomic::AtomicBool) -> Result<()> {
    if c.load(std::sync::atomic::Ordering::Relaxed) {
        Err(Error::LaunchFailed("已取消".into()))
    } else {
        Ok(())
    }
}

fn home() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"))
}

// ================= 内部化基础能力（替代 curl/shasum/dd/tar/zstd/ditto） =================

/// sha256（流式，8.6G 约 30s，与 shasum 相当）。
fn sha256_file(
    p: &Path,
    cancel: &std::sync::atomic::AtomicBool,
    on_bytes: &dyn Fn(u64),
) -> Result<String> {
    let mut f = std::fs::File::open(p)
        .map_err(|e| Error::LaunchFailed(format!("打开 {} 失败: {e}", p.display())))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 4 << 20];
    let mut done = 0u64;
    loop {
        check_cancel(cancel)?;
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
        done += n as u64;
        on_bytes(done);
    }
    Ok(format!("{:x}", h.finalize()))
}

fn build_agent(proxy: Option<&str>) -> Result<ureq::Agent> {
    let mut b = ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(15))
        .user_agent("Houttuynia/1.0");
    if let Some(p) = proxy.filter(|p| !p.is_empty()) {
        let proxy = ureq::Proxy::new(p)
            .map_err(|e| Error::LaunchFailed(format!("代理地址无效（{p}）: {e}")))?;
        b = b.proxy(proxy);
    }
    Ok(b.build())
}

/// HTTP GET 到字符串（官方仓库清单用）。
fn http_fetch(url: &str, proxy: Option<&str>) -> Result<String> {
    let agent = build_agent(proxy)?;
    let resp = agent
        .get(url)
        .timeout(Duration::from_secs(30))
        .call()
        .map_err(|e| Error::LaunchFailed(format!("请求 {url} 失败: {e}")))?;
    resp.into_string()
        .map_err(|e| Error::LaunchFailed(format!("读取响应失败: {e}")))
}

/// 下载到文件：Range 断点续传 + 失败重试（服务端不支持 Range 时自动整档重下）。
/// 进度经 log 回调按 64MiB 粒度汇报。
fn http_download(url: &str, dest: &Path, proxy: Option<&str>, log: &dyn Fn(&str), cancel: &std::sync::atomic::AtomicBool, prog: ProgressFn, size_hint: u64) -> Result<()> {
    let agent = build_agent(proxy)?;
    let mut attempt = 0;
    loop {
        attempt += 1;
        check_cancel(cancel)?;
        let have = std::fs::metadata(dest).map(|m| m.len()).unwrap_or(0);
        let mut req = agent.get(url);
        if have > 0 {
            req = req.set("Range", &format!("bytes={have}-"));
        }
        let resp = match req.timeout(Duration::from_secs(60)).call() {
            Ok(r) => r,
            Err(e) => {
                if attempt <= 5 {
                    log(&format!("下载中断（{e}），第 {attempt} 次续传重试…"));
                    std::thread::sleep(Duration::from_secs(2));
                    continue;
                }
                return Err(Error::LaunchFailed(format!("下载失败: {e}")));
            }
        };
        let status = resp.status();
        // 200 = 服务端忽略 Range（从头给）；206 = 续传；416 = 已下完（个别服务器）
        let (mut writer, mut done) = if status == 206 {
            (
                std::fs::OpenOptions::new()
                    .append(true)
                    .open(dest)
                    .map_err(|e| Error::LaunchFailed(format!("打开 {} 失败: {e}", dest.display())))?,
                have,
            )
        } else if status == 200 {
            (
                std::fs::File::create(dest)
                    .map_err(|e| Error::LaunchFailed(format!("创建 {} 失败: {e}", dest.display())))?,
                0,
            )
        } else if status == 416 && have > 0 {
            return Ok(());
        } else {
            return Err(Error::LaunchFailed(format!("下载失败：HTTP {status}")));
        };
        // 总量：Content-Range > Content-Length > 清单里的 size_hint
        // （经代理常为 chunked 传输，两个头都没有；官方仓库清单自带精确大小）
        let total = resp
            .header("Content-Range")
            .and_then(|v| v.rsplit('/').next())
            .and_then(|v| v.parse::<u64>().ok())
            .or_else(|| {
                resp.header("Content-Length")
                    .and_then(|v| v.parse::<u64>().ok())
                    .map(|l| l + done)
            })
            .or(if size_hint > 0 { Some(size_hint) } else { None });
        let mut reader = resp.into_reader();
        let mut buf = vec![0u8; 1 << 20];
        let mut last_prog = std::time::Instant::now();
        loop {
            check_cancel(cancel)?;
            match reader.read(&mut buf) {
                Ok(0) => return Ok(()),
                Ok(n) => {
                    if writer.write_all(&buf[..n]).is_err() {
                        break; // 写失败按中断处理，走外层重试
                    }
                    done += n as u64;
                    // 2 秒粒度上报结构化进度（UI 轮询间隔 1.5s，更密无意义）
                    if last_prog.elapsed() >= Duration::from_secs(2) {
                        last_prog = std::time::Instant::now();
                        prog("下载", done, total.unwrap_or(0));
                    }
                }
                Err(e) => {
                    if attempt <= 5 {
                        log(&format!("下载中断（{e}），续传重试…"));
                        std::thread::sleep(Duration::from_secs(2));
                        break;
                    }
                    return Err(Error::LaunchFailed(format!("下载中断: {e}")));
                }
            }
        }
        if attempt > 5 {
            return Err(Error::LaunchFailed("下载重试次数耗尽".into()));
        }
    }
}

/// 解压 tar.zst 到目录（保留 symlink；纯 Rust）。
fn extract_tar_zst(tarball: &Path, dest: &Path) -> Result<()> {
    let f = std::fs::File::open(tarball)
        .map_err(|e| Error::LaunchFailed(format!("打开 {} 失败: {e}", tarball.display())))?;
    let dec = zstd::stream::read::Decoder::new(f)
        .map_err(|e| Error::LaunchFailed(format!("zstd 解压失败（包损坏？）: {e}")))?;
    let mut ar = tar::Archive::new(dec);
    ar.unpack(dest)
        .map_err(|e| Error::LaunchFailed(format!("tar 解包失败: {e}")))
}

/// 解压 zip 到目录（官方镜像用；纯 Rust，路径经 enclosed_name 防穿越）。
fn extract_zip(zip_path: &Path, dest: &Path) -> Result<()> {
    let f = std::fs::File::open(zip_path)
        .map_err(|e| Error::LaunchFailed(format!("打开 {} 失败: {e}", zip_path.display())))?;
    let mut ar = zip::ZipArchive::new(f)
        .map_err(|e| Error::LaunchFailed(format!("zip 打开失败: {e}")))?;
    for i in 0..ar.len() {
        let mut entry = ar
            .by_index(i)
            .map_err(|e| Error::LaunchFailed(format!("zip 条目读取失败: {e}")))?;
        let Some(rel) = entry.enclosed_name() else { continue };
        let out = dest.join(rel);
        if entry.is_dir() {
            std::fs::create_dir_all(&out).ok();
        } else {
            if let Some(p) = out.parent() {
                std::fs::create_dir_all(p).ok();
            }
            let mut w = std::fs::File::create(&out)
                .map_err(|e| Error::LaunchFailed(format!("创建 {} 失败: {e}", out.display())))?;
            std::io::copy(&mut entry, &mut w)
                .map_err(|e| Error::LaunchFailed(format!("解压 {} 失败: {e}", out.display())))?;
        }
    }
    Ok(())
}

/// 稀疏化：全零 4MiB 块跳过不写（APFS 打洞）。逻辑内容字节不变。
fn sparse_rewrite(p: &Path, cancel: &std::sync::atomic::AtomicBool, on_bytes: &dyn Fn(u64)) -> Result<()> {
    let mut r = std::fs::File::open(p)
        .map_err(|e| Error::LaunchFailed(format!("打开 {} 失败: {e}", p.display())))?;
    let tmp = p.with_extension("sparse.tmp");
    let mut w = std::fs::File::create(&tmp)
        .map_err(|e| Error::LaunchFailed(format!("创建临时文件失败: {e}")))?;
    const BS: usize = 4 << 20;
    let mut buf = vec![0u8; BS];
    let mut pos = 0u64;
    loop {
        check_cancel(cancel)?;
        let n = r.read(&mut buf)?;
        if n == 0 {
            break;
        }
        if buf[..n].iter().all(|&b| b == 0) {
            w.seek(SeekFrom::Start(pos + n as u64))
                .map_err(|e| Error::LaunchFailed(format!("稀疏写失败: {e}")))?;
        } else if w.write_all(&buf[..n]).is_err() {
            return Err(Error::LaunchFailed("稀疏写失败".into()));
        }
        pos += n as u64;
        on_bytes(pos);
    }
    w.set_len(pos)
        .map_err(|e| Error::LaunchFailed(format!("收尾文件长度失败: {e}")))?;
    drop(w);
    std::fs::rename(&tmp, p)
        .map_err(|e| Error::LaunchFailed(format!("替换原文件失败: {e}")))
}

// ================= 导入分发包 =================

/// 导入本地云分发包（tar.zst）。返回最终镜像目录。
pub fn import_image(
    tarball: &Path,
    expected: Option<&str>,
    name: &str,
    sdk: &Sdk,
    log: &dyn Fn(&str),
    cancel: &std::sync::atomic::AtomicBool,
    prog: ProgressFn,
) -> Result<PathBuf> {
    // 取消粒度：阶段间 + 哈希/稀疏化的块级；解包（单流）只能在前后检查
    let size = std::fs::metadata(tarball).map(|m| m.len()).unwrap_or(0);
    if size < 10 * 1024 * 1024 {
        return Err(Error::LaunchFailed(format!(
            "{} 仅 {} 字节，不是有效的镜像分发包",
            tarball.display(),
            size
        )));
    }
    let dl_dir = home().join(".mwine/downloads");
    std::fs::create_dir_all(&dl_dir)?;

    prog("准备", 0, 0);
    if let Some(h) = expected {
        let sz = std::fs::metadata(tarball).map(|m| m.len()).unwrap_or(0);
        let got = sha256_file(tarball, cancel, &|d| prog("校验整包", d, sz))?;
        if got != h.to_lowercase() {
            return Err(Error::LaunchFailed(format!(
                "sha256 不匹配：期望 {h}，实际 {got}"
            )));
        }
        log("✓ 整包 sha256 校验通过");
    }

    check_cancel(cancel)?;
    prog("解压", 0, 0);
    let stage = dl_dir.join("stage");
    let _ = std::fs::remove_dir_all(&stage);
    std::fs::create_dir_all(&stage)?;
    extract_tar_zst(tarball, &stage)?;
    check_cancel(cancel)?;
    let inner = std::fs::read_dir(&stage)?
        .filter_map(|e| e.ok())
        .find(|e| e.path().is_dir())
        .map(|e| e.path())
        .ok_or_else(|| Error::LaunchFailed("压缩包里找不到目录".into()))?;

    let m: Manifest = serde_json::from_str(&std::fs::read_to_string(inner.join("manifest.json"))?)
        .map_err(|e| Error::LaunchFailed(format!("manifest.json 解析失败: {e}")))?;
    log(&format!(
        "✓ 包版本 {}（{} 个文件待校验）",
        m.version,
        m.files.len()
    ));
    // 校验阶段：按累计字节给整体百分比
    let mut base = 0u64;
    let total_bytes: u64 = m
        .files
        .keys()
        .map(|f| std::fs::metadata(inner.join(f)).map(|x| x.len()).unwrap_or(0))
        .sum();
    let mut names: Vec<&String> = m.files.keys().collect();
    names.sort();
    for f in names {
        check_cancel(cancel)?;
        let want = &m.files[f];
        let fp = inner.join(f);
        let sz = std::fs::metadata(&fp).map(|x| x.len()).unwrap_or(0);
        let b = base;
        let got = sha256_file(&fp, cancel, &|d| prog("校验文件", b + d, total_bytes))?;
        if got != want.to_lowercase() {
            return Err(Error::LaunchFailed(format!(
                "{f} 校验失败：期望 {want}，实际 {got}"
            )));
        }
        base += sz;
    }
    log("✓ 全部文件 sha256 校验通过");

    install_dir(&inner, m.android_api, name, sdk, log, cancel, prog)
}

/// 把解压好的镜像目录稀疏化并原子落地到 SDK。
fn install_dir(
    inner: &Path,
    android_api: u32,
    name: &str,
    sdk: &Sdk,
    log: &dyn Fn(&str),
    cancel: &std::sync::atomic::AtomicBool,
    prog: ProgressFn,
) -> Result<PathBuf> {
    // 大文件打洞（逻辑 8.6G → 实占 ~1.3G）
    for entry in std::fs::read_dir(inner)?.filter_map(|e| e.ok()) {
        let p = entry.path();
        if p.symlink_metadata().map(|x| x.is_symlink()).unwrap_or(true) {
            continue; // symlink 不动：顺着链接会把链接盖成实体文件
        }
        let len = std::fs::metadata(&p).map(|x| x.len()).unwrap_or(0);
        if len < 100 * 1024 * 1024 {
            continue;
        }
        check_cancel(cancel)?;
        let fname = p.file_name().unwrap().to_string_lossy().into_owned();
        log(&format!("稀疏化 {fname}…"));
        let sz = len;
        let _ = sz;
        sparse_rewrite(&p, cancel, &|d| prog("稀疏化", d, len))?;
    }

    let dest = sdk
        .root
        .join(format!("system-images/android-{}/{}", android_api, name))
        .join("arm64-v8a");
    if dest.exists() {
        return Err(Error::LaunchFailed(format!(
            "目标目录已存在：{}（换 --name 或先删除旧目录）",
            dest.display()
        )));
    }
    prog("落地", 0, 0);
    std::fs::create_dir_all(dest.parent().unwrap())?;
    std::fs::rename(inner, &dest)?;
    let _ = std::fs::remove_dir_all(home().join(".mwine/downloads/stage"));
    log(&format!("✓ 已安装到 {}", dest.display()));
    Ok(dest)
}

// ================= Google 官方镜像拉取 =================

const SYSIMG_REPO: &str = "https://dl.google.com/android/repository/sys-img/android/sys-img2-1.xml";
const SYSIMG_BASE: &str = "https://dl.google.com/android/repository/sys-img/android/";

/// API 级别 → 用户可读的 Android 版本名（用户只认 Android 13/16，不认 android-36）
pub fn android_version_name(api: &str) -> Option<String> {
    let n: u32 = api
        .trim_start_matches("android-")
        .split('-')
        .next()?
        .parse()
        .ok()?;
    let name = match n {
        19 => "4.4", 21 => "5.0", 22 => "5.1", 23 => "6.0",
        24 => "7.0", 25 => "7.1", 26 => "8.0", 27 => "8.1",
        28 => "9", 29 => "10", 30 => "11", 31 => "12", 32 => "12L",
        33 => "13", 34 => "14", 35 => "15", 36 => "16",
        _ => return None,
    };
    Some(format!("Android {name}"))
}

#[derive(Debug, Clone)]
pub struct OfficialImage {
    /// 形如 "android-33;default;arm64-v8a"
    pub path: String,
    pub size: u64,
    pub url: String,
}

/// 拉取官方仓库清单，列出可选 system image（可按 api/版本名过滤）。
pub fn official_list(filter: Option<&str>, proxy: Option<&str>) -> Result<Vec<OfficialImage>> {
    let xml = http_fetch(SYSIMG_REPO, proxy)?;
    parse_repo(&xml, filter)
}

/// 从 sys-img2-1.xml 里解析 remotePackage（无 XML 依赖的轻量扫描）。
fn parse_repo(xml: &str, filter: Option<&str>) -> Result<Vec<OfficialImage>> {
    let mut list = Vec::new();
    for pkg in xml.split("<remotePackage ").skip(1) {
        let path = pkg.split('"').nth(1).unwrap_or("").to_string();
        if !path.starts_with("system-images;") {
            continue;
        }
        // 本产品只在 Apple Silicon 上跑 arm64 模拟器：非 arm64-v8a（x86/mips/armeabi）不可用，不列出
        if !path.ends_with(";arm64-v8a") {
            continue;
        }
        let url = pkg
            .split('<')
            .find_map(|seg| seg.strip_prefix("url>"))
            .map(|u| u.split('<').next().unwrap_or("").trim().to_string())
            .unwrap_or_default();
        let size = pkg
            .split('<')
            .find_map(|seg| seg.strip_prefix("size>"))
            .and_then(|s| s.split('<').next()?.trim().parse::<u64>().ok())
            .unwrap_or(0);
        if url.is_empty() {
            continue;
        }
        if let Some(f) = filter {
            let api_seg = path.split(';').nth(1).unwrap_or("");
            let hit = path.contains(f)
                || android_version_name(api_seg)
                    .map(|n| n.contains(f))
                    .unwrap_or(false);
            if !hit {
                continue;
            }
        }
        list.push(OfficialImage {
            path,
            size,
            url: SYSIMG_BASE.to_string() + &url,
        });
    }
    list.sort_by(|a, b| b.path.cmp(&a.path));
    Ok(list)
}

/// 下载并安装一个官方 system image。
/// `target`："android-33;default;arm64-v8a" 或简写 "android-33"（取过滤后最新一项）。
pub fn official_pull(
    target: &str,
    sdk: &Sdk,
    log: &dyn Fn(&str),
    proxy: Option<&str>,
    cancel: &std::sync::atomic::AtomicBool,
    prog: ProgressFn,
) -> Result<PathBuf> {
    let list = official_list(Some(target), proxy)?;
    let pick = list
        .first()
        .ok_or_else(|| Error::LaunchFailed(format!("官方仓库里找不到匹配 {target} 的镜像")))?;
    let parts: Vec<&str> = pick.path.split(';').collect();
    let tag = parts.get(2).copied().unwrap_or("default");
    let api = parts
        .get(1)
        .and_then(|s| s.trim_start_matches("android-").split('-').next().map(String::from))
        .and_then(|s| s.parse::<u32>().ok())
        .unwrap_or(33);

    let dl = home().join(".mwine/downloads");
    std::fs::create_dir_all(&dl)?;
    let zip = dl.join(format!(
        "official-{}.zip",
        pick.url.rsplit('/').next().unwrap_or("img.zip")
    ));
    let _ = std::fs::remove_file(&zip);
    log(&format!(
        "↓ 下载官方镜像 {}（{} MiB）",
        pick.path,
        pick.size / 1048576
    ));
    log(&format!("  {}", pick.url));
    http_download(&pick.url, &zip, proxy, log, cancel, prog, pick.size)?;
    check_cancel(cancel)?;

    let stage = dl.join("official-stage");
    let _ = std::fs::remove_dir_all(&stage);
    std::fs::create_dir_all(&stage)?;
    log("✓ 解压中…");
    extract_zip(&zip, &stage)?;
    check_cancel(cancel)?;
    let _ = std::fs::remove_file(&zip);

    // 官方 zip 布局：顶层就是 <abi>/ 目录（内含 system.img 等稀疏镜像）
    let inner = std::fs::read_dir(&stage)?
        .filter_map(|e| e.ok())
        .find(|e| e.path().is_dir())
        .map(|e| e.path())
        .ok_or_else(|| Error::LaunchFailed("官方 zip 里找不到目录".into()))?;
    install_dir(&inner, api, tag, sdk, log, cancel, prog)
}
