// mw_capture 自测 ← crates/mwine-capture 各模块 #[cfg(test)]
// 覆盖：事件流解析（长度分帧/流式安全）、HTTP/1 重组（Content-Length/分块/
// keep-alive 配对/半路接入/gzip/deflate body）、HTTP/2（前言/半路接入/动态表/
// Huffman/多流 DATA）、SQLite 建表+插入+查询 roundtrip（临时文件库）、
// Session 静态查询面、pcap/TLS 解密管线（keylog 解析、经典 pcap、链路层剥离、
// TCP 重组、OpenSSL 真实 TLS1.3/1.2 握手 roundtrip 解密、WebView 解密入库）。
// 不跑 eBPF/不拉起进程/不碰 ADB 设备。
#include "capture.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

using namespace mw::capture;

static int g_failed = 0;
static int g_total = 0;

template <typename T>
QString dbgVal(const T& v)
{
    if constexpr (std::is_enum_v<T>)
        return QString::number(int(v));
    else if constexpr (std::is_convertible_v<const T&, const QString&>)
        return v;
    else
        return QString::fromStdString(std::to_string(v));
}

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_total;                                                                   \
        if (!(cond)) {                                                               \
            ++g_failed;                                                              \
            qWarning().noquote()                                                     \
                << QStringLiteral("FAIL %1:%2 %3")                                   \
                       .arg(QStringLiteral(__FILE__), QString::number(__LINE__),     \
                            QStringLiteral(#cond));                                  \
        }                                                                            \
    } while (false)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        ++g_total;                                                                   \
        const auto va_ = (a); /* 按值持有：a 可能是临时 optional 的解引用 */            \
        const auto vb_ = (b);                                                        \
        if (!(va_ == vb_)) {                                                         \
            ++g_failed;                                                              \
            qWarning().noquote() << QStringLiteral("FAIL %1:%2 %3 == %4  实际 %5 vs %6") \
                                       .arg(QStringLiteral(__FILE__),                 \
                                           QString::number(__LINE__),                 \
                                           QStringLiteral(#a), QStringLiteral(#b),    \
                                           dbgVal(va_), dbgVal(vb_));                \
        }                                                                            \
    } while (false)

// ---------------------------------------------------------------------------
// 测试辅助

static Event connEvent(quint32 pid)
{
    return Event::makeConnect(pid, QStringLiteral("com.demo.app"), 42,
                              QStringLiteral("[::ffff:10.0.2.15]:51000"),
                              QStringLiteral("[::ffff:1.2.3.4]:443"));
}

static Event ioEvent(quint32 pid, Direction dir, const QByteArray& data)
{
    return Event::makeIo(QStringLiteral("2026-10-07 15:36:32.210"), 1791387392210, pid,
                         pid, QStringLiteral("main"), dir, data);
}

// 定稿 flow + 全量懒解析（等价详情打开时存储层行为）。
static Flow fullFlow(Flow f)
{
    f.exchanges = computeExchanges(f);
    return f;
}

// 两轮 keep-alive 的 flow（未懒解析：exchanges 为空）。带 pid→包名富化。
static Flow keepaliveFlow(qint64 id)
{
    Reassembler r;
    QMap<quint32, QString> pkg;
    pkg.insert(quint32(id), QStringLiteral("com.demo.app"));
    r.setPidPackages(pkg);
    r.feed(connEvent(quint32(id)));
    r.feed(ioEvent(quint32(id), Direction::Write,
                   "GET /a HTTP/1.1\r\nHost: api.demo.com\r\n\r\n"
                   "GET /b HTTP/1.1\r\nHost: api.demo.com\r\n\r\n"));
    r.feed(ioEvent(quint32(id), Direction::Read,
                   "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                   "Content-Length: 9\r\n\r\n[{\"a\":1}]"
                   "HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno"));
    QVector<Flow> done = r.flush();
    Flow f = done.first();
    f.id = id;
    return f;
}

static QByteArray gzipCompress(const QByteArray& in)
{
    z_stream zs {};
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = uInt(in.size());
    QByteArray out;
    char buf[65536];
    zs.next_out = reinterpret_cast<Bytef*>(buf);
    zs.avail_out = sizeof(buf);
    while (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
        out.append(buf, sizeof(buf) - zs.avail_out);
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
    }
    out.append(buf, sizeof(buf) - zs.avail_out);
    deflateEnd(&zs);
    return out;
}

static QByteArray zlibCompress(const QByteArray& in, int windowBits)
{
    z_stream zs {};
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8,
                 Z_DEFAULT_STRATEGY);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = uInt(in.size());
    QByteArray out;
    char buf[65536];
    zs.next_out = reinterpret_cast<Bytef*>(buf);
    zs.avail_out = sizeof(buf);
    while (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
        out.append(buf, sizeof(buf) - zs.avail_out);
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
    }
    out.append(buf, sizeof(buf) - zs.avail_out);
    deflateEnd(&zs);
    return out;
}

static void pushFrame(QByteArray* v, uint8_t kind, uint8_t flags, uint32_t stream,
                      const QByteArray& payload)
{
    v->append(char(uint8_t(payload.size() >> 16)));
    v->append(char(uint8_t(payload.size() >> 8)));
    v->append(char(uint8_t(payload.size())));
    v->append(char(kind));
    v->append(char(flags));
    v->append(char(uint8_t(stream >> 24)));
    v->append(char(uint8_t(stream >> 16)));
    v->append(char(uint8_t(stream >> 8)));
    v->append(char(uint8_t(stream)));
    v->append(payload);
}

// ---------------------------------------------------------------------------
// events：时间戳

static void testTimestamps()
{
    CHECK_EQ(*parseTsMs(QStringLiteral("1970-01-01 00:00:00.000")), qint64(0));
    CHECK_EQ(*parseTsMs(QStringLiteral("2026-10-07 15:36:32.247")), qint64(1791387392247));
    CHECK(!parseTsMs(QStringLiteral("garbage")).has_value());
}

// ---------------------------------------------------------------------------
// events：元数据/IO 解析 + 长度分帧不误同步

static QVector<Event> parseAll(const QByteArray& bytes)
{
    EventParser p;
    p.feed(bytes);
    return p.drain();
}

static void testEventStreamFraming()
{
    // 载荷内嵌 PID:/时间戳样式与 NUL 字节：长度分帧不误同步
    QByteArray payload;
    payload.append("PID:0, Comm:, TID:0, FD:0\n");
    payload.append("[2020-01-01 00:00:00.000] PID:1 TID:1 Comm:x FD:0 READ (2 bytes):\n");
    payload.append('\x00');
    payload.append('\xff');

    QByteArray stream;
    stream.append("[2026-10-07 15:36:32.210] PID:99 TID:99 Comm:t FD:0 WRITE (")
        .append(QByteArray::number(payload.size()))
        .append(" bytes):\n");
    stream.append(payload);
    stream.append('\n');
    stream.append("PID:1234, Comm:ok, TID:1234, FD:1, Tuple: [a]:1->[b]:2\n");

    const QVector<Event> evs = parseAll(stream);
    CHECK_EQ(int(evs.size()), 2);
    CHECK(evs.at(0).type == Event::Type::Io);
    CHECK_EQ(evs.at(0).pid, quint32(99));
    CHECK_EQ(evs.at(0).data, payload); // 载荷内嵌 PID:/时间戳样式不误同步
    CHECK(evs.at(1).type == Event::Type::Connect);
    CHECK_EQ(evs.at(1).pid, quint32(1234));
    CHECK(evs.at(1).comm == QLatin1String("ok"));

    // 流式安全：任意切块喂入，事件一致
    for (int chunk : { 1, 3, 7, 64, 300 }) {
        EventParser p;
        QVector<Event> got;
        for (int i = 0; i < stream.size(); i += chunk) {
            p.feed(stream.mid(i, chunk));
            got += p.drain();
        }
        CHECK_EQ(int(got.size()), 2);
        CHECK_EQ(got.at(0).data, payload);
        CHECK_EQ(got.at(0).pid, quint32(99));
    }
}

static void testEventStreamMetaAndClosed()
{
    QByteArray stream;
    stream.append("2026-10-07 INF some ecapture runtime log line\n"); // 噪声行忽略
    stream.append("PID:5, Comm:a, TID:5, FD:9, Tuple: [s]:1->[d]:2\n");
    stream.append("PID:0, Comm:, TID:0, FD:0, Tuple: [::]:0->[::]:0\n"); // 全零=Closed
    const QVector<Event> evs = parseAll(stream);
    CHECK_EQ(int(evs.size()), 2);
    CHECK(evs.at(0).type == Event::Type::Connect);
    CHECK(evs.at(0).src == QLatin1String("[s]:1"));
    CHECK(evs.at(0).dst == QLatin1String("[d]:2"));
    CHECK(evs.at(1).type == Event::Type::Closed);
}

// ---------------------------------------------------------------------------
// flow：HTTP/1 基础事务（Content-Length 配对）

static void testH1BasicTransaction()
{
    Reassembler r;
    r.feed(connEvent(7));
    r.feed(ioEvent(7, Direction::Write,
                   "GET /api/v1/x HTTP/1.1\r\nHost: api.demo.com\r\nUser-Agent: test\r\n"
                   "Accept: */*\r\n\r\n"));
    r.feed(ioEvent(7, Direction::Read,
                   "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                   "Content-Length: 4\r\n\r\nabcd"));
    QVector<Flow> done = r.feed(Event::makeClosed());
    CHECK_EQ(int(done.size()), 1);
    const Flow f = fullFlow(done.first());
    CHECK(f.kind == FlowKind::Http1);
    CHECK(f.src == QLatin1String("[::ffff:10.0.2.15]:51000"));
    CHECK_EQ(int(f.exchanges.size()), 1);
    const ExchangeView& e = f.exchanges.at(0);
    CHECK(e.method == QLatin1String("GET"));
    CHECK(e.host == QLatin1String("api.demo.com"));
    CHECK(e.path == QLatin1String("/api/v1/x"));
    CHECK_EQ(int(e.status), 200);
    CHECK(e.respBody && e.respBody->text == QLatin1String("abcd"));
    CHECK(e.reqHead.contains(QLatin1String("Host: api.demo.com")));

    const FlowSummary s = FlowSummary::from(f);
    CHECK(s.host == QLatin1String("api.demo.com"));
    CHECK(s.method == QLatin1String("GET"));
    CHECK_EQ(int(s.status), 200);
    CHECK(s.contentType == QLatin1String("application/json"));
    CHECK_EQ(s.exchanges, 1);

    // JSON 字段与 Rust serde 完全一致
    const QJsonObject j = s.toJson();
    const QStringList keys = j.keys();
    CHECK_EQ(int(keys.size()), 17);
    for (const QString& k : { QStringLiteral("id"), QStringLiteral("first_ts_ms"),
                              QStringLiteral("last_ts_ms"), QStringLiteral("pid"),
                              QStringLiteral("comm"), QStringLiteral("package"),
                              QStringLiteral("src"), QStringLiteral("dst"),
                              QStringLiteral("kind"), QStringLiteral("host"),
                              QStringLiteral("method"), QStringLiteral("path"),
                              QStringLiteral("status"), QStringLiteral("wbytes"),
                              QStringLiteral("rbytes"), QStringLiteral("content_type"),
                              QStringLiteral("exchanges") })
        CHECK(keys.contains(k));
    CHECK(j.value(QLatin1String("kind")).toString() == QLatin1String("http1"));
    // Rust parity：Connect 先到时以 ts=0 建 flow（first_ts_ms=0），Io 推进 last_ts_ms
    CHECK_EQ(j.value(QLatin1String("first_ts_ms")).toInteger(), qint64(0));
    CHECK_EQ(j.value(QLatin1String("last_ts_ms")).toInteger(), qint64(1791387392210));
}

// keep-alive：一条连接两组完整请求/响应 → 2 个 exchange。
static void testKeepaliveTwoRounds()
{
    QByteArray write("GET /a HTTP/1.1\r\nHost: h.cn\r\n\r\n"
                     "GET /b?x=1 HTTP/1.1\r\nHost: h.cn\r\n\r\n");
    QByteArray read("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nA"
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 1\r\n\r\nB");
    Reassembler r;
    r.feed(connEvent(3));
    r.feed(ioEvent(3, Direction::Write, write));
    r.feed(ioEvent(3, Direction::Read, read));
    const Flow f = fullFlow(r.flush().first());
    CHECK_EQ(int(f.exchanges.size()), 2);
    CHECK(f.exchanges.at(0).path == QLatin1String("/a"));
    CHECK_EQ(int(f.exchanges.at(0).status), 200);
    CHECK(f.exchanges.at(1).path == QLatin1String("/b?x=1"));
    CHECK_EQ(int(f.exchanges.at(1).status), 404);
    // 摘要路径的交互数为占位值（真实计数由存储层详情缓存维护）
    CHECK_EQ(FlowSummary::from(f).exchanges, 1);
}

// 分块编码：两个 chunked 响应背靠背 → 边界游走正确切出 2 个 exchange。
static void testChunkedTransferEncoding()
{
    Reassembler r;
    r.feed(connEvent(31));
    r.feed(ioEvent(31, Direction::Write,
                   "GET /c HTTP/1.1\r\nHost: chunk.cn\r\n\r\n"
                   "GET /d HTTP/1.1\r\nHost: chunk.cn\r\n\r\n"));
    r.feed(ioEvent(31, Direction::Read,
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"
                   "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "3\r\nabc\r\n0\r\n\r\n"));
    const Flow f = fullFlow(r.flush().first());
    CHECK_EQ(int(f.exchanges.size()), 2);
    CHECK_EQ(int(f.exchanges.at(0).status), 200);
    CHECK(f.exchanges.at(0).respBody->text.contains(QLatin1String("Wiki")));
    CHECK(f.exchanges.at(0).respBody->text.contains(QLatin1String("pedia")));
    CHECK_EQ(int(f.exchanges.at(1).status), 201);
    CHECK(f.exchanges.at(1).respBody->text.contains(QLatin1String("abc")));
}

// 半路接入：响应方向先是无头残片，后面跟着完整响应 → 跳过残片解析出后续。
static void testMidstreamResponseSalvaged()
{
    QByteArray read;
    read.append("\x1f\x8b\x08garbage-body-of-previous-download\n"); // 37B 残片（无内嵌 NUL）
    read.append("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-99/1000\r\n"
                "Content-Length: 3\r\n\r\nxyz");
    Reassembler r;
    r.feed(connEvent(4));
    r.feed(ioEvent(4, Direction::Write,
                   "GET /video HTTP/1.1\r\nHost: v.cn\r\nRange: bytes=1000-\r\n\r\n"));
    r.feed(ioEvent(4, Direction::Read, read));
    const Flow f = fullFlow(r.flush().first());
    CHECK(!f.exchanges.isEmpty());
    CHECK_EQ(int(f.exchanges.at(0).status), 206);
    CHECK(f.exchanges.at(0).respBody->text == QLatin1String("xyz"));
}

// 头部跨多个 IO 事件分片到达：累积重组后必须能解析出 Host。
static void testSplitHeaderEvents()
{
    Reassembler r;
    r.feed(connEvent(11));
    r.feed(ioEvent(11, Direction::Write, "GET /api/slow HTTP/1.1\r\nHost: api."));
    r.feed(ioEvent(11, Direction::Write, "example.com\r\nAccept: */*\r\n\r\n"));
    const Flow f = fullFlow(r.flush().first());
    CHECK(f.exchanges.at(0).host == QLatin1String("api.example.com"));
    CHECK(f.exchanges.at(0).path == QLatin1String("/api/slow"));
}

// gzip 响应体自动解压。
static void testGzipResponseDecoded()
{
    const QByteArray gz = gzipCompress("hello-gzip-content-123");
    QByteArray read;
    read.append("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: ")
        .append(QByteArray::number(gz.size()))
        .append("\r\n\r\n");
    read.append(gz);
    Reassembler r;
    r.feed(connEvent(5));
    r.feed(ioEvent(5, Direction::Write, "GET /gz HTTP/1.1\r\nHost: g.cn\r\n\r\n"));
    r.feed(ioEvent(5, Direction::Read, read));
    const Flow f = fullFlow(r.flush().first());
    const ExchangeView& e = f.exchanges.at(0);
    CHECK(e.respEncoding == QLatin1String("gzip"));
    CHECK(e.respBodyDecoded);
    CHECK(e.respBodyDecoded->text == QLatin1String("hello-gzip-content-123"));
    CHECK_EQ(int(e.respBodyDecoded->size), int(strlen("hello-gzip-content-123")));
}

// deflate（zlib 包装与 raw deflate 两种写法都应解出）。
static void testDeflateResponseDecoded()
{
    const char* body = "deflate-body-42";
    const QByteArray zl = zlibCompress(body, 15);
    QByteArray read;
    read.append("HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: ")
        .append(QByteArray::number(zl.size()))
        .append("\r\n\r\n");
    read.append(zl);
    Reassembler r;
    r.feed(connEvent(51));
    r.feed(ioEvent(51, Direction::Write, "POST /zl HTTP/1.1\r\nHost: z.cn\r\n\r\n"));
    r.feed(ioEvent(51, Direction::Read, read));
    const Flow f = fullFlow(r.flush().first());
    CHECK(f.exchanges.at(0).respBodyDecoded);
    CHECK(f.exchanges.at(0).respBodyDecoded->text == QLatin1String(body));

    // raw deflate（无 zlib 头，部分服务器的历史行为）
    const QByteArray raw = zlibCompress(body, -15);
    QByteArray read2;
    read2.append("HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: ")
        .append(QByteArray::number(raw.size()))
        .append("\r\n\r\n");
    read2.append(raw);
    Reassembler r2;
    r2.feed(connEvent(52));
    r2.feed(ioEvent(52, Direction::Write, "POST /rz HTTP/1.1\r\nHost: z.cn\r\n\r\n"));
    r2.feed(ioEvent(52, Direction::Read, read2));
    const Flow f2 = fullFlow(r2.flush().first());
    CHECK(f2.exchanges.at(0).respBodyDecoded);
    CHECK(f2.exchanges.at(0).respBodyDecoded->text == QLatin1String(body));
}

// ---------------------------------------------------------------------------
// flow：HTTP/2

static void testH2PrefaceMultiStream()
{
    QByteArray block;
    block.append(char(0x82)); // :method GET（静态索引）
    block.append(char(0x40)); // 字面量+增量索引 新名
    block.append(char(10));
    block.append(":authority");
    block.append(char(13));
    block.append("api.h2test.cn");
    block.append(char(0x87)); // :scheme https
    block.append(char(0x84)); // :path /

    QByteArray write("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
    pushFrame(&write, 0x4, 0, 0, QByteArray());          // SETTINGS
    pushFrame(&write, 0x1, 0x4, 1, block);               // HEADERS 流1 END_HEADERS
    pushFrame(&write, 0x0, 0x0, 1, QByteArray("abc"));   // DATA
    pushFrame(&write, 0x0, 0x1, 1, QByteArray("def"));   // DATA END_STREAM
    QByteArray read;
    pushFrame(&read, 0x1, 0x4, 1, QByteArray(1, char(0x88))); // :status 200
    pushFrame(&read, 0x0, 0x1, 1, QByteArray("ok!"));

    Reassembler r;
    r.feed(connEvent(21));
    r.feed(ioEvent(21, Direction::Write, write));
    r.feed(ioEvent(21, Direction::Read, read));
    const Flow f = fullFlow(r.flush().first());
    CHECK(f.kind == FlowKind::Http2);
    CHECK_EQ(int(f.exchanges.size()), 1);
    const ExchangeView& e = f.exchanges.at(0);
    CHECK(e.host == QLatin1String("api.h2test.cn"));
    CHECK(e.method == QLatin1String("GET"));
    CHECK(e.reqBody->text == QLatin1String("abcdef"));
    CHECK_EQ(int(e.status), 200);
    CHECK(e.respBody->text == QLatin1String("ok!"));
}

// H2 半路接入（无前言、裸帧）+ 动态表跨块：第二块引用第一块建立的动态表项。
static void testH2MidstreamDynamicTable()
{
    QByteArray blockA;
    blockA.append(char(0x82));
    blockA.append(char(0x40)); // 字面量+增量索引 新名（进动态表，索引 62）
    blockA.append(char(10));
    blockA.append("x-custom-h");
    blockA.append(char(5));
    blockA.append("aaa01");
    QByteArray blockB;
    blockB.append(char(0xbe)); // 引用动态表 62（0x40|62）
    blockB.append(char(0x88)); // :status 200

    QByteArray read;
    pushFrame(&read, 0x1, 0x4, 1, blockA);
    pushFrame(&read, 0x1, 0x4, 3, blockB);

    Reassembler r;
    r.feed(connEvent(22));
    r.feed(ioEvent(22, Direction::Read, read));
    const Flow f = fullFlow(r.flush().first());
    CHECK(f.kind == FlowKind::Http2); // 无前言也应识别为 H2
    const ExchangeView* e3 = nullptr;
    for (const ExchangeView& e : f.exchanges) {
        if (e.streamId == 3)
            e3 = &e;
    }
    CHECK(e3 != nullptr);
    CHECK_EQ(int(e3->status), 200);
    CHECK(e3->respHead.contains(QLatin1String("x-custom-h: aaa01"))); // 动态表项解出
}

// HPACK Huffman 字面量（RFC 7541 C.4.1 黄金样本）。
static void testHpackHuffman()
{
    hpack::Decoder dec;
    const QByteArray block = QByteArray::fromHex("8286" "84" "418cf1e3c2e5f23a6ba0ab90f4ff");
    QVector<QPair<QByteArray, QByteArray>> headers;
    CHECK(dec.decode(block, &headers));
    CHECK_EQ(int(headers.size()), 4);
    CHECK(headers.at(0).first == ":method" && headers.at(0).second == "GET");
    CHECK(headers.at(1).first == ":scheme" && headers.at(1).second == "http");
    CHECK(headers.at(2).first == ":path" && headers.at(2).second == "/");
    CHECK(headers.at(3).first == ":authority" && headers.at(3).second == "www.example.com");
}

// ---------------------------------------------------------------------------
// flow：明文/富化/live 快照

static void testPlaintextRawPreview()
{
    Reassembler r;
    QMap<quint32, QString> pkg;
    pkg.insert(9, QStringLiteral("com.private.proto"));
    r.setPidPackages(pkg);
    r.feed(connEvent(9));
    r.feed(ioEvent(9, Direction::Write, QByteArray("\x00\x01PROTO\x02\x03", 11)));
    const Flow f = r.flush().first();
    CHECK(f.kind == FlowKind::Plaintext);
    CHECK(f.rawPreview.contains(QLatin1String("PROTO")));
    CHECK(f.rawPreview.contains(QChar(0x00B7))); // ·
    CHECK(f.package == QLatin1String("com.private.proto"));
}

static void testLiveSnapshotWithoutMutation()
{
    Reassembler r;
    r.feed(connEvent(3));
    r.feed(ioEvent(3, Direction::Write, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
    const QVector<Flow> live = r.liveFlows();
    CHECK_EQ(int(live.size()), 1);
    CHECK(live.at(0).kind == FlowKind::Http1);
    CHECK_EQ(int(r.flush().size()), 1); // 快照不消费状态
}

// ---------------------------------------------------------------------------
// store：SQLite roundtrip（临时文件库）

static void testStoreRoundtrip()
{
    QTemporaryDir dir;
    const QString dbPath = QDir(dir.path()).filePath(QStringLiteral("flows.db"));
    auto opened = Store::open(dbPath);
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    Store s = std::move(opened.value());

    const Flow f = keepaliveFlow(1);
    CHECK_EQ(int(f.exchanges.size()), 0); // 定稿不 eagerly 解析
    CHECK(f.request && f.request->host() && *f.request->host() == QLatin1String("api.demo.com"));
    const auto up = s.upsertFlows(QVector<Flow> { f });
    CHECK(up.ok());
    CHECK_EQ(up.value(), 1);

    // 列表：摘要可用
    {
        FlowsQuery q;
        q.limit = 10;
        const auto list = s.querySummaries(q);
        CHECK(list.ok());
        CHECK_EQ(int(list.value().size()), 1);
        const FlowSummary& sum = list.value().at(0);
        CHECK(sum.host == QLatin1String("api.demo.com"));
        CHECK(sum.method == QLatin1String("GET"));
        CHECK(sum.path == QLatin1String("/a"));
        CHECK_EQ(int(sum.status), 200);
        CHECK(sum.contentType == QLatin1String("application/json"));
    }

    // 过滤器 + 计数
    {
        FlowsQuery q;
        q.q = QStringLiteral("api.demo.com");
        q.limit = 10;
        CHECK_EQ(int(s.querySummaries(q).value().size()), 1);
    }
    {
        FlowsQuery q;
        q.q = QStringLiteral("no"); // 正文关键词：轻表搜索不命中（预期行为）
        q.limit = 10;
        CHECK_EQ(int(s.querySummaries(q).value().size()), 0);
    }
    {
        FlowsQuery q;
        q.package = QStringLiteral("com.demo.app");
        CHECK_EQ(int(s.querySummaries(q).value().size()), 1);
        q.package = QStringLiteral("com.other");
        CHECK_EQ(int(s.querySummaries(q).value().size()), 0);
    }
    {
        FlowsQuery q;
        q.method = QStringLiteral("GET");
        q.kind = QStringLiteral("http1");
        q.status = 200;
        q.statusClass = 2;
        q.sinceMs = 0;
        CHECK_EQ(int(s.querySummaries(q).value().size()), 1);
        q.statusClass = 4;
        CHECK_EQ(int(s.querySummaries(q).value().size()), 0);
        CHECK_EQ(s.countFiltered(q).value(), quint64(0));
        CHECK_EQ(s.count().value(), quint64(1));
    }
    {
        FlowsQuery q;
        q.sinceMs = 1791387392210; // last_ts_ms == 自身 → 命中
        CHECK_EQ(s.countFiltered(q).value(), quint64(1));
        q.sinceMs = 1791387392211;
        CHECK_EQ(s.countFiltered(q).value(), quint64(0));
    }

    // 详情：懒解析出两轮 keep-alive
    const auto d = s.getDetail(1);
    CHECK(d.ok());
    const QJsonObject detail = d.value();
    CHECK(!detail.isEmpty());
    const QJsonArray ex = detail.value(QLatin1String("exchanges")).toArray();
    CHECK_EQ(int(ex.size()), 2);
    CHECK_EQ(ex.at(1).toObject().value(QLatin1String("status")).toInt(), 404);
    CHECK(ex.at(0).toObject().value(QLatin1String("resp_body")).toObject()
              .value(QLatin1String("text")).toString()
          == QLatin1String("[{\"a\":1}]"));
    CHECK(detail.value(QLatin1String("host")).toString() == QLatin1String("api.demo.com"));

    // 二次详情走缓存（parsed=1）
    const auto d2 = s.getDetail(1);
    CHECK(d2.ok());
    CHECK_EQ(int(d2.value().value(QLatin1String("exchanges")).toArray().size()), 2);
    {
        // 懒解析后交互计数回填
        FlowsQuery q;
        const auto list = s.querySummaries(q);
        CHECK_EQ(list.value().at(0).exchanges, 2);
    }

    // 未命中详情 → 空对象
    const auto miss = s.getDetail(999);
    CHECK(miss.ok());
    CHECK(miss.value().isEmpty());

    // kind 统计
    const auto stats = s.kindStats();
    CHECK(stats.ok());
    CHECK_EQ(stats.value().value(QLatin1String("total")).toInteger(), qint64(1));
    CHECK_EQ(stats.value().value(QLatin1String("parsed")).toInteger(), qint64(1));
    CHECK(stats.value().value(QLatin1String("kinds")).toObject()
              .value(QLatin1String("http1")).toObject()
              .value(QLatin1String("count")).toInteger()
          == 1);

    // clear
    const auto cleared = s.clear();
    CHECK(cleared.ok());
    CHECK_EQ(s.count().value(), quint64(0));
}

// upsert：同 id 再写 → 更新而不是重复行。
static void testStoreUpsertUpdates()
{
    auto opened = Store::openInMemory();
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    Store s = std::move(opened.value());
    Flow f = keepaliveFlow(7);
    f.wbytes = 100;
    s.upsertFlows(QVector<Flow> { f });
    Flow f2 = f;
    f2.wbytes = 250; // 同 id 更新 wbytes
    s.upsertFlows(QVector<Flow> { f2 });
    CHECK_EQ(s.count().value(), quint64(1));
    const auto list = s.querySummaries(FlowsQuery {});
    CHECK_EQ(list.value().at(0).wbytes, quint64(250));
}

// ---------------------------------------------------------------------------
// session：静态查询面（不拉起进程/eBPF）

static void testSessionIdleQueries()
{
    QTemporaryDir dir;
    Session s;
    s.setDbPath(QDir(dir.path()).filePath(QStringLiteral("flows.db")));

    const QJsonObject st = s.status();
    CHECK_EQ(st.value(QLatin1String("running")).toBool(), false);
    CHECK(st.contains(QLatin1String("serial")));
    CHECK(st.contains(QLatin1String("targets")));
    CHECK(st.contains(QLatin1String("events_parsed")));
    CHECK(st.contains(QLatin1String("bytes_seen")));
    CHECK(st.contains(QLatin1String("collector_lag_bytes")));
    CHECK(st.contains(QLatin1String("flows_live")));
    CHECK(st.contains(QLatin1String("flows_stored")));
    CHECK(st.contains(QLatin1String("db_path")));
    CHECK(st.contains(QLatin1String("last_error")));
    CHECK(st.contains(QLatin1String("guest_note")));
    CHECK(st.contains(QLatin1String("connections")));
    CHECK(st.contains(QLatin1String("empty_connect_comms")));
    CHECK(st.contains(QLatin1String("webview_keys_bytes")));
    CHECK(st.contains(QLatin1String("webview_pcap_bytes")));

    // 未运行时 stop 报错
    CHECK(!s.stop().ok());

    // 写入一条后经由 Session 查询
    {
        auto opened = Store::open(QDir(dir.path()).filePath(QStringLiteral("flows.db")));
        CHECK(opened.ok());
        if (opened.ok()) {
            Store st2 = std::move(opened.value());
            st2.upsertFlows(QVector<Flow> { keepaliveFlow(11) });
        }
    }
    const auto flows = s.queryFlows(FlowsQuery {});
    CHECK(flows.ok());
    CHECK_EQ(int(flows.value().size()), 1);
    CHECK(flows.value().at(0).value(QLatin1String("host")).toString()
          == QLatin1String("api.demo.com"));
    CHECK_EQ(s.totalFlows(FlowsQuery {}).value(), quint64(1));
    const auto detail = s.flowDetail(11);
    CHECK(detail.ok());
    CHECK_EQ(int(detail.value().value(QLatin1String("exchanges")).toArray().size()), 2);
    CHECK_EQ(s.liveFlows().size(), qsizetype(0));
    CHECK_EQ(s.stats().value(QLatin1String("total")).toInteger(), qint64(1));
    CHECK(s.clear().ok());
    CHECK_EQ(s.totalFlows(FlowsQuery {}).value(), quint64(0));
}

// ---------------------------------------------------------------------------
// 黄金样本端到端：真实 ecapture 采集样本 → 解析 → 重组 → 摘要（不碰进程/SQLite）
// （对应 Rust lib.rs golden_sample_end_to_end + events.rs fixture_* 测试）
static void testGoldenSampleEndToEnd()
{
    QFile f(QFileInfo(QStringLiteral(__FILE__)).dir().filePath(
        QStringLiteral("ecap-baidu-sample.log")));
    CHECK(f.open(QIODevice::ReadOnly));
    if (!f.isOpen())
        return;
    const QByteArray raw = f.readAll();

    EventParser p;
    p.feed(raw);
    const QVector<Event> events = p.drain();

    int connects = 0, closed = 0, ios = 0;
    const Event* firstIo = nullptr;
    for (const Event& e : events) {
        if (e.type == Event::Type::Connect)
            ++connects;
        else if (e.type == Event::Type::Closed)
            ++closed;
        else {
            ++ios;
            if (!firstIo)
                firstIo = &e;
        }
    }
    CHECK_EQ(connects, 3); // 3 条连接元数据行
    CHECK(closed >= 2);
    CHECK_EQ(ios, 2); // WRITE + READ 各一条
    if (firstIo) {
        CHECK_EQ(int(firstIo->data.size()), 103);
        CHECK(firstIo->data.startsWith("GET / HTTP/1.1"));
        CHECK(QString::fromUtf8(firstIo->data).contains(QLatin1String("mwine-tlsprobe/1.0")));
        CHECK_EQ(firstIo->tsMs, qint64(1791387392210)); // 按 UTC 解析
        CHECK(firstIo->ts == QLatin1String("2026-10-07 15:36:32.210"));
    }

    Reassembler r;
    QVector<Flow> done;
    for (const Event& ev : events)
        done += r.feed(ev);
    done += r.flush(); // 样本最后有全零行，应定稿出 flow

    const Flow* baidu = nullptr;
    for (const Flow& fl : done) {
        if (fl.request && fl.request->host()
            && fl.request->host()->contains(QLatin1String("baidu.com")))
            baidu = &fl;
    }
    CHECK(baidu != nullptr);
    if (!baidu)
        return;
    CHECK(baidu->kind == FlowKind::Http1);
    CHECK(baidu->dst.contains(QLatin1String("183.2.172.177")));
    CHECK(baidu->request->startLine == QLatin1String("GET / HTTP/1.1"));
    CHECK(baidu->response->startLine == QLatin1String("HTTP/1.1 200 OK"));
    CHECK_EQ(baidu->wbytes, quint64(103));
    CHECK_EQ(baidu->rbytes, quint64(1179));

    const FlowSummary s = FlowSummary::from(*baidu);
    CHECK(s.host == QLatin1String("www.baidu.com"));
    CHECK(s.method == QLatin1String("GET"));
    CHECK_EQ(int(s.status), 200);
    CHECK(s.contentType.startsWith(QLatin1String("text/html")));

    // 详情懒解析（黄金样本过 Store 管线）
    auto opened = Store::openInMemory();
    CHECK(opened.ok());
    if (!opened.ok())
        return;
    Store st = std::move(opened.value());
    CHECK(st.upsertFlows(QVector<Flow> { *baidu }).ok());
    const auto d = st.getDetail(baidu->id);
    CHECK(d.ok());
    const QJsonArray ex = d.value().value(QLatin1String("exchanges")).toArray();
    CHECK_EQ(int(ex.size()), 1);
    CHECK(ex.at(0).toObject().value(QLatin1String("host")).toString()
          == QLatin1String("www.baidu.com"));
    CHECK(ex.at(0).toObject().value(QLatin1String("resp_body")).toObject()
              .value(QLatin1String("size")).toInteger()
          >= 0);
}

// ---------------------------------------------------------------------------
// pcap_decrypt：keylog / pcap 解析 / 链路层 / TCP 重组 / TLS 解密 roundtrip
// （对应 Rust pcap_decrypt.rs #[cfg(test)]；rustls 真实握手等价换用 OpenSSL
//   memory-BIO 握手 + keylog 回调）

static QByteArray repeatHex(char c, int n)
{
    QByteArray hex;
    for (int i = 0; i < n; ++i)
        hex.append(char('0' + (i % 10)));
    return hex;
}

static void testKeylogParse()
{
    // CLIENT_RANDOM：32B id + 48B secret
    const QString cr = QStringLiteral("CLIENT_RANDOM %1 %2").arg(repeatHex('a', 64), repeatHex('b', 96));
    // TLS1.3 secret：64B id + 32B secret
    const QString s3 = QStringLiteral("CLIENT_HANDSHAKE_TRAFFIC_SECRET %1 %2")
                           .arg(repeatHex('c', 128), repeatHex('d', 64));
    const QString text = QStringLiteral("# comment line\n"
                                        "%1\n"
                                        "%2\n"
                                        "%3\n"
                                        "\n"
                                        "garbage\n"
                                        "ODDHEX z z\n")
                             .arg(cr, s3,
                                  QStringLiteral("CLIENT_RANDOM %1 %2")
                                      .arg(repeatHex('e', 64), repeatHex('f', 20))); // secret 长度错 → 落 secrets
    const KeyLog kl = KeyLog::parse(text);
    CHECK_EQ(int(kl.clientRandom.size()), 1);
    CHECK_EQ(int(kl.secrets.size()), 2); // TLS1.3 label + 长度不符的 CLIENT_RANDOM
    for (auto it = kl.clientRandom.cbegin(); it != kl.clientRandom.cend(); ++it) {
        CHECK_EQ(int(it.key().size()), 32);
        CHECK_EQ(int(it.value().size()), 48);
    }
    CHECK_EQ(int(kl.secrets.value(QLatin1String("CLIENT_HANDSHAKE_TRAFFIC_SECRET")).size()), 1);

    // Rust 语义：CLIENT_RANDOM 但 id/secret 长度不符 → 归入 secrets
    CHECK(kl.secrets.contains(QLatin1String("CLIENT_RANDOM")));

    // 空文本/纯噪声
    CHECK(KeyLog::parse(QString()).clientRandom.isEmpty());
    CHECK(KeyLog::parse(QStringLiteral("a b c d e")).secrets.isEmpty());
}

// 合成经典 pcap：链路层 + IPv4/TCP 头 + 载荷。
static QByteArray makeEthTcpPacket(quint32 link, const QByteArray& payload, quint16 sport,
                                   quint16 dport, quint32 seq, quint8 flags)
{
    QByteArray pkt;
    if (link == 1) {
        pkt = QByteArray(14, '\0');
        pkt[12] = char(0x08);
    } else if (link == 113) {
        pkt = QByteArray(16, '\0');
        pkt[14] = char(0x08);
    } else {
        pkt = QByteArray(20, '\0');
        pkt[0] = char(0x08);
    }
    QByteArray ip(20, '\0');
    ip[0] = char(0x45);
    ip[8] = char(64);
    ip[9] = char(6);
    const unsigned char src[4] = { 10, 0, 2, 15 };
    const unsigned char dst[4] = { 93, 184, 216, 34 };
    memcpy(ip.data() + 12, src, 4);
    memcpy(ip.data() + 16, dst, 4);
    QByteArray tcp(20, '\0');
    qToBigEndian(sport, tcp.data());
    qToBigEndian(dport, tcp.data() + 2);
    qToBigEndian(seq, tcp.data() + 4);
    tcp[12] = char(0x50);
    tcp[13] = char(flags);
    tcp += payload;
    const quint32 total = quint32(20 + tcp.size());
    qToBigEndian(quint16(total), ip.data() + 2);
    return pkt + ip + tcp;
}

static QByteArray wrapPcap(quint32 link, bool le, const QVector<QByteArray>& frames)
{
    QByteArray out;
    const unsigned char magic[4] = { 0xd4, 0xc3, 0xb2, 0xa1 };
    const unsigned char magicBe[4] = { 0xa1, 0xb2, 0xc3, 0xd4 };
    out.append(reinterpret_cast<const char*>(le ? magic : magicBe), 4);
    QByteArray gh(20, '\0');
    gh[0] = char(2);
    gh[2] = char(4); // version 2.4
    gh[16] = char(0xff);
    gh[17] = char(0xff);
    gh[18] = char(0x01);
    qToLittleEndian<quint32>(link, gh.data() + 20 - 4); // 写 network 字段（按字节序统一处理）
    if (!le) {
        // BE：network 字段按大端重写
        gh.truncate(16);
        char nb[4];
        qToBigEndian<quint32>(link, nb);
        gh.append(nb, 4);
    }
    out += gh;
    for (int i = 0; i < frames.size(); ++i) {
        QByteArray rh(16, '\0');
        qToLittleEndian<quint32>(quint32(1000 + i), rh.data());
        qToLittleEndian<quint32>(quint32(10), rh.data() + 4);
        qToLittleEndian<quint32>(quint32(frames.at(i).size()), rh.data() + 8);
        if (!le) {
            rh.clear();
            char b[16];
            qToBigEndian<quint32>(quint32(1000 + i), b);
            qToBigEndian<quint32>(quint32(10), b + 4);
            qToBigEndian<quint32>(quint32(frames.at(i).size()), b + 8);
            qToBigEndian<quint32>(quint32(0), b + 12);
            rh.append(b, 16);
        }
        out += rh;
        out += frames.at(i);
    }
    return out;
}

static void testPcapClassicParse()
{
    const QByteArray payload("ABCD");
    const QByteArray pkt = makeEthTcpPacket(1, payload, 51000, 443, 1000, 0x18);

    // LE magic
    {
        const auto f = readPcapLink(wrapPcap(1, true, { pkt }));
        CHECK(f.ok());
        if (f.ok()) {
            CHECK(f.value().link.kind == LinkType::Ethernet);
            CHECK_EQ(int(f.value().packets.size()), 1);
            CHECK_EQ(f.value().packets.at(0).tsUs, quint64(1000) * 1000000ull + 10);
            CHECK(f.value().packets.at(0).data == pkt);
        }
    }
    // BE magic
    {
        const auto f = readPcapLink(wrapPcap(1, false, { pkt, pkt }));
        CHECK(f.ok());
        if (f.ok())
            CHECK_EQ(int(f.value().packets.size()), 2);
    }
    // SLL / SLL2 / 未知链路层
    {
        const auto sll = readPcapLink(wrapPcap(113, true, { makeEthTcpPacket(113, payload, 1, 2, 0, 0x18) }));
        CHECK(sll.ok());
        if (sll.ok())
            CHECK(sll.value().link.kind == LinkType::LinuxSll);
        const auto sll2 = readPcapLink(wrapPcap(276, true, { makeEthTcpPacket(276, payload, 1, 2, 0, 0x18) }));
        CHECK(sll2.ok());
        if (sll2.ok())
            CHECK(sll2.value().link.kind == LinkType::LinuxSll2);
        const auto other = readPcapLink(wrapPcap(147, true, { QByteArray(30, 'x') }));
        CHECK(other.ok());
        if (other.ok()) {
            CHECK(other.value().link.kind == LinkType::Other);
            CHECK_EQ(other.value().link.raw, quint32(147));
        }
    }
    // 非法输入
    CHECK(!readPcapLink(QByteArray(10, 'x')).ok());       // 太短
    CHECK(!readPcapLink(QByteArray("PCAP-garbage-not-a-pcap-file-at-all")).ok()); // 非法 magic
    // 截断的记录头：已有包保留
    {
        QByteArray pcap = wrapPcap(1, true, { pkt });
        pcap.append(QByteArray(8, '\0')); // 半条记录头
        const auto f = readPcap(pcap);
        CHECK(f.ok());
        if (f.ok())
            CHECK_EQ(int(f.value().size()), 1);
    }
}

static void testPcapLinkLayerExtract()
{
    const QByteArray payload("GET /x HTTP/1.1");
    QVector<QByteArray> frames;
    frames.push_back(makeEthTcpPacket(1, QByteArray(), 51000, 443, 999, 0x02)); // SYN 无载荷
    frames.push_back(makeEthTcpPacket(1, payload, 51000, 443, 1000, 0x18)); // PSH|ACK
    const auto f = readPcapLink(wrapPcap(1, true, frames));
    CHECK(f.ok());
    if (!f.ok())
        return;
    const QVector<TcpSeg> segs = extractTcpSegs(f.value().packets, f.value().link);
    CHECK_EQ(int(segs.size()), 2);
    if (segs.size() == 2) {
        CHECK_EQ(segs.at(0).src.ip, quint32(0x0a00020f)); // 10.0.2.15
        CHECK_EQ(segs.at(0).src.port, quint16(51000));
        CHECK_EQ(segs.at(0).dst.port, quint16(443));
        CHECK_EQ(segs.at(0).seq, quint32(999));
        CHECK(segs.at(0).syn);
        CHECK(segs.at(0).payload.isEmpty()); // SYN 无载荷
        CHECK(!segs.at(1).syn);
        CHECK(segs.at(1).payload == payload);
        CHECK(ipToString(0x5db8d822) == QLatin1String("93.184.216.34"));
    }
    // SLL/SLL2 剥离等价
    {
        const auto sll = readPcapLink(wrapPcap(113, true, { makeEthTcpPacket(113, payload, 1, 2, 0, 0x18) }));
        CHECK(sll.ok());
        if (sll.ok())
            CHECK_EQ(int(extractTcpSegs(sll.value().packets, sll.value().link).size()), 1);
        const auto sll2 = readPcapLink(wrapPcap(276, true, { makeEthTcpPacket(276, payload, 1, 2, 0, 0x18) }));
        CHECK(sll2.ok());
        if (sll2.ok())
            CHECK_EQ(int(extractTcpSegs(sll2.value().packets, sll2.value().link).size()), 1);
    }
    // 非 IPv4 / 非 TCP 丢弃
    {
        QByteArray ip6 = makeEthTcpPacket(1, payload, 1, 2, 0, 0x18);
        ip6[14] = char(0x60); // version 6
        const auto bad = readPcapLink(wrapPcap(1, true, { ip6 }));
        CHECK(bad.ok());
        if (bad.ok())
            CHECK(extractTcpSegs(bad.value().packets, bad.value().link).isEmpty());
    }
}

static TcpSeg mkSeg(quint32 seq, const QByteArray& payload, bool syn = false,
                    quint16 sport = 1000, quint16 dport = 80)
{
    TcpSeg s;
    s.tsUs = 1000;
    s.src = IpEndpoint { 0x0a00020f, sport };
    s.dst = IpEndpoint { 0x5db8d822, dport };
    s.seq = seq;
    s.syn = syn;
    s.payload = payload;
    return s;
}

static void testTcpStreamBufReassembly()
{
    // SYN 定基 + 乱序 + 同偏移长者胜 + 空洞截断
    TcpStreamBuf b;
    b.push(mkSeg(999, QByteArray(), true)); // SYN → base 1000
    b.push(mkSeg(1000, "AAAA"));
    b.push(mkSeg(1008, "CCCC")); // 乱序先到
    b.push(mkSeg(1004, "BBBB"));
    CHECK(b.assemble() == QByteArray("AAAABBBBCCCC"));
    b.push(mkSeg(1008, "XXXXX")); // 同偏移长者胜（5 > 4）
    CHECK(b.assemble() == QByteArray("AAAABBBBXXXXX"));
    b.push(mkSeg(1013, "Y")); // XXXXX 覆盖 8..13，Y 接续
    CHECK(b.assemble() == QByteArray("AAAABBBBXXXXXY"));

    TcpStreamBuf hole;
    hole.push(mkSeg(0, "AB"));
    hole.push(mkSeg(6, "GH")); // 2..6 空洞
    CHECK(hole.assemble() == QByteArray("AB"));

    // 覆盖重叠段（后段伸入已拼区间：保留其超出部分）
    TcpStreamBuf overlap;
    overlap.push(mkSeg(0, "AAAAAA"));
    overlap.push(mkSeg(4, "bbbb"));
    CHECK(overlap.assemble() == QByteArray("AAAAAAbb"));

    // 无 SYN：首见定基
    TcpStreamBuf nosyn;
    nosyn.push(mkSeg(500, "xx"));
    nosyn.push(mkSeg(502, "yy"));
    CHECK(nosyn.assemble() == QByteArray("xxyy"));
}

static void testAssembleStreamsDirections()
{
    // 两条连接：4 元组各归各；方向按规范化端点拆 a2b/b2a
    const auto revSeg = [](TcpSeg s) {
        const IpEndpoint t = s.src;
        s.src = s.dst;
        s.dst = t;
        return s;
    };
    QVector<TcpSeg> segs;
    segs.push_back(mkSeg(0, "req", false, 51000, 443));
    segs.push_back(revSeg(mkSeg(0, "resp", false, 51000, 443))); // 反向：IP+端口都翻转
    segs.push_back(mkSeg(0, "r2", false, 51001, 443));
    segs.push_back(revSeg(mkSeg(0, "s2", false, 51001, 443)));
    const QVector<TcpConn> conns = assembleStreams(segs);
    CHECK_EQ(int(conns.size()), 2);
    if (conns.size() == 2) {
        // 规范化：小端点（443 服务端）在前 → a=server
        const TcpConn& c = conns.at(0);
        CHECK_EQ(c.a.port, quint16(443));
        CHECK_EQ(c.b.port, quint16(51000));
        CHECK(c.a2b.assemble() == QByteArray("resp")); // server→client
        CHECK(c.b2a.assemble() == QByteArray("req"));  // client→server
        const TcpConn& c2 = conns.at(1);
        CHECK_EQ(c2.b.port, quint16(51001));
        CHECK(c2.b2a.assemble() == QByteArray("r2"));
        CHECK(c2.a2b.assemble() == QByteArray("s2"));
    }
}

static void testSplitRecordsAndRobustness()
{
    // 记录切分：完整两条 + 尾部半条忽略
    QByteArray buf;
    const auto pushRec = [&buf](quint8 typ, const QByteArray& p) {
        buf.append(char(typ));
        buf.append(char(0x03));
        buf.append(char(0x03));
        buf.append(char(uint8_t(p.size() >> 8)));
        buf.append(char(uint8_t(p.size())));
        buf += p;
    };
    pushRec(22, "hello-hs");
    pushRec(23, "app-data");
    buf.append(char(23));
    buf.append(char(0x03));
    const QVector<QPair<quint8, QByteArray>> recs = splitRecords(buf);
    CHECK_EQ(int(recs.size()), 2);
    CHECK_EQ(recs.at(0).first, quint8(22));
    CHECK(recs.at(0).second == QByteArray("hello-hs"));
    CHECK_EQ(recs.at(1).first, quint8(23));
    CHECK(recs.at(1).second == QByteArray("app-data"));

    // decryptPcap 对空/垃圾输入静默返回空
    CHECK(decryptPcap(QByteArray(), QString()).isEmpty());
    CHECK(decryptPcap(QByteArray("not a pcap at all"), QString()).isEmpty());
    CHECK(decryptPcap(wrapPcap(1, true, { makeEthTcpPacket(1, "junk", 1, 2, 0, 0x18) }),
                      QStringLiteral("CLIENT_TRAFFIC_SECRET_0 aa bb"))
              .isEmpty());
    // 无密钥：解不出
    CHECK(!decryptDirection(QByteArray("\x17\x03\x03\x00\x05" "abcde", 10),
                            KeyLog::parse(QString()), true)
               .has_value());
}

// ---------------------------------------------------------------------------
// OpenSSL memory-BIO TLS 端点对：真实握手（keylog 回调收 NSS 密钥），
// 方向字节 tee 成 c2s/s2c → 合成 pcap → 全管线解密（等价 Rust rustls 版自测）。

static bool makeSelfSignedCert(SSL_CTX* ctx)
{
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* kp = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!kp)
        return false;
    bool ok = EVP_PKEY_keygen_init(kp) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(kp, 2048) == 1
              && EVP_PKEY_keygen(kp, &pkey) == 1;
    EVP_PKEY_CTX_free(kp);
    if (!ok || !pkey)
        return false;
    X509* x = X509_new();
    if (!x) {
        EVP_PKEY_free(pkey);
        return false;
    }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_NAME* n = X509_NAME_new();
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("test.local"), -1, -1, 0);
    X509_set_subject_name(x, n);
    X509_set_issuer_name(x, n);
    X509_NAME_free(n);
    X509_set_pubkey(x, pkey);
    ok = X509_sign(x, pkey, EVP_sha256()) > 0 && SSL_CTX_use_certificate(ctx, x) == 1
         && SSL_CTX_use_PrivateKey(ctx, pkey) == 1;
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ok;
}

struct MemBioTls {
    SSL_CTX* cctx = nullptr;
    SSL_CTX* sctx = nullptr;
    SSL* cli = nullptr;
    SSL* srv = nullptr;
    BIO* cRbio = nullptr; // client 读侧（向其写入 server 字节）
    BIO* sRbio = nullptr; // server 读侧（向其写入 client 字节）
    QString keylog;
    QVector<QByteArray> c2s, s2c;

    static void keylogCb(const SSL* ssl, const char* line)
    {
        void* p = SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
        if (p)
            *static_cast<QString*>(p) += QString::fromLatin1(line) + QLatin1Char('\n');
    }

    bool init(bool tls12, const char* cipherList)
    {
        cctx = SSL_CTX_new(TLS_method());
        sctx = SSL_CTX_new(TLS_method());
        if (!cctx || !sctx)
            return false;
        if (tls12) {
            SSL_CTX_set_min_proto_version(cctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(cctx, TLS1_2_VERSION);
            SSL_CTX_set_min_proto_version(sctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(sctx, TLS1_2_VERSION);
            if (cipherList && SSL_CTX_set_cipher_list(cctx, cipherList) != 1)
                return false;
        }
        SSL_CTX_set_app_data(cctx, &keylog);
        SSL_CTX_set_app_data(sctx, &keylog);
        SSL_CTX_set_keylog_callback(cctx, keylogCb);
        SSL_CTX_set_keylog_callback(sctx, keylogCb);
        if (!makeSelfSignedCert(sctx))
            return false;
        cli = SSL_new(cctx);
        srv = SSL_new(sctx);
        if (!cli || !srv)
            return false;
        BIO* cW = BIO_new(BIO_s_mem());
        cRbio = BIO_new(BIO_s_mem());
        BIO* sW = BIO_new(BIO_s_mem());
        sRbio = BIO_new(BIO_s_mem());
        SSL_set_bio(cli, cRbio, cW); // SSL 接管 BIO 生命周期
        SSL_set_bio(srv, sRbio, sW);
        SSL_set_connect_state(cli);
        SSL_set_accept_state(srv);
        return true;
    }

    // 把两侧写缓冲搬到对端读缓冲，同时 tee 进方向日志
    void drain()
    {
        char buf[16384];
        int n;
        while ((n = BIO_read(SSL_get_wbio(cli), buf, sizeof buf)) > 0) {
            c2s.push_back(QByteArray(buf, n));
            BIO_write(sRbio, buf, n);
        }
        while ((n = BIO_read(SSL_get_wbio(srv), buf, sizeof buf)) > 0) {
            s2c.push_back(QByteArray(buf, n));
            BIO_write(cRbio, buf, n);
        }
    }

    bool handshake()
    {
        for (int i = 0; i < 200; ++i) {
            if (SSL_is_init_finished(cli) && SSL_is_init_finished(srv))
                break;
            if (!SSL_is_init_finished(cli))
                SSL_do_handshake(cli);
            drain();
            if (!SSL_is_init_finished(srv))
                SSL_do_handshake(srv);
            drain();
        }
        drain(); // TLS1.3 NewSessionTicket 尾随字节
        return SSL_is_init_finished(cli) && SSL_is_init_finished(srv);
    }

    bool exchange(const QByteArray& req, const QByteArray& resp)
    {
        if (SSL_write(cli, req.constData(), int(req.size())) <= 0)
            return false;
        drain();
        QByteArray got;
        char buf[4096];
        while (got.size() < req.size()) {
            const int n = SSL_read(srv, buf, sizeof buf);
            if (n <= 0)
                break;
            got.append(buf, n);
        }
        if (got != req)
            return false;
        if (SSL_write(srv, resp.constData(), int(resp.size())) <= 0)
            return false;
        drain();
        QByteArray got2;
        while (got2.size() < resp.size()) {
            const int n = SSL_read(cli, buf, sizeof buf);
            if (n <= 0)
                break;
            got2.append(buf, n);
        }
        SSL_shutdown(cli); // close_notify（加密 alert 记录，也应被管线容忍）
        drain();
        return got2 == resp;
    }

    QByteArray concat(const QVector<QByteArray>& v) const
    {
        QByteArray out;
        for (const QByteArray& b : v)
            out += b;
        return out;
    }

    ~MemBioTls()
    {
        SSL_free(cli);
        SSL_free(srv);
        SSL_CTX_free(cctx);
        SSL_CTX_free(sctx);
    }
};

// 把方向字节包成 eth/ip/tcp 段合成经典 pcap（任意分块，喂重组器）。
// 字段合成与 Rust build_pcap 一致（SYN 定基、分块独立 seq 递增）。
static void pcapPush(QByteArray* out, quint64 ts, quint16 sport, quint16 dport, quint32 seq,
                     const QByteArray& payload, bool toServer)
{
    const quint32 total = quint32(14 + 20 + 20 + payload.size());
    QByteArray eth(14, '\0');
    eth[12] = char(0x08);
    QByteArray ip(20, '\0');
    ip[0] = char(0x45);
    qToBigEndian(quint16(total), ip.data() + 2);
    ip[8] = char(64);
    ip[9] = char(6);
    const unsigned char cip[4] = { 10, 0, 2, 15 };
    const unsigned char sip[4] = { 93, 184, 216, 34 };
    memcpy(ip.data() + 12, toServer ? cip : sip, 4);
    memcpy(ip.data() + 16, toServer ? sip : cip, 4);
    QByteArray tcp(20, '\0');
    qToBigEndian(sport, tcp.data());
    qToBigEndian(dport, tcp.data() + 2);
    qToBigEndian(seq, tcp.data() + 4);
    tcp[12] = char(0x50);
    tcp[13] = ((payload.isEmpty() && seq == 999) || seq == 4999) ? char(0x02) : char(0x18);
    tcp += payload;
    eth += ip;
    eth += tcp;
    char rh[16];
    qToLittleEndian<quint32>(quint32(ts), rh);
    qToLittleEndian<quint32>(quint32(ts / 1000), rh + 4);
    qToLittleEndian<quint32>(quint32(eth.size()), rh + 8);
    qToLittleEndian<quint32>(quint32(0), rh + 12);
    out->append(rh, 16);
    out->append(eth);
}

static QByteArray buildPcap(const QVector<QByteArray>& c2s, const QVector<QByteArray>& s2c)
{
    QByteArray out;
    const unsigned char hdr[24] = { 0xd4, 0xc3, 0xb2, 0xa1, 2, 0, 4, 0, 0, 0, 0, 0,
                                    0,    0,    0,    0,    0xff, 0xff, 1, 0, 1, 0, 0, 0 };
    out.append(reinterpret_cast<const char*>(hdr), 24);
    quint32 sc = 1000, ss = 5000;
    quint64 ts = 1000;
    pcapPush(&out, ts, 51000, 443, sc - 1, QByteArray(), true);  // SYN c→s
    pcapPush(&out, ts, 443, 51000, ss - 1, QByteArray(), false); // SYN s→c
    for (const QByteArray& c : c2s) {
        pcapPush(&out, ts, 51000, 443, sc, c, true);
        sc += quint32(c.size());
        ts += 10;
    }
    for (const QByteArray& c : s2c) {
        pcapPush(&out, ts, 443, 51000, ss, c, false);
        ss += quint32(c.size());
        ts += 10;
    }
    return out;
}

// TLS1.3 真实握手端到端：tee 方向字节 → 合成 pcap → 全管线解密。
static void testTls13RoundtripDecrypt()
{
    MemBioTls t;
    CHECK(t.init(false, nullptr));
    if (!t.cli || !t.srv)
        return;
    CHECK(t.handshake());
    const QByteArray req = "GET /api/v1/hello HTTP/1.1\r\nHost: test.local\r\n\r\n";
    const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    CHECK(t.exchange(req, resp));
    CHECK(t.keylog.contains(QLatin1String("CLIENT_HANDSHAKE_TRAFFIC_SECRET")));
    CHECK(t.keylog.contains(QLatin1String("SERVER_TRAFFIC_SECRET_0")));

    const QByteArray pcap = buildPcap(t.c2s, t.s2c);
    const QVector<DecryptedStream> streams = decryptPcap(pcap, t.keylog);
    CHECK_EQ(int(streams.size()), 1); // 应恰好解出一条连接
    if (!streams.isEmpty()) {
        const DecryptedStream& s = streams.first();
        CHECK(s.clientPlain.contains(req)); // 客户端明文含 HTTP 请求
        CHECK(s.serverPlain.contains("HTTP/1.1 200 OK"));
        CHECK(s.serverPlain.contains("hello")); // 服务端明文含响应体
        CHECK_EQ(s.client.port, quint16(51000));
        CHECK_EQ(s.server.port, quint16(443));
        CHECK(ipToString(s.client.ip) == QLatin1String("10.0.2.15"));
        CHECK(ipToString(s.server.ip) == QLatin1String("93.184.216.34"));
        // content-type 过滤：握手残片（Finished/NST）不应出现在明文头部
        CHECK(s.clientPlain.startsWith("GET /"));
        CHECK(s.serverPlain.startsWith("HTTP/1.1 200 OK"));
    }
}

// TLS1.2 真实握手端到端（ECDHE-RSA-AES128-GCM-SHA256 + CLIENT_RANDOM keylog）。
// （Rust 侧 rustls 构造差异被 ignore 的用例；OpenSSL 为教科书构造，直接过）
static void testTls12RoundtripDecrypt()
{
    MemBioTls t;
    CHECK(t.init(true, "ECDHE-RSA-AES128-GCM-SHA256"));
    if (!t.cli || !t.srv)
        return;
    CHECK(t.handshake());
    const QByteArray req = "GET /t12 HTTP/1.1\r\nHost: test.local\r\n\r\n";
    const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
    CHECK(t.exchange(req, resp));
    CHECK(t.keylog.contains(QLatin1String("CLIENT_RANDOM")));

    // 直接喂原始字节（隔离链路层变量）
    {
        const KeyLog kl = KeyLog::parse(t.keylog);
        const auto cpl = decryptTls12(t.concat(t.c2s), t.concat(t.s2c), kl, true);
        const auto spl = decryptTls12(t.concat(t.c2s), t.concat(t.s2c), kl, false);
        CHECK(cpl.has_value()); // TLS1.2 客户端方向应解出
        CHECK(spl.has_value());
        if (cpl)
            CHECK(cpl->contains(req));
        if (spl)
            CHECK(spl->contains("hi"));
    }
    // 全管线（pcap 合成 + TLS1.3 失败回落 TLS1.2）
    {
        const QVector<DecryptedStream> streams = decryptPcap(buildPcap(t.c2s, t.s2c), t.keylog);
        CHECK_EQ(int(streams.size()), 1);
        if (!streams.isEmpty()) {
            CHECK(streams.first().clientPlain.contains(req));
            CHECK(streams.first().serverPlain.contains("hi"));
        }
    }
}

// WebView 密文侧收尾纯逻辑：解密 → 合成 webview 事件 → flow（含入库 roundtrip）。
static void testWebviewFlowsFromPcap()
{
    MemBioTls t;
    CHECK(t.init(false, nullptr));
    if (!t.cli || !t.srv)
        return;
    CHECK(t.handshake());
    const QByteArray req = "GET /api/v1/hello HTTP/1.1\r\nHost: test.local\r\n\r\n";
    const QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    CHECK(t.exchange(req, resp));

    // 空产物静默跳过
    CHECK(webviewFlowsFromPcap(QByteArray(), QByteArray()).isEmpty());
    CHECK(webviewFlowsFromPcap(buildPcap(t.c2s, t.s2c), QByteArray()).isEmpty());

    const QVector<Flow> flows = webviewFlowsFromPcap(buildPcap(t.c2s, t.s2c), t.keylog.toUtf8());
    CHECK(!flows.isEmpty());
    if (!flows.isEmpty()) {
        const Flow& f = flows.first();
        CHECK(f.comm == QStringLiteral("WebView(解密)"));
        CHECK(f.kind == FlowKind::Http1);
        CHECK_EQ(f.pid, quint32(9100000)); // 合成 pid
        CHECK(f.src == QLatin1String("10.0.2.15:51000"));
        CHECK(f.dst == QLatin1String("93.184.216.34:443"));
        CHECK(f.request && f.request->host() && *f.request->host() == QLatin1String("test.local"));
        CHECK(f.response && f.response->startLine == QLatin1String("HTTP/1.1 200 OK"));
        CHECK_EQ(f.wbytes, quint64(req.size()));
        CHECK_EQ(f.rbytes, quint64(resp.size()));

        // 入库 roundtrip
        auto opened = Store::openInMemory();
        CHECK(opened.ok());
        if (opened.ok()) {
            Store st = std::move(opened.value());
            CHECK(st.upsertFlows(flows).ok());
            FlowsQuery q;
            q.limit = 10;
            const auto list = st.querySummaries(q);
            CHECK(list.ok());
            CHECK_EQ(int(list.value().size()), 1);
            const FlowSummary& sum = list.value().at(0);
            CHECK(sum.comm == QStringLiteral("WebView(解密)"));
            CHECK(sum.host == QLatin1String("test.local"));
            CHECK(sum.method == QLatin1String("GET"));
            CHECK_EQ(int(sum.status), 200);
        }
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mw_capture_test"));

    testTimestamps();
    testEventStreamFraming();
    testEventStreamMetaAndClosed();
    testH1BasicTransaction();
    testKeepaliveTwoRounds();
    testChunkedTransferEncoding();
    testMidstreamResponseSalvaged();
    testSplitHeaderEvents();
    testGzipResponseDecoded();
    testDeflateResponseDecoded();
    testH2PrefaceMultiStream();
    testH2MidstreamDynamicTable();
    testHpackHuffman();
    testPlaintextRawPreview();
    testLiveSnapshotWithoutMutation();
    testStoreRoundtrip();
    testStoreUpsertUpdates();
    testSessionIdleQueries();
    testGoldenSampleEndToEnd();
    testKeylogParse();
    testPcapClassicParse();
    testPcapLinkLayerExtract();
    testTcpStreamBufReassembly();
    testAssembleStreamsDirections();
    testSplitRecordsAndRobustness();
    testTls13RoundtripDecrypt();
    testTls12RoundtripDecrypt();
    testWebviewFlowsFromPcap();

    qInfo().noquote() << QStringLiteral("mw_capture_test: %1/%2 通过")
                             .arg(g_total - g_failed)
                             .arg(g_total);
    return g_failed == 0 ? 0 : 1;
}
