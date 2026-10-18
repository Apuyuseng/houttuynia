// pcap + SSLKEYLOGFILE 解密管线实现 ← crates/mwine-capture/src/pcap_decrypt.rs
// （Rust 侧 ring/rustls 的 AEAD/HKDF 在此换用 OpenSSL EVP；解析/重组/套件
//   选择逻辑逐行对应，见 pcap_decrypt.hpp 头注的两处有意差异）
#include "pcap_decrypt.hpp"

#include <QtEndian>

#include <openssl/evp.h>

#include <cstring>

namespace mw::capture {

namespace {

// ===================== 小工具 =====================

quint32 rd32(const QByteArray& b, qsizetype off, bool le)
{
    const uchar* p = reinterpret_cast<const uchar*>(b.constData()) + off;
    return le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
}

int hexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// 严格 hex（偶数长度 + 合法字符），对应 Rust 侧 hex() 的失败语义。
std::optional<QByteArray> hexParse(const QString& s)
{
    if (s.size() % 2 != 0)
        return std::nullopt;
    QByteArray out(s.size() / 2, Qt::Uninitialized);
    for (int i = 0; i < s.size(); i += 2) {
        const int hi = hexVal(s.at(i).toLatin1());
        const int lo = hexVal(s.at(i + 1).toLatin1());
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out[i / 2] = char((hi << 4) | lo);
    }
    return out;
}

// TLS content-type 常量
constexpr quint8 kCtHandshake = 22;
constexpr quint8 kCtApp = 23;

// ===================== 摘要 / HMAC / HKDF =====================

// 哈希选择：名称 + HMAC 块大小（SHA-256=64，SHA-384=128）。
struct HashSel {
    const char* name;
    int blockSize;
};

QByteArray digest(const HashSel& h, const QByteArray& d)
{
    unsigned char buf[EVP_MAX_MD_SIZE];
    size_t len = 0;
    if (EVP_Q_digest(nullptr, h.name, nullptr,
                     reinterpret_cast<const unsigned char*>(d.constData()), size_t(d.size()),
                     buf, &len)
        != 1)
        return QByteArray();
    return QByteArray(reinterpret_cast<char*>(buf), int(len));
}

// HMAC（RFC 2104，手搓 ipad/opad——与 Rust 实现一致，绕开不同 OpenSSL 版本的
// HMAC 接口差异）。
QByteArray hmac(const HashSel& h, const QByteArray& key, const QByteArray& msg)
{
    QByteArray k = key;
    if (k.size() > h.blockSize)
        k = digest(h, k); // 理论分支：本模块 key ≤ 48B < 64/128，不会触发
    if (k.size() < h.blockSize)
        k.resize(h.blockSize, '\0');
    QByteArray ipad = k, opad = k;
    for (int i = 0; i < h.blockSize; ++i) {
        ipad[i] = char(uchar(ipad.at(i)) ^ 0x36);
        opad[i] = char(uchar(opad.at(i)) ^ 0x5c);
    }
    ipad += msg;
    const QByteArray inner = digest(h, ipad);
    opad += inner;
    return digest(h, opad);
}

// HKDF-Expand（RFC 5869）。
QByteArray hkdfExpand(const HashSel& h, const QByteArray& prk, const QByteArray& info, int len)
{
    QByteArray t, out;
    uint8_t i = 1;
    while (out.size() < len) {
        QByteArray input = t;
        input += info;
        input += char(i);
        t = hmac(h, prk, input);
        out += t;
        ++i;
    }
    out.truncate(len);
    return out;
}

// HKDF-Expand-Label（RFC 8446 §7.1）。
QByteArray expandLabel(const HashSel& h, const QByteArray& secret, const char* label,
                       const QByteArray& ctx, int len)
{
    const QByteArray full = QByteArray("tls13 ") + label;
    QByteArray info;
    info += char(uchar(len >> 8));
    info += char(uchar(len));
    info += char(uchar(full.size()));
    info += full;
    info += char(uchar(ctx.size()));
    info += ctx;
    return hkdfExpand(h, secret, info, len);
}

// ===================== AEAD（OpenSSL EVP） =====================

enum class AeadAlgo { Aes128Gcm, Aes256Gcm, Chacha20Poly1305 };

// AEAD 解密（tag 校验失败返回 false——调用方以此作为「密钥/序号不匹配」信号）。
bool aeadOpen(AeadAlgo algo, const QByteArray& key, const unsigned char nonce[12],
              const unsigned char* aad, size_t aadLen, const unsigned char* ct, size_t ctLen,
              const unsigned char* tag, QByteArray* out)
{
    const EVP_CIPHER* cipher = nullptr;
    switch (algo) {
    case AeadAlgo::Aes128Gcm:
        cipher = EVP_aes_128_gcm();
        break;
    case AeadAlgo::Aes256Gcm:
        cipher = EVP_aes_256_gcm();
        break;
    case AeadAlgo::Chacha20Poly1305:
        cipher = EVP_chacha20_poly1305();
        break;
    }
    if (!cipher)
        return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;
    bool ok = false;
    int mid = 0;
    QByteArray pt(int(ctLen), Qt::Uninitialized);
    do {
        if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1)
            break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char*>(key.constData()), nonce)
            != 1)
            break;
        if (aadLen > 0
            && EVP_DecryptUpdate(ctx, nullptr, &mid, aad, int(aadLen)) != 1)
            break;
        if (ctLen > 0
            && EVP_DecryptUpdate(ctx, reinterpret_cast<uchar*>(pt.data()), &mid, ct, int(ctLen))
                   != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                const_cast<unsigned char*>(tag))
            != 1)
            break;
        unsigned char fin[EVP_MAX_BLOCK_LENGTH];
        int flen = 0;
        if (EVP_DecryptFinal_ex(ctx, fin, &flen) != 1)
            break; // tag 失败：密钥或序号不对
        pt.truncate(mid);
        *out = std::move(pt);
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

