//! mwine-scrcpy：scrcpy 4.0 线协议的纯 Rust 客户端。
//!
//! 协议事实全部从 scrcpy v4.0 server 源码逐字节确认：
//! - 三个独立 socket（video / audio / control），forward 模式按序连接
//! - 首个 socket：1 字节 dummy + 64 字节 UTF-8 设备名
//! - 视频流：4 字节 codec id + session meta + 帧包（8B PTS/flags + 4B 长度）
//! - 控制通道 22 种消息类型，大端
//! - 虚拟显示器（new_display / flex_display）即"每应用一个窗口"的基石

pub mod audio;
pub mod client;
pub mod control;
pub mod device_msg;
pub mod error;
pub mod options;
pub mod video;

use std::path::PathBuf;

pub use audio::{AudioCodecId, AudioPacket, AudioStream};
pub use client::{ControlChannel, ScrcpyClient, Session, SessionGuard};
pub use control::ControlMessage;
pub use device_msg::DeviceMessage;
pub use error::{Error, Result};
pub use options::{NewDisplay, ServerOptions, VideoCodecId};
pub use video::{VideoPacket, VideoStream};

/// 常见 scrcpy-server 安装位置。
/// 优先 mwine 自带版本（与客户端 4.0 协议严格对齐；brew 的 jar 随上游漂移，
/// 版本不匹配时 server 立即退出 → 连接头读失败 "failed to fill whole buffer"）。
pub fn find_server_jar() -> Option<PathBuf> {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/root".into());
    let candidates = [
        format!("{home}/.mwine/bin/scrcpy-server-v4.0"),
        "/opt/homebrew/share/scrcpy/scrcpy-server".into(),
        "/opt/homebrew/opt/scrcpy/share/scrcpy/scrcpy-server".into(),
        "/usr/local/share/scrcpy/scrcpy-server".into(),
    ];
    candidates.iter().map(PathBuf::from).find(|p| p.is_file())
}
