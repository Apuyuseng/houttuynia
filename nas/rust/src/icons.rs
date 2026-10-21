//! 应用图标提取 + 设备壁纸抓取。
//!
//! 图标策略（启发式，规避 AXML/arsc 全量解析）：
//!   pm path 得 base.apk → sync pull → zip 中央目录扫描 → 按路径挑图标
//!   （res/mipmap-*dpi/ic_launcher.* 优先，密度越高越好，png > webp）；
//!   自适应图标（XML）等无 PNG 产物的应用回退 404（前端显示字母块）。
//! 缓存复用桌面版目录：~/.mwine/appcache/<avd>/icons/<pkg>.png——桌面端
//! 抓过的图标零成本命中，本模块抓到的也沉淀进去供桌面端复用。

use std::io::Read;
use std::path::PathBuf;

use crate::adb::{self, AdbResult};

fn err<T>(msg: impl Into<String>) -> AdbResult<T> {
    Err(adb::AdbError(msg.into()))
}

fn home() -> String {
    std::env::var("HOME").unwrap_or_default()
}

/// 桌面版图标缓存目录（任一 AVD 键下命中即用）
fn desktop_icon_hit(pkg: &str) -> Option<Vec<u8>> {
    let base = format!("{}/.mwine/appcache", home());
    let rd = std::fs::read_dir(base).ok()?;
    for e in rd.flatten() {
        let p = e.path().join("icons").join(format!("{pkg}.png"));
        if let Ok(bytes) = std::fs::read(&p) {
            if bytes.len() > 8 && &bytes[..4] == b"\x89PNG" {
                return Some(bytes);
            }
        }
    }
    None
}

fn rust_icon_cache_dir() -> PathBuf {
    let dir = PathBuf::from(format!("{}/.mwine/appcache/rust/icons", home()));
    let _ = std::fs::create_dir_all(&dir);
    dir
}

// ---------------- zip 最小读取（EOCD → 中央目录 → 条目） ----------------

struct ZipEntry {
    name: String,
    method: u16, // 0=stored 8=deflate
    comp_size: u32,
    local_off: u32,
}

fn zip_list(apk: &[u8]) -> Vec<ZipEntry> {
    let mut out = Vec::new();
    if apk.len() < 22 {
        return out;
    }
    // EOCD 从尾部找 0x06054b50
    let mut eocd = None;
    let mut i = apk.len() - 22;
    loop {
        if apk[i..i + 4] == [0x50, 0x4b, 0x05, 0x06] {
            eocd = Some(i);
            break;
        }
        if i == 0 {
            break;
        }
        i -= 1;
    }
    let Some(eocd) = eocd else { return out };
    let cd_off = u32::from_le_bytes([apk[eocd + 16], apk[eocd + 17], apk[eocd + 18], apk[eocd + 19]])
        as usize;
    let count =
        u16::from_le_bytes([apk[eocd + 10], apk[eocd + 11]]) as usize;
    let mut p = cd_off;
    for _ in 0..count {
        if p + 46 > apk.len() || apk[p..p + 4] != [0x50, 0x4b, 0x01, 0x02] {
            break;
        }
        let method = u16::from_le_bytes([apk[p + 10], apk[p + 11]]);
        let comp_size =
            u32::from_le_bytes([apk[p + 20], apk[p + 21], apk[p + 22], apk[p + 23]]);
        let name_len = u16::from_le_bytes([apk[p + 28], apk[p + 29]]) as usize;
        let extra_len = u16::from_le_bytes([apk[p + 30], apk[p + 31]]) as usize;
        let comment_len = u16::from_le_bytes([apk[p + 32], apk[p + 33]]) as usize;
        let local_off =
            u32::from_le_bytes([apk[p + 42], apk[p + 43], apk[p + 44], apk[p + 45]]);
        let name =
            String::from_utf8_lossy(&apk[p + 46..p + 46 + name_len.min(apk.len() - p - 46)])
                .to_string();
        out.push(ZipEntry { name, method, comp_size, local_off });
        p += 46 + name_len + extra_len + comment_len;
    }
    out
}

