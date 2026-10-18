// mwine-window 实现：会话编排 + NV12 渲染 + 输入注入 + 剪贴板互通。
#include "window.hpp"

#include "fs.hpp"

#include <signal.h>

#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpSocket>
#include <QWheelEvent>
#include <QWindow>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

#include <cmath>
#include <sys/socket.h>
#include <cstdlib>

namespace mwine {

using namespace mw::scrcpy;

// ---- Android 按键映射（Qt::Key → AKEYCODE；scrcpy 桌面惯用集）----

namespace {

std::optional<qint32> androidKeycode(int key)
{
    using K = Qt::Key;
    switch (key) {
    case K::Key_Return:
    case K::Key_Enter: return control::AKEYCODE_ENTER;
    case K::Key_Backspace: return control::AKEYCODE_DEL;
    case K::Key_Delete: return 112;           // FORWARD_DEL
    case K::Key_Escape: return control::AKEYCODE_BACK;
    case K::Key_Tab: return 61;
    case K::Key_Backtab: return 61;
    case K::Key_Space: return 62;
    case K::Key_Home: return control::AKEYCODE_HOME;
    case K::Key_End: return 123;              // MOVE_END
    case K::Key_PageUp: return 92;
    case K::Key_PageDown: return 93;
    case K::Key_Left: return control::AKEYCODE_DPAD_LEFT;
    case K::Key_Right: return control::AKEYCODE_DPAD_RIGHT;
    case K::Key_Up: return control::AKEYCODE_DPAD_UP;
    case K::Key_Down: return control::AKEYCODE_DPAD_DOWN;
    case K::Key_Comma: return 55;
    case K::Key_Period: return 56;
    case K::Key_Minus: return 69;
    case K::Key_Equal: return 70;
    case K::Key_BracketLeft: return 71;
    case K::Key_BracketRight: return 72;
    case K::Key_Backslash: return 73;
    case K::Key_Semicolon: return 74;
    case K::Key_Apostrophe: return 75;
    case K::Key_Slash: return 76;
    case K::Key_At: return 77;
    case K::Key_Plus: return 78;
    case K::Key_QuoteLeft: return 79;
    case K::Key_CapsLock: return 115;
    case K::Key_Shift: return 59;             // SHIFT_LEFT（Right 由 nativeScanCode 细分，此处统一）
    case K::Key_Control: return 113;          // CTRL_LEFT
    case K::Key_Alt: return 57;               // ALT_LEFT
    case K::Key_Meta: return 117;             // META_LEFT
    default: break;
    }
    if (key >= K::Key_A && key <= K::Key_Z)
        return control::AKEYCODE_A + (key - K::Key_A);
    if (key >= K::Key_0 && key <= K::Key_9)
        return 7 + (key - K::Key_0);
    if (key >= K::Key_F1 && key <= K::Key_F12)
        return 131 + (key - K::Key_F1);
    return std::nullopt;
}

qint32 androidMetaState(Qt::KeyboardModifiers mods)
{
    qint32 m = 0;
    if (mods & Qt::ShiftModifier)
        m |= 0x01 | 0x40; // SHIFT_ON | SHIFT_LEFT_ON
    if (mods & Qt::ControlModifier)
        m |= control::META_CTRL_ON_COMBO;
    if (mods & Qt::AltModifier)
        m |= 0x02 | 0x10; // ALT_ON | ALT_LEFT_ON
    if (mods & Qt::MetaModifier)
        m |= 0x10000 | 0x20000; // META_ON | META_LEFT_ON
    return m;
}

// ---- emu console（qemu 物理副屏：本镜像上虚拟显示器不合成内容，唯一可靠通路）----

/// 发一条 emulator console 命令（auth + cmd + quit），返回应答文本。
static QString emuConsoleCmd(const QString& serial, const QString& cmd)
{
    const quint16 port = mw::input::consolePortOfSerial(serial);
    if (port == 0)
        return QString();
    QFile tokenFile(QDir::home().filePath(QStringLiteral(".emulator_console_auth_token")));
    const QString token =
        tokenFile.open(QIODevice::ReadOnly) ? QString::fromUtf8(tokenFile.readAll()).trimmed()
                                            : QString();
    auto conn = mw::adb::TcpConn::connect(QStringLiteral("127.0.0.1"), port, 3000);
    if (!conn.ok())
        return QString();
    QString req;
    if (!token.isEmpty())
        req += QStringLiteral("auth %1\n").arg(token);
    req += cmd + QLatin1Char('\n') + QStringLiteral("quit\n");
    if (!conn.value().writeAll(req.toUtf8()).ok())
        return QString();
    mw::adb::TcpConn& c = conn.value();
    QByteArray all;
    for (;;) {
        auto r = c.readAtMost(4096, 1500);
        if (!r.ok() || r.value().isEmpty())
            break;
        all += r.value();
    }
    return QString::fromUtf8(all);
}

/// guest 当前全部 display id（dumpsys 解析）。
static QVector<quint32> guestDisplayIds(const mw::adb::AdbClient& adb, const QString& serial)
{
    QVector<quint32> ids;
    const auto o = adb.shellBlocking(
        serial, QStringLiteral("dumpsys display 2>/dev/null | grep -oE 'mDisplayId=[0-9]+'"),
        10000);
    if (!o.ok())
        return ids;
    static const QRegularExpression re(QStringLiteral("(\\d+)$"));
    for (const QString& line : o.value().outStr().split(QLatin1Char('\n'))) {
        const auto m = re.match(line.trimmed());
        if (m.hasMatch())
            ids.append(m.captured(1).toUInt());
    }
    return ids;
}

// 真机副屏条目清理：从 overlay_display_devices 设置串里只摘掉本窗口的条目
//（保留其他真机窗口的条目；串空则删键恢复原状）
static void removeRealOverlayEntry(const mw::adb::AdbClient& adb, const QString& serial,
                                   const QString& spec)
{
    const auto g = adb.shellBlocking(
        serial, QStringLiteral("settings get global overlay_display_devices"), 8000);
    if (!g.ok() || !g.value().ok())
        return;
    QString v = g.value().outStr().trimmed();
    if (v.isEmpty() || v == QLatin1String("null"))
        return;
    QStringList entries = v.split(QLatin1Char(','), Qt::SkipEmptyParts);
    entries.removeAll(spec);
    if (entries.isEmpty()) {
        adb.shellBlocking(serial, QStringLiteral("settings delete global overlay_display_devices"),
                          8000);
    } else {
        adb.shellBlocking(
            serial,
            QStringLiteral("settings put global overlay_display_devices '%1'")
                .arg(entries.join(QLatin1Char(','))),
            8000);
    }
}

/// console 副屏槽位注册表（~/.mwine/window-displays.json：idx → 持有 pid）。
/// console `multidisplay add` 对已占 index 是静默 modify（抢走别人副屏），
/// 多窗口并发必须先占号。
static QJsonObject displaySlotTable()
{
    QFile f(QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/window-displays.json")));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

static void writeDisplaySlotTable(const QJsonObject& o)
{
    const QString p = QDir(mw::homeDir()).filePath(QStringLiteral(".mwine/window-displays.json"));
    mw::writeFile(p, QJsonDocument(o).toJson(QJsonDocument::Compact));
}

} // namespace

bool ScreenWidget::tryTakeDisplaySlot()
{
    QJsonObject tbl = displaySlotTable();
    const auto myPid = QCoreApplication::applicationPid();
    // 清理已退出的持有者
    for (auto it = tbl.begin(); it != tbl.end();) {
        const qint64 pid = it.value().toVariant().toLongLong();
        if (pid <= 0 || (pid != myPid && ::kill(pid, 0) != 0 && errno == ESRCH))
            it = tbl.erase(it);
        else
            ++it;
    }
    // 会话重建：本进程已持槽直接复用（不重复占位）
    for (auto it = tbl.begin(); it != tbl.end(); ++it) {
        if (it.value().toVariant().toLongLong() == qint64(myPid)) {
            m_consoleIdx = it.key().toInt();
            return true;
        }
    }
    for (int idx = 1; idx <= 10; ++idx) {
        const QString key = QString::number(idx);
        if (!tbl.contains(key)) {
            tbl.insert(key, double(myPid));
            writeDisplaySlotTable(tbl);
            m_consoleIdx = idx;
            return true;
        }
    }
    return false;
}

void ScreenWidget::releaseDisplaySlot()
{
    if (m_consoleIdx <= 0)
        return;
    QJsonObject tbl = displaySlotTable();
    const QString key = QString::number(m_consoleIdx);
    if (tbl.value(key).toVariant().toLongLong() == qint64(QCoreApplication::applicationPid()))
        tbl.remove(key);
    writeDisplaySlotTable(tbl);
    m_consoleIdx = 0;
}

void ScreenWidget::setStatusText(const QString& s)
{
    m_status = s;
    update();
}

// ---- HostWidget：并列布局宿主（窗口级职责全在这层）----

HostWidget::HostWidget(WindowOptions opt, QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(opt.title.isEmpty()
                       ? (opt.mirror ? QStringLiteral("mwine 镜像")
                                     : opt.package)
                       : opt.title);
    // daemon 偏好透传：无边框窗口（bar 空白处可拖动移动）。
    // 参数形态优先（open 拉 bundle 不继承 env），env 兼容裸进程拉起链
    if (opt.borderless
        || (qEnvironmentVariableIsSet("MWINE_BORDERLESS")
            && qEnvironmentVariable("MWINE_BORDERLESS") == QStringLiteral("1"))) {
        setWindowFlag(Qt::FramelessWindowHint, true);
    }
    setMinimumSize(280, 280 + BarWidget::kBarH);

    // 初始窗口：优先恢复上次几何（按 package 记忆）；无记忆时按显示器规格
    // 缩到主屏以内。高度含 bar——视频区与 display 同比例，flex resize 生效
    // 前的过渡期也不出 letterbox 黑边。
    {
        const QString geomKey = QStringLiteral("geom/") + opt.package;
        QSettings st;
        const QByteArray last = st.value(geomKey).toByteArray();
        if (!last.isEmpty() && restoreGeometry(last)) {
            // 恢复成功
        } else {
            if (!last.isEmpty())
                st.remove(geomKey);
            const QRect scr = QGuiApplication::primaryScreen()->availableGeometry();
            const qreal cap = qMin<qreal>(1.0,
                                          qMin(qreal(scr.width()) * 0.55 / opt.dispW,
                                               qreal(scr.height() - BarWidget::kBarH) * 0.75
                                                   / opt.dispH));
            resize(qMax(280, int(opt.dispW * cap)),
                   qMax(280 + BarWidget::kBarH, int(opt.dispH * cap) + BarWidget::kBarH));
        }
    }

    // 并列：视频（伸展）在上、功能条固定底部
    m_screen = new ScreenWidget(std::move(opt));
    m_bar = new BarWidget(m_screen);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_screen, 1);
    lay->addWidget(m_bar);
    m_screen->setBarWidget(m_bar);
}

void HostWidget::closeEvent(QCloseEvent* e)
{
    m_screen->handleWindowClose(e);
}

// ---- 构造 / 析构 ----

ScreenWidget::ScreenWidget(WindowOptions opt, QWidget* parent)
    : QOpenGLWidget(parent), m_opt(std::move(opt))
{
    // 并列布局下本部件是纯视频区：窗口级职责（标题/无边框/几何记忆/bar）
    // 全在 HostWidget；bar 经 setBarWidget() 注入（主题/图标联动）
    setMouseTracking(true);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    // 键盘焦点：macOS IME 只挂在 first responder（有焦点的部件）上，
    // StrongFocus = 点击即得焦点（键盘注入与宿主输入法的前提）
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(280, 280);

    // 单实例（按 package）：后启动的进程唤醒既有窗口后退出。
    // 先连接探测（removeServer 会 unlink 正在监听的 socket 文件，
    // 盲目清理会让第二个实例也 listen 成功 → 双实例并存）
    {
        // mirror 无 package——用固定名参与单实例（重复「显示」只激活既有窗口）
        const QString name = m_opt.package.isEmpty()
                ? QStringLiteral("mwine-window-mirror")
                : QStringLiteral("mwine-window-%1").arg(m_opt.package);
        {
            QLocalSocket probe;
            probe.connectToServer(name);
            if (probe.waitForConnected(300)) {
                probe.write(QStringLiteral("activate").toUtf8());
                probe.flush();
                probe.waitForBytesWritten(500);
                std::exit(0); // 既有窗口已被唤醒
            }
        }
        QLocalServer::removeServer(name); // 清理崩溃残留的 server 文件
        if (!m_instanceServer.listen(name)) {
            qWarning().noquote() << "[mwine-window] 单实例监听失败，继续运行（多开风险）";
        }
        connect(&m_instanceServer, &QLocalServer::newConnection, this, [this] {
            if (QLocalSocket* c = m_instanceServer.nextPendingConnection()) {
                c->waitForReadyRead(300);
                const QString msg = QString::fromUtf8(c->readAll()).trimmed();
                c->deleteLater();
                if (msg == QLatin1String("quit")) {
                    if (QWidget* w = window())
                        w->close(); // 请求退出（两段式收尾）
                    return;
                }
                if (msg == QLatin1String("dragtest")) {
                    // 自检：模拟 bar 空白处按下→拖动→抬起（进程内真实事件链），
                    // 窗口应位移 (60, 40)；结果打日志供外部核验
                    QTimer::singleShot(0, this, [this] {
                        QWidget* win = window();
                        const QPoint before = win ? win->frameGeometry().topLeft()
                                                  : frameGeometry().topLeft();
                        const QPoint p = m_barWidget->rect().center();
                        QMouseEvent press(QEvent::MouseButtonPress,
                                          QPointF(p), m_barWidget->mapToGlobal(p),
                                          Qt::LeftButton, Qt::LeftButton,
                                          Qt::NoModifier);
                        QApplication::sendEvent(m_barWidget, &press);
                        for (int i = 1; i <= 6; ++i) {
                            const QPoint q = p + QPoint(10 * i, 0) + QPoint(0, 8 * i);
                            QMouseEvent move(QEvent::MouseMove,
                                             QPointF(q), m_barWidget->mapToGlobal(q),
                                             Qt::LeftButton, Qt::LeftButton,
                                             Qt::NoModifier);
                            QApplication::sendEvent(m_barWidget, &move);
                        }
                        QMouseEvent release(QEvent::MouseButtonRelease,
                                            QPointF(p + QPoint(60, 48)),
                                            m_barWidget->mapToGlobal(p + QPoint(60, 48)),
                                            Qt::LeftButton, Qt::LeftButton,
                                            Qt::NoModifier);
                        QApplication::sendEvent(m_barWidget, &release);
                        const QPoint after = win ? win->frameGeometry().topLeft()
                                                 : frameGeometry().topLeft();
                        qWarning().noquote()
                            << "[dragtest] before" << before << "after" << after
                            << "delta" << (after - before)
                            << ((after != before) ? "✓ 拖拽路径工作" : "✗ 拖拽未生效");
                    });
                    return;
                }
                if (QWidget* w = window()) {
                    w->showNormal();
                    w->raise();
                    w->activateWindow();
                }
            }
        });
    }

    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(350);
    connect(&m_resizeTimer, &QTimer::timeout, this, &ScreenWidget::requestResizeDisplay);


    m_bootThread = std::thread([this] { beginSession(); });
}

void ScreenWidget::setBarWidget(BarWidget* b)
{
    m_barWidget = b;
    if (b && !m_opt.iconPath.isEmpty())
        b->setAppIcon(m_opt.iconPath);
}

ScreenWidget::~ScreenWidget()
{
    // 等 collector 完成它独占的 join（析构与其并发 join 即崩溃，见
    // stopSessionInBackground 注释；上限 30s > 25s quit 兜底）
    requestSessionStop();
    for (int i = 0; m_bgJoin.load() == 1 && i < 600; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    teardown();
}

// ---- 会话生命周期 ----

void ScreenWidget::beginSession()
{
    ServerOptions so;
    so.video = true;
    so.audio = !m_opt.noAudio;
    so.control = true;
    so.videoCodec = VideoCodecId::H264; // VideoToolbox 解码器当前仅 H.264
    // 流参数：显式指定优先；否则按目标设备类型取默认。
    // 真机镜像走 adb Wi-Fi/USB 隧道，带宽与抖动远差于本地 adbd 直连——
    // 原始分辨率（如 1220x2712@120Hz）直推必然卡顿，降规格（1600/10M/60fps）换流畅；
    // 真机窗口模式显示器尺寸由我们指定（本身就是目标分辨率），保持原始。
    const bool realDev = !m_opt.serial.startsWith(QStringLiteral("emulator-"));
    so.videoBitRate =
        m_opt.bitrate ? m_opt.bitrate : (realDev && m_opt.mirror ? 10000000u : 12000000u);
    so.maxSize = m_opt.maxSize ? m_opt.maxSize : (realDev && m_opt.mirror ? 1600u : 0u);
    if (m_opt.maxFps.has_value()) {
        so.maxFps = m_opt.maxFps;
    } else if (realDev && m_opt.mirror) {
        so.maxFps = 60.0f;
    }
    so.keepActive = true;
    so.stayAwake = true;
    so.powerOffOnClose = false;

    mw::adb::AdbClient adb;
    // window 模式：预授运行时权限——冷启动应用停在主屏权限对话框时，
    // 副屏无内容表现为黑屏（授权后应用才能进入副屏渲染）
    if (!m_opt.mirror && !m_opt.package.isEmpty()) {
        if (const auto n = adb.grantRuntimePermissions(m_opt.serial, m_opt.package);
            n.ok() && n.value() > 0)
            qWarning().noquote() << "[mwine-window] 已预授" << n.value() << "项运行时权限";
    }

    if (m_opt.mirror) {
        if (m_opt.displayId)
            so.displayId = m_opt.displayId; // 镜像指定显示器；缺省主屏
    } else {
        // qemu 物理副屏 + scrcpy display_id 镜像：本镜像（AOSP33）上
        // scrcpy new_display 的虚拟显示器不合成内容（纯色帧），
        // qemu console 副屏是物理显示路径，实测可靠。
        auto phase = [this](const QString& s) {
            QMetaObject::invokeMethod(this, [this, s] { setStatusText(s); },
                                      Qt::QueuedConnection);
        };
        phase(QStringLiteral("正在创建副屏…"));
        if (realDev) {
            // 真机：开发者选项「模拟辅助显示」（overlay_display_devices）建屏，
            // 与 qemu console 副屏同构——建屏 → display id 差集 → am start --display
            const QVector<quint32> before = guestDisplayIds(adb, m_opt.serial);
            m_realOverlaySpec = QStringLiteral("%1x%2/%3")
                                    .arg(m_opt.dispW)
                                    .arg(m_opt.dispH)
                                    .arg(m_opt.dispDpi);
            // 读改写设置串：保留其他真机窗口的条目（多窗口并存互不覆盖）
            QString curVal;
            {
                const auto g = adb.shellBlocking(
                    m_opt.serial,
                    QStringLiteral("settings get global overlay_display_devices"), 8000);
                if (g.ok() && g.value().ok()) {
                    curVal = g.value().outStr().trimmed();
                    if (curVal == QLatin1String("null"))
                        curVal.clear();
                }
            }
            const QString newVal =
                curVal.isEmpty() ? m_realOverlaySpec
                                 : curVal + QLatin1Char(',') + m_realOverlaySpec;
            const auto put =
                adb.shellBlocking(m_opt.serial,
                                  QStringLiteral("settings put global overlay_display_devices '%1'")
                                      .arg(newVal),
                                  8000);
            if (!put.ok() || !put.value().ok()) {
                m_realOverlaySpec.clear();
                QMetaObject::invokeMethod(
                    this, [this] { sessionFailed(QStringLiteral("辅助显示设置写入失败")); },
                    Qt::QueuedConnection);
                return;
            }
            phase(QStringLiteral("正在等待系统就绪…"));
            for (int i = 0; i < 30; ++i) {
                const QVector<quint32> now = guestDisplayIds(adb, m_opt.serial);
                for (quint32 id : now) {
                    if (!before.contains(id) && id != 0) {
                        m_guestDisplayId = id;
                        break;
                    }
                }
                if (m_guestDisplayId != 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (m_guestDisplayId == 0) {
                if (m_reuseDisplayId != 0) {
                    m_guestDisplayId = m_reuseDisplayId; // 会话重建：条目仍在，id 未变
                } else {
                    const QVector<quint32> now = guestDisplayIds(adb, m_opt.serial);
                    for (quint32 id : now)
                        if (id != 0 && id > m_guestDisplayId)
                            m_guestDisplayId = id;
                }
            } else {
                m_reuseDisplayId = m_guestDisplayId;
            }
            if (m_guestDisplayId == 0) {
                const QString spec = m_realOverlaySpec;
                m_realOverlaySpec.clear();
                removeRealOverlayEntry(adb, m_opt.serial, spec);
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        sessionFailed(QStringLiteral(
                            "辅助显示器未创建（设备不支持或未开启「模拟辅助显示」："
                            "开发者选项 → 模拟辅助显示）"));
                    },
                    Qt::QueuedConnection);
                return;
            }
            so.displayId = m_guestDisplayId;
        } else {
            if (!tryTakeDisplaySlot()) {
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        sessionFailed(QStringLiteral("副屏槽位已满（最多 10 个独立窗口）"));
                    },
                    Qt::QueuedConnection);
                return;
            }
            // 清掉本槽位的历史残留副屏（异常退出未 del 的）：残留屏会让差集
            // 找不到新 display，退化到"最大 id 兜底"→ 绑到别人的空屏 = 黑屏
            emuConsoleCmd(m_opt.serial,
                          QStringLiteral("multidisplay del %1").arg(m_consoleIdx));
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const QVector<quint32> before = guestDisplayIds(adb, m_opt.serial);
            // 初始建屏分辨率同样受 qemu 下限 768×768 约束（小规格 --display 直接
            // 建屏失败），按比例放大保持 aspect
            quint32 addW = m_opt.dispW, addH = m_opt.dispH;
            if (addW < 768 || addH < 768) {
                const qreal s = qMax<qreal>(768.0 / addW, 768.0 / addH);
                addW = quint32(qMin<qreal>(3840, addW * s));
                addH = quint32(qMin<qreal>(3840, addH * s));
            }
            const QString resp = emuConsoleCmd(
                m_opt.serial, QStringLiteral("multidisplay add %1 %2 %3 %4 0")
                                  .arg(m_consoleIdx)
                                  .arg(addW)
                                  .arg(addH)
                                  .arg(m_opt.dispDpi));
            if (!resp.contains(QLatin1String("OK"))) {
                releaseDisplaySlot(); // add 失败立即放号，绝不占用他人槽位
                QMetaObject::invokeMethod(
                    this,
                    [this, r = resp.trimmed()] {
                        sessionFailed(QStringLiteral("qemu 副屏创建失败: %1").arg(r));
                    },
                    Qt::QueuedConnection);
                return;
            }
            // 等 guest 侧新 display 出现（HWC 上报有延迟）
            phase(QStringLiteral("正在等待系统就绪…"));
            for (int i = 0; i < 30; ++i) {
                const QVector<quint32> now = guestDisplayIds(adb, m_opt.serial);
                for (quint32 id : now) {
                    if (!before.contains(id) && id != 0) {
                        m_guestDisplayId = id;
                        break;
                    }
                }
                if (m_guestDisplayId != 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (m_guestDisplayId == 0) {
                // 兜底：modify 既有副屏时 ids 无新增——会话重建场景优先复用原 id，
                // 其余取现有列表里最大的非零 id（单副屏场景）
                if (m_reuseDisplayId != 0) {
                    m_guestDisplayId = m_reuseDisplayId;
                } else {
                    const QVector<quint32> now = guestDisplayIds(adb, m_opt.serial);
                    for (quint32 id : now)
                        if (id != 0 && id > m_guestDisplayId)
                            m_guestDisplayId = id;
                }
            } else {
                m_reuseDisplayId = m_guestDisplayId; // 记住，供重建复用
            }
            if (m_guestDisplayId == 0) {
                releaseDisplaySlot();
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        sessionFailed(QStringLiteral("副屏已建但 guest 未上报 display id"));
                    },
                    Qt::QueuedConnection);
                return;
            }
            so.displayId = m_guestDisplayId;
        } // 模拟器 console 副屏路径结束

        // 启动应用到副屏（resolve-activity → am start --display；模拟器/真机共用）
        if (!m_opt.package.isEmpty()) {
            phase(QStringLiteral("正在启动应用…"));
            const auto comp = adb.shellBlocking(
                m_opt.serial,
                QStringLiteral(
                    "cmd package resolve-activity --brief -c android.intent.category.LAUNCHER %1 "
                    "2>/dev/null | tail -1")
                    .arg(m_opt.package),
                8000);
            if (comp.ok() && comp.value().outStr().contains(QLatin1Char('/'))) {
                const QString activity = comp.value().outStr().trimmed();
                adb.shellBlocking(
                    m_opt.serial,
                    QStringLiteral("am start --display %1 -n '%2'")
                        .arg(m_guestDisplayId)
                        .arg(activity),
                    10000);
            }
        }
        phase(QStringLiteral("正在连接视频流…"));
    }

