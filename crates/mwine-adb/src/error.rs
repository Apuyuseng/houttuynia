#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("adb server unreachable at {addr}: {source}")]
    ServerUnreachable {
        addr: String,
        source: std::io::Error,
    },
    #[error("adb service request failed: {0}")]
    ServiceFailed(String),
    #[error("protocol error: {0}")]
    Protocol(String),
    #[error("device {0} not found / offline")]
    DeviceNotFound(String),
    #[error("adb binary not found: {0}")]
    AdbBinaryNotFound(String),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
}

pub type Result<T> = std::result::Result<T, Error>;
