// 映射引擎：宿主键鼠事件 → Android 多点触控流。
// ← crates/mwine-keymap/src/engine.rs（运行时引擎归 input 模块，
//    数据模型复用已落地的 mw::keymap —— 不改 keymap 模块文件）
//
// 纯逻辑（无 macOS / 网络依赖），输入 InputEvent，输出经 TouchSink。
// 连点 / 滑动插值 / 宏等延时行为由内部定时器驱动：
// - Engine：带后台工作线程（2ms 轮询驱动定时器），生产用
// - EngineCore：裸状态机，runDue(now) 手动推进，测试用（可注入假时钟）
//
// 多指：每个虚拟手指一个稳定 pointerId（从 1001 递增分配，避开
// 注入后端约定），摇杆/技能/开火/视角可同时按住。
#pragma once

#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <atomic>

#include "keymap.hpp"
#include "result.hpp"

namespace mw::input {

using Screen = QPair<quint16, quint16>; ///< (width, height) guest 像素

/// 引擎输入事件（宿主侧归一化）。
struct InputEvent {
    enum class Kind { Key, MouseMove, Scroll };

    Kind kind = Kind::Key;
    // Key
    QString key;
    bool down = true;
    // MouseMove：宿主像素增量 + 当前位置（归一化到 Android 屏，窗口外为空）
    double dx = 0.0;
    double dy = 0.0;
    bool hasPos = false;
    double posX = 0.0;
    double posY = 0.0;
    // Scroll：像素增量（dy<0 = 上滚）
    double scrollDy = 0.0;

    /// 工厂名带 Event 后缀：字段 key 与同名成员函数不能共存
    ///（Rust 侧为 Event::Key 枚举变体，无此约束）。
    static InputEvent keyEvent(QString key, bool down)
    {
        InputEvent e;
        e.kind = Kind::Key;
        e.key = std::move(key);
        e.down = down;
        return e;
    }
    static InputEvent mouseMove(double dx, double dy, bool hasPos, double px, double py)
    {
        InputEvent e;
        e.kind = Kind::MouseMove;
        e.dx = dx;
        e.dy = dy;
        e.hasPos = hasPos;
        e.posX = px;
        e.posY = py;
        return e;
    }
    static InputEvent scroll(double dy)
    {
        InputEvent e;
        e.kind = Kind::Scroll;
        e.scrollDy = dy;
        return e;
    }
};

/// 触摸输出（坐标已换算为 guest 像素）。
class TouchSink {
public:
    virtual ~TouchSink() = default;
    virtual void touch(mw::keymap::TouchAction action, quint64 pointer, qint32 x, qint32 y) = 0;
};

/// 虚拟手指指针 id 起点（预留低位给系统/鼠标指针约定）。
inline constexpr quint64 kFirstPointer = 1001;
/// 滑动插值帧间隔（ms）
inline constexpr quint64 kSwipeFrameMs = 16;
/// 滚轮触发节流（ms）
inline constexpr int kScrollThrottleMs = 60;
/// 方向技能取消施法半径（归一化，相对短边）
inline constexpr double kCancelRadius = 0.06;

// ---- 摇杆方向位 ----
inline constexpr quint8 kDirUp = 1 << 0;
inline constexpr quint8 kDirDown = 1 << 1;
inline constexpr quint8 kDirLeft = 1 << 2;
inline constexpr quint8 kDirRight = 1 << 3;

/// 摇杆方向组合 → 目标点（归一化坐标）。对角按 45° 归一，模拟真实摇杆。
/// radius 相对屏幕短边（按像素空间计算后折回归一化，避免 x/y 轴比例失真）。
QPair<double, double> joystickTarget(QPair<double, double> center, double radius, quint8 dirs,
                                     Screen screen);

/// 裸状态机（无线程）。所有方法需外部串行化。
/// 时钟可注入（默认 steady_clock）——测试用假时钟保证调度确定性。
class EngineCore {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using ClockFn = std::function<TimePoint()>;

    EngineCore(mw::keymap::Profile profile, Screen screen, ClockFn clock = nullptr);

    const mw::keymap::Profile& profile() const { return m_profile; }
    Screen screen() const { return m_screen; }
    bool active() const { return m_active; }
    /// FPS 鼠标捕获态（active && capture 模式）
    bool captured() const { return m_active && m_profile.mouseMode == mw::keymap::MouseMode::Capture; }

    /// 处理宿主事件。返回 true = 该事件应被宿主吞掉。
    bool handleEvent(const InputEvent& ev, TouchSink* sink);

    /// 推进到 now：执行所有到期定时器。可能级联入队新定时器。
    void runDue(TimePoint now, TouchSink* sink);

    /// 下一个定时器到期时间（工作线程休眠用）。
    std::optional<TimePoint> nextDeadline() const;

