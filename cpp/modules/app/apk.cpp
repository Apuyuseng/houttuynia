// apk.rs 的 C++ 移植：EOCD 尾扫 + 中央目录遍历 + local header 提取 + raw inflate。
#include "apk.hpp"

#include "types.hpp"

#include <QFile>

#include <zlib.h>

namespace mw::app {
namespace {

constexpr quint32 kEocdSig = 0x06054b50;
constexpr quint32 kCenSig = 0x02014b50;
constexpr quint32 kLocSig = 0x04034b50;
// EOCD 最小 22 字节 + 注释最长 64KB，从尾部扫这么多即可
constexpr qsizetype kEocdScanWindow = 22 + 65535;

quint16 u16At(const QByteArray& d, qsizetype p)
{
    return quint16((quint8)d[p] | ((quint8)d[p + 1] << 8));
}

quint32 u32At(const QByteArray& d, qsizetype p)
{
    return quint32((quint8)d[p]) | (quint32((quint8)d[p + 1]) << 8)
        | (quint32((quint8)d[p + 2]) << 16) | (quint32((quint8)d[p + 3]) << 24);
}

} // namespace

mw::Result<QByteArray> rawDeflate(const QByteArray& compressed)
{
    QByteArray out;
    z_stream zs{};
    // 负窗口位 = raw deflate（无 zlib/gzip 头）
    if (inflateInit2(&zs, -15) != Z_OK)
        return mw::err<QByteArray>(QStringLiteral("inflateInit2 失败"));
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
    zs.avail_in = uInt(compressed.size());
    std::vector<char> buf(64 * 1024);
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf.data());
        zs.avail_out = uInt(buf.size());
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            const QString reason = QString::fromLocal8Bit(zs.msg ? zs.msg : "zlib error");
            inflateEnd(&zs);
            return mw::err<QByteArray>(QStringLiteral("deflate 解压失败: %1").arg(reason));
        }
        out.append(buf.data(), int(buf.size()) - int(zs.avail_out));
        if (ret == Z_BUF_ERROR && zs.avail_in == 0)
            break; // 输入耗尽但流未结束（截断）：下方按错误处理
    } while (ret != Z_STREAM_END);
    const bool truncated = ret != Z_STREAM_END;
    inflateEnd(&zs);
    if (truncated)
        return mw::err<QByteArray>(QStringLiteral("deflate 解压失败: 输入流截断"));
    return mw::Result<QByteArray>::Ok(std::move(out));
}

mw::Result<QByteArray> rawCompress(const QByteArray& data)
{
    QByteArray out;
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return mw::err<QByteArray>(QStringLiteral("deflateInit2 失败"));
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    zs.avail_in = uInt(data.size());
    std::vector<char> buf(64 * 1024);
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf.data());
        zs.avail_out = uInt(buf.size());
        ret = deflate(&zs, Z_FINISH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            deflateEnd(&zs);
            return mw::err<QByteArray>(QStringLiteral("deflate 压缩失败"));
        }
        out.append(buf.data(), int(buf.size()) - int(zs.avail_out));
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return mw::Result<QByteArray>::Ok(std::move(out));
}

mw::Result<ZipArchive> ZipArchive::open(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return mw::err<ZipArchive>(errIo(
            QStringLiteral("无法读取 %1: %2").arg(path, f.errorString())));
    return fromBytes(f.readAll(), path);
}

mw::Result<ZipArchive> ZipArchive::fromBytes(const QByteArray& data, const QString& label)
{
    ZipArchive z;
    QString reason;
    if (!readCentralDirectory(data, z.entries_, reason))
        return mw::err<ZipArchive>(errApkParse(label, reason));
    z.data_ = data;
    return mw::Result<ZipArchive>::Ok(std::move(z));
}

