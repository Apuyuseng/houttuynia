//! 二进制 AndroidManifest.xml（AXML）解析：提取 package / versionCode /
//! versionName / label（字面量）/ debuggable / minSdk 等 manifest 级信息。
//!
//! 只解析 manifest 元素自身的属性——这些值的字符串引用指向 **manifest 自己的
//! string pool**（无需 resources.arsc），因此零 SDK 即可完成。application 级
//! 属性也顺带提取（label 字面量、debuggable），用于安装后的元数据快速填充。

use crate::{Error, Result};

const RES_STRING_POOL_TYPE: u16 = 0x0001;
const RES_XML_START_ELEMENT_TYPE: u16 = 0x0102;
const RES_XML_END_ELEMENT_TYPE: u16 = 0x0103;
/// string pool flags: UTF8 编码位
const UTF8_FLAG: u32 = 1 << 8;
const NO_INDEX: u32 = 0xFFFF_FFFF;
/// 属性值类型：字符串
const TYPE_STRING: u8 = 0x03;
/// 属性值类型：整数（十进制）
const TYPE_INT_DEC: u8 = 0x10;

/// manifest 级元数据（全部为“解析得到什么是什么”，缺失即 None）。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ManifestInfo {
    pub package: String,
    pub version_name: Option<String>,
    pub version_code: Option<u64>,
    /// application android:label——仅当是字面字符串；@string 引用返回 None（需 arsc）
    pub label: Option<String>,
    pub debuggable: Option<bool>,
    pub min_sdk: Option<u32>,
    pub target_sdk: Option<u32>,
}

struct StringPool {
    /// UTF-16 解码后的字符串（UTF-8 pool 也统一转存 String）
    strings: Vec<String>,
}

impl StringPool {
    fn get(&self, idx: u32) -> Option<&str> {
        if idx == NO_INDEX {
            return None;
        }
        self.strings.get(idx as usize).map(|s| s.as_str())
    }
}

struct Cursor<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Cursor<'a> {
    fn u16(&mut self) -> Option<u16> {
        let p = self.pos;
        if p + 2 > self.data.len() {
            return None;
        }
        self.pos += 2;
        Some(u16::from_le_bytes([self.data[p], self.data[p + 1]]))
    }

    fn u32(&mut self) -> Option<u32> {
        let p = self.pos;
        if p + 4 > self.data.len() {
            return None;
        }
        self.pos += 4;
        Some(u32::from_le_bytes([
            self.data[p],
            self.data[p + 1],
            self.data[p + 2],
            self.data[p + 3],
        ]))
    }

    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        let p = self.pos;
        if p + n > self.data.len() {
            return None;
        }
        self.pos += n;
        Some(&self.data[p..p + n])
    }
}

fn parse_string_pool(data: &[u8]) -> std::result::Result<StringPool, String> {
    let mut c = Cursor { data, pos: 0 };
    if c.u16() != Some(RES_STRING_POOL_TYPE) {
        return Err("不是 string pool chunk".into());
    }
    let header_size = c.u16().unwrap_or(0) as usize;
    let _chunk_size = c.u32().unwrap_or(0) as usize;
    let string_count = c.u32().unwrap_or(0) as usize;
    let _style_count = c.u32().unwrap_or(0);
    let flags = c.u32().unwrap_or(0);
    let strings_start = c.u32().unwrap_or(0) as usize;
    let _styles_start = c.u32();
    let utf8 = flags & UTF8_FLAG != 0;
    c.pos = header_size.max(28);
    let mut offsets = Vec::with_capacity(string_count);
    for _ in 0..string_count {
        match c.u32() {
            Some(off) => offsets.push(off as usize),
            None => return Err("string 偏移表截断".into()),
        }
    }
    // stringsStart 相对 pool chunk 起点（已含 header + 偏移表）
    let strings_base = strings_start;
    let mut strings = Vec::with_capacity(string_count);
    for off in offsets {
        let abs = strings_base + off;
        if abs >= data.len() {
            strings.push(String::new());
            continue;
        }
        let mut sc = Cursor {
            data,
            pos: abs,
        };
        let s = if utf8 {
            read_pool_string_utf8(&mut sc)
        } else {
            read_pool_string_utf16(&mut sc)
        };
        strings.push(s.unwrap_or_default());
    }
    Ok(StringPool { strings })
}

