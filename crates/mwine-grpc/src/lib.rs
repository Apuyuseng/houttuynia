//! mwine-grpc：Android emulator gRPC 客户端
//! 通过 `-grpc <port>` 启动的免认证端口与 qemu 通信

pub mod emulator {
    tonic::include_proto!("android.emulation.control");
}

pub mod client;
pub use client::EmuClient;
