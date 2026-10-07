//! 视频流解复用：codec id + session meta + 帧包（PTS/flags 前缀）。
//! 与 server 端 Streamer.java 对齐，大端。

use std::io::Read;
use std::net::TcpStream;

use crate::error::{Error, Result};
use crate::options::VideoCodecId;

const PACKET_FLAG_SESSION: u64 = 1 << 63;
const PACKET_FLAG_CONFIG: u64 = 1 << 62;
const PACKET_FLAG_KEY_FRAME: u64 = 1 << 61;

/// 一个视频包（可能是配置包 SPS/PPS 或实际帧数据）。
#[derive(Debug, Clone)]
pub struct VideoPacket {
    pub pts: u64,
    pub config: bool,
    pub key_frame: bool,
    pub data: Vec<u8>,
}

/// 视频包解析核心，泛型于 reader。
pub struct PacketDemuxer<R: Read> {
    reader: R,
    pub width: u32,
    pub height: u32,
}

impl<R: Read> PacketDemuxer<R> {
    pub fn new(reader: R) -> Self {
        Self {
            reader,
            width: 0,
            height: 0,
        }
    }

    /// 读取下一个视频包；同时吸收中途出现的 session meta（分辨率变化）。
    /// 流结束返回 None。
    pub fn next_packet(&mut self) -> Result<Option<VideoPacket>> {
        loop {
            let mut header = [0u8; 8];
            match read_full(&mut self.reader, &mut header) {
                Ok(true) => {}
                Ok(false) => return Ok(None),
                Err(e) => return Err(e.into()),
            }
            let pts_and_flags = u64::from_be_bytes(header);
            if pts_and_flags & PACKET_FLAG_SESSION != 0 {
                // session meta：前 8 字节 = [flags(4B)][width(4B)]，再读 4 字节 height
                let width = (pts_and_flags & 0xFFFF_FFFF) as u32;
                let mut h = [0u8; 4];
                self.reader.read_exact(&mut h)?;
                self.width = width;
                self.height = u32::from_be_bytes(h);
                continue;
            }
            let config = pts_and_flags & PACKET_FLAG_CONFIG != 0;
            let key_frame = pts_and_flags & PACKET_FLAG_KEY_FRAME != 0;
            let pts = pts_and_flags & !(PACKET_FLAG_CONFIG | PACKET_FLAG_KEY_FRAME);
            let mut size = [0u8; 4];
            self.reader.read_exact(&mut size)?;
            let size = u32::from_be_bytes(size) as usize;
            let mut data = vec![0u8; size];
            self.reader.read_exact(&mut data)?;
            return Ok(Some(VideoPacket {
                pts,
                config,
                key_frame,
                data,
            }));
        }
    }
}

/// 从 socket 读取并解析视频流。
pub struct VideoStream {
    demux: PacketDemuxer<TcpStream>,
    pub codec: VideoCodecId,
}

impl VideoStream {
    /// 读取流头部：4 字节 codec id。
    pub fn new(mut stream: TcpStream) -> Result<Self> {
        let mut codec = [0u8; 4];
        stream.read_exact(&mut codec)?;
        let id = u32::from_be_bytes(codec);
        if id == 0 {
            return Err(Error::Protocol(
                "device disabled the video stream (codec id = 0)".into(),
            ));
        }
        if id == 1 {
            return Err(Error::Protocol(
                "video stream configuration error on device (codec id = 1)".into(),
            ));
        }
        Ok(Self {
            demux: PacketDemuxer::new(stream),
            codec: VideoCodecId::from_wire(id)?,
        })
    }

    pub fn codec(&self) -> VideoCodecId {
        self.codec
    }

    pub fn size(&self) -> (u32, u32) {
        (self.demux.width, self.demux.height)
    }

    pub fn next_packet(&mut self) -> Result<Option<VideoPacket>> {
        self.demux.next_packet()
    }

    /// 裸访问底层流（录制整流等用途）。
    pub fn stream_mut(&mut self) -> &mut TcpStream {
        &mut self.demux.reader
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
                "eof in the middle of frame header",
            ));
        }
        filled += n;
    }
    Ok(true)
}

