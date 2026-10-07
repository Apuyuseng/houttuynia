//! 设备 -> 宿主 消息（控制通道反向）。
//! 与 server 端 DeviceMessageWriter.java 对齐，大端。

use std::io::Read;

use crate::error::{Error, Result};

pub const TYPE_CLIPBOARD: u8 = 0;
pub const TYPE_ACK_CLIPBOARD: u8 = 1;
pub const TYPE_UHID_OUTPUT: u8 = 2;

#[derive(Debug, Clone)]
pub enum DeviceMessage {
    /// 设备剪贴板内容（响应 GetClipboard，或设备端复制时的自动同步）。
    Clipboard {
        text: String,
    },
    /// SetClipboard 的确认（sequence 回显）。
    AckClipboard {
        sequence: u64,
    },
    UhidOutput {
        id: u16,
        data: Vec<u8>,
    },
}

/// 从 reader 读取一条完整的设备消息。
/// 返回 None 表示流已结束。
pub fn read_device_message<R: Read>(reader: &mut R) -> Result<Option<DeviceMessage>> {
    let mut type_byte = [0u8; 1];
    match reader.read_exact(&mut type_byte) {
        Ok(_) => {}
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(e) => return Err(e.into()),
    }
    match type_byte[0] {
        TYPE_CLIPBOARD => {
            let text = read_lp32_string(reader)?;
            Ok(Some(DeviceMessage::Clipboard { text }))
        }
        TYPE_ACK_CLIPBOARD => {
            let mut seq = [0u8; 8];
            reader.read_exact(&mut seq)?;
            Ok(Some(DeviceMessage::AckClipboard {
                sequence: u64::from_be_bytes(seq),
            }))
        }
        TYPE_UHID_OUTPUT => {
            let mut id = [0u8; 2];
            reader.read_exact(&mut id)?;
            let mut len = [0u8; 2];
            reader.read_exact(&mut len)?;
            let mut data = vec![0u8; u16::from_be_bytes(len) as usize];
            reader.read_exact(&mut data)?;
            Ok(Some(DeviceMessage::UhidOutput {
                id: u16::from_be_bytes(id),
                data,
            }))
        }
        other => Err(Error::Protocol(format!(
            "unknown device message type: {other}"
        ))),
    }
}

fn read_lp32_string<R: Read>(reader: &mut R) -> Result<String> {
    let mut len = [0u8; 4];
    reader.read_exact(&mut len)?;
    let len = u32::from_be_bytes(len) as usize;
    let mut buf = vec![0u8; len];
    reader.read_exact(&mut buf)?;
    Ok(String::from_utf8_lossy(&buf).to_string())
}
