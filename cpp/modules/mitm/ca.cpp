// CA 管理与动态叶子证书签发 ← crates/mwine-mitm/src/ca.rs
#include "ca.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace mw::mitm {

namespace {

// —— OpenSSL RAII 别名 ——
struct X509Del {
    void operator()(X509* p) const { X509_free(p); }
};
struct EvpKeyDel {
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct Asn1IntDel {
    void operator()(ASN1_INTEGER* p) const { ASN1_INTEGER_free(p); }
};
struct BnDel {
    void operator()(BIGNUM* p) const { BN_free(p); }
};
using X509Ptr = std::unique_ptr<X509, X509Del>;
using EvpKeyPtr = std::unique_ptr<EVP_PKEY, EvpKeyDel>;
using BnPtr = std::unique_ptr<BIGNUM, BnDel>;
using Asn1IntPtr = std::unique_ptr<ASN1_INTEGER, Asn1IntDel>;

QString lastSslError(const char* what)
{
    unsigned long e = ERR_peek_last_error();
    char buf[256] = { 0 };
    if (e != 0)
        ERR_error_string_n(e, buf, sizeof(buf));
    else
        qstrncpy(buf, "unknown", sizeof(buf));
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(what), QString::fromLatin1(buf));
}

// EC P-256 密钥对（rcgen KeyPair::generate 默认曲线）
EvpKeyPtr generateEcKey(QString* errOut)
{
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx) {
        *errOut = lastSslError("EVP_PKEY_CTX_new_id 失败");
        return nullptr;
    }
    EvpKeyPtr key;
    if (EVP_PKEY_keygen_init(ctx) <= 0
        || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) <= 0) {
        *errOut = lastSslError("EC keygen init 失败");
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx, &raw) != 1) {
        *errOut = lastSslError("EC 密钥生成失败");
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY_CTX_free(ctx);
    key.reset(raw);
    return key;
}

// 随机正数序列号（rcgen 默认 128 位随机）
bool setRandomSerial(X509* x, QString* errOut)
{
    unsigned char buf[16];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        *errOut = lastSslError("RAND_bytes 失败");
        return false;
    }
    buf[0] &= 0x7f; // 保证非负
    if (buf[0] == 0)
        buf[0] = 1;
    BnPtr bn(BN_bin2bn(buf, sizeof(buf), nullptr));
    if (!bn) {
        *errOut = lastSslError("BN_bin2bn 失败");
        return false;
    }
    Asn1IntPtr ai(BN_to_ASN1_INTEGER(bn.get(), nullptr));
    if (!ai || X509_set_serialNumber(x, ai.get()) != 1) {
        *errOut = lastSslError("序列号设置失败");
        return false;
    }
    return true;
}

bool addExtStr(X509* x, int nid, const char* value, QString* errOut)
{
    X509_EXTENSION* ex = X509V3_EXT_conf_nid(nullptr, nullptr, nid, value);
    if (!ex) {
        *errOut = lastSslError("X509V3_EXT_conf_nid 失败");
        return false;
    }
    const bool ok = X509_add_ext(x, ex, -1) == 1;
    X509_EXTENSION_free(ex);
    if (!ok)
        *errOut = lastSslError("X509_add_ext 失败");
    return ok;
}

bool addSanDns(X509* x, const QByteArray& dns, QString* errOut)
{
    ASN1_IA5STRING* ia5 = ASN1_IA5STRING_new();
    if (!ia5 || ASN1_STRING_set(ia5, dns.constData(), int(dns.size())) != 1) {
        *errOut = lastSslError("SAN 字符串构造失败");
        return false;
    }
    GENERAL_NAME* gn = GENERAL_NAME_new();
    GENERAL_NAME_set0_value(gn, GEN_DNS, ia5); // gn 接管 ia5
    GENERAL_NAMES* gns = GENERAL_NAMES_new();
    sk_GENERAL_NAME_push(gns, gn); // gns 接管 gn
    X509_EXTENSION* ex = X509V3_EXT_i2d(NID_subject_alt_name, 0 /*非临界*/, gns);
    GENERAL_NAMES_free(gns);
    if (!ex) {
        *errOut = lastSslError("SAN 扩展构造失败");
        return false;
    }
    const bool ok = X509_add_ext(x, ex, -1) == 1;
    X509_EXTENSION_free(ex);
    if (!ok)
        *errOut = lastSslError("SAN 扩展挂载失败");
    return ok;
}

QByteArray x509ToPem(X509* x)
{
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return QByteArray();
    PEM_write_bio_X509(bio, x);
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    QByteArray out(data, int(len));
    BIO_free(bio);
    return out;
}

QByteArray keyToPkcs8Pem(EVP_PKEY* k)
{
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio)
        return QByteArray();
    PEM_write_bio_PKCS8PrivateKey(bio, k, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    QByteArray out(data, int(len));
    BIO_free(bio);
    return out;
}

