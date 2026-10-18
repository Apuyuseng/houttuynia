// lib.rs 级公共类型：AppIcon/IconMime/AppInfo/ListOptions/AppManager/ApkInfo + 错误构造。
// 对应 crates/mwine-app/src/lib.rs（camelCase 化；Error 枚举 → QString 格式化助手）。
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

#include "result.hpp"

namespace mw::app {

// ---- 错误构造（Rust Error 枚举的字符串化对应物）----
inline QString errAdb(const QString& what) { return QStringLiteral("adb: %1").arg(what); }
inline QString errShell(const QString& cmd, const QString& output)
{
    return QStringLiteral("shell 命令失败（%1）: %2").arg(cmd, output.trimmed());
}
inline QString errNotInstalled(const QString& package)
{
    return QStringLiteral("包 %1 未安装").arg(package);
}
inline QString errNoLauncher(const QString& package)
{
    return QStringLiteral("包 %1 没有可启动的 launcher 入口").arg(package);
}
inline QString errApkParse(const QString& path, const QString& reason)
{
    return QStringLiteral("APK 解析失败（%1）: %2").arg(path, reason);
}
inline QString errIo(const QString& what) { return QStringLiteral("io: %1").arg(what); }

// ---- 应用图标：原始位图字节 + MIME（PNG 或 WebP），宿主侧不做格式转换 ----
enum class IconMime { Png, Webp };

inline const char* iconMimeStr(IconMime m)
{
    return m == IconMime::Png ? "image/png" : "image/webp";
}

// 从文件名或魔数判断格式（未知返回 nullopt）。对应 Rust IconMime::detect。
inline std::optional<IconMime> detectIconMime(const QString& path, const QByteArray& bytes)
{
    static const QByteArray kPngMagic = QByteArray::fromRawData("\x89\x50\x4e\x47", 4);
    const QString lower = path.toLower();
    if (lower.endsWith(QLatin1String(".png"))
        || (bytes.size() >= 4 && bytes.startsWith(kPngMagic)))
        return IconMime::Png;
    if (lower.endsWith(QLatin1String(".webp"))
        || (bytes.size() > 12 && bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP"))
        return IconMime::Webp;
    return std::nullopt;
}

struct AppIcon {
    QByteArray data;
    IconMime mime = IconMime::Png;
};

// ---- 单个已安装应用的元数据（对应 Rust AppInfo；Option<T> → std::optional<T>）----
struct AppInfo {
    QString package;
    std::optional<QString> name;        // 显示标签（APK label 字面量或 aapt2 解析）
    std::optional<QString> version;     // versionName
    std::optional<quint64> versionCode;
    std::optional<AppIcon> icon;
    bool isRunning = false;
    std::optional<QString> apkPath;     // guest 侧 base APK 路径（pm path 首行）
    std::optional<QString> launcherActivity; // pkg/.MainActivity 形式（懒解析）
    std::optional<QString> installedAt;
    std::optional<QString> updatedAt;

    // UI 展示名：标签缺失时退化为包名末段（com.foo.bar → bar）。
    QString displayName() const
    {
        if (name && !name->trimmed().isEmpty())
            return *name;
        const int idx = package.lastIndexOf(QLatin1Char('.'));
        return idx < 0 ? package : package.mid(idx + 1);
    }
};

// bool 值载体：mw::Result<bool> 已被基础库定义为 unit 语义（对应 Rust Result<(), E>），
// 需要把布尔当“值”携带时（如 isInstalled）用它。
struct BoolValue {
    bool value = false;
};

// list() 元数据填充策略：档位递增、成本递增。
struct ListOptions {
    bool enrich = false;        // 填充 version/installedAt/isRunning（dumpsys + ps 批量）
    bool pullMetadata = false;  // 每应用一次 pull 解析标签（字面量）/图标
};

// 应用管理接口（对应 Rust trait AppManager）。
// 实现约定：设备离线返回 adb 错误；不存在的包返回 NotInstalled；shell 输出异常返回 Shell 错误。
class AppManager {
public:
    virtual ~AppManager() = default;

    // 安装 APK（pm install -r -t），安装后补授运行时权限并返回完整元数据。
    virtual mw::Result<AppInfo> install(const QString& apkPath) = 0;
    virtual mw::Result<bool> uninstall(const QString& package) = 0;
    // displayId 指定目标显示器（multidisplay 场景）；nullopt 启动到默认显示器。
    virtual mw::Result<bool> launch(const QString& package, std::optional<quint32> displayId) = 0;
    virtual mw::Result<QVector<AppInfo>> list(const ListOptions& options) = 0;
    virtual mw::Result<AppInfo> get(const QString& package) = 0;
    virtual mw::Result<bool> stop(const QString& package) = 0;
    virtual mw::Result<BoolValue> isInstalled(const QString& package) = 0;
};

// ---- 一步到位的 APK 本地解析结果（任务约定的便捷 API）----
// label 为空 = 未能解析（@string 引用需 resources.arsc，v1 未实现——降级语义，
// 精确 Option 语义见 ManifestInfo::label）；versionCode 为 0 = 未知。
struct ApkInfo {
    QString package;
    QString label;
    QString versionName;
    quint64 versionCode = 0;
    std::optional<QByteArray> iconPng; // PNG 或 WebP 原始字节（entry 未命中时 nullopt）
    QString iconMime;                  // iconPng 存在时为 "image/png" / "image/webp"

    QString displayName() const; // 标签缺失时退化为包名末段
};

// 解析本地 APK：zip + AXML 纯 C++ 路径为主，aapt2 存在时叠加 badging 增强。
mw::Result<ApkInfo> parseApk(const QString& apkPath);

} // namespace mw::app
