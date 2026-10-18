// 豌豆荚源：网页解析（无公开 API，DOM 结构稳定）。
// ← crates/mwine-store/src/wandoujia.rs（内部头文件，不对 daemon 暴露）。
//
// 链路（2026-10 实测）：
// - 搜索：GET /search?key=<kw>（302 → 会话无关的 /search/<hash>，自动跟随）
// - 版本：GET /apps/<id>（最新版按钮带 data-app-vcode/vname）
//         + GET /apps/<id>/history（历史版本列表，含体积）
// - 下载：最新版 GET /apps/<id>/download/dot 302 → ucdl.25pp.com/*.apk
//   （查询串含 md5/vcode）；历史版本从 /apps/<id>/history_v<vcode> 页
//   提取 binding?downloadUrl=<urlencoded 直链>（同样带 md5/size）。
#pragma once

#include <QString>
#include <QVector>

#include <optional>

#include "../common/result.hpp"
#include "fdroid.hpp"  // ResolvedInternal

namespace mw::store::wandoujia {

inline constexpr const char* BASE = "https://www.wandoujia.com";

class WandoujiaSource {
public:
    mw::Result<QVector<StoreApp>> search(const QString& q, const SourceOpts& opts) const;

    /// 版本列表：最新版（详情页）在前，其后是历史版本（新→旧）。
    mw::Result<QVector<StoreVersion>> versions(const QString& appId, const SourceOpts& opts) const;

    /// 解析下载直链。code=nullopt 或等于最新版 → download/dot；
    /// 否则抓 history_v<code> 页提取 binding 直链。
    /// nameHint：UI 从版本列表带来的版本名（页面解析不到时兜底）。
    mw::Result<ResolvedInternal> resolve(const QString& appId, std::optional<quint64> code,
        const QString& nameHint, const SourceOpts& opts) const;
};

// ---- 纯解析函数（单测直接覆盖）----

/// 搜索页 → 应用条目（`<li class="search-item">` 块）。
QVector<StoreApp> parseSearch(const QString& html);

/// 详情页 → 最新版（「普通下载」按钮携带 data-app-vcode/vname）。
std::optional<StoreVersion> latestFromDetail(const QString& html);

/// 历史版本页 → 版本列表（`<li class="history-list…">` 块；去重 + 按版本号降序）。
QVector<StoreVersion> parseHistory(const QString& html);

/// "260.81MB" / "1.2GB" / "845.6KB" → 字节数（无法解析返回 nullopt）。
std::optional<quint64> parseSizeCn(const QString& s);

}  // namespace mw::store::wandoujia