// AEAD 解一条 TLS1.3 记录，返回内部明文（含 content-type 尾字节）。
std::optional<QByteArray> openRecord(const QByteArray& payload, const QByteArray& key,
                                     const QByteArray& iv, quint64 seq)
{
    if (payload.size() < 17 || key.size() < 16 || iv.size() < 12)
        return std::nullopt;
    unsigned char nonce[12];
    std::memcpy(nonce, iv.constData(), 12);
    for (int i = 0; i < 8; ++i)
        nonce[4 + i] ^= uchar(seq >> (56 - 8 * i));
    const int plen = int(payload.size());
    const unsigned char aad[5] = { 0x17, 0x03, 0x03, uchar(plen >> 8), uchar(plen) };
    const unsigned char* p = reinterpret_cast<const unsigned char*>(payload.constData());
    const int ctLen = plen - 16;
    QByteArray pt;
    if (key.size() == 16) {
        if (!aeadOpen(AeadAlgo::Aes128Gcm, key, nonce, aad, sizeof(aad), p, size_t(ctLen),
                      p + ctLen, &pt))
            return std::nullopt;
    } else if (key.size() == 32) {
        // 32B 密钥：AES-256-GCM 优先，失败追试 ChaCha20-Poly1305
        //（TLS1.3 两种套件 key/iv 尺寸相同，nonce/aad 构造也相同）
        if (!aeadOpen(AeadAlgo::Aes256Gcm, key, nonce, aad, sizeof(aad), p, size_t(ctLen),
                      p + ctLen, &pt)
            && !aeadOpen(AeadAlgo::Chacha20Poly1305, key, nonce, aad, sizeof(aad), p,
                         size_t(ctLen), p + ctLen, &pt))
            return std::nullopt;
    } else {
        return std::nullopt;
    }
    return pt;
}

// TLS 1.2 PRF（RFC 5246）：P_hash 分段异或。
QByteArray prf12(const HashSel& h, const QByteArray& secret, const char* label,
                 const QByteArray& seed, int len)
{
    QByteArray seed2(label);
    seed2 += seed;
    QByteArray a = hmac(h, secret, seed2);
    QByteArray out;
    while (out.size() < len) {
        QByteArray input = a;
        input += seed2;
        out += hmac(h, secret, input);
        a = hmac(h, secret, a);
    }
    out.truncate(len);
    return out;
}

// 端点编码为可比较的 64 位键（端口高位——对应 Rust 的 (port, ip) 元组序）。
quint64 epKey(const IpEndpoint& e)
{
    return (quint64(e.port) << 32) | quint64(e.ip);
}

