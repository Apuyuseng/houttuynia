//! 音频流解复用：codec id + 帧包（PTS/flags 前缀）。
//! 与 server 端 Streamer.java 对齐，大端；音频包只使用 config 标志位。

use std::io::Read;
use std::net::TcpStream;

use crate::error::{Error, Result};

const PACKET_FLAG_CONFIG: u64 = 1 << 63;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AudioCodecId {
    Flac,
    Opus,
    Aac,
}

impl AudioCodecId {
    pub fn wire_id(self) -> u32 {
        match self {
            AudioCodecId::Flac => 0x666c_6163, // "flac"
            AudioCodecId::Opus => 0x6f70_7573, // "opus"
            AudioCodecId::Aac => 0x6161_6363,  // "aac"
        }
    }

    pub fn from_wire(id: u32) -> Result<Self> {
        match id {
            0x666c_6163 => Ok(AudioCodecId::Flac),
            0x6f70_7573 => Ok(AudioCodecId::Opus),
            0x6161_6363 => Ok(AudioCodecId::Aac),
            other => Err(Error::Protocol(format!(
                "unknown audio codec id: {other:#x}"
            ))),
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            AudioCodecId::Flac => "flac",
            AudioCodecId::Opus => "opus",
            AudioCodecId::Aac => "aac",
        }
    }
}

/// 一个音频包（config 包为解码器初始化数据，如 FLAC 头）。
#[derive(Debug, Clone)]
pub struct AudioPacket {
    pub pts: u64,
    pub config: bool,
    pub data: Vec<u8>,
}

/// 音频包解析核心，泛型于 reader。
pub struct AudioDemuxer<R: Read> {
    reader: R,
}

impl<R: Read> AudioDemuxer<R> {
    pub fn new(reader: R) -> Self {
        Self { reader }
    }

    /// 读取下一个音频包；流结束返回 None。
    pub fn next_packet(&mut self) -> Result<Option<AudioPacket>> {
        let mut header = [0u8; 8];
        match read_full(&mut self.reader, &mut header) {
            Ok(true) => {}
            Ok(false) => return Ok(None),
            Err(e) => return Err(e.into()),
        }
        let pts_and_flags = u64::from_be_bytes(header);
        let config = pts_and_flags & PACKET_FLAG_CONFIG != 0;
        let pts = pts_and_flags & !PACKET_FLAG_CONFIG;
        let mut size = [0u8; 4];
        self.reader.read_exact(&mut size)?;
        let size = u32::from_be_bytes(size) as usize;
        let mut data = vec![0u8; size];
        self.reader.read_exact(&mut data)?;
        Ok(Some(AudioPacket { pts, config, data }))
    }
}

/// 从 socket 读取并解析音频流。
pub struct AudioStream {
    demux: AudioDemuxer<TcpStream>,
    pub codec: AudioCodecId,
}

impl AudioStream {
    /// 读取流头部：4 字节 codec id。
    pub fn new(mut stream: TcpStream) -> Result<Self> {
        let mut codec = [0u8; 4];
        stream.read_exact(&mut codec)?;
        let id = u32::from_be_bytes(codec);
        if id == 0 {
            return Err(Error::Protocol(
                "device disabled the audio stream (codec id = 0)".into(),
            ));
        }
        if id == 1 {
            return Err(Error::Protocol(
                "audio stream configuration error on device (codec id = 1)".into(),
            ));
        }
        Ok(Self {
            demux: AudioDemuxer::new(stream),
            codec: AudioCodecId::from_wire(id)?,
        })
    }

    pub fn codec(&self) -> AudioCodecId {
        self.codec
    }

    /// 读取下一个音频包；流结束返回 None。
    pub fn next_packet(&mut self) -> Result<Option<AudioPacket>> {
        self.demux.next_packet()
    }
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
                "eof in the middle of audio packet header",
            ));
        }
        filled += n;
    }
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 与 Streamer.java 字节序一致的 fixture（codec id 由 new 消费）。
    fn fixture() -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(&AudioCodecId::Flac.wire_id().to_be_bytes());
        // config 包：flac 头 42 字节（示意 4 字节）
        b.extend_from_slice(&PACKET_FLAG_CONFIG.to_be_bytes());
        b.extend_from_slice(&4u32.to_be_bytes());
        b.extend_from_slice(b"fLaC");
        // 数据包：pts=1234
        b.extend_from_slice(&1234u64.to_be_bytes());
        b.extend_from_slice(&3u32.to_be_bytes());
        b.extend_from_slice(&[0xAA, 0xBB, 0xCC]);
        b
    }

    #[test]
    fn demux_fixture() {
        let mut cursor = std::io::Cursor::new(fixture());
        let mut codec = [0u8; 4];
        use std::io::Read;
        cursor.read_exact(&mut codec).unwrap();
        assert_eq!(u32::from_be_bytes(codec), AudioCodecId::Flac.wire_id());

        let mut demux = AudioDemuxer::new(cursor);
        let p1 = demux.next_packet().unwrap().unwrap();
        assert!(p1.config);
        assert_eq!(p1.data, b"fLaC".to_vec());
        let p2 = demux.next_packet().unwrap().unwrap();
        assert!(!p2.config);
        assert_eq!(p2.pts, 1234);
        assert_eq!(p2.data, vec![0xAA, 0xBB, 0xCC]);
        assert!(demux.next_packet().unwrap().is_none());
    }
}
