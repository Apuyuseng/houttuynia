//! 窗口渲染（feature = "render"）：
//! winit 窗口 + wgpu（macOS 上即 Metal 后端）等比缩放显示帧流。
//!
//! 帧来源是 [`LatestFrames`] 信箱（非阻塞 take，无帧时复用上一纹理），
//! 生产者由 grpc::FramePump 或 VNC 帧泵线程驱动——渲染循环永不阻塞在
//! 网络上。Esc / 关窗退出并返回统计。
//!
//! 另内置 [`PatternSource`] 离线测试源（移动彩条），不依赖 emulator
//! 即可验证渲染链路与帧率（`mwine display --pattern`）。

use std::sync::Arc;
use std::time::{Duration, Instant};

use winit::application::ApplicationHandler;
use winit::event::{ElementState, WindowEvent};
use winit::event_loop::{ControlFlow, EventLoop};
use winit::keyboard::{Key, NamedKey};
use winit::window::{Window, WindowId};

use crate::{letterbox, DisplayError, FrameBuffer, FrameSource, LatestFrames, PixelFormat, Result};

const SHADER: &str = r#"
struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var tex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOut {
    var out: VertexOut;
    let p = array<vec2<f32>, 4>(
        vec2<f32>(-1.0,  1.0), vec2<f32>(1.0, 1.0),
        vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0),
    )[idx];
    out.clip = vec4<f32>(p, 0.0, 1.0);
    // wgpu 纹理原点在左上、NDC y 向上 → 翻 v
    out.uv = vec2<f32>((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
    return out;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
    return textureSample(tex, samp, in.uv);
}
"#;

/// 离线测试帧源：移动彩条 + 计数条，验证渲染与帧率，不依赖 emulator。
pub struct PatternSource {
    width: u32,
    height: u32,
    tick: u64,
    start: Instant,
    interval: Duration,
}

impl PatternSource {
    pub fn new(width: u32, height: u32) -> Self {
        Self {
            width,
            height,
            tick: 0,
            start: Instant::now(),
            interval: Duration::from_millis(1000 / 60),
        }
    }
}

impl FrameSource for PatternSource {
    fn next_frame(&mut self) -> Result<FrameBuffer> {
        let target = self.start + self.interval * (self.tick + 1) as u32;
        let now = Instant::now();
        if target > now {
            std::thread::sleep(target - now);
        }
        self.tick += 1;
        let t = self.tick;
        let (w, h) = (self.width as usize, self.height as usize);
        let mut data = vec![0u8; w * h * 4];
        for y in 0..h {
            for x in 0..w {
                let px = &mut data[(y * w + x) * 4..][..4];
                // 对角移动的 RGB 相位带 + 底部进度条
                let phase = ((x + y + (t as usize * 8)) % 512) as f32 / 511.0;
                px[0] = (phase * 255.0) as u8;
                px[1] = (255.0 - phase * 255.0) as u8;
                px[2] = ((x as f32 / w as f32) * 255.0) as u8;
                px[3] = 255;
                if y + 16 > h && x < w * (t as usize % 97) / 96 {
                    px[..3].copy_from_slice(&[255, 255, 255]);
                }
            }
        }
        FrameBuffer::new(
            self.width,
            self.height,
            PixelFormat::Rgba,
            data,
            self.start.elapsed().as_micros() as u64,
            t,
        )
    }
}

/// 窗口渲染统计。
#[derive(Debug, Clone, Copy)]
pub struct WindowStats {
    pub rendered_frames: u64,
    pub duration: Duration,
    pub avg_fps: f64,
    pub dropped_by_mailbox: u64,
}

struct BlitState {
    device: wgpu::Device,
    queue: wgpu::Queue,
    surface: wgpu::Surface<'static>,
    config: wgpu::SurfaceConfiguration,
    pipeline: wgpu::RenderPipeline,
    sampler: wgpu::Sampler,
    bind_group: Option<wgpu::BindGroup>,
    texture: Option<(wgpu::Texture, u32, u32, PixelFormat)>,
}

impl BlitState {
    fn new(window: Arc<Window>) -> Result<Self> {
        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor::default());
        let surface = instance
            .create_surface(window.clone())
            .map_err(|e| DisplayError::Render(format!("创建 surface 失败: {e}")))?;
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            force_fallback_adapter: false,
        }))
        .ok_or_else(|| DisplayError::Render("没有可用 GPU 适配器".into()))?;
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default(), None))
            .map_err(|e| DisplayError::Render(format!("GPU 设备初始化失败: {e}")))?;

        let size = window.inner_size();
        let caps = surface.get_capabilities(&adapter);
        let format = caps
            .formats
            .iter()
            .copied()
            .find(|f| *f == wgpu::TextureFormat::Bgra8UnormSrgb)
            .unwrap_or(caps.formats[0]);
        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format,
            width: size.width.max(1),
            height: size.height.max(1),
            present_mode: wgpu::PresentMode::Fifo, // vsync
            alpha_mode: wgpu::CompositeAlphaMode::Auto,
            view_formats: vec![],
            desired_maximum_frame_latency: 2,
        };
        surface.configure(&device, &config);

        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("mwine-display-blit"),
            source: wgpu::ShaderSource::Wgsl(SHADER.into()),
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("blit-bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("blit-pl"),
            bind_group_layouts: &[&bgl],
            push_constant_ranges: &[],
        });
        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("blit-pipeline"),
            layout: Some(&layout),
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("vs_main"),
                compilation_options: Default::default(),
                buffers: &[],
            },
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fs_main"),
                compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState {
                    format,
                    blend: Some(wgpu::BlendState::ALPHA_BLENDING),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleStrip,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: Default::default(),
            multiview: None,
            cache: None,
        });
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });
        Ok(Self {
            device,
            queue,
            surface,
            config,
            pipeline,
            sampler,
            bind_group: None,
            texture: None,
        })
    }

    /// 帧尺寸/格式变化时重建源纹理并重挂 bind group。
    fn ensure_texture(&mut self, frame: &FrameBuffer) {
        let need_new = match &self.texture {
            Some((_, w, h, fmt)) => *w != frame.width || *h != frame.height || *fmt != frame.format,
            None => true,
        };
        if !need_new {
            return;
        }
        let format = match frame.format {
            // 输入纹理按帧格式直建：gRPC RGBA → Rgba8Unorm；VNC BGRA → Bgra8Unorm。
            // 均用非 srgb 视图（帧数据即目标色彩值，不做二次 gamma）
            PixelFormat::Rgba => wgpu::TextureFormat::Rgba8Unorm,
            PixelFormat::Bgra => wgpu::TextureFormat::Bgra8Unorm,
        };
        let tex = self.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("frame"),
            size: wgpu::Extent3d {
                width: frame.width,
                height: frame.height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        let view = tex.create_view(&Default::default());
        let bind = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("frame-bind"),
            layout: &self.pipeline.get_bind_group_layout(0),
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&self.sampler),
                },
            ],
        });
        self.bind_group = Some(bind);
        self.texture = Some((tex, frame.width, frame.height, frame.format));
    }

    fn upload(&mut self, frame: &FrameBuffer) {
        self.ensure_texture(frame);
        if let Some((tex, ..)) = &self.texture {
            self.queue.write_texture(
                wgpu::TexelCopyTextureInfo {
                    texture: tex,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                &frame.data,
                wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(frame.width * 4),
                    rows_per_image: Some(frame.height),
                },
                wgpu::Extent3d {
                    width: frame.width,
                    height: frame.height,
                    depth_or_array_layers: 1,
                },
            );
        }
    }

    fn resize(&mut self, w: u32, h: u32) {
        if w > 0 && h > 0 {
            self.config.width = w;
            self.config.height = h;
            self.surface.configure(&self.device, &self.config);
        }
    }

    fn draw(&mut self, img_w: u32, img_h: u32, window: &Window) {
        let Some(bind) = self.bind_group.clone() else { return };
        let frame = match self.surface.get_current_texture() {
            Ok(f) => f,
            Err(_) => {
                self.resize(self.config.width.max(1), self.config.height.max(1));
                return;
            }
        };
        let view = frame.texture.create_view(&Default::default());
        let mut enc = self
            .device
            .create_command_encoder(&Default::default());
        {
            let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("blit"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            let (vx, vy, vw, vh) = letterbox(
                self.config.width,
                self.config.height,
                img_w,
                img_h,
                false,
            );
            if vw > 0 && vh > 0 {
                pass.set_viewport(vx as f32, vy as f32, vw as f32, vh as f32, 0.0, 1.0);
                pass.set_pipeline(&self.pipeline);
                pass.set_bind_group(0, &bind, &[]);
                pass.draw(0..4, 0..1);
            }
        }
        self.queue.submit([enc.finish()]);
        frame.present();
        let _ = window.pre_present_notify();
    }
}

