//! mwine-nas：Houttuynia NAS 网关（Rust 版守护）。
//!
//! 环境变量：
//!   MWINE_HOST     绑定地址（默认 127.0.0.1；0.0.0.0 对局域网开放）
//!   MWINE_PORT     端口（默认 7788）
//!   MWINE_TOKEN    访问令牌（设置后 /api/* 需 Bearer 或 ?token=；/health 恒开放）
//!   MWINE_WEB_DIR  Web 控制台目录（默认自动探测 web-vue/dist → web/）
//!   MWINE_HEADLESS 无头启动模拟器（-no-window）

mod adb;
mod bridge;
mod adbkeys;
mod api;
mod console;
mod devices;
mod filemgr;
mod icons;
mod meta;
mod pairing;
mod usb;
mod scrcpy;
mod state;
mod tls;
mod transport;
mod guard;

use std::net::IpAddr;
use std::sync::Arc;

use axum::http::StatusCode;
use axum::middleware::{self, Next};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use serde_json::json;
use tower_http::services::{ServeDir, ServeFile};

/// 投屏帧通道消息（已编码的完整帧：15 字节头 + 载荷）。
pub type FrameMsg = Vec<u8>;

// ===================== 投屏喂包线程 =====================

const PKT_CONFIG: u8 = 0;
const PKT_DELTA: u8 = 1;
const PKT_KEY: u8 = 2;
const PKT_META: u8 = 3;
const PKT_AUDIO_CONFIG: u8 = 5; // 音频解码器配置（AAC = AudioSpecificConfig）
const PKT_AUDIO: u8 = 6; // 音频帧

fn encode_frame(typ: u8, pts: u64, data: &[u8]) -> FrameMsg {
    let mut out = Vec::with_capacity(15 + data.len());
    out.extend_from_slice(&0x4D57u16.to_be_bytes());
    out.push(typ);
    out.extend_from_slice(&pts.to_be_bytes());
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(data);
    out
}

/// 音频包 → 帧队列（独立线程：audio socket 阻塞读不影响视频喂包）。
/// 音频流异常只降级（视频继续），绝不置位会话 ended。
fn scrcpy_audio_feeder(sock: std::net::TcpStream, tx: tokio::sync::mpsc::Sender<FrameMsg>) {
    let mut demux = scrcpy::AudioDemuxer::new(sock);
    loop {
        match demux.next_packet() {
            Ok(Some(pkt)) => {
                let typ = if pkt.config { PKT_AUDIO_CONFIG } else { PKT_AUDIO };
                if tx.blocking_send(encode_frame(typ, pkt.pts, &pkt.data)).is_err() {
                    break; // 浏览器已断开（视频线程负责会话清理）
                }
            }
            Ok(None) => break,
            Err(_) => break, // 音频中断仅降级
        }
    }
}

