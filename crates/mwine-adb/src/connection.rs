//! ADB smart-socket 连接层：服务请求的长度前缀帧、OKAY/FAIL 状态、
//! 十六进制长度前缀负载（host 级服务）。

use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};

use crate::error::{Error, Result};

const MAX_PAYLOAD: usize = 4 * 1024 * 1024;

pub struct SmartConnection {
    stream: TcpStream,
}

impl SmartConnection {
    pub fn connect<A: ToSocketAddrs>(addr: A) -> Result<Self> {
        let stream = TcpStream::connect(addr).map_err(|e| Error::ServerUnreachable {
            addr: "adb server".into(),
            source: e,
        })?;
        stream.set_nodelay(true).ok();
        Ok(Self { stream })
    }

    pub fn from_stream(stream: TcpStream) -> Self {
        Self { stream }
    }

    pub fn stream(&self) -> &TcpStream {
        &self.stream
    }

    pub fn stream_mut(&mut self) -> &mut TcpStream {
        &mut self.stream
    }

    pub fn into_stream(self) -> TcpStream {
        self.stream
    }

    /// 发送一个服务请求（自动加 4 位十六进制长度前缀），并校验 OKAY/FAIL。
    pub fn send_service(&mut self, service: &str) -> Result<()> {
        let payload = service.as_bytes();
        if payload.len() > 0xFFFF {
            return Err(Error::Protocol("service string too long".into()));
        }
        let framed = format!("{:04x}", payload.len());
        self.stream.write_all(framed.as_bytes())?;
        self.stream.write_all(payload)?;
        read_status(&mut self.stream)
    }

    /// 读取 host 级服务的负载（4 位十六进制长度前缀 + 数据）。
    pub fn read_payload(&mut self) -> Result<Vec<u8>> {
        let len = read_hex_length(&mut self.stream)?;
        if len > MAX_PAYLOAD {
            return Err(Error::Protocol(format!("payload too large: {len}")));
        }
        let mut buf = vec![0u8; len];
        self.stream.read_exact(&mut buf)?;
        Ok(buf)
    }

    /// 读取以 OKAY/FAIL 开头的状态行。
    pub fn read_status(&mut self) -> Result<()> {
        read_status(&mut self.stream)
    }
}

pub fn read_status(stream: &mut TcpStream) -> Result<()> {
    let mut status = [0u8; 4];
    stream.read_exact(&mut status)?;
    match &status {
        b"OKAY" => Ok(()),
        b"FAIL" => {
            let msg = read_hex_payload(stream)?;
            Err(Error::ServiceFailed(
                String::from_utf8_lossy(&msg).trim().to_string(),
            ))
        }
        other => Err(Error::Protocol(format!(
            "unexpected status bytes: {:?}",
            String::from_utf8_lossy(other)
        ))),
    }
}

pub fn read_hex_length(stream: &mut TcpStream) -> Result<usize> {
    let mut hex = [0u8; 4];
    stream.read_exact(&mut hex)?;
    let s = std::str::from_utf8(&hex).map_err(|_| Error::Protocol("invalid hex length".into()))?;
    usize::from_str_radix(s, 16).map_err(|_| Error::Protocol(format!("invalid hex length: {s}")))
}

pub fn read_hex_payload(stream: &mut TcpStream) -> Result<Vec<u8>> {
    let len = read_hex_length(stream)?;
    let mut buf = vec![0u8; len];
    stream.read_exact(&mut buf)?;
    Ok(buf)
}
