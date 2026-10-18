// 端口自动分配：console/adb 必须成对（adb=console+1），gRPC 独立。
// ← crates/mwine-qemu/src/ports.rs
#pragma once

#include <QPair>
#include <QtGlobal>

#include <optional>

namespace mw::qemu {

/// 端口是否空闲（bind 探测；仅探测 127.0.0.1，qemu console 也只听本地）。
///（Rust 侧为私有 fn port_free，此处公开以便测试复用）
bool portFree(quint16 port);

/// 从 start（含）起向上找成对空闲端口，返回 (console, console+1)。
/// qemu 的 `-ports` 约定 adb 端口 = console + 1，且 serial 为
/// `emulator-<console>`，因此成对探测不可拆。
std::optional<QPair<quint16, quint16>> pickPortPair(quint16 start);

/// 从 start（含）起向上找单个空闲端口（gRPC 用）。
std::optional<quint16> pickPort(quint16 start);

} // namespace mw::qemu
