//! emulator console 协议（与 `adb emu` / SDK emulator launcher 同路径）：
//! TCP 连 console 端口 → 问候 → auth token → 命令 → 应答。

use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

use crate::{Error, Result};

/// 认证 token 文件（qemu 与 emulator launcher 同源逻辑，二者互认）。
pub fn console_token() -> String {
    std::env::var_os("HOME")
        .map(|h| std::path::PathBuf::from(h).join(".emulator_console_auth_token"))
        .and_then(|p| std::fs::read_to_string(p).ok())
        .unwrap_or_default()
        .trim()
        .to_string()
}

/// 对指定 console 端口执行一条命令，返回原始应答文本。
/// 应答以 OK/KO 结尾（多数命令），由调用方判断。
pub fn console_command(port: u16, token: &str, cmd: &str) -> Result<String> {
    let mut stream = TcpStream::connect(("127.0.0.1", port))
        .map_err(|e| Error::Console(format!("连接 console :{port} 失败: {e}")))?;
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    stream.set_write_timeout(Some(Duration::from_secs(3)))?;

    // 问候语（读到静默或超时均可，控制台行为随版本略有差异）
    let mut buf = [0u8; 1024];
    let _ = stream.read(&mut buf);
    if !token.is_empty() {
        writeln!(stream, "auth {token}")?;
        let mut resp = [0u8; 256];
        let _ = stream.read(&mut resp);
    }
    writeln!(stream, "{cmd}")?;

    // 循环读到对端静默/关闭，上限 8KB（kill/multidisplay 等应答都很短）
    let mut out = Vec::new();
    let mut chunk = [0u8; 1024];
    loop {
        match stream.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                out.extend_from_slice(&chunk[..n]);
                if out.len() >= 8 * 1024 {
                    break;
                }
            }
            Err(_) => break, // EWOULDBLOCK/超时：应答已收完
        }
    }
    Ok(String::from_utf8_lossy(&out).to_string())
}

/// 应答是否为 KO（命令被拒）。
pub fn is_ko(resp: &str) -> bool {
    resp.contains("KO")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn token_is_trimmed_or_empty() {
        // 无 token 文件的环境下不应 panic，返回空串即可
        let t = console_token();
        assert!(!t.contains('\n'), "token 不应含换行");
    }

    #[test]
    fn ko_detection() {
        assert!(is_ko("KO: unknown command"));
        assert!(!is_ko("OK\r\n"));
    }

    #[test]
    fn console_command_on_closed_port_fails_gracefully() {
        // 找一个必然空闲的端口（bind 后立刻释放仍有竞态，改用保留端口段）
        let err = console_command(1, "", "avd name").unwrap_err();
        assert!(matches!(err, Error::Console(_)));
    }
}
