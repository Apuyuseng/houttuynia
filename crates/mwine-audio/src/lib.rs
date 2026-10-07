//! mwine-audio：guest 音频 → macOS 输出路由。
//!
//! 候选路径：
//! 1. qemu `-audiodev coreaudio` 直出——零代码但路由/音量不可控（未采用）
//! 2. qemu `-audiodev none` + scrcpy 音频流——可编程，本 crate 的实现路径
//! 3. qemu gRPC audio——待后续调研，接口存在时另加 AudioBridge 实现
//!
//! 接口先行：mwine-core 只面向 AudioBridge trait 组装；
//! 具体实现（CoreAudioBridge）在本 crate 内，接线时才被引用。
//!
//! 注意区分两个同名概念：mwine_scrcpy::AudioStream 是压缩包 socket（flac 等），
//! 本 crate 的 AudioStream 是解码后的交错 s16 PCM 回调流。

pub mod coreaudio;
pub mod scrcpy;

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

pub use coreaudio::{CoreAudioBridge, CoreAudioOutput, QueueStats};
pub use scrcpy::{DecoderHandle, PacketSource};

/// 音频路由错误。
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("无音频输出设备: {0}")]
    NoOutputDevice(String),
    #[error("音频输出流打开失败: {0}")]
    OpenFailed(String),
    #[error("音频路由已在运行")]
    AlreadyRunning,
    #[error("音频路由未启动")]
    NotRunning,
    #[error("ffmpeg 未找到（brew install ffmpeg）: {0}")]
    FfmpegNotFound(String),
    #[error("不支持的音频编码: {0}（当前仅 flac）")]
    UnsupportedCodec(String),
    #[error("解码失败: {0}")]
    Decode(String),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

/// PCM 回调：一段交错 s16 样本（按实际生效配置的采样率/声道数）。
pub type PcmCallback = Box<dyn FnMut(&[i16]) + Send>;

/// 音频参数。`start` 传入的是期望值，实际以设备为准
///（经 `AudioStream::config` 返回），源端应重采样对齐。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AudioConfig {
    /// 采样率（Hz）
    pub sample_rate: u32,
    /// 交错声道数
    pub channels: u16,
    /// 输出缓冲上限（帧）。0 = 自动（0.5s）；积压超限即丢弃，限制延迟
    pub buffer_size: u32,
}

impl Default for AudioConfig {
    fn default() -> Self {
        Self {
            sample_rate: 48000,
            channels: 2,
            buffer_size: 0,
        }
    }
}

impl AudioConfig {
    /// 常用默认：立体声 48kHz，缓冲自动
    pub const fn stereo48k() -> Self {
        Self {
            sample_rate: 48000,
            channels: 2,
            buffer_size: 0,
        }
    }
}

struct StreamInner {
    config: AudioConfig,
    callback: Mutex<Option<PcmCallback>>,
    emits: AtomicU64,
    samples: AtomicU64,
}

/// 运行中的音频流：guest 侧解出的每段 PCM 都会送达回调
///（默认回调为 CoreAudio 输出队列，可替换以实现录制/电平表）。
///
/// 桥接实现的解码线程持有 clone 调用 [`AudioStream::emit`]；
/// 调用方持有的 clone 用于换回调/查统计。clone 是共享句柄。
#[derive(Clone)]
pub struct AudioStream {
    inner: Arc<StreamInner>,
}

impl AudioStream {
    /// 手动构造流（mock/自定义路由用；生产路径走 [`AudioBridge::start`]）。
    pub fn new(config: AudioConfig, callback: PcmCallback) -> Self {
        Self {
            inner: Arc::new(StreamInner {
                config,
                callback: Mutex::new(Some(callback)),
                emits: AtomicU64::new(0),
                samples: AtomicU64::new(0),
            }),
        }
    }

    /// 实际生效参数
    pub fn config(&self) -> AudioConfig {
        self.inner.config
    }

    /// 送一段交错 s16 PCM 给回调（由解码线程驱动；测试/手动路由可直接调用）。
    pub fn emit(&self, pcm: &[i16]) {
        if pcm.is_empty() {
            return;
        }
        self.inner.emits.fetch_add(1, Ordering::Relaxed);
        self.inner
            .samples
            .fetch_add(pcm.len() as u64, Ordering::Relaxed);
        if let Some(cb) = self.inner.callback.lock().unwrap().as_mut() {
            cb(pcm);
        }
    }

