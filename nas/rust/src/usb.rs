//! USB 传输层（双平台后端，公共接口统一）：
//! - Linux：usbfs 直驱（sysfs 枚举 + ioctl bulk）——NAS 容器生产路径，零 C 依赖。
//! - macOS：nusb（纯 Rust IOKit 绑定）——开发机真机联调路径（Cargo 仅在 macOS 引入）。
//!
//! 职责：枚举带 ADB 接口（class ff / subclass 42 / protocol 01）的 USB 设备 →
//! 打开设备 → 认领 ADB 接口 → bulk 双向读写（读写半分离：读线程独占读半，
//! 写侧经 split() 克隆，互不阻塞）。ADB 协议帧在 transport.rs。

/// 一个可用 ADB USB 设备的描述（扫描结果）
#[derive(Debug, Clone)]
pub struct UsbAdbDevice {
    pub serial: String,
    pub bus: u16,
    pub dev: u16,
    pub ifnum: u8,
    pub ep_out: u8,
    pub ep_in: u8,
    pub max_packet: u16,
    /// Linux: /dev/bus/usb/{bus:03}/{dev:03}；macOS: 信息性标识
    pub node: String,
}

/// ADB 接口特征（class=vendor-specific ff / subclass 42 / protocol 01）
const IFACE_CLASS: u8 = 0xff;
const IFACE_SUBCLASS: u8 = 0x42;
const IFACE_PROTOCOL: u8 = 0x01;

// =====================================================================
// Linux：usbfs 直驱
// =====================================================================
#[cfg(target_os = "linux")]
mod imp {
    use super::*;
    use std::fs;
    use std::io;
    use std::os::unix::io::AsRawFd;

    const SYS_USB: &str = "/sys/bus/usb/devices";

    fn read_trim(p: &str) -> String {
        fs::read_to_string(p).unwrap_or_default().trim().to_string()
    }

