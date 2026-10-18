#include "tcpconn.hpp"

#include <QLoggingCategory>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace mw::adb {

namespace {
QString errnoStr() { return QString::fromLatin1(::strerror(errno)); }
} // namespace

mw::Result<TcpConn> TcpConn::connect(const QString& host, quint16 port, int timeoutMs)
{
    const QString service = QString::number(port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int gai = ::getaddrinfo(host.toUtf8().constData(), service.toUtf8().constData(), &hints, &res);
    if (gai != 0 || !res)
        return mw::err<TcpConn>(QStringLiteral("connect %1:%2: 域名解析失败: %3")
                                    .arg(host)
                                    .arg(port)
                                    .arg(QString::fromLatin1(gai_strerror(gai))));

    QString lastErr = QStringLiteral("no address");
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        const qintptr fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastErr = errnoStr();
            continue;
        }
        // 非阻塞 connect + poll 兑现 timeoutMs，成功后恢复阻塞模式
        const int oldFlags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, (oldFlags >= 0 ? oldFlags : 0) | O_NONBLOCK);
        bool established = false;
        const int c = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (c == 0) {
            established = true;
        } else if (errno == EINPROGRESS) {
            pollfd p{};
            p.fd = static_cast<int>(fd);
            p.events = POLLOUT;
            const int pr = ::poll(&p, 1, timeoutMs < 0 ? -1 : timeoutMs);
            if (pr > 0) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0)
                    established = true;
                else
                    lastErr = soerr != 0 ? QString::fromLatin1(::strerror(soerr))
                                         : QStringLiteral("connect failed");
            } else if (pr == 0) {
                lastErr = QStringLiteral("connect 超时（%1ms）").arg(timeoutMs);
            } else {
                lastErr = errnoStr();
            }
        } else {
            lastErr = errnoStr();
        }
        if (established) {
            if (oldFlags >= 0)
                ::fcntl(fd, F_SETFL, oldFlags); // 恢复阻塞
            TcpConn conn(fd);
            conn.setNoDelay(true);
            conn.setSocketTimeout(timeoutMs > 0 ? timeoutMs : 0);
            ::freeaddrinfo(res);
            return mw::Result<TcpConn>::Ok(std::move(conn));
        }
        ::close(fd);
    }
    ::freeaddrinfo(res);
    return mw::err<TcpConn>(
        QStringLiteral("connect %1:%2: %3").arg(host).arg(port).arg(lastErr));
}

mw::Result<qint64> TcpConn::writeAll(const QByteArray& data) const
{
    if (m_fd < 0)
        return mw::err<qint64>(QStringLiteral("连接无效"));
    const char* p = data.constData();
    qint64 remaining = data.size();
    qint64 written = 0;
    while (remaining > 0) {
        const ssize_t n = ::write(m_fd, p, static_cast<size_t>(remaining));
        if (n > 0) {
            p += n;
            remaining -= n;
            written += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 发送缓冲满：等待可写再试（SO_SNDTIMEO 之外的第二道闸）
            pollfd pf{};
            pf.fd = static_cast<int>(m_fd);
            pf.events = POLLOUT;
            const int pr = ::poll(&pf, 1, 5000);
            if (pr > 0)
                continue;
            if (pr < 0 && errno == EINTR)
                continue;
            return mw::err<qint64>(pr == 0 ? QStringLiteral("写超时") : errnoStr());
        }
        return mw::err<qint64>(errnoStr());
    }
    return mw::Result<qint64>::Ok(written);
}

mw::Result<QByteArray> TcpConn::readAtMost(int maxBytes, int timeoutMs) const
{
    if (m_fd < 0)
        return mw::err<QByteArray>(QStringLiteral("连接无效"));
    if (maxBytes <= 0)
        return mw::Result<QByteArray>::Ok(QByteArray());
    char buf[64 * 1024];
    const int want = maxBytes < int(sizeof(buf)) ? maxBytes : int(sizeof(buf));
    for (;;) {
        pollfd p{};
        p.fd = static_cast<int>(m_fd);
        p.events = POLLIN;
        const int pr = ::poll(&p, 1, timeoutMs < 0 ? -1 : timeoutMs);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return mw::err<QByteArray>(errnoStr());
        }
        if (pr == 0)
            return mw::err<QByteArray>(QStringLiteral("读超时（%1ms）").arg(timeoutMs));
        if ((p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0)
            continue; // 罕见：无关心事件，重试
        const ssize_t n = ::recv(m_fd, buf, static_cast<size_t>(want), MSG_DONTWAIT);
        if (n > 0)
            return mw::Result<QByteArray>::Ok(QByteArray(buf, int(n)));
        if (n == 0)
            return mw::Result<QByteArray>::Ok(QByteArray()); // EOF
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue; // 竞态：数据被别的 dup 句柄取走，重回 poll
        return mw::err<QByteArray>(errnoStr());
    }
}

mw::Result<QByteArray> TcpConn::readExact(int n, int timeoutMs) const
{
    if (n <= 0)
        return mw::Result<QByteArray>::Ok(QByteArray());
    QByteArray buf(n, Qt::Uninitialized);
    char* dst = buf.data();
    int filled = 0;
    while (filled < n) {
        auto r = readAtMost(n - filled, timeoutMs);
        if (!r.ok())
            return mw::err<QByteArray>(r.error());
        const QByteArray& part = r.value();
        if (part.isEmpty())
            return mw::err<QByteArray>(errIo(QStringLiteral("连接提前关闭（已读 %1/%2 字节）")
                                                 .arg(filled)
                                                 .arg(n)));
        ::memcpy(dst + filled, part.constData(), static_cast<size_t>(part.size()));
        filled += part.size();
    }
    return mw::Result<QByteArray>::Ok(buf);
}

TcpConn TcpConn::dup() const
{
    if (m_fd < 0)
        return TcpConn();
    const qintptr d = ::dup(m_fd);
    return d >= 0 ? TcpConn(d) : TcpConn();
}

void TcpConn::shutdownBoth() const
{
    if (m_fd >= 0)
        ::shutdown(static_cast<int>(m_fd), SHUT_RDWR);
}

void TcpConn::shutdownWrite() const
{
    if (m_fd >= 0)
        ::shutdown(static_cast<int>(m_fd), SHUT_WR);
}

qintptr TcpConn::release()
{
    const qintptr fd = m_fd;
    m_fd = -1;
    return fd;
}

void TcpConn::setSocketTimeout(int timeoutMs) const
{
    if (m_fd < 0)
        return;
    timeval tv{};
    if (timeoutMs > 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
    }
    ::setsockopt(static_cast<int>(m_fd), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(static_cast<int>(m_fd), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void TcpConn::setNoDelay(bool on) const
{
    if (m_fd < 0)
        return;
    const int one = on ? 1 : 0;
    ::setsockopt(static_cast<int>(m_fd), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void TcpConn::closeFd()
{
    if (m_fd >= 0) {
        if (qEnvironmentVariableIsSet("MWINE_FD_TRACE")) {
            void* frames[40];
            const int n = ::backtrace(frames, 40);
            char** syms = ::backtrace_symbols(frames, n);
            qWarning().noquote() << "[fd-trace] close fd=" << m_fd;
            for (int i = 2; i < n && i < 22; ++i)
                qWarning().noquote() << "   " << QString::fromLatin1(syms[i]);
            ::free(syms);
        }
        ::close(static_cast<int>(m_fd));
        m_fd = -1;
    }
}

} // namespace mw::adb
