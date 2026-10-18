// apk_min.hpp 实现 ← crates/mwine-app/src/apk.rs + manifest.rs（最小子集移植）。
#include "apk_min.hpp"

#include <QFile>

#include <zlib.h>

namespace mw::store::apkmin {

namespace {

constexpr quint32 EOCD_SIG = 0x06054b50;
constexpr quint32 CEN_SIG = 0x02014b50;
constexpr quint32 LOC_SIG = 0x04034b50;
// EOCD 最小 22 字节 + 注释最长 64KB，从尾部扫这么多即可
constexpr qsizetype EOCD_SCAN_WINDOW = 22 + 65535;

quint16 u16At(const QByteArray& d, qsizetype p)
{
    return static_cast<quint16>(static_cast<quint8>(d.at(p))
                                | (static_cast<quint16>(static_cast<quint8>(d.at(p + 1))) << 8));
}

quint32 u32At(const QByteArray& d, qsizetype p)
{
    return static_cast<quint32>(static_cast<quint8>(d.at(p)))
         | (static_cast<quint32>(static_cast<quint8>(d.at(p + 1))) << 8)
         | (static_cast<quint32>(static_cast<quint8>(d.at(p + 2))) << 16)
         | (static_cast<quint32>(static_cast<quint8>(d.at(p + 3))) << 24);
}

bool rawInflate(const QByteArray& in, QByteArray& out, QString* why)
{
    z_stream zs{};
    if (inflateInit2(&zs, -15) != Z_OK) {  // raw deflate（zip entry 无 gzip 头）
        if (why) *why = QStringLiteral("inflateInit2 失败");
        return false;
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = static_cast<uInt>(in.size());
    char buf[65536];
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            if (why)
                *why = QStringLiteral("deflate 解压失败: %1")
                           .arg(QString::fromLatin1(zs.msg ? zs.msg : "inflate 错误"));
            inflateEnd(&zs);
            return false;
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (ret == Z_BUF_ERROR && zs.avail_in == 0)
            break;  // 输入耗尽
    }
    inflateEnd(&zs);
    return true;
}

}  // namespace

namespace {

struct CentralEntry {
    QString name;
    quint16 method = 0;
    quint64 compSize = 0;
    quint64 uncompSize = 0;
    quint64 localOffset = 0;
};

mw::Result<QVector<CentralEntry>> readCentralDirectory(const QByteArray& data, const QString& label)
{
    if (data.size() < 22)
        return mw::err<QVector<CentralEntry>>(
            QStringLiteral("APK 解析失败（%1）: 文件小于 EOCD 最小长度").arg(label));
    const qsizetype scanStart = std::max<qsizetype>(data.size() - EOCD_SCAN_WINDOW, 0);
    qsizetype eocd = -1;
    for (qsizetype i = data.size() - 22; i >= scanStart; --i) {
        if (u32At(data, i) == EOCD_SIG) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        return mw::err<QVector<CentralEntry>>(
            QStringLiteral("APK 解析失败（%1）: 未找到 EOCD 记录").arg(label));
    }
    const qsizetype total = u16At(data, eocd + 10);
    const qsizetype cdSize = u32At(data, eocd + 12);
    const qsizetype cdOff = u32At(data, eocd + 16);
    if (cdOff + cdSize > data.size()) {
        return mw::err<QVector<CentralEntry>>(
            QStringLiteral("APK 解析失败（%1）: 中央目录越界: off=%2 size=%3 file=%4")
                .arg(label)
                .arg(cdOff)
                .arg(cdSize)
                .arg(data.size()));
    }
    QVector<CentralEntry> entries;
    entries.reserve(static_cast<int>(total));
    qsizetype p = cdOff;
    for (qsizetype k = 0; k < total; ++k) {
        if (p + 46 > data.size() || u32At(data, p) != CEN_SIG) {
            return mw::err<QVector<CentralEntry>>(
                QStringLiteral("APK 解析失败（%1）: 中央目录 entry 损坏").arg(label));
        }
        CentralEntry e;
        e.method = u16At(data, p + 10);
        e.compSize = u32At(data, p + 20);
        e.uncompSize = u32At(data, p + 24);
        const qsizetype nameLen = u16At(data, p + 28);
        const qsizetype extraLen = u16At(data, p + 30);
        const qsizetype commentLen = u16At(data, p + 32);
        e.localOffset = u32At(data, p + 42);
        e.name = QString::fromUtf8(data.mid(p + 46, nameLen));  // from_utf8_lossy
        entries.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return mw::Result<QVector<CentralEntry>>::Ok(std::move(entries));
}

}  // namespace

mw::Result<ZipArchive> ZipArchive::open(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return mw::err<ZipArchive>(QStringLiteral("io: 无法读取 %1: %2").arg(path, f.errorString()));
    const QByteArray data = f.readAll();
    auto entries = readCentralDirectory(data, path);
    if (!entries)
        return mw::err<ZipArchive>(entries.error());
    ZipArchive z;
    z.data_ = data;
    for (const CentralEntry& e : entries.value()) {
        Entry x;
        x.name = e.name;
        x.method = e.method;
        x.compSize = e.compSize;
        x.uncompSize = e.uncompSize;
        x.localOffset = e.localOffset;
        z.entries_.push_back(std::move(x));
    }
    return mw::Result<ZipArchive>::Ok(std::move(z));
}

QVector<QString> ZipArchive::entryNames() const
{
    QVector<QString> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_)
        out.push_back(e.name);
    return out;
}

mw::Result<QByteArray> ZipArchive::readEntry(const QString& name) const
{
    const Entry* entry = nullptr;
    for (const Entry& e : entries_) {
        if (e.name == name) {
            entry = &e;
            break;
        }
    }
    if (!entry)
        return mw::Result<QByteArray>::Ok(QByteArray());  // 不存在
    const QByteArray& d = data_;
    const qsizetype off = entry->localOffset;
    const auto corrupt = [&](const QString& reason) {
        return mw::err<QByteArray>(QStringLiteral("APK 解析失败（%1）: %2").arg(name, reason));
    };
    if (off + 30 > d.size())
        return corrupt(QStringLiteral("本地头越界"));
    if (u32At(d, off) != LOC_SIG)
        return corrupt(QStringLiteral("本地头签名不匹配"));
    const qsizetype nameLen = u16At(d, off + 26);
    const qsizetype extraLen = u16At(d, off + 28);
    const qsizetype dataOff = off + 30 + nameLen + extraLen;
    const qsizetype dataEnd = dataOff + static_cast<qsizetype>(entry->compSize);
    if (dataEnd > d.size())
        return corrupt(QStringLiteral("entry 数据越界"));
    const QByteArray raw = d.mid(dataOff, dataEnd - dataOff);
    if (entry->method == 0)
        return mw::Result<QByteArray>::Ok(raw);
    if (entry->method == 8) {
        QByteArray out;
        out.reserve(static_cast<int>(std::min<quint64>(entry->uncompSize, 64 << 20)));
        QString why;
        if (!rawInflate(raw, out, &why))
            return corrupt(why);
        return mw::Result<QByteArray>::Ok(std::move(out));
    }
    return corrupt(QStringLiteral("不支持的压缩方法 %1").arg(entry->method));
}

// ================= AXML =================

namespace {

constexpr quint16 RES_STRING_POOL_TYPE = 0x0001;
constexpr quint16 RES_XML_START_ELEMENT_TYPE = 0x0102;
constexpr quint16 RES_XML_END_ELEMENT_TYPE = 0x0103;
constexpr quint32 UTF8_FLAG = 1u << 8;
constexpr quint32 NO_INDEX = 0xFFFFFFFFu;
constexpr quint8 TYPE_STRING = 0x03;
constexpr quint8 TYPE_INT_DEC = 0x10;

struct Cursor {
    const uchar* d;
    qsizetype len;
    qsizetype pos = 0;