    const QString jar = findServerJar();
    if (jar.isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                sessionFailed(QStringLiteral(
                    "scrcpy-server 未找到（~/.mwine/bin/scrcpy-server-v4.1）"));
            },
            Qt::QueuedConnection);
        return;
    }

    ScrcpyClient client{adb, m_opt.serial, jar};
    client.options = so;
    auto session = std::move(client).start();
    if (!session.ok()) {
        const QString err = session.error();
        QMetaObject::invokeMethod(this, [this, err] { sessionFailed(err); },
                                  Qt::QueuedConnection);
        return;
    }
    // 会话部件接管（m_guard/m_audio 等成员归 GUI 线程所有）在 GUI 线程执行
    QMetaObject::invokeMethod(
        this, [this, s = std::move(session.value())]() mutable { sessionUp(std::move(s)); },
        Qt::QueuedConnection);
}

void ScreenWidget::sessionUp(mw::scrcpy::Session&& s)
{
    // 排队期间用户可能已关窗（WA_DeleteOnClose 在即）：再起线程会导致
    // joinable thread 随对象析构 → std::terminate
    if (m_quitting.load() || m_tornDown.load()) {
        return;
    }
    auto parts = s.split(); // 清理职责移交 SessionGuard

    // 音频：flac → ffmpeg → CoreAudio（失败不致命，仅静音）
    if (parts.audio) {
        m_audio.emplace(mw::audio::CoreAudioBridge::withScrcpy(std::move(*parts.audio)));
        if (!m_audio->start(mw::audio::AudioConfig::stereo48k()).ok()) {
            m_audio.reset();
            qWarning().noquote()
                << "[mwine-window] 音频桥启动失败（CoreAudio）；本次会话无声音";
        }
    } else if (!m_opt.noAudio) {
        // 服务端未建立音频流：常见于 DRM 视频应用（Widevine 内容默认禁止
        // 播放采集，scrcpy 侧拿不到音轨——平台行为，非本程序故障）
        qWarning().noquote()
            << "[mwine-window] 音频流未建立（DRM 应用/系统会禁止播放采集）";
    }

    // 控制通道：writer 线程串行发送；设备消息走 dup 流独立读取
    mw::adb::TcpConn devDup;
    if (parts.control)
        devDup = parts.control->tryCloneStream();

    m_guard = std::move(parts.guard);

    if (parts.control) {
        m_writerThread = std::thread([this, ch = std::move(*parts.control)]() mutable {
            writerLoop(std::move(ch));
        });
        if (devDup.isValid())
            m_devMsgFd = devDup.fd();
        m_devMsgThread = std::thread([this, dup = std::move(devDup)]() mutable {
            deviceMsgLoop(std::move(dup));
        });
    }

    // window 模式的应用启动走 am start --display（beginSession 已做）；
    // scrcpy StartApp 消息不用于 qemu 副屏路线
    m_videoFd = parts.video.stream().fd();
    m_videoThread = std::thread([this, v = std::move(parts.video)]() mutable {
        videoLoop(std::move(v));
    });

    // 会话就绪：把副屏分辨率对齐当前窗口（首启默认 944×2048 与窗口尺寸
    // 不符 → letterbox 黑边；此前只在 resizeEvent 触发，用户不动窗口就
    // 永远黑边）。必须清掉去重缓存——初始 resizeEvent 可能已把
    // m_lastWidth 记成窗口尺寸，而 beginSession 的 add(944×2048) 在那之后
    // 才落位，去重会把这次矫正 modify 挡掉（实测副屏停 944×2048 的根因）
    if (!m_opt.mirror) {
        m_lastWidth = 0;
        m_lastHeight = 0;
        m_resizeTimer.start();
    }

    // guest 主题跟随：常驻线程轮询 ui_night_mode（白面板/暗面板切换）。
    // 与其他会话线程同生命周期——关闭收尾 join 它，杜绝悬垂 invoke。
    // 首查延迟 5s：建链期（resolve/am start/首帧）adb 连接紧张，主题
    // 查询并发插入会拖慢会话建立（页面加载变慢的放大器）
    if (!m_themeThread.joinable())
        m_themeThread = std::thread([this] {
            for (int i = 0; i < 50 && !m_quitting.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            themeLoop();
        });
}

// guest 实际输出 aspect 的闭环矫正：qemu/scrcpy 链路对请求分辨率存在
// 系统性偏差（实测请求 768×1667 时 guest 输出过 768×1748，height +5%——
// 表现为视频 letterbox 后底部黑条 / 内容"被 bar 挤占"）。
// 机制：首帧 800ms 后（flex 已生效、流稳定）比对 实际流 aspect 与
// videoArea aspect，偏差 >0.5% 时按偏差比例修正请求高度重发 modify。
// 每次 flex modify 落定后校验（偏差 >0.5% 才动），resize 后同样自动收敛。
void ScreenWidget::correctDisplayAspect()
{
    if (m_opt.mirror || m_quitting.load() || m_consoleIdx == 0)
        return;
    quint32 vw = 0, vh = 0;
    {
        std::lock_guard<std::mutex> lk(m_frame.mu);
        if (m_frame.seq == 0)
            return;
        vw = m_frame.img.width;
        vh = m_frame.img.height;
    }
    if (vw == 0 || vh == 0 || m_lastWidth == 0 || height() <= 0)
        return;
    const qreal target = qreal(width()) / qreal(height());
    const qreal actual = qreal(vw) / qreal(vh);
    if (qFuzzyIsNull(target) || qAbs(actual - target) / target < 0.005)
        return;
    // 输出偏高(actual<target) → 请求高度放大 target/actual 倍，反之缩小
    const quint16 nh = quint16(qBound<qreal>(320, qreal(m_lastHeight) * actual / target, 3840));
    if (qint32(nh) == qint32(m_lastHeight))
        return;
    qWarning().noquote() << "[mwine-window] aspect 矫正: 流" << vw << "x" << vh
                         << "aspect" << actual << "≠ videoArea" << target
                         << "→ 请求高度" << m_lastHeight << "→" << nh;
    m_lastHeight = nh;
    const int idx = m_consoleIdx;
    const QString serial = m_opt.serial;
    const quint16 nw = m_lastWidth;
    const quint32 dpi = m_opt.dispDpi;
    std::thread([idx, serial, nw, nh, dpi] {
        emuConsoleCmd(serial, QStringLiteral("multidisplay add %1 %2 %3 %4 0")
                                    .arg(idx)
                                    .arg(nw)
                                    .arg(nh)
                                    .arg(dpi));
    }).detach();
}

void ScreenWidget::themeLoop()
{
    // 首查立即可见（bar 默认暗面板，亮主题下尽快翻成白面板），之后 8s 一轮。
    // adb 单查 ~50ms 在后台线程无感；sleep 分段保证关闭时秒退。
    // 信号：`cmd uimode night`（"Night mode: yes/no/auto"）；auto 未落盘时
    // 看 ui_night_mode 数值（**2=YES 深色、1=NO 亮色——与字面直觉相反**，
    // MODE_NIGHT_NO=1），仍未定按本地时间近似（18:00–06:00 深色）
    const auto parseDark = [](const QString& modeOut, const QString& settingOut) {
        const QString m = modeOut.toLower();
        if (m.contains(QLatin1String("yes")))
            return true;
        if (m.contains(QLatin1String("no")))
            return false;
        const QString v = settingOut.simplified();
        if (v.contains(QLatin1String(" 2")))
            return true;
        if (v.contains(QLatin1String(" 1")))
            return false;
        const int h = QDateTime::currentDateTime().time().hour();
        return h >= 18 || h < 6;
    };
    mw::adb::AdbClient adb;
    while (!m_quitting.load()) {
        if (!m_opt.serial.isEmpty()) {
            // 超时 2.5s：关闭收尾 join 本线程时上限可控（两条查询 + 清理 <8s）
            const auto combined = adb.shellBlocking(
                m_opt.serial,
                QStringLiteral("cmd uimode night; settings get secure ui_night_mode"), 2500);
            const auto mode = combined;
            const auto setting = combined;
            if (mode.ok()) {
                const bool dark = parseDark(mode.value().outStr(),
                                            setting.ok() ? setting.value().outStr()
                                                         : QString());
                QMetaObject::invokeMethod(this, [this, dark] { applyBarTheme(dark); },
                                          Qt::QueuedConnection);
            }
        }
        for (int i = 0; i < 80 && !m_quitting.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void ScreenWidget::applyBarTheme(bool dark)
{
    if (m_tornDown.load() || dark == m_guestDark)
        return;
    m_guestDark = dark;
    if (m_barWidget)
        m_barWidget->applyTheme(dark);
}

void ScreenWidget::videoLoop(mw::scrcpy::VideoStream stream)
{
    qWarning().noquote() << "[mwine-window] videoLoop enter fd=" << stream.stream().fd();
    QFile rec;
    if (!m_opt.recordPath.isEmpty()) {
        rec.setFileName(m_opt.recordPath);
        rec.open(QIODevice::WriteOnly | QIODevice::Truncate);
    }
    std::optional<VideoToolboxDecoder> dec;
    // 纯色帧 stall 检测：冷启后图形栈预热期副屏合成未就绪，scrcpy 只出黑帧
    // 且不会自愈——首帧起连续 12s 纯黑即重建会话。
    // 只认"全黑"（未合成的真实表现）：白/纯色页面是应用正常内容
    // （webview 空白页全白曾误判 → 无限重建 → 48s 后弹框强关）
    bool stallTimerOn = false;
    auto stallSince = std::chrono::steady_clock::now();
    // stall 检测仅限会话启动初期（图形栈预热窗口）：60s 后应用深色/纯黑
    // 界面（deskclock 夜间模式实测 Y 全 <16）会被误判"副屏无画面"→ 无限
    // 重建会话。60s 还全黑 = 真异常，sessionFailed 路径处理
    const auto sessionStart = std::chrono::steady_clock::now();
    const auto stallWindowOver = [sessionStart] {
        return std::chrono::steady_clock::now() - sessionStart
            > std::chrono::seconds(60);
    };
    auto frameIsSolidBlack = [](const mw::scrcpy::Nv12Image& img) {
        if (img.width == 0)
            return true;
        const qsizetype step = qMax<qsizetype>(1, img.yStride * img.height / 32);
        const char* y = img.data.constData();
        for (qsizetype off = 0; off < img.yStride * qsizetype(img.height); off += step) {
            if (quint8(y[off]) > 16)
                return false;
        }
        return true;
    };
    while (!m_quitting.load()) {
        auto p = stream.nextPacket();
        if (!p.ok()) {
            qWarning().noquote() << "[mwine-window] nextPacket 失败 fd=" << stream.stream().fd()
                                 << "err=" << p.error();
            QMetaObject::invokeMethod(this, [this, e = p.error()] { sessionFailed(e); },
                                      Qt::QueuedConnection);
            return;
        }
        auto pkt = std::move(p.value());
        if (!pkt) { // EOF：server 退出
            QMetaObject::invokeMethod(
                this, [this] { sessionFailed(QStringLiteral("视频流结束（server 已退出）")); },
                Qt::QueuedConnection);
            return;
        }
        if (pkt->config) {
            if (rec.isOpen())
                rec.write(pkt->data);
            if (!dec) {
                auto d = VideoToolboxDecoder::create(pkt->data);
                if (!d.ok()) {
                    QMetaObject::invokeMethod(this,
                                              [this, e = d.error()] { sessionFailed(e); },
                                              Qt::QueuedConnection);
                    return;
                }
                dec.emplace(std::move(d.value()));
            } else {
                // flex display 重协商：新 SPS/PPS 重建会话
                auto r = dec->reconfigure(pkt->data);
                if (!r.ok()) {
                    QMetaObject::invokeMethod(this,
                                              [this, e = r.error()] { sessionFailed(e); },
                                              Qt::QueuedConnection);
                    return;
                }
            }
            continue;
        }
        if (rec.isOpen())
            rec.write(pkt->data);
        if (!dec)
            continue;
        if (!dec->decode(pkt->pts, pkt->data).ok())
            continue; // 单帧解码失败跳过，流继续
        while (auto f = dec->drain()) {
            auto nv = f->toNv12();
            if (!nv.ok())
                continue;
            // 纯黑帧 stall：仅启动初期 12s 全黑（图形栈未就绪）→ 重建会话
            if (!m_opt.mirror && nv.value().width > 0 && !stallWindowOver()) {
                if (frameIsSolidBlack(nv.value())) {
                    if (!stallTimerOn) {
                        stallTimerOn = true;
                        stallSince = std::chrono::steady_clock::now();
                    } else if (std::chrono::steady_clock::now() - stallSince
                               > std::chrono::seconds(12)) {
                        qWarning().noquote()
                            << "[mwine-window] 副屏持续纯色帧（图形栈未就绪），重建会话";
                        QMetaObject::invokeMethod(this, &ScreenWidget::restartAfterStall,
                                                  Qt::QueuedConnection);
                        return;
                    }
                } else {
                    stallTimerOn = false; // 有真实内容，清除计时
                }
            }
            publishFrame(std::move(nv.value()));
        }
    }
}

void ScreenWidget::writerLoop(mw::scrcpy::ControlChannel ch)
{
    for (;;) {
        QVector<ControlMessage> batch;
        {
            std::unique_lock<std::mutex> lk(m_writeMu);
            m_writeCv.wait(lk, [&] { return m_writerQuit || !m_writeQueue.isEmpty(); });
            if (m_writerQuit && m_writeQueue.isEmpty())
                return;
            batch = std::move(m_writeQueue);
            m_writeQueue.clear();
        }
        for (const auto& m : batch) {
            if (!ch.send(m).ok()) {
                // 控制通道断开（server 退出/网络断）。视频线程会给出用户可见的失败。
                return;
            }
        }
    }
}

void ScreenWidget::deviceMsgLoop(mw::adb::TcpConn dup)
{
    if (!dup.isValid())
        return;
    TcpReader reader(dup, -1);
    while (!m_quitting.load()) {
        auto m = readDeviceMessage(reader);
        if (!m.ok() || !m.value())
            return; // EOF / 错误：静默退出（视频线程负责上报）
        if (m.value()->kind == DeviceMessage::Kind::Clipboard) {
            const QString text = m.value()->text;
            QMetaObject::invokeMethod(
                this,
                [this, text] { QGuiApplication::clipboard()->setText(text); },
                Qt::QueuedConnection);
        }
    }
}

void ScreenWidget::sessionFailed(const QString& why)
{
    qWarning().noquote() << "[mwine-window] 会话中断:" << why;
    if (m_quitting.load())
        return;
    // VM 已停/重启（退出主程序、daemon 关停）：EOF 是连带结果，静默收窗；
    // 只有 VM 还活着时的中断才值得打断用户（流层真异常）
    if (!vmStillAlive()) {
        if (QWidget* w = window())
            w->close();
        return;
    }
    QMessageBox::critical(this, window()->windowTitle(),
                          QStringLiteral("独立窗口会话中断：%1").arg(why));
    if (QWidget* w = window())
        w->close();
}

void ScreenWidget::restartAfterStall()
{
    if (m_quitting.load() || m_tornDown.load())
        return;
    if (++m_sessionAttempts > 3) {
        sessionFailed(QStringLiteral("副屏长时间无画面（系统图形栈未就绪），请稍后重试"));
        return;
    }
    setStatusText(QStringLiteral("正在重连视频流（%1/3）…").arg(m_sessionAttempts));
    qWarning().noquote() << "[mwine-window] 会话重建 attempt" << m_sessionAttempts;
    // 停旧部件移后台（guard->stop 含 adb 清理最长 ~12s，GUI 线程做会冻结窗口）；
    // 停完回 GUI 线程重置状态并重建（复用既有副屏与槽位；am start 幂等）
    {
        std::lock_guard<std::mutex> lk(m_writeMu);
        m_writerQuit = true;
    }
    m_writeCv.notify_all();
    if (m_audio)
        (void)m_audio->stop();
    stopSessionInBackground([this] {
        if (m_tornDown.load())
            return; // 等待期间用户关窗：不再重建
        m_bgJoin.store(0); // 新会话周期：允许下一轮 stop 再收集
        m_guard.reset();
        m_audio.reset();
        {
            std::lock_guard<std::mutex> lk(m_writeMu);
            m_writerQuit = false; // writerLoop 重启前置位
        }
        {
            std::lock_guard<std::mutex> lk(m_frame.mu);
            m_frame.seq = 0; // 回到提示层
            m_frame.img = {};
        }
        m_guestDisplayId = 0; // 差集会失败（副屏已存在），兜底走 m_reuseDisplayId
        m_bootThread = std::thread([this] { beginSession(); });
    });
}

// 非阻塞停止：只发停止指令（音频停 / writer 解锁 / 单实例下线）。
// 会话 server 的停止（SessionGuard::stop 内含 adb shell 清理，最长 10s+）
// 一并移到调用方的后台线程执行——GUI 线程调用安全且不冻结。
void ScreenWidget::requestSessionStop()
{
    bool expected = false;
    if (!m_tornDown.compare_exchange_strong(expected, true))
        return;
    m_quitting.store(true);
    {
        std::lock_guard<std::mutex> lk(m_writeMu);
        m_writerQuit = true;
    }
    m_writeCv.notify_all();
    if (m_audio)
        (void)m_audio->stop();
    m_instanceServer.close(); // 立即下线：后续 open 起新进程而非唤醒本将退出的窗口
    // 读线程强制解锁：对视频/设备消息 socket 主动 shutdown。对端链路
    //（adbd wire 桥）在 server 死后可能不关流，poll 会永远等——shutdown
    // 让 poll 立即以错误返回，线程退出、collector 的 join 才能完成
    if (m_videoFd >= 0)
        ::shutdown(static_cast<int>(m_videoFd), SHUT_RDWR);
    if (m_devMsgFd >= 0)
        ::shutdown(static_cast<int>(m_devMsgFd), SHUT_RDWR);
}

// 后台线程执行：停 server（含 adb 清理，慢）+ join 全部会话线程 + 副屏回收。
// done 回 GUI 线程（QueuedConnection close / 重建回调）。
void ScreenWidget::stopSessionInBackground(const std::function<void()>& done)
{
    // join 职责唯一化：collector 是唯一 join 会话线程的地方（并发双重 join
    // = "Invalid argument" terminate，2026-10-17 关闭崩溃根因——25s 兜底
    // quit → aboutToQuit 补 close → deleteLater 析构撞上还在 join 的
    // collector）。状态机防重入：close 与 stall 重建竞态时先到者胜。
    int expected = 0;
    if (!m_bgJoin.compare_exchange_strong(expected, 1))
        return; // 已有 collector 在跑
    std::thread([this, done] {
        if (m_guard)
            m_guard->stop();
        if (m_videoThread.joinable())
            m_videoThread.join();
        if (m_writerThread.joinable())
            m_writerThread.join();
        if (m_devMsgThread.joinable())
            m_devMsgThread.join();
        if (m_bootThread.joinable())
            m_bootThread.join();
        if (m_themeThread.joinable())
            m_themeThread.join();
        m_bgJoin.store(2);
        if (done)
            QMetaObject::invokeMethod(this, done, Qt::QueuedConnection);
    }).detach();
}

void ScreenWidget::teardown()
{
    requestSessionStop();
    if (m_guard)
        m_guard->stop();
    if (m_videoThread.joinable())
        m_videoThread.join();
    if (m_writerThread.joinable())
        m_writerThread.join();
    if (m_devMsgThread.joinable())
        m_devMsgThread.join();
    if (m_bootThread.joinable())
        m_bootThread.join();
    if (m_themeThread.joinable())
        m_themeThread.join();
    // qemu 副屏回收（mirror 模式无副屏）；del 走 detached 线程避免阻塞 GUI
    if (m_consoleIdx > 0 && !m_opt.mirror) {
        const int idx = m_consoleIdx;
        const QString serial = m_opt.serial;
        releaseDisplaySlot();
        std::thread([idx, serial] {
            emuConsoleCmd(serial, QStringLiteral("multidisplay del %1").arg(idx));
        }).detach();
    }
    // 真机副屏回收：从 overlay_display_devices 里只摘掉自己条目（detached，
    // adb shell 慢；条目残留会在手机上留下"幽灵辅助屏"）
    if (!m_realOverlaySpec.isEmpty() && !m_opt.mirror) {
        const QString serial = m_opt.serial;
        const QString spec = m_realOverlaySpec;
        m_realOverlaySpec.clear();
        std::thread([serial, spec] {
            mw::adb::AdbClient adb;
            removeRealOverlayEntry(adb, serial, spec);
        }).detach();
    }
}

// ---- 帧交付 ----

void ScreenWidget::publishFrame(mw::scrcpy::Nv12Image img)
{

    // 诊断：MWINE_DUMP_DIR 存在时 dump 解码后帧（限 3 张，判断解码环节）
    if (const QByteArray dir = qgetenv("MWINE_DUMP_DIR"); !dir.isEmpty()) {
        static std::atomic<int> dumped{ 0 };
        if (dumped.fetch_add(1) < 3) {
            const QString path = QStringLiteral("%1/decoded_%2.png")
                                     .arg(QString::fromLocal8Bit(dir))
                                     .arg(dumped.load() - 1);
            const quint8* y = reinterpret_cast<const quint8*>(img.data.constData());
            QImage qimg(img.width, img.height, QImage::Format_Grayscale8);
            for (quint32 r = 0; r < img.height; ++r)
                ::memcpy(qimg.scanLine(r), y + r * img.yStride, img.width);
            qimg.scaled(188, 408).save(path);
            qWarning().noquote() << "[dump] decoded ->" << path;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_frame.mu);
        m_frame.img = std::move(img);
        m_frame.seq++;
    }
    scheduleUpdate();
}

void ScreenWidget::scheduleUpdate() const
{
    QMetaObject::invokeMethod(const_cast<ScreenWidget*>(this), "update",
                              Qt::QueuedConnection);
}

// ---- 渲染 ----

void ScreenWidget::initializeGL()
{
    m_prog.emplace();
    const bool ok = m_prog->addShaderFromSourceCode(
                        QOpenGLShader::Vertex,
                        "#version 330 core\n"
                        "in vec2 aPos;\n"
                        "out vec2 vUv;\n"
                        "void main() {\n"
                        "    vUv = vec2((aPos.x + 1.0) * 0.5, (1.0 - aPos.y) * 0.5);\n"
                        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
                        "}\n")
        && m_prog->addShaderFromSourceCode(
               QOpenGLShader::Fragment,
               "#version 330 core\n"
               "in vec2 vUv;\n"
               "out vec4 fragColor;\n"
               "uniform sampler2D uY;\n"
               "uniform sampler2D uUV;\n"
               "uniform vec2 uYTexScale;\n"
               "uniform vec2 uUVTexScale;\n"
               "uniform float uFullRange;\n"
               "void main() {\n"
               "    float y = texture(uY, vUv * uYTexScale).r;\n"
               "    vec2 uv = texture(uUV, vUv * uUVTexScale).rg - vec2(0.5);\n"
               "    float y1 = uFullRange > 0.5 ? y : (y - 0.0625) * 1.164;\n"
               "    float r = y1 + 1.596 * uv.y;\n"
               "    float g = y1 - 0.391 * uv.x - 0.813 * uv.y;\n"
               "    float b = y1 + 2.018 * uv.x;\n"
               "    fragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);\n"
               "}\n")
        && m_prog->link();
    if (!ok) {
        qWarning() << "[mwine-window] shader 链接失败:" << m_prog->log();
        m_prog.reset();
        return;
    }
    m_uYTexScale = m_prog->uniformLocation("uYTexScale");
    m_uUVTexScale = m_prog->uniformLocation("uUVTexScale");
    m_uFullRange = m_prog->uniformLocation("uFullRange");

    // core profile 下顶点数据必须经 VBO（client-side 数组被禁用，
    // 此前 glDrawArrays 实际画不出任何东西 → 永远只有清屏深黑）
    m_vao.emplace();
    m_vao->create();
    m_vbo.emplace();
    m_vbo->create();
    m_vbo->setUsagePattern(QOpenGLBuffer::DynamicDraw);
    static const GLfloat kInitVerts[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    m_vao->bind();
    m_vbo->bind();
    m_vbo->allocate(kInitVerts, sizeof(kInitVerts));
    m_prog->enableAttributeArray(0);
    m_prog->setAttributeBuffer(0, GL_FLOAT, 0, 2);
    m_vbo->release();
    m_vao->release();

    m_texY = new QOpenGLTexture(QOpenGLTexture::Target2D);
    m_texUV = new QOpenGLTexture(QOpenGLTexture::Target2D);
}

void ScreenWidget::paintGL()
{
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, int(width() * dpr), int(height() * dpr));
    glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    mw::scrcpy::Nv12Image local;
    quint64 seq = 0;
    {
        std::lock_guard<std::mutex> lk(m_frame.mu);
        if (m_frame.seq == 0 || !m_prog)
            local = mw::scrcpy::Nv12Image{};
        else {
            local = m_frame.img; // QByteArray 隐式共享，拷贝廉价
            seq = m_frame.seq;
        }
    }

    static std::atomic<int> paintCount{ 0 };
    if (paintCount.fetch_add(1) == 30)
        qWarning().noquote() << "[dump] paintGL alive: seq=" << seq
                             << "prog=" << (m_prog ? "yes" : "null");
    if (seq == 0) {
        QPainter pt(this);
        pt.setPen(QColor(160, 164, 172));
        const QString hint = m_status.isEmpty()
                                 ? QStringLiteral("正在连接 %1 …").arg(m_opt.serial)
                                 : m_status;
        pt.drawText(rect(), Qt::AlignCenter, hint);
        return;
    }

    if (seq != m_uploadedSeq) {
        // Y 平面：整块上传（宽度 = yStride，含行尾 pad）
        if (!m_texY->isCreated() || m_texY->width() != int(local.yStride)
            || m_texY->height() != int(local.height)) {
            m_texY->destroy();
            m_texY->setFormat(QOpenGLTexture::R8_UNorm);
            m_texY->setSize(int(local.yStride), int(local.height));
            m_texY->setMinificationFilter(QOpenGLTexture::Linear);
            m_texY->setMagnificationFilter(QOpenGLTexture::Linear);
            m_texY->allocateStorage(QOpenGLTexture::Red, QOpenGLTexture::UInt8);
        }
        if (!m_texUV->isCreated() || m_texUV->width() != int(local.uvStride / 2)
            || m_texUV->height() != int(local.uvHeight())) {
            m_texUV->destroy();
            m_texUV->setFormat(QOpenGLTexture::RG8_UNorm);
            m_texUV->setSize(int(local.uvStride / 2), int(local.uvHeight()));
            m_texUV->setMinificationFilter(QOpenGLTexture::Linear);
            m_texUV->setMagnificationFilter(QOpenGLTexture::Linear);
            m_texUV->allocateStorage(QOpenGLTexture::RG, QOpenGLTexture::UInt8);
        }
        m_texY->setData(QOpenGLTexture::Red, QOpenGLTexture::UInt8,
                        reinterpret_cast<const void*>(local.data.constData()));
        m_texUV->setData(QOpenGLTexture::RG, QOpenGLTexture::UInt8,
                         reinterpret_cast<const void*>(local.data.constData() + local.uvOffset));
        m_uploadedSeq = seq;
    }

    // 内容框（letterbox）
    const QRectF cr = contentRect();
    const GLfloat x0 = GLfloat(cr.left() / width() * 2.0 - 1.0);
    const GLfloat x1 = GLfloat(cr.right() / width() * 2.0 - 1.0);
    const GLfloat y0 = GLfloat(1.0 - cr.bottom() / height() * 2.0);
    const GLfloat y1 = GLfloat(1.0 - cr.top() / height() * 2.0);
    const GLfloat verts[] = { x0, y0, x1, y0, x0, y1, x1, y1 };

    m_prog->bind();
    m_vao->bind();
    m_vbo->bind();
    m_vbo->write(0, verts, sizeof(verts));
    m_vbo->release();
    m_prog->setUniformValue(m_uYTexScale,
                            GLfloat(local.width) / GLfloat(local.yStride), GLfloat(1.0f));
    m_prog->setUniformValue(m_uUVTexScale,
                            GLfloat(local.width) / GLfloat(local.uvStride), GLfloat(1.0f));
    m_prog->setUniformValue(m_uFullRange, local.fullRange ? 1.0f : 0.0f);
    m_texY->bind(0);
    m_texUV->bind(1);
    m_prog->setUniformValue("uY", 0);
    m_prog->setUniformValue("uUV", 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_texUV->release(1);
    m_texY->release(0);
    m_vao->release();
    m_prog->release();

    // 诊断：读回渲染帧缓冲（前 3 次渲染判定输出黑不黑）
    if (const QByteArray dir = qgetenv("MWINE_DUMP_DIR"); !dir.isEmpty()) {
        static std::atomic<int> dumped{ 0 };
        if (dumped.fetch_add(1) < 3) {
            // 帧缓冲是物理像素（Retina ×dpr），按物理尺寸读全幅（含顶栏）
            const int w = qMax(1, int(width() * dpr)), h = qMax(1, int(height() * dpr));
            QImage qimg(w, h, QImage::Format_RGBA8888);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, qimg.bits());
            qimg.mirrored().save(QStringLiteral("%1/rendered_%2.png")
                                     .arg(QString::fromLocal8Bit(dir))
                                     .arg(dumped.load() - 1));
        }
    }
}

QRectF ScreenWidget::contentRect() const
{
    quint32 vw = 9, vh = 16;
    {
        std::lock_guard<std::mutex> lk(m_frame.mu);
        if (m_frame.seq != 0) {
            vw = m_frame.img.width;
            vh = m_frame.img.height;
        } else if (!m_opt.mirror) {
            vw = m_opt.dispW;
            vh = m_opt.dispH;
        }
    }
    const QRectF va = rect(); // 并列布局：本部件全域即视频区
    const qreal scale = qMin(va.width() / vw, va.height() / vh);
    const qreal w = vw * scale, h = vh * scale;
    return QRectF(va.left() + (va.width() - w) / 2, va.top() + (va.height() - h) / 2, w, h);
}

QPointF ScreenWidget::videoPos(const QPointF& p) const
{
    const QRectF cr = contentRect();
    quint32 vw = 1, vh = 1;
    {
        std::lock_guard<std::mutex> lk(m_frame.mu);
        if (m_frame.seq != 0) {
            vw = m_frame.img.width;
            vh = m_frame.img.height;
        }
    }
    const qreal x = (p.x() - cr.left()) / cr.width() * vw;
    const qreal y = (p.y() - cr.top()) / cr.height() * vh;
    return QPointF(qBound<qreal>(0, x, vw - 1), qBound<qreal>(0, y, vh - 1));
}

// ---- 顶栏 ----

// 功能按钮统一样式：图标钮（悬停/按下由 QSS 承担）。深浅主题的悬停底色不同。
static QString barBtnStyle(bool dark)
{
    return dark
        ? QStringLiteral(
              "QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 0;"
              " background: transparent; }"
              "QToolButton:hover { background: rgba(255,255,255,0.18);"
              " border: 1px solid rgba(255,255,255,0.10); }"
              "QToolButton:pressed { background: rgba(255,255,255,0.30); }"
              "QToolButton[danger=\"true\"]:hover {"
              " background: rgba(232,70,62,0.45); border: 1px solid rgba(232,70,62,0.5); }"
              "QToolButton[danger=\"true\"]:pressed { background: rgba(232,70,62,0.65); }")
        : QStringLiteral(
              "QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 0;"
              " background: transparent; }"
              "QToolButton:hover { background: rgba(0,0,0,0.10);"
              " border: 1px solid rgba(0,0,0,0.07); }"
              "QToolButton:pressed { background: rgba(0,0,0,0.18); }"
              "QToolButton[danger=\"true\"]:hover {"
              " background: rgba(232,70,62,0.32); border: 1px solid rgba(232,70,62,0.4); }"
              "QToolButton[danger=\"true\"]:pressed { background: rgba(232,70,62,0.55); }");
}

namespace {

// Material 线性图标（24x24 逻辑尺寸，固定 2x 栅格渲染）：图标钮不依赖字体，
// 自绘矢量任何缩放下都锐利。线色由主题给（暗面板浅灰 / 白面板深灰；
// 关闭钮恒红）。
QIcon iconFromDrawer(const std::function<void(QPainter&)>& draw)
{
    QPixmap pm(QSize(48, 48));
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(24.0 / 20.0, 24.0 / 20.0); // 绘制坐标沿用 20x20 栅格
    draw(p);
    p.end();
    return QIcon(pm);
}

QPen iconPen(const QColor& c)
{
    return QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

QIcon backIcon(const QColor& c)
{
    return iconFromDrawer([c](QPainter& p) {
        // Material arrow_back：左向箭头（横线 + 折线箭头）
        p.setPen(iconPen(c));
        p.drawLine(QPointF(15, 10), QPointF(5, 10));
        p.drawLine(QPointF(9, 5.5), QPointF(4.5, 10));
        p.drawLine(QPointF(4.5, 10), QPointF(9, 14.5));
    });
}

QIcon volumeIcon(bool up, const QColor& c)
{
    return iconFromDrawer([up, c](QPainter& p) {
        // Material volume_down/up：喇叭（实心）+ 右侧弧形波纹（1 道 / 2 道）
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF() << QPointF(2.5, 8.25) << QPointF(5.5, 8.25)
                                  << QPointF(9, 4.75) << QPointF(9, 15.25)
                                  << QPointF(5.5, 11.75) << QPointF(2.5, 11.75));
        p.setBrush(Qt::NoBrush);
        p.setPen(iconPen(c));
        // 波纹：以喇叭右侧为圆心的两段圆弧（-40°..40°）
        p.drawArc(QRectF(7.5, 7.0, 6.0, 6.0), -40 * 16, 80 * 16);          // 内圈
        if (up)
            p.drawArc(QRectF(5.0, 4.5, 11.0, 11.0), -40 * 16, 80 * 16);    // 外圈
    });
}

QIcon muteIcon(const QColor& c)
{
    return iconFromDrawer([c](QPainter& p) {
        // Material volume_off：喇叭（实心）+ 一道内波纹 + 贯穿斜线（静音杠）
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF() << QPointF(1.5, 8.25) << QPointF(4.5, 8.25)
                                  << QPointF(8, 4.75) << QPointF(8, 15.25)
                                  << QPointF(4.5, 11.75) << QPointF(1.5, 11.75));
        p.setBrush(Qt::NoBrush);
        p.setPen(iconPen(c));
        p.drawArc(QRectF(6.5, 7.0, 6.0, 6.0), -40 * 16, 80 * 16);
        // 静音斜线（喇叭右上 → 右下）
        p.drawLine(QPointF(11, 4.5), QPointF(17.5, 15.5));
    });
}

QIcon closeIcon()
{
    return iconFromDrawer([](QPainter& p) {
        p.setPen(iconPen(QColor(0xE8, 0x86, 0x80)));
        p.drawLine(QPointF(6, 6), QPointF(14, 14));
        p.drawLine(QPointF(14, 6), QPointF(6, 14));
    });
}

} // namespace

BarWidget::BarWidget(ScreenWidget* host, QWidget* parent)
    : QWidget(parent), m_host(host)
{
    setFixedHeight(kBarH);
    setCursor(Qt::ArrowCursor);

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 0, 8, 0);
    lay->setSpacing(4);

    // 左：应用图标（真彩缓存 PNG；缺省回退应用名文字）
    m_iconBox = new QLabel(this);
    m_iconBox->setFixedSize(QSize(24, 24));
    m_iconBox->setScaledContents(true);
    m_iconBox->setVisible(false);
    lay->addWidget(m_iconBox);
    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::PlainText);
    lay->addWidget(m_title);
    lay->addStretch(1);

    // 手机底部操作区：返回 / 静音 / 音量±（多任务已按需求移除）
    addKeyButton([this] { return backIcon(themeIconColor()); },
                 control::AKEYCODE_BACK, QStringLiteral("返回 (Android Back)"));
    addKeyButton([this] { return muteIcon(themeIconColor()); },
                 control::AKEYCODE_VOLUME_MUTE, QStringLiteral("静音 / 取消静音"));
    addKeyButton([this] { return volumeIcon(false, themeIconColor()); },
                 control::AKEYCODE_VOLUME_DOWN, QStringLiteral("音量减小"));
    addKeyButton([this] { return volumeIcon(true, themeIconColor()); },
                 control::AKEYCODE_VOLUME_UP, QStringLiteral("音量增大"));

    m_close = new QToolButton(this);
    m_close->setIcon(closeIcon());
    m_close->setIconSize(QSize(24, 24));
    m_close->setFixedSize(34, 30);
    m_close->setToolTip(QStringLiteral("关闭窗口 (⌘W)"));
    m_close->setProperty("danger", true);
    connect(m_close, &QToolButton::clicked, this, [this] {
        if (QWidget* w = window())
            w->close();
    });
    lay->addWidget(m_close);

    applyTheme(true); // 默认暗面板；会话就绪后按 guest 主题切换
}

void BarWidget::setAppIcon(const QString& iconPath)
{
    m_iconPath = iconPath;
    reloadIcon();
}

void BarWidget::reloadIcon()
{
    QPixmap pm;
    const bool ok = !m_iconPath.isEmpty() && pm.load(m_iconPath);
    if (ok) {
        // @2x 渲染：24pt 逻辑位图
        QPixmap scaled = pm.scaled(48, 48, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(2.0);
        m_iconBox->setPixmap(scaled);
        m_iconBox->setToolTip(m_host->window()
                                      ? m_host->window()->windowTitle() : QString());
        m_iconBox->setVisible(true);
        m_title->setVisible(false);
    } else {
        m_iconBox->setVisible(false);
        m_title->setVisible(true);
    }
}

// 功能图标线色：暗面板浅灰 / 白面板深灰（魅族白面板的图标基调）
QColor BarWidget::themeIconColor() const
{
    return m_dark ? QColor(0xD8, 0xD8, 0xDE) : QColor(0x6E, 0x6E, 0x73);
}

void BarWidget::applyTheme(bool dark)
{
    m_dark = dark;
    // 暗面板：深底浅字；白面板：Flyme 式 #F7F7FA 底、深灰字与图标
    setStyleSheet(dark
        ? QStringLiteral(
              "BarWidget { background: #1C1C1F; border-bottom: 1px solid #303036; }"
              "QLabel { color: #9A9AA4; font-size: 12px; padding-left: 4px; }")
        : QStringLiteral(
              "BarWidget { background: #F7F7FA; border-bottom: 1px solid #E4E4E9; }"
              "QLabel { color: #3C3C43; font-size: 12px; padding-left: 4px; }"));
    const QString btn = barBtnStyle(dark);
    for (auto& k : m_keyBtns)
        k.btn->setStyleSheet(btn);
    if (m_close)
        m_close->setStyleSheet(btn);
    rebuildIcons();
    update();

    // 诊断：MWINE_BAR_DUMP=<dir> 时每次主题切换落盘 bar-<dark|light>.png
    if (const QByteArray dir = qgetenv("MWINE_BAR_DUMP"); !dir.isEmpty()) {
        QTimer::singleShot(300, this, [this, dir, dark] {
            grab().save(QStringLiteral("%1/bar-%2.png")
                            .arg(QString::fromLocal8Bit(dir))
                            .arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
        });
    }
}

void BarWidget::rebuildIcons()
{
    for (auto& k : m_keyBtns)
        k.btn->setIcon(k.iconGen());
}

void BarWidget::addKeyButton(const std::function<QIcon()>& iconGen,
                             qint32 keycode, const QString& tip)
{
    auto* b = new QToolButton(this);
    b->setIcon(iconGen());
    b->setIconSize(QSize(24, 24));
    b->setFixedSize(34, 30);
    b->setToolTip(tip);
    connect(b, &QToolButton::clicked, this, [this, keycode] {
        // 与 ⌃ 快捷键同款完整点按（Down+Up）
        m_host->enqueue(ControlMessage::injectKeycode(KeyAction::Down, keycode, 0, 0));
        m_host->enqueue(ControlMessage::injectKeycode(KeyAction::Up, keycode, 0, 0));
    });
    layout()->addWidget(b);
    m_keyBtns.append({ b, iconGen });
}

void BarWidget::paintEvent(QPaintEvent* e)
{
    QWidget::paintEvent(e);
    // 标题文字仅作图标缺失时的回退（reloadIcon 控制可见性）；内容跟随窗口标题
    const QString t = m_host->window() ? m_host->window()->windowTitle() : QString();
    if (m_title->text() != t)
        m_title->setText(t);
    // bar 渲染取证在 applyTheme（MWINE_BAR_DUMP=<dir>，按主题落盘）
}

void BarWidget::mousePressEvent(QMouseEvent* e)
{
    // 无边框窗口：栏内空白按住即拖动移窗（按钮区域不会到这——子部件先收事件）。
    // 纯手动拖动（不依赖 startSystemMove）：macOS 上 performWindowDrag 需要
    // 真实 NSEvent 在队列中，Qt 转发时机不保证——手动增量 setFramePosition
    // 在所有平台行为一致。
    // **坑（拖拽从未生效的根因）**：BarWidget 是子部件，windowHandle() 对
    // 子部件恒为 null——必须经 window()（顶层 widget）取 QWindow
    if (e->button() == Qt::LeftButton && window()->windowHandle()) {
        m_manualMove = true;
        // 锚点用事件自带全局位置（比 QCursor::pos() 可靠：合成事件、拖动
        // 跨屏时 QCursor 与事件流可能瞬时不一致）
        m_pressGlobal = e->globalPosition().toPoint();
        m_pressFramePos = QPointF(window()->pos()); // QWidget 坐标（move 同基准）
        return;
    }
    QWidget::mousePressEvent(e);
}

void BarWidget::mouseMoveEvent(QMouseEvent* e)
{
    // 手动拖动：以按下瞬间锚点增量移动窗口（window()->windowHandle()，
    // 子部件自身的 windowHandle() 为 null——见 mousePressEvent 注释）
    if (m_manualMove && (e->buttons() & Qt::LeftButton)) {
        const QPointF delta = e->globalPosition() - QPointF(m_pressGlobal);
        // 用 QWidget::move（几何记忆同路径，实测可靠）。曾用 QWindow::
        // setFramePosition——对该窗口实测不生效（frame 不动）
        const QPointF target = QPointF(m_pressFramePos) + delta;
        window()->move(target.toPoint());
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void BarWidget::mouseReleaseEvent(QMouseEvent* e)
{
    m_manualMove = false;
    QWidget::mouseReleaseEvent(e);
}

void ScreenWidget::resizeEvent(QResizeEvent* e)
{
    QOpenGLWidget::resizeEvent(e);
    saveGeometryNow();
    update();
    // flex display：窗口稳定后按内容尺寸重排 guest 分辨率（去 letterbox）。
    // 不再要求 m_guard 就绪——会话未起时 timer 触发会在 requestResizeDisplay
    // 里被 m_consoleIdx==0 挡下；sessionUp 会再主动触发一次兜底
    if (!m_opt.mirror && !m_quitting.load())
        m_resizeTimer.start();
}

void ScreenWidget::requestResizeDisplay()
{
    if (m_opt.mirror || m_quitting.load() || m_consoleIdx == 0)
        return;
    // 物理像素（Retina 下 guest 需要物理分辨率，否则 GPU 拉伸发虚）。
    // 并列布局：本部件高度即视频区高度，无需扣 bar
    const qreal dpr = devicePixelRatioF();
    // qemu 副屏分辨率下限 768×768（实测：任一边 <768 的 multidisplay add 直接
    // "KO: setMultiDisplay not supported"，副屏卡旧分辨率 → letterbox 黑边）。
    // 小窗请求按比例放大到下限之上：guest 分辨率 > 窗口物理像素时渲染整体
    // 缩小，aspect 不变 → 无黑边，窗口侧无清晰度损失
    qreal w = qBound<qreal>(320, width() * dpr, 3840);
    qreal h = qBound<qreal>(320, height() * dpr, 3840);
    if (w < 768 || h < 768) {
        const qreal s = qMax(768.0 / w, 768.0 / h);
        w = qMin<qreal>(3840, w * s);
        h = qMin<qreal>(3840, h * s);
    }
    const quint16 qw = quint16(w), qh = quint16(h);
    if (qw == m_lastWidth && qh == m_lastHeight)
        return;
    m_lastWidth = qw;
    m_lastHeight = qh;
    // console 调用移后台线程：emuConsoleCmd 是同步 TCP（connect 3s + 读 1.5s），
    // 在 GUI 线程跑会冻结窗口（拖动 resize 时尤其明显）
    const int idx = m_consoleIdx;
    const QString serial = m_opt.serial;
    const quint32 dpi = m_opt.dispDpi;
    std::thread([this, idx, serial, qw, qh, dpi] {
        // qemu 副屏分辨率重排（multidisplay add 对既有 index = modify 语义）；
        // scrcpy 流随分辨率变化重发 session meta，解码器按新 SPS 重建
        const QString cmd = QStringLiteral("multidisplay add %1 %2 %3 %4 0")
                                .arg(idx)
                                .arg(qw)
                                .arg(qh)
                                .arg(dpi);
        const QString resp = emuConsoleCmd(serial, cmd);
        qWarning().noquote() << "[mwine-window] flex resize:" << cmd << "->"
                             << resp.trimmed().replace(QLatin1Char('\n'), QLatin1Char(' '));
        // modify 落定后校验实际输出 aspect（偏差自动二次矫正；GUI 线程调度）
        QMetaObject::invokeMethod(
            this, [this] { QTimer::singleShot(1200, this, [this] { correctDisplayAspect(); }); },
            Qt::QueuedConnection);
    }).detach();
}

// ---- 输入 ----

void ScreenWidget::enqueue(const ControlMessage& msg)
{
    if (m_quitting.load())
        return;
    {
        std::lock_guard<std::mutex> lk(m_writeMu);
        m_writeQueue.append(msg);
    }
    m_writeCv.notify_all();
}

void ScreenWidget::injectTextSmart(const QString& text)
{
    bool ascii = true;
    for (const QChar& c : text) {
        const ushort u = c.unicode();
        if (u < 32 || u > 127) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        sendTextChunked(text);
        return;
    }
    // 非 ASCII：scrcpy 官方 server 的 INJECT_TEXT 只处理 32..127（其余静默
    // 丢弃，实测注入「你好ABC」仅 ABC 落地）。改走 SET_CLIPBOARD(paste=true)
    // 原子消息：server 置剪贴板后立即触发 KEYCODE_PASTE，焦点字段即得全文。
    // 代价：设备剪贴板被覆盖（与 scrcpy 官方 CJK 方案一致）
    enqueue(ControlMessage::setClipboard(m_clipSeq++, true, text));
}

void ScreenWidget::sendTextChunked(const QString& text)
{
    for (int i = 0; i < text.size(); i += control::kInjectTextMax)
        enqueue(ControlMessage::injectText(text.mid(i, control::kInjectTextMax)));
}

// ⌃ 组合 → Android 系统键（scrcpy MOD 惯例子集；⌘ 已被窗口级快捷键占用）。
// 按下即完整点按（Down+Up），auto-repeat 不连发；未命中的 ⌃ 组合照常透传设备。
bool ScreenWidget::handleSystemShortcut(QKeyEvent* e, bool press)
{
    if (!(e->modifiers() & Qt::ControlModifier))
        return false;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool fire = press && !e->isAutoRepeat();
    auto tap = [this, fire](qint32 code) {
        if (!fire)
            return;
        enqueue(ControlMessage::injectKeycode(KeyAction::Down, code, 0, 0));
        enqueue(ControlMessage::injectKeycode(KeyAction::Up, code, 0, 0));
    };
    switch (e->key()) {
    case Qt::Key_B: tap(control::AKEYCODE_BACK); return true;         // 返回/回退（Esc 同义）
    case Qt::Key_H: tap(control::AKEYCODE_HOME); return true;         // 主页
    case Qt::Key_S: tap(control::AKEYCODE_APP_SWITCH); return true;   // 最近任务
    case Qt::Key_M: tap(control::AKEYCODE_MENU); return true;         // 菜单
    case Qt::Key_P: tap(control::AKEYCODE_POWER); return true;        // 电源键
    case Qt::Key_Up: tap(control::AKEYCODE_VOLUME_UP); return true;   // 音量+
    case Qt::Key_Down: tap(control::AKEYCODE_VOLUME_DOWN); return true; // 音量−
    case Qt::Key_N:                                                   // 通知栏 ⌃N 展开 / ⌃⇧N 收起
        if (fire)
            enqueue(shift ? ControlMessage::collapsePanels()
                          : ControlMessage::expandNotificationPanel());
        return true;
    case Qt::Key_O:                                                   // ⌃O 关闭屏幕（镜像继续）/ ⌃⇧O 点亮
        if (fire)
            enqueue(ControlMessage::setDisplayPower(shift));
        return true;
    case Qt::Key_R:                                                   // 旋转设备
        if (fire)
            enqueue(ControlMessage::rotateDevice());
        return true;
    default:
        return false;
    }
}

void ScreenWidget::mousePressEvent(QMouseEvent* e)
{
    m_imeAnchor = e->position();
    // 候选框/组词浮层即时挪到点击处（macOS IME 读 ImCursorRectangle）
    if (QInputMethod* im = qApp->inputMethod())
        im->update(Qt::ImCursorRectangle);
    const QPointF v = videoPos(e->position());
    if (e->button() == Qt::LeftButton) {
        enqueue(ControlMessage::injectTouch(control::ACTION_DOWN, control::kPointerIdMouse,
                                            qint32(v.x()), qint32(v.y()),
                                            quint16(m_frame.img.width),
                                            quint16(m_frame.img.height), 1.0f, 1, 1));
    } else if (e->button() == Qt::RightButton) {
        enqueue(ControlMessage::injectKeycode(KeyAction::Down, control::AKEYCODE_BACK, 0, 0));
    } else if (e->button() == Qt::MiddleButton) {
        enqueue(ControlMessage::injectKeycode(KeyAction::Down, control::AKEYCODE_HOME, 0, 0));
    }
}

void ScreenWidget::mouseMoveEvent(QMouseEvent* e)
{
    QOpenGLWidget::mouseMoveEvent(e);
    if (!(e->buttons() & Qt::LeftButton))
        return;
    const QPointF v = videoPos(e->position());
    enqueue(ControlMessage::injectTouch(control::ACTION_MOVE, control::kPointerIdMouse,
                                        qint32(v.x()), qint32(v.y()),
                                        quint16(m_frame.img.width),
                                        quint16(m_frame.img.height), 1.0f, 1, 1));
}

void ScreenWidget::mouseReleaseEvent(QMouseEvent* e)
{
    const QPointF v = videoPos(e->position());
    if (e->button() == Qt::LeftButton) {
        enqueue(ControlMessage::injectTouch(control::ACTION_UP, control::kPointerIdMouse,
                                            qint32(v.x()), qint32(v.y()),
                                            quint16(m_frame.img.width),
                                            quint16(m_frame.img.height), 0.0f, 1, 0));
    } else if (e->button() == Qt::RightButton) {
        enqueue(ControlMessage::injectKeycode(KeyAction::Up, control::AKEYCODE_BACK, 0, 0));
    } else if (e->button() == Qt::MiddleButton) {
        enqueue(ControlMessage::injectKeycode(KeyAction::Up, control::AKEYCODE_HOME, 0, 0));
    }
}

void ScreenWidget::wheelEvent(QWheelEvent* e)
{
    const QPointF v = videoPos(e->position());
    const QPointF d = e->angleDelta();
    // Android ACTION_SCROLL：正值 = 内容上移（滚轮上滚）
    float h = 0.0f, vert = 0.0f;
    if (e->modifiers() & Qt::ShiftModifier) { // Shift：横向滚动
        h = d.y() / 120.0f;
        vert = d.x() / 120.0f;
    } else {
        h = d.x() / 120.0f;
        vert = d.y() / 120.0f;
    }
    enqueue(ControlMessage::injectScroll(qint32(v.x()), qint32(v.y()),
                                         quint16(m_frame.img.width),
                                         quint16(m_frame.img.height), h, -vert,
                                         (e->buttons() & Qt::LeftButton) ? 1 : 0));
}

void ScreenWidget::keyPressEvent(QKeyEvent* e)
{
    // macOS 窗口级快捷键（⌘ = Qt::MetaModifier）不进设备
    if (e->modifiers() & Qt::MetaModifier) {
        switch (e->key()) {
        case Qt::Key_Q:
        case Qt::Key_W:
            if (QWidget* w = window())
                w->close();
            return;
        case Qt::Key_F:
            window()->isFullScreen() ? window()->showNormal() : window()->showFullScreen();
            return;
        case Qt::Key_C:
            enqueue(ControlMessage::getClipboard(control::COPY_KEY_COPY));
            return;
        case Qt::Key_X:
            enqueue(ControlMessage::getClipboard(control::COPY_KEY_CUT));
            return;
        case Qt::Key_V: {
            const QString text = QGuiApplication::clipboard()->text();
            if (!text.isEmpty())
                enqueue(ControlMessage::setClipboard(m_clipSeq++, true, text));
            return;
        }
        default:
            return;
        }
    }
    if (e->key() == Qt::Key_Escape && isFullScreen()) {
        showNormal();
        return;
    }
    // ⌃ 组合：Android 系统键快捷键（返回/主页/最近任务/音量…），未命中的再透传
    if (handleSystemShortcut(e, true))
        return;

    const auto code = androidKeycode(e->key());
    if (code) {
        enqueue(ControlMessage::injectKeycode(KeyAction::Down, *code,
                                              e->isAutoRepeat() ? 1 : 0,
                                              androidMetaState(e->modifiers())));
    } else if (!e->text().isEmpty() && e->text().at(0).isPrint()) {
        injectTextSmart(e->text()); // 无按键映射的可打印字符走文本注入
    }
}

void ScreenWidget::keyReleaseEvent(QKeyEvent* e)
{
    if (e->modifiers() & Qt::MetaModifier)
        return;
    if (e->key() == Qt::Key_Escape && isFullScreen())
        return;
    if (handleSystemShortcut(e, false))
        return;
    if (e->isAutoRepeat())
        return;
    const auto code = androidKeycode(e->key());
    if (code)
        enqueue(ControlMessage::injectKeycode(KeyAction::Up, *code, 0,
                                              androidMetaState(e->modifiers())));
}

void ScreenWidget::inputMethodEvent(QInputMethodEvent* e)
{
    // 宿主输入法：commit 文本注入设备（组词/候选由 macOS IME 面板呈现，
    // 提交串走智能通道：ASCII 逐字注入、CJK 走剪贴板原子粘贴）
    if (!e->commitString().isEmpty())
        injectTextSmart(e->commitString());
}

QVariant ScreenWidget::inputMethodQuery(Qt::InputMethodQuery q) const
{
    if (q == Qt::ImEnabled)
        return true;
    if (q == Qt::ImCursorRectangle)
        return QRectF(m_imeAnchor, QSizeF(1, 1));
    return QOpenGLWidget::inputMethodQuery(q);
}

// 关闭（两段式）：首次 close 只发停止指令 + 立即隐藏（视觉上即刻消失），
// 真正的 close 延后到后台线程停完会话（含 adb 清理，最长 ~12s）之后。
// 动机：这些慢操作在 GUI 线程同步做 = 窗口/进程冻结无响应（点关闭"没反应"）。
void ScreenWidget::closeEvent(QCloseEvent* e)
{
    handleWindowClose(e);
}

void ScreenWidget::handleWindowClose(QCloseEvent* e)
{
    if (m_tornDown.load()) {
        // 后台收尾已完成（或从未起过会话）：直通关闭
        saveGeometryNow();
        QOpenGLWidget::closeEvent(e);
        return;
    }
    e->ignore(); // 拦下本次，延迟真关
    saveGeometryNow();
    if (QWidget* w = window())
        w->hide();
    requestSessionStop();
    // 兜底：后台收尾异常久卡（adb 长超时）也保证进程能退
    QTimer::singleShot(25000, qApp, [] { QCoreApplication::quit(); });
    stopSessionInBackground([this] {
        if (m_consoleIdx > 0 && !m_opt.mirror) {
            const int idx = m_consoleIdx;
            const QString serial = m_opt.serial;
            releaseDisplaySlot();
            std::thread([idx, serial] {
                emuConsoleCmd(serial, QStringLiteral("multidisplay del %1").arg(idx));
            }).detach();
        }
        // 真机副屏条目回收（与 teardown() 同逻辑；closeEvent 两段式路径）
        if (!m_realOverlaySpec.isEmpty() && !m_opt.mirror) {
            const QString serial = m_opt.serial;
            const QString spec = m_realOverlaySpec;
            m_realOverlaySpec.clear();
            std::thread([serial, spec] {
                mw::adb::AdbClient adb;
                removeRealOverlayEntry(adb, serial, spec);
            }).detach();
        }
        if (QWidget* w = window())
            w->close(); // 此时 m_tornDown=true → 直通分支
        // 隐藏窗口的 close 不触发 lastWindowClosed（quitOnLastWindowClosed 失效），
        // 显式退出事件循环——否则进程空转到兜底 timer
        QCoreApplication::quit();
    });
}

void ScreenWidget::saveGeometryNow()
{
    if (m_opt.package.isEmpty())
        return;
    QSettings st;
    st.setValue(QStringLiteral("geom/") + m_opt.package,
                window() ? window()->saveGeometry() : saveGeometry());
}

// 会话中断的"真异常"判定：VM 还活着（console 端口可连）= 流层故障，弹框告知；
// VM 已停/重启 = 主动关停的连带 EOF，静默收窗（否则每个窗口都弹错误框）。
bool ScreenWidget::vmStillAlive()
{
    // 真机无 emulator console 端口可探：以 adb 在线为准（会话中断时设备
    // 还在线 = 流层异常，值得报错；掉线 = 静默收窗）
    if (!m_opt.serial.startsWith(QStringLiteral("emulator-"))) {
        const auto devs = mw::adb::AdbClient().devices();
        if (!devs.ok())
            return false;
        for (const auto& d : devs.value())
            if (d.serial == m_opt.serial)
                return d.state == QLatin1String("device");
        return false;
    }
    const quint16 port = mw::input::consolePortOfSerial(m_opt.serial);
    if (port == 0)
        return false;
    QTcpSocket s;
    s.connectToHost(QStringLiteral("127.0.0.1"), port);
    return s.waitForConnected(400);
}

} // namespace mwine