IpEndpoint epFromKey(quint64 k)
{
    return IpEndpoint { quint32(k & 0xffffffffu), quint16(k >> 32) };
}

} // namespace

// ===================== KeyLog =====================

KeyLog KeyLog::parse(const QString& text)
{
    KeyLog kl;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        // 空白切分取前三个 token：LABEL <id> <secret>
        int pos = 0;
        QString toks[3];
        int ntok = 0;
        while (pos < line.size() && ntok < 3) {
            while (pos < line.size() && line.at(pos).isSpace())
                ++pos;
            int start = pos;
            while (pos < line.size() && !line.at(pos).isSpace())
                ++pos;
            if (pos > start)
                toks[ntok++] = line.mid(start, pos - start);
        }
        if (ntok < 3)
            continue;
        const auto id = hexParse(toks[1]);
        const auto sec = hexParse(toks[2]);
        if (!id || !sec)
            continue;
        if (toks[0] == QLatin1String("CLIENT_RANDOM") && id->size() == 32 && sec->size() == 48) {
            kl.clientRandom.insert(*id, *sec);
        } else {
            kl.secrets[toks[0]][*id] = *sec;
        }
    }
    return kl;
}

// ===================== pcap 解析 =====================

LinkType LinkType::fromValue(quint32 v)
{
    switch (v) {
    case 1:
        return LinkType { Ethernet, 0 };
    case 113:
        return LinkType { LinuxSll, 0 };
    case 276:
        return LinkType { LinuxSll2, 0 };
    default:
        return LinkType { Other, v };
    }
}

mw::Result<PcapFile> readPcapLink(const QByteArray& bytes)
{
    using R = mw::Result<PcapFile>;
    if (bytes.size() < 24)
        return R::Err(QStringLiteral("pcap 太短"));
    const uchar m[4] = { uchar(bytes.at(0)), uchar(bytes.at(1)), uchar(bytes.at(2)),
                         uchar(bytes.at(3)) };
    bool le;
    if ((m[0] == 0xd4 && m[1] == 0xc3 && m[2] == 0xb2 && m[3] == 0xa1)
        || (m[0] == 0x4d && m[1] == 0x3c && m[2] == 0xb2 && m[3] == 0xa1)) {
        le = true;
    } else if ((m[0] == 0xa1 && m[1] == 0xb2 && m[2] == 0xc3 && m[3] == 0xd4)
               || (m[0] == 0xa1 && m[1] == 0xb2 && m[2] == 0x3c && m[3] == 0x4d)) {
        le = false;
    } else {
        return R::Err(QStringLiteral("非经典 pcap"));
    }
    PcapFile out;
    out.link = LinkType::fromValue(rd32(bytes, 20, le));
    qsizetype off = 24;
    while (off + 16 <= bytes.size()) {
        const quint32 tsS = rd32(bytes, off, le);
        const quint32 tsUs = rd32(bytes, off + 4, le);
        const quint32 caplen = rd32(bytes, off + 8, le);
        off += 16;
        if (caplen > 262144 || quint64(caplen) > quint64(bytes.size() - off))
            break;
        PcapPacket p;
        p.tsUs = quint64(tsS) * 1000000ull + quint64(tsUs);
        p.data = bytes.mid(off, int(caplen));
        out.packets.push_back(std::move(p));
        off += caplen;
    }
    return R::Ok(std::move(out));
}

mw::Result<QVector<PcapPacket>> readPcap(const QByteArray& bytes)
{
    using R = mw::Result<QVector<PcapPacket>>;
    auto f = readPcapLink(bytes);
    if (!f)
        return R::Err(f.error());
    return R::Ok(std::move(f.value().packets));
}

QString ipToString(quint32 ip)
{
    return QStringLiteral("%1.%2.%3.%4")
        .arg(ip >> 24)
        .arg((ip >> 16) & 0xff)
        .arg((ip >> 8) & 0xff)
        .arg(ip & 0xff);
}

