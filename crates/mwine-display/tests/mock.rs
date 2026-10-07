//! Mock 实现：接口先行，供上层（mwine-core/UI）
//! 无 emulator 环境下开发联调。

use mwine_display::{
    DisplayBackend, DisplayConfig, DisplayError, DisplayHandle, DisplayInfo, FrameBuffer,
    FrameSource, PixelFormat, Result,
};

/// 可编程 mock：固定尺寸 + 计数彩帧；显示器表内存模拟 gRPC 整表替换语义。
pub struct MockDisplayBackend {
    pub displays: Vec<DisplayInfo>,
    pub width: u32,
    pub height: u32,
    pub tick: u64,
}

impl MockDisplayBackend {
    pub fn new(width: u32, height: u32) -> Self {
        Self {
            displays: vec![DisplayInfo {
                display_id: 0,
                width,
                height,
                dpi: 320,
            }],
            width,
            height,
            tick: 0,
        }
    }
}

impl DisplayBackend for MockDisplayBackend {
    fn create_display(&mut self, config: DisplayConfig) -> Result<DisplayHandle> {
        match self.displays.iter_mut().find(|d| d.display_id == config.display_id) {
            Some(d) => {
                d.width = config.width;
                d.height = config.height;
                d.dpi = config.dpi;
            }
            None => self.displays.push(DisplayInfo {
                display_id: config.display_id,
                width: config.width,
                height: config.height,
                dpi: config.dpi,
            }),
        }
        Ok(DisplayHandle {
            id: config.display_id,
        })
    }

    fn remove_display(&mut self, display_id: u32) -> Result<()> {
        let before = self.displays.len();
        self.displays.retain(|d| d.display_id != display_id);
        if self.displays.len() == before {
            return Err(DisplayError::NoSuchDisplay(display_id));
        }
        Ok(())
    }

    fn list_displays(&mut self) -> Result<Vec<DisplayInfo>> {
        Ok(self.displays.clone())
    }

    fn get_framebuffer(&mut self, display_id: u32) -> Result<FrameBuffer> {
        let d = self
            .displays
            .iter()
            .find(|d| d.display_id == display_id)
            .ok_or(DisplayError::NoSuchDisplay(display_id))?;
        self.tick += 1;
        let v = (self.tick % 256) as u8;
        let data = vec![v, 255 - v, 128, 255u8];
        let mut full = Vec::with_capacity((d.width * d.height * 4) as usize);
        for _ in 0..d.width * d.height {
            full.extend_from_slice(&data);
        }
        FrameBuffer::new(d.width, d.height, PixelFormat::Rgba, full, self.tick, self.tick)
    }
}

/// mock 帧流：按 tick 递增生成帧。
pub struct MockFrameSource {
    backend: MockDisplayBackend,
    display_id: u32,
}

impl MockFrameSource {
    pub fn new(backend: MockDisplayBackend, display_id: u32) -> Self {
        Self {
            backend,
            display_id,
        }
    }
}

impl FrameSource for MockFrameSource {
    fn next_frame(&mut self) -> Result<FrameBuffer> {
        self.backend.get_framebuffer(self.display_id)
    }
}

#[test]
fn mock_backend_display_lifecycle() {
    let mut b = MockDisplayBackend::new(1080, 2400);
    assert_eq!(b.list_displays().unwrap().len(), 1);
    // 追加副屏
    b.create_display(DisplayConfig {
        display_id: 2,
        width: 1280,
        height: 800,
        dpi: 240,
    })
    .unwrap();
    assert_eq!(b.list_displays().unwrap().len(), 2);
    // upsert 同 id 改参数
    b.create_display(DisplayConfig {
        display_id: 2,
        width: 800,
        height: 600,
        dpi: 160,
    })
    .unwrap();
    let d2 = b.list_displays().unwrap().into_iter().find(|d| d.display_id == 2).unwrap();
    assert_eq!((d2.width, d2.height, d2.dpi), (800, 600, 160));
    // 删除
    b.remove_display(2).unwrap();
    assert!(b.remove_display(2).is_err());
}

#[test]
fn mock_frame_source_yields_progressing_frames() {
    let mut src = MockFrameSource::new(MockDisplayBackend::new(4, 4), 0);
    let f1 = src.next_frame().unwrap();
    let f2 = src.next_frame().unwrap();
    assert_ne!(f1.seq, f2.seq);
    assert_eq!(f1.data.len(), 4 * 4 * 4);
    assert_eq!(f1.data[3], 255); // alpha
}

#[test]
fn mock_backend_reports_missing_display() {
    let mut b = MockDisplayBackend::new(64, 64);
    assert!(matches!(
        b.get_framebuffer(5),
        Err(DisplayError::NoSuchDisplay(5))
    ));
}
