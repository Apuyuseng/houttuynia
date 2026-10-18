// MITM 代理主循环 ← crates/mwine-mitm/src/main.rs
#include "run.hpp"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstring>
#include <memory>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ca.hpp"
#include "log.hpp"
#include "store.hpp"

namespace mw::mitm {

namespace {

// ===================== OpenSSL RAII =====================

struct SslCtxDel {
    void operator()(SSL_CTX* p) const { SSL_CTX_free(p); }
};
struct SslDel {
    void operator()(SSL* p) const { SSL_free(p); }
};
struct FdDel {
    void operator()(int* p) const
    {
        if (p && *p >= 0)
            ::close(*p);
        delete p;
    }
};
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDel>;
using SslPtr = std::unique_ptr<SSL, SslDel>;
using FdPtr = std::unique_ptr<int, FdDel>;

QString sslErrorText(const char* what)
{
    unsigned long e = ERR_peek_last_error();
    char buf[256] = { 0 };
    if (e != 0)
        ERR_error_string_n(e, buf, sizeof(buf));
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(what),
                                        e != 0 ? QString::fromLatin1(buf) : QStringLiteral("unknown"));
}

// ===================== 服务端动态证书（rustls ResolvesServerCert 的 OpenSSL 对应物） =====================

// 握手期回调：读 SNI → 动态签发叶子证书挂到本次连接。
// 返回 1 继续 / 0 失败（无 SNI 时返回 1 走默认无证书路径，握手自然失败，
// 对应 Rust resolve() 里 client_hello.server_name()? 的 None 短路）。
int dynamicCertCb(SSL* ssl, void*)
{
    const char* sni = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!sni || !*sni)
        return 1;
    const CaAuthority* ca = initCa();
    if (!ca)
        return 0;
    auto leaf = ca->signLeaf(QString::fromLatin1(sni));
    if (!leaf)
        return 0;
    const LeafCert leafCert = std::move(leaf.value());

    BIO* cb = BIO_new_mem_buf(leafCert.certPem.constData(), int(leafCert.certPem.size()));
    X509* cert = cb ? PEM_read_bio_X509(cb, nullptr, nullptr, nullptr) : nullptr;
    if (cb)
        BIO_free(cb);
    BIO* kb = BIO_new_mem_buf(leafCert.keyPem.constData(), int(leafCert.keyPem.size()));
    EVP_PKEY* key = kb ? PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr) : nullptr;
    if (kb)
        BIO_free(kb);
    if (!cert || !key) {
        MW_LOG("mitm", QStringLiteral("动态证书加载失败: %1").arg(QString::fromLatin1(sni)));
        if (cert)
            X509_free(cert);
        if (key)
            EVP_PKEY_free(key);
        return 0;
    }
    const int ok = SSL_use_certificate(ssl, cert); // 内部加引用
    X509_free(cert);
    const int ok2 = (ok == 1) ? SSL_use_PrivateKey(ssl, key) : 0; // 内部加引用
    EVP_PKEY_free(key);
    return ok2 == 1 ? 1 : 0;
}

// ===================== 阻塞 IO 小工具 =====================

bool writeAllFd(int fd, const char* data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n <= 0)
            return false;
        off += size_t(n);
    }
    return true;
}

bool sslWriteAll(SSL* ssl, const char* data, size_t len)
{
    while (len > 0) {
        const int n = SSL_write(ssl, data, int(len));
        if (n <= 0)
            return false;
        data += n;
        len -= size_t(n);
    }
    return true;
}

int connectUpstream(const QString& host, QString* errOut)
{
    // DNS + 443（Rust TcpStream::connect(format!("{host}:443"))）
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const QByteArray hostLatin1 = host.toLatin1();
    if (::getaddrinfo(hostLatin1.constData(), "443", &hints, &res) != 0 || !res) {
        *errOut = QStringLiteral("DNS 解析失败: %1").arg(host);
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        timeval tv{ 10, 0 };
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0)
        *errOut = QStringLiteral("连接上游失败: %1:443").arg(host);
    return fd;
}

// ===================== 单连接处理（Rust async fn handle + pipe_and_capture） =====================

void pumpAndCapture(SslPtr sslClient, SslPtr sslUp, const QString& host)
{
    QByteArray cbufAll; // 客户端 → 上游的明文全集
    QByteArray ubufAll; // 上游 → 客户端的明文全集
    char cbuf[16384];
    char ubuf[16384];

    // 双向管道（Rust tokio::join!(c2u, u2c) → 两线程汇合；
    // 单向结束时发 TLS close_notify，上游随之断流，对向 read 收 EOF 退出）
    std::thread c2u([&]() {
        while (true) {
            const int n = SSL_read(sslClient.get(), cbuf, sizeof(cbuf));
            if (n <= 0)
                break;
            cbufAll.append(cbuf, n);
            if (!sslWriteAll(sslUp.get(), cbuf, size_t(n)))
                break;
        }
        (void)SSL_shutdown(sslUp.get()); // 对应 Rust uw.shutdown()
    });
    std::thread u2c([&]() {
        while (true) {
            const int n = SSL_read(sslUp.get(), ubuf, sizeof(ubuf));
            if (n <= 0)
                break;
            ubufAll.append(ubuf, n);
            if (!sslWriteAll(sslClient.get(), ubuf, size_t(n)))
                break;
        }
        (void)SSL_shutdown(sslClient.get()); // 对应 Rust cw.shutdown()
    });
    c2u.join();
    u2c.join();

    if (!cbufAll.isEmpty() || !ubufAll.isEmpty())
        captureExchange(host, cbufAll, ubufAll);
}

