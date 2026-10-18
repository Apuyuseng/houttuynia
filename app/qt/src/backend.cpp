#include "backend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTranslator>
#include <QUrl>
#include <QUrlQuery>
#include <QQmlEngine>

static QString baseUrl()
{
    const auto env = qEnvironmentVariable("MWINE_PORT");
    return QStringLiteral("http://127.0.0.1:%1").arg(env.isEmpty() ? QStringLiteral("7788") : env);
}

// JSON 便捷取值
static bool jBool(const QJsonObject &o, const char *k, bool d = false)
{
    const auto v = o.value(QLatin1String(k));
    return v.isBool() ? v.toBool() : d;
}
static QString jStr(const QJsonObject &o, const char *k)
{
    return o.value(QLatin1String(k)).toString();
}
static double jNum(const QJsonObject &o, const char *k, double d = 0)
{
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toDouble() : d;
}

static QString encPkg(const QString &pkg)
{
    return QString::fromUtf8(QUrl::toPercentEncoding(pkg));
}

Backend::Backend(QObject *parent)
    : QObject(parent)
{
    // 语言：默认中文（源文即中文，不装翻译器）；持久化于 QSettings
    const QString saved = QSettings().value(QStringLiteral("ui/language")).toString();
    if (!saved.isEmpty())
        m_language = saved;
    // 深/浅色跟随系统
    if (auto *app = qGuiApp) {
        if (auto *hints = app->styleHints()) {
            m_darkMode = hints->colorScheme() == Qt::ColorScheme::Dark;
            connect(hints, &QStyleHints::colorSchemeChanged, this, [this, hints]() {
                const bool dark = hints->colorScheme() == Qt::ColorScheme::Dark;
                if (dark != m_darkMode) {
                    m_darkMode = dark;
                    emit darkModeChanged();
                }
            });
        }
    }

    QSettings settings;
    m_keepApk = settings.value(QStringLiteral("store/keepApk"), false).toBool();

    // 任务/性能对象预填默认结构：避免 QML 在首次轮询到达前读到 undefined
    const auto taskDefaults = [] {
        return QJsonObject{{QStringLiteral("running"), false},
                           {QStringLiteral("done"), false},
                           {QStringLiteral("error"), QString()},
                           {QStringLiteral("result"), QString()},
                           {QStringLiteral("package"), QString()},
                           {QStringLiteral("label"), QString()},
                           {QStringLiteral("version"), QString()},
                           {QStringLiteral("phase"), QString()},
                           {QStringLiteral("progress"), 0},
                           {QStringLiteral("progress_total"), 0},
                           {QStringLiteral("lines"), QJsonArray()}};
    };
    m_imageTask = taskDefaults();
    m_runtimeTask = taskDefaults();
    m_storeTask = taskDefaults();
    m_installTask = taskDefaults();
    m_perf = QJsonObject{{QStringLiteral("running"), false},
                         {QStringLiteral("cpu_pct"), 0},
                         {QStringLiteral("uptime_s"), 0},
                         {QStringLiteral("mem"), QJsonObject{{QStringLiteral("total_mb"), 0},
                                                             {QStringLiteral("available_mb"), 0}}},
                         {QStringLiteral("host"), QJsonObject{{QStringLiteral("qemu_cpu_pct"), 0},
                                                              {QStringLiteral("qemu_mem_mb"), 0}}}};
    m_captureStatus = QJsonObject{{QStringLiteral("running"), false},
                                   {QStringLiteral("db_path"), QString()},
                                   {QStringLiteral("flows_live"), 0},
                                   {QStringLiteral("flows_stored"), 0}};

    connect(&m_slowTimer, &QTimer::timeout, this, &Backend::pollStatus);
    m_slowTimer.start(2000);

    connect(&m_lazyTimer, &QTimer::timeout, this, [this]() {
        refreshImages();
        refreshKeys();
        refreshSnapshots();
        refreshDisk();
    });
    m_lazyTimer.start(6000);
    // 首屏数据兜底：启动 4s/8s 各再拉一轮（首张截图/首屏轮询前数据就位）
    QTimer::singleShot(4000, this, [this]() {
        refreshImages();
        refreshSnapshots();
        refreshDisk();
    });
    QTimer::singleShot(8000, this, [this]() { refreshImages(); });

    connect(&m_taskTimer, &QTimer::timeout, this, &Backend::pollTasks);
    connect(&m_installTimer, &QTimer::timeout, this, &Backend::pollInstallTask);
    connect(&m_perfTimer, &QTimer::timeout, this, &Backend::pollPerf);
    connect(&m_captureTimer, &QTimer::timeout, this, &Backend::pollCapture);
    connect(&m_deviceTimer, &QTimer::timeout, this, [this]() {
        // 轮询仅设备列表（mDNS 发现有独立子进程开销，不在周期轮询里跑）
        if (m_devicesActive)
            fetch(QStringLiteral("/api/devices"), [this](QJsonObject obj, int code) {
                if (code == 200) {
                    m_deviceList = obj.value(QStringLiteral("devices")).toArray();
                    emit devicesChanged();
                }
            });
    });
    connect(&m_termTimer, &QTimer::timeout, this, &Backend::pollTerminal);

    pollStatus();
    refreshImages();
    refreshKeys();
    refreshSnapshots();
    refreshDisk();
    refreshStoreSources();
}

Backend::~Backend()
{
    if (m_keywatcher) {
        m_keywatcher->terminate();
        m_keywatcher->kill();
    }
    if (m_daemon) {
        m_daemon->terminate();
        m_daemon->kill();
    }
}

// ================= HTTP 基础 =================

void Backend::fetch(const QString &path, std::function<void(QJsonObject, int)> cb)
{
    send(QStringLiteral("GET"), path, {}, std::move(cb));
}

void Backend::fetch(const QString &path, std::function<void(QJsonObject, int)> cb, int timeoutMs)
{
    send(QStringLiteral("GET"), path, {}, std::move(cb), timeoutMs);
}

void Backend::send(const QString &method, const QString &path, const QByteArray &body,
                   std::function<void(QJsonObject, int)> cb)
{
    send(method, path, body, std::move(cb), 120000);
}

