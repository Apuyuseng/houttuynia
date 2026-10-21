//! 模拟器运行时在线下载（切虚拟机模式时自动触发）：断点续传 → 解压落位。
//! 对齐桌面版：源 MWINE_RUNTIME_URL > 内置 GitHub Releases latest；
//! 目标目录 = Sdk::externalRuntimeDir 同口径（macOS Application Support /
//! Linux XDG）；下载中可取消。

use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};

use serde_json::{json, Value};

use crate::state::{self, SharedState};

pub const DEFAULT_RUNTIME_URL: &str =
    "https://github.com/Apuyuseng/houttuynia/releases/latest/download/Houttuynia-runtime-v1.zip";

fn runtime_dir() -> String {
    #[cfg(target_os = "macos")]
    {
        let home = std::env::var("HOME").unwrap_or_default();
        format!("{home}/Library/Application Support/MWine/runtime")
    }
    #[cfg(not(target_os = "macos"))]
    {
        if let Ok(x) = std::env::var("XDG_DATA_HOME") {
            if !x.is_empty() {
                return format!("{x}/MWine/runtime");
            }
        }
        let home = std::env::var("HOME").unwrap_or_default();
        format!("{home}/.local/share/MWine/runtime")
    }
}

pub fn runtime_present() -> bool {
    if let Some(sdk) = state::sdk_root() {
        if std::path::Path::new(&format!("{sdk}/emulator")).is_dir() {
            return true;
        }
    }
    std::path::Path::new(&format!("{}/emulator", runtime_dir())).is_dir()
}

/// POST /api/runtime/download：已存在 → 409；否则后台下载 + 解压
pub fn start_download(st: &SharedState, body_url: Option<String>) -> Value {
    if runtime_present() {
        return json!({ "error": "运行时已存在，无需下载", "ok": false });
    }
    if !st.runtime_task.try_begin("下载模拟器运行时…") {
        return json!({ "error": "已有下载任务在进行中", "ok": false });
    }
    let url = body_url
        .filter(|u| !u.trim().is_empty())
        .or_else(|| std::env::var("MWINE_RUNTIME_URL").ok().filter(|u| !u.trim().is_empty()))
        .unwrap_or_else(|| DEFAULT_RUNTIME_URL.into());
    let st2 = st.clone();
    std::thread::spawn(move || download_worker(st2, url));
    json!({ "ok": true })
}

fn download_worker(st: SharedState, url: String) {
    let r = download_and_install(&st, &url);
    match r {
        Ok(p) => st.runtime_task.finish_ok(&p),
        Err(e) => st.runtime_task.finish_err(&e),
    }
}

fn download_and_install(st: &SharedState, url: &str) -> Result<String, String> {
    let home = std::env::var("HOME").unwrap_or_default();
    let dl_dir = format!("{home}/.mwine/downloads");
    std::fs::create_dir_all(&dl_dir).map_err(|e| e.to_string())?;
    let zip_path = format!("{dl_dir}/Houttuynia-runtime.zip");

    // 断点续传：已有部分文件 → Range 续下
    let existing = std::fs::metadata(&zip_path).map(|m| m.len()).unwrap_or(0);
    let client = reqwest::blocking::Client::builder()
        .timeout(std::time::Duration::from_secs(7200))
        .build()
        .map_err(|e| e.to_string())?;
    let mut req = client.get(url);
    if existing > 0 {
        req = req.header("Range", format!("bytes={existing}-"));
    }
    let mut resp = req.send().map_err(|e| format!("下载请求失败: {e}"))?;
    if !resp.status().is_success() && resp.status().as_u16() != 206 {
        return Err(format!("下载失败: HTTP {}", resp.status()));
    }
    let resumed = resp.status().as_u16() == 206;
    let total = resp
        .content_length()
        .map(|l| l + if resumed { existing } else { 0 })
        .unwrap_or(0);
    st.runtime_task.set_progress(
        "下载运行时（断点续传）",
        if resumed { existing } else { 0 },
        total,
    );

    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .write(true)
        .append(resumed)
        .truncate(!resumed)
        .open(&zip_path)
        .map_err(|e| e.to_string())?;
    let mut done = if resumed { existing } else { 0 };
    let mut buf = vec![0u8; 256 * 1024];
    loop {
        let n = resp.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n]).map_err(|e| e.to_string())?;
        done += n as u64;
        st.runtime_task.set_progress("下载运行时（断点续传）", done, total);
    }
    drop(file);

    // 解压到运行时目录（zip 内含 emulator/ platform-tools/ 顶层目录）
    st.runtime_task.set_progress("解压安装中", 0, 0);
    let target = runtime_dir();
    let _ = std::fs::remove_dir_all(format!("{target}.tmp"));
    std::fs::create_dir_all(format!("{target}.tmp")).map_err(|e| e.to_string())?;
    {
        let f = std::fs::File::open(&zip_path).map_err(|e| e.to_string())?;
        let mut z = zip::ZipArchive::new(f).map_err(|e| format!("zip 打开失败: {e}"))?;
        z.extract(format!("{target}.tmp"))
            .map_err(|e| format!("解压失败: {e}"))?;
    }
    // zip 里若多一层同名顶层目录则上抬
    let tmp = format!("{target}.tmp");
    let has_emu = std::path::Path::new(&format!("{tmp}/emulator")).is_dir();
    let final_tmp = if has_emu {
        tmp.clone()
    } else {
        // 找唯一子目录上抬
        let mut sub = None;
        if let Ok(rd) = std::fs::read_dir(&tmp) {
            let entries: Vec<_> = rd.flatten().collect();
            if entries.len() == 1 && entries[0].path().is_dir() {
                sub = Some(entries[0].path().to_string_lossy().to_string());
            }
        }
        match sub {
            Some(s) => s,
            None => tmp.clone(),
        }
    };
    let _ = std::fs::remove_dir_all(&target);
    std::fs::create_dir_all(
        std::path::Path::new(&target)
            .parent()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_default(),
    )
    .map_err(|e| e.to_string())?;
    std::fs::rename(&final_tmp, &target).map_err(|e| format!("落位失败: {e}"))?;
    if final_tmp != tmp {
        let _ = std::fs::remove_dir_all(&tmp);
    }
    // emulator/platform-tools 加可执行位
    for sub in ["emulator/emulator", "platform-tools/adb"] {
        let p = format!("{target}/{sub}");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if let Ok(meta) = std::fs::metadata(&p) {
                let mut perm = meta.permissions();
                perm.set_mode(0o755);
                let _ = std::fs::set_permissions(&p, perm);
            }
        }
    }
    Ok(target)
}

/// 静态占位（取消由后续任务模型需要时再加）
pub static RUNTIME_CANCEL: AtomicBool = AtomicBool::new(false);
pub fn cancel() {
    RUNTIME_CANCEL.store(true, Ordering::SeqCst);
}
