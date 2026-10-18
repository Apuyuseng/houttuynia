// 事件捕获与 KeymapRunner 实现 ← crates/mwine-input/src/event_tap.rs
#include "event_tap.hpp"

#include <algorithm>
#include <chrono>

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace mw::input {

// ---- 纯函数（全平台）----

QString keyName(quint32 vk)
{
    switch (vk) {
    case 0x00: return QStringLiteral("A");
    case 0x01: return QStringLiteral("S");
    case 0x02: return QStringLiteral("D");
    case 0x03: return QStringLiteral("F");
    case 0x04: return QStringLiteral("H");
    case 0x05: return QStringLiteral("G");
    case 0x06: return QStringLiteral("Z");
    case 0x07: return QStringLiteral("X");
    case 0x08: return QStringLiteral("C");
    case 0x09: return QStringLiteral("V");
    case 0x0B: return QStringLiteral("B");
    case 0x0C: return QStringLiteral("Q");
    case 0x0D: return QStringLiteral("W");
    case 0x0E: return QStringLiteral("E");
    case 0x0F: return QStringLiteral("R");
    case 0x10: return QStringLiteral("Y");
    case 0x11: return QStringLiteral("T");
    case 0x12: return QStringLiteral("1");
    case 0x13: return QStringLiteral("2");
    case 0x14: return QStringLiteral("3");
    case 0x15: return QStringLiteral("4");
    case 0x16: return QStringLiteral("6");
    case 0x17: return QStringLiteral("5");
    case 0x18: return QStringLiteral("Equals");
    case 0x19: return QStringLiteral("9");
    case 0x1A: return QStringLiteral("7");
    case 0x1B: return QStringLiteral("Minus");
    case 0x1C: return QStringLiteral("8");
    case 0x1D: return QStringLiteral("0");
    case 0x1E: return QStringLiteral("RightBracket");
    case 0x1F: return QStringLiteral("O");
    case 0x20: return QStringLiteral("U");
    case 0x21: return QStringLiteral("LeftBracket");
    case 0x22: return QStringLiteral("I");
    case 0x23: return QStringLiteral("P");
    case 0x24: return QStringLiteral("Enter");
    case 0x25: return QStringLiteral("L");
    case 0x26: return QStringLiteral("J");
    case 0x27: return QStringLiteral("Apostrophe");
    case 0x28: return QStringLiteral("K");
    case 0x29: return QStringLiteral("Semicolon");
    case 0x2A: return QStringLiteral("Backslash");
    case 0x2B: return QStringLiteral("Comma");
    case 0x2C: return QStringLiteral("Slash");
    case 0x2D: return QStringLiteral("N");
    case 0x2E: return QStringLiteral("M");
    case 0x2F: return QStringLiteral("Period");
    case 0x30: return QStringLiteral("Tab");
    case 0x31: return QStringLiteral("Space");
    case 0x32: return QStringLiteral("Grave");
    case 0x33: return QStringLiteral("Backspace");
    case 0x35: return QStringLiteral("Escape");
    case 0x36: return QStringLiteral("CommandRight");
    case 0x37: return QStringLiteral("CommandLeft");
    case 0x38: return QStringLiteral("ShiftLeft");
    case 0x39: return QStringLiteral("CapsLock");
    case 0x3A: return QStringLiteral("OptionLeft");
    case 0x3B: return QStringLiteral("ControlLeft");
    case 0x3C: return QStringLiteral("ShiftRight");
    case 0x3D: return QStringLiteral("OptionRight");
    case 0x3E: return QStringLiteral("ControlRight");
    case 0x3F: return QStringLiteral("Function");
    case 0x40: return QStringLiteral("F17");
    case 0x41: return QStringLiteral("KeypadDecimal");
    case 0x43: return QStringLiteral("KeypadMultiply");
    case 0x45: return QStringLiteral("KeypadPlus");
    case 0x47: return QStringLiteral("KeypadClear");
    case 0x48: return QStringLiteral("VolumeUp");
    case 0x49: return QStringLiteral("VolumeDown");
    case 0x4A: return QStringLiteral("Mute");
    case 0x4B: return QStringLiteral("KeypadDivide");
    case 0x4C: return QStringLiteral("KeypadEnter");
    case 0x4E: return QStringLiteral("KeypadMinus");
    case 0x4F: return QStringLiteral("F18");
    case 0x50: return QStringLiteral("F19");
    case 0x51: return QStringLiteral("KeypadEquals");
    case 0x52: return QStringLiteral("Keypad0");
    case 0x53: return QStringLiteral("Keypad1");
    case 0x54: return QStringLiteral("Keypad2");
    case 0x55: return QStringLiteral("Keypad3");
    case 0x56: return QStringLiteral("Keypad4");
    case 0x57: return QStringLiteral("Keypad5");
    case 0x58: return QStringLiteral("Keypad6");
    case 0x59: return QStringLiteral("Keypad7");
    case 0x5A: return QStringLiteral("F20");
    case 0x5B: return QStringLiteral("Keypad8");
    case 0x5C: return QStringLiteral("Keypad9");
    case 0x60: return QStringLiteral("F5");
    case 0x61: return QStringLiteral("F6");
    case 0x62: return QStringLiteral("F7");
    case 0x63: return QStringLiteral("F3");
    case 0x64: return QStringLiteral("F8");
    case 0x65: return QStringLiteral("F9");
    case 0x67: return QStringLiteral("F11");
    case 0x69: return QStringLiteral("F13");
    case 0x6A: return QStringLiteral("F16");
    case 0x6B: return QStringLiteral("F14");
    case 0x6D: return QStringLiteral("F10");
    case 0x6F: return QStringLiteral("F12");
    case 0x71: return QStringLiteral("F15");
    case 0x72: return QStringLiteral("Help");
    case 0x73: return QStringLiteral("Home");
    case 0x74: return QStringLiteral("PageUp");
    case 0x75: return QStringLiteral("ForwardDelete");
    case 0x76: return QStringLiteral("F4");
    case 0x77: return QStringLiteral("End");
    case 0x78: return QStringLiteral("F2");
    case 0x79: return QStringLiteral("PageDown");
    case 0x7A: return QStringLiteral("F1");
    case 0x7B: return QStringLiteral("Left");
    case 0x7C: return QStringLiteral("Right");
    case 0x7D: return QStringLiteral("Down");
    case 0x7E: return QStringLiteral("Up");
    default: return QString();
    }
}

bool modifierOf(quint32 vk, QString* name, quint64* mask)
{
    switch (vk) {
    case 0x38: *name = QStringLiteral("ShiftLeft"); *mask = kMaskShift; return true;
    case 0x3C: *name = QStringLiteral("ShiftRight"); *mask = kMaskShift; return true;
    case 0x3B: *name = QStringLiteral("ControlLeft"); *mask = kMaskControl; return true;
    case 0x3E: *name = QStringLiteral("ControlRight"); *mask = kMaskControl; return true;
    case 0x3A: *name = QStringLiteral("OptionLeft"); *mask = kMaskAlternate; return true;
    case 0x3D: *name = QStringLiteral("OptionRight"); *mask = kMaskAlternate; return true;
    case 0x37: *name = QStringLiteral("CommandLeft"); *mask = kMaskCommand; return true;
    case 0x36: *name = QStringLiteral("CommandRight"); *mask = kMaskCommand; return true;
    case 0x39: *name = QStringLiteral("CapsLock"); *mask = kMaskAlphaShift; return true;
    default: return false;
    }
}

QString HostMouseButton::keyName() const
{
    switch (kind) {
    case Kind::Left: return QStringLiteral("MouseLeft");
    case Kind::Right: return QStringLiteral("MouseRight");
    case Kind::Middle: return QStringLiteral("MouseMiddle");
    case Kind::Other: return QStringLiteral("Mouse%1").arg(other);
    }
    return QStringLiteral("MouseLeft");
}

std::optional<HostInputEvent> classify(const RawEvent& raw)
{
    switch (raw.eventType) {
    case kEvtKeyDown:
    case kEvtKeyUp: {
        const QString key = keyName(quint32(raw.keycode));
        if (key.isEmpty())
            return std::nullopt;
        return HostInputEvent::keyEvent(key,
                                   raw.eventType == kEvtKeyDown ? HostKeyAction::Down
                                                                : HostKeyAction::Up,
                                   raw.autorepeat && raw.eventType == kEvtKeyDown, raw.flags);
    }
    case kEvtFlagsChanged: {
        QString key;
        quint64 mask = 0;
        if (!modifierOf(quint32(raw.keycode), &key, &mask))
            return std::nullopt;
        return HostInputEvent::keyEvent(key, (raw.flags & mask) != 0 ? HostKeyAction::Down
                                                                : HostKeyAction::Up,
                                   false, raw.flags);
    }
    case kEvtLeftMouseDown:
    case kEvtLeftMouseUp:
        return HostInputEvent::mouseButton(HostMouseButton{ HostMouseButton::Kind::Left, 0 },
                                           raw.eventType == kEvtLeftMouseUp ? HostKeyAction::Up
                                                                            : HostKeyAction::Down,
                                           raw.flags);
    case kEvtRightMouseDown:
    case kEvtRightMouseUp:
        return HostInputEvent::mouseButton(HostMouseButton{ HostMouseButton::Kind::Right, 0 },
                                           raw.eventType == kEvtRightMouseUp ? HostKeyAction::Up
                                                                             : HostKeyAction::Down,
                                           raw.flags);
    case kEvtOtherMouseDown:
    case kEvtOtherMouseUp: {
        HostMouseButton b;
        if (raw.buttonNumber == 2) {
            b.kind = HostMouseButton::Kind::Middle;
        } else {
            b.kind = HostMouseButton::Kind::Other;
            b.other = quint8(raw.buttonNumber);
        }
        return HostInputEvent::mouseButton(
            b, raw.eventType == kEvtOtherMouseUp ? HostKeyAction::Up : HostKeyAction::Down,
            raw.flags);
    }
    case kEvtMouseMoved:
    case kEvtLeftMouseDragged:
    case kEvtRightMouseDragged:
        return HostInputEvent::mouseMoved(0.0, 0.0, raw.dx, raw.dy);
    case kEvtScrollWheel:
        return HostInputEvent::scroll(double(raw.scrollDx), double(raw.scrollDy));
    default:
        return std::nullopt;
    }
}

// ---- FFI（__APPLE__ 门控）----

bool isProcessTrusted()
{
#ifdef __APPLE__
    return AXIsProcessTrusted();
#else
    return false;
#endif
}

bool requestTrustPrompt()
{
#ifdef __APPLE__
    const void* keys[] = { kAXTrustedCheckOptionPrompt };
    const void* values[] = { kCFBooleanTrue };
    CFDictionaryRef dict
        = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                             &kCFTypeDictionaryValueCallBacks);
    const bool trusted = dict != nullptr ? AXIsProcessTrustedWithOptions(dict) : false;
    if (dict != nullptr)
        CFRelease(dict);
    return trusted;
#else
    return false;
#endif
}

