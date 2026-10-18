#include "compress.hpp"

#include <QCryptographicHash>

#include <zlib.h>

namespace mw {

mw::Result<QByteArray> gzipDecompress(const QByteArray& gzData)
{
    if (gzData.isEmpty())
        return mw::err<QByteArray>(QStringLiteral("空输入"));

    z_stream zs{};
    // 15 + 16 = zlib inflate + gzip 头；32 = 自动识别 zlib/gzip
    if (inflateInit2(&zs, 15 + 32) != Z_OK)
        return mw::err<QByteArray>(QStringLiteral("inflateInit2 失败"));

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(gzData.constData()));
    zs.avail_in = static_cast<uInt>(gzData.size());

    QByteArray out;
    out.reserve(gzData.size() * 4);
    char buf[65536];
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            const QString msg = QString::fromLatin1(zs.msg ? zs.msg : "inflate 错误");
            inflateEnd(&zs);
            return mw::err<QByteArray>(msg);
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (ret == Z_BUF_ERROR && zs.avail_in == 0)
            break; // 输入耗尽
    }
    inflateEnd(&zs);
    return mw::Result<QByteArray>::Ok(out);
}

} // namespace mw
