// mw::scrcpy 对外总头文件 —— include 全部子头。
// ← crates/mwine-scrcpy/src/lib.rs
//
// 组成：
// - options：   server 启动参数（key=value）与 app_process 命令行
// - control：   控制通道消息编码（22 种类型，大端）
// - device_msg：设备 -> 宿主消息解析
// - io：        流读取抽象（TcpConn / 缓冲 / 回退读）
// - video：     视频流解复用（codec id + session meta + 帧包）
// - audio：     音频流解复用（codec id + 帧包）
// - client：    会话编排（部署 server -> forward -> 三 socket 连接）
// - decoder：   Annex-B 纯函数 + VideoToolbox H.264 硬解（__APPLE__）
//
// player.rs（winit/wgpu 窗口管线）不移植：解码帧以 CVPixelBuffer 形态
// 经 decoder.hpp 交出，渲染交给未来的 Qt 播放器。
#pragma once

#include "audio.hpp"
#include "client.hpp"
#include "control.hpp"
#include "decoder.hpp"
#include "device_msg.hpp"
#include "error.hpp"
#include "io.hpp"
#include "options.hpp"
#include "video.hpp"
