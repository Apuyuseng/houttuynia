// HPACK 解码器实现：整数前缀 / 字面量（Huffman）/ 索引引用 / 动态表。
// 对应 Rust 侧 hpack-0.3 crate 的 Decoder（只解码，不编码）。
#include "hpack.hpp"

#include <QHash>

#include <array>
#include <cstdint>

namespace mw::capture::hpack {

namespace {

// RFC 7541 Appendix A 静态表（1-based 索引 1..61）。
struct StaticEntry {
    const char* name;
    const char* value;
};
const StaticEntry kStaticTable[] = {
    { ":authority", "" },
    { ":method", "GET" },
    { ":method", "POST" },
    { ":path", "/" },
    { ":path", "/index.html" },
    { ":scheme", "http" },
    { ":scheme", "https" },
    { ":status", "200" },
    { ":status", "204" },
    { ":status", "206" },
    { ":status", "304" },
    { ":status", "400" },
    { ":status", "404" },
    { ":status", "500" },
    { "accept-charset", "" },
    { "accept-encoding", "gzip, deflate" },
    { "accept-language", "" },
    { "accept-ranges", "" },
    { "accept", "" },
    { "access-control-allow-origin", "" },
    { "age", "" },
    { "allow", "" },
    { "authorization", "" },
    { "cache-control", "" },
    { "content-disposition", "" },
    { "content-encoding", "" },
    { "content-language", "" },
    { "content-length", "" },
    { "content-location", "" },
    { "content-range", "" },
    { "content-type", "" },
    { "cookie", "" },
    { "date", "" },
    { "etag", "" },
    { "expect", "" },
    { "expires", "" },
    { "from", "" },
    { "host", "" },
    { "if-match", "" },
    { "if-modified-since", "" },
    { "if-none-match", "" },
    { "if-range", "" },
    { "if-unmodified-since", "" },
    { "last-modified", "" },
    { "link", "" },
    { "location", "" },
    { "max-forwards", "" },
    { "proxy-authenticate", "" },
    { "proxy-authorization", "" },
    { "range", "" },
    { "referer", "" },
    { "refresh", "" },
    { "retry-after", "" },
    { "server", "" },
    { "set-cookie", "" },
    { "strict-transport-security", "" },
    { "transfer-encoding", "" },
    { "user-agent", "" },
    { "vary", "" },
    { "via", "" },
    { "www-authenticate", "" },
};
constexpr int kStaticTableLen = int(sizeof(kStaticTable) / sizeof(kStaticTable[0]));

// RFC 7541 Appendix B 规范 Huffman 码表：符号 0..256（256=EOS）。
struct HuffCode {
    uint32_t code;
    uint8_t bits;
};
const HuffCode kHuffmanTable[257] = {
    { 0x1ff8, 13 }, { 0x7fffd8, 23 }, { 0xfffffe2, 28 }, { 0xfffffe3, 28 },
    { 0xfffffe4, 28 }, { 0xfffffe5, 28 }, { 0xfffffe6, 28 }, { 0xfffffe7, 28 },
    { 0xfffffe8, 28 }, { 0xffffea, 24 }, { 0x3ffffffc, 30 }, { 0xfffffe9, 28 },
    { 0xfffffea, 28 }, { 0x3ffffffd, 30 }, { 0xfffffeb, 28 }, { 0xfffffec, 28 },
    { 0xfffffed, 28 }, { 0xfffffee, 28 }, { 0xfffffef, 28 }, { 0xffffff0, 28 },
    { 0xffffff1, 28 }, { 0xffffff2, 28 }, { 0x3ffffffe, 30 }, { 0xffffff3, 28 },
    { 0xffffff4, 28 }, { 0xffffff5, 28 }, { 0xffffff6, 28 }, { 0xffffff7, 28 },
    { 0xffffff8, 28 }, { 0xffffff9, 28 }, { 0xffffffa, 28 }, { 0xffffffb, 28 },
    { 0x14, 6 }, { 0x3f8, 10 }, { 0x3f9, 10 }, { 0xffa, 12 },
    { 0x1ff9, 13 }, { 0x15, 6 }, { 0xf8, 8 }, { 0x7fa, 11 },
    { 0x3fa, 10 }, { 0x3fb, 10 }, { 0xf9, 8 }, { 0x7fb, 11 },
    { 0xfa, 8 }, { 0x16, 6 }, { 0x17, 6 }, { 0x18, 6 },
    { 0x0, 5 }, { 0x1, 5 }, { 0x2, 5 }, { 0x19, 6 },
    { 0x1a, 6 }, { 0x1b, 6 }, { 0x1c, 6 }, { 0x1d, 6 },
    { 0x1e, 6 }, { 0x1f, 6 }, { 0x5c, 7 }, { 0xfb, 8 },
    { 0x7ffc, 15 }, { 0x20, 6 }, { 0xffb, 12 }, { 0x3fc, 10 },
    { 0x1ffa, 13 }, { 0x21, 6 }, { 0x5d, 7 }, { 0x5e, 7 },
    { 0x5f, 7 }, { 0x60, 7 }, { 0x61, 7 }, { 0x62, 7 },
    { 0x63, 7 }, { 0x64, 7 }, { 0x65, 7 }, { 0x66, 7 },
    { 0x67, 7 }, { 0x68, 7 }, { 0x69, 7 }, { 0x6a, 7 },
    { 0x6b, 7 }, { 0x6c, 7 }, { 0x6d, 7 }, { 0x6e, 7 },
    { 0x6f, 7 }, { 0x70, 7 }, { 0x71, 7 }, { 0x72, 7 },
    { 0xfc, 8 }, { 0x73, 7 }, { 0xfd, 8 }, { 0x1ffb, 13 },
    { 0x7fff0, 19 }, { 0x1ffc, 13 }, { 0x3ffc, 14 }, { 0x22, 6 },
    { 0x7ffd, 15 }, { 0x3, 5 }, { 0x23, 6 }, { 0x4, 5 },
    { 0x24, 6 }, { 0x5, 5 }, { 0x25, 6 }, { 0x26, 6 },
    { 0x27, 6 }, { 0x6, 5 }, { 0x74, 7 }, { 0x75, 7 },
    { 0x28, 6 }, { 0x29, 6 }, { 0x2a, 6 }, { 0x7, 5 },
    { 0x2b, 6 }, { 0x76, 7 }, { 0x2c, 6 }, { 0x8, 5 },
    { 0x9, 5 }, { 0x2d, 6 }, { 0x77, 7 }, { 0x78, 7 },
    { 0x79, 7 }, { 0x7a, 7 }, { 0x7b, 7 }, { 0x7ffe, 15 },
    { 0x7fc, 11 }, { 0x3ffd, 14 }, { 0x1ffd, 13 }, { 0xffffffc, 28 },
    { 0xfffe6, 20 }, { 0x3fffd2, 22 }, { 0xfffe7, 20 }, { 0xfffe8, 20 },
    { 0x3fffd3, 22 }, { 0x3fffd4, 22 }, { 0x7fffd9, 23 }, { 0x3fffd6, 22 },
    { 0x7fffda, 23 }, { 0x7fffdb, 23 }, { 0x7fffdc, 23 }, { 0x7fffdd, 23 },
    { 0x7fffde, 23 }, { 0xffffeb, 24 }, { 0x7fffdf, 23 }, { 0xffffec, 24 },
    { 0xffffed, 24 }, { 0x3fffd7, 22 }, { 0x7fffe0, 23 }, { 0xffffee, 24 },
    { 0x7fffe1, 23 }, { 0x7fffe2, 23 }, { 0x7fffe3, 23 }, { 0x7fffe4, 23 },
    { 0x1fffdc, 21 }, { 0x3fffd8, 22 }, { 0x7fffe5, 23 }, { 0x3fffd9, 22 },
    { 0x7fffe6, 23 }, { 0x7fffe7, 23 }, { 0xffffef, 24 }, { 0x3fffda, 22 },
    { 0x1fffdd, 21 }, { 0xfffe9, 20 }, { 0x3fffdb, 22 }, { 0x3fffdc, 22 },
    { 0x7fffe8, 23 }, { 0x7fffe9, 23 }, { 0x1fffde, 21 }, { 0x7fffea, 23 },
    { 0x3fffdd, 22 }, { 0x3fffde, 22 }, { 0xfffff0, 24 }, { 0x1fffdf, 21 },
    { 0x3fffdf, 22 }, { 0x7fffeb, 23 }, { 0x7fffec, 23 }, { 0x1fffe0, 21 },
    { 0x1fffe1, 21 }, { 0x3fffe0, 22 }, { 0x1fffe2, 21 }, { 0x7fffed, 23 },
    { 0x3fffe1, 22 }, { 0x7fffee, 23 }, { 0x7fffef, 23 }, { 0xfffea, 20 },
    { 0x3fffe2, 22 }, { 0x3fffe3, 22 }, { 0x3fffe4, 22 }, { 0x7ffff0, 23 },
    { 0x3fffe5, 22 }, { 0x3fffe6, 22 }, { 0x7ffff1, 23 }, { 0x3ffffe0, 26 },
    { 0x3ffffe1, 26 }, { 0xfffeb, 20 }, { 0x7fff1, 19 }, { 0x3fffe7, 22 },
    { 0x7ffff2, 23 }, { 0x3fffe8, 22 }, { 0x1ffffec, 25 }, { 0x3ffffe2, 26 },
    { 0x3ffffe3, 26 }, { 0x3ffffe4, 26 }, { 0x7ffffde, 27 }, { 0x7ffffdf, 27 },
    { 0x3ffffe5, 26 }, { 0xfffff1, 24 }, { 0x1ffffed, 25 }, { 0x7fff2, 19 },
    { 0x1fffe3, 21 }, { 0x3ffffe6, 26 }, { 0x7ffffe0, 27 }, { 0x7ffffe1, 27 },
    { 0x3ffffe7, 26 }, { 0x7ffffe2, 27 }, { 0xfffff2, 24 }, { 0x1fffe4, 21 },
    { 0x1fffe5, 21 }, { 0x3ffffe8, 26 }, { 0x3ffffe9, 26 }, { 0xffffffd, 28 },
    { 0x7ffffe3, 27 }, { 0x7ffffe4, 27 }, { 0x7ffffe5, 27 }, { 0xfffec, 20 },
    { 0xfffff3, 24 }, { 0xfffed, 20 }, { 0x1fffe6, 21 }, { 0x3fffe9, 22 },
    { 0x1fffe7, 21 }, { 0x1fffe8, 21 }, { 0x7ffff3, 23 }, { 0x3fffea, 22 },
    { 0x3fffeb, 22 }, { 0x1ffffee, 25 }, { 0x1ffffef, 25 }, { 0xfffff4, 24 },
    { 0xfffff5, 24 }, { 0x3ffffea, 26 }, { 0x7ffff4, 23 }, { 0x3ffffeb, 26 },
    { 0x7ffffe6, 27 }, { 0x3ffffec, 26 }, { 0x3ffffed, 26 }, { 0x7ffffe7, 27 },
    { 0x7ffffe8, 27 }, { 0x7ffffe9, 27 }, { 0x7ffffea, 27 }, { 0x7ffffeb, 27 },
    { 0xffffffe, 28 }, { 0x7ffffec, 27 }, { 0x7ffffed, 27 }, { 0x7ffffee, 27 },
    { 0x7ffffef, 27 }, { 0x7fffff0, 27 }, { 0x3ffffee, 26 }, { 0x3fffffff, 30 }, // EOS
};
static_assert(sizeof(kHuffmanTable) / sizeof(kHuffmanTable[0]) == 257,
              "Huffman 码表必须恰好 257 个符号");

// 长度 → (code → symbol) 的查找表（静态构造一次）。
struct HuffmanLookup {
    // bits 5..30
    std::array<QHash<uint32_t, int>, 31> byLen;

