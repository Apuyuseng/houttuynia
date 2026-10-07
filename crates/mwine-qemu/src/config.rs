//! 启动配置与 argv 组装。

use std::path::PathBuf;

use crate::{Error, Result};

/// qemu 直启配置（`image_dir` 扩展为 Option 以兼容
/// 自建镜像直载模式，`ports`/`grpc_port` 为端口显式覆盖项）。
#[derive(Debug, Clone)]
pub struct QemuConfig {
    pub avd_name: String,
    /// 自建镜像目录（system-qemu.img / vendor-qemu.img / ramdisk-qemu.img /
    /// kernel-ranchu / build.prop）。Some 时走 `-sysdir` 直载，忽略 AVD 镜像。
    pub image_dir: Option<PathBuf>,
    /// 用户数据目录（userdata-qemu.img / snapshots 所在）。
    /// None=不传 `-datadir`，qemu 落回 AVD 自身目录（已有初始化好的
    /// userdata + encryptionkey，避开自建镜像「清 userdata 首启挂死」）；
    /// Some=宿主持久路径（产品形态，首次需从镜像模板初始化）。
    pub data_dir: Option<PathBuf>,
    /// guest 内存 MB（默认 1536，实测甜点；qemu 层直传不受 emulator 钳制）。
    pub memory_mb: u32,
    /// vCPU 数（默认 4）。
    pub cores: u32,
    /// 方案 A 恒 true：`-no-window -no-audio`。
    pub headless: bool,
    /// 快照名；None=冷启（`-no-snapshot`）。
    pub snapshot: Option<String>,
    /// 追加的原始 qemu 参数（最后拼接，可覆盖前面的选项）。
    pub extra_args: Vec<String>,
    /// 显式 (console, adb) 端口对；None=从 5554 起自动找成对空闲端口。
    pub ports: Option<(u16, u16)>,
    /// 显式 gRPC 端口；None=从 8554 起自动找空闲端口。
    pub grpc_port: Option<u16>,
}

impl Default for QemuConfig {
    fn default() -> Self {
        Self {
            avd_name: String::new(),
            image_dir: None,
            data_dir: None,
            memory_mb: 1536,
            cores: 4,
            headless: true,
            snapshot: None,
            extra_args: Vec::new(),
            ports: None,
            grpc_port: None,
        }
    }
}

/// qemu 二进制及其资源目录的定位（SDK 布局或 .app bundle 布局）。
#[derive(Debug, Clone)]
pub struct QemuRuntime {
    /// qemu-system-aarch64-headless 绝对路径。
    pub qemu_bin: PathBuf,
    /// emulator 安装目录（`ANDROID_EMULATOR_LAUNCHER_DIR`；缺失时 qemu 静默退出）。
    pub launcher_dir: PathBuf,
    /// qemu dylib 闭包目录（`DYLD_LIBRARY_PATH` 基准，SDK 为 emulator/lib64）。
    pub lib_dir: PathBuf,
    /// `ANDROID_SDK_ROOT`（qemu 仅用它找 system-images，存在即可）。
    pub sdk_root: Option<PathBuf>,
    /// `ANDROID_AVD_HOME`（AVD `.ini` 所在目录）。
    pub avd_home: Option<PathBuf>,
}

