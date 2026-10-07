fn main() {
    tonic_build::configure()
        .build_server(false)
        .compile(&["proto/emulator.proto"], &["proto"]);
    println!("cargo:rerun-if-changed=proto/emulator.proto");
}
