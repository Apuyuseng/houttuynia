// 启动器：环境组装 + spawn ← crates/mwine-qemu/src/launcher.rs
//
// spawn 用 fork + execve（而非 QProcess）：Rust 语义要求句柄 Drop 不杀进程，
// 且子进程需继承完整定制环境（DYLD_LIBRARY_PATH 等五个变量）与日志重定向。
#include "launcher.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStringList>
#include <QVector>

#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "ports.hpp"

namespace mw::qemu {

namespace {

// fd 守卫（fork 失败/成功后统一在父进程侧回收）
struct FdGuard {
    int fd = -1;
    ~FdGuard()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

} // namespace

DirectLauncher::DirectLauncher(QemuRuntime rt) : runtime(std::move(rt)) {}

mw::Result<DirectLauncher> DirectLauncher::detect()
{
    auto rt = QemuRuntime::detectFromSdk();
    if (!rt.has_value())
        return mw::err<DirectLauncher>(errBinaryNotFound(QStringLiteral("(SDK 未找到 qemu)")));
    return mw::Result<DirectLauncher>::Ok(DirectLauncher(*rt));
}

mw::Result<QemuHandle> DirectLauncher::launch(const QemuConfig& config) const
{
    if (!config.imageDir.has_value() && config.avdName.isEmpty())
        return mw::err<QemuHandle>(errNoTarget()); // Error::NoTarget
    if (auto v = runtime.validate(); !v)
        return mw::err<QemuHandle>(v.error());
    if (auto v = config.validateImageDir(); !v)
        return mw::err<QemuHandle>(v.error());

    quint16 consolePort = 0;
    quint16 adbPort = 0;
    if (config.ports.has_value()) {
        consolePort = config.ports->first;
        adbPort = config.ports->second;
    } else if (auto p = pickPortPair(5554)) {
        consolePort = p->first;
        adbPort = p->second;
    } else {
        return mw::err<QemuHandle>(errPortExhausted());
    }
    quint16 grpcPort = 0;
    if (config.grpcPort.has_value()) {
        grpcPort = *config.grpcPort;
    } else if (auto p = pickPort(8554)) {
        grpcPort = *p;
    } else {
        return mw::err<QemuHandle>(errPortExhausted());
    }

    const QStringList args = config.buildArgs(consolePort, adbPort, grpcPort);

    // 环境集是直启的命门（缺失对应症状见 Rust lib 文档）
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("DYLD_LIBRARY_PATH"), runtime.dyldLibraryPath());
    env.insert(QStringLiteral("ANDROID_EMULATOR_LAUNCHER_DIR"), runtime.launcherDir);
    env.insert(QStringLiteral("ANDROID_EMULATOR_WRAPPER_PID"), QString::number(quint64(::getpid())));
    if (runtime.sdkRoot.has_value())
        env.insert(QStringLiteral("ANDROID_SDK_ROOT"), *runtime.sdkRoot);
    if (runtime.avdHome.has_value())
        env.insert(QStringLiteral("ANDROID_AVD_HOME"), *runtime.avdHome);

    // —— fork 前备好全部字节缓冲（fork 后仅做 async-signal-safe 调用）——
    QVector<QByteArray> argvBytes;
    argvBytes.reserve(args.size() + 1);
    argvBytes << QFile::encodeName(runtime.qemuBin);
    for (const QString& a : args)
        argvBytes << QFile::encodeName(a);
    const QStringList envList = env.toStringList();
    QVector<QByteArray> envBytes;
    envBytes.reserve(envList.size());
    for (const QString& e : envList)
        envBytes << e.toUtf8();
    std::vector<char*> argvp;
    std::vector<char*> envpp;
    argvp.reserve(argvBytes.size() + 1);
    for (QByteArray& b : argvBytes)
        argvp.push_back(b.data());
    argvp.push_back(nullptr);
    envpp.reserve(envBytes.size() + 1);
    for (QByteArray& b : envBytes)
        envpp.push_back(b.data());
    envpp.push_back(nullptr);

    FdGuard devnull{ ::open("/dev/null", O_RDWR) };
    if (devnull.fd < 0)
        return mw::err<QemuHandle>(QStringLiteral("打开 /dev/null 失败: %1")
                                       .arg(QString::fromLatin1(::strerror(errno))));
    int outFd = devnull.fd;
    int logFd = -1; // 父进程侧需显式关闭的日志 fd
    if (logFile.has_value()) {
        const QDir parent(QFileInfo(*logFile).absolutePath());
        if (!parent.mkpath(QStringLiteral("."))) { /* 尽力而为，open 自会失败 */ }
        // Rust File::create → 截断写
        logFd = ::open(QFile::encodeName(*logFile).constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logFd < 0)
            return mw::err<QemuHandle>(QStringLiteral("日志文件创建失败: %1: %2")
                                           .arg(*logFile, QString::fromLatin1(::strerror(errno))));
        outFd = logFd;
    }

    const long maxFd = ::sysconf(_SC_OPEN_MAX);
    const pid_t pid = ::fork();
    if (pid < 0) {
        if (logFd >= 0)
            ::close(logFd);
        return mw::err<QemuHandle>(QStringLiteral("fork 失败: %1")
                                       .arg(QString::fromLatin1(::strerror(errno))));
    }
    if (pid == 0) {
        // 子进程：重定向 + 关闭其余继承 fd + exec（对齐 std::process::Child 默认行为）
        ::dup2(devnull.fd, STDIN_FILENO);
        ::dup2(outFd, STDOUT_FILENO);
        ::dup2(outFd, STDERR_FILENO);
        for (long fd = 3; fd < maxFd; ++fd)
            ::close(fd);
        ::execve(argvp[0], argvp.data(), envpp.data());
        ::_exit(127);
    }
    if (logFd >= 0)
        ::close(logFd);

    // Error::LaunchFailed(..) 的另一形态：spawn 成功即返回句柄（进程死活由 waitAdb 探测）
    return mw::Result<QemuHandle>::Ok(QemuHandle::fromChild(quint32(pid), consolePort, grpcPort));
}

} // namespace mw::qemu
