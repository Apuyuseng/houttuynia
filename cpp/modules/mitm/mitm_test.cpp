// mw_mitm 自测 ← crates/mwine-mitm 各模块 #[cfg(test)]（Rust 侧无测试，此处补齐核心面）
// 覆盖：CA 生成→落盘→重载（同证书，非重生成）→按 SNI 签发叶子证书→OpenSSL 链校验、
// PEM 形态（证书/CERTIFICATE、私钥/PKCS#8 PRIVATE KEY——与 rcgen 产物互通）、
// 证书/私钥错配回退重生成、明文 HTTP 解析（配对/首部/体/keep-alive 多交互）、
// 端口/路径推导。全部落 QTemporaryDir，不触碰 ~/.mwine、不启代理。
#include "ca.hpp"
#include "store.hpp"
#include "run.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <ctime>
#include <memory>

using namespace mw::mitm;

static int g_failures = 0;
static int g_total = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_total;                                                                   \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            qWarning().noquote() << QStringLiteral("FAIL %1:%2 %3")                   \
                                       .arg(QString::fromLatin1(__FILE__),            \
                                            QString::number(__LINE__),                \
                                            QString::fromLatin1(#cond));              \
        }                                                                            \
    } while (false)

// —— OpenSSL RAII ——
struct X509Del {
    void operator()(X509* p) const { X509_free(p); }
};
struct StoreDel {
    void operator()(X509_STORE* p) const { X509_STORE_free(p); }
};
using X509Ptr = std::unique_ptr<X509, X509Del>;
using X509StorePtr = std::unique_ptr<X509_STORE, StoreDel>;

static X509Ptr x509FromPem(const QByteArray& pem)
{
    BIO* bio = BIO_new_mem_buf(pem.constData(), int(pem.size()));
    if (!bio)
        return nullptr;
    X509Ptr x(PEM_read_bio_X509(bio, nullptr, nullptr, nullptr));
    BIO_free(bio);
    return x;
}

// ASN1_TIME → epoch 秒（ASN1_TIME_to_tm 为非弃用路径）
static qint64 asn1TimeToEpoch(const ASN1_TIME* t)
{
    std::tm tmv {};
    if (!t || ASN1_TIME_to_tm(t, &tmv) != 1)
        return 0;
    return qint64(::timegm(&tmv));
}

// 提取主题 CN（X509_NAME_get_text_by_NID 已弃用，走条目遍历）
static QString cnOf(X509* x)
{
    // OpenSSL 4 的 X509_get_subject_name 返回 const X509_NAME*
    const X509_NAME* name = X509_get_subject_name(x);
    if (!name)
        return QString();
    const int idx = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if (idx < 0)
        return QString();
    const ASN1_STRING* cn = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, idx));
    if (!cn)
        return QString();
    return QString::fromLatin1(reinterpret_cast<const char*>(ASN1_STRING_get0_data(cn)),
                               int(ASN1_STRING_length(cn)));
}

// ===================== CA 生成 → 加载 → 签发 → 校验链 =====================