fn read_pool_string_utf16(c: &mut Cursor) -> Option<String> {
    // UTF-16：u16 长度（最高位为 1 时是两段式长度，APK 实际不会触发，截断处理）
    let mut len = c.u16()? as usize;
    if len & 0x8000 != 0 {
        len = (len & 0x7FFF) << 16 | c.u16()? as usize;
    }
    let bytes = c.take(len * 2)?;
    let units: Vec<u16> = bytes
        .as_chunks::<2>().0.iter()
        .map(|b| u16::from_le_bytes([b[0], b[1]]))
        .collect();
    Some(String::from_utf16_lossy(&units))
}

fn read_pool_string_utf8(c: &mut Cursor) -> Option<String> {
    // UTF-8：utf16 字符数（忽略）与 utf8 字节数都用 7-bit 分块变长编码（1 或 2 字节）
    let _utf16_len = read_var_len(c)?;
    let len = read_var_len(c)?;
    let bytes = c.take(len)?;
    Some(String::from_utf8_lossy(bytes).into_owned())
}

/// 变长长度：首字节高位为 1 时取低 7 位 + 次字节拼 14 位。
fn read_var_len(c: &mut Cursor) -> Option<usize> {
    let first = c.take(1)?[0] as usize;
    if first & 0x80 != 0 {
        let second = c.take(1)?[0] as usize;
        Some(((first & 0x7f) << 8) | second)
    } else {
        Some(first)
    }
}

/// 解析 AXML 字节（APK 内 AndroidManifest.xml 的解压内容）。
pub fn parse_manifest(axml: &[u8], label: &str) -> Result<ManifestInfo> {
    parse_manifest_inner(axml).map_err(|reason| Error::ApkParse {
        path: label.to_string(),
        reason,
    })
}

fn parse_manifest_inner(axml: &[u8]) -> std::result::Result<ManifestInfo, String> {
    // 文件头：type=0x0003(RES_XML) + headerSize + size
    let mut c = Cursor {
        data: axml,
        pos: 0,
    };
    let file_type = c.u16().ok_or("空数据")?;
    if file_type != 0x0003 {
        return Err(format!("不是二进制 XML（type={file_type:#06x}）"));
    }
    let file_header_size = c.u16().unwrap_or(8) as usize;
    let _file_size = c.u32().unwrap_or(0) as usize;
    c.pos = file_header_size;

    // 逐 chunk 扫描：string pool 只出现一次；随后是命名空间/元素事件
    let mut pool: Option<StringPool> = None;
    let mut info = ManifestInfo::default();
    let mut depth = 0usize; // manifest=1, application=2
    while c.pos + 8 <= axml.len() {
        let mut chunk = Cursor {
            data: axml,
            pos: c.pos,
        };
        let chunk_type = chunk.u16().unwrap_or(0);
        let header_size = chunk.u16().unwrap_or(0) as usize;
        let chunk_size = chunk.u32().unwrap_or(0) as usize;
        if chunk_size < header_size || c.pos + chunk_size > axml.len() {
            return Err("chunk 尺寸越界".into());
        }
        let body = &axml[c.pos + header_size..c.pos + chunk_size];
        match chunk_type {
            RES_STRING_POOL_TYPE => {
                pool = Some(parse_string_pool(&axml[c.pos..c.pos + chunk_size])?);
            }
            RES_XML_START_ELEMENT_TYPE => {
                depth += 1;
                let pool = pool.as_ref().ok_or("元素出现在 string pool 之前")?;
                apply_start_element(body, pool, depth, &mut info)?;
            }
            RES_XML_END_ELEMENT_TYPE => depth = depth.saturating_sub(1),
            _ => {} // 命名空间/CDATAs 等跳过
        }
        c.pos += chunk_size;
    }
    if info.package.is_empty() {
        return Err("manifest 元素缺 package 属性".into());
    }
    Ok(info)
}

