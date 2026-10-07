//! CoreAudio（经 cpal）输出引擎 + CoreAudioBridge（路径 2 的组装实现）。
//!
//! cpal 的 Stream 是 !Send，不能跨线程移动：本模块把它停泊在专属"owner 线程"，
//! play/pause/drop 经 channel 传递——启停/音量/静音全程可控，
//! 替代 mwine-player 音频桥的进程生命周期持有（mem::forget）方案。

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::thread::JoinHandle;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};

use crate::scrcpy::{self, PacketSource};
use crate::{AudioBridge, AudioConfig, AudioStream, Error, Result};

/// 交错 f32 样本队列：生产者 push，cpal 回调拉取。
/// 延迟策略与 mwine-player 实测一致：超上限丢积压（限制延迟），
/// 起播前攒阈值（避免起步欠载爆音）。
pub(crate) struct PcmQueue {
    queue: VecDeque<f32>,
    channels: usize,
    /// 缓冲上限（帧）
    cap_frames: usize,
    /// 起播阈值（帧）
    pre_frames: usize,
    primed: bool,
    pushed_frames: u64,
    dropped_frames: u64,
}

impl PcmQueue {
    pub(crate) fn new(channels: usize, cap_frames: usize, pre_frames: usize) -> Self {
        Self {
            queue: VecDeque::new(),
            channels: channels.max(1),
            cap_frames: cap_frames.max(1),
            pre_frames: pre_frames.max(1),
            primed: false,
            pushed_frames: 0,
            dropped_frames: 0,
        }
    }

    /// 压入交错 s16（调用方保证声道数与队列一致；ffmpeg 侧已重采样对齐）。
    /// 压入前若积压已达上限，整段丢弃（不清新数据，保持延迟有界）。
    pub(crate) fn push_i16(&mut self, pcm: &[i16]) {
        if self.queue.len() >= self.cap_frames * self.channels {
            self.dropped_frames += (self.queue.len() / self.channels) as u64;
            self.queue.clear();
        }
        self.pushed_frames += (pcm.len() / self.channels) as u64;
        self.queue.extend(pcm.iter().map(|&s| s as f32 / 32768.0));
    }

    /// 回调拉取：填满 `out`（不足输出静音）并乘增益。
    /// `discard = true`（暂停语义）：照常消费但输出静音，防止积压失控。
    pub(crate) fn pull(&mut self, out: &mut [f32], gain: f32, discard: bool) {
        if discard {
            let n = out.len().min(self.queue.len());
            for _ in 0..n {
                self.queue.pop_front();
            }
            out.fill(0.0);
            return;
        }
        let ch = self.channels;
        let want_frames = out.len() / ch;
        if !self.primed {
            let buffered = self.queue.len() / ch;
            if buffered < self.pre_frames.max(want_frames) {
                out.fill(0.0);
                return;
            }
            self.primed = true;
        }
        for slot in out.iter_mut() {
            *slot = self.queue.pop_front().unwrap_or(0.0) * gain;
        }
    }

    pub(crate) fn stats(&self) -> QueueStats {
        QueueStats {
            pushed_frames: self.pushed_frames,
            dropped_frames: self.dropped_frames,
            buffered_frames: self.queue.len() / self.channels,
            primed: self.primed,
        }
    }
}

/// 输出队列统计（延迟观测）
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct QueueStats {
    /// 累计压入帧数
    pub pushed_frames: u64,
    /// 因超上限丢弃的帧数（延迟控制）
    pub dropped_frames: u64,
    /// 当前缓冲帧数
    pub buffered_frames: usize,
    /// 是否已过起播阈值
    pub primed: bool,
}

enum Cmd {
    Play,
    Pause,
    Drop,
}

struct OutputShared {
    queue: Mutex<PcmQueue>,
    volume: Mutex<f32>,
    muted: AtomicBool,
    paused: AtomicBool,
}

impl OutputShared {
    /// 回调侧统一取增益：静音输出 0，暂停由 discard 语义处理
    fn gain(&self) -> f32 {
        if self.muted.load(Ordering::Relaxed) {
            0.0
        } else {
            *self.volume.lock().unwrap()
        }
    }
}

struct OutputInner {
    shared: Arc<OutputShared>,
    cmd: Mutex<Option<mpsc::Sender<Cmd>>>,
    owner: Mutex<Option<JoinHandle<()>>>,
    actual: AudioConfig,
}

/// CoreAudio 输出句柄，浅克隆（共享队列与控制通道）。
/// stop 后所有 clone 一同失效。
#[derive(Clone)]
pub struct CoreAudioOutput {
    inner: Arc<OutputInner>,
}

