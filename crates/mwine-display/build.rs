//! 用 protox（纯 Rust proto 编译器，免 protoc）把 vendored proto 编成
//! FileDescriptorSet，再交给 tonic-build 生成客户端桩。

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let file = protox::compile(["proto/emu_display.proto"], ["proto/"])?;
    tonic_build::configure()
        .build_server(false)
        .compile_fds(file)?;
    Ok(())
}