#ifdef __APPLE__

namespace {

struct TapState {
    TapHandler handler;
    /// 超时禁用后自恢复用（CGEventTapEnable 需要 mach port）
    std::atomic<void*> machPort{ nullptr };
    /// 上次 flags（修饰键 Up 推导兜底：个别设备 FlagsChanged 不带变化位）
    quint64 lastFlags = 0;
};

quint64 maskBit(quint32 t) { return quint64(1) << t; }

CGEventRef tapCallback(CGEventTapProxy, CGEventType eventType, CGEventRef event, void* userInfo)
{
    auto* state = static_cast<TapState*>(userInfo);
    const uint32_t type = uint32_t(eventType);
    // 超时被系统禁用：立即重新启用（否则 tap 静默死亡）
    if (type == kEvtTapDisabledByTimeout || type == kEvtTapDisabledByUserInput) {
        void* port = state->machPort.load(std::memory_order_seq_cst);
        if (port != nullptr)
            CGEventTapEnable(static_cast<CFMachPortRef>(port), true);
        return event;
    }

    RawEvent raw;
    raw.eventType = type;
    raw.keycode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    raw.flags = CGEventGetFlags(event);
    raw.autorepeat = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
    raw.buttonNumber = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
    raw.dx = CGEventGetDoubleValueField(event, kCGMouseEventDeltaX);
    raw.dy = CGEventGetDoubleValueField(event, kCGMouseEventDeltaY);
    raw.scrollDx = CGEventGetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis2);
    raw.scrollDy = CGEventGetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis1);

    auto ev = classify(raw);
    if (ev.has_value()) {
        // 移动事件补充全局坐标（classify 保持纯函数，坐标由 FFI 提供）
        if (ev->kind == HostInputEvent::Kind::MouseMoved) {
            const CGPoint loc = CGEventGetLocation(event);
            ev->x = loc.x;
            ev->y = loc.y;
        }
        state->lastFlags = raw.flags;
        if (state->handler && state->handler(*ev))
            return nullptr; // 吞掉
    }
    return event; // 放行
}

} // namespace

