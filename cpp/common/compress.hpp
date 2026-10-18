// 压缩与哈希工具：gzip 解压（F-Droid 索引）、sha256/md5（下载校验）。
#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

#include "result.hpp"

namespace mw {

// gzip 流解压（zlib inflate with gzip window）
mw::Result<QByteArray> gzipDecompress(const QByteArray& gzData);

inline QByteArray sha256(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}
inline QByteArray md5(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
}

// 流式哈希（大文件分块喂入）
class IncrementalHash {
public:
    explicit IncrementalHash(QCryptographicHash::Algorithm algo)
        : h_(algo) {}
    void add(const QByteArray& chunk) { h_.addData(chunk); }
    QByteArray resultHex() const { return h_.result().toHex(); }
    void reset() { h_.reset(); }
private:
    QCryptographicHash h_;
};

} // namespace mw
