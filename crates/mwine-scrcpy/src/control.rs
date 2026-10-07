//! 控制通道（宿主 -> 设备）消息编码。
//! 布局与 server 端 ControlMessageReader.java 逐字节对齐，全部大端。

use std::io::Write;

use crate::error::Result;

// ---- 消息类型常量（ControlMessage.java）----
pub const TYPE_INJECT_KEYCODE: u8 = 0;
pub const TYPE_INJECT_TEXT: u8 = 1;
pub const TYPE_INJECT_TOUCH_EVENT: u8 = 2;
pub const TYPE_INJECT_SCROLL_EVENT: u8 = 3;
pub const TYPE_BACK_OR_SCREEN_ON: u8 = 4;
pub const TYPE_EXPAND_NOTIFICATION_PANEL: u8 = 5;
pub const TYPE_EXPAND_SETTINGS_PANEL: u8 = 6;
pub const TYPE_COLLAPSE_PANELS: u8 = 7;
pub const TYPE_GET_CLIPBOARD: u8 = 8;
pub const TYPE_SET_CLIPBOARD: u8 = 9;
pub const TYPE_SET_DISPLAY_POWER: u8 = 10;
pub const TYPE_ROTATE_DEVICE: u8 = 11;
pub const TYPE_UHID_CREATE: u8 = 12;
pub const TYPE_UHID_INPUT: u8 = 13;
pub const TYPE_UHID_DESTROY: u8 = 14;
pub const TYPE_OPEN_HARD_KEYBOARD_SETTINGS: u8 = 15;
pub const TYPE_START_APP: u8 = 16;
pub const TYPE_RESET_VIDEO: u8 = 17;
pub const TYPE_CAMERA_SET_TORCH: u8 = 18;
pub const TYPE_CAMERA_ZOOM_IN: u8 = 19;
pub const TYPE_CAMERA_ZOOM_OUT: u8 = 20;
pub const TYPE_RESIZE_DISPLAY: u8 = 21;

// ---- 常用 Android 按键码 ----
pub const AKEYCODE_HOME: i32 = 3;
pub const AKEYCODE_BACK: i32 = 4;
pub const AKEYCODE_APP_SWITCH: i32 = 187;
pub const AKEYCODE_ENTER: i32 = 66;
pub const AKEYCODE_DEL: i32 = 67;
pub const AKEYCODE_DPAD_UP: i32 = 19;
pub const AKEYCODE_DPAD_DOWN: i32 = 20;
pub const AKEYCODE_DPAD_LEFT: i32 = 21;
pub const AKEYCODE_DPAD_RIGHT: i32 = 22;
// 字母键（⌘ 快捷键映射 Android Ctrl 组合用：粘贴/复制/全选/剪切）
pub const AKEYCODE_A: i32 = 29;
pub const AKEYCODE_C: i32 = 31;
pub const AKEYCODE_V: i32 = 50;
pub const AKEYCODE_X: i32 = 52;
pub const AKEYCODE_VOLUME_UP: i32 = 24;
pub const AKEYCODE_VOLUME_DOWN: i32 = 25;
pub const AKEYCODE_VOLUME_MUTE: i32 = 164;
/// META_CTRL_ON | META_CTRL_LEFT_ON
pub const META_CTRL_ON_COMBO: i32 = 0x1000 | 0x2000;

/// 触摸动作（AMOTION_EVENT_ACTION_*）。
pub const ACTION_DOWN: u8 = 0;
pub const ACTION_UP: u8 = 1;
pub const ACTION_MOVE: u8 = 2;

/// GET_CLIPBOARD 的 copyKey。
pub const COPY_KEY_NONE: u8 = 0;
pub const COPY_KEY_COPY: u8 = 1;
pub const COPY_KEY_CUT: u8 = 2;

/// INJECT_TEXT 服务端上限（ControlMessageReader.INJECT_TEXT_MAX_LENGTH）。
pub const INJECT_TEXT_MAX: usize = 300;

/// scrcpy 客户端惯用的鼠标指针 id。
pub const POINTER_ID_MOUSE: u64 = 0xFFFF_FFFF_FFFF_FFFF;

