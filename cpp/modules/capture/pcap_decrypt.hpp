// pcap + SSLKEYLOGFILE 宿主侧解密管线 ← crates/mwine-capture/src/pcap_decrypt.rs
//
// 背景：WebView/Cronet 流量（ChromiumNet）的 TLS 库静态链接进 libwebviewchromium.so，
// ecapture 挂不上钩子——改走「密钥旁路」：guest 的 Chromium 开关
// --ssl-key-log-file 产出 NSS keylog，模拟器级 -tcpdump 产出全量密文 pcap，
// 两者都在宿主侧，本模块把它们变成明文 TCP 流，喂给现有 flow 解析器。
//
// 能力边界（v1，与 Rust 版一致）：
// - TLS 1.3（AES-128/256-GCM-SHA256/384、ChaCha20-Poly1305）——Chromium/conscrypt
//   默认；TLS 1.2 走 CLIENT_RANDOM keylog 兜底；
// - 每条记录按「应用密钥优先、握手密钥兜底」双试（AEAD tag 即真值，免跟踪
//   密钥切换点）；
// - QUIC/UDP 不处理：webview 命令行加 --disable-quic 规避。
//
// 与 Rust 版的两处有意差异（严格更优，见 pcap_decrypt.cpp 对应注释）：
// 1. TLS1.3 内部明文按 content-type 过滤，仅拼接应用数据（type 23）记录——
//    Rust 版会把解密的 Finished/NewSessionTicket 握手残片一并拼进明文（其
//    自测仅做子串断言故无害）；C++ 版明文直接喂 Reassembler，过滤二进制
//    残片才能让 H1/H2 解析器稳定命中消息起点；
// 2. 32B 密钥候选在 AES-256-GCM 失败后追加 ChaCha20-Poly1305 尝试（Rust 版
//    仅 AES——头注声称支持 ChaCha 但候选表没排）。
//
// AEAD/HMAC 走系统 libcrypto（模块 CMake 链接 OpenSSL::Crypto）。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>
#include <map>
#include <optional>

#include "result.hpp"

namespace mw::capture {

// NSS keylog 一行：LABEL <client_random|secret_id> <secret>。
struct KeyLog {
    // TLS1.2: client_random(32B) -> master_secret(48B)
    QHash<QByteArray, QByteArray> clientRandom;
    // TLS1.3 各阶段 secret：label -> id -> secret
    QHash<QString, QHash<QByteArray, QByteArray>> secrets;

    static KeyLog parse(const QString& text);
};

// pcap 包。
struct PcapPacket {
    quint64 tsUs = 0;
    QByteArray data;
};

// pcap 全局头解析出的链路层类型（guest `tcpdump -i any` 即 SLL）。
struct LinkType {
    enum Kind { Ethernet, LinuxSll, LinuxSll2, Other };
    Kind kind = Other;
    quint32 raw = 0; // Other 时的原始 LINKTYPE 值

    static LinkType fromValue(quint32 v);
};

struct PcapFile {
    QVector<PcapPacket> packets;
    LinkType link;
};

// 经典 pcap 读取（little/big endian 均支持；尾部不完整记录忽略）。
mw::Result<PcapFile> readPcapLink(const QByteArray& bytes);
mw::Result<QVector<PcapPacket>> readPcap(const QByteArray& bytes);

// 端点（IPv4）：ip 为大端语义的 32 位值（ip>>24 即点分首段）。
struct IpEndpoint {
    quint32 ip = 0;
    quint16 port = 0;

    friend bool operator==(const IpEndpoint& a, const IpEndpoint& b)
    {
        return a.ip == b.ip && a.port == b.port;
    }
};

QString ipToString(quint32 ip); // "93.184.216.34"

// TCP 段（IPv4）。
struct TcpSeg {
    quint64 tsUs = 0;
    IpEndpoint src;
    IpEndpoint dst;
    quint32 seq = 0;
    bool syn = false;
    QByteArray payload;
};

// 链路层剥离 → IPv4/TCP 段提取（Ethernet / SLL 16B / SLL2 20B）。
// 校验和省略（模拟器本地 slirp 抓包不丢不错，与 Rust 一致）。
QVector<TcpSeg> extractTcpSegs(const QVector<PcapPacket>& pkts, const LinkType& link);

// 一条 TCP 连接单方向的字节流（乱序容忍，同偏移长者胜，空洞终止拼接）。
class TcpStreamBuf {
public:
    void push(const TcpSeg& seg);
    // 依序拼出（到第一个空洞为止）。
    QByteArray assemble() const;
    bool isEmpty() const { return frags_.empty(); }
    quint64 total() const { return total_; }

private:
    std::optional<quint32> base_; // SYN（或首见）的初始 seq
    std::map<quint32, QByteArray> frags_; // 相对 seq -> payload
    quint64 total_ = 0;
};

// 一条 TCP 连接的双向字节流（4 元组规范化：小端点在前）。
struct TcpConn {
    IpEndpoint a, b;
    TcpStreamBuf a2b, b2a;
    quint64 tsUs = 0;
};

QVector<TcpConn> assembleStreams(const QVector<TcpSeg>& segs);

// TLS 记录切分：[(content_type, payload)]，尾部不完整记录忽略（抓包窗口边界）。
QVector<QPair<quint8, QByteArray>> splitRecords(const QByteArray& buf);

// TLS1.3 方向解密：应用层明文（APP 记录拼接；密钥候选双试，见头注）。
// 解不出（无该方向密钥/无记录可解）返回空 optional。
std::optional<QByteArray> decryptDirection(const QByteArray& stream, const KeyLog& kl,
                                           bool isClient);

// TLS1.2 方向解密（CLIENT_RANDOM keylog；双向流都要——Hello 随机数与套件取自双方）。
std::optional<QByteArray> decryptTls12(const QByteArray& clientStream,
                                       const QByteArray& serverStream, const KeyLog& kl,
                                       bool wantClient);

// 完整管线：pcap 字节 + keylog 文本 → 各连接的 (client明文, server明文)。
struct DecryptedStream {
    IpEndpoint client;
    IpEndpoint server;
    QByteArray clientPlain;
    QByteArray serverPlain;
};

QVector<DecryptedStream> decryptPcap(const QByteArray& pcap, const QString& keylogText);

} // namespace mw::capture
