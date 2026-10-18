// ChildProcess 实现：posix_spawn + 管道
#include "childproc.hpp"

#include "audio.hpp" // errIo（错误文案构造）

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

extern char** environ;

namespace mw::audio {

namespace {

// SIGPIPE 忽略只做一次（与 Rust 运行时默认一致：写断管返回 EPIPE）
void ignoreSigpipeOnce()
{
    static std::once_flag flag;
    std::call_once(flag, [] { ::signal(SIGPIPE, SIG_IGN); });
}

} // namespace

ChildProcess::~ChildProcess()
{
    killAndReap();
    closeFds();
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : m_pid(other.m_pid), m_stdinFd(other.m_stdinFd), m_stdoutFd(other.m_stdoutFd),
      m_reaped(other.m_reaped)
{
    other.m_pid = -1;
    other.m_stdinFd = -1;
    other.m_stdoutFd = -1;
    other.m_reaped = true;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
{
    if (this != &other) {
        killAndReap();
        closeFds();
        m_pid = other.m_pid;
        m_stdinFd = other.m_stdinFd;
        m_stdoutFd = other.m_stdoutFd;
        m_reaped = other.m_reaped;
        other.m_pid = -1;
        other.m_stdinFd = -1;
        other.m_stdoutFd = -1;
        other.m_reaped = true;
    }
    return *this;
}

mw::Result<ChildProcess> ChildProcess::spawnPiped(const QString& program, const QStringList& args)
{
    ignoreSigpipeOnce();

    int inPipe[2] = { -1, -1 };  // 父写 -> 子读（子 stdin）
    int outPipe[2] = { -1, -1 }; // 子写 -> 父读（子 stdout）
    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
        const QString e = QString::fromLatin1(::strerror(errno));
        if (inPipe[0] >= 0) { ::close(inPipe[0]); ::close(inPipe[1]); }
        if (outPipe[0] >= 0) { ::close(outPipe[0]); ::close(outPipe[1]); }
        return mw::err<ChildProcess>(errIo(QStringLiteral("pipe: %1").arg(e)));
    }
    const int devNull = ::open("/dev/null", O_WRONLY);
    if (devNull < 0) {
        const QString e = QString::fromLatin1(::strerror(errno));
        ::close(inPipe[0]); ::close(inPipe[1]);
        ::close(outPipe[0]); ::close(outPipe[1]);
        return mw::err<ChildProcess>(errIo(QStringLiteral("open /dev/null: %1").arg(e)));
    }

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, devNull, STDERR_FILENO);
    // 子进程侧的原始 fd 关闭（父进程侧保持打开）
    ::posix_spawn_file_actions_addclose(&actions, inPipe[0]);
    ::posix_spawn_file_actions_addclose(&actions, inPipe[1]);
    ::posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    ::posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    ::posix_spawn_file_actions_addclose(&actions, devNull);

    // argv 组装（程序名 + 参数 + nullptr）
    QVector<QByteArray> utf8;
    utf8.append(program.toUtf8());
    for (const QString& a : args)
        utf8.append(a.toUtf8());
    QVector<char*> argv;
    for (QByteArray& b : utf8)
        argv.append(b.data());
    argv.append(nullptr);

    pid_t pid = -1;
    const int st = ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    // 父进程侧：关闭子进程持有的端点
    ::close(inPipe[0]);
    ::close(outPipe[1]);
    ::close(devNull);
    if (st != 0) {
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        return mw::err<ChildProcess>(
            errIo(QStringLiteral("spawn %1: %2").arg(program, QString::fromLatin1(::strerror(st)))));
    }

    ChildProcess child;
    child.m_pid = pid;
    child.m_stdinFd = inPipe[1];
    child.m_stdoutFd = outPipe[0];
    child.m_reaped = false;
    return mw::Result<ChildProcess>::Ok(std::move(child));
}

bool ChildProcess::writeStdin(const QByteArray& data)
{
    if (m_stdinFd < 0 || data.isEmpty())
        return m_stdinFd >= 0;
    const char* p = data.constData();
    size_t remaining = size_t(data.size());
    while (remaining > 0) {
        const ssize_t n = ::write(m_stdinFd, p, remaining);
        if (n > 0) {
            p += n;
            remaining -= size_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false; // EPIPE（对端退出）或其他错误
    }
    return true;
}

void ChildProcess::closeStdin()
{
    if (m_stdinFd >= 0) {
        ::close(m_stdinFd);
        m_stdinFd = -1;
    }
}

qint64 ChildProcess::readStdout(char* buf, int maxBytes)
{
    if (m_stdoutFd < 0 || maxBytes <= 0)
        return m_stdoutFd < 0 ? 0 : -1;
    for (;;) {
        const ssize_t n = ::read(m_stdoutFd, buf, size_t(maxBytes));
        if (n >= 0)
            return qint64(n); // 0 = EOF
        if (errno == EINTR)
            continue;
        return qint64(-1);
    }
}

void ChildProcess::kill()
{
    if (m_pid > 0)
        ::kill(m_pid, SIGKILL);
}

void ChildProcess::wait()
{
    std::lock_guard<std::mutex> lock(m_reapMutex);
    if (m_pid <= 0 || m_reaped)
        return;
    ::waitpid(m_pid, nullptr, 0);
    m_reaped = true;
}

void ChildProcess::killAndReap()
{
    kill();
    wait();
}

void ChildProcess::closeFds()
{
    if (m_stdinFd >= 0) {
        ::close(m_stdinFd);
        m_stdinFd = -1;
    }
    if (m_stdoutFd >= 0) {
        ::close(m_stdoutFd);
        m_stdoutFd = -1;
    }
}

} // namespace mw::audio
