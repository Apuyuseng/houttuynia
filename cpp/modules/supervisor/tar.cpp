// tar 流解析 + tar.zst 解包 + zip 解包 ← cloud.rs 的 extract_tar_zst / extract_zip
// （Rust 侧由 tar/zstd/zip crate 承担；C++ 侧 zstd 已链接，tar/zip 流解析自研。）
#include "supervisor.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <zstd.h>
#include <zlib.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <vector>

namespace mw::supervisor {

namespace {

// C 字符串字段（NUL 截断）→ QString
QString cstrField(const char* p, int len)
{
    return QString::fromUtf8(p, int(qstrnlen(p, size_t(len))));
}

// 8/12 字节 octal 字段 → quint64（容错前导空格/NUL；GNU base-256 高位置 1 扩展：
// 镜像单文件可超 8GB，GNU tar 超出 octal 11 位上限时启用）
quint64 octalField(const char* p, int len)
{
    if (len > 0 && (quint8(p[0]) & 0x80)) {
        quint64 v = quint64(quint8(p[0]) & 0x7f);
        for (int i = 1; i < len; ++i)
            v = (v << 8) | quint8(p[i]);
        return v;
    }
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '0'))
        ++i; // 前导 '0' 在 octal 里不影响数值
    quint64 v = 0;
    bool any = false;
    for (; i < len && p[i] >= '0' && p[i] <= '7'; ++i) {
        v = (v << 3) | quint64(p[i] - '0');
        any = true;
    }
    return any ? v : 0;
}

// 相对路径净化：拒绝绝对路径与 ".." 穿越（对齐 tar crate enclosed_name 语义）。
// 返回净化后的相对路径；空串 = 非法（穿越或为空）。
QString sanitizeRelPath(const QString& raw)
{
    QString rel = raw;
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (rel.startsWith(QLatin1Char('/')))
        rel.remove(0, 1);
    QStringList keep;
    for (const QString& part : rel.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral("."))
            continue;
        if (part == QStringLiteral(".."))
            return QString();
        keep << part;
    }
    if (keep.isEmpty())
        return QString();
    return keep.join(QLatin1Char('/'));
}

void applyMeta(const QString& path, quint32 mode, quint64 mtime)
{
    ::chmod(path.toUtf8().constData(), mode_t(mode));
    struct timespec ts[2];
    ts[0].tv_sec = time_t(mtime);
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    ::utimensat(AT_FDCWD, path.toUtf8().constData(), ts, 0);
}

} // namespace

// ===================== TarStreamParser =====================

struct TarStreamParser::Impl {
    enum class St { WantHeader, WantData, Done };

    QString destDir;
    QByteArray buf;           // 未消费缓冲
    St st = St::WantHeader;

    // 当前条目
    QString name;
    QString link;
    quint64 size = 0;         // payload 字节数
    quint64 remaining = 0;    // 未消费 payload
    quint64 paddedRemain = 0; // 含 512 对齐 padding 的未消费量
    char type = 0;
    quint32 mode = 0644;
    quint64 mtime = 0;

    // 控制载荷（'L'/'K'/'x'/'g'）收集
    QByteArray control;
    bool collectingControl = false;

    // 下一条目的名字/链接覆盖（来自 'L'/'K'/'x'）
    QString pendingName;
    QString pendingLink;

    std::unique_ptr<QFile> out;
    QStringList entries;

    explicit Impl(QString d) : destDir(std::move(d)) {}

    mw::Result<bool> startEntry(const QByteArray& hdr);
    mw::Result<bool> finishEntry();
    mw::Result<bool> writePayload(const char* p, qint64 n);
};

TarStreamParser::TarStreamParser(QString destDir) : d_(std::make_unique<Impl>(std::move(destDir))) {}
TarStreamParser::~TarStreamParser() = default;

const QStringList& TarStreamParser::entries() const
{
    return d_->entries;
}

