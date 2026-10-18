// ecapture 文本事件流解析器 ← crates/mwine-capture/src/events.rs
//
// 实测格式（ecapture tls 模块 text 输出）：
//   PID:2678, Comm:main, TID:2678, FD:42, Tuple: [::ffff:10.0.2.15]:42418->[::ffff:183.2.172.177]:443
//   [2026-10-07 15:36:32.210] PID:2678 TID:2678 Comm:main FD:0 WRITE (103 bytes):
//   <正好 103 字节原始载荷（可含任意二进制/换行）>
//   PID:0, Comm:, TID:0, FD:0, Tuple: [::]:0->[::]:0        ← 全零行=连接关闭/分隔
//
// 分帧采用长度分帧（IO 事件头声明载荷字节数），载荷里即使出现 `PID:` 或
// 时间戳样式的字节序列也不会误同步。未完成的记录留在内部缓冲等待后续数据，
// 因此可直接接在进程 stdout 的流式输出后面。
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <optional>

namespace mw::capture {

// IO 方向：Write=应用发出（请求方向），Read=应用收到（响应方向）。
enum class Direction {
    Write,
    Read,
};

inline const char* directionStr(Direction d)
{
    return d == Direction::Write ? "WRITE" : "READ";
}

// 一条解析出的 ecapture 事件（对应 Rust 的 Event 枚举）。
struct Event {
    enum class Type {
        Connect, // 连接建立（元数据行，字段非全零）
        Closed,  // 全零元数据行：连接关闭/批次分隔
        Io,      // 明文 IO 事件（加密前/解密后的应用层数据）
    };

    Type type = Type::Closed;

    // Connect 字段
    quint32 pid = 0;
    QString comm;
    quint32 fd = 0;
    QString src; // 源地址 ip:port（v4 会以 [::ffff:a.b.c.d]:p 形式出现，原样保留）
    QString dst; // 目的地址 ip:port

    // Io 字段
    QString ts; // 原始时间戳字符串（guest 本地时间 YYYY-MM-DD HH:MM:SS.mmm）
    qint64 tsMs = 0;
    quint32 tid = 0;
    Direction direction = Direction::Write;
    QByteArray data;

    static Event makeConnect(quint32 pid, const QString& comm, quint32 fd,
                             const QString& src, const QString& dst)
    {
        Event e;
        e.type = Type::Connect;
        e.pid = pid;
        e.comm = comm;
        e.fd = fd;
        e.src = src;
        e.dst = dst;
        return e;
    }
    static Event makeClosed()
    {
        Event e;
        e.type = Type::Closed;
        return e;
    }
    static Event makeIo(const QString& ts, qint64 tsMs, quint32 pid, quint32 tid,
                        const QString& comm, Direction dir, const QByteArray& data)
    {
        Event e;
        e.type = Type::Io;
        e.ts = ts;
        e.tsMs = tsMs;
        e.pid = pid;
        e.tid = tid;
        e.comm = comm;
        e.direction = dir;
        e.data = data;
        return e;
    }
};

// `2026-10-07 15:36:32.210` → Unix 毫秒（按 UTC 解析；days-from-civil 手写换算）。
std::optional<qint64> parseTsMs(const QString& ts);

// 增量解析器：feed() 喂入任意切块的字节流，drain() 取出已完成的事件。
// 解析失败的行跳过（尽力而为，绝不抛出）。
class EventParser {
public:
    EventParser() = default;

    // 喂入新字节。返回是否消费了数据。
    bool feed(const QByteArray& chunk);

    // 取出当前所有已完成的事件。
    QVector<Event> drain();

    // 当前缓冲中的未完成字节数（诊断用）。
    qsizetype buffered() const;

private:
    struct PendingIo {
        QString ts;
        qint64 tsMs = 0;
        quint32 pid = 0;
        quint32 tid = 0;
        QString comm;
        Direction direction = Direction::Write;
        qsizetype declared = 0; // 头部声明的总字节数
        QByteArray data;
    };

    QByteArray buf_;
    std::optional<PendingIo> pending_;

    static std::optional<PendingIo> parseIoHeader(const QString& s);
};

} // namespace mw::capture