/// 视频包 → 帧队列（客户端断开 → send 失败 → 退出并清理会话）。
/// 传输层自检：CNXN/AUTH → shell → sync 推/拉 全链验证。
/// --adb-test <serial|ip:port>（serial 无冒号走 USB 直驱）
async fn adb_test_entry(target: &str) {
    // 0) 环境准备（macOS 桥模式保证 adb server 存活；Linux 劝退外部 server）
    let _ = adb::ensure_server();
    // CNXN/AUTH（未授权设备会在手机弹「允许 USB 调试」，最长等 120s）
    let tcp = if target.contains(':') { Some(target.to_string()) } else { None };
    match transport::get_conn(target, tcp.as_deref()) {
        Ok(c) => println!("✓ CNXN/AUTH 完成 serial={} kind={}", c.serial, c.kind),
        Err(e) => {
            eprintln!("✗ 连接失败: {e}");
            std::process::exit(1);
        }
    }

    // 1) shell
    match adb::shell_blocking(target, "echo ADB_TEST_OK", 15000) {
        Ok(out) => {
            let text = String::from_utf8_lossy(&out).trim().to_string();
            println!("✓ shell: {text:?}");
            if !text.contains("ADB_TEST_OK") {
                eprintln!("✗ shell 输出异常");
                std::process::exit(1);
            }
        }
        Err(e) => {
            eprintln!("✗ shell 失败: {e}");
            std::process::exit(1);
        }
    }

    // 2) sync 推送（SEND/DATA/DONE/OKAY）
    let payload = format!("mwine-adb-test-{}", std::process::id());
    let remote = "/data/local/tmp/mwine_adb_test.txt";
    if let Err(e) = adb::push_bytes(target, payload.as_bytes(), remote) {
        eprintln!("✗ sync 推送失败: {e}");
        std::process::exit(1);
    }
    println!("✓ sync 推送 {remote} ({}B)", payload.len());

    // 3) 回读校验（RECV 全链）
    match adb::pull_file(target, remote, 4096) {
        Ok(got) => {
            let text = String::from_utf8_lossy(&got).trim().to_string();
            if text == payload {
                println!("✓ sync 拉取校验一致");
            } else {
                eprintln!("✗ sync 拉取内容不一致: {text:?}");
                std::process::exit(1);
            }
        }
        Err(e) => {
            eprintln!("✗ sync 拉取失败: {e}");
            std::process::exit(1);
        }
    }
    let _ = adb::shell_blocking(target, &format!("rm -f {remote}"), 5000);

    println!("✓ 传输层自检全部通过");
}

fn scrcpy_feeder(
    mut session: scrcpy::Session,
    tx: tokio::sync::mpsc::Sender<FrameMsg>,
    stream: Arc<state::StreamSession>,
) {
    // 标记活动时间（帧到达即刷新；空闲回收看门狗读取）
    let touch = |s: &state::StreamSession| {
        s.last_frame_ms.store(
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_millis() as u64)
                .unwrap_or(0),
            std::sync::atomic::Ordering::Relaxed,
        );
    };
    touch(&stream);
    let mut last_size = (session.width, session.height);
    loop {
        if stream.ended.load(std::sync::atomic::Ordering::SeqCst) {
            break;
        }
        match session.next_packet() {
            Ok(Some(pkt)) => {
                let sz = (session.width, session.height);
                if sz != last_size && sz.0 > 0 && sz.1 > 0 {
                    last_size = sz;
                    stream.width.store(sz.0 as u64, std::sync::atomic::Ordering::SeqCst);
                    stream.height.store(sz.1 as u64, std::sync::atomic::Ordering::SeqCst);
                    let mut meta = Vec::with_capacity(8);
                    meta.extend_from_slice(&sz.0.to_be_bytes());
                    meta.extend_from_slice(&sz.1.to_be_bytes());
                    if tx.blocking_send(encode_frame(PKT_META, pkt.pts, &meta)).is_err() {
                        break;
                    }
                }
                let typ = if pkt.config {
                    PKT_CONFIG
                } else if pkt.key_frame {
                    PKT_KEY
                } else {
                    PKT_DELTA
                };
                if tx.blocking_send(encode_frame(typ, pkt.pts, &pkt.data)).is_err() {
                    break; // 浏览器已断开
                }
            }
            Ok(None) => break, // 流干净结束
            Err(e) => {
                *stream.err.lock().unwrap() = e.to_string();
                break;
            }
        }
    }
    stream.ended.store(true, std::sync::atomic::Ordering::SeqCst);
    session.stop();
}

// ===================== 鉴权 =====================

fn expected_token() -> Option<String> {
    std::env::var("MWINE_TOKEN").ok().map(|t| t.trim().to_string()).filter(|t| !t.is_empty())
}

async fn auth_mw(req: axum::extract::Request, next: Next) -> Response {
    let Some(token) = expected_token() else {
        return next.run(req).await;
    };
    let ok_hdr = req
        .headers()
        .get("authorization")
        .and_then(|v| v.to_str().ok())
        .map(|v| v == format!("Bearer {token}"))
        .unwrap_or(false);
    let ok_query = req
        .uri()
        .query()
        .map(|q| {
            q.split('&')
                .filter_map(|kv| kv.split_once('='))
                .any(|(k, v)| k == "token" && urlencoding_decode(v) == token)
        })
        .unwrap_or(false);
    if ok_hdr || ok_query {
        next.run(req).await
    } else {
        (
            StatusCode::UNAUTHORIZED,
            [("www-authenticate", "Bearer")],
            Json(json!({ "error": "未授权：请提供访问令牌" })),
        )
            .into_response()
    }
}

