//! gRPC 客户端封装。

use tonic::transport::Channel;
use super::emulator::emulator_controller_client::EmulatorControllerClient;
use super::emulator::*;

pub struct EmuClient {
    client: EmulatorControllerClient<Channel>,
}

impl EmuClient {
    pub async fn connect(port: u16) -> Result<Self, String> {
        let addr = format!("http://127.0.0.1:{port}");
        let channel = Channel::from_shared(addr)
            .map_err(|e| e.to_string())?
            .connect()
            .await
            .map_err(|e| e.to_string())?;
        let client = EmulatorControllerClient::new(channel)
            .max_decoding_message_size(64 * 1024 * 1024)
            .max_encoding_message_size(64 * 1024 * 1024);
        Ok(Self { client })
    }

    pub async fn screenshot(&mut self, display: u32, w: u32, h: u32)
        -> Result<Image, tonic::Status>
    {
        let fmt = ImageFormat {
            format: image_format::ImgFormat::Rgba8888 as i32,
            width: w,
            height: h,
            display,
        };
        self.client.get_screenshot(fmt).await.map(|r| r.into_inner())
    }

    pub async fn stream_screenshots(&mut self, display: u32, w: u32, h: u32)
        -> Result<tonic::Streaming<Image>, tonic::Status>
    {
        let fmt = ImageFormat {
            format: image_format::ImgFormat::Rgba8888 as i32,
            width: w,
            height: h,
            display,
        };
        self.client.stream_screenshot(fmt).await.map(|r| r.into_inner())
    }

    pub async fn touch(&mut self, display: u32, kind: i32, x: i32, y: i32)
        -> Result<(), tonic::Status>
    {
        let ev = TouchEvent { display, x, y, pressure: 50, kind };
        self.client.send_touch(ev).await?;
        Ok(())
    }

    pub async fn key(&mut self, kind: i32, code: &str)
        -> Result<(), tonic::Status>
    {
        let ev = KeyboardEvent {
            kind, code: code.to_string(), modifiers: 0, text: String::new(),
        };
        self.client.send_key(ev).await?;
        Ok(())
    }

    pub async fn status(&mut self) -> Result<EmulatorStatus, tonic::Status> {
        self.client.get_status(Ok {}).await.map(|r| r.into_inner())
    }

    pub async fn set_clipboard(&mut self, text: &str) -> Result<(), tonic::Status> {
        self.client.set_clipboard(ClipData { text: text.to_string() }).await?;
        Ok(())
    }

    pub async fn get_clipboard(&mut self) -> Result<String, tonic::Status> {
        let clip = self.client.get_clipboard(Ok {}).await?;
        Ok(clip.into_inner().text)
    }
}
