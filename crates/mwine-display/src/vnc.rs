//! VNC 备选后端：qemu VNC server 的纯 std RFB 3.8 客户端。
//!
//! 启用条件：emulator 透传 qemu 参数 `-qemu -vnc 127.0.0.1:0`（监听 5900）。
//! 注意 goldfish 显示管线下 VNC 可能无帧推送（`-gpu guest`
//! 限制）——因此定位为 gRPC 不可用时的人工排查备选，不是产品主路径。
//!
//! 实现：仅无认证（安全类型 None=1）；请求 32bpp 小端真彩像素
//! （内存序 BGRA，直达 wgpu bgra 纹理）；编码仅 Raw(0) + CopyRect(1)
//! + DesktopSize(-223) 伪编码（分辨率变化通知）。

use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

use crate::{
    DisplayBackend, DisplayConfig, DisplayError, DisplayHandle, DisplayInfo, FrameBuffer,
    FrameSource, PixelFormat, Result,
};

const ENC_RAW: i32 = 0;
const ENC_COPY_RECT: i32 = 1;
const ENC_DESKTOP_SIZE: i32 = -223;

/// 一批服务器消息的处理结果。
#[derive(Debug, Clone, Copy)]
enum BatchOutcome {
    /// FramebufferUpdate（可能伴随分辨率变化）
    Update { resized: Option<(u32, u32)> },
    /// 无像素消息（Bell / ServerCutText / 色表），继续等待
    Skipped,
}

/// VNC 连接（RFB 3.8，阻塞 IO）。
pub struct VncConnection {
    stream: TcpStream,
    pub width: u32,
    pub height: u32,
    fb: Vec<u8>,
    /// 已请求过至少一次全量刷新。
    primed: bool,
}

impl VncConnection {
    /// 连接并完成 RFB 握手 + 像素格式/编码协商。
    pub fn connect(addr: &str) -> Result<Self> {
        let mut stream = TcpStream::connect(addr).map_err(|e| DisplayError::GrpcConnection {
            endpoint: addr.to_string(),
            detail: format!("VNC TCP 连接失败: {e}"),
        })?;
        stream.set_nodelay(true).ok();

        // --- 版本 ---
        stream.write_all(b"RFB 003.008\n")?;
        let server_version = read_bytes::<12>(&mut stream)?;
        if !server_version.starts_with(b"RFB ") {
            return Err(DisplayError::VncProtocol(format!(
                "非 RFB 协议应答: {:?}",
                String::from_utf8_lossy(&server_version)
            )));
        }

        // --- 安全类型（3.8：u8 数量 + 类型列表；数量 0 = u32 长度 + 原因）---
        let count = read_u8(&mut stream)?;
        if count == 0 {
            let len = read_u32(&mut stream)? as usize;
            let reason = read_exact(&mut stream, len)?;
            return Err(DisplayError::VncProtocol(format!(
                "VNC 拒绝连接: {}",
                String::from_utf8_lossy(&reason)
            )));
        }
        let types = read_exact(&mut stream, count as usize)?;
        if !types.contains(&1) {
            return Err(DisplayError::VncAuthUnsupported(types));
        }
        write_u8(&mut stream, 1)?; // 选择 None
        let result = read_u32(&mut stream)?;
        if result != 0 {
            return Err(DisplayError::VncProtocol(format!(
                "VNC 安全握手失败 result={result}"
            )));
        }

        // --- ClientInit / ServerInit ---
        write_u8(&mut stream, 1)?; // 共享连接
        let width = read_u16(&mut stream)? as u32;
        let height = read_u16(&mut stream)? as u32;
        let _server_pf = read_bytes::<16>(&mut stream)?;
        let name_len = read_u32(&mut stream)? as usize;
        let _name = read_exact(&mut stream, name_len)?;

        let mut conn = Self {
            stream,
            width,
            height,
            fb: vec![0u8; FrameBuffer::expected_len(width, height)],
            primed: false,
        };
        conn.set_pixel_format()?;
        conn.set_encodings()?;
        Ok(conn)
    }