X509Ptr x509FromPem(const QByteArray& pem, QString* errOut)
{
    BIO* bio = BIO_new_mem_buf(pem.constData(), int(pem.size()));
    if (!bio) {
        *errOut = QStringLiteral("BIO 构造失败");
        return nullptr;
    }
    X509Ptr x(PEM_read_bio_X509(bio, nullptr, nullptr, nullptr));
    BIO_free(bio);
    if (!x)
        *errOut = lastSslError("证书 PEM 解析失败");
    return x;
}

EvpKeyPtr keyFromPem(const QByteArray& pem, QString* errOut)
{
    BIO* bio = BIO_new_mem_buf(pem.constData(), int(pem.size()));
    if (!bio) {
        *errOut = QStringLiteral("BIO 构造失败");
        return nullptr;
    }
    EvpKeyPtr k(PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr));
    BIO_free(bio);
    if (!k)
        *errOut = lastSslError("私钥 PEM 解析失败");
    return k;
}

// CA 自签证书：CN=mwine-mitm，CA:TRUE，keyUsage=keyCertSign/cRLSign/digitalSignature，
// 有效期 now-1d ~ now+3650d（对应 Rust init_ca 的 CertificateParams）
X509Ptr makeCaCertificate(EVP_PKEY* key, QString* errOut)
{
    X509Ptr x(X509_new());
    if (!x) {
        *errOut = lastSslError("X509_new 失败");
        return nullptr;
    }
    if (X509_set_version(x.get(), 2) != 1 // v3
        || !setRandomSerial(x.get(), errOut))
        return nullptr;
    // 自签：主题=签发者，CN=mwine-mitm（OpenSSL 4 的 X509_get_subject_name 返回 const，
    // 主题经独立 X509_NAME 构造后一次性挂回）
    X509_NAME* name = X509_NAME_new();
    if (!name
        || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char*>("mwine-mitm"), -1, -1, 0)
               != 1) {
        X509_NAME_free(name);
        *errOut = lastSslError("CA 主题构造失败");
        return nullptr;
    }
    const bool namesOk = X509_set_subject_name(x.get(), name) == 1
                         && X509_set_issuer_name(x.get(), name) == 1;
    X509_NAME_free(name);
    if (!namesOk || X509_set_pubkey(x.get(), key) != 1) {
        *errOut = lastSslError("CA 签发者/公钥设置失败");
        return nullptr;
    }
    X509_gmtime_adj(X509_getm_notBefore(x.get()), -86400);           // now - 1d
    X509_gmtime_adj(X509_getm_notAfter(x.get()), 3650L * 86400);     // now + 3650d
    if (!addExtStr(x.get(), NID_basic_constraints, "critical,CA:TRUE", errOut))
        return nullptr;
    if (!addExtStr(x.get(), NID_key_usage, "critical,keyCertSign,cRLSign,digitalSignature", errOut))
        return nullptr;
    if (X509_sign(x.get(), key, EVP_sha256()) <= 0) {
        *errOut = lastSslError("CA 自签失败");
        return nullptr;
    }
    return x;
}

bool writeAllBytes(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(data) == data.size() && f.flush();
}

} // namespace

// ===================== Impl =====================

struct CaAuthority::Impl {
    QByteArray certPem;
    QByteArray keyPem;
    QString dir;
    X509Ptr caCert; // 签发叶子用（解析态）
    EvpKeyPtr caKey;
};

// ===================== 构造 =====================

mw::Result<CaAuthority> CaAuthority::fromPemPair(const QByteArray& certPem, const QByteArray& keyPem,
                                                 const QString& dir)
{
    QString err;
    X509Ptr cert = x509FromPem(certPem, &err);
    if (!cert)
        return mw::err<CaAuthority>(err);
    EvpKeyPtr key = keyFromPem(keyPem, &err);
    if (!key)
        return mw::err<CaAuthority>(err);
    // 证书/私钥须配对（等价 rcgen Issuer 的 key 归属约束）
    EVP_PKEY* pub = X509_get0_pubkey(cert.get());
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (!pub || EVP_PKEY_eq(pub, key.get()) != 1)
#else
    if (!pub || EVP_PKEY_cmp(pub, key.get()) != 1)
#endif
        return mw::err<CaAuthority>(QStringLiteral("CA 证书与私钥不匹配"));

    auto impl = std::make_shared<Impl>();
    impl->certPem = certPem;
    impl->keyPem = keyPem;
    impl->dir = dir;
    impl->caCert = std::move(cert);
    impl->caKey = std::move(key);
    return mw::Result<CaAuthority>::Ok(CaAuthority(std::move(impl)));
}