void Backend::send(const QString &method, const QString &path, const QByteArray &body,
                   std::function<void(QJsonObject, int)> cb, int timeoutMs)
{
    QNetworkRequest req(QUrl(baseUrl() + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(timeoutMs); // 默认 120s（商店索引类长请求）
    auto *reply = m_nam.sendCustomRequest(req, method.toUtf8(), body);
    connect(reply, &QNetworkReply::finished, this, [reply, cb, path, this]() {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 0 && path == QStringLiteral("/api/status"))
            maybeSpawnDaemon(); // 网络层失败仅对探活路径触发自拉起
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        cb(doc.object(), code);
        reply->deleteLater();
    });
}

void Backend::post(const QString &path, const QJsonObject &body,
                   std::function<void(QJsonObject, int)> cb)
{
    send(QStringLiteral("POST"), path, QJsonDocument(body).toJson(QJsonDocument::Compact),
         std::move(cb));
}

void Backend::postEmpty(const QString &path, std::function<void(QJsonObject, int)> cb)
{
    send(QStringLiteral("POST"), path, QByteArray{}, std::move(cb));
}

void Backend::del(const QString &path, std::function<void(QJsonObject, int)> cb)
{
    send(QStringLiteral("DELETE"), path, {}, std::move(cb));
}

// ================= 轮询 =================

void Backend::pollStatus()
{
    ++m_tick;

    fetch(QStringLiteral("/api/status"), [this](QJsonObject obj, int code) {
        // 守护在线判定带 15 拍（约 15s）宽限：下载/安装大 APK 时 daemon 的 adb 被
        // 长命令独占（adb push / install 期间 /api/status 的 adb 探活会连续超时），
        // 单次/几次超时不代表守护死了。此前 1~3 次超时就 maybeSpawnDaemon()，会用
        // REPLACE=1 把正在跑任务的旧守护顶替掉——任务完成态(done)随之丢失、「下载完
        // 不自动安装」、安装中断。连续 15 拍不可达才认定真离线并重启。
        const bool reachable = code == 200;
        if (reachable)
            m_offlineTicks = 0;
        else
            ++m_offlineTicks;
        const bool online = m_offlineTicks < 15;
        if (online != m_daemonOnline) {
            m_daemonOnline = online;
            emit statusChanged();
        }
        if (!online) {
            maybeSpawnDaemon();
            if (m_vmRunning) { // 守护掉线，保守清运行态
                m_vmRunning = false;
                emit statusChanged();
            }
            return;
        }
        if (!reachable)
            return;  // 宽限期内：沿用上一拍状态，不刷新
        const auto vm = obj.value(QStringLiteral("vm")).toObject();
        const bool running = jBool(vm, "running");
        const auto serial = jStr(vm, "serial");
        const auto err = jStr(obj, "vm_error");
        if (running) {
            m_bootTicks = 0;
            if (m_vmBooting) { // 启动完成：清除"启动中"（原先只在 150s 超时清 → 永远启动中）
                m_vmBooting = false;
                emit statusChanged();
            }
        } else if (m_vmBooting && m_bootTicks++ > 75) { // 150s 冷启上限
            m_vmBooting = false;
            emit statusChanged();
        }
        if (running != m_vmRunning || serial != m_vmSerial || err != m_vmError) {
            m_vmRunning = running;
            m_vmSerial = serial;
            m_vmError = err;
            emit statusChanged();
        }

        // 待装队列：开机成功即装
        if (running && !m_pendingInstalls.isEmpty()) {
            auto job = m_pendingInstalls.takeFirst();
            installApk(job.path, job.deleteOnSuccess);
        }
    });

    fetch(QStringLiteral("/api/runtime/status"), [this](QJsonObject obj, int) {
        const bool present = jBool(obj, "present");
        const bool sdk = jBool(obj, "sdk_available");
        // 首次响应前 runtimePresent 仍是默认 false——必须区分"未知"与"确认缺失"，
        // 否则启动早期（daemon 刚拉起）首启引导会误弹（本机明明已装）
        const bool wasKnown = m_runtimeKnown;
        m_runtimeKnown = true;
        if (present != m_runtimePresent || sdk != m_sdkAvailable || !wasKnown) {
            m_runtimePresent = present;
            m_sdkAvailable = sdk;
            emit runtimeChanged();
        }
    });

    // 应用列表只在运行中有意义（无设备时该接口会空转 adb）
    if (m_vmRunning && m_tick % 2 == 0)
        refreshApps();

    if (m_config.isEmpty())
        fetch(QStringLiteral("/api/config"), [this](QJsonObject obj, int) {
            if (!obj.isEmpty()) {
                m_config = obj;
                emit configChanged();
            }
        });
}

void Backend::pollTasks()
{
    // 注意：回调是异步的，不能在栈上聚合 running 状态——用成员累计 + 延迟判定
    m_anyTaskSeen = false;

    fetch(QStringLiteral("/api/image/task"), [this](QJsonObject obj, int) {
        const bool changed = obj != m_imageTask;
        m_imageTask = obj;
        if (jBool(obj, "running"))
            m_anyTaskSeen = true;
        if (changed)
            emit imageTaskChanged();
        if (changed && jBool(obj, "done")) {
            refreshImages();
            // 二次刷新：daemon 安装完成与目录扫描可见之间可能有一拍时差
            QTimer::singleShot(1200, this, [this] { refreshImages(); });
        }
    });

    fetch(QStringLiteral("/api/runtime/task"), [this](QJsonObject obj, int) {
        const bool changed = obj != m_runtimeTask;
        m_runtimeTask = obj;
        if (jBool(obj, "running"))
            m_anyTaskSeen = true;
        if (changed)
            emit runtimeTaskChanged();
        if (changed && jBool(obj, "done")) {
            emit toast(QStringLiteral("✓ 运行时安装完成"), QStringLiteral("ok"));
            fetch(QStringLiteral("/api/runtime/status"), [this](QJsonObject r2, int) {
                m_runtimePresent = jBool(r2, "present");
                emit runtimeChanged();
            });
        }
    });

    fetch(QStringLiteral("/api/store/task"), [this](QJsonObject obj, int) {
        const bool changed = obj != m_storeTask;
        m_storeTask = obj;
        if (jBool(obj, "running"))
            m_anyTaskSeen = true;
        if (changed)
            emit storeTaskChanged();
        // 下载完成 → 自动安装（一次性触发）
        if (jBool(obj, "done") && !jStr(obj, "result").isEmpty() && !m_storeInstalled) {
            m_storeInstalled = true;
            const QString apk = jStr(obj, "result");
            const QString label = jStr(obj, "label");
            emit toast(QStringLiteral("下载完成，正在安装 %1…").arg(label), QStringLiteral("ok"));
            installApk(apk, !m_keepApk);
        }
        // 失败只报一次（m_storeInstalled 兼作「任务已消费」标记）
        if (!jBool(obj, "running") && !jStr(obj, "error").isEmpty() && !m_storeInstalled) {
            m_storeInstalled = true;
            emit toast(QStringLiteral("下载失败：%1").arg(jStr(obj, "error")),
                       QStringLiteral("error"));
        }
    });

    // 回调落定后再判定是否停表（全部结束再轮询两拍拿终态）
    QTimer::singleShot(600, this, [this]() {
        if (m_anyTaskSeen) {
            m_taskIdleTicks = 0;
        } else if (++m_taskIdleTicks > 2) {
            m_taskTimer.stop();
            m_taskIdleTicks = 0;
        }
    });
}

void Backend::pollPerf()
{
    fetch(QStringLiteral("/api/perf"), [this](QJsonObject obj, int) {
        if (obj.isEmpty())
            return;
        m_perf = obj;
        if (jBool(obj, "running")) {
            QVariantMap sample;
            sample.insert(QStringLiteral("cpu"), jNum(obj, "cpu_pct"));
            const auto mem = obj.value(QStringLiteral("mem")).toObject();
            const double total = jNum(mem, "total_mb");
            const double avail = jNum(mem, "available_mb");
            sample.insert(QStringLiteral("memPct"),
                          total > 0 ? (total - avail) * 100.0 / total : 0);
            sample.insert(QStringLiteral("memUsedMB"), total - avail);
            sample.insert(QStringLiteral("memTotalMB"), total);
            m_perfHistory.append(sample);
            while (m_perfHistory.size() > 90) // 3 分钟历史
                m_perfHistory.removeFirst();
        }
        emit perfChanged();
    });
}

void Backend::pollCapture()
{
    fetch(QStringLiteral("/api/capture/status"), [this](QJsonObject obj, int) {
        if (obj != m_captureStatus) {
            m_captureStatus = obj;
            emit captureChanged();
        }
    });
    if (m_captureFlowsQueried && m_tick % 2 == 0)
        captureQueryFlows(m_lastCaptureQ, m_lastCaptureHost, m_lastCapturePkg);
}

void Backend::pollTerminal()
{
    if (m_termSessionId < 0 || m_termReadInFlight)
        return;
    m_termReadInFlight = true;
    m_termSessionReadReqCursor = m_termCursor;
    fetch(QStringLiteral("/api/term/%1/read?cursor=%2").arg(m_termSessionId).arg(m_termCursor),
          [this](QJsonObject obj, int) {
              m_termReadInFlight = false;
              m_termCursor = static_cast<quint64>(jNum(obj, "cursor"));
              const auto data = jStr(obj, "data");
              if (!data.isEmpty()) {
                  m_terminalRaw += data;
                  // raw 上限（防无限增长）；裁剪后同步平移定稿偏移
                  if (m_terminalRaw.size() > 600000) {
                      const qsizetype drop = m_terminalRaw.size() - 400000;
                      m_terminalRaw.remove(0, drop);
                      m_termStableBytes = qMax<qsizetype>(0, m_termStableBytes - drop);
                  }
                  // 定稿推进：把新到的完整行（到最后一个 \n）一次性渲染并入 stable
                  const qsizetype lastNL = m_terminalRaw.lastIndexOf(QLatin1Char('\n'));
                  if (lastNL + 1 > m_termStableBytes) {
                      bool cleared = false;
                      const QString seg = renderSegment(m_terminalRaw, m_termStableBytes,
                                                        lastNL + 1, &cleared);
                      if (cleared)
                          m_termStable = seg; // ESC[2J：定稿区作废，仅保留清屏后内容
                      else
                          m_termStable += seg;
                      m_termStableBytes = lastNL + 1;
                      if (m_termStable.size() > 120000) // 定稿上限（防文档无限增长）
                          m_termStable.remove(0, m_termStable.size() - 100000);
                  }
                  // 不稳定尾段（最后未换行行）每轮重渲（\r 覆盖/退格在此生效）
                  bool tailCleared = false;
                  const QString tail = renderSegment(m_terminalRaw, m_termStableBytes,
                                                     m_terminalRaw.size(), &tailCleared);
                  m_terminalOutput = tailCleared ? tail : (m_termStable + tail);
                  if (m_termInitEchoLeft > 0)
                      suppressInitEcho();
                  emit terminalOutputChanged();
              }
              if (!jBool(obj, "alive", true)) {
                  m_terminalConnected = false;
                  m_termTimer.stop();
                  emit terminalChanged();
              }
          }, 5000); // 终端读 5s 超时：防止一次悬挂请求停摆整个回显轮询
}

// 段渲染：对 raw[from,to) 执行与全量版相同的状态机（行首=段起点），
// 供增量渲染复用；cleared 置位表示段内发生 ESC[2J 清屏。
// 段起点必须位于 \n 之后（行边界），状态机才能无上下文重启。
QString Backend::renderSegment(const QString& raw, qsizetype from, qsizetype to, bool* cleared)
{
    QString out;
    out.reserve(to - from);
    int pos = 0;       // 段内光标（覆盖写位置）
    int lineStart = 0; // 段内行首
    qsizetype i = from;
    auto putChar = [&](const QChar& c) {
        if (pos < out.size())
            out[pos] = c;
        else
            out += c;
        ++pos;
    };
    while (i < to) {
        const QChar c = raw.at(i);
        if (c == QLatin1Char('\r')) {
            if (i + 1 < to && raw.at(i + 1) == QLatin1Char('\n')) {
                ++i;
                continue;
            }
            pos = lineStart;
            ++i;
            continue;
        }
        if (c == QLatin1Char('\n')) {
            out += QLatin1Char('\n'); // 不截断：\r\r\n 模式下截断会抹掉已输入行
            pos = out.size();
            lineStart = pos;
            ++i;
            continue;
        }
        if (c == QLatin1Char('\b')) {
            pos = qMax(lineStart, pos - 1);
            ++i;
            continue;
        }
        if (c == u'\x1b') {
            if (i + 1 < to && raw.at(i + 1) == QLatin1Char('[')) {
                qsizetype j = i + 2;
                while (j < to && !(raw.at(j).unicode() >= 0x40 && raw.at(j).unicode() <= 0x7E))
                    ++j;
                if (j >= to) {
                    break; // 悬挂序列：留给下一段（不稳定尾会重渲）
                }
                const QString params = raw.mid(i + 2, j - (i + 2));
                const QChar fin = raw.at(j);
                if (fin == QLatin1Char('K') && (params.isEmpty() || params == QLatin1String("0")))
                    out.truncate(pos); // 清除行尾残留（\r 覆盖后的旧尾巴）
                else if (fin == QLatin1Char('J') && params == QLatin1String("2")) {
                    out.clear(); // 清屏：段内从头开始
                    pos = 0;
                    lineStart = 0;
                    if (cleared)
                        *cleared = true;
                }
                i = j + 1;
                continue;
            }
            if (i + 1 < to && raw.at(i + 1) == QLatin1Char(']')) {
                qsizetype j = i + 2;
                while (j < to && raw.at(j) != QLatin1Char('\x07'))
                    ++j;
                i = qMin(j + 1, to);
                continue;
            }
            i += 2;
            continue;
        }
        if (c.unicode() < 0x20) {
            ++i;
            continue;
        }
        putChar(c);
        ++i;
    }
    return out;
}

// 终端流渲染：ANSI 剥离 + \b 退格 + \r 行内覆盖（进度条友好）。
// 逐块处理原始字节流（保留在 m_terminalRaw），输出为干净的多行文本。
QString Backend::renderTerminalStream(const QString& raw)
{
    QString out;
    out.reserve(raw.size());
    int pos = 0;       // 行内光标（覆盖写位置）
    int lineStart = 0; // 当前行起点
    const int n = raw.size();
    int i = 0;
    auto putChar = [&](const QChar& c) {
        if (pos < out.size())
            out[pos] = c;
        else
            out += c;
        ++pos;
    };
    while (i < n) {
        const QChar c = raw.at(i);
        if (c == QLatin1Char('\r')) {
            if (i + 1 < n && raw.at(i + 1) == QLatin1Char('\n')) {
                ++i; // \r\n → 按 \n 处理
                continue;
            }
            pos = lineStart; // 回行首：后续字符覆盖旧行
            ++i;
            continue;
        }
        if (c == QLatin1Char('\n')) {
            // 注意：\n 不做截断——mksh 回显模式是 "\r\r\n"（\r 先退行首再换行），
            // 若在 \n 时截断光标后内容，会把整条已输入命令行抹掉
            out += QLatin1Char('\n');
            pos = out.size();
            lineStart = pos;
            ++i;
            continue;
        }
        if (c == QLatin1Char('\b')) {
            pos = qMax(lineStart, pos - 1);
            ++i;
            continue;
        }
        if (c == u'\x1b') {
            // CSI: ESC [ <params> <最终字节@-~>。实现最小语义集：
            //  K（无参/0 = 清除到行尾，进度条收尾用）；其余（着色/光标移动）剥离
            if (i + 1 < n && raw.at(i + 1) == QLatin1Char('[')) {
                int j = i + 2;
                while (j < n && !(raw.at(j).unicode() >= 0x40 && raw.at(j).unicode() <= 0x7E))
                    ++j;
                if (j >= n) {
                    i = n; // 尾部悬挂序列整段挂起（下次续）
                    continue;
                }
                const QString params = raw.mid(i + 2, j - (i + 2));
                const QChar fin = raw.at(j);
                if (fin == QLatin1Char('K') && (params.isEmpty() || params == QLatin1String("0")))
                    out.truncate(pos); // 清除行尾残留（\r 覆盖后的旧尾巴）
                else if (fin == QLatin1Char('J') && params == QLatin1String("2")) {
                    out.clear(); // 清屏
                    pos = 0;
                    lineStart = 0;
                }
                i = j + 1;
                continue;
            }
            // OSC: ESC ] ... BEL/ST（窗口标题等）
            if (i + 1 < n && raw.at(i + 1) == QLatin1Char(']')) {
                int j = i + 2;
                while (j < n && raw.at(j) != QLatin1Char('\x07')
                       && !(raw.at(j) == QLatin1Char('\x1b') && j + 1 < n
                            && raw.at(j + 1) == QLatin1Char('\\')))
                    ++j;
                i = (j < n) ? j + 1 : n;
                continue;
            }
            i += 2; // 其他两字节转义
            continue;
        }
        if (c.unicode() < 0x20) { // BEL 等控制字符丢弃
            ++i;
            continue;
        }
        putChar(c);
        ++i;
    }
    out.truncate(pos); // 光标后残留（被覆盖的旧尾巴）不显示
    return out;
}

// 会话初始化 stty 的回显抑制：裸行与提示符跟随两种形态，最多剥 2 行
// （实测 PTY 时序会把同一命令回显成「提示符前 + 提示符后」两处）；未匹配到则
// 留待下轮（初始化回显可能晚到）。
void Backend::suppressInitEcho()
{
    static const QRegularExpression re(
        QStringLiteral("(?m)^(?:\$ )?stty rows 40 cols 120\r?\n"));
    int removed = 0;
    while (removed < 2) {
        auto m = re.match(m_terminalOutput);
        if (!m.hasMatch())
            break;
        m_terminalOutput.remove(m.capturedStart(), m.capturedLength());
        ++removed;
    }
    if (removed > 0)
        m_termInitEchoLeft = 0;
}

void Backend::terminalClear()
{
    m_terminalRaw.clear();
    m_terminalOutput.clear();
    m_termStable.clear();
    m_termStableBytes = 0;
    emit terminalOutputChanged();
}

// ================= 子集刷新 =================

void Backend::refreshApps()
{
    fetch(QStringLiteral("/api/apps"), [this](QJsonObject obj, int code) {
        // guest 半死（快照恢复后 adbd 失联）时接口 500：置错误态给 UI 明示，
        // 而不是静默维持旧列表（用户看到"运行中却不刷新"无从理解）
        if (code != 200) {
            if (!m_appsError) {
                m_appsError = true;
                emit appsChanged();
            }
            return;
        }
        const auto arr = obj.value(QStringLiteral("apps")).toArray();
        const auto wp = jStr(obj, "wallpaper");
        const bool dark = jBool(obj, "vm_dark");
        const bool changed = arr != m_apps || wp != m_appWallpaper || dark != m_vmDark;
        if (changed || m_appsError) {
            m_appsError = false;
            m_apps = arr;
            m_appWallpaper = wp;
            m_vmDark = dark;
            emit appsChanged();
        }
    });
}

void Backend::refreshImages()
{
    fetch(QStringLiteral("/api/image/list"), [this](QJsonObject obj, int) {
        const auto arr = obj.value(QStringLiteral("images")).toArray();
        if (arr != m_images) {
            m_images = arr;
            emit imagesChanged();
        }
    });
}

void Backend::refreshKeys()
{
    fetch(QStringLiteral("/api/keys/list"), [this](QJsonObject obj, int) {
        const auto arr = obj.value(QStringLiteral("profiles")).toArray();
        if (arr != m_keyProfiles) {
            m_keyProfiles = arr;
            emit keysChanged();
        }
    });
    if (m_keyTemplates.isEmpty()) {
        fetch(QStringLiteral("/api/keys/templates"), [this](QJsonObject obj, int) {
            const auto arr = obj.value(QStringLiteral("templates")).toArray();
            if (!arr.isEmpty()) {
                m_keyTemplates = arr;
                emit keysChanged();
            }
        });
    }
}

void Backend::refreshSnapshots()
{
    fetch(QStringLiteral("/api/snapshots"), [this](QJsonObject obj, int) {
        const auto arr = obj.value(QStringLiteral("snapshots")).toArray();
        if (arr != m_snapshots) {
            m_snapshots = arr;
            emit snapshotsChanged();
        }
    });
}

void Backend::refreshDisk()
{
    fetch(QStringLiteral("/api/vm/disk"), [this](QJsonObject obj, int) {
        const int gb = static_cast<int>(jNum(obj, "gb"));
        if (gb != m_diskGB && gb > 0) {
            m_diskGB = gb;
            emit diskChanged();
        }
    });
}

void Backend::refreshStoreSources()
{
    fetch(QStringLiteral("/api/store/sources"), [this](QJsonObject obj, int) {
        const auto arr = obj.value(QStringLiteral("sources")).toArray();
        if (arr != m_storeSources) {
            m_storeSources = arr;
            emit storeChanged();
        }
    });
}

void Backend::refreshForeground()
{
    fetch(QStringLiteral("/api/foreground"), [this](QJsonObject obj, int) {
        const auto pkg = jStr(obj, "package");
        if (pkg != m_foregroundPkg) {
            m_foregroundPkg = pkg;
            emit foregroundChanged();
        }
    });
}

// ================= VM 控制 =================

void Backend::startVm()
{
    if (!m_vmBooting) {
        m_vmBooting = true;
        m_bootTicks = 0;
        emit statusChanged();
    }
    postEmpty(QStringLiteral("/api/vm/start"), [this](QJsonObject obj, int code) {
        // 202 = 后台启动已受理（daemon 对长任务的标准应答），非失败
        if (code != 200 && code != 202)
            emit toast(jStr(obj, "error").isEmpty() ? QStringLiteral("启动失败")
                                                    : jStr(obj, "error"),
                       QStringLiteral("error"));
    });
}

void Backend::stopVm()
{
    postEmpty(QStringLiteral("/api/vm/stop"), [this](QJsonObject obj, int code) {
        if (code == 200)
            emit toast(QStringLiteral("正在存档关机…"), QStringLiteral("ok"));
        else
            emit toast(jStr(obj, "error"), QStringLiteral("error"));
    });
}

// 显示 emulator 原生窗口（概览页「显示」按钮）：可见模式直接激活；
// 隐藏模式下守护进程会存档重启为窗口模式（5-10s，独立窗口自动收起）
void Backend::showVmWindow()
{
    postEmpty(QStringLiteral("/api/vm/show-window"), [this](QJsonObject obj, int code) {
        if (code == 202) {
            emit toast(QStringLiteral("正在切换为窗口模式（快照恢复约 5-10 秒）…"),
                       QStringLiteral("ok"));
        } else if (code == 200) {
            if (obj.value(QStringLiteral("switching")).toBool())
                emit toast(QStringLiteral("正在切换为窗口模式…"), QStringLiteral("ok"));
        } else {
            const QString err = jStr(obj, "error");
            emit toast(err.isEmpty() ? QStringLiteral("显示失败") : err,
                       QStringLiteral("error"));
        }
    });
}

void Backend::screenshot()
{
    postEmpty(QStringLiteral("/api/screenshot"), [this](QJsonObject obj, int code) {
        if (code == 200) {
            emit toast(QStringLiteral("截图已保存到桌面"), QStringLiteral("ok"));
            revealPath(jStr(obj, "path"));
        } else {
            emit toast(jStr(obj, "error").isEmpty() ? QStringLiteral("截图失败：Android 未运行")
                                                    : jStr(obj, "error"),
                       QStringLiteral("error"));
        }
    });
}

void Backend::identityReset()
{
    postEmpty(QStringLiteral("/api/identity/reset"), [this](QJsonObject obj, int code) {
        if (code == 200 && jBool(obj, "ok"))
            emit toast(QStringLiteral("✓ 设备身份已重置（android_id / 广告 ID 已换新）"),
                       QStringLiteral("ok"));
        else
            emit toast(QStringLiteral("重置失败"), QStringLiteral("error"));
    });
}

void Backend::vmReset()
{
    postEmpty(QStringLiteral("/api/vm/reset"), [this](QJsonObject obj, int code) {
        if (code == 200)
            emit toast(QStringLiteral("✓ 已重置，下次启动为全新系统"), QStringLiteral("ok"));
        else
            emit toast(jStr(obj, "error"), QStringLiteral("error"));
    });
}

void Backend::resizeDisk(int gb)
{
    post(QStringLiteral("/api/vm/disk"),
         QJsonObject{{QStringLiteral("size_gb"), gb},
                     {QStringLiteral("stop_vm"), true}},
         [this, gb](QJsonObject obj, int code) {
             if (code == 200) {
                 m_diskGB = gb;
                 emit diskChanged();
                 emit toast(QStringLiteral("✓ 磁盘已调整为 %1 GB（数据已清空，下次启动生效）").arg(gb),
                            QStringLiteral("ok"));
             } else if (code == 409 && jBool(obj, "needs_confirm")) {
                 // 运行中且未确认：QML 弹确认后带 stop_vm 重新提交（本次已带，一般不会到这）
                 emit confirmationRequested(QStringLiteral("resize:%1").arg(gb),
                                            jStr(obj, "error"));
             } else {
                 emit toast(jStr(obj, "error"), QStringLiteral("error"));
             }
         });
}

void Backend::confirmAction(const QString &action)
{
    if (action.startsWith(QStringLiteral("resize:"))) {
        const int gb = action.mid(7).toInt();
        if (gb > 0)
            resizeDisk(gb);
    } else if (action == QStringLiteral("use-image") && !m_pendingUseImage.isEmpty()) {
        const QString path = m_pendingUseImage;
        m_pendingUseImage.clear();
        postImageUse(path, true);
    }
    emit confirmed(action);
}

// ================= 应用 =================

void Backend::launchApp(const QString &pkg)
{
    post(QStringLiteral("/api/apps/%1/run").arg(encPkg(pkg)), QJsonObject{},
         [this](QJsonObject obj, int code) {
             if (code != 200)
                 emit toast(QStringLiteral("启动失败：%1").arg(jStr(obj, "error")),
                            QStringLiteral("error"));
         });
}

void Backend::setAppMode(const QString &pkg, const QString &mode)
{
    post(QStringLiteral("/api/apps/%1/mode").arg(pkg), QJsonObject{{QStringLiteral("mode"), mode}},
         [this, pkg, mode](QJsonObject, int) {
             for (int i = 0; i < m_apps.size(); ++i) {
                 auto obj = m_apps[i].toObject();
                 if (jStr(obj, "package") == pkg) {
                     obj[QStringLiteral("mode")] = mode;
                     m_apps[i] = obj;
                     break;
                 }
             }
             emit appsChanged();
             emit toast(mode == QStringLiteral("window")
                            ? QStringLiteral("已切换为独立窗口模式")
                            : QStringLiteral("已切换为模拟器内打开"),
                        QStringLiteral("ok"));
         });
}

// ================= CLI 进程集成 =================

QString Backend::findProgram(const QString &name) const
{
    const QStringList cands = [&] {
        const QString appDir = QCoreApplication::applicationDirPath();
        QStringList l;
        if (!qEnvironmentVariable("MWINE_BIN_DIR").isEmpty())
            l << QDir(qEnvironmentVariable("MWINE_BIN_DIR")).filePath(name);
        // 安装态：bundle Contents/MacOS（Unix）或安装目录（Win/Linux）
        l << QDir(appDir).filePath(name);
#ifdef Q_OS_MACOS
        // build/ 产物直接运行时 appDir=…/app/qt/build → 仓库 cpp/build/{modules/cli,daemon}
        l << QDir(appDir).filePath(QStringLiteral("../../../cpp/build/modules/cli/") + name);
        l << QDir(appDir).filePath(QStringLiteral("../../../cpp/build/daemon/") + name);
#endif
        return l;
    }();
    for (const auto &c : cands) {
        if (QFileInfo::exists(c) && QFileInfo(c).isExecutable())
            return c;
    }
    return QStandardPaths::findExecutable(name); // 兜底：PATH
}

QString Backend::cliPath()
{
    return findProgram(QStringLiteral("mwine"));
}

void Backend::runCli(const QStringList &args, std::function<void(bool, const QString &)> cb)
{
    const QString exe = cliPath();
    if (exe.isEmpty()) {
        emit toast(QStringLiteral("找不到 mwine 命令行工具"), QStringLiteral("error"));
        cb(false, QString());
        return;
    }
    auto *p = new QProcess(this);
    p->setProgram(exe);
    p->setArguments(args);
    beginBusy();
    connect(p, &QProcess::errorOccurred, this, [this, p, cb](QProcess::ProcessError) {
        endBusy();
        emit toast(QStringLiteral("执行 %1 失败").arg(p->program()), QStringLiteral("error"));
        cb(false, QString());
        p->deleteLater();
    });
    connect(p, &QProcess::finished, this, [this, p, cb](int exitCode, QProcess::ExitStatus) {
        endBusy();
        const QString errTail = QString::fromUtf8(p->readAllStandardError())
                                    .section(QLatin1Char('\n'), -2, -2)
                                    .trimmed();
        cb(exitCode == 0, errTail);
        p->deleteLater();
    });
    p->start();
}

// 重试上次失败的安装（安装包仍在时直接重发；已删除则提示重新选择）
void Backend::retryInstall()
{
    if (m_installPath.isEmpty() || !QFileInfo::exists(m_installPath)) {
        emit toast(QStringLiteral("安装包已不在原位置，请重新选择或拖入"), QStringLiteral("error"));
        return;
    }
    installApk(m_installPath, m_installDelete);
}

void Backend::installApk(const QString &path, bool deleteOnSuccess)
{
    const QString local = path.startsWith(QStringLiteral("file:"))
                              ? QUrl(path).toLocalFile()
                              : path;
    if (!QFileInfo::exists(local)) {
        emit toast(QStringLiteral("文件不存在：%1").arg(local), QStringLiteral("error"));
        return;
    }
    // 未开机：先启动，等 running 后再装（pollStatus 队列驱动）
    if (!m_vmRunning) {
        m_pendingInstalls.append({local, deleteOnSuccess, 0});
        startVm();
        emit toast(QStringLiteral("正在启动 Android，随后自动安装…"), QStringLiteral("ok"));
        return;
    }
    // daemon 安装任务（字节级进度 → 应用库安装卡）；完成后由 CLI 生成桌面壳
    m_installPath = local;
    m_installDelete = deleteOnSuccess;
    post(QStringLiteral("/api/apps/install"),
         QJsonObject{{QStringLiteral("path"), local}},
         [this](QJsonObject, int code) {
             if (code != 200 && code != 202) {
                 emit toast(QStringLiteral("安装启动失败"), QStringLiteral("error"));
                 return;
             }
             m_installActive = true;
             if (!m_installTimer.isActive())
                 m_installTimer.start();
             emit installTaskChanged();
         });
}

void Backend::installApkTo(const QString &path, const QString &serial)
{
    const QString local = path.startsWith(QStringLiteral("file://"))
                              ? QUrl(path).toLocalFile()
                              : path;
    if (!QFileInfo::exists(local)) {
        emit toast(QStringLiteral("文件不存在：%1").arg(local), QStringLiteral("error"));
        return;
    }
    if (serial.trimmed().isEmpty()) {
        emit toast(QStringLiteral("缺少目标设备"), QStringLiteral("error"));
        return;
    }
    m_installTargetReal = true;
    post(QStringLiteral("/api/apps/install"),
         QJsonObject{{QStringLiteral("path"), local},
                     {QStringLiteral("serial"), serial.trimmed()}},
         [this](QJsonObject, int code) {
             if (code != 200 && code != 202) {
                 m_installTargetReal = false;
                 emit toast(QStringLiteral("安装启动失败"), QStringLiteral("error"));
                 return;
             }
             m_installActive = true;
             if (!m_installTimer.isActive())
                 m_installTimer.start();
             emit installTaskChanged();
         });
}

void Backend::pollInstallTask()
{
    send(QStringLiteral("GET"), QStringLiteral("/api/apps/install/task"), {},
         [this](QJsonObject obj, int code) {
        if (code != 200)
            return;
        m_installTask = obj;
        emit installTaskChanged();
        const bool running = obj.value(QStringLiteral("running")).toBool();
        const bool done = obj.value(QStringLiteral("done")).toBool();
        if (running)
            return;
        m_installTimer.stop();
        if (!m_installActive)
            return; // 无人等待的历史任务
        m_installActive = false;
        const QString err = obj.value(QStringLiteral("error")).toString();
        if (!err.isEmpty()) {
            emit toast(QStringLiteral("安装失败：%1").arg(err), QStringLiteral("error"));
            emit installTaskChanged();
            return;
        }
        // 安装成功。真机安装：仅回执 + 刷设备（不生成桌面壳——那是 VM 应用的能力）
        if (m_installTargetReal) {
            m_installTargetReal = false;
            emit toast(QStringLiteral("✓ 已安装到真机"), QStringLiteral("ok"));
            emit installTaskChanged();
            return;
        }
        // VM 安装：生成桌面壳（本地操作，秒级）→ 刷新应用库
        const QString apk = obj.value(QStringLiteral("result")).toString();
        runCli({QStringLiteral("app"), QStringLiteral("create"), QStringLiteral("--apk"), apk,
                QStringLiteral("--no-install")},
               [this, apk](bool ok, const QString &) {
                   Q_UNUSED(ok) // 壳生成失败不影响安装成功本身
                   emit toast(QStringLiteral("✓ 安装成功"), QStringLiteral("ok"));
                   if (m_installDelete && !apk.isEmpty())
                       QFile::remove(apk);
                   refreshApps();
                   emit installTaskChanged();
               });
    });
}

void Backend::uninstallApp(const QString &pkg)
{
    runCli({QStringLiteral("apps"), QStringLiteral("uninstall"), pkg},
           [this, pkg](bool ok, const QString &tail) {
               if (ok) {
                   emit toast(QStringLiteral("✓ 已卸载 %1").arg(pkg), QStringLiteral("ok"));
                   refreshApps();
               } else {
                   emit toast(QStringLiteral("卸载失败%1").arg(tail.isEmpty() ? QString() : QStringLiteral("：") + tail),
                              QStringLiteral("error"));
               }
           });
}

// ================= 在线商店 =================

void Backend::storeSearch(const QString &q, const QString &source, const QString &mirror)
{
    if (q.trimmed().isEmpty())
        return;
    m_storeSearching = true;
    emit storeChanged();
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("q"), q);
    if (!source.isEmpty())
        params.addQueryItem(QStringLiteral("source"), source);
    if (!mirror.isEmpty())
        params.addQueryItem(QStringLiteral("mirror"), mirror);
    // 首搜 F-Droid 要拉索引（~15MB），60s 客户端超时内一般够
    fetch(QStringLiteral("/api/store/search?") + params.toString(),
          [this](QJsonObject obj, int code) {
              m_storeSearching = false;
              if (code != 200) {
                  emit toast(jStr(obj, "error").isEmpty()
                                 ? QStringLiteral("搜索失败（守护进程无响应？）")
                                 : jStr(obj, "error"),
                             QStringLiteral("error"));
                  emit storeChanged();
                  return;
              }
              m_storeResults = obj.value(QStringLiteral("apps")).toArray();
              const auto errs = obj.value(QStringLiteral("errors")).toObject();
              for (auto it = errs.begin(); it != errs.end(); ++it) {
                  if (it.key() == QLatin1String("daemon"))
                      continue;
                  emit toast(QStringLiteral("%1 源不可用").arg(it.key()), QStringLiteral("warn"));
              }
              emit storeChanged();
          });
}

