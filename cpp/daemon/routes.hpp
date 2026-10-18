// REST 路由（crates/mwine-core/src/api.rs 忠实移植）。
// JSON 字段名 / 状态码 / 错误文案与 Rust 版逐字一致（SwiftUI/Qt 两个 UI 依赖此契约）。
#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>

namespace mw::daemon {

struct AppState;

using Resp = QPair<int, QByteArray>;

// 两级分发（结构照抄 api.rs 的 route_with_body → route）。
Resp routeWithBody(const QString& method, const QString& url, const QByteArray& body,
                   AppState* state);
Resp route(const QString& method, const QString& url, AppState* state);

} // namespace mw::daemon
