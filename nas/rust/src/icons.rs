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

/// 读 zip 内指定路径条目（供 meta.rs 取 AndroidManifest.xml / resources.arsc）
pub fn apk_file_entry(apk: &[u8], name: &str) -> Option<Vec<u8>> {
    let entries = zip_list(apk);
    let e = entries.iter().find(|e| e.name == name)?;
    zip_read(apk, e)
}

/// 同义别名（语义清晰用）
pub fn apk_file_bytes(apk: &[u8], name: &str) -> Option<Vec<u8>> {
    apk_file_entry(apk, name)
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

/// zip 中央目录条目（绝对偏移）
struct CdEntry {
    name: String,
    method: u16,
    comp_size: u32,
    local_off: u64,
}

/// 从中央目录缓冲解析条目（offset 相对 cd 起点）
fn cd_parse(cd: &[u8]) -> Vec<CdEntry> {
    let mut out = Vec::new();
    let mut p = 0usize;
    while p + 46 <= cd.len() && cd[p..p + 4] == [0x50, 0x4b, 0x01, 0x02] {
        let method = u16::from_le_bytes([cd[p + 10], cd[p + 11]]);
        let comp_size = u32::from_le_bytes([cd[p + 20], cd[p + 21], cd[p + 22], cd[p + 23]]);
        let name_len = u16::from_le_bytes([cd[p + 28], cd[p + 29]]) as usize;
        let extra_len = u16::from_le_bytes([cd[p + 30], cd[p + 31]]) as usize;
        let comment_len = u16::from_le_bytes([cd[p + 32], cd[p + 33]]) as usize;
        let local_off =
            u32::from_le_bytes([cd[p + 42], cd[p + 43], cd[p + 44], cd[p + 45]]) as u64;
        let name = String::from_utf8_lossy(&cd[p + 46..(p + 46 + name_len).min(cd.len())])
            .to_string();
        out.push(CdEntry { name, method, comp_size, local_off });
        p += 46 + name_len + extra_len + comment_len;
    }
    out
}

/// 区间数据上的 zip 读：给定已拉到的切片（含 local header 起），解出内容
fn zip_read_slice(slice: &[u8], method: u16, comp_size: u32) -> Option<Vec<u8>> {
    if slice.len() < 30 || slice[..4] != [0x50, 0x4b, 0x03, 0x04] {
        return None;
    }
    let name_len = u16::from_le_bytes([slice[26], slice[27]]) as usize;
    let extra_len = u16::from_le_bytes([slice[28], slice[29]]) as usize;
    let data_off = 30 + name_len + extra_len;
    let data = slice.get(data_off..data_off + comp_size as usize)?;
    match method {
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

/// 挑图标条目（中央目录语义筛选；不读内容）。
/// 维度：名字命中度（ic_launcher > launcher > icon）→ 形状（方形优先，
/// 宽 banner 剔除）→ dpi 高优先 → png>webp。自适应图标的矩形回退也认
/// （其前景已被 _foreground 覆盖，方形候选缺席时取它）。
fn pick_icon_entry(entries: &[CdEntry]) -> Option<&CdEntry> {
    let mut best: Option<(u32, &CdEntry)> = None;
    for e in entries {
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
        // 维度几何：mipmap 目录语义推导尺寸（banner 大体按宽/高比剔除）
        let shape_score = shape_score_of(&lower);
        if shape_score == 0 {
            continue; // 宽 banner（>1.8:1）：非图标用途
        }
        // 名字命中度：方形前景（108×108 自适应画布，系统就是拿它渲染图标）
        // > ic_launcher（可能矩形回退/banner） > launcher > icon
        let name_score = if lower.contains("ic_launcher_foreground") {
            130
        } else if lower.contains("ic_launcher") {
            100
        } else if lower.contains("launcher") {
            60
        } else if lower.contains("icon") {
            30
        } else {
            continue;
        };
        // 名字命中 × 形状（方形加成） + dpi + 格式
        let score = name_score * 100 + shape_score * 30 + dpi_weight(&lower) * 6 + if is_png { 2 } else { 1 };
        if best.map(|(s, _)| score > s).unwrap_or(true) {
            best = Some((score, e));
        }
    }
    best.map(|(_, e)| e)
}

/// mipmap/drawable 目录 → 基准像素尺寸的宽/高比近似（w/h）；
/// 返回 3（方形/近似方形）| 2（1.1–1.8，竖立 banner 可救）| 0（宽 banner 剔除）。
/// 自适应图标真实尺寸未知时以其矩形回退档同样命中（方形前景缺席的兜底）。
fn shape_score_of(path: &str) -> u32 {
    for (token, wh) in [
        ("xxxhdpi", 192.0f32),
        ("xxhdpi", 144.0),
        ("xhdpi", 96.0),
        ("hdpi", 72.0),
        ("mdpi", 48.0),
        ("ldpi", 36.0),
        ("nodpi", 48.0),
        ("tvdpi", 64.0),
    ] {
        if path.contains(token) {
            // 近似：图标边长 ~wh；无法精确定向比例，用名字形态判：
            // 「round」「square」「circle」直接方形满分
            if path.contains("round") || path.contains("square") || path.contains("circle") {
                return 3;
            }
            // "banner"/"wide"/"logo_land" 类宽图剔除
            if path.contains("banner") || path.contains("wide") || path.contains("_land") {
                return 0;
            }
            let _ = wh;
            return 3; // 默认方形：mipmap 下的标准图标位
        }
    }
    // drawable-anydpi 等无 dpi 标记：看名字
    if path.contains("banner") || path.contains("wide") || path.contains("_land") {
        return 0;
    }
    3
}

/// 区间读取式图标提取：尾部 EOCD → 中央目录 → 图标切片（三次小读）
fn icon_from_apk_ranged(serial: &str, path: &str, size: u64) -> Option<(Vec<u8>, &'static str)> {
    // 尾部 256KB 找 EOCD
    let tail_len = 256 * 1024u64.min(size);
    let tail = adb::pull_range(serial, path, size - tail_len, tail_len).ok()?;
    let mut eocd = None;
    let mut i = tail.len().saturating_sub(22);
    loop {
        if tail[i..i + 4] == [0x50, 0x4b, 0x05, 0x06] {
            eocd = Some(i);
            break;
        }
        if i == 0 {
            break;
        }
        i -= 1;
    }
    let eocd = eocd?;
    let cd_size = u32::from_le_bytes([tail[eocd + 12], tail[eocd + 13], tail[eocd + 14], tail[eocd + 15]]) as u64;
    let cd_off = u32::from_le_bytes([tail[eocd + 16], tail[eocd + 17], tail[eocd + 18], tail[eocd + 19]]) as u64;
    if cd_size == 0 || cd_size > 4 * 1024 * 1024 || cd_off + cd_size > size {
        return None;
    }
    let cd = adb::pull_range(serial, path, cd_off, cd_size).ok()?;
    let entries = cd_parse(&cd);
    let e = pick_icon_entry(&entries)?;
    // 图标切片：local header(30) + name + extra + 压缩数据；extra 长度在 local header 里，
    // 先读 512 头再按 comp_size 补读（一次读足 comp_size + 512 通常够）
    let want = 512 + e.comp_size as u64;
    let slice = adb::pull_range(serial, path, e.local_off, want).ok()?;
    let data = zip_read_slice(&slice, e.method, e.comp_size)?;
    if data.len() > 0x1000_0000 {
        return None; // >256MB 瓦片保护
    }
    if data.len() > 8 && &data[..4] == b"\x89PNG" {
        return Some((data, "image/png"));
    }
    if data.len() > 12 && &data[..4] == b"RIFF" && &data[8..12] == b"WEBP" {
        return Some((data, "image/webp"));
    }
    None
}

/// 从 APK 字节里挑启动器图标（PNG/WebP）——整包路径（兜底）
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

/// 提取并发槽（无线真机/繁忙链路上防止图标提取风暴挤占其它请求）
static ICON_SLOTS: [std::sync::Mutex<()>; 3] = [
    std::sync::Mutex::new(()),
    std::sync::Mutex::new(()),
    std::sync::Mutex::new(()),
];

/// 同包提取在飞集合（并发重复请求合并到首次结果）
static ICON_INFLIGHT: std::sync::LazyLock<std::sync::Mutex<std::collections::HashSet<String>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(std::collections::HashSet::new()));

struct InflightGuard {
    pkg: String,
}
impl Drop for InflightGuard {
    fn drop(&mut self) {
        ICON_INFLIGHT.lock().unwrap().remove(&self.pkg);
    }
}

/// 应用图标（缓存 → 提取 → 缓存）。返回 (字节, MIME)。
pub fn app_icon(serial: &str, pkg: &str) -> AdbResult<(Vec<u8>, &'static str)> {
    // 桌面缓存命中
    if let Some(bytes) = desktop_icon_hit(pkg) {
        return Ok((bytes, "image/png"));
    }
    // 本端缓存命中（按月过期：30 天外的缓存视为陈旧，重新提取）
    let dir = rust_icon_cache_dir();
    for ext in ["png", "webp"] {
        let p = dir.join(format!("{pkg}.{ext}"));
        if let Ok(bytes) = std::fs::read(&p) {
            let stale = std::fs::metadata(&p)
                .and_then(|m| m.modified())
                .map(|t| {
                    std::time::SystemTime::now()
                        .duration_since(t)
                        .map(|d| d.as_secs() > 30 * 24 * 3600)
                        .unwrap_or(false)
                })
                .unwrap_or(false);
            if stale {
                let _ = std::fs::remove_file(&p);
                continue;
            }
            let mime = if ext == "png" { "image/png" } else { "image/webp" };
            return Ok((bytes, mime));
        }
    }
    // 负缓存：确认无可提取图标后 7 天内不再拉 APK（APK 升级可能带新图标）
    if let Ok(m) = std::fs::metadata(dir.join(format!("{pkg}.noicon"))) {
        let recent = m
            .modified()
            .ok()
            .and_then(|t| std::time::SystemTime::now().duration_since(t).ok())
            .map(|d| d.as_secs() < 7 * 24 * 3600)
            .unwrap_or(true);
        if recent {
            return err("无可提取图标");
        }
        let _ = std::fs::remove_file(dir.join(format!("{pkg}.noicon")));
    }
    // 同包在飞合并：第二个请求者等前者落缓存后直接读
    {
        let mut inflight = ICON_INFLIGHT.lock().unwrap();
        if inflight.contains(pkg) {
            drop(inflight);
            for _ in 0..120 {
                std::thread::sleep(std::time::Duration::from_millis(500));
                for ext in ["png", "webp"] {
                    let p = dir.join(format!("{pkg}.{ext}"));
                    if let Ok(bytes) = std::fs::read(&p) {
                        let mime = if ext == "png" { "image/png" } else { "image/webp" };
                        return Ok((bytes, mime));
                    }
                }
                if dir.join(format!("{pkg}.noicon")).exists() {
                    return err("无可提取图标");
                }
                if !ICON_INFLIGHT.lock().unwrap().contains(pkg) {
                    break; // 前者异常结束
                }
            }
            return err("图标提取等待超时");
        }
        inflight.insert(pkg.to_string());
    }
    let _inflight_guard = InflightGuard { pkg: pkg.to_string() };

    // 提取（并发槽限流）：pm path → 尺寸 → 区间读取（三小读）→ 兜底整拉
    let slot = (pkg.bytes().fold(0usize, |a, b| a.wrapping_add(b as usize))) % ICON_SLOTS.len();
    let _guard = ICON_SLOTS[slot].lock().unwrap();
    let out = adb::shell_blocking(serial, &format!("pm path {pkg}"), 30000)
        .map_err(|e| adb::AdbError(e.to_string()))?;
    let text = String::from_utf8_lossy(&out);
    let apk_path = text
        .lines()
        .find_map(|l| l.trim().strip_prefix("package:").map(str::to_string))
        .ok_or_else(|| adb::AdbError(format!("pm path 无输出: {pkg}")))?;
    let size = adb::remote_file_size(serial, &apk_path).unwrap_or(0);
    if size > 0 {
        if let Some((icon, mime)) = icon_from_apk_ranged(serial, &apk_path, size) {
            let ext = if mime == "image/png" { "png" } else { "webp" };
            let _ = std::fs::write(dir.join(format!("{pkg}.{ext}")), &icon);
            return Ok((icon, mime));
        }
    }
    // 兜底 A：整包拉取（>120MB 的巨型包跳过——游戏包整拉不现实）
    if size > 0 && size <= 120 * 1024 * 1024 {
        if let Ok(apk) = adb::pull_file(serial, &apk_path, 120 * 1024 * 1024) {
            if let Some((icon, mime)) = icon_from_apk(&apk) {
                let ext = if mime == "image/png" { "png" } else { "webp" };
                let _ = std::fs::write(dir.join(format!("{pkg}.{ext}")), &icon);
                return Ok((icon, mime));
            }
        }
    }
    // 兜底 B：豌豆荚按包名取图标（设备侧提取全部失败时；服务器出网即可）
    if let Some((icon, mime)) = icon_from_wandoujia(pkg) {
        let ext = if mime == "image/png" { "png" } else { "webp" };
        let _ = std::fs::write(dir.join(format!("{pkg}.{ext}")), &icon);
        return Ok((icon, mime));
    }
    let _ = std::fs::write(dir.join(format!("{pkg}.noicon")), b"");
    err("APK 提取与豌豆荚均无可用图标")
}

// ---------------- 应用标签 ----------------

fn labels_cache_path() -> PathBuf {
    let dir = PathBuf::from(format!("{}/.mwine/appcache/rust", home()));
    let _ = std::fs::create_dir_all(&dir);
    dir.join("labels.json")
}

fn labels_read() -> serde_json::Map<String, serde_json::Value> {
    std::fs::read_to_string(labels_cache_path())
        .ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|v| v.as_object().cloned())
        .unwrap_or_default()
}

fn label_cached(pkg: &str) -> Option<String> {
    labels_read().get(pkg)?.as_str().map(String::from)
}

fn label_store(pkg: &str, label: &str) {
    let mut m = labels_read();
    m.insert(pkg.to_string(), serde_json::Value::String(label.to_string()));
    let _ = std::fs::write(labels_cache_path(), serde_json::to_string(&m).unwrap_or_default());
}

/// 图标是否已在缓存（本端 + 桌面端 appcache 命中都算；负缓存不算）
pub fn icon_cached(pkg: &str) -> bool {
    let dir = rust_icon_cache_dir();
    dir.join(format!("{pkg}.png")).exists()
        || dir.join(format!("{pkg}.webp")).exists()
        || desktop_icon_hit(pkg).is_some()
}

/// 只读标签缓存（不触发提取）：列表响应用
pub fn app_label_cached_only(pkg: &str) -> Option<String> {
    label_cached(pkg)
}

/// 应用真实标签：缓存（含桌面版 apps.json 的 label）→ APK 元信息解析。
pub fn app_label(serial: &str, pkg: &str) -> Option<String> {
    if let Some(l) = label_cached(pkg) {
        return Some(l);
    }
    // 桌面缓存 apps.json（appcache/<avd>/apps.json 的 {pkg:{label}}）
    let base = format!("{}/.mwine/appcache", home());
    if let Ok(rd) = std::fs::read_dir(&base) {
        for e in rd.flatten() {
            let p = e.path().join("apps.json");
            if let Ok(text) = std::fs::read_to_string(&p) {
                if let Ok(v) = serde_json::from_str::<serde_json::Value>(&text) {
                    if let Some(l) = v.get(pkg).and_then(|e| e.get("label")).and_then(|l| l.as_str()) {
                        if !l.is_empty() {
                            label_store(pkg, l);
                            return Some(l.to_string());
                        }
                    }
                }
            }
        }
    }
    // APK 解析
    let out = adb::shell_blocking(serial, &format!("pm path {pkg}"), 30000).ok()?;
    let text = String::from_utf8_lossy(&out);
    let apk_path = text
        .lines()
        .find_map(|l| l.trim().strip_prefix("package:").map(str::to_string))?;
    let apk = adb::pull_file(serial, &apk_path, 200 * 1024 * 1024).ok()?;
    let label = crate::meta::app_label_from_apk(&apk)?;
    label_store(pkg, &label);
    Some(label)
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
        adb::shell_blocking(serial, "cat /data/system/users/0/wallpaper 2>/dev/null", 30000)
    {
        if bytes.len() > 8 && &bytes[..4] == b"\x89PNG" {
            let _ = std::fs::write(&cache, &bytes);
            let _ = std::fs::write(&stamp_file, &stamp);
            return Ok((bytes, "direct"));
        }
    }
    // 通道 2：companion 桥导出（对齐 iconfetch --wallpaper 协议）；
    // 桥缺失时按需自动装配（桌面版同样在首用时装桥）
    let bridge = adb::shell_blocking(serial, "pm list packages com.mwine.bridge", 8000)
        .map(|o| String::from_utf8_lossy(&o).contains("com.mwine.bridge"))
        .unwrap_or(false);
    if !bridge {
        if let Some(apk) = find_bridge_apk() {
            if let Ok(bytes) = std::fs::read(&apk) {
                let remote = "/data/local/tmp/mwine-bridge.apk";
                if adb::push_bytes(serial, &bytes, remote).is_ok() {
                    let _ = adb::shell_blocking(serial, &format!("pm install -r '{remote}'"), 120000);
                    let _ = adb::shell_blocking(serial, &format!("rm -f '{remote}'"), 10000);
                }
            }
        }
    }
    let bridge = bridge || adb::shell_blocking(serial, "pm list packages com.mwine.bridge", 8000)
        .map(|o| String::from_utf8_lossy(&o).contains("com.mwine.bridge"))
        .unwrap_or(false);
    if bridge {
        // 对齐 iconfetch fetchViaBridge：先解冻（新装应用 stopped 态收不到
        // 广播）+ 显式组件广播（隐式会被系统拦）+ 等文件出现
        let dev_path = "/sdcard/Android/data/com.mwine.bridge/files/wallpaper.png";
        let _ = adb::shell_blocking(
            serial,
            &format!(
                "am start -n com.mwine.bridge/.UnlockActivity; \
                 rm -f '{dev_path}'; \
                 am broadcast -n com.mwine.bridge/.WallpaperReceiver \
                 -a com.mwine.bridge.EXPORT_WALLPAPER --es out wallpaper.png --ei maxDim 1080"
            ),
            15000,
        );
        std::thread::sleep(std::time::Duration::from_millis(1500));
        for _ in 0..5 {
            if let Ok(bytes) = adb::pull_file(serial, dev_path, 32 * 1024 * 1024) {
                if bytes.len() > 1024 && &bytes[..4] == b"\x89PNG" {
                    let _ = std::fs::write(&cache, &bytes);
                    let _ = std::fs::write(&stamp_file, &stamp);
                    return Ok((bytes, "bridge"));
                }
            }
            std::thread::sleep(std::time::Duration::from_secs(1));
        }
        let _ = adb::shell_blocking(serial, &format!("rm -f '{dev_path}'"), 5000);
    }
    // 通道 3：主屏截图（前端 CSS 高斯模糊后视觉等同氛围壁纸）
    if let Ok(bytes) = adb::shell_blocking(serial, "screencap -p", 15000) {
        if bytes.len() > 8 && &bytes[..4] == b"\x89PNG" {
            return Ok((bytes, "screenshot"));
        }
    }
    err("壁纸不可用（非 root、无 companion 桥、截图兜底失败）")
}

/// companion 桥 APK 查找（bundle bin / ~/.mwine/bin / 开发树 assets）
fn find_bridge_apk() -> Option<String> {
    let home = home();
    let mut cands = vec![
        "/opt/mwine/bin/mwine-bridge.apk".to_string(), // Docker 镜像内置
        format!("{home}/.mwine/bin/mwine-bridge.apk"),
        format!("{home}/Downloads/aiproject/mwine/assets/companion/dist/mwine-bridge.apk"),
    ];
    if let Ok(exe) = std::env::current_exe() {
        if let Some(d) = exe.parent() {
            cands.push(format!("{}/../Resources/bin/mwine-bridge.apk", d.display()));
            cands.push(format!("{}/mwine-bridge.apk", d.display()));
        }
    }
    cands.into_iter().find(|p| std::path::Path::new(p).is_file())
}

/// 豌豆荚应用详情页按包名取图标（itemprop="image" 的 img src）。
/// 对齐桌面版 wandoujia 商店的图标来源（android-artworks CDN）。
pub fn icon_from_wandoujia(pkg: &str) -> Option<(Vec<u8>, &'static str)> {
    let client = reqwest::blocking::Client::builder()
        .timeout(std::time::Duration::from_secs(15))
        .user_agent("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36")
        .build()
        .ok()?;
    let page = client
        .get(format!("https://www.wandoujia.com/apps/{pkg}"))
        .send()
        .ok()?
        .text()
        .ok()?;
    // 找 itemprop="image" 的 img 的 src
    let mut idx = 0;
    let icon_url = loop {
        let Some(img) = page[idx..].find("<img").map(|o| idx + o) else { return None };
        let end = page[img..].find('>').map(|e| img + e).unwrap_or(page.len() - 1);
        let tag = &page[img..end];
        if tag.contains("itemprop=\"image\"") {
            if let Some(s0) = tag.find("src=\"") {
                let rest = &tag[s0 + 5..];
                if let Some(e0) = rest.find('"') {
                    let u = &rest[..e0];
                    let full = if u.starts_with("//") {
                        format!("https:{u}")
                    } else {
                        u.to_string()
                    };
                    break full;
                }
            }
        }
        idx = end + 1;
        if idx >= page.len() { return None }
    };
    let bytes = client.get(&icon_url).send().ok()?.bytes().ok()?.to_vec();
    if bytes.len() > 8 && &bytes[..4] == b"\x89PNG" {
        Some((bytes, "image/png"))
    } else if bytes.len() > 12 && &bytes[..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        Some((bytes, "image/webp"))
    } else if bytes.len() > 3 && &bytes[..3] == b"\xff\xd8\xff" {
        Some((bytes, "image/jpeg"))
    } else {
        None
    }
}
