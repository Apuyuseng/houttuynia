// MITM 明文入库：写入 flows 库。
// ← crates/mwine-mitm/src/store.rs
// 连接结束后把双向明文交给解析（HTTP 消息切分/配对），组装 Flow 后交给
// mw_capture 的 SQLite Store（对应 Rust 侧依赖 mwine-capture 的 Store::upsert_flows）。
//
// 注：任务书对 store.rs 的描述是「证书存储」，实际 Rust 源是明文流量落库
//（证书的生成/加载在 ca.*）；本文件忠实移植其真实职责。
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include "../capture/flow.hpp"

namespace mw::mitm {

/// flows 库路径（Rust：$HOME/.mwine/capture/flows.db，HOME 缺失回退 /tmp）。
QString flowsDbPath();

/// 纯解析：把双向明文切成 HTTP/1 交互视图（Rust parse_http，公开以便测试）。
QVector<mw::capture::ExchangeView> parseHttp(const QString& sni, const QByteArray& clientBytes,
                                             const QByteArray& serverBytes);

/// 连接结束后入库（Rust capture_exchange）：
/// 解析为空 → 跳过；Store 惰性打开（首次调用时落 ~/.mwine/capture/flows.db），
/// 打开/写入失败仅告警不中断（Rust 为 expect panic，守护进程内放宽）。
void captureExchange(const QString& sni, const QByteArray& clientBytes,
                     const QByteArray& serverBytes);

} // namespace mw::mitm
