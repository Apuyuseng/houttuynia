//! shell 服务：v2 协议（stdout/stderr/exit 分离）与 v1 回退。

use std::io::{Read, Write};

use crate::error::{Error, Result};

#[derive(Debug, Clone)]
pub struct ShellOutput {
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
    /// v2 协议下有值；v1 回退时为 None
    pub exit_code: Option<u8>,
}

impl ShellOutput {
    pub fn stdout_string(&self) -> String {
        String::from_utf8_lossy(&self.stdout).to_string()
    }

    pub fn ok(&self) -> bool {
        self.exit_code.map(|c| c == 0).unwrap_or(true)
    }
}

/// shell v2 流读取器：`[id: u8][len: u32 LE][data]`，
/// id 0x1=stdout, 0x2=stderr, 0x3=exit(1 字节)。
pub struct ShellV2Reader<R: Read> {
    reader: R,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShellChunk {
    Stdout,
    Stderr,
    Exit,
}

impl<R: Read> ShellV2Reader<R> {
    pub fn new(reader: R) -> Self {
        Self { reader }
    }

    /// 读取下一个 chunk，流结束返回 None。
    pub fn next_chunk(&mut self) -> Result<Option<(ShellChunk, Vec<u8>)>> {
        let mut header = [0u8; 5];
        match read_full(&mut self.reader, &mut header) {
            Ok(true) => {}
            Ok(false) => return Ok(None), // EOF
            Err(e) => return Err(e.into()),
        }
        let id = header[0];
        let len = u32::from_le_bytes([header[1], header[2], header[3], header[4]]) as usize;
        let mut data = vec![0u8; len];
        self.reader.read_exact(&mut data)?;
        let chunk = match id {
            0x1 => ShellChunk::Stdout,
            0x2 => ShellChunk::Stderr,
            0x3 => ShellChunk::Exit,
            other => {
                return Err(Error::Protocol(format!(
                    "unknown shell v2 chunk id: {other:#x}"
                )))
            }
        };
        Ok(Some((chunk, data)))
    }

    /// 消费到结束，返回完整输出。
    pub fn collect(mut self) -> Result<ShellOutput> {
        let mut out = ShellOutput {
            stdout: Vec::new(),
            stderr: Vec::new(),
            exit_code: None,
        };
        while let Some((chunk, data)) = self.next_chunk()? {
            match chunk {
                ShellChunk::Stdout => out.stdout.extend_from_slice(&data),
                ShellChunk::Stderr => out.stderr.extend_from_slice(&data),
                ShellChunk::Exit => {
                    out.exit_code = data.first().copied();
                }
            }
        }
        Ok(out)
    }
}

/// v1：读到 EOF，stdout/stderr 混流。
pub fn read_v1_to_end<R: Read>(mut reader: R) -> Result<Vec<u8>> {
    let mut buf = Vec::new();
    reader.read_to_end(&mut buf)?;
    Ok(buf)
}

/// 组装 shell v2 服务名。禁用 PTY 相关参数，走 raw 模式。
pub fn v2_service(cmd: &str) -> String {
    format!("shell,v2,raw:{cmd}")
}

pub fn v1_service(cmd: &str) -> String {
    format!("shell:{cmd}")
}

fn read_full<R: Read>(reader: &mut R, buf: &mut [u8]) -> std::io::Result<bool> {
    let mut filled = 0;
    while filled < buf.len() {
        let n = reader.read(&mut buf[filled..])?;
        if n == 0 {
            if filled == 0 {
                return Ok(false);
            }
            return Err(std::io::Error::new(
                std::io::ErrorKind::UnexpectedEof,
                "eof in the middle of shell v2 header",
            ));
        }
        filled += n;
    }
    Ok(true)
}

/// 供测试与其他模块复用的 write 帮助函数。
pub fn write_all<W: Write>(w: &mut W, buf: &[u8]) -> Result<()> {
    w.write_all(buf)?;
    Ok(())
}