impl CoreAudioOutput {
    /// 打开默认输出设备。返回句柄与实际生效参数（源端按此重采样对齐）。
    /// 请求的采样率/声道不被支持时回退设备默认配置。
    pub fn open(requested: &AudioConfig) -> Result<(Self, AudioConfig)> {
        let host = cpal::default_host();
        let device = host
            .default_output_device()
            .ok_or_else(|| Error::NoOutputDevice("未找到默认输出设备".into()))?;
        let default_cfg = device
            .default_output_config()
            .map_err(|e| Error::OpenFailed(e.to_string()))?;

        // 优先取与请求采样率/声道匹配的配置，否则回退设备默认
        // （supported_output_configs 返回迭代器；列举失败同样回退默认）
        let chosen = device
            .supported_output_configs()
            .ok()
            .and_then(|mut ranges| {
                ranges
                    .find(|r| {
                        r.channels() == requested.channels
                            && r.min_sample_rate().0 <= requested.sample_rate
                            && requested.sample_rate <= r.max_sample_rate().0
                    })
                    .map(|r| r.with_sample_rate(cpal::SampleRate(requested.sample_rate)))
            })
            .unwrap_or_else(|| default_cfg.clone());

        let channels = chosen.channels();
        let rate = chosen.sample_rate().0;
        let format = chosen.sample_format();
        let stream_cfg: cpal::StreamConfig = chosen.into();

        let cap_frames = if requested.buffer_size > 0 {
            requested.buffer_size as usize
        } else {
            (rate as f32 * 0.5) as usize // 0.5s 上限
        };
        let pre_frames = (cap_frames / 5).max(1); // 0.1s 起播阈值

        let shared = Arc::new(OutputShared {
            queue: Mutex::new(PcmQueue::new(channels as usize, cap_frames, pre_frames)),
            volume: Mutex::new(1.0),
            muted: AtomicBool::new(false),
            paused: AtomicBool::new(false),
        });

        let (cmd_tx, cmd_rx) = mpsc::channel::<Cmd>();
        // 打开结果经 ready 回传：owner 线程内才能感知 build/play 失败
        let (ready_tx, ready_rx) = mpsc::channel::<Result<()>>();
        let owner_shared = shared.clone();
        let owner = std::thread::Builder::new()
            .name("mwine-audio-out".into())
            .spawn(move || {
                let s = owner_shared;
                let built = build_stream(&device, &stream_cfg, format, s).and_then(|st| {
                    st.play()
                        .map(|_| st)
                        .map_err(|e| Error::OpenFailed(format!("play 失败: {e}")))
                });
                let stream = match built {
                    Ok(st) => {
                        let _ = ready_tx.send(Ok(()));
                        st
                    }
                    Err(e) => {
                        let _ = ready_tx.send(Err(e));
                        return;
                    }
                };
                // 停泊在此：命令驱动 play/pause，Drop 时 stream 随线程退出而释放
                for cmd in cmd_rx {
                    match cmd {
                        Cmd::Play => {
                            let _ = stream.play();
                        }
                        Cmd::Pause => {
                            let _ = stream.pause();
                        }
                        Cmd::Drop => break,
                    }
                }
            })
            .map_err(|e| Error::OpenFailed(format!("输出线程启动失败: {e}")))?;
        ready_rx
            .recv()
            .map_err(|_| Error::OpenFailed("输出线程异常退出".into()))??;

        let actual = AudioConfig {
            sample_rate: rate,
            channels,
            buffer_size: cap_frames as u32,
        };
        Ok((
            Self {
                inner: Arc::new(OutputInner {
                    shared,
                    cmd: Mutex::new(Some(cmd_tx)),
                    owner: Mutex::new(Some(owner)),
                    actual,
                }),
            },
            actual,
        ))
    }

    /// 实际生效参数
    pub fn actual_config(&self) -> AudioConfig {
        self.inner.actual
    }

    /// 压入交错 s16 PCM（按 actual 参数；上游已重采样，此处不做声道转换）
    pub fn push_pcm(&self, pcm: &[i16]) {
        self.inner.shared.queue.lock().unwrap().push_i16(pcm);
    }

    /// 软件音量（0.0..=1.0，回调内乘增益）
    pub fn set_volume(&self, volume: f32) {
        *self.inner.shared.volume.lock().unwrap() = volume.clamp(0.0, 1.0);
    }

    pub fn volume(&self) -> f32 {
        *self.inner.shared.volume.lock().unwrap()
    }

    pub fn set_muted(&self, muted: bool) {
        self.inner.shared.muted.store(muted, Ordering::Relaxed);
    }

    pub fn muted(&self) -> bool {
        self.inner.shared.muted.load(Ordering::Relaxed)
    }

