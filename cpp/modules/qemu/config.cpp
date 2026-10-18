// 启动配置与 argv 组装 ← crates/mwine-qemu/src/config.rs
#include "config.hpp"

#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

namespace mw::qemu {

// ===================== QemuConfig =====================

mw::Result<bool> QemuConfig::validateImageDir() const
{
    if (!imageDir.has_value())
        return mw::Result<bool>::Ok();
    const QDir dir(*imageDir);
    for (const char* f : { "system-qemu.img", "ramdisk-qemu.img", "kernel-ranchu", "build.prop" }) {
        if (!QFileInfo(dir.filePath(QString::fromLatin1(f))).isFile())
            // Error::ImageMissing { file, dir }
            return mw::err<bool>(QStringLiteral("镜像目录缺少 %1: %2")
                                     .arg(QString::fromLatin1(f), *imageDir));
    }
    // Error::RamdiskTooSmall { kb }（<8MB 视为构建不完整）
    const QFileInfo ramdisk(dir.filePath(QStringLiteral("ramdisk-qemu.img")));
    const qint64 kb = ramdisk.size() / 1024;
    if (kb < 8 * 1024)
        return mw::err<bool>(QStringLiteral("ramdisk-qemu.img 仅 %1KB（不足 8MB，构建不完整）").arg(kb));
    return mw::Result<bool>::Ok();
}

QStringList QemuConfig::buildArgs(quint16 consolePort, quint16 adbPort, quint16 grpcPort) const
{
    QStringList args;
    if (imageDir.has_value()) {
        const QDir dir(*imageDir);
        args << QStringLiteral("-sysdir") << *imageDir;
        args << QStringLiteral("-system") << dir.filePath(QStringLiteral("system-qemu.img"));
        const QString vendor = dir.filePath(QStringLiteral("vendor-qemu.img"));
        if (QFileInfo::exists(vendor) && QFileInfo(vendor).isFile())
            args << QStringLiteral("-vendor") << vendor;
        args << QStringLiteral("-ramdisk") << dir.filePath(QStringLiteral("ramdisk-qemu.img"));
        args << QStringLiteral("-kernel") << dir.filePath(QStringLiteral("kernel-ranchu"));
        if (!avdName.isEmpty())
            // 仅为让 qemu 记录身份，镜像已由上面的显式路径覆盖
            args << QStringLiteral("-id") << avdName;
    } else {
        args << QStringLiteral("-avd") << avdName;
    }
    if (headless)
        args << QStringLiteral("-no-window") << QStringLiteral("-no-audio");
    // headless 下启动动画毫无用处，白白拖慢冷启
    args << QStringLiteral("-no-boot-anim");
    args << QStringLiteral("-memory") << QString::number(memoryMb);
    args << QStringLiteral("-cores") << QString::number(cores);
    if (snapshot.has_value())
        args << QStringLiteral("-snapshot") << *snapshot;
    else
        args << QStringLiteral("-no-snapshot");
    if (dataDir.has_value())
        args << QStringLiteral("-datadir") << *dataDir;
    args << QStringLiteral("-ports")
         << QStringLiteral("%1,%2").arg(consolePort).arg(adbPort);
    args << QStringLiteral("-grpc") << QString::number(grpcPort);
    args << extraArgs;
    return args;
}

// ===================== QemuRuntime =====================

std::optional<QemuRuntime> QemuRuntime::detectFromSdk()
{
    // ANDROID_HOME / ANDROID_SDK_ROOT（存在 emulator/qemu 子目录者优先），否则 ~/Library/Android/sdk
    const auto hasQemuDir = [](const QString& sdk) {
        return QFileInfo(QDir(sdk).filePath(QStringLiteral("emulator/qemu"))).isDir();
    };
    QString sdk;
    for (const char* var : { "ANDROID_HOME", "ANDROID_SDK_ROOT" }) {
        const QString v = qEnvironmentVariable(var);
        if (!v.isEmpty() && hasQemuDir(v)) {
            sdk = v;
            break;
        }
    }
    if (sdk.isEmpty()) {
        const QString home = qEnvironmentVariable("HOME");
        if (!home.isEmpty()) {
            const QString p = QDir(home).filePath(QStringLiteral("Library/Android/sdk"));
            if (hasQemuDir(p))
                sdk = p;
        }
    }
    if (sdk.isEmpty())
        return std::nullopt;

#if defined(__aarch64__)
    const char* archDir = "darwin-aarch64";
    const char* binName = "qemu-system-aarch64-headless";
#else
    const char* archDir = "darwin-x86_64";
    const char* binName = "qemu-system-x86_64-headless";
#endif
    const QString launcherDir = QDir(sdk).filePath(QStringLiteral("emulator"));
    const QString qemuBin = QDir(QDir(launcherDir).filePath(QStringLiteral("qemu")))
                                .filePath(QString::fromLatin1(archDir) + QChar(u'/')
                                          + QString::fromLatin1(binName));
    if (!QFileInfo(qemuBin).isFile())
        return std::nullopt;

    QemuRuntime rt;
    rt.qemuBin = qemuBin;
    rt.launcherDir = launcherDir;
    rt.libDir = QDir(launcherDir).filePath(QStringLiteral("lib64"));
    rt.sdkRoot = sdk;
    // ANDROID_AVD_HOME → HOME/.android/avd（HOME 缺失时保持空）
    QString avdEnv = qEnvironmentVariable("ANDROID_AVD_HOME");
    if (!avdEnv.isEmpty()) {
        rt.avdHome = avdEnv;
    } else {
        const QString home = qEnvironmentVariable("HOME");
        if (!home.isEmpty())
            rt.avdHome = QDir(home).filePath(QStringLiteral(".android/avd"));
    }
    return rt;
}

QemuRuntime QemuRuntime::bundle(QString qemuBin, QString launcherDir, QString libDir)
{
    QemuRuntime rt;
    rt.qemuBin = std::move(qemuBin);
    rt.launcherDir = std::move(launcherDir);
    rt.libDir = std::move(libDir);
    return rt;
}

QString QemuRuntime::dyldLibraryPath() const
{
    const QDir l(libDir);
    return QStringList{ l.filePath(QStringLiteral("vulkan")),
                        l.filePath(QStringLiteral("gles_angle")),
                        libDir }
        .join(QChar(u':'));
}

mw::Result<bool> QemuRuntime::validate() const
{
    if (!QFileInfo(qemuBin).isFile())
        return mw::err<bool>(errBinaryNotFound(qemuBin));
    return mw::Result<bool>::Ok();
}

} // namespace mw::qemu
