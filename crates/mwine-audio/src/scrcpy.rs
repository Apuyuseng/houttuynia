//! scrcpy 音频源（路径 2）：把 [`mwine_scrcpy::AudioStream`]（FLAC 压缩包）
//! 解码为交错 s16 PCM，送入 [`AudioStream`] 回调。
//!
//! 解码用 ffmpeg 子进程（与 mwine-player 一致，重采样对齐输出参数）；
//! 将来换纯 Rust 解码不影响本模块接口。

use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use mwine_scrcpy::{
    AudioCodecId, AudioPacket, AudioStream as ScrcpyAudioStream, Result as ScrcpyResult,
};

use crate::{AudioConfig, AudioStream, Error, Result};

/// 音频包源。生产实现是 scrcpy socket；抽象出来便于离线注入 mock 测试解码管线。
pub trait PacketSource: Send {
    fn next_packet(&mut self) -> ScrcpyResult<Option<AudioPacket>>;
    fn codec(&self) -> AudioCodecId;
}

impl PacketSource for ScrcpyAudioStream {
    fn next_packet(&mut self) -> ScrcpyResult<Option<AudioPacket>> {
        ScrcpyAudioStream::next_packet(self)
    }

    fn codec(&self) -> AudioCodecId {
        ScrcpyAudioStream::codec(self)
    }
}

/// 解码线程组句柄。
pub struct DecoderHandle {
    feed: Option<std::thread::JoinHandle<()>>,
    drain: Option<std::thread::JoinHandle<()>>,
    ffmpeg: Arc<Mutex<Option<Child>>>,
    stopping: Arc<AtomicBool>,
}

impl DecoderHandle {
    /// 停止解码（幂等）。杀掉 ffmpeg 并等待读线程退出；
    /// feed 线程可能阻塞在 scrcpy socket 读上——不 join，随会话结束（EOF）自然退出，滞留无害。
    pub fn stop(&mut self) {
        self.stopping.store(true, Ordering::Relaxed);
        if let Some(mut child) = self.ffmpeg.lock().unwrap().take() {
            let _ = child.kill();
            let _ = child.wait();
        }
        if let Some(drain) = self.drain.take() {
            let _ = drain.join();
        }
        drop(self.feed.take());
    }
}

/// 启动解码线程组：source（flac 包）→ ffmpeg（s16le，重采样对齐 out）→ target.emit。
/// 编码非 flac / 缺 ffmpeg 时返回 Err（不起任何线程）。
pub fn spawn_decoder(
    source: Box<dyn PacketSource>,
    target: AudioStream,
    out: AudioConfig,
) -> Result<DecoderHandle> {
    let codec = source.codec();
    if codec != AudioCodecId::Flac {
        return Err(Error::UnsupportedCodec(codec.name().to_string()));
    }
    let ffmpeg = resolve_ffmpeg().ok_or_else(|| {
        Error::FfmpegNotFound("PATH 与常见安装位置均未找到 ffmpeg".into())
    })?;

    let mut child = Command::new(&ffmpeg)
        .args([
            "-hide_banner", "-loglevel", "error",
            "-f", "flac", "-i", "pipe:0",
            "-f", "s16le",
        ])
        .arg("-ar")
        .arg(out.sample_rate.to_string())
        .arg("-ac")
        .arg(out.channels.to_string())
        .arg("pipe:1")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| Error::Decode(format!("ffmpeg 启动失败: {e}")))?;
    let mut stdin = child.stdin.take().expect("piped stdin");
    let stdout = child.stdout.take().expect("piped stdout");
    let child = Arc::new(Mutex::new(Some(child)));
    let stopping = Arc::new(AtomicBool::new(false));

    // feed 线程：flac 包 → ffmpeg stdin（config 包补全文件头）
    let feed_child = child.clone();
    let feed_stop = stopping.clone();
    let mut source = source;
    let feed = match std::thread::Builder::new()
        .name("mwine-audio-feed".into())
        .spawn(move || {
            while !feed_stop.load(Ordering::Relaxed) {
                let packet = match source.next_packet() {
                    Ok(p) => p,
                    Err(_) => break,
                };
                let Some(packet) = packet else { break }; // 流结束（会话停止）
                let bytes = if packet.config {
                    ensure_flac_header(packet.data)
                } else {
                    packet.data
                };
                if stdin.write_all(&bytes).is_err() || stdin.flush().is_err() {
                    break; // 解码端已退出
                }
            }
            drop(stdin); // EOF → ffmpeg 收尾
            if let Some(mut c) = feed_child.lock().unwrap().take() {
                let _ = c.wait(); // 收尸（stop() 已杀掉时此处为 None）
            }
        }) {
        Ok(t) => t,
        Err(e) => {
            if let Some(mut c) = child.lock().unwrap().take() {
                let _ = c.kill();
                let _ = c.wait();
            }
            return Err(Error::Decode(format!("feed 线程启动失败: {e}")));
        }
    };

    // 解码线程：ffmpeg stdout（s16le）→ target 回调
    let drain = match std::thread::Builder::new()
        .name("mwine-audio-decode".into())
        .spawn(move || {
            let mut stdout = stdout;
            let mut chunk = [0u8; 16384];
            loop {
                match stdout.read(&mut chunk) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => {
                        let samples: Vec<i16> = chunk[..n]
                            .as_chunks::<2>()
                            .0
                            .iter()
                            .map(|b| i16::from_le_bytes(*b))
                            .collect();
                        target.emit(&samples);
                    }
                }
            }
        }) {
        Ok(t) => t,
        Err(e) => {
            stopping.store(true, Ordering::Relaxed);
            if let Some(mut c) = child.lock().unwrap().take() {
                let _ = c.kill();
                let _ = c.wait();
            }
            let _ = feed.join();
            return Err(Error::Decode(format!("解码线程启动失败: {e}")));
        }
    };

    Ok(DecoderHandle {
        feed: Some(feed),
        drain: Some(drain),
        ffmpeg: child,
        stopping,
    })
}

