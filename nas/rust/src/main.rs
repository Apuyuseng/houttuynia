//! mwine-nas：Houttuynia NAS 网关（Rust 版守护）。
//!
//! 环境变量：
//!   MWINE_HOST     绑定地址（默认 127.0.0.1；0.0.0.0 对局域网开放）
//!   MWINE_PORT     端口（默认 7788）
//!   MWINE_TOKEN    访问令牌（设置后 /api/* 需 Bearer 或 ?token=；/health 恒开放）
//!   MWINE_WEB_DIR  Web 控制台目录（默认自动探测 web-vue/dist → web/）
//!   MWINE_HEADLESS 无头启动模拟器（-no-window）

mod adb;
mod api;
mod console;
mod devices;
mod icons;
mod meta;
mod pairing;
mod scrcpy;
mod state;
mod tls;

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

fn encode_frame(typ: u8, pts: u64, data: &[u8]) -> FrameMsg {
    let mut out = Vec::with_capacity(15 + data.len());
    out.extend_from_slice(&0x4D57u16.to_be_bytes());
    out.push(typ);
    out.extend_from_slice(&pts.to_be_bytes());
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(data);
    out
}

/// 视频包 → 帧队列（客户端断开 → send 失败 → 退出并清理会话）。
fn scrcpy_feeder(
    mut session: scrcpy::Session,
    tx: tokio::sync::mpsc::Sender<FrameMsg>,
    stream: Arc<state::StreamSession>,
) {
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

    let _ = adb::ensure_server();

    let state = state::AppState::new();

    // 501 占位处理器共用
    let api = Router::new()
        // 基础
        .route("/status", get(api::status))
        .route("/events/status", get(api::events_status))
        .route("/events/apps", get(api::events_apps))
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