void Backend::storeRecommend(const QString &source, const QString &mirror)
{
    if (m_storeRecommendLoading)
        return;
    m_storeRecommendLoading = true;
    m_storeRecommendError.clear();
    emit storeChanged();
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("source"), source.isEmpty()
                                              ? QStringLiteral("fdroid") : source);
    if (!mirror.isEmpty())
        params.addQueryItem(QStringLiteral("mirror"), mirror);
    // 首次可能要拉 F-Droid 索引（~15MB），60s 客户端超时内一般够
    fetch(QStringLiteral("/api/store/recommend?") + params.toString(),
          [this](QJsonObject obj, int code) {
              m_storeRecommendLoading = false;
              if (code != 200) {
                  m_storeRecommended = {};
                  m_storeRecommendError = jStr(obj, "error").isEmpty()
                          ? QStringLiteral("推荐加载失败（守护进程无响应？）")
                          : jStr(obj, "error");
                  emit storeChanged();
                  return;
              }
              m_storeRecommended = obj.value(QStringLiteral("apps")).toArray();
              emit storeChanged();
          });
}

void Backend::storeLoadVersions(const QJsonObject &app)
{
    m_storeApp = app;
    m_storeVersions = {};
    m_storeInstalledCode = 0;
    m_storeVersionsLoading = true;
    emit storeChanged();
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("source"), jStr(app, "source"));
    params.addQueryItem(QStringLiteral("app_id"), jStr(app, "appId"));
    params.addQueryItem(QStringLiteral("package"), jStr(app, "package"));
    fetch(QStringLiteral("/api/store/versions?") + params.toString(),
          [this](QJsonObject obj, int code) {
              m_storeVersionsLoading = false;
              if (code != 200) {
                  emit toast(jStr(obj, "error"), QStringLiteral("error"));
              } else {
                  m_storeVersions = obj.value(QStringLiteral("versions")).toArray();
                  m_storeInstalledCode = jNum(obj, "installed_code");
              }
              emit storeChanged();
          });
}

