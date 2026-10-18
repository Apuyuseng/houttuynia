// 高层 ADB 客户端：设备发现、shell、推送、安装、端口转发。
// ← crates/mwine-adb/src/client.rs（AdbClient/InstallProgress/AdbDevice/
//    ShellSession/ShellV1Session/ShellStream）+ src/sync.rs（SyncChannel）
//
// 线程安全：AdbClient 无状态（host/port 拷贝），每次调用新建连接——与 Rust 一致。
#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QPair>
#include <QString>
#include <QVector>
#include <functional>
#include <optional>

#include "result.hpp"
#include "shell.hpp"
#include "tcpconn.hpp"
#include "wire.hpp"

namespace mw::adb {

constexpr quint16 kDefaultAdbPort = 5037;

struct AdbDevice {
    QString serial;
    QString state;
};

// APK 安装进度（Rust enum InstallProgress）
struct InstallProgress {
    enum class Kind { Push, Installing };
    Kind kind = Kind::Installing;
    quint64 sent = 0; // Push：已传字节
    quint64 total = 0; // Push：总量字节

    static InstallProgress push(quint64 sent, quint64 total)
    {
        return InstallProgress{Kind::Push, sent, total};
    }
    static InstallProgress installing() { return InstallProgress{Kind::Installing, 0, 0}; }
};

// adb sync 子协议：文件推送（SEND/DATA/DONE，回复 OKAY/FAIL）与拉取（STAT/RECV）。
// 所有整数均为小端。（Rust SyncChannel；借持 TcpConn，作用域内使用）
class SyncChannel {
public:
    SyncChannel(TcpConn& conn, int timeoutMs = 30000) : m_conn(conn), m_timeoutMs(timeoutMs) {}

    mw::Result<bool> sendHeader(const QString& remotePath, quint32 mode);
    mw::Result<bool> dataChunk(const QByteArray& data);
    mw::Result<bool> done(quint32 mtime);
    mw::Result<bool> quit();
    mw::Result<bool> readReply();

    // 流式推送（边读边发，进度回调按已发字节报告；避免大 APK 整读内存）
    mw::Result<bool> pushStream(QIODevice& reader, const QString& remotePath,
                                const std::function<void(quint64)>& onProgress);
    // 完整推送一段内存数据到设备
    mw::Result<bool> pushBytes(const QByteArray& data, const QString& remotePath);
    // 拉取设备文件（STAT 校验 + RECV 循环），返回全部内容
    mw::Result<QByteArray> pullFile(const QString& remotePath);

private:
    mw::Result<bool> writeRequest(const char cmd[4], const QByteArray& payload);

    TcpConn& m_conn;
    int m_timeoutMs;
};

// 长驻 shell 会话：reader 读去帧后的 stdout，句柄可从其他线程关停（Rust ShellSession）
class ShellSession {
public:
    ShellSession() = default;
    ShellSession(ShellV2Stream rdr, TcpConn stream)
        : reader(std::move(rdr)), m_stream(std::move(stream))
    {
    }

    ShellV2Stream reader; // v2 协议读流（仅 stdout 载荷）

    // 连接克隆：对其 shutdownBoth()/析构即断开整条会话
    TcpConn shutdownHandle() const { return m_stream.dup(); }
    const TcpConn& stream() const { return m_stream; }

private:
    TcpConn m_stream; // 底层连接（Drop 时断流终止远端进程）
};

// 长驻 shell 进程句柄：保持连接打开即保持远端进程存活（Rust ShellStream）
class ShellStream {
public:
    ShellStream() = default;
    explicit ShellStream(TcpConn conn) : m_conn(std::move(conn)) {}

