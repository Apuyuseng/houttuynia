// 启动配置与 argv 组装。
// ← crates/mwine-qemu/src/config.rs
//
// 约定（Rust → C++）：
//   Result<T>     → mw::Result<T>（错误 QString）
//   Option<T>     → std::optional<T>
//   PathBuf       → QString
//   Vec<String>   → QStringList
//   (u16, u16)    → QPair<quint16, quint16>
#pragma once

#include <QPair>
#include <QString>
#include <QStringList>
#include <optional>

#include "result.hpp"

namespace mw::qemu {

// —— 错误文案（Rust Error enum 各变体 Display 的等价物；跨 TU 共用的三条在此） ——

// Error::BinaryNotFound(path)
inline QString errBinaryNotFound(const QString& path)
{
    return QStringLiteral("qemu 二进制不存在: %1").arg(path);
}

// Error::NoTarget（avd 名与镜像目录均为空）
inline QString errNoTarget()
{
    return QStringLiteral("未指定启动目标（avd 名与 imageDir 均为空）");
}

// Error::PortExhausted
inline QString errPortExhausted()
{
    return QStringLiteral("无可用端口");
}

/// qemu 直启配置（Rust QemuConfig）。
/// `imageDir` 扩展为 optional 以兼容自建镜像直载模式，
/// `ports`/`grpcPort` 为端口显式覆盖项。
struct QemuConfig {
    QString avdName;
    /// 自建镜像目录（system-qemu.img / vendor-qemu.img / ramdisk-qemu.img /
    /// kernel-ranchu / build.prop）。有值时走 `-sysdir` 直载，忽略 AVD 镜像。
    std::optional<QString> imageDir;
    /// 用户数据目录（userdata-qemu.img / snapshots 所在）。
    /// 空=不传 `-datadir`，qemu 落回 AVD 自身目录（已有初始化好的
    /// userdata + encryptionkey，避开自建镜像「清 userdata 首启挂死」）；
    /// 有值=宿主持久路径（产品形态，首次需从镜像模板初始化）。
    std::optional<QString> dataDir;
    quint32 memoryMb = 1536; ///< guest 内存 MB（默认 1536，实测甜点；qemu 层直传不受 emulator 钳制）
    quint32 cores = 4;       ///< vCPU 数（默认 4）
    bool headless = true;    ///< 方案 A 恒 true：`-no-window -no-audio`
    std::optional<QString> snapshot; ///< 快照名；空=冷启（`-no-snapshot`）
    QStringList extraArgs;   ///< 追加的原始 qemu 参数（最后拼接，可覆盖前面的选项）
    std::optional<QPair<quint16, quint16>> ports; ///< 显式 (console, adb) 端口对；空=从 5554 起自动找成对空闲端口
    std::optional<quint16> grpcPort;              ///< 显式 gRPC 端口；空=从 8554 起自动找空闲端口

    /// 校验自建镜像目录。存在性与 ramdisk 完整性两项都来自实测根因：
    /// build.prop 缺失会静默回退 TCG（TROUBLESHOOTING #1）；
    /// 不完整 ramdisk（<8MB）内核 panic 循环、adb 永久 offline。
    mw::Result<bool> validateImageDir() const;

    /// 组装 qemu argv（端口为已解析的具体值；extraArgs 最后拼接）。
    QStringList buildArgs(quint16 consolePort, quint16 adbPort, quint16 grpcPort) const;
};

/// qemu 二进制及其资源目录的定位（SDK 布局或 .app bundle 布局）。Rust QemuRuntime。
struct QemuRuntime {
    QString qemuBin;     ///< qemu-system-aarch64-headless 绝对路径
    QString launcherDir; ///< emulator 安装目录（ANDROID_EMULATOR_LAUNCHER_DIR；缺失时 qemu 静默退出）
    QString libDir;      ///< qemu dylib 闭包目录（DYLD_LIBRARY_PATH 基准，SDK 为 emulator/lib64）
    std::optional<QString> sdkRoot; ///< ANDROID_SDK_ROOT（qemu 仅用它找 system-images，存在即可）
    std::optional<QString> avdHome; ///< ANDROID_AVD_HOME（AVD .ini 所在目录）

    /// 从本机 Android SDK 布局探测（开发机/CLI 路径）。探测失败返回空。
    static std::optional<QemuRuntime> detectFromSdk();

    /// `.app` bundle 布局：qemu 与 dylib 都在 bundle 内（产品形态）。
    static QemuRuntime bundle(QString qemuBin, QString launcherDir, QString libDir);

    /// qemu 必须能找到的 dylib 搜索路径（vulkan/gles_angle 子目录缺一不可，
    /// 否则 headless 渲染器初始化失败）。
    QString dyldLibraryPath() const;

    mw::Result<bool> validate() const;
};

} // namespace mw::qemu