QVector<TcpSeg> extractTcpSegs(const QVector<PcapPacket>& pkts, const LinkType& link)
{
    QVector<TcpSeg> out;
    for (const PcapPacket& p : pkts) {
        const QByteArray& d = p.data;
        const uchar* ip = nullptr;
        qsizetype ipLen = 0;
        switch (link.kind) {
        case LinkType::Ethernet:
            if (d.size() < 14 || uchar(d.at(12)) != 0x08 || uchar(d.at(13)) != 0x00)
                continue;
            ip = reinterpret_cast<const uchar*>(d.constData()) + 14;
            ipLen = d.size() - 14;
            break;
        case LinkType::LinuxSll:
            // SLL: pkttype(2) hatype(2) halen(2) addr(8) protocol(2)
            if (d.size() < 16 || uchar(d.at(14)) != 0x08 || uchar(d.at(15)) != 0x00)
                continue;
            ip = reinterpret_cast<const uchar*>(d.constData()) + 16;
            ipLen = d.size() - 16;
            break;
        case LinkType::LinuxSll2:
            // SLL2: protocol(2) + reserved(2) + ifindex(4) + hatype(2) + pkttype(1)
            //       + halen(1) + addr(8)
            if (d.size() < 20 || uchar(d.at(0)) != 0x08 || uchar(d.at(1)) != 0x00)
                continue;
            ip = reinterpret_cast<const uchar*>(d.constData()) + 20;
            ipLen = d.size() - 20;
            break;
        default:
            continue;
        }
        if (ipLen < 20 || (ip[0] >> 4) != 4)
            continue;
        const qsizetype ihl = qsizetype(ip[0] & 0xf) * 4;
        if (ihl < 20 || ipLen < ihl + 20 || ip[9] != 6)
            continue;
        const quint32 srcIp = qFromBigEndian<quint32>(ip + 12);
        const quint32 dstIp = qFromBigEndian<quint32>(ip + 16);
        const uchar* tcp = ip + ihl;
        const qsizetype tcpLen = ipLen - ihl;
        if (tcpLen < 20)
            continue;
        const quint16 sport = qFromBigEndian<quint16>(tcp);
        const quint16 dport = qFromBigEndian<quint16>(tcp + 2);
        const quint32 seq = qFromBigEndian<quint32>(tcp + 4);
        const qsizetype doff = qsizetype(std::max(int(tcp[12] >> 4), 1)) * 4;
        const quint8 flags = tcp[13];
        if (tcpLen < doff)
            continue;
        TcpSeg s;
        s.tsUs = p.tsUs;
        s.src = IpEndpoint { srcIp, sport };
        s.dst = IpEndpoint { dstIp, dport };
        s.seq = seq;
        s.syn = (flags & 0x02) != 0;
        s.payload = QByteArray(reinterpret_cast<const char*>(tcp + doff), int(tcpLen - doff));
        out.push_back(std::move(s));
    }
    return out;
}

// ===================== TCP 重组 =====================

void TcpStreamBuf::push(const TcpSeg& seg)
{
    const quint32 synOff = seg.syn ? 1u : 0u;
    if (!base_.has_value())
        base_ = seg.seq + synOff;
    if (seg.payload.isEmpty())
        return;
    const quint32 base = *base_;
    quint32 rel = seg.seq - base;
    if (rel > 0x80000000u)
        rel = 0; // SYN 前/回绕容错
    total_ += quint64(seg.payload.size());
    auto it = frags_.find(rel);
    if (it != frags_.end()) {
        if (seg.payload.size() > it->second.size())
            it->second = seg.payload; // 同偏移长者胜
    } else {
        frags_.emplace(rel, seg.payload);
    }
}

QByteArray TcpStreamBuf::assemble() const
{
    QByteArray out;
    out.reserve(int(total_));
    quint32 expect = 0;
    for (const auto& [rel, frag] : frags_) {
        if (rel != expect) {
            if (rel < expect) {
                const quint32 skip = expect - rel;
                if (skip < quint32(frag.size())) {
                    out += frag.mid(int(skip));
                    expect += quint32(frag.size());
                }
            }
            continue;
        }
        out += frag;
        expect += quint32(frag.size());
    }
    return out;
}

