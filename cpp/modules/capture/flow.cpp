// Flow 重组实现 ← crates/mwine-capture/src/flow.rs
#include "flow.hpp"

#include <QJsonArray>
#include <QJsonValue>

#include <set>
#include <utility>

#include <zlib.h>

#include "compress.hpp"
#include "hpack.hpp"

namespace mw::capture {

const char* flowKindStr(FlowKind k)
{
    switch (k) {
    case FlowKind::Http1: return "http1";
    case FlowKind::Http2: return "http2";
    case FlowKind::Encrypted: return "encrypted";
    case FlowKind::Plaintext: return "plaintext";
    case FlowKind::Unknown: return "unknown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// BodyView / ExchangeView JSON

std::optional<BodyView> BodyView::from(const QByteArray& b)
{
    if (b.isEmpty())
        return std::nullopt;
    BodyView v;
    const qsizetype cap = 2 * 1024 * 1024;
    v.text = QString::fromUtf8(b.left(int(std::min<qsizetype>(b.size(), cap))));
    v.size = b.size();
    return v;
}

QJsonObject BodyView::toJson() const
{
    return QJsonObject {
        { QStringLiteral("text"), text },
        { QStringLiteral("size"), qint64(size) },
    };
}

QJsonObject ExchangeView::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("stream_id"), qint64(streamId));
    o.insert(QStringLiteral("method"), method);
    o.insert(QStringLiteral("host"), host);
    o.insert(QStringLiteral("path"), path);
    o.insert(QStringLiteral("status"), status);
    o.insert(QStringLiteral("req_head"), reqHead);
    o.insert(QStringLiteral("req_body"), reqBody ? reqBody->toJson() : QJsonValue());
    o.insert(QStringLiteral("req_body_decoded"),
             reqBodyDecoded ? reqBodyDecoded->toJson() : QJsonValue());
    o.insert(QStringLiteral("resp_head"), respHead);
    o.insert(QStringLiteral("resp_body"), respBody ? respBody->toJson() : QJsonValue());
    o.insert(QStringLiteral("resp_body_decoded"),
             respBodyDecoded ? respBodyDecoded->toJson() : QJsonValue());
    o.insert(QStringLiteral("resp_encoding"), respEncoding);
    return o;
}

// ---------------------------------------------------------------------------
// HttpMessage

std::optional<QString> HttpMessage::header(const QString& name) const
{
    for (const auto& kv : headers) {
        if (kv.first.compare(name, Qt::CaseInsensitive) == 0)
            return kv.second;
    }
    return std::nullopt;
}

std::optional<QString> HttpMessage::host() const
{
    auto h = header(QStringLiteral("Host"));
    if (!h)
        h = header(QStringLiteral(":authority"));
    return h;
}

namespace {

// 检测消息起点：请求方法 token 或 HTTP/1.x 状态行。
bool looksLikeHttpAt(const QByteArray& raw, qsizetype pos)
{
    if (pos >= raw.size())
        return false;
    const int nl = raw.indexOf('\n', pos);
    const qsizetype end = nl < 0 ? raw.size() : nl;
    QByteArray first = raw.mid(pos, end - pos);
    if (!first.isEmpty() && first.back() == '\r')
        first.chop(1);
    const QString line = QString::fromUtf8(first);
    const int sp = line.indexOf(QLatin1Char(' '));
    const QString method = sp < 0 ? line : line.left(sp);
    static const QList<QString> kMethods = {
        QStringLiteral("GET"), QStringLiteral("POST"), QStringLiteral("PUT"),
        QStringLiteral("DELETE"), QStringLiteral("HEAD"), QStringLiteral("OPTIONS"),
        QStringLiteral("PATCH"), QStringLiteral("TRACE"), QStringLiteral("CONNECT"),
    };
    return kMethods.contains(method) || line.startsWith(QLatin1String("HTTP/1."));
}

// 找到 \r\n\r\n 的结束位置（返回头部区长度）。
std::optional<qsizetype> findHeaderEnd(const QByteArray& data)
{
    const int p = data.indexOf("\r\n\r\n");
    if (p < 0)
        return std::nullopt;
    return qsizetype(p) + 4;
}

std::optional<qsizetype> findSub(const QByteArray& hay, const char* needle)
{
    const int p = hay.indexOf(needle);
    if (p < 0)
        return std::nullopt;
    return qsizetype(p);
}

// zlib/raw-inflate 全量解压（windowBits: 15=zlib, -15=raw deflate）。
std::optional<QByteArray> inflateAll(const QByteArray& data, int windowBits)
{
    z_stream zs {};
    if (inflateInit2(&zs, windowBits) != Z_OK)
        return std::nullopt;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    zs.avail_in = static_cast<uInt>(data.size());
    QByteArray out;
    char buf[65536];
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
            break;
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (ret == Z_BUF_ERROR && zs.avail_in == 0)
            break;
        if (ret == Z_OK && zs.avail_in == 0 && zs.avail_out > 0)
            break; // 输入耗尽且无进展
    }
    const bool complete = (ret == Z_STREAM_END);
    inflateEnd(&zs);
    if (!complete || out.isEmpty())
        return std::nullopt;
    return out;
}

std::optional<QByteArray> gzipDecode(const QByteArray& data)
{
    // 基础库 gzip 解压（zlib inflate with gzip window）
    auto r = mw::gzipDecompress(data);
    if (!r || r.value().isEmpty())
        return std::nullopt;
    return r.value();
}

std::optional<QByteArray> deflateDecode(const QByteArray& data)
{
    // 先按 zlib 包装解，失败再按 raw deflate 解
    if (auto z = inflateAll(data, 15))
        return z;
    return inflateAll(data, -15);
}

// gzip/deflate 解压（多成员 gzip 由基础库处理）。
void decodeBody(HttpMessage* msg)
{
    const QString enc = msg->header(QStringLiteral("Content-Encoding"))
                            .value_or(QString())
                            .toLower();
    if (enc.isEmpty() || enc == QLatin1String("identity"))
        return;
    msg->encoding = enc;
    std::optional<QByteArray> decoded;
    if (enc.contains(QLatin1String("gzip")))
        decoded = gzipDecode(msg->body);
    else if (enc.contains(QLatin1String("deflate")))
        decoded = deflateDecode(msg->body);
    // br/zstd 等：仅标注，不解压
    if (decoded)
        msg->bodyDecoded = *decoded;
}

// 解析单条 HTTP 消息（截断容忍）。
std::optional<HttpMessage> parseHttp(const QByteArray& data)
{
    if (data.startsWith("PRI * HTTP/2.0"))
        return std::nullopt;
    if (!looksLikeHttpAt(data, 0))
        return std::nullopt;
    QByteArray head, body;
    bool truncated = false;
    if (const auto sep = findHeaderEnd(data)) {
        head = data.left(int(*sep));
        body = data.mid(int(*sep));
    } else {
        head = data;
        truncated = true;
    }
    HttpMessage msg;
    msg.truncated = truncated;
    const QString text = QString::fromUtf8(head);
    const QStringList lines = text.split(QLatin1Char('\n'));
    bool first = true;
    for (const QString& rawLine : lines) {
        QString l = rawLine;
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
        if (first) {
            first = false;
            msg.startLine = l;
            continue;
        }
        if (l.isEmpty())
            continue;
        const int colon = l.indexOf(QLatin1Char(':'));
        if (colon > 0)
            msg.headers.append(qMakePair(l.left(colon).trimmed(), l.mid(colon + 1).trimmed()));
    }
    msg.body = body;
    msg.bodyOffset = data.size() - body.size();
    decodeBody(&msg);
    return msg;
}

// 单条消息的边界：(占用字节数, 正文有效长度提示)。
// - 有 Content-Length → (头+定长体, Some(定长))；
// - chunked → (到终止块, None 保原始块)；
// - 请求且两者皆无 → (头部, Some(0))；
// - 响应且两者皆无 → 连接关闭语义 → (吃满, None)。
std::pair<qsizetype, std::optional<qsizetype>> messageExtents(const HttpMessage& msg,
                                                              qsizetype available)
{
    bool ok = false;
    const quint64 cl = msg.header(QStringLiteral("Content-Length"))
                           .value_or(QString())
                           .trimmed()
                           .toULongLong(&ok);
    if (ok) {
        const qsizetype want = msg.bodyOffset + qsizetype(cl);
        return { std::min(want, available), std::optional<qsizetype>(qsizetype(cl)) };
    }
    const QString te = msg.header(QStringLiteral("Transfer-Encoding"))
                           .value_or(QString())
                           .toLower();
    if (te.contains(QLatin1String("chunked"))) {
        if (const auto p = findSub(msg.body, "\r\n0\r\n"))
            return { msg.bodyOffset + *p + 5, std::nullopt };
    }
    const bool isRequest = !msg.startLine.startsWith(QLatin1String("HTTP/"));
    if (isRequest)
        return { msg.bodyOffset, std::optional<qsizetype>(qsizetype(0)) };
    return { available, std::nullopt };
}

// H1 消息序列解析：支持 keep-alive 多消息 + 半路接入残片跳过。
QVector<HttpMessage> parseH1Messages(const QByteArray& raw)
{
    QVector<HttpMessage> out;
    qsizetype pos = 0;
    while (pos < raw.size() && out.size() < 128) {
        if (!looksLikeHttpAt(raw, pos)) {
            const int nl = raw.indexOf('\n', pos);
            if (nl < 0)
                break;
            pos = nl + 1;
            continue;
        }
        auto msg = parseHttp(raw.mid(int(pos)));
        if (!msg)
            break;
        const qsizetype available = raw.size() - pos;
        const auto [consumed, bodyCut] = messageExtents(*msg, available);
        pos += std::min(consumed, available);
        // 截断容忍的 body 是"剩余全部"：keep-alive 场景按声明长度裁剪
        if (bodyCut && msg->body.size() > *bodyCut)
            msg->body.truncate(int(*bodyCut));
        out.push_back(std::move(*msg));
    }
    return out;
}

// ---------------------------------------------------------------------------
// HTTP/2

const char kH2Preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// 宽松帧形校验：前两个帧头的长度/类型/保留位 sane。
bool looksLikeH2Frames(const QByteArray& raw)
{
    const auto check = [](const char* f, qsizetype len) -> bool {
        if (len < 9)
            return false;
        const auto len24 = (uint32_t(uint8_t(f[0])) << 16) | (uint32_t(uint8_t(f[1])) << 8)
                           | uint32_t(uint8_t(f[2]));
        const uint8_t kind = uint8_t(f[3]);
        const bool streamOk = (uint8_t(f[5]) & 0x80) == 0;
        return len24 <= (1u << 20) && kind <= 0x9 && streamOk;
    };
    if (raw.size() < 9 || !check(raw.constData(), raw.size()))
        return false;
    const uint32_t len0 = (uint32_t(uint8_t(raw.at(0))) << 16)
                          | (uint32_t(uint8_t(raw.at(1))) << 8) | uint32_t(uint8_t(raw.at(2)));
    if (raw.size() >= qsizetype(9 + len0) + 9)
        return check(raw.constData() + 9 + len0, raw.size() - 9 - len0);
    return true;
}

// 是否像 H2：有前言，或首部像合法帧头（半路接入）。
bool isH2(const QByteArray& raw)
{
    return raw.startsWith(kH2Preface) || looksLikeH2Frames(raw);
}

std::optional<qsizetype> h2BodyStart(const QByteArray& raw)
{
    if (raw.startsWith(kH2Preface))
        return qsizetype(qstrlen(kH2Preface));
    if (looksLikeH2Frames(raw))
        return qsizetype(0);
    return std::nullopt;
}

struct H2Frame {
    uint8_t kind = 0;
    uint8_t flags = 0;
    uint32_t stream = 0;
    qsizetype begin = 0; // payload 区间 [begin, end)
    qsizetype end = 0;
};

// 帧游走（容忍尾部不完整帧）。
QVector<H2Frame> h2Frames(const QByteArray& raw)
{
    QVector<H2Frame> out;
    const auto start = h2BodyStart(raw);
    if (!start)
        return out;
    qsizetype pos = *start;
    while (pos + 9 <= raw.size()) {
        const uint32_t len = (uint32_t(uint8_t(raw.at(pos))) << 16)
                             | (uint32_t(uint8_t(raw.at(pos + 1))) << 8)
                             | uint32_t(uint8_t(raw.at(pos + 2)));
        const uint8_t kind = uint8_t(raw.at(pos + 3));
        const uint8_t flags = uint8_t(raw.at(pos + 4));
        const uint32_t stream = (uint32_t(uint8_t(raw.at(pos + 5)) & 0x7f) << 24)
                                | (uint32_t(uint8_t(raw.at(pos + 6))) << 16)
                                | (uint32_t(uint8_t(raw.at(pos + 7))) << 8)
                                | uint32_t(uint8_t(raw.at(pos + 8)));
        const qsizetype end = pos + 9 + qsizetype(len);
        if (end > raw.size())
            break;
        out.push_back(H2Frame { kind, flags, stream, pos + 9, end });
        pos = end;
    }
    return out;
}

// HEADERS 载荷去除 PADDED(0x8)/优先级(0x20) 前缀。
QByteArray h2HeadersPayload(const QByteArray& payload, uint8_t flags)
{
    QByteArray p = payload;
    if (flags & 0x8) {
        if (p.isEmpty())
            return p;
        const qsizetype pad = uint8_t(p.at(0));
        p = p.mid(1, std::max<qsizetype>(0, p.size() - 1 - pad));
    }
    if ((flags & 0x20) && p.size() >= 5)
        p = p.mid(5);
    return p;
}

QVector<QPair<QString, QString>> utf8Headers(const QVector<QPair<QByteArray, QByteArray>>& raw)
{
    QVector<QPair<QString, QString>> out;
    for (const auto& kv : raw)
        out.append(qMakePair(QString::fromUtf8(kv.first), QString::fromUtf8(kv.second)));
    return out;
}

// H2 头列表 → HttpMessage（伪头展开到 startLine，:authority 保留为头）。
HttpMessage messageFromH2Headers(const QVector<QPair<QString, QString>>& headers)
{
    QString method, authority, path, status;
    QVector<QPair<QString, QString>> rest;
    for (const auto& kv : headers) {
        if (kv.first == QLatin1String(":method"))
            method = kv.second;
        else if (kv.first == QLatin1String(":authority"))
            authority = kv.second;
        else if (kv.first == QLatin1String(":path"))
            path = kv.second;
        else if (kv.first == QLatin1String(":status"))
            status = kv.second;
        else
            rest.append(kv);
    }
    HttpMessage msg;
    if (!status.isEmpty()) {
        msg.startLine = QStringLiteral("HTTP/2 %1").arg(status);
        msg.headers = rest;
        return msg;
    }
    if (!authority.isEmpty())
        rest.prepend(qMakePair(QStringLiteral(":authority"), authority));
    msg.startLine = QStringLiteral("%1 %2 HTTP/2")
                        .arg(method, path.isEmpty() ? QStringLiteral("/") : path);
    msg.headers = rest;
    return msg;
}

struct SideMessages {
    std::map<quint32, std::pair<std::optional<HttpMessage>, QByteArray>> map;
};

void storeH2Headers(SideMessages* side, quint32 stream,
                    const QVector<QPair<QString, QString>>& headers)
{
    auto& e = side->map[stream];
    if (!e.first.has_value())
        e.first = messageFromH2Headers(headers);
}

// 单侧字节流按流收集：HEADERS（CONTINUATION 拼接，同一解码器跨块保持
// 动态表状态）与 DATA。
SideMessages h2SideMessages(const QByteArray& raw)
{
    SideMessages side;
    hpack::Decoder decoder;
    std::optional<std::pair<quint32, QByteArray>> pending;
    for (const H2Frame& f : h2Frames(raw)) {
        const QByteArray payload = raw.mid(int(f.begin), int(f.end - f.begin));
        switch (f.kind) {
        case 0x1: { // HEADERS
            const QByteArray block = h2HeadersPayload(payload, f.flags);
            if (f.flags & 0x4) { // END_HEADERS
                QVector<QPair<QByteArray, QByteArray>> headers;
                if (decoder.decode(block, &headers))
                    storeH2Headers(&side, f.stream, utf8Headers(headers));
            } else {
                pending = std::make_pair(f.stream, block);
            }
            break;
        }
        case 0x9: { // CONTINUATION
            if (pending.has_value()) {
                pending->second.append(payload);
                if ((f.flags & 0x4) && pending->first == f.stream) {
                    const auto done = *pending;
                    pending.reset();
                    QVector<QPair<QByteArray, QByteArray>> headers;
                    if (decoder.decode(done.second, &headers))
                        storeH2Headers(&side, done.first, utf8Headers(headers));
                }
            }
            break;
        }
        case 0x0: { // DATA
            QByteArray data = payload;
            if ((f.flags & 0x8) && !data.isEmpty()) {
                const qsizetype pad = uint8_t(data.at(0));
                data = data.mid(1, std::max<qsizetype>(0, data.size() - 1 - pad));
            }
            side.map[f.stream].second.append(data);
            break;
        }
        default:
            break;
        }
    }
    // DATA 并入消息体并尝试解压
    for (auto& kv : side.map) {
        if (kv.second.first.has_value()) {
            kv.second.first->body = kv.second.second;
            decodeBody(&*kv.second.first);
        }
    }
    return side;
}

ExchangeView exchangeView(quint32 streamId, const HttpMessage* req, const HttpMessage* resp)
{
    const auto headText = [](const HttpMessage* m) -> QString {
        if (!m)
            return QString();
        QString s = m->startLine;
        for (const auto& kv : m->headers) {
            s += QLatin1Char('\n');
            s += kv.first;
            s += QLatin1String(": ");
            s += kv.second;
        }
        return s;
    };
    ExchangeView e;
    e.streamId = streamId;
    if (req) {
        const QStringList parts = req->startLine.split(QLatin1Char(' '));
        e.method = parts.isEmpty() ? QString() : parts.at(0);
        e.host = req->host().value_or(QString());
        e.path = parts.size() > 1 ? parts.at(1) : QString();
    }
    if (resp) {
        const QStringList parts = resp->startLine.split(QLatin1Char(' '));
        if (parts.size() > 1)
            e.status = quint16(parts.at(1).toUShort());
    }
    e.reqHead = headText(req);
    if (req)
        e.reqBody = BodyView::from(req->body);
    if (req && req->bodyDecoded)
        e.reqBodyDecoded = BodyView::from(*req->bodyDecoded);
    e.respHead = headText(resp);
    if (resp)
        e.respBody = BodyView::from(resp->body);
    if (resp && resp->bodyDecoded)
        e.respBodyDecoded = BodyView::from(*resp->bodyDecoded);
    e.respEncoding = resp ? resp->encoding : QString();
    return e;
}

// 原始字节预览：前 512B 可见化（不可打印字符 → ·）。
QString rawPreviewText(const QByteArray& write, const QByteArray& read)
{
    const auto vis = [](const QByteArray& b) -> QString {
        QString out;
        const qsizetype n = std::min<qsizetype>(b.size(), 512);
        for (qsizetype i = 0; i < n; ++i) {
            const uint8_t c = uint8_t(b.at(i));
            if ((c >= 0x20 && c < 0x7f) || c == '\n' || c == '\r' || c == '\t')
                out += QChar(QLatin1Char(char(c)));
            else
                out += QChar(0x00B7); // ·
        }
        return out;
    };
    return QStringLiteral(">> %1\n<< %2").arg(vis(write), vis(read));
}

// 廉价 H1 首消息：只取头部（正文按 Content-Length 声明截取前 4KB 作预览，不搬大正文）。
std::optional<HttpMessage> firstH1Head(const QByteArray& raw, bool requestSide)
{
    qsizetype pos = 0;
    while (pos < raw.size()) {
        if (!looksLikeHttpAt(raw, pos)) {
            const int nl = raw.indexOf('\n', pos);
            if (nl < 0)
                return std::nullopt;
            pos = nl + 1;
            continue;
        }
        auto msg = parseHttp(raw.mid(int(pos)));
        if (!msg)
            return std::nullopt;
        const bool isResponse = msg->startLine.startsWith(QLatin1String("HTTP/"));
        if (isResponse == requestSide) {
            // 方向不符（请求侧遇到响应行/响应侧遇到请求行）→ 继续找
            pos += std::max<qsizetype>(msg->bodyOffset, 1);
            continue;
        }
        // 大正文不随摘要搬运（全量正文走 computeExchanges）：截 4KB 预览
        if (msg->body.size() > 4096)
            msg->body.truncate(4096);
        return msg;
    }
    return std::nullopt;
}

// 廉价 H2 首头部：只解码第一个 HEADERS 块（独立解码器；动态表引用在首块
// 不成立时返回 None）。
std::optional<HttpMessage> firstH2Head(const QByteArray& raw)
{
    hpack::Decoder decoder;
    for (const H2Frame& f : h2Frames(raw)) {
        if (f.kind == 0x1) {
            const QByteArray block = h2HeadersPayload(raw.mid(int(f.begin), int(f.end - f.begin)),
                                                       f.flags);
            QVector<QPair<QByteArray, QByteArray>> headers;
            if (!decoder.decode(block, &headers))
                return std::nullopt;
            return messageFromH2Headers(utf8Headers(headers));
        }
    }
    return std::nullopt;
}

// 定稿（摘要优先）：只做廉价的首消息头部扫描，全量解析（多消息/多流/
// HPACK DATA 重组/gzip）推迟到详情打开时（computeExchanges，按需 + 缓存）。
void finalizeFlow(Flow* f)
{
    const bool h2 = isH2(f->rawWrite) || isH2(f->rawRead);
    if (h2) {
        f->kind = FlowKind::Http2;
    } else if (parseHttp(f->rawWrite) || parseHttp(f->rawRead)) {
        f->kind = FlowKind::Http1;
    } else if (f->wbytes > 0 || f->rbytes > 0) {
        f->kind = FlowKind::Plaintext;
    } else if (!f->dst.isEmpty()) {
        // 有连接五元组但无明文数据：Cronet/X5/Flutter 等不可解密的 TLS
        f->kind = FlowKind::Encrypted;
    } else {
        f->kind = FlowKind::Unknown;
    }
    switch (f->kind) {
    case FlowKind::Http1:
        // 廉价路径：各自方向只解析首条消息的头部
        f->request = firstH1Head(f->rawWrite, true);
        f->response = firstH1Head(f->rawRead, false);
        break;
    case FlowKind::Http2:
        // 廉价路径：每侧首个 HEADERS 块（跳过 DATA 累积）
        f->request = firstH2Head(f->rawWrite);
        f->response = firstH2Head(f->rawRead);
        break;
    default:
        break;
    }
    if (f->exchanges.isEmpty() && (f->wbytes > 0 || f->rbytes > 0) && !f->request)
        f->rawPreview = rawPreviewText(f->rawWrite, f->rawRead);
}

} // namespace

