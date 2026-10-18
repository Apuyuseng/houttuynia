// 同步子进程（stdin/stdout 管道 + stderr 丢弃），posix_spawn 实现。
// ← Rust std::process::Command + Stdio::piped 的直接对应物
//（QProcess 有线程亲和限制，ffmpeg 解码线程组需要裸管道语义）。
//
// 线程分工约定（与 Rust 侧一致）：
// - stdin 写端由 feed 线程独占使用（writeStdin/closeStdin）
// - stdout 读端由 drain 线程独占使用（readStdout）
// - kill/wait 可从任意线程调用（内部幂等 + 串行化）
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <mutex>
#include <sys/types.h>

#include "result.hpp"

namespace mw::audio {

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    /// 启动子进程：stdin/stdout 接管道，stderr 丢弃（/dev/null）。
    /// SIGPIPE 进程级忽略（与 Rust 运行时一致），写断管得到 EPIPE 而非信号。
    static mw::Result<ChildProcess> spawnPiped(const QString& program, const QStringList& args);

    bool valid() const { return m_pid > 0; }

    /// 全量写 stdin。false = 管道断（解码端退出）或进程无效。
    bool writeStdin(const QByteArray& data);
    /// 关闭 stdin（对端收到 EOF）。
    void closeStdin();

    /// 读 stdout：>0 = 字节数；0 = EOF；<0 = 错误。
    qint64 readStdout(char* buf, int maxBytes);

    /// SIGKILL（幂等；不 wait）。
    void kill();
    /// 收尸（幂等；未 kill 时阻塞至退出）。
    void wait();
    /// kill + 收尸（幂等）。
    void killAndReap();

private:
    void closeFds();

    pid_t m_pid = -1;
    int m_stdinFd = -1;  // feed 线程独占
    int m_stdoutFd = -1; // drain 线程独占
    std::mutex m_reapMutex;
    bool m_reaped = false;
};

} // namespace mw::audio
