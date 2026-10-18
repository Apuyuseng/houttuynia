// emulator console 协议 ← crates/mwine-qemu/src/console.rs
#include "console.hpp"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTcpSocket>

namespace mw::qemu {

QString consoleToken()
{
    const QString home = qEnvironmentVariable("HOME");
    if (home.isEmpty())
        return QString();
    QFile f(QDir(home).filePath(QStringLiteral(".emulator_console_auth_token")));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}

mw::Result<QByteArray> consoleCommand(quint16 port, const QString& token, const QString& cmd)
{
    QTcpSocket s;
    s.connectToHost(QStringLiteral("127.0.0.1"), port);
    if (!s.waitForConnected(3000))
        // Error::Console(..)
        return mw::err<QByteArray>(QStringLiteral("连接 console :%1 失败: %2")
                                       .arg(port)
                                       .arg(s.errorString()));

    // 问候语（读到静默或超时均可，控制台行为随版本略有差异）
    if (s.waitForReadyRead(3000))
        s.read(1024);
    if (!token.isEmpty()) {
        s.write(QStringLiteral("auth %1\n").arg(token).toUtf8());
        s.waitForBytesWritten(3000);
        if (s.waitForReadyRead(3000))
            s.read(256);
    }
    s.write(QStringLiteral("%1\n").arg(cmd).toUtf8());
    if (!s.waitForBytesWritten(3000))
        return mw::err<QByteArray>(
            QStringLiteral("发送 console 命令失败: %1").arg(s.errorString()));

    // 循环读到对端静默/关闭，上限 8KB（kill/multidisplay 等应答都很短）
    QByteArray out;
    while (s.waitForReadyRead(3000)) {
        out += s.read(1024);
        if (out.size() >= 8 * 1024)
            break;
    }
    return mw::Result<QByteArray>::Ok(out);
}

bool isKo(const QByteArray& resp)
{
    return resp.contains("KO");
}

} // namespace mw::qemu