    // 非阻塞地丢弃远端输出（减少缓冲堆积）
    mw::Result<qint64> drainAvailable();
    // 主动断开（远端进程将收到 EOF 并退出）
    void shutdown();

private:
    TcpConn m_conn;
};

class AdbClient {
public:
    QString host;
    quint16 port = kDefaultAdbPort;
    // 直连模式（默认）：绕过 adb server，直接连模拟器内建 adbd 的 TCP 端口
    //（发现注册表 port.adb / serial 规则 console+1）。产品只有模拟器设备且
    // 镜像 ro.adb.secure=0 免认证，无 server 必要。MWINE_ADB_SERVER=1 强制
    // 回退 server 模式（真机/调试用）。
    bool direct = true;

    AdbClient(); // 127.0.0.1:5037；MWINE_ADB_SERVER 置位时 direct=false
    static AdbClient serverMode(); // 强制 adb server 模式（传统路径）

    // 发现注册表里的全部模拟器：(serial, adbd_port)
    QVector<QPair<QString, quint16>> directDevices() const;
    // 直连模式：打开设备侧 abstract socket（等效 forward + 本地 TCP 二合一）。
    // 返回已就绪的裸流（服务协商完成）。
    mw::Result<TcpConn> openAbstract(const QString& serial, const QString& name) const;

    mw::Result<QString> version() const;
    // adb server 不在线时尝试用 adb 二进制拉起，然后重试连接。
    // 直连模式无需 server：恒成功。
    mw::Result<bool> ensureServer(const QString& adbBin = QString()) const;
    // 重启 adb server（模拟器晚于 server 启动错过端口注册时用）
    mw::Result<bool> restartServer() const;
    // adb root：让 adbd 以 root 运行（userdebug/eng 镜像可用）。
    // 重启 adbd 会短暂掉线，返回后调用方通常需 waitForDevice。
    mw::Result<QString> root(const QString& serial) const;
    mw::Result<QVector<AdbDevice>> devices() const;
    mw::Result<AdbDevice> findOnlineDevice() const;
    // 通过 adb server 建立 TCP 设备连接（host:connect），等效 `adb connect <addr>`
    mw::Result<QString> connectNetwork(const QString& addr) const;
    // 断开无线连接（host:disconnect），等效 `adb disconnect <addr>`。
    // 返回 server 应答文本（"disconnected ..." / "no such device" 等）。
    mw::Result<QString> disconnectNetwork(const QString& addr) const;
    // 等待指定 serial 上线并处于 device 状态
    mw::Result<bool> waitForDevice(const QString& serial, int timeoutMs) const;

    // 执行 shell 命令并等待结束（shell v2，分离 stdout/stderr/exit code；
    // 协商被拒时回退 v1：混流、无退出码）
    mw::Result<ShellOutput> shellBlocking(const QString& serial, const QString& cmd,
                                          int timeoutMs) const;
    // 启动长驻 shell 进程（v2 raw 流），reader 持续读去帧后的 stdout
    mw::Result<ShellSession> shellSession(const QString& serial, const QString& cmd) const;
    // PTY 形态（交互终端：远端分配伪终端，输出带回显与控制序列）
    mw::Result<ShellSession> shellPtySession(const QString& serial, const QString& cmd) const;
    // v1 交互 shell（裸字节流，无帧协议；终端页用：独占连接移交调用方）
    mw::Result<TcpConn> shellV1Session(const QString& serial, const QString& cmd) const;
    // 启动长驻 shell 进程，返回持有连接的流
    mw::Result<ShellStream> shellStream(const QString& serial, const QString& cmd) const;