QVector<TcpConn> assembleStreams(const QVector<TcpSeg>& segs)
{
    QVector<TcpConn> conns;
    std::map<std::pair<quint64, quint64>, int> index;
    for (const TcpSeg& s : segs) {
        const quint64 sk = epKey(s.src);
        const quint64 dk = epKey(s.dst);
        // 规范化：小端点在前（对应 Rust 的 (port, ip) 元组比较）
        const bool flip = sk > dk;
        const std::pair<quint64, quint64> key = flip ? std::make_pair(dk, sk)
                                                     : std::make_pair(sk, dk);
        auto it = index.find(key);
        int i;
        if (it == index.end()) {
            conns.push_back(TcpConn { epFromKey(key.first), epFromKey(key.second), {},
                                      TcpStreamBuf {}, s.tsUs });
            i = int(conns.size()) - 1;
            index.emplace(key, i);
        } else {
            i = it->second;
        }
        if (flip)
            conns[i].b2a.push(s);
        else
            conns[i].a2b.push(s);
    }
    return conns;
}

// ===================== TLS 记录切分 =====================

QVector<QPair<quint8, QByteArray>> splitRecords(const QByteArray& buf)
{
    QVector<QPair<quint8, QByteArray>> out;
    qsizetype off = 0;
    while (off + 5 <= buf.size()) {
        const int len = (uchar(buf.at(off + 3)) << 8) | uchar(buf.at(off + 4));
        if (off + 5 + len > buf.size())
            break; // 尾部不完整（抓包窗口边界）
        out.push_back(qMakePair(quint8(uchar(buf.at(off))), buf.mid(int(off + 5), len)));
        off += 5 + len;
    }
    return out;
}

// ===================== TLS 1.3 解密 =====================

std::optional<QByteArray> decryptDirection(const QByteArray& stream, const KeyLog& kl,
                                           bool isClient)
{
    const char* hsLabel = isClient ? "CLIENT_HANDSHAKE_TRAFFIC_SECRET"
                                   : "SERVER_HANDSHAKE_TRAFFIC_SECRET";
    const char* appLabel = isClient ? "CLIENT_TRAFFIC_SECRET_0" : "SERVER_TRAFFIC_SECRET_0";

    static const HashSel hashes[2] = { { "SHA2-256", 64 }, { "SHA2-384", 128 } };

    // 密钥候选 (key, iv)：应用密钥优先（多数记录），握手密钥兜底；
    // 同一 secret 推 16B 与 32B 两种 key 长度（AES-128/AES-256/ChaCha）。
    struct Cand {
        QByteArray key, iv;
    };
    QVector<Cand> cands;
    for (const char* label : { appLabel, hsLabel }) {
        const auto it = kl.secrets.constFind(QString::fromLatin1(label));
        if (it == kl.secrets.constEnd())
            continue;
        for (const QByteArray& sec : it.value()) {
            for (const HashSel& h : hashes) {
                Cand c16, c32;
                c16.key = expandLabel(h, sec, "key", QByteArray(), 16);
                c16.iv = expandLabel(h, sec, "iv", QByteArray(), 12);
                c32.key = expandLabel(h, sec, "key", QByteArray(), 32);
                c32.iv = c16.iv;
                cands.push_back(std::move(c16));
                cands.push_back(std::move(c32));
            }
        }
    }

    // 每个候选独立成功计数器作为记录序列号（AEAD tag 即真值，错误密钥/错误
    // 序号都会 tag 失败——免跟踪密钥切换点与所属连接）。
    QVector<quint64> counters(cands.size(), 0);
    QByteArray out;
    for (const auto& rec : splitRecords(stream)) {
        const quint8 typ = rec.first;
        const QByteArray& payload = rec.second;
        if (typ != kCtApp)
            continue; // 明文握手在前，密文握手记录不参与方向明文
        bool hit = false;
        QByteArray pt;
        for (int ci = 0; ci < cands.size(); ++ci) {
            const auto opened = openRecord(payload, cands[ci].key, cands[ci].iv, counters[ci]);
            if (opened) {
                ++counters[ci];
                pt = *opened;
                hit = true;
                break;
            }
        }
        if (!hit)
            continue; // 所有候选失败（抓包窗口边界/其他连接密钥缺失）
        // 内部明文：剥 padding 零 + 1 字节 content-type（TLS1.3 每条记录必带）。
        while (!pt.isEmpty() && pt.back() == '\0')
            pt.chop(1);
        if (!pt.isEmpty() && uchar(pt.back()) <= 26) {
            const uchar innerType = uchar(pt.back());
            pt.chop(1);
            // 仅保留应用数据（type 23）——解出的 Finished/NST 握手残片不进明文
            //（Rust 版无此过滤，见 pcap_decrypt.hpp 头注差异说明）
            if (innerType != kCtApp)
                continue;
        }
        out += pt;
    }
    if (out.isEmpty())
        return std::nullopt;
    return out;
}