struct EventTapHandle::Impl {
    std::mutex mutex;
    CFRunLoopRef runloop = nullptr; // 仅用于 stop 时投递 CFRunLoopStop
    std::atomic<bool> stopped{ false };
    std::atomic<bool> startFailed{ false };
    std::unique_ptr<TapState> state;
};

EventTapHandle::EventTapHandle(EventTapHandle&& other) noexcept
    : m_impl(std::move(other.m_impl)), m_thread(std::move(other.m_thread))
{
}

EventTapHandle& EventTapHandle::operator=(EventTapHandle&& other) noexcept
{
    if (this != &other) {
        stop();
        m_impl = std::move(other.m_impl);
        m_thread = std::move(other.m_thread);
    }
    return *this;
}

void EventTapHandle::stop()
{
    if (!m_impl)
        return;
    bool expected = false;
    if (!m_impl->stopped.compare_exchange_strong(expected, true))
        return;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->runloop != nullptr)
            CFRunLoopStop(m_impl->runloop);
    }
    if (m_thread.joinable())
        m_thread.join();
}

mw::Result<EventTapHandle> spawnTap(TapConfig config, TapHandler handler)
{
    quint64 mask = 0;
    if (config.keyboard)
        mask |= maskBit(kEvtKeyDown) | maskBit(kEvtKeyUp) | maskBit(kEvtFlagsChanged);
    if (config.mouse) {
        mask |= maskBit(kEvtLeftMouseDown) | maskBit(kEvtLeftMouseUp)
                | maskBit(kEvtRightMouseDown) | maskBit(kEvtRightMouseUp)
                | maskBit(kEvtOtherMouseDown) | maskBit(kEvtOtherMouseUp) | maskBit(kEvtMouseMoved)
                | maskBit(kEvtLeftMouseDragged) | maskBit(kEvtRightMouseDragged)
                | maskBit(kEvtScrollWheel);
    }
    if (mask == 0) {
        return mw::err<EventTapHandle>(
            errEventTap(QStringLiteral("keyboard/mouse 至少启用一项")));
    }

    auto impl = std::make_shared<EventTapHandle::Impl>();
    impl->state = std::make_unique<TapState>();
    impl->state->handler = std::move(handler);
    impl->startFailed = false;

    std::thread worker([impl, mask, active = config.active]() {
        const CGEventTapLocation tapLoc = kCGSessionEventTap;
        const CGEventTapPlacement place = kCGHeadInsertEventTap;
        const CGEventTapOptions options
            = active ? kCGEventTapOptionDefault : kCGEventTapOptionListenOnly;
        CFMachPortRef port = CGEventTapCreate(tapLoc, place, options, CGEventMask(mask),
                                              tapCallback, impl->state.get());
        if (port == nullptr) {
            qWarning().noquote() << QStringLiteral(
                "[input] CGEventTapCreate 失败（多为辅助功能未授权：系统设置 → 隐私与安全性 → 辅助功能）");
            impl->startFailed = true;
            impl->stopped.store(true);
            return;
        }
        impl->state->machPort.store(port, std::memory_order_seq_cst);

        CFRunLoopRef runloop = CFRunLoopGetCurrent();
        CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(nullptr, port, 0);
        if (source == nullptr) {
            impl->startFailed = true;
            impl->stopped.store(true);
            CFRelease(port);
            return;
        }
        CFRunLoopAddSource(runloop, source, kCFRunLoopCommonModes);
        CGEventTapEnable(port, true);
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->runloop = runloop;
        }

        CFRunLoopRun(); // stop() 后返回

        // 清理（线程退出路径）
        if (CFRunLoopContainsSource(runloop, source, kCFRunLoopCommonModes))
            CFRunLoopRemoveSource(runloop, source, kCFRunLoopCommonModes);
        CFRelease(source);
        CFRelease(port);
        impl->stopped.store(true);
    });

    // 等 runloop 就绪或启动失败（避免 stop() 在挂载前竞态）
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->runloop != nullptr || impl->stopped.load())
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (impl->startFailed) {
        worker.join();
        return mw::err<EventTapHandle>(
            errEventTap(QStringLiteral("CGEventTapCreate 失败（多为辅助功能未授权：系统设置 → 隐私与安全性 → 辅助功能）")));
    }

    EventTapHandle handle;
    handle.m_impl = impl;
    handle.m_thread = std::move(worker);
    return mw::Result<EventTapHandle>::Ok(std::move(handle));
}