void Backend::storeDownload(const QJsonObject &app, const QJsonObject &ver)
{
    QJsonObject body{{QStringLiteral("source"), jStr(app, "source")},
                     {QStringLiteral("app_id"), jStr(app, "appId")},
                     {QStringLiteral("package"), jStr(app, "package")},
                     {QStringLiteral("name"), jStr(app, "name")}};
    if (ver.contains(QStringLiteral("code")))
        body.insert(QStringLiteral("version_code"),
                    static_cast<double>(jNum(ver, "code")));
    const auto vname = jStr(ver, "name");
    if (!vname.isEmpty())
        body.insert(QStringLiteral("version_name"), vname);
    post(QStringLiteral("/api/store/download"), body, [this](QJsonObject obj, int code) {
        if (code == 200) {
            m_storeInstalled = false;
            m_taskTimer.start(1000);
            emit toast(QStringLiteral("开始下载…"), QStringLiteral("ok"));
        } else if (code == 409) {
            emit toast(QStringLiteral("已有下载任务在进行中"), QStringLiteral("warn"));
        } else {
            emit toast(jStr(obj, "error"), QStringLiteral("error"));
        }
    });
}

void Backend::storeCancel()
{
    postEmpty(QStringLiteral("/api/store/cancel"), [](QJsonObject, int) {});
}

