// mwine-window：独立应用窗口（Qt 播放器）。
// 原 Rust winit/wgpu 管线（crates/mwine-scrcpy/src/player.rs）的 Qt 化重建：
// scrcpy 会话 → VideoToolbox 解码 → QOpenGLWidget NV12 渲染；
// 鼠标/滚轮/键盘经控制通道注入；剪贴板双向互通；窗口模式建虚拟显示器
// （flex display：窗口 resize → guest 真实重排分辨率）。
//
// 跨平台说明：本文件的渲染/输入/底部功能条/主题跟随/几何记忆均为纯 Qt，
// 无平台分支。平台差异收敛在三处（移植清单另见模块 CMakeLists）：
//   1. 解码：videoLoop 的 VideoToolboxDecoder（mac；其他平台待接）
//   2. 音频：sessionUp 的 CoreAudioBridge（mac；其他平台待接）
//   3. 窗口外观：chrome.hpp（mac/win/stub 三实现已就位）
#pragma once

#include <QLocalServer>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QLabel>
#include <QOpenGLWidget>
#include <QTimer>
#include <QToolButton>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "client.hpp"
#include "console.hpp"
#include "coreaudio.hpp"
#include "decoder.hpp"

class QCloseEvent;

namespace mwine {

class ScreenWidget;

/// 底部功能条（ScreenWidget 的子部件，手机底部操作区）：返回/静音/音量±/关闭。
/// 做成真实 QWidget 而非 GL 叠绘——原生命中测试/悬停/无障碍，事件不会漏进视频注入。
/// 左侧为应用图标（--icon 传入的缓存 PNG；缺省回退应用名文字）。
/// 配色跟随 guest 系统主题：浅色 = 魅族式"白面板"，深色 = 暗面板；
/// 空白处按住可拖动移窗（startSystemMove，失败退化为手动 move）。
class BarWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int kBarH = 36;   // 底部功能条高度
    explicit BarWidget(ScreenWidget* host, QWidget* parent = nullptr);
    /// guest 深浅色主题（ui_night_mode）：白面板 / 暗面板
    void applyTheme(bool dark);
    /// 应用图标（图标缓存 PNG 路径；空 = 回退应用名文字）
    void setAppIcon(const QString& iconPath);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    void addKeyButton(const std::function<QIcon()>& iconGen, qint32 keycode,
                      const QString& tip);
    void rebuildIcons();          // 按当前主题色重画全部功能按钮图标
    QColor themeIconColor() const;
    void reloadIcon();            // 图标/文字形态切换

    ScreenWidget* m_host;
    QLabel* m_title = nullptr;    // 图标缺失时的文字回退
    QLabel* m_iconBox = nullptr;  // 应用图标（QLabel 嵌 pixmap）
    QString m_iconPath;
    QToolButton* m_close = nullptr;
    struct KeyBtn {
        QToolButton* btn = nullptr;
        std::function<QIcon()> iconGen; // 主题色闭包（applyTheme 后重画）
    };
    QVector<KeyBtn> m_keyBtns;
    bool m_dark = true;
    // startSystemMove 失败时的手动拖动兜底（个别平台/时机）
    bool m_manualMove = false;
    QPoint m_pressGlobal;
    QPointF m_pressFramePos;
};

struct WindowOptions {
    QString serial;
    /// window 模式：会话建立后在虚拟显示器上启动该应用；mirror 模式为空
    QString package;
    QString title;
    /// 应用图标缓存 PNG（bar 左侧展示；空 = 回退应用名文字）
    QString iconPath;
    /// 虚拟显示器规格（W / H / DPI），仅 window 模式
    quint32 dispW = 944;
    quint32 dispH = 2048;
    quint32 dispDpi = 384;
    /// 镜像既有显示器（不建虚拟显示器）。0 = 主屏
    bool mirror = false;
    std::optional<quint32> displayId;
    bool noAudio = false;
    /// 流参数（0 = 未指定，按目标设备类型取默认：模拟器原始分辨率 12M 不限帧；
    /// 真机镜像 1600 上限 / 10M / 60fps——Wi-Fi 隧道带宽有限，原始分辨率
    /// 高刷直推必然卡顿，降规格换流畅。真机窗口模式保持原始分辨率）
    quint32 bitrate = 0;
    quint32 maxSize = 0;
    std::optional<float> maxFps;
    /// 无边框窗口（--borderless 或 MWINE_BORDERLESS=1；open 拉起 bundle 时
    /// 环境变量不可传，daemon 偏好经 CLI 转成参数）
    bool borderless = false;
    /// 裸流 H.264 录制（config + 帧包原样落盘，.h264 可直接播放）
    QString recordPath;
};

class ScreenWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit ScreenWidget(WindowOptions opt, QWidget* parent = nullptr);
    ~ScreenWidget() override;
    /// 窗口层关闭入口（并列布局下 HostWidget 的 closeEvent 转发到这里；
    /// 内部两段式：先藏窗+停会话，后台收尾后真关）
    void handleWindowClose(QCloseEvent* e);
    /// 注入并列的底部功能条（HostWidget 布局完成后调用；主题/图标联动用）
    void setBarWidget(BarWidget* b);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void inputMethodEvent(QInputMethodEvent* e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override;
    void closeEvent(QCloseEvent* e) override;

private:
    // ---- 会话生命周期 ----
    void beginSession();                       // 后台：ScrcpyClient::start + 接管
    void sessionUp(mw::scrcpy::Session&& s);   // 会话就绪：拆分部件、起线程（GUI 线程）
    void videoLoop(mw::scrcpy::VideoStream stream);
    void writerLoop(mw::scrcpy::ControlChannel ch);
    void deviceMsgLoop(mw::adb::TcpConn dup);
    void sessionFailed(const QString& why);    // GUI 线程：报错并关窗
    // 关闭（两段式）：GUI 线程只发停止指令并立即隐藏窗口；join 等收尾在
    // collector 线程做，完成后回 GUI 线程真正 close（closeEvent 直通）。
    // 动机：closeEvent 里同步 join 会把 adb/TCP 阻塞（最长 15s+）带进 GUI
    // 线程 → 窗口冻结无响应（boot 阶段关窗必现）。
    void requestSessionStop();                 // 非阻塞：停音频/writer、单实例下线
    // 后台线程：guard->stop（含 adb 清理，慢）+ join 全部会话线程；完成后
    // done 回 GUI 线程。closeEvent 两段式与 stall 重建共用。
    void stopSessionInBackground(const std::function<void()>& done);
    void teardown();                           // 幂等：停线程/停音频/停会话
    void restartAfterStall();                  // 纯色帧超时：重建会话（限次）
    bool tryTakeDisplaySlot();                 // 注册表抢占 console 副屏 index（1..10）
    void releaseDisplaySlot();                 // 注销（幂等）
    void setStatusText(const QString& s);      // 首帧阶段提示（GUI 线程）
    bool vmStillAlive();                       // console 端口探测：VM 在=会话中断属真异常
    void saveGeometryNow();                    // 几何记忆（QSettings，按 package）
    // ---- guest 主题跟随（顶栏白面板/暗面板）----
    void themeLoop();                          // 常驻：轮询 ui_night_mode（8s）
    void applyBarTheme(bool dark);             // GUI 线程：bar 应用主题
    // guest 输出 aspect 闭环矫正（首帧后一次；qemu 链路分辨率偏差 → 黑条根治）
    void correctDisplayAspect();

    // ---- 输入/控制 ----
    void enqueue(const mw::scrcpy::ControlMessage& msg);
    /// ⌃ 组合 → Android 系统键/动作（scrcpy 惯例）。press = 按下事件。
    /// 返回 true = 已消费，不再透传。
    bool handleSystemShortcut(QKeyEvent* e, bool press);
    void sendTextChunked(const QString& text); // inject text 300 字符上限分片
    /// 宿主输入文本注入：ASCII 走逐字符通道；非 ASCII（CJK/emoji）走
    /// setClipboard(paste) 原子粘贴（scrcpy injectText 仅收 32..127，实测
    /// 「你好ABC」只有 ABC 落地——kasmweb 式宿主 IME 方案的落地通道）
    void injectTextSmart(const QString& text);
    QPointF videoPos(const QPointF& p) const;  // 部件坐标 → 视频坐标（内容框内）
    QRectF contentRect() const;                // 视频在部件内的 letterbox 内容框
    void requestResizeDisplay();               // flex：窗口尺寸 → guest 分辨率重排

    // 并列底部功能条（非子部件；主题联动/拖动自检引用）
    BarWidget* m_barWidget = nullptr;
    friend class BarWidget;   // bar 经 host 注入键码（enqueue）

    // 视频线程 → GUI 的帧交付槽（latest-wins；const 方法内也要可锁）
    struct FrameSlot {
        mutable std::mutex mu;
        mw::scrcpy::Nv12Image img;
        quint64 seq = 0; // 0 = 无帧
    };
    void publishFrame(mw::scrcpy::Nv12Image img);
    void scheduleUpdate() const;

    // 单实例：同 package 已有窗口时，后启动进程经 local socket 唤醒前者并退出
    QLocalServer m_instanceServer;
    QString m_status;         // 首帧阶段提示（仅 GUI 线程）
    quint16 m_lastWidth = 0;  // 上次 console 分辨率（去重，避免重复 modify）
    quint16 m_lastHeight = 0;

    WindowOptions m_opt;
    std::atomic_bool m_quitting{false};
    std::atomic_bool m_tornDown{false};

    // qemu 副屏（window 模式）：console multidisplay index 与 guest display id
    int m_consoleIdx = 0;           // 0 = 未建
    quint32 m_guestDisplayId = 0;   // 0 = 未解析
    quint32 m_reuseDisplayId = 0;   // 会话重建时复用的 display id（多副屏时防误绑）
    int m_sessionAttempts = 0;      // 会话重建计数（纯色帧 stall 重试用）
    // 真机副屏（window 模式）：overlay_display_devices 里由本窗口添加的条目
    //（形如 1080x2340/420）；teardown 时从设置串里只摘掉自己，保留其他窗口条目
    QString m_realOverlaySpec;

    // 会话部件
    std::optional<mw::scrcpy::SessionGuard> m_guard;
    std::optional<mw::audio::CoreAudioBridge> m_audio;
    std::thread m_bootThread;
    std::thread m_videoThread;
    std::thread m_writerThread;
    std::thread m_devMsgThread;
    std::thread m_themeThread;    // guest 主题轮询（与关闭收尾同步 join）
    bool m_guestDark = true;      // guest ui_night_mode（白/暗面板基准）
    std::atomic<int> m_bgJoin{0}; // collector 状态：0 无 / 1 join 进行中 / 2 完成
    // 读线程强制解锁：关闭时对视频/设备消息 socket 主动 shutdown——
    // adbd 在 server 死后不关 wire 流，poll 永等（join 卡死的根源）
    qintptr m_videoFd = -1;
    qintptr m_devMsgFd = -1;

    // 控制消息发送队列（GUI 压入，writer 线程串行发送）
    std::mutex m_writeMu;
    std::condition_variable m_writeCv;
    QVector<mw::scrcpy::ControlMessage> m_writeQueue;
    bool m_writerQuit = false;
    quint64 m_clipSeq = 1; // SetClipboard 序列号

    FrameSlot m_frame;
    QTimer m_resizeTimer;
    QPointF m_imeAnchor; // 输入法候选框锚点（最近一次点击位置）

    // GL 资源
    std::optional<QOpenGLShaderProgram> m_prog;
    std::optional<QOpenGLVertexArrayObject> m_vao;
    std::optional<QOpenGLBuffer> m_vbo; // 顶点数据（core profile 禁 client-side 数组）
    QOpenGLTexture* m_texY = nullptr;  // GL_R8，宽度 = yStride
    QOpenGLTexture* m_texUV = nullptr; // GL_RG8，宽度 = uvStride/2
    quint64 m_uploadedSeq = 0;
    int m_uYTexScale = -1;
    int m_uUVTexScale = -1;
    int m_uFullRange = -1;
};

/// 独立窗口宿主：垂直布局 = ScreenWidget(视频) + BarWidget(底部功能条)。
/// bar 与视频是**并列**关系（用户拍板）——视频部件自身几何即画面区，
/// 无 overlay 补偿数学，天然互不遮挡。窗口级职责（标题/无边框/几何记忆/
/// 关闭两段式入口）都在这一层。
class HostWidget : public QWidget {
    Q_OBJECT
public:
    explicit HostWidget(WindowOptions opt, QWidget* parent = nullptr);
    ScreenWidget* screen() const { return m_screen; }

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    ScreenWidget* m_screen = nullptr;
    BarWidget* m_bar = nullptr;
};

} // namespace mwine
