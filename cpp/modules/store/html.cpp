// html.hpp 实现 ← crates/mwine-store/src/html.rs（逐函数语义对齐）。
#include "html.hpp"

#include <QStringList>

namespace mw::store::html {

std::optional<QString> attr(const QString& html, const QString& name)
{
    const QString needle = name + QStringLiteral("=\"");
    const int i = html.indexOf(needle);
    if (i < 0)
        return std::nullopt;
    const int v = i + needle.size();
    const int end = html.indexOf(QLatin1Char('"'), v);
    if (end < 0)
        return std::nullopt;
    return html.mid(v, end - v);
}

std::optional<QString> attrNth(const QString& html, const QString& name, int n)
{
    const QString needle = name + QStringLiteral("=\"");
    int from = 0;
    std::optional<QString> found;
    for (int k = 0; k <= n; ++k) {
        const int i = html.indexOf(needle, from);
        if (i < 0)
            return std::nullopt;
        const int v = i + needle.size();
        const int end = html.indexOf(QLatin1Char('"'), v);
        // split('"').next()：无闭合引号时取余下全文
        found = end < 0 ? html.mid(v) : html.mid(v, end - v);
        from = v;
    }
    return found;
}

std::optional<QString> between(const QString& html, const QString& open, const QString& close)
{
    const int i = html.indexOf(open);
    if (i < 0)
        return std::nullopt;
    const int after = i + open.size();
    const int j = html.indexOf(close, after);
    if (j < 0)
        return std::nullopt;
    return html.mid(after, j - after);
}

namespace {

void appendCodePoint(QString& out, quint32 cp)
{
    if (cp <= 0xFFFF) {
        out += QChar(static_cast<char16_t>(cp));
    } else {
        out += QChar::highSurrogate(cp);
        out += QChar::lowSurrogate(cp);
    }
}

// char::from_u32 等价：拒绝代理对区与 > 0x10FFFF
bool isValidScalar(quint32 cp)
{
    return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

}  // namespace

QString decodeEntities(const QString& s)
{
    if (!s.contains(QLatin1Char('&')))
        return s;
    QString out;
    out.reserve(s.size());
    int pos = 0;
    while (true) {
        const int i = s.indexOf(QLatin1Char('&'), pos);
        if (i < 0)
            break;
        out += s.mid(pos, i - pos);
        // tail 从 '&' 开始；实体名（含 '&' 与 ';'）总长封顶 12（j <= 11）
        const int semi = s.indexOf(QLatin1Char(';'), i + 1);
        if (semi < 0 || semi == i || semi - i > 11) {
            out += QLatin1Char('&');
            pos = i + 1;
            continue;
        }
        const QString body = s.mid(i + 1, semi - i - 1);
        bool replaced = false;
        if (body == QStringLiteral("amp")) { out += QLatin1Char('&'); replaced = true; }
        else if (body == QStringLiteral("lt")) { out += QLatin1Char('<'); replaced = true; }
        else if (body == QStringLiteral("gt")) { out += QLatin1Char('>'); replaced = true; }
        else if (body == QStringLiteral("quot")) { out += QLatin1Char('"'); replaced = true; }
        else if (body == QStringLiteral("apos") || body == QStringLiteral("rsquo")
                 || body == QStringLiteral("lsquo")) { out += QLatin1Char('\''); replaced = true; }
        else if (body == QStringLiteral("ldquo") || body == QStringLiteral("rdquo")) {
            out += QLatin1Char('"'); replaced = true;
        } else if (body == QStringLiteral("nbsp")) { out += QLatin1Char(' '); replaced = true; }
        else if (body == QStringLiteral("middot")) { out += QChar(0x00B7); replaced = true; }
        else if (body == QStringLiteral("hellip")) { out += QChar(0x2026); replaced = true; }
        else if (body == QStringLiteral("mdash")) { out += QChar(0x2014); replaced = true; }
        else if (body.startsWith(QLatin1String("#x")) || body.startsWith(QLatin1String("#X"))) {
            bool ok = false;
            const quint32 cp = body.mid(2).toUInt(&ok, 16);
            if (ok && isValidScalar(cp)) {
                appendCodePoint(out, cp);
                replaced = true;
            }
        } else if (body.startsWith(QLatin1Char('#'))) {
            bool ok = false;
            const quint32 cp = body.mid(1).toUInt(&ok, 10);
            if (ok && isValidScalar(cp)) {
                appendCodePoint(out, cp);
                replaced = true;
            }
        }
        if (!replaced)
            out += s.mid(i, semi - i + 1);  // 原样保留 "&body;"
        pos = semi + 1;
    }
    out += s.mid(pos);
    return out;
}

QString stripTags(const QString& s)
{
    QString out;
    out.reserve(s.size());
    bool inTag = false;
    for (const QChar& c : s) {
        if (c == QLatin1Char('<')) {
            inTag = true;
        } else if (c == QLatin1Char('>')) {
            inTag = false;
        } else if (!inTag) {
            out += c;
        }
    }
    // split_whitespace().join(" ") 的等价物
    return out.simplified();
}

QString urlencode(const QString& s)
{
    static const char hex[] = "0123456789ABCDEF";
    const QByteArray bytes = s.toUtf8();
    QString out;
    out.reserve(bytes.size() * 3);
    for (const char c : bytes) {
        const uchar u = static_cast<uchar>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')
            || u == '-' || u == '_' || u == '.' || u == '~') {
            out += QChar(u);
        } else {
            out += QLatin1Char('%');
            out += QLatin1Char(hex[u >> 4]);
            out += QLatin1Char(hex[u & 0x0F]);
        }
    }
    return out;
}

namespace {

int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

QString urldecode(const QString& s)
{
    const QByteArray bytes = s.toUtf8();
    QByteArray out;
    out.reserve(bytes.size());
    int i = 0;
    const int n = static_cast<int>(bytes.size());
    while (i < n) {
        const char c = bytes.at(i);
        if (c == '%' && i + 2 < n) {
            const int h = hexVal(bytes.at(i + 1));
            const int l = hexVal(bytes.at(i + 2));
            if (h >= 0 && l >= 0) {
                out += static_cast<char>((h << 4) | l);
                i += 3;
                continue;
            }
            out += c;
            ++i;
        } else if (c == '+') {
            out += ' ';
            ++i;
        } else {
            out += c;
            ++i;
        }
    }
    return QString::fromUtf8(out);  // from_utf8_lossy 等价（非法序列替换字符）
}

std::optional<QString> queryParam(const QString& url, const QString& key)
{
    const int qi = url.indexOf(QLatin1Char('?'));
    if (qi < 0)
        return std::nullopt;
    const QString q = url.mid(qi + 1);
    // 值可能包含 %26 编码的 &，因此按原始 & 切分后再逐段解码
    const QStringList kvs = q.split(QLatin1Char('&'), Qt::KeepEmptyParts);
    for (const QString& kv : kvs) {
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq < 0)
            return std::nullopt;  // 忠实移植：Rust split_once(..)? 提前返回 None
        if (urldecode(kv.left(eq)) == key)
            return urldecode(kv.mid(eq + 1));
    }
    return std::nullopt;
}

}  // namespace mw::store::html
