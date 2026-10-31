//! 设备侧桥 APK（mwine-bridge.apk）事件接入。
//! BridgeService（NotificationListenerService）在设备 127.0.0.1:27185 推
//! JSON 行通知事件——传输层直开 `tcp:27185` 流即得实时推送，零轮询。

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::LazyLock;

/// 全局广播：桥事件（通知等）。SSE 端点订阅此通道。
pub static BUS: LazyLock<tokio::sync::broadcast::Sender<String>> =
    LazyLock::new(|| tokio::sync::broadcast::channel(64).0);

static STARTED: AtomicBool = AtomicBool::new(false);

pub fn ensure_started() {
    if STARTED.swap(true, Ordering::Relaxed) {
        return;
    }
    std::thread::spawn(|| loop {
        // 当前在线真机（桥 APK 只装在手机上）
        let Some(serial) = crate::state::online_real_devices_first() else {
            std::thread::sleep(std::time::Duration::from_secs(5));
            continue;
        };
        // adbd 的 `tcp:27185` 流服务受限（FAIL，实测）——走官方 forward 特权路径：
        // 本地端口 → 设备 127.0.0.1:27185，再本地 TCP 接入
        let port = 27185u16;
        if let Err(e) = crate::adb::forward_service(&serial, port, "tcp:27185") {
            tracing::debug!("桥 forward 建立失败（{e}）");
            std::thread::sleep(std::time::Duration::from_secs(5));
            continue;
        }
        // forward 监听线程 bind 有毫秒级延迟——connect 重试 5×200ms
        let mut sock = None;
        for _ in 0..5 {
            match std::net::TcpStream::connect(("127.0.0.1", port)) {
                Ok(s) => { sock = Some(s); break; }
                Err(_) => std::thread::sleep(std::time::Duration::from_millis(200)),
            }
        }
        match sock {
            Some(sock) => {
                tracing::info!("桥 APK 通知流已连接（{serial}，经 forward）");
                sock.set_read_timeout(Some(std::time::Duration::from_secs(60))).ok();
                let mut rd = sock;
                use std::io::BufRead;
                let mut reader = std::io::BufReader::new(rd.try_clone().unwrap());
                let mut line = String::new();
                loop {
                    line.clear();
                    match reader.read_line(&mut line) {
                        Ok(0) | Err(_) => break,
                        Ok(_) => {
                            let l = line.trim();
                            // 桥的握手帧 {"event":"hello"} 不是通知——过滤掉
                            //（不过滤会以「设备通知 无内容」出现在通知中心）
                            if l.starts_with('{') && !l.contains("\"event\"") {
                                let _ = BUS.send(l.to_string());
                            }
                        }
                    }
                }
                tracing::debug!("桥通知流断开，5s 后重连");
            }
            None => {
                tracing::debug!("桥本地连接失败，5s 后重试");
            }
        }
        let _ = crate::adb::remove_forward(&serial, port);
        std::thread::sleep(std::time::Duration::from_secs(5));
    });
}

fn transport_conn(serial: &str) -> Option<std::sync::Arc<crate::transport::DeviceConn>> {
    crate::transport::get_conn(serial, None).ok()
}
