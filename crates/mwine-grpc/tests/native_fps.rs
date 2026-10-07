#[tokio::test]
#[ignore]
async fn native_fps() {
    let mut client = mwine_grpc::EmuClient::connect(8555).await.expect("connect");
    for (w, h) in [(360, 640), (720, 1280)] {
        let mut frames = 0;
        let t0 = std::time::Instant::now();
        while t0.elapsed().as_secs_f64() < 3.0 {
            if let Ok(img) = client.screenshot(0, w, h).await {
                if !img.image.is_empty() { frames += 1; }
            }
        }
        let el = t0.elapsed().as_secs_f64();
        println!("  {}x{}: {:.0} fps", w, h, frames as f64 / el);
    }
}