struct App {
    window: Option<Arc<Window>>,
    gpu: Option<BlitState>,
    frames: LatestFrames,
    img_size_hint: (u32, u32),
    img_size: (u32, u32),
    rendered: u64,
    started: Instant,
    last_sec: Instant,
    fps_counter: u64,
    title_base: String,
    init_error: Option<crate::DisplayError>,
    /// 自动退出时刻（None=手动 Esc/关窗）。自动验收用。
    auto_exit_at: Option<Instant>,
    exit: bool,
}

impl App {
    fn init_error(&mut self, e: crate::DisplayError) {
        self.init_error = Some(e);
        self.exit = true;
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &winit::event_loop::ActiveEventLoop) {
        if self.window.is_some() {
            return;
        }
        let (hint_w, hint_h) = self.img_size_hint;
        // 窗口不放大过 1080 高、不小于 240
        let scale = (1080.0 / hint_h as f64).min(1.0).max(0.1);
        let win_w = (hint_w as f64 * scale).round().max(240.0) as u32;
        let win_h = (hint_h as f64 * scale).round().max(240.0) as u32;
        let window = match event_loop.create_window(
            winit::window::Window::default_attributes()
                .with_title(&self.title_base)
                .with_inner_size(winit::dpi::PhysicalSize::new(win_w, win_h))
                .with_resizable(true),
        ) {
            Ok(w) => Arc::new(w),
            Err(e) => {
                self.init_error(crate::DisplayError::Render(format!("窗口创建失败: {e}")));
                event_loop.exit();
                return;
            }
        };
        let gpu = match BlitState::new(window.clone()) {
            Ok(g) => g,
            Err(e) => {
                self.init_error(e);
                event_loop.exit();
                return;
            }
        };
        window.request_redraw();
        self.window = Some(window);
        self.gpu = Some(gpu);
    }

