//! 网关自带 TLS：自签证书自动生成/持久化，HTTPS 监听（WebCodecs 投屏需要
//! 安全上下文，纯 HTTP 下浏览器不暴露 VideoDecoder）。
//!
//! 证书落在 `$HOME/.mwine/tls/{cert,key}.pem`（持久卷里，重启不变——用户
//! 浏览器只需接受一次自签警告）。SAN 自动含 localhost/127.0.0.1/::1 与
//! 本机主出口 IP（host 网络容器里即宿主 LAN IP）。

use std::net::IpAddr;
use std::path::PathBuf;

fn tls_dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_default();
    let dir = PathBuf::from(format!("{home}/.mwine/tls"));
    let _ = std::fs::create_dir_all(&dir);
    dir
}

/// 本机全部非回环 IPv4（getifaddrs 枚举；多网卡宿主上 SAN 都要覆盖，
/// 否则用户按另一张网卡的 IP 访问时证书域不匹配）
fn local_ipv4s() -> Vec<IpAddr> {
    let mut out = Vec::new();
    unsafe {
        let mut ifaddrs: *mut libc::ifaddrs = std::ptr::null_mut();
        if libc::getifaddrs(&mut ifaddrs) != 0 {
            return out;
        }
        let mut cur = ifaddrs;
        while !cur.is_null() {
            let ifa = &*cur;
            let sa = ifa.ifa_addr;
            if !sa.is_null() && (*sa).sa_family as u16 == libc::AF_INET as u16 {
                let sin = &*(sa as *const libc::sockaddr_in);
                let ip = std::net::Ipv4Addr::from(u32::from_be(sin.sin_addr.s_addr));
                if !ip.is_loopback() && !out.contains(&IpAddr::V4(ip)) {
                    out.push(IpAddr::V4(ip));
                }
            }
            cur = ifa.ifa_next;
        }
        libc::freeifaddrs(ifaddrs);
    }
    out
}

/// 读取已有证书；缺失时生成自签（SAN 含 localhost/回环 + 全部本机 IPv4）并落盘。
/// 返回 (cert_pem_path, key_pem_path)。
pub fn ensure_self_signed() -> std::io::Result<(PathBuf, PathBuf)> {
    let dir = tls_dir();
    let cert_p = dir.join("cert.pem");
    let key_p = dir.join("key.pem");
    if cert_p.is_file() && key_p.is_file() {
        return Ok((cert_p, key_p));
    }
    let mut sans = vec![
        "localhost".to_string(),
        "127.0.0.1".to_string(),
        "::1".to_string(),
    ];
    for ip in local_ipv4s() {
        sans.push(ip.to_string());
    }
    let cert = rcgen::generate_simple_self_signed(sans)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))?;
    let pem = cert.cert.pem();
    let key = cert.key_pair.serialize_pem();
    std::fs::write(&cert_p, pem)?;
    std::fs::write(&key_p, key)?;
    // 私钥限主用户可读
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&key_p, std::fs::Permissions::from_mode(0o600));
    }
    tracing::info!("TLS 自签证书已生成: {}", cert_p.display());
    Ok((cert_p, key_p))
}