void handleConnection(int clientFd)
{
    FdPtr clientFdGuard(new int(clientFd));

    // 1) 读 CONNECT → 200（上限 4KB，读到 \r\n\r\n 为止）
    QByteArray head;
    char buf[4096];
    while (!head.contains("\r\n\r\n") && head.size() < 4096) {
        const ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0)
            return;
        head.append(buf, int(n));
    }
    const int headerEnd = head.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;
    const QString connectLine = QString::fromUtf8(head.left(headerEnd));
    // Rust connect_line.split_whitespace().nth(1)
    const QStringList parts = connectLine.split(QChar(u' '), Qt::SkipEmptyParts);
    const QString target = parts.value(1);
    if (!connectLine.startsWith(QStringLiteral("CONNECT")) || target.isEmpty())
        return;
    const QString host = target.section(QLatin1Char(':'), 0, 0);
    if (host.isEmpty())
        return;
    if (!writeAllFd(clientFd, "HTTP/1.1 200 Connection established\r\n\r\n",
                    sizeof("HTTP/1.1 200 Connection established\r\n\r\n") - 1))
        return;

    // 2) TLS 握手（服务端角色，SNI 动态签发）
    SslCtxPtr serverCtx(SSL_CTX_new(TLS_server_method()));
    if (!serverCtx)
        return;
    SSL_CTX_set_min_proto_version(serverCtx.get(), TLS1_2_VERSION);
    SSL_CTX_set_cert_cb(serverCtx.get(), dynamicCertCb, nullptr);
    SslPtr sslClient(SSL_new(serverCtx.get()));
    if (!sslClient || SSL_set_fd(sslClient.get(), clientFd) != 1)
        return;
    if (SSL_accept(sslClient.get()) != 1) {
        MW_LOG("mitm", QStringLiteral("TLS(client) %1: %2").arg(host, sslErrorText("accept")));
        return;
    }

    // 3) TLS 连上游（跳过验证——研究代理；rustls NoVerify 的对应物）
    QString upErr;
    const int upFd = connectUpstream(host, &upErr);
    if (upFd < 0) {
        MW_LOG("mitm", QStringLiteral("connect %1: %2").arg(host, upErr));
        return;
    }
    FdPtr upFdGuard(new int(upFd));
    SslCtxPtr clientCtx(SSL_CTX_new(TLS_client_method()));
    if (!clientCtx)
        return;
    SSL_CTX_set_min_proto_version(clientCtx.get(), TLS1_2_VERSION);
    SSL_CTX_set_verify(clientCtx.get(), SSL_VERIFY_NONE, nullptr);
    SslPtr sslUp(SSL_new(clientCtx.get()));
    if (!sslUp || SSL_set_fd(sslUp.get(), upFd) != 1)
        return;
    const QByteArray hostLatin1 = host.toLatin1();
    if (!SSL_set_tlsext_host_name(sslUp.get(), hostLatin1.constData()))
        return;
    if (SSL_connect(sslUp.get()) != 1) {
        MW_LOG("mitm", QStringLiteral("TLS(upstream) %1: %2").arg(host, sslErrorText("connect")));
        return;
    }

    // 4) 双向解密管道 + 明文捕获
    pumpAndCapture(std::move(sslClient), std::move(sslUp), host);
}

} // namespace

// ===================== 对外入口 =====================

quint16 proxyPortFromEnv()
{
    bool ok = false;
    const int p = qEnvironmentVariableIntValue("MWINE_MITM_PORT", &ok);
    return (ok && p > 0 && p <= 65535) ? quint16(p) : quint16(8899);
}

mw::Result<bool> runProxy(quint16 port)
{
    (void)initCa(); // Rust main：ca::init_ca()

    ::signal(SIGPIPE, SIG_IGN); // SSL_write 对端断开不致带崩进程

    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
        return mw::err<bool>(QStringLiteral("socket 创建失败: %1")
                                 .arg(QString::fromLatin1(::strerror(errno))));
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        return mw::err<bool>(QStringLiteral("绑定端口失败: 127.0.0.1:%1: %2")
                                 .arg(port)
                                 .arg(QString::fromLatin1(::strerror(errno))));
    if (::listen(lfd, 64) < 0)
        return mw::err<bool>(QStringLiteral("listen 失败: %1")
                                 .arg(QString::fromLatin1(::strerror(errno))));
    MW_LOG("mitm", QStringLiteral("mwine-mitm 就绪 :%1").arg(port));

    while (true) {
        const int c = ::accept(lfd, nullptr, nullptr);
        if (c < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        std::thread(handleConnection, c).detach(); // 对应 tokio::spawn(handle(stream))
    }
    ::close(lfd);
    return mw::err<bool>(QStringLiteral("accept 循环退出"));
}

} // namespace mw::mitm
