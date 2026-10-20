#include "httpserver.hpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <qthreadpool.h>
#include <unistd.h>
#include <QtGlobal>

namespace mw {

namespace {

// query 值解码：百分号 + '+' → 空格（query 场景专用，path 部分 Qt 已处理）
QString queryDecode(const QString& s)
{
    return QString::fromUtf8(
        QByteArray(s.toUtf8().replace('+', ' '))
            .percentDecoded());
}

QByteArray reasonPhrase(int code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return QByteArray::number(code);
    }
}

} // namespace

QString HttpRequest::query(const QString& key) const
{
    return queryAll().value(key);
}

QMap<QString, QString> HttpRequest::queryAll() const
{
    QMap<QString, QString> out;
    const QString q = path.section(QLatin1Char('?'), 1);
    if (q.isEmpty())
        return out;
    for (const auto& kv : q.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const auto idx = kv.indexOf(QLatin1Char('='));
        if (idx < 0) {
            out.insert(queryDecode(kv), QString());
        } else {
            out.insert(queryDecode(kv.left(idx)), queryDecode(kv.mid(idx + 1)));
        }
    }
    return out;
}

QPair<int, QByteArray> jsonReply(int code, const QByteArray& json)
{
    return qMakePair(code, json);
}

QPair<int, QByteArray> jsonError(int code, const QString& message)
{
    QJsonObject o;
    o[QStringLiteral("error")] = message;
    return qMakePair(code, QJsonDocument(o).toJson(QJsonDocument::Compact));
}

HttpHandler adaptJsonHandler(std::function<QPair<int, QByteArray>(const HttpRequest&)> fn)
{
    return [fn = std::move(fn)](const HttpRequest& req) -> HttpResponse {
        const auto [code, body] = fn(req);
        return HttpResponse::json(code, body);
    };
}

namespace {

// 单连接处理：解析请求 → handler → 写响应（在池线程执行）
class ConnJob : public QRunnable
{
public:
    ConnJob(qintptr fd, HttpHandler handler) : m_fd(fd), m_handler(std::move(handler))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QTcpSocket sock;
        if (!sock.setSocketDescriptor(m_fd))
            return;
        // 读超时 30s（慢客户端不占线程）
        if (!sock.waitForReadyRead(30000))
            return;

        const QByteArray head = sock.readAll();
        const int hdrEnd = head.indexOf(QByteArrayLiteral("\r\n\r\n"));
        if (hdrEnd < 0)
            return;
        const QList<QByteArray> lines = head.left(hdrEnd).replace('\r', '\n').split('\n');
        if (lines.isEmpty())
            return;
        const QList<QByteArray> parts = lines[0].split(' ');
        if (parts.size() < 2)
            return;

        HttpRequest req;
        req.method = QString::fromLatin1(parts[0]);
        req.path = QString::fromUtf8(QByteArray(parts[1].percentDecoded()));
        for (int i = 1; i < lines.size(); ++i) {
            const int c = lines[i].indexOf(':');
            if (c > 0)
                req.headers.insert(QString::fromLatin1(lines[i].left(c)).toLower(),
                                   QString::fromLatin1(lines[i].mid(c + 1).trimmed()));
        }

        // body（Content-Length）
        QByteArray body = head.mid(hdrEnd + 4);
        const int want = req.headers.value(QStringLiteral("content-length")).toInt();
        while (body.size() < want) {
            if (!sock.waitForReadyRead(30000))
                break;
            body += sock.readAll();
        }
        req.body = body.left(want);

        const HttpResponse r = m_handler(req);
        QByteArray resp;
        resp += "HTTP/1.1 " + QByteArray::number(r.code) + " " + reasonPhrase(r.code) + "\r\n";
        resp += "Content-Type: " + r.contentType + "\r\n";
        resp += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
        resp += "Connection: close\r\n";
        for (auto it = r.extraHeaders.constBegin(); it != r.extraHeaders.constEnd(); ++it) {
            // 响应头注入防护：键值均不允许换行
            if (it.key().contains('\n') || it.key().contains('\r') || it.value().contains('\n')
                || it.value().contains('\r'))
                continue;
            resp += it.key() + ": " + it.value() + "\r\n";
        }
        resp += "\r\n";
        resp += r.body;
        // 大响应（截图/静态资源）分块写，避免单次 write 截断
        qint64 off = 0;
        while (off < resp.size()) {
            const qint64 n = sock.write(resp.mid(off, 256 * 1024));
            if (n < 0)
                break;
            off += n;
            if (!sock.waitForBytesWritten(30000))
                break;
        }
        sock.disconnectFromHost();
    }

private:
    qintptr m_fd;
    HttpHandler m_handler;
};

} // namespace

HttpServer::HttpServer(HttpHandler handler, quint16 port, QObject* parent)
    : HttpServer(std::move(handler), QStringLiteral("127.0.0.1"), port, parent)
{
}

HttpServer::HttpServer(HttpHandler handler, const QString& host, quint16 port,
                       QObject* parent)
    : QObject(parent), m_handler(std::move(handler)), m_host(host), m_port(port)
{
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(16);
    connect(&m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

bool HttpServer::start()
{
    QHostAddress addr(m_host);
    if (m_host == QLatin1String("*") || m_host.compare(QLatin1String("any"),
                                                       Qt::CaseInsensitive)
                                            == 0)
        addr = QHostAddress::Any;
    if (addr.isNull())
        addr = QHostAddress::LocalHost;
    if (!m_server.listen(addr, m_port)) {
        qWarning() << "[http] 监听失败:" << m_server.errorString();
        return false;
    }
    qInfo().noquote() << QStringLiteral("[http] mwine-cored 监听 %1:%2")
                                 .arg(m_server.serverAddress().toString())
                                 .arg(m_port);
    return true;
}

void HttpServer::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket* sock = m_server.nextPendingConnection();
        if (!sock || sock->state() != QAbstractSocket::ConnectedState)
            continue;
        // 描述符 dup 后移交池线程（原 socket 析构会关掉原 fd，dup 副本存活）
        const qintptr fd = ::dup(static_cast<int>(sock->socketDescriptor()));
        sock->close();
        delete sock;
        m_pool->start(new ConnJob(fd, m_handler));
    }
}

} // namespace mw
