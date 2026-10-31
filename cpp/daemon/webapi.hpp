// NAS/Web 模式：静态资源服务 + Token 鉴权 + 二进制/上传扩展路由。
//
// 调度顺序（webDispatch）：
//  1. /api/* 先过 Token 校验（MWINE_TOKEN 非空时强制）；
//  2. 扩展路由：PNG 截图直出 / 应用图标 / APK 原始字节上传（这些不走 JSON 契约）；
//  3. 其余 /api/* 落回 daemon 既有 JSON 契约路由（body 上限 64KB，对齐 Rust）；
//  4. 非 /api 路径 → webDir 静态文件（SPA 回落 index.html）。
#pragma once

#include <QString>

#include "httpserver.hpp"

namespace mw::daemon {

struct AppState;

// Web 根目录解析：MWINE_WEB_DIR > exe 同级 web/ > exe 上两级 Resources/web（.app）> cwd/web/
// 找不到返回空串（此时静态路由 404，API 不受影响）。
QString resolveWebDir();

// 总调度入口（main.cpp 的 HttpServer handler 直接挂这个）
mw::HttpResponse webDispatch(const mw::HttpRequest& req, AppState* state);

} // namespace mw::daemon
