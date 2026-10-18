// mw::input（输入桥）：macOS 键鼠事件 → Android 触控/按键注入。
// ← crates/mwine-input/src/lib.rs
//
// 分层：
// - InputInjector（本文件）：统一注入接口，displayId 定向
// - scrcpy.hpp（ScrcpyInjector）：快路径（scrcpy 控制通道，毫秒级；会话绑定单个显示器）
// - adb.hpp（AdbInjector）：低速备选（`adb shell input`，无 server 依赖，任何显示器）
// - console.hpp（ConsoleInjector）：模拟器 console `event send` 通道（本机最优延迟）
// - InputRouter：多显示器分发（mwine-core 组装用）
// - event_tap.hpp（macOS）：CGEventTap 全局键鼠捕获 → KeymapRunner 驱动按键映射
// - engine.hpp：mwine-keymap 映射引擎（宿主事件 → 触摸流）
//
// 关键事实：scrcpy control-only 会话会退回主屏（displayId 失效），
// 定向副屏必须携带最低开销视频（320px@1fps）把会话锚到目标显示器——
// 该逻辑封装在 ScrcpyInjector::connect。
#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <memory>
#include <mutex>
#include <unordered_map>

#include "engine.hpp"
#include "result.hpp"

namespace mw::input {

// ---- 错误文案构造（Rust enum InputError → QString 约定）----
/// 该后端未绑定目标显示器（如 scrcpy 会话锚定 display 2，却被要求注入 display 0）
inline QString errDisplayNotBound(quint32 display)
{
    return QStringLiteral("注入后端未绑定显示器 %1").arg(display);
}
inline QString errScrcpy(const QString& m) { return QStringLiteral("scrcpy 通道错误: ") + m; }
inline QString errAdb(const QString& m) { return QStringLiteral("adb 通道错误: ") + m; }
inline QString errUnsupported(const QString& m) { return QStringLiteral("该后端不支持此操作: ") + m; }
inline QString errText(const QString& m) { return QStringLiteral("文本注入失败: ") + m; }
inline QString errEventTap(const QString& m) { return QStringLiteral("事件捕获（CGEventTap）错误: ") + m; }
inline QString errConsole(const QString& m) { return QStringLiteral("模拟器 console 通道错误: ") + m; }
inline QString errIo(const QString& m) { return QStringLiteral("io 错误: ") + m; }

/// 按键动作（AKEYEVENT_ACTION_DOWN/UP）。
enum class KeyAction { Down, Up };

inline qint32 keyActionI32(KeyAction a) { return a == KeyAction::Down ? 0 : 1; }

/// 触摸动作（对应 AMOTION_EVENT_ACTION_DOWN/MOVE/UP）。
enum class TouchAction { Down, Move, Up };

/// 线协议动作位（AMOTION_EVENT_ACTION_*：Down=0、Up=1、Move=2）。
inline quint8 touchActionU8(TouchAction a)
{
    switch (a) {
    case TouchAction::Down: return 0;
    case TouchAction::Up: return 1;
    case TouchAction::Move: return 2;
    }
    return 0;
}

/// Android 按键码（AOSP KeyEvent.java AKEYCODE_* 子集，本项目所需）。
/// 纯注入逻辑不依赖任何外部类型；toI32 与 guest 端数值一一对应。
struct AndroidKeycode {
    enum class K {
        Unknown, Home, Back, AppSwitch, Menu, Enter, Backspace, Del, Escape, Space, Tab,
        DpadUp, DpadDown, DpadLeft, DpadRight, DpadCenter, VolumeUp, VolumeDown, VolumeMute,
        Power, Camera, Focus, ShiftLeft, ShiftRight, AltLeft, AltRight, CtrlLeft, CtrlRight,
        MetaLeft, MetaRight, CapsLock, PageUp, PageDown, MoveHome, MoveEnd, Insert,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        Comma, Period, Minus, Equals, LeftBracket, RightBracket, Backslash, Semicolon,
        Apostrophe, Slash, Grave, At, Plus, Asterisk, Hash, Char,
    };
    K k = K::Unknown;
    char32_t ch = 0; ///< K::Char 时有效

    /// 可打印 ASCII 字符 → 按键码。
    static AndroidKeycode fromChar(char32_t c);
    /// 从 AKEYCODE 数值反解（用于事件回读/日志）。
    static AndroidKeycode fromI32(qint32 code);
    qint32 toI32() const;

