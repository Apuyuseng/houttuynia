//! 守护进程主体

use crate::api;
use std::sync::{Arc, Mutex};

pub struct AppState {
    pub qemu_pid: Mutex<Option<u32>>,
    /// vm_start 后台的错误信息（UI 轮询 status 时带回显示；空=无错误）
    pub vm_error: Mutex<String>,
    pub adb: mwine_adb::AdbClient,
    /// emulator 子进程本体：wait() 收尸，防僵尸导致 kill(pid,0) 恒真 → "already" 死锁
    pub qemu_child: Mutex<Option<std::process::Child>>,
    /// 镜像导入/拉取的当前任务状态（UI 轮询 /api/image/task）
    pub image_task: Mutex<crate::api::ImageTask>,
    /// 当前镜像任务的取消令牌（/api/image/cancel 置 true）
    pub image_cancel: Mutex<mwine_supervisor::cloud::CancelFlag>,
    /// 本守护进程拉起的 VM serial（多设备在线时安装/启动的优先目标）
    pub vm_serial: Mutex<Option<String>>,
}

pub fn run(port: u16) {
    // 通知桥自启：mwine notify 是宿主常驻监听（已幂等：装 companion/授权/forward/重连）
    spawn_notify_bridge();

    let state = Arc::new(AppState {
        vm_error: Mutex::new(String::new()),
        qemu_pid: Mutex::new(None),
        qemu_child: Mutex::new(None),
        adb: mwine_adb::AdbClient::new(),
        image_task: Mutex::new(crate::api::ImageTask::default()),
        image_cancel: Mutex::new(std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false))),
        vm_serial: Mutex::new(None),
    });

    let addr = format!("127.0.0.1:{port}");
    let server = tiny_http::Server::http(&addr).expect("无法绑定端口");
    println!("✓ MWine Core 就绪: http://{addr}");

    for request in server.incoming_requests() {
        let method = request.method().to_string();
        let url = request.url().to_string();
        let state = state.clone();

        // 每请求一线程：慢端点（vm_start/apps 等）不能阻塞 UI 的 2s 状态轮询
        std::thread::spawn(move || {
            let mut request = request;
            // 读取请求体（POST 配置等）；限制 64KB
            let mut body = String::new();
            {
                use std::io::Read;
                let mut reader = request.as_reader().take(64 * 1024);
                let _ = reader.read_to_string(&mut body);
            }
            let (status, resp_body) = api::route_with_body(&method, &url, &body, &state);
            let response = tiny_http::Response::from_string(resp_body)
                .with_status_code(status)
                .with_header(
                    tiny_http::Header::from_bytes(&b"Content-Type"[..], &b"application/json"[..])
                        .unwrap(),
                );
            request.respond(response).ok();
        });
    }
}

/// 拉起 `mwine notify`（guest→macOS 通知桥）。失败静默（无 VM 时循环重试开销可接受）。
fn spawn_notify_bridge() {
    std::thread::spawn(|| loop {
        let exe = std::env::current_exe().ok();
        let cli = exe
            .as_ref()
            .and_then(|e| e.parent())
            .map(|p| p.join("mwine"))
            .filter(|p| p.is_file());
        let Some(cli) = cli else { return };
        let _ = std::process::Command::new(&cli)
            .arg("notify")
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
        // notify 退出（VM 不在等场景内部 2s 重试；进程被杀才到这里）→ 5 分钟后重拉
        std::thread::sleep(std::time::Duration::from_secs(300));
    });
}
