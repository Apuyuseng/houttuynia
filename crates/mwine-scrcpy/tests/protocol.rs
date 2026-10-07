//! 协议字节级测试：编码结果与 server 端 reader 的期望布局逐字节对齐。

use mwine_scrcpy::control::{self, ControlMessage, KeyAction};
use mwine_scrcpy::options::{NewDisplay, ServerOptions, VideoCodecId};

#[test]
fn inject_keycode_encoding() {
    // [type=0][action=0][keycode BE32][repeat BE32][meta BE32]
    let msg = ControlMessage::InjectKeycode {
        action: KeyAction::Down,
        keycode: 4, // BACK
        repeat: 0,
        meta_state: 0,
    };
    let bytes = msg.encode().unwrap();
    // [type=0][action=0][keycode=4 BE32][repeat=0 BE32][meta=0 BE32] 共 14 字节
    assert_eq!(
        bytes,
        vec![0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
    );
}

#[test]
fn inject_text_encoding() {
    let msg = ControlMessage::InjectText("hi".into());
    let bytes = msg.encode().unwrap();
    // [type=1][len BE32=2]['h']['i']
    assert_eq!(bytes, vec![0x01, 0x00, 0x00, 0x00, 0x02, b'h', b'i']);
}

#[test]
fn inject_touch_encoding() {
    let msg = ControlMessage::InjectTouch {
        action: control::ACTION_DOWN,
        pointer_id: 0xFFFF_FFFF_FFFF_FFFF,
        x: 100,
        y: 200,
        screen_width: 1280,
        screen_height: 720,
        pressure: 1.0, // -> 0xffff
        action_button: 1,
        buttons: 1,
    };
    let bytes = msg.encode().unwrap();
    assert_eq!(bytes.len(), 32);
    assert_eq!(bytes[0], 0x02);
    assert_eq!(bytes[1], control::ACTION_DOWN);
    assert_eq!(&bytes[2..10], &0xFFFF_FFFF_FFFF_FFFFu64.to_be_bytes());
    assert_eq!(&bytes[10..14], &100i32.to_be_bytes());
    assert_eq!(&bytes[18..20], &1280u16.to_be_bytes());
    // pressure 1.0 = 0xffff
    assert_eq!(&bytes[22..24], &0xffffu16.to_be_bytes());
}

#[test]
fn set_clipboard_encoding() {
    let msg = ControlMessage::SetClipboard {
        sequence: 7,
        paste: true,
        text: "abc".into(),
    };
    let bytes = msg.encode().unwrap();
    let mut expect = vec![0x09];
    expect.extend_from_slice(&7u64.to_be_bytes());
    expect.push(1);
    expect.extend_from_slice(&3u32.to_be_bytes());
    expect.extend_from_slice(b"abc");
    assert_eq!(bytes, expect);
}

#[test]
fn resize_display_encoding() {
    let msg = ControlMessage::ResizeDisplay {
        width: 1024,
        height: 768,
    };
    assert_eq!(msg.encode().unwrap(), vec![21, 0x04, 0x00, 0x03, 0x00]);
}

#[test]
fn start_app_encoding() {
    let msg = ControlMessage::StartApp {
        name: "com.example.app".into(),
    };
    let bytes = msg.encode().unwrap();
    assert_eq!(bytes[0], 0x10);
    assert_eq!(bytes[1] as usize, "com.example.app".len());
    assert_eq!(&bytes[2..], b"com.example.app");
}

#[test]
fn server_options_serialization() {
    let opts = ServerOptions {
        new_display: Some(NewDisplay::new(1920, 1080, 160)),
        flex_display: true,
        ..Default::default()
    };
    let args = opts.to_args();
    let joined = args.join(" ");
    assert!(joined.contains("video=true"));
    assert!(joined.contains("audio=false"));
    assert!(joined.contains("control=true"));
    assert!(joined.contains("new_display=1920x1080/160"));
    assert!(joined.contains("flex_display=true"));
    assert!(joined.contains("tunnel_forward=true"));
    assert!(joined.contains("video_codec=h264"));
}

#[test]
fn new_display_param_forms() {
    assert_eq!(NewDisplay::new(800, 600, 240).to_param(), "800x600/240");
    assert_eq!(
        NewDisplay {
            width: None,
            height: None,
            dpi: Some(120)
        }
        .to_param(),
        "/120"
    );
}

#[test]
fn flex_display_requires_new_display() {
    let mut opts = ServerOptions::default();
    opts.flex_display = true;
    assert!(opts.validate().is_err());
    opts.new_display = Some(NewDisplay::new(800, 600, 120));
    assert!(opts.validate().is_ok());
}

#[test]
fn codec_ids_match_server() {
    assert_eq!(VideoCodecId::H264.wire_id(), 0x68323634);
    assert_eq!(VideoCodecId::H265.wire_id(), 0x68323635);
    assert_eq!(VideoCodecId::Av1.wire_id(), 0x00617631);
}

#[test]
fn scid_socket_name_and_serialization() {
    let mut opts = ServerOptions::default();
    assert_eq!(opts.socket_name(), "scrcpy");
    opts.scid = Some(0x1a2b3c4d);
    assert_eq!(opts.socket_name(), "scrcpy_1a2b3c4d");
    let args = opts.to_args().join(" ");
    assert!(args.contains("scid=1a2b3c4d"));

    // 随机 scid 在 31 位范围内且非负
    for _ in 0..100 {
        let scid = ServerOptions::random_scid();
        assert!(scid <= 0x7FFF_FFFF);
    }
}