#else // !__APPLE__

struct EventTapHandle::Impl {
    std::atomic<bool> stopped{ false };
    std::atomic<bool> startFailed{ false };
};

EventTapHandle::EventTapHandle(EventTapHandle&& other) noexcept
    : m_impl(std::move(other.m_impl)), m_thread(std::move(other.m_thread))
{
}

EventTapHandle& EventTapHandle::operator=(EventTapHandle&& other) noexcept
{
    if (this != &other) {
        stop();
        m_impl = std::move(other.m_impl);
        m_thread = std::move(other.m_thread);
    }
    return *this;
}

void EventTapHandle::stop()
{
    if (m_thread.joinable())
        m_thread.join();
}

mw::Result<EventTapHandle> spawnTap(TapConfig, TapHandler)
{
    // 非 Apple 平台：编译排除（TODO：对应平台的全局输入捕获）
    return mw::err<EventTapHandle>(
        errEventTap(QStringLiteral("本平台暂不支持全局事件捕获（TODO）")));
}

#endif // __APPLE__

// ---- KeymapRunner ----

namespace {

/// 窗口 frame 刷新间隔（CGWindowList 有毫秒级开销，节制调用）
constexpr auto kFrameRefresh = std::chrono::milliseconds(500);
/// 前台态刷新间隔
constexpr auto kFrontRefresh = std::chrono::milliseconds(150);
/// Command/Control 任一按下 = 系统组合键
constexpr quint64 kModsSystem = kMaskCommand | kMaskControl;

} // namespace