/// START_ELEMENT body：headerSize(16) 已含 lineNumber/comment（chunk 8..16），
/// body 从 attrExt 开始——ns/name + 属性头 + 属性数组。
fn apply_start_element(
    body: &[u8],
    pool: &StringPool,
    depth: usize,
    info: &mut ManifestInfo,
) -> std::result::Result<(), String> {
    let mut c = Cursor { data: body, pos: 0 };
    let _ns = c.u32();
    let name_idx = c.u32().ok_or("元素名截断")?;
    let name = pool.get(name_idx).ok_or("元素名索引越界")?.to_string();
    let _attr_start = c.u16().unwrap_or(0x14);
    let _attr_size = c.u16().unwrap_or(0x14);
    let attr_count = c.u16().unwrap_or(0) as usize;
    // idIndex/classIndex/styleIndex
    c.u16();
    c.u16();
    c.u16();

    // 属性值：属性名匹配 manifest 关心的键；取 rawValue（字符串）或 typed value
    for _ in 0..attr_count {
        let _attr_ns = c.u32().ok_or("属性截断")?;
        let attr_name_idx = c.u32().ok_or("属性名截断")?;
        let raw_idx = c.u32().unwrap_or(NO_INDEX);
        let value_size = c.u16().unwrap_or(8);
        let _res0 = c.take(1);
        let data_type = c.take(1).and_then(|b| b.first().copied()).unwrap_or(0);
        let data = c.u32().unwrap_or(0);
        // 个别工具写 size≠8 的扩展属性头，按声明的 size 对齐
        let extra = value_size.saturating_sub(8) as usize;
        if extra > 0 {
            c.take(extra).ok_or("属性头截断")?;
        }
        let attr_name = pool.get(attr_name_idx).unwrap_or("");
        let string_value = || -> Option<String> {
            if data_type == TYPE_STRING {
                pool.get(data).map(|s| s.to_string())
            } else if raw_idx != NO_INDEX {
                pool.get(raw_idx).map(|s| s.to_string())
            } else {
                None
            }
        };
        match (depth, name.as_str(), attr_name) {
            (1, "manifest", "package") => info.package = string_value().unwrap_or_default(),
            (1, "manifest", "versionName") => info.version_name = string_value().filter(|s| !s.is_empty()),
            (1, "manifest", "versionCode") if data_type == TYPE_INT_DEC => {
                info.version_code = Some(data as u64)
            }
            (1, "manifest", "versionCode") => info.version_code = parse_digits(&string_value()),
            // uses-sdk 与 application 都是 manifest 直接子元素
            (2, "application", "label") => {
                info.label = string_value().filter(|s| !s.trim().is_empty())
            }
            (2, "application", "debuggable") => info.debuggable = Some(data != 0),
            (2, "uses-sdk", "minSdkVersion") => {
                info.min_sdk = sdk_version(data_type, data, &string_value())
            }
            (2, "uses-sdk", "targetSdkVersion") => {
                info.target_sdk = sdk_version(data_type, data, &string_value())
            }
            _ => {}
        }
    }
    Ok(())
}

fn sdk_version(data_type: u8, data: u32, raw: &Option<String>) -> Option<u32> {
    if data_type == TYPE_INT_DEC {
        return Some(data);
    }
    raw.as_deref().and_then(|s| s.parse().ok())
}

