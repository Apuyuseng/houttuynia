//! 设备文件管理器（不走 APK/桥，纯 adb 通道）。
//!
//! 高可用设计：
//!   - 传输全流式（sync 协议 DATA 分块直通 channel），大文件内存恒定；
//!   - 每个操作独立 adb 连接 + 独立超时，互不阻塞（adb server 多路复用）；
//!   - 路径白名单（/sdcard、/storage/emulated/0）+ 规范化（拒绝 .. 穿越）；
//!   - 设备离线返回 503（自动回连循环恢复后即可用），操作失败带设备侧原文。

use axum::body::Body;
use axum::extract::{Query, State};
use axum::http::{header, StatusCode};
use axum::response::IntoResponse;
use axum::Json;
use futures_util::StreamExt;
use serde_json::{json, Value};

use crate::adb;
use crate::state::SharedState;

fn err<T>(msg: impl Into<String>) -> Result<T, (StatusCode, Json<Value>)> {
    Err((
        StatusCode::BAD_REQUEST,
        Json(json!({ "error": msg.into() })),
    ))
}

fn err_status(status: StatusCode, msg: impl Into<String>) -> (StatusCode, Json<Value>) {
    (status, Json(json!({ "error": msg.into() })))
}

fn ok(v: Value) -> (StatusCode, Json<Value>) {
    (StatusCode::OK, Json(v))
}

/// 路径规范化 + 白名单：仅 /sdcard 与 /storage/emulated/0（同一存储）。
/// 拒绝 .. 穿越、空段；返回规范化后的绝对路径。
pub fn norm_path(raw: &str) -> Option<String> {
    let p = raw.trim();
    if !p.starts_with('/') {
        return None;
    }
    let mut parts: Vec<&str> = Vec::new();
    for seg in p.split('/') {
        match seg {
            "" | "." => {}
            ".." => {
                parts.pop()?;
            }
            s => parts.push(s),
        }
    }
    let out = format!("/{}", parts.join("/"));
    let ok = out == "/sdcard"
        || out.starts_with("/sdcard/")
        || out == "/storage/emulated/0"
        || out.starts_with("/storage/emulated/0/");
    ok.then_some(out)
}

/// shell 单引号包裹
fn shq(p: &str) -> String {
    format!("'{}'", p.replace('\'', "'\\''"))
}

/// 扩展名 → MIME（预览/播放需要正确类型；未知回落 octet-stream）
fn mime_by_ext(name: &str) -> &'static str {
    let ext = name.rsplit('.').next().unwrap_or("").to_lowercase();
    match ext.as_str() {
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "webp" => "image/webp",
        "gif" => "image/gif",
        "bmp" => "image/bmp",
        "mp4" | "mkv" | "mov" | "m4v" => "video/mp4",
        "webm" => "video/webm",
        "ts" => "video/mp2t",
        "mp3" => "audio/mpeg",
        "flac" => "audio/flac",
        "wav" => "audio/wav",
        "ogg" | "m4a" | "aac" => "audio/aac",
        "pdf" => "application/pdf",
        "apk" => "application/vnd.android.package-archive",
        "txt" | "log" | "json" | "xml" | "html" | "htm" | "js" | "css" | "csv" | "ini"
        | "conf" | "prop" | "sh" => "text/plain; charset=utf-8",
        _ => "application/octet-stream",
    }
}

/// 当前设备（离线 503）
async fn need_device(st: &SharedState) -> Result<String, (StatusCode, Json<Value>)> {
    st.current_serial().map_or_else(
        || Err(err_status(StatusCode::SERVICE_UNAVAILABLE, "Android 未连接（设备上线后自动恢复）")),
        Ok,
    )
}

