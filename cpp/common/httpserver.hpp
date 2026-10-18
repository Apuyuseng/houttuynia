// 极简 REST 服务器：QTcpServer + 线程池，每连接一线程阻塞处理。
// 契约对齐 mwine-core 的 daemon.rs：单 handler 接 (method, path, body) 返回 (code, body)。
// 连接逐请求关闭（Connection: close），localhost UI 轮询场景足够。
#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTcpServer>
#include <functional>

class QThreadPool;

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

using HttpHandler = std::function<QPair<int, QByteArray>(const HttpRequest&)>;

class HttpServer : public QObject {
    Q_OBJECT
public:
    explicit HttpServer(HttpHandler handler, quint16 port, QObject* parent = nullptr);

    // 监听成功返回 true（端口占用返回 false）
    bool start();

private:
    void onNewConnection();

    HttpHandler m_handler;
    quint16 m_port;
    QTcpServer m_server;
    QThreadPool* m_pool = nullptr;
};

// JSON 应答构造捷径
QPair<int, QByteArray> jsonReply(int code, const QByteArray& json);
QPair<int, QByteArray> jsonError(int code, const QString& message);

} // namespace mw
