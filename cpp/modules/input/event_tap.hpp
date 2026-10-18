// CGEventTap 全局键鼠捕获（macOS 特化）：宿主任意位置的系统键盘/鼠标事件
// → HostInputEvent → KeymapRunner 驱动映射引擎。
// ← crates/mwine-input/src/event_tap.rs
//
// 事实依据（SDK 头文件核对，非猜测）：
// - 事件类型：LDown=1/LUp=2/RDown=3/RUp=4/Moved=5/LDragged=6/RDragged=7/
//   KeyDown=10/KeyUp=11/FlagsChanged=12/ScrollWheel=22/OtherDown=25/OtherUp=26
//   （IOLLEvent.h NX_* 宏）
// - 字段：ButtonNumber=3/DeltaX=4/DeltaY=5/Keycode=9/
//   ScrollPointDeltaAxis1=96(纵)/Axis2=97(横)（CGEventTypes.h CGEventField）
// - 修饰键只有 FlagsChanged 事件（无独立 Down/Up），需按 flags 位与上次状态异或
//
// 权限：捕获输入事件（active tap）要求进程被授予「辅助功能」信任
// （AXIsProcessTrusted），未授权时 CGEventTapCreate 返回 NULL。
//
// 纯函数部分（keyName / classify / RawEvent）全平台可用；FFI 部分 __APPLE__ 门控。
#pragma once

#include <QPair>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "capture.hpp"
#include "engine.hpp"
#include "input.hpp"
#include "result.hpp"

namespace mw::input {

// ---- CGEventType（NX_* 宏，IOLLEvent.h）----
inline constexpr quint32 kEvtLeftMouseDown = 1;
inline constexpr quint32 kEvtLeftMouseUp = 2;
inline constexpr quint32 kEvtRightMouseDown = 3;
inline constexpr quint32 kEvtRightMouseUp = 4;
inline constexpr quint32 kEvtMouseMoved = 5;
inline constexpr quint32 kEvtLeftMouseDragged = 6;
inline constexpr quint32 kEvtRightMouseDragged = 7;
inline constexpr quint32 kEvtKeyDown = 10;
inline constexpr quint32 kEvtKeyUp = 11;
inline constexpr quint32 kEvtFlagsChanged = 12;
inline constexpr quint32 kEvtScrollWheel = 22;
inline constexpr quint32 kEvtOtherMouseDown = 25;
inline constexpr quint32 kEvtOtherMouseUp = 26;
inline constexpr quint32 kEvtTapDisabledByTimeout = 0xFFFFFFFE;
inline constexpr quint32 kEvtTapDisabledByUserInput = 0xFFFFFFFF;

// ---- CGEventField ----
inline constexpr quint32 kFieldMouseButtonNumber = 3;
inline constexpr quint32 kFieldMouseDeltaX = 4;
inline constexpr quint32 kFieldMouseDeltaY = 5;
inline constexpr quint32 kFieldKeyboardAutorepeat = 8;
inline constexpr quint32 kFieldKeyboardKeycode = 9;
inline constexpr quint32 kFieldScrollPointDeltaAxis1 = 96;
inline constexpr quint32 kFieldScrollPointDeltaAxis2 = 97;

// ---- 修饰键 flags 位（IOLLEvent.h NX_*MASK）----
inline constexpr quint64 kMaskAlphaShift = 0x00010000;
inline constexpr quint64 kMaskShift = 0x00020000;
inline constexpr quint64 kMaskControl = 0x00040000;
inline constexpr quint64 kMaskAlternate = 0x00080000;
inline constexpr quint64 kMaskCommand = 0x00100000;

/// macOS 虚拟键码 → 键名（与 keymap 档案命名一致：
/// 字母大写单字符、"Space"/"Enter"/"ShiftLeft"/"MouseRight"/"Up"…）。
/// 返回空串 = 未收录键码（静默忽略）。
QString keyName(quint32 vk);

/// 修饰键码 → (键名, flags 位)。FlagsChanged 的 Down/Up 判定用。
/// 返回 false = 非修饰键。
bool modifierOf(quint32 vk, QString* name, quint64* mask);

enum class HostKeyAction { Down, Up };

struct HostMouseButton {
    enum class Kind { Left, Right, Middle, Other };
    Kind kind = Kind::Left;
    quint8 other = 0; ///< Kind::Other 时的按钮号

