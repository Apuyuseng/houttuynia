#include "streamweb.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpSocket>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <sys/socket.h>
#include <sys/types.h>

#include "log.hpp"
#include "scrcpy/client.hpp"
#include "scrcpy/control.hpp"
#include "scrcpy/options.hpp"
#include "state.hpp"

namespace mw::daemon {

namespace {

// ===================== 线协议（浏览器侧 app.js/stream.js 对齐解析，全大端）=====================
// 帧头 15 字节：u16 magic(0x4D57) | u8 type | u64 pts_us | u32 payload_len
enum : quint8 {
    kPktConfig = 0, // SPS/PPS（Annex-B）
    kPktDelta = 1,
    kPktKey = 2,
    kPktMeta = 3,  // 尺寸变化：u32 w + u32 h
    kPktEnd = 4,   // 流结束（会话关闭/出错）
};

constexpr quint16 kMagic = 0x4D57;
constexpr size_t kQueueCap = 120; // 约 2–4 秒（30–60fps），超出即慢客户端

struct OutFrame {
    quint8 type = kPktDelta;
    quint64 pts = 0;
    QByteArray data;

    QByteArray encode() const
    {
        QByteArray out;
        out.reserve(15 + data.size());
        out.append(char(kMagic >> 8));
        out.append(char(kMagic & 0xFF));
        out.append(char(type));
        for (int i = 7; i >= 0; --i)
            out.append(char((pts >> (i * 8)) & 0xFF));
        const quint32 len = quint32(data.size());
        out.append(char((len >> 24) & 0xFF));
        out.append(char((len >> 16) & 0xFF));
        out.append(char((len >> 8) & 0xFF));
        out.append(char(len & 0xFF));
        out.append(data);
        return out;
    }
};

// ===================== 会话 =====================

struct StreamSession {
    quint64 id = 0;
    QString serial;
    QDateTime createdAt = QDateTime::currentDateTime();

    // scrcpy 会话部件（video 在开流时即移入喂包线程，不在此持有）
    std::optional<mw::scrcpy::ControlChannel> control;
    mw::scrcpy::SessionGuard guard;

    std::atomic_bool attached{false};  // 视频推流已被领取
    std::atomic_bool aborted{false};
    std::atomic_bool streamEnded{false}; // 喂包线程终止（正常结束或对端编码器挂了）
    QString streamError;               // 喂包线程出错详情（仅启动期读取，单写者）
    QString codec = QStringLiteral("h264");
    std::atomic<quint32> width{0}, height{0};

    // 控制通道写串行化（多个 HTTP 池线程并发 touch/key 时会写到同一 socket）
    std::mutex controlMu;

    // 喂包线程 → 推流线程 的有界队列
    std::mutex qMu;
    std::condition_variable qCv;
    std::deque<OutFrame> queue;
    QByteArray lastConfig; // 最近的 SPS/PPS（断流恢复时重发）

    void stop()
    {
        if (aborted.exchange(true))
            return;
        {
            std::lock_guard lk(qMu);
            queue.push_back(OutFrame{ kPktEnd, 0, {} });
        }
        qCv.notify_all();
        guard.stop(); // 杀 server / 拆转发 → 喂包线程读失败退出
    }

    bool sendControl(const mw::scrcpy::ControlMessage& msg, QString* err)
    {
        if (!control) {
            *err = QStringLiteral("控制通道不可用");
            return false;
        }
        std::lock_guard lk(controlMu);
        const auto r = control->send(msg);
        if (!r) {
            *err = r.error();
            return false;
        }
        return true;
    }
};

struct StreamRegistry {
    std::mutex mu;
    QMap<quint64, std::shared_ptr<StreamSession>> sessions;
    quint64 nextId = 1;

