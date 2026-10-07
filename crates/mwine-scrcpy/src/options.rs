//! scrcpy 4.0 server 启动参数（key=value 形式，经 app_process 传入）。
//! 字段名与 server/src/main/java/.../Options.java 的解析逻辑一一对应。

use crate::error::{Error, Result};

pub const SERVER_VERSION: &str = "4.0";
pub const SERVER_CLASS: &str = "com.genymobile.scrcpy.Server";
pub const SERVER_REMOTE_PATH: &str = "/data/local/tmp/scrcpy-server";

/// 每会话独立的远端 jar 路径：并发会话共用同一路径会在推送时互相覆盖，
/// 后启动会话把先启动会话正在加载的字节码写坏（表现为会话超时/黑屏窗口）。
pub fn session_remote_path(session: u32) -> String {
    format!("{SERVER_REMOTE_PATH}.{session:x}")
}
pub const SOCKET_NAME: &str = "scrcpy";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VideoCodecId {
    H264,
    H265,
    Av1,
}

impl VideoCodecId {
    pub fn wire_id(self) -> u32 {
        match self {
            VideoCodecId::H264 => 0x6832_3634, // "h264"
            VideoCodecId::H265 => 0x6832_3635, // "h265"
            VideoCodecId::Av1 => 0x0061_7631,  // "av1"
        }
    }

    pub fn from_wire(id: u32) -> Result<Self> {
        match id {
            0x6832_3634 => Ok(VideoCodecId::H264),
            0x6832_3635 => Ok(VideoCodecId::H265),
            0x0061_7631 => Ok(VideoCodecId::Av1),
            other => Err(Error::Protocol(format!(
                "unknown video codec id: {other:#x}"
            ))),
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            VideoCodecId::H264 => "h264",
            VideoCodecId::H265 => "h265",
            VideoCodecId::Av1 => "av1",
        }
    }
}

/// 虚拟显示器尺寸：`(width, height, dpi)`。
/// `new_display` 参数支持 "WxH/DPI"、"WxH"、"/DPI"、"" 四种形式。
#[derive(Debug, Clone)]
pub struct NewDisplay {
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub dpi: Option<u32>,
}

impl NewDisplay {
    pub fn new(width: u32, height: u32, dpi: u32) -> Self {
        Self {
            width: Some(width),
            height: Some(height),
            dpi: Some(dpi),
        }
    }

    pub fn to_param(&self) -> String {
        let size = match (self.width, self.height) {
            (Some(w), Some(h)) => format!("{w}x{h}"),
            _ => String::new(),
        };
        match self.dpi {
            Some(dpi) => format!("{size}/{dpi}"),
            None => size,
        }
    }
}

#[derive(Debug, Clone)]
pub struct ServerOptions {
    pub log_level: LogLevel,
    pub video: bool,
    pub audio: bool,
    pub control: bool,
    pub video_codec: VideoCodecId,
    /// 音频编码：宿主侧裸流解码按 flac 设计（可直接喂 ffmpeg flac demuxer），
    /// opus 需 OGG 封装，暂不使用
    pub audio_codec: crate::audio::AudioCodecId,
    pub video_bit_rate: u32,
    pub max_size: u32,
    pub max_fps: Option<f32>,
    /// 会话 id（31 位非负值）。None 表示用固定的 "scrcpy" socket 名；
    /// Some(v) 时 socket 名为 "scrcpy_%08x"，多会话并存必备。
    pub scid: Option<u32>,
    /// 创建独立虚拟显示器（本项目"每应用一个窗口"的基石）
    pub new_display: Option<NewDisplay>,
    /// 4.0 flex display：虚拟显示器可动态调整尺寸
    pub flex_display: bool,
    /// 关闭虚拟显示器时销毁其内容
    pub vd_destroy_content: bool,
    /// 虚拟显示器启用系统装饰（状态栏等）
    pub vd_system_decorations: bool,
    pub stay_awake: bool,
    pub power_off_on_close: bool,
    /// 会话空闲时不退出（对应用窗口保持很重要）
    pub keep_active: bool,
    pub show_touches: bool,
}

impl Default for ServerOptions {
    fn default() -> Self {
        Self {
            log_level: LogLevel::Info,
            video: true,
            audio: false,
            control: true,
            video_codec: VideoCodecId::H264,
            audio_codec: crate::audio::AudioCodecId::Flac,
            video_bit_rate: 8_000_000,
            max_size: 0,
            max_fps: None,
            scid: None,
            new_display: None,
            flex_display: false,
            vd_destroy_content: true,
            vd_system_decorations: true,
            stay_awake: true,
            power_off_on_close: false,
            keep_active: false,
            show_touches: false,
        }
    }
}