    /// 换回调并返回旧回调（包装旧回调可实现 tap：既播放又录制）。
    pub fn set_callback(&self, cb: PcmCallback) -> Option<PcmCallback> {
        self.inner.callback.lock().unwrap().replace(cb)
    }

    pub fn stats(&self) -> StreamStats {
        StreamStats {
            emits: self.inner.emits.load(Ordering::Relaxed),
            samples: self.inner.samples.load(Ordering::Relaxed),
        }
    }
}

/// [`AudioStream`] 的累计统计
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StreamStats {
    /// emit 调用次数
    pub emits: u64,
    /// 累计样本数（含声道）
    pub samples: u64,
}

/// 音频路由：mwine-core 的组装面（仅依赖此 trait，不依赖具体实现）。
pub trait AudioBridge: Send {
    /// 打开路由。config 为期望值，实际设备参数以返回流的
    /// [`AudioStream::config`] 为准。
    fn start(&mut self, config: AudioConfig) -> Result<AudioStream>;
    /// 停止并释放资源（幂等）。
    fn stop(&mut self) -> Result<()>;
    fn is_running(&self) -> bool;
}

/// qemu 音频后端模式：mwine-qemu 组装命令行时取用
/// （后端选择是 M4 的决策，进程拼装属 M1）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QemuAudioMode {
    /// 路径 1：qemu 直出 host CoreAudio——零代码，但路由/音量不可控
    Native,
    /// 路径 2（默认）：qemu 侧静音，guest 音频经 scrcpy 音频流带出
    ViaScrcpy,
}

/// 组装 qemu `-audiodev` 参数。ViaScrcpy 必须静音 qemu 侧，
/// 否则同一音频既走 qemu CoreAudio 又走 scrcpy 捕获，双重出声。
/// 具体设备挂载方式（goldfish/virtio-sound）由 qemu 镜像决定，M2 集成时验证。
pub fn qemu_audio_args(mode: QemuAudioMode) -> Vec<String> {
    match mode {
        QemuAudioMode::Native => vec!["-audiodev".into(), "coreaudio,id=snd0".into()],
        QemuAudioMode::ViaScrcpy => vec!["-audiodev".into(), "none,id=snd0".into()],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_config_is_stereo_48k() {
        assert_eq!(AudioConfig::default(), AudioConfig::stereo48k());
        assert_eq!(AudioConfig::stereo48k().sample_rate, 48000);
        assert_eq!(AudioConfig::stereo48k().channels, 2);
    }

    #[test]
    fn emit_delivers_pcm_to_callback_in_order() {
        let received = std::sync::Arc::new(Mutex::new(Vec::new()));
        let r = received.clone();
        let stream = AudioStream::new(
            AudioConfig::stereo48k(),
            Box::new(move |pcm: &[i16]| r.lock().unwrap().extend_from_slice(pcm)),
        );
        stream.emit(&[1, -2, 3]);
        stream.emit(&[4]);
        assert_eq!(*received.lock().unwrap(), vec![1, -2, 3, 4]);
        assert_eq!(stream.stats().emits, 2);
        assert_eq!(stream.stats().samples, 4);
    }

    #[test]
    fn set_callback_swaps_and_returns_old() {
        let stream = AudioStream::new(AudioConfig::stereo48k(), Box::new(|_: &[i16]| {}));
        let old = stream.set_callback(Box::new(|pcm: &[i16]| {
            assert_eq!(pcm, &[7]);
        }));
        assert!(old.is_some(), "默认回调应被返回");
        stream.emit(&[7]);
    }

    #[test]
    fn qemu_args_match_mode() {
        assert_eq!(
            qemu_audio_args(QemuAudioMode::Native),
            vec!["-audiodev", "coreaudio,id=snd0"]
        );
        assert_eq!(
            qemu_audio_args(QemuAudioMode::ViaScrcpy),
            vec!["-audiodev", "none,id=snd0"]
        );
    }
}