    /// 请求 32bpp/24depth/小端/真彩，[b,g,r,x] 内存序（Bgra）。
    fn set_pixel_format(&mut self) -> Result<()> {
        let mut msg = [0u8; 20];
        msg[0] = 0; // SetPixelFormat
        // msg[1..4] padding
        msg[4] = 32; // bits-per-pixel
        msg[5] = 24; // depth
        msg[6] = 0; // little-endian
        msg[7] = 1; // true colour
        msg[8..10].copy_from_slice(&255u16.to_be_bytes()); // red max
        msg[10..12].copy_from_slice(&255u16.to_be_bytes()); // green max
        msg[12..14].copy_from_slice(&255u16.to_be_bytes()); // blue max
        msg[14] = 16; // red shift
        msg[15] = 8; // green shift
        msg[16] = 0; // blue shift
        self.stream.write_all(&msg)?;
        Ok(())
    }

    fn set_encodings(&mut self) -> Result<()> {
        let mut msg = Vec::with_capacity(12);
        msg.push(2u8); // SetEncodings
        msg.push(0); // padding
        msg.extend_from_slice(&2u16.to_be_bytes()); // 数量
        msg.extend_from_slice(&ENC_RAW.to_be_bytes());
        msg.extend_from_slice(&ENC_DESKTOP_SIZE.to_be_bytes());
        self.stream.write_all(&msg)?;
        Ok(())
    }

    fn request_update(&mut self, incremental: bool) -> Result<()> {
        let mut msg = [0u8; 10];
        msg[0] = 3; // FramebufferUpdateRequest
        msg[1] = u8::from(incremental);
        // 请求矩形：从 (0,0) 覆盖整屏
        msg[6..8].copy_from_slice(&(self.width as u16).to_be_bytes());
        msg[8..10].copy_from_slice(&(self.height as u16).to_be_bytes());
        self.stream.write_all(&msg)?;
        Ok(())
    }

    /// 读取一批服务器消息。FramebufferUpdate 应用到帧缓冲；无像素消息
    /// （Bell/CutText/色表）返回 Skipped 供上层继续等待。
    fn apply_update_batch(&mut self) -> Result<BatchOutcome> {
        let msg_type = read_u8(&mut self.stream)?;
        match msg_type {
            0 => {
                let _padding = read_u8(&mut self.stream)?;
                let num_rects = read_u16(&mut self.stream)?;
                let mut resize = None;
                for _ in 0..num_rects {
                    let x = read_u16(&mut self.stream)? as u32;
                    let y = read_u16(&mut self.stream)? as u32;
                    let w = read_u16(&mut self.stream)? as u32;
                    let h = read_u16(&mut self.stream)? as u32;
                    let encoding = read_i32(&mut self.stream)?;
                    match encoding {
                        ENC_RAW => {
                            let len = FrameBuffer::expected_len(w, h);
                            let data = read_exact(&mut self.stream, len)?;
                            blit(&mut self.fb, self.width, x, y, w, h, &data)?;
                        }
                        ENC_COPY_RECT => {
                            let src_x = read_u16(&mut self.stream)? as u32;
                            let src_y = read_u16(&mut self.stream)? as u32;
                            copy_rect(&mut self.fb, self.width, (src_x, src_y), (x, y), w, h)?;
                        }
                        ENC_DESKTOP_SIZE => {
                            // 伪编码：w/h = 新桌面尺寸，无像素数据
                            resize = Some((w, h));
                        }
                        other => return Err(DisplayError::VncUnsupportedEncoding(other)),
                    }
                }
                Ok(BatchOutcome::Update { resized: resize })
            }
            1 => Ok(BatchOutcome::Skipped), // Bell：无负载
            2 => {
                // SetColourMapEntries: pad u8, first u16, count u16, count*6 字节
                let _ = read_u8(&mut self.stream)?;
                let _first = read_u16(&mut self.stream)?;
                let count = read_u16(&mut self.stream)?;
                read_exact(&mut self.stream, count as usize * 6)?;
                Ok(BatchOutcome::Skipped)
            }
            3 => {
                // ServerCutText: 3 字节 pad + u32 len（新协议首字节为符号位）+ 文本
                read_exact(&mut self.stream, 3)?;
                let len = read_u32(&mut self.stream)? & 0x7FFF_FFFF;
                read_exact(&mut self.stream, len as usize)?;
                Ok(BatchOutcome::Skipped)
            }
            other => Err(DisplayError::VncProtocol(format!(
                "未知服务器消息类型 {other}"
            ))),
        }
    }

