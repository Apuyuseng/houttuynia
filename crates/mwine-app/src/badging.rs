//! aapt2 `dump badging` 输出解析与 aapt2 定位（可选增强路径）。
//!
//! 产品形态（方案 A）无 SDK，aapt2 不存在——模块主路径不依赖它；
//! 开发机上有 build-tools 时用它富化标签/图标（自适应图标、@string 引用
//! 都能解析）。逻辑与 mwine-cli 中已验证的实现同源。

use std::path::{Path, PathBuf};

/// badging 解析结果。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ApkBadging {
    pub package: String,
    pub label: Option<String>,
    /// 最高密度图标的 APK 内 entry 路径（可能是自适应图标的 .xml）
    pub icon: Option<String>,
}

fn unquote(s: &str) -> String {
    let s = s.trim();
    s.strip_prefix('\'')
        .and_then(|s| s.strip_suffix('\''))
        .unwrap_or(s)
        .to_string()
}

fn is_raster_icon(path: &str) -> bool {
    let lower = path.to_ascii_lowercase();
    [".png", ".webp", ".jpg", ".jpeg"]
        .iter()
        .any(|e| lower.ends_with(e))
}

/// 解析 `aapt2 dump badging` 全文输出。
pub fn parse_badging(text: &str) -> ApkBadging {
    let mut package = None;
    let mut label = None;
    let mut icons: Vec<(u32, String)> = Vec::new();
    for line in text.lines() {
        if package.is_none() {
            if let Some(rest) = line.strip_prefix("package:") {
                if let Some(idx) = rest.find("name=") {
                    let v = rest[idx + "name=".len()..]
                        .split_whitespace()
                        .next()
                        .unwrap_or("");
                    let v = unquote(v);
                    if !v.is_empty() {
                        package = Some(v);
                    }
                }
            }
        }
        if label.is_none() {
            if let Some(rest) = line.strip_prefix("application-label:") {
                let v = unquote(rest);
                if !v.is_empty() {
                    label = Some(v);
                }
            }
        }
        if let Some(rest) = line.strip_prefix("application-icon-") {
            if let Some((dpi, path)) = rest.split_once(':') {
                if let Ok(dpi) = dpi.trim().parse::<u32>() {
                    let path = unquote(path);
                    if !path.is_empty() {
                        icons.push((dpi, path));
                    }
                }
            }
        }
    }
    // 优先位图资源；全是 adaptive xml 时保留（由调用方做位图回退扫描）
    let pool: Vec<&(u32, String)> = {
        let raster: Vec<&(u32, String)> = icons.iter().filter(|(_, p)| is_raster_icon(p)).collect();
        if raster.is_empty() {
            icons.iter().collect()
        } else {
            raster
        }
    };
    let icon = pool
        .iter()
        .max_by_key(|(dpi, _)| *dpi)
        .map(|(_, p)| p.clone());
    ApkBadging {
        package: package.unwrap_or_default(),
        label,
        icon,
    }
}

/// adaptive icon（.xml）在 APK 里通常伴随同名位图；扫描 zip 目录找最高密度位图。
pub fn pick_raster_fallback(entries: &[&str], xml_icon: &str) -> Option<String> {
    let stem = xml_icon
        .rsplit('/')
        .next()
        .unwrap_or("")
        .split('.')
        .next()
        .unwrap_or("");
    if stem.is_empty() {
        return None;
    }
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
        if !is_raster_icon(e) || !e.starts_with("res/") {
            continue;
        }
        let e_stem = e
            .rsplit('/')
            .next()
            .unwrap_or("")
            .split('.')
            .next()
            .unwrap_or("");
        if e_stem == stem {
            let d = density_of(e);
            if best.as_ref().is_none_or(|(bd, _)| d > *bd) {
                best = Some((d, e));
            }
        }
    }
    best.map(|(_, e)| e.to_string())
}

