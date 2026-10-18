// ecapture 文本事件流解析器实现 ← crates/mwine-capture/src/events.rs
#include "events.hpp"

#include <QStringList>

namespace mw::capture {

namespace {

// 空白切分（不引入正则依赖）。
QStringList splitWhitespace(const QString& s)
{
    QStringList out;
    QString cur;
    for (const QChar& c : s) {
        if (c.isSpace()) {
            if (!cur.isEmpty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.append(c);
        }
    }
    if (!cur.isEmpty())
        out.push_back(cur);
    return out;
}

// `PID:.., Comm:.., TID:.., FD:.., Tuple: [src]->[dst]`
// 返回 nullopt=格式异常；Some(None)=全零行（Closed）。
std::optional<std::optional<Event>> parseMetaLine(const QString& s)
{
    const auto grab = [&s](const char* key) -> std::optional<QString> {
        const int idx = s.indexOf(QLatin1String(key));
        if (idx < 0)
            return std::nullopt;
        const QString rest = s.mid(idx + int(qstrlen(key)));
        const int end = rest.indexOf(QLatin1Char(','));
        return rest.left(end < 0 ? rest.size() : end).trimmed();
    };

    bool ok = false;
    const auto pidStr = grab("PID:");
    if (!pidStr.has_value())
        return std::nullopt;
    const quint32 pid = pidStr->toUInt(&ok);
    if (!ok)
        return std::nullopt;
    const QString comm = grab("Comm:").value_or(QString());
    const auto tidStr = grab("TID:");
    if (!tidStr.has_value())
        return std::nullopt;
    const quint32 tid = tidStr->toUInt(&ok);
    if (!ok)
        return std::nullopt;
    const auto fdStr = grab("FD:");
    if (!fdStr.has_value())
        return std::nullopt;
    const quint32 fd = fdStr->toUInt(&ok);
    if (!ok)
        return std::nullopt;
    const QString tuple = grab("Tuple:").value_or(QString());

    // `[::ffff:10.0.2.15]:49874->[::ffff:110.242.68.66]:443` → (src, dst)
    QString src, dst;
    const int arrow = tuple.indexOf(QLatin1String("->"));
    if (arrow >= 0) {
        src = tuple.left(arrow).trimmed();
        dst = tuple.mid(arrow + 2).trimmed();
    } else {
        src = tuple;
    }

    if (pid == 0 && tid == 0 && fd == 0 && comm.isEmpty())
        return std::optional<std::optional<Event>>(std::optional<Event>()); // 全零行
    return std::optional<std::optional<Event>>(
        std::optional<Event>(Event::makeConnect(pid, comm, fd, src, dst)));
}

} // namespace


bool EventParser::feed(const QByteArray& chunk)
{
    if (chunk.isEmpty())
        return false;
    buf_.append(chunk);
    return true;
}

QVector<Event> EventParser::drain()
{
    QVector<Event> out;
    for (;;) {
        if (pending_.has_value()) {
            const qsizetype need = pending_->declared - pending_->data.size();
            if (buf_.size() < need)
                break; // 载荷未到齐，等待后续数据
            pending_->data.append(buf_.constData(), need);
            buf_.remove(0, need);
            Event ev = Event::makeIo(pending_->ts, pending_->tsMs, pending_->pid,
                                      pending_->tid, pending_->comm,
                                      pending_->direction, pending_->data);
            pending_.reset();
            // 载荷后的一个换行分隔符
            if (!buf_.isEmpty() && buf_.at(0) == '\n')
                buf_.remove(0, 1);
            out.push_back(std::move(ev));
            continue;
        }
        // 找一行
        const int nl = buf_.indexOf('\n');
        if (nl < 0)
            break; // 行未完成
        QByteArray lineBytes = buf_.left(nl);
        buf_.remove(0, nl + 1);
        if (!lineBytes.isEmpty() && lineBytes.back() == '\r')
            lineBytes.chop(1);
        const QString s = QString::fromUtf8(lineBytes);
        if (s.isEmpty())
            continue;
        if (s.startsWith(QLatin1String("PID:"))) {
            // Some(Some(ev))=连接；Some(None)=全零行（Closed）；None=格式异常跳过
            const std::optional<std::optional<Event>> meta = parseMetaLine(s);
            if (!meta.has_value())
                continue;
            if (meta->has_value())
                out.push_back(**meta);
            else
                out.push_back(Event::makeClosed());
        } else if (s.startsWith(QLatin1Char('['))) {
            auto p = parseIoHeader(s);
            if (p.has_value())
                pending_ = std::move(p);
            // 无法识别，跳过
        }
        // 其余行（ecapture 运行日志混入等）直接忽略
    }
    return out;
}

qsizetype EventParser::buffered() const
{
    return buf_.size() + (pending_.has_value() ? pending_->data.size() : 0);
}

std::optional<EventParser::PendingIo> EventParser::parseIoHeader(const QString& s)
{
    const int close = s.lastIndexOf(QLatin1Char(']'));
    if (close < 0)
        return std::nullopt;
    PendingIo p;
    p.ts = s.mid(1, close - 1).trimmed();
    p.tsMs = parseTsMs(p.ts).value_or(0);
    const QString rest = s.mid(close + 1);

    const auto field = [&rest](const char* key) -> std::optional<QString> {
        for (const QString& part : splitWhitespace(rest)) {
            if (part.startsWith(QLatin1String(key)))
                return part.mid(int(qstrlen(key)));
        }
        return std::nullopt;
    };

    bool ok = false;
    const auto pidStr = field("PID:");
    if (!pidStr.has_value())
        return std::nullopt;
    p.pid = pidStr->toUInt(&ok);
    if (!ok)
        return std::nullopt;
    const auto tidStr = field("TID:");
    if (!tidStr.has_value())
        return std::nullopt;
    p.tid = tidStr->toUInt(&ok);
    if (!ok)
        return std::nullopt;
    p.comm = field("Comm:").value_or(QString());
    if (rest.contains(QLatin1String(" WRITE "), Qt::CaseSensitive)) {
        p.direction = Direction::Write;
    } else if (rest.contains(QLatin1String(" READ "), Qt::CaseSensitive)) {
        p.direction = Direction::Read;
    } else {
        return std::nullopt;
    }
    const int lb = rest.indexOf(QLatin1Char('('));
    const int rb = rest.indexOf(QLatin1String(" bytes)"));
    if (lb < 0 || rb < 0 || rb <= lb)
        return std::nullopt;
    const quint64 declared = rest.mid(lb + 1, rb - lb - 1).trimmed().toULongLong(&ok);
    if (!ok)
        return std::nullopt;
    p.declared = qsizetype(declared);
    return p;
}

// ---------------------------------------------------------------------------
// 时间戳换算

std::optional<qint64> parseTsMs(const QString& ts)
{
    const QString b = ts;
    if (b.size() < 19)
        return std::nullopt;
    if (b.at(4) != QLatin1Char('-') || b.at(7) != QLatin1Char('-'))
        return std::nullopt;
    if (b.at(10) != QLatin1Char(' ') && b.at(10) != QLatin1Char('T'))
        return std::nullopt;
    const auto num = [&b](int a, int n) -> std::optional<qint64> {
        bool ok = false;
        const qint64 v = b.mid(a, n).toLongLong(&ok);
        if (!ok)
            return std::nullopt;
        return v;
    };
    const auto y = num(0, 4), mo = num(5, 2), d = num(8, 2);
    const auto h = num(11, 2), mi = num(14, 2), sec = num(17, 2);
    if (!y || !mo || !d || !h || !mi || !sec)
        return std::nullopt;
    qint64 ms = 0;
    if (b.size() >= 23 && b.at(19) == QLatin1Char('.'))
        ms = num(20, 3).value_or(0);
    // days_from_civil（Howard Hinnant 算法）
    const qint64 yy = (mo <= 2) ? (*y - 1) : *y;
    const qint64 era = (yy >= 0) ? (yy / 400) : ((yy - 399) / 400);
    const qint64 yoe = yy - era * 400;
    const qint64 mp = (*mo + 9) % 12;
    const qint64 doy = (153 * mp + 2) / 5 + *d - 1;
    const qint64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const qint64 days = era * 146097 + doe - 719468;
    return ((days * 86400) + *h * 3600 + *mi * 60 + *sec) * 1000 + ms;
}

} // namespace mw::capture
