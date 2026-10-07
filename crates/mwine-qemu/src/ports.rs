//! 端口自动分配：console/adb 必须成对（adb=console+1），gRPC 独立。

use std::net::TcpListener;

/// 端口是否空闲（bind 探测；仅探测 127.0.0.1，qemu console 也只听本地）。
fn port_free(port: u16) -> bool {
    TcpListener::bind(("127.0.0.1", port)).is_ok()
}

/// 从 start（含）起向上找成对空闲端口，返回 (console, console+1)。
/// qemu 的 `-ports` 约定 adb 端口 = console + 1，且 serial 为
/// `emulator-<console>`，因此成对探测不可拆。
pub fn pick_port_pair(start: u16) -> Option<(u16, u16)> {
    let mut c = start;
    while c < 65534 {
        if port_free(c) && port_free(c + 1) {
            return Some((c, c + 1));
        }
        c += 2;
    }
    None
}

/// 从 start（含）起向上找单个空闲端口（gRPC 用）。
pub fn pick_port(start: u16) -> Option<u16> {
    (start..=65535).find(|p| port_free(*p))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pair_is_adjacent_and_free() {
        let (c, a) = pick_port_pair(5554).expect("本机 5554 附近应有空闲对");
        assert_eq!(a, c + 1);
        assert!(c >= 5554);
    }

    #[test]
    fn pair_skips_occupied_console() {
        // 占住探测起点，迫使分配器跳到下一对
        let hold = TcpListener::bind(("127.0.0.1", 0)).expect("bind ephemeral");
        let held = hold.local_addr().unwrap().port();
        // 对齐到偶数作为起点，保证 held 恰好落在某个候选 console 上
        let start = held - (held % 2);
        let (c, _) = pick_port_pair(start).expect("应跳过被占端口");
        assert_ne!(c, start, "起点被占用时应跳过");
        assert!(c > start);
    }

    #[test]
    fn single_port_returns_free() {
        let p = pick_port(8554).expect("本机 8554 附近应有空闲端口");
        assert!(p >= 8554);
    }
}
