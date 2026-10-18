// 零依赖 HTML 解析辅助：属性抽取 / 实体解码 / 去标签 / URL 编解码。
// ← crates/mwine-store/src/html.rs（标记切片风格：只解析需要的字段，不建 DOM）。
#pragma once

#include <QString>

#include <optional>

namespace mw::store::html {

/// 在 `html` 中找 `attr="`（如 `data-app-pname="`）并返回属性值。
/// 值内的 HTML 实体不在此解码（属性值一般已是明文）。
std::optional<QString> attr(const QString& html, const QString& name);

/// 在 `html` 中找第 n 个（0 基）`attr="` 出现的属性值。
std::optional<QString> attrNth(const QString& html, const QString& name, int n);

/// 取两个标记之间的内容（第一次出现）。
std::optional<QString> between(const QString& html, const QString& open, const QString& close);

/// HTML 实体解码（常见命名实体 + 十进制/十六进制数字实体）。
/// 实体名长度封顶 10 字符：`&` 后过远才出现 `;` 视为普通文本。
QString decodeEntities(const QString& s);

/// 去掉全部标签并压缩空白。
QString stripTags(const QString& s);

/// 查询串里的百分号编码（保留 unreserved 字符）。
QString urlencode(const QString& s);

/// 百分号解码（也把 `+` 当空格，用于查询参数值；非法 UTF-8 容错替换）。
QString urldecode(const QString& s);

/// 从 URL 查询串（`?a=1&b=2`，可带完整 URL）里取参数值（返回已解码）。
/// 注意（忠实移植 Rust）：某个 kv 段缺少 `=` 时整个查找直接失败返回 nullopt。
std::optional<QString> queryParam(const QString& url, const QString& key);

}  // namespace mw::store::html