KeymapRunner::KeymapRunner(std::shared_ptr<Engine> engine)
    : m_engine(std::move(engine)),
      m_frameAt(std::chrono::steady_clock::now() - kFrameRefresh - std::chrono::seconds(1)),
      m_frontAt(std::chrono::steady_clock::now() - kFrontRefresh - std::chrono::seconds(1))
{
}

KeymapRunner::~KeymapRunner()
{
    // 进程退出兜底：恢复光标关联（幂等）
    capture::associateMouseCursor(true);
}

bool KeymapRunner::handleEvent(const HostInputEvent& ev)
{
    // 焦点门控：模拟器窗口不在前台时放行一切（用户正在别的窗口操作；
    // 尤其保证"点回游戏窗口"的那次鼠标事件能送达，游戏才能解除失焦暂停）
    if (!m_focusGateOff) {
        bool front = false;
        {
            std::lock_guard<std::mutex> lock(m_innerMutex);
            const auto sinceFront = std::chrono::steady_clock::now() - m_frontAt;
            if (m_front.has_value() && sinceFront < kFrontRefresh) {
                front = *m_front;
            } else {
                front = capture::emulatorFrontmost();
                m_front = front;
                m_frontAt = std::chrono::steady_clock::now();
            }
        }
        // 目标 Activity 不匹配时同样挂起（登录/选服页不映射）
        const bool activityOk = m_activityOk.load(std::memory_order_relaxed);
        if (!front || !activityOk) {
            {
                std::lock_guard<std::mutex> lock(m_innerMutex);
                if (m_disassociated) {
                    capture::associateMouseCursor(true);
                    m_disassociated = false;
                }
            }
            m_engine->releaseFingers();
            return false;
        }
    }

    bool consumed = false;
    switch (ev.kind) {
    case HostInputEvent::Kind::Key: {
        if (ev.autorepeat) {
            // 按住产生的重复 Down：吞掉（若已绑定）但不重复注入
            consumed = engineKeyBound(ev.key);
        } else if (ev.mods & kModsSystem) {
            consumed = false; // Cmd/Ctrl 组合键：放行给系统
        } else {
            consumed = m_engine->handleEvent(InputEvent::keyEvent(ev.key, ev.action == HostKeyAction::Down));
        }
        break;
    }
    case HostInputEvent::Kind::MouseButton: {
        if (ev.mods & kModsSystem) {
            return false; // Cmd/Ctrl 组合键：放行给系统（不经 syncCapture）
        }
        const bool down = ev.action == HostKeyAction::Down;
        const bool handled = m_engine->handleEvent(InputEvent::keyEvent(ev.button.keyName(), down));
        // 捕获模式下鼠标键全部吞掉（即便未绑定），避免宿主误响应
        consumed = handled || m_engine->captured();
        break;
    }
    case HostInputEvent::Kind::MouseMoved: {
        const auto pos = mousePosNorm(ev.x, ev.y);
        consumed = m_engine->handleEvent(
            InputEvent::mouseMove(ev.dx, ev.dy, pos.has_value(), pos.has_value() ? pos->first : 0.0,
                                  pos.has_value() ? pos->second : 0.0));
        break;
    }
    case HostInputEvent::Kind::Scroll:
        // 捕获模式或存在滚轮节点时由引擎处理并吞掉
        consumed = m_engine->handleEvent(InputEvent::scroll(ev.scrollDy));
        break;
    }
    syncCapture();
    return consumed;
}

