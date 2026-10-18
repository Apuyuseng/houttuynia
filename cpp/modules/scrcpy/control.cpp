// 控制消息编码实现 ← crates/mwine-scrcpy/src/control.rs
#include "control.hpp"

namespace mw::scrcpy {

using namespace mw::scrcpy::control;

namespace {

/// 浮点 -> u16 定点（0..=0xffff，0xffff 表示 1.0）。
quint16 floatToU16Fp(float v)
{
    if (v >= 1.0f)
        return 0xffff;
    if (v <= 0.0f)
        return 0;
    return quint16(v * 65536.0f);
}

/// 浮点 -> i16 定点（server 端按 [-1,1] 满量程解码后再 *16）。
qint16 floatToI16Fp(float v)
{
    const float clamped = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return qint16(clamped * 32767.0f);
}

void pushU32BE(QByteArray& b, quint32 v)
{
    b.append(char((v >> 24) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}
void pushI32BE(QByteArray& b, qint32 v) { pushU32BE(b, quint32(v)); }
void pushU16BE(QByteArray& b, quint16 v)
{
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}
void pushU64BE(QByteArray& b, quint64 v)
{
    pushU32BE(b, quint32(v >> 32));
    pushU32BE(b, quint32(v & 0xFFFFFFFF));
}
void pushStrLp32(QByteArray& b, const QString& s)
{
    const QByteArray utf8 = s.toUtf8();
    pushU32BE(b, quint32(utf8.size()));
    b.append(utf8);
}

} // namespace

ControlMessage ControlMessage::injectKeycode(KeyAction action, qint32 kc, qint32 repeat_,
                                             qint32 meta)
{
    ControlMessage m;
    m.kind = Kind::InjectKeycode;
    m.keyAction = action;
    m.keycode = kc;
    m.repeat = repeat_;
    m.metaState = meta;
    return m;
}

ControlMessage ControlMessage::injectText(QString text)
{
    ControlMessage m;
    m.kind = Kind::InjectText;
    m.text = std::move(text);
    return m;
}

ControlMessage ControlMessage::injectTouch(quint8 action, quint64 pointerId_, qint32 x_, qint32 y_,
                                           quint16 sw, quint16 sh, float pressure_,
                                           qint32 actionButton_, qint32 buttons_)
{
    ControlMessage m;
    m.kind = Kind::InjectTouch;
    m.touchAction = action;
    m.pointerId = pointerId_;
    m.x = x_;
    m.y = y_;
    m.screenWidth = sw;
    m.screenHeight = sh;
    m.pressure = pressure_;
    m.actionButton = actionButton_;
    m.buttons = buttons_;
    return m;
}

ControlMessage ControlMessage::injectScroll(qint32 x_, qint32 y_, quint16 sw, quint16 sh,
                                            float h, float v, qint32 buttons_)
{
    ControlMessage m;
    m.kind = Kind::InjectScroll;
    m.x = x_;
    m.y = y_;
    m.screenWidth = sw;
    m.screenHeight = sh;
    m.hScroll = h;
    m.vScroll = v;
    m.buttons = buttons_;
    return m;
}

ControlMessage ControlMessage::back(KeyAction action)
{
    ControlMessage m;
    m.kind = Kind::BackOrScreenOn;
    m.keyAction = action;
    return m;
}

ControlMessage ControlMessage::expandNotificationPanel()
{
    ControlMessage m;
    m.kind = Kind::ExpandNotificationPanel;
    return m;
}
ControlMessage ControlMessage::expandSettingsPanel()
{
    ControlMessage m;
    m.kind = Kind::ExpandSettingsPanel;
    return m;
}
ControlMessage ControlMessage::collapsePanels()
{
    ControlMessage m;
    m.kind = Kind::CollapsePanels;
    return m;
}
ControlMessage ControlMessage::getClipboard(quint8 copyKey_)
{
    ControlMessage m;
    m.kind = Kind::GetClipboard;
    m.copyKey = copyKey_;
    return m;
}
ControlMessage ControlMessage::setClipboard(quint64 sequence_, bool paste, QString text_)
{
    ControlMessage m;
    m.kind = Kind::SetClipboard;
    m.sequence = sequence_;
    m.flag = paste;
    m.text = std::move(text_);
    return m;
}
ControlMessage ControlMessage::setDisplayPower(bool on)
{
    ControlMessage m;
    m.kind = Kind::SetDisplayPower;
    m.flag = on;
    return m;
}
ControlMessage ControlMessage::rotateDevice()
{
    ControlMessage m;
    m.kind = Kind::RotateDevice;
    return m;
}
ControlMessage ControlMessage::uhidCreate(quint16 id, quint16 vid, quint16 pid, QString name,
                                          QByteArray data)
{
    ControlMessage m;
    m.kind = Kind::UhidCreate;
    m.uhidId = id;
    m.vendorId = vid;
    m.productId = pid;
    m.uhidName = std::move(name);
    m.uhidData = std::move(data);
    return m;
}
ControlMessage ControlMessage::uhidInput(quint16 id, QByteArray data)
{
    ControlMessage m;
    m.kind = Kind::UhidInput;
    m.uhidId = id;
    m.uhidData = std::move(data);
    return m;
}
ControlMessage ControlMessage::uhidDestroy(quint16 id)
{
    ControlMessage m;
    m.kind = Kind::UhidDestroy;
    m.uhidId = id;
    return m;
}
ControlMessage ControlMessage::openHardKeyboardSettings()
{
    ControlMessage m;
    m.kind = Kind::OpenHardKeyboardSettings;
    return m;
}
ControlMessage ControlMessage::startApp(QString name)
{
    ControlMessage m;
    m.kind = Kind::StartApp;
    m.text = std::move(name);
    return m;
}
ControlMessage ControlMessage::resetVideo()
{
    ControlMessage m;
    m.kind = Kind::ResetVideo;
    return m;
}
ControlMessage ControlMessage::cameraSetTorch(bool on)
{
    ControlMessage m;
    m.kind = Kind::CameraSetTorch;
    m.flag = on;
    return m;
}
ControlMessage ControlMessage::cameraZoomIn()
{
    ControlMessage m;
    m.kind = Kind::CameraZoomIn;
    return m;
}
ControlMessage ControlMessage::cameraZoomOut()
{
    ControlMessage m;
    m.kind = Kind::CameraZoomOut;
    return m;
}
ControlMessage ControlMessage::resizeDisplay(quint16 w, quint16 h)
{
    ControlMessage m;
    m.kind = Kind::ResizeDisplay;
    m.width = w;
    m.height = h;
    return m;
}

mw::Result<QByteArray> ControlMessage::encode() const
{
    QByteArray b;
    b.reserve(32);
    switch (kind) {
    case Kind::InjectKeycode:
        b.append(char(kTypeInjectKeycode));
        b.append(char(keyActionU8(keyAction)));
        pushI32BE(b, keycode);
        pushI32BE(b, repeat);
        pushI32BE(b, metaState);
        break;
    case Kind::InjectText: {
        const QByteArray utf8 = text.toUtf8();
        if (utf8.size() > kInjectTextMax) {
            return mw::err<QByteArray>(errProtocol(QStringLiteral("inject text too long: %1 > %2")
                                                       .arg(utf8.size())
                                                       .arg(kInjectTextMax)));
        }
        b.append(char(kTypeInjectText));
        pushStrLp32(b, text);
        break;
    }
    case Kind::InjectTouch:
        b.append(char(kTypeInjectTouchEvent));
        b.append(char(touchAction));
        pushU64BE(b, pointerId);
        pushI32BE(b, x);
        pushI32BE(b, y);
        pushU16BE(b, screenWidth);
        pushU16BE(b, screenHeight);
        pushU16BE(b, floatToU16Fp(pressure));
        pushI32BE(b, actionButton);
        pushI32BE(b, buttons);
        break;
    case Kind::InjectScroll:
        b.append(char(kTypeInjectScrollEvent));
        pushI32BE(b, x);
        pushI32BE(b, y);
        pushU16BE(b, screenWidth);
        pushU16BE(b, screenHeight);
        pushU16BE(b, quint16(floatToI16Fp(hScroll)));
        pushU16BE(b, quint16(floatToI16Fp(vScroll)));
        pushI32BE(b, buttons);
        break;
    case Kind::BackOrScreenOn:
        b.append(char(kTypeBackOrScreenOn));
        b.append(char(keyActionU8(keyAction)));
        break;
    case Kind::ExpandNotificationPanel:
        b.append(char(kTypeExpandNotificationPanel));
        break;
    case Kind::ExpandSettingsPanel:
        b.append(char(kTypeExpandSettingsPanel));
        break;
    case Kind::CollapsePanels:
        b.append(char(kTypeCollapsePanels));
        break;
    case Kind::GetClipboard:
        b.append(char(kTypeGetClipboard));
        b.append(char(copyKey));
        break;
    case Kind::SetClipboard:
        b.append(char(kTypeSetClipboard));
        pushU64BE(b, sequence);
        b.append(char(flag ? 1 : 0));
        pushStrLp32(b, text);
        break;
    case Kind::SetDisplayPower:
        b.append(char(kTypeSetDisplayPower));
        b.append(char(flag ? 1 : 0));
        break;
    case Kind::RotateDevice:
        b.append(char(kTypeRotateDevice));
        break;
    case Kind::UhidCreate: {
        const QByteArray name8 = uhidName.toUtf8();
        b.append(char(kTypeUhidCreate));
        pushU16BE(b, uhidId);
        pushU16BE(b, vendorId);
        pushU16BE(b, productId);
        // name 长度 1 字节，data 长度 2 字节（服务端 parseString(1)/parseByteArray(2)）
        b.append(char(uchar(name8.size())));
        b.append(name8);
        pushU16BE(b, quint16(uhidData.size()));
        b.append(uhidData);
        break;
    }
    case Kind::UhidInput:
        b.append(char(kTypeUhidInput));
        pushU16BE(b, uhidId);
        pushU16BE(b, quint16(uhidData.size()));
        b.append(uhidData);
        break;
    case Kind::UhidDestroy:
        b.append(char(kTypeUhidDestroy));
        pushU16BE(b, uhidId);
        break;
    case Kind::OpenHardKeyboardSettings:
        b.append(char(kTypeOpenHardKeyboardSettings));
        break;
    case Kind::StartApp: {
        const QByteArray name8 = text.toUtf8();
        b.append(char(kTypeStartApp));
        b.append(char(uchar(name8.size())));
        b.append(name8);
        break;
    }
    case Kind::ResetVideo:
        b.append(char(kTypeResetVideo));
        break;
    case Kind::CameraSetTorch:
        b.append(char(kTypeCameraSetTorch));
        b.append(char(flag ? 1 : 0));
        break;
    case Kind::CameraZoomIn:
        b.append(char(kTypeCameraZoomIn));
        break;
    case Kind::CameraZoomOut:
        b.append(char(kTypeCameraZoomOut));
        break;
    case Kind::ResizeDisplay:
        b.append(char(kTypeResizeDisplay));
        pushU16BE(b, width);
        pushU16BE(b, height);
        break;
    }
    return mw::Result<QByteArray>::Ok(std::move(b));
}

} // namespace mw::scrcpy
