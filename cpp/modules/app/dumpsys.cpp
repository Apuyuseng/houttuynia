// dumpsys.rs 的 C++ 移植。
#include "dumpsys.hpp"

namespace mw::app {
namespace {

std::optional<QString> nonEmpty(const QString& v)
{
    const QString t = v.trimmed();
    if (t.isEmpty() || t == QLatin1String("null"))
        return std::nullopt;
    return t;
}

// Rust split_whitespace().next()：首个空白分隔 token。
QString firstTokenOf(const QString& s)
{
    for (int i = 0; i < s.size(); ++i) {
        const QChar ch = s.at(i);
        if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t') || ch == QLatin1Char('\r'))
            return s.left(i);
    }
    return s;
}

// 从 dumpsys 文本取“当前段”包名（供单包解析复用分段键）。
QString sectionKey(const QString& text)
{
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& l : lines) {
        QString t = l;
        while (t.startsWith(QLatin1Char(' ')) || t.startsWith(QLatin1Char('\t')))
            t.remove(0, 1);
        if (t.startsWith(QLatin1String("Package ["))) {
            const QString rest = t.mid(QString("Package [").size());
            const int end = rest.indexOf(QLatin1Char(']'));
            if (end >= 0)
                return rest.left(end);
        }
    }
    return QString();
}

} // namespace

QMap<QString, PackageDump> parseDumpsysAll(const QString& text)
{
    QMap<QString, PackageDump> map;
    QString current;
    bool inSection = false;
    PackageDump dump;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& line : lines) {
        QString t = line;
        while (t.startsWith(QLatin1Char(' ')) || t.startsWith(QLatin1Char('\t')))
            t.remove(0, 1);
        if (t.startsWith(QLatin1String("Package ["))) {
            const QString rest = t.mid(QString("Package [").size());
            const int end = rest.indexOf(QLatin1Char(']'));
            if (end >= 0) {
                if (inSection)
                    map.insert(current, dump);
                current = rest.left(end);
                dump = PackageDump{};
                inSection = true;
                continue;
            }
        }
        if (!inSection)
            continue;
        const QString trimmed = line.trimmed();
        if (!dump.versionName && trimmed.startsWith(QLatin1String("versionName=")))
            dump.versionName = nonEmpty(trimmed.mid(QString("versionName=").size()));
        if (!dump.versionCode && trimmed.startsWith(QLatin1String("versionCode="))) {
            // 形如 `versionCode=42 minSdk=24 targetSdk=34`
            const QString first = firstTokenOf(trimmed.mid(QString("versionCode=").size()));
            bool ok = false;
            const quint64 v = first.toULongLong(&ok);
            if (ok)
                dump.versionCode = v;
        }
        if (!dump.firstInstallTime && trimmed.startsWith(QLatin1String("firstInstallTime=")))
            dump.firstInstallTime = nonEmpty(trimmed.mid(QString("firstInstallTime=").size()));
        if (!dump.lastUpdateTime && trimmed.startsWith(QLatin1String("lastUpdateTime=")))
            dump.lastUpdateTime = nonEmpty(trimmed.mid(QString("lastUpdateTime=").size()));
    }
    if (inSection)
        map.insert(current, dump);
    return map;
}

PackageDump parseDumpsysPackage(const QString& text)
{
    return parseDumpsysAll(text).value(sectionKey(text), PackageDump{});
}

QSet<QString> parseProcessNames(const QString& text)
{
    QSet<QString> procs;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    bool first = true;
    for (const QString& l : lines) {
        if (first) { // NAME 表头
            first = false;
            continue;
        }
        const QString t = l.trimmed();
        if (t.isEmpty())
            continue;
        procs.insert(firstTokenOf(t));
    }
    return procs;
}

bool packageRunning(const QSet<QString>& procs, const QString& package)
{
    for (const QString& p : procs) {
        if (p == package)
            return true;
        if (p.startsWith(package)) {
            const QString rest = p.mid(package.size());
            if (rest.startsWith(QLatin1Char(':')) || rest.startsWith(QLatin1Char('.')))
                return true;
        }
    }
    return false;
}

std::optional<QString> parseResolveActivity(const QString& text)
{
    std::optional<QString> found;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& l : lines) {
        const QString t = l.trimmed();
        if (t.contains(QLatin1Char('/')) && !t.contains(QLatin1Char(' ')))
            found = t; // rfind：取最后一个匹配
    }
    return found;
}

} // namespace mw::app
