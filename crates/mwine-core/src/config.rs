//! 虚拟机配置（~/.mwine/config.json）：模式/尺寸/内存/sdcard 挂载位置。
//! MWineUI 的设置页读写，vm_start 按此启动。

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct VmConfig {
    /// phone | tablet | custom
    pub mode: String,
    /// custom 模式生效的尺寸
    pub width: u32,
    pub height: u32,
    pub dpi: u32,
    /// 内存 MB（0 = 镜像默认 2048）
    pub memory_mb: u32,
    /// sdcard 挂载位置（emulator -sdcard 指向的镜像文件路径；空 = AVD 默认）
    pub sdcard_path: String,
    /// 宿主共享目录（9p 挂载到 guest /mnt/host；空 = 不挂载）
    #[serde(default)]
    pub share_dir: String,
    /// AVD 名
    pub avd: String,
    /// guest HTTP 代理（如 http://127.0.0.1:7897；空 = 不代理）
    #[serde(default)]
    pub proxy: String,
    // ===== 专业参数 =====
    /// CPU 核数（0 = 默认 4）
    #[serde(default)]
    pub cpu_cores: u32,
    /// GPU 后端：auto / host / swangle / swiftshader_indirect（空 = 不传，走默认）
    #[serde(default)]
    pub gpu_mode: String,
    /// QEMU 附加参数（原样追加到 -qemu 透传块；空 = 无）
    #[serde(default)]
    pub qemu_extra: String,
    /// 网络速度模拟（如 gsm / hscsd / full；空 = 不模拟）
    #[serde(default)]
    pub netspeed: String,
    /// 网络延迟模拟（如 gprs / umts / none；空 = 不模拟）
    #[serde(default)]
    pub netdelay: String,
    /// 禁用快照（每次冷启动）
    #[serde(default)]
    pub no_snapshot: bool,
    /// 禁用启动动画
    #[serde(default)]
    pub no_boot_anim: bool,
}

impl Default for VmConfig {
    fn default() -> Self {
        Self {
            mode: "phone".into(),
            width: 1080,
            height: 2400,
            dpi: 440,
            memory_mb: 2048,
            sdcard_path: String::new(),
            share_dir: String::new(),
            avd: "MWine4".into(),
            proxy: String::new(),
            cpu_cores: 0,
            gpu_mode: String::new(),
            qemu_extra: String::new(),
            netspeed: String::new(),
            netdelay: String::new(),
            no_snapshot: false,
            no_boot_anim: false,
        }
    }
}

impl VmConfig {
    pub fn load() -> Self {
        let path = Self::path();
        std::fs::read_to_string(&path)
            .ok()
            .and_then(|t| serde_json::from_str(&t).ok())
            .unwrap_or_default()
    }

    pub fn save(&self) -> std::io::Result<()> {
        let path = Self::path();
        if let Some(dir) = std::path::Path::new(&path).parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::write(&path, serde_json::to_string_pretty(self).unwrap_or_default())
    }

    fn path() -> String {
        let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
        format!("{home}/.mwine/config.json")
    }

    /// 当前生效的 (宽, 高, dpi)
    pub fn lcd(&self) -> (u32, u32, u32) {
        match self.mode.as_str() {
            "phone" => (1080, 2400, 440),
            "tablet" => (2560, 1600, 320),
            "tv" => (1920, 1080, 320),
            _ => (self.width, self.height, self.dpi),
        }
    }
}
