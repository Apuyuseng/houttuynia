// scrcpy 客户端编排：部署 server -> 端口转发 -> 启动 -> 连接三 socket。
// ← crates/mwine-scrcpy/src/client.rs
//
// 播放器（原 Rust player.rs 的 winit/wgpu 管线）已由 cpp/modules/window 的
// mwine-window（Qt）重建：解码帧以 CVPixelBuffer 形态经 decoder.hpp 的
// DecodedFrame/drain 交出，VideoStream/AudioStream 经 split() 拆给多线程消费。
#pragma once

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>

#include "adb.hpp"
#include "audio.hpp"
#include "control.hpp"
#include "device_msg.hpp"
#include "io.hpp"
#include "options.hpp"
#include "result.hpp"
#include "video.hpp"

namespace mw::scrcpy {

/// 控制通道发送端。
class ControlChannel {
public:
    ControlChannel() = default;
    /// 用已建立的 TCP 流包装控制通道（测试/复用外部会话）。
    explicit ControlChannel(mw::adb::TcpConn conn) : m_conn(std::move(conn)) {}

    ControlChannel(ControlChannel&&) = default;
    ControlChannel& operator=(ControlChannel&&) = default;

    mw::Result<bool> send(const ControlMessage& msg);

    /// 阻塞读取一条设备消息（剪贴板同步等）。流结束返回空。
    mw::Result<std::optional<DeviceMessage>> readDeviceMessageBlocking();

    /// 非阻塞读取一条设备消息。无数据到达时返回空。
    mw::Result<std::optional<DeviceMessage>> tryReadDeviceMessage();

    /// 克隆底层流（供独立的设备消息读取线程使用；TcpConn 双工可分线程读写）。
    mw::adb::TcpConn tryCloneStream() const { return m_conn.dup(); }

    bool isValid() const { return m_conn.isValid(); }

private:
    mw::adb::TcpConn m_conn;
};

/// 会话守卫：保持 scrcpy server 进程存活，析构时终止并清理转发。
/// 清理状态放 shared_ptr 控制块——被移动/复制语义下源对象自然失活，
/// 对应 Rust 侧 Session::split 的 ManuallyDrop 接管约定。
class SessionGuard {
public:
    SessionGuard() = default;
    ~SessionGuard();

    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;
    SessionGuard(SessionGuard&&) = default;
    SessionGuard& operator=(SessionGuard&&) = default;

    /// 终止 server 进程、移除端口转发、清理远端 jar（幂等）。
    void stop();

    bool valid() const { return m_state != nullptr; }

private:
    friend class Session;
    struct State {
        mw::adb::AdbClient adb;
        QString serial;
        quint16 forwardPort = 0;
        QString remoteJar;
        std::optional<mw::adb::ShellStream> serverProcess;
        bool stopped = false;
    };
    explicit SessionGuard(std::shared_ptr<State> s) : m_state(std::move(s)) {}

    std::shared_ptr<State> m_state;
};

/// 一次 scrcpy 会话。析构时终止远端 server 并清理端口转发。
class Session {
public:
    Session() = default;
    Session(Session&&) = default;
    Session& operator=(Session&&) = default;
    ~Session() { stop(); }

    mw::adb::AdbClient adb;
    QString serial;
    QString deviceName;
    VideoStream video;
    /// 音频流（ServerOptions.audio 开启时）
    std::optional<AudioStream> audio;
    std::optional<ControlChannel> control;
    quint16 forwardPort = 0;

    /// 显式停止会话（幂等；split 后的搬空会话为 no-op）。
    void stop();
    /// server 进程是否仍被本会话持有。
    bool serverAlive() const { return m_serverProcess.has_value(); }
    /// 本会话的远端 jar 路径（stop 时清理）。
    QString remoteJar() const { return m_remoteJar; }

    /// 拆分为四部分，供多线程播放器使用：
    /// - VideoStream：视频 socket（通常移入独立读线程）
    /// - ControlChannel：控制通道
    /// - AudioStream：音频 socket（未启用为空）
    /// - SessionGuard：持有 server 进程存活；析构时自动清理
    struct Parts {
        VideoStream video;
        std::optional<ControlChannel> control;
        std::optional<AudioStream> audio;
        SessionGuard guard;
    };
    Parts split();

private:
    friend class ScrcpyClient;
    void cleanupJar();

    std::optional<mw::adb::ShellStream> m_serverProcess;
    QString m_remoteJar;
    bool m_split = false;
};

struct ScrcpyClient {
    mw::adb::AdbClient adb;
    QString serial;
    QString serverJar;
    ServerOptions options;

    ScrcpyClient() = default;
    ScrcpyClient(mw::adb::AdbClient adbClient, QString serial_, QString serverJar_)
        : adb(std::move(adbClient)), serial(std::move(serial_)), serverJar(std::move(serverJar_))
    {
    }

    /// 启动完整会话。
    mw::Result<Session> start() &&;
};

} // namespace mw::scrcpy
