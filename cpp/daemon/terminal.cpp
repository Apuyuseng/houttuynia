#include "terminal.hpp"

#include <QDateTime>

#include "client.hpp" // adb::AdbClient
#include "log.hpp"
#include "shell.hpp" // adb::stdinFrame

namespace mw::term {

// ===== OutBuf =====

void OutBuf::push(const QByteArray& bytes)
{
    data.append(bytes);
    written += static_cast<quint64>(bytes.size());
    if (data.size() > kMaxBuffer) {
        const int dropN = data.size() - kMaxBuffer;
        data.remove(0, dropN);
        trimmed += static_cast<quint64>(dropN);
    }
}

int utf8SafeEnd(const QByteArray& buf)
{
    const int n = buf.size();
    const int backMax = qMin(n, 3);
    for (int back = 1; back <= backMax; ++back) {
        const quint8 b = static_cast<quint8>(buf.constData()[n - back]);
        if ((b & 0xC0) == 0x80)
            continue; // continuation 字节，继续找前导
        int need;
        if (b <= 0x7F)
            need = 1;
        else if (b >= 0xC2 && b <= 0xDF)
            need = 2;
        else if (b >= 0xE0 && b <= 0xEF)
            need = 3;
        else if (b >= 0xF0 && b <= 0xF4)
            need = 4;
        else
            return n; // 非法前导：原样交给 lossy
        return need > back ? n - back : n;
    }
    return n; // 末尾全是 continuation（罕见）
}

QPair<quint64, QByteArray> OutBuf::readSince(quint64 since) const
{
    if (since >= written)
        return qMakePair(written, QByteArray());
    const quint64 startIdx = qMax<quint64>(since, trimmed) - trimmed;
    const char* base = data.constData() + startIdx;
    const int avail = data.size() - static_cast<int>(startIdx);
    const int end = utf8SafeEnd(QByteArray::fromRawData(base, avail));
    // cursor 必须含 startIdx：Rust 原版为 start + utf8_safe_end(...)。
    // 丢失 startIdx 会使 cursor 倒退 → 客户端从旧位置重读 → 同一数据块
    // 永远重投（表现为终端无限刷同一输出）。
    return qMakePair(trimmed + startIdx + static_cast<quint64>(end), QByteArray(base, end));
}

// ===== TermSession =====

TermSession::~TermSession()
{
    kill();
}

void TermSession::readLoop(adb::TcpConn reader, std::shared_ptr<TermSession> self)
{
    Q_UNUSED(self) // 持有引用保证对象存活；循环退出后随 lambda 释放
    forever {
        const auto chunk = reader.readSome(3600 * 1000); // 长阻塞读
        if (!chunk.ok() || chunk.value().isEmpty())
            break; // EOF / 出错
        QMutexLocker l(&self->m_mu);
        self->m_out.push(chunk.value());
    }
    {
        QMutexLocker l(&self->m_mu);
        self->m_out.push(QByteArray("\n[会话已结束]\n"));
    }
    self->alive.store(false);
    {
        QMutexLocker l(&self->m_mu);
        self->m_finishedAt = QDateTime::currentMSecsSinceEpoch();
    }
}

mw::Result<std::shared_ptr<TermSession>> TermSession::start(adb::AdbClient* adb,
                                                            const QString& serial, quint64 id)
{
    auto session = adb->shellV1Session(serial, QString());
    if (!session)
        return mw::err<std::shared_ptr<TermSession>>(
            QStringLiteral("打开 shell 失败: %1").arg(session.error()));

    auto s = std::shared_ptr<TermSession>(new TermSession());
    s->id = id;
    s->serial = serial;
    s->m_stdin = session.value().dup(); // 写句柄（shutdown 语义用）
    s->m_lastUsed.start();
    std::thread(&TermSession::readLoop, std::move(session.value()), s).detach();
    // 注意：不能用「发送 stty rows/cols」方式设置终端尺寸——它作为 shell 输入会
    // 进入命令历史，用户按 ↑ 即调出反复执行（表现为「回车后 stty 无限循环」）；
    // v1 shell 服务无带外 TIOCSWINSZ 通道，接受默认尺寸。
    return mw::Result<std::shared_ptr<TermSession>>::Ok(s);
}

QPair<quint64, QByteArray> TermSession::readSince(quint64 since)
{
    QMutexLocker l(&m_mu);
    m_lastUsed.restart();
    return m_out.readSince(since);
}

mw::Result<bool> TermSession::write(const QByteArray& data)
{
    if (!alive.load())
        return mw::Result<bool>::Err(QStringLiteral("会话已结束"));
    m_lastUsed.restart();
    // v1 裸流会话：原样写入（v2 分帧的帧字节会被 PTY 回显成 ^@ 乱码，
    // \x03 天然就是 Ctrl-C；曾沿袭 Rust 版误用 stdinFrame 的缺陷）
    const auto r = m_stdin.writeAll(data);
    if (!r)
        return mw::Result<bool>::Err(r.error());
    return mw::Result<bool>::Ok();
}

void TermSession::kill()
{
    m_stdin.shutdownBoth();
}

qint64 TermSession::lastUsedMs() const
{
    return m_lastUsed.elapsed();
}

std::optional<qint64> TermSession::finishedAtMs() const
{
    QMutexLocker l(&m_mu);
    return m_finishedAt;
}

// ===== TermManager =====

mw::Result<std::shared_ptr<TermSession>> TermManager::open(adb::AdbClient* adb,
                                                           const QString& serial)
{
    gc();
    const quint64 id = m_nextId.fetch_add(1) + 1;
    auto s = TermSession::start(adb, serial, id);
    if (!s)
        return s;
    {
        QMutexLocker l(&m_mu);
        while (m_sessions.size() >= kMaxSessions) {
            // 踢掉最久未用的
            quint64 victim = 0;
            qint64 best = -1;
            for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
                const qint64 used = it.value()->lastUsedMs();
                if (best < 0 || used > best) {
                    best = used;
                    victim = it.key();
                }
            }
            if (auto v = m_sessions.take(victim))
                v->kill();
            else
                break;
        }
        m_sessions.insert(id, s.value());
    }
    MW_LOG("term", QStringLiteral("会话 #%1 已建立（%2）").arg(id).arg(serial));
    return s;
}

std::shared_ptr<TermSession> TermManager::get(quint64 id)
{
    QMutexLocker l(&m_mu);
    return m_sessions.value(id);
}

bool TermManager::close(quint64 id)
{
    QMutexLocker l(&m_mu);
    auto s = m_sessions.take(id);
    if (!s)
        return false;
    s->kill();
    return true;
}

void TermManager::gc()
{
    QMutexLocker l(&m_mu);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        auto* s = it.value().get();
        const bool deadExpired = s->finishedAtMs().has_value() && now - *s->finishedAtMs() > kDeadKeepMs;
        const bool idleExpired = s->lastUsedMs() > kIdleTimeoutMs;
        if (deadExpired || idleExpired) {
            s->kill();
            MW_LOG("term", QStringLiteral("会话 #%1 已回收").arg(s->id));
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace mw::term