/// 定位 aapt2：MWINE_AAPT2 环境变量 > PATH > ANDROID_SDK_ROOT/ANDROID_HOME
/// > macOS 默认 SDK 路径（~/Library/Android/sdk）。版本号目录取字典序最大。
/// > 找不到返回 None（调用方走无 SDK 路径）。
pub fn find_aapt2() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("MWINE_AAPT2") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Some(p);
        }
    }
    // PATH
    if let Ok(path) = std::env::var("PATH") {
        for dir in std::env::split_paths(&path) {
            let candidate = dir.join("aapt2");
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    let sdk = std::env::var_os("ANDROID_SDK_ROOT")
        .or_else(|| std::env::var_os("ANDROID_HOME"))
        .map(PathBuf::from)
        .or_else(|| {
            // macOS Android Studio 默认安装路径（无需环境变量）
            #[cfg(target_os = "macos")]
            {
                std::env::var_os("HOME").map(|h| PathBuf::from(h).join("Library/Android/sdk"))
            }
            #[cfg(not(target_os = "macos"))]
            {
                None
            }
        })?;
    let bt = sdk.join("build-tools");
    let mut versions: Vec<_> = std::fs::read_dir(&bt)
        .ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.join("aapt2").is_file())
        .map(|p| p.join("aapt2"))
        .collect();
    versions.sort();
    versions.pop()
}

/// 用 aapt2 提取 badging（工具不存在/执行失败返回 None，不视为错误）。
pub fn aapt2_badging(aapt2: &Path, apk: &Path) -> Option<ApkBadging> {
    let out = std::process::Command::new(aapt2)
        .args(["dump", "badging"])
        .arg(apk)
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let badging = parse_badging(&text);
    if badging.package.is_empty() {
        None
    } else {
        Some(badging)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"package: name='com.example.app' versionCode='42' versionName='1.2.3' platformBuildVersionCode='34'
sdkVersion:'24'
targetSdkVersion:'34'
application: label='示例应用' icon='res/mipmap-xxxhdpi-v4/ic_launcher.png'
application-label:'示例应用'
application-label-hi:'उदाहरण'
application-icon-160:'res/mipmap-hdpi-v4/ic_launcher.png'
application-icon-240:'res/mipmap-hdpi-v4/ic_launcher.png'
application-icon-640:'res/mipmap-xxxhdpi-v4/ic_launcher.png'
launchable-activity: name='com.example.app.MainActivity'
"#;

    #[test]
    fn badging_full_parse() {
        let b = parse_badging(SAMPLE);
        assert_eq!(b.package, "com.example.app");
        assert_eq!(b.label.as_deref(), Some("示例应用"));
        assert_eq!(b.icon.as_deref(), Some("res/mipmap-xxxhdpi-v4/ic_launcher.png"));
    }

    #[test]
    fn badging_adaptive_icon_prefers_raster() {
        let text = "package: name='com.adaptive' versionName='1'\n\
                    application-icon-480:'res/mipmap-xxhdpi/ic_launcher.xml'\n\
                    application-icon-160:'res/mipmap-mdpi-v4/ic_launcher.png'\n";
        let b = parse_badging(text);
        // 全 xml 池保留 xml；混有 png 时优先 png
        assert_eq!(b.icon.as_deref(), Some("res/mipmap-mdpi-v4/ic_launcher.png"));
    }

    #[test]
    fn raster_fallback_picks_highest_density() {
        let entries = vec![
            "res/mipmap-mdpi-v4/ic_launcher.png",
            "res/mipmap-xxhdpi-v4/ic_launcher.png",
            "res/mipmap-xxhdpi-v4/other.png",
            "res/mipmap-anydpi-v26/ic_launcher.xml",
        ];
        let got = pick_raster_fallback(&entries, "res/mipmap-anydpi-v26/ic_launcher.xml");
        assert_eq!(
            got.as_deref(),
            Some("res/mipmap-xxhdpi-v4/ic_launcher.png")
        );
    }

    #[test]
    fn badging_empty_and_garbage() {
        let b = parse_badging("");
        assert_eq!(b.package, "");
        assert_eq!(b.icon, None);
    }
}