void Backend::setKeepApk(bool v)
{
    if (v == m_keepApk)
        return;
    m_keepApk = v;
    QSettings().setValue(QStringLiteral("store/keepApk"), v);
    emit storeChanged();
}

// ================= 独立窗口几何记忆 =================

QVariantList Backend::winGeometry(const QString &key) const
{
    const QVariant v = QSettings().value(QStringLiteral("ui/win-") + key);
    if (!v.isValid())
        return {};
    const QVariantList geo = v.toList();
    return geo.size() == 4 ? geo : QVariantList();
}

void Backend::saveWinGeometry(const QString &key, int x, int y, int w, int h)
{
    QSettings().setValue(QStringLiteral("ui/win-") + key, QVariantList{ x, y, w, h });
}

// ================= 镜像 =================

void Backend::importImage(const QString &path)
{
    const QString local = path.startsWith(QStringLiteral("file:"))
                              ? QUrl(path).toLocalFile()
                              : path;
    post(QStringLiteral("/api/image/import"),
         QJsonObject{{QStringLiteral("path"), local},
                     {QStringLiteral("name"), QStringLiteral("houttuynia")}},
         [this](QJsonObject obj, int code) {
             if (code == 200) {
                 m_taskTimer.start(1000);
                 emit toast(QStringLiteral("导入任务已提交"), QStringLiteral("ok"));
             } else {
                 emit toast(jStr(obj, "error"), QStringLiteral("error"));
             }
         });
}

void Backend::postImageUse(const QString &path, bool stopVm)
{
    // 契约（与 SwiftUI 端一致）：{"path": 绝对路径, "stop_vm": JSON bool}
    // stop_vm 必须是 bool——daemon 对字符串 "true" 会判 false，曾致确认后仍 409 死循环
    // 真正执行切换的那次（stop_vm=true，含 VM 关停重启，全程 10-60s）发
    // 切换开始/结束信号——UI 呈现现代化切换浮层而非干等
    const bool announce = stopVm;
    if (announce)
        emit imageSwitchStarted();
    post(QStringLiteral("/api/image/use"),
         QJsonObject{{QStringLiteral("path"), path},
                     {QStringLiteral("stop_vm"), stopVm}},
         [this, path, announce](QJsonObject obj, int code) {
             if (code == 409 && jBool(obj, "needs_confirm")) {
                 if (announce)
                     emit imageSwitchCancelled(); // 转确认框，浮层撤下
                 m_pendingUseImage = path; // 确认后由 confirmAction 带 stop_vm=true 重发
                 emit confirmationRequested(QStringLiteral("use-image"),
                                            jStr(obj, "error"));
             } else if (code == 200) {
                 if (announce)
                     emit imageSwitchFinished(true, QString());
                 refreshImages();
             } else {
                 if (announce)
                     emit imageSwitchFinished(false, jStr(obj, "error"));
                 emit toast(jStr(obj, "error"), QStringLiteral("error"));
             }
         });
}