bool KeymapRunner::engineKeyBound(const QString& key)
{
    const mw::keymap::Profile p = m_engine->profileSnapshot();
    if (key == p.switchKey)
        return true;
    for (const mw::keymap::Node& n : p.nodes) {
        if (n.hasKey() && n.key == key)
            return true;
    }
    return false;
}

std::optional<QPair<double, double>> KeymapRunner::mousePosNorm(double gx, double gy)
{
    std::lock_guard<std::mutex> lock(m_innerMutex);
    const bool refresh = (std::chrono::steady_clock::now() - m_frameAt) >= kFrameRefresh
                         || !m_frame.has_value();
    if (refresh) {
        m_frame = capture::emulatorWindowFrame();
        m_frameAt = std::chrono::steady_clock::now();
    }
    if (!m_frame.has_value())
        return std::nullopt;
    const capture::Rect& f = *m_frame;
    if (f.width <= 0.0 || f.height <= 0.0)
        return std::nullopt;
    const double lx = (gx - f.x) / f.width;
    const double ly = (gy - f.y) / f.height;
    // 窗口内容有标题栏等装饰偏移时可能略微越界：夹紧即可
    if (lx < 0.0 || lx > 1.0 || ly < 0.0 || ly > 1.0)
        return std::nullopt;
    return QPair<double, double>(lx, ly);
}

void KeymapRunner::syncCapture()
{
    const bool wantCapture = m_engine->captured();
    std::lock_guard<std::mutex> lock(m_innerMutex);
    if (wantCapture && !m_disassociated) {
        capture::associateMouseCursor(false);
        m_disassociated = true;
    } else if (!wantCapture && m_disassociated) {
        capture::associateMouseCursor(true);
        m_disassociated = false;
    }
}

void KeymapRunner::setActivityOk(bool ok)
{
    m_activityOk.store(ok, std::memory_order_relaxed);
}

void KeymapRunner::shutdown()
{
    m_engine->shutdown();
    std::lock_guard<std::mutex> lock(m_innerMutex);
    if (m_disassociated) {
        capture::associateMouseCursor(true);
        m_disassociated = false;
    }
}

mw::Result<EventTapHandle> KeymapRunner::spawnGlobal(bool includeMouse)
{
    shutdown(); // 确保 associate 状态干净（spawn 前）
    TapConfig config;
    config.keyboard = true;
    config.mouse = includeMouse;
    config.active = true;
    std::shared_ptr<KeymapRunner> self = shared_from_this();
    auto tap = spawnTap(config, [self](const HostInputEvent& ev) { return self->handleEvent(ev); });
    if (!tap.ok())
        return tap;
    if (!tap.value().valid()) {
        return mw::err<EventTapHandle>(
            errEventTap(QStringLiteral("全局键鼠捕获启动失败（可用 requestTrustPrompt() 引导授权）")));
    }
    return mw::Result<EventTapHandle>::Ok(std::move(tap.value()));
}

} // namespace mw::input
