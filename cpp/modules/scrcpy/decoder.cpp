// Annex-B 纯函数实现 ← crates/mwine-scrcpy/src/decoder.rs（split_annexb 等）
#include "decoder.hpp"

namespace mw::scrcpy {

namespace {

/// 从 `from` 起找下一个起始码（00 00 01），返回 (起始码位置, NAL 内容起始位置)。
bool findStartCode(const char* data, qsizetype len, qsizetype from, qsizetype* scPos,
                   qsizetype* after)
{
    for (qsizetype i = from; i + 3 <= len; ++i) {
        const quint8* p = reinterpret_cast<const quint8*>(data + i);
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            *scPos = i;
            *after = i + 3;
            return true;
        }
    }
    return false;
}

} // namespace

QVector<QByteArray> splitAnnexB(const QByteArray& data)
{
    QVector<QByteArray> out;
    const char* p = data.constData();
    const qsizetype len = data.size();
    qsizetype pos = 0, first = 0;
    if (!findStartCode(p, len, 0, &pos, &first))
        return out; // 无起始码 → 空
    qsizetype cur = first; // 当前 NAL 内容起点
    for (;;) {
        qsizetype sc = 0, next = 0;
        if (!findStartCode(p, len, cur, &sc, &next))
            break;
        out.append(data.mid(cur, sc - cur));
        cur = next;
    }
    if (cur <= len)
        out.append(data.mid(cur));
    return out;
}

QVector<QByteArray> parameterSets(const QByteArray& config)
{
    QVector<QByteArray> out;
    for (const QByteArray& nal : splitAnnexB(config)) {
        if (nal.isEmpty())
            continue;
        const quint8 type = quint8(nal[0]) & 0x1f;
        if (type == 7 || type == 8)
            out.append(nal);
    }
    return out;
}

void annexbToAvcc(const QByteArray& data, QByteArray& out)
{
    for (const QByteArray& nal : splitAnnexB(data)) {
        if (nal.isEmpty())
            continue;
        const quint8 type = quint8(nal[0]) & 0x1f;
        if (type == 9 || type == 12)
            continue; // AUD / filler
        const quint32 n = quint32(nal.size());
        out.append(char((n >> 24) & 0xFF));
        out.append(char((n >> 16) & 0xFF));
        out.append(char((n >> 8) & 0xFF));
        out.append(char(n & 0xFF));
        out.append(nal);
    }
}

} // namespace mw::scrcpy
