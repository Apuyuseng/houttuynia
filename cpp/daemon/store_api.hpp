// 在线应用商店 API（crates/mwine-core/src/store_api.rs 忠实移植）：
// 来源列表 / 搜索 / 版本列表 / 下载任务。
//
// 下载任务（与镜像任务同款单任务模型）只负责「解析直链 → 下载 → 校验哈希 →
// 校验包名」，产出本地 APK 路径；安装与桌面壳生成走既有 CLI 流程
// （UI 拿到路径后调 `mwine app create --apk`），装完由 UI 删除安装包。
#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>

namespace mw::daemon {

struct AppState;

using Resp = QPair<int, QByteArray>;

// 清理 7 天前的残留安装包（下载完成但 UI 中途退出/安装失败未清理的场景）。
void purgeStaleApks();

// GET /api/store/sources
Resp storeSources(AppState* state);
// GET /api/store/search?q=..&source=..&mirror=..&proxy=..
Resp storeSearch(AppState* state, const QString& url);
// GET /api/store/recommend?source=fdroid&mirror=..&proxy=..（精选推荐）
Resp storeRecommend(AppState* state, const QString& url);
// GET /api/store/versions?source=..&app_id=..&package=..
Resp storeVersions(AppState* state, const QString& url);
// GET /api/store/task
Resp storeTaskGet(AppState* state);
// POST /api/store/cancel
Resp storeCancel(AppState* state);
// POST /api/store/download（body: source/app_id/package/name/version_code/...）
Resp storeDownload(AppState* state, const QByteArray& body);

// GET /api/apps/install/task（APK 安装任务：phase/progress/progress_total）
Resp appsInstallTaskGet(AppState* state);
// POST /api/apps/install（body: {"path": 本地 APK 路径}；字节级进度经任务端点轮询）
Resp appsInstallPost(AppState* state, const QByteArray& body);

} // namespace mw::daemon