// ---------------------------------------------------------------------------
// Reassembler

Reassembler::Reassembler() = default;

void Reassembler::setPidPackages(const QMap<quint32, QString>& map)
{
    pidPackage_ = map;
}

Flow Reassembler::blank(quint32 pid, const QString& comm, qint64 tsMs)
{
    Flow f;
    f.id = ++nextId_;
    f.firstTsMs = tsMs;
    f.lastTsMs = tsMs;
    f.pid = pid;
    f.comm = comm;
    f.package = pidPackage_.value(pid);
    f.kind = FlowKind::Unknown;
    return f;
}

QVector<Flow> Reassembler::feed(const Event& ev)
{
    QVector<Flow> done;
    switch (ev.type) {
    case Event::Type::Connect: {
        ++connectsTotal_;
        Flow b = blank(ev.pid, ev.comm, 0);
        auto it = live_.find(ev.pid);
        if (it == live_.end())
            it = live_.emplace(ev.pid, std::move(b)).first;
        Flow& flow = it->second;
        if (flow.src.isEmpty()) {
            flow.src = ev.src;
            flow.dst = ev.dst;
        }
        break;
    }
    case Event::Type::Closed: {
        for (auto& kv : live_) {
            finalizeFlow(&kv.second);
            done.push_back(std::move(kv.second));
        }
        live_.clear();
        break;
    }
    case Event::Type::Io: {
        Flow b = blank(ev.pid, ev.comm, ev.tsMs);
        auto it = live_.find(ev.pid);
        if (it == live_.end())
            it = live_.emplace(ev.pid, std::move(b)).first;
        Flow& flow = it->second;
        if (flow.comm.isEmpty())
            flow.comm = ev.comm;
        flow.lastTsMs = std::max(ev.tsMs, flow.lastTsMs);
        if (ev.direction == Direction::Write) {
            flow.wbytes += quint64(ev.data.size());
            if (flow.rawWrite.size() < rawCap_) {
                const qsizetype room = rawCap_ - flow.rawWrite.size();
                flow.rawWrite.append(ev.data.constData(),
                                     int(std::min<qsizetype>(ev.data.size(), room)));
            }
        } else {
            flow.rbytes += quint64(ev.data.size());
            if (flow.rawRead.size() < rawCap_) {
                const qsizetype room = rawCap_ - flow.rawRead.size();
                flow.rawRead.append(ev.data.constData(),
                                    int(std::min<qsizetype>(ev.data.size(), room)));
            }
        }
        break;
    }
    }
    return done;
}

