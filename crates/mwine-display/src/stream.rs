//! 流式模式：streamScreenshot（画面变化时推送帧）。

use mwine_grpc::EmuClient;
use super::{Frame, DisplayError};

pub struct StreamDisplay {
    client: EmuClient,
    display: u32,
    width: u32,
    height: u32,
}

impl StreamDisplay {
    pub fn new(client: EmuClient, display: u32, width: u32, height: u32) -> Self {
        Self { client, display, width, height }
    }

    /// 启动流式接收（回调模式；画面不变时不推帧）
    pub async fn run<F>(&mut self, mut callback: F) -> Result<(), DisplayError>
    where F: FnMut(Frame) + Send {
        let mut stream = self.client.stream_screenshots(self.display, self.width, self.height)
            .await
            .map_err(|e| DisplayError::Grpc(e.message().to_string()))?;

        use tokio_stream::StreamExt;
        while let Some(img) = stream.next().await {
            match img {
                Ok(image) => {
                    let fmt = image.format.unwrap_or_default();
                    if !image.data.is_empty() {
                        callback(Frame {
                            width: fmt.width as u32,
                            height: fmt.height as u32,
                            data: image.data,
                            timestamp: image.timestamp as u64,
                        });
                    }
                }
                Err(e) => {
                    return Err(DisplayError::Grpc(e.message().to_string()));
                }
            }
        }
        Ok(())
    }
}
