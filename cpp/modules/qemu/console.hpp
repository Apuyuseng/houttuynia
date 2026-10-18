// emulator console 协议（与 `adb emu` / SDK emulator launcher 同路径）：
// TCP 连 console 端口 → 问候 → auth token → 命令 → 应答。
// ← crates/mwine-qemu/src/console.rs
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include "result.hpp"

namespace mw::qemu {

/// 认证 token 文件（qemu 与 emulator launcher 同源逻辑，二者互认）。
/// 文件缺失/无 HOME 时返回空串（token 为空则跳过 auth）。
QString consoleToken();

/// 对指定 console 端口执行一条命令，返回原始应答文本。
/// 应答以 OK/KO 结尾（多数命令），由调用方判断。
///（Rust 返回 String；按仓库约定字符串值走 mw::Result<QByteArray>）
mw::Result<QByteArray> consoleCommand(quint16 port, const QString& token, const QString& cmd);

/// 应答是否为 KO（命令被拒）。
bool isKo(const QByteArray& resp);

} // namespace mw::qemu
