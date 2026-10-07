//! 轮询模式：定时调 getScreenshot 获取帧。

use mwine_grpc::EmuClient;
use super::{Frame, DisplayError};
use std::time::Duration;

pub struct PollingDisplay {
    client: EmuClient,
    display: u32,
    width: u32,
    height: u32,
    interval: Duration,
}

impl PollingDisplay {
    pub fn new(client: EmuClient, display: u32, width: u32, height: u32, fps: u32) -> Self {
        Self {
            client,
            display,
            width,
            height,
            interval: Duration::from_millis(1000 / fps.max(1) as u64),
        }
    }

    /// 获取一帧（阻塞直到成功或超时）
    pub async fn next_frame(&mut self) -> Result<Frame, DisplayError> {
        let img = self.client.screenshot(self.display, self.width, self.height)
            .await
            .map_err(|e| DisplayError::Grpc(e.message().to_string()))?;
        self.image_to_frame(img)
    }

    /// 连续获取帧（回调模式）
    pub async fn run<F>(&mut self, mut callback: F) -> Result<(), DisplayError>
    where F: FnMut(Frame) + Send {
        let mut ticker = tokio::time::interval(self.interval);
        loop {
            ticker.tick().await;
            if let Ok(frame) = self.next_frame().await {
                callback(frame);
            }
        }
    }

    fn image_to_frame(&self, img: mwine_grpc::Image) -> Result<Frame, DisplayError> {
        let fmt = img.format.unwrap_or_default();
        let w = fmt.width as u32;
        let h = fmt.height as u32;
        if img.data.is_empty() {
            return Err(DisplayError::EmptyFrame);
        }
        Ok(Frame {
            width: w,
            height: h,
            data: img.data,
            timestamp: img.timestamp as u64,
        })
    }
}