    /// keymap 档案键名（"MouseLeft"/"MouseRight"/"MouseMiddle"）。
    QString keyName() const;
};

/// 宿主输入事件（已归一化，与 CGEvent 细节解耦）。
struct HostInputEvent {
    enum class Kind { Key, MouseButton, MouseMoved, Scroll };

    Kind kind = Kind::Key;
    // Key：键盘（含修饰键——由 FlagsChanged 推导）。
    QString key;
    HostKeyAction action = HostKeyAction::Down;
    bool autorepeat = false; ///< OS 按住重复
    quint64 mods = 0;        ///< 事件 flags（Command/Control 位），用于系统组合键放行
    // MouseButton
    HostMouseButton button;
    // MouseMoved：全局坐标 + 增量
    double x = 0.0;
    double y = 0.0;
    double dx = 0.0;
    double dy = 0.0;
    // Scroll：像素增量；Axis1 纵、Axis2 横，符号遵循系统自然滚动设置
    double scrollDx = 0.0;
    double scrollDy = 0.0;

    /// 工厂名带 Event 后缀：与字段 key 同名不能共存。
    static HostInputEvent keyEvent(QString key, HostKeyAction action, bool autorepeat,
                                   quint64 mods)
    {
        HostInputEvent e;
        e.kind = Kind::Key;
        e.key = std::move(key);
        e.action = action;
        e.autorepeat = autorepeat;
        e.mods = mods;
        return e;
    }
    static HostInputEvent mouseButton(HostMouseButton button, HostKeyAction action, quint64 mods)
    {
        HostInputEvent e;
        e.kind = Kind::MouseButton;
        e.button = button;
        e.action = action;
        e.mods = mods;
        return e;
    }
    static HostInputEvent mouseMoved(double x, double y, double dx, double dy)
    {
        HostInputEvent e;
        e.kind = Kind::MouseMoved;
        e.x = x;
        e.y = y;
        e.dx = dx;
        e.dy = dy;
        return e;
    }
    static HostInputEvent scroll(double dx, double dy)
    {
        HostInputEvent e;
        e.kind = Kind::Scroll;
        e.scrollDx = dx;
        e.scrollDy = dy;
        return e;
    }
};

/// CGEvent 提取后的原始值（classify 的输入，与 FFI 解耦便于测试）。
struct RawEvent {
    quint32 eventType = 0;
    qint64 keycode = 0;
    quint64 flags = 0;
    bool autorepeat = false;
    qint64 buttonNumber = 0;
    double dx = 0.0;
    double dy = 0.0;
    qint64 scrollDx = 0;
    qint64 scrollDy = 0;
};

/// 事件分类（纯函数）：从 CGEvent 提取的原始值 → 归一化事件。
/// 返回空 = 与输入捕获无关的事件类型。
std::optional<HostInputEvent> classify(const RawEvent& raw);

/// 进程是否已被授予辅助功能信任（未授权时 active tap 创建失败）。
bool isProcessTrusted();

/// 弹出系统授权引导对话框（不阻塞，返回当前信任状态）。
bool requestTrustPrompt();

/// 捕获配置。
struct TapConfig {
    /// 捕获键盘（含修饰键 FlagsChanged）
    bool keyboard = true;
    /// 捕获鼠标（按键/移动/滚轮）
    bool mouse = false;
    /// true = 拦截模式（handler 返回 true 的事件被吞掉，宿主无感）；
    /// false = 纯监听（所有事件原样放行）
    bool active = true;
};

/// 捕获 handler：返回 true 且 active 时该事件被拦截（宿主收不到）。
using TapHandler = std::function<bool(const HostInputEvent&)>;

/// 运行中的事件捕获句柄。stop()/析构时停止捕获线程。
class EventTapHandle {
public:
    EventTapHandle() = default;
    ~EventTapHandle() { stop(); }

