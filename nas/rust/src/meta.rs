//! 应用元信息解析：真实标签（含中文本地化）。
//!
//! 纯 Rust 极简实现，无第三方依赖：
//!  - AXML（二进制 AndroidManifest.xml）：找 <application android:label>
//!    ——字面值（TYPE_STRING）直接用，资源引用（@7Fxx0001）进 arsc 解析；
//!  - resources.arsc：ResTable 全局字符串池 + 包内 type/spec/config 表，
//!    按资源 id 找条目，配置选择 zh* 优先、否则默认配置。
//! 图标沿用 icons.rs 的路径启发式（本模块只管 label）。

use crate::adb::AdbResult;

// ===================== 小端读取辅助 =====================

fn u16le(b: &[u8], off: usize) -> u16 {
    if off + 2 > b.len() { return 0; }
    u16::from_le_bytes([b[off], b[off + 1]])
}
fn u32le(b: &[u8], off: usize) -> u32 {
    if off + 4 > b.len() { return 0; }
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

// ===================== 字符串池（AXML/ARSC 共用布局） =====================

/// 解析 ResStringPool，返回全部字符串（UTF-8/UTF-16 自适应）。
fn parse_string_pool(chunk: &[u8]) -> Vec<String> {
    if chunk.len() < 28 || u16le(chunk, 0) != 0x0001 {
        return Vec::new();
    }
    let string_count = u32le(chunk, 8) as usize;
    let flags = u32le(chunk, 16);
    let is_utf8 = flags & 0x100 != 0;
    let strings_start = u32le(chunk, 20) as usize;
    let offsets_start = 28usize; // headerSize 固定 28
    let mut out = Vec::with_capacity(string_count.min(4096));
    for i in 0..string_count {
        let off = u32le(chunk, offsets_start + i * 4) as usize;
        let pos = strings_start + off;
        if pos >= chunk.len() {
            out.push(String::new());
            continue;
        }
        let s = if is_utf8 {
            read_utf8_pool_string(&chunk[pos..])
        } else {
            read_utf16_pool_string(&chunk[pos..])
        };
        out.push(s.unwrap_or_default());
    }
    out
}

/// UTF-8 池字符串：u16 变长 utf16 长度 + u8 变长 utf8 长度 + 字节 + 0。
fn read_utf8_pool_string(b: &[u8]) -> Option<String> {
    if b.is_empty() {
        return None;
    }
    let mut i = 0usize;
    // utf16 长度（1 或 2 字节）
    let _utf16_len = if b[i] & 0x80 != 0 {
        let v = ((b[i] as usize & 0x7f) << 8) | b[i + 1] as usize;
        i += 2;
        v
    } else {
        let v = b[i] as usize;
        i += 1;
        v
    };
    // utf8 长度（1 或 2 字节）
    let ulen = if b[i] & 0x80 != 0 {
        let v = ((b[i] as usize & 0x7f) << 8) | b[i + 1] as usize;
        i += 2;
        v
    } else {
        let v = b[i] as usize;
        i += 1;
        v
    };
    let end = i + ulen;
    if end > b.len() {
        return None;
    }
    Some(String::from_utf8_lossy(&b[i..end]).to_string())
}

/// UTF-16 池字符串：u16 变长长度前缀（1 或 2 个 u16 单元，高位标记）+ UTF-16LE 字节。
fn read_utf16_pool_string(b: &[u8]) -> Option<String> {
    if b.len() < 2 {
        return None;
    }
    let first = u16::from_le_bytes([b[0], b[1]]);
    let (len, skip) = if first & 0x8000 != 0 {
        if b.len() < 4 {
            return None;
        }
        let second = u16::from_le_bytes([b[2], b[3]]);
        ((((first & 0x7fff) as usize) << 16) | second as usize, 4)
    } else {
        (first as usize, 2)
    };
    let byte_len = len * 2;
    if skip + byte_len > b.len() {
        return None;
    }
    let u16s: Vec<u16> = b[skip..skip + byte_len]
        .chunks_exact(2)
        .map(|c| u16::from_le_bytes([c[0], c[1]]))
        .collect();
    Some(String::from_utf16_lossy(&u16s))
}

// ===================== AXML（AndroidManifest.xml） =====================

/// 从 AXML 字节里取 <application android:label>：
/// 返回 字面串 或 资源 id（0x7F……）。
pub fn axml_app_label(axml: &[u8]) -> Option<AppLabel> {
    if axml.len() < 8 || u16le(axml, 0) != 0x0003 {
        return None;
    }
    // 第一块应为字符串池
    let mut off = u16le(axml, 2) as usize + u32le(axml, 4) as usize * 0 + 8; // XML 头后
    // XML chunk header 自身 8 字节（type+headerSize+size 中的 headerSize 通常 8）
    off = 8;
    let mut strings: Vec<String> = Vec::new();
    let mut res_map: Vec<u32> = Vec::new();
    while off + 8 <= axml.len() {
        let ctype = u16le(axml, off);
        let csize = u32le(axml, off + 4) as usize;
        if csize < 8 || off + csize > axml.len() {
            break;
        }
        let chunk = &axml[off..off + csize];
        match ctype {
            0x0001 => strings = parse_string_pool(chunk),
            0x0180 => {
                // ResXMLResourceMap：u32 数组
                res_map.clear();
                for i in (8..csize).step_by(4) {
                    res_map.push(u32le(chunk, i));
                }
            }
            0x0102 => {
                // START_ELEMENT
                if !strings.is_empty() {
                    if let Some(l) = parse_start_element_label(chunk, &strings, &res_map) {
                        return Some(l);
                    }
                }
            }
            _ => {}
        }
        off += csize;
    }
    None
}

pub enum AppLabel {
    Literal(String),
    Resource(u32),
}

/// START_ELEMENT 布局：line(4) comment(4) ns(4) name(4) attrStart(2) attrSize(2)
/// attrCount(2) idIdx(2) classIdx(2) styleIdx(2)，随后属性各 20 字节。
fn parse_start_element_label(chunk: &[u8], strings: &[String], res_map: &[u32]) -> Option<AppLabel> {
    if chunk.len() < 36 {
        return None;
    }
    let name_idx = u32le(chunk, 20) as usize;
    let elem = strings.get(name_idx).map(String::as_str).unwrap_or("");
    if elem != "application" {
        return None;
    }
    // ResXMLTree_node 头 8 + lineNumber/comment 8 = 16；attrStart 相对 attrExt 起点
    let attr_start = 16 + u16le(chunk, 24) as usize;
    let attr_size = u16le(chunk, 26) as usize;
    let attr_count = u16le(chunk, 28) as usize;
    for i in 0..attr_count {
        let base = attr_start + i * attr_size;
        if base + 20 > chunk.len() {
            break;
        }
        let aname_idx = u32le(chunk, base + 4) as usize;
        let aname = strings.get(aname_idx).map(String::as_str).unwrap_or("");
        if aname != "label" {
            continue;
        }
        let raw_idx = u32le(chunk, base + 8) as usize;
        // typed_value: size(2) res0(1) type(1) data(4) —— 起始于 rawValue 之后
        let vtype = chunk[base + 15];
        let vdata = u32le(chunk, base + 16);
        match vtype {
            0x03 => {
                // TYPE_STRING：raw_idx 为字符串池索引
                let s = strings.get(raw_idx).cloned().unwrap_or_default();
                if !s.is_empty() {
                    return Some(AppLabel::Literal(s));
                }
                return None;
            }
            0x01 => return Some(AppLabel::Resource(vdata)), // TYPE_REFERENCE
            _ => {
                // 命名空间校验（android:label 的 attr id = 0x01010003）兜底：
                // 有些清单 rawValue 也带串
                let s = strings.get(raw_idx).cloned().unwrap_or_default();
                if !s.is_empty() {
                    return Some(AppLabel::Literal(s));
                }
                return Some(AppLabel::Resource(vdata));
            }
        }
        #[allow(unreachable_code)]
        {
            let _ = res_map;
        }
    }
    None
}

// ===================== resources.arsc =====================

/// 按资源 id 解析为本地化字符串（zh* 优先，默认配置兜底）。
pub fn arsc_resolve_string(arsc: &[u8], res_id: u32) -> Option<String> {
    if arsc.len() < 12 || u16le(arsc, 0) != 0x0002 {
        return None;
    }
    let mut off = u16le(arsc, 2) as usize; // ResTable headerSize
    // 全局字符串池（值串）
    let mut values: Vec<String> = Vec::new();
    // 目标：package id = res_id>>24, type = (res_id>>16)&0xff, entry = res_id&0xffff
    let want_pkg = (res_id >> 24) as u32;
    let want_type = ((res_id >> 16) & 0xff) as u32;
    let want_entry = (res_id & 0xffff) as usize;
    let mut best_default: Option<String> = None;
    let mut zh_any: Option<String> = None;
    while off + 12 <= arsc.len() {
        let ctype = u16le(arsc, off);
        let csize = u32le(arsc, off + 4) as usize;
        if csize < 8 || off + csize > arsc.len() {
            break;
        }
        let chunk = &arsc[off..off + csize];
        match ctype {
            0x0001 => values = parse_string_pool(chunk),
            0x0200 => {
                // ResTablePackage
                let pkg_id = u32le(chunk, 8);
                if pkg_id == want_pkg {
                    if let Some(s) = resolve_in_package(chunk, &values, want_type, want_entry, &mut best_default, &mut zh_any) {
                        return Some(s);
                    }
                }
            }
            _ => {}
        }
        off += csize;
    }
    zh_any.or(best_default)
}

/// 在包内找 type 的条目；zh* 配置直接返回，默认配置记为兜底。
fn resolve_in_package(
    pkg: &[u8],
    values: &[String],
    want_type: u32,
    want_entry: usize,
    best_default: &mut Option<String>,
    zh_any: &mut Option<String>,
) -> Option<String> {
    let type_strings_off = u32le(pkg, 16) as usize; // typeStrings 池偏移（包内绝对）
    // 包内子 chunk 从 headerSize 起
    let mut off = u16le(pkg, 2) as usize;
    while off + 12 <= pkg.len() {
        let ctype = u16le(pkg, off);
        let csize = u32le(pkg, off + 4) as usize;
        if csize < 8 || off + csize > pkg.len() {
            break;
        }
        if ctype == 0x0201 {
            // ResTableType
            let t = &pkg[off..off + csize];
            let type_id = u32le(t, 8) & 0xff; // 实际 u8 at +8
            let tid = t[8] as u32;
            let _ = type_id;
            if tid == want_type {
                // ResTable_type: id(1) res0(1) res1(2) entryCount(4) entriesStart(4)
                // configSize(4) config(...)
                let hdr_size = u16le(t, 2) as usize;
                let entry_count = u32le(t, 12) as usize;
                let entries_start = u32le(t, 16) as usize;
                // config 紧跟 +20，自带 size（u16）；locale 语言码在 config 内偏移随
                // AOSP 版本/构建变体漂移（本镜像实测 +8；spec 名义 +6）——在 6..12
                // 内找第一对小写 ASCII 字母，都没有 = 默认配置
                let config_size = u16le(t, 20) as usize;
                let lang = if config_size >= 12 && 20 + config_size <= t.len() {
                    let mut found = String::new();
                    for i in 6..12 {
                        let c0 = t[20 + i] as char;
                        let c1 = t[20 + i + 1] as char;
                        if c0.is_ascii_lowercase() && c1.is_ascii_lowercase() {
                            found = format!("{c0}{c1}");
                            break;
                        }
                    }
                    found
                } else {
                    String::new()
                };
                if want_entry < entry_count {
                    #[cfg(test)] {
                        let raw32 = u32le(t, hdr_size) as usize;
                        let data_len = csize.saturating_sub(entries_start);
                        let c16 = !(raw32 % 8 == 0 && raw32 <= data_len);
                        let eo = if c16 { (u16le(t, hdr_size + want_entry * 2) as usize) << 2 } else { u32le(t, hdr_size + want_entry * 4) as usize };
                        println!("    resolve: type={} hdr={} data_len={} raw32={:#x} compact16={} eoff={:#x}", want_type, hdr_size, data_len, raw32, c16, eo);
                        let ep = entries_start + eo;
                        println!("    entry bytes: {}", t[ep..ep+16].iter().map(|b| format!("{b:02x}")).collect::<String>());
                    }
                    // 条目偏移数组在 hdrSize 处（config 之后）。
                    // 两种格式自适应：经典 u32 偏移（8 的倍数且不超数据区）；
                    // AAPT2 compact16：u16 偏移左移 2（--enable-compact-entries 产物）。
                    let data_len = csize.saturating_sub(entries_start);
                    let raw32 = u32le(t, hdr_size) as usize;
                    let compact16 = !(raw32 % 8 == 0 && raw32 <= data_len);
                    let eoff = if compact16 {
                        (u16le(t, hdr_size + want_entry * 2) as usize) << 2
                    } else {
                        u32le(t, hdr_size + want_entry * 4) as usize
                    };
                    if eoff != 0xFFFF_FFFF && !(compact16 && u16le(t, hdr_size + want_entry * 2) == 0xFFFF) {
                        let epos = entries_start + eoff;
                        if epos + 16 <= t.len() {
                            // compact16 条目裸存 Res_value（省掉 8 字节条目前缀）；
                            // 经典格式 = ResTable_entry 头 8 字节 + Res_value
                            let (vtype, vdata) = if compact16 {
                                (t[epos + 3], u32le(t, epos + 4))
                            } else {
                                let flags = u16le(t, epos + 2);
                                if flags & 0x0001 != 0 {
                                    (0u8, 0u32) // complex 条目不是字符串
                                } else {
                                    (t[epos + 11], u32le(t, epos + 12))
                                }
                            };
                            if vtype == 0x03 {
                                let s = values.get(vdata as usize).cloned().unwrap_or_default();
                                #[cfg(test)]
                                {
                                    println!("    vdata={:#x} s={:?} lang={:?}", vdata, s, lang);
                                    if s == "連絡先" || s == "Contacts" {
                                        println!("      config hex: {}", t[20..84].iter().map(|b| format!("{b:02x}")).collect::<String>());
                                    }
                                }
                                if !s.is_empty() {
                                    if lang.starts_with("zh") {
                                        // zh 变体里优先简体中文（读后续 region 字节）
                                        let r0 = t.get(20 + 10).copied().unwrap_or(0) as char;
                                        let r1 = t.get(20 + 11).copied().unwrap_or(0) as char;
                                        let region = format!("{r0}{r1}").to_uppercase();
                                        if region.contains("CN") {
                                            return Some(s);
                                        }
                                        if zh_any.is_none() {
                                            *zh_any = Some(s);
                                        }
                                    } else if lang.is_empty() && best_default.is_none() {
                                        *best_default = Some(s);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        off += csize;
    }
    None
}

// ===================== 对外：APK → 标签 =====================

use crate::icons::{apk_file_bytes, apk_file_entry};

pub fn app_label_from_apk(apk: &[u8]) -> Option<String> {
    let manifest = apk_file_entry(apk, "AndroidManifest.xml")?;
    match axml_app_label(&manifest)? {
        AppLabel::Literal(s) => Some(s),
        AppLabel::Resource(id) => {
            let arsc = apk_file_bytes(apk, "resources.arsc")?;
            arsc_resolve_string(&arsc, id)
        }
    }
}

pub type MetaResult<T> = AdbResult<T>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn real_apk_label() {
        let serial = match crate::adb::online_emulator() {
            Some(s) => s,
            None => return,
        };
        let out = crate::adb::shell_blocking(serial.as_str(), "pm path com.android.contacts", 10000).unwrap();
        let text = String::from_utf8_lossy(&out);
        let apk_path = text.lines().find_map(|l| l.trim().strip_prefix("package:").map(str::to_string)).unwrap();
        let apk = crate::adb::pull_file(&serial, &apk_path, 200 * 1024 * 1024).unwrap();
        let manifest = crate::icons::apk_file_entry(&apk, "AndroidManifest.xml").expect("manifest");
        // 调试：dump 元素与属性
        {
            let mut off = 8usize;
            let mut strings: Vec<String> = Vec::new();
            while off + 8 <= manifest.len() {
                let ctype = u16le(&manifest, off);
                let csize = u32le(&manifest, off + 4) as usize;
                if csize < 8 || off + csize > manifest.len() { break; }
                let chunk = &manifest[off..off + csize];
                if ctype == 0x0001 { strings = parse_string_pool(chunk); }
                if ctype == 0x0102 && !strings.is_empty() {
                    let name_idx = u32le(chunk, 20) as usize;
                    let elem = strings.get(name_idx).cloned().unwrap_or_default();
                    let ac = u16le(chunk, 28);
                    println!("elem <{}> attrs={}", elem, ac);
                    if elem == "application" {
                        let attr_start = 16 + u16le(chunk, 24) as usize;
                        let attr_size = u16le(chunk, 26) as usize;
                        for i in 0..ac as usize {
                            let base = attr_start + i * attr_size;
                            if base + 20 > chunk.len() { break; }
                            let ai = u32le(chunk, base + 4) as usize;
                            let vt = chunk[base + 15];
                            let vd = u32le(chunk, base + 16);
                            let raw = u32le(chunk, base + 8) as usize; println!("  attr {} type={:#x} data={:#x} raw={:?}", strings.get(ai).cloned().unwrap_or_default(), vt, vd, strings.get(raw).cloned().unwrap_or_default());
                        }
                    }
                }
                off += csize;
            }
        }
        let label = axml_app_label(&manifest);
        println!("manifest {} bytes", manifest.len());
        match label {
            Some(AppLabel::Literal(s)) => println!("literal: {s}"),
            Some(AppLabel::Resource(id)) => {
                println!("resource id: {id:#x}");
                let arsc = crate::icons::apk_file_bytes(&apk, "resources.arsc").expect("arsc");
                // dump：包与 type 结构
                {
                    println!("arsc {} bytes", arsc.len());
                    let mut off = u16le(&arsc, 2) as usize;
                    while off + 12 <= arsc.len() {
                        let ct = u16le(&arsc, off);
                        let cs = u32le(&arsc, off + 4) as usize;
                        if cs < 8 || off + cs > arsc.len() { break; }
                        if ct == 0x0200 {
                            let pkg = &arsc[off..off+cs];
                            println!("package id={:#x} headerSize={} typeStrings={} keyStrings={}",
                                u32le(pkg, 8), u16le(pkg, 2), u32le(pkg, 16), u32le(pkg, 20));
                            let mut o2 = u16le(pkg, 2) as usize;
                            while o2 + 12 <= pkg.len() {
                                let ct2 = u16le(pkg, o2);
                                let cs2 = u32le(pkg, o2+4) as usize;
                                if cs2 < 8 || o2+cs2 > pkg.len() { break; }
                                if ct2 == 0x0201 {
                                    let t = &pkg[o2..o2+cs2];

                                    if t[8] == 0x11 {}
                                }
                                o2 += cs2;
                            }
                        }
                        off += cs;
                    }
                }
                let r = arsc_resolve_string(&arsc, id);
                println!("resolved: {:?}", r);
            }
            None => panic!("no label found"),
        }
    }
}