/// GET /api/files?path= —— 目录列表（一次 shell 合并 stat；不存在 404）
pub async fn files_list(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
) -> (StatusCode, Json<Value>) {
    let serial = match need_device(&st).await {
        Ok(s) => s,
        Err(e) => return e,
    };
    let raw = q.get("path").map(String::as_str).unwrap_or("/sdcard");
    let Some(dir) = norm_path(raw) else {
        return err_status(StatusCode::BAD_REQUEST, "路径非法（仅允许 /sdcard 下）");
    };
    let r = tokio::task::spawn_blocking(move || -> Result<Value, String> {
        let dirq = shq(&dir);
        // %F 类型 | %s 尺寸 | %Y mtime | %n 名字；dotfiles 第二个 glob
        let out = adb::shell_blocking(
            &serial,
            &format!(
                "cd {dirq} 2>/dev/null && stat -c '%F|%s|%Y|%n' * .[!.]* 2>/dev/null; echo __LIST_DONE__"
            ),
            15000,
        )
        .map_err(|e| e.to_string())?;
        let text = String::from_utf8_lossy(&out);
        if !text.contains("__LIST_DONE__") {
            return Err(format!("目录不存在或不可访问: {dir}"));
        }
        let mut entries: Vec<Value> = Vec::new();
        for l in text.lines() {
            if l == "__LIST_DONE__" || l.is_empty() {
                continue;
            }
            let mut it = l.splitn(4, '|');
            let (Some(ftype), Some(sz), Some(mt), Some(name)) =
                (it.next(), it.next(), it.next(), it.next())
            else {
                continue;
            };
            entries.push(json!({
                "name": name,
                "dir": ftype == "directory",
                "size": sz.parse::<u64>().unwrap_or(0),
                "mtime": mt.parse::<i64>().unwrap_or(0),
            }));
        }
        entries.sort_by(|a, b| {
            b["dir"].as_bool().cmp(&a["dir"].as_bool())
                .then(a["name"].as_str().cmp(&b["name"].as_str()))
        });
        Ok(json!({ "path": dir, "entries": entries }))
    })
    .await;
    match r {
        Ok(Ok(v)) => (StatusCode::OK, Json(v)),
        Ok(Err(e)) => err_status(StatusCode::NOT_FOUND, e),
        Err(e) => err_status(StatusCode::INTERNAL_SERVER_ERROR, format!("列表任务失败: {e}")),
    }
}

/// GET /api/files/download?path= —— 流式下载（sync RECV 直通，不整包缓冲）
pub async fn files_download(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
) -> axum::response::Response {
    let Some(serial) = st.current_serial() else {
        return err_status(StatusCode::SERVICE_UNAVAILABLE, "Android 未连接").into_response();
    };
    let Some(path) = q.get("path").and_then(|p| norm_path(p)) else {
        return err_status(StatusCode::BAD_REQUEST, "路径非法").into_response();
    };
    let s2 = serial.clone();
    let p2 = path.clone();
    let meta = tokio::task::spawn_blocking(move || {
        let out = adb::shell_blocking(
            &s2,
            &format!("stat -c '%F|%s' {} 2>/dev/null", crate::filemgr::shq(&p2)),
            8000,
        )
        .ok()?;
        let text = String::from_utf8_lossy(&out);
        let mut it = text.trim().splitn(2, '|');
        let ftype = it.next()?.to_string();
        let size: u64 = it.next()?.trim().parse().ok()?;
        Some((ftype, size))
    })
    .await;
    let meta = match meta {
        Ok(m) => m,
        Err(e) => {
            return err_status(StatusCode::INTERNAL_SERVER_ERROR, format!("stat 任务失败: {e}"))
                .into_response()
        }
    };
    let Some((ftype, size)) = meta else {
        return err_status(StatusCode::NOT_FOUND, format!("文件不存在: {path}")).into_response();
    };
    if ftype == "directory" {
        return err_status(StatusCode::BAD_REQUEST, "目录请打包后下载（暂不支持）").into_response();
    }
    let fname = path.rsplit('/').next().unwrap_or("file").to_string();
    let (tx, mut rx) = tokio::sync::mpsc::channel::<Vec<u8>>(16);
    let serial_c = serial.clone();
    let path_c = path.clone();
    tokio::task::spawn_blocking(move || {
        if let Err(e) = adb::pull_stream(&serial_c, &path_c, tx) {
            tracing::warn!("下载流中断: {e}");
        }
    });
    // Vec<u8> → Bytes 的桥接（拉流在阻塞线程，Body 消费在 async）
    let body = Body::from_stream(futures_util::stream::unfold(rx, |mut rx| async move {
        rx.recv().await.map(|chunk| {
            (
                Ok::<_, std::io::Error>(bytes::Bytes::from(chunk)),
                rx,
            )
        })
    }));
    (
        [
            (header::CONTENT_TYPE, mime_by_ext(&fname)),
            (
                header::CONTENT_DISPOSITION,
                &format!("attachment; filename=\"{}\"", fname.replace('"', "")),
            ),
            (header::CONTENT_LENGTH, &size.to_string()),
        ],
        body,
    )
        .into_response()
}