    std::optional<quint16> u16()
    {
        if (pos + 2 > len)
            return std::nullopt;
        const quint16 v = static_cast<quint16>(d[pos] | (d[pos + 1] << 8));
        pos += 2;
        return v;
    }
    std::optional<quint32> u32()
    {
        if (pos + 4 > len)
            return std::nullopt;
        const quint32 v = static_cast<quint32>(d[pos]) | (static_cast<quint32>(d[pos + 1]) << 8)
                          | (static_cast<quint32>(d[pos + 2]) << 16)
                          | (static_cast<quint32>(d[pos + 3]) << 24);
        pos += 4;
        return v;
    }
    std::optional<QByteArray> take(qsizetype n)
    {
        if (pos + n > len || n < 0)
            return std::nullopt;
        const QByteArray out(reinterpret_cast<const char*>(d + pos), static_cast<int>(n));
        pos += n;
        return out;
    }
};

QStringList parseStringPool(const uchar* d, qsizetype len)
{
    Cursor c{ d, len, 0 };
    if (c.u16() != RES_STRING_POOL_TYPE)
        return {};
    const qsizetype headerSize = c.u16().value_or(0);
    c.u32();  // chunk size
    const qsizetype stringCount = c.u32().value_or(0);
    c.u32();  // style count
    const quint32 flags = c.u32().value_or(0);
    const qsizetype stringsStart = c.u32().value_or(0);
    c.u32();  // styles start
    const bool utf8 = (flags & UTF8_FLAG) != 0;
    // 防御损坏 AXML：stringCount 异常巨大时 reserve 会撑爆内存（实测 parseManifest
    // 在 push_back/emplace 段错误）。string pool 条目不可能超过合理上限。
    if (stringCount <= 0 || stringCount > 1 << 20)
        return {};
    c.pos = std::max<qsizetype>(headerSize, 28);
    QVector<quint32> offsets;
    offsets.reserve(static_cast<int>(stringCount));
    for (qsizetype i = 0; i < stringCount; ++i) {
        const auto off = c.u32();
        if (!off)
            return {};  // string 偏移表截断
        offsets.push_back(*off);
    }
    // stringsStart 相对 pool chunk 起点（已含 header + 偏移表）
    QStringList strings;
    strings.reserve(static_cast<int>(stringCount));
    for (const quint32 off : offsets) {
        const qsizetype abs = stringsStart + static_cast<qsizetype>(off);
        if (abs >= len) {
            strings.push_back(QString());
            continue;
        }
        Cursor sc{ d, len, abs };
        if (utf8) {
            // UTF-8：utf16 字符数（忽略）与 utf8 字节数都用 7-bit 分块变长编码
            const auto varLen = [&sc]() -> std::optional<quint32> {
                const auto b1 = sc.take(1);
                if (!b1)
                    return std::nullopt;
                const quint32 first = static_cast<quint8>((*b1)[0]);
                if (first & 0x80) {
                    const auto b2 = sc.take(1);
                    if (!b2)
                        return std::nullopt;
                    return ((first & 0x7F) << 8) | static_cast<quint8>((*b2)[0]);
                }
                return first;
            };
            const auto u16len = varLen();
            const auto len8 = varLen();
            if (!u16len || !len8)
                strings.push_back(QString());
            else if (const auto bytes = sc.take(*len8))
                strings.push_back(QString::fromUtf8(*bytes));
            else
                strings.push_back(QString());
        } else {
            // UTF-16：u16 长度（最高位为 1 时是两段式长度，APK 实际不会触发）
            auto len16 = sc.u16();
            if (!len16)
                strings.push_back(QString());
            else {
                qsizetype slen = *len16;
                if (slen & 0x8000) {
                    const auto hi = sc.u16();
                    if (!hi) {
                        strings.push_back(QString());
                        continue;
                    }
                    slen = ((slen & 0x7FFF) << 16) | *hi;
                }
                if (const auto bytes = sc.take(slen * 2)) {
                    // 逐 u16 安全解码：bytes->constData() 对齐不保证（AXML 原始流奇数
                    // 地址），直接 reinterpret_cast<char16_t*> 在 ARM64 上 fromUtf16 的
                    // NEON 路径会未对齐段错误（parseManifest 崩溃根因）。
                    QString s;
                    s.reserve(static_cast<int>(slen));
                    const uchar* p = reinterpret_cast<const uchar*>(bytes->constData());
                    for (qsizetype k = 0; k < slen; ++k)
                        s.append(static_cast<char16_t>(p[k * 2] | (p[k * 2 + 1] << 8)));
                    strings.push_back(std::move(s));
                } else {
                    strings.push_back(QString());
                }
            }
        }
    }
    return strings;
}

const QString* poolGet(const QStringList& pool, quint32 idx)
{
    if (idx == NO_INDEX || idx >= static_cast<quint32>(pool.size()))
        return nullptr;
    return &pool.at(static_cast<int>(idx));
}

bool applyStartElement(const QByteArray& body, const QStringList& pool, int depth, ManifestInfo& info,
    QString* why)
{
    Cursor c{ reinterpret_cast<const uchar*>(body.constData()), body.size(), 0 };
    c.u32();  // ns
    const auto nameIdx = c.u32();
    if (!nameIdx)
        return false;  // 元素名截断
    const QString* namePtr = poolGet(pool, *nameIdx);
    if (!namePtr) {
        if (why) *why = QStringLiteral("元素名索引越界");
        return false;
    }
    const QString name = *namePtr;
    c.u16();  // attrStart（默认 0x14）
    c.u16();  // attrSize（默认 0x14）
    const auto attrCount = c.u16().value_or(0);
    c.u16();  // idIndex
    c.u16();  // classIndex
    c.u16();  // styleIndex

    for (int i = 0; i < attrCount; ++i) {
        if (!c.u32()) {
            if (why) *why = QStringLiteral("属性截断");
            return false;
        }
        const auto attrNameIdx = c.u32();
        if (!attrNameIdx) {
            if (why) *why = QStringLiteral("属性名截断");
            return false;
        }
        const quint32 rawIdx = c.u32().value_or(NO_INDEX);
        const quint16 valueSize = c.u16().value_or(8);
        c.take(1);  // res0
        const auto dtypeBytes = c.take(1);
        const quint8 dataType = dtypeBytes ? static_cast<quint8>((*dtypeBytes)[0]) : 0;
        const quint32 data = c.u32().value_or(0);
        // 个别工具写 size≠8 的扩展属性头，按声明的 size 对齐
        const qsizetype extra = static_cast<qsizetype>(valueSize) - 8;
        if (extra > 0 && !c.take(extra)) {
            if (why) *why = QStringLiteral("属性头截断");
            return false;
        }
        const QString* attrNamePtr = poolGet(pool, *attrNameIdx);
        const QString attrName = attrNamePtr ? *attrNamePtr : QString();
        const auto stringValue = [&]() -> std::optional<QString> {
            if (dataType == TYPE_STRING) {
                if (const QString* s = poolGet(pool, data))
                    return *s;
                return std::nullopt;
            }
            if (rawIdx != NO_INDEX) {
                if (const QString* s = poolGet(pool, rawIdx))
                    return *s;
            }
            return std::nullopt;
        };
        const std::optional<QString> sval = stringValue();
        if (depth == 1 && name == QLatin1String("manifest")) {
            if (attrName == QLatin1String("package"))
                info.package = sval.value_or(QString());
            else if (attrName == QLatin1String("versionName")) {
                info.versionName = (sval && !sval->isEmpty()) ? sval : std::nullopt;
            } else if (attrName == QLatin1String("versionCode")) {
                if (dataType == TYPE_INT_DEC)
                    info.versionCode = static_cast<quint64>(data);
                else if (sval) {
                    bool ok = false;
                    const quint64 n = sval->toULongLong(&ok);
                    if (ok)
                        info.versionCode = n;
                }
            }
        } else if (depth == 2 && name == QLatin1String("application")
                   && attrName == QLatin1String("label")) {
            info.label = (sval && !sval->trimmed().isEmpty()) ? sval : std::nullopt;
        }
    }
    return true;
}

}  // namespace

mw::Result<ManifestInfo> parseManifest(const QByteArray& axml, const QString& label)
{
    const auto wrapErr = [&](const QString& reason) {
        return mw::err<ManifestInfo>(QStringLiteral("APK 解析失败（%1）: %2").arg(label, reason));
    };
    const uchar* d = reinterpret_cast<const uchar*>(axml.constData());
    const qsizetype len = axml.size();
    Cursor c{ d, len, 0 };
    const auto fileType = c.u16();
    if (!fileType)
        return wrapErr(QStringLiteral("空数据"));
    if (*fileType != 0x0003)
        return wrapErr(QStringLiteral("不是二进制 XML（type=0x%1）")
                           .arg(*fileType, 4, 16, QChar('0')));
    const qsizetype fileHeaderSize = c.u16().value_or(8);
    c.u32();  // file size
    c.pos = fileHeaderSize;

    // 逐 chunk 扫描：string pool 只出现一次；随后是命名空间/元素事件
    QStringList pool;
    bool hasPool = false;
    ManifestInfo info;
    int depth = 0;  // manifest=1, application=2
    while (c.pos + 8 <= len) {
        Cursor chunk{ d, len, c.pos };
        const quint16 chunkType = chunk.u16().value_or(0);
        const qsizetype headerSize = chunk.u16().value_or(0);
        const qsizetype chunkSize = chunk.u32().value_or(0);
        if (chunkSize < headerSize || c.pos + chunkSize > len)
            return wrapErr(QStringLiteral("chunk 尺寸越界"));
        const QByteArray body = axml.mid(c.pos + headerSize, chunkSize - headerSize);
        if (chunkType == RES_STRING_POOL_TYPE) {
            pool = parseStringPool(d + c.pos, chunkSize);
            hasPool = true;
        } else if (chunkType == RES_XML_START_ELEMENT_TYPE) {
            // START 元素
            ++depth;
            if (!hasPool)
                return wrapErr(QStringLiteral("元素出现在 string pool 之前"));
            QString why;
            if (!applyStartElement(body, pool, depth, info, &why))
                return wrapErr(why.isEmpty() ? QStringLiteral("元素解析失败") : why);
        } else if (chunkType == RES_XML_END_ELEMENT_TYPE) {
            depth = depth > 0 ? depth - 1 : 0;
        }
        // 命名空间/CDATAs 等跳过
        c.pos += chunkSize;
    }
    if (info.package.isEmpty())
        return wrapErr(QStringLiteral("manifest 元素缺 package 属性"));
    return mw::Result<ManifestInfo>::Ok(std::move(info));
}

}  // namespace mw::store::apkmin
