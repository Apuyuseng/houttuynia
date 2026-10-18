// CA 管理与动态叶子证书签发。
// ← crates/mwine-mitm/src/ca.rs
//
// Rust 侧依赖 rcgen（ECDSA P-256 + 自签 CA + 级联签发 + rustls 握手集成）；
// C++ 等价实现用 OpenSSL EVP/X509 API：
//   KeyPair::generate()              → EVP_PKEY keygen（EC P-256）
//   params.self_signed(&kp)          → X509 自签（SHA256-ECDSA）
//   params.signed_by(&kp, &issuer)   → X509 用 CA 密钥签发
//   kp.serialize_pem()               → PEM_write_bio_PKCS8PrivateKey（PKCS#8 PEM）
//   cert.pem()                       → PEM_write_bio_X509（证书 PEM）
//   DynamicCertResolver::resolve     → CaAuthority::signLeaf（run.cpp 的 SNI 回调调用）
//   rustls NoVerify/tls_connect      → SSL_CTX_set_verify(SSL_VERIFY_NONE)（见 run.cpp）
//
// 落盘格式与 Rust 版产物互通：
//   ca.crt = "-----BEGIN CERTIFICATE-----"（X509 PEM）
//   ca.key = "-----BEGIN PRIVATE KEY-----"（PKCS#8 PEM，rcgen serialize_pem 同款）
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <memory>

#include "result.hpp"

namespace mw::mitm {

/// CA 目录（Rust ca_dir：$HOME/.mwine/capture/mitm-ca，HOME 缺失回退 /tmp）。
QString caDir();

/// 动态签发的叶子证书（Rust DynamicCertResolver::resolve 的产出形态；
/// keyPem 为 PKCS#8，对应 rustls 侧 PrivateKeyDer::Pkcs8）。
struct LeafCert {
    QByteArray certPem;
    QByteArray keyPem;
};

/// 一份可用 CA：加载已有 ca.crt/ca.key，或首启生成并落盘。
///（Rust init_ca 的 OnceLock<CaCtx> 在此实体化为可注入目录的对象，
//  便于测试与多环境；全局单例见 initCa()）
class CaAuthority {
public:
    /// dir 为空 → caDir()。加载已有对（证书/私钥匹配）；否则生成新 CA 并写盘。
    static mw::Result<CaAuthority> loadOrGenerate(const QString& dir = QString());

    /// 从 PEM 对构造（不落盘）。证书与私钥不匹配/无法解析 → Err。
    static mw::Result<CaAuthority> fromPemPair(const QByteArray& certPem, const QByteArray& keyPem,
                                               const QString& dir = QString());

    QByteArray certPem() const; ///< CA 证书 PEM
    QByteArray keyPem() const;  ///< CA 私钥 PKCS#8 PEM
    QString dir() const;        ///< 落盘目录（内存构造时为空）

    /// 按 SNI 签发叶子证书：CN=sni + SAN dns:sni，
    /// 有效期 now-1h ~ now+365d（Rust DynamicCertResolver::resolve 的核心）。
    mw::Result<LeafCert> signLeaf(const QString& sni) const;

private:
    struct Impl;
    explicit CaAuthority(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

    std::shared_ptr<const Impl> impl_;
};

/// 进程级默认 CA（Rust init_ca：全局 OnceLock，代理启动前调用）。幂等。
/// 失败返回 nullptr（Rust 为 expect panic；守护进程内改为留空+日志）。
const CaAuthority* initCa();

} // namespace mw::mitm
