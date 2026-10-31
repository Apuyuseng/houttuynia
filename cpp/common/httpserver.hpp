// 极简 REST 服务器：QTcpServer + 线程池，每连接一线程阻塞处理。
// 契约对齐 mwine-core 的 daemon.rs：单 handler 接 (method, path, body) 返回 (code, body)。
// 连接逐请求关闭（Connection: close），localhost UI 轮询场景足够。
//
// NAS/Web 模式扩展：HttpResponse 携带 Content-Type 与自定义响应头
// （静态资源 / PNG 二进制直出），构造时可指定绑定地址（默认 127.0.0.1）。
#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTcpServer>
#include <functional>

class QThreadPool;
class QTcpSocket;

namespace mw {

struct HttpRequest {
    QString method;   // GET / POST / PUT / DELETE
    QString path;     // 含 query（?a=b）
    QByteArray body;  // 请求体（原样字节）
    QMap<QString, QString> headers; // 小写键

    QString pathOnly() const { return path.section(QLatin1Char('?'), 0, 0); }
    // query 参数（百分号已解码；+ 解为空格）
    QString query(const QString& key) const;
    QMap<QString, QString> queryAll() const;
};

struct HttpResponse {
    int code = 200;
    QByteArray contentType = QByteArrayLiteral("application/json");
    QByteArray body;
    QMap<QByteArray, QByteArray> extraHeaders;

    // 流式模式：置位后忽略 body——先写响应头（无 Content-Length，
    // Connection: close），再在池线程内回调持续写流，直到回调返回
    // 或对端断开。视频推流等长连接场景用（占一个池线程是预期内）。
    // 回调返回 false 表示异常终止（连接已被破坏则不再写关闭帧）。
    std::function<bool(QTcpSocket&)> streamer;

    static HttpResponse json(int code, const QByteArray& body)
    {
        HttpResponse r;
        r.code = code;
        r.body = body;
        return r;
    }
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// 旧契约 (code, body) JSON handler 适配器（daemon 的 REST 契约路由继续用）
HttpHandler adaptJsonHandler(std::function<QPair<int, QByteArray>(const HttpRequest&)> fn);

class HttpServer : public QObject {
    Q_OBJECT
public:
    explicit HttpServer(HttpHandler handler, quint16 port, QObject* parent = nullptr);
    // host 形如 "127.0.0.1"（默认）/ "0.0.0.0" / "::" / 具体网卡 IP
    HttpServer(HttpHandler handler, const QString& host, quint16 port,
               QObject* parent = nullptr);

    // 监听成功返回 true（端口占用返回 false）
    bool start();

private:
    void onNewConnection();

    HttpHandler m_handler;
    QString m_host;
    quint16 m_port;
    QTcpServer m_server;
    QThreadPool* m_pool = nullptr;
};

// JSON 应答构造捷径
QPair<int, QByteArray> jsonReply(int code, const QByteArray& json);
QPair<int, QByteArray> jsonError(int code, const QString& message);

} // namespace mw
