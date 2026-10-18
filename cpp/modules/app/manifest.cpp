// manifest.rs 的 C++ 移植：string pool（UTF-8/UTF-16）+ 平铺元素事件扫描。
#include "manifest.hpp"

#include "types.hpp"

namespace mw::app {
namespace {

constexpr quint16 kResStringPoolType = 0x0001;
constexpr quint16 kResXmlStartElementType = 0x0102;
constexpr quint16 kResXmlEndElementType = 0x0103;
constexpr quint32 kUtf8Flag = 1u << 8; // string pool flags: UTF-8 编码位
constexpr quint32 kNoIndex = 0xFFFFFFFFu;
constexpr quint8 kTypeString = 0x03;   // 属性值类型：字符串
constexpr quint8 kTypeIntDec = 0x10;   // 属性值类型：整数（十进制）

struct Cursor {
    const uchar* d = nullptr;
    qsizetype n = 0;
    qsizetype pos = 0;

    std::optional<quint16> u16()
    {
        if (pos + 2 > n)
            return std::nullopt;
        const quint16 v = quint16(d[pos] | (d[pos + 1] << 8));
        pos += 2;
        return v;
    }

    std::optional<quint32> u32()
    {
        if (pos + 4 > n)
            return std::nullopt;
        const quint32 v = quint32(d[pos]) | (quint32(d[pos + 1]) << 8)
            | (quint32(d[pos + 2]) << 16) | (quint32(d[pos + 3]) << 24);
        pos += 4;
        return v;
    }

    const uchar* take(qsizetype bytes)
    {
        if (pos + bytes > n)
            return nullptr;
        const uchar* p = d + pos;
        pos += bytes;
        return p;
    }
};

struct StringPool {
    // UTF-16 解码后的字符串（UTF-8 pool 也统一转存 QString）
    QVector<QString> strings;

    const QString* get(quint32 idx) const
    {
        if (idx == kNoIndex)
            return nullptr;
        return idx < quint32(strings.size()) ? &strings[int(idx)] : nullptr;
    }
};

std::optional<QString> readPoolStringUtf16(Cursor& c)
{
    // UTF-16：u16 长度（最高位为 1 时是两段式长度，APK 实际不会触发，截断处理）
    const auto len0 = c.u16();
    if (!len0)
        return std::nullopt;
    quint32 len = *len0;
    if (len & 0x8000) {
        const auto len1 = c.u16();
        if (!len1)
            return std::nullopt;
        len = ((len & 0x7FFF) << 16) | *len1;
    }
    const uchar* bytes = c.take(qsizetype(len) * 2);
    if (!bytes)
        return std::nullopt;
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes), int(len));
}

// 变长长度：首字节高位为 1 时取低 7 位 + 次字节拼 14 位。
std::optional<quint32> readVarLen(Cursor& c)
{
    const uchar* first = c.take(1);
    if (!first)
        return std::nullopt;
    if (first[0] & 0x80) {
        const uchar* second = c.take(1);
        if (!second)
            return std::nullopt;
        return quint32(((first[0] & 0x7f) << 8) | second[0]);
    }
    return quint32(first[0]);
}

std::optional<QString> readPoolStringUtf8(Cursor& c)
{
    // UTF-8：utf16 字符数（忽略）与 utf8 字节数都用 7-bit 分块变长编码（1 或 2 字节）
    readVarLen(c); // utf16 len
    const std::optional<quint32> len = readVarLen(c);
    if (!len)
        return std::nullopt;
    const uchar* bytes = c.take(*len);
    if (!bytes)
        return std::nullopt;
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes), int(*len)); // 无效序列按替换字符（lossy）
}

