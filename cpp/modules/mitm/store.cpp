// MITM 明文入库 ← crates/mwine-mitm/src/store.rs
#include "store.hpp"

#include <QDateTime>
#include <QDir>
#include <QPair>
#include <QStandardPaths>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <mutex>
#include <optional>

#include "../capture/store.hpp"
#include "log.hpp"

namespace mw::mitm {

namespace {

// 进程级 flows 库（Rust：static FLOWS: Mutex<Option<Store>>）
std::mutex g_flowsMutex;
std::optional<mw::capture::Store> g_flows;

// 流量 ID 基数（Rust：static NEXT_ID: AtomicI64 = 9_000_000_000，与 eBPF 侧 ID 空间隔离）
std::atomic<qint64> g_nextId{ 9000000000LL };

/// Rust str::lines()：按 \n 切分、去行尾 \r、且不产尾随空行
///（"a\n" → ["a"]；Qt split('\n') 会多出一个尾部空串，须去掉）。
QStringList rustLines(const QString& text)
{
    QStringList raw = text.split(QChar(u'\n'));
    if (!raw.isEmpty() && raw.last().isEmpty())
        raw.removeLast();
    QStringList out;
    out.reserve(raw.size());
    for (const QString& r : raw) {
        QString line = r;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        out << line;
    }
    return out;
}

/// 请求/响应边界识别（Rust split_http_messages 的 is_start 判定）。
bool isStartLine(const QString& line, bool isRequest)
{
    if (isRequest) {
        for (const char* m : { "GET ", "POST ", "PUT ", "DELETE ", "HEAD ", "OPTIONS ", "PATCH " }) {
            if (line.startsWith(QString::fromLatin1(m)))
                return true;
        }
        return false;
    }
    return line.startsWith(QStringLiteral("HTTP/1.")) || line.startsWith(QStringLiteral("HTTP/2 "));
}

/// 按起始行切成消息块（Rust split_http_messages）。
QStringList splitHttpMessages(const QString& text, bool isRequest)
{
    QStringList blocks;
    QString current;
    for (const QString& line : rustLines(text)) {
        if (isStartLine(line, isRequest) && !current.isEmpty()) {
            blocks << current;
            current.clear();
        }
        current += line;
        current += QChar(u'\n');
    }
    if (!current.isEmpty())
        blocks << current;
    return blocks;
}

/// 解析单个消息块（Rust parse_single）。空块/无起始行 → 空。
std::optional<mw::capture::HttpMessage> parseSingle(const QString& block, bool /*isRequest*/)
{
    const QStringList lines = rustLines(block);
    if (lines.isEmpty() || lines.first().isEmpty())
        return std::nullopt;

    mw::capture::HttpMessage msg;
    msg.startLine = lines.first();
    QString body;
    bool inBody = false;
    for (int i = 1; i < lines.size(); ++i) {
        const QString& line = lines.at(i);
        if (line.isEmpty() && !inBody) {
            inBody = true;
            continue;
        }
        if (inBody) {
            body += line;
            body += QChar(u'\n');
        } else {
            const int colon = line.indexOf(QLatin1Char(':')); // Rust split_once(':')：首个冒号
            if (colon > 0)
                msg.headers << qMakePair(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
        }
    }
    msg.body = body.toUtf8();
    msg.bodyOffset = qsizetype(block.size()) - qsizetype(body.size()); // 近似（与 Rust 注释一致）
    msg.truncated = false;
    msg.encoding = QString();
    return msg;
}

QVector<mw::capture::HttpMessage> parseMessages(const QByteArray& data, bool isRequest)
{
    // Rust String::from_utf8_lossy → QString::fromUtf8（无效序列替换 U+FFFD）
    QVector<mw::capture::HttpMessage> messages;
    for (const QString& block : splitHttpMessages(QString::fromUtf8(data), isRequest)) {
        if (auto m = parseSingle(block, isRequest))
            messages << *m;
    }
    return messages;
}

QString headText(const mw::capture::HttpMessage* m)
{
    if (!m)
        return QString();
    QString s = m->startLine;
    for (const auto& [k, v] : m->headers) {
        s += QChar(u'\n');
        s += k;
        s += QStringLiteral(": ");
        s += v;
    }
    return s;
}

/// 组装一个交互视图（Rust make_view）。
mw::capture::ExchangeView makeView(const QString& sni, const mw::capture::HttpMessage* req,
                                   const mw::capture::HttpMessage* resp)
{
    mw::capture::ExchangeView v;
    v.streamId = 0;
    v.host = sni;
    if (req) {
        const QStringList parts = req->startLine.split(QChar(u' '));
        v.method = parts.value(0);
        v.path = parts.value(1);
    }
    if (resp) {
        const QStringList parts = resp->startLine.split(QChar(u' '));
        bool ok = false;
        const ushort st = parts.value(1).toUShort(&ok);
        v.status = ok ? st : quint16(0);
    }
    v.reqHead = headText(req);
    v.respHead = headText(resp);
    if (req && !req->body.isEmpty())
        v.reqBody = mw::capture::BodyView::from(req->body);
    if (resp && !resp->body.isEmpty())
        v.respBody = mw::capture::BodyView::from(resp->body);
    return v;
}

} // namespace

QString flowsDbPath()
{
    const QString home = qEnvironmentVariable("HOME");
    return QDir(home.isEmpty() ? QStringLiteral("/tmp") : home)
        .filePath(QStringLiteral(".mwine/capture/flows.db"));
}

QVector<mw::capture::ExchangeView> parseHttp(const QString& sni, const QByteArray& clientBytes,
                                             const QByteArray& serverBytes)
{
    const auto reqMsgs = parseMessages(clientBytes, true);
    const auto respMsgs = parseMessages(serverBytes, false);
    const int n = qMin(qMax(int(reqMsgs.size()), int(respMsgs.size())), 64);
    QVector<mw::capture::ExchangeView> exchanges;
    for (int i = 0; i < n; ++i) {
        const mw::capture::HttpMessage* req = i < reqMsgs.size() ? &reqMsgs[i] : nullptr;
        const mw::capture::HttpMessage* resp = i < respMsgs.size() ? &respMsgs[i] : nullptr;
        if (!req && !resp)
            continue;
        exchanges << makeView(sni, req, resp);
    }
    return exchanges;
}

void captureExchange(const QString& sni, const QByteArray& clientBytes, const QByteArray& serverBytes)
{
    const QVector<mw::capture::ExchangeView> exchanges = parseHttp(sni, clientBytes, serverBytes);
    if (exchanges.isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 id = g_nextId.fetch_add(1, std::memory_order_relaxed);

    // 首行摘要（廉价路径；Rust 同款字段推导：首个 \n 前的切片）
    const auto firstLine = [](const QByteArray& b) {
        const int nl = b.indexOf('\n');
        return QString::fromUtf8(nl >= 0 ? b.left(nl) : b).trimmed();
    };
    const QString reqLine = firstLine(clientBytes);
    const QString respLine = firstLine(serverBytes);
    const QStringList reqParts = reqLine.split(QChar(u' '));
    const QString method = reqParts.value(0);
    const QString path = reqParts.value(1);
    //（status/首部文本在 Rust 侧同为计算后弃用：摘要由 FlowSummary::from 提取）
    mw::capture::Flow flow;
    flow.id = id;
    flow.firstTsMs = now;
    flow.lastTsMs = now;
    flow.pid = 0;
    flow.comm = QStringLiteral("mitm");
    flow.package = QStringLiteral("mitm");
    flow.src = QString();
    flow.dst = QStringLiteral("%1:443").arg(sni);
    flow.kind = mw::capture::FlowKind::Http1;
    {
        mw::capture::HttpMessage request;
        request.startLine = QStringLiteral("%1 %2 HTTP/1.1").arg(method, path);
        request.headers << qMakePair(QStringLiteral("Host"), sni);
        flow.request = std::move(request);
    }
    {
        mw::capture::HttpMessage response;
        response.startLine = respLine.isEmpty() ? QStringLiteral("HTTP/1.1 0") : respLine;
        flow.response = std::move(response);
    }
    flow.exchanges = exchanges;
    flow.wbytes = quint64(clientBytes.size());
    flow.rbytes = quint64(serverBytes.size());
    flow.rawWrite = clientBytes.left(512 * 1024);
    flow.rawRead = serverBytes.left(512 * 1024);
    // status/method/path 摘要由存储层 FlowSummary::from(flow) 提取（与 Rust 注释一致）

    std::lock_guard<std::mutex> lock(g_flowsMutex);
    if (!g_flows || !g_flows->isOpen()) {
        auto s = mw::capture::Store::open(flowsDbPath());
        if (!s) {
            MW_LOG("mitm", QStringLiteral("打开 flows.db 失败: %1").arg(s.error()));
            g_flows.reset();
            return;
        }
        g_flows = std::move(s.value());
    }
    (void)g_flows->upsertFlows(QVector<mw::capture::Flow>{ flow }); // Rust：忽略写入错误
}

} // namespace mw::mitm