impl QemuRuntime {
    /// 从本机 Android SDK 布局探测（开发机/CLI 路径）。
    pub fn detect_from_sdk() -> Option<Self> {
        let sdk = [
            std::env::var_os("ANDROID_HOME"),
            std::env::var_os("ANDROID_SDK_ROOT"),
        ]
        .into_iter()
        .flatten()
        .map(PathBuf::from)
        .find(|p| p.join("emulator/qemu").is_dir())
        .or_else(|| {
            let home = std::env::var_os("HOME").map(PathBuf::from)?;
            let p = home.join("Library/Android/sdk");
            p.join("emulator/qemu").is_dir().then_some(p)
        })?;

        let (arch_dir, bin_name) = if cfg!(target_arch = "aarch64") {
            ("darwin-aarch64", "qemu-system-aarch64-headless")
        } else {
            ("darwin-x86_64", "qemu-system-x86_64-headless")
        };
        let launcher_dir = sdk.join("emulator");
        let qemu_bin = launcher_dir.join("qemu").join(arch_dir).join(bin_name);
        if !qemu_bin.is_file() {
            return None;
        }
        Some(Self {
            qemu_bin,
            launcher_dir: launcher_dir.clone(),
            lib_dir: launcher_dir.join("lib64"),
            sdk_root: Some(sdk),
            avd_home: std::env::var_os("ANDROID_AVD_HOME")
                .map(PathBuf::from)
                .or_else(|| {
                    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".android/avd"))
                }),
        })
    }

    /// `.app` bundle 布局：qemu 与 dylib 都在 bundle 内（产品形态）。
    pub fn bundle(qemu_bin: PathBuf, launcher_dir: PathBuf, lib_dir: PathBuf) -> Self {
        Self {
            qemu_bin,
            launcher_dir,
            lib_dir,
            sdk_root: None,
            avd_home: None,
        }
    }

    /// qemu 必须能找到的 dylib 搜索路径（vulkan/gles_angle 子目录缺一不可，
    /// 否则 headless 渲染器初始化失败）。
    pub fn dyld_library_path(&self) -> String {
        let l = &self.lib_dir;
        format!("{}:{}:{}", l.join("vulkan").display(), l.join("gles_angle").display(), l.display())
    }

    pub fn validate(&self) -> Result<()> {
        if !self.qemu_bin.is_file() {
            return Err(Error::BinaryNotFound(self.qemu_bin.clone()));
        }
        Ok(())
    }
}

impl QemuConfig {
    /// 校验自建镜像目录。存在性与 ramdisk 完整性两项都来自实测根因：
    /// build.prop 缺失会静默回退 TCG（TROUBLESHOOTING #1）；
    /// 不完整 ramdisk（<8MB）内核 panic 循环、adb 永久 offline。
    pub fn validate_image_dir(&self) -> Result<()> {
        let Some(dir) = &self.image_dir else {
            return Ok(());
        };
        for f in [
            "system-qemu.img",
            "ramdisk-qemu.img",
            "kernel-ranchu",
            "build.prop",
        ] {
            if !dir.join(f).is_file() {
                return Err(Error::ImageMissing {
                    file: f,
                    dir: dir.clone(),
                });
            }
        }
        let kb = std::fs::metadata(dir.join("ramdisk-qemu.img"))
            .map_err(|e| Error::LaunchFailed(format!("读取 ramdisk 元数据失败: {e}")))?
            .len()
            / 1024;
        if kb < 8 * 1024 {
            return Err(Error::RamdiskTooSmall { kb });
        }
        Ok(())
    }