    // 推送内存数据到设备（sync SEND）
    mw::Result<bool> pushBytes(const QString& serial, const QByteArray& data,
                               const QString& remotePath) const;
    // 推送本地文件到设备
    mw::Result<bool> pushFile(const QString& serial, const QString& local,
                              const QString& remotePath) const;
    // 推送本地文件或目录到设备目录（目录递归重建，返回全部推送的远端路径）
    mw::Result<QVector<QString>> pushPath(const QString& serial, const QString& local,
                                          const QString& remoteDir) const;
    // 推送本地文件到设备（流式 + 进度回调，报告已传/总字节）
    mw::Result<bool> pushFileProgress(const QString& serial, const QString& local,
                                      const QString& remotePath,
                                      const std::function<void(quint64, quint64)>& onProgress) const;
    // 从设备拉取文件（sync STAT + RECV），返回全部内容
    mw::Result<QByteArray> pullFile(const QString& serial, const QString& remotePath) const;
    // 查询包的 base APK 在设备上的路径（`pm path` 首行）
    mw::Result<QString> packageApkPath(const QString& serial, const QString& package) const;
    // 安装 APK：push 到临时路径 + `pm install -r`（带进度回调）
    mw::Result<QString> installApkWithProgress(
        const QString& serial, const QString& apk, int timeoutMs,
        const std::function<void(const InstallProgress&)>& onProgress) const;
    // 安装 APK：push 到临时路径 + `pm install -r`。
    // 返回 pm 的输出文本；失败时文本中含错误原因。
    mw::Result<QString> installApk(const QString& serial, const QString& apkPath,
                                   int timeoutMs) const;
    // 授予应用已申请但未授予的运行时权限；返回成功授予的数量
    mw::Result<int> grantRuntimePermissions(const QString& serial, const QString& package) const;
    // 列出已安装的第三方应用（包名）
    mw::Result<QVector<QString>> listPackages(const QString& serial) const;
    // 通过 monkey 启动应用的 launcher 入口（无需知道 Activity 名）
    mw::Result<bool> launchPackage(const QString& serial, const QString& package) const;

    // 反向连接（transport 级服务 reverse:forward:）：设备 localhost:device_port -> 宿主 tcp:host_port
    mw::Result<bool> reverseTcp(const QString& serial, quint16 devicePort, quint16 hostPort) const;
    // 移除反向连接（transport 级服务）
    mw::Result<bool> removeReverse(const QString& serial, quint16 devicePort) const;
    // 端口转发：宿主 tcp:port -> 设备 localabstract:name。
    // host-serial 形式（显式指定目标设备）：多设备并存（含离线）时裸 host:forward
    // 会以空消息 FAIL。直连模式走本地 accept 池 + adbd 线协议桥接。
    mw::Result<bool> forwardAbstract(const QString& serial, quint16 tcpPort,
                                     const QString& abstractName) const;
    // 端口转发：宿主 tcp:host_port -> 设备 tcp:device_port（127.0.0.1）
    mw::Result<bool> forwardTcp(const QString& serial, quint16 hostPort, quint16 devicePort) const;
    // 删除宿主端口转发（host-serial 形式）
    mw::Result<bool> removeForward(const QString& serial, quint16 tcpPort) const;
    mw::Result<bool> killServer() const;

private:
    // 发现注册表目录（qemu 写入：pid_*.ini，含 port.serial/port.adb）
    static QString discoveryDir();
    static QString findAdbOnPath();
    // serial → adbd TCP 端口：发现注册表优先，serial 规则（console+1）兜底
    std::optional<quint16> resolveAdbdPort(const QString& serial) const;
    // 直连 adbd（等效 server 模式的 transport：连上后直接发设备服务）
    mw::Result<SmartConnection> connectAdbd(quint16 port, int timeoutMs) const;
    mw::Result<SmartConnection> connectServer(int timeoutMs) const;
    mw::Result<SmartConnection> transport(const QString& serial) const;
    // 建立一条已协商好设备服务的连接（上层统一入口）。
    // 直连模式：ADB 线协议 OPEN + 本地 TCP 桥接（对调用方透明）；
    // server 模式：host:transport + 服务请求。直连解析失败自动回退 server。
    mw::Result<SmartConnection> deviceConn(const QString& serial, const QString& service) const;
    mw::Result<ShellSession> shellSessionWith(const QString& serial, const QString& service) const;
};

} // namespace mw::adb