fn urlencoding_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        if bytes[i] == b'+' {
            out.push(b' ');
        } else {
            out.push(bytes[i]);
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).to_string()
}

// ===================== Web 目录探测 =====================

fn resolve_web_dir() -> Option<String> {
    if let Ok(d) = std::env::var("MWINE_WEB_DIR") {
        if !d.is_empty() && std::path::Path::new(&format!("{d}/index.html")).is_file() {
            return Some(d);
        }
    }
    let exe_dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
    let cwd = std::env::current_dir().unwrap_or_default();
    let mut cands: Vec<String> = Vec::new();
    // 逐级上溯（exe 所在目录与 cwd 的全部祖先），任一层出现 web 根即命中
    for start in [exe_dir, cwd] {
        let mut d = Some(start.as_path());
        while let Some(dir) = d {
            cands.push(format!("{}/web-vue/dist", dir.display()));
            cands.push(format!("{}/web", dir.display()));
            d = dir.parent();
        }
    }
    cands
        .into_iter()
        .find(|c| std::path::Path::new(&format!("{c}/index.html")).is_file())
}

// ===================== main =====================

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "info".into()),
        )
        .init();

    // 传输层自检入口：--adb-test <serial|tcp:addr>
    {
        let args: Vec<String> = std::env::args().collect();
        if args.len() > 2 && args[1] == "--adb-test" {
            return adb_test_entry(&args[2]).await;
        }
        if args.len() > 3 && args[1] == "--adb-pull" {
            return adb_pull_diag(&args[2], &args[3]).await;
        }
        if args.len() > 2 && args[1] == "--adb-pty" {
            return adb_pty_diag(&args[2]).await;
        }
        if args.len() > 1 && args[1] == "--sig-test" {
            return sig_test_entry().await;
        }
    }

    let _ = adb::ensure_server();

    let state = state::AppState::new();
    // 家长管控巡检线程（每 5s：屏幕状态/前台应用/规则执行）
    {
        let st = state.clone();
        std::thread::spawn(move || crate::guard::run_loop(st));
    }
    // 已知无线设备自动回连：adb server 与网关同生命周期（容器内），
    // 重启后 adb 侧无线连接清空——按选定设备/管控设备主动 connect 恢复
    {
        let st = state.clone();
        std::thread::spawn(move || {
            // USB 自愈状态：连续检测到「总线有 ADB 接口但 adb 无 USB transport」
            // 达 2 轮即触发；60s 防抖
            let mut usb_strikes: u32 = 0;
            let mut last_heal_ms: i64 = 0;
            loop {
            let mut cands: Vec<String> = Vec::new();
            if let Some(sel) = st.current_serial() {
                cands.push(sel);
            }
            for s in crate::guard::known_serials() {
                if !cands.contains(&s) {
                    cands.push(s);
                }
            }
            for s in state::known_devices() {
                if !cands.contains(&s) {
                    cands.push(s);
                }
            }
            for serial in cands {
                // 无线设备序列号形如 192.168.x.x:5555
                if serial.contains(':') && adb::devices()
                    .map(|ds| !ds.iter().any(|d| d.serial == serial))
                    .unwrap_or(true)
                {
                    // 快速端口探测：TCP 通才走完整 connect（省无谓的握手超时）
                    let host = serial.rsplit_once(':').map(|(h, _)| h).unwrap_or("");
                    let port: u16 = serial.rsplit_once(':').and_then(|(_, p)| p.parse().ok()).unwrap_or(5555);
                    let reachable = std::net::TcpStream::connect_timeout(
                        &format!("{host}:{port}").parse().unwrap(),
                        std::time::Duration::from_millis(800),
                    ).is_ok();
                    if !reachable {
                        tracing::debug!("回连跳过（{serial} 端口未开）");
                        continue;
                    }
                    match adb::adb_connect(&serial) {
                        Ok(_) => tracing::info!("已回连设备 {serial}"),
                        Err(e) => tracing::debug!("回连 {serial} 未成功: {e}"),
                    }
                }
            }
            // USB 自愈：/sys 有 ADB 接口但 adb 无 USB transport（外部 server
            // 占用 5037 / 热插拔事件丢失）→ 协议级重启 adb server 重扫，
            // 随后立即再跑一轮回连恢复 WiFi 连接
            let usb_iface = adb::usb_adb_iface_present();
            let has_usb_transport = adb::devices()
                .map(|ds| {
                    ds.iter().any(|d| {
                        !d.serial.contains(':') && !d.serial.starts_with("emulator")
                    })
                })
                .unwrap_or(true);
            if usb_iface && !has_usb_transport {
                usb_strikes += 1;
            } else {
                usb_strikes = 0;
            }
            let now_ms = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_millis() as i64)
                .unwrap_or(0);
            if usb_iface && !has_usb_transport && usb_strikes >= 2 && now_ms - last_heal_ms > 60_000 {
                match adb::restart_server() {
                    Ok(_) => {
                        tracing::info!("USB 自愈：adb server 已重启并重扫（5037 被外部占用或热插拔丢失）");
                        last_heal_ms = now_ms;
                        usb_strikes = 0;
                        continue; // 立即进入下一轮回连
                    }
                    Err(e) => tracing::warn!("USB 自愈失败: {e}"),
                }
            }
            std::thread::sleep(std::time::Duration::from_secs(30));
            }
        });
    }

    // 501 占位处理器共用
    let api = Router::new()
        // 基础
        .route("/status", get(api::status))
        .route("/about", get(api::about))
        .route("/sms/list", get(api::sms_list))
        .route("/sms/send", post(api::sms_send))
        .route("/events/status", get(api::events_status))
        .route("/events/apps", get(api::events_apps))
        .route("/events/device", get(api::events_device))
        .route("/perf", get(api::perf))
        // 应用
        .route("/apps", get(api::apps))
        .route("/apps/{pkg}/run", post(api::app_run))
        .route("/apps/{pkg}/uninstall", post(api::app_uninstall))
        .route("/apps/install-upload", post(api::install_upload))
        .route("/apps/install/task", get(api::install_task))
        .route("/apps/icon", get(api::app_icon))
        .route("/wallpaper.png", get(api::wallpaper_png))
        // 截图 / 配置 / 快照
        .route("/screenshot.png", get(api::screenshot_png))
        .route("/screenshot", post(api::screenshot_png)) // 兼容桌面版语义差异：直接回 PNG
        .route("/config", get(api::config_get).post(api::config_set))
        // 镜像 / 运行时
        // 终端
        .route("/term/open", post(api::term_open))
        .route("/term/ws", get(api::term_ws))
        .route("/term/{id}/read", get(api::term_read))
        .route("/term/{id}/write", post(api::term_write))
        .route("/term/{id}/close", post(api::term_close))
        // 设备
        .route("/devices", get(api::devices_list))
        .route("/devices/discovered", get(api::devices_discovered))
        .route("/devices/pair", post(api::devices_pair))
        .route("/devices/hostpair/begin", post(api::hostpair_begin))
        .route("/devices/hostpair", get(api::hostpair_get))
        .route("/devices/hostpair/cancel", post(api::hostpair_cancel))
        .route("/devices/connect", post(api::devices_connect))
        .route("/devices/disconnect", post(api::devices_disconnect))
        .route("/devices/select", post(api::devices_select))
        .route("/device/op", post(api::device_op))
        .route("/devices/known/remove", post(api::devices_known_remove))
        .route("/device/info", get(api::device_info))
        .route("/guard", get(api::guard_get).post(api::guard_set))
        .route("/files", get(filemgr::files_list))
        .route("/files/download", get(filemgr::files_download))
        .route("/files/upload", post(filemgr::files_upload))
        .route("/files/op", post(filemgr::files_op))
        .route("/devices/forget", post(api::devices_disconnect))
        // 运行模式
        // 投屏
        .route("/stream/open", post(api::stream_open))
        .route("/stream/{id}/video", get(api::stream_video))
        .route("/stream/{id}/touch", post(api::stream_touch))
        .route("/stream/{id}/key", post(api::stream_key))
        .route("/stream/{id}/text", post(api::stream_text))
        .route("/stream/{id}/scroll", post(api::stream_scroll))
        .route("/stream/{id}/reset", post(api::stream_reset))
        .route("/stream/{id}/close", post(api::stream_close))
        .route("/stream/{id}/clipboard", get(api::stream_clipboard_get).post(api::stream_clipboard_set))
        // 商店 / 抓包 / 其余 → 501
        .route("/store/sources", get(api::not_impl_store))
        .route("/store/search", get(api::not_impl_store))
        .route("/store/recommend", get(api::not_impl_store))
        .route("/store/versions", get(api::not_impl_store))
        .route("/store/task", get(api::not_impl_store))
        .route("/store/download", post(api::not_impl_store))
        .route("/store/cancel", post(api::not_impl_store))
        .route("/capture/{*rest}", get(api::not_impl_capture).post(api::not_impl_capture))
        .route("/keys/{*rest}", get(api::not_impl_store))
        .route("/foreground", get(api::not_impl_store))
        .route("/identity/reset", post(api::not_impl_misc))
        .route("/vm/reset", post(api::not_impl_misc))
        .route("/vm/disk", get(api::not_impl_misc).post(api::not_impl_misc))
        .route("/vm/show-window", post(api::not_impl_misc))
        .route("/runtime/geo", post(api::not_impl_misc))
        .route("/runtime/battery", post(api::not_impl_misc))
        .route("/runtime/camera-image", post(api::not_impl_misc))
        .route_layer(middleware::from_fn(auth_mw));

    let app = Router::new()
        .route("/health", get(|| async { Json(json!({"ok": true})) }))
        .nest("/api", api)
        .with_state(state);

    // 静态 Web（SPA 回落 index.html）
    let web_dir = resolve_web_dir();
    let app = match &web_dir {
        Some(dir) => {
            let index = format!("{dir}/index.html");
            let service = ServeDir::new(dir).not_found_service(ServeFile::new(index));
            app.fallback_service(service)
        }
        None => app.fallback(|| async {
            (
                StatusCode::NOT_FOUND,
                Json(json!({"error": "Web 前端未安装（MWINE_WEB_DIR 未指向有效目录）"})),
            )
                .into_response()
        }),
    };
    // 大体积上传（APK 512MB）
    let app = app.layer(axum::extract::DefaultBodyLimit::max(600 * 1024 * 1024));

    let host = std::env::var("MWINE_HOST").unwrap_or_else(|_| "127.0.0.1".into());
    let port: u16 = std::env::var("MWINE_PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(7788);
    let ip: IpAddr = host.parse().unwrap_or(IpAddr::from([127, 0, 0, 1]));
    let lan_exposed = !ip.is_loopback();
    if lan_exposed && expected_token().is_none() {
        tracing::warn!("⚠ 守护进程对外监听且未设置 MWINE_TOKEN——局域网内任何设备都可完全控制虚拟机！");
    }
    let web_note = web_dir.as_deref().unwrap_or("未安装");
    let nas_note = if lan_exposed { "（NAS 模式）" } else { "" };

    // MWINE_TLS_PORT 置位 = 纯 HTTPS（不绑 HTTP 监听；MWINE_PORT 被忽略）。
    // WebCodecs 投屏只在安全上下文可用，NAS 对外部署一律 TLS-only。
    // 自签证书自动生成于 $HOME/.mwine/tls/{cert,key}.pem（重启不变）。
    let tls_port = std::env::var("MWINE_TLS_PORT")
        .ok()
        .and_then(|p| p.parse::<u16>().ok());
    if let Some(tls_port) = tls_port {
        // rustls 0.23 无默认 provider 时必须显式安装（ring）
        let _ = rustls::crypto::ring::default_provider().install_default();
        let (cert, key) = match tls::ensure_self_signed() {
            Ok(v) => v,
            Err(e) => {
                tracing::error!("TLS 证书生成失败: {}", e);
                std::process::exit(1);
            }
        };
        let cfg = match axum_server::tls_rustls::RustlsConfig::from_pem_file(cert, key).await {
            Ok(c) => c,
            Err(e) => {
                tracing::error!("TLS 配置加载失败: {}", e);
                std::process::exit(1);
            }
        };
        let tls_addr = std::net::SocketAddr::new(ip, tls_port);
        tracing::info!(
            "✓ mwine-nas (Rust) 就绪: https://{}:{}{}（Web: {}）",
            host, tls_port, nas_note, web_note
        );
        if let Err(e) = axum_server::bind_rustls(tls_addr, cfg)
            .serve(app.into_make_service())
            .await
        {
            tracing::error!("HTTPS 监听失败: {}", e);
            std::process::exit(1);
        }
        return;
    }

    let addr = std::net::SocketAddr::new(ip, port);
    tracing::info!(
        "✓ mwine-nas (Rust) 就绪: http://{}:{}{}（Web: {}）",
        host, port, nas_note, web_note
    );
    let listener = match tokio::net::TcpListener::bind(addr).await {
        Ok(l) => l,
        Err(e) => {
            tracing::error!("监听 {} 失败: {}", addr, e);
            std::process::exit(1);
        }
    };
    axum::serve(listener, app).await.unwrap();
}

