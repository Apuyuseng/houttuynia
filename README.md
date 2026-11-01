# Houttuynia（鱼腥草）

<p align="center"><img src="assets/logo/png/logo-wordmark-1600.png" width="600" alt="Houttuynia 鱼腥草 — Android on macOS"></p>

**在 Mac 上运行 Android 应用 —— 零依赖、秒级开关机、GPU 加速显示。**

Houttuynia 是一款运行在 Apple Silicon Mac 上的 Android 虚拟机管理器：把 Android 应用与游戏带进 macOS 桌面。APK 拖放即装，安装的每个应用自动生成可双击、可进 Dock / Launchpad 的 Mac 应用；GPU 硬件加速的显示窗口；游戏按键随意 DIY；关机自动存档、再开秒回现场。

Qt + C++ 构建（跨平台：macOS / Linux / Windows）：界面、守护进程、ADB / scrcpy 协议栈均为自研 C++。

## 特色功能

### 🚀 开箱即用，零依赖
纯净 macOS 开箱即用：无需安装 Android Studio、SDK、Homebrew 或任何前置组件，运行时全部内嵌，拖入 `/Applications` 即可使用。

### ⚡ 秒级开关机（快照）
关机自动存档，下次启动 **5 秒级**回到上次现场（应用、页面、登录状态原样保留）；首次冷启动约 15–30 秒。

### 📱 每个应用都是 Mac 应用
安装任意 APK 后自动生成同名 `.app`：真彩图标（设备侧渲染）、Finder / Dock / Launchpad 全部可用，双击直达应用。

### 🛍 在线应用下载
「在线下载」是应用库里的内置应用：点击图标弹出独立窗口（像普通应用一样，不占主界面）。**默认 F-Droid 源（国内镜像直连）**，打开即见**推荐应用**落地页——F-Droid 精选（Fennec / K-9 / VLC / NewPipe / Termux / KDE Connect…）点击即查版本安装；也可切换来源搜索：
- **豌豆荚**：国内应用全量检索，支持**选择历史版本**（旧版回退 / 新版抢先都行）
- **F-Droid**：开源应用库，内置**国内镜像**（清华 TUNA / 南大 NJU 自动选择，官方源国内不可达时无感切换）
- 下载断点续传；APK 哈希校验（F-Droid sha256 / 豌豆荚 md5）+ 包名核对后才安装
- **装完自动删除安装包**（可在设置里改为保留；中断的下载保留供续传，7 天自动清理）

### 📱 真机连接（扫码配对 / USB / 配对码 / 手动）
虚拟机之外直接连接你的 Android 手机：**Mac 显示二维码、手机扫码——与 Android Studio 无线配对同款**（Android 11+）。手机「设置 → 开发者选项 → 无线调试 → 使用二维码配对设备」扫一下 Mac 上的二维码，自动完成配对与连接，全程无需相机权限；也可输 6 位配对码，USB 插线即用，或手动输 IP:端口。配对一次后同 Wi-Fi 自动回连。连接后可投屏（独立窗口）、安装应用。实现细节见 `docs/REAL-DEVICE.md`。

### 🖥 GPU 加速显示
显示由模拟器自带窗口承担，Metal / gfxstream GPU 硬件加速；窗口可自由缩放、全屏。设备形态一键切换：手机 1080×2400 竖屏 / 平板 2560×1600 横屏 / 自定义 W×H@DPI（运行中保存立即生效）。

> GPU 后端默认 `host`（guest 渲染在宿主 Apple GPU 重放，gfxstream→MoltenVK→Metal）：
> guest CPU 零渲染负担，独立窗口进程 CPU 实测从 1.7% 降至 0%。此前 qemu 默认的
> SwiftShader 软渲是 guest 卡顿与资源占用的根源。个别镜像不兼容时可在设置 →
> 专业参数把 gpu_mode 改回 `swiftshader_indirect`。

### 🪟 每应用独立窗口
应用可以不占用模拟器主窗口，在自己的 macOS 原生窗口中运行（实现文档：`docs/INDEPENDENT-WINDOW.md`）：

```bash
mwine apps mode com.example.app window   # 该应用改用独立窗口（默认 emulator 模式）
```

- **虚拟显示器直通**（qemu 副屏 + scrcpy 镜像）：窗口 resize 时 guest **真实重排分辨率**（非拉伸）；关窗自动销毁副屏
- guest 渲染在宿主 Apple GPU（Metal）重放，窗口进程 CPU ≈ 0；视频 VideoToolbox 硬解 + OpenGL 渲染
- 鼠标 / 滚轮 / 键盘经 scrcpy 控制通道注入（毫秒级延迟）；**⌘C/⌘X/⌘V 剪贴板与 Mac 双向互通**；宿主输入法可打中文
- 另有 `mwine mirror --record out.h264`（主屏镜像 + 裸流录制，适合录制 / 诊断）