static void testCaGenerateLoadSignVerifyChain()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString dir = QDir(tmp.path()).filePath(QStringLiteral("mitm-ca"));

    // 1) 首启生成
    auto caR = CaAuthority::loadOrGenerate(dir);
    CHECK(caR);
    if (!caR)
        return;
    const QByteArray caCertPem = caR.value().certPem();
    const QByteArray caKeyPem = caR.value().keyPem();
    CHECK(caCertPem.contains("-----BEGIN CERTIFICATE-----"));
    CHECK(caKeyPem.contains("-----BEGIN PRIVATE KEY-----")); // PKCS#8（rcgen serialize_pem 同款）

    // 落盘两件套
    const QString crtPath = QDir(dir).filePath(QStringLiteral("ca.crt"));
    const QString keyPath = QDir(dir).filePath(QStringLiteral("ca.key"));
    CHECK(QFileInfo::exists(crtPath));
    CHECK(QFileInfo::exists(keyPath));

    // CA 证书形态：v3 + CA:TRUE + CN=mwine-mitm + 十年期
    {
        X509Ptr ca = x509FromPem(caCertPem);
        CHECK(ca != nullptr);
        if (ca) {
        CHECK(X509_get_version(ca.get()) == 2);
        const int idx = X509_get_ext_by_NID(ca.get(), NID_basic_constraints, -1);
        CHECK(idx >= 0);
        if (idx >= 0) {
            BASIC_CONSTRAINTS* bc = reinterpret_cast<BASIC_CONSTRAINTS*>(
                X509V3_EXT_d2i(X509_get_ext(ca.get(), idx)));
            CHECK(bc && bc->ca);
            BASIC_CONSTRAINTS_free(bc);
        }
        CHECK(cnOf(ca.get()) == QStringLiteral("mwine-mitm"));
        // 自签：签发者=持有者；有效期 now-1d ~ now+3650d（±2d 容差）
        CHECK(X509_NAME_cmp(X509_get_subject_name(ca.get()), X509_get_issuer_name(ca.get())) == 0);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 notBefore = asn1TimeToEpoch(X509_getm_notBefore(ca.get()));
        const qint64 notAfter = asn1TimeToEpoch(X509_getm_notAfter(ca.get()));
        CHECK(qAbs(notBefore - (now - 86400)) < 2 * 86400);
        CHECK(qAbs(notAfter - (now + 3650LL * 86400)) < 2 * 86400);
        }
    }

    // 2) 二次加载：同证书（非重生成）
    {
        auto again = CaAuthority::loadOrGenerate(dir);
        CHECK(again);
        CHECK(again.value().certPem() == caCertPem);
    }

    // 3) 按 SNI 签发叶子证书
    const QString sni = QStringLiteral("api.example.com");
    auto leafR = caR.value().signLeaf(sni);
    CHECK(leafR);
    if (!leafR)
        return;
    CHECK(leafR.value().certPem.contains("-----BEGIN CERTIFICATE-----"));
    CHECK(leafR.value().keyPem.contains("-----BEGIN PRIVATE KEY-----"));

    X509Ptr leaf = x509FromPem(leafR.value().certPem);
    CHECK(leaf != nullptr);
    if (leaf) {
        // CN=sni + SAN dns:sni
        CHECK(cnOf(leaf.get()) == sni);
        const int sanIdx = X509_get_ext_by_NID(leaf.get(), NID_subject_alt_name, -1);
        CHECK(sanIdx >= 0);
        if (sanIdx >= 0) {
            GENERAL_NAMES* gns = reinterpret_cast<GENERAL_NAMES*>(
                X509V3_EXT_d2i(X509_get_ext(leaf.get(), sanIdx)));
            CHECK(gns != nullptr);
            if (gns) {
                bool found = false;
                for (int i = 0; i < sk_GENERAL_NAME_num(gns); ++i) {
                    const GENERAL_NAME* gn = sk_GENERAL_NAME_value(gns, i);
                    if (gn->type == GEN_DNS) {
                        const char* dns = reinterpret_cast<const char*>(ASN1_STRING_get0_data(gn->d.dNSName));
                        found |= (QString::fromLatin1(dns) == sni);
                    }
                }
                GENERAL_NAMES_free(gns);
                CHECK(found);
            }
        }
        // 有效期：now-1h ~ now+365d（右端点至少在 360 天之外）
        const qint64 leafNotAfter = asn1TimeToEpoch(X509_getm_notAfter(leaf.get()));
        CHECK(leafNotAfter > QDateTime::currentSecsSinceEpoch() + 360LL * 24 * 3600);

        // 4) 校验链：leaf 由 CA 签出且链验证通过
        X509StorePtr store(X509_STORE_new());
        CHECK(store != nullptr);
        X509Ptr ca = x509FromPem(caCertPem);
        CHECK(ca != nullptr);
        if (ca) {
            CHECK(X509_STORE_add_cert(store.get(), ca.get()) == 1);
            X509_STORE_CTX* ctx = X509_STORE_CTX_new();
            CHECK(ctx != nullptr);
            if (ctx) {
                CHECK(X509_STORE_CTX_init(ctx, store.get(), leaf.get(), nullptr) == 1);
                const int verifyOk = X509_verify_cert(ctx);
                if (verifyOk != 1) {
                    qWarning().noquote()
                        << QStringLiteral("chain verify failed: %1")
                               .arg(QString::fromLatin1(
                                   X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx))));
                }
                CHECK(verifyOk == 1);
                X509_STORE_CTX_free(ctx);
            }
        }

        // 私钥可用且与叶子证书配对（PKCS#8 PEM 可解析）
        BIO* kb = BIO_new_mem_buf(leafR.value().keyPem.constData(), int(leafR.value().keyPem.size()));
        EVP_PKEY* pkey = kb ? PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr) : nullptr;
        if (kb)
            BIO_free(kb);
        CHECK(pkey != nullptr);
        if (pkey) {
            CHECK(X509_check_private_key(leaf.get(), pkey) == 1);
            EVP_PKEY_free(pkey);
        }
    }

    // 5) 错配回退：换掉 ca.key → loadOrGenerate 应重生成（证书变化）
    {
        QFile bad(keyPath);
        CHECK(bad.open(QIODevice::WriteOnly | QIODevice::Truncate));
        bad.write("-----BEGIN PRIVATE KEY-----\ngarbage\n-----END PRIVATE KEY-----\n");
        bad.close();
        auto regen = CaAuthority::loadOrGenerate(dir);
        CHECK(regen);
        CHECK(regen.value().certPem() != caCertPem); // 新 CA
    }

    // 6) 内存构造（fromPemPair）
    {
        auto mem = CaAuthority::fromPemPair(caCertPem, caKeyPem);
        CHECK(mem);
        CHECK(mem.value().dir().isEmpty());
        CHECK(mem.value().signLeaf(QStringLiteral("mem.example.org")));
        // 证书/私钥不匹配 → Err
        auto other = CaAuthority::loadOrGenerate(QDir(tmp.path()).filePath(QStringLiteral("ca2")));
        CHECK(other);
        auto mismatch = CaAuthority::fromPemPair(caCertPem, other.value().keyPem());
        CHECK(!mismatch);
    }
}

