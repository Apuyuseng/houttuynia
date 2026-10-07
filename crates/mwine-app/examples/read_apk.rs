//! 调试工具：离线解析 APK 的 manifest 元数据与图标。
//!
//! 用法：cargo run -p mwine-app --example read_apk -- <apk路径>
//! 不需要设备与 SDK——验证纯 Rust 路径（zip + AXML）能否拿到包名/版本/标签/图标。

use std::path::Path;

use mwine_app::badging::{self, pick_raster_fallback};
use mwine_app::{parse_manifest, ZipArchive};

fn main() {
    let path = std::env::args()
        .nth(1)
        .expect("用法: read_apk <apk路径>");
    let path = Path::new(&path);
    let zip = ZipArchive::open(path).expect("zip 打开失败");

    let axml = zip
        .read_entry("AndroidManifest.xml")
        .expect("读取 manifest 失败")
        .expect("无 AndroidManifest.xml");
    let info = parse_manifest(&axml, &path.display().to_string()).expect("AXML 解析失败");
    println!("== 纯 Rust AXML 解析 ==");
    println!("package      : {}", info.package);
    println!("versionName  : {:?}", info.version_name);
    println!("versionCode  : {:?}", info.version_code);
    println!("label(字面量) : {:?}", info.label);
    println!("debuggable   : {:?}", info.debuggable);
    println!("minSdk/target: {:?} / {:?}", info.min_sdk, info.target_sdk);

    // 图标（无 arsc 的启发式）
    let entries: Vec<&str> = zip.entry_names().collect();
    println!("\n== 图标启发式 ==");
    let icon = entries
        .iter()
        .copied()
        .filter(|e| e.starts_with("res/") && (e.contains("launcher") || e.contains("icon")))
        .take(8)
        .collect::<Vec<_>>();
    for e in &icon {
        println!("  候选: {e}");
    }
    if let Some(picked) = heuristic(&entries) {
        let bytes = zip.read_entry(&picked).ok().flatten();
        println!(
            "  选中: {picked}（{} 字节）",
            bytes.as_ref().map(|b| b.len()).unwrap_or(0)
        );
    }

    // aapt2 增强（存在时）
    if let Some(tool) = badging::find_aapt2() {
        println!("\n== aapt2 增强（{}）==", tool.display());
        match badging::aapt2_badging(&tool, path) {
            Some(b) => {
                println!("package     : {}", b.package);
                println!("label       : {:?}", b.label);
                println!("icon entry  : {:?}", b.icon);
                if let Some(icon) = &b.icon {
                    let entry = if icon.ends_with(".xml") {
                        pick_raster_fallback(&entries, icon).unwrap_or_else(|| icon.clone())
                    } else {
                        icon.clone()
                    };
                    if let Some(bytes) = zip.read_entry(&entry).ok().flatten() {
                        println!("icon bytes  : {}（entry {entry}）", bytes.len());
                    }
                }
            }
            None => println!("  badging 解析失败"),
        }
    } else {
        println!("\n（aapt2 不可用——纯 Rust 路径即产品形态）");
    }
}

fn heuristic(entries: &[&str]) -> Option<String> {
    let mut best: Option<(u32, &str)> = None;
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