#[derive(Debug, Clone)]
pub enum ControlMessage {
    /// 注入按键事件。
    InjectKeycode {
        action: KeyAction,
        keycode: i32,
        repeat: i32,
        meta_state: i32,
    },
    /// 注入文本（IME 桥的基础，上限 300 字符）。
    InjectText(String),
    /// 注入触摸事件。pressure ∈ [0,1]。
    InjectTouch {
        action: u8,
        pointer_id: u64,
        x: i32,
        y: i32,
        screen_width: u16,
        screen_height: u16,
        pressure: f32,
        action_button: i32,
        buttons: i32,
    },
    /// 注入滚轮事件。h_scroll / v_scroll ∈ [-1,1]。
    InjectScroll {
        x: i32,
        y: i32,
        screen_width: u16,
        screen_height: u16,
        h_scroll: f32,
        v_scroll: f32,
        buttons: i32,
    },
    /// 返回键 / 亮屏。
    BackOrScreenOn {
        action: KeyAction,
    },
    ExpandNotificationPanel,
    ExpandSettingsPanel,
    CollapsePanels,
    /// 请求设备剪贴板内容（回复走 DeviceMessage::Clipboard）。
    GetClipboard {
        copy_key: u8,
    },
    /// 设置设备剪贴板（paste=true 时同时触发粘贴）。
    SetClipboard {
        sequence: u64,
        paste: bool,
        text: String,
    },
    SetDisplayPower {
        on: bool,
    },
    RotateDevice,
    /// 键鼠以 UHID 设备形式直通（高级输入方案预留）。
    UhidCreate {
        id: u16,
        vendor_id: u16,
        product_id: u16,
        name: String,
        data: Vec<u8>,
    },
    UhidInput {
        id: u16,
        data: Vec<u8>,
    },
    UhidDestroy {
        id: u16,
    },
    OpenHardKeyboardSettings,
    /// 在设备上启动一个应用（包名或组件名）。
    StartApp {
        name: String,
    },
    ResetVideo,
    CameraSetTorch {
        on: bool,
    },
    CameraZoomIn,
    CameraZoomOut,
    /// 动态调整虚拟显示器尺寸（4.0 flex display）。
    ResizeDisplay {
        width: u16,
        height: u16,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyAction {
    Down,
    Up,
}

impl KeyAction {
    fn as_u8(self) -> u8 {
        match self {
            KeyAction::Down => 0,
            KeyAction::Up => 1,
        }
    }
}

/// 浮点 -> u16 定点（0..=0xffff，0xffff 表示 1.0）。
fn float_to_u16_fp(v: f32) -> u16 {
    if v >= 1.0 {
        0xffff
    } else if v <= 0.0 {
        0
    } else {
        (v * 65_536.0) as u16
    }
}

/// 浮点 -> i16 定点（server 端按 [-1,1] 满量程解码后再 *16）。
fn float_to_i16_fp(v: f32) -> i16 {
    let clamped = v.clamp(-1.0, 1.0);
    (clamped * 32_767.0) as i16
}

fn push_u32_be(buf: &mut Vec<u8>, v: u32) {
    buf.extend_from_slice(&v.to_be_bytes());
}

fn push_i32_be(buf: &mut Vec<u8>, v: i32) {
    buf.extend_from_slice(&v.to_be_bytes());
}

fn push_u16_be(buf: &mut Vec<u8>, v: u16) {
    buf.extend_from_slice(&v.to_be_bytes());
}

fn push_u64_be(buf: &mut Vec<u8>, v: u64) {
    buf.extend_from_slice(&v.to_be_bytes());
}

fn push_str_lp32(buf: &mut Vec<u8>, s: &str) {
    push_u32_be(buf, s.len() as u32);
    buf.extend_from_slice(s.as_bytes());
}

impl ControlMessage {
    pub fn back(action: KeyAction) -> Self {
        ControlMessage::BackOrScreenOn { action }
    }

    pub fn encode(&self) -> Result<Vec<u8>> {
        let mut b = Vec::with_capacity(32);
        match self {
            ControlMessage::InjectKeycode {
                action,
                keycode,
                repeat,
                meta_state,
            } => {
                b.push(TYPE_INJECT_KEYCODE);
                b.push(action.as_u8());
                push_i32_be(&mut b, *keycode);
                push_i32_be(&mut b, *repeat);
                push_i32_be(&mut b, *meta_state);
            }
            ControlMessage::InjectText(text) => {
                if text.len() > INJECT_TEXT_MAX {
                    return Err(crate::error::Error::Protocol(format!(
                        "inject text too long: {} > {INJECT_TEXT_MAX}",
                        text.len()
                    )));
                }
                b.push(TYPE_INJECT_TEXT);
                push_str_lp32(&mut b, text);
            }
            ControlMessage::InjectTouch {
                action,
                pointer_id,
                x,
                y,
                screen_width,
                screen_height,
                pressure,
                action_button,
                buttons,
            } => {
                b.push(TYPE_INJECT_TOUCH_EVENT);
                b.push(*action);
                push_u64_be(&mut b, *pointer_id);
                push_i32_be(&mut b, *x);
                push_i32_be(&mut b, *y);
                push_u16_be(&mut b, *screen_width);
                push_u16_be(&mut b, *screen_height);
                push_u16_be(&mut b, float_to_u16_fp(*pressure));
                push_i32_be(&mut b, *action_button);
                push_i32_be(&mut b, *buttons);
            }
            ControlMessage::InjectScroll {
                x,
                y,
                screen_width,
                screen_height,
                h_scroll,
                v_scroll,
                buttons,
            } => {
                b.push(TYPE_INJECT_SCROLL_EVENT);
                push_i32_be(&mut b, *x);
                push_i32_be(&mut b, *y);
                push_u16_be(&mut b, *screen_width);
                push_u16_be(&mut b, *screen_height);
                b.extend_from_slice(&float_to_i16_fp(*h_scroll).to_be_bytes());
                b.extend_from_slice(&float_to_i16_fp(*v_scroll).to_be_bytes());
                push_i32_be(&mut b, *buttons);
            }
            ControlMessage::BackOrScreenOn { action } => {
                b.push(TYPE_BACK_OR_SCREEN_ON);
                b.push(action.as_u8());
            }
            ControlMessage::ExpandNotificationPanel => b.push(TYPE_EXPAND_NOTIFICATION_PANEL),
            ControlMessage::ExpandSettingsPanel => b.push(TYPE_EXPAND_SETTINGS_PANEL),
            ControlMessage::CollapsePanels => b.push(TYPE_COLLAPSE_PANELS),
            ControlMessage::GetClipboard { copy_key } => {
                b.push(TYPE_GET_CLIPBOARD);
                b.push(*copy_key);
            }
            ControlMessage::SetClipboard {
                sequence,
                paste,
                text,
            } => {
                b.push(TYPE_SET_CLIPBOARD);
                push_u64_be(&mut b, *sequence);
                b.push(u8::from(*paste));
                push_str_lp32(&mut b, text);
            }
            ControlMessage::SetDisplayPower { on } => {
                b.push(TYPE_SET_DISPLAY_POWER);
                b.push(u8::from(*on));
            }
            ControlMessage::RotateDevice => b.push(TYPE_ROTATE_DEVICE),
            ControlMessage::UhidCreate {
                id,
                vendor_id,
                product_id,
                name,
                data,
            } => {
                b.push(TYPE_UHID_CREATE);
                push_u16_be(&mut b, *id);
                push_u16_be(&mut b, *vendor_id);
                push_u16_be(&mut b, *product_id);
                // name 长度 1 字节，data 长度 2 字节（服务端 parseString(1)/parseByteArray(2)）
                b.push(name.len() as u8);
                b.extend_from_slice(name.as_bytes());
                push_u16_be(&mut b, data.len() as u16);
                b.extend_from_slice(data);
            }
            ControlMessage::UhidInput { id, data } => {
                b.push(TYPE_UHID_INPUT);
                push_u16_be(&mut b, *id);
                push_u16_be(&mut b, data.len() as u16);
                b.extend_from_slice(data);
            }
            ControlMessage::UhidDestroy { id } => {
                b.push(TYPE_UHID_DESTROY);
                push_u16_be(&mut b, *id);
            }
            ControlMessage::OpenHardKeyboardSettings => b.push(TYPE_OPEN_HARD_KEYBOARD_SETTINGS),
            ControlMessage::StartApp { name } => {
                b.push(TYPE_START_APP);
                b.push(name.len() as u8);
                b.extend_from_slice(name.as_bytes());
            }
            ControlMessage::ResetVideo => b.push(TYPE_RESET_VIDEO),
            ControlMessage::CameraSetTorch { on } => {
                b.push(TYPE_CAMERA_SET_TORCH);
                b.push(u8::from(*on));
            }
            ControlMessage::CameraZoomIn => b.push(TYPE_CAMERA_ZOOM_IN),
            ControlMessage::CameraZoomOut => b.push(TYPE_CAMERA_ZOOM_OUT),
            ControlMessage::ResizeDisplay { width, height } => {
                b.push(TYPE_RESIZE_DISPLAY);
                push_u16_be(&mut b, *width);
                push_u16_be(&mut b, *height);
            }
        }
        Ok(b)
    }

    pub fn write_to<W: Write>(&self, w: &mut W) -> Result<()> {
        let bytes = self.encode()?;
        w.write_all(&bytes)?;
        Ok(())
    }
}
