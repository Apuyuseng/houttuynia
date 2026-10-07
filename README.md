# Houttuynia（鱼腥草）

**在 Mac 上运行 Android 应用 —— 零依赖、秒级开关机、GPU 加速显示。**

Houttuynia 是一款运行在 Apple Silicon Mac 上的 Android 虚拟机管理器：把 Android 应用与游戏带进 macOS 桌面。APK 拖放即装，安装的每个应用自动生成可双击、可进 Dock / Launchpad 的 Mac 应用；GPU 硬件加速的显示窗口；游戏按键随意 DIY；关机自动存档、再开秒回现场。

Rust + SwiftUI 构建，界面、守护进程、ADB / scrcpy 协议栈全部自研，无任何 Java / Qt 依赖。

## 特色功能

### 🚀 开箱即用，零依赖
纯净 macOS 开箱即用：无需安装 Android Studio、SDK、Homebrew 或任何前置组件，运行时全部内嵌，拖入 `/Applications` 即可使用。

### ⚡ 秒级开关机（快照）
关机自动存档，下次启动 **5 秒级**回到上次现场（应用、页面、登录状态原样保留）；首次冷启动约 15–30 秒。

### 📱 每个应用都是 Mac 应用
安装任意 APK 后自动生成同名 `.app`：真彩图标（设备侧渲染）、Finder / Dock / Launchpad 全部可用，双击直达应用。

### 🖥 GPU 加速显示
显示由模拟器自带窗口承担，Metal / gfxstream GPU 硬件加速；窗口可自由缩放、全屏。设备形态一键切换：手机 1080×2400 竖屏 / 平板 2560×1600 横屏 / 自定义 W×H@DPI（运行中保存立即生效）。

### 🎮 游戏按键 DIY
截屏 → 点选触点 → 绑定物理按键，支持点按 / 连点 / 按住 / 滑动手势；按应用保存方案，「启用映射」后台全局生效（首次需授权「输入监控」）。

### 🔔 通知与文件互通
- 设备通知实时转发到 macOS 通知中心（companion 桥自动装配）
- 命令行一键推送文件进设备；截屏直接存桌面并在 Finder 中定位

### 📊 性能监控与省电
CPU / 内存仪表 + 3 分钟历史曲线 + 宿主开销，2 秒刷新；空闲 30 分钟自动存档关停，不占后台。

### 💿 镜像管理
- **在线拉取**：从官方仓库直接下载 arm64 系统镜像（断点续传、自动校验、支持代理）
- **本地导入**：导入 `houttuynia-image-*.tar.zst` 分发包（内嵌逐文件 sha256 校验）
- 多镜像共存，随时切换

### 🛡 代理与设备身份
HTTP 代理一键指向宿主；设备身份重置（android_id / 广告 ID 换新，应用视作全新设备）；一键重置虚拟机（清空全部数据与快照）。

### ⌨️ 命令行与 API
GUI 能做的，CLI 都能做；UI 与 CLI 同源，统一走 `mwine-core` 守护进程（localhost REST API）。

## 快速开始

1. 从 [Releases](../../releases) 下载并解压 `Houttuynia-*.zip`，把 **Houttuynia.app** 拖入「应用程序」
2. 首次打开若提示「已损坏」，双击下载页附带的 **修复打开权限.command**（或终端执行 `xattr -cr /Applications/Houttuynia.app`）
3. 启动后进入「镜像」页：在线拉取官方镜像，或导入本地分发包
4. 回到「概览」点「启动」，冷启约 15–30 秒，之后开关机都是 5 秒级
5. 拖放任意 `.apk` 到窗口即安装，桌面上会自动出现该应用的 Mac 壳

界面一览：**概览**（开关机 / 截屏 / 状态速览）、**设置**（设备形态 / 内存 / sdcard / 代理 / 身份 / 重置）、**应用库**（搜索 / 图标网格 / 卸载）、**按键映射**、**性能**、**镜像**。

## 命令行

```bash
mwine vm start <名称>             # 快照启动（~5s）
mwine vm stop <serial>            # 存档 + 关停
mwine install X.apk               # 仅安装
mwine app create --apk X.apk      # 安装 + 生成桌面壳
mwine apps list|launch|uninstall  # 应用管理
mwine keys shot|get|set|watch     # 按键映射
mwine files push <文件>           # 传文件进设备
mwine monitor                     # 资源监护
```

## 架构

```
Houttuynia.app
 ├─ MWineUI（SwiftUI 界面）
 │    └─ HTTP → mwine-core 守护进程（localhost:7788 REST）
 │         ├─ mwine-supervisor   虚拟机生命周期 / 镜像下载与导入
 │         ├─ mwine-qemu         模拟器核心进程管理（HVF 加速）
 │         ├─ mwine-adb          纯 Rust ADB 客户端（smart socket 协议）
 │         ├─ mwine-scrcpy       纯 Rust scrcpy v4 线协议客户端
 │         ├─ mwine-app          APK 解析与应用管理
 │         ├─ mwine-config       设备配置持久化与热生效
 │         ├─ mwine-keymap       按键映射引擎
 │         ├─ mwine-display      帧缓冲管线（gRPC / VNC）
 │         ├─ mwine-audio        音频路由（CoreAudio）
 │         └─ mwine-grpc         emulator gRPC 客户端
 └─ 壳 .app（每个 Android 应用一个）：内嵌 mwine 二进制 + app.conf
```

另含 `mwine-wine`（Windows 程序运行子系统，实验性）。

## 从源码构建

要求：macOS 14+（Apple Silicon）、Rust（见 `rust-toolchain.toml`）、Swift 5.9+（Xcode Command Line Tools）、本机 Android SDK（emulator + platform-tools + arm64-v8a 系统镜像）。

```bash
cargo build --release                 # 全部 Rust 组件
scripts/build_companion.sh            # 通知桥 APK（需 JDK + build-tools）
scripts/package-app.sh                # 组装 Houttuynia.app
```

> 运行时组件（模拟器核心与系统镜像）体积大，不随源码仓库分发；打包脚本从本机 Android SDK 取用。

## 已知限制

- 部分自编译镜像的媒体编码栈缺陷会导致 `/sdcard` 在个别场景不可用（文件推送已自动走降级通道）
- 独立应用窗口模式在部分镜像下无画面，已自动回退到主窗口运行

## 命名

Houttuynia = 鱼腥草（*Houttuynia cordata*）。内部二进制与 crate 沿用 `mwine` 前缀。

## 许可

[Apache-2.0](LICENSE)

Android 是 Google LLC 的商标。本项目为独立开源项目，与 Google 及其他公司无关，仅面向合法用途（应用开发、测试与个人使用）。