    /// 拉一帧：发增量/全量请求后阻塞等下一个矩形批（跳过无像素消息）。
    pub fn read_frame(&mut self) -> Result<FrameBuffer> {
        loop {
            self.request_update(self.primed)?;
            self.primed = true;
            loop {
                match self.apply_update_batch()? {
                    BatchOutcome::Skipped => continue, // Bell 等无像素消息，继续等矩形
                    BatchOutcome::Update { resized: Some((w, h)) } => {
                        // 分辨率变化：重置帧缓冲并重新全量
                        self.width = w;
                        self.height = h;
                        self.fb = vec![0u8; FrameBuffer::expected_len(w, h)];
                        self.primed = false;
                        break; // 回外层重新请求
                    }
                    BatchOutcome::Update { resized: None } => {
                        return FrameBuffer::new(
                            self.width,
                            self.height,
                            PixelFormat::Bgra,
                            self.fb.clone(),
                            0,
                            0,
                        );
                    }
                }
            }
        }
    }

    pub fn set_read_timeout(&mut self, timeout: Option<Duration>) -> Result<()> {
        self.stream.set_read_timeout(timeout)?;
        Ok(())
    }
}

/// 把 Raw 矩形写入帧缓冲（行对齐逐行拷贝，越界即错）。
fn blit(fb: &mut [u8], fb_w: u32, x: u32, y: u32, w: u32, h: u32, data: &[u8]) -> Result<()> {
    // 行边界校验：矩形不得越过右/下边缘（否则像素会"绕"进下一行）
    if x + w > fb_w || data.len() != FrameBuffer::expected_len(w, h) {
        return Err(DisplayError::VncProtocol(format!(
            "矩形越界或数据长度不符: rect({x},{y},{w},{h}) data_len={}",
            data.len()
        )));
    }
    let stride = fb_w as usize * 4;
    let fb_h = (fb.len() / stride) as u32;
    if y + h > fb_h {
        return Err(DisplayError::VncProtocol(format!(
            "矩形纵向越界: y{y}+h{h} > {fb_h}"
        )));
    }
    for row in 0..h as usize {
        let dst = (y as usize + row) * stride + x as usize * 4;
        let src = row * w as usize * 4;
        let n = w as usize * 4;
        if dst + n > fb.len() || src + n > data.len() {
            return Err(DisplayError::VncProtocol(format!(
                "矩形越界: rect({x},{y},{w},{h}) row{row}"
            )));
        }
        fb[dst..dst + n].copy_from_slice(&data[src..src + n]);
    }
    Ok(())
}

/// 源矩形 → 目标矩形搬运（重叠也安全：先拷出）。
fn copy_rect(
    fb: &mut [u8],
    fb_w: u32,
    (src_x, src_y): (u32, u32),
    (dst_x, dst_y): (u32, u32),
    w: u32,
    h: u32,
) -> Result<()> {
    let stride = fb_w as usize * 4;
    let fb_h = (fb.len() / stride) as u32;
    if src_x + w > fb_w || dst_x + w > fb_w || src_y + h > fb_h || dst_y + h > fb_h {
        return Err(DisplayError::VncProtocol(format!(
            "copyrect 越界: src({src_x},{src_y}) dst({dst_x},{dst_y}) {w}x{h}"
        )));
    }
    let n = w as usize * 4;
    let mut tmp = vec![0u8; n * h as usize];
    for row in 0..h as usize {
        let src = (src_y as usize + row) * stride + src_x as usize * 4;
        if src + n > fb.len() {
            return Err(DisplayError::VncProtocol("copyrect 源越界".into()));
        }
        tmp[row * n..(row + 1) * n].copy_from_slice(&fb[src..src + n]);
    }
    for row in 0..h as usize {
        let dst = (dst_y as usize + row) * stride + dst_x as usize * 4;
        if dst + n > fb.len() {
            return Err(DisplayError::VncProtocol("copyrect 目标越界".into()));
        }
        fb[dst..dst + n].copy_from_slice(&tmp[row * n..(row + 1) * n]);
    }
    Ok(())
}