// ===================== TLS 1.2 解密（CLIENT_RANDOM keylog） =====================

std::optional<QByteArray> decryptTls12(const QByteArray& clientStream,
                                       const QByteArray& serverStream, const KeyLog& kl,
                                       bool wantClient)
{
    // 双向流都要：握手随机数与套件取自双方 Hello
    std::optional<QByteArray> cr, sr;
    quint16 code = 0;
    bool haveSuite = false;
    for (const auto& rec : splitRecords(clientStream)) {
        if (rec.first == kCtHandshake && rec.second.size() >= 35
            && uchar(rec.second.at(0)) == 1)
            cr = rec.second.mid(6, 32);
    }
    for (const auto& rec : splitRecords(serverStream)) {
        if (rec.first == kCtHandshake && rec.second.size() >= 40
            && uchar(rec.second.at(0)) == 2) {
            // ServerHello：legacy_version(2)+random(32)+sess_id_len+…+cipher(2)
            const int sl = uchar(rec.second.at(38));
            if (rec.second.size() >= qsizetype(41 + sl)) {
                sr = rec.second.mid(6, 32);
                code = (quint16(uchar(rec.second.at(39 + sl))) << 8)
                       | quint16(uchar(rec.second.at(40 + sl)));
                haveSuite = true;
            }
        }
    }
    if (!cr || !sr || !haveSuite)
        return std::nullopt;
    const auto masterIt = kl.clientRandom.constFind(*cr);
    if (masterIt == kl.clientRandom.constEnd())
        return std::nullopt;
    const QByteArray& master = masterIt.value();

    struct Suite12 {
        int keyLen;
        int ivLen; // GCM: 4B 隐式（另 8B 显式在记录内）；ChaCha: 12B 隐式
        bool sha384;
        bool chacha;
    };
    Suite12 suite;
    switch (code) {
    case 0xc02b:
    case 0xc02c:
    case 0xc02f:
        suite = { 16, 4, false, false };
        break;
    case 0xc030:
        suite = { 32, 4, true, false };
        break;
    case 0xcc13:
    case 0xcc14:
    case 0xcc15:
    case 0xcc16:
        suite = { 32, 12, false, true };
        break;
    default:
        return std::nullopt;
    }
    // key block: client_write_key || server_write_key || client_write_iv || server_write_iv
    const int need = suite.keyLen * 2 + suite.ivLen * 2;
    QByteArray seed = *sr;
    seed += *cr;
    const HashSel h = suite.sha384 ? HashSel { "SHA2-384", 128 } : HashSel { "SHA2-256", 64 };
    const QByteArray kb = prf12(h, master, "key expansion", seed, need);
    if (kb.size() < need)
        return std::nullopt;
    const QByteArray ckey = kb.mid(0, suite.keyLen);
    const QByteArray skey = kb.mid(suite.keyLen, suite.keyLen);
    const QByteArray civ = kb.mid(suite.keyLen * 2, suite.ivLen);
    const QByteArray siv = kb.mid(suite.keyLen * 2 + suite.ivLen, suite.ivLen);
    const QByteArray& key = wantClient ? ckey : skey;
    const QByteArray& iv = wantClient ? civ : siv;
    const QByteArray& stream = wantClient ? clientStream : serverStream;

    QByteArray out;
    quint64 seq = 0;
    bool afterCcs = false;
    for (const auto& rec : splitRecords(stream)) {
        const quint8 typ = rec.first;
        const QByteArray& payload = rec.second;
        if (typ == 20) {
            seq = 0; // ChangeCipherSpec：序列号重新开始
            afterCcs = true;
            continue;
        }
        if (!afterCcs)
            continue; // CCS 之前全是明文握手
        // CCS 之后所有记录都是密文（含加密的 Finished），都占序列号
        if (typ != kCtHandshake && typ != kCtApp) {
            ++seq;
            continue;
        }
        unsigned char nonce[12];
        unsigned char aad[13];
        const uchar* body = nullptr;
        qsizetype bodyLen = 0;
        int plen = 0;
        if (suite.chacha) {
            if (iv.size() < 12)
                return std::nullopt;
            std::memcpy(nonce, iv.constData(), 12);
            for (int i = 0; i < 8; ++i)
                nonce[4 + i] ^= uchar(seq >> (56 - 8 * i));
            body = reinterpret_cast<const uchar*>(payload.constData());
            bodyLen = payload.size();
            plen = int(payload.size()) - 16;
        } else {
            // GCM：记录前 8B 显式 nonce
            if (payload.size() < qsizetype(8 + 16) || iv.size() < 4) {
                ++seq;
                continue;
            }
            std::memcpy(nonce, iv.constData(), 4);
            std::memcpy(nonce + 4, payload.constData(), 8);
            body = reinterpret_cast<const uchar*>(payload.constData()) + 8;
            bodyLen = payload.size() - 8;
            plen = int(payload.size()) - 24;
        }
        for (int i = 0; i < 8; ++i)
            aad[i] = uchar(seq >> (56 - 8 * i));
        aad[8] = typ; // AAD 类型字节=记录实际类型（0x16 加密握手 / 0x17 应用数据）
        aad[9] = 0x03;
        aad[10] = 0x03;
        // 长度字段 = TLSCompressed 明文长度（载荷 − 显式 nonce − tag；ChaCha 无显式 nonce）
        aad[11] = uchar(plen >> 8);
        aad[12] = uchar(plen);
        const qsizetype ctLen = bodyLen >= 16 ? bodyLen - 16 : 0;
        QByteArray pt;
        const AeadAlgo algo = suite.chacha ? AeadAlgo::Chacha20Poly1305
                                           : (suite.keyLen == 32 ? AeadAlgo::Aes256Gcm
                                                                 : AeadAlgo::Aes128Gcm);
        if (aeadOpen(algo, key, nonce, aad, sizeof(aad), body, size_t(ctLen), body + ctLen, &pt)
            && typ == kCtApp) {
            out += pt; // 只取应用数据（22 是加密握手）
        }
        ++seq;
    }
    if (out.isEmpty())
        return std::nullopt;
    return out;
}