    EventTapHandle(const EventTapHandle&) = delete;
    EventTapHandle& operator=(const EventTapHandle&) = delete;
    EventTapHandle(EventTapHandle&& other) noexcept;
    EventTapHandle& operator=(EventTapHandle&& other) noexcept;

    /// 停止捕获（幂等）。
    void stop();

    bool valid() const { return m_thread.joinable() || m_impl != nullptr; }

private:
    friend mw::Result<EventTapHandle> spawnTap(TapConfig config, TapHandler handler);
    struct Impl;
    std::shared_ptr<Impl> m_impl; // Apple: runloop/stopped 共享态；其他平台为空
    std::thread m_thread;
};

/// 创建全局事件捕获：后台线程跑独立 runloop，事件经归一化后回调 handler。
/// handler 返回 true 且 config.active 时该事件被拦截（宿主收不到）。
/// 非 Apple 平台恒返回 Err（TODO）。
mw::Result<EventTapHandle> spawnTap(TapConfig config, TapHandler handler);

// ---- KeymapRunner：tap + 映射引擎 + 注入后端 三件套 ----

/// 把全局键鼠事件喂给按键映射引擎的运行器（吞掉策略在此层）。
///
/// - 引擎命中绑定（或总开关键）的事件被吞掉，宿主无感
/// - OS 自动重复被丢弃（连点由引擎定时器自驱动）
/// - Command/Ctrl 组合键始终放行（保住 Cmd-Tab 等系统操作）
/// - FPS 捕获模式：鼠标事件全吞 + 断开光标关联（进入时）/
///   恢复（退出时，含总开关关闭与进程退出）
/// - free 模式：鼠标位置换算到模拟器窗口归一化坐标供技能瞄准
class KeymapRunner : public std::enable_shared_from_this<KeymapRunner> {
public:
    explicit KeymapRunner(std::shared_ptr<Engine> engine);

    /// 引擎引用（查询状态/手动 shutdown）。
    const std::shared_ptr<Engine>& engine() const { return m_engine; }

    /// 处理单个宿主事件。返回值 = 是否吞掉该事件。
    /// 也可脱离 tap 单独调用（喂自定义事件源，如窗口内 NSEvent）。
    bool handleEvent(const HostInputEvent& ev);

    /// 更新 Activity 门状态（外部轮询 guest 前台后调用）。
    void setActivityOk(bool ok);

    /// 焦点门控旁路（单元测试环境无 qemu 窗口时置 true）。
    void setFocusGateOff(bool off) { m_focusGateOff = off; }

    /// 停机清理：释放虚拟手指 + 恢复光标。
    void shutdown();

    /// 挂载为全局捕获（active tap）。返回句柄，其销毁即卸载。
    mw::Result<EventTapHandle> spawnGlobal(bool includeMouse);

    ~KeymapRunner();

private:
    bool engineKeyBound(const QString& key);
    std::optional<QPair<double, double>> mousePosNorm(double gx, double gy);
    void syncCapture();

    std::shared_ptr<Engine> m_engine;
    std::mutex m_innerMutex;
    /// 当前是否处于光标断开状态（associate 开关的去重）
    bool m_disassociated = false;
    /// 模拟器窗口 frame 缓存（全局屏幕坐标）
    std::optional<capture::Rect> m_frame;
    std::chrono::steady_clock::time_point m_frameAt;
    /// 模拟器窗口前台态缓存（焦点门控）
    std::optional<bool> m_front;
    std::chrono::steady_clock::time_point m_frontAt;
    /// Activity 门：目标应用的指定 Activity（如游戏对局页）不在前台时挂起映射。
    std::atomic<bool> m_activityOk{true};
    /// 焦点门控旁路（测试环境无 qemu 窗口）
    bool m_focusGateOff = false;
};

} // namespace mw::input
