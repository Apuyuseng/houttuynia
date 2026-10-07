//! 极简 ZIP 读取：只做 APK 内单 entry 提取（AndroidManifest.xml / 图标）。
//!
//! 覆盖 APK 实际使用的特性：store(0) / deflate(8)、中央目录定位。
//! 不支持：ZIP64、加密、跨盘——APK 内目标 entry 不会触发这些。
//! 尺寸以中央目录为准（本地头可能带 data descriptor 占位 0）。

use std::io::Read;
use std::path::Path;

use crate::{Error, Result};

const EOCD_SIG: u32 = 0x0605_4b50;
const CEN_SIG: u32 = 0x0201_4b50;
const LOC_SIG: u32 = 0x0403_4b50;
/// EOCD 最小 22 字节 + 注释最长 64KB，从尾部扫这么多即可
const EOCD_SCAN_WINDOW: usize = 22 + 65_535;

struct CentralEntry {
    name: String,
    method: u16,
    comp_size: u64,
    uncomp_size: u64,
    local_offset: u64,
}

pub struct ZipArchive {
    data: Vec<u8>,
    entries: Vec<CentralEntry>,
}

impl ZipArchive {
    pub fn open(path: &Path) -> Result<Self> {
        let data = std::fs::read(path).map_err(Error::Io)?;
        Self::from_bytes(data, path.display().to_string())
    }

    pub fn from_bytes(data: Vec<u8>, label: String) -> Result<Self> {
        let entries = Self::read_central_directory(&data)
            .map_err(|reason| Error::ApkParse { path: label, reason })?;
        Ok(Self { data, entries })
    }

    fn read_central_directory(data: &[u8]) -> std::result::Result<Vec<CentralEntry>, String> {
        if data.len() < 22 {
            return Err("文件小于 EOCD 最小长度".into());
        }
        // 从尾部向前找 EOCD 签名
        let scan_start = data.len().saturating_sub(EOCD_SCAN_WINDOW);
        let mut eocd = None;
        let mut i = (data.len() - 22) as isize;
        while i >= scan_start as isize {
            let p = i as usize;
            if u32::from_le_bytes([data[p], data[p + 1], data[p + 2], data[p + 3]]) == EOCD_SIG {
                eocd = Some(p);
                break;
            }
            i -= 1;
        }
        let eocd = eocd.ok_or("未找到 EOCD 记录")?;
        let u16_at = |p: usize| u16::from_le_bytes([data[p], data[p + 1]]);
        let u32_at = |p: usize| u32::from_le_bytes([data[p], data[p + 1], data[p + 2], data[p + 3]]);
        let total = u16_at(eocd + 10) as usize; // 总 entry 数
        let cd_size = u32_at(eocd + 12) as usize;
        let cd_off = u32_at(eocd + 16) as usize;
        if cd_off.checked_add(cd_size).map(|end| end > data.len()) != Some(false) {
            return Err(format!("中央目录越界: off={cd_off} size={cd_size} file={}", data.len()));
        }

        let mut entries = Vec::with_capacity(total);
        let mut p = cd_off;
        for _ in 0..total {
            if p + 46 > data.len() || u32_at(p) != CEN_SIG {
                return Err("中央目录 entry 损坏".into());
            }
            let method = u16_at(p + 10);
            let comp_size = u32_at(p + 20) as u64;
            let uncomp_size = u32_at(p + 24) as u64;
            let name_len = u16_at(p + 28) as usize;
            let extra_len = u16_at(p + 30) as usize;
            let comment_len = u16_at(p + 32) as usize;
            let local_offset = u32_at(p + 42) as u64;
            let name = String::from_utf8_lossy(&data[p + 46..p + 46 + name_len]).into_owned();
            entries.push(CentralEntry {
                name,
                method,
                comp_size,
                uncomp_size,
                local_offset,
            });
            p += 46 + name_len + extra_len + comment_len;
        }
        Ok(entries)
    }

    pub fn entry_names(&self) -> impl Iterator<Item = &str> {
        self.entries.iter().map(|e| e.name.as_str())
    }

    /// 提取指定 entry 的解压内容；不存在返回 None；数据损坏返回 Err。
    pub fn read_entry(&self, name: &str) -> Result<Option<Vec<u8>>> {
        let entry = self.entries.iter().find(|e| e.name == name);
        let Some(entry) = entry else { return Ok(None) };
        let d = &self.data;
        let off = entry.local_offset as usize;
        if off + 30 > d.len() {
            return Err(Error::ApkParse {
                path: name.into(),
                reason: "本地头越界".into(),
            });
        }
        let u16_at = |p: usize| u16::from_le_bytes([d[p], d[p + 1]]);
        if u32_le(d, off) != LOC_SIG {
            return Err(Error::ApkParse {
                path: name.into(),
                reason: "本地头签名不匹配".into(),
            });
        }
        let name_len = u16_at(off + 26) as usize;
        let extra_len = u16_at(off + 28) as usize;
        let data_off = off + 30 + name_len + extra_len;
        let data_end = data_off + entry.comp_size as usize;
        if data_end > d.len() {
            return Err(Error::ApkParse {
                path: name.into(),
                reason: "entry 数据越界".into(),
            });
        }
        let raw = &d[data_off..data_end];
        let out = match entry.method {
            0 => raw.to_vec(),
            8 => {
                let mut out = Vec::with_capacity(entry.uncomp_size as usize);
                flate2::read::DeflateDecoder::new(raw)
                    .read_to_end(&mut out)
                    .map_err(|e| Error::ApkParse {
                        path: name.into(),
                        reason: format!("deflate 解压失败: {e}"),
                    })?;
                out
            }
            m => {
                return Err(Error::ApkParse {
                    path: name.into(),
                    reason: format!("不支持的压缩方法 {m}"),
                })
            }
        };
        Ok(Some(out))
    }
}