fn zip_read(apk: &[u8], e: &ZipEntry) -> Option<Vec<u8>> {
    let p = e.local_off as usize;
    if p + 30 > apk.len() || apk[p..p + 4] != [0x50, 0x4b, 0x03, 0x04] {
        return None;
    }
    let name_len = u16::from_le_bytes([apk[p + 26], apk[p + 27]]) as usize;
    let extra_len = u16::from_le_bytes([apk[p + 28], apk[p + 29]]) as usize;
    let data_off = p + 30 + name_len + extra_len;
    let data = apk.get(data_off..data_off + e.comp_size as usize)?;
    match e.method {
        0 => Some(data.to_vec()),
        8 => {
            let mut d = flate2::read::DeflateDecoder::new(data);
            let mut out = Vec::new();
            d.read_to_end(&mut out).ok()?;
            Some(out)
        }
        _ => None,
    }
}

/// dpi 密度权重（路径内匹配）
fn dpi_weight(path: &str) -> u32 {
    for (k, w) in [
        ("xxxhdpi", 6u32),
        ("xxhdpi", 5),
        ("xhdpi", 4),
        ("hdpi", 3),
        ("mdpi", 2),
        ("ldpi", 1),
    ] {
        if path.contains(k) {
            return w;
        }
    }
    0
}

/// 从 APK 字节里挑启动器图标（PNG/WebP）。
fn icon_from_apk(apk: &[u8]) -> Option<(Vec<u8>, &'static str)> {
    let entries = zip_list(apk);
    let mut best: Option<(u32, &ZipEntry)> = None;
    for e in &entries {
        let n = &e.name;
        if !(n.starts_with("res/") || n.starts_with("assets/")) {
            continue;
        }
        let lower = n.to_lowercase();
        let is_png = lower.ends_with(".png");
        let is_webp = lower.ends_with(".webp");
        if !is_png && !is_webp {
            continue;
        }
        // 图标名命中度：ic_launcher > launcher > icon；nodpi 目录 png 也常见
        let name_score = if lower.contains("ic_launcher") {
            100
        } else if lower.contains("launcher") {
            60
        } else if lower.contains("icon") {
            30
        } else {
            continue; // 与图标无关的资源不碰
        };
        let score = name_score * 100 + dpi_weight(&lower) * 10 + if is_png { 2 } else { 1 };
        if best.map(|(s, _)| score > s).unwrap_or(true) {
            best = Some((score, e));
        }
    }
    let (_, e) = best?;
    let data = zip_read(apk, e)?;
    if data.len() > 8 && &data[..4] == b"\x89PNG" {
        return Some((data, "image/png"));
    }
    if data.len() > 12 && &data[..4] == b"RIFF" && &data[8..12] == b"WEBP" {
        return Some((data, "image/webp"));
    }
    None
}

/// 应用图标（缓存 → 提取 → 缓存）。返回 (字节, MIME)。
pub fn app_icon(serial: &str, pkg: &str) -> AdbResult<(Vec<u8>, &'static str)> {
    // 桌面缓存命中
    if let Some(bytes) = desktop_icon_hit(pkg) {
        return Ok((bytes, "image/png"));
    }
    // 本端缓存命中
    let dir = rust_icon_cache_dir();
    for ext in ["png", "webp"] {
        let p = dir.join(format!("{pkg}.{ext}"));
        if let Ok(bytes) = std::fs::read(&p) {
            let mime = if ext == "png" { "image/png" } else { "image/webp" };
            return Ok((bytes, mime));
        }
    }
    // 负缓存：已确认无可提取图标（自适应 XML 图标等），不再反复拉 APK
    if dir.join(format!("{pkg}.noicon")).exists() {
        return err("无可提取图标");
    }
    // 提取：pm path → base.apk → pull → zip 挑图标
    let out = adb::shell_blocking(serial, &format!("pm path {pkg}"), 10000)
        .map_err(|e| adb::AdbError(e.to_string()))?;
    let text = String::from_utf8_lossy(&out);
    let apk_path = text
        .lines()
        .find_map(|l| l.trim().strip_prefix("package:").map(str::to_string))
        .ok_or_else(|| adb::AdbError(format!("pm path 无输出: {pkg}")))?;
    let apk = adb::pull_file(serial, &apk_path, 200 * 1024 * 1024)?;
    let Some((icon, mime)) = icon_from_apk(&apk) else {
        let _ = std::fs::write(dir.join(format!("{pkg}.noicon")), b"");
        return err("APK 内未找到可用图标（自适应 XML 图标）");
    };
    let ext = if mime == "image/png" { "png" } else { "webp" };
    let _ = std::fs::write(dir.join(format!("{pkg}.{ext}")), &icon);
    Ok((icon, mime))
}

