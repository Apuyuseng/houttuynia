#include "net.hpp"

#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace mw {

static QNetworkAccessManager* tnam()
{
    // 线程本地：QNAM 必须与使用它的线程同亲和
    thread_local QNetworkAccessManager nam;
    nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    return &nam;
}

HttpReply httpRequest(const QString& method, const QUrl& url, const QByteArray& body,
                      const QMap<QByteArray, QByteArray>& headers, int timeoutMs)
{
    QNetworkRequest req(url);
    for (auto it = headers.begin(); it != headers.end(); ++it)
        req.setRawHeader(it.key(), it.value());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    auto* reply = tnam()->sendCustomRequest(req, method.toUtf8(), body);
    HttpReply out;
    QEventLoop loop;
    QTimer timer;
    if (timeoutMs > 0) {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = reply->readAll();
    const auto hdrs = reply->rawHeaderPairs();
    for (const auto& h : hdrs)
        out.headers.insert(h.first, h.second);
    if (reply->error() != QNetworkReply::NoError)
        out.error = reply->errorString();
    else if (timeoutMs > 0 && !timer.isActive())
        out.error = QStringLiteral("请求超时（%1ms）").arg(timeoutMs);
    reply->deleteLater();
    return out;
}

HttpReply httpGet(const QUrl& url, int timeoutMs)
{
    return httpRequest(QStringLiteral("GET"), url, {}, {}, timeoutMs);
}

HttpReply httpPost(const QUrl& url, const QByteArray& body, const QByteArray& contentType,
                   int timeoutMs)
{
    QMap<QByteArray, QByteArray> h;
    h.insert("Content-Type", contentType);
    return httpRequest(QStringLiteral("POST"), url, body, h, timeoutMs);
}

HttpReply httpDownload(const QUrl& url, const QString& destPath,
                       const std::function<void(qint64, qint64)>& progress,
                       const std::atomic_bool* cancel,
                       const QMap<QByteArray, QByteArray>& headers, int timeoutMs)
{
    QNetworkRequest req(url);
    for (auto it = headers.begin(); it != headers.end(); ++it)
        req.setRawHeader(it.key(), it.value());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QFile file(destPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return { 0, {}, QStringLiteral("无法写入 %1: %2").arg(destPath, file.errorString()), {} };

    auto* reply = tnam()->get(req);
    qint64 done = 0, total = 0;
    HttpReply out;

    QObject::connect(reply, &QNetworkReply::downloadProgress, &file,
                     [&](qint64 d, qint64 t) {
                         done = d;
                         total = t;
                         if (progress)
                             progress(d, t);
                     });
    QObject::connect(reply, &QNetworkReply::readyRead, &file, [&]() {
        file.write(reply->readAll());
    });

    QEventLoop loop;
    QTimer timer;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (timeoutMs > 0) {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
    }
    // 取消轮询：cancel 置位即 abort（100ms 粒度）
    QTimer cancelTimer;
    if (cancel) {
        cancelTimer.setInterval(100);
        QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&]() {
            if (cancel->load()) {
                reply->abort();
                loop.quit();
            }
        });
        cancelTimer.start();
    }
    loop.exec();
    cancelTimer.stop();
    file.write(reply->readAll()); // 余量
    file.close();

    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::OperationCanceledError)
        out.error = reply->errorString();
    if (cancel && cancel->load())
        out.error = QStringLiteral("已取消");
    reply->deleteLater();
    return out;
}

} // namespace mw
