// shell 服务：v2 协议（stdout/stderr/exit 分离）与 v1 回退。
// ← crates/mwine-adb/src/shell.rs
//
// 协议细节（严格对照 Rust 实现）：
// - ShellV2Reader（collect 路径）：`[id: u8][len: u32 小端][data]`，
//   id 0x1=stdout, 0x2=stderr, 0x3=exit(1 字节)
// - ShellV2Stream（交互路径）：长度字段为 大端（保留 Rust 实现原样）
// - stdin 写入帧：id=0 + 4 字节大端长度 + 数据
#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

#include "result.hpp"
#include "tcpconn.hpp"

namespace mw::adb {

struct ShellOutput {
    QByteArray out;                 // stdout
    QByteArray err;                 // stderr
    std::optional<int> exitCode;    // v2 协议下有值；v1 回退时为空

    QString outStr() const { return QString::fromUtf8(out); }
    QString errStr() const { return QString::fromUtf8(err); }
    bool ok() const { return !exitCode.has_value() || *exitCode == 0; }
};

enum class ShellChunk { Stdout, Stderr, Exit };

// 单个 v2 chunk（对应 Rust Option<(ShellChunk, Vec<u8>)> 的载荷部分）
struct ShellV2Chunk {
    ShellChunk kind = ShellChunk::Stdout;
    QByteArray data;
};

// shell v2 流读取器：消费到结束返回完整输出（Rust ShellV2Reader）
class ShellV2Reader {
public:
    ShellV2Reader(TcpConn& conn, int timeoutMs) : m_conn(conn), m_timeoutMs(timeoutMs) {}

    // 读取下一个 chunk：Ok(有值)=得到数据；Ok(空)=流结束（EOF）；Err=帧错误
    mw::Result<std::optional<ShellV2Chunk>> nextChunk();
    // 消费到结束，返回完整输出
    mw::Result<ShellOutput> collect();

private:
    TcpConn& m_conn; // 生命周期由调用方保证（与 Rust 借用一致）
    int m_timeoutMs;
};

// v1：读到 EOF，stdout/stderr 混流
mw::Result<QByteArray> readV1ToEnd(TcpConn& conn, int timeoutMs);

// shell v2 协议读流：只吐出 stdout（id=1）载荷，stderr/exit 包静默跳过
//（交互终端与长连接采集共用）
class ShellV2Stream {
public:
    ShellV2Stream() = default;
    explicit ShellV2Stream(TcpConn conn, int timeoutMs = -1)
        : m_conn(std::move(conn)), m_timeoutMs(timeoutMs)
    {
    }
    ShellV2Stream(const ShellV2Stream&) = delete;
    ShellV2Stream& operator=(const ShellV2Stream&) = delete;
    ShellV2Stream(ShellV2Stream&&) = default;
    ShellV2Stream& operator=(ShellV2Stream&&) = default;

    // 读 stdout：阻塞至 ≥1 字节；帧错误/流结束 → Err（调用方按 EOF 处理）
    mw::Result<QByteArray> read(int maxBytes);

private:
    mw::Result<QByteArray> nextStdoutPacket();

    TcpConn m_conn;
    int m_timeoutMs = -1;
    QByteArray m_pending; // 上一个 stdout 包未消费完的字节
};

// 组装 shell v2 服务名。禁用 PTY 相关参数，走 raw 模式。
inline QString v2Service(const QString& cmd)
{
    return QStringLiteral("shell,v2,raw:%1").arg(cmd);
}
// PTY 形态的 v2 shell 服务（交互终端用：远端分配伪终端，输出带回显）
inline QString v2PtyService(const QString& cmd)
{
    return QStringLiteral("shell,v2,pty:%1").arg(cmd);
}
inline QString v1Service(const QString& cmd)
{
    return QStringLiteral("shell:%1").arg(cmd);
}

// 构造远端 stdin 写入帧（shell v2：id=0 + 4 字节大端长度 + 数据）
inline QByteArray stdinFrame(const QByteArray& data)
{
    QByteArray frame;
    frame.reserve(data.size() + 5);
    frame.append(char(0));
    char len[4];
    const quint32 n = quint32(data.size());
    len[0] = char((n >> 24) & 0xFF);
    len[1] = char((n >> 16) & 0xFF);
    len[2] = char((n >> 8) & 0xFF);
    len[3] = char(n & 0xFF);
    frame.append(len, 4);
    frame.append(data);
    return frame;
}

} // namespace mw::adb
