// 交互式 ADB 终端会话（terminal.rs 忠实移植）：
// 每会话 = 设备侧 PTY shell（v1 裸流），读线程汇入环形缓冲，REST 轮询增量拉取。
// 生命周期：管理表与读者线程各持 shared_ptr，双方放手后对象在读者线程自毁。
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QString>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>

#include "result.hpp"
#include "tcpconn.hpp"

namespace mw::adb { class AdbClient; }

namespace mw::term {

constexpr int kMaxBuffer = 512 * 1024; // ~50 万字符回滚
constexpr int kMaxSessions = 8;
constexpr qint64 kIdleTimeoutMs = 3600 * 1000;
constexpr qint64 kDeadKeepMs = 300 * 1000;

// 输出环形缓冲：cursor = 绝对写入字节数，丢弃头部时 trimmed 前移
struct OutBuf {
    QByteArray data;
    quint64 written = 0;
    quint64 trimmed = 0;

    void push(const QByteArray& bytes);
    // since 之后的新数据，尾部截到 UTF-8 安全边界；返回 (新cursor, 数据)
    QPair<quint64, QByteArray> readSince(quint64 since) const;
};

// 尾部不完整多字节序列检测（细节照抄 Rust utf8_safe_end）
int utf8SafeEnd(const QByteArray& buf);

class TermSession {
public:
    ~TermSession();

    quint64 id = 0;
    QString serial;
    bool pty = true;
    std::atomic<bool> alive{true};

    QPair<quint64, QByteArray> readSince(quint64 since); // 附带续期 lastUsed
    mw::Result<bool> write(const QByteArray& data);      // \n 执行；\x03 = Ctrl-C
    void kill();                                         // 关闭写句柄 → 远端 shell 终止

    qint64 lastUsedMs() const;
    std::optional<qint64> finishedAtMs() const;

    // 建立会话（设备上打开 v1 shell；读流 + dup 写句柄 + 读者线程）
    static mw::Result<std::shared_ptr<TermSession>> start(adb::AdbClient* adb,
                                                          const QString& serial, quint64 id);

private:
    TermSession() = default;
    static void readLoop(adb::TcpConn reader, std::shared_ptr<TermSession> self);

    mutable QMutex m_mu;
    OutBuf m_out;
    adb::TcpConn m_stdin; // dup 出的写句柄
    QElapsedTimer m_lastUsed;
    std::optional<qint64> m_finishedAt; // epoch ms
};

class TermManager {
public:
    mw::Result<std::shared_ptr<TermSession>> open(adb::AdbClient* adb, const QString& serial);
    std::shared_ptr<TermSession> get(quint64 id);
    bool close(quint64 id);
    void gc();

private:
    std::atomic<quint64> m_nextId{0};
    QMutex m_mu;
    QHash<quint64, std::shared_ptr<TermSession>> m_sessions;
};

} // namespace mw::term
