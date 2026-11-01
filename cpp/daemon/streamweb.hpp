// scrcpy Web 投屏：会话注册表 + 路由处理（webDispatch 的扩展）。
//
// 设计要点：
// - 每个浏览器页签一个 scrcpy 会话（scid 随机，天然支持并发）；
// - 视频包经「喂包线程 → 有界队列 → HTTP 池线程推流」两级传递：
//   队列满则清空并 resetVideo 让编码器重发 config+关键帧（慢网自愈）；
// - 推流线程 1s 无包时探测 HTTP 连接活性（MSG_PEEK），浏览器关闭即回收会话；
// - 控制消息（触摸/按键/文本/滚轮）走 control socket，注入语义与
//   独立窗口（modules/window/window.cpp）逐字节一致。
#pragma once

#include <QString>

#include "httpserver.hpp"

namespace mw::daemon {

struct AppState;

// /api/stream/* 路由；pathOnly 以 /api/stream 开头时调用。未匹配返回 false。
bool streamDispatch(const mw::HttpRequest& req, AppState* state, const QString& pathOnly,
                    mw::HttpResponse* out);

} // namespace mw::daemon
