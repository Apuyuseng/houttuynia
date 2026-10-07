//! MockAudioBridge（测试用 mock 实现）：
//! 无真实设备、无 scrcpy、无 ffmpeg，供 mwine-core / UI 层开发与联调。

use std::sync::{Arc, Mutex};

use mwine_audio::{AudioBridge, AudioConfig, AudioStream, Error, Result};

/// mock 音频路由：start 返回手动 emit 驱动的 AudioStream，行为可断言。
#[derive(Default)]
pub struct MockAudioBridge {
    running: bool,
    last_config: Option<AudioConfig>,
    stream: Option<AudioStream>,
}

impl MockAudioBridge {
    pub fn new() -> Self {
        Self::default()
    }

    /// 最近一次 start 的配置（断言调用方传参用）
    pub fn last_config(&self) -> Option<AudioConfig> {
        self.last_config
    }

    /// 模拟 guest 音频（测试驱动侧调用）
    pub fn emit_guest_pcm(&self, pcm: &[i16]) -> Result<()> {
        self.stream.as_ref().ok_or(Error::NotRunning)?.emit(pcm);
        Ok(())
    }
}

impl AudioBridge for MockAudioBridge {
    fn start(&mut self, config: AudioConfig) -> Result<AudioStream> {
        if self.running {
            return Err(Error::AlreadyRunning);
        }
        let stream = AudioStream::new(config, Box::new(|_: &[i16]| {}));
        self.running = true;
        self.last_config = Some(config);
        self.stream = Some(stream.clone());
        Ok(stream)
    }

    fn stop(&mut self) -> Result<()> {
        self.running = false;
        self.stream = None;
        Ok(())
    }

    fn is_running(&self) -> bool {
        self.running
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lifecycle_start_stop_idempotent() {
        let mut bridge = MockAudioBridge::new();
        assert!(!bridge.is_running());

        let stream = bridge.start(AudioConfig::stereo48k()).unwrap();
        assert!(bridge.is_running());
        assert_eq!(bridge.last_config(), Some(AudioConfig::stereo48k()));
        assert_eq!(stream.config(), AudioConfig::stereo48k());

        assert!(matches!(
            bridge.start(AudioConfig::default()),
            Err(Error::AlreadyRunning)
        ));
        bridge.stop().unwrap();
        bridge.stop().unwrap(); // 幂等
        assert!(!bridge.is_running());
    }

    #[test]
    fn emitted_pcm_reaches_installed_callback() {
        let mut bridge = MockAudioBridge::new();
        let stream = bridge.start(AudioConfig::stereo48k()).unwrap();

        let seen = Arc::new(Mutex::new(Vec::new()));
        let s = seen.clone();
        let old = stream.set_callback(Box::new(move |pcm: &[i16]| {
            s.lock().unwrap().extend_from_slice(pcm)
        }));
        assert!(old.is_some(), "应能换掉默认回调");

        bridge.emit_guest_pcm(&[1, 2, 3]).unwrap();
        assert_eq!(*seen.lock().unwrap(), vec![1, 2, 3]);

        bridge.stop().unwrap();
        assert!(matches!(
            bridge.emit_guest_pcm(&[4]),
            Err(Error::NotRunning)
        ));
    }
}