bool parseStringPool(const uchar* chunk, qsizetype size, StringPool& pool, QString& err)
{
    Cursor c{chunk, size, 0};
    if (c.u16() != kResStringPoolType) {
        err = QStringLiteral("不是 string pool chunk");
        return false;
    }
    const qsizetype headerSize = c.u16().value_or(0);
    c.u32(); // chunk size
    const quint32 stringCount = c.u32().value_or(0);
    c.u32(); // style count
    const quint32 flags = c.u32().value_or(0);
    const qsizetype stringsStart = c.u32().value_or(0);
    c.u32(); // styles start
    const bool utf8 = flags & kUtf8Flag;
    c.pos = qMax<qsizetype>(headerSize, 28);

    QVector<quint32> offsets;
    offsets.reserve(int(stringCount));
    for (quint32 i = 0; i < stringCount; ++i) {
        const std::optional<quint32> off = c.u32();
        if (!off) {
            err = QStringLiteral("string 偏移表截断");
            return false;
        }
        offsets.push_back(*off);
    }
    // stringsStart 相对 pool chunk 起点（已含 header + 偏移表）
    pool.strings.reserve(int(stringCount));
    for (const quint32 off : offsets) {
        const qsizetype abs = stringsStart + qsizetype(off);
        if (abs >= size) {
            pool.strings.push_back(QString());
            continue;
        }
        Cursor sc{chunk, size, abs};
        std::optional<QString> s = utf8 ? readPoolStringUtf8(sc) : readPoolStringUtf16(sc);
        pool.strings.push_back(s ? std::move(*s) : QString());
    }
    return true;
}

std::optional<quint32> sdkVersion(quint8 dataType, quint32 data, const std::optional<QString>& raw)
{
    if (dataType == kTypeIntDec)
        return data;
    if (!raw)
        return std::nullopt;
    bool ok = false;
    const quint32 v = raw->toUInt(&ok);
    return ok ? std::optional<quint32>(v) : std::nullopt;
}

std::optional<quint64> parseDigits(const std::optional<QString>& s)
{
    if (!s)
        return std::nullopt;
    bool ok = false;
    const quint64 v = s->toULongLong(&ok);
    return ok ? std::optional<quint64>(v) : std::nullopt;
}

// START_ELEMENT body：headerSize(16) 已含 lineNumber/comment（chunk 8..16），
// body 从 attrExt 开始——ns/name + 属性头 + 属性数组。
bool applyStartElement(const uchar* body, qsizetype bodySize, const StringPool& pool,
                       qsizetype depth, ManifestInfo& info, QString& err)
{
    Cursor c{body, bodySize, 0};
    c.u32(); // ns
    const std::optional<quint32> nameIdx = c.u32();
    if (!nameIdx) {
        err = QStringLiteral("元素名截断");
        return false;
    }
    const QString* namePtr = pool.get(*nameIdx);
    if (!namePtr) {
        err = QStringLiteral("元素名索引越界");
        return false;
    }
    const QString name = *namePtr;
    c.u16(); // attrStart（默认 0x14）
    c.u16(); // attrSize（默认 0x14）
    const quint32 attrCount = c.u16().value_or(0);
    c.u16(); // idIndex
    c.u16(); // classIndex
    c.u16(); // styleIndex

    for (quint32 i = 0; i < attrCount; ++i) {
        if (!c.u32()) {
            err = QStringLiteral("属性截断");
            return false;
        }
        const std::optional<quint32> attrNameIdx = c.u32();
        if (!attrNameIdx) {
            err = QStringLiteral("属性名截断");
            return false;
        }
        const quint32 rawIdx = c.u32().value_or(kNoIndex);
        const quint32 valueSize = c.u16().value_or(8);
        c.take(1); // res0
        const uchar* typeByte = c.take(1);
        const quint8 dataType = typeByte ? typeByte[0] : 0;
        const quint32 data = c.u32().value_or(0);
        // 个别工具写 size≠8 的扩展属性头，按声明的 size 对齐
        const qsizetype extra = qsizetype(valueSize) - 8;
        if (extra > 0 && !c.take(extra)) {
            err = QStringLiteral("属性头截断");
            return false;
        }
        const QString* attrNamePtr = pool.get(*attrNameIdx);
        const QString attrName = attrNamePtr ? *attrNamePtr : QString();
        const auto stringValue = [&]() -> std::optional<QString> {
            if (dataType == kTypeString) {
                const QString* s = pool.get(data);
                return s ? std::optional<QString>(*s) : std::nullopt;
            }
            if (rawIdx != kNoIndex) {
                const QString* s = pool.get(rawIdx);
                return s ? std::optional<QString>(*s) : std::nullopt;
            }
            return std::nullopt;
        }();

        if (depth == 1 && name == QLatin1String("manifest")) {
            if (attrName == QLatin1String("package")) {
                info.package = stringValue ? *stringValue : QString();
            } else if (attrName == QLatin1String("versionName")) {
                info.versionName = (stringValue && !stringValue->isEmpty())
                    ? stringValue : std::nullopt;
            } else if (attrName == QLatin1String("versionCode")) {
                info.versionCode = dataType == kTypeIntDec
                    ? std::optional<quint64>(data) // data 为 u32 字面，与 Rust `data as u64` 一致
                    : parseDigits(stringValue);
            }
        } else if (depth == 2 && (name == QLatin1String("application")
                                  || name == QLatin1String("uses-sdk"))) {
            // uses-sdk 与 application 都是 manifest 直接子元素
            if (name == QLatin1String("application")) {
                if (attrName == QLatin1String("label")) {
                    info.label = (stringValue && !stringValue->trimmed().isEmpty())
                        ? stringValue : std::nullopt;
                } else if (attrName == QLatin1String("debuggable")) {
                    info.debuggable = data != 0;
                }
            } else { // uses-sdk
                if (attrName == QLatin1String("minSdkVersion"))
                    info.minSdk = sdkVersion(dataType, data, stringValue);
                else if (attrName == QLatin1String("targetSdkVersion"))
                    info.targetSdk = sdkVersion(dataType, data, stringValue);
            }
        }
    }
    return true;
}