QVector<Flow> Reassembler::flush()
{
    QVector<Flow> done;
    for (auto& kv : live_) {
        finalizeFlow(&kv.second);
        done.push_back(std::move(kv.second));
    }
    live_.clear();
    return done;
}

QVector<Flow> Reassembler::flushIdle(qint64 nowMs, qint64 maxIdleMs)
{
    QVector<Flow> done;
    for (auto it = live_.begin(); it != live_.end();) {
        if (nowMs - it->second.lastTsMs > maxIdleMs) {
            finalizeFlow(&it->second);
            done.push_back(std::move(it->second));
            it = live_.erase(it);
        } else {
            ++it;
        }
    }
    return done;
}

qsizetype Reassembler::liveCount() const
{
    return qsizetype(live_.size());
}

quint64 Reassembler::connectsTotal() const
{
    return connectsTotal_;
}

QVector<Flow> Reassembler::capLive(qsizetype maxLive)
{
    if (liveCount() <= maxLive)
        return {};
    return flush();
}

QVector<Flow> Reassembler::liveFlows() const
{
    QVector<Flow> out;
    for (const auto& kv : live_) {
        Flow f = kv.second; // 拷贝，不篡改状态
        finalizeFlow(&f);
        out.push_back(std::move(f));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 全量解析 / 摘要

QVector<ExchangeView> computeExchanges(const Flow& f)
{
    QVector<ExchangeView> out;
    const bool h2 = isH2(f.rawWrite) || isH2(f.rawRead);
    if (h2) {
        const SideMessages client = h2SideMessages(f.rawWrite);
        const SideMessages server = h2SideMessages(f.rawRead);
        std::set<quint32> streams;
        for (const auto& kv : client.map)
            streams.insert(kv.first);
        for (const auto& kv : server.map)
            streams.insert(kv.first);
        for (const quint32 sid : streams) {
            const HttpMessage* req = nullptr;
            const HttpMessage* resp = nullptr;
            const auto cit = client.map.find(sid);
            if (cit != client.map.end())
                req = cit->second.first.has_value() ? &*cit->second.first : nullptr;
            const auto sit = server.map.find(sid);
            if (sit != server.map.end())
                resp = sit->second.first.has_value() ? &*sit->second.first : nullptr;
            if (!req && !resp)
                continue;
            out.push_back(exchangeView(sid, req, resp));
        }
    } else {
        const QVector<HttpMessage> reqs = parseH1Messages(f.rawWrite);
        const QVector<HttpMessage> resps = parseH1Messages(f.rawRead);
        const qsizetype n = std::max(reqs.size(), resps.size());
        for (qsizetype i = 0; i < n; ++i) {
            out.push_back(exchangeView(0, i < reqs.size() ? &reqs.at(i) : nullptr,
                                       i < resps.size() ? &resps.at(i) : nullptr));
        }
    }
    return out;
}

FlowSummary FlowSummary::from(const Flow& f)
{
    // 摘要来源 = 定稿时的首消息（廉价路径），不再要求全量 exchanges
    FlowSummary s;
    s.id = f.id;
    s.firstTsMs = f.firstTsMs;
    s.lastTsMs = f.lastTsMs;
    s.pid = f.pid;
    s.comm = f.comm;
    s.package = f.package;
    s.src = f.src;
    s.dst = f.dst;
    s.kind = QLatin1String(flowKindStr(f.kind));
    s.host = f.request ? f.request->host().value_or(QString()) : QString();
    if (f.request) {
        const QStringList it = f.request->startLine.split(QLatin1Char(' '));
        s.method = it.isEmpty() ? QString() : it.at(0);
        s.path = it.size() > 1 ? it.at(1) : QString();
    }
    if (f.response) {
        const QStringList parts = f.response->startLine.split(QLatin1Char(' '));
        if (parts.size() > 1)
            s.status = quint16(parts.at(1).toUShort());
    }
    s.wbytes = f.wbytes;
    s.rbytes = f.rbytes;
    s.contentType = f.response
                        ? f.response->header(QStringLiteral("Content-Type")).value_or(QString())
                        : QString();
    s.exchanges = (f.request || f.response) ? 1 : 0;
    return s;
}

QJsonObject FlowSummary::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("first_ts_ms"), firstTsMs);
    o.insert(QStringLiteral("last_ts_ms"), lastTsMs);
    o.insert(QStringLiteral("pid"), qint64(pid));
    o.insert(QStringLiteral("comm"), comm);
    o.insert(QStringLiteral("package"), package);
    o.insert(QStringLiteral("src"), src);
    o.insert(QStringLiteral("dst"), dst);
    o.insert(QStringLiteral("kind"), kind);
    o.insert(QStringLiteral("host"), host);
    o.insert(QStringLiteral("method"), method);
    o.insert(QStringLiteral("path"), path);
    o.insert(QStringLiteral("status"), status);
    o.insert(QStringLiteral("wbytes"), qint64(wbytes));
    o.insert(QStringLiteral("rbytes"), qint64(rbytes));
    o.insert(QStringLiteral("content_type"), contentType);
    o.insert(QStringLiteral("exchanges"), exchanges);
    return o;
}

} // namespace mw::capture
