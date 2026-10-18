// 桌面 .app 壳生成 / 扫描 / APK 双击安装器（macOS 特有部分）。
// 对应 mwine-cli 中 app create / app installer 的核心逻辑（mwine-app 模块职责的
// “应用桌面壳”面），QProcess/QFile 等价实现；进程序列（sips/iconutil/osacompile/
// codesign/lsregister）与 Rust 版一致。
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

#include "badging.hpp"
#include "manager.hpp"
#include "result.hpp"

namespace mw::app {

// —— 路径与文本工具 ——
// 壳输出目录（Rust 实现取 ~/Applications/Mwine；CLI 帮助文案里的 Houttuynia 为旧称）。
QString defaultAppShellsDir();
// 作为文件名/CFBundleExecutable 使用的安全化（保留中文等合法字符）。
QString sanitizeFileName(const QString& s);
QString xmlEscape(const QString& s);

// —— bundle 内嵌配置（app.conf: key=value）——
struct BundledConfig {
    QString package;
    std::optional<QString> title;
    std::optional<QString> avd;
    QString display = QStringLiteral("944x2048/384");
    std::optional<float> fps;
    bool fullscreen = false;
};

// 解析 app.conf 文本（纯函数；package 缺失返回 nullopt）。
std::optional<BundledConfig> parseAppConf(const QByteArray& text);
// 检测当前可执行文件是否位于 <X>.app/Contents/MacOS/ 内，是则读取配置。
std::optional<BundledConfig> readBundledConfig();
// 壳启动时读 appmodes：window 模式的应用直接走直通窗口。
// appmodesPath 缺省读 ~/.mwine/appmodes.json。
bool bundledAppWindowMode(const QString& package, const QString& appmodesPath = {});

// —— 壳扫描：列出目录下全部 .app 壳及其内嵌配置 ——
struct AppShellEntry {
    QString appDir;     // …/Foo.app
    QString exeName;    // CFBundleExecutable
    BundledConfig conf; // Resources/app.conf（package 为空 = 非 mwine 壳）
    bool hasIcon = false;
};
QVector<AppShellEntry> scanAppShells(const QString& dir = QString());

// —— 图标工具 ——
// 位图 -> AppIcon.icns（sips 生成 iconset + iconutil 打包）。
mw::Result<bool> makeIcns(const QString& sourceBitmap, const QString& destIcns);
// 尽力从 APK 生成 Resources/AppIcon.icns；失败返回 nullopt（壳仍可用，无图标）。
std::optional<QString> extractAppIcon(const QString& apkPath,
                                      const std::optional<ApkBadging>& badging,
                                      const QString& resourcesDir);
// companion（com.mwine.bridge IconDump）经 Android 自身管线渲染 adaptive/矢量图标。
std::optional<QString> renderIconOnDevice(AdbShell& adb, const QString& package,
                                          const QString& resourcesDir);
// companion APK 定位：MWINE_BRIDGE_APK > bundle Resources/bin > 仓库 assets/companion。
std::optional<QString> findCompanionApk();

// —— .app 壳生成（原 mwine-cli cmd_app_create 核心）——
struct CreateAppShellOptions {
    // 应用包名（与 apkPath 二选一）
    QString package;
    // 从本地 APK 创建：解析包名/显示名/图标（纯 C++ + aapt2 可选增强），可顺带安装
    QString apkPath;
    // 显示名（默认取 APK label 或包名）
    QString name;
    // 输出目录（默认 defaultAppShellsDir()）
    QString outDir;
    // 把 AVD 名烘焙进启动器（无在线设备时自动启动该 AVD）
    QString avd;
    // apkPath 模式下跳过安装到设备
    bool noInstall = false;
    // 安装/设备侧图标渲染的目标设备（空 = 只生成壳不安装）
    QString serial;
    // 只通过日志输出 .app 路径（供安装器脚本消费）
    bool printPath = false;
};

struct CreateAppShellResult {
    QString appDir;
    QString package;
    QString display;
    bool installedToDevice = false;
    bool hasIcon = false;
};

mw::Result<CreateAppShellResult> createAppShell(const CreateAppShellOptions& opts);

// —— APK 双击安装器 .app（注册 .apk 文件关联：双击即安装并生成应用壳）——
// 生成的 .app 路径经 appDirOut 带出。
mw::Result<bool> createApkInstaller(const QString& outDir = QString(),
                                    QString* appDirOut = nullptr);

// —— macOS 进程辅助 ——
// ad-hoc 签名（幂等）：返回签名后是否通过校验（非 macOS 恒 true）。
bool codesignBestEffort(const QString& path);
// 让 LaunchServices 立即收录（Launchpad/Dock 可见），不必等系统扫描。
mw::Result<bool> lsregister(const QString& appDir);

} // namespace mw::app
