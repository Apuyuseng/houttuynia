// input lib 实现 ← crates/mwine-input/src/lib.rs
#include "input.hpp"

namespace mw::input {

AndroidKeycode AndroidKeycode::fromChar(char32_t c)
{
    using K = AndroidKeycode::K;
    AndroidKeycode k;
    switch (c) {
    case U' ': k.k = K::Space; return k;
    case U'\n': case U'\r': k.k = K::Enter; return k;
    case U'\t': k.k = K::Tab; return k;
    case 0x08: k.k = K::Backspace; return k;
    case U',': k.k = K::Comma; return k;
    case U'.': k.k = K::Period; return k;
    case U'-': k.k = K::Minus; return k;
    case U'=': k.k = K::Equals; return k;
    case U'[': k.k = K::LeftBracket; return k;
    case U']': k.k = K::RightBracket; return k;
    case U'\\': k.k = K::Backslash; return k;
    case U';': k.k = K::Semicolon; return k;
    case U'\'': k.k = K::Apostrophe; return k;
    case U'/': k.k = K::Slash; return k;
    case U'`': k.k = K::Grave; return k;
    case U'@': k.k = K::At; return k;
    case U'+': k.k = K::Plus; return k;
    case U'*': k.k = K::Asterisk; return k;
    case U'#': k.k = K::Hash; return k;
    default: break;
    }
    if ((c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z')) {
        k.k = K::Char;
        k.ch = c;
        return k;
    }
    k.k = K::Unknown;
    return k;
}

AndroidKeycode AndroidKeycode::fromI32(qint32 code)
{
    using K = AndroidKeycode::K;
    AndroidKeycode k;
    switch (code) {
    case 3: k.k = K::Home; return k;
    case 4: k.k = K::Back; return k;
    case 187: k.k = K::AppSwitch; return k;
    case 82: k.k = K::Menu; return k;
    case 66: k.k = K::Enter; return k;
    case 67: k.k = K::Backspace; return k;
    case 111: k.k = K::Escape; return k;
    case 62: k.k = K::Space; return k;
    case 61: k.k = K::Tab; return k;
    case 19: k.k = K::DpadUp; return k;
    case 20: k.k = K::DpadDown; return k;
    case 21: k.k = K::DpadLeft; return k;
    case 22: k.k = K::DpadRight; return k;
    case 23: k.k = K::DpadCenter; return k;
    case 24: k.k = K::VolumeUp; return k;
    case 25: k.k = K::VolumeDown; return k;
    case 164: k.k = K::VolumeMute; return k;
    case 26: k.k = K::Power; return k;
    case 27: k.k = K::Camera; return k;
    case 80: k.k = K::Focus; return k;
    case 59: k.k = K::ShiftLeft; return k;
    case 60: k.k = K::ShiftRight; return k;
    case 57: k.k = K::AltLeft; return k;
    case 58: k.k = K::AltRight; return k;
    case 113: k.k = K::CtrlLeft; return k;
    case 114: k.k = K::CtrlRight; return k;
    case 117: k.k = K::MetaLeft; return k;
    case 118: k.k = K::MetaRight; return k;
    case 115: k.k = K::CapsLock; return k;
    case 92: k.k = K::PageUp; return k;
    case 93: k.k = K::PageDown; return k;
    case 122: k.k = K::MoveHome; return k;
    case 123: k.k = K::MoveEnd; return k;
    case 124: k.k = K::Insert; return k;
    case 131: case 132: case 133: case 134: case 135: case 136: case 137:
    case 138: case 139: case 140: case 141: case 142:
        k.k = K(int(K::F1) + (code - 131));
        return k;
    case 55: k.k = K::Comma; return k;
    case 56: k.k = K::Period; return k;
    case 69: k.k = K::Minus; return k;
    case 70: k.k = K::Equals; return k;
    case 71: k.k = K::LeftBracket; return k;
    case 72: k.k = K::RightBracket; return k;
    case 73: k.k = K::Backslash; return k;
    case 74: k.k = K::Semicolon; return k;
    case 75: k.k = K::Apostrophe; return k;
    case 76: k.k = K::Slash; return k;
    case 68: k.k = K::Grave; return k;
    case 77: k.k = K::At; return k;
    case 81: k.k = K::Plus; return k;
    case 17: k.k = K::Asterisk; return k;
    case 18: k.k = K::Hash; return k;
    default: break;
    }
    // '0'..'9' = 7..16, 'a'..'z' = 29..54
    if (code >= 7 && code <= 16) {
        k.k = K::Char;
        k.ch = char32_t(U'0' + (code - 7));
        return k;
    }
    if (code >= 29 && code <= 54) {
        k.k = K::Char;
        k.ch = char32_t(U'a' + (code - 29));
        return k;
    }
    k.k = K::Unknown;
    return k;
}

qint32 AndroidKeycode::toI32() const
{
    using K = AndroidKeycode::K;
    switch (k) {
    case K::Unknown: return 0;
    case K::Home: return 3;
    case K::Back: return 4;
    case K::AppSwitch: return 187;
    case K::Menu: return 82;
    case K::Enter: return 66;
    case K::Backspace: case K::Del: return 67;
    case K::Escape: return 111;
    case K::Space: return 62;
    case K::Tab: return 61;
    case K::DpadUp: return 19;
    case K::DpadDown: return 20;
    case K::DpadLeft: return 21;
    case K::DpadRight: return 22;
    case K::DpadCenter: return 23;
    case K::VolumeUp: return 24;
    case K::VolumeDown: return 25;
    case K::VolumeMute: return 164;
    case K::Power: return 26;
    case K::Camera: return 27;
    case K::Focus: return 80;
    case K::ShiftLeft: return 59;
    case K::ShiftRight: return 60;
    case K::AltLeft: return 57;
    case K::AltRight: return 58;
    case K::CtrlLeft: return 113;
    case K::CtrlRight: return 114;
    case K::MetaLeft: return 117;
    case K::MetaRight: return 118;
    case K::CapsLock: return 115;
    case K::PageUp: return 92;
    case K::PageDown: return 93;
    case K::MoveHome: return 122;
    case K::MoveEnd: return 123;
    case K::Insert: return 124;
    case K::F1: return 131;
    case K::F2: return 132;
    case K::F3: return 133;
    case K::F4: return 134;
    case K::F5: return 135;
    case K::F6: return 136;
    case K::F7: return 137;
    case K::F8: return 138;
    case K::F9: return 139;
    case K::F10: return 140;
    case K::F11: return 141;
    case K::F12: return 142;
    case K::Comma: return 55;
    case K::Period: return 56;
    case K::Minus: return 69;
    case K::Equals: return 70;
    case K::LeftBracket: return 71;
    case K::RightBracket: return 72;
    case K::Backslash: return 73;
    case K::Semicolon: return 74;
    case K::Apostrophe: return 75;
    case K::Slash: return 76;
    case K::Grave: return 68;
    case K::At: return 77;
    case K::Plus: return 81;
    case K::Asterisk: return 17;
    case K::Hash: return 18;
    case K::Char: {
        char32_t c = ch;
        if (c >= U'A' && c <= U'Z')
            c = c - U'A' + U'a';
        if (c >= U'0' && c <= U'9')
            return 7 + qint32(c - U'0');
        if (c >= U'a' && c <= U'z')
            return 29 + qint32(c - U'a');
        return 0;
    }
    }
    return 0;
}

// ---- InputRouter ----

mw::Result<bool> InputRouter::injectTouch(quint32 displayId, TouchAction action, quint64 pointerId,
                                          qint32 x, qint32 y)
{
    return forDisplay(displayId)->injectTouch(displayId, action, pointerId, x, y);
}

mw::Result<bool> InputRouter::injectKey(const AndroidKeycode& keycode, KeyAction action)
{
    return m_default->injectKey(keycode, action);
}

mw::Result<bool> InputRouter::injectText(const QString& text)
{
    return m_default->injectText(text);
}

mw::Result<bool> InputRouter::injectScroll(quint32 displayId, qint32 x, qint32 y, float h, float v)
{
    return forDisplay(displayId)->injectScroll(displayId, x, y, h, v);
}

// ---- EngineSink ----

void EngineSink::touch(mw::keymap::TouchAction action, quint64 pointer, qint32 x, qint32 y)
{
    TouchAction a;
    switch (action) {
    case mw::keymap::TouchAction::Down: a = TouchAction::Down; break;
    case mw::keymap::TouchAction::Move: a = TouchAction::Move; break;
    case mw::keymap::TouchAction::Up: a = TouchAction::Up; break;
    default: return;
    }
    if (!m_injector)
        return;
    std::lock_guard<std::mutex> lock(m_injector->mutex);
    if (m_injector->injector)
        m_injector->injector->injectTouch(m_displayId, a, pointer, x, y);
}

} // namespace mw::input