    /// 暂停输出：设备暂停 + 回调侧消费丢弃，双保险防积压
    pub fn pause(&self) -> Result<()> {
        self.inner.shared.paused.store(true, Ordering::Relaxed);
        self.send_cmd(Cmd::Pause)
    }

    pub fn resume(&self) -> Result<()> {
        self.inner.shared.paused.store(false, Ordering::Relaxed);
        self.send_cmd(Cmd::Play)
    }

    pub fn stats(&self) -> QueueStats {
        self.inner.shared.queue.lock().unwrap().stats()
    }

    /// 停止输出线程并释放设备（幂等；所有 clone 一同失效）
    pub fn close(&self) {
        let tx = self.inner.cmd.lock().unwrap().take();
        if let Some(tx) = tx {
            let _ = tx.send(Cmd::Drop);
        }
        if let Some(owner) = self.inner.owner.lock().unwrap().take() {
            let _ = owner.join();
        }
    }

    fn send_cmd(&self, cmd: Cmd) -> Result<()> {
        let guard = self.inner.cmd.lock().unwrap();
        match guard.as_ref() {
            Some(tx) => tx.send(cmd).map_err(|_| Error::NotRunning),
            None => Err(Error::NotRunning),
        }
    }
}

fn build_stream(
    device: &cpal::Device,
    cfg: &cpal::StreamConfig,
    format: cpal::SampleFormat,
    shared: Arc<OutputShared>,
) -> Result<cpal::Stream> {
    let err_fn = |e| eprintln!("[mwine] 音频输出错误: {e}");
    match format {
        cpal::SampleFormat::F32 => device
            .build_output_stream(
                cfg,
                move |data: &mut [f32], _| {
                    let s = shared.clone();
                    let gain = s.gain();
                    let discard = s.paused.load(Ordering::Relaxed);
                    let mut q = s.queue.lock().unwrap();
                    q.pull(data, gain, discard);
                },
                err_fn,
                None,
            )
            .map_err(|e| Error::OpenFailed(e.to_string())),
        cpal::SampleFormat::I16 => device
            .build_output_stream(
                cfg,
                move |data: &mut [i16], _| {
                    let s = shared.clone();
                    let gain = s.gain();
                    let discard = s.paused.load(Ordering::Relaxed);
                    let mut q = s.queue.lock().unwrap();
                    let mut buf = [0f32; 256];
                    for chunk in data.chunks_mut(256) {
                        q.pull(&mut buf[..chunk.len()], gain, discard);
                        for (o, f) in chunk.iter_mut().zip(buf.iter()) {
                            *o = (*f * 32767.0).clamp(i16::MIN as f32, i16::MAX as f32) as i16;
                        }
                    }
                },
                err_fn,
                None,
            )
            .map_err(|e| Error::OpenFailed(e.to_string())),
        other => Err(Error::OpenFailed(format!(
            "设备样本格式暂不支持: {other:?}"
        ))),
    }
}

struct Route {
    output: CoreAudioOutput,
    decoder: Option<scrcpy::DecoderHandle>,
}

/// 路径 2 组装：scrcpy 音频源（guest）→ flac 解码 → CoreAudio 输出。
///
/// 用法：scrcpy 会话 `split()` 拆出音频 socket 后经 [`CoreAudioBridge::with_scrcpy`]
/// 注入；qemu 应以 `qemu_audio_args(QemuAudioMode::ViaScrcpy)`（`-audiodev none`）
/// 启动，避免 qemu CoreAudio 与 scrcpy 捕获双重出声。
pub struct CoreAudioBridge {
    source: Option<Box<dyn PacketSource>>,
    route: Option<Route>,
}

impl CoreAudioBridge {
    pub fn new() -> Self {
        Self {
            source: None,
            route: None,
        }
    }

    /// 注入 scrcpy 音频源；`start` 后开始解码路由
    pub fn with_scrcpy(audio: mwine_scrcpy::AudioStream) -> Self {
        Self {
            source: Some(Box::new(audio)),
            route: None,
        }
    }

    /// 当前路由的输出统计（延迟观测）
    pub fn stats(&self) -> Result<QueueStats> {
        Ok(self.output()?.stats())
    }

    pub fn set_volume(&self, volume: f32) -> Result<()> {
        self.output()?.set_volume(volume);
        Ok(())
    }

    pub fn set_muted(&self, muted: bool) -> Result<()> {
        self.output()?.set_muted(muted);
        Ok(())
    }

    pub fn pause(&self) -> Result<()> {
        self.output()?.pause()
    }

    pub fn resume(&self) -> Result<()> {
        self.output()?.resume()
    }

    fn output(&self) -> Result<&CoreAudioOutput> {
        self.route.as_ref().map(|r| &r.output).ok_or(Error::NotRunning)
    }
}

