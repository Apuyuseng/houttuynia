#include "devices.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

#include "process.hpp"

namespace mw::adb {

namespace {

// ZXing WIFI 载荷转义还原：`\;` `\:` `\,` `\\`
QString unescapeWifiField(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) == QLatin1Char('\\') && i + 1 < s.size()) {
            const QChar next = s.at(i + 1);
            if (next == QLatin1Char(';') || next == QLatin1Char(':')
                || next == QLatin1Char(',') || next == QLatin1Char('\\')) {
                out.append(next);
                ++i;
                continue;
            }
        }
        out.append(s.at(i));
    }
    return out;
}

// 按未转义分号切分 WIFI 载荷字段
QStringList splitUnescaped(const QString& s)
{
    QStringList out;
    QString cur;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\') && i + 1 < s.size()) {
            cur.append(c);
            cur.append(s.at(i + 1));
            ++i;
            continue;
        }
        if (c == QLatin1Char(';')) {
            out.append(cur);
            cur.clear();
            continue;
        }
        cur.append(c);
    }
    if (!cur.isEmpty())
        out.append(cur);
    return out;
}

} // namespace

mw::Result<PairQr> parsePairQr(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return mw::err<PairQr>(QStringLiteral("二维码内容为空"));
    // 裸地址形态：直接就是 host:port
    if (looksLikeAddr(trimmed)) {
        PairQr q;
        q.addr = trimmed;
        return mw::Result<PairQr>::Ok(q);
    }
    const QString prefix = QStringLiteral("WIFI:");
    if (!trimmed.startsWith(prefix, Qt::CaseInsensitive))
        return mw::err<PairQr>(
            QStringLiteral("不是 ADB 配对二维码（应以 WIFI:T:ADB 开头）"));
    PairQr q;
    bool typeOk = false;
    const QStringList fields = splitUnescaped(trimmed.mid(prefix.size()));
    for (const QString& f : fields) {
        const int colon = f.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = f.left(colon).toUpper();
        const QString value = unescapeWifiField(f.mid(colon + 1));
        if (key == QLatin1String("T")) {
            typeOk = value.compare(QLatin1String("ADB"), Qt::CaseInsensitive) == 0;
        } else if (key == QLatin1String("S")) {
            q.service = value;
        } else if (key == QLatin1String("P")) {
            q.password = value;
        } else if (key == QLatin1String("C")) {
            // 少数实现把连接地址放进 C 字段
            if (looksLikeAddr(value))
                q.addr = value;
        } // M: 等未知字段忽略
    }
    if (!typeOk)
        return mw::err<PairQr>(QStringLiteral("不是 ADB 配对二维码（T 字段非 ADB）"));
    if (q.password.isEmpty())
        return mw::err<PairQr>(QStringLiteral("二维码缺少配对码（P 字段）"));
    return mw::Result<PairQr>::Ok(q);
}

QVector<MdnsService> parseMdnsServices(const QString& adbOutput)
{
    QVector<MdnsService> out;
    const QStringList lines = adbOutput.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QStringList toks =
            raw.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        QString instance, type, addrTok;
        if (toks.size() >= 3 && toks.at(1).startsWith(QLatin1String("_adb"))) {
            // 三段形态：实例名 服务类型 ip:port
            instance = toks.at(0);
            type = toks.at(1);
            addrTok = toks.at(2);
        } else if (toks.size() >= 2) {
            // 两段形态：实例名._adb-xxx._tcp ip:port（实例与类型连写）
            const int split = toks.at(0).indexOf(QLatin1String("._adb"));
            if (split <= 0)
                continue;
            instance = toks.at(0).left(split);
            type = toks.at(0).mid(split + 1);
            addrTok = toks.at(1);
        } else {
            continue;
        }
        const int colon = addrTok.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0 || colon + 1 >= addrTok.size())
            continue;
        bool ok = false;
        const quint16 port = addrTok.mid(colon + 1).toUShort(&ok);
        if (!ok || port == 0)
            continue;
        MdnsService s;
        s.instance = instance;
        s.type = type;
        s.host = addrTok.left(colon);
        s.port = port;
        // 后端可能重复通告（IPv4/IPv6 各一行）：按实例+类型去重，保留首个
        const auto dup = std::find_if(out.cbegin(), out.cend(), [&](const MdnsService& e) {
            return e.instance == s.instance && e.type == s.type;
        });
        if (dup == out.cend())
            out.append(s);
    }
    return out;
}

