//! 模拟器控制台（telnet 风格文本协议）：serial → console 端口，auth token，命令应答。
//! 对齐 cpp/modules/supervisor/vm.cpp 的 emuConsoleCmdRaw。

use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::{Duration, Instant};

use crate::adb::{AdbError, AdbResult};

/// emulator-5554 → 5554（console 端口即 serial 数字部分）
pub fn console_port(serial: &str) -> AdbResult<u16> {
    serial
        .strip_prefix("emulator-")
        .and_then(|s| s.parse::<u16>().ok())
        .ok_or_else(|| AdbError(format!("无法从 serial 解析控制台端口: {serial}")))
}

/// 发一条控制台命令，读应答（large=true 追读至静默，上限 64KB）。
pub fn cmd(serial: &str, command: &str, large: bool) -> AdbResult<Vec<u8>> {
    let port = console_port(serial)?;
    let home = std::env::var("HOME").unwrap_or_default();
    let token = std::fs::read_to_string(format!("{home}/.emulator_console_auth_token"))
        .map(|t| t.trim().to_string())
        .unwrap_or_default();

    let mut s = TcpStream::connect(("127.0.0.1", port))
        .map_err(|e| AdbError(format!("连接 emulator 控制台失败: {e}")))?;
    s.set_read_timeout(Some(Duration::from_millis(500))).ok();

    // 读问候（有则读）
    let mut buf = [0u8; 4096];
    let _ = s.read(&mut buf);
    if !token.is_empty() {
        s.write_all(format!("auth {token}\n").as_bytes()).ok();
        s.flush().ok();
        let _ = s.read(&mut buf);
    }
    s.write_all(format!("{command}\n").as_bytes())
        .map_err(|e| AdbError(format!("发送控制台命令失败: {e}")))?;
    s.flush().ok();

    let mut out = Vec::new();
    if !large {
        s.set_read_timeout(Some(Duration::from_secs(3))).ok();
        match s.read(&mut buf) {
            Ok(n) => out.extend_from_slice(&buf[..n]),
            Err(_) => {}
        }
        return Ok(out);
    }
    // large：追读直到静默
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        if Instant::now() > deadline || out.len() > 64 * 1024 {
            break;
        }
        match s.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => out.extend_from_slice(&buf[..n]),
            Err(e)
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                if !out.is_empty() {
                    break; // 静默 = 应答完毕
                }
            }
            Err(_) => break,
        }
    }
    Ok(out)
}

/// 控制台应答文本里抹掉 OK/提示行后的有效内容。
pub fn clean_output(raw: &[u8]) -> String {
    let text = String::from_utf8_lossy(raw);
    let mut lines: Vec<String> = Vec::new();
    for l in text.lines() {
        let t = l.trim_end();
        if t.starts_with("OK") || t.starts_with("Android Console") || t.starts_with("auth ")
            || t.starts_with("Type ")
        {
            continue;
        }
        lines.push(t.to_string());
    }
    lines.join("\n").trim().to_string()
}
