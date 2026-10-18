// 设备侧输出解析（纯函数，便于离线单测）。对应 crates/mwine-app/src/dumpsys.rs：
// - `dumpsys package`（单包 / 全量）→ 版本与安装时间
// - `ps -A -o NAME` → 运行中进程集合
// - `cmd package resolve-activity --brief` → 启动入口组件
#pragma once

#include <QMap>
#include <QSet>
#include <QString>

#include <optional>

namespace mw::app {

// dumpsys 中我们关心的包级字段。
struct PackageDump {
    std::optional<QString> versionName;
    std::optional<quint64> versionCode;
    std::optional<QString> firstInstallTime;
    std::optional<QString> lastUpdateTime;
};

// 解析 `dumpsys package <pkg>`（单包）输出。
PackageDump parseDumpsysPackage(const QString& text);

// 解析 `dumpsys package`（全量）输出：包名 → 字段。
// 以 `Package [name]` 行分段；非 package 段落（Providers 等）天然跳过。
QMap<QString, PackageDump> parseDumpsysAll(const QString& text);

// 解析 `ps -A -o NAME` 输出为进程名集合（忽略表头）。
QSet<QString> parseProcessNames(const QString& text);

// 判断包是否有进程在运行：进程名等于包名或以 `包名:` / `包名.` 开头。
bool packageRunning(const QSet<QString>& procs, const QString& package);

// 从 `cmd package resolve-activity --brief <pkg>` 输出提取入口组件
//（最后一行含 '/' 的行；形如 `com.pkg/.MainActivity`）。
std::optional<QString> parseResolveActivity(const QString& text);

} // namespace mw::app
