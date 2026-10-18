// mw::qemu 对外总头文件 —— include 全部子头。
// ← crates/mwine-qemu/src/{config,console,handle,launcher,ports,mock}.rs
//（Rust lib.rs 为旧版单体实现，已被上述分模块设计取代，未移植）
//
// 组成：
// - config：  QemuConfig（argv 组装/镜像目录校验）+ QemuRuntime（二进制定位）
// - ports：   console/adb 成对端口与 gRPC 端口的自动分配
// - console： emulator console 协议（token/命令/应答）
// - handle：  运行实例句柄（console 命令、boot 等待、优雅关停）
// - launcher：QemuLauncher 接口 + DirectLauncher（fork/exec 直启）
// - mock：    MockLauncher（测试与下游联调桩）
#pragma once

#include "config.hpp"
#include "console.hpp"
#include "handle.hpp"
#include "launcher.hpp"
#include "mock.hpp"
#include "ports.hpp"
