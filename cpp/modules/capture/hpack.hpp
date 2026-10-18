// HPACK 解码器 ← crates: hpack-0.3（RFC 7541）
// 供 flow.cpp 的 HTTP/2 头部块解码使用；跨块保持动态表状态（CONTINUATION）。
#pragma once

#include <QByteArray>
#include <QPair>
#include <QVector>

namespace mw::capture::hpack {

// 解码一个完整的 header block（已去掉 PADDED/优先级前缀的 HEADERS 载荷）。
// 成功返回 true 并追加 (name, value) 对；失败返回 false（调用方丢弃该块，
// 与 Rust 侧 decoder.decode(...).is_err() 行为一致）。
class Decoder {
public:
    Decoder();

    bool decode(const QByteArray& block, QVector<QPair<QByteArray, QByteArray>>* out);

private:
    struct Entry {
        QByteArray name;
        QByteArray value;
    };

    void insertDynamic(const QByteArray& name, const QByteArray& value);
    bool getFromTable(quint64 index, Entry* out) const;
    bool readString(const QByteArray& buf, qsizetype* pos, QByteArray* out);
    void setMaxDynamicSize(quint64 maxSize);

    QVector<Entry> dynamic_; // front = 最新（索引 62）
    quint64 dynSize_ = 0;    // 字节（name+value+32/条）
    quint64 maxDynSize_ = 4096;
};

} // namespace mw::capture::hpack