/// VNC 后端：仅帧获取；显示器管理（multidisplay）走 gRPC/console。
pub struct VncDisplayBackend {
    conn: VncConnection,
}

impl VncDisplayBackend {
    pub fn connect(addr: &str) -> Result<Self> {
        Ok(Self {
            conn: VncConnection::connect(addr)?,
        })
    }

    pub fn into_frame_source(self) -> VncFrameSource {
        VncFrameSource { conn: self.conn }
    }
}

impl DisplayBackend for VncDisplayBackend {
    fn create_display(&mut self, _config: DisplayConfig) -> Result<DisplayHandle> {
        Err(DisplayError::VncProtocol(
            "VNC 后端不支持显示器管理（multidisplay 请用 gRPC 后端或 console）".into(),
        ))
    }

    fn remove_display(&mut self, _display_id: u32) -> Result<()> {
        Err(DisplayError::VncProtocol(
            "VNC 后端不支持显示器管理（multidisplay 请用 gRPC 后端或 console）".into(),
        ))
    }

    fn list_displays(&mut self) -> Result<Vec<DisplayInfo>> {
        Ok(vec![DisplayInfo {
            display_id: 0,
            width: self.conn.width,
            height: self.conn.height,
            dpi: 0,
        }])
    }

    fn get_framebuffer(&mut self, _display_id: u32) -> Result<FrameBuffer> {
        self.conn.read_frame()
    }
}

/// VNC 增量帧流。
pub struct VncFrameSource {
    conn: VncConnection,
}

impl FrameSource for VncFrameSource {
    fn next_frame(&mut self) -> Result<FrameBuffer> {
        self.conn.read_frame()
    }
}

// ---------------------------------------------------------------------------
// 基础 IO
// ---------------------------------------------------------------------------

fn read_exact<R: Read>(r: &mut R, n: usize) -> Result<Vec<u8>> {
    let mut buf = vec![0u8; n];
    r.read_exact(&mut buf)?;
    Ok(buf)
}

fn read_bytes<const N: usize>(r: &mut TcpStream) -> Result<[u8; N]> {
    let mut buf = [0u8; N];
    r.read_exact(&mut buf)?;
    Ok(buf)
}

fn read_u8(r: &mut TcpStream) -> Result<u8> {
    Ok(read_bytes::<1>(r)?[0])
}

fn read_u16(r: &mut TcpStream) -> Result<u16> {
    Ok(u16::from_be_bytes(read_bytes::<2>(r)?))
}

fn read_u32(r: &mut TcpStream) -> Result<u32> {
    Ok(u32::from_be_bytes(read_bytes::<4>(r)?))
}

fn read_i32(r: &mut TcpStream) -> Result<i32> {
    Ok(i32::from_be_bytes(read_bytes::<4>(r)?))
}

fn write_u8(w: &mut TcpStream, v: u8) -> Result<()> {
    w.write_all(&[v])?;
    Ok(())
}