    bool operator==(const AndroidKeycode& o) const { return k == o.k && ch == o.ch; }
    bool operator!=(const AndroidKeycode& o) const { return !(*this == o); }
};

/// 统一注入接口（M3 核心接口）。
///
/// 坐标系：guest 像素坐标（与 `wm size` 一致）。
/// scrcpy 后端按会话创建时的分辨率做归一化，分辨率变化需重建会话。
class InputInjector {
public:
    virtual ~InputInjector() = default;

    /// 注入触摸事件。pointerId 多指时各指不同（scrcpy 惯例鼠标 = u64::MAX）。
    virtual mw::Result<bool> injectTouch(quint32 displayId, TouchAction action, quint64 pointerId,
                                         qint32 x, qint32 y) = 0;
    /// 注入按键（Down/Up 分离；adb 备选后端退化为完整按压，见其文档）。
    virtual mw::Result<bool> injectKey(const AndroidKeycode& keycode, KeyAction action) = 0;
    /// 注入文本（IME 桥基础；超长自动分块）。
    virtual mw::Result<bool> injectText(const QString& text) = 0;
    /// 注入滚轮。h/v ∈ [-1,1]，与 scrcpy InjectScroll 语义一致。
    virtual mw::Result<bool> injectScroll(quint32 displayId, qint32 x, qint32 y, float h,
                                          float v) = 0;
};

/// 多显示器路由：touch/scroll 按 displayId 分发到绑定的后端
/// （未绑定的 displayId 落到 default 后端，由其自行决定支持或报
/// errDisplayNotBound）；key/text 恒走 default。
///
/// 典型组装：default = AdbInjector（全显示器低速兜底），
/// 每个直通窗口 bind 一个 ScrcpyInjector。
class InputRouter : public InputInjector {
public:
    explicit InputRouter(std::unique_ptr<InputInjector> defaultInjector)
        : m_default(std::move(defaultInjector))
    {
    }

    /// 绑定定向后端（重复绑定覆盖旧的）。
    void bindDisplay(quint32 displayId, std::unique_ptr<InputInjector> injector)
    {
        m_perDisplay.insert_or_assign(displayId, std::move(injector));
    }
    /// 解绑（后端销毁，scrcpy 会话随之清理）。
    bool unbindDisplay(quint32 displayId) { return m_perDisplay.erase(displayId) > 0; }
    QVector<quint32> boundDisplays() const
    {
        QVector<quint32> out;
        out.reserve(int(m_perDisplay.size()));
        for (const auto& kv : m_perDisplay)
            out.append(kv.first);
        return out;
    }

    mw::Result<bool> injectTouch(quint32 displayId, TouchAction action, quint64 pointerId,
                                 qint32 x, qint32 y) override;
    mw::Result<bool> injectKey(const AndroidKeycode& keycode, KeyAction action) override;
    mw::Result<bool> injectText(const QString& text) override;
    mw::Result<bool> injectScroll(quint32 displayId, qint32 x, qint32 y, float h,
                                  float v) override;

private:
    InputInjector* forDisplay(quint32 displayId)
    {
        const auto it = m_perDisplay.find(displayId);
        return it != m_perDisplay.end() ? it->second.get() : m_default.get();
    }

    std::unique_ptr<InputInjector> m_default;
    // unordered_map：QHash 的 insert 重载会拷贝 value（unique_ptr 不可拷贝）
    std::unordered_map<quint32, std::unique_ptr<InputInjector>> m_perDisplay;
};

/// 线程共享的注入后端（对应 Rust Arc<Mutex<Box<dyn InputInjector>>>）。
struct SharedInjector {
    std::mutex mutex;
    std::unique_ptr<InputInjector> injector;
    explicit SharedInjector(std::unique_ptr<InputInjector> i) : injector(std::move(i)) {}
};

/// 桥接器：把 Engine 产出的多点触控流导向任意 InputInjector 后端
/// （scrcpy 快路径 / adb 备选 / console），统一注入出口。
/// 引擎回调可能来自定时器线程，内部加锁转发。
class EngineSink : public TouchSink {
public:
    EngineSink(std::shared_ptr<SharedInjector> injector, quint32 displayId)
        : m_injector(std::move(injector)), m_displayId(displayId)
    {
    }

    void touch(mw::keymap::TouchAction action, quint64 pointer, qint32 x, qint32 y) override;

private:
    std::shared_ptr<SharedInjector> m_injector;
    quint32 m_displayId = 0;
};

} // namespace mw::input