mw::Result<QVector<MdnsService>> discoverMdnsServices(const QString& adbBin)
{
    if (adbBin.isEmpty())
        return mw::err<QVector<MdnsService>>(
            QStringLiteral("未找到 adb 二进制（运行时未安装？）"));
    // mdns check 唤醒 mDNS 后端（幂等；部分版本在未初始化时 services 输出为空）
    mw::runProcess(adbBin, { QStringLiteral("mdns"), QStringLiteral("check") }, 10000);
    const auto out = mw::runProcess(
        adbBin, { QStringLiteral("mdns"), QStringLiteral("services") }, 15000);
    if (!out.error.isEmpty())
        return mw::err<QVector<MdnsService>>(
            QStringLiteral("adb mdns services 执行失败: %1").arg(out.error));
    return mw::Result<QVector<MdnsService>>::Ok(
        parseMdnsServices(out.stdOutStr() + QLatin1Char('\n') + out.stdErrStr()));
}

QString findAdbBinary()
{
    // 1) PATH
    const QString path = qEnvironmentVariable("PATH");
    if (!path.isEmpty()) {
        const QStringList dirs = path.split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const QString& dir : dirs) {
            const QString candidate = QDir(dir).filePath(QStringLiteral("adb"));
            if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile())
                return candidate;
        }
    }
    // 2) 打包布局 .app 与 开发树 runtime/（exeDir 向上逐级找）
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (!exeDir.isEmpty()) {
        QDir d(exeDir);
        for (;;) {
            for (const QString& rel : { QStringLiteral("Resources/runtime/platform-tools/adb"),
                                        QStringLiteral("runtime/platform-tools/adb") }) {
                const QString cand = d.filePath(rel);
                if (QFileInfo::exists(cand) && QFileInfo(cand).isFile())
                    return cand;
            }
            if (!d.cdUp())
                break;
        }
    }
    return QString();
}

mw::Result<QString> runPair(const QString& adbBin, const QString& addr, const QString& code,
                            int timeoutMs)
{
    if (adbBin.isEmpty())
        return mw::err<QString>(QStringLiteral("未找到 adb 二进制"));
    if (!looksLikeAddr(addr))
        return mw::err<QString>(QStringLiteral("配对地址格式应为 ip:端口"));
    if (code.trimmed().isEmpty())
        return mw::err<QString>(QStringLiteral("配对码为空"));
    const auto out = mw::runProcess(adbBin,
                                    { QStringLiteral("pair"), addr, code.trimmed() },
                                    timeoutMs);
    const QString text = (out.stdOutStr() + QLatin1Char('\n') + out.stdErrStr())
                             .simplified();
    if (text.contains(QLatin1String("Successfully paired"), Qt::CaseInsensitive))
        return mw::Result<QString>::Ok(text);
    if (!out.error.isEmpty())
        return mw::err<QString>(QStringLiteral("adb pair 执行失败: %1").arg(out.error));
    return mw::err<QString>(text.isEmpty()
                                ? QStringLiteral("adb pair 失败（无输出）")
                                : text);
}

mw::Result<QString> runTcpip(const QString& adbBin, const QString& serial, quint16 port,
                             int timeoutMs)
{
    if (adbBin.isEmpty())
        return mw::err<QString>(QStringLiteral("未找到 adb 二进制"));
    const auto out = mw::runProcess(
        adbBin,
        { QStringLiteral("-s"), serial, QStringLiteral("tcpip"),
          QString::number(port) },
        timeoutMs);
    const QString text = (out.stdOutStr() + QLatin1Char('\n') + out.stdErrStr())
                             .simplified();
    if (out.ok())
        return mw::Result<QString>::Ok(text);
    if (!out.error.isEmpty())
        return mw::err<QString>(QStringLiteral("adb tcpip 执行失败: %1").arg(out.error));
    return mw::err<QString>(text.isEmpty() ? QStringLiteral("adb tcpip 失败（无输出）")
                                           : text);
}

bool looksLikeAddr(const QString& s)
{
    const QRegularExpression re(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9.\\-]*:(\\d{1,5})$"));
    const auto m = re.match(s.trimmed());
    if (!m.hasMatch())
        return false;
    bool ok = false;
    const quint16 port = m.captured(1).toUShort(&ok);
    return ok && port > 0;
}

} // namespace mw::adb
