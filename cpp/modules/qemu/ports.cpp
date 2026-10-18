// 端口自动分配 ← crates/mwine-qemu/src/ports.rs
#include "ports.hpp"

#include <QHostAddress>
#include <QTcpServer>

namespace mw::qemu {

bool portFree(quint16 port)
{
    // Rust TcpListener::bind(("127.0.0.1", port)) 的 bind 探测
    QTcpServer srv;
    return srv.listen(QHostAddress::LocalHost, port);
}

std::optional<QPair<quint16, quint16>> pickPortPair(quint16 start)
{
    quint32 c = start;
    while (c < 65534) {
        if (portFree(quint16(c)) && portFree(quint16(c + 1)))
            return QPair<quint16, quint16>(quint16(c), quint16(c + 1));
        c += 2;
    }
    return std::nullopt;
}

std::optional<quint16> pickPort(quint16 start)
{
    for (quint32 p = start; p <= 65535; ++p)
        if (portFree(quint16(p)))
            return quint16(p);
    return std::nullopt;
}

} // namespace mw::qemu