### 🎮 游戏按键 DIY（v2）
可视化节点编辑器 + 游戏级映射引擎（能力对齐 QtScrcpy / MuMu / 雷电）：
- **虚拟摇杆**：WASD → 中心按下、8 向拖拽（王者 / 和平精英走位）
- **技能**：快速施法，或「鼠标方向施法」（按住技能 → 指针向鼠标方向拖拽 → 松手施放，拖回按钮可取消）
- **按键**：点按 / 按住（开火、开镜）/ 连点（普攻）/ 双击切换（探头）
- **FPS 视角**：鼠标捕获模式，增量驱动视角拖拽，越界自动回绕；灵敏度可调
- **滑动 / 滚轮 / 宏**：手势插值、滚轮切枪、按序宏（点按/长按/滑动/延时）
- 内置王者荣耀（MOBA）与和平精英（FPS）模板，一键套用后拖拽微调；总开关 `` ` `` 随时启停
注入走**模拟器原生 console 通道**（`event send` → virtio 多点触控设备 → guest 内核，微秒级、真实硬件事件身份、最多 11 指并发、无任何 guest 进程），按应用保存方案（首次需授权「辅助功能」）。

### 🔔 通知与文件互通
- 设备通知实时转发到 macOS 通知中心（companion 桥自动装配）
- 命令行一键推送文件进设备；截屏直接存桌面并在 Finder 中定位

### 📊 性能监控与省电
概览页内嵌 CPU / 内存仪表 + 3 分钟历史曲线 + 宿主开销，2 秒刷新；空闲 30 分钟自动存档关停，不占后台。

### 💻 ADB 终端
GUI 内置交互式 ADB 终端：与 `adb shell` 行为一致（提示符、回显、Ctrl-C 中断、`cd`/环境变量跨命令保持），支持命令历史（↑/↓）、清屏、一键重连，`logcat`、`top` 等长驻命令实时流式输出——开发调试不用离开应用。

### 🔍 流量分析（安全研究）
一键 eBPF 明文旁挂：在自建 userdebug 镜像上以 uprobe 挂系统 conscrypt，HTTPS 请求/响应
明文直接落库——无需证书、无感（不设代理/无 VPN/不动网络栈）、按应用筛选。详情见
`docs/Traffic-Capture-可行性研究报告.md`。

### 💿 镜像管理
- **在线拉取**：从官方仓库直接下载 arm64 系统镜像（断点续传、自动校验、支持代理）
- **本地导入**：导入 `houttuynia-image-*.tar.zst` 分发包（内嵌逐文件 sha256 校验）
- 多镜像共存，随时切换

### 🛡 代理与设备身份
HTTP 代理一键指向宿主；设备身份重置（android_id / 广告 ID 换新，应用视作全新设备）；一键重置虚拟机（清空全部数据与快照）。

### ⌨️ 命令行与 API
GUI 能做的，CLI 都能做；UI 与 CLI 同源，统一走 `mwine-cored` 守护进程（localhost REST API）。

### 🌐 NAS 版（Web 控制台，nas 分支）
`nas` 分支把产品变成家庭服务器上的 Web 服务：**Rust 网关（axum + 自研
adb/scrcpy 协议栈）**对外开放 REST API，内嵌 **Vue 3 + Element Plus** 控制台
——局域网里任何设备的浏览器都能开关虚拟机、看 scrcpy 实时画面（WebCodecs
硬解 + 触控注入）、上传 APK 装应用、管镜像、开 ADB 终端、改设置。Docker
一键部署（群晖/威联通/TrueNAS/绿联通用），也可裸跑在常开的 Mac mini 上
（全速 HVF 加速）。

```bash
docker build -t houttuynia-nas -f nas/Dockerfile .   # 国内加 --build-arg APT_MIRROR=mirrors.ustc.edu.cn CARGO_MIRROR=rsproxy
cd nas && docker compose up -d                        # 先改 compose 里的 MWINE_TOKEN
# 浏览器打开 http://<NAS的IP>:7788，输入令牌即可
```

部署形态、环境变量、能力矩阵、安全须知与 FAQ 详见 `nas/README.md`。

## 快速开始

1. 从 [Releases](../../releases) 下载并解压 `Houttuynia-*.zip`，把 **Houttuynia.app** 拖入「应用程序」
2. 首次打开若提示「已损坏」，双击下载页附带的 **修复打开权限.command**（或终端执行 `xattr -cr /Applications/Houttuynia.app`）
3. 首次启动会提示补装**模拟器运行时**（约 330 MB，不随安装包分发）：一键**在线下载**（断点续传，中断可续）或导入本地 `Houttuynia-runtime-*.zip`
4. 进入「镜像」页：在线拉取官方镜像，或导入本地分发包
5. 回到「概览」点「启动」，冷启约 15–30 秒，之后开关机都是 5 秒级
6. 拖放任意 `.apk` 到窗口即安装（有进度卡片）；也可以点应用库里的「在线下载」应用，在弹出的独立窗口里直接搜索安装（豌豆荚 / F-Droid）

界面一览：**概览**（开关机 / 截屏 / 性能监控）、**应用库**、**真机**（扫码配对（Mac 出码·手机扫）/ USB / 配对码 / 手动连接 + 投屏）、**设置**（设备形态 / 内存 / sdcard / 代理 / 身份 / 重置）、**终端**（交互式 ADB shell）、**镜像**、**流量分析**。

## 命令行

```bash
mwine vm start <名称>             # 快照启动（~5s）
mwine vm stop <serial>            # 存档 + 关停
mwine install X.apk               # 安装
mwine apps list|launch|uninstall  # 应用管理
mwine device list                 # 真机列表（USB + 无线）
mwine device qrcode               # 出码配对：终端显示二维码，手机扫码即连
mwine device pair --code 482615   # 配对码连接真机（--qr 传二维码内容）
mwine device connect 192.168.1.42:40113   # 手动连接已配对设备
mwine device tcpip                # USB 一键转无线
mwine mirror --serial <serial>    # 真机/虚拟机投屏窗口
mwine store search 微信           # 在线搜索（--source wandoujia|fdroid）
mwine store versions <包名>       # 版本列表（含豌豆荚历史版本）
mwine store install <包名> --source wandoujia --app-id 596157 [--code 2740]
                                  # 下载校验 + 安装（装完删包，--keep-apk 保留）