// 镜像任务错误"已读"（UI 确认按钮）：清 daemon 侧任务态，轮询不再回放
void Backend::ackImageTask()
{
    postEmpty(QStringLiteral("/api/image/task/ack"), [](QJsonObject, int) {});
}

void Backend::useImage(const QString &path)
{
    postImageUse(path, false);
}

void Backend::deleteImage(const QString &path)
{
    post(QStringLiteral("/api/image/delete"), QJsonObject{{QStringLiteral("path"), path}},
         [this](QJsonObject obj, int code) {
             if (code == 200) {
                 emit toast(QStringLiteral("✓ 已删除"), QStringLiteral("ok"));
                 refreshImages();
             } else {
                 emit toast(jStr(obj, "error"), QStringLiteral("error"));
             }
         });
}

void Backend::imageCancelTask()
{
    postEmpty(QStringLiteral("/api/image/cancel"), [](QJsonObject, int) {});
}

void Backend::officialImagesLoad(const QString &filter)
{
    m_officialLoading = true;
    emit officialImagesChanged();
    QUrlQuery params;
    if (!filter.isEmpty())
        params.addQueryItem(QStringLiteral("filter"), filter);
    const QString proxy = jStr(m_config, "proxy");
    if (!proxy.isEmpty())
        params.addQueryItem(QStringLiteral("proxy"), proxy);
    fetch(QStringLiteral("/api/image/official?") + params.toString(),
          [this](QJsonObject obj, int code) {
              m_officialLoading = false;
              if (code != 200) {
                  emit toast(jStr(obj, "error"), QStringLiteral("error"));
              } else {
                  m_officialImages = obj.value(QStringLiteral("images")).toArray();
              }
              emit officialImagesChanged();
          });
}

void Backend::officialPull(const QString &target)
{
    QJsonObject body{{QStringLiteral("target"), target}};
    const QString proxy = jStr(m_config, "proxy");
    if (!proxy.isEmpty())
        body.insert(QStringLiteral("proxy"), proxy);
    post(QStringLiteral("/api/image/official-pull"), body, [this](QJsonObject obj, int code) {
        if (code == 200) {
            m_taskTimer.start(1000);
            emit toast(QStringLiteral("开始拉取官方镜像…"), QStringLiteral("ok"));
        } else {
            emit toast(jStr(obj, "error"), QStringLiteral("error"));
        }
    });
}

// ================= 运行时 =================

// 退出主程序的收尾（aboutToQuit）：GUI 自己拉起的 daemon 场景下，把 VM 存档
// 关停并同步等待完成（快照约 3–8s，上限 30s）——否则 daemon 随 GUI 被 terminate、
// qemu 成为孤儿继续跑（用户退出主程序后 VM/独立窗口还活着的根源）。
// 外部自管的 daemon（CLI 用户手动起的）不动它的 VM。
// 独立窗口（mwine-window）随 qemu 停止收 EOF 走静默收窗路径，无需逐个通知。
void Backend::shutdownForQuit()
{
    // 不区分 daemon 归属：即便连的是历史孤儿 daemon（上个 GUI 崩溃/被强退
    // 遗留的），退出桌面应用也应收摊——否则孤儿链下 VM 永远没人管
    //（实测：GUI 复用孤儿 daemon 时 m_daemon==null，若跳过则退出不停机）。
    // CLI headless 用户（手动起 daemon 不开 GUI）不受影响。
    if (!m_daemonOnline || !m_vmRunning)
        return;
    postEmpty(QStringLiteral("/api/vm/stop"), nullptr);
    // daemon 的 vm/stop 是同步慢操作：存档（上限 20s）+ emuKillWait（上限 35s）。
    // 必须等它完成再退——GUI 一退 daemon（QProcess 子进程）即被 terminate，
    // 关停中途被腰斩 qemu 就成孤儿（实测 30s 不够）。正常全程 ~10–20s。
    QEventLoop loop;
    int ticks = 0;
    QTimer timer;
    connect(&timer, &QTimer::timeout, this, [this, &loop, &ticks]() {
        if (++ticks > 25) { // 2s × 25 = 50s 上限，超时放行（异常卡死场景）
            loop.quit();
            return;
        }
        fetch(QStringLiteral("/api/status"), [this, &loop](QJsonObject obj, int code) {
            if (code == 200
                && !obj.value(QStringLiteral("vm")).toObject()
                      .value(QStringLiteral("running")).toBool())
                loop.quit();
        });
    });
    timer.start(2000);
    loop.exec();
    timer.stop();
}

// 在线下载运行时分发包（daemon 端断点续传 → 自动解压安装；进度经 runtimeTask 轮询）
void Backend::runtimeDownload()
{
    post(QStringLiteral("/api/runtime/download"), {}, [this](QJsonObject obj, int code) {
        if (code == 200) {
            m_taskTimer.start(1000);
            emit toast(QStringLiteral("开始下载运行时（约 330 MB，支持断点续传）"),
                       QStringLiteral("ok"));
        } else {
            const QString err = jStr(obj, "error");
            emit toast(err.isEmpty() ? QStringLiteral("启动下载失败") : err,
                       QStringLiteral("error"));
        }
    });
}

void Backend::runtimeCancel()
{
    postEmpty(QStringLiteral("/api/runtime/cancel"), [this](QJsonObject, int) {
        emit toast(QStringLiteral("已取消（已下载部分保留，可断点续传）"), QStringLiteral("warn"));
    });
}

void Backend::importRuntime(const QString &path)
{
    const QString local = path.startsWith(QStringLiteral("file:"))
                              ? QUrl(path).toLocalFile()
                              : path;
    post(QStringLiteral("/api/runtime/import"), QJsonObject{{QStringLiteral("path"), local}},
         [this](QJsonObject obj, int code) {
             if (code == 200) {
                 m_taskTimer.start(1000);
                 emit toast(QStringLiteral("运行时安装中…"), QStringLiteral("ok"));
             } else {
                 emit toast(jStr(obj, "error").isEmpty() ? QStringLiteral("导入失败")
                                                        : jStr(obj, "error"),
                            QStringLiteral("error"));
             }
         });
}

// ================= 配置 =================

void Backend::saveConfig(const QJsonObject &cfg)
{
    post(QStringLiteral("/api/config"), cfg, [this, cfg](QJsonObject obj, int code) {
        if (code == 200) {
            m_config = cfg; // 采纳本地值（daemon 会补 avd，下轮轮询校准）
            emit configChanged();
            emit toast(QStringLiteral("✓ 已保存（部分选项重启后生效；屏幕配置运行中即时切换）"),
                       QStringLiteral("ok"));
            refreshDisk();
        } else {
            emit toast(QStringLiteral("保存失败：%1").arg(jStr(obj, "error")),
                       QStringLiteral("error"));
        }
    });
}

// ================= 快照 =================

void Backend::snapshotSave(const QString &name)
{
    post(QStringLiteral("/api/snapshot/save"), QJsonObject{{QStringLiteral("name"), name}},
         [this](QJsonObject obj, int code) {
             if (code == 200) {
                 emit toast(QStringLiteral("✓ 存档已保存"), QStringLiteral("ok"));
                 refreshSnapshots();
             } else {
                 emit toast(jStr(obj, "error"), QStringLiteral("error"));
             }
         });
}

void Backend::snapshotLoad(const QString &name)
{
    post(QStringLiteral("/api/snapshot/load"), QJsonObject{{QStringLiteral("name"), name}},
         [this, name](QJsonObject obj, int code) {
             if (code == 200)
                 emit toast(QStringLiteral("✓ 已恢复到 %1").arg(name), QStringLiteral("ok"));
             else
                 emit toast(jStr(obj, "error"), QStringLiteral("error"));
         });
}

void Backend::snapshotDelete(const QString &name)
{
    post(QStringLiteral("/api/snapshot/delete"), QJsonObject{{QStringLiteral("name"), name}},
         [this](QJsonObject obj, int code) {
             if (code == 200) {
                 emit toast(QStringLiteral("✓ 已删除存档"), QStringLiteral("ok"));
                 refreshSnapshots();
             } else {
                 emit toast(jStr(obj, "error"), QStringLiteral("error"));
             }
         });
}

// ================= 终端 =================

void Backend::openTerminal()
{
    // 互斥：ensureConnected 有多个触发源（状态轮询/页切换/信号），POST 异步返回前
    // 只允许一次 open——否则连开多会话，每会话初始化 stty 都回显一遍（会话风暴）
    if (m_termOpening)
        return;
    m_termOpening = true;
    postEmpty(QStringLiteral("/api/term/open"), [this](QJsonObject obj, int code) {
        m_termOpening = false;
        if (code == 200) {
            m_termSessionId = static_cast<qint64>(jNum(obj, "id"));
            m_termCursor = 0;
            m_terminalRaw.clear();
            m_terminalOutput.clear();
            m_termStable.clear();
            m_termStableBytes = 0;
            m_termInitEchoLeft = 1; // 抑制一次初始化命令的回显
            m_terminalConnected = true;
            m_termTimer.start(120);
            emit terminalOutputChanged();
            emit terminalChanged();
        } else {
            emit toast(jStr(obj, "error").isEmpty() ? QStringLiteral("无法连接终端（Android 未运行？）")
                                                    : jStr(obj, "error"),
                       QStringLiteral("error"));
        }
    });
}

