// mw::adb 对外总头文件 —— include 全部子头。
// ← crates/mwine-adb/src/lib.rs
//
// 组成：
// - tcpconn：POSIX fd RAII 封装（替代 Rust std::net::TcpStream）
// - wire：   smart socket 线协议（长度前缀帧/OKAY/FAIL）+ adbd 直连线协议
// - shell：  shell v1/v2 协议解析、stdin 帧、服务字符串
// - client： AdbClient 高层 API（设备/shell/推送/安装/转发）+ sync 子协议
// - devices：真机连接辅助（配对二维码/mDNS 发现/adb pair·tcpip/二进制定位）
//
// 说明：mw::Result<QString> 为 tcpconn.hpp 中的显式特化
//（common/result.hpp 主模板禁止实例化该类型，而本模块契约需要）。
#pragma once

#include "client.hpp"
#include "devices.hpp"
#include "shell.hpp"
#include "tcpconn.hpp"
#include "wire.hpp"