// ===================== 明文解析（Rust parse_http） =====================

static void testParseHttpBasicExchange()
{
    const QByteArray client = "GET /index.html HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "User-Agent: mwine-test\r\n"
                              "\r\n";
    const QByteArray server = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html\r\n"
                              "Content-Length: 5\r\n"
                              "\r\n"
                              "hello";
    const auto exchanges = parseHttp(QStringLiteral("example.com"), client, server);
    CHECK(exchanges.size() == 1);
    if (exchanges.size() == 1) {
        const auto& ex = exchanges.first();
        CHECK(ex.method == QStringLiteral("GET"));
        CHECK(ex.path == QStringLiteral("/index.html"));
        CHECK(ex.host == QStringLiteral("example.com"));
        CHECK(ex.status == 200);
        CHECK(ex.streamId == 0);
        CHECK(ex.reqHead.contains(QStringLiteral("Host: example.com")));
        CHECK(ex.reqHead.startsWith(QStringLiteral("GET /index.html HTTP/1.1")));
        CHECK(ex.respHead.contains(QStringLiteral("Content-Type: text/html")));
        CHECK(ex.respBody.has_value());
        if (ex.respBody) {
            CHECK(ex.respBody->text.contains(QStringLiteral("hello")));
            CHECK(ex.respBody->size == 6); // body + 结尾换行（与 Rust 近似解析一致）
        }
        CHECK(!ex.reqBody.has_value()); // 请求无体
    }
}

static void testParseHttpKeepalivePairs()
{
    QByteArray client;
    client += "POST /a HTTP/1.1\r\nHost: h\r\n\r\nbody-a\n";
    client += "POST /b HTTP/1.1\r\nHost: h\r\n\r\nbody-b\n";
    QByteArray server;
    server += "HTTP/1.1 201 Created\r\n\r\nA\n";
    server += "HTTP/1.1 404 Not Found\r\n\r\nB\n";
    const auto exchanges = parseHttp(QStringLiteral("h"), client, server);
    CHECK(exchanges.size() == 2);
    if (exchanges.size() == 2) {
        CHECK(exchanges[0].method == QStringLiteral("POST"));
        CHECK(exchanges[0].path == QStringLiteral("/a"));
        CHECK(exchanges[0].status == 201);
        CHECK(exchanges[1].path == QStringLiteral("/b"));
        CHECK(exchanges[1].status == 404);
    }
}

static void testParseHttpEmptyInputs()
{
    CHECK(parseHttp(QStringLiteral("x"), QByteArray(), QByteArray()).isEmpty());
    // 纯噪声（无起始行）：与 Rust 行为一致——整段被当作单块解析为一条交互
    //（split_http_messages 只在遇到起始行时切块，无起始行则整体一块）
    const auto noise = parseHttp(QStringLiteral("x"), QByteArrayLiteral("noise noise"),
                                 QByteArrayLiteral("more noise"));
    CHECK(noise.size() == 1);
    if (noise.size() == 1)
        CHECK(noise.first().status == 0); // 响应侧无状态行 → status 兜底 0
}

static void testPathsAndPort()
{
    const QString ca = caDir();
    CHECK(ca.contains(QStringLiteral(".mwine/capture/mitm-ca")));
    const QString db = flowsDbPath();
    CHECK(db.contains(QStringLiteral(".mwine/capture/flows.db")));
    CHECK(proxyPortFromEnv() == 8899); // MWINE_MITM_PORT 未设置时缺省
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mw_mitm_test"));

    testCaGenerateLoadSignVerifyChain();
    testParseHttpBasicExchange();
    testParseHttpKeepalivePairs();
    testParseHttpEmptyInputs();
    testPathsAndPort();

    qInfo().noquote() << QStringLiteral("mw_mitm_test: %1/%2 通过").arg(g_total - g_failures).arg(g_total);
    return g_failures == 0 ? 0 : 1;
}