    fn window_event(&mut self, event_loop: &winit::event_loop::ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        let (Some(window), Some(gpu)) = (self.window.as_ref(), self.gpu.as_mut()) else {
            return;
        };
        match event {
            WindowEvent::CloseRequested => {
                self.exit = true;
                event_loop.exit();
            }
            WindowEvent::KeyboardInput { event, .. } => {
                // winit 0.30：逻辑键在 logical_key（物理键为 event.physical_key）
                if event.state == ElementState::Pressed
                    && event.logical_key == Key::Named(NamedKey::Escape)
                {
                    self.exit = true;
                    event_loop.exit();
                }
            }
            WindowEvent::Resized(size) => {
                gpu.resize(size.width.max(1), size.height.max(1));
            }
            WindowEvent::RedrawRequested => {
                if let Some(f) = self.frames.take() {
                    gpu.upload(&f);
                    self.img_size = (f.width, f.height);
                    self.rendered += 1;
                }
                if self.img_size.0 > 0 {
                    gpu.draw(self.img_size.0, self.img_size.1, window);
                }
                // 每秒更新一次标题帧率
                self.fps_counter += 1;
                if self.last_sec.elapsed() >= Duration::from_secs(1) {
                    let dropped = self.frames.dropped_count();
                    window.set_title(&format!(
                        "{} — {}fps（累计 {} 帧，丢弃 {}）",
                        self.title_base, self.fps_counter, self.rendered, dropped
                    ));
                    self.fps_counter = 0;
                    self.last_sec = Instant::now();
                }
                window.request_redraw();
            }
            _ => {}
        }
    }