    // ---- 总开关 ----
    bool toggle(TouchSink* sink);
    /// 释放全部虚拟手指并清空延时状态（停用/退出时调用）。
    void releaseAll(TouchSink* sink);

private:
    struct Held {
        quint64 pointer = 0;
        int node = -1;
        enum class Kind { Hold, Toggle, SkillAim } kind = Kind::Hold;
    };
    struct JoyState {
        quint64 pointer = 0;
        quint8 dirs = 0;
        QPair<double, double> px{0.0, 0.0}; // 当前位置（guest 像素）
    };
    struct LookState {
        std::optional<quint64> pointer;
        QPair<double, double> pos{0.5, 0.4}; // 归一化
    };
    struct Timer {
        enum class Kind { RepeatTap, MacroStep, SwipeFrame } kind = Kind::RepeatTap;
        // RepeatTap：key 仍处于按住（timers 是连点是否存活的唯一登记处）
        QString key;
        int node = -1;
        // MacroStep
        quint64 run = 0;
        // SwipeFrame
        quint64 pointer = 0;
        QPair<double, double> from{0.0, 0.0};
        QPair<double, double> to{0.0, 0.0};
        quint32 idx = 0;
        quint32 total = 0;
        bool last = false; // 最后一帧附加 Up
    };
    struct MacroRun {
        int node = -1;
        int step = 0;
        /// TouchDown 步骤占用的指针（配对 TouchUp）
        std::optional<quint64> pointer;
    };

    TimePoint now() const { return m_clock ? m_clock() : Clock::now(); }
    quint64 allocPointer();
    QPair<qint32, qint32> toPx(double x, double y) const;
    double minSidePx() const;
    QPair<double, double> nodePos(int idx) const;
    void emit(TouchSink* sink, mw::keymap::TouchAction action, quint64 pointer,
              QPair<double, double> px);
    bool onKey(const QString& key, bool down, TouchSink* sink);
    void triggerNode(int idx, const QString& key, bool down, TouchSink* sink);
    std::optional<QPair<double, double>> aimTarget(QPair<double, double> pos, double dragRadius) const;
    void tapNow(TouchSink* sink, QPair<double, double> pos);
    void swipeNow(TouchSink* sink, QPair<double, double> from, QPair<double, double> to,
                  quint32 durationMs, std::optional<quint64> boundPointer);
    void joystickDir(quint8 bit, bool down, TouchSink* sink);
    bool onMouseMove(double dx, double dy, std::optional<QPair<double, double>> pos, TouchSink* sink);
    void lookMove(double dx, double dy, TouchSink* sink);
    bool onScroll(double dy, TouchSink* sink);
    void advanceMacro(quint64 run, TouchSink* sink);

    mw::keymap::Profile m_profile;
    Screen m_screen;
    ClockFn m_clock;
    bool m_active = true;
    quint64 m_nextPointer = kFirstPointer;
    std::optional<JoyState> m_joystick;
    std::optional<int> m_joystickNode;
    QHash<QString, quint8> m_joystickKeys;
    /// 按住中的手指（hold/toggle/skill-aim）
    QHash<QString, Held> m_held;
    LookState m_look;
    QVector<QPair<TimePoint, Timer>> m_timers;
    QHash<quint64, MacroRun> m_macros;
    quint64 m_nextMacroRun = 0;
    /// 鼠标在 Android 屏上的归一化位置（free 模式技能瞄准用）
    std::optional<QPair<double, double>> m_mousePos;
    std::optional<TimePoint> m_lastScroll;
    /// 物理 key → 节点下标（一键多节点：全部触发）
    QHash<QString, QVector<int>> m_keyIndex;
    std::optional<int> m_wheelNode;
};

/// 共享 sink：Engine 与调用方共用一把锁保护同一 TouchSink
///（对应 Rust 的 Arc<Mutex<dyn TouchSink>>）。
struct SharedSink {
    std::shared_ptr<TouchSink> sink;
    std::shared_ptr<std::mutex> mutex;
};

/// 生产用引擎：包装 EngineCore，后台线程驱动连点/滑动/宏定时器。
///
/// 线程安全：handleEvent 可从任意线程调用（内部互斥）。
/// 锁序恒为 core → sink。
class Engine {
public:
    /// 创建并启动定时器线程（对应 Rust Engine::new → Arc<Self>）。
    static std::shared_ptr<Engine> create(mw::keymap::Profile profile, Screen screen,
                                          SharedSink sink);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// 处理宿主事件，返回是否吞掉。
    bool handleEvent(const InputEvent& ev);
    bool active() const;
    bool captured() const;
    QString switchKey() const;
    /// 释放全部虚拟手指（摇杆/技能/视角），映射保持启用。
    /// 焦点门控：模拟器窗口失焦时调用，避免游戏里出现"按住不放"。
    void releaseFingers();
    /// 档案快照（吞键预判等轻量查询用）。
    mw::keymap::Profile profileSnapshot() const;
    Screen screen() const;
    /// 停止：结束定时器线程并释放全部虚拟手指。
    void shutdown();

private:
    Engine(std::unique_ptr<EngineCore> core, SharedSink sink);

    mutable std::mutex m_coreMutex;
    std::unique_ptr<EngineCore> m_core;
    SharedSink m_sink;
    std::atomic<bool> m_stop{false};
    std::thread m_worker;
};

} // namespace mw::input
