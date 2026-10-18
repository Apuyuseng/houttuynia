// 控制通道（宿主 -> 设备）消息编码。
// 布局与 server 端 ControlMessageReader.java 逐字节对齐，全部大端。
// ← crates/mwine-scrcpy/src/control.rs
#pragma once

#include <QByteArray>
#include <QString>

#include "error.hpp"
#include "result.hpp"

namespace mw::scrcpy::control {

// ---- 消息类型常量（ControlMessage.java）----
inline constexpr quint8 kTypeInjectKeycode = 0;
inline constexpr quint8 kTypeInjectText = 1;
inline constexpr quint8 kTypeInjectTouchEvent = 2;
inline constexpr quint8 kTypeInjectScrollEvent = 3;
inline constexpr quint8 kTypeBackOrScreenOn = 4;
inline constexpr quint8 kTypeExpandNotificationPanel = 5;
inline constexpr quint8 kTypeExpandSettingsPanel = 6;
inline constexpr quint8 kTypeCollapsePanels = 7;
inline constexpr quint8 kTypeGetClipboard = 8;
inline constexpr quint8 kTypeSetClipboard = 9;
inline constexpr quint8 kTypeSetDisplayPower = 10;
inline constexpr quint8 kTypeRotateDevice = 11;
inline constexpr quint8 kTypeUhidCreate = 12;
inline constexpr quint8 kTypeUhidInput = 13;
inline constexpr quint8 kTypeUhidDestroy = 14;
inline constexpr quint8 kTypeOpenHardKeyboardSettings = 15;
inline constexpr quint8 kTypeStartApp = 16;
inline constexpr quint8 kTypeResetVideo = 17;
inline constexpr quint8 kTypeCameraSetTorch = 18;
inline constexpr quint8 kTypeCameraZoomIn = 19;
inline constexpr quint8 kTypeCameraZoomOut = 20;
inline constexpr quint8 kTypeResizeDisplay = 21;

// ---- 常用 Android 按键码 ----
inline constexpr qint32 AKEYCODE_HOME = 3;
inline constexpr qint32 AKEYCODE_BACK = 4;
inline constexpr qint32 AKEYCODE_APP_SWITCH = 187;
inline constexpr qint32 AKEYCODE_ENTER = 66;
inline constexpr qint32 AKEYCODE_DEL = 67;
inline constexpr qint32 AKEYCODE_DPAD_UP = 19;
inline constexpr qint32 AKEYCODE_DPAD_DOWN = 20;
inline constexpr qint32 AKEYCODE_DPAD_LEFT = 21;
inline constexpr qint32 AKEYCODE_DPAD_RIGHT = 22;
// 字母键（⌘ 快捷键映射 Android Ctrl 组合用：粘贴/复制/全选/剪切）
inline constexpr qint32 AKEYCODE_A = 29;
inline constexpr qint32 AKEYCODE_C = 31;
inline constexpr qint32 AKEYCODE_V = 50;
inline constexpr qint32 AKEYCODE_X = 52;
inline constexpr qint32 AKEYCODE_VOLUME_UP = 24;
inline constexpr qint32 AKEYCODE_VOLUME_DOWN = 25;
inline constexpr qint32 AKEYCODE_VOLUME_MUTE = 164;
inline constexpr qint32 AKEYCODE_MENU = 82;
inline constexpr qint32 AKEYCODE_POWER = 26;
/// META_CTRL_ON | META_CTRL_LEFT_ON
inline constexpr qint32 META_CTRL_ON_COMBO = 0x1000 | 0x2000;

/// 触摸动作（AMOTION_EVENT_ACTION_*）。
inline constexpr quint8 ACTION_DOWN = 0;
inline constexpr quint8 ACTION_UP = 1;
inline constexpr quint8 ACTION_MOVE = 2;

/// GET_CLIPBOARD 的 copyKey。
inline constexpr quint8 COPY_KEY_NONE = 0;
inline constexpr quint8 COPY_KEY_COPY = 1;
inline constexpr quint8 COPY_KEY_CUT = 2;

/// INJECT_TEXT 服务端上限（ControlMessageReader.INJECT_TEXT_MAX_LENGTH）。
inline constexpr int kInjectTextMax = 300;

/// scrcpy 客户端惯用的鼠标指针 id。
inline constexpr quint64 kPointerIdMouse = 0xFFFFFFFFFFFFFFFFULL;

} // namespace mw::scrcpy::control