    /// 组装 qemu argv（端口为已解析的具体值；extra_args 最后拼接）。
    pub fn build_args(&self, console_port: u16, adb_port: u16, grpc_port: u16) -> Vec<String> {
        let mut args: Vec<String> = Vec::new();
        match &self.image_dir {
            Some(dir) => {
                args.push("-sysdir".into());
                args.push(dir.display().to_string());
                args.push("-system".into());
                args.push(dir.join("system-qemu.img").display().to_string());
                let vendor = dir.join("vendor-qemu.img");
                if vendor.is_file() {
                    args.push("-vendor".into());
                    args.push(vendor.display().to_string());
                }
                args.push("-ramdisk".into());
                args.push(dir.join("ramdisk-qemu.img").display().to_string());
                args.push("-kernel".into());
                args.push(dir.join("kernel-ranchu").display().to_string());
                if !self.avd_name.is_empty() {
                    // 仅为让 qemu 记录身份，镜像已由上面的显式路径覆盖
                    args.push("-id".into());
                    args.push(self.avd_name.clone());
                }
            }
            None => {
                args.push("-avd".into());
                args.push(self.avd_name.clone());
            }
        }
        if self.headless {
            args.extend(["-no-window".into(), "-no-audio".into()]);
        }
        // headless 下启动动画毫无用处，白白拖慢冷启
        args.push("-no-boot-anim".into());
        args.push("-memory".into());
        args.push(self.memory_mb.to_string());
        args.push("-cores".into());
        args.push(self.cores.to_string());
        match &self.snapshot {
            Some(name) => {
                args.push("-snapshot".into());
                args.push(name.clone());
            }
            None => args.push("-no-snapshot".into()),
        }
        if let Some(dd) = &self.data_dir {
            args.push("-datadir".into());
            args.push(dd.display().to_string());
        }
        args.push("-ports".into());
        args.push(format!("{console_port},{adb_port}"));
        args.push("-grpc".into());
        args.push(grpc_port.to_string());
        args.extend(self.extra_args.iter().cloned());
        args
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base() -> QemuConfig {
        QemuConfig {
            avd_name: "MWine1".into(),
            ..Default::default()
        }
    }

    #[test]
    fn args_avd_mode_covers_blueprint_surface() {
        let args = base().build_args(5554, 5555, 8554);
        let joined = args.join(" ");
        assert!(joined.contains("-avd MWine1"), "应有 -avd: {joined}");
        assert!(joined.contains("-no-window"));
        assert!(joined.contains("-no-audio"));
        assert!(joined.contains("-memory 1536"));
        assert!(joined.contains("-cores 4"));
        assert!(joined.contains("-no-snapshot"), "snapshot=None 应冷启");
        assert!(joined.contains("-ports 5554,5555"));
        assert!(joined.contains("-grpc 8554"));
        // data_dir=None：不传 -datadir，qemu 落回 AVD 自身目录
        assert!(!args.contains(&"-datadir".to_string()));
    }

    #[test]
    fn args_explicit_data_dir_is_passed() {
        let cfg = QemuConfig {
            data_dir: Some(PathBuf::from("/data/mwine")),
            ..base()
        };
        let joined = cfg.build_args(5554, 5555, 8554).join(" ");
        assert!(joined.contains("-datadir /data/mwine"));
    }

    #[test]
    fn args_snapshot_and_extra_override_tail() {
        let cfg = QemuConfig {
            snapshot: Some("quick1".into()),
            extra_args: vec!["-http-proxy".into(), "http://127.0.0.1:7890".into()],
            ..base()
        };
        let args = cfg.build_args(5556, 5557, 8556);
        assert!(args.contains(&"-snapshot".to_string()));
        assert!(!args.contains(&"-no-snapshot".to_string()));
        // extra_args 拼在最后，可覆盖前面的默认值
        assert_eq!(&args[args.len() - 2..], &["-http-proxy", "http://127.0.0.1:7890"]);
    }

    #[test]
    fn args_image_dir_mode_uses_explicit_paths() {
        let cfg = QemuConfig {
            image_dir: Some(PathBuf::from("/img")),
            ..base()
        };
        let joined = cfg.build_args(5554, 5555, 8554).join(" ");
        assert!(joined.contains("-sysdir /img"));
        assert!(joined.contains("-system /img/system-qemu.img"));
        assert!(joined.contains("-ramdisk /img/ramdisk-qemu.img"));
        assert!(joined.contains("-kernel /img/kernel-ranchu"));
        assert!(!joined.contains("-vendor"), "vendor 缺失时应省略而非报错");
        assert!(joined.contains("-id MWine1"), "保留身份标识便于 console 查询");
    }

    #[test]
    fn validate_image_dir_rejects_missing_and_stubby_ramdisk() {
        let d = std::env::temp_dir().join(format!("mwine_qemu_img_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();

        // 空目录：缺 system-qemu.img
        let err = QemuConfig {
            image_dir: Some(d.clone()),
            ..base()
        }
        .validate_image_dir()
        .unwrap_err();
        assert!(matches!(err, Error::ImageMissing { file: "system-qemu.img", .. }));

        // 文件齐全但 ramdisk 只有 1KB（构建不完整特征）
        for f in ["system-qemu.img", "vendor-qemu.img", "kernel-ranchu", "build.prop"] {
            std::fs::write(d.join(f), b"x").unwrap();
        }
        std::fs::write(d.join("ramdisk-qemu.img"), vec![0u8; 1024]).unwrap();
        let err = QemuConfig {
            image_dir: Some(d.clone()),
            ..base()
        }
        .validate_image_dir()
        .unwrap_err();
        assert!(matches!(err, Error::RamdiskTooSmall { kb: 1 }));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn runtime_dyld_path_includes_gl_subdirs() {
        let rt = QemuRuntime::bundle(
            PathBuf::from("/app/qemu"),
            PathBuf::from("/app"),
            PathBuf::from("/app/lib64"),
        );
        assert_eq!(
            rt.dyld_library_path(),
            "/app/lib64/vulkan:/app/lib64/gles_angle:/app/lib64"
        );
    }
}