    fn about_to_wait(&mut self, event_loop: &winit::event_loop::ActiveEventLoop) {
        // 自动退出（无人值守验收）
        if let Some(deadline) = self.auto_exit_at {
            if Instant::now() >= deadline {
                self.exit = true;
                event_loop.exit();
                return;
            }
        }
        // 连续渲染模式：等 vsync（PresentMode::Fifo）限速
        if let Some(w) = self.window.as_ref() {
            w.request_redraw();
        }
    }
}

/// 运行窗口渲染循环直到关窗/Esc。
///
/// `initial_size_hint`：(w, h) 期望首帧尺寸，窗口按此适配（无帧时黑屏）。
pub fn run_window(
    frames: LatestFrames,
    title: &str,
    initial_size_hint: Option<(u32, u32)>,
) -> Result<WindowStats> {
    run_window_auto_exit(frames, title, initial_size_hint, None)
}

/// [`run_window`] + 自动退出（无人值守验收）。
pub fn run_window_auto_exit(
    frames: LatestFrames,
    title: &str,
    initial_size_hint: Option<(u32, u32)>,
    auto_exit: Option<std::time::Duration>,
) -> Result<WindowStats> {
    let event_loop =
        EventLoop::new().map_err(|e| DisplayError::Render(format!("事件循环创建失败: {e}")))?;
    event_loop.set_control_flow(ControlFlow::Poll);
    let mut app = App {
        window: None,
        gpu: None,
        frames,
        img_size_hint: initial_size_hint.unwrap_or((720, 1280)),
        img_size: (0, 0),
        rendered: 0,
        started: Instant::now(),
        last_sec: Instant::now(),
        fps_counter: 0,
        title_base: title.to_string(),
        init_error: None,
        auto_exit_at: auto_exit.map(|d| Instant::now() + d),
        exit: false,
    };
    event_loop
        .run_app(&mut app)
        .map_err(|e| DisplayError::Render(format!("事件循环异常退出: {e}")))?;
    if let Some(e) = app.init_error {
        return Err(e);
    }
    let duration = app.started.elapsed();
    Ok(WindowStats {
        rendered_frames: app.rendered,
        duration,
        avg_fps: app.rendered as f64 / duration.as_secs_f64().max(0.001),
        dropped_by_mailbox: app.frames.dropped_count(),
    })
}

#[cfg(all(test, feature = "render"))]
mod tests {
    use super::*;

    #[test]
    fn pattern_source_produces_changing_frames() {
        let mut src = PatternSource::new(64, 32);
        let f1 = src.next_frame().unwrap();
        assert_eq!((f1.width, f1.height), (64, 32));
        assert_eq!(f1.format, PixelFormat::Rgba);
        assert!(f1.data.iter().skip(3).step_by(4).all(|&a| a == 255));
        // 两帧不同（移动彩条）
        let f2 = src.next_frame().unwrap();
        assert_ne!(f1.data, f2.data);
        // 时间节流：60fps 生成 10 帧约 167ms
        let t0 = Instant::now();
        for _ in 0..8 {
            src.next_frame().unwrap();
        }
        let elapsed = t0.elapsed();
        assert!(elapsed >= Duration::from_millis(130), "10 帧应节流到 ~167ms，实际 {elapsed:?}");
    }
}
