// flow 持久化实现 ← crates/mwine-capture/src/store.rs（sqlite3 C API）。
#include "store.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>

#include <sqlite3.h>

namespace mw::capture {

// raw 字节的入库上限（每方向；懒解析用，超出部分丢尾）。
constexpr qsizetype kRawCap = 256 * 1024;
// 详情缓存的正文上限（每消息每方向）。
constexpr qsizetype kBodyCap = 2 * 1024 * 1024;

static const char kSchemaV3[] = R"SQL(
    CREATE TABLE IF NOT EXISTS flows (
        id             INTEGER PRIMARY KEY,
        first_ts_ms    INTEGER NOT NULL DEFAULT 0,
        last_ts_ms     INTEGER NOT NULL DEFAULT 0,
        pid            INTEGER NOT NULL,
        comm           TEXT    NOT NULL DEFAULT '',
        package        TEXT    NOT NULL DEFAULT '',
        src            TEXT    NOT NULL DEFAULT '',
        dst            TEXT    NOT NULL DEFAULT '',
        kind           TEXT    NOT NULL DEFAULT '',
        host           TEXT    NOT NULL DEFAULT '',
        method         TEXT    NOT NULL DEFAULT '',
        path           TEXT    NOT NULL DEFAULT '',
        status         INTEGER NOT NULL DEFAULT 0,
        wbytes         INTEGER NOT NULL DEFAULT 0,
        rbytes         INTEGER NOT NULL DEFAULT 0,
        content_type   TEXT    NOT NULL DEFAULT '',
        req_head       TEXT    NOT NULL DEFAULT '',
        resp_head      TEXT    NOT NULL DEFAULT '',
        raw_preview    TEXT    NOT NULL DEFAULT '',
        raw_write      BLOB,
        raw_read       BLOB,
        parsed         INTEGER NOT NULL DEFAULT 0,
        exchange_count INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX IF NOT EXISTS idx_flows_ts ON flows(first_ts_ms DESC);
    CREATE INDEX IF NOT EXISTS idx_flows_host ON flows(host);
    CREATE INDEX IF NOT EXISTS idx_flows_package ON flows(package);
    CREATE INDEX IF NOT EXISTS idx_flows_kind ON flows(kind);
    CREATE INDEX IF NOT EXISTS idx_flows_method ON flows(method);
    CREATE INDEX IF NOT EXISTS idx_flows_status ON flows(status);

    CREATE TABLE IF NOT EXISTS flow_exchanges (
        flow_id         INTEGER NOT NULL,
        idx             INTEGER NOT NULL,
        stream_id       INTEGER NOT NULL DEFAULT 0,
        method          TEXT    NOT NULL DEFAULT '',
        host            TEXT    NOT NULL DEFAULT '',
        path            TEXT    NOT NULL DEFAULT '',
        status          INTEGER NOT NULL DEFAULT 0,
        req_head        TEXT    NOT NULL DEFAULT '',
        req_body        BLOB,
        req_body_decoded BLOB,
        resp_head       TEXT    NOT NULL DEFAULT '',
        resp_body       BLOB,
        resp_body_decoded BLOB,
        resp_encoding   TEXT    NOT NULL DEFAULT '',
        PRIMARY KEY (flow_id, idx)
    ) WITHOUT ROWID;
)SQL";

namespace {

QString sqliteErr(sqlite3* db)
{
    return QStringLiteral("SQLite 错误：%1").arg(QString::fromUtf8(sqlite3_errmsg(db)));
}

QString headText(const std::optional<HttpMessage>& m)
{
    if (!m)
        return QString();
    QString s = m->startLine;
    for (const auto& kv : m->headers) {
        s += QLatin1Char('\n');
        s += kv.first;
        s += QLatin1String(": ");
        s += kv.second;
    }
    return s;
}

// raw 上限裁剪；空 → nullopt（存 NULL）。
std::optional<QByteArray> capBlob(const QByteArray& b, qsizetype cap)
{
    if (b.isEmpty())
        return std::nullopt;
    return b.left(int(std::min<qsizetype>(b.size(), cap)));
}

// BodyView 文本 → BLOB（保真存文本字节）。
std::optional<QByteArray> textBlob(const std::optional<BodyView>& b)
{
    if (!b)
        return std::nullopt;
    return b->text.toUtf8();
}

QJsonObject bodyToJson(const unsigned char* p, int bytes)
{
    if (!p || bytes == 0)
        return QJsonObject(); // 由调用方替换为 null
    const QByteArray raw(reinterpret_cast<const char*>(p), bytes);
    BodyView v;
    v.text = QString::fromUtf8(raw.left(int(std::min<qsizetype>(bytes, kBodyCap))));
    v.size = bytes;
    return v.toJson();
}

// RAII 语句。
struct Stmt {
    sqlite3* db = nullptr;
    sqlite3_stmt* st = nullptr;