// ---------------------------------------------------------------------------
// 测试：进程内假 RFB 服务器，驱动完整握手 + 各类矩形更新
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    /// 假服务器：4x2 桌面；握手→全量 Raw 帧→Bell/CutText 干扰→部分矩形
    /// →DesktopSize 扩到 2x2→（客户端重置后再全量）→结束。
    fn spawn_fake_vnc() -> (String, std::thread::JoinHandle<()>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap().to_string();
        let handle = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            // 版本
            let mut v = [0u8; 12];
            s.read_exact(&mut v).unwrap();
            assert_eq!(&v, b"RFB 003.008\n");
            s.write_all(b"RFB 003.008\n").unwrap();
            // 安全类型：None
            s.write_all(&[1u8, 1u8]).unwrap();
            let mut choice = [0u8; 1];
            s.read_exact(&mut choice).unwrap();
            assert_eq!(choice[0], 1);
            s.write_all(&0u32.to_be_bytes()).unwrap(); // OK
            // ClientInit
            let mut shared = [0u8; 1];
            s.read_exact(&mut shared).unwrap();
            assert_eq!(shared[0], 1);
            // ServerInit：4x2 + 任一 pixel format + 名字
            let mut init = Vec::new();
            init.extend_from_slice(&4u16.to_be_bytes());
            init.extend_from_slice(&2u16.to_be_bytes());
            init.extend_from_slice(&[32, 24, 0, 1]); // bpp/depth/LE/truecolor
            init.extend_from_slice(&255u16.to_be_bytes());
            init.extend_from_slice(&255u16.to_be_bytes());
            init.extend_from_slice(&255u16.to_be_bytes());
            init.extend_from_slice(&[16, 8, 0, 0, 0, 0]);
            init.extend_from_slice(&4u32.to_be_bytes());
            init.extend_from_slice(b"fake");
            s.write_all(&init).unwrap();
            // SetPixelFormat（校验我们请求的 BGRA 小端格式）
            let mut pf = [0u8; 20];
            s.read_exact(&mut pf).unwrap();
            assert_eq!(pf[0], 0);
            assert_eq!((pf[4], pf[6], pf[14]), (32, 0, 16));
            // SetEncodings：type+pad+count(4B 头)，随后 count×4B 编码号
            let mut enc = [0u8; 4];
            s.read_exact(&mut enc).unwrap();
            assert_eq!(&enc[..2], &[2, 0]);
            assert_eq!(u16::from_be_bytes(enc[2..4].try_into().unwrap()), 2);
            let mut raw_enc = [0u8; 8];
            s.read_exact(&mut raw_enc).unwrap();
            assert_eq!(i32::from_be_bytes(raw_enc[0..4].try_into().unwrap()), ENC_RAW);
            assert_eq!(i32::from_be_bytes(raw_enc[4..8].try_into().unwrap()), ENC_DESKTOP_SIZE);
            // 第一个 FBU 请求：全量
            let mut req = [0u8; 10];
            s.read_exact(&mut req).unwrap();
            assert_eq!((req[0], req[1]), (3, 0));
            assert_eq!(u16::from_be_bytes(req[6..8].try_into().unwrap()), 4);
            // 全屏 Raw：8 像素
            let mut frame = vec![0u8; 8 * 4];
            for (i, px) in frame.chunks_exact_mut(4).enumerate() {
                px.copy_from_slice(&[i as u8, 0xAA, 0xBB, 0xFF]);
            }
            let mut update = vec![0u8, 0u8]; // 类型 0 + pad
            update.extend_from_slice(&1u16.to_be_bytes());
            update.extend_from_slice(&0u16.to_be_bytes());
            update.extend_from_slice(&0u16.to_be_bytes());
            update.extend_from_slice(&4u16.to_be_bytes());
            update.extend_from_slice(&2u16.to_be_bytes());
            update.extend_from_slice(&ENC_RAW.to_be_bytes());
            update.extend_from_slice(&frame);
            s.write_all(&update).unwrap();
            // 第二次请求（增量）——中间插 Bell + CutText 干扰
            s.write_all(&[1u8]).unwrap(); // Bell
            s.write_all(&[3u8, 0, 0, 0]).unwrap(); // CutText
            s.write_all(&5u32.to_be_bytes()).unwrap();
            s.write_all(b"clip!");
            let mut req2 = [0u8; 10];
            s.read_exact(&mut req2).unwrap();
            assert_eq!((req2[0], req2[1]), (3, 1));
            // 部分矩形：仅 (2,1) 起 1x1
            let mut partial = vec![0u8, 0u8];
            partial.extend_from_slice(&1u16.to_be_bytes());
            partial.extend_from_slice(&2u16.to_be_bytes());
            partial.extend_from_slice(&1u16.to_be_bytes());
            partial.extend_from_slice(&1u16.to_be_bytes());
            partial.extend_from_slice(&1u16.to_be_bytes());
            partial.extend_from_slice(&ENC_RAW.to_be_bytes());
            partial.extend_from_slice(&[9, 9, 9, 9]);
            s.write_all(&partial).unwrap();
            // 第三次请求：DesktopSize 伪编码 → 6x2
            let mut req3 = [0u8; 10];
            s.read_exact(&mut req3).unwrap();
            let mut resize = vec![0u8, 0u8];
            resize.extend_from_slice(&1u16.to_be_bytes());
            resize.extend_from_slice(&0u16.to_be_bytes());
            resize.extend_from_slice(&0u16.to_be_bytes());
            resize.extend_from_slice(&6u16.to_be_bytes());
            resize.extend_from_slice(&2u16.to_be_bytes());
            resize.extend_from_slice(&ENC_DESKTOP_SIZE.to_be_bytes());
            s.write_all(&resize).unwrap();
            // 第四次（重置后应再次全量）：回一帧 6x2 全量再关闭
            let mut req4 = [0u8; 10];
            s.read_exact(&mut req4).unwrap();
            assert_eq!((req4[0], req4[1]), (3, 0));
            let mut full6 = vec![0u8, 0u8];
            full6.extend_from_slice(&1u16.to_be_bytes());
            full6.extend_from_slice(&0u16.to_be_bytes());
            full6.extend_from_slice(&0u16.to_be_bytes());
            full6.extend_from_slice(&6u16.to_be_bytes());
            full6.extend_from_slice(&2u16.to_be_bytes());
            full6.extend_from_slice(&ENC_RAW.to_be_bytes());
            full6.extend_from_slice(&[0x11u8; 6 * 2 * 4]);
            s.write_all(&full6).unwrap();
            // 等客户端收完（读了该帧）再关，避免 RST 截断
            std::thread::sleep(std::time::Duration::from_millis(100));
            let _ = s.shutdown(std::net::Shutdown::Both);
        });
        (addr, handle)
    }

    #[test]
    fn handshake_full_partial_resize() {
        let (addr, server) = spawn_fake_vnc();
        let mut conn = VncConnection::connect(&addr).unwrap();
        assert_eq!((conn.width, conn.height), (4, 2));

        // 帧 1：全量
        let f1 = conn.read_frame().unwrap();
        assert_eq!(f1.format, PixelFormat::Bgra);
        assert_eq!(&f1.data[4..8], &[1, 0xAA, 0xBB, 0xFF]); // 像素 1

        // 帧 2：Bell/CutText 被跳过，部分矩形生效
        let f2 = conn.read_frame().unwrap();
        let off = (1 * 4 + 2) * 4; // y=1 行 x=2 列
        assert_eq!(&f2.data[off..off + 4], &[9, 9, 9, 9]);

        // 帧 3：DesktopSize → 6x2 重置（primed 归零）→ 重新全量帧
        let f3 = conn.read_frame().unwrap();
        assert_eq!((conn.width, conn.height), (6, 2));
        assert_eq!((f3.width, f3.height), (6, 2));
        assert_eq!(&f3.data[..4], &[0x11; 4]);
        server.join().unwrap();
    }

    #[test]
    fn blit_and_copyrect_bounds() {
        let mut fb = vec![0u8; 4 * 2 * 4];
        blit(&mut fb, 4, 1, 0, 2, 1, &[1, 2, 3, 4, 5, 6, 7, 8]).unwrap();
        assert_eq!(&fb[4..12], &[1, 2, 3, 4, 5, 6, 7, 8]);
        // 越界报错
        assert!(blit(&mut fb, 4, 3, 0, 2, 1, &[0; 8]).is_err());
        // copyrect (0,0)→(2,1) 1x1
        copy_rect(&mut fb, 4, (1, 0), (2, 1), 1, 1).unwrap();
        assert_eq!(&fb[(1 * 4 + 2) * 4..][..4], &[1, 2, 3, 4]);
        assert!(copy_rect(&mut fb, 4, (3, 0), (0, 0), 2, 1).is_err());
    }
}