    HuffmanLookup()
    {
        for (int sym = 0; sym < 257; ++sym)
            byLen[kHuffmanTable[sym].bits].insert(kHuffmanTable[sym].code, sym);
    }
    const QHash<uint32_t, int>* forLen(int len) const
    {
        if (len < 0 || len > 30)
            return nullptr;
        const auto& h = byLen[size_t(len)];
        return h.isEmpty() ? nullptr : &h;
    }
};
const HuffmanLookup& huffmanLookup()
{
    static const HuffmanLookup lookup;
    return lookup;
}

// Huffman 解码（与 hpack-0.3 一致的逐位游走 + padding 校验）。
bool huffmanDecode(const QByteArray& buf, QByteArray* out)
{
    const auto& table = huffmanLookup();
    uint32_t current = 0;
    int currentLen = 0;
    for (const char c : buf) {
        const uint8_t b = uint8_t(c);
        for (int bit = 7; bit >= 0; --bit) {
            current <<= 1;
            if (b & (1u << bit))
                current |= 1;
            ++currentLen;
            const auto* lenTable = table.forLen(currentLen);
            if (lenTable) {
                const auto it = lenTable->constFind(current);
                if (it != lenTable->cend()) {
                    const int sym = it.value();
                    if (sym == 256)
                        return false; // EOS 出现在字符串中：错误
                    out->append(char(uint8_t(sym)));
                    current = 0;
                    currentLen = 0;
                }
            }
        }
    }
    // padding 不得长于 7 位，且必须是 EOS（全 1）的最高位前缀。
    if (currentLen > 7)
        return false;
    if (currentLen > 0 && current != ((1u << currentLen) - 1))
        return false;
    return true;
}

// 整数前缀解码：返回 (value, consumed)。
struct IntResult {
    uint64_t value = 0;
    qsizetype consumed = 0;
};
bool decodeInteger(const QByteArray& buf, qsizetype pos, int prefix, IntResult* out)
{
    if (pos >= buf.size())
        return false;
    const uint8_t mask = prefix == 8 ? 0xFF : uint8_t((1u << prefix) - 1);
    uint64_t value = uint8_t(buf.at(pos)) & mask;
    if (value < mask) {
        out->value = value;
        out->consumed = 1;
        return true;
    }
    qsizetype total = 1;
    int m = 0;
    for (qsizetype i = pos + 1; i < buf.size(); ++i) {
        ++total;
        const uint8_t b = uint8_t(buf.at(i));
        value += uint64_t(b & 127) << m;
        m += 7;
        if ((b & 128) == 0) {
            out->value = value;
            out->consumed = total;
            return true;
        }
        if (total == 5)
            return false; // 表示过长
    }
    return false; // 未终结
}

} // namespace

Decoder::Decoder() = default;

void Decoder::setMaxDynamicSize(quint64 maxSize)
{
    maxDynSize_ = maxSize;
    while (dynSize_ > maxDynSize_ && !dynamic_.isEmpty()) {
        const Entry& last = dynamic_.back();
        dynSize_ -= quint64(last.name.size() + last.value.size() + 32);
        dynamic_.pop_back();
    }
}

void Decoder::insertDynamic(const QByteArray& name, const QByteArray& value)
{
    dynamic_.push_front(Entry { name, value });
    dynSize_ += quint64(name.size() + value.size() + 32);
    while (dynSize_ > maxDynSize_ && !dynamic_.isEmpty()) {
        const Entry& last = dynamic_.back();
        dynSize_ -= quint64(last.name.size() + last.value.size() + 32);
        dynamic_.pop_back();
    }
}

bool Decoder::getFromTable(quint64 index, Entry* out) const
{
    if (index == 0)
        return false;
    if (index <= quint64(kStaticTableLen)) {
        const StaticEntry& e = kStaticTable[index - 1];
        out->name = QByteArray(e.name);
        out->value = QByteArray(e.value);
        return true;
    }
    const quint64 dynIdx = index - quint64(kStaticTableLen) - 1; // 0 = 最新
    if (dynIdx >= quint64(dynamic_.size()))
        return false;
    *out = dynamic_.at(int(dynIdx));
    return true;
}

bool Decoder::readString(const QByteArray& buf, qsizetype* pos, QByteArray* out)
{
    if (*pos >= buf.size())
        return false;
    const bool huff = (uint8_t(buf.at(*pos)) & 0x80) != 0;
    IntResult len;
    if (!decodeInteger(buf, *pos, 7, &len))
        return false;
    *pos += len.consumed;
    if (*pos + qsizetype(len.value) > buf.size())
        return false;
    QByteArray raw = buf.mid(*pos, int(len.value));
    *pos += qsizetype(len.value);
    if (huff)
        return huffmanDecode(raw, out);
    *out = raw;
    return true;
}

bool Decoder::decode(const QByteArray& block, QVector<QPair<QByteArray, QByteArray>>* out)
{
    qsizetype pos = 0;
    while (pos < block.size()) {
        const uint8_t b = uint8_t(block.at(pos));
        if ((b & 0x80) != 0) {
            // 索引头部字段（7 位前缀）
            IntResult idx;
            if (!decodeInteger(block, pos, 7, &idx))
                return false;
            pos += idx.consumed;
            Entry e;
            if (!getFromTable(idx.value, &e))
                return false;
            out->append(qMakePair(e.name, e.value));
        } else if ((b & 0xC0) == 0x40) {
            // 字面量 + 增量索引（6 位前缀）
            IntResult nameIdx;
            if (!decodeInteger(block, pos, 6, &nameIdx))
                return false;
            pos += nameIdx.consumed;
            QByteArray name;
            if (nameIdx.value == 0) {
                if (!readString(block, &pos, &name))
                    return false;
            } else {
                Entry e;
                if (!getFromTable(nameIdx.value, &e))
                    return false;
                name = e.name;
            }
            QByteArray value;
            if (!readString(block, &pos, &value))
                return false;
            insertDynamic(name, value);
            out->append(qMakePair(name, value));
        } else if ((b & 0xE0) == 0x20) {
            // 动态表尺寸更新（5 位前缀）
            IntResult n;
            if (!decodeInteger(block, pos, 5, &n))
                return false;
            pos += n.consumed;
            setMaxDynamicSize(n.value);
        } else {
            // 字面量不索引 / 从不索引（0x00..0x1F，4 位前缀）
            IntResult nameIdx;
            if (!decodeInteger(block, pos, 4, &nameIdx))
                return false;
            pos += nameIdx.consumed;
            QByteArray name;
            if (nameIdx.value == 0) {
                if (!readString(block, &pos, &name))
                    return false;
            } else {
                Entry e;
                if (!getFromTable(nameIdx.value, &e))
                    return false;
                name = e.name;
            }
            QByteArray value;
            if (!readString(block, &pos, &value))
                return false;
            out->append(qMakePair(name, value));
        }
    }
    return true;
}

} // namespace mw::capture::hpack