bool ZipArchive::readCentralDirectory(const QByteArray& data, std::vector<CentralEntry>& out,
                                      QString& err)
{
    if (data.size() < 22) {
        err = QStringLiteral("文件小于 EOCD 最小长度");
        return false;
    }
    // 从尾部向前找 EOCD 签名
    const qsizetype scanStart = qMax<qsizetype>(0, data.size() - kEocdScanWindow);
    qsizetype eocd = -1;
    for (qsizetype i = data.size() - 22; i >= scanStart; --i) {
        if (u32At(data, i) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        err = QStringLiteral("未找到 EOCD 记录");
        return false;
    }
    const quint64 total = u16At(data, eocd + 10); // 总 entry 数
    const quint64 cdSize = u32At(data, eocd + 12);
    const quint64 cdOff = u32At(data, eocd + 16);
    if (cdOff + cdSize > quint64(data.size())) {
        err = QStringLiteral("中央目录越界: off=%1 size=%2 file=%3")
                  .arg(cdOff)
                  .arg(cdSize)
                  .arg(data.size());
        return false;
    }

    out.clear();
    out.reserve(int(total));
    quint64 p = cdOff;
    for (quint64 i = 0; i < total; ++i) {
        if (p + 46 > quint64(data.size()) || u32At(data, p) != kCenSig) {
            err = QStringLiteral("中央目录 entry 损坏");
            return false;
        }
        CentralEntry e;
        e.method = u16At(data, p + 10);
        e.compSize = u32At(data, p + 20);
        e.uncompSize = u32At(data, p + 24);
        const quint64 nameLen = u16At(data, p + 28);
        const quint64 extraLen = u16At(data, p + 30);
        const quint64 commentLen = u16At(data, p + 32);
        e.localOffset = u32At(data, p + 42);
        e.name = QString::fromUtf8(data.constData() + p + 46, int(nameLen));
        out.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

QStringList ZipArchive::entryNames() const
{
    QStringList names;
    names.reserve(int(entries_.size()));
    for (const auto& e : entries_)
        names << e.name;
    return names;
}

bool ZipArchive::hasEntry(const QString& name) const
{
    for (const auto& e : entries_)
        if (e.name == name)
            return true;
    return false;
}

mw::Result<std::optional<QByteArray>> ZipArchive::readEntry(const QString& name) const
{
    const CentralEntry* entry = nullptr;
    for (const auto& e : entries_) {
        if (e.name == name) {
            entry = &e;
            break;
        }
    }
    if (!entry)
        return mw::Result<std::optional<QByteArray>>::Ok(std::nullopt);

    const QByteArray& d = data_;
    const quint64 off = entry->localOffset;
    if (off + 30 > quint64(d.size()))
        return mw::err<std::optional<QByteArray>>(
            errApkParse(name, QStringLiteral("本地头越界")));
    if (u32At(d, off) != kLocSig)
        return mw::err<std::optional<QByteArray>>(
            errApkParse(name, QStringLiteral("本地头签名不匹配")));
    const quint64 nameLen = u16At(d, off + 26);
    const quint64 extraLen = u16At(d, off + 28);
    const quint64 dataOff = off + 30 + nameLen + extraLen;
    const quint64 dataEnd = dataOff + entry->compSize;
    if (dataEnd > quint64(d.size()))
        return mw::err<std::optional<QByteArray>>(
            errApkParse(name, QStringLiteral("entry 数据越界")));

    const QByteArray raw = d.mid(dataOff, entry->compSize);
    switch (entry->method) {
    case 0:
        return mw::Result<std::optional<QByteArray>>::Ok(raw);
    case 8: {
        auto out = rawDeflate(raw);
        if (!out)
            return mw::err<std::optional<QByteArray>>(
                errApkParse(name, QStringLiteral("deflate 解压失败: %1").arg(out.error())));
        return mw::Result<std::optional<QByteArray>>::Ok(std::move(out.value()));
    }
    default:
        return mw::err<std::optional<QByteArray>>(
            errApkParse(name, QStringLiteral("不支持的压缩方法 %1").arg(entry->method)));
    }
}

} // namespace mw::app