impl Default for CoreAudioBridge {
    fn default() -> Self {
        Self::new()
    }
}

impl AudioBridge for CoreAudioBridge {
    fn start(&mut self, config: AudioConfig) -> Result<AudioStream> {
        if self.route.is_some() {
            return Err(Error::AlreadyRunning);
        }
        let (output, actual) = CoreAudioOutput::open(&config)?;
        // 默认回调：PCM 直入输出队列
        let sink = output.clone();
        let stream = AudioStream::new(actual, Box::new(move |pcm| sink.push_pcm(pcm)));
        let decoder = match self.source.take() {
            Some(source) => match scrcpy::spawn_decoder(source, stream.clone(), actual) {
                Ok(d) => Some(d),
                Err(e) => {
                    output.close(); // 解码线程组起不来时释放已开设备
                    return Err(e);
                }
            },
            None => None, // 纯输出路由：emit 手动驱动（测试/未来 gRPC 路径）
        };
        self.route = Some(Route { output, decoder });
        Ok(stream)
    }

    fn stop(&mut self) -> Result<()> {
        if let Some(mut route) = self.route.take() {
            if let Some(mut d) = route.decoder.take() {
                d.stop();
            }
            route.output.close();
        }
        Ok(())
    }

    fn is_running(&self) -> bool {
        self.route.is_some()
    }
}

impl Drop for CoreAudioBridge {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn queue_primes_then_pops_in_order() {
        let mut q = PcmQueue::new(2, 24000, 4); // 起播阈值 4 帧
        let mut out = [0f32; 8]; // 4 帧
        q.pull(&mut out, 1.0, false);
        assert!(out.iter().all(|&s| s == 0.0), "起播阈值未到应静音");

        q.push_i16(&[16384, -16384, 16384, -16384]); // 2 帧（0.5 幅度）
        q.pull(&mut out, 1.0, false);
        assert!(out.iter().all(|&s| s == 0.0), "仍低于阈值");

        q.push_i16(&[32767; 12]); // +6 帧 = 8 帧 ≥ 阈值
        let mut got = [1f32; 8];
        q.pull(&mut got, 1.0, false);
        assert_eq!(&got[..4], &[0.5, -0.5, 0.5, -0.5]);
        assert!(got[4..].iter().all(|&s| (s - 1.0).abs() < 1e-3));

        // 剩余 4 帧取尽后，再拉输出静音且不 panic
        let mut next = [1f32; 8];
        q.pull(&mut next, 1.0, false);
        assert!(next.iter().all(|&s| (s - 1.0).abs() < 1e-3));
        let mut empty = [1f32; 4];
        q.pull(&mut empty, 1.0, false);
        assert!(empty.iter().all(|&s| s == 0.0));
    }

    #[test]
    fn queue_drops_backlog_beyond_cap() {
        let mut q = PcmQueue::new(2, 100, 1); // 上限 100 帧
        q.push_i16(&[0i16; 200]); // 100 帧
        q.push_i16(&[0i16; 200]); // 再压入：触发整段丢弃
        let stats = q.stats();
        assert!(stats.dropped_frames >= 100);
        assert!(stats.buffered_frames <= 100);
    }

    #[test]
    fn queue_discard_consumes_silently() {
        let mut q = PcmQueue::new(1, 1000, 1);
        q.push_i16(&[1000; 100]);
        let mut out = [1f32; 50];
        q.pull(&mut out, 1.0, true); // 暂停语义
        assert!(out.iter().all(|&s| s == 0.0));
        assert_eq!(q.stats().buffered_frames, 50, "暂停期间照常消费");
    }

    #[test]
    fn output_open_push_close_roundtrip() {
        // 无输出设备的环境（CI）跳过
        if cpal::default_host().default_output_device().is_none() {
            eprintln!("[mwine] 无音频设备，跳过输出引擎测试");
            return;
        }
        let (out, actual) = CoreAudioOutput::open(&AudioConfig::stereo48k()).unwrap();
        assert!(actual.sample_rate > 0);
        assert!(actual.channels >= 1);
        assert!(actual.buffer_size > 0);

        out.set_volume(2.0);
        assert_eq!(out.volume(), 1.0, "音量应钳制到 1.0");
        out.set_volume(0.5);
        out.set_muted(true);
        assert!(out.muted());
        out.push_pcm(&[0i16; 512]);
        assert_eq!(out.stats().pushed_frames, 256);

        out.pause().unwrap();
        std::thread::sleep(std::time::Duration::from_millis(100));
        out.push_pcm(&[0i16; 512]);
        out.resume().unwrap();
        out.close();
        out.close(); // 幂等
        assert!(matches!(out.pause(), Err(Error::NotRunning)));
    }
}
