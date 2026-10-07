//! mwine-adb：纯 Rust 实现的 ADB smart-socket 客户端。
//!
//! 覆盖本项目所需的子集：设备枚举、shell（v2 优先）、文件推送（sync）、
//! APK 安装、localabstract 端口转发。不依赖 adb 命令行（仅在需要
//! 拉起 adb server 时调用 adb 二进制）。

pub mod client;
pub mod connection;
pub mod error;
pub mod shell;
pub mod sync;

pub use client::{AdbClient, AdbDevice, InstallProgress, ShellStream};
pub use error::{Error, Result};
pub use shell::ShellOutput;