    ~Stmt() { finalize(); }
    void finalize()
    {
        if (st) {
            sqlite3_finalize(st);
            st = nullptr;
        }
    }
    bool prepare(sqlite3* d, const QString& sql, QString* err)
    {
        db = d;
        const QByteArray u8 = sql.toUtf8();
        if (sqlite3_prepare_v2(db, u8.constData(), -1, &st, nullptr) != SQLITE_OK) {
            if (err)
                *err = sqliteErr(db);
            return false;
        }
        return true;
    }
    bool exec(QString* err)
    {
        if (sqlite3_step(st) != SQLITE_DONE) {
            if (err)
                *err = sqliteErr(db);
            return false;
        }
        return true;
    }
};

bool execSimple(sqlite3* db, const QString& sql, QString* err = nullptr,
                bool ignoreError = false)
{
    char* msg = nullptr;
    const QByteArray u8 = sql.toUtf8();
    const int rc = sqlite3_exec(db, u8.constData(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK && !ignoreError) {
        if (err)
            *err = msg ? QStringLiteral("SQLite 错误：%1").arg(QString::fromUtf8(msg))
                       : sqliteErr(db);
    }
    if (msg)
        sqlite3_free(msg);
    return rc == SQLITE_OK;
}

void bindText(sqlite3_stmt* st, int idx, const QString& v)
{
    const QByteArray u8 = v.toUtf8();
    sqlite3_bind_text(st, idx, u8.constData(), int(u8.size()), SQLITE_TRANSIENT);
}
void bindInt(sqlite3_stmt* st, int idx, qint64 v) { sqlite3_bind_int64(st, idx, v); }
void bindBlobOpt(sqlite3_stmt* st, int idx, const std::optional<QByteArray>& v)
{
    if (v)
        sqlite3_bind_blob(st, idx, v->constData(), int(v->size()), SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, idx);
}

QString columnStr(sqlite3_stmt* st, int col)
{
    const unsigned char* p = sqlite3_column_text(st, col);
    return p ? QString::fromUtf8(reinterpret_cast<const char*>(p)) : QString();
}
QByteArray columnBlob(sqlite3_stmt* st, int col)
{
    const void* p = sqlite3_column_blob(st, col);
    const int n = sqlite3_column_bytes(st, col);
    if (!p || n <= 0)
        return QByteArray();
    return QByteArray(reinterpret_cast<const char*>(p), n);
}

// WHERE 子句构造（参数按序号通过 bindArgs 绑定）。
struct WhereClause {
    QString clause;
    QVector<QByteArray> tmp; // 参数字面量持有
    enum class Kind { Text, Int };
    struct Arg {
        Kind kind;
        qint64 num = 0;
    };
    QVector<Arg> args;

    void addTextLike(const QString& like)
    {
        tmp.push_back(like.toUtf8());
        args.push_back(Arg { Kind::Text, 0 });
    }
    void addInt(qint64 v) { args.push_back(Arg { Kind::Int, v }); }

    bool bindAll(sqlite3_stmt* st, int startIdx, QString* err) const
    {
        int idx = startIdx;
        int textIdx = 0;
        for (const Arg& a : args) {
            if (a.kind == Kind::Text) {
                const QByteArray& v = tmp.at(textIdx++);
                sqlite3_bind_text(st, idx, v.constData(), int(v.size()), SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_int64(st, idx, a.num);
            }
            ++idx;
        }
        Q_UNUSED(err)
        return true;
    }
};

WhereClause whereClause(const FlowsQuery& q)
{
    WhereClause w;
    QStringList conds;
    const auto nonEmpty = [](const QString& s) { return !s.isEmpty(); };

    if (nonEmpty(q.q)) {
        // 轻表搜索：host/path/方法/头部/进程/包名（索引+LIKE 前缀，不碰正文巨表）
        conds.push_back(
            QStringLiteral("(host LIKE ? OR path LIKE ? OR req_head LIKE ? OR resp_head "
                           "LIKE ? OR comm LIKE ? OR package LIKE ? OR method LIKE ? OR "
                           "dst LIKE ?)"));
        for (int i = 0; i < 8; ++i)
            w.addTextLike(QStringLiteral("%%1%").arg(q.q));
    }
    if (nonEmpty(q.host)) {
        conds.push_back(QStringLiteral("host LIKE ?"));
        w.addTextLike(QStringLiteral("%%1%").arg(q.host));
    }
    if (nonEmpty(q.package)) {
        conds.push_back(QStringLiteral("package = ?"));
        w.tmp.push_back(q.package.toUtf8());
        w.args.push_back(WhereClause::Arg { WhereClause::Kind::Text, 0 });
    }
    if (nonEmpty(q.method)) {
        conds.push_back(QStringLiteral("method = ?"));
        w.tmp.push_back(q.method.toUtf8());
        w.args.push_back(WhereClause::Arg { WhereClause::Kind::Text, 0 });
    }
    if (q.status.has_value()) {
        conds.push_back(QStringLiteral("status = ?"));
        w.addInt(*q.status);
    }
    if (q.statusClass.has_value()) {
        conds.push_back(QStringLiteral("(status >= ? AND status < ?)"));
        w.addInt(qint64(*q.statusClass) * 100);
        w.addInt(qint64(*q.statusClass) * 100 + 100);
    }
    if (nonEmpty(q.kind)) {
        conds.push_back(QStringLiteral("kind = ?"));
        w.tmp.push_back(q.kind.toUtf8());
        w.args.push_back(WhereClause::Arg { WhereClause::Kind::Text, 0 });
    }
    if (q.sinceMs.has_value()) {
        conds.push_back(QStringLiteral("last_ts_ms >= ?"));
        w.addInt(*q.sinceMs);
    }
    w.clause = conds.isEmpty()
                   ? QString()
                   : QStringLiteral("WHERE %1").arg(conds.join(QStringLiteral(" AND ")));
    return w;
}

} // namespace

Store::~Store()
{
    close();
}

Store::Store(Store&& other) noexcept
    : db_(other.db_)
{
    other.db_ = nullptr;
}

Store& Store::operator=(Store&& other) noexcept
{
    if (this != &other) {
        close();
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void Store::close()
{
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

mw::Result<Store> Store::open(const QString& path)
{
    const QString parent = QFileInfo(path).absolutePath();
    if (!parent.isEmpty())
        QDir().mkpath(parent);
    Store s;
    const QByteArray u8 = path.toUtf8();
    if (sqlite3_open_v2(u8.constData(), &s.db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
        != SQLITE_OK) {
        const QString err = s.db_ ? sqliteErr(s.db_) : QStringLiteral("SQLite 错误：无法打开 %1").arg(path);
        s.close();
        return mw::err<Store>(err);
    }
    sqlite3_busy_timeout(s.db_, 500);
    execSimple(s.db_, QStringLiteral("PRAGMA journal_mode = WAL;"), nullptr, true);
    execSimple(s.db_, QStringLiteral("PRAGMA synchronous = NORMAL;"), nullptr, true);
    execSimple(s.db_, QStringLiteral("PRAGMA temp_store = MEMORY;"), nullptr, true);
    execSimple(s.db_, QStringLiteral("PRAGMA cache_size = -8000;"), nullptr, true);
    // v2 → v3 迁移：旧表把多 MB 的 exchanges 大 JSON 放在热表里，是列表/统计
    // 变慢的根因。迁移只保留摘要列（旧缓存体不搬）。
    bool legacy = false;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(s.db_, "SELECT COUNT(*) FROM pragma_table_info('flows') "
                                      "WHERE name IN ('exchanges','req_body')",
                               -1, &st, nullptr)
            == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                legacy = sqlite3_column_int64(st, 0) > 0;
            sqlite3_finalize(st);
        }
    }
    if (legacy) {
        execSimple(s.db_, QStringLiteral("ALTER TABLE flows RENAME TO flows_legacy_v2; "
                                         "DROP TABLE IF EXISTS flow_exchanges_cache;"));
    }
    QString err;
    if (!execSimple(s.db_, QStringLiteral("BEGIN;"))
        || !execSimple(s.db_, QString::fromUtf8(kSchemaV3), &err)
        || !execSimple(s.db_, QStringLiteral("COMMIT;"), &err)) {
        execSimple(s.db_, QStringLiteral("ROLLBACK;"), nullptr, true);
        return mw::err<Store>(err.isEmpty() ? sqliteErr(s.db_) : err);
    }
    if (legacy) {
        execSimple(
            s.db_,
            QStringLiteral(
                "INSERT OR IGNORE INTO flows (id, first_ts_ms, last_ts_ms, pid, comm, "
                "package, src, dst, kind, host, method, path, status, wbytes, rbytes, "
                "content_type, req_head, resp_head, parsed, exchange_count) "
                "SELECT id, first_ts_ms, last_ts_ms, pid, comm, package, src, dst, "
                "kind, host, method, path, status, wbytes, rbytes, content_type, "
                "req_head, resp_head, 0, 0 FROM flows_legacy_v2; "
                "DROP TABLE flows_legacy_v2;"));
    }
    return mw::Result<Store>::Ok(std::move(s));
}

mw::Result<Store> Store::openInMemory()
{
    Store s;
    if (sqlite3_open_v2(":memory:", &s.db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
        != SQLITE_OK) {
        const QString err = sqliteErr(s.db_);
        s.close();
        return mw::err<Store>(err);
    }
    sqlite3_busy_timeout(s.db_, 500);
    QString err;
    if (!execSimple(s.db_, QStringLiteral("BEGIN;"))
        || !execSimple(s.db_, QString::fromUtf8(kSchemaV3), &err)
        || !execSimple(s.db_, QStringLiteral("COMMIT;"))) {
        execSimple(s.db_, QStringLiteral("ROLLBACK;"), nullptr, true);
        return mw::err<Store>(err.isEmpty() ? sqliteErr(s.db_) : err);
    }
    return mw::Result<Store>::Ok(std::move(s));
}

mw::Result<int> Store::upsertFlows(const QVector<Flow>& flows) const
{
    if (flows.isEmpty())
        return mw::Result<int>::Ok(0);
    QString err;
    if (!execSimple(db_, QStringLiteral("BEGIN;")))
        return mw::err<int>(sqliteErr(db_));
    Stmt st;
    if (!st.prepare(db_, QStringLiteral(
                            "INSERT INTO flows (id, first_ts_ms, last_ts_ms, pid, comm, "
                            "package, src, dst, kind, host, method, path, status, wbytes, "
                            "rbytes, content_type, req_head, resp_head, raw_preview, "
                            "raw_write, raw_read, parsed, exchange_count) "
                            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,"
                            "?16,?17,?18,?19,?20,?21,0,?22) "
                            "ON CONFLICT(id) DO UPDATE SET "
                            "last_ts_ms=excluded.last_ts_ms, wbytes=excluded.wbytes, "
                            "rbytes=excluded.rbytes, req_head=excluded.req_head, "
                            "resp_head=excluded.resp_head, "
                            "raw_preview=excluded.raw_preview, "
                            "raw_write=excluded.raw_write, raw_read=excluded.raw_read, "
                            "parsed=0, host=excluded.host, method=excluded.method, "
                            "path=excluded.path, status=excluded.status, "
                            "kind=excluded.kind, package=excluded.package, "
                            "content_type=excluded.content_type, "
                            "exchange_count=excluded.exchange_count"),
                      &err)) {
        execSimple(db_, QStringLiteral("ROLLBACK;"), nullptr, true);
        return mw::err<int>(err);
    }
    for (const Flow& f : flows) {
        const FlowSummary sum = FlowSummary::from(f);
        sqlite3_reset(st.st);
        sqlite3_clear_bindings(st.st);
        bindInt(st.st, 1, f.id);
        bindInt(st.st, 2, f.firstTsMs);
        bindInt(st.st, 3, f.lastTsMs);
        bindInt(st.st, 4, qint64(f.pid));
        bindText(st.st, 5, f.comm);
        bindText(st.st, 6, f.package);
        bindText(st.st, 7, f.src);
        bindText(st.st, 8, f.dst);
        bindText(st.st, 9, sum.kind);
        bindText(st.st, 10, sum.host);
        bindText(st.st, 11, sum.method);
        bindText(st.st, 12, sum.path);
        bindInt(st.st, 13, sum.status);
        bindInt(st.st, 14, qint64(f.wbytes));
        bindInt(st.st, 15, qint64(f.rbytes));
        bindText(st.st, 16, sum.contentType);
        bindText(st.st, 17, headText(f.request));
        bindText(st.st, 18, headText(f.response));
        bindText(st.st, 19, f.rawPreview);
        bindBlobOpt(st.st, 20, capBlob(f.rawWrite, kRawCap));
        bindBlobOpt(st.st, 21, capBlob(f.rawRead, kRawCap));
        bindInt(st.st, 22, sum.exchanges);
        if (!st.exec(&err)) {
            execSimple(db_, QStringLiteral("ROLLBACK;"), nullptr, true);
            return mw::err<int>(err);
        }
    }
    if (!execSimple(db_, QStringLiteral("COMMIT;"))) {
        execSimple(db_, QStringLiteral("ROLLBACK;"), nullptr, true);
        return mw::err<int>(sqliteErr(db_));
    }
    execSimple(db_, QStringLiteral("PRAGMA wal_checkpoint(PASSIVE)"), nullptr, true);
    return mw::Result<int>::Ok(int(flows.size()));
}

mw::Result<QVector<FlowSummary>> Store::querySummaries(const FlowsQuery& q) const
{
    const WhereClause w = whereClause(q);
    const QString sql = QStringLiteral(
                            "SELECT id, first_ts_ms, last_ts_ms, pid, comm, package, src, "
                            "dst, kind, host, method, path, status, wbytes, rbytes, "
                            "content_type, exchange_count FROM flows %1 "
                            "ORDER BY first_ts_ms DESC LIMIT ? OFFSET ?")
                            .arg(w.clause);
    Stmt st;
    QString err;
    if (!st.prepare(db_, sql, &err))
        return mw::err<QVector<FlowSummary>>(err);
    if (!w.bindAll(st.st, 1, &err))
        return mw::err<QVector<FlowSummary>>(err);
    const int nArgs = int(w.args.size());
    bindInt(st.st, nArgs + 1, std::max(1, q.limit));
    bindInt(st.st, nArgs + 2, q.offset);
    QVector<FlowSummary> out;
    for (;;) {
        const int rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            return mw::err<QVector<FlowSummary>>(sqliteErr(db_));
        FlowSummary s;
        s.id = sqlite3_column_int64(st.st, 0);
        s.firstTsMs = sqlite3_column_int64(st.st, 1);
        s.lastTsMs = sqlite3_column_int64(st.st, 2);
        s.pid = quint32(sqlite3_column_int64(st.st, 3));
        s.comm = columnStr(st.st, 4);
        s.package = columnStr(st.st, 5);
        s.src = columnStr(st.st, 6);
        s.dst = columnStr(st.st, 7);
        s.kind = columnStr(st.st, 8);
        s.host = columnStr(st.st, 9);
        s.method = columnStr(st.st, 10);
        s.path = columnStr(st.st, 11);
        s.status = quint16(sqlite3_column_int64(st.st, 12));
        s.wbytes = quint64(sqlite3_column_int64(st.st, 13));
        s.rbytes = quint64(sqlite3_column_int64(st.st, 14));
        s.contentType = columnStr(st.st, 15);
        s.exchanges = int(sqlite3_column_int64(st.st, 16));
        out.push_back(std::move(s));
    }
    return mw::Result<QVector<FlowSummary>>::Ok(std::move(out));
}

mw::Result<quint64> Store::countFiltered(const FlowsQuery& q) const
{
    const WhereClause w = whereClause(q);
    const QString sql =
        QStringLiteral("SELECT COUNT(*) FROM flows %1").arg(w.clause);
    Stmt st;
    QString err;
    if (!st.prepare(db_, sql, &err))
        return mw::err<quint64>(err);
    if (!w.bindAll(st.st, 1, &err))
        return mw::err<quint64>(err);
    const int rc = sqlite3_step(st.st);
    if (rc != SQLITE_ROW)
        return mw::err<quint64>(sqliteErr(db_));
    return mw::Result<quint64>::Ok(quint64(sqlite3_column_int64(st.st, 0)));
}

mw::Result<quint64> Store::count() const
{
    Stmt st;
    QString err;
    if (!st.prepare(db_, QStringLiteral("SELECT COUNT(*) FROM flows"), &err))
        return mw::err<quint64>(err);
    if (sqlite3_step(st.st) != SQLITE_ROW)
        return mw::err<quint64>(sqliteErr(db_));
    return mw::Result<quint64>::Ok(quint64(sqlite3_column_int64(st.st, 0)));
}

// 缓存 exchanges（首开详情时）。
static mw::Result<bool> cacheExchanges(sqlite3* db, qint64 id,
                                       const QVector<ExchangeView>& views)
{
    QString err;
    if (!execSimple(db, QStringLiteral("BEGIN;")))
        return mw::Result<bool>::Err(sqliteErr(db));
    {
        Stmt del;
        if (!del.prepare(db, QStringLiteral("DELETE FROM flow_exchanges WHERE flow_id = ?"),
                         &err)
            || (sqlite3_bind_int64(del.st, 1, id), !del.exec(&err))) {
            execSimple(db, QStringLiteral("ROLLBACK;"), nullptr, true);
            return mw::Result<bool>::Err(err);
        }
    }
    {
        Stmt ins;
        if (!ins.prepare(db, QStringLiteral(
                                 "INSERT INTO flow_exchanges (flow_id, idx, stream_id, "
                                 "method, host, path, status, req_head, req_body, "
                                 "req_body_decoded, resp_head, resp_body, "
                                 "resp_body_decoded, resp_encoding) "
                                 "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)"),
                         &err)) {
            execSimple(db, QStringLiteral("ROLLBACK;"), nullptr, true);
            return mw::Result<bool>::Err(err);
        }
        for (int i = 0; i < views.size(); ++i) {
            const ExchangeView& e = views.at(i);
            sqlite3_reset(ins.st);
            sqlite3_clear_bindings(ins.st);
            bindInt(ins.st, 1, id);
            bindInt(ins.st, 2, i);
            bindInt(ins.st, 3, qint64(e.streamId));
            bindText(ins.st, 4, e.method);
            bindText(ins.st, 5, e.host);
            bindText(ins.st, 6, e.path);
            bindInt(ins.st, 7, e.status);
            bindText(ins.st, 8, e.reqHead);
            bindBlobOpt(ins.st, 9, textBlob(e.reqBody));
            bindBlobOpt(ins.st, 10, textBlob(e.reqBodyDecoded));
            bindText(ins.st, 11, e.respHead);
            bindBlobOpt(ins.st, 12, textBlob(e.respBody));
            bindBlobOpt(ins.st, 13, textBlob(e.respBodyDecoded));
            bindText(ins.st, 14, e.respEncoding);
            if (!ins.exec(&err)) {
                execSimple(db, QStringLiteral("ROLLBACK;"), nullptr, true);
                return mw::Result<bool>::Err(err);
            }
        }
    }
    // 回填真实交互数 + 摘要（若首列空）
    {
        Stmt upd;
        if (!upd.prepare(db, QStringLiteral("UPDATE flows SET parsed=1, exchange_count=? "
                                            "WHERE id=?"),
                         &err)) {
            execSimple(db, QStringLiteral("ROLLBACK;"), nullptr, true);
            return mw::Result<bool>::Err(err);
        }
        bindInt(upd.st, 1, views.size());
        bindInt(upd.st, 2, id);
        if (!upd.exec(&err)) {
            execSimple(db, QStringLiteral("ROLLBACK;"), nullptr, true);
            return mw::Result<bool>::Err(err);
        }
    }
    if (!views.isEmpty()) {
        const ExchangeView& first = views.at(0);
        Stmt upd;
        if (!upd.prepare(db, QStringLiteral(
                                 "UPDATE flows SET host=CASE WHEN host='' THEN ? ELSE "
                                 "host END, method=CASE WHEN method='' THEN ? ELSE "
                                 "method END, path=CASE WHEN path='' THEN ? ELSE path "
                                 "END, status=CASE WHEN status=0 THEN ? ELSE status END, "
                                 "req_head=CASE WHEN req_head='' THEN ? ELSE req_head "
                                 "END, resp_head=CASE WHEN resp_head='' THEN ? ELSE "
                                 "resp_head END WHERE id=?"),
                         &err)) {
            execSimple(db, QStringLiteral("ROLLBACK;"), nullptr, true);
            return mw::Result<bool>::Err(err);
        }
        bindText(upd.st, 1, first.host);
        bindText(upd.st, 2, first.method);
        bindText(upd.st, 3, first.path);
        bindInt(upd.st, 4, first.status);
        bindText(upd.st, 5, first.reqHead);
        bindText(upd.st, 6, first.respHead);
        bindInt(upd.st, 7, id);
        if (!upd.exec(&err)) {
            execSimple(db, QStringLiteral("ROLLBACK;"), nullptr, true);
            return mw::Result<bool>::Err(err);
        }
    }
    if (!execSimple(db, QStringLiteral("COMMIT;"))) {
        execSimple(db, QStringLiteral("ROLLBACK;"), nullptr, true);
        return mw::Result<bool>::Err(sqliteErr(db));
    }
    return mw::Result<bool>::Ok();
}

static QJsonArray loadCachedExchanges(sqlite3* db, qint64 id)
{
    QJsonArray arr;
    Stmt st;
    if (!st.prepare(db, QStringLiteral(
                            "SELECT stream_id, method, host, path, status, req_head, "
                            "req_body, req_body_decoded, resp_head, resp_body, "
                            "resp_body_decoded, resp_encoding FROM flow_exchanges WHERE "
                            "flow_id = ? ORDER BY idx"),
                    nullptr))
        return arr;
    sqlite3_bind_int64(st.st, 1, id);
    for (;;) {
        const int rc = sqlite3_step(st.st);
        if (rc != SQLITE_ROW)
            break;
        QJsonObject o;
        o.insert(QStringLiteral("stream_id"), sqlite3_column_int64(st.st, 0));
        o.insert(QStringLiteral("method"), columnStr(st.st, 1));
        o.insert(QStringLiteral("host"), columnStr(st.st, 2));
        o.insert(QStringLiteral("path"), columnStr(st.st, 3));
        o.insert(QStringLiteral("status"),
                 quint16(sqlite3_column_int64(st.st, 4)));
        o.insert(QStringLiteral("req_head"), columnStr(st.st, 5));
        const auto bodyJson = [](sqlite3_stmt* s, int col) -> QJsonValue {
            const void* p = sqlite3_column_blob(s, col);
            const int n = sqlite3_column_bytes(s, col);
            if (!p || n == 0)
                return QJsonValue(); // null
            return bodyToJson(reinterpret_cast<const unsigned char*>(p), n);
        };
        o.insert(QStringLiteral("req_body"), bodyJson(st.st, 6));
        o.insert(QStringLiteral("req_body_decoded"), bodyJson(st.st, 7));
        o.insert(QStringLiteral("resp_head"), columnStr(st.st, 8));
        o.insert(QStringLiteral("resp_body"), bodyJson(st.st, 9));
        o.insert(QStringLiteral("resp_body_decoded"), bodyJson(st.st, 10));
        o.insert(QStringLiteral("resp_encoding"), columnStr(st.st, 11));
        arr.append(std::move(o));
    }
    return arr;
}

mw::Result<QJsonObject> Store::getDetail(qint64 id) const
{
    QString kind;
    QByteArray rawWrite, rawRead;
    bool parsed = false;
    bool found = false;
    {
        Stmt st;
        QString err;
        if (!st.prepare(db_, QStringLiteral("SELECT kind, raw_write, raw_read, parsed FROM "
                                            "flows WHERE id = ?"),
                        &err))
            return mw::err<QJsonObject>(err);
        sqlite3_bind_int64(st.st, 1, id);
        const int rc = sqlite3_step(st.st);
        if (rc == SQLITE_ROW) {
            found = true;
            kind = columnStr(st.st, 0);
            rawWrite = columnBlob(st.st, 1);
            rawRead = columnBlob(st.st, 2);
            parsed = sqlite3_column_int64(st.st, 3) == 1;
        } else if (rc != SQLITE_DONE) {
            return mw::err<QJsonObject>(sqliteErr(db_));
        }
    }
    if (!found)
        return mw::Result<QJsonObject>::Ok(QJsonObject());

    QJsonArray exchangesJson;
    if (parsed) {
        exchangesJson = loadCachedExchanges(db_, id);
    } else {
        // 首次打开：全量解析（重活，一次性）
        Flow ghost;
        ghost.id = id;
        ghost.kind = FlowKind::Plaintext;
        ghost.rawWrite = rawWrite;
        ghost.rawRead = rawRead;
        const QVector<ExchangeView> views = computeExchanges(ghost);
        for (const ExchangeView& v : views)
            exchangesJson.append(v.toJson());
        const auto cached = cacheExchanges(db_, id, views);
        if (!cached)
            return mw::err<QJsonObject>(cached.error());
    }

    QJsonObject detail;
    {
        Stmt st;
        QString err;
        if (!st.prepare(db_, QStringLiteral(
                                "SELECT id, first_ts_ms, last_ts_ms, pid, comm, package, "
                                "src, dst, kind, host, method, path, status, wbytes, "
                                "rbytes, content_type, req_head, resp_head, raw_preview, "
                                "exchange_count FROM flows WHERE id = ?"),
                        &err))
            return mw::err<QJsonObject>(err);
        sqlite3_bind_int64(st.st, 1, id);
        const int rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE)
            return mw::Result<QJsonObject>::Ok(QJsonObject());
        if (rc != SQLITE_ROW)
            return mw::err<QJsonObject>(sqliteErr(db_));
        detail.insert(QStringLiteral("id"), sqlite3_column_int64(st.st, 0));
        detail.insert(QStringLiteral("first_ts_ms"), sqlite3_column_int64(st.st, 1));
        detail.insert(QStringLiteral("last_ts_ms"), sqlite3_column_int64(st.st, 2));
        detail.insert(QStringLiteral("pid"), sqlite3_column_int64(st.st, 3));
        detail.insert(QStringLiteral("comm"), columnStr(st.st, 4));
        detail.insert(QStringLiteral("package"), columnStr(st.st, 5));
        detail.insert(QStringLiteral("src"), columnStr(st.st, 6));
        detail.insert(QStringLiteral("dst"), columnStr(st.st, 7));
        detail.insert(QStringLiteral("kind"), columnStr(st.st, 8));
        detail.insert(QStringLiteral("host"), columnStr(st.st, 9));
        detail.insert(QStringLiteral("method"), columnStr(st.st, 10));
        detail.insert(QStringLiteral("path"), columnStr(st.st, 11));
        detail.insert(QStringLiteral("status"), sqlite3_column_int64(st.st, 12));
        detail.insert(QStringLiteral("wbytes"), sqlite3_column_int64(st.st, 13));
        detail.insert(QStringLiteral("rbytes"), sqlite3_column_int64(st.st, 14));
        detail.insert(QStringLiteral("content_type"), columnStr(st.st, 15));
        detail.insert(QStringLiteral("req_head"), columnStr(st.st, 16));
        detail.insert(QStringLiteral("resp_head"), columnStr(st.st, 17));
        detail.insert(QStringLiteral("raw_preview"), columnStr(st.st, 18));
    }
    // 首个交互的正文（兼容旧 UI 字段）
    if (!exchangesJson.isEmpty()) {
        const QJsonObject first = exchangesJson.at(0).toObject();
        if (detail.value(QLatin1String("req_head")).toString().isEmpty())
            detail.insert(QStringLiteral("req_head"), first.value(QLatin1String("req_head")));
        if (detail.value(QLatin1String("resp_head")).toString().isEmpty())
            detail.insert(QStringLiteral("resp_head"), first.value(QLatin1String("resp_head")));
        detail.insert(QStringLiteral("req_body"), first.value(QLatin1String("req_body")));
        detail.insert(QStringLiteral("resp_body"), first.value(QLatin1String("resp_body")));
    }
    detail.insert(QStringLiteral("exchanges"), std::move(exchangesJson));
    return mw::Result<QJsonObject>::Ok(std::move(detail));
}

mw::Result<QJsonObject> Store::kindStats() const
{
    Stmt st;
    QString err;
    if (!st.prepare(db_, QStringLiteral("SELECT kind, COUNT(*), COALESCE(SUM(wbytes),0), "
                                        "COALESCE(SUM(rbytes),0) FROM flows GROUP BY kind"),
                    &err))
        return mw::err<QJsonObject>(err);
    QJsonObject kinds;
    qint64 total = 0, parsed = 0;
    for (;;) {
        const int rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            return mw::err<QJsonObject>(sqliteErr(db_));
        const QString kind = columnStr(st.st, 0);
        const qint64 n = sqlite3_column_int64(st.st, 1);
        const qint64 w = sqlite3_column_int64(st.st, 2);
        const qint64 r = sqlite3_column_int64(st.st, 3);
        total += n;
        if (kind == QLatin1String("http1") || kind == QLatin1String("http2"))
            parsed += n;
        kinds.insert(kind, QJsonObject {
                               { QStringLiteral("count"), n },
                               { QStringLiteral("wbytes"), w },
                               { QStringLiteral("rbytes"), r },
                           });
    }
    QJsonObject out;
    out.insert(QStringLiteral("kinds"), std::move(kinds));
    out.insert(QStringLiteral("total"), total);
    out.insert(QStringLiteral("parsed"), parsed);
    out.insert(QStringLiteral("parse_rate"),
               total > 0 ? double(parsed) / double(total) : 0.0);
    return mw::Result<QJsonObject>::Ok(std::move(out));
}

mw::Result<bool> Store::clear() const
{
    QString err;
    if (!execSimple(db_, QStringLiteral("DELETE FROM flows"), &err))
        return mw::Result<bool>::Err(err);
    if (!execSimple(db_, QStringLiteral("DELETE FROM flow_exchanges"), &err))
        return mw::Result<bool>::Err(err);
    execSimple(db_, QStringLiteral("VACUUM"), nullptr, true);
    return mw::Result<bool>::Ok();
}

} // namespace mw::capture