    std::shared_ptr<StreamSession> get(quint64 id)
    {
        std::lock_guard lk(mu);
        return sessions.value(id);
    }
    void add(const std::shared_ptr<StreamSession>& s)
    {
        std::lock_guard lk(mu);
        sessions.insert(s->id, s);
    }
    void remove(quint64 id)
    {
        std::lock_guard lk(mu);
        sessions.remove(id);
    }
    // 回收从未被领取视频的陈旧会话（浏览器开了没看）
    void reapStale()
    {
        std::lock_guard lk(mu);
        const QDateTime now = QDateTime::currentDateTime();
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!it.value()->attached.load()
                && it.value()->createdAt.secsTo(now) > 600) {
                it.value()->stop();
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }
};

StreamRegistry& registry()
{
    static StreamRegistry r;
    return r;
}

// ===================== 喂包线程 =====================

void feederLoop(std::shared_ptr<StreamSession> s, mw::scrcpy::VideoStream video)
{
    QPair<quint32, quint32> lastSize = video.size();
    auto push = [&](OutFrame f) {
        std::lock_guard lk(s->qMu);
        if (s->queue.size() >= kQueueCap) {
            // 慢客户端：清空积压，让编码器重发 config+关键帧续流
            s->queue.clear();
            MW_LOG("stream", QStringLiteral("会话 %1 客户端消费过慢，重置视频流").arg(s->id));
            QString err;
            s->sendControl(mw::scrcpy::ControlMessage::resetVideo(), &err);
            // resetVideo 后 server 会重发 config+key；不必在队列里塞东西
        }
        s->queue.push_back(std::move(f));
        s->qCv.notify_one();
    };

    while (!s->aborted.load()) {
        auto pkt = video.nextPacket();
        if (!pkt.ok() || !pkt.value().has_value()) {
            if (!pkt.ok())
                s->streamError = pkt.error();
            break; // 流结束/出错
        }
        const auto& p = *pkt.value();
        // 尺寸变化（旋转/分屏）——demux 已吸收 session meta
        const QPair<quint32, quint32> sz = video.size();
        if (sz != lastSize && sz.first > 0 && sz.second > 0) {
            lastSize = sz;
            s->width.store(sz.first);
            s->height.store(sz.second);
            QByteArray meta;
            meta.append(char((sz.first >> 24) & 0xFF)).append(char((sz.first >> 16) & 0xFF))
                .append(char((sz.first >> 8) & 0xFF)).append(char(sz.first & 0xFF));
            meta.append(char((sz.second >> 24) & 0xFF)).append(char((sz.second >> 16) & 0xFF))
                .append(char((sz.second >> 8) & 0xFF)).append(char(sz.second & 0xFF));
            push(OutFrame{ kPktMeta, p.pts, meta });
        }
        quint8 type = kPktDelta;
        if (p.config) {
            type = kPktConfig;
            s->lastConfig = p.data;
        } else if (p.keyFrame) {
            type = kPktKey;
        }
        push(OutFrame{ type, p.pts, p.data });
    }
    s->streamEnded.store(true);
    s->stop();
}

// ===================== 推流（HTTP 池线程内执行）=====================

// 浏览器断开探测：无包期间每秒 MSG_PEEK 一次
bool peerGone(QTcpSocket& sock)
{
    if (sock.state() != QAbstractSocket::ConnectedState)
        return true;
    char c;
    const qint64 n = ::recv(int(sock.socketDescriptor()), &c, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0; // 0 = EOF；<0 = 无数据（EAGAIN）或错误（按存活处理，写出错会暴露）
}

bool writeAll(QTcpSocket& sock, const QByteArray& data)
{
    qint64 off = 0;
    while (off < data.size()) {
        const qint64 n = sock.write(data.mid(off, 256 * 1024));
        if (n < 0)
            return false;
        off += n;
        if (!sock.waitForBytesWritten(10000))
            return false;
    }
    return true;
}

bool pumpStream(std::shared_ptr<StreamSession> s, QTcpSocket& sock)
{
    // 领取视频消费权（每会话一份；第二个观众应自开会话）
    if (s->attached.exchange(true))
        return false;
    while (!s->aborted.load()) {
        OutFrame f;
        {
            std::unique_lock lk(s->qMu);
            s->qCv.wait_for(lk, std::chrono::seconds(1),
                            [&] { return !s->queue.empty() || s->aborted.load(); });
            if (s->queue.empty()) {
                if (s->aborted.load())
                    break;
                if (peerGone(sock))
                    break;
                continue;
            }
            f = std::move(s->queue.front());
            s->queue.pop_front();
        }
        if (f.type == kPktEnd)
            break;
        if (!writeAll(sock, f.encode()))
            break;
    }
    registry().remove(s->id);
    s->stop();
    return true;
}

// ===================== 路由处理 =====================

mw::HttpResponse jsonErr(int code, const QString& msg)
{
    const auto e = mw::jsonError(code, msg);
    return mw::HttpResponse::json(e.first, e.second);
}

mw::HttpResponse jsonOk(const QJsonObject& o)
{
    return mw::HttpResponse::json(200, QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString resolveSerial(const mw::HttpRequest& req, AppState* state)
{
    QString serial = req.query(QStringLiteral("serial")).trimmed();
    if (!serial.isEmpty())
        return serial;
    serial = state->currentSerial();
    if (!serial.isEmpty())
        return serial;
    const auto dev = state->adb.findOnlineDevice();
    if (dev.ok())
        return dev.value().serial;
    return QString();
}

// POST /api/stream/open?serial=&max_size=&bit_rate=&fps=
mw::HttpResponse streamOpen(AppState* state, const mw::HttpRequest& req)
{
    registry().reapStale();
    const QString serial = resolveSerial(req, state);
    if (serial.isEmpty())
        return jsonErr(503, QStringLiteral("Android 未运行"));
    const QString jar = mw::scrcpy::findServerJar();
    if (jar.isEmpty())
        return jsonErr(500, QStringLiteral("找不到 scrcpy-server（运行时未就绪）"));

    quint32 maxSize = req.query(QStringLiteral("max_size")).toUInt();
    if (maxSize == 0 || maxSize > 2560)
        maxSize = 1280;
    quint32 bitRate = req.query(QStringLiteral("bit_rate")).toUInt();
    if (bitRate < 250000 || bitRate > 20000000)
        bitRate = 4000000;
    quint32 fps = req.query(QStringLiteral("fps")).toUInt();
    if (fps == 0 || fps > 60)
        fps = 30;

    mw::scrcpy::ServerOptions opts;
    opts.video = true;
    opts.audio = false;
    opts.control = true;
    opts.videoCodec = mw::scrcpy::VideoCodecId::H264;
    opts.maxSize = maxSize;
    opts.videoBitRate = bitRate;
    opts.maxFps = float(fps);
    opts.scid = mw::scrcpy::ServerOptions::randomScid();
    opts.stayAwake = true;

    mw::scrcpy::ScrcpyClient client(state->adb, serial, jar);
    client.options = opts;
    auto sessionR = std::move(client).start();
    if (!sessionR)
        return jsonErr(500, QStringLiteral("投屏会话启动失败: %1").arg(sessionR.error()));

    auto s = std::make_shared<StreamSession>();
    s->serial = serial;
    {
        std::lock_guard lk(registry().mu);
        s->id = registry().nextId++;
    }
    auto parts = sessionR.value().split();
    s->control = std::move(parts.control);
    s->guard = std::move(parts.guard);
    // 视频流交喂包线程（立即开始消费，队列缓冲；尺寸在首批 meta 后可用）
    const auto codecId = parts.video.codec();
    s->codec = QString::fromLatin1(mw::scrcpy::videoCodecName(codecId));
    std::thread(feederLoop, s, std::move(parts.video)).detach();

    // 等首个 meta（编码器冷启可达数秒），拿不到也给 0 由 meta 帧后续通知
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 8000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline && !s->aborted.load()) {
        if (s->width.load() > 0)
            break;
        if (s->streamEnded.load())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // 编码器启动即死（老镜像 OMX 编码器不可用等）：一个包都没产出就结束——
    // 明确报错而不是把死流交给客户端
    if (s->streamEnded.load() && s->width.load() == 0) {
        const QString detail = s->streamError;
        s->stop();
        return jsonErr(500, QStringLiteral("设备未产出视频流（该镜像可能不支持投屏编码）%1")
                                .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
    }
    registry().add(s);
    MW_LOG("stream", QStringLiteral("会话 %1 开启：%2 %3x%4 %5kbps@%6")
                         .arg(s->id)
                         .arg(serial)
                         .arg(s->width.load())
                         .arg(s->height.load())
                         .arg(bitRate / 1000)
                         .arg(fps));
    return jsonOk({ { QStringLiteral("id"), double(s->id) },
                    { QStringLiteral("serial"), serial },
                    { QStringLiteral("codec"), s->codec },
                    { QStringLiteral("width"), double(s->width.load()) },
                    { QStringLiteral("height"), double(s->height.load()) } });
}

// GET /api/stream/<id>/video —— 长连接推流
mw::HttpResponse streamVideo(quint64 id)
{
    auto s = registry().get(id);
    if (!s)
        return jsonErr(404, QStringLiteral("会话不存在"));
    if (s->attached.load())
        return jsonErr(409, QStringLiteral("该会话的视频已被领取（每会话一个观众）"));
    mw::HttpResponse r;
    r.code = 200;
    r.contentType = QByteArrayLiteral("application/octet-stream");
    r.extraHeaders.insert("Cache-Control", "no-store");
    r.extraHeaders.insert("X-Accel-Buffering", "no"); // 反向代理禁缓冲
    r.streamer = [s](QTcpSocket& sock) { return pumpStream(s, sock); };
    return r;
}

// POST /api/stream/<id>/touch {action, x, y}
mw::HttpResponse streamTouch(quint64 id, const QByteArray& body)
{
    auto s = registry().get(id);
    if (!s)
        return jsonErr(404, QStringLiteral("会话不存在"));
    const QJsonObject v = QJsonDocument::fromJson(body).object();
    const int action = v.value(QStringLiteral("action")).toInt(-1);
    const qint32 x = qint32(v.value(QStringLiteral("x")).toDouble());
    const qint32 y = qint32(v.value(QStringLiteral("y")).toDouble());
    if (action < 0 || action > 2)
        return jsonErr(400, QStringLiteral("action 须为 0(down)/1(up)/2(move)"));
    const quint16 w = quint16(s->width.load());
    const quint16 h = quint16(s->height.load());
    if (w == 0 || h == 0)
        return jsonErr(409, QStringLiteral("画面尺寸尚未就绪"));
    // 与独立窗口注入语义逐字节一致（window.cpp mousePress/Move/Release）
    const bool isUp = action == 1;
    QString err;
    if (!s->sendControl(mw::scrcpy::ControlMessage::injectTouch(
                            quint8(action), mw::scrcpy::control::kPointerIdMouse, x, y, w, h,
                            isUp ? 0.0f : 1.0f, 1, isUp ? 0 : 1),
                        &err))
        return jsonErr(500, err);
    return jsonOk({ { QStringLiteral("ok"), true } });
}

// POST /api/stream/<id>/key {keycode, action: "tap"|"down"|"up"}
mw::HttpResponse streamKey(quint64 id, const QByteArray& body)
{
    auto s = registry().get(id);
    if (!s)
        return jsonErr(404, QStringLiteral("会话不存在"));
    const QJsonObject v = QJsonDocument::fromJson(body).object();
    const qint32 keycode = qint32(v.value(QStringLiteral("keycode")).toDouble(-1));
    const QString action = v.value(QStringLiteral("action")).toString(QStringLiteral("tap"));
    if (keycode < 0)
        return jsonErr(400, QStringLiteral("缺少 keycode"));
    QString err;
    const auto send = [&](mw::scrcpy::KeyAction a) {
        return s->sendControl(
            mw::scrcpy::ControlMessage::injectKeycode(a, keycode, 0, 0), &err);
    };
    if (action == QStringLiteral("down")) {
        if (!send(mw::scrcpy::KeyAction::Down))
            return jsonErr(500, err);
    } else if (action == QStringLiteral("up")) {
        if (!send(mw::scrcpy::KeyAction::Up))
            return jsonErr(500, err);
    } else {
        if (!send(mw::scrcpy::KeyAction::Down) || !send(mw::scrcpy::KeyAction::Up))
            return jsonErr(500, err);
    }
    return jsonOk({ { QStringLiteral("ok"), true } });
}

// POST /api/stream/<id>/text {text}
mw::HttpResponse streamText(quint64 id, const QByteArray& body)
{
    auto s = registry().get(id);
    if (!s)
        return jsonErr(404, QStringLiteral("会话不存在"));
    const QJsonObject v = QJsonDocument::fromJson(body).object();
    const QString text = v.value(QStringLiteral("text")).toString();
    if (text.isEmpty() || text.size() > mw::scrcpy::control::kInjectTextMax)
        return jsonErr(400, QStringLiteral("text 为空或超长"));
    QString err;
    if (!s->sendControl(mw::scrcpy::ControlMessage::injectText(text), &err))
        return jsonErr(500, err);
    return jsonOk({ { QStringLiteral("ok"), true } });
}

// POST /api/stream/<id>/scroll {x, y, hs, vs}
mw::HttpResponse streamScroll(quint64 id, const QByteArray& body)
{
    auto s = registry().get(id);
    if (!s)
        return jsonErr(404, QStringLiteral("会话不存在"));
    const QJsonObject v = QJsonDocument::fromJson(body).object();
    const qint32 x = qint32(v.value(QStringLiteral("x")).toDouble());
    const qint32 y = qint32(v.value(QStringLiteral("y")).toDouble());
    const float hs = float(v.value(QStringLiteral("hs")).toDouble());
    const float vs = float(v.value(QStringLiteral("vs")).toDouble());
    const quint16 w = quint16(s->width.load());
    const quint16 h = quint16(s->height.load());
    if (w == 0 || h == 0)
        return jsonErr(409, QStringLiteral("画面尺寸尚未就绪"));
    QString err;
    if (!s->sendControl(mw::scrcpy::ControlMessage::injectScroll(x, y, w, h, hs, vs, 0), &err))
        return jsonErr(500, err);
    return jsonOk({ { QStringLiteral("ok"), true } });
}

// POST /api/stream/<id>/reset —— 解码错误自愈：重发 config+关键帧
mw::HttpResponse streamReset(quint64 id)
{
    auto s = registry().get(id);
    if (!s)
        return jsonErr(404, QStringLiteral("会话不存在"));
    QString err;
    if (!s->sendControl(mw::scrcpy::ControlMessage::resetVideo(), &err))
        return jsonErr(500, err);
    return jsonOk({ { QStringLiteral("ok"), true } });
}

// POST /api/stream/<id>/close
mw::HttpResponse streamClose(quint64 id)
{
    auto s = registry().get(id);
    if (s) {
        registry().remove(id);
        s->stop();
    }
    return jsonOk({ { QStringLiteral("ok"), true } });
}

} // namespace

bool streamDispatch(const mw::HttpRequest& req, AppState* state, const QString& pathOnly,
                    mw::HttpResponse* out)
{
    if (req.method == QStringLiteral("POST") && pathOnly == QStringLiteral("/api/stream/open")) {
        *out = streamOpen(state, req);
        return true;
    }
    // /api/stream/<id>/<action>
    static const QString prefix = QStringLiteral("/api/stream/");
    if (!pathOnly.startsWith(prefix))
        return false;
    const QString rest = pathOnly.mid(prefix.size());
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return false;
    bool ok = false;
    const quint64 id = rest.left(slash).toULongLong(&ok);
    if (!ok || id == 0)
        return false;
    const QString action = rest.mid(slash + 1);
    if (req.method == QStringLiteral("GET") && action == QStringLiteral("video"))
        *out = streamVideo(id);
    else if (req.method == QStringLiteral("POST") && action == QStringLiteral("touch"))
        *out = streamTouch(id, req.body);
    else if (req.method == QStringLiteral("POST") && action == QStringLiteral("key"))
        *out = streamKey(id, req.body);
    else if (req.method == QStringLiteral("POST") && action == QStringLiteral("text"))
        *out = streamText(id, req.body);
    else if (req.method == QStringLiteral("POST") && action == QStringLiteral("scroll"))
        *out = streamScroll(id, req.body);
    else if (req.method == QStringLiteral("POST") && action == QStringLiteral("reset"))
        *out = streamReset(id);
    else if (req.method == QStringLiteral("POST") && action == QStringLiteral("close"))
        *out = streamClose(id);
    else
        return false;
    return true;
}

} // namespace mw::daemon