// ---------------- 壁纸 ----------------

fn wallpaper_cache() -> PathBuf {
    let dir = PathBuf::from(format!("{}/.mwine/appcache/rust", home()));
    let _ = std::fs::create_dir_all(&dir);
    dir.join("wallpaper.png")
}

fn wallpaper_stamp(serial: &str) -> Option<String> {
    let out = adb::shell_blocking(
        serial,
        "stat -c '%Y %s' /data/system/users/0/wallpaper_info.xml 2>/dev/null",
        5000,
    )
    .ok()?;
    Some(String::from_utf8_lossy(&out).trim().to_string())
}

/// 设备壁纸（带指纹缓存）。多通道：userdebug 直接读系统文件；否则走
/// companion 桥应用导出（桌面版同协议）；都没有 → Err（前端用渐变兜底）。
pub fn wallpaper(serial: &str) -> AdbResult<(Vec<u8>, &'static str)> {
    let cache = wallpaper_cache();
    let stamp_file = cache.with_extension("stamp");
    let stamp = wallpaper_stamp(serial).unwrap_or_default();
    if !stamp.is_empty() {
        if let (Ok(old_stamp), Ok(bytes)) =
            (std::fs::read_to_string(&stamp_file), std::fs::read(&cache))
        {
            if old_stamp == stamp && !bytes.is_empty() {
                return Ok((bytes, "cache"));
            }
        }
    }
    // 通道 1：userdebug 镜像 root 直读
    if let Ok(bytes) =
        adb::shell_blocking(serial, "cat /data/system/users/0/wallpaper 2>/dev/null", 10000)
    {
        if bytes.len() > 8 && &bytes[..4] == b"\x89PNG" {
            let _ = std::fs::write(&cache, &bytes);
            let _ = std::fs::write(&stamp_file, &stamp);
            return Ok((bytes, "direct"));
        }
    }
    // 通道 2：companion 桥导出（对齐 iconfetch --wallpaper 协议）
    let bridge = adb::shell_blocking(serial, "pm list packages com.mwine.bridge", 8000)
        .map(|o| String::from_utf8_lossy(&o).contains("com.mwine.bridge"))
        .unwrap_or(false);
    if bridge {
        let _ = adb::shell_blocking(
            serial,
            "am broadcast -a com.mwine.bridge.EXPORT_WALLPAPER --es out wallpaper.png",
            8000,
        );
        if let Ok(bytes) = adb::pull_file(
            serial,
            "/sdcard/Android/data/com.mwine.bridge/files/wallpaper.png",
            32 * 1024 * 1024,
        ) {
            if bytes.len() > 8 && &bytes[..4] == b"\x89PNG" {
                let _ = std::fs::write(&cache, &bytes);
                let _ = std::fs::write(&stamp_file, &stamp);
                return Ok((bytes, "bridge"));
            }
        }
    }
    // 通道 3：主屏截图（前端 CSS 高斯模糊后视觉等同氛围壁纸）
    if let Ok(bytes) = adb::shell_blocking(serial, "screencap -p", 15000) {
        if bytes.len() > 8 && &bytes[..4] == b"\x89PNG" {
            return Ok((bytes, "screenshot"));
        }
    }
    err("壁纸不可用（非 root、无 companion 桥、截图兜底失败）")
}
