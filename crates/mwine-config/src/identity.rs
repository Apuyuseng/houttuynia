//! 设备伪装身份：由种子确定性派生。
//!
//! 同一种子恒派生同一身份（重启/迁移不变）；不同种子互不相关。
//! 派生仅用于属性伪装与 Android ID 热写入，不涉及运营商网络。

/// 一个种子派生出的设备身份。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceIdentity {
    /// 16 位小写 hex（`settings put secure android_id`，可热生效）。
    pub android_id: String,
    /// 序列号风格：11 位 `[0-9A-Z]`（ro.serialno，经 ramdisk 注入）。
    pub serial_no: String,
    /// 15 位数字伪 IMEI（仅属性伪装用途，无基站语义）。
    pub imei: String,
}

/// SplitMix64：确定性 PRNG（一状态一步出值，质量足够且零依赖）。
fn splitmix64(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
    let mut z = *state;
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

/// 由种子派生设备身份。
pub fn derive_identity(seed: u64) -> DeviceIdentity {
    let mut state = seed;
    let a = splitmix64(&mut state);
    let b = splitmix64(&mut state);
    let c = splitmix64(&mut state);
    let d = splitmix64(&mut state);

    let android_id = format!("{a:016x}");

    // 序列号：11 字符 [0-9A-Z]
    const CHARSET: &[u8] = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    let serial_no: String = [c, d]
        .iter()
        .flat_map(|v| {
            (0..u64::BITS / 6)
                .map(move |i| CHARSET[((v >> (i * 6)) % CHARSET.len() as u64) as usize] as char)
        })
        .take(11)
        .collect();

    // 伪 IMEI：15 位数字（首两位非 00，形似真实号段）
    let mut digits = String::with_capacity(60);
    for v in [a, b, c] {
        digits += &format!("{v:020}"); // u64 十进制恒 20 位（零填充）
    }
    let imei = format!("86{}", &digits[2..15]);

    DeviceIdentity {
        android_id,
        serial_no,
        imei,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn same_seed_same_identity() {
        assert_eq!(derive_identity(42), derive_identity(42));
        assert_ne!(derive_identity(42), derive_identity(43));
    }

    #[test]
    fn formats_match_android_conventions() {
        let id = derive_identity(u64::MAX);
        assert_eq!(id.android_id.len(), 16);
        assert!(id.android_id.chars().all(|c| c.is_ascii_hexdigit()));

        assert_eq!(id.serial_no.len(), 11);
        assert!(id.serial_no.chars().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()));

        assert_eq!(id.imei.len(), 15);
        assert!(id.imei.chars().all(|c| c.is_ascii_digit()));
        assert!(!id.imei.starts_with("00"));
    }
}
