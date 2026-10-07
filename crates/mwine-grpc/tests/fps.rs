use mwine_grpc::EmuClient;

#[tokio::test]
#[ignore] // 需要 VM 运行中
async fn native_fps_test() {
    let mut client = EmuClient::connect(8555).await.expect("gRPC connect");
    tokio::time::sleep(std::time::Duration::from_secs(1)).await;
    for (w, h) in [(720, 1280), (540, 960), (360, 640)] {
        let mut frames = 0;
        let t0 = std::time::Instant::now();
        while t0.elapsed().as_secs_f64() < 3.0 {
            if let Ok(img) = client.screenshot(0, w, h).await {
                if !img.image.is_empty() { frames += 1; }
            }
        }
        let el = t0.elapsed().as_secs_f64();
        println!("  {}x{}: {:.1} fps", w, h, frames as f64 / el);
    }
}