mw::Result<CaAuthority> CaAuthority::loadOrGenerate(const QString& dirParam)
{
    const QString dir = dirParam.isEmpty() ? caDir() : dirParam;
    const QString crtPath = QDir(dir).filePath(QStringLiteral("ca.crt"));
    const QString keyPath = QDir(dir).filePath(QStringLiteral("ca.key"));

    // 已有对 → 加载（Rust：读两文件 + KeyPair/Issuer 解析）
    QFile crtF(crtPath);
    QFile keyF(keyPath);
    if (crtF.open(QIODevice::ReadOnly) && keyF.open(QIODevice::ReadOnly)) {
        auto loaded = fromPemPair(crtF.readAll(), keyF.readAll(), dir);
        if (loaded) {
            qInfo().noquote() << QStringLiteral("[mitm] CA 已加载：") << crtPath;
            return loaded;
        }
    }

    // 首启生成
    if (!QDir().mkpath(dir))
        return mw::err<CaAuthority>(QStringLiteral("创建 CA 目录失败: %1").arg(dir));
    QString err;
    EvpKeyPtr key = generateEcKey(&err);
    if (!key)
        return mw::err<CaAuthority>(err);
    X509Ptr cert = makeCaCertificate(key.get(), &err);
    if (!cert)
        return mw::err<CaAuthority>(err);
    const QByteArray certPem = x509ToPem(cert.get());
    const QByteArray keyPem = keyToPkcs8Pem(key.get());
    if (certPem.isEmpty() || keyPem.isEmpty())
        return mw::err<CaAuthority>(QStringLiteral("CA 序列化失败"));
    if (!writeAllBytes(crtPath, certPem) || !writeAllBytes(keyPath, keyPem))
        return mw::err<CaAuthority>(QStringLiteral("CA 落盘失败: %1").arg(dir));
    qInfo().noquote() << QStringLiteral("[mitm] 已生成新 CA：") << crtPath;

    return fromPemPair(certPem, keyPem, dir);
}

QByteArray CaAuthority::certPem() const { return impl_->certPem; }
QByteArray CaAuthority::keyPem() const { return impl_->keyPem; }
QString CaAuthority::dir() const { return impl_->dir; }

// ===================== 叶子签发 =====================

mw::Result<LeafCert> CaAuthority::signLeaf(const QString& sni) const
{
    if (sni.isEmpty() || sni.contains(QLatin1Char('/')) || sni.contains(QLatin1Char(':')))
        return mw::err<LeafCert>(QStringLiteral("非法 SNI: %1").arg(sni));

    QString err;
    EvpKeyPtr leafKey = generateEcKey(&err);
    if (!leafKey)
        return mw::err<LeafCert>(err);

    X509Ptr x(X509_new());
    if (!x) {
        return mw::err<LeafCert>(lastSslError("X509_new 失败"));
    }
    if (X509_set_version(x.get(), 2) != 1 || !setRandomSerial(x.get(), &err))
        return mw::err<LeafCert>(err);
    const QByteArray sniUtf8 = sni.toUtf8();
    X509_NAME* name = X509_NAME_new();
    if (!name
        || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                      reinterpret_cast<const unsigned char*>(sniUtf8.constData()),
                                      int(sniUtf8.size()), -1, 0)
               != 1) {
        X509_NAME_free(name);
        return mw::err<LeafCert>(lastSslError("叶子证书主题构造失败"));
    }
    const bool namesOk = X509_set_subject_name(x.get(), name) == 1
                         // 签发者 = CA 主题（const 引用即可）
                         && X509_set_issuer_name(x.get(), X509_get_subject_name(impl_->caCert.get())) == 1;
    X509_NAME_free(name);
    if (!namesOk || X509_set_pubkey(x.get(), leafKey.get()) != 1)
        return mw::err<LeafCert>(lastSslError("叶子证书签发者/公钥设置失败"));
    X509_gmtime_adj(X509_getm_notBefore(x.get()), -3600);        // now - 1h
    X509_gmtime_adj(X509_getm_notAfter(x.get()), 365L * 86400);  // now + 365d
    if (!addSanDns(x.get(), sni.toLatin1(), &err)) // SNI 域名为 ASCII（ punycode 已就位）
        return mw::err<LeafCert>(err);
    if (X509_sign(x.get(), impl_->caKey.get(), EVP_sha256()) <= 0)
        return mw::err<LeafCert>(lastSslError("叶子证书签发失败"));

    LeafCert leaf;
    leaf.certPem = x509ToPem(x.get());
    leaf.keyPem = keyToPkcs8Pem(leafKey.get());
    if (leaf.certPem.isEmpty() || leaf.keyPem.isEmpty())
        return mw::err<LeafCert>(QStringLiteral("叶子证书序列化失败"));
    return mw::Result<LeafCert>::Ok(std::move(leaf));
}

// ===================== 全局单例 =====================

const CaAuthority* initCa()
{
    static CaAuthority* ca = []() -> CaAuthority* {
        auto r = CaAuthority::loadOrGenerate();
        if (!r) {
            qWarning().noquote() << QStringLiteral("[mitm] CA 初始化失败: %1").arg(r.error());
            return nullptr;
        }
        return new CaAuthority(std::move(r.value()));
    }();
    return ca;
}

QString caDir()
{
    const QString home = qEnvironmentVariable("HOME");
    return QDir(home.isEmpty() ? QStringLiteral("/tmp") : home)
        .filePath(QStringLiteral(".mwine/capture/mitm-ca"));
}

} // namespace mw::mitm
