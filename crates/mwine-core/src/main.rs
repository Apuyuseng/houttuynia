//! mwine-core：MWine 守护进程（REST API on localhost:7788）
//!
//! UI（SwiftUI）通过 HTTP 调用本守护进程操作 VM。
//! CLI（mwine）也可以调用同一 API。

mod api;
mod config;
mod daemon;

fn main() {
    println!("MWine Core v{}", env!("CARGO_PKG_VERSION"));
    let port: u16 = std::env::var("MWINE_PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(7788);
    daemon::run(port);
}