namespace mw::scrcpy {

/// 按键动作（AKEYEVENT_ACTION_DOWN/UP）。
enum class KeyAction { Down, Up };

inline quint8 keyActionU8(KeyAction a) { return a == KeyAction::Down ? 0 : 1; }

/// 控制消息：Rust enum ControlMessage → 带 Kind 判别的扁平结构，
/// 字段按 Kind 取用（未用到的字段保持默认值）。
struct ControlMessage {
    enum class Kind {
        InjectKeycode,
        InjectText,
        InjectTouch,
        InjectScroll,
        BackOrScreenOn,
        ExpandNotificationPanel,
        ExpandSettingsPanel,
        CollapsePanels,
        GetClipboard,
        SetClipboard,
        SetDisplayPower,
        RotateDevice,
        UhidCreate,
        UhidInput,
        UhidDestroy,
        OpenHardKeyboardSettings,
        StartApp,
        ResetVideo,
        CameraSetTorch,
        CameraZoomIn,
        CameraZoomOut,
        ResizeDisplay,
    };

    Kind kind = Kind::RotateDevice;

    // InjectKeycode
    KeyAction keyAction = KeyAction::Down;
    qint32 keycode = 0;
    qint32 repeat = 0;
    qint32 metaState = 0;

    // InjectText / SetClipboard / StartApp
    QString text;

    // InjectTouch
    quint8 touchAction = 0; // control::ACTION_*
    quint64 pointerId = 0;
    qint32 x = 0;
    qint32 y = 0;
    quint16 screenWidth = 0;
    quint16 screenHeight = 0;
    float pressure = 0.0f;
    qint32 actionButton = 0;
    qint32 buttons = 0;

    // InjectScroll
    float hScroll = 0.0f;
    float vScroll = 0.0f;

    // GetClipboard
    quint8 copyKey = 0;

    // SetClipboard
    quint64 sequence = 0;
    bool flag = false; ///< paste（SetClipboard）/ on（SetDisplayPower / CameraSetTorch）

    // UhidCreate / UhidInput / UhidDestroy
    quint16 uhidId = 0;
    quint16 vendorId = 0;
    quint16 productId = 0;
    QString uhidName;
    QByteArray uhidData;

    // ResizeDisplay
    quint16 width = 0;
    quint16 height = 0;

    // ---- 工厂 ----
    static ControlMessage injectKeycode(KeyAction action, qint32 keycode, qint32 repeat,
                                        qint32 metaState);
    static ControlMessage injectText(QString text);
    static ControlMessage injectTouch(quint8 action, quint64 pointerId, qint32 x, qint32 y,
                                      quint16 screenWidth, quint16 screenHeight, float pressure,
                                      qint32 actionButton, qint32 buttons);
    static ControlMessage injectScroll(qint32 x, qint32 y, quint16 screenWidth,
                                       quint16 screenHeight, float hScroll, float vScroll,
                                       qint32 buttons);
    static ControlMessage back(KeyAction action);
    static ControlMessage expandNotificationPanel();
    static ControlMessage expandSettingsPanel();
    static ControlMessage collapsePanels();
    static ControlMessage getClipboard(quint8 copyKey);
    static ControlMessage setClipboard(quint64 sequence, bool paste, QString text);
    static ControlMessage setDisplayPower(bool on);
    static ControlMessage rotateDevice();
    static ControlMessage uhidCreate(quint16 id, quint16 vendorId, quint16 productId, QString name,
                                     QByteArray data);
    static ControlMessage uhidInput(quint16 id, QByteArray data);
    static ControlMessage uhidDestroy(quint16 id);
    static ControlMessage openHardKeyboardSettings();
    static ControlMessage startApp(QString name);
    static ControlMessage resetVideo();
    static ControlMessage cameraSetTorch(bool on);
    static ControlMessage cameraZoomIn();
    static ControlMessage cameraZoomOut();
    static ControlMessage resizeDisplay(quint16 width, quint16 height);

    /// 编码为线协议字节（大端）。
    mw::Result<QByteArray> encode() const;
};

} // namespace mw::scrcpy
