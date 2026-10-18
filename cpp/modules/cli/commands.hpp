// mwine CLI 子命令处理函数声明（分发在 main.cpp）。
// 命令/参数名与 Rust 版 clap 定义一致；帮助文案精简。
#pragma once

#include <optional>

#include "args.hpp"
#include "result.hpp"

namespace mwine {

// bundle 模式探测（可执行文件位于 <X>.app/Contents/MacOS/ 内时读 app.conf
// 直接运行内嵌应用；非 bundle 环境返回 nullopt）。实现在 commands_apps.cpp。
std::optional<int> maybeBundledMain();

// commands_vm.cpp
mw::Result<bool> cmdDoctor(Args& args);
mw::Result<bool> cmdVm(Args& args);
mw::Result<bool> cmdDevices(Args& args);
mw::Result<bool> cmdKill(Args& args);
mw::Result<bool> cmdImage(Args& args);

// commands_apps.cpp
mw::Result<bool> cmdInstall(Args& args);
mw::Result<bool> cmdApps(Args& args);
mw::Result<bool> cmdFiles(Args& args);
mw::Result<bool> cmdCamera(Args& args);
mw::Result<bool> cmdRun(Args& args);
mw::Result<bool> cmdNotify(Args& args);

// commands_store.cpp
mw::Result<bool> cmdStore(Args& args);

// commands_device.cpp：真机连接（与守护 /api/devices* 同源）
mw::Result<bool> cmdDevice(Args& args);

// commands_keys.cpp
mw::Result<bool> cmdKeys(Args& args);
mw::Result<bool> cmdMonitor(Args& args);

// commands_misc.cpp
mw::Result<bool> cmdCapture(Args& args);
mw::Result<bool> cmdMirror(Args& args);
mw::Result<bool> cmdDebug(Args& args);
mw::Result<bool> cmdApp(Args& args);

// 帮助输出
void printMainHelp();
void printVersion();

} // namespace mwine
