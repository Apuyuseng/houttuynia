// 客户端编排实现 ← crates/mwine-scrcpy/src/client.rs
#include "client.hpp"

#include <QDebug>
#include <QFile>

#include <chrono>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mw::scrcpy {

namespace {

bool scrcpyDebugEnabled()
{
    return !qEnvironmentVariableIsEmpty("MWINE_SCRCPY_DEBUG");
}

/// 在本机分配一个空闲 TCP 端口（bind :0 后立即释放）。
mw::Result<quint16> freePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return mw::err<quint16>(errIo(QStringLiteral("socket: %1")
                                              .arg(QString::fromLatin1(::strerror(errno)))));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return mw::err<quint16>(errIo(QStringLiteral("bind: %1")
                                              .arg(QString::fromLatin1(::strerror(errno)))));
    }
    socklen_t len = sizeof(addr);
    quint16 port = 0;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        port = ntohs(addr.sin_port);
    ::close(fd);
    if (port == 0)
        return mw::err<quint16>(errIo(QStringLiteral("getsockname 失败")));
    return mw::Result<quint16>::Ok(port);
}

mw::Result<mw::adb::TcpConn> connectWithRetry(quint16 port, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        auto conn = mw::adb::TcpConn::connect(QStringLiteral("127.0.0.1"), port, 2000);
        if (conn.ok()) {
            conn.value().setNoDelay(true);
            return conn;
        }
        if (std::chrono::steady_clock::now() > deadline)
            return mw::err<mw::adb::TcpConn>(errConnectTimeout());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace

namespace {

// 稳定共享路径（与 start() 的 remoteJar 构造同源）：升级内容变化前的所有
// 会话都复用它。该路径不做会话结束清理——jar 留在设备上供下次秒启
//（老镜像重推意味着重新 dexopt 数秒）；/data/local/tmp 由 guest 重启自清。
bool isSharedStableJar(const QString& path)
{
    return path == QStringLiteral("%1-v%2.jar")
                       .arg(QString::fromLatin1(kServerRemotePath),
                            QString::fromLatin1(kServerVersion));
}

} // namespace

// ---- ControlChannel ----

mw::Result<bool> ControlChannel::send(const ControlMessage& msg)
{
    auto bytes = msg.encode();
    if (!bytes.ok())
        return mw::err<bool>(bytes.error());
    auto w = m_conn.writeAll(bytes.value());
    if (!w.ok())
        return mw::err<bool>(errIo(w.error()));
    return mw::Result<bool>::Ok();
}

mw::Result<std::optional<DeviceMessage>> ControlChannel::readDeviceMessageBlocking()
{
    TcpReader reader(m_conn, -1);
    return readDeviceMessage(reader);
}

mw::Result<std::optional<DeviceMessage>> ControlChannel::tryReadDeviceMessage()
{
    // 先以 0ms 超时探测 1 字节（类型位）：无数据 → 空结果
    auto probe = m_conn.readAtMost(1, 0);
    if (!probe.ok())
        return mw::Result<std::optional<DeviceMessage>>::Ok(std::nullopt); // 无数据
    if (probe.value().isEmpty())
        return mw::Result<std::optional<DeviceMessage>>::Ok(std::nullopt); // EOF
    // 有数据：塞回后按正常流程解析（余下字段已随后到达，短暂等待）
    TcpReader inner(m_conn, 2000);
    PushbackReader reader(inner);
    reader.pushBack(probe.value());
    return readDeviceMessage(reader);
}

// ---- SessionGuard ----

SessionGuard::~SessionGuard()
{
    stop();
}

void SessionGuard::stop()
{
    if (!m_state || m_state->stopped)
        return;
    m_state->stopped = true;
    if (m_state->serverProcess)
        m_state->serverProcess->shutdown();
    if (!m_state->serial.isEmpty())
        m_state->adb.removeForward(m_state->serial, m_state->forwardPort);
    // 远端 jar 清理移后台：adb shellBlocking 最长 10s，同步做会拖住调用方
    //（窗口关闭路径）。残留 /data/local/tmp jar 无害：随机名不会复用，
    // guest 重启自动清空。
    if (!m_state->remoteJar.isEmpty() && !m_state->serial.isEmpty()
        && !isSharedStableJar(m_state->remoteJar)) {
        const QString escaped = [jar = m_state->remoteJar]() {
            QString out;
            for (const QChar c : jar) {
                if (c == QLatin1Char('\''))
                    out += QStringLiteral("'\\''");
                else
                    out += c;
            }
            return out;
        }();
        const QString serial = m_state->serial;
        std::thread([serial, escaped] {
            mw::adb::AdbClient().shellBlocking(
                serial, QStringLiteral("rm -f '%1'").arg(escaped), 10000);
        }).detach();
    }
}

// ---- Session ----

void Session::stop()
{
    if (m_split)
        return; // split 已把清理职责移交 SessionGuard
    if (m_serverProcess) {
        m_serverProcess->shutdown();
        m_serverProcess.reset();
    }
    if (!serial.isEmpty()) {
        adb.removeForward(serial, forwardPort);
        cleanupJar();
    }
}

void Session::cleanupJar()
{
    if (m_remoteJar.isEmpty() || serial.isEmpty() || isSharedStableJar(m_remoteJar))
        return;
    QString escaped;
    for (const QChar c : m_remoteJar) {
        if (c == QLatin1Char('\''))
            escaped += QStringLiteral("'\\''");
        else
            escaped += c;
    }
    adb.shellBlocking(serial, QStringLiteral("rm -f '%1'").arg(escaped), 10000);
}

Session::Parts Session::split()
{
    Parts parts;
    parts.video = std::move(video);
    parts.control = std::move(control);
    parts.audio = std::move(audio);

    auto state = std::make_shared<SessionGuard::State>();
    state->adb = adb;
    state->serial = serial;
    state->forwardPort = forwardPort;
    state->remoteJar = m_remoteJar;
    state->serverProcess = std::move(m_serverProcess);
    parts.guard = SessionGuard(std::move(state));

    // 搬空本会话（析构 / 再调 stop 不再重复清理）
    m_split = true;
    m_serverProcess.reset();
    m_remoteJar.clear();
    serial.clear();
    deviceName.clear();
    forwardPort = 0;
    return parts;
}

// ---- ScrcpyClient ----

mw::Result<Session> ScrcpyClient::start() &&
{
    auto valid = options.validate();
    if (!valid.ok())
        return mw::err<Session>(valid.error());
    if (!QFile::exists(serverJar))
        return mw::err<Session>(errServerJarNotFound(serverJar));

    // 1. 推送 server jar
    //    远端用「稳定路径 + 同尺寸跳过」而非每会话随机路径：Android 5.0 的 ART
    //    对每个新 jar 路径都要做一次 dexopt（老镜像上数秒），随机路径意味着
    //    每次开流都付出冷 dexopt——首连探测会先于 server 就绪耗尽耐心，
    //    半开连接还把刚 accept 的 server 拖死（2026-10-20 实测定位）。
    //    并发会话共享同一 jar 没有安全问题（jar 只读；scid 隔离 socket），
    //    上游 scrcpy 也是单一远端路径。内容变化（升级）时覆盖推送一次即可。
    const quint32 session = options.scid.value_or(ServerOptions::randomScid());
    options.scid = session;
    const QString remoteJar = QStringLiteral("%1-v%2.jar")
                                  .arg(QString::fromLatin1(kServerRemotePath),
                                       QString::fromLatin1(kServerVersion));

    QFile jarFile(serverJar);
    if (!jarFile.open(QIODevice::ReadOnly))
        return mw::err<Session>(errIo(QStringLiteral("读取 server jar 失败: %1").arg(serverJar)));
    const QByteArray jar = jarFile.readAll();
    // 已存在同尺寸 jar 则跳过推送（dexopt 已热，启动快一个数量级）
    bool skipPush = false;
    {
        const auto st = adb.shellBlocking(
            serial, QStringLiteral("stat -c %s '%1' 2>/dev/null || echo 0").arg(remoteJar), 5000);
        if (st.ok() && st.value().outStr().trimmed().toLongLong() == jar.size())
            skipPush = true;
    }
    if (!skipPush) {
        auto pushed = adb.pushBytes(serial, jar, remoteJar);
        if (!pushed.ok())
            return mw::err<Session>(errLaunchFailed(
                QStringLiteral("push server jar 失败: %1").arg(pushed.error())));
    }

    // 2. 分配转发端口（socket 名由 scid 决定，多会话天然隔离）
    auto port = freePort();
    if (!port.ok())
        return mw::err<Session>(port.error());
    {
        auto fwd = adb.forwardAbstract(serial, port.value(), options.socketName());
        if (!fwd.ok())
            return mw::err<Session>(
                errLaunchFailed(QStringLiteral("forward 失败: %1").arg(fwd.error())));
    }

    // 3. 启动 server 进程（长驻 shell；持有句柄即保持存活）
    const QVector<QString> args = options.toArgs();
    const QString cmd = serverCommand(remoteJar, args);
    auto serverProcess = adb.shellStream(serial, cmd);
    if (!serverProcess.ok()) {
        return mw::err<Session>(errLaunchFailed(
            QStringLiteral("启动 server 进程失败: %1").arg(serverProcess.error())));
    }

    if (scrcpyDebugEnabled())
        qInfo().noquote() << "[scrcpy] step4: 连接 video";
    // 4. 连接 video socket 并读取 dummy 字节。
    // adb forward 在 abstract socket 尚未创建时也会接受 TCP 连接后立刻关闭，
    // 因此必须把"读到 dummy 字节"作为 server 就绪判据，失败即重连。
    // 首探前置等待：server 进程启动到监听需数百毫秒，零延迟首探几乎必失败
    // （每次失败探测 = 一次经 emulator ADB vsock 代理的被拒连接，高频抖动
    // 正是上游 AdbVsyncPipe 竞态崩溃的触发面），先等 400ms 再探可消掉
    // 绝大多数失败探测。
    // 首个 dummy 读取给足 12s：直连 accept 池内部对未绑定的 abstract socket
    // 重试 ~10s——探测方 2s 就放弃会把 server 刚 accept 的连接掐死，
    // server 视首个连接断开为会话结束直接退出（老镜像冷启动必现）。
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    mw::adb::TcpConn videoTcp;
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool got = false;
        bool first = true;
        while (std::chrono::steady_clock::now() < deadline) {
            auto conn = mw::adb::TcpConn::connect(QStringLiteral("127.0.0.1"), port.value(), 2000);
            if (!conn.ok()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            conn.value().setNoDelay(true);
            auto dummy = conn.value().readAtMost(1, first ? 12000 : 2000);
            if (dummy.ok() && !dummy.value().isEmpty()) {
                videoTcp = std::move(conn.value());
                got = true;
                break;
            }
            first = false;
            // server 尚未监听（或刚启动即退出），重试
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!got)
            return mw::err<Session>(errConnectTimeout());
    }

    if (scrcpyDebugEnabled())
        qInfo().noquote() << "[scrcpy] step5: audio/control";
    // 5. 连接 audio socket（若启用；server 在 video 后按序 accept 它），
    //    再连接 control socket——全部 accept 完成后才发送 64 字节设备名
    // server 只 accept 启用的 socket：control=false 时连了也永远无人 accept
    //（客户端会 15s 超时并连带泄漏 forward 与 server 进程）
    std::optional<mw::adb::TcpConn> audioTcp;
    if (options.audio) {
        auto c = connectWithRetry(port.value(), 30000);
        if (!c.ok())
            return mw::err<Session>(c.error());
        audioTcp = std::move(c.value());
    }
    std::optional<mw::adb::TcpConn> controlTcp;
    if (options.control) {
        auto c = connectWithRetry(port.value(), 30000);
        if (!c.ok())
            return mw::err<Session>(c.error());
        controlTcp = std::move(c.value());
    }

    if (scrcpyDebugEnabled())
        qInfo().noquote() << "[scrcpy] step6: 读设备名";
    QString deviceName;
    {
        TcpReader nameReader(videoTcp, 10000);
        auto name = readDeviceName(nameReader);
        if (!name.ok())
            return mw::err<Session>(name.error());
        deviceName = name.value();
    }

    if (scrcpyDebugEnabled())
        qInfo().noquote() << "[scrcpy] step7: VideoStream::create";
    // 6. 流（codec id 紧跟设备名之后）
    auto video = VideoStream::create(std::move(videoTcp));
    if (!video.ok())
        return mw::err<Session>(video.error());

    std::optional<AudioStream> audioStream;
    if (audioTcp) {
        auto a = AudioStream::create(std::move(*audioTcp));
        if (!a.ok())
            return mw::err<Session>(a.error());
        audioStream = std::move(a.value());
    }

    Session s;
    s.adb = adb;
    s.serial = serial;
    s.deviceName = deviceName;
    s.video = std::move(video.value());
    s.audio = std::move(audioStream);
    if (controlTcp)
        s.control = ControlChannel(std::move(*controlTcp));
    s.m_serverProcess = std::move(serverProcess.value());
    s.forwardPort = port.value();
    s.m_remoteJar = remoteJar;
    return mw::Result<Session>::Ok(std::move(s));
    // 注意：m_serverProcess 一旦会话析构（Session 销毁），远端进程随之退出
}

} // namespace mw::scrcpy