mwine keys shot|get|set|watch|template  # 按键映射（template: 内置模板，如 peace_elite）
mwine files push <文件>           # 传文件进设备
mwine monitor                     # 资源监护
mwine capture start|stop|status   # 流量采集（eBPF 明文旁挂）
mwine capture flows --host x.com  # flow 查询/筛选
mwine capture show <id>           # 单条 flow 明文详情
```

## 架构

```
Houttuynia.app
 ├─ Qt 前端（app/qt，跨平台：macOS/Linux/Windows）
 │    └─ HTTP → 守护进程 mwine-cored（cpp/，C++17，localhost:7788 REST）
 │         ├─ modules/supervisor   虚拟机生命周期 / 镜像下载与导入
 │         ├─ modules/qemu         模拟器核心进程管理（HVF 加速）
 │         ├─ modules/adb          自研 ADB 客户端（smart-socket/wire/shell v1+v2）
 │         │                       + 真机配对（出码/配对码/mDNS 发现/adb pair·tcpip）
 │         ├─ modules/scrcpy       scrcpy v4 线协议 + VideoToolbox 硬解
 │         ├─ modules/app          APK 解析 / 安装 / .app 壳生成
 │         ├─ modules/store        在线应用商店（豌豆荚 / F-Droid 镜像，下载与校验）
 │         ├─ modules/config       设备配置持久化与热生效
 │         ├─ modules/keymap       按键映射数据模型 + v1 迁移 + 内置模板
 │         ├─ modules/input        CGEventTap 捕获 / console·scrcpy 注入引擎
 │         ├─ modules/audio        CoreAudio 输出 + scrcpy 音频
 │         ├─ modules/capture      eBPF 流量采集：HTTP 重组 + sqlite + pcap TLS 解密
 │         └─ modules/mitm         HTTPS 中间人 CA（OpenSSL）
 ├─ mwine CLI（cpp/modules/cli，与守护同源业务能力）
 └─ 壳 .app（每个 Android 应用一个）：内嵌 mwine 二进制 + app.conf
```

> 全 C++：原约 2.7 万行 Rust 已全部移植为 C++ 并从仓库移除（模块对照见 `cpp/README.md`）。
> 独立窗口播放器已用 Qt 重建（cpp/modules/window → mwine-window：VideoToolbox 硬解 +
> OpenGL 渲染 + scrcpy 控制注入）。待收口：`mwine debug display-fps / snap`。

## 从源码构建

要求：macOS 14+（Apple Silicon）、CMake ≥ 3.19、Qt 6（`brew install qtbase qtsvg`）、zstd / sqlite3 / openssl（`brew install zstd sqlite openssl`）、本机 Android SDK（emulator + platform-tools + arm64-v8a 系统镜像）。

```bash
scripts/build-cpp.sh                 # C++ 守护 + CLI + 模块测试（ctest）
scripts/build_companion.sh           # 通知桥 APK（需 JDK + build-tools）
scripts/build-qt.sh                  # Qt 前端
scripts/package-app.sh               # 组装 Houttuynia.app
```

> 运行时组件（模拟器核心与系统镜像）体积大，不随源码仓库分发；打包脚本从本机 Android SDK 取用。

## 已知限制

- 部分自编译镜像的媒体编码栈缺陷会导致 `/sdcard` 在个别场景不可用（文件推送已自动走降级通道）
- `mwine debug display-fps / snap` 暂为桩（见 `cpp/README.md`）

## 命名

Houttuynia = 鱼腥草（*Houttuynia cordata*）。内部二进制沿用 `mwine` 前缀。

## 许可

[Apache-2.0](LICENSE)

Android 是 Google LLC 的商标。本项目为独立开源项目，与 Google 及其他公司无关，仅面向合法用途（应用开发、测试与个人使用）。
