// mwine CLI 跨命令共享工具（实现在 main.cpp）。
// ← crates/mwine-cli/src/main.rs 的 adb()/resolve_serial()/pick_default_avd()/
//    resolve_install_serial()/daemon_start_and_wait()/ensure_install_target()/
//    emu_cmd()/core_api() 等自由函数。
#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <functional>
#include <optional>

#include "adb.hpp"
#include "result.hpp"

namespace mwine {

// AdbClient 工厂（对应 Rust fn adb()）
mw::adb::AdbClient adbClient();

// 目标设备解析：显式 serial 优先，否则唯一在线设备
mw::Result<QString> resolveSerial(const std::optional<QString>& explicitSerial);

// ~/.config/mwine/last-avd 记录 / 读取链
void recordLastAvd(const QString& name);
std::optional<QString> pickDefaultAvd();

// 多设备在线时的安装目标解析（install / app create / run 共用）：
// 1) 显式 --serial；2) daemon 拉起的 VM（/api/status.serial 且在线）；
// 3) 唯一在线设备；4) 多台 → 报错列出。
// Ok(nullopt) = 无在线设备（调用方走自动启动默认 AVD 分支）。
mw::Result<std::optional<QString>> resolveInstallSerial(const mw::adb::AdbClient* client,
                                                         const std::optional<QString>& explicitSerial);

// 无在线设备时请 daemon 启动它管理的 VM；失败 → nullopt（调用方本地兜底）。
std::optional<QString> daemonStartAndWait(const mw::adb::AdbClient* client,
                                          const std::function<void(const QString&)>& vmLine);

// 解析安装目标：无在线设备时自动拉起虚拟机（daemon 优先，本地 headless 兜底）。
mw::Result<QString> ensureInstallTarget(const mw::adb::AdbClient* client,
                                        const std::function<void(const QString&)>& vmLine);
// 在线模拟器 serial 列表 / daemon 上报的 VM serial（mirror 等查看类命令用）
QStringList onlineEmulators(const mw::adb::AdbClient* client);
QString daemonVmSerial();

// emulator 控制台简封：打印应答，KO 视为错误
mw::Result<QString> emuCmd(const QString& serial, const QString& cmd);

// scrcpy-server 定位（对应 mwine_scrcpy::find_server_jar 的候选路径扫描；
// mw_scrcpy 模块转换落地后应切换到模块实现）。
std::optional<QString> findServerJar();

// macOS 通知中心（osascript）
void postMacosNotification(const QString& title, const QString& subtitle, const QString& body);

// 单引号 shell 转义
QString shellQuote(const QString& s);

// daemon REST（capture 命令路径，对应 Rust core_api()；MWINE_PORT 覆盖端口）。
// 返回 (HTTP status, body)；连接失败返回 Err。
mw::Result<QPair<int, QString>> coreApi(const QString& method, const QString& path,
                                        const QByteArray& body = {});

// 用法错误（对齐 clap：exit 2 由 main 统一处理，这里只构造错误文案）
template <typename T>
mw::Result<T> badUsage(const QString& msg)
{
    return mw::err<T>(msg);
}

// stdout 是否 TTY
bool stdoutIsTty();

// 拉起独立应用窗口播放器（mwine-window，Qt）。run --window / mirror 共用。
// package 为空 = mirror 模式；record 非空时落 .h264 裸流；extra 透传附加参数。
mw::Result<bool> spawnWindowPlayer(const QString& package, const QString& serial,
                                   const QString& display, bool mirror, const QString& record,
                                   const QStringList& extra = {});

} // namespace mwine