/// POST /api/files/upload?path=<目标目录>&name=<文件名> —— 流式上传
/// （body 分块直写 sync DATA 帧；XHR progress 可见；内存恒定）
pub async fn files_upload(
    State(st): State<SharedState>,
    Query(q): Query<std::collections::HashMap<String, String>>,
    body: Body,
) -> (StatusCode, Json<Value>) {
    let serial = match need_device(&st).await {
        Ok(s) => s,
        Err(e) => return e,
    };
    let Some(dir) = q.get("path").and_then(|p| norm_path(p)) else {
        return err_status(StatusCode::BAD_REQUEST, "路径非法（仅允许 /sdcard 下）");
    };
    // 文件名只取末段，拒绝路径成分
    let name = q
        .get("name")
        .map(|n| n.rsplit('/').next().unwrap_or("").trim().to_string())
        .unwrap_or_default();
    if name.is_empty() || name == "." || name == ".." {
        return err_status(StatusCode::BAD_REQUEST, "文件名非法");
    }
    let target = format!("{dir}/{name}");
    let (tx, mut rx) = tokio::sync::mpsc::channel::<Vec<u8>>(16);
    let serial_c = serial.clone();
    let target_c = target.clone();
    let pump = tokio::task::spawn_blocking(move || -> Result<u64, String> {
        let mut push = adb::SyncPush::start(&serial_c, &target_c)
            .map_err(|e| format!("建立上传通道失败: {e}"))?;
        let mut total: u64 = 0;
        while let Some(chunk) = rx.blocking_recv() {
            push.write(&chunk).map_err(|e| format!("写入中断: {e}"))?;
            total += chunk.len() as u64;
        }
        push.finish().map_err(|e| format!("上传收尾失败: {e}"))?;
        Ok(total)
    });
    // body 分块喂给 pump（send 自带背压）
    let mut data_stream = body.into_data_stream();
    let mut feed_err: Option<String> = None;
    while let Some(chunk) = data_stream.next().await {
        match chunk {
            Ok(c) => {
                if tx.send(c.to_vec()).await.is_err() {
                    break; // pump 已结束（出错）
                }
            }
            Err(e) => {
                feed_err = Some(format!("读取上传流失败: {e}"));
                break;
            }
        }
    }
    drop(tx);
    let r = match pump.await {
        Ok(Ok(total)) => ok(json!({ "ok": true, "bytes": total, "path": target })),
        Ok(Err(e)) => err_status(StatusCode::INTERNAL_SERVER_ERROR, e),
        Err(e) => err_status(StatusCode::INTERNAL_SERVER_ERROR, format!("上传任务失败: {e}")),
    };
    if let Some(m) = feed_err {
        tracing::warn!("上传流读取异常: {m}");
    }
    r
}

/// POST /api/files/op {op: mkdir|delete|rename, path, to?} —— 结构操作
pub async fn files_op(
    State(st): State<SharedState>,
    Json(v): Json<Value>,
) -> (StatusCode, Json<Value>) {
    let serial = match need_device(&st).await {
        Ok(s) => s,
        Err(e) => return e,
    };
    let op = v.get("op").and_then(|x| x.as_str()).unwrap_or("").to_string();
    let Some(path) = v.get("path").and_then(|x| x.as_str()).and_then(norm_path) else {
        return err_status(StatusCode::BAD_REQUEST, "path 非法（仅允许 /sdcard 下）");
    };
    let cmd = match op.as_str() {
        "mkdir" => format!("mkdir -p {}", shq(&path)),
        "delete" => {
            if path == "/sdcard" || path == "/storage/emulated/0" {
                return err_status(StatusCode::BAD_REQUEST, "拒绝删除存储根目录");
            }
            // 目录递归删；文件普通删
            format!(
                "if [ -d {} ]; then rm -rf {}; else rm -f {}; fi",
                shq(&path),
                shq(&path),
                shq(&path)
            )
        }
        "rename" => {
            let Some(to) = v.get("to").and_then(|x| x.as_str()).and_then(norm_path) else {
                return err_status(StatusCode::BAD_REQUEST, "to 非法（仅允许 /sdcard 下）");
            };
            if to == path {
                return err_status(StatusCode::BAD_REQUEST, "目标与原路径相同");
            }
            format!("mv {} {}", shq(&path), shq(&to))
        }
        _ => return err_status(StatusCode::BAD_REQUEST, "op 须为 mkdir/delete/rename"),
    };
    let r = tokio::task::spawn_blocking(move || -> Result<(), String> {
        let out = adb::shell_blocking(&serial, &format!("{cmd} && echo __OP_OK__"), 30000)
            .map_err(|e| e.to_string())?;
        if !String::from_utf8_lossy(&out).contains("__OP_OK__") {
            return Err("设备侧执行失败（路径不存在或权限不足）".into());
        }
        Ok(())
    })
    .await;
    match r {
        Ok(Ok(())) => ok(json!({ "ok": true })),
        Ok(Err(e)) => err_status(StatusCode::INTERNAL_SERVER_ERROR, e),
        Err(e) => err_status(StatusCode::INTERNAL_SERVER_ERROR, format!("操作任务失败: {e}")),
    }
}