mw::Result<bool> TarStreamParser::Impl::startEntry(const QByteArray& hdr)
{
    const char* h = hdr.constData();

    // 全零块 = 归档结束（tar 规范以整块零识别，而非校验和为 0）
    bool allZero = true;
    for (int i = 0; i < 512; ++i) {
        if (h[i] != 0) {
            allZero = false;
            break;
        }
    }
    if (allZero) {
        st = St::Done;
        return mw::Result<bool>::Ok();
    }

    // 校验和：字段 148..156 视作空格求和
    const quint64 stored = octalField(h + 148, 8);
    quint64 sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? quint64(' ') : quint64(quint8(h[i]));
    if (sum != stored)
        return mw::err<bool>(QStringLiteral("tar 头校验和不符（期望 %1，实际 %2）")
                                 .arg(stored).arg(sum));

    type = h[156];
    size = octalField(h + 124, 12);
    mode = quint32(octalField(h + 100, 8) & 07777u);
    mtime = octalField(h + 136, 12);
    name = cstrField(h, 100);
    link = cstrField(h + 157, 100);
    // ustar prefix 字段（POSIX）；GNU 长名走 'L'，两者都兼容
    if (QString::fromLatin1(h + 257, 5) == QStringLiteral("ustar")) {
        const QString prefix = cstrField(h + 345, 155);
        if (!prefix.isEmpty())
            name = prefix + QLatin1Char('/') + name;
    }
    if (!pendingName.isEmpty()) {
        name = pendingName;
        pendingName.clear();
    }
    if (!pendingLink.isEmpty()) {
        link = pendingLink;
        pendingLink.clear();
    }

    remaining = size;
    paddedRemain = (size + 511) & ~quint64(511);
    collectingControl = (type == 'L' || type == 'K' || type == 'x' || type == 'g');
    control.clear();
    if (collectingControl && size > (1ull << 20))
        return mw::err<bool>(QStringLiteral("tar 扩展头超限（%1 字节）").arg(size));
    st = St::WantData;
    return mw::Result<bool>::Ok();
}

mw::Result<bool> TarStreamParser::Impl::writePayload(const char* p, qint64 n)
{
    if (collectingControl) {
        control.append(p, int(n));
        return mw::Result<bool>::Ok();
    }
    if (type == '0' || type == '\0' || type == '7') {
        if (!out) {
            const QString rel = sanitizeRelPath(name);
            if (rel.isEmpty())
                return mw::err<bool>(QStringLiteral("tar 条目路径非法（穿越或为空）: %1").arg(name));
            const QString path = QDir(destDir).filePath(rel);
            QDir().mkpath(QFileInfo(path).absolutePath());
            out = std::make_unique<QFile>(path);
            if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate))
                return mw::err<bool>(QStringLiteral("创建 %1 失败: %2")
                                         .arg(path, out->errorString()));
        }
        if (out->write(p, n) != n)
            return mw::err<bool>(QStringLiteral("写入 tar 条目失败: %1").arg(out->fileName()));
    }
    // 其余类型的 payload 丢弃（跳过）
    return mw::Result<bool>::Ok();
}