    /// 扫描全部带 ADB 接口的 USB 设备（sysfs 是内核全局视图，容器内免挂载可读）
    pub fn scan() -> Vec<UsbAdbDevice> {
        let mut out = Vec::new();
        let Ok(rd) = fs::read_dir(SYS_USB) else { return out };
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            // 设备端口形如 1-2 / 2-1.3；根 hub（usbN）与接口（含 :）跳过
            if !name.contains('-') || name.contains(':') || name.starts_with("usb") {
                continue;
            }
            let base = format!("{SYS_USB}/{name}");
            let mut iface_dir = None;
            let mut ifnum = None;
            if let Ok(ifaces) = fs::read_dir(&base) {
                for i in ifaces.flatten() {
                    let iname = i.file_name().to_string_lossy().to_string();
                    if !iname.contains(':') {
                        continue;
                    }
                    let ibase = format!("{}/{}", base, iname);
                    if read_trim(&format!("{ibase}/bInterfaceClass")) == "ff"
                        && read_trim(&format!("{ibase}/bInterfaceSubClass")) == "42"
                        && read_trim(&format!("{ibase}/bInterfaceProtocol")) == "01"
                    {
                        // bInterfaceNumber 从接口名解析（1-2:1.2 → 末段 2）
                        ifnum = iname.rsplit('.').next().and_then(|s| s.parse().ok());
                        iface_dir = Some(iname.clone());
                        break;
                    }
                }
            }
            let (Some(iface_dir), Some(ifnum)) = (iface_dir, ifnum) else { continue };

            let bus = read_trim(&format!("{base}/busnum")).parse().unwrap_or(0);
            let dev = read_trim(&format!("{base}/devnum")).parse().unwrap_or(0);
            if bus == 0 || dev == 0 {
                continue;
            }
            let serial = read_trim(&format!("{base}/serial"));
            let serial = if serial.is_empty() { format!("usb-{name}") } else { serial };

            // ADB 接口的 bulk 双端点（sysfs 不暴露传输类型；ADB 接口保证
            // 只有一对 bulk 端点，非 IN 即 OUT）
            let epbase = format!("{SYS_USB}/{iface_dir}");
            let mut ep_out = None;
            let mut ep_in = None;
            let mut max_packet = 512;
            if let Ok(eps) = fs::read_dir(&epbase) {
                for ep in eps.flatten() {
                    let epname = ep.file_name().to_string_lossy().to_string();
                    if !epname.starts_with("ep_") {
                        continue;
                    }
                    let eb = format!("{}/{}", epbase, epname);
                    let addr =
                        u8::from_str_radix(&read_trim(&format!("{eb}/bEndpointAddress")), 16)
                            .unwrap_or(0);
                    let wmax = read_trim(&format!("{eb}/wMaxPacketSize"))
                        .parse()
                        .unwrap_or(512);
                    if addr & 0x80 != 0 {
                        ep_in = Some(addr);
                    } else {
                        ep_out = Some(addr);
                    }
                    max_packet = wmax.max(8);
                }
            }
            let (Some(ep_out), Some(ep_in)) = (ep_out, ep_in) else { continue };

            out.push(UsbAdbDevice {
                serial,
                bus,
                dev,
                ifnum,
                ep_out,
                ep_in,
                max_packet,
                node: format!("/dev/bus/usb/{bus:03}/{dev:03}"),
            });
        }
        out
    }

    const USBDEVFS_RESET: u64 = 0x00005514; // _IO('U', 20)
    const USBDEVFS_CLAIMINTERFACE: u64 = 0x8004550f;

    /// usbfs 设备句柄。读侧独占（reader 线程）；写侧经 split() 用 dup 出的
    /// 同一 file description（接口认领状态随 file 共享，bulk 收发内核按端点串行）。
    pub struct UsbHandle {
        fd: i32,
        ep_out: u8,
        ep_in: u8,
    }

    /// 写半（dup 出的 fd；与读侧共享认领）
    pub struct UsbWrite {
        fd: i32,
        ep_out: u8,
    }

    /// 同步 bulk 传输（USBDEVFS_BULK：自带超时，免 URB 管理）
    fn bulk(fd: i32, endpoint: u8, buf: &mut [u8], timeout_ms: u32) -> io::Result<usize> {
        #[repr(C)]
        struct UsbFsBulk {
            ep: u32,
            len: u32,
            timeout: u32,
            data: *mut u8,
        }
        const USBDEVFS_BULK: u64 = 0xc0185502; // _IOWR('U', 2, 24)
        let mut bulk = UsbFsBulk {
            ep: endpoint as u32,
            len: buf.len() as u32,
            timeout: timeout_ms,
            data: buf.as_mut_ptr(),
        };
        let rc = unsafe { libc::ioctl(fd, USBDEVFS_BULK as _, &mut bulk as *mut UsbFsBulk) };
        if rc < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(rc as usize)
    }

    impl UsbHandle {
        /// 打开并认领 ADB 接口。
        /// EBUSY：前任句柄 claim 未释放（本进程重试残留/异常退出）——
        /// usbfs DISCONNECTINTERFACE 强制剥离原持有者后重认领（容器内自愈关键）。
        pub fn open(d: &UsbAdbDevice) -> io::Result<UsbHandle> {
            let path = std::ffi::CString::new(d.node.as_str())?;
            let fd = unsafe { libc::open(path.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            let ifnum: u32 = d.ifnum as u32;
            let rc =
                unsafe { libc::ioctl(fd, USBDEVFS_CLAIMINTERFACE as _, &ifnum as *const u32) };
            if rc < 0 {
                let busy = io::Error::last_os_error().raw_os_error() == Some(16);
                if busy {
                    const USBDEVFS_DISCONNECTINTERFACE: u64 = 0x80085510; // _IOW('U', 16, unsigned int)
                    unsafe {
                        libc::ioctl(
                            fd,
                            USBDEVFS_DISCONNECTINTERFACE as _,
                            &ifnum as *const u32,
                        );
                    }
                    std::thread::sleep(std::time::Duration::from_millis(150));
                    let rc2 = unsafe {
                        libc::ioctl(fd, USBDEVFS_CLAIMINTERFACE as _, &ifnum as *const u32)
                    };
                    if rc2 >= 0 {
                        return Ok(UsbHandle { fd, ep_out: d.ep_out, ep_in: d.ep_in });
                    }
                }
                let e = io::Error::last_os_error();
                unsafe { libc::close(fd) };
                return Err(e);
            }
            Ok(UsbHandle { fd, ep_out: d.ep_out, ep_in: d.ep_in })
        }

        /// bulk 读（≤buf.len()，返回实际读取；20s 超时供读循环判活）。
        /// EPROTO(71)：数据翻转错位（上一会话残留）——CLEAR_HALT 后重试一次。
        pub fn read(&self, buf: &mut [u8]) -> std::io::Result<usize> {
            match bulk(self.fd, self.ep_in | 0x80, buf, 20000) {
                Ok(n) => Ok(n),
                Err(e) if e.raw_os_error() == Some(71) => {
                    const USBDEVFS_CLEAR_HALT: u64 = 0x80085503; // _IOW('U', 3, unsigned int)
                    let ep = self.ep_in | 0x80;
                    unsafe {
                        libc::ioctl(self.fd, USBDEVFS_CLEAR_HALT as _, &ep as *const u8);
                    }
                    bulk(self.fd, self.ep_in | 0x80, buf, 20000)
                }
                Err(e) => Err(e),
            }
        }

        /// USB 端口复位（adbd 收到断开后重新监听——旧会话异常退出的恢复手段）
        pub fn reset(&self) -> io::Result<()> {
            let rc = unsafe { libc::ioctl(self.fd, USBDEVFS_RESET as _) };
            if rc < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        }

        /// 分出写半（dup 共享同一打开文件）
        pub fn split(&self) -> io::Result<UsbWrite> {
            let nfd = unsafe { libc::dup(self.fd) };
            if nfd < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(UsbWrite { fd: nfd, ep_out: self.ep_out })
        }
    }

    impl Drop for UsbHandle {
        fn drop(&mut self) {
            unsafe { libc::close(self.fd) };
        }
    }

    impl UsbWrite {
        /// bulk 写（按 16KB 分块——usbfs 单次传输尺寸上限的保守值）
        pub fn write_all(&self, mut data: &[u8]) -> io::Result<()> {
            while !data.is_empty() {
                let n = data.len().min(16 * 1024);
                let mut chunk = data[..n].to_vec();
                let done = bulk(self.fd, self.ep_out, &mut chunk, 20000)?;
                data = &data[done..];
            }
            Ok(())
        }
    }

    impl Drop for UsbWrite {
        fn drop(&mut self) {
            unsafe { libc::close(self.fd) };
        }
    }
}

// =====================================================================
// macOS：nusb（IOKit）
// =====================================================================
#[cfg(target_os = "macos")]
mod imp {
    use super::*;
    use std::io;
    use std::sync::Arc;

    /// 扫描带 ADB 接口的 USB 设备（纯注册表枚举：不开设备、零 IO——
    /// devices() 高频轮询也零总线扰动；USB 地址会变，调用方一律以
    /// serial 为准，端点在 open 时从配置描述符解析）。
    pub fn scan() -> Vec<UsbAdbDevice> {
        let mut out = Vec::new();
        let Ok(list) = nusb::list_devices() else { return out };
        for info in list {
            let Some(adbif) = info.interfaces().find(|i| {
                i.class() == IFACE_CLASS && i.subclass() == IFACE_SUBCLASS && i.protocol() == IFACE_PROTOCOL
            }) else {
                continue;
            };
            // 无序列号的设备不可稳定寻址，跳过（真机都有序列号）
            let Some(serial) = info.serial_number().map(String::from) else { continue };
            if serial.is_empty() {
                continue;
            }
            out.push(UsbAdbDevice {
                serial,
                bus: info.bus_number() as u16,
                dev: info.device_address() as u16,
                ifnum: adbif.interface_number(),
                ep_out: 0,
                ep_in: 0,
                max_packet: 512,
                node: format!(
                    "iokit://{}/{}/{}",
                    info.bus_number(),
                    info.device_address(),
                    adbif.interface_number()
                ),
            });
        }
        out
    }

    /// park 型 block_on（读线程/写调用都是裸线程，无 async 运行时）
    fn block_on<F: std::future::Future>(fut: F) -> F::Output {
        use std::task::{Context, Poll, Wake, Waker};
        struct ParkWaker(std::thread::Thread);
        impl Wake for ParkWaker {
            fn wake(self: Arc<Self>) {
                self.0.unpark();
            }
        }
        let mut fut = Box::pin(fut);
        let waker = Waker::from(Arc::new(ParkWaker(std::thread::current())));
        let mut cx = Context::from_waker(&waker);
        loop {
            match fut.as_mut().poll(&mut cx) {
                Poll::Ready(v) => return v,
                Poll::Pending => std::thread::park(),
            }
        }
    }

    /// IOKit 设备句柄。Interface 内部是 Arc，读写可并发共享
    /// （macOS 无 usbfs 超时机制：bulk_in 阻塞至有数据或断开，读线程长驻无妨）。
    pub struct UsbHandle {
        _dev: nusb::Device,
        iface: Arc<nusb::Interface>,
        ep_out: u8,
        ep_in: u8,
    }

    pub struct UsbWrite {
        iface: Arc<nusb::Interface>,
        ep_out: u8,
    }

    /// 从配置描述符解析 ADB 接口的 bulk 双端点
    fn adb_endpoints(dev: &nusb::Device, ifnum: u8) -> io::Result<(u8, u8)> {
        for cfg in dev.configurations() {
            for alt in cfg.interface_alt_settings() {
                if alt.interface_number() != ifnum {
                    continue;
                }
                let mut ep_out = None;
                let mut ep_in = None;
                for ep in alt.endpoints() {
                    if ep.transfer_type() != nusb::transfer::EndpointType::Bulk {
                        continue;
                    }
                    if ep.direction() == nusb::transfer::Direction::In {
                        ep_in = Some(ep.address());
                    } else {
                        ep_out = Some(ep.address());
                    }
                }
                if let (Some(o), Some(i)) = (ep_out, ep_in) {
                    return Ok((o, i));
                }
            }
        }
        Err(io::Error::new(io::ErrorKind::NotFound, "ADB 接口缺少 bulk 双端点"))
    }

    impl UsbHandle {
        /// 打开并认领 ADB 接口。只按 serial 匹配（USB 地址会因重枚举漂移，
        /// 按 bus/addr 兜底曾把集线器错认成手机）。
        pub fn open(d: &UsbAdbDevice) -> io::Result<UsbHandle> {
            let target = nusb::list_devices()
                .map_err(|e| io::Error::new(io::ErrorKind::Other, format!("USB 枚举: {e}")))?
                .find(|i| i.serial_number() == Some(d.serial.as_str()))
                .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "USB 设备已移除"))?;
            let dev = target.open()?;
            let (ep_out, ep_in) = adb_endpoints(&dev, d.ifnum)?;
            let iface = dev.claim_interface(d.ifnum)?;
            Ok(UsbHandle { _dev: dev, iface: Arc::new(iface), ep_out, ep_in })
        }

        /// bulk 读（≤buf.len()；Completion.data 已截断到实际长度）
        pub fn read(&self, buf: &mut [u8]) -> io::Result<usize> {
            let n = buf.len().min(64 * 1024);
            let comp =
                block_on(self.iface.bulk_in(self.ep_in, nusb::transfer::RequestBuffer::new(n)));
            let data = comp.data;
            comp.status.map_err(|e| {
                io::Error::new(io::ErrorKind::ConnectionAborted, format!("USB 传输: {e:?}"))
            })?;
            let n = data.len().min(buf.len());
            buf[..n].copy_from_slice(&data[..n]);
            Ok(n)
        }

        pub fn reset(&self) -> io::Result<()> {
            self._dev.reset()
        }

        pub fn split(&self) -> io::Result<UsbWrite> {
            Ok(UsbWrite { iface: Arc::clone(&self.iface), ep_out: self.ep_out })
        }
    }

    impl UsbWrite {
        pub fn write_all(&self, mut data: &[u8]) -> io::Result<()> {
            while !data.is_empty() {
                let n = data.len().min(16 * 1024);
                let chunk = data[..n].to_vec();
                let comp = block_on(self.iface.bulk_out(self.ep_out, chunk));
                // 成功即整块已发（bulk 单次传输原子语义）
                comp.status.map_err(|e| {
                    io::Error::new(io::ErrorKind::ConnectionAborted, format!("USB 写: {e:?}"))
                })?;
                data = &data[n..];
            }
            Ok(())
        }
    }
}

pub use imp::scan;
#[cfg(target_os = "linux")]
pub use imp::{UsbHandle, UsbWrite};
#[cfg(target_os = "macos")]
pub use imp::{UsbHandle, UsbWrite};