void Backend::closeTerminal()
{
    if (m_termSessionId >= 0)
        post(QStringLiteral("/api/term/%1/close").arg(m_termSessionId), QJsonObject{},
             [](QJsonObject, int) {});
    m_termSessionId = -1;
    m_terminalConnected = false;
    m_termTimer.stop();
    emit terminalChanged();
}

void Backend::terminalWrite(const QString &data)
{
    if (m_termSessionId < 0)
        return;
    post(QStringLiteral("/api/term/%1/write").arg(m_termSessionId),
         QJsonObject{{QStringLiteral("data"), data}}, [](QJsonObject, int) {});
    // 低延迟：敲键后不等轮询周期，立即拉一次回显
    if (!m_termReadInFlight)
        QTimer::singleShot(0, this, [this]() { pollTerminal(); });
}

void Backend::terminalInterrupt()
{
    terminalWrite(QStringLiteral("\u0003"));
}

// ================= 按键映射 =================

void Backend::keysApplyTemplate(const QString &pkg, const QString &templateId)
{
    // 模板档案已在 keys/templates 响应里，直接落盘
    for (const auto &tv : m_keyTemplates) {
        const auto t = tv.toObject();
        if (jStr(t, "id") != templateId)
            continue;
        send(QStringLiteral("PUT"), QStringLiteral("/api/keys/") + encPkg(pkg),
             QJsonDocument(t.value(QStringLiteral("profile")).toObject())
                 .toJson(QJsonDocument::Compact),
             [this, pkg](QJsonObject obj, int code) {
                 if (code == 200) {
                     emit toast(QStringLiteral("✓ 已为 %1 套用模板，可微调后生效").arg(pkg),
                                QStringLiteral("ok"));
                     refreshKeys();
                 } else {
                     emit toast(jStr(obj, "error"), QStringLiteral("error"));
                 }
             });
        return;
    }
    emit toast(QStringLiteral("模板不存在"), QStringLiteral("error"));
}

void Backend::keysDelete(const QString &pkg)
{
    del(QStringLiteral("/api/keys/") + encPkg(pkg), [this](QJsonObject obj, int code) {
        if (code == 200) {
            emit toast(QStringLiteral("✓ 已删除键位档案"), QStringLiteral("ok"));
            refreshKeys();
        } else {
            emit toast(jStr(obj, "error"), QStringLiteral("error"));
        }
    });
}

void Backend::keysShot(const QString &pkg)
{
    postEmpty(QStringLiteral("/api/keys/%1/shot").arg(encPkg(pkg)),
              [this](QJsonObject obj, int code) {
                  if (code == 200) {
                      m_keyShotPath = jStr(obj, "path");
                      emit keysChanged();
                  } else {
                      emit toast(jStr(obj, "error"), QStringLiteral("error"));
                  }
              });
}

void Backend::keywatchStart(const QString &pkg)
{
    keywatchStop();
    const QString exe = cliPath();
    if (exe.isEmpty()) {
        emit toast(QStringLiteral("找不到 mwine 命令行工具"), QStringLiteral("error"));
        return;
    }
    auto *p = new QProcess(this);
    p->setProgram(exe);
    p->setArguments({QStringLiteral("keys"), QStringLiteral("watch"), pkg});
    beginBusy();
    connect(p, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        endBusy();
        m_keywatcherRunning = false;
        emit keysChanged();
        emit toast(QStringLiteral("按键映射进程启动失败"), QStringLiteral("error"));
    });
    connect(p, &QProcess::started, this, [this, pkg]() {
        endBusy();
        m_keywatcherRunning = true;
        m_keywatcherPkg = pkg;
        emit keysChanged();
        emit toast(QStringLiteral("按键映射已启用（%1）· 点一下模拟器窗口激活，` 键开关").arg(pkg),
                   QStringLiteral("ok"));
    });
    connect(p, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        m_keywatcherRunning = false;
        emit keysChanged();
    });
    p->start();
    m_keywatcher = p;
}

void Backend::keywatchStop()
{
    if (!m_keywatcher)
        return;
    m_keywatcher->disconnect(this);
    m_keywatcher->terminate();
    m_keywatcher->deleteLater();
    m_keywatcher = nullptr;
    m_keywatcherRunning = false;
    emit keysChanged();
    emit toast(QStringLiteral("按键映射已停用"), QStringLiteral("ok"));
}

// ================= 流量分析 =================

void Backend::captureStart()
{
    postEmpty(QStringLiteral("/api/capture/start"), [this](QJsonObject obj, int code) {
        if (code == 200)
            emit toast(QStringLiteral("✓ 流量采集已启动（eBPF 明文旁挂）"), QStringLiteral("ok"));
        else
            emit toast(jStr(obj, "error"), QStringLiteral("error"));
    });
}

void Backend::captureStop()
{
    postEmpty(QStringLiteral("/api/capture/stop"), [this](QJsonObject obj, int code) {
        if (code == 200)
            emit toast(QStringLiteral("已停止采集"), QStringLiteral("ok"));
        else
            emit toast(jStr(obj, "error"), QStringLiteral("error"));
    });
}

void Backend::captureQueryFlows(const QString &q, const QString &host, const QString &package)
{
    m_lastCaptureQ = q;
    m_lastCaptureHost = host;
    m_lastCapturePkg = package;
    m_captureFlowsQueried = true;
    QUrlQuery params;
    if (!q.isEmpty())
        params.addQueryItem(QStringLiteral("q"), q);
    if (!host.isEmpty())
        params.addQueryItem(QStringLiteral("host"), host);
    if (!package.isEmpty())
        params.addQueryItem(QStringLiteral("package"), package);
    params.addQueryItem(QStringLiteral("total"), QStringLiteral("1"));
    fetch(QStringLiteral("/api/capture/flows?") + params.toString(),
          [this](QJsonObject obj, int code) {
              if (code != 200)
                  return;
              m_captureFlows = obj.value(QStringLiteral("flows")).toArray();
              const auto total = obj.value(QStringLiteral("total"));
              m_captureTotal = total.isDouble() ? static_cast<int>(total.toDouble())
                                                : static_cast<int>(m_captureFlows.size());
              emit captureChanged();
          });
}

void Backend::captureLoadDetail(qint64 id)
{
    m_captureDetail = {};
    emit captureChanged();
    fetch(QStringLiteral("/api/capture/flows/%1").arg(id), [this](QJsonObject obj, int) {
        m_captureDetail = obj;
        emit captureChanged();
    });
}

void Backend::captureClear()
{
    postEmpty(QStringLiteral("/api/capture/clear"), [this](QJsonObject obj, int code) {
        if (code == 200) {
            m_captureFlows = {};
            m_captureTotal = 0;
            m_captureDetail = {};
            emit captureChanged();
            emit toast(QStringLiteral("✓ 已清空采集数据"), QStringLiteral("ok"));
        }
    });
}

// ================= 页面级开关 =================

void Backend::setPerfActive(bool active)
{
    if (active && !m_perfTimer.isActive()) {
        m_perfTimer.start(2000);
        pollPerf();
    } else if (!active) {
        m_perfTimer.stop();
    }
}

void Backend::setCaptureActive(bool active)
{
    m_captureActive = active;
    if (active && !m_captureTimer.isActive()) {
        m_captureTimer.start(2000);
        fetch(QStringLiteral("/api/capture/status"), [this](QJsonObject obj, int) {
            m_captureStatus = obj;
            emit captureChanged();
        });
    } else if (!active) {
        m_captureTimer.stop();
    }
}

// ================= 真机连接 =================

void Backend::setDevicesActive(bool active)
{
    m_devicesActive = active;
    if (active && !m_deviceTimer.isActive()) {
        m_deviceTimer.start(3000);
        deviceRefresh();
    } else if (!active) {
        m_deviceTimer.stop();
    }
}

void Backend::refreshDiscovered()
{
    fetch(QStringLiteral("/api/devices/discovered"), [this](QJsonObject obj, int code) {
        if (code == 200) {
            m_deviceDiscovered = obj.value(QStringLiteral("services")).toArray();
            emit devicesChanged();
        }
    });
}

void Backend::deviceRefresh()
{
    if (!m_daemonOnline)
        return;
    // 轮询只拉设备列表；mDNS 发现（每次 2 个 adb 子进程）改为按需拉取，
    // 3s 轮询里跑会持续骚扰 adb server / 模拟器 netsim 环境
    fetch(QStringLiteral("/api/devices"), [this](QJsonObject obj, int code) {
        if (code == 200) {
            m_deviceList = obj.value(QStringLiteral("devices")).toArray();
            emit devicesChanged();
        }
    });
    refreshDiscovered();
    refreshHostPair();
}

