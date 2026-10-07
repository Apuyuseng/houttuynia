//! 集成测试。
//!
//! - mock 部分（默认跑）：验证 mock 启动器与句柄行为，供 S2/S3 参考用法。
//! - 真机部分（`#[ignore]`）：需要本机 Android SDK + 可启动 AVD，
//!   `cargo test -p mwine-qemu -- --ignored` 显式执行；
//!   AVD 名取环境变量 `MWINE_QEMU_TEST_AVD`（缺省 MWine1）。

use std::time::Duration;

use mwine_qemu::{DirectLauncher, MockLauncher, QemuConfig, QemuLauncher, QemuRuntime};

#[test]
fn mock_launcher_end_to_end_shape() {
    let m = MockLauncher::with_ports(5560, 8560);
    let h = m
        .launch(&QemuConfig {
            avd_name: "IT".into(),
            ..Default::default()
        })
        .unwrap();
    assert_eq!(h.adb_serial, "emulator-5560");
    // mock 句柄：无进程、关停立即成功
    assert!(!h.is_running());
    h.shutdown().unwrap();
}

#[test]
#[ignore = "需要本机 SDK + AVD；cargo test -- --ignored 显式运行"]
fn direct_launch_reaches_adb_and_console() {
    let runtime = QemuRuntime::detect_from_sdk().expect("本机应可探测到 SDK qemu");
    let mut launcher = DirectLauncher::new(runtime);
    launcher.log_file = Some(std::env::temp_dir().join("mwine-qemu-it.log"));
    let avd = std::env::var("MWINE_QEMU_TEST_AVD").unwrap_or_else(|_| "MWine1".into());

    let handle = launcher
        .launch(&QemuConfig {
            avd_name: avd.clone(),
            ..Default::default()
        })
        .expect("直启应成功");
    println!(
        "launched avd={avd} pid={} serial={} console={} grpc={}",
        handle.pid, handle.adb_serial, handle.console_port, handle.grpc_port
    );

    // M1 验收：qemu 进程运行 + adb 可连（自建镜像 adbd 上线慢，放宽到 3 分钟）
    assert!(handle.is_running());
    handle.wait_adb(Duration::from_secs(180)).expect("adb 应可见");

    // console 通道应答（avd name 返回实例身份）
    let resp = handle.send_console("avd name").expect("console 应答");
    println!("console avd name -> {resp:?}");
    assert!(!resp.contains("KO"), "console 查询不应被拒: {resp}");

    // 优雅关停
    handle.shutdown().expect("优雅关停");
    assert!(!handle.is_running());
}