/// 读取视频 socket 的连接头：1 字节 dummy + 64 字节设备名。
/// 设备名在 server 完成 video+control 双 accept 之后才发送。
pub fn read_connection_header<R: Read>(reader: &mut R) -> Result<String> {
    let mut dummy = [0u8; 1];
    reader.read_exact(&mut dummy)?;
    read_device_name(reader)
}

/// 仅读取 64 字节设备名（dummy 字节已被连接流程消费时使用）。
pub fn read_device_name<R: Read>(reader: &mut R) -> Result<String> {
    let mut name = [0u8; 64];
    reader.read_exact(&mut name)?;
    let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
    Ok(String::from_utf8_lossy(&name[..end]).to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 构造与 Streamer.java 相同字节序的 fixture。
    fn fixture() -> Vec<u8> {
        let mut b = Vec::new();
        // codec id: h264（由 VideoStream::new 消费，demuxer 不含）
        b.extend_from_slice(&0x6832_3634u32.to_be_bytes());
        // session meta: flags=0x80000000, w=1280, h=720
        b.extend_from_slice(&0x8000_0000u32.to_be_bytes());
        b.extend_from_slice(&1280u32.to_be_bytes());
        b.extend_from_slice(&720u32.to_be_bytes());
        // config packet (SPS): pts=flags=CONFIG, size=3, data [1,2,3]
        b.extend_from_slice(&PACKET_FLAG_CONFIG.to_be_bytes());
        b.extend_from_slice(&3u32.to_be_bytes());
        b.extend_from_slice(&[1, 2, 3]);
        // keyframe: pts=1000
        let kf = 1000u64 | PACKET_FLAG_KEY_FRAME;
        b.extend_from_slice(&kf.to_be_bytes());
        b.extend_from_slice(&2u32.to_be_bytes());
        b.extend_from_slice(&[0xAA, 0xBB]);
        // 中途 session meta（分辨率变化 800x600，client-resize 标志位）
        b.extend_from_slice(&0x8000_0001u32.to_be_bytes());
        b.extend_from_slice(&800u32.to_be_bytes());
        b.extend_from_slice(&600u32.to_be_bytes());
        // 普通 p 帧
        b.extend_from_slice(&2000u64.to_be_bytes());
        b.extend_from_slice(&1u32.to_be_bytes());
        b.extend_from_slice(&[0xCC]);
        b
    }

    #[test]
    fn demux_fixture() {
        let mut cursor = std::io::Cursor::new(fixture());
        // 先按 VideoStream::new 的方式读 codec
        let mut codec = [0u8; 4];
        cursor.read_exact(&mut codec).unwrap();
        assert_eq!(u32::from_be_bytes(codec), 0x6832_3634);

        let mut demux = PacketDemuxer::new(cursor);
        let p1 = demux.next_packet().unwrap().unwrap();
        assert!(p1.config);
        assert!(!p1.key_frame);
        assert_eq!(p1.data, vec![1, 2, 3]);
        assert_eq!(demux.width, 1280);
        assert_eq!(demux.height, 720);

        let p2 = demux.next_packet().unwrap().unwrap();
        assert!(p2.key_frame);
        assert_eq!(p2.pts, 1000);
        assert_eq!(p2.data, vec![0xAA, 0xBB]);

        let p3 = demux.next_packet().unwrap().unwrap();
        assert!(!p3.config);
        assert_eq!(p3.pts, 2000);
        assert_eq!(p3.data, vec![0xCC]);
        assert_eq!(demux.width, 800);
        assert_eq!(demux.height, 600);

        assert!(demux.next_packet().unwrap().is_none());
    }

    #[test]
    fn connection_header_parse() {
        let mut buf = vec![0u8];
        let name = "test_device";
        let mut name_bytes = [0u8; 64];
        name_bytes[..name.len()].copy_from_slice(name.as_bytes());
        buf.extend_from_slice(&name_bytes);
        let mut cursor = std::io::Cursor::new(buf);
        let parsed = read_connection_header(&mut cursor).unwrap();
        assert_eq!(parsed, "test_device");
    }
}