// 出码配对会话轮询：状态推进时发信号；终态发 toast（弹窗可能已关）
void Backend::refreshHostPair()
{
    fetch(QStringLiteral("/api/devices/hostpair"), [this](QJsonObject obj, int code) {
        if (code != 200)
            return;
        const QString prev = m_deviceHostPair.value(QStringLiteral("state")).toString();
        const bool wasActive = prev == QStringLiteral("waiting")
            || prev == QStringLiteral("pairing") || prev == QStringLiteral("connecting");
        if (obj == m_deviceHostPair)
            return;
        m_deviceHostPair = obj;
        emit deviceHostPairChanged();
        const QString state = obj.value(QStringLiteral("state")).toString();
        if (wasActive) {
            if (state == QStringLiteral("done")) {
                const QJsonObject res = obj.value(QStringLiteral("result")).toObject();
                if (obj.value(QStringLiteral("connected")).toBool()) {
                    const QJsonObject d = res.value(QStringLiteral("device")).toObject();
                    emit toast(QStringLiteral("✓ 配对并连接成功：%1（Android %2）")
                                   .arg(d.value(QStringLiteral("model")).toString(),
                                        d.value(QStringLiteral("android")).toString()),
                               QStringLiteral("ok"));
                } else {
                    emit toast(QStringLiteral("✓ 配对成功，但连接端口未就绪；稍后在列表手动连接"),
                               QStringLiteral("warn"));
                }
            } else if (state == QStringLiteral("failed")) {
                emit toast(obj.value(QStringLiteral("message")).toString(),
                           QStringLiteral("error"));
            } else if (state == QStringLiteral("cancelled")) {
                emit toast(QStringLiteral("已取消出码配对"), QStringLiteral("warn"));
            }
            deviceRefresh();
        }
    });
}

// 配对结果统一反馈（qr / code 两路共用）
void Backend::devicePairFinish(const QJsonObject &obj, int code)
{
    m_devicePairing = false;
    emit devicesChanged();
    if (code == 200) {
        const bool connected = obj.value(QStringLiteral("connected")).toBool();
        if (connected) {
            const QJsonObject d = obj.value(QStringLiteral("device")).toObject();
            emit toast(QStringLiteral("✓ 配对并连接成功：%1（Android %2）")
                           .arg(d.value(QStringLiteral("model")).toString(),
                                d.value(QStringLiteral("android")).toString()),
                       QStringLiteral("ok"));
        } else {
            emit toast(QStringLiteral("✓ 配对成功，但连接端口未就绪；稍后在列表手动连接"),
                       QStringLiteral("warn"));
        }
    } else {
        emit toast(obj.value(QStringLiteral("error")).toString(), QStringLiteral("error"));
    }
    deviceRefresh();
}

void Backend::devicePairQr(const QString &qr)
{
    if (m_devicePairing)
        return;
    m_devicePairing = true;
    emit devicesChanged();
    emit toast(QStringLiteral("… 已识别二维码，配对中"), QStringLiteral("ok"));
    post(QStringLiteral("/api/devices/pair"), QJsonObject{ { QStringLiteral("qr"), qr } },
         [this](QJsonObject obj, int code) { devicePairFinish(obj, code); });
}

void Backend::devicePairCode(const QString &code, const QString &addr, const QString &service)
{
    if (m_devicePairing)
        return;
    m_devicePairing = true;
    emit devicesChanged();
    emit toast(QStringLiteral("… 配对中"), QStringLiteral("ok"));
    QJsonObject body{ { QStringLiteral("code"), code.trimmed() } };
    if (!addr.trimmed().isEmpty())
        body[QStringLiteral("addr")] = addr.trimmed();
    if (!service.isEmpty())
        body[QStringLiteral("service")] = service;
    post(QStringLiteral("/api/devices/pair"), body,
         [this](QJsonObject obj, int code2) { devicePairFinish(obj, code2); });
}

// 出码配对（Android Studio 同款）：daemon 生成二维码会话并后台等手机扫码；
// 会话经 deviceHostPair 属性推给 QML（qr 载荷 + rows 矩阵 + 状态）
void Backend::deviceHostPairBegin()
{
    post(QStringLiteral("/api/devices/hostpair/begin"), QJsonObject{},
         [this](QJsonObject obj, int code) {
             if (code != 200) {
                 emit toast(obj.value(QStringLiteral("error")).toString(),
                            QStringLiteral("error"));
                 return;
             }
             if (obj != m_deviceHostPair) {
                 m_deviceHostPair = obj;
                 emit deviceHostPairChanged();
             }
         });
}

void Backend::deviceHostPairCancel()
{
    postEmpty(QStringLiteral("/api/devices/hostpair/cancel"),
              [this](QJsonObject, int) { refreshHostPair(); });
}

void Backend::deviceConnect(const QString &addr)
{
    post(QStringLiteral("/api/devices/connect"),
         QJsonObject{ { QStringLiteral("addr"), addr.trimmed() } },
         [this](QJsonObject obj, int code) {
             if (code == 200) {
                 emit toast(QStringLiteral("✓ 已连接 %1（%2）")
                                .arg(obj.value(QStringLiteral("serial")).toString(),
                                     obj.value(QStringLiteral("model")).toString()),
                            QStringLiteral("ok"));
             } else {
                 emit toast(obj.value(QStringLiteral("error")).toString(),
                            QStringLiteral("error"));
             }
             deviceRefresh();
         });
}

void Backend::deviceDisconnect(const QString &serial)
{
    post(QStringLiteral("/api/devices/disconnect"),
         QJsonObject{ { QStringLiteral("serial"), serial } }, [this, serial](QJsonObject obj, int code) {
             if (code == 200)
                 emit toast(QStringLiteral("✓ 已断开 %1").arg(serial), QStringLiteral("ok"));
             else
                 emit toast(obj.value(QStringLiteral("error")).toString(),
                            QStringLiteral("error"));
             deviceRefresh();
         });
}

void Backend::deviceForget(const QString &serial)
{
    post(QStringLiteral("/api/devices/forget"),
         QJsonObject{ { QStringLiteral("serial"), serial } }, [this, serial](QJsonObject obj, int code) {
             if (code == 200)
                 emit toast(QStringLiteral("✓ 已忘记 %1").arg(serial), QStringLiteral("ok"));
             else
                 emit toast(obj.value(QStringLiteral("error")).toString(),
                            QStringLiteral("error"));
             deviceRefresh();
         });
}

void Backend::deviceTcpip(const QString &serial)
{
    emit toast(QStringLiteral("… USB 转无线切换中（约 3~5 秒，期间设备会短暂掉线）"),
               QStringLiteral("ok"));
    post(QStringLiteral("/api/devices/tcpip"),
         QJsonObject{ { QStringLiteral("serial"), serial } },
         [this](QJsonObject obj, int code) {
             if (code == 200) {
                 emit toast(QStringLiteral("✓ 已转无线：%1")
                                .arg(obj.value(QStringLiteral("serial")).toString()),
                            QStringLiteral("ok"));
             } else {
                 emit toast(obj.value(QStringLiteral("error")).toString(),
                            QStringLiteral("error"));
             }
             deviceRefresh();
         });
}

void Backend::deviceMirror(const QString &serial)
{
    post(QStringLiteral("/api/devices/mirror"),
         QJsonObject{ { QStringLiteral("serial"), serial } }, [this, serial](QJsonObject obj, int code) {
             if (code == 200)
                 emit toast(QStringLiteral("✓ 投屏窗口已启动（%1）").arg(serial),
                            QStringLiteral("ok"));
             else
                 emit toast(obj.value(QStringLiteral("error")).toString(),
                            QStringLiteral("error"));
         });
}

// ================= 杂项 =================

void Backend::revealPath(const QString &path)
{
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

// ================= 国际化 =================

// qm 资源路径（qt_add_translations 内嵌：:/i18n/<code>.qm；缺失即无该语言）
void Backend::setLanguage(const QString& code)
{
    if (code == m_language)
        return;
    if (m_translator) {
        qApp->removeTranslator(m_translator);
        delete m_translator;
        m_translator = nullptr;
    }
    if (code != QStringLiteral("zh")) {
        auto* t = new QTranslator(this);
        if (t->load(QStringLiteral(":/i18n/") + code + QStringLiteral(".qm"))) {
            qApp->installTranslator(t);
            m_translator = t;
        } else {
            delete t;
        }
    }
    m_language = code;
    QSettings().setValue(QStringLiteral("ui/language"), code);
    emit languageChanged();
    // 即时生效：触发所有 qsTr 绑定重算（Qt 6.2+）
    if (m_engine)
        m_engine->retranslate();
}

QStringList Backend::availableLanguages() const
{
    QStringList out{ QStringLiteral("zh") };
    const QDir dir(QStringLiteral(":/i18n"));
    for (const QString& f : dir.entryList({ QStringLiteral("*.qm") }))
        out.append(f.chopped(3)); // 去 .qm 后缀
    out.removeDuplicates();
    return out;
}

void Backend::beginBusy()
{
    if (m_busyCount++ == 0)
        emit busyChanged();
}

void Backend::endBusy()
{
    if (m_busyCount > 0 && --m_busyCount == 0)
        emit busyChanged();
}

void Backend::maybeSpawnDaemon()
{
    const auto now = QDateTime::currentDateTime();
    if (m_lastSpawnAttempt.isValid() && m_lastSpawnAttempt.secsTo(now) < 8)
        return;
    m_lastSpawnAttempt = now;
    if (m_daemon) // 已在拉起中
        return;
    // C++ 守护（mwine-cored；图标缓存依赖同目录的 mwine-iconfetch 子进程）
    const QString exe = findProgram(QStringLiteral("mwine-cored"));
    if (exe.isEmpty())
        return;
    auto *p = new QProcess(this);
    p->setProgram(exe);
    // 升级残留自愈：旧版守护占着 7788 时，新守护替换旧实例
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MWINE_REPLACE_DAEMON"), QStringLiteral("1"));
    p->setProcessEnvironment(env);
    connect(p, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit toast(QStringLiteral("守护进程启动失败"), QStringLiteral("error"));
    });
    connect(p, &QProcess::finished, this, [this, p](int, QProcess::ExitStatus) {
        if (m_daemon == p) {
            m_daemon = nullptr;
            if (m_daemonOnline) {
                m_daemonOnline = false;
                emit statusChanged();
            }
        }
        p->deleteLater();
    });
    p->start();
    m_daemon = p;
}