bool parseManifestInner(const QByteArray& axml, ManifestInfo& info, QString& err)
{
    // 文件头：type=0x0003(RES_XML) + headerSize + size
    Cursor c{reinterpret_cast<const uchar*>(axml.constData()), axml.size(), 0};
    const std::optional<quint16> fileType = c.u16();
    if (!fileType) {
        err = QStringLiteral("空数据");
        return false;
    }
    if (*fileType != 0x0003) {
        err = QStringLiteral("不是二进制 XML（type=%1）").arg(*fileType, 4, 16, QChar('0'));
        return false;
    }
    const qsizetype fileHeaderSize = c.u16().value_or(8);
    c.u32(); // file size
    c.pos = fileHeaderSize;

    // 逐 chunk 扫描：string pool 只出现一次；随后是命名空间/元素事件
    std::optional<StringPool> pool;
    qsizetype depth = 0; // manifest=1, application=2
    while (c.pos + 8 <= c.n) {
        Cursor chunk{c.d, c.n, c.pos};
        const quint16 chunkType = chunk.u16().value_or(0);
        const qsizetype headerSize = chunk.u16().value_or(0);
        const qsizetype chunkSize = chunk.u32().value_or(0);
        if (chunkSize < headerSize || c.pos + chunkSize > c.n) {
            err = QStringLiteral("chunk 尺寸越界");
            return false;
        }
        const uchar* body = c.d + c.pos + headerSize;
        const qsizetype bodySize = chunkSize - headerSize;
        if (chunkType == kResStringPoolType) {
            StringPool p;
            if (!parseStringPool(c.d + c.pos, chunkSize, p, err))
                return false;
            pool = std::move(p);
        } else if (chunkType == kResXmlStartElementType) {
            ++depth;
            if (!pool) {
                err = QStringLiteral("元素出现在 string pool 之前");
                return false;
            }
            if (!applyStartElement(body, bodySize, *pool, depth, info, err))
                return false;
        } else if (chunkType == kResXmlEndElementType) {
            depth = depth > 0 ? depth - 1 : 0;
        }
        // 命名空间/CDATAs 等跳过
        c.pos += chunkSize;
    }
    if (info.package.isEmpty()) {
        err = QStringLiteral("manifest 元素缺 package 属性");
        return false;
    }
    return true;
}

} // namespace

mw::Result<ManifestInfo> parseManifest(const QByteArray& axml, const QString& label)
{
    ManifestInfo info;
    QString reason;
    if (!parseManifestInner(axml, info, reason))
        return mw::err<ManifestInfo>(errApkParse(label, reason));
    return mw::Result<ManifestInfo>::Ok(std::move(info));
}

} // namespace mw::app
