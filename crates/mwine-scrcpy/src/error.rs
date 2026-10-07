#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("adb error: {0}")]
    Adb(#[from] mwine_adb::Error),
    #[error("scrcpy server exited unexpectedly: {0}")]
    ServerExited(String),
    #[error("launch step failed: {0}")]
    LaunchFailed(String),
    #[error("connection to scrcpy server timed out")]
    ConnectTimeout,
    #[error("protocol error: {0}")]
    Protocol(String),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("server jar not found: {0}")]
    ServerJarNotFound(String),
    #[error("invalid option: {0}")]
    InvalidOption(String),
}

pub type Result<T> = std::result::Result<T, Error>;