// ===================== 完整管线 =====================

QVector<DecryptedStream> decryptPcap(const QByteArray& pcap, const QString& keylogText)
{
    const KeyLog kl = KeyLog::parse(keylogText);
    QVector<PcapPacket> pkts;
    LinkType link;
    if (auto f = readPcapLink(pcap)) {
        pkts = std::move(f.value().packets);
        link = f.value().link;
    }
    const QVector<TcpSeg> segs = extractTcpSegs(pkts, link);
    const QVector<TcpConn> conns = assembleStreams(segs);
    QVector<DecryptedStream> out;
    for (const TcpConn& c : conns) {
        const QByteArray x = c.a2b.assemble();
        const QByteArray y = c.b2a.assemble();
        if (x.isEmpty() || y.isEmpty())
            continue;
        // 方向判定：流首条 handshake 记录的消息类型 1=ClientHello（比端口更通用）
        const auto startsClientHello = [](const QByteArray& s) {
            return s.size() >= 6 && uchar(s.at(0)) == kCtHandshake && uchar(s.at(3)) < 0x40
                   && uchar(s.at(5)) == 1;
        };
        const bool xCh = startsClientHello(x);
        const bool yCh = startsClientHello(y);
        if (!xCh && !yCh)
            continue;
        const QByteArray& client = xCh ? x : y;
        const QByteArray& server = xCh ? y : x;
        std::optional<QByteArray> cpl = decryptDirection(client, kl, true);
        if (!cpl)
            cpl = decryptTls12(client, server, kl, true);
        std::optional<QByteArray> spl = decryptDirection(server, kl, false);
        if (!spl)
            spl = decryptTls12(client, server, kl, false);
        if (cpl || spl) {
            DecryptedStream ds;
            ds.client = xCh ? c.a : c.b;
            ds.server = xCh ? c.b : c.a;
            ds.clientPlain = cpl ? *cpl : QByteArray();
            ds.serverPlain = spl ? *spl : QByteArray();
            out.push_back(std::move(ds));
        }
    }
    return out;
}

} // namespace mw::capture