mw::Result<bool> TarStreamParser::Impl::finishEntry()
{
    switch (type) {
    case '0':
    case '\0':
    case '7': {
        const QString rel = sanitizeRelPath(name);
        if (rel.isEmpty())
            return mw::err<bool>(QStringLiteral("tar 条目路径非法（穿越或为空）: %1").arg(name));
        const QString path = QDir(destDir).filePath(rel);
        if (!out && size == 0) { // 空文件：payload 阶段不会触发 open
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile empty(path);
            if (!empty.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return mw::err<bool>(QStringLiteral("创建 %1 失败: %2")
                                         .arg(path, empty.errorString()));
        }
        if (out)
            out->close();
        applyMeta(path, mode, mtime);
        entries << rel;
        break;
    }
    case '5': {
        const QString rel = sanitizeRelPath(name);
        if (rel.isEmpty())
            return mw::err<bool>(QStringLiteral("tar 条目路径非法（穿越或为空）: %1").arg(name));
        const QString path = QDir(destDir).filePath(rel);
        if (!QDir().mkpath(path))
            return mw::err<bool>(QStringLiteral("创建目录 %1 失败").arg(path));
        entries << rel;
        break;
    }
    case '2': { // symlink
        const QString rel = sanitizeRelPath(name);
        if (rel.isEmpty())
            return mw::err<bool>(QStringLiteral("tar 条目路径非法（穿越或为空）: %1").arg(name));
        const QString path = QDir(destDir).filePath(rel);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile::remove(path); // 幂等
        if (::symlink(link.toUtf8().constData(), path.toUtf8().constData()) != 0)
            return mw::err<bool>(QStringLiteral("创建符号链接 %1 → %2 失败: %3")
                                     .arg(path, link, QString::fromUtf8(strerror(errno))));
        entries << rel;
        break;
    }
    case '1': { // hardlink：link 失败退化为 symlink
        const QString rel = sanitizeRelPath(name);
        if (rel.isEmpty())
            return mw::err<bool>(QStringLiteral("tar 条目路径非法（穿越或为空）: %1").arg(name));
        const QString path = QDir(destDir).filePath(rel);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile::remove(path);
        const QString target = QDir(destDir).filePath(link);
        if (::link(target.toUtf8().constData(), path.toUtf8().constData()) != 0
            && ::symlink(link.toUtf8().constData(), path.toUtf8().constData()) != 0)
            return mw::err<bool>(QStringLiteral("创建硬链接 %1 → %2 失败").arg(path, link));
        entries << rel;
        break;
    }
    case 'L': { // GNU long name：下一 entry 的名字
        QString v = QString::fromUtf8(control);
        const int nul = v.indexOf(QLatin1Char('\0'));
        if (nul >= 0)
            v.truncate(nul);
        if (v.trimmed().isEmpty())
            return mw::err<bool>(QStringLiteral("GNU 长名为空"));
        pendingName = v;
        break;
    }
    case 'K': { // GNU long link
        QString v = QString::fromUtf8(control);
        const int nul = v.indexOf(QLatin1Char('\0'));
        if (nul >= 0)
            v.truncate(nul);
        pendingLink = v;
        break;
    }
    case 'x':
    case 'g': { // pax 扩展头：仅取 "path" 记录
        // 记录格式：<len> <key>=<value>\n，len 含数字自身与结尾换行
        int i = 0;
        while (i < control.size()) {
            const int sp = control.indexOf(' ', i);
            if (sp < 0)
                break;
            bool okNum = false;
            const int reclen = control.mid(i, sp - i).toInt(&okNum);
            if (!okNum || reclen <= 0 || i + reclen > control.size())
                break;
            const int bodyLen = reclen - (sp - i) - 2; // 去掉 " " 与 "\n"
            const QByteArray rec = control.mid(sp + 1, bodyLen > 0 ? bodyLen : 0);
            const int eq = rec.indexOf('=');
            if (eq > 0 && rec.left(eq) == QByteArrayLiteral("path"))
                pendingName = QString::fromUtf8(rec.mid(eq + 1));
            i += reclen;
        }
        break;
    }
    default:
        break; // GNU 特殊类型（'D'/'M'/'S'/'V'…）只跳过 payload
    }
    out.reset();
    st = St::WantHeader;
    return mw::Result<bool>::Ok();
}

mw::Result<bool> TarStreamParser::feed(const char* data, qint64 n)
{
    if (d_->st == Impl::St::Done)
        return mw::Result<bool>::Ok();
    d_->buf.append(data, int(n));

    while (true) {
        if (d_->st == Impl::St::WantHeader) {
            if (d_->buf.size() < 512)
                break;
            const QByteArray hdr = d_->buf.left(512);
            d_->buf.remove(0, 512);
            if (auto r = d_->startEntry(hdr); !r)
                return r;
            if (d_->st == Impl::St::Done)
                break;
        } else { // WantData
            if (d_->paddedRemain == 0) {
                if (auto r = d_->finishEntry(); !r)
                    return r;
                continue;
            }
            const qint64 take =
                qint64(std::min<quint64>(quint64(d_->buf.size()), d_->paddedRemain));
            if (take == 0)
                break;
            const qint64 real = qint64(std::min<quint64>(quint64(take), d_->remaining));
            if (real > 0) {
                if (auto r = d_->writePayload(d_->buf.constData(), real); !r)
                    return r;
                d_->remaining -= quint64(real);
            }
            d_->paddedRemain -= quint64(take);
            d_->buf.remove(0, int(take));
        }
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> TarStreamParser::finish()
{
    if (d_->st == Impl::St::WantData) {
        if (d_->paddedRemain > 0)
            return mw::err<bool>(QStringLiteral("tar 流在条目中间截断"));
        return d_->finishEntry();
    }
    return mw::Result<bool>::Ok();
}

// ===================== extractTarZst =====================

mw::Result<bool> extractTarZst(const QString& tarZstPath, const QString& destDir)
{
    QFile f(tarZstPath);
    if (!f.open(QIODevice::ReadOnly))
        return mw::err<bool>(QStringLiteral("打开 %1 失败: %2").arg(tarZstPath, f.errorString()));

    ZSTD_DStream* zds = ZSTD_createDStream();
    if (!zds)
        return mw::err<bool>(QStringLiteral("zstd 解压失败（包损坏？）: 初始化失败"));
    ZSTD_initDStream(zds);

    std::vector<char> inBuf(ZSTD_DStreamInSize());
    std::vector<char> outBuf(ZSTD_DStreamOutSize());
    TarStreamParser parser(destDir);

    auto fail = [&](const QString& msg) {
        ZSTD_freeDStream(zds);
        return mw::err<bool>(msg);
    };

    ZSTD_inBuffer ib{ nullptr, 0, 0 };
    while (true) {
        const qint64 n = f.read(inBuf.data(), qint64(inBuf.size()));
        if (n < 0)
            return fail(QStringLiteral("读取 %1 失败").arg(tarZstPath));
        ib.src = inBuf.data();
        ib.size = size_t(n);
        ib.pos = 0;
        do {
            ZSTD_outBuffer ob{ outBuf.data(), outBuf.size(), 0 };
            const size_t r = ZSTD_decompressStream(zds, &ob, &ib);
            if (ZSTD_isError(r))
                return fail(QStringLiteral("zstd 解压失败（包损坏？）: %1")
                                .arg(QString::fromUtf8(ZSTD_getErrorName(r))));
            if (ob.pos > 0) {
                if (auto fr = parser.feed(reinterpret_cast<const char*>(ob.dst), qint64(ob.pos)); !fr)
                    return fail(fr.error());
            }
        } while (ib.pos < ib.size);
        if (n == 0)
            break;
    }
    ZSTD_freeDStream(zds);

    if (auto fr = parser.finish(); !fr)
        return fr;
    return mw::Result<bool>::Ok();
}

// ===================== zipExtract（central directory + raw inflate） =====================

namespace {

quint16 le16(const uchar* p) { return quint16(p[0] | (p[1] << 8)); }
quint32 le32(const uchar* p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

// 解压单个 deflate 条目（zip 方法 8 = raw deflate，无 zlib 头）
mw::Result<bool> inflateEntry(QFile& zip, quint32 compSize, quint32 uncompSize, QFile& out)
{
    z_stream zs{};
    if (inflateInit2(&zs, -15) != Z_OK)
        return mw::err<bool>(QStringLiteral("zlib inflate 初始化失败"));
    std::vector<uchar> in(4u << 20), ob(4u << 20);
    quint32 left = compSize;
    quint64 written = 0;
    int ret = Z_OK;

    while (true) {
        if (zs.avail_in == 0 && left > 0) {
            const quint32 chunk = quint32(std::min<quint32>(left, quint32(in.size())));
            const qint64 n = zip.read(reinterpret_cast<char*>(in.data()), qint64(chunk));
            if (n <= 0) {
                inflateEnd(&zs);
                return mw::err<bool>(QStringLiteral("zip 条目数据截断"));
            }
            zs.next_in = in.data();
            zs.avail_in = uint(n);
            left -= quint32(n);
        }
        zs.next_out = ob.data();
        zs.avail_out = uint(ob.size());
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return mw::err<bool>(QStringLiteral("zip 条目解压失败（zlib 错误 %1）").arg(ret));
        }
        const quint32 got = quint32(ob.size() - zs.avail_out);
        if (got > 0) {
            if (out.write(reinterpret_cast<const char*>(ob.data()), qint64(got)) != qint64(got)) {
                inflateEnd(&zs);
                return mw::err<bool>(QStringLiteral("解压 %1 失败: %2")
                                          .arg(out.fileName(), out.errorString()));
            }
            written += got;
        }
        if (ret == Z_STREAM_END)
            break;
        if (zs.avail_in == 0 && left == 0) {
            if (ret == Z_BUF_ERROR)
                break; // 流结束但无 STREAM_END 标记（罕见）
            inflateEnd(&zs);
            return mw::err<bool>(QStringLiteral("zip 条目数据不完整"));
        }
    }
    inflateEnd(&zs);
    if (written != uncompSize)
        return mw::err<bool>(QStringLiteral("zip 条目长度不符（期望 %1，实际 %2）")
                                 .arg(uncompSize).arg(written));
    return mw::Result<bool>::Ok();
}

} // namespace

mw::Result<bool> zipExtract(const QString& zipPath, const QString& destDir)
{
    QFile zip(zipPath);
    if (!zip.open(QIODevice::ReadOnly))
        return mw::err<bool>(QStringLiteral("zip 打开失败: %1: %2").arg(zipPath, zip.errorString()));
    const qint64 sz = zip.size();

    // EOCD（End Of Central Directory）在尾部 64KB + 22 内
    const qint64 tailLen = std::min<qint64>(sz, 65536 + 22);
    zip.seek(sz - tailLen);
    const QByteArray tail = zip.read(tailLen);
    int eocd = -1;
    for (int i = int(tail.size()) - 22; i >= 0; --i) {
        const uchar* q = reinterpret_cast<const uchar*>(tail.constData() + i);
        if (le32(q) == 0x06054b50u && le16(q + 20) == quint16(tail.size() - 22 - i)) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return mw::err<bool>(QStringLiteral("zip 打开失败: 未找到目录结尾（EOCD）"));

    const uchar* e = reinterpret_cast<const uchar*>(tail.constData() + eocd);
    const quint32 cdSize = le32(e + 12);
    const quint32 cdOfs = le32(e + 16);
    if (cdOfs == 0xffffffffu || cdSize == 0xffffffffu)
        return mw::err<bool>(QStringLiteral("zip64 不支持"));

    zip.seek(cdOfs);
    const QByteArray cd = zip.read(cdSize);
    int p = 0;
    while (p + 46 <= cd.size()) {
        const uchar* c = reinterpret_cast<const uchar*>(cd.constData() + p);
        if (le32(c) != 0x02014b50u)
            break;
        const quint16 method = le16(c + 10);
        const quint32 compSize = le32(c + 20);
        const quint32 uncompSize = le32(c + 24);
        const quint16 nameLen = le16(c + 28);
        const quint16 extraLen = le16(c + 30);
        const quint16 cmtLen = le16(c + 32);
        const quint32 lho = le32(c + 42);
        const QString name =
            QString::fromUtf8(cd.mid(p + 46, nameLen)).replace(QLatin1Char('\\'), QLatin1Char('/'));
        p += 46 + nameLen + extraLen + cmtLen;

        if (name.isEmpty())
            continue;
        if (name.endsWith(QLatin1Char('/'))) { // 目录条目
            const QString rel = sanitizeRelPath(name.chopped(1));
            if (rel.isEmpty())
                return mw::err<bool>(QStringLiteral("zip 条目路径非法（穿越或为空）: %1").arg(name));
            QDir().mkpath(QDir(destDir).filePath(rel));
            continue;
        }
        const QString rel = sanitizeRelPath(name);
        if (rel.isEmpty())
            return mw::err<bool>(QStringLiteral("zip 条目路径非法（穿越或为空）: %1").arg(name));
        const QString outPath = QDir(destDir).filePath(rel);

        if (method != 0 && method != 8)
            return mw::err<bool>(QStringLiteral("zip 条目 %1 使用不支持的压缩方法 %2")
                                     .arg(name).arg(method));
        // 本地头：跳过文件名/扩展字段取数据起点
        zip.seek(qint64(lho));
        const QByteArray lh = zip.read(30);
        if (lh.size() < 30
            || le32(reinterpret_cast<const uchar*>(lh.constData())) != 0x04034b50u)
            return mw::err<bool>(QStringLiteral("zip 条目读取失败: %1 本地头损坏").arg(name));
        const quint16 ln = le16(reinterpret_cast<const uchar*>(lh.constData()) + 26);
        const quint16 le = le16(reinterpret_cast<const uchar*>(lh.constData()) + 28);
        zip.seek(qint64(lho) + 30 + ln + le);

        QDir().mkpath(QFileInfo(outPath).absolutePath());
        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return mw::err<bool>(QStringLiteral("创建 %1 失败: %2").arg(outPath, out.errorString()));

        if (method == 0) { // stored：原样拷贝
            quint32 left = compSize;
            std::vector<char> buf(1u << 20);
            while (left > 0) {
                const quint32 chunk = quint32(std::min<quint32>(left, quint32(buf.size())));
                const qint64 n = zip.read(buf.data(), qint64(chunk));
                if (n <= 0 || out.write(buf.data(), n) != n)
                    return mw::err<bool>(QStringLiteral("解压 %1 失败").arg(outPath));
                left -= quint32(n);
            }
        } else {
            if (auto r = inflateEntry(zip, compSize, uncompSize, out); !r)
                return r;
        }
        out.close();
    }
    return mw::Result<bool>::Ok();
}

} // namespace mw::supervisor