/// server 的 FLAC config 包应为 "fLaC" + 块头 + STREAMINFO；
/// 兼容只发 STREAMINFO 的实现（补上文件头前缀）。
fn ensure_flac_header(mut data: Vec<u8>) -> Vec<u8> {
    if data.starts_with(b"fLaC") {
        return data;
    }
    let mut out = b"fLaC".to_vec();
    // last-metadata-block=1, type=STREAMINFO(0) → 0x80；块头共 4 字节（1B 标志 + 3B 长度），长度 34 = 0x000022
    out.extend_from_slice(&[0x80, 0x00, 0x00, 0x22]);
    if data.len() < 34 {
        // 尺寸不足时补零（采样率等由 ffmpeg 从首帧兜底）
        data.resize(34, 0);
    }
    out.extend_from_slice(&data);
    out
}

/// 查找 ffmpeg 可执行文件（PATH 优先，其次常见 macOS 安装位置）。
/// 与 mwine-player decoder 同策略，后续统一收敛到此处。
pub fn resolve_ffmpeg() -> Option<PathBuf> {
    if Command::new("ffmpeg")
        .arg("-version")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok()
    {
        return Some("ffmpeg".into());
    }
    [
        "/opt/homebrew/bin/ffmpeg",
        "/usr/local/bin/ffmpeg",
        "/opt/local/bin/ffmpeg",
    ]
    .iter()
    .map(PathBuf::from)
    .find(|p| p.is_file())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    struct FakeSource {
        packets: VecDeque<AudioPacket>,
    }

    impl PacketSource for FakeSource {
        fn next_packet(&mut self) -> ScrcpyResult<Option<AudioPacket>> {
            Ok(self.packets.pop_front())
        }

        fn codec(&self) -> AudioCodecId {
            AudioCodecId::Flac
        }
    }

    /// 解析 FLAC 文件：返回 (裸 STREAMINFO, 全部元数据块之后的音频帧)。
    /// ffmpeg 生成的文件除 STREAMINFO 外还可能带 VORBIS_COMMENT/PADDING 块，
    /// 不能按固定偏移切——按块头（1bit last + 7bit type + 24bit 长度）走。
    fn split_flac(flac: &[u8]) -> (Vec<u8>, Vec<u8>) {
        assert_eq!(&flac[..4], b"fLaC");
        let mut pos = 4;
        let mut streaminfo = None;
        loop {
            let header = &flac[pos..pos + 4];
            let last = header[0] & 0x80 != 0;
            let block_type = header[0] & 0x7f;
            let len = u32::from_be_bytes([0, header[1], header[2], header[3]]) as usize;
            if block_type == 0 {
                assert_eq!(len, 34, "STREAMINFO 固定 34 字节");
                streaminfo = Some(flac[pos + 4..pos + 4 + len].to_vec());
            }
            pos += 4 + len;
            if last {
                break;
            }
        }
        (streaminfo.expect("FLAC 必含 STREAMINFO"), flac[pos..].to_vec())
    }

    #[test]
    fn flac_header_passthrough_and_pad() {
        let full = b"fLaC\x80\x00\x22rest-of-file".to_vec();
        assert_eq!(ensure_flac_header(full.clone()), full);

        let bare = vec![7u8; 10]; // 只发 STREAMINFO（且尺寸不足）
        let padded = ensure_flac_header(bare);
        assert_eq!(&padded[..4], b"fLaC");
        assert_eq!(&padded[4..8], &[0x80, 0x00, 0x00, 0x22], "块头 4 字节：标志+24 位长度");
        assert_eq!(padded.len(), 4 + 4 + 34);
        assert_eq!(padded[8], 7, "原数据保留在 STREAMINFO 位");
    }

    #[test]
    fn reject_non_flac_codec() {
        struct OpusSource;
        impl PacketSource for OpusSource {
            fn next_packet(&mut self) -> ScrcpyResult<Option<AudioPacket>> {
                Ok(None)
            }
            fn codec(&self) -> AudioCodecId {
                AudioCodecId::Opus
            }
        }
        let target = AudioStream::new(AudioConfig::stereo48k(), Box::new(|_: &[i16]| {}));
        match spawn_decoder(Box::new(OpusSource), target, AudioConfig::stereo48k()) {
            Err(Error::UnsupportedCodec(name)) => assert_eq!(name, "opus"),
            Err(other) => panic!("应拒绝非 flac 编码，实际错误 {other}"),
            Ok(_) => panic!("应拒绝非 flac 编码，却返回了解码句柄"),
        }
    }

    /// 离线端到端：ffmpeg 生成 0.2s 正弦 FLAC → 按 scrcpy 包语义（config+帧）
    /// 喂给解码线程组 → 断言回调收到足量、有幅度的 s16 样本。
    #[test]
    fn decode_flac_roundtrip() {
        let Some(ffmpeg) = resolve_ffmpeg() else {
            eprintln!("[mwine] 无 ffmpeg，跳过解码回环测试");
            return;
        };
        let out = Command::new(&ffmpeg)
            .args([
                "-hide_banner", "-loglevel", "error",
                "-f", "lavfi",
                "-i", "aevalsrc=sin(2*PI*440*t):d=0.2:s=48000",
                "-f", "flac", "pipe:1",
            ])
            .stdin(Stdio::null())
            .output()
            .expect("ffmpeg 生成 flac 失败");
        assert!(out.status.success(), "ffmpeg 应正常退出");
        let flac = &out.stdout[..];
        assert!(flac.starts_with(b"fLaC"), "应是 FLAC 文件");
        let (streaminfo, frames) = split_flac(flac);

        let source = FakeSource {
            packets: VecDeque::from(vec![
                AudioPacket {
                    pts: 0,
                    config: true,
                    data: streaminfo, // 裸 STREAMINFO（server 的 config 包形态）
                },
                AudioPacket {
                    pts: 1,
                    config: false,
                    data: frames,
                },
            ]),
        };

        let received = Arc::new(Mutex::new(Vec::new()));
        let r = received.clone();
        let target = AudioStream::new(AudioConfig::stereo48k(), Box::new(move |pcm: &[i16]| {
            r.lock().unwrap().extend_from_slice(pcm)
        }));

        let mut handle =
            spawn_decoder(Box::new(source), target.clone(), AudioConfig::stereo48k()).unwrap();
        // 0.2s @48k 立体声 ≈ 19200 样本
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while target.stats().samples < 10000 && std::time::Instant::now() < deadline {
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
        handle.stop();

        let got = received.lock().unwrap();
        let peak = got.iter().map(|s| s.abs()).max().unwrap_or(0);
        assert!(
            got.len() > 10000,
            "应解码出足量样本，实际 {}",
            got.len()
        );
        assert!(peak > 8000, "正弦应有显著幅度，峰值 {peak}");
    }
}