fn u32_le(d: &[u8], p: usize) -> u32 {
    u32::from_le_bytes([d[p], d[p + 1], d[p + 2], d[p + 3]])
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 在测试里手工构造一个最小 zip：一个 store + 一个 deflate entry。
    fn build_test_zip() -> Vec<u8> {
        fn deflate(data: &[u8]) -> Vec<u8> {
            let mut c = Vec::new();
            flate2::read::DeflateEncoder::new(data, flate2::Compression::default())
                .read_to_end(&mut c)
                .unwrap();
            c
        }
        struct Entry {
            name: &'static str,
            stored: Vec<u8>,
            uncomp_len: u32,
            method: u16,
        }
        let repetitive: Vec<u8> = std::iter::repeat(b'x').take(5000).collect();
        let deflated = deflate(&repetitive);
        let entries = vec![
            Entry {
                name: "AndroidManifest.xml",
                stored: b"<manifest/>".to_vec(),
                uncomp_len: b"<manifest/>".len() as u32,
                method: 0,
            },
            Entry {
                name: "data.bin",
                stored: deflated,
                uncomp_len: repetitive.len() as u32,
                method: 8,
            },
        ];

        let mut out = Vec::new();
        let mut central = Vec::new();
        let mut offsets = Vec::new();
        for e in &entries {
            offsets.push(out.len() as u32);
            out.extend_from_slice(&0x0403_4b50u32.to_le_bytes()); // LOC sig
            out.extend_from_slice(&0u16.to_le_bytes()); // ver need
            out.extend_from_slice(&0u16.to_le_bytes()); // flags
            out.extend_from_slice(&e.method.to_le_bytes());
            out.extend_from_slice(&[0u8; 4]); // time/date
            out.extend_from_slice(&0u32.to_le_bytes()); // crc（读取器不校验）
            out.extend_from_slice(&(e.stored.len() as u32).to_le_bytes()); // comp
            out.extend_from_slice(&e.uncomp_len.to_le_bytes()); // uncomp
            out.extend_from_slice(&(e.name.len() as u16).to_le_bytes());
            out.extend_from_slice(&0u16.to_le_bytes()); // extra len
            out.extend_from_slice(e.name.as_bytes());
            out.extend_from_slice(&e.stored);
        }
        let cd_off = out.len() as u32;
        for (e, off) in entries.iter().zip(&offsets) {
            central.extend_from_slice(&0x0201_4b50u32.to_le_bytes()); // CEN sig
            central.extend_from_slice(&[0u8; 6]); // verMade/verNeed/flags
            central.extend_from_slice(&e.method.to_le_bytes());
            central.extend_from_slice(&[0u8; 4]); // time/date
            central.extend_from_slice(&0u32.to_le_bytes()); // crc
            central.extend_from_slice(&(e.stored.len() as u32).to_le_bytes()); // comp
            central.extend_from_slice(&e.uncomp_len.to_le_bytes()); // uncomp
            central.extend_from_slice(&(e.name.len() as u16).to_le_bytes());
            central.extend_from_slice(&0u16.to_le_bytes()); // extra
            central.extend_from_slice(&0u16.to_le_bytes()); // comment
            central.extend_from_slice(&0u16.to_le_bytes()); // disk start
            central.extend_from_slice(&0u16.to_le_bytes()); // int attr
            central.extend_from_slice(&0u32.to_le_bytes()); // ext attr
            central.extend_from_slice(&off.to_le_bytes());
            central.extend_from_slice(e.name.as_bytes());
        }
        out.extend_from_slice(&central);
        let total = entries.len() as u16;
        out.extend_from_slice(&0x0605_4b50u32.to_le_bytes()); // EOCD sig
        out.extend_from_slice(&[0u8; 4]); // disk numbers
        out.extend_from_slice(&total.to_le_bytes());
        out.extend_from_slice(&total.to_le_bytes());
        out.extend_from_slice(&(central.len() as u32).to_le_bytes());
        out.extend_from_slice(&cd_off.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes()); // comment len
        out
    }

    #[test]
    fn zip_read_store_and_deflate_entries() {
        let bytes = build_test_zip();
        let zip = ZipArchive::from_bytes(bytes, "test.apk".into()).unwrap();
        let manifest = zip.read_entry("AndroidManifest.xml").unwrap().unwrap();
        assert_eq!(manifest, b"<manifest/>");
        let data = zip.read_entry("data.bin").unwrap().unwrap();
        assert_eq!(data.len(), 5000);
        assert!(data.iter().all(|&b| b == b'x'));
    }

    #[test]
    fn zip_missing_entry_returns_none() {
        let zip = ZipArchive::from_bytes(build_test_zip(), "test.apk".into()).unwrap();
        assert!(zip.read_entry("nope.bin").unwrap().is_none());
        let names: Vec<&str> = zip.entry_names().collect();
        assert_eq!(names, vec!["AndroidManifest.xml", "data.bin"]);
    }

    #[test]
    fn zip_garbage_rejected() {
        let err = ZipArchive::from_bytes(vec![1, 2, 3], "bad.apk".into());
        assert!(matches!(err, Err(Error::ApkParse { .. })));
    }
}