impl ServerOptions {
    /// 本会话使用的 abstract socket 名（与 server 端 DesktopConnection.getSocketName 一致）。
    pub fn socket_name(&self) -> String {
        match self.scid {
            None => SOCKET_NAME.to_string(),
            Some(scid) => format!("{SOCKET_NAME}_{scid:08x}"),
        }
    }

    /// 生成一个随机 31 位 scid（时间 + pid + 地址熵混合）。
    pub fn random_scid() -> u32 {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos() as u64 ^ d.as_secs())
            .unwrap_or(0);
        let pid = std::process::id() as u64;
        let stack = &nanos as *const u64 as u64;
        let mixed =
            nanos.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ pid.rotate_left(32) ^ stack.rotate_left(17);
        (mixed & 0x7FFF_FFFF) as u32
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LogLevel {
    Verbose,
    Debug,
    Info,
    Warn,
    Error,
}

impl LogLevel {
    fn as_str(self) -> &'static str {
        match self {
            LogLevel::Verbose => "verbose",
            LogLevel::Debug => "debug",
            LogLevel::Info => "info",
            LogLevel::Warn => "warn",
            LogLevel::Error => "error",
        }
    }
}

impl ServerOptions {
    /// 序列化为 app_process 参数（不含类名与版本号）。
    pub fn to_args(&self) -> Vec<String> {
        let mut args: Vec<String> = Vec::new();
        let mut push = |k: &str, v: String| args.push(format!("{k}={v}"));
        if let Some(scid) = self.scid {
            // 与 socket_name() 保持一致：server 端按 scid 绑定 scrcpy_<scid>
            push("scid", format!("{scid:08x}"));
        }
        push("log_level", self.log_level.as_str().to_string());
        push("video", self.video.to_string());
        push("audio", self.audio.to_string());
        push("control", self.control.to_string());
        push("video_codec", self.video_codec.name().to_string());
        if self.audio {
            push("audio_codec", self.audio_codec.name().to_string());
        }
        push("video_bit_rate", self.video_bit_rate.to_string());
        push("max_size", self.max_size.to_string());
        if let Some(scid) = self.scid {
            // server 端按 16 进制解析（Options.parse: Integer.parseInt(value, 0x10)）
            push("scid", format!("{scid:x}"));
        }
        if let Some(fps) = self.max_fps {
            push("max_fps", format!("{fps}"));
        }
        if let Some(vd) = &self.new_display {
            push("new_display", vd.to_param());
            push("vd_destroy_content", self.vd_destroy_content.to_string());
            push(
                "vd_system_decorations",
                self.vd_system_decorations.to_string(),
            );
            if self.flex_display {
                push("flex_display", "true".to_string());
            }
        }
        push("stay_awake", self.stay_awake.to_string());
        push("power_off_on_close", self.power_off_on_close.to_string());
        push("keep_active", self.keep_active.to_string());
        push("show_touches", self.show_touches.to_string());
        // forward 模式：server 监听 abstract socket，宿主主动连
        push("tunnel_forward", "true".to_string());
        args
    }

    /// 校验参数组合合法性（与 server 端约束一致）。
    pub fn validate(&self) -> Result<()> {
        if !self.video && !self.audio {
            return Err(Error::InvalidOption("video 与 audio 不能同时关闭".into()));
        }
        if let (Some(w), Some(h)) = (
            self.new_display.as_ref().and_then(|d| d.width),
            self.new_display.as_ref().and_then(|d| d.height),
        ) {
            if w == 0 || h == 0 {
                return Err(Error::InvalidOption("new_display 尺寸必须为正".into()));
            }
        }
        if self.flex_display && self.new_display.is_none() {
            return Err(Error::InvalidOption(
                "flex_display 需要 new_display（虚拟显示器）".into(),
            ));
        }
        Ok(())
    }
}

/// 组装完整的 app_process 命令行（CLASSPATH 指向已推送的 server jar）。
pub fn server_command(remote_path: &str, args: &[String]) -> String {
    let mut cmd = format!("CLASSPATH={remote_path} app_process / {SERVER_CLASS} {SERVER_VERSION}");
    for a in args {
        cmd.push(' ');
        cmd.push_str(a);
    }
    cmd
}
