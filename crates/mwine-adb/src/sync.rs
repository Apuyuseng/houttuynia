//! adb sync 子协议：文件推送（SEND / DATA / DONE，回复 OKAY / FAIL）
//! 与拉取（STAT / RECV）。所有整数均为小端。

use std::io::{Read, Write};
use std::net::TcpStream;

use crate::error::{Error, Result};

const SYNC_DATA_MAX: usize = 64 * 1024;
/// 0o100666：常规文件、所有人可读写
const REGULAR_FILE_MODE: u32 = 33_206;

pub struct SyncChannel<'a> {
    stream: &'a mut TcpStream,
}

impl<'a> SyncChannel<'a> {
    pub fn new(stream: &'a mut TcpStream) -> Self {
        Self { stream }
    }

    fn write_request(&mut self, cmd: &[u8; 4], payload: &[u8]) -> Result<()> {
        self.stream.write_all(cmd)?;
        self.stream
            .write_all(&(payload.len() as u32).to_le_bytes())?;
        self.stream.write_all(payload)?;
        Ok(())
    }

    pub fn send_header(&mut self, remote_path: &str, mode: u32) -> Result<()> {
        let payload = format!("{remote_path},{mode}");
        self.write_request(b"SEND", payload.as_bytes())
    }

    pub fn data_chunk(&mut self, data: &[u8]) -> Result<()> {
        self.write_request(b"DATA", data)
    }

    pub fn done(&mut self, mtime: u32) -> Result<()> {
        self.stream.write_all(b"DONE")?;
        self.stream.write_all(&mtime.to_le_bytes())?;
        Ok(())
    }

    pub fn quit(&mut self) -> Result<()> {
        self.stream.write_all(b"QUIT")?;
        self.stream.write_all(&0u32.to_le_bytes())?;
        Ok(())
    }

    pub fn read_reply(&mut self) -> Result<()> {
        let mut header = [0u8; 8];
        self.stream.read_exact(&mut header)?;
        let status = &header[0..4];
        let len = u32::from_le_bytes([header[4], header[5], header[6], header[7]]) as usize;
        match status {
            b"OKAY" => Ok(()),
            b"FAIL" => {
                let mut msg = vec![0u8; len];
                self.stream.read_exact(&mut msg)?;
                Err(Error::ServiceFailed(
                    String::from_utf8_lossy(&msg).trim().to_string(),
                ))
            }
            other => Err(Error::Protocol(format!(
                "unexpected sync reply: {:?}",
                String::from_utf8_lossy(other)
            ))),
        }
    }

    /// 流式推送（边读边发，进度回调按已发字节报告；顺带避免大 APK 整读内存）。
    pub fn push_stream<R: Read>(
        &mut self,
        reader: &mut R,
        remote_path: &str,
        mut on_progress: impl FnMut(u64),
    ) -> Result<()> {
        self.send_header(remote_path, REGULAR_FILE_MODE)?;
        let mut buf = vec![0u8; SYNC_DATA_MAX];
        let mut sent: u64 = 0;
        loop {
            let n = reader.read(&mut buf)?;
            if n == 0 {
                break;
            }
            self.data_chunk(&buf[..n])?;
            sent += n as u64;
            on_progress(sent);
        }
        self.done(0)?;
        self.read_reply()?;
        Ok(())
    }

    /// 完整推送一段内存数据到设备。
    pub fn push_bytes(&mut self, data: &[u8], remote_path: &str) -> Result<()> {
        self.send_header(remote_path, REGULAR_FILE_MODE)?;
        for chunk in data.chunks(SYNC_DATA_MAX) {
            self.data_chunk(chunk)?;
        }
        // mtime=0 让设备使用当前时间
        self.done(0)?;
        self.read_reply()?;
        Ok(())
    }

    /// 拉取设备文件（STAT 校验 + RECV 循环）。
    /// STAT 应答为 16 字节裸数据（id + mode/size/mtime，无长度前缀）；
    /// RECV 应答为 DATA 块序列，以 DONE 结束。
    pub fn pull_file(&mut self, remote_path: &str) -> Result<Vec<u8>> {
        self.write_request(b"STAT", remote_path.as_bytes())?;
        let mut stat = [0u8; 16];
        self.stream.read_exact(&mut stat)?;
        if &stat[0..4] != b"STAT" {
            return Err(Error::Protocol(format!(
                "unexpected sync stat reply: {:?}",
                String::from_utf8_lossy(&stat[0..4])
            )));
        }
        let mode = u32::from_le_bytes([stat[4], stat[5], stat[6], stat[7]]);
        if mode == 0 {
            return Err(Error::ServiceFailed(format!("no such file: {remote_path}")));
        }

        self.write_request(b"RECV", remote_path.as_bytes())?;
        let mut out = Vec::new();
        loop {
            let mut header = [0u8; 8];
            self.stream.read_exact(&mut header)?;
            let id = [header[0], header[1], header[2], header[3]];
            let len = u32::from_le_bytes([header[4], header[5], header[6], header[7]]) as usize;
            match &id {
                b"DATA" => {
                    let mut chunk = vec![0u8; len];
                    self.stream.read_exact(&mut chunk)?;
                    out.extend_from_slice(&chunk);
                }
                // DONE 的长度字段实际是 mtime
                b"DONE" => return Ok(out),
                b"FAIL" => {
                    let mut msg = vec![0u8; len];
                    self.stream.read_exact(&mut msg)?;
                    return Err(Error::ServiceFailed(
                        String::from_utf8_lossy(&msg).trim().to_string(),
                    ));
                }
                other => {
                    return Err(Error::Protocol(format!(
                        "unexpected sync recv reply: {:?}",
                        String::from_utf8_lossy(other)
                    )))
                }
            }
        }
    }
}
