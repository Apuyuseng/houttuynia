// Flow 重组 ← crates/mwine-capture/src/flow.rs
// 把按连接分组的明文 IO 事件重组为可查询的 HTTP 事务。
//
// 键策略（实测 ecapture 的 IO 事件不带五元组、FD 恒为 0）：
// 以 pid 为主键（辅以 comm），连接元数据行登记五元组，全零 Closed 行定稿。
//
// 解析能力：
// - HTTP/1.x：多消息解析（keep-alive/管道化按 Content-Length 切分；
//   半路接入时跳过残片定位后续完整消息）；
// - HTTP/2：无需连接前言即可识别（半路接入），帧游走 + 跨块持久 HPACK
//   解码器（动态表状态正确），DATA 帧按流重组为请求/响应体；
// - gzip/deflate 响应体自动解压；
// - 解析不出时保留原始字节预览（详情页兜底）。
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QString>
#include <QVector>
#include <map>
#include <optional>

#include "events.hpp"

namespace mw::capture {

enum class FlowKind {
    Http1,
    Http2,
    Encrypted, // 密文连接（eBPF 追踪到但无法解密：Cronet/X5/Flutter 等）
    Plaintext, // 明文但不是 HTTP（私有协议/半路接入残片/WebSocket 帧）
    Unknown,
};

const char* flowKindStr(FlowKind k);

// 文本体视图（lossy 文本 + 尺寸）。JSON: {"text": ..., "size": ...}
struct BodyView {
    QString text;
    qsizetype size = 0;

    static std::optional<BodyView> from(const QByteArray& b);
    QJsonObject toJson() const;
};

// 一次请求/响应交互（H1 按序配对；H2 按流）。存储与 REST 的呈现形态。
struct ExchangeView {
    quint32 streamId = 0; // HTTP/2 流 ID；HTTP/1.x 恒 0
    QString method;
    QString host;
    QString path;
    quint16 status = 0;
    QString reqHead;
    std::optional<BodyView> reqBody;
    std::optional<BodyView> reqBodyDecoded; // gzip/deflate 解压后的请求体
    QString respHead;
    std::optional<BodyView> respBody;
    std::optional<BodyView> respBodyDecoded;
    QString respEncoding; // 响应 Content-Encoding（br 暂不解压，仅标注）

    QJsonObject toJson() const;
};

// 一条 HTTP（或近似 HTTP）消息的解析结果。
struct HttpMessage {
    QString startLine;                     // 起始行（请求行/状态行）
    QVector<QPair<QString, QString>> headers; // 顺序保留，重复头各占一条
    QByteArray body;                       // 起始行+头部之后的正文
    qsizetype bodyOffset = 0;
    bool truncated = false;                // 头部未完整收到（无终止空行）
    QString encoding;                      // Content-Encoding 原值（小写；空=identity）
    std::optional<QByteArray> bodyDecoded; // gzip/deflate 成功解压后的正文

    std::optional<QString> header(const QString& name) const;
    // 常用摘要字段（H1 是 Host，H2 是 :authority）
    std::optional<QString> host() const;
};

// 一条 flow（一次 TCP/TLS 连接上的完整交互）。
struct Flow {
    qint64 id = 0;
    qint64 firstTsMs = 0;
    qint64 lastTsMs = 0;
    quint32 pid = 0;
    QString comm;
    QString package; // 安装包名（编排层富化；映射不到时为空）
    QString src;
    QString dst;
    FlowKind kind = FlowKind::Unknown;
    std::optional<HttpMessage> request;  // 首个交互的消息形态（摘要/存储兼容）
    std::optional<HttpMessage> response;
    QVector<ExchangeView> exchanges;     // 全部解析出的交互（多请求连接/多流 H2）
    quint64 wbytes = 0;
    quint64 rbytes = 0;
    QString rawPreview; // 原始字节预览（解析失败兜底）
    QByteArray rawWrite; // 原始双向字节（内存保留；超限截断）
    QByteArray rawRead;
};

// 列表用摘要（小对象，REST 返回体）。JSON 字段与 Rust serde 完全一致：
// id/first_ts_ms/last_ts_ms/pid/comm/package/src/dst/kind/host/method/path/
// status/wbytes/rbytes/content_type/exchanges
struct FlowSummary {
    qint64 id = 0;
    qint64 firstTsMs = 0;
    qint64 lastTsMs = 0;
    quint32 pid = 0;
    QString comm;
    QString package;
    QString src;
    QString dst;
    QString kind;
    QString host;
    QString method;
    QString path;
    quint16 status = 0;
    quint64 wbytes = 0;
    quint64 rbytes = 0;
    QString contentType;
    int exchanges = 0; // 解析出的交互条数（keep-alive/H2 多流）

    static FlowSummary from(const Flow& f);
    QJsonObject toJson() const;
};

// 流重组器：喂入事件，产出定稿 flow。
class Reassembler {
public:
    Reassembler();

    // 更新 pid→package 富化映射。
    void setPidPackages(const QMap<quint32, QString>& map);

    // 喂事件；返回本批定稿（连接关闭）的 flow 列表。
    QVector<Flow> feed(const Event& ev);

    // 冲刷：把所有 live flow 定稿（会话停止时调用）。
    QVector<Flow> flush();

    // 定稿空闲超时的 flow（关闭行不带 pid 的并发场景靠空闲兜底）。
    QVector<Flow> flushIdle(qint64 nowMs, qint64 maxIdleMs);

    // live flow 计数（状态轮询用，零拷贝零解析）。
    qsizetype liveCount() const;

    // 连接总数（自会话开始累计）。
    quint64 connectsTotal() const;

    // 爆增防御：live flow 超上限时全部定稿（内存交还落库管道）。
    QVector<Flow> capLive(qsizetype maxLive);

    // 当前 live flow 快照（UI 实时列表用；不篡改状态）。
    QVector<Flow> liveFlows() const;

private:
    Flow blank(quint32 pid, const QString& comm, qint64 tsMs);

    std::map<quint32, Flow> live_;
    qint64 nextId_ = 0;
    QMap<quint32, QString> pidPackage_;
    qsizetype rawCap_ = 512 * 1024;
    quint64 connectsTotal_ = 0;
};

// 全量解析（按需调用：详情打开时；结果由存储层缓存）。
QVector<ExchangeView> computeExchanges(const Flow& f);

} // namespace mw::capture
