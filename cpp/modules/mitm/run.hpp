// MITM 代理主循环（Rust bin crate 的 main → 可调用函数）。
// ← crates/mwine-mitm/src/main.rs
//
// 流程与 Rust 一致：
//   CONNECT → 200 → 服务端 TLS 握手（按 SNI 动态签发叶子证书）→
//   连上游 TLS（跳过验证——研究代理）→ 双向解密管道 → 明文入库。
// rustls ResolvesServerCert 在 OpenSSL 侧对应 SSL_CTX_set_cert_cb
//（握手期回调，读 SNI 后用 CaAuthority::signLeaf 现签证书）。
//
// 可执行性说明：本模块编译为静态库 mw_mitm（对齐仓库「模块库 + 守护进程」布局）。
// 需要独立二进制（对应 Rust bin `mwine-mitm`）时薄壳即可：
//   #include "run.hpp"
//   #include <QCoreApplication>
//   int main(int argc, char** argv) {
//       QCoreApplication app(argc, argv);
//       const auto r = mw::mitm::runProxy(mw::mitm::proxyPortFromEnv());
//       return r ? 0 : 1;
//   }
#pragma once

#include <QtGlobal>

#include "result.hpp"

namespace mw::mitm {

/// 代理端口（Rust：MWINE_MITM_PORT 环境变量，缺省 8899）。
quint16 proxyPortFromEnv();

/// 阻塞运行代理（accept 循环，每连接一个 detached 线程，对应 tokio::spawn）。
/// 绑定失败返回 Err；正常情况下永不返回。
mw::Result<bool> runProxy(quint16 port);

} // namespace mw::mitm
