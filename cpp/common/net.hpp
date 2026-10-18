// 同步 HTTP 客户端：线程本地 QNetworkAccessManager + 本地事件循环。
// 守护进程的工作线程内使用（每个线程各自一个 QNAM 实例，Qt 线程亲和安全）。
#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>
#include <functional>
#include <atomic>

#include "result.hpp"

namespace mw {

struct HttpReply {
    int status = 0;          // 0 = 网络层失败
    QByteArray body;
    QString error;
    QMap<QByteArray, QByteArray> headers;

    bool ok() const { return status >= 200 && status < 300; }
};

// 单次请求（GET/POST/PUT/DELETE…）
HttpReply httpRequest(const QString& method, const QUrl& url,
                       const QByteArray& body = {},
                       const QMap<QByteArray, QByteArray>& headers = {},
                       int timeoutMs = 60000);

HttpReply httpGet(const QUrl& url, int timeoutMs = 60000);
HttpReply httpPost(const QUrl& url, const QByteArray& body,
                   const QByteArray& contentType = "application/json",
                   int timeoutMs = 60000);

// 流式下载到文件：progress(done, total)（total=0 不定长）；cancel 置位即中止。
// 返回 status；body 为空，成功时落盘 destPath（追加模式续传由调用方处理 Range）。
HttpReply httpDownload(const QUrl& url, const QString& destPath,
                       const std::function<void(qint64, qint64)>& progress = nullptr,
                       const std::atomic_bool* cancel = nullptr,
                       const QMap<QByteArray, QByteArray>& headers = {},
                       int timeoutMs = 0 /*0=不限*/);

} // namespace mw
