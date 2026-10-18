// 本地 mock smart-socket 服务器验证客户端协议实现的正确性。
// ← crates/mwine-adb/tests/mock_server.rs（纯 main + CHECK，不用 QTest）
// 额外补充：wire 编解码（hex 长度 / crc32 / 包 round-trip / OKAY/FAIL）、
// stdin 帧、服务字符串、ShellV2Reader 分帧（socketpair 直喂）。
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "adb.hpp"

using namespace mw::adb;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

static const char* kSerial = "emulator-5554";

// mock 设备上可供拉取的虚拟文件（b"PK\x03\x04fake apk bytes \x00\x01"，21 字节）
static QByteArray mockPullBytes()
{
    return QByteArray("PK\003\004fake apk bytes \000\001", 21);
}

static QByteArray le32(quint32 v)
{
    QByteArray b(4, Qt::Uninitialized);
    b[0] = char(v & 0xFF);
    b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF);
    b[3] = char((v >> 24) & 0xFF);
    return b;
}

// ===== mock 事件通道（Rust mpsc channel 的轮询等价物）=====

struct MockEvent {
    QString path;
    QByteArray bytes;
};

struct MockHub {
    std::mutex mu;
    std::vector<MockEvent> pushes;

    void push(const QString& p, const QByteArray& b)
    {
        std::lock_guard<std::mutex> l(mu);
        pushes.push_back(MockEvent{p, b});
    }
    std::optional<MockEvent> waitForPush(int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            {
                std::lock_guard<std::mutex> l(mu);
                if (!pushes.empty()) {
                    MockEvent e = std::move(pushes.front());
                    pushes.erase(pushes.begin());
                    return e;
                }
            }
            if (std::chrono::steady_clock::now() > deadline)
                return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

// ===== mock 服务端 =====

// 读满 n 字节；连接关闭 → 空 QByteArray（客户端收尾属正常，静默结束）
static QByteArray readExactN(TcpConn& c, int n)
{
    auto r = c.readExact(n, 5000);
    if (!r.ok())
        return QByteArray();
    return r.value();
}

static bool readRequest(TcpConn& c, QString* out)
{
    const QByteArray hex = readExactN(c, 4);
    if (hex.size() != 4)
        return false;
    const auto lenRes = parseHexLength(hex);
    if (!lenRes.ok())
        return false;
    const QByteArray payload = readExactN(c, lenRes.value());
    if (payload.size() != lenRes.value())
        return false;
    *out = QString::fromUtf8(payload);
    return true;
}

static void writeStatus(TcpConn& c, const char* s)
{
    CHECK(c.writeAll(QByteArray(s, int(qstrlen(s)))).ok());
}

static void writePayload(TcpConn& c, const QByteArray& d)
{
    CHECK(c.writeAll(QByteArray::number(d.size(), 16).rightJustified(4, '0')).ok());
    CHECK(c.writeAll(d).ok());
}

static void handleSync(TcpConn& stream, const std::shared_ptr<MockHub>& hub);

static void handleTransported(TcpConn& stream, const QString& service,
                              const std::shared_ptr<MockHub>& hub)
{
    if (service == QLatin1String("sync:")) {
        writeStatus(stream, "OKAY");
        handleSync(stream, hub);
    } else if (service.startsWith(QLatin1String("reverse:"))) {
        // reverse:forward / reverse:killforward：均以 OKAY 起头
        //（forward 之后真实 adb 还会回显端口，客户端不读取，这里省略）
        writeStatus(stream, "OKAY");
    } else if (service.startsWith(QLatin1String("shell,v2,raw:"))) {
        writeStatus(stream, "OKAY");
        const QString cmd = service.mid(int(qstrlen("shell,v2,raw:")));
        if (cmd == QLatin1String("echo hello")) {
            const QByteArray data = QByteArray("hello\n", 6);
            CHECK(stream.writeAll(QByteArray(1, char(0x1))).ok());
            CHECK(stream.writeAll(le32(quint32(data.size()))).ok());
            CHECK(stream.writeAll(data).ok());
        } else if (cmd.startsWith(QLatin1String("pm install"))) {
            CHECK(stream.writeAll(QByteArray(1, char(0x1))).ok());
            CHECK(stream.writeAll(le32(8)).ok());
            CHECK(stream.writeAll(QByteArray("Success\n", 8)).ok());
        }
        // exit chunk = 0
        CHECK(stream.writeAll(QByteArray(1, char(0x3))).ok());
        CHECK(stream.writeAll(le32(1)).ok());
        CHECK(stream.writeAll(QByteArray(1, char(0))).ok());
    } else if (service.startsWith(QLatin1String("shell:"))) {
        writeStatus(stream, "OKAY");
        const QString cmd = service.mid(int(qstrlen("shell:")));
        if (cmd.startsWith(QLatin1String("pm install")))
            CHECK(stream.writeAll(QByteArray("Success\n", 8)).ok());
    } else {
        writeStatus(stream, "FAIL");
        writePayload(stream, QByteArray("unsupported transported service", 30));
    }
}

static void handleSync(TcpConn& stream, const std::shared_ptr<MockHub>& hub)
{
    // 读到 QUIT 或连接关闭为止；解析 SEND/DATA/DONE、STAT/RECV
    QString path;
    QByteArray data;
    for (;;) {
        const QByteArray cmd = readExactN(stream, 4);
        const QByteArray lenB = readExactN(stream, 4);
        if (cmd.size() != 4 || lenB.size() != 4)
            return;
        const quint32 len = getLE32(lenB.constData());
        if (cmd == QByteArrayLiteral("SEND")) {
            path = QString::fromUtf8(readExactN(stream, int(len)));
            // "path,mode" 格式
            const int comma = path.lastIndexOf(QLatin1Char(','));
            if (comma >= 0)
                path.truncate(comma);
        } else if (cmd == QByteArrayLiteral("DATA")) {
            data.append(readExactN(stream, int(len)));
        } else if (cmd == QByteArrayLiteral("DONE")) {
            readExactN(stream, int(len)); // mtime
            // 真实 adb 的 sync 回复固定 8 字节：4 字节状态 + 4 字节长度
            CHECK(stream.writeAll(QByteArray("OKAY", 4)).ok());
            CHECK(stream.writeAll(le32(0)).ok());
            hub->push(path, data);
            data.clear();
        } else if (cmd == QByteArrayLiteral("STAT")) {
            readExactN(stream, int(len)); // path
            QByteArray stat("STAT", 4);
            stat.append(le32(33206))   // mode
                .append(le32(quint32(mockPullBytes().size())))
                .append(le32(0)); // mtime
            CHECK(stream.writeAll(stat).ok());
        } else if (cmd == QByteArrayLiteral("RECV")) {
            readExactN(stream, int(len)); // path
            const QByteArray bytes = mockPullBytes();
            for (int off = 0; off < bytes.size(); off += 8) {
                const QByteArray chunk = bytes.mid(off, 8);
                CHECK(stream.writeAll(QByteArray("DATA", 4)).ok());
                CHECK(stream.writeAll(le32(quint32(chunk.size()))).ok());
                CHECK(stream.writeAll(chunk).ok());
            }
            CHECK(stream.writeAll(QByteArray("DONE", 4)).ok());
            CHECK(stream.writeAll(le32(0)).ok());
        } else if (cmd == QByteArrayLiteral("QUIT")) {
            return;
        } else {
            writeStatus(stream, "FAIL");
            writePayload(stream, QByteArray("unexpected sync request", 23));
            return;
        }
    }
}

static void handleConn(TcpConn& stream, const std::shared_ptr<MockHub>& hub)
{
    for (;;) {
        QString service;
        if (!readRequest(stream, &service))
            return; // 连接关闭
        if (service == QLatin1String("host:version")) {
            writeStatus(stream, "OKAY");
            writePayload(stream, QByteArray("0024", 4));
        } else if (service == QLatin1String("host:devices")) {
            writeStatus(stream, "OKAY");
            writePayload(stream, QByteArray("emulator-5554\tdevice\n", 21));
        } else if (service.startsWith(QLatin1String("host:transport:"))) {
            writeStatus(stream, "OKAY");
            // transport 之后同一连接上还会跟一个服务请求；
            // 该请求结束后连接即由客户端关闭
            QString next;
            if (!readRequest(stream, &next))
                return;
            handleTransported(stream, next, hub);
            return;
        } else if (service.startsWith(QLatin1String("host:forward:"))
                   || service.startsWith(QLatin1String("host:remove-forward:"))
                   || service.startsWith(QLatin1String("host-serial:"))) {
            writeStatus(stream, "OKAY");
        } else {
            writeStatus(stream, "FAIL");
            writePayload(stream, QStringLiteral("unknown service: %1").arg(service).toUtf8());
            return;
        }
    }
}

struct MockServer {
    quint16 port = 0;
    std::shared_ptr<MockHub> hub;
};

static MockServer spawnMock()
{
    MockServer m;
    m.hub = std::make_shared<MockHub>();
    qintptr lfd = makeLoopbackListener(0, &m.port);
    CHECK(lfd >= 0);
    std::thread([lfd, hub = m.hub] {
        for (;;) {
            const qintptr cfd = ::accept(lfd, nullptr, nullptr);
            if (cfd < 0)
                break;
            std::thread([cfd, hub] {
                TcpConn conn = TcpConn::fromFd(cfd);
                handleConn(conn, hub);
            }).detach();
        }
        ::close(static_cast<int>(lfd));
    }).detach();
    return m;
}

static AdbClient clientFor(quint16 port)
{
    AdbClient c = AdbClient::serverMode();
    c.port = port;
    return c;
}

// ===== tests（对齐 Rust）=====

static void testVersionRoundTrip()
{
    auto m = spawnMock();
    const AdbClient c = clientFor(m.port);
    auto v = c.version();
    CHECK(v.ok());
    CHECK(v.value() == QStringLiteral("0024"));
}

static void testDevicesParse()
{
    auto m = spawnMock();
    const AdbClient c = clientFor(m.port);
    auto devices = c.devices();
    CHECK(devices.ok());
    CHECK(devices.value().size() == 1);
    CHECK(devices.value().at(0).serial == QLatin1String(kSerial));
    CHECK(devices.value().at(0).state == QLatin1String("device"));
}

static void testShellV2OutputAndExitCode()
{
    auto m = spawnMock();
    const AdbClient c = clientFor(m.port);
    auto out = c.shellBlocking(QLatin1String(kSerial), QStringLiteral("echo hello"), 5000);
    CHECK(out.ok());
    CHECK(out.value().outStr() == QStringLiteral("hello\n"));
    CHECK(out.value().exitCode.has_value());
    CHECK(*out.value().exitCode == 0);
    CHECK(out.value().ok());
}

static void testPushBytesRoundTripViaSync()
{
    auto m = spawnMock();
    const AdbClient c = clientFor(m.port);
    const QByteArray payload("fake jar bytes \000\001\002", 18);
    auto r = c.pushBytes(QLatin1String(kSerial), payload,
                         QStringLiteral("/data/local/tmp/scrcpy-server"));
    CHECK(r.ok());
    auto ev = m.hub->waitForPush(5000);
    CHECK(ev.has_value());
    CHECK(ev->path == QStringLiteral("/data/local/tmp/scrcpy-server"));
    CHECK(ev->bytes == payload);
}

static void testForwardAndRemove()
{
    auto m = spawnMock();
    const AdbClient c = clientFor(m.port);
    CHECK(c.forwardAbstract(QLatin1String(kSerial), 27183, QStringLiteral("scrcpy")).ok());
    CHECK(c.removeForward(QLatin1String(kSerial), 27183).ok());
}

static void testReverseInstallAndRemove()
{
    auto m = spawnMock();
    const AdbClient c = clientFor(m.port);
    CHECK(c.reverseTcp(QLatin1String(kSerial), 27184, 27184).ok());
    CHECK(c.removeReverse(QLatin1String(kSerial), 27184).ok());
}

static void testPullFileRoundTripViaSync()
{
    auto m = spawnMock();
    const AdbClient c = clientFor(m.port);
    auto data = c.pullFile(QLatin1String(kSerial), QStringLiteral("/data/app/~~demo==/base.apk"));
    CHECK(data.ok());
    CHECK(data.value() == mockPullBytes());
}

static void testInstallFlowReportsSuccess()
{
    auto m = spawnMock();
    const AdbClient c = clientFor(m.port);
    // 写一个假 apk 文件走完整 install 链路（push + pm install）
    const QString dir = QDir::temp().filePath(QStringLiteral("mwine-adb-test"));
    CHECK(QDir().mkpath(dir));
    const QString apk = QDir(dir).filePath(QStringLiteral("demo.apk"));
    {
        QFile f(apk);
        CHECK(f.open(QIODevice::WriteOnly));
        CHECK(f.write(QByteArray("PK\003\004fake", 8)) == 8);
    }
    auto out = c.installApk(QLatin1String(kSerial), apk, 5000);
    CHECK(out.ok());
    CHECK(out.value().contains(QLatin1String("Success")));
}

// ===== 额外：wire 编解码 / 帧构造纯逻辑 =====

static void testWireCodec()
{
    // parseHexLength（大小写、非法字符）
    CHECK(parseHexLength(QByteArray("0010", 4)).value() == 16);
    CHECK(parseHexLength(QByteArray("ffff", 4)).value() == 65535);
    CHECK(parseHexLength(QByteArray("FFFF", 4)).value() == 65535);
    CHECK(!parseHexLength(QByteArray("zzzz", 4)).ok());
    CHECK(!parseHexLength(QByteArray("12", 2)).ok());

    // crc32 标准校验向量
    CHECK(wireCrc32(QByteArray("123456789", 9)) == 0xCBF43926u);
    CHECK(wireCrc32(QByteArray()) == 0);

    // stdin 帧：id=0 + 4 字节大端长度 + 数据（前缀共 5 字节）
    CHECK(stdinFrame(QByteArray("hello", 5)) == QByteArray::fromHex("0000000005" "68656c6c6f"));

    // 服务字符串
    CHECK(v1Service(QStringLiteral("echo hi")) == QStringLiteral("shell:echo hi"));
    CHECK(v2Service(QStringLiteral("echo hi")) == QStringLiteral("shell,v2,raw:echo hi"));
    CHECK(v2PtyService(QStringLiteral("top")) == QStringLiteral("shell,v2,pty:top"));

    // 线协议包 round-trip（socketpair 直连）
    int sv[2];
    CHECK(::socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) == 0);
    TcpConn a = TcpConn::fromFd(sv[0]);
    TcpConn b = TcpConn::fromFd(sv[1]);
    CHECK(writeWirePacket(a, wireCmd("WRTE"), 1, 2, QByteArray("payload", 7)).ok());
    auto p = readWirePacket(b, 5000);
    CHECK(p.ok());
    CHECK(p.value().command == wireCmd("WRTE"));
    CHECK(p.value().arg1 == 1);
    CHECK(p.value().arg2 == 2);
    CHECK(p.value().data == QByteArray("payload", 7));

    // magic 错误
    {
        char bad[24];
        putLE32(bad + 0, wireCmd("CNXN"));
        putLE32(bad + 4, 0);
        putLE32(bad + 8, 0);
        putLE32(bad + 12, 0);
        putLE32(bad + 16, 0);
        putLE32(bad + 20, 0x12345678); // 错误 magic
        CHECK(a.writeAll(QByteArray(bad, 24)).ok());
        auto badRes = readWirePacket(b, 5000);
        CHECK(badRes.isErr());
        CHECK(badRes.error().contains(QLatin1String("magic")));
    }

    // OKAY / FAIL 状态行 + 十六进制负载
    CHECK(a.writeAll(QByteArray("OKAY", 4)).ok());
    CHECK(readStatus(b, 5000).ok());
    CHECK(a.writeAll(QByteArray("0005", 4)).ok());
    CHECK(a.writeAll(QByteArray("hello", 5)).ok());
    CHECK(readHexPayload(b, 5000).value() == QByteArray("hello", 5));
    CHECK(a.writeAll(QByteArray("FAIL", 4)).ok());
    CHECK(a.writeAll(QByteArray("0003", 4)).ok());
    CHECK(a.writeAll(QByteArray("err", 3)).ok());
    {
        auto st = readStatus(b, 5000);
        CHECK(st.isErr());
        CHECK(st.error().contains(QLatin1String("err")));
    }
    // EOF：writeAll 后关闭写侧 → 对端 readSome 得 Ok(空)
    a.shutdownWrite();
    auto eof = b.readSome(5000);
    CHECK(eof.ok());
    CHECK(eof.value().isEmpty());
}

static void testShellV2ReaderFraming()
{
    // socketpair 直喂 v2 帧（小端长度）：stdout / stderr / exit / EOF
    int sv[2];
    CHECK(::socketpair(AF_LOCAL, SOCK_STREAM, 0, sv) == 0);
    TcpConn a = TcpConn::fromFd(sv[0]);
    TcpConn b = TcpConn::fromFd(sv[1]);
    QByteArray frames;
    frames.append(char(0x1)).append(le32(2)).append(QByteArray("AA", 2));
    frames.append(char(0x2)).append(le32(2)).append(QByteArray("BB", 2));
    frames.append(char(0x3)).append(le32(1)).append(char(7));
    CHECK(a.writeAll(frames).ok());
    a.shutdownWrite();
    ShellV2Reader reader(b, 5000);
    auto out = reader.collect();
    CHECK(out.ok());
    CHECK(out.value().outStr() == QStringLiteral("AA"));
    CHECK(out.value().errStr() == QStringLiteral("BB"));
    CHECK(out.value().exitCode.has_value());
    CHECK(*out.value().exitCode == 7);
    CHECK(!out.value().ok());
}

int main()
{
    testWireCodec();
    printf("[PASS] wire codec (hex length / crc32 / packet round-trip / OKAY-FAIL / stdin frame)\n");
    testShellV2ReaderFraming();
    printf("[PASS] shell v2 reader framing (LE length / stdout-stderr-exit / EOF)\n");
    testVersionRoundTrip();
    printf("[PASS] version round trip\n");
    testDevicesParse();
    printf("[PASS] devices parse\n");
    testShellV2OutputAndExitCode();
    printf("[PASS] shell v2 output and exit code\n");
    testPushBytesRoundTripViaSync();
    printf("[PASS] push bytes round trip via sync\n");
    testForwardAndRemove();
    printf("[PASS] forward and remove\n");
    testReverseInstallAndRemove();
    printf("[PASS] reverse install and remove\n");
    testPullFileRoundTripViaSync();
    printf("[PASS] pull file round trip via sync\n");
    testInstallFlowReportsSuccess();
    printf("[PASS] install flow reports success\n");
    printf("全部通过\n");
    return 0;
}
