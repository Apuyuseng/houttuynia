// badging.rs 的 C++ 移植。
#include "badging.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

#include <algorithm>

#include "process.hpp"

namespace mw::app {
namespace {

QString unquote(const QString& s)
{
    QString t = s.trimmed();
    if (t.size() >= 2 && t.startsWith(QLatin1Char('\'')) && t.endsWith(QLatin1Char('\'')))
        t = t.mid(1, t.size() - 2);
    return t;
}

// Rust split_whitespace().next()：首个空白分隔 token。
QString firstToken(const QString& s)
{
    for (int i = 0; i < s.size(); ++i) {
        const QChar ch = s.at(i);
        if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t') || ch == QLatin1Char('\r'))
            return s.left(i);
    }
    return s;
}

} // namespace

bool isRasterIcon(const QString& path)
{
    const QString lower = path.toLower();
    return lower.endsWith(QLatin1String(".png")) || lower.endsWith(QLatin1String(".webp"))
        || lower.endsWith(QLatin1String(".jpg")) || lower.endsWith(QLatin1String(".jpeg"));
}

ApkBadging parseBadging(const QString& text)
{
    ApkBadging b;
    std::optional<QString> package;
    std::optional<QString> label;
    QVector<QPair<quint32, QString>> icons;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString& line : lines) {
        if (!package && line.startsWith(QLatin1String("package:"))) {
            const QString rest = line.mid(QString("package:").size());
            const int idx = rest.indexOf(QLatin1String("name="));
            if (idx >= 0) {
                const QString v = unquote(firstToken(rest.mid(idx + 5)));
                if (!v.isEmpty())
                    package = v;
            }
        }
        if (!label && line.startsWith(QLatin1String("application-label:"))) {
            const QString v = unquote(line.mid(QString("application-label:").size()));
            if (!v.isEmpty())
                label = v;
        }
        if (line.startsWith(QLatin1String("application-icon-"))) {
            const QString rest = line.mid(QString("application-icon-").size());
            const int colon = rest.indexOf(QLatin1Char(':'));
            if (colon > 0) {
                bool ok = false;
                const quint32 dpi = rest.left(colon).trimmed().toUInt(&ok);
                if (ok) {
                    const QString path = unquote(rest.mid(colon + 1));
                    if (!path.isEmpty())
                        icons.push_back({dpi, path});
                }
            }
        }
    }
    // 优先位图资源；全是 adaptive xml 时保留（由调用方做位图回退扫描）
    QVector<QPair<quint32, QString>> pool;
    for (const auto& ic : icons) {
        if (isRasterIcon(ic.second))
            pool.push_back(ic);
    }
    if (pool.isEmpty())
        pool = icons;
    // max_by_key 平手取最后一个：>= 保序迭代即得
    const QPair<quint32, QString>* best = nullptr;
    for (const auto& ic : pool) {
        if (!best || ic.first >= best->first)
            best = &ic;
    }
    b.package = package.value_or(QString());
    b.label = label;
    b.icon = best ? std::optional<QString>(best->second) : std::nullopt;
    return b;
}

std::optional<QString> pickRasterFallback(const QStringList& entries, const QString& xmlIcon)
{
    const QString leaf = xmlIcon.section(QLatin1Char('/'), -1);
    const QString stem = leaf.section(QLatin1Char('.'), 0, 0);
    if (stem.isEmpty())
        return std::nullopt;
    const auto densityOf = [](const QString& path) -> quint32 {
        static const std::pair<const char*, quint32> names[] = {
            {"xxxhdpi", 640}, {"xxhdpi", 480}, {"xhdpi", 320}, {"hdpi", 240}, {"mdpi", 160},
        };
        const QStringList segs = path.split(QLatin1Char('/'));
        for (const QString& seg : segs) {
            for (const auto& [name, dpi] : names) {
                if (seg.contains(QLatin1String(name)))
                    return dpi;
            }
        }
        return 0;
    };
    std::optional<QString> best;
    quint32 bestDpi = 0;
    for (const QString& e : entries) {
        if (!isRasterIcon(e) || !e.startsWith(QLatin1String("res/")))
            continue;
        const QString eStem = e.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
        if (eStem != stem)
            continue;
        const quint32 d = densityOf(e);
        if (!best || d > bestDpi) {
            best = e;
            bestDpi = d;
        }
    }
    return best;
}

std::optional<QString> findAapt2()
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const auto isFile = [](const QString& p) {
        return !p.isEmpty() && QFileInfo(p).isFile();
    };

    if (env.contains(QStringLiteral("MWINE_AAPT2"))) {
        const QString p = env.value(QStringLiteral("MWINE_AAPT2"));
        if (isFile(p))
            return p;
    }
    // PATH
    const QString path = env.value(QStringLiteral("PATH"));
    const QStringList dirs = path.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString& dir : dirs) {
        const QString candidate = QDir(dir).filePath(QStringLiteral("aapt2"));
        if (isFile(candidate))
            return candidate;
    }
    QString sdk = env.value(QStringLiteral("ANDROID_SDK_ROOT"));
    if (sdk.isEmpty())
        sdk = env.value(QStringLiteral("ANDROID_HOME"));
#ifdef Q_OS_MACOS
    if (sdk.isEmpty()) {
        // macOS Android Studio 默认安装路径（无需环境变量）
        const QString home = env.value(QStringLiteral("HOME"));
        if (!home.isEmpty())
            sdk = QDir(home).filePath(QStringLiteral("Library/Android/sdk"));
    }
#endif
    if (sdk.isEmpty())
        return std::nullopt;
    const QDir bt(QDir(sdk).filePath(QStringLiteral("build-tools")));
    QStringList versions;
    const auto entries = bt.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& e : entries) {
        const QString tool = QDir(e.absoluteFilePath()).filePath(QStringLiteral("aapt2"));
        if (isFile(tool))
            versions << e.absoluteFilePath(); // 与 Rust 一致按含 aapt2 的目录名排序
    }
    std::sort(versions.begin(), versions.end());
    return versions.isEmpty() ? std::nullopt : std::optional<QString>(versions.last());
}

std::optional<ApkBadging> aapt2Badging(const QString& aapt2, const QString& apkPath)
{
    const auto out = mw::runProcess(aapt2,
                                    {QStringLiteral("dump"), QStringLiteral("badging"), apkPath},
                                    120000);
    if (!out.ok())
        return std::nullopt;
    const ApkBadging badging = parseBadging(out.stdOutStr());
    if (badging.package.isEmpty())
        return std::nullopt;
    return badging;
}

} // namespace mw::app