// —— 调试辅助：--adb-pty <serial>（pty 桥字节流诊断）——
#[allow(dead_code)]
async fn adb_pty_diag(serial: &str) {
    match adb::shell_pty(serial) {
        Ok((mut proc, pty)) => {
            println!("✓ pty 打开 pty={pty}");
            use std::io::{Read, Write as _};
            let mut r = proc.stream.try_clone().unwrap();
            std::thread::sleep(std::time::Duration::from_millis(1500));
            let _ = proc.stream.write_all(b"echo PTY_TEST_OK
");
            let mut buf = [0u8; 4096];
            r.set_read_timeout(Some(std::time::Duration::from_secs(3))).ok();
            match r.read(&mut buf) {
                Ok(n) => println!("✓ 读到 {n}B: {:?}", String::from_utf8_lossy(&buf[..n])),
                Err(e) => println!("✗ 读失败: {e}"),
            }
            proc.shutdown();
        }
        Err(e) => println!("✗ pty 打开失败: {e}"),
    }
}

// —— 调试辅助：--sig-test（签名正确性验证：token 写 /tmp/sig_token.bin，
// 签名写 /tmp/sig_out.bin；用 `openssl dgst -sha1 -verify pub.pem -signature
// /tmp/sig_out.bin /tmp/sig_token.bin` 按 adbd 同款语义验签）——
#[allow(dead_code)]
async fn sig_test_entry() {
    let key = adbkeys::load_or_create();
    let token = b"SIG_TEST_TOKEN_0123"; // 20 字节
    let sig = adbkeys::sign_token(&key, token);
    std::fs::write("/tmp/sig_token.bin", token).unwrap();
    std::fs::write("/tmp/sig_out.bin", &sig).unwrap();
    println!("签名长度: {}B（应为 256）", sig.len());
}

// —— 调试辅助：--adb-pull <serial> <path>（拉取诊断）——
#[allow(dead_code)]
async fn adb_pull_diag(serial: &str, path: &str) {
    match adb::pull_file(serial, path, 8 * 1024 * 1024) {
        Ok(d) => println!("✓ pull {path}: {}B", d.len()),
        Err(e) => println!("✗ pull {path}: {e}"),
    }
    let sz = adb::remote_file_size(serial, path);
    println!("shell stat size: {sz:?}");
}
