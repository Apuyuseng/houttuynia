// flow 持久化 ← crates/mwine-capture/src/store.rs
// 双表拆分 + 懒解析：
// - flows = 轻表：列表/统计轮询只碰摘要列 + 原始字节（raw_write/raw_read 各≤256KB）；
// - flow_exchanges = 详情缓存表：首次打开详情才做全量解析（computeExchanges），
//   结果缓存于此。采集线程只做廉价首头部扫描，重活按需且只做一次。
#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <optional>

#include "flow.hpp"
#include "result.hpp"

struct sqlite3;

namespace mw::capture {

// 查询过滤器（全部字段可选；对应 Rust store::FlowsQuery，契约默认 limit=200）。
struct FlowsQuery {
    QString q;       // 关键词：host/path/头部/进程/包名/目的地址（轻表 LIKE）
    QString host;    // host LIKE
    QString package; // package =
    QString method;  // method =
    QString kind;    // kind =（http1/http2/encrypted/plaintext/unknown）
    std::optional<int> status;     // status =
    std::optional<int> statusClass; // 2xx/4xx…（status 区间）
    std::optional<qint64> sinceMs; // last_ts_ms >=
    int limit = 200;
    int offset = 0;
};

class Store {
public:
    Store() = default;
    ~Store();
    Store(Store&& other) noexcept;
    Store& operator=(Store&& other) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // 打开（必要时创建父目录）。WAL + busy_timeout 500ms + v2→v3 迁移。
    static mw::Result<Store> open(const QString& path);
    // 内存库（测试用）。
    static mw::Result<Store> openInMemory();

    bool isOpen() const { return db_ != nullptr; }

    // 轻量 upsert：只写摘要 + raw（全量解析推迟到详情）。
    mw::Result<int> upsertFlows(const QVector<Flow>& flows) const;

    // 列表查询：只碰轻表。
    mw::Result<QVector<FlowSummary>> querySummaries(const FlowsQuery& q) const;

    // 筛选条件下的总条数（分页用）。
    mw::Result<quint64> countFiltered(const FlowsQuery& q) const;
    mw::Result<quint64> count() const;

    // 详情（懒解析 + 缓存）。未命中返回空 QJsonObject；JSON 形态与 Rust v2 兼容
    // （含 exchanges 数组与首个交互的 req_body/resp_body）。
    mw::Result<QJsonObject> getDetail(qint64 id) const;

    // 解析质量统计（轻表聚合）：
    // {"kinds": {kind: {count,wbytes,rbytes}}, "total", "parsed", "parse_rate"}
    mw::Result<QJsonObject> kindStats() const;

    mw::Result<bool> clear() const;

private:
    void close();

    sqlite3* db_ = nullptr;
};

} // namespace mw::capture