fn parse_digits(s: &Option<String>) -> Option<u64> {
    s.as_deref()?.parse().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 元素树节点：名字 + 属性 + 子元素（真实 AXML 是嵌套结构）。
    struct Node<'a> {
        name: &'a str,
        attrs: Vec<(&'a str, u8, u32)>,
        children: Vec<Node<'a>>,
    }

    /// 构造最小可解析的 AXML：UTF-8 string pool + 元素树（START/END 配对）。
    /// 字符串值以 data=池索引表示。
    fn build_axml(strings: &[&str], root: &Node) -> Vec<u8> {
        let mut pool = Vec::new();
        let mut offsets = Vec::new();
        for s in strings {
            offsets.push(pool.len() as u32);
            let bytes = s.as_bytes();
            pool.extend_from_slice(&[bytes.len() as u8]); // utf16 len（<128 单字节）
            pool.extend_from_slice(&[bytes.len() as u8]); // utf8 len
            pool.extend_from_slice(bytes);
            pool.push(0); // null 结尾
        }
        while pool.len() % 4 != 0 {
            pool.push(0);
        }

        let idx = |s: &str| strings.iter().position(|x| *x == s).unwrap() as u32;
        // 递归输出 START(children...)END：START chunk 的 size 覆盖整个子树
        fn emit(node: &Node, idx: &dyn Fn(&str) -> u32, buf: &mut Vec<u8>) {
            let el_idx = idx(node.name);
            let mut start = Vec::new();
            start.extend_from_slice(&RES_XML_START_ELEMENT_TYPE.to_le_bytes());
            start.extend_from_slice(&16u16.to_le_bytes()); // headerSize
            let size_pos = start.len();
            start.extend_from_slice(&0u32.to_le_bytes()); // size 回填
            start.extend_from_slice(&1u32.to_le_bytes()); // lineNumber
            start.extend_from_slice(&NO_INDEX.to_le_bytes()); // comment
            start.extend_from_slice(&NO_INDEX.to_le_bytes()); // ns
            start.extend_from_slice(&el_idx.to_le_bytes()); // name
            start.extend_from_slice(&0x14u16.to_le_bytes()); // attrStart
            start.extend_from_slice(&0x14u16.to_le_bytes()); // attrSize
            start.extend_from_slice(&(node.attrs.len() as u16).to_le_bytes());
            start.extend_from_slice(&0u16.to_le_bytes()); // idIndex
            start.extend_from_slice(&0u16.to_le_bytes()); // classIndex
            start.extend_from_slice(&0u16.to_le_bytes()); // styleIndex
            for (attr, dtype, data) in &node.attrs {
                start.extend_from_slice(&NO_INDEX.to_le_bytes()); // attr ns
                start.extend_from_slice(&idx(attr).to_le_bytes()); // attr name
                start.extend_from_slice(&NO_INDEX.to_le_bytes()); // rawValue
                start.extend_from_slice(&8u16.to_le_bytes()); // value size
                start.extend_from_slice(&0u8.to_le_bytes()); // res0
                start.extend_from_slice(&dtype.to_le_bytes()); // dataType
                start.extend_from_slice(&data.to_le_bytes()); // data
            }
            // END 元素：header(16) + line + comment + ns + name = 24
            let mut end = Vec::new();
            end.extend_from_slice(&RES_XML_END_ELEMENT_TYPE.to_le_bytes());
            end.extend_from_slice(&16u16.to_le_bytes());
            end.extend_from_slice(&24u32.to_le_bytes());
            end.extend_from_slice(&1u32.to_le_bytes()); // lineNumber
            end.extend_from_slice(&NO_INDEX.to_le_bytes()); // comment
            end.extend_from_slice(&NO_INDEX.to_le_bytes()); // ns
            end.extend_from_slice(&el_idx.to_le_bytes()); // name

            let mut children_buf = Vec::new();
            for child in &node.children {
                emit(child, idx, &mut children_buf);
            }
            // AXML chunk 是平铺事件序列：START 的 size 只覆盖自身（不含子元素），
            // 嵌套语义由事件顺序表达
            let size = start.len() as u32;
            start[size_pos..size_pos + 4].copy_from_slice(&size.to_le_bytes());
            buf.extend_from_slice(&start);
            buf.extend_from_slice(&children_buf);
            buf.extend_from_slice(&end);
        }

        let mut elements_buf = Vec::new();
        emit(root, &idx, &mut elements_buf);

        let mut out = Vec::new();
        out.extend_from_slice(&0x0003u16.to_le_bytes()); // RES_XML
        out.extend_from_slice(&8u16.to_le_bytes()); // headerSize
        let file_size_pos = out.len();
        out.extend_from_slice(&0u32.to_le_bytes()); // 回填

        // string pool chunk
        let sp_start = out.len();
        out.extend_from_slice(&RES_STRING_POOL_TYPE.to_le_bytes());
        out.extend_from_slice(&28u16.to_le_bytes()); // headerSize
        let sp_size_pos = out.len();
        out.extend_from_slice(&0u32.to_le_bytes()); // 回填
        out.extend_from_slice(&(strings.len() as u32).to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes()); // style count
        out.extend_from_slice(&UTF8_FLAG.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes()); // stringsStart 回填
        out.extend_from_slice(&0u32.to_le_bytes()); // stylesStart
        for off in &offsets {
            out.extend_from_slice(&off.to_le_bytes());
        }
        out.extend_from_slice(&pool);
        let sp_size = (out.len() - sp_start) as u32;
        out[sp_size_pos..sp_size_pos + 4].copy_from_slice(&sp_size.to_le_bytes());
        let strings_start = 28 + strings.len() as u32 * 4;
        out[sp_start + 20..sp_start + 24].copy_from_slice(&strings_start.to_le_bytes());

        out.extend_from_slice(&elements_buf);
        let total = out.len() as u32;
        out[file_size_pos..file_size_pos + 4].copy_from_slice(&total.to_le_bytes());
        out
    }

    #[test]
    fn parse_minimal_manifest() {
        let strings = [
            "manifest",
            "package",
            "com.example.app",
            "versionName",
            "1.2.3",
            "application",
            "label",
            "示例应用",
            "versionCode",
        ];
        let axml = build_axml(
            &strings,
            &Node {
                name: "manifest",
                attrs: vec![
                    ("package", TYPE_STRING, 2),
                    ("versionCode", TYPE_INT_DEC, 42),
                    ("versionName", TYPE_STRING, 4),
                ],
                children: vec![Node {
                    name: "application",
                    attrs: vec![("label", TYPE_STRING, 7)],
                    children: vec![],
                }],
            },
        );
        let info = parse_manifest(&axml, "test").unwrap();
        assert_eq!(info.package, "com.example.app");
        assert_eq!(info.version_code, Some(42));
        assert_eq!(info.version_name.as_deref(), Some("1.2.3"));
        assert_eq!(info.label.as_deref(), Some("示例应用"));
    }

    #[test]
    fn label_reference_yields_none() {
        // label 属性是引用类型（dataType=0x01）：无法解析，应得 None 而非报错
        let strings = ["manifest", "package", "com.ref.app", "application", "label"];
        let axml = build_axml(
            &strings,
            &Node {
                name: "manifest",
                attrs: vec![("package", TYPE_STRING, 2)],
                children: vec![Node {
                    name: "application",
                    attrs: vec![("label", 0x01, 0x7f0e0001)],
                    children: vec![],
                }],
            },
        );
        let info = parse_manifest(&axml, "test").unwrap();
        assert_eq!(info.package, "com.ref.app");
        assert_eq!(info.label, None);
    }

    #[test]
    fn nested_wrong_depth_not_mistaken() {
        // application 被嵌到更深一层（模拟意外结构）：label 不应被采信
        let strings = [
            "manifest", "package", "com.deep.app", "middle", "application", "label", "深层的",
        ];
        let axml = build_axml(
            &strings,
            &Node {
                name: "manifest",
                attrs: vec![("package", TYPE_STRING, 2)],
                children: vec![Node {
                    name: "middle",
                    attrs: vec![],
                    children: vec![Node {
                        name: "application",
                        attrs: vec![("label", TYPE_STRING, 6)],
                        children: vec![],
                    }],
                }],
            },
        );
        let info = parse_manifest(&axml, "test").unwrap();
        assert_eq!(info.package, "com.deep.app");
        assert_eq!(info.label, None);
    }

    #[test]
    fn reject_text_xml_and_empty() {
        assert!(parse_manifest(b"<manifest/>", "t").is_err());
        assert!(parse_manifest(b"", "t").is_err());
        // 有文件头但无 manifest 元素
        let garbage = {
            let mut v = vec![];
            v.extend_from_slice(&0x0003u16.to_le_bytes());
            v.extend_from_slice(&8u16.to_le_bytes());
            v.extend_from_slice(&8u32.to_le_bytes());
            v
        };
        assert!(parse_manifest(&garbage, "t").is_err());
    }
}
