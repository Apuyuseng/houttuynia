//! ADB 密钥：加载/生成 ~/.android/adbkey（PKCS8 PEM），AUTH 签名
//! （RSASSA-PKCS1v15-SHA1，与 adbd auth.cpp 一致），Android 公钥格式导出。
//!
//! 线格式（AOSP libcrypto_utils/android_pubkey.cpp，268B@2048bit，已用
//! 本机 adb 真实生成的 adbkey.pub 逐字节定标）：
//!   len(4, LE u32, 32 位字数=64) | n0inv(4, LE, -n⁻¹ mod 2³²)
//!   | n(len*4 字节，整串字节反转=小端字序) | exponent(4, LE=65537)
//!   → base64（无 padding）

use rsa::traits::PublicKeyParts;
use rsa::RsaPrivateKey;
use rsa::pkcs8::DecodePrivateKey;
use rsa::pkcs8::EncodePrivateKey;

static KEY: std::sync::LazyLock<std::sync::Mutex<RsaPrivateKey>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(load_or_create_inner()));

fn key_path() -> String {
    let home = std::env::var("HOME").unwrap_or_default();
    format!("{home}/.android/adbkey")
}

fn load_or_create_inner() -> RsaPrivateKey {
    let path = key_path();
    if let Ok(pem) = std::fs::read_to_string(&path) {
        if let Ok(k) = RsaPrivateKey::from_pkcs8_pem(&pem) {
            return k;
        }
        // 旧格式（PKCS1）兼容
        if let Ok(k) = rsa::pkcs1::DecodeRsaPrivateKey::from_pkcs1_pem(&pem) {
            return k;
        }
    }
    // 生成 RSA-2048（adbd 解码器只认 2048/4096；2048 是全版本最大兼容档）
    let mut rng = rsa::rand_core::OsRng;
    let key = RsaPrivateKey::new(&mut rng, 2048).expect("RSA 密钥生成");
    if let Ok(pem) = key.to_pkcs8_pem(rsa::pkcs8::LineEnding::LF) {
        let _ = std::fs::create_dir_all(std::path::Path::new(&path).parent().unwrap());
        let _ = std::fs::write(&path, pem.as_bytes());
        // 公钥（Android 线格式 base64）
        let pub_android = android_pubkey(&key);
        let _ = std::fs::write(format!("{path}.pub"), format!("{pub_android} mwine@nas\n"));
        // 权限 600
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600));
        }
    }
    key
}

pub fn load_or_create() -> RsaPrivateKey {
    KEY.lock().unwrap().clone()
}

/// AUTH 应答签名：对 token 先 SHA1 再 RSA PKCS1v15 签名（与 adbd 验签一致：
/// 设备侧 RSA_public_decrypt 后比对 DigestInfo||SHA1(token)，等价 AOSP 的
/// RSA_sign(NID_sha1, ...)）。
/// 手动构造 PKCS#1 v1.5 块（00 01 FF..FF 00 || DigestInfo || digest）再裸 RSA。
/// 注意：rsa crate 的 key.size() 返回模数【字节数】（2048bit → 256），
/// 曾误当位数再除 8 → 32 字节垃圾签名 → adbd 验签失败踢传输（荣耀
/// MagicOS 上即 CNXN 后立即 USB 断开）。
pub fn sign_token(key: &RsaPrivateKey, token: &[u8]) -> Vec<u8> {
    use rsa::traits::{PrivateKeyParts, PublicKeyParts};
    use sha1::Sha1;
    use sha2::Digest;
    let digest: [u8; 20] = Sha1::digest(token).into();
    // DigestInfo(SHA1) DER 前缀（固定 15 字节）
    const SHA1_DIGEST_INFO_PREFIX: [u8; 15] = [
        0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14,
    ];
    let mut t = Vec::with_capacity(35);
    t.extend_from_slice(&SHA1_DIGEST_INFO_PREFIX);
    t.extend_from_slice(&digest);
    // PKCS#1 v1.5 块：EM = 0x00 0x01 PS(0xFF..) 0x00 T；k = 模数字节数
    let k = key.size(); // 字节数！（rsa crate PublicKeyParts::size 语义）
    debug_assert_eq!(k, (key.n().bits() as usize + 7) / 8, "size() 语义应为字节数");
    let mut em = Vec::with_capacity(k);
    em.push(0x00);
    em.push(0x01);
    let ps_len = k - 3 - t.len();
    em.extend(std::iter::repeat(0xFFu8).take(ps_len));
    em.push(0x00);
    em.extend_from_slice(&t);
    debug_assert_eq!(em.len(), k);
    // 裸 RSA：m^d mod n
    use rsa::BigUint;
    let m = BigUint::from_bytes_be(&em);
    let n = BigUint::from_bytes_be(&key.n().to_bytes_be());
    let d = BigUint::from_bytes_be(&key.d().to_bytes_be());
    let s = m.modpow(&d, &n);
    let mut out = s.to_bytes_be();
    // 左补零到 k 字节
    if out.len() < k {
        let mut padded = vec![0u8; k - out.len()];
        padded.extend_from_slice(&out);
        out = padded;
    }
    out
}

/// Android 公钥线格式导出（见模块头注释；仅支持 2048/4096 位）。
pub fn android_pubkey(key: &RsaPrivateKey) -> String {
    use base64::Engine;
    let n = key.to_public_key().n().to_bytes_be();
    let words = n.len() / 4; // 32 位字数（2048bit → 64）
    // n0inv = -n⁻¹ mod 2³²（n 的最低 32 位字的模逆）
    let n0 = u32::from_be_bytes([n[n.len() - 4], n[n.len() - 3], n[n.len() - 2], n[n.len() - 1]]);
    let n0inv = n0.inv_mod_neg();
    let mut buf = Vec::with_capacity(4 + 4 + n.len() + 4);
    buf.extend_from_slice(&(words as u32).to_le_bytes());
    buf.extend_from_slice(&n0inv.to_le_bytes());
    buf.extend(n.iter().rev()); // 小端字序 = 整串反转
    buf.extend_from_slice(&65537u32.to_le_bytes());
    base64::engine::general_purpose::STANDARD_NO_PAD.encode(buf)
}

/// -self⁻¹ mod 2³²（牛顿迭代求模逆：x_{k+1} = x_k·(2 - a·x_k)，5 步收敛）
trait InvModNeg {
    fn inv_mod_neg(&self) -> u32;
}
impl InvModNeg for u32 {
    fn inv_mod_neg(&self) -> u32 {
        let a = *self; // 奇数
        let mut x: u32 = 1; // 满足 a·x ≡ 1 (mod 2) 的初值
        for _ in 0..5 {
            x = x.wrapping_mul(2u32.wrapping_sub(a.wrapping_mul(x)));
        }
        x.wrapping_neg()
    }
}
